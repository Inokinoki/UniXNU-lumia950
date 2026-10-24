#!/usr/bin/env python3
"""Check a GPU addon blob before it is put in a boot payload.

The last rung of the GPU ladder is not a register sequence: it is a Metal
driver built outside the kernel, shipped as a bundle plus a few sidecar files,
and handed over as one blob. That blob's format is upstream's, not this
repository's (UniXNU/UniXNU, scripts/make-gpu-addon.py), so nothing here writes
one. What a host can prove is that the blob it was given will be read the way
the phone reads it, and that the plugin inside was built for the system this
board actually runs.

Layout, little endian:

    "NGPUADD2" | u32 count | u32 0 | u64 total bytes
    per file: u32 path bytes (with its NUL, at most 160) | u32 mode |
              u64 size | u64 compressed size |
              path, padded to 8 | the zlib stream, padded to 8

Byte order and padding are checked because the kernel parses this on the phone
by hand: a blob whose header is big endian, or whose padding is missing, does
not fail politely there.

The order is the order a bad blob fails: container, paths and modes, bundle,
sidecars, and last the two things a file listing cannot show -- the architecture
and the minimum iOS version of the plugin's Mach-O. That last one is why this
exists. The plugin published for this generation is built for iOS 17, this
board's userland is 16.7.16, and the loader does not refuse such a binary with a
message: the render server simply never gets a device.

usage:
  python3 scripts/check-gpu-addon.py build/gpu-addon.bin
  python3 scripts/check-gpu-addon.py --ios 16.7 build/gpu-addon.bin
  python3 scripts/check-gpu-addon.py --self-test
"""
from __future__ import annotations

import argparse
import plistlib
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"NGPUADD2"
HEADER = struct.Struct("<IIQ")
ENTRY = struct.Struct("<IIQQ")
MAX_PATH_BYTES = 160
BUNDLE = "addons/NativeAdrenoMetal.bundle/"
BUNDLE_BINARY = BUNDLE + "NativeAdrenoMetal"
PROBED = "bin/adrenoprobed"
PUBLISH = "addons/NativeAdrenoMetal.publish"
CLIENTS = "addons/NativeAdrenoMetal.clients"
PRIMELIST = "addons/NativeAdrenoMetal.primelist"
CLIENT_CHARS = "._-*"

# This board's userland is 16.7.16 (README.md, "The ladder").
BOARD_IOS = (16, 7)

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
LC_VERSION_MIN_IPHONEOS = 0x25
LC_BUILD_VERSION = 0x32
PLATFORM_NAMES = {
    1: "macOS", 2: "iOS", 3: "tvOS", 4: "watchOS", 5: "bridgeOS", 6: "Mac Catalyst",
    7: "iOS simulator", 8: "tvOS simulator", 9: "watchOS simulator",
}


class Error(Exception):
    pass


@dataclass
class Entry:
    path: str
    mode: int
    size: int
    data: bytes
    compressed: int


@dataclass
class Addon:
    entries: list[Entry] = field(default_factory=list)
    declared_total: int = 0
    actual_total: int = 0
    version: int = 0

    def get(self, path: str) -> Entry | None:
        for entry in self.entries:
            if entry.path == path:
                return entry
        return None


def parse(data: bytes) -> Addon:
    addon = Addon(actual_total=len(data))
    if len(data) < 24 or not data.startswith(MAGIC):
        raise Error("not a GPU addon: no NGPUADD2 signature")
    count, addon.version, addon.declared_total = HEADER.unpack_from(data, 8)
    if addon.declared_total != len(data):
        raise Error(f"header claims {addon.declared_total} bytes, the file is {len(data)}")
    if count > 4096:
        raise Error(f"{count} files in one addon is not a count this format explains")

    offset = 24
    seen: set[str] = set()
    for index in range(count):
        if offset + ENTRY.size > len(data):
            raise Error(f"file {index}: entry header runs past the end")
        path_bytes, mode, size, compressed = ENTRY.unpack_from(data, offset)
        offset += ENTRY.size
        if not 1 < path_bytes <= MAX_PATH_BYTES:
            raise Error(f"file {index}: path length {path_bytes} is outside 2..{MAX_PATH_BYTES}")
        padded = (path_bytes + 7) & ~7
        if offset + padded > len(data):
            raise Error(f"file {index}: path runs past the end")
        raw_path = data[offset:offset + path_bytes]
        if not raw_path.endswith(b"\0"):
            raise Error(f"file {index}: path is not NUL terminated")
        try:
            path = raw_path[:-1].decode("ascii")
        except UnicodeDecodeError:
            raise Error(f"file {index}: path is not ASCII") from None
        if any(data[offset + path_bytes:offset + padded]):
            raise Error(f"{path}: padding after the path is not zero")
        offset += padded

        padded_compressed = (compressed + 7) & ~7
        if offset + padded_compressed > len(data):
            raise Error(f"{path}: data runs past the end")
        try:
            content = zlib.decompress(data[offset:offset + compressed])
        except zlib.error as error:
            raise Error(f"{path}: not a zlib stream ({error})") from None
        if len(content) != size:
            raise Error(f"{path}: holds {len(content)} bytes, the header says {size}")
        if any(data[offset + compressed:offset + padded_compressed]):
            raise Error(f"{path}: padding after the data is not zero")
        offset += padded_compressed

        if path in seen:
            raise Error(f"{path}: listed twice")
        seen.add(path)
        addon.entries.append(Entry(path, mode, size, content, compressed))

    if offset != len(data):
        raise Error(f"{len(data) - offset} unexplained bytes after the last file")
    return addon


