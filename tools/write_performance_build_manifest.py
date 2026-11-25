#!/usr/bin/env python3
"""Write a normalized compile/link manifest and SHA-256 benchmark fingerprint."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path


HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
LIBRARY_TARGET = re.compile(r"\blib([A-Za-z0-9_]+)\.(?:a|dylib|so)\b")
OBJECT_TARGET = re.compile(r"CMakeFiles/([^/]+)\.dir/")


def normalized(value: str, source_root: Path, build_root: Path) -> str:
    result = value.replace(str(build_root), "$BUILD").replace(
        str(source_root), "$SOURCE")
    result = re.sub(
        r'TALAS_BENCHMARK_COMMIT=\\"[0-9a-f]{40}\\"',
        'TALAS_BENCHMARK_COMMIT=<COMMIT>', result)
    result = re.sub(
        r'TALAS_BENCHMARK_COMMIT="[0-9a-f]{40}"',
        'TALAS_BENCHMARK_COMMIT=<COMMIT>', result)
    result = re.sub(
        r'TALAS_PRODUCT_PROBE_COMMIT=\\"[0-9a-f]{40}\\"',
        'TALAS_PRODUCT_PROBE_COMMIT=<COMMIT>', result)
    return re.sub(
        r'TALAS_PRODUCT_PROBE_COMMIT="[0-9a-f]{40}"',
        'TALAS_PRODUCT_PROBE_COMMIT=<COMMIT>', result)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


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


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--compile-commands", required=True)
    parser.add_argument("--invocation-log-dir", required=True)
    parser.add_argument("--link-command", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--configuration-fingerprint", required=True)
    parser.add_argument("--root-target", default="talas_core_performance_benchmarks")
    parser.add_argument("--executable")
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--fingerprint", required=True)
    arguments = parser.parse_args()

    source_root = Path(arguments.source_root).resolve()
    build_root = Path(arguments.build_root).resolve()
    if not HEX_SHA256.fullmatch(arguments.configuration_fingerprint):
        parser.error("configuration fingerprint must be 64 lowercase hex digits")

    compile_path = Path(arguments.compile_commands)
    invocation_log_dir = Path(arguments.invocation_log_dir)
    link_path = Path(arguments.link_command)
    try:
        commands = json.loads(compile_path.read_text(encoding="utf-8"))
        link_command = link_path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        parser.error(f"cannot read build commands: {error}")
    if not isinstance(commands, list) or not commands or not link_command:
        parser.error("build commands must be non-empty")

    if not re.fullmatch(r"[A-Za-z0-9_]+", arguments.root_target):
        parser.error("root target is invalid")
    targets = {arguments.root_target}
    targets.update(LIBRARY_TARGET.findall(link_command))
    targets.update(OBJECT_TARGET.findall(link_command))
    compile_entries = []
    for index, entry in enumerate(commands):
        if not isinstance(entry, dict):
            parser.error(f"compile command {index} must be an object")
        if set(entry) not in ({"directory", "command", "file", "output"},
                              {"directory", "arguments", "file", "output"}):
            parser.error(f"compile command {index} has unexpected fields")
        output = entry.get("output")
        file_name = entry.get("file")
        if not isinstance(output, str) or not isinstance(file_name, str):
            parser.error(f"compile command {index} has invalid paths")
        match = OBJECT_TARGET.search(output)
        if match is None or match.group(1) not in targets:
            continue
        directory = entry.get("directory")
        if not isinstance(directory, str) or not directory:
            parser.error(f"compile command {index} has invalid directory")
        expected_output = Path(output)
        if not expected_output.is_absolute():
            expected_output = Path(directory) / expected_output
        expected_output = expected_output.resolve()
        invocation_key = hashlib.sha256(
            str(expected_output).encode("utf-8")).hexdigest()
        invocation_path = invocation_log_dir / f"{invocation_key}.json"
        try:
            invocation = json.loads(invocation_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            parser.error(
                f"cannot read actual compiler invocation for {output}: {error}")
        version = invocation.get("version") if isinstance(invocation, dict) else None
        expected_fields = {
            "format", "version", "directory", "arguments", "output"}
        if version == 2:
            expected_fields |= {"commit", "outputSha256"}
        elif version == 3:
            expected_fields |= {"commit", "juceCommit", "outputSha256"}
        if (not isinstance(invocation, dict)
            or set(invocation) != expected_fields
            or invocation.get("format") != "talas.compiler-invocation"
            or type(invocation.get("version")) is not int
            or invocation.get("version") not in (1, 2, 3)
            or not isinstance(invocation.get("directory"), str)
            or not isinstance(invocation.get("arguments"), list)
            or not invocation["arguments"]
            or not all(isinstance(value, str) for value in invocation["arguments"])
            or not isinstance(invocation.get("output"), str)):
            parser.error(f"actual compiler invocation for {output} is invalid")
        if Path(invocation["output"]).resolve() != expected_output:
            parser.error(f"actual compiler invocation output mismatches {output}")
        compile_entry = {
            "target": match.group(1),
            "file": normalized(file_name, source_root, build_root),
            "output": normalized(str(expected_output), source_root, build_root),
            "directory": normalized(
                invocation["directory"], source_root, build_root),
            "arguments": [
                normalized(value, source_root, build_root)
                for value in invocation["arguments"]
            ],
        }
        if version in (2, 3):
            if (not isinstance(invocation["commit"], str)
                or not re.fullmatch(r"[0-9a-f]{40}", invocation["commit"])
                or not isinstance(invocation["outputSha256"], str)
                or not HEX_SHA256.fullmatch(invocation["outputSha256"])):
                parser.error(f"actual compiler provenance for {output} is invalid")
            try:
                current_output_sha256 = file_sha256(expected_output)
            except OSError as error:
                parser.error(f"cannot hash compiler output {output}: {error}")
            if current_output_sha256 != invocation["outputSha256"]:
                parser.error(f"compiler output identity changed for {output}")
            compile_entry["commit"] = invocation["commit"]
            compile_entry["outputSha256"] = invocation["outputSha256"]
            if version == 3:
                if (not isinstance(invocation["juceCommit"], str)
                    or not re.fullmatch(
                        r"[0-9a-f]{40}", invocation["juceCommit"])):
                    parser.error(
                        f"actual compiler JUCE provenance for {output} is invalid")
                compile_entry["juceCommit"] = invocation["juceCommit"]
        compile_entries.append(compile_entry)
    if not compile_entries:
        parser.error("no transitive benchmark compile commands were found")
    compiled_targets = {entry["target"] for entry in compile_entries}
    missing_targets = sorted(targets - compiled_targets)
    if missing_targets:
        parser.error(
            "missing transitive benchmark compile commands: "
            + ", ".join(missing_targets))
    compile_entries.sort(key=lambda item: (
        item["target"], item["file"], item["output"], item["arguments"]))

    payload = {
        "format": "talas.performance-build-manifest",
        "version": 1,
        "generator": arguments.generator,
        "configurationFingerprint": arguments.configuration_fingerprint,
        "targets": sorted(targets),
        "compile": compile_entries,
        "link": normalized(link_command, source_root, build_root),
    }
    if arguments.executable is not None:
        executable = Path(arguments.executable).resolve()
        try:
            metadata = executable.stat()
            executable_sha256 = file_sha256(executable)
        except OSError as error:
            parser.error(f"cannot inspect root executable: {error}")
        if not executable.is_file() or metadata.st_size < 1:
            parser.error("root executable must be a non-empty regular file")
        payload["executable"] = {
            "path": normalized(str(executable), source_root, build_root),
            "bytes": metadata.st_size,
            "sha256": executable_sha256,
        }
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    fingerprint = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    manifest = dict(payload)
    manifest["fingerprint"] = fingerprint
    write_atomic(
        Path(arguments.manifest),
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    write_atomic(Path(arguments.fingerprint), fingerprint + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
