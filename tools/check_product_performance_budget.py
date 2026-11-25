#!/usr/bin/env python3
"""Validate product performance reports against artifact-only V1 budgets."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


REPORT_KEYS = {
    "format", "version", "environment", "build", "artifacts", "package",
    "ui", "device",
}
ENVIRONMENT_KEYS = {
    "runId", "commit", "platform", "architecture", "compilerId",
    "compilerVersion", "buildType", "preset", "generator",
    "configurationFingerprint", "buildEnvironmentFingerprint",
    "collectorVersion",
}
BUDGET_ENVIRONMENT_KEYS = ENVIRONMENT_KEYS - {
    "runId", "commit",
}
UNAVAILABLE_KEYS = {"availability", "authority", "reason"}
PROBE_PROVENANCE_KEYS = {
    "probeRunId", "probeBuildFingerprint", "probeExecutableSha256",
}
BUILD_KEYS = {
    "availability", "authority", "command", "startedUtc", "finishedUtc",
    "elapsedNs",
}
ARTIFACTS_KEYS = {"availability", "authority", "bundle", "executables"}
ARTIFACT_KEYS = {"id", "path", "bytes", "sha256"}
PACKAGE_KEYS = {"availability", "authority", "kind", "artifact"}
UI_KEYS = {
    "availability", "authority", "fixture", "width", "height", "scale",
    "warmups", "samples", "nodeCount", "connectionCount", "p50Ns",
    "p95Ns", "p99Ns", "maxNs", "rebuildSamples", "rebuildP50Ns",
    "rebuildP95Ns", "rebuildMaxNs", "pixelDigest",
    *PROBE_PROVENANCE_KEYS,
}
DEVICE_KEYS = {
    "availability", "authority", "name", "type", "sampleRate",
    "blockFrames", "inputChannels", "outputChannels", "durationMs",
    "callbacks", "callbackPeriodNs", "maximumCallbackNs",
    "deadlineOverruns", *PROBE_PROVENANCE_KEYS,
}
BUDGET_KEYS = {
    "format", "version", "environment", "minimumReports", "authorities",
    "artifacts",
}
AUTHORITIES_KEYS = {"build", "artifacts", "package", "ui", "device"}
ARTIFACT_RULE_KEYS = {
    "id", "baselineBytes", "absoluteMaxBytes", "relativeMax",
}
HEX_COMMIT = re.compile(r"^[0-9a-f]{40}$")
HEX_RUN_ID = re.compile(r"^[0-9a-f]{32}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
UTC_TIMESTAMP = re.compile(
    r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")
ARTIFACT_ID = re.compile(r"^[a-z][a-z0-9-]*$")
PACKAGE_KINDS = {"unsigned-local", "signed-local", "notarized-ingested"}


class BudgetError(RuntimeError):
    pass


class StoreOnce(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"duplicate {option_string} option")
        setattr(namespace, self.dest, values)


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BudgetError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _load(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source, object_pairs_hook=_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BudgetError(f"cannot read {path}: {error}") from error


def _closed(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BudgetError(f"{label} must be an object")
    actual = set(value)
    if actual != keys:
        raise BudgetError(
            f"{label} fields mismatch: missing={sorted(keys - actual)} "
            f"unknown={sorted(actual - keys)}")
    return value


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise BudgetError(f"{label} must be a non-empty string")
    return value


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise BudgetError(f"{label} must be an integer >= {minimum}")
    return value


def _number(value: Any, label: str, minimum: float = 0.0) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BudgetError(f"{label} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < minimum:
        raise BudgetError(f"{label} must be finite and >= {minimum}")
    return result


def _authority(value: Any, expected: str, label: str) -> None:
    if value != expected:
        raise BudgetError(f"{label} must be {expected!r}")


def _unavailable(value: Any, authority: str, label: str) -> bool:
    if not isinstance(value, dict) or value.get("availability") != "unavailable":
        return False
    section = _closed(value, UNAVAILABLE_KEYS, label)
    _authority(section["authority"], authority, f"{label}.authority")
    _string(section["reason"], f"{label}.reason")
    return True


def _probe_provenance(value: dict[str, Any], label: str) -> None:
    if (not isinstance(value.get("probeRunId"), str)
        or not HEX_RUN_ID.fullmatch(value["probeRunId"])):
        raise BudgetError(f"{label}.probeRunId must be 32 lowercase hex digits")
    for key in ("probeBuildFingerprint", "probeExecutableSha256"):
        raw = value.get(key)
        if not isinstance(raw, str) or not HEX_SHA256.fullmatch(raw):
            raise BudgetError(f"{label}.{key} must be 64 lowercase hex digits")


def _observation_unavailable(value: Any, label: str) -> bool:
    if not isinstance(value, dict) or value.get("availability") != "unavailable":
        return False
    keys = set(value)
    if keys not in (UNAVAILABLE_KEYS, UNAVAILABLE_KEYS | PROBE_PROVENANCE_KEYS):
        _closed(value, UNAVAILABLE_KEYS, label)
    _authority(value["authority"], "observational", f"{label}.authority")
    _string(value["reason"], f"{label}.reason")
    if keys == UNAVAILABLE_KEYS | PROBE_PROVENANCE_KEYS:
        _probe_provenance(value, label)
    return True


def _artifact(value: Any, label: str) -> dict[str, Any]:
    artifact = _closed(value, ARTIFACT_KEYS, label)
    artifact_id = _string(artifact["id"], f"{label}.id")
    if not ARTIFACT_ID.fullmatch(artifact_id):
        raise BudgetError(f"{label}.id is invalid")
    path = _string(artifact["path"], f"{label}.path")
    if path.startswith("/") or ".." in Path(path).parts:
        raise BudgetError(f"{label}.path must be a contained relative path")
    _integer(artifact["bytes"], f"{label}.bytes", 1)
    if not isinstance(artifact["sha256"], str) \
       or not HEX_SHA256.fullmatch(artifact["sha256"]):
        raise BudgetError(f"{label}.sha256 must be 64 lowercase hex digits")
    return artifact


def validate_report_value(value: Any, label: str = "report") -> dict[str, Any]:
    report = _closed(value, REPORT_KEYS, label)
    if (report["format"] != "talas.product-performance-report"
        or type(report["version"]) is not int
        or report["version"] != 1):
        raise BudgetError(f"{label} has unsupported format/version")

    environment = _closed(
        report["environment"], ENVIRONMENT_KEYS, f"{label}.environment")
    for key, raw in environment.items():
        _string(raw, f"{label}.environment.{key}")
    if not HEX_RUN_ID.fullmatch(environment["runId"]):
        raise BudgetError(f"{label}.environment.runId is invalid")
    if not HEX_COMMIT.fullmatch(environment["commit"]):
        raise BudgetError(f"{label}.environment.commit is invalid")
    if not HEX_SHA256.fullmatch(environment["configurationFingerprint"]):
        raise BudgetError(
            f"{label}.environment.configurationFingerprint is invalid")
    if not HEX_SHA256.fullmatch(environment["buildEnvironmentFingerprint"]):
        raise BudgetError(
            f"{label}.environment.buildEnvironmentFingerprint is invalid")

    build_label = f"{label}.build"
    if not _unavailable(report["build"], "observational", build_label):
        build = _closed(report["build"], BUILD_KEYS, build_label)
        if build["availability"] != "available":
            raise BudgetError(f"{build_label}.availability is invalid")
        _authority(build["authority"], "observational", f"{build_label}.authority")
        _string(build["command"], f"{build_label}.command")
        for key in ("startedUtc", "finishedUtc"):
            if not isinstance(build[key], str) or not UTC_TIMESTAMP.fullmatch(build[key]):
                raise BudgetError(f"{build_label}.{key} must be UTC seconds")
        _integer(build["elapsedNs"], f"{build_label}.elapsedNs", 1)

    artifacts_label = f"{label}.artifacts"
    if not _unavailable(report["artifacts"], "budgeted", artifacts_label):
        artifacts = _closed(report["artifacts"], ARTIFACTS_KEYS, artifacts_label)
        if artifacts["availability"] != "available":
            raise BudgetError(f"{artifacts_label}.availability is invalid")
        _authority(
            artifacts["authority"], "budgeted", f"{artifacts_label}.authority")
        bundle = _artifact(artifacts["bundle"], f"{artifacts_label}.bundle")
        executables = artifacts["executables"]
        if not isinstance(executables, list) or not executables:
            raise BudgetError(f"{artifacts_label}.executables must be non-empty")
        records = [bundle]
        records.extend(
            _artifact(raw, f"{artifacts_label}.executables[{index}]")
            for index, raw in enumerate(executables))
        ids = [record["id"] for record in records]
        paths = [record["path"] for record in records]
        if len(set(ids)) != len(ids):
            raise BudgetError(f"{artifacts_label} has duplicate artifact ids")
        if len(set(paths)) != len(paths):
            raise BudgetError(f"{artifacts_label} has duplicate artifact paths")

    package_label = f"{label}.package"
    if not _unavailable(report["package"], "observational", package_label):
        package = _closed(report["package"], PACKAGE_KEYS, package_label)
        if package["availability"] != "available":
            raise BudgetError(f"{package_label}.availability is invalid")
        _authority(
            package["authority"], "observational", f"{package_label}.authority")
        if package["kind"] not in PACKAGE_KINDS:
            raise BudgetError(f"{package_label}.kind is invalid")
        _artifact(package["artifact"], f"{package_label}.artifact")

    ui_label = f"{label}.ui"
    if not _observation_unavailable(report["ui"], ui_label):
        ui = _closed(report["ui"], UI_KEYS, ui_label)
        if ui["availability"] != "available":
            raise BudgetError(f"{ui_label}.availability is invalid")
        _authority(ui["authority"], "observational", f"{ui_label}.authority")
        _probe_provenance(ui, ui_label)
        _string(ui["fixture"], f"{ui_label}.fixture")
        for key in (
            "width", "height", "warmups", "samples", "nodeCount",
            "connectionCount", "p50Ns", "p95Ns", "p99Ns", "maxNs",
            "rebuildSamples", "rebuildP50Ns", "rebuildP95Ns",
            "rebuildMaxNs",
        ):
            minimum = 0 if key == "connectionCount" else 1
            _integer(ui[key], f"{ui_label}.{key}", minimum)
        _number(ui["scale"], f"{ui_label}.scale", 0.01)
        if (not isinstance(ui["pixelDigest"], str)
            or not HEX_SHA256.fullmatch(ui["pixelDigest"])):
            raise BudgetError(f"{ui_label}.pixelDigest is invalid")
        if not (ui["p50Ns"] <= ui["p95Ns"] <= ui["p99Ns"] <= ui["maxNs"]):
            raise BudgetError(f"{ui_label} percentiles are not monotonic")
        if not (ui["rebuildP50Ns"] <= ui["rebuildP95Ns"]
                <= ui["rebuildMaxNs"]):
            raise BudgetError(
                f"{ui_label} rebuild percentiles are not monotonic")

    device_label = f"{label}.device"
    if not _observation_unavailable(report["device"], device_label):
        device = _closed(report["device"], DEVICE_KEYS, device_label)
        if device["availability"] != "available":
            raise BudgetError(f"{device_label}.availability is invalid")
        _authority(
            device["authority"], "observational", f"{device_label}.authority")
        _probe_provenance(device, device_label)
        _string(device["name"], f"{device_label}.name")
        _string(device["type"], f"{device_label}.type")
        for key in (
            "sampleRate", "blockFrames", "outputChannels", "durationMs",
            "callbacks", "callbackPeriodNs", "maximumCallbackNs",
        ):
            _integer(device[key], f"{device_label}.{key}", 1)
        _integer(device["inputChannels"], f"{device_label}.inputChannels")
        _integer(device["deadlineOverruns"], f"{device_label}.deadlineOverruns")
        if device["deadlineOverruns"] > device["callbacks"]:
            raise BudgetError(
                f"{device_label}.deadlineOverruns exceeds callbacks")
        observed_overrun = device["maximumCallbackNs"] \
            > device["callbackPeriodNs"]
        if observed_overrun != (device["deadlineOverruns"] > 0):
            raise BudgetError(
                f"{device_label} maximum/overrun evidence is inconsistent")
        expected_period = (
            device["blockFrames"] * 1_000_000_000
            + device["sampleRate"] // 2
        ) // device["sampleRate"]
        if device["callbackPeriodNs"] != expected_period:
            raise BudgetError(
                f"{device_label}.callbackPeriodNs does not match audio tuple")
    ui_provenance = {
        key: report["ui"].get(key) for key in PROBE_PROVENANCE_KEYS}
    device_provenance = {
        key: report["device"].get(key) for key in PROBE_PROVENANCE_KEYS}
    if ui_provenance != device_provenance:
        raise BudgetError(f"{label} UI/device probe provenance differs")
    return report


def validate_report(path: Path) -> dict[str, Any]:
    return validate_report_value(_load(path), f"report {path}")


def validate_budget_value(value: Any, label: str = "budget") -> dict[str, Any]:
    budget = _closed(value, BUDGET_KEYS, label)
    if (budget["format"] != "talas.product-performance-budget"
        or type(budget["version"]) is not int
        or budget["version"] != 1):
        raise BudgetError(f"{label} has unsupported format/version")
    environment = _closed(
        budget["environment"], BUDGET_ENVIRONMENT_KEYS,
        f"{label}.environment")
    for key, raw in environment.items():
        _string(raw, f"{label}.environment.{key}")
    if not HEX_SHA256.fullmatch(environment["configurationFingerprint"]):
        raise BudgetError(f"{label}.environment.configurationFingerprint is invalid")
    if not HEX_SHA256.fullmatch(environment["buildEnvironmentFingerprint"]):
        raise BudgetError(
            f"{label}.environment.buildEnvironmentFingerprint is invalid")
    _integer(budget["minimumReports"], f"{label}.minimumReports", 3)
    authorities = _closed(
        budget["authorities"], AUTHORITIES_KEYS, f"{label}.authorities")
    expected_authorities = {
        "build": "observational", "artifacts": "budgeted",
        "package": "observational", "ui": "observational",
        "device": "observational",
    }
    if authorities != expected_authorities:
        raise BudgetError(
            f"{label}.authorities must keep V1 timing/package observational")
    rules = budget["artifacts"]
    if not isinstance(rules, list) or not rules:
        raise BudgetError(f"{label}.artifacts must be non-empty")
    ids: list[str] = []
    for index, raw in enumerate(rules):
        rule_label = f"{label}.artifacts[{index}]"
        rule = _closed(raw, ARTIFACT_RULE_KEYS, rule_label)
        artifact_id = _string(rule["id"], f"{rule_label}.id")
        if not ARTIFACT_ID.fullmatch(artifact_id):
            raise BudgetError(f"{rule_label}.id is invalid")
        ids.append(artifact_id)
        baseline = _integer(rule["baselineBytes"], f"{rule_label}.baselineBytes", 1)
        absolute = _integer(
            rule["absoluteMaxBytes"], f"{rule_label}.absoluteMaxBytes", 1)
        if absolute < baseline:
            raise BudgetError(f"{rule_label} absolute maximum is below baseline")
        _number(rule["relativeMax"], f"{rule_label}.relativeMax")
    if len(set(ids)) != len(ids):
        raise BudgetError(f"{label} has duplicate artifact rules")
    return budget


def validate_budget(path: Path) -> dict[str, Any]:
    return validate_budget_value(_load(path), f"budget {path}")


def _artifact_map(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    artifacts = report["artifacts"]
    if artifacts["availability"] != "available":
        raise BudgetError("budgeted artifacts are unavailable")
    records = [artifacts["bundle"], *artifacts["executables"]]
    return {record["id"]: record for record in records}


def check_budget(
    reports: list[dict[str, Any]],
    budget: dict[str, Any],
    expected_commit: str,
    explain: bool,
) -> None:
    if len(reports) < budget["minimumReports"]:
        raise BudgetError(
            f"need at least {budget['minimumReports']} reports, got {len(reports)}")
    run_ids = [report["environment"]["runId"] for report in reports]
    if len(set(run_ids)) != len(run_ids):
        raise BudgetError("reports must have unique runId values")
    first_environment = reports[0]["environment"]
    if first_environment["commit"] != expected_commit:
        raise BudgetError("report commit does not match --expected-commit")
    for key, expected in budget["environment"].items():
        if first_environment[key] != expected:
            raise BudgetError(
                f"environment mismatch for {key}: "
                f"{first_environment[key]!r} != {expected!r}")
    stable_environment = {
        key: value for key, value in first_environment.items() if key != "runId"}
    artifact_maps: list[dict[str, dict[str, Any]]] = []
    probe_run_ids: set[str] = set()
    for index, report in enumerate(reports):
        environment = {
            key: value for key, value in report["environment"].items()
            if key != "runId"}
        if environment != stable_environment:
            raise BudgetError(f"report {index} environment differs")
        probe_run_id = report["ui"].get("probeRunId")
        if probe_run_id is not None:
            if probe_run_id in probe_run_ids:
                raise BudgetError("product probe reports require unique probeRunId")
            probe_run_ids.add(probe_run_id)
        artifact_maps.append(_artifact_map(report))
    rule_ids = {rule["id"] for rule in budget["artifacts"]}
    if set(artifact_maps[0]) != rule_ids:
        raise BudgetError("report and budget artifact sets differ")
    for index, artifacts in enumerate(artifact_maps[1:], 1):
        if artifacts != artifact_maps[0]:
            raise BudgetError(f"report {index} artifact identities differ")
    for rule in budget["artifacts"]:
        artifact = artifact_maps[0][rule["id"]]
        observed = artifact["bytes"]
        relative_limit = rule["baselineBytes"] * (1.0 + rule["relativeMax"])
        if observed > rule["absoluteMaxBytes"]:
            raise BudgetError(
                f"absolute artifact regression {rule['id']}: "
                f"observed={observed} max={rule['absoluteMaxBytes']}")
        if observed > relative_limit:
            raise BudgetError(
                f"relative artifact regression {rule['id']}: "
                f"observed={observed} baseline={rule['baselineBytes']} "
                f"limit={relative_limit:.0f}")
        if explain:
            print(
                f"PASS {rule['id']} bytes={observed} "
                f"sha256={artifact['sha256']}")


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--report", action="append", required=True)
    parser.add_argument("--budget", required=True, action=StoreOnce)
    parser.add_argument("--expected-commit", required=True, action=StoreOnce)
    parser.add_argument("--explain", action="store_true")
    arguments = parser.parse_args()
    try:
        if not HEX_COMMIT.fullmatch(arguments.expected_commit):
            raise BudgetError("--expected-commit must be a full lowercase Git SHA")
        reports = [validate_report(Path(path)) for path in arguments.report]
        budget = validate_budget(Path(arguments.budget))
        check_budget(reports, budget, arguments.expected_commit, arguments.explain)
        print(
            f"Product performance budget: PASS "
            f"({len(reports)} reports, {len(budget['artifacts'])} artifacts)")
        return 0
    except BudgetError as error:
        print(f"Product performance budget: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
