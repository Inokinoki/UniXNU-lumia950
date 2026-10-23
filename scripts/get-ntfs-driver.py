#!/usr/bin/env python3
r"""Fetch the NTFS driver that makes MainOS reachable from UEFI.

    python3 scripts/get-ntfs-driver.py [--out out/ntfs_aa64.efi]

The Lumia's UEFI image carries EDK II's FAT driver and nothing else, so the NTFS MainOS
partition has no file system until one is started. `scripts/stage-kernel.py --ntfs-driver`
stages this file as \EFI\UniXNU\Drivers\ntfs.efi and gives it a [[driver]] entry; UniXNU
starts it and connects it, and then DarwinBoot can read \EFI\UniXNU\Payloads\kernel.macho
from MainOS — which is where the prelinked kernel has to live, because EFIESP is 32 MB and
the image is 56 MB.

The binary is EfiFs's read-only NTFS driver, published by pbatard at
https://github.com/pbatard/EfiFs/releases/tag/v1.13 under GPLv3; the copy is checked against
the hash that release published, so a changed download is refused rather than staged.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URL = "https://github.com/pbatard/EfiFs/releases/download/v1.13/ntfs_aa64.efi"
SHA256 = "a56a93d2ea6f97e3548fae54eddc806288443f8b7d7f61c741fc7857b5b60096"
SIZE = 61440


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "out/ntfs_aa64.efi",
                        help="where to write the driver (default: out/ntfs_aa64.efi)")
    parser.add_argument("--force", action="store_true",
                        help="download again even if a verified copy is already there")
    args = parser.parse_args()

    out = args.out if args.out.is_absolute() else ROOT / args.out
    if out.is_file() and not args.force:
        data = out.read_bytes()
        if len(data) == SIZE and hashlib.sha256(data).hexdigest() == SHA256:
            print(f"{out} is already the pinned v1.13 driver")
            return 0

    print(f"== fetch {URL}")
    try:
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read()
    except OSError as error:
        print(f"error: {error}; download the file by hand from {URL} and pass --out", file=sys.stderr)
        return 1

    digest = hashlib.sha256(data).hexdigest()
    if data[:2] != b"MZ" or len(data) != SIZE or digest != SHA256:
        print(f"error: {len(data)} B, sha256 {digest}, magic {data[:2]!r} is not the pinned "
              f"driver ({SIZE} B, {SHA256}); refusing to stage it", file=sys.stderr)
        return 1

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"{out}: {SIZE} B, sha256 {digest[:16]}")
    print("EfiFs v1.13, GPLv3, read-only NTFS; see https://github.com/pbatard/EfiFs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
