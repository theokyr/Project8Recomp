#!/usr/bin/env python3
"""Fail when a Linux release requires a newer ABI than the Steam Deck build floor."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


VERSION = re.compile(r"\b(GLIBCXX|GLIBC)_([0-9]+(?:\.[0-9]+)+)\b")


def version(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def elf_files(paths: list[Path]) -> list[Path]:
    result: list[Path] = []
    for path in paths:
        candidates = path.rglob("*") if path.is_dir() else [path]
        for candidate in candidates:
            if not candidate.is_file():
                continue
            try:
                with candidate.open("rb") as stream:
                    magic = stream.read(4)
                if magic == b"\x7fELF":
                    result.append(candidate)
            except OSError:
                continue
    return sorted(set(result))


def requirements(path: Path) -> dict[str, tuple[int, ...]]:
    output = subprocess.run(
        ["readelf", "--version-info", str(path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout
    maxima: dict[str, tuple[int, ...]] = {}
    for family, raw in VERSION.findall(output):
        maxima[family] = max(maxima.get(family, (0,)), version(raw))
    return maxima


def display(value: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--max-glibc", default="2.39")
    parser.add_argument("--max-glibcxx", default="3.4.32")
    args = parser.parse_args()

    limits = {
        "GLIBC": version(args.max_glibc),
        "GLIBCXX": version(args.max_glibcxx),
    }
    files = elf_files(args.paths)
    if not files:
        print("no ELF files found", file=sys.stderr)
        return 2

    failed = False
    overall: dict[str, tuple[int, ...]] = {}
    for path in files:
        needed = requirements(path)
        for family, required in needed.items():
            overall[family] = max(overall.get(family, (0,)), required)
            if required > limits[family]:
                print(
                    f"!! {path}: {family}_{display(required)} exceeds "
                    f"{family}_{display(limits[family])}",
                    file=sys.stderr,
                )
                failed = True

    for family in ("GLIBC", "GLIBCXX"):
        found = overall.get(family, (0,))
        print(
            f"{family}: {display(found)} "
            f"(allowed through {display(limits[family])})"
        )
    print(f"ELF files checked: {len(files)}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
