#!/usr/bin/env python3
"""Check that a screenshot is the Lumia 950's panel, 1440x2560.

A screenshot is evidence of what the panel showed only if it is the panel. QEMU defaults to
whatever size the runner was given (the earlier SpringBoard captures in this project's history
are 1125x2048), so a capture can look right and still be the wrong screen. This reads the PNG
header and compares it with scripts/panel.py, which is the same number the menu layout uses.

    python3 scripts/check-screenshot.py out/lockscreen.png
    python3 scripts/check-screenshot.py --panel 1080x2400 out/menu.png   # a deliberate other panel

Exit status is 0 only for a match. No image library is needed: a PNG's IHDR is at a fixed
offset, and a file that is not a PNG is a failure rather than something to guess about.
"""
from __future__ import annotations

import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import panel  # noqa: E402  (same directory, no package)

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_size(path: pathlib.Path) -> tuple[int, int]:
    raw = path.read_bytes()[:24]
    if len(raw) < 24 or not raw.startswith(PNG_SIGNATURE) or raw[12:16] != b"IHDR":
        raise SystemExit(f"{path}: not a PNG (no signature or no IHDR)")
    width, height = struct.unpack(">II", raw[16:24])
    return width, height


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", type=pathlib.Path, nargs="+", help="PNG to check")
    parser.add_argument("--panel", default=panel.describe(panel.PANEL),
                        help=f"expected size (default: this board's, "
                             f"{panel.describe(panel.PANEL)})")
    args = parser.parse_args()

    try:
        expected = panel.parse(args.panel)
    except ValueError as error:
        parser.error(str(error))

    failures = 0
    for image in args.image:
        if not image.is_file():
            print(f"error: {image} does not exist", file=sys.stderr)
            failures += 1
            continue
        width, height = png_size(image)
        if (width, height) == expected:
            print(f"ok   {image}: {width}x{height}")
        else:
            print(f"FAIL {image}: {width}x{height}, not {panel.describe(expected)}")
            failures += 1

    if failures:
        print(f"{failures} screenshot(s) are not this panel", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
