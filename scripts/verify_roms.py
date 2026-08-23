#!/usr/bin/env python3
"""Perform structural checks on release ROMs without requiring an emulator."""

from __future__ import annotations

import hashlib
import os
import sys

Z64_MAGIC = bytes.fromhex("80371240")
MIN_ROM_SIZE = 512 * 1024
MAX_ROM_SIZE = 64 * 1024 * 1024


def verify(path: str) -> str:
    with open(path, "rb") as handle:
        data = handle.read()
    size = len(data)
    if data[:4] != Z64_MAGIC:
        raise RuntimeError(f"{path}: not a big-endian .z64 ROM")
    if not MIN_ROM_SIZE <= size <= MAX_ROM_SIZE:
        raise RuntimeError(f"{path}: implausible size {size}")
    if size % 16_384 != 0:
        raise RuntimeError(f"{path}: size is not aligned to 16 KiB")
    title = data[0x20:0x34].decode("ascii", errors="replace").rstrip(" \0")
    if not title.startswith("Teeworlds 64"):
        raise RuntimeError(f"{path}: unexpected ROM title {title!r}")
    digest = hashlib.sha256(data).hexdigest()
    print(f"{digest}  {os.path.basename(path)} ({size} bytes, {title})")
    return digest


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} ROM...", file=sys.stderr)
        return 2
    digests = [verify(path) for path in sys.argv[1:]]
    if len(set(digests)) != len(digests):
        raise RuntimeError("two release ROM variants are byte-identical")
    print(f"ROM verification passed for {len(digests)} variants")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"verify_roms: {error}", file=sys.stderr)
        sys.exit(1)
