#!/usr/bin/env python3
"""Build and collect a strict local Workbench Release performance envelope."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import platform
import re
import secrets
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


EXPECTED_XPC = {
    "TalasComponentHost.xpc": "xpc-host",
    "TalasComponentProbe.xpc": "xpc-probe",
    "TalasComponentRuntime.xpc": "xpc-runtime",
}
EXECUTABLES = {
    "workbench-main": Path("Contents/MacOS/Talas Workbench"),
    "xpc-host": Path(
        "Contents/XPCServices/TalasComponentHost.xpc/Contents/MacOS/"
        "TalasComponentHost"),
    "xpc-probe": Path(
        "Contents/XPCServices/TalasComponentProbe.xpc/Contents/MacOS/"
        "TalasComponentProbe"),
    "xpc-runtime": Path(
        "Contents/XPCServices/TalasComponentRuntime.xpc/Contents/MacOS/"
        "TalasComponentRuntime"),
}
HEX_COMMIT = re.compile(r"^[0-9a-f]{40}$")
HEX_RUN_ID = re.compile(r"^[0-9a-f]{32}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
PINNED_JUCE_COMMIT = "91ad83ae34a81e0833b1a2b0866f54846370ae53"
CANONICAL_BUILD_PATH = "/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
COMPILER_VALUE = re.compile(
    r'^set\(CMAKE_CXX_COMPILER_(ID|VERSION) "([^"]+)"\)$')


class CollectorError(RuntimeError):
    pass


class StoreOnce(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"duplicate {option_string} option")
        setattr(namespace, self.dest, values)


def checker_module():
    path = Path(__file__).with_name("check_product_performance_budget.py")
    spec = importlib.util.spec_from_file_location(
        "talas_product_performance_checker", path)
    if spec is None or spec.loader is None:
        raise CollectorError("cannot load product performance schema validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def write_atomic(path: Path, value: dict[str, Any]) -> None:
    if path.exists():
        raise CollectorError("report path already exists")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
        os.link(temporary, path)
        os.unlink(temporary)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as error:
        raise CollectorError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def artifact_record(artifact_id: str, path: Path, root: Path) -> dict[str, Any]:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise CollectorError(f"cannot inspect artifact {path}: {error}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise CollectorError(f"artifact is not a regular file: {path}")
    return {
        "id": artifact_id,
        "path": path.relative_to(root).as_posix(),
        "bytes": metadata.st_size,
        "sha256": file_sha256(path),
    }


def collect_bundle(bundle: Path) -> dict[str, Any]:
    try:
        root = bundle.resolve(strict=True)
    except OSError as error:
        raise CollectorError(f"cannot resolve app bundle {bundle}: {error}") from error
    if not root.is_dir():
        raise CollectorError(f"app bundle is not a directory: {root}")

    entries: list[dict[str, Any]] = []
    total_bytes = 0
    for current, directory_names, file_names in os.walk(root, followlinks=False):
        directory_names.sort()
        file_names.sort()
        current_path = Path(current)
        for name in [*directory_names, *file_names]:
            path = current_path / name
            relative = path.relative_to(root).as_posix()
            try:
                metadata = path.lstat()
            except OSError as error:
                raise CollectorError(f"cannot inspect bundle entry {path}: {error}") from error
            if stat.S_ISLNK(metadata.st_mode):
                try:
                    target = path.resolve(strict=True)
                except OSError as error:
                    raise CollectorError(f"broken bundle symlink {relative}: {error}") from error
                if not target.is_relative_to(root):
                    raise CollectorError(f"bundle symlink escapes root: {relative}")
                entries.append({
                    "path": relative,
                    "type": "symlink",
                    "target": os.readlink(path),
                })
            elif stat.S_ISDIR(metadata.st_mode):
                entries.append({"path": relative, "type": "directory"})
            elif stat.S_ISREG(metadata.st_mode):
                total_bytes += metadata.st_size
                entries.append({
                    "path": relative,
                    "type": "file",
                    "bytes": metadata.st_size,
                    "sha256": file_sha256(path),
                })
            else:
                raise CollectorError(f"unsupported bundle entry type: {relative}")
    entries.sort(key=lambda entry: entry["path"])
    canonical = json.dumps(
        entries, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return {
        "id": "app-bundle",
        "path": ".",
        "bytes": total_bytes,
        "sha256": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
    }


def collect_artifacts(bundle: Path) -> dict[str, Any]:
    bundle = bundle.resolve(strict=True)
    services = bundle / "Contents" / "XPCServices"
    if not services.is_dir():
        raise CollectorError("app bundle has no XPCServices directory")
    actual_services = {
        path.name for path in services.iterdir()
        if path.name.endswith(".xpc")
    }
    if actual_services != set(EXPECTED_XPC):
        raise CollectorError(
            "XPC service set mismatch: "
            f"missing={sorted(set(EXPECTED_XPC) - actual_services)} "
            f"unexpected={sorted(actual_services - set(EXPECTED_XPC))}")
    executables = [
        artifact_record(artifact_id, bundle / relative, bundle)
        for artifact_id, relative in EXECUTABLES.items()
    ]
    return {
        "availability": "available",
        "authority": "budgeted",
        "bundle": collect_bundle(bundle),
        "executables": executables,
    }


def normalized_cache_fingerprint(
    cache_path: Path, source_root: Path, build_root: Path) -> str:
    try:
        raw_lines = cache_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise CollectorError(f"cannot read CMake cache: {error}") from error
    entries = []
    for raw in raw_lines:
        if not raw or raw.startswith("#") or raw.startswith("//") or "=" not in raw:
            continue
        normalized = raw.replace(str(build_root), "$BUILD").replace(
            str(source_root), "$SOURCE")
        entries.append(normalized)
    if not entries:
        raise CollectorError("CMake cache has no fingerprintable entries")
    canonical = "\n".join(sorted(entries)) + "\n"
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def cache_values(cache_path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in cache_path.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#") or raw.startswith("//") or "=" not in raw:
            continue
        left, value = raw.split("=", 1)
        name = left.split(":", 1)[0]
        result[name] = value
    return result


def compiler_identity(build_root: Path) -> tuple[str, str]:
    candidates = sorted(
        (build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(candidates) != 1:
        raise CollectorError("cannot identify the configured C++ compiler metadata")
    values: dict[str, str] = {}
    for line in candidates[0].read_text(encoding="utf-8").splitlines():
        match = COMPILER_VALUE.fullmatch(line)
        if match is not None:
            values[match.group(1)] = match.group(2)
    if set(values) != {"ID", "VERSION"}:
        raise CollectorError("configured C++ compiler identity is incomplete")
    return values["ID"], values["VERSION"]


def git_output(source_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments], cwd=source_root, text=True,
        capture_output=True, check=False)
    if completed.returncode != 0:
        raise CollectorError(completed.stderr.strip() or "Git command failed")
    return completed.stdout.strip()


def verify_source(source_root: Path, expected_commit: str) -> None:
    if git_output(source_root, "rev-parse", "--verify", "HEAD") != expected_commit:
        raise CollectorError("source HEAD does not match --expected-commit")
    dirty = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--"], cwd=source_root,
        check=False)
    if dirty.returncode != 0:
        raise CollectorError("collector requires clean tracked sources")


def verify_juce_source(juce_root: Path) -> Path:
    try:
        resolved = juce_root.resolve(strict=True)
    except OSError as error:
        raise CollectorError(f"cannot resolve local JUCE source: {error}") from error
    if not (resolved / "CMakeLists.txt").is_file():
        raise CollectorError("local JUCE source has no CMakeLists.txt")
    if git_output(resolved, "rev-parse", "--verify", "HEAD") != PINNED_JUCE_COMMIT:
        raise CollectorError("local JUCE source is not the pinned exact commit")
    dirty = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--"], cwd=resolved,
        check=False)
    if dirty.returncode != 0:
        raise CollectorError("local JUCE source has dirty tracked files")
    return resolved


def validate_report_destination(
    report_path: Path,
    source_root: Path,
    build_root: Path,
) -> None:
    resolved = report_path.resolve()
    if resolved.exists():
        raise CollectorError("report path already exists")
    if (resolved.is_relative_to(source_root.resolve())
        or resolved.is_relative_to(build_root.resolve())):
        raise CollectorError("report path must be outside source and build roots")


def deterministic_build_environment(
    build_root: Path,
    source: dict[str, str] | None = None,
    sdk_root: str | None = None,
) -> dict[str, str]:
    _ = os.environ if source is None else source
    result = {
        "PATH": CANONICAL_BUILD_PATH,
        "HOME": str(build_root / ".talas-home"),
        "TMPDIR": str(build_root / ".talas-tmp"),
    }
    result["SOURCE_DATE_EPOCH"] = "0"
    result["LC_ALL"] = "C"
    result["LANG"] = "C"
    if sdk_root is not None:
        result["SDKROOT"] = sdk_root
    return result


def build_environment_fingerprint(
    environment: dict[str, str],
    source_root: Path,
    build_root: Path,
) -> str:
    normalized = {
        key: value.replace(str(build_root), "$BUILD").replace(
            str(source_root), "$SOURCE")
        for key, value in environment.items()
    }
    canonical = json.dumps(
        normalized, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def canonical_macos_sdk_root(source_root: Path) -> str:
    completed = subprocess.run(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
        cwd=source_root, text=True, capture_output=True, check=False)
    sdk_root = completed.stdout.strip()
    if (completed.returncode != 0 or not sdk_root
        or not Path(sdk_root).is_absolute()
        or not Path(sdk_root).is_dir()):
        raise CollectorError("cannot resolve the canonical macOS SDK root")
    return sdk_root


def build_workbench(
    source_root: Path,
    build_root: Path,
    jobs: int,
    juce_root: Path,
) -> tuple[dict[str, Any], str]:
    if build_root.exists():
        raise CollectorError("build directory must not already exist")
    build_root.mkdir(parents=True)
    (build_root / ".talas-home").mkdir()
    (build_root / ".talas-tmp").mkdir()
    sdk_root = canonical_macos_sdk_root(source_root)
    command = (
        f"sanitized-env SOURCE_DATE_EPOCH=0 SDKROOT={sdk_root} "
        f"cmake --preset workbench-release -B {build_root} "
        f"-DTALAS_FETCH_JUCE=OFF -DTALAS_JUCE_PATH={juce_root} "
        f"-DFETCHCONTENT_FULLY_DISCONNECTED=ON && "
        f"cmake --build {build_root} --target talas_workbench -j {jobs}")
    environment = deterministic_build_environment(
        build_root, sdk_root=sdk_root)
    environment_fingerprint = build_environment_fingerprint(
        environment, source_root, build_root)
    started_utc = utc_now()
    started = time.monotonic_ns()
    configure = subprocess.run(
        ["cmake", "--preset", "workbench-release", "-B", str(build_root),
         "-DTALAS_FETCH_JUCE=OFF", f"-DTALAS_JUCE_PATH={juce_root}",
         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"],
        cwd=source_root, env=environment, check=False)
    if configure.returncode != 0:
        raise CollectorError("Workbench Release configure failed")
    build = subprocess.run(
        ["cmake", "--build", str(build_root), "--target", "talas_workbench",
         "-j", str(jobs)],
        cwd=source_root, env=environment, check=False)
    elapsed = time.monotonic_ns() - started
    finished_utc = utc_now()
    if build.returncode != 0:
        raise CollectorError("Workbench Release build failed")
    return {
        "availability": "available",
        "authority": "observational",
        "command": command,
        "startedUtc": started_utc,
        "finishedUtc": finished_utc,
        "elapsedNs": elapsed,
    }, environment_fingerprint


def package_section(path: Path | None, kind: str | None) -> dict[str, Any]:
    if path is None:
        return {
            "availability": "unavailable",
            "authority": "observational",
            "reason": "not-collected",
        }
    if kind is None:
        raise CollectorError("--package-kind is required with --package")
    resolved = path.resolve(strict=True)
    metadata = resolved.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise CollectorError("package is not a regular file")
    return {
        "availability": "available",
        "authority": "observational",
        "kind": kind,
        "artifact": {
            "id": "product-package",
            "path": resolved.name,
            "bytes": metadata.st_size,
            "sha256": file_sha256(resolved),
        },
    }


def load_probe_manifest(
    path: Path,
    executable_path: Path,
    expected_commit: str,
) -> tuple[str, str]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CollectorError(f"cannot read product probe manifest: {error}") from error
    expected_keys = {
        "format", "version", "generator", "configurationFingerprint",
        "targets", "compile", "link", "executable", "fingerprint",
    }
    if (not isinstance(manifest, dict)
        or set(manifest) != expected_keys
        or manifest.get("format") != "talas.performance-build-manifest"
        or type(manifest.get("version")) is not int
        or manifest.get("version") != 1
        or manifest.get("generator") != "Unix Makefiles"
        or not isinstance(manifest.get("configurationFingerprint"), str)
        or not HEX_SHA256.fullmatch(manifest["configurationFingerprint"])
        or not isinstance(manifest.get("targets"), list)
        or "talas_product_performance_probe" not in manifest["targets"]
        or not all(isinstance(target, str) and target for target in manifest["targets"])
        or len(set(manifest["targets"])) != len(manifest["targets"])
        or not isinstance(manifest.get("compile"), list)
        or not manifest["compile"]
        or not isinstance(manifest.get("link"), str)
        or not manifest["link"]):
        raise CollectorError("product probe manifest is invalid")
    for index, entry in enumerate(manifest["compile"]):
        if (not isinstance(entry, dict)
            or set(entry) != {
                "target", "file", "output", "directory", "arguments",
                "commit", "juceCommit", "outputSha256"}
            or entry.get("commit") != expected_commit
            or entry.get("juceCommit") != PINNED_JUCE_COMMIT
            or entry.get("target") not in manifest["targets"]
            or not isinstance(entry.get("file"), str)
            or not isinstance(entry.get("output"), str)
            or not isinstance(entry.get("directory"), str)
            or not isinstance(entry.get("arguments"), list)
            or not entry["arguments"]
            or not all(isinstance(value, str) for value in entry["arguments"])
            or not isinstance(entry.get("outputSha256"), str)
            or not HEX_SHA256.fullmatch(entry["outputSha256"])):
            raise CollectorError(
                f"product probe manifest compile entry {index} is invalid")
    if {entry["target"] for entry in manifest["compile"]} != set(manifest["targets"]):
        raise CollectorError("product probe manifest transitive target set is incomplete")
    executable = manifest.get("executable")
    if (not isinstance(executable, dict)
        or set(executable) != {"path", "bytes", "sha256"}
        or type(executable.get("bytes")) is not int
        or executable["bytes"] < 1
        or not isinstance(executable.get("sha256"), str)
        or not HEX_SHA256.fullmatch(executable["sha256"])):
        raise CollectorError("product probe manifest executable is invalid")
    payload = dict(manifest)
    fingerprint = payload.pop("fingerprint", None)
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    actual_fingerprint = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    if fingerprint != actual_fingerprint:
        raise CollectorError("product probe manifest fingerprint is invalid")
    try:
        resolved_executable = executable_path.resolve(strict=True)
        metadata = resolved_executable.stat()
    except OSError as error:
        raise CollectorError(f"cannot inspect product probe executable: {error}") from error
    actual_executable_sha256 = file_sha256(resolved_executable)
    if (not stat.S_ISREG(metadata.st_mode)
        or metadata.st_size != executable["bytes"]
        or actual_executable_sha256 != executable["sha256"]):
        raise CollectorError("product probe executable does not match manifest")
    return actual_fingerprint, actual_executable_sha256


def load_probe(
    path: Path,
    expected_commit: str,
    manifest_path: Path,
    executable_path: Path,
) -> dict[str, Any]:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in values:
            if key in result:
                raise CollectorError(f"duplicate product probe JSON key: {key}")
            result[key] = value
        return result

    try:
        with path.open("r", encoding="utf-8") as source:
            probe = json.load(source, object_pairs_hook=pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CollectorError(f"cannot read product probe report: {error}") from error
    if (not isinstance(probe, dict)
        or set(probe) != {
            "format", "version", "commit", "runId", "buildFingerprint",
            "executableSha256", "ui", "device"}
        or probe.get("format") != "talas.product-probe-report"
        or type(probe.get("version")) is not int
        or probe.get("version") != 2):
        raise CollectorError("product probe report has invalid fields or version")
    if probe["commit"] != expected_commit:
        raise CollectorError("product probe commit does not match collected artifacts")
    if not isinstance(probe["runId"], str) or not HEX_RUN_ID.fullmatch(probe["runId"]):
        raise CollectorError("product probe run identity is invalid")
    fingerprint, executable_sha256 = load_probe_manifest(
        manifest_path, executable_path, expected_commit)
    if probe["buildFingerprint"] != fingerprint:
        raise CollectorError("product probe report build fingerprint mismatches manifest")
    if probe["executableSha256"] != executable_sha256:
        raise CollectorError("product probe report executable identity mismatches manifest")
    result = {"ui": dict(probe["ui"]), "device": dict(probe["device"])}
    for section in result.values():
        section["probeBuildFingerprint"] = fingerprint
        section["probeExecutableSha256"] = executable_sha256
        section["probeRunId"] = probe["runId"]
    return result


def collect_report(
    source_root: Path,
    build_root: Path,
    expected_commit: str,
    jobs: int,
    package: Path | None,
    package_kind: str | None,
    probe_report: Path | None,
    probe_manifest: Path | None,
    probe_executable: Path | None,
    juce_root: Path,
) -> dict[str, Any]:
    verify_source(source_root, expected_commit)
    build, build_environment = build_workbench(
        source_root, build_root, jobs, juce_root)
    verify_source(source_root, expected_commit)
    verify_juce_source(juce_root)
    bundle = (
        build_root / "talas_workbench_artefacts" / "Release"
        / "Talas Workbench.app")
    verified = subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", "--verbose=2",
         str(bundle)],
        cwd=source_root, check=False)
    if verified.returncode != 0:
        raise CollectorError("deep/strict Workbench signature verification failed")
    cache_path = build_root / "CMakeCache.txt"
    cache = cache_values(cache_path)
    required_cache = {
        "CMAKE_BUILD_TYPE": "Release",
        "TALAS_BUILD_TESTS": "FALSE",
        "TALAS_BUILD_WORKBENCH": "TRUE",
        "TALAS_FETCH_JUCE": "OFF",
        "TALAS_BUILD_BENCHMARKS": "OFF",
        "TALAS_BUILD_PRODUCT_BENCHMARKS": "OFF",
        "FETCHCONTENT_FULLY_DISCONNECTED": "ON",
    }
    for key, expected in required_cache.items():
        if cache.get(key) != expected:
            raise CollectorError(f"unexpected Workbench cache value {key}")
    compiler_id, compiler_version = compiler_identity(build_root)
    observations = load_probe(
        probe_report, expected_commit, probe_manifest, probe_executable) \
        if probe_report is not None else {
            "ui": {
                "availability": "unavailable",
                "authority": "observational",
                "reason": "product-probe-not-collected",
            },
            "device": {
                "availability": "unavailable",
                "authority": "observational",
                "reason": "device-not-requested",
            },
        }
    report = {
        "format": "talas.product-performance-report",
        "version": 1,
        "environment": {
            "runId": secrets.token_hex(16),
            "commit": expected_commit,
            "platform": "macos" if sys.platform == "darwin" else sys.platform,
            "architecture": platform.machine(),
            "compilerId": compiler_id,
            "compilerVersion": compiler_version,
            "buildType": cache["CMAKE_BUILD_TYPE"],
            "preset": "workbench-release",
            "generator": cache.get("CMAKE_GENERATOR", ""),
            "configurationFingerprint": normalized_cache_fingerprint(
                cache_path, source_root, build_root),
            "buildEnvironmentFingerprint": build_environment,
            "collectorVersion": "1",
        },
        "build": build,
        "artifacts": collect_artifacts(bundle),
        "package": package_section(package, package_kind),
        "ui": observations["ui"],
        "device": observations["device"],
    }
    checker_module().validate_report_value(report, "collected report")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--source-root", required=True, action=StoreOnce)
    parser.add_argument("--build-dir", required=True, action=StoreOnce)
    parser.add_argument("--report", required=True, action=StoreOnce)
    parser.add_argument("--expected-commit", required=True, action=StoreOnce)
    parser.add_argument("--jobs", type=int, action=StoreOnce)
    parser.add_argument("--package", action=StoreOnce)
    parser.add_argument("--probe-report", action=StoreOnce)
    parser.add_argument("--probe-manifest", action=StoreOnce)
    parser.add_argument("--probe-executable", action=StoreOnce)
    parser.add_argument("--juce-source", required=True, action=StoreOnce)
    parser.add_argument(
        "--package-kind", action=StoreOnce,
        choices=("unsigned-local", "signed-local", "notarized-ingested"))
    arguments = parser.parse_args()
    try:
        if not HEX_COMMIT.fullmatch(arguments.expected_commit):
            raise CollectorError("--expected-commit must be a full lowercase Git SHA")
        jobs = arguments.jobs if arguments.jobs is not None else 8
        if jobs < 1 or jobs > 64:
            raise CollectorError("--jobs must be in [1, 64]")
        if arguments.package is None and arguments.package_kind is not None:
            raise CollectorError("--package-kind requires --package")
        probe_arguments = (
            arguments.probe_report,
            arguments.probe_manifest,
            arguments.probe_executable,
        )
        if any(value is not None for value in probe_arguments) \
           and not all(value is not None for value in probe_arguments):
            raise CollectorError(
                "--probe-report, --probe-manifest, and --probe-executable "
                "must be used together")
        source_root = Path(arguments.source_root).resolve(strict=True)
        report_path = Path(arguments.report).resolve()
        build_root = Path(arguments.build_dir).resolve()
        validate_report_destination(report_path, source_root, build_root)
        juce_root = verify_juce_source(Path(arguments.juce_source))
        report = collect_report(
            source_root, build_root, arguments.expected_commit, jobs,
            Path(arguments.package) if arguments.package is not None else None,
            arguments.package_kind,
            Path(arguments.probe_report) if arguments.probe_report is not None
            else None,
            Path(arguments.probe_manifest)
            if arguments.probe_manifest is not None else None,
            Path(arguments.probe_executable)
            if arguments.probe_executable is not None else None,
            juce_root)
        write_atomic(report_path, report)
        print(f"Product performance report: {report_path}")
        return 0
    except (CollectorError, OSError, UnicodeError, ValueError) as error:
        print(f"Product performance collection: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
