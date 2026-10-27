#!/usr/bin/env python3
"""Turn interruptmap.txt into the compiled-in runtime syscall registry."""

import argparse
from pathlib import Path


def parse_map(path: Path):
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.strip().rstrip(",")
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{line_number}: expected NAME = NUMBER")
        name, number = (part.strip() for part in line.split("=", 1))
        if not name.isidentifier():
            raise ValueError(f"{path}:{line_number}: invalid C symbol {name!r}")
        yield name, int(number, 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("map", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    entries = list(parse_map(args.map))
    if len({number for _, number in entries}) != len(entries):
        raise ValueError("duplicate syscall number in interrupt map")

    lines = ["/* Generated from tools/interruptmap.txt. */"]
    lines.extend(
        f"PRIMESHIM_SYSCALL(0x{number:06x}u, {name})"
        for name, number in entries
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