def check_paths(addon: Addon) -> list[str]:
    problems: list[str] = []
    for entry in addon.entries:
        path = entry.path
        if path.startswith("/") or ".." in path.split("/") or "//" in path:
            problems.append(f"{path}: not a relative path inside the addon root")
        if entry.mode not in (0o644, 0o755):
            problems.append(f"{path}: mode {entry.mode:o}, not 644 or 755")
        if path in (PROBED, BUNDLE_BINARY):
            if entry.mode != 0o755:
                problems.append(f"{path}: mode {entry.mode:o}; a program that is not executable "
                                f"is not run")
        elif entry.mode == 0o755:
            problems.append(f"{path}: executable, but only {PROBED} and the bundle binary run")
        if path.endswith(".plist") and entry.mode != 0o644:
            problems.append(f"{path}: a plist is 644, not {entry.mode:o}")
        if not entry.data:
            problems.append(f"{path}: empty")
    return problems


def macho_requirement(data: bytes) -> tuple[int, int, int]:
    """(platform, major, minor) from a thin ARM64 Mach-O. A fat file is refused."""
    if len(data) < 32:
        raise Error("shorter than a Mach-O header")
    (magic,) = struct.unpack_from("<I", data, 0)
    if magic != MH_MAGIC_64:
        if struct.unpack_from(">I", data, 0)[0] == MH_MAGIC_64:
            raise Error("big-endian Mach-O; the phone reads little endian")
        raise Error("not a thin 64-bit Mach-O (thin a fat file first)")
    cputype, _subtype, _filetype, ncmds, sizeofcmds = struct.unpack_from("<IIIII", data, 4)
    if cputype != CPU_TYPE_ARM64:
        raise Error(f"cputype 0x{cputype:08x}, not ARM64")
    if sizeofcmds > len(data) - 32:
        raise Error("load commands run past the end of the file")

    offset = 32
    stated: list[tuple[int, int]] = []
    for _ in range(ncmds):
        if offset + 8 > len(data):
            raise Error("load command runs past the end")
        cmd, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > len(data):
            raise Error(f"load command 0x{cmd:x} has size {size}")
        if cmd == LC_BUILD_VERSION:
            if size < 24:
                raise Error("LC_BUILD_VERSION is shorter than itself")
            platform, minos = struct.unpack_from("<II", data, offset + 8)
            stated.append((platform, minos))
        elif cmd == LC_VERSION_MIN_IPHONEOS:
            (minos,) = struct.unpack_from("<I", data, offset + 8)
            stated.append((2, minos))
        offset += size
    if not stated:
        raise Error("no LC_BUILD_VERSION or LC_VERSION_MIN_IPHONEOS: nothing says what it needs")

    # The strictest statement in the file decides whether it loads.
    platform, minos = max(stated, key=lambda item: item[1])
    return platform, minos >> 16, (minos >> 8) & 0xFF


