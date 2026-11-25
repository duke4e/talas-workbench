#!/usr/bin/env python3
"""Check that byte-compared text keeps LF and binary assets stay binary."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

TEXT_SUFFIXES = {".txt", ".json", ".md", ".csv"}
BINARY_SUFFIXES = {".wav", ".mid"}


def hashed_example_assets(root: Path) -> list[str]:
    paths = set()
    for example in sorted((root / "examples").glob("*.talas.json")):
        document = json.loads(example.read_text(encoding="utf-8"))
        for asset in document.get("assets", []):
            if asset.get("contentHash") and asset.get("path"):
                resolved = (example.parent / asset["path"]).resolve()
                paths.add(resolved.relative_to(root).as_posix())
    return sorted(paths)


def attributes(root: Path, paths: list[str]) -> dict[str, dict[str, str]]:
    completed = subprocess.run(
        ["git", "check-attr", "text", "eol", "--", *paths],
        cwd=root, capture_output=True, check=True, text=True)
    result: dict[str, dict[str, str]] = {}
    for line in completed.stdout.splitlines():
        path, name, value = line.rsplit(": ", 2)
        result.setdefault(path, {})[name] = value
    return result


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    inside = subprocess.run(
        ["git", "rev-parse", "--is-inside-work-tree"],
        cwd=root, capture_output=True, text=True)
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        print("Line endings: SKIP: not a git work tree")
        return 0
    paths = sorted(set(hashed_example_assets(root)))
    failures = []
    for path, values in attributes(root, paths).items():
        suffix = Path(path).suffix
        if suffix in TEXT_SUFFIXES and values.get("eol") != "lf":
            failures.append(f"{path} must be checked out with eol=lf")
        elif suffix in BINARY_SUFFIXES and values.get("text") != "unset":
            failures.append(f"{path} must be marked binary")
        elif suffix not in TEXT_SUFFIXES | BINARY_SUFFIXES:
            failures.append(f"{path} has no line-ending policy")
    if failures:
        for failure in failures:
            print(f"Line endings: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"Line endings: PASS: {len(paths)} byte-compared files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
