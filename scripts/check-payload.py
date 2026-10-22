#!/usr/bin/env python3
"""Check the UniXNU chainload payload: payload/EFI/UniXNU/unixnu.toml against the parser
the firmware actually runs, and optionally the staged tree it names.

The keys, types and enums here are read from package/UniXNUPkg/Library/UniXNULib/Config.c in
the UniXNU checkout, not from the documentation, because the two differ where it matters:

  * an unknown [ui] console value silently becomes "auto", an unknown layout is an error;
  * an action entry with an unknown action is an error, but the entry stays in the menu with
    no action set;
  * confirm_titles and confirm_args are ignored unless confirm is present and the two
    arrays are the same non-zero length;
  * [boot] default must name an entry id or an in-range index, or the whole file is rejected.

usage:
  python3 scripts/check-payload.py
  python3 scripts/check-payload.py --payload-dir out/payload   # after staging the EFI files
"""
from __future__ import annotations

import argparse
import pathlib
import sys

try:
    import tomllib
except ModuleNotFoundError:  # 3.11+
    sys.exit("python3.11 or newer is required (tomllib)")

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG_RELATIVE = pathlib.Path("payload/EFI/UniXNU/unixnu.toml")
# UNIXNU_CONFIG_DIR in Config.c: every path in the file is relative to this directory.
CONFIG_DIR = "EFI/UniXNU"

