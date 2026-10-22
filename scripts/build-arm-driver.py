#!/usr/bin/env python3
"""Compile a device XNU driver against the recorded source-kernel ABI.

Same recipe as the base tree's build-arm-driver.py: replay the compile command
XNU recorded for IOPlatformExpert, swap in the driver source, and check that the
object really is an ARM64 Mach-O object. The differences are the component list
(which also covers this device's drivers) and the archive step.

Run it from the build tree; `unixnu build kernel` does that for every entry of
[kernel] archives in board.toml.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# component -> sources, output directory, extra include directories
COMPONENTS = {
    "ArmDTPlatform": {
        "sources": ["drivers/ArmDTPlatform/ArmDTPlatform.cpp"],
        "out": "armdt-platform",
    },
    "ArmCpu": {
        "sources": ["drivers/ArmCpu/ArmCpu.cpp"],
        "out": "armcpu",
    },
    "NativeSdhci": {
        "sources": ["drivers/NativeSdhci/NativeSdhci.cpp", "drivers/NativeSdhci/SdhciCore.cpp"],
        "out": "native-sdhci",
        # The storage ABI headers are prepared from IOStorageFamily by the kernel step;
        # osfmk is where the recorded recipe resolves <mach/...> from.
        "include": ["drivers/NativeSdhci", "build/storage-abi/include", ".cache/xnu-work/osfmk"],
    },
    "NativeRamBlock": {
        "sources": ["drivers/NativeRamBlock/NativeRamBlock.cpp"],
        "out": "native-ram-block",
        "include": ["build/storage-abi/include"],
    },
}

RECIPE = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
MH_OBJECT = 1


def check_object(path):
    """The three fields a driver object must have to link into this kernel."""
    data = path.read_bytes()[:32]
    if len(data) < 32:
        raise SystemExit(f"{path}: shorter than a Mach-O header")
    magic, cputype, _subtype, filetype = struct.unpack("<IIII", data[:16])
    if magic != MH_MAGIC_64:
        raise SystemExit(f"{path}: magic 0x{magic:08x}, expected 0x{MH_MAGIC_64:08x}")
    if cputype != CPU_TYPE_ARM64:
        raise SystemExit(f"{path}: cputype 0x{cputype:08x}, expected 0x{CPU_TYPE_ARM64:08x}")
    if filetype != MH_OBJECT:
        raise SystemExit(f"{path}: filetype {filetype}, expected {MH_OBJECT}")


def build_command(record, source, destination, include_dirs):
    command = []
    arguments = record["arguments"]
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1

    command.extend([
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-Wno-c99-designator", "-Wno-reorder-init-list",
        "-Wno-deprecated-enum-enum-conversion", "-Wno-unnecessary-virtual-specifier",
    ])
    for directory in include_dirs:
        command.extend(["-I", str(directory)])
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=sorted(COMPONENTS))
    args = parser.parse_args()

    spec = COMPONENTS[args.component]
    if not RECIPE.is_file():
        raise SystemExit(
            f"No compile record at {RECIPE}. The kernel step must run first, and it needs a "
            "macOS host: the XNU build is not supported on this machine."
        )

    record = json.loads(RECIPE.read_text().strip().rstrip(","))
    output = ROOT / "build" / spec["out"]
    output.mkdir(parents=True, exist_ok=True)
    include_dirs = [ROOT / directory for directory in spec.get("include", [])]

    objects = []
    for source_name in spec["sources"]:
        source = ROOT / source_name
        if not source.is_file():
            raise SystemExit(f"{source} is missing: is the module linked into the build tree?")
        destination = output / (source.stem + ".o")
        command = build_command(record, source, destination, include_dirs)
        subprocess.run(command, cwd=record["directory"], check=True)
        check_object(destination)
        objects.append(destination)

    if shutil.which("ar") is None:
        raise SystemExit("ar is not on PATH")
    archive = output / f"lib{args.component}.a"
    subprocess.run(["ar", "rcs", str(archive)] + [str(o) for o in objects], check=True)
    print(f"{args.component}: {archive}")
    print(f"  link with  -Wl,-force_load,{archive}")


if __name__ == "__main__":
    sys.exit(main())