def check_bundle(addon: Addon, ios: tuple[int, int]) -> list[str]:
    problems: list[str] = []

    binary = addon.get(BUNDLE_BINARY)
    if binary is None:
        problems.append(f"{BUNDLE_BINARY}: missing; the bundle has nothing to load")
    else:
        try:
            platform, major, minor = macho_requirement(binary.data)
        except Error as error:
            problems.append(f"{BUNDLE_BINARY}: {error}")
        else:
            if platform != 2:
                problems.append(f"{BUNDLE_BINARY}: built for "
                                f"{PLATFORM_NAMES.get(platform, f'platform {platform}')}, not iOS")
            elif (major, minor) > ios:
                problems.append(f"{BUNDLE_BINARY}: needs iOS {major}.{minor}, this board runs "
                                f"{ios[0]}.{ios[1]}")

    info_entry = addon.get(BUNDLE + "Info.plist")
    if info_entry is None:
        problems.append(f"{BUNDLE}Info.plist: missing; a bundle without one is not a bundle")
    else:
        try:
            info = plistlib.loads(info_entry.data)
        except Exception as error:  # plistlib raises several unrelated types
            info = {}
            problems.append(f"{BUNDLE}Info.plist: not a property list ({error})")
        if info:
            if info.get("CFBundleExecutable") != "NativeAdrenoMetal":
                problems.append(f"{BUNDLE}Info.plist: CFBundleExecutable is "
                                f"{info.get('CFBundleExecutable')!r}, not 'NativeAdrenoMetal'")
            if not info.get("CFBundleIdentifier"):
                problems.append(f"{BUNDLE}Info.plist: no CFBundleIdentifier")
            minimum = info.get("MinimumOSVersion")
            if minimum is not None:
                try:
                    wanted = tuple(int(part) for part in str(minimum).split(".")[:2])
                except ValueError:
                    problems.append(f"{BUNDLE}Info.plist: MinimumOSVersion {minimum!r} "
                                    f"is not a version")
                else:
                    if wanted > ios:
                        problems.append(f"{BUNDLE}Info.plist: MinimumOSVersion {minimum} is past "
                                        f"this board's {ios[0]}.{ios[1]}")

    if addon.get(PROBED) is None:
        problems.append(f"{PROBED}: missing; nothing publishes the plugin to the render server")

    for entry in addon.entries:
        if entry.path.startswith("daemons/") and entry.path.endswith("-entitlements.plist"):
            problems.append(f"{entry.path}: a build input, not a launchd job")
    return problems


def check_sidecars(addon: Addon) -> tuple[list[str], list[str]]:
    problems: list[str] = []
    notes: list[str] = []

    publish = addon.get(PUBLISH)
    if publish is None:
        notes.append(f"{PUBLISH}: absent, so the driver is shipped but not published to "
                     f"any client -- the state rung 6 starts in")
    elif publish.data.strip() != b"1":
        problems.append(f"{PUBLISH}: holds {publish.data!r}; the format writes '1'")

    clients = addon.get(CLIENTS)
    if clients is not None:
        names = [line.strip() for line in clients.data.decode("ascii", "replace").split()]
        if not names:
            problems.append(f"{CLIENTS}: no process names")
        for name in names:
            if any(char not in CLIENT_CHARS and not char.isalnum() for char in name):
                problems.append(f"{CLIENTS}: {name!r} is not a process name")
        if names.count("*") and len(names) > 1:
            notes.append(f"{CLIENTS}: '*' with other names; the wildcard already covers them")

    primelist = addon.get(PRIMELIST)
    if primelist is not None:
        paths = primelist.data.decode("ascii", "replace").split()
        if not paths:
            problems.append(f"{PRIMELIST}: no libraries")
        for path in paths:
            if not path.startswith("/") or not path.endswith(".metallib"):
                problems.append(f"{PRIMELIST}: {path!r} is not an absolute .metallib path")
    return problems, notes


def report(path: Path, ios: tuple[int, int]) -> int:
    try:
        addon = parse(path.read_bytes())
    except Error as error:
        print(f"FAIL {path}: {error}")
        return 1

    problems = check_paths(addon) + check_bundle(addon, ios)
    sidecar_problems, notes = check_sidecars(addon)
    problems += sidecar_problems

    executables = sum(1 for entry in addon.entries if entry.mode == 0o755)
    print(f"{path}: {len(addon.entries)} files, {addon.actual_total} bytes, "
          f"{executables} executable")
    for note in notes:
        print(f"note {note}")
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        print(f"{len(problems)} problem(s) in {path}", file=sys.stderr)
        return 1
    print(f"ok   {path}: layout, bundle and iOS {ios[0]}.{ios[1]} target all check out")
    return 0


# ---------------------------------------------------------------------------
# Self-test: a checker that only ever sees good input is not a checker.
# ---------------------------------------------------------------------------

def macho_stub(platform: int = 2, minimum: tuple[int, int] = (16, 0)) -> bytes:
    minos = (minimum[0] << 16) | (minimum[1] << 8)
    body = struct.pack("<IIIIII", LC_BUILD_VERSION, 24, platform, minos, minos, 0)
    return struct.pack("<IIIIIIII", MH_MAGIC_64, CPU_TYPE_ARM64, 0, 6, 1, len(body), 0, 0) + body


def info_plist(minimum: str = "16.0") -> bytes:
    return plistlib.dumps({
        "CFBundleExecutable": "NativeAdrenoMetal",
        "CFBundleIdentifier": "org.blu-sharky.NativeAdrenoMetal",
        "CFBundlePackageType": "BNDL",
        "MinimumOSVersion": minimum,
    })