TOP_KEYS = {"version", "boot", "ui", "kits", "kit", "driver", "entry", "dt_patch", "block_slice", "acpi"}
BOOT_KEYS = {"timeout": int, "rotate": int, "fallback_timeout": int, "connect_all": bool, "default": (str, int)}
UI_KEYS = {"title": str, "scale": int, "logo": bool, "layout": str, "portrait_below": int, "console": str}
UI_ENUMS = {"layout": {"auto", "landscape", "portrait"}, "console": {"auto", "graphics", "text", "none"}}
KITS_KEYS = {"autoload": bool, "directory": str}
DRIVER_KEYS = {"path": str, "file": str, "args": str, "connect": bool, "enabled": bool}
ENTRY_KEYS = {
    "id": str, "title": str, "type": str, "path": str, "args": str, "dtb": str, "initrd": str,
    "confirm": str, "confirm_titles": list, "confirm_args": list, "action": str, "enabled": bool,
}
ENTRY_TYPES = {"efi", "linux", "xnu", "action"}
ACTIONS = {"reset", "shutdown", "exit"}
PORTRAIT_BELOW_DEFAULT = 800  # UNIXNU_PORTRAIT_BELOW_DEFAULT


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notes: list[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def note(self, message: str) -> None:
        self.notes.append(message)


def typed(table: dict, key: str, want, where: str, report: Report) -> None:
    value = table[key]
    if isinstance(want, tuple):
        if not any(type(value) is option for option in want):
            expected = " or ".join(option.__name__ for option in want)
            report.error(f"{where}.{key} is {type(value).__name__}; expected {expected}")
    elif type(value) is not want:
        report.error(f"{where}.{key} is {type(value).__name__}; expected {want.__name__}")


def keys_of(table: object, known: dict | set, where: str, report: Report) -> None:
    if not isinstance(table, dict):
        report.error(f"{where} is not a table")
        return
    for key in table:
        if key not in known:
            report.error(f"{where}.{key} is not a key Config.c reads")


def relative_path(value: str, where: str, report: Report) -> str:
    """A config path is joined to \\EFI\\UniXNU with a backslash; keep it that simple."""
    if value.startswith(("/", "\\")) or ":" in value:
        report.error(f"{where} is absolute ({value!r}); it must be relative to \\EFI\\UniXNU")
    if "\\" in value:
        report.error(f"{where} uses a backslash ({value!r}); write it with forward slashes")
    if ".." in value.split("/"):
        report.error(f"{where} leaves \\EFI\\UniXNU ({value!r})")
    return value


def check_drivers(root: dict, report: Report) -> None:
    drivers = root.get("driver", [])
    if not isinstance(drivers, list):
        report.error("driver is not an array of tables; write [[driver]]")
        return
    for index, driver in enumerate(drivers):
        where = f"driver[{index}]"
        keys_of(driver, DRIVER_KEYS, where, report)
        if not isinstance(driver, dict):
            continue
        for key, want in DRIVER_KEYS.items():
            if key in driver:
                typed(driver, key, want, where, report)
        # A config driver uses path, a kit driver uses file; either is relative to \EFI\UniXNU.
        if ("path" in driver) == ("file" in driver):
            report.error(f"{where} needs exactly one of path (config) or file (kit)")
        key = "path" if "path" in driver else "file"
        if isinstance(driver.get(key), str):
            relative_path(driver[key], f"{where}.{key}", report)


def check_entries(root: dict, report: Report) -> list[dict]:
    entries = root.get("entry", [])
    if not isinstance(entries, list):
        report.error("entry is not an array of tables; write [[entry]]")
        return []
    ids: dict[str, int] = {}
    for index, entry in enumerate(entries):
        where = f"entry[{index}]"
        keys_of(entry, ENTRY_KEYS, where, report)
        if not isinstance(entry, dict):
            continue
        for key, want in ENTRY_KEYS.items():
            if key in entry:
                typed(entry, key, want, where, report)
        entry_id = entry.get("id")
        if entry_id is None:
            report.error(f"{where} has no id; the menu shows 'Untitled' and [boot] default cannot name it")
        elif entry_id in ids:
            report.error(f"{where}.id {entry_id!r} is also entry[{ids[entry_id]}].id")
        elif isinstance(entry_id, str):
            ids[entry_id] = index
        kind = entry.get("type", "efi")
        if kind not in ENTRY_TYPES:
            report.error(f"{where}.type {kind!r} is not efi, linux, xnu or action")
        if kind == "action":
            if "action" not in entry:
                report.error(f"{where} is an action entry with no action; Config.c errors and boots nothing")
            elif entry["action"] not in ACTIONS:
                report.error(f"{where}.action {entry['action']!r} is not reset, shutdown or exit")
            if "path" in entry:
                report.note(f"{where} is an action entry; its path {entry['path']!r} is ignored")
        else:
            if "path" not in entry:
                report.error(f"{where} has type {kind!r} and no path; Config.c errors")
            else:
                relative_path(entry["path"], f"{where}.path", report)
            if "action" in entry:
                report.error(f"{where} is a {kind} entry with an action; the action is ignored")
        if "initrd" in entry and isinstance(entry["initrd"], str):
            relative_path(entry["initrd"], f"{where}.initrd", report)
        titles, args = entry.get("confirm_titles"), entry.get("confirm_args")
        if titles is not None or args is not None:
            if "confirm" not in entry:
                report.error(f"{where} has confirm_titles or confirm_args without confirm; they are ignored")
            elif not (isinstance(titles, list) and isinstance(args, list) and titles and len(titles) == len(args)):
                report.error(f"{where} needs equally long, non-empty confirm_titles and confirm_args")
            elif not all(isinstance(item, str) for item in titles + args):
                report.error(f"{where} confirm_titles and confirm_args must be strings")
    return entries


def check_config(config: pathlib.Path, panel: tuple[int, int], report: Report) -> dict:
    if not config.is_file():
        report.error(f"{config} is missing; UniXNU reads \\EFI\\UniXNU\\unixnu.toml")
        return {}
    if config.name != "unixnu.toml" or config.parent.as_posix().split("/")[-2:] != CONFIG_DIR.split("/"):
        report.error(f"the config is at {config}; UniXNU reads it from {CONFIG_DIR}")
    raw = config.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        report.error("the config starts with a UTF-8 BOM; the TOML parser does not skip it")
    try:
        root = tomllib.loads(raw.decode("utf-8"))
    except tomllib.TOMLDecodeError as error:
        report.error(f"unixnu.toml does not parse: {error}")
        return {}

    keys_of(root, TOP_KEYS, "unixnu.toml", report)
    if root.get("version") != 1:
        report.error(f"version is {root.get('version')!r}; UniXNU payloads are version 1")

    boot = root.get("boot", {})
    keys_of(boot, BOOT_KEYS, "boot", report)
    for key, want in BOOT_KEYS.items():
        if key in boot:
            typed(boot, key, want, "boot", report)
    ui = root.get("ui", {})
    keys_of(ui, UI_KEYS, "ui", report)
    for key, want in UI_KEYS.items():
        if key in ui:
            typed(ui, key, want, "ui", report)
    for key, allowed in UI_ENUMS.items():
        if key in ui and ui[key] not in allowed:
            report.error(f"ui.{key} {ui[key]!r} is not one of {', '.join(sorted(allowed))}")
    entries = check_entries(root, report)
    check_drivers(root, report)

    # The layout depends on the panel, not on the file: report the choice this phone makes.
    width, height = panel
    below = ui.get("portrait_below", PORTRAIT_BELOW_DEFAULT)
    layout = ui.get("layout", "auto")
    chosen = layout if layout != "auto" else (
        "portrait" if width / height * 1000 < below else "landscape")
    report.note(f"{width}x{height} at portrait_below={below} draws the {chosen} layout")

    default = boot.get("default")
    if default is not None:
        if isinstance(default, int):
            if not 0 <= default < len(entries):
                report.error(f"boot.default {default} is not an index into {len(entries)} entries")
        elif not any(entry.get("id") == default for entry in entries if isinstance(entry, dict)):
            report.error(f"boot.default {default!r} names no entry id; Config.c rejects the whole file")
    confirm = {entry.get("id") for entry in entries
               if isinstance(entry, dict) and "confirm" in entry}
    if isinstance(default, str) and default in confirm:
        report.note(f"boot.default {default!r} asks for confirmation, so the countdown falls back "
                    "to the first entry that does not")
    timeout = boot.get("timeout", 5)
    if isinstance(timeout, int) and timeout < 0:
        report.note("boot.timeout is negative: the menu waits for a key, and fallback_timeout "
                    "applies only when no input device exists")
    return root


def check_staged(root: dict, base: pathlib.Path, report: Report) -> list[str]:
    """Every file the config names has to be in the staged tree, at the path it will have."""
    wanted = [base / "unixnu.toml"]
    for entry in root.get("entry", []):
        if isinstance(entry, dict) and entry.get("type", "efi") != "action":
            for key in ("path", "initrd"):
                if isinstance(entry.get(key), str):
                    wanted.append(base / entry[key])
    for driver in root.get("driver", []):
        if isinstance(driver, dict) and isinstance(driver.get("path"), str):
            wanted.append(base / driver["path"])
    return [str(path) for path in wanted]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=pathlib.Path, default=ROOT / CONFIG_RELATIVE,
                        help=f"config to check (default: {CONFIG_RELATIVE} in this repository)")
    parser.add_argument("--payload-dir", type=pathlib.Path,
                        help="staged ESP root, e.g. out/payload; every named file must exist there")
    parser.add_argument("--panel", default="1440x2560",
                        help="panel in pixels, for the layout report (default: the Lumia 950's)")
    args = parser.parse_args()

    try:
        width, height = (int(part) for part in args.panel.lower().split("x", 1))
    except ValueError:
        parser.error(f"--panel takes WIDTHxHEIGHT, not {args.panel!r}")

    report = Report()
    root = check_config(args.config, (width, height), report)
    if root and args.payload_dir:
        base = args.payload_dir / CONFIG_DIR
        missing = [path for path in check_staged(root, base, report) if not pathlib.Path(path).is_file()]
        for path in missing:
            report.error(f"the staged payload has no {path}; stage the EFI files before checking")
    elif root:
        for path in check_staged(root, args.config.parent, report):
            if not pathlib.Path(path).is_file():
                report.note(f"not staged yet: {path}")

    for note in report.notes:
        print(f"note: {note}")
    for error in report.errors:
        print(f"error: {error}", file=sys.stderr)
    if report.errors:
        return 1
    entries = root.get("entry", []) if root else []
    try:
        shown = args.config.resolve().relative_to(ROOT)
    except ValueError:
        shown = args.config
    print(f"payload config ok: {shown} ({len(entries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
