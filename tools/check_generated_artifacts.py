#!/usr/bin/env python3
"""Check all descriptor-derived repository artifacts without rewriting them."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


class GeneratedArtifactError(RuntimeError):
    pass


def run_checked(command: list[str], root: Path, label: str) -> str:
    completed = subprocess.run(
        command,
        cwd=root,
        capture_output=True,
        check=False,
        timeout=60,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).decode(
            "utf-8", errors="replace").strip()
        raise GeneratedArtifactError(
            f"{label} failed" + (f": {detail}" if detail else ""))
    if completed.stderr:
        raise GeneratedArtifactError(f"{label} wrote unexpected stderr")
    return completed.stdout.decode("utf-8", errors="strict")


def canonical_stdout_bytes(value: str, label: str) -> bytes:
    normalized = value.replace("\r\n", "\n")
    if "\r" in normalized:
        raise GeneratedArtifactError(f"{label} contains a bare carriage return")
    return normalized.encode("utf-8")


def check_catalog(cli: Path, root: Path) -> None:
    actual = canonical_stdout_bytes(
        run_checked(
            [str(cli), "catalog", "--format", "json"],
            root,
            "live catalog projection",
        ),
        "live catalog projection",
    )
    expected_path = root / "docs/coverage/builtin-node-catalog-v2.json"
    if not expected_path.is_file() or actual != expected_path.read_bytes():
        raise GeneratedArtifactError(
            f"generated catalog artifact is stale: {expected_path}")


def check_child_projections(
    root: Path,
    python_executable: str,
) -> tuple[str, str]:
    def clean_success(value: str, pattern: str, label: str) -> str:
        canonical = canonical_stdout_bytes(value, label).decode("utf-8")
        if re.fullmatch(pattern, canonical) is None:
            raise GeneratedArtifactError(
                f"{label} returned malformed success output")
        return canonical[:-1]

    capability = clean_success(run_checked(
        [python_executable, "-B", str(root / "tools/report_capability_matrix.py"),
         "--check"],
        root,
        "capability matrix check",
    ), (
        r"Capability matrix v2: offline \{'unsupported': \d+, 'subset': \d+, "
        r"'supported': \d+\}; prepared realtime \{'unsupported': \d+, "
        r"'subset': \d+, 'supported': \d+\}\n"
    ), "capability matrix check")
    node_help = clean_success(run_checked(
        [python_executable, "-B", str(root / "tools/report_node_help.py"),
         "--check"],
        root,
        "node help check",
    ), r"Node help: PASS: \d+ nodes, \d+ examples\n", "node help check")
    return capability, node_help


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--cli", required=True, type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        cli = arguments.cli.resolve(strict=True)
        if not cli.is_file() or not os.access(cli, os.X_OK):
            raise GeneratedArtifactError("--cli must be an executable file")
        check_catalog(cli, root)
        capability, node_help = check_child_projections(
            root, sys.executable)
    except (GeneratedArtifactError, OSError, UnicodeError,
            subprocess.TimeoutExpired) as error:
        print(f"Generated artifact check: FAIL: {error}", file=sys.stderr)
        return 1
    print("Generated catalog artifact: PASS")
    print(capability)
    print(node_help)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