def build(files: list[tuple[str, int, bytes]], pad: bool = True,
          magic: bytes = MAGIC) -> bytes:
    body = b""
    for name, mode, data in files:
        encoded = name.encode() + b"\0"
        packed = zlib.compress(data, 9)
        padding = (lambda chunk: chunk + bytes(-len(chunk) % 8)) if pad else (lambda chunk: chunk)
        body += ENTRY.pack(len(encoded), mode, len(data), len(packed))
        body += padding(encoded) + padding(packed)
    return magic + HEADER.pack(len(files), 0, 24 + len(body)) + body


def good_files() -> list[tuple[str, int, bytes]]:
    return [
        (BUNDLE_BINARY, 0o755, macho_stub()),
        (BUNDLE + "Info.plist", 0o644, info_plist()),
        (PROBED, 0o755, b"\xcf\xfa\xed\xfe"),
        ("daemons/org.blu-sharky.adrenoprobed.plist", 0o644, b"<plist/>"),
    ]


def self_test() -> int:
    failures = 0

    def expect(label: str, blob: bytes, accept: bool) -> None:
        nonlocal failures
        try:
            addon = parse(blob)
            problems = check_paths(addon) + check_bundle(addon, BOARD_IOS)
            problems += check_sidecars(addon)[0]
        except Error as error:
            problems = [str(error)]
        if bool(problems) == accept:
            found = problems[0] if problems else "no problem reported"
            print(f"FAIL {label}: {found}")
            failures += 1
        else:
            print(f"ok   {label}")

    good = build(good_files())
    expect("a coherent addon is accepted", good, True)

    expect("no signature is refused", build(good_files(), magic=b"NGPUADD1"), False)
    expect("a header that miscounts bytes is refused", good[:-8], False)
    expect("a missing pad byte is refused", build(good_files(), pad=False), False)

    files = good_files()
    files[0] = ("../NativeAdrenoMetal", 0o755, macho_stub())
    expect("a path that climbs out is refused", build(files), False)

    files = good_files()
    files[2] = (PROBED, 0o644, files[2][2])
    expect("a probe daemon that is not executable is refused", build(files), False)

    files = good_files()
    files[1] = (BUNDLE + "Info.plist", 0o755, info_plist())
    expect("an executable Info.plist is refused", build(files), False)

    files = [item for item in good_files() if item[0] != BUNDLE + "Info.plist"]
    expect("a bundle without Info.plist is refused", build(files), False)

    files = good_files()
    files[0] = (BUNDLE_BINARY, 0o755, macho_stub(minimum=(17, 0)))
    expect("a plugin that needs iOS 17 is refused", build(files), False)

    files = good_files()
    files[1] = (BUNDLE + "Info.plist", 0o644, info_plist("17.0"))
    expect("an Info.plist that asks for iOS 17 is refused", build(files), False)

    files = good_files()
    files[0] = (BUNDLE_BINARY, 0o755, macho_stub(platform=1))
    expect("a macOS build is refused", build(files), False)

    files = good_files()
    files[0] = (BUNDLE_BINARY, 0o755, b"\x00" * 64)
    expect("something that is not a Mach-O is refused", build(files), False)

    files = good_files()
    files[3] = ("daemons/NativeAdrenoMetal-entitlements.plist", 0o644, b"<plist/>")
    expect("an entitlements plist in daemons is refused", build(files), False)

    files = good_files() + [(PUBLISH, 0o644, b"yes\n")]
    expect("a publish file that does not say 1 is refused", build(files), False)
    expect("an addon without a publish file is accepted (shipped, not published)",
           build(good_files()), True)

    files = good_files() + [(CLIENTS, 0o644, b"SpringBoard\nSpring Board!\n")]
    expect("a malformed client name is refused", build(files), False)

    files = good_files() + [(PRIMELIST, 0o644, b"relative.metallib\n")]
    expect("a relative primelist entry is refused", build(files), False)

    if failures:
        print(f"{failures} self-test case(s) failed", file=sys.stderr)
        return 1
    print("all self-test cases passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("addon", type=Path, nargs="*", help="addon blob to check")
    parser.add_argument("--ios", default=".".join(str(part) for part in BOARD_IOS),
                        help="this board's iOS version to build against "
                             f"(default: {BOARD_IOS[0]}.{BOARD_IOS[1]})")
    parser.add_argument("--self-test", action="store_true",
                        help="mutate a known-good addon and require each defect to be refused")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.addon:
        parser.error("name an addon, or pass --self-test")

    try:
        parts = tuple(int(part) for part in args.ios.split(".")[:2])
    except ValueError:
        parser.error(f"--ios takes a version like 16.7, not {args.ios!r}")

    failures = 0
    for path in args.addon:
        if not path.is_file():
            print(f"error: {path} does not exist", file=sys.stderr)
            failures += 1
            continue
        failures += report(path, parts)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
