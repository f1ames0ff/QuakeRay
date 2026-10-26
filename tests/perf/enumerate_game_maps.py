#!/usr/bin/env python3
"""Enumerate the top-level BSPs a Quake game directory contributes to `map`."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


PAK_ENTRY_SIZE = 64
MAP_MIN_SIZE = 32 * 1024
PAK_NAME = re.compile(r"^pak(\d+)\.pak$", re.IGNORECASE)
TOP_LEVEL_MAP = re.compile(r"^maps/([^/]+)\.bsp$", re.IGNORECASE)


def read_directory(pak_path: Path) -> dict[str, tuple[int, int]]:
    with pak_path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"PACK":
            raise ValueError(f"{pak_path} is not a Quake PAK")
        offset, length = struct.unpack("<II", header[4:])
        stream.seek(offset)
        directory = stream.read(length)

    entries: dict[str, tuple[int, int]] = {}
    for position in range(0, len(directory), PAK_ENTRY_SIZE):
        raw = directory[position : position + PAK_ENTRY_SIZE]
        if len(raw) != PAK_ENTRY_SIZE:
            break
        name = raw[:56].split(b"\0", 1)[0].decode("latin-1").replace("\\", "/")
        file_offset, file_length = struct.unpack("<II", raw[56:])
        entries[name.lower()] = (file_offset, file_length)
    return entries


def hash_entry(pak_path: Path, offset: int, length: int) -> str:
    digest = hashlib.sha256()
    with pak_path.open("rb") as stream:
        stream.seek(offset)
        remaining = length
        while remaining:
            chunk = stream.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise ValueError(f"truncated PAK entry in {pak_path}")
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", required=True, type=Path)
    parser.add_argument("--game", required=True, choices=("id1", "mg1", "mg3", "ad"))
    args = parser.parse_args()

    game_dir = args.data_root / args.game
    paks = [path for path in game_dir.glob("*.pak") if PAK_NAME.match(path.name)]
    paks.sort(key=lambda path: int(PAK_NAME.match(path.name).group(1)))
    if not paks:
        raise SystemExit(f"no pak<number>.pak files found in {game_dir}")

    winners: dict[str, tuple[Path, int, int]] = {}
    for pak_path in paks:
        for name, (offset, length) in read_directory(pak_path).items():
            winners[name] = (pak_path, offset, length)

    maps = []
    for name, (pak_path, offset, length) in sorted(winners.items()):
        match = TOP_LEVEL_MAP.match(name)
        if match and length >= MAP_MIN_SIZE:
            maps.append(
                {
                    "name": match.group(1),
                    "entry": name,
                    "size": length,
                    "sha256": hash_entry(pak_path, offset, length),
                    "pak": pak_path.name,
                }
            )

    print(
        json.dumps(
            {
                "game": args.game,
                "data_root": str(args.data_root),
                "paks": [path.name for path in paks],
                "count": len(maps),
                "maps": maps,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
