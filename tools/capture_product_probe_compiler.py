#!/usr/bin/env python3
"""Capture exact product-probe compiler invocations from one build contract."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import capture_compiler_invocation as capture


CONTRACT_NAME = "talas-product-probe-compiler-contract.json"


def load_contract(working_directory: Path) -> dict[str, str]:
    for directory in (working_directory, *working_directory.parents):
        path = directory / CONTRACT_NAME
        if not path.is_file():
            continue
        value = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(value, dict)
            or set(value) != {
                "format", "version", "sourceRoot", "commit",
                "juceRoot", "juceCommit", "logDir"}
            or value.get("format") != "talas.product-probe-compiler-contract"
            or type(value.get("version")) is not int
            or value.get("version") != 2
            or not all(isinstance(value.get(key), str) and value[key]
                       for key in (
                           "sourceRoot", "commit", "juceRoot",
                           "juceCommit", "logDir"))):
            raise ValueError("product probe compiler contract is invalid")
        return value
    raise ValueError("product probe compiler contract is missing")


def main() -> int:
    command = sys.argv[1:]
    if not command:
        print("product probe compiler command is missing", file=sys.stderr)
        return 2
    working_directory = Path.cwd().resolve()
    try:
        contract = load_contract(working_directory)
        source_root = Path(contract["sourceRoot"]).resolve(strict=True)
        juce_root = Path(contract["juceRoot"]).resolve(strict=True)
        log_dir = Path(contract["logDir"]).resolve()
        expected_commit = contract["commit"]
        expected_juce_commit = contract["juceCommit"]
        output = capture.compiler_output(command, working_directory)
        capture.verify_source(source_root, expected_commit)
        capture.verify_source(juce_root, expected_juce_commit)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"product probe compiler capture: {error}", file=sys.stderr)
        return 2

    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        return completed.returncode if completed.returncode > 0 \
            else 128 - completed.returncode
    try:
        if not output.is_file():
            raise ValueError("successful compiler invocation created no output")
        capture.verify_source(source_root, expected_commit)
        capture.verify_source(juce_root, expected_juce_commit)
        key = hashlib.sha256(str(output).encode("utf-8")).hexdigest()
        record = {
            "format": "talas.compiler-invocation",
            "version": 3,
            "directory": str(working_directory),
            "arguments": command,
            "output": str(output),
            "commit": expected_commit,
            "juceCommit": expected_juce_commit,
            "outputSha256": capture.file_sha256(output),
        }
        capture.write_atomic(
            log_dir / f"{key}.json",
            json.dumps(record, indent=2, sort_keys=True) + "\n")
    except (OSError, ValueError) as error:
        print(f"product probe compiler capture: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
