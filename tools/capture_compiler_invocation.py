#!/usr/bin/env python3
"""Run a compiler command and atomically record the argv that actually ran."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


def write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(text)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def compiler_output(arguments: list[str], working_directory: Path) -> Path:
    outputs: list[str] = []
    for index, value in enumerate(arguments):
        if value == "-o":
            if index + 1 >= len(arguments):
                raise ValueError("compiler -o option is missing its output")
            outputs.append(arguments[index + 1])
        elif value.startswith("-o") and len(value) > 2:
            outputs.append(value[2:])
    if len(outputs) != 1:
        raise ValueError("compiler invocation must contain exactly one output")
    output = Path(outputs[0])
    if not output.is_absolute():
        output = working_directory / output
    return output.resolve()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_source(source_root: Path, expected_commit: str) -> None:
    head = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=source_root, text=True, capture_output=True, check=False)
    if head.returncode != 0 or head.stdout.strip() != expected_commit:
        raise ValueError("benchmark source commit changed")
    dirty = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--"],
        cwd=source_root, check=False)
    if dirty.returncode != 0:
        raise ValueError("benchmark compiler requires clean tracked sources")


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--source-root")
    parser.add_argument("--expected-commit")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("compiler command is required after --")
    if (arguments.source_root is None) != (arguments.expected_commit is None):
        parser.error("--source-root and --expected-commit must be used together")

    source_root = None
    if arguments.source_root is not None:
        source_root = Path(arguments.source_root).resolve()
        if (len(arguments.expected_commit) != 40
            or any(character not in "0123456789abcdef"
                   for character in arguments.expected_commit)):
            parser.error("--expected-commit must be 40 lowercase hex digits")
        try:
            verify_source(source_root, arguments.expected_commit)
        except (OSError, ValueError) as error:
            parser.error(str(error))

    working_directory = Path.cwd().resolve()
    try:
        output = compiler_output(command, working_directory)
    except ValueError as error:
        parser.error(str(error))

    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        return completed.returncode if completed.returncode > 0 else 128 - completed.returncode
    if not output.is_file():
        parser.error("successful compiler invocation did not create its output")

    if source_root is not None:
        try:
            verify_source(source_root, arguments.expected_commit)
        except (OSError, ValueError) as error:
            parser.error(str(error))

    key = hashlib.sha256(str(output).encode("utf-8")).hexdigest()
    record = {
        "format": "talas.compiler-invocation",
        "version": 2 if source_root is not None else 1,
        "directory": str(working_directory),
        "arguments": command,
        "output": str(output),
    }
    if source_root is not None:
        record["commit"] = arguments.expected_commit
        record["outputSha256"] = file_sha256(output)
    write_atomic(
        Path(arguments.log_dir) / f"{key}.json",
        json.dumps(record, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
