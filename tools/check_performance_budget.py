#!/usr/bin/env python3
"""Validate Talas core performance reports against a closed local budget."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Any


REPORT_TOP_KEYS = {
    "format", "version", "runId", "environment", "campaign", "scenarios",
}
ENVIRONMENT_KEYS = {
    "platform", "architecture", "compilerId", "compilerVersion",
    "buildType", "commit", "preset", "buildFingerprint",
}
BUDGET_ENVIRONMENT_KEYS = ENVIRONMENT_KEYS - {"commit"}
CAMPAIGN_KEYS = {
    "warmups", "samples", "callbackSamples", "sampleRate", "blockFrames",
    "callbackDeadlineNs", "seed",
}
SCENARIO_KEYS = {"id", "nodeCount", "connectionCount", "metrics"}
DISTRIBUTION_KEYS = {"p50_ns", "p95_ns", "p99_ns", "max_ns"}
METRIC_KEYS = {
    "compile", "prepare", "callback", "graphAdoption",
    "callbackDeadlineMissCount", "planStepCount", "planBufferSlots",
    "estimatedPreparedBytes", "checksum",
}
BUDGET_TOP_KEYS = {
    "format", "version", "environment", "campaign", "minimumReports",
    "scenarios",
}
TIMING_RULE_KEYS = {
    "mode", "baseline", "absoluteMax", "relativeMax", "maxSpread",
}
EXACT_RULE_KEYS = {"mode", "value"}
MAXIMUM_RULE_KEYS = {"mode", "maximum"}
HEX_COMMIT = re.compile(r"^[0-9a-f]{40}$")
HEX_RUN_ID = re.compile(r"^[0-9a-f]{32}$")
HEX_FINGERPRINT = re.compile(r"^[0-9a-f]{64}$")
EXACT_METRICS = {
    "nodeCount", "connectionCount", "planStepCount", "planBufferSlots",
    "estimatedPreparedBytes",
}
MAXIMUM_METRICS = {"callbackDeadlineMissCount"}
TIMING_METRIC = re.compile(
    r"^(?:compile|prepare|callback|graphAdoption)\.(?:p50_ns|p95_ns|p99_ns)$")
MAXIMUM_TIMING_METRIC = re.compile(
    r"^(?:compile|prepare|callback|graphAdoption)\.max_ns$")


class BudgetError(RuntimeError):
    pass


class StoreOnce(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"duplicate {option_string} option")
        setattr(namespace, self.dest, values)


def _closed_object(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BudgetError(f"{label} must be an object")
    actual = set(value)
    if actual != keys:
        missing = sorted(keys - actual)
        unknown = sorted(actual - keys)
        raise BudgetError(
            f"{label} fields mismatch: missing={missing} unknown={unknown}")
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


def _finite_number(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BudgetError(f"{label} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise BudgetError(f"{label} must be finite")
    return result


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise BudgetError(f"{label} must be a non-empty string")
    return value


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


def validate_report(path: Path) -> dict[str, Any]:
    report = _closed_object(_load(path), REPORT_TOP_KEYS, f"report {path}")
    if (report["format"] != "talas.performance-report"
        or isinstance(report["version"], bool)
        or not isinstance(report["version"], int)
        or report["version"] != 1):
        raise BudgetError(f"report {path} has unsupported format/version")
    _string(report["runId"], f"report {path} runId")
    if not HEX_RUN_ID.fullmatch(report["runId"]):
        raise BudgetError(f"report {path} runId must be 32 lowercase hex digits")
    environment = _closed_object(
        report["environment"], ENVIRONMENT_KEYS, f"report {path} environment")
    for key, value in environment.items():
        _string(value, f"report {path} environment.{key}")
    if not HEX_COMMIT.fullmatch(environment["commit"]):
        raise BudgetError(f"report {path} commit must be a full Git SHA")
    if not HEX_FINGERPRINT.fullmatch(environment["buildFingerprint"]):
        raise BudgetError(f"report {path} buildFingerprint must be 64 lowercase hex digits")
    campaign = _closed_object(
        report["campaign"], CAMPAIGN_KEYS, f"report {path} campaign")
    for key in CAMPAIGN_KEYS:
        minimum = 0 if key == "warmups" else 1
        _integer(campaign[key], f"report {path} campaign.{key}", minimum)
    scenarios = report["scenarios"]
    if not isinstance(scenarios, list) or not scenarios:
        raise BudgetError(f"report {path} scenarios must be a non-empty array")
    seen: set[str] = set()
    for index, raw in enumerate(scenarios):
        label = f"report {path} scenarios[{index}]"
        scenario = _closed_object(raw, SCENARIO_KEYS, label)
        scenario_id = _string(scenario["id"], f"{label}.id")
        if scenario_id in seen:
            raise BudgetError(f"report {path} duplicate scenario: {scenario_id}")
        seen.add(scenario_id)
        _integer(scenario["nodeCount"], f"{label}.nodeCount", 1)
        _integer(scenario["connectionCount"], f"{label}.connectionCount", 0)
        metrics = _closed_object(scenario["metrics"], METRIC_KEYS, f"{label}.metrics")
        for distribution_name in ("compile", "prepare", "callback", "graphAdoption"):
            distribution = _closed_object(
                metrics[distribution_name], DISTRIBUTION_KEYS,
                f"{label}.metrics.{distribution_name}")
            previous = 0
            for percentile in ("p50_ns", "p95_ns", "p99_ns", "max_ns"):
                current = _integer(
                    distribution[percentile],
                    f"{label}.metrics.{distribution_name}.{percentile}", 1)
                if current < previous:
                    raise BudgetError(
                        f"{label}.metrics.{distribution_name} is not monotonic")
                previous = current
        for metric in (
            "callbackDeadlineMissCount", "planStepCount", "planBufferSlots",
            "estimatedPreparedBytes",
        ):
            minimum = 0 if metric == "callbackDeadlineMissCount" else 1
            _integer(metrics[metric], f"{label}.metrics.{metric}", minimum)
        _finite_number(metrics["checksum"], f"{label}.metrics.checksum")
    return report


def validate_budget(path: Path) -> dict[str, Any]:
    budget = _closed_object(_load(path), BUDGET_TOP_KEYS, f"budget {path}")
    if (budget["format"] != "talas.performance-budget"
        or isinstance(budget["version"], bool)
        or not isinstance(budget["version"], int)
        or budget["version"] != 1):
        raise BudgetError(f"budget {path} has unsupported format/version")
    environment = _closed_object(
        budget["environment"], BUDGET_ENVIRONMENT_KEYS,
        f"budget {path} environment")
    for key, value in environment.items():
        _string(value, f"budget {path} environment.{key}")
    if not HEX_FINGERPRINT.fullmatch(environment["buildFingerprint"]):
        raise BudgetError(
            f"budget {path} buildFingerprint must be 64 lowercase hex digits")
    campaign = _closed_object(
        budget["campaign"], CAMPAIGN_KEYS, f"budget {path} campaign")
    for key in CAMPAIGN_KEYS:
        minimum = 0 if key == "warmups" else 1
        _integer(campaign[key], f"budget {path} campaign.{key}", minimum)
    _integer(budget["minimumReports"], f"budget {path} minimumReports", 2)
    scenarios = budget["scenarios"]
    if not isinstance(scenarios, dict) or not scenarios:
        raise BudgetError(f"budget {path} scenarios must be a non-empty object")
    for scenario_id, rules in scenarios.items():
        _string(scenario_id, f"budget {path} scenario id")
        if not isinstance(rules, dict) or not rules:
            raise BudgetError(f"budget {path} scenario {scenario_id} must have rules")
        for metric, raw_rule in rules.items():
            _string(metric, f"budget {path} scenario {scenario_id} metric")
            if not isinstance(raw_rule, dict) or "mode" not in raw_rule:
                raise BudgetError(f"budget rule {scenario_id}.{metric} is invalid")
            mode = raw_rule["mode"]
            if metric in EXACT_METRICS:
                expected_mode = "exact"
            elif metric in MAXIMUM_METRICS or MAXIMUM_TIMING_METRIC.fullmatch(metric):
                expected_mode = "maximum"
            elif TIMING_METRIC.fullmatch(metric):
                expected_mode = "timing"
            else:
                raise BudgetError(
                    f"budget rule {scenario_id}.{metric} names an unknown metric")
            if mode != expected_mode:
                raise BudgetError(
                    f"budget rule {scenario_id}.{metric} must use "
                    f"{expected_mode} mode, got {mode}")
            if mode == "timing":
                rule = _closed_object(
                    raw_rule, TIMING_RULE_KEYS, f"budget rule {scenario_id}.{metric}")
                _integer(rule["baseline"], f"{scenario_id}.{metric}.baseline", 1)
                _integer(rule["absoluteMax"], f"{scenario_id}.{metric}.absoluteMax", 1)
                _number(rule["relativeMax"], f"{scenario_id}.{metric}.relativeMax")
                _number(rule["maxSpread"], f"{scenario_id}.{metric}.maxSpread")
            elif mode == "exact":
                rule = _closed_object(
                    raw_rule, EXACT_RULE_KEYS, f"budget rule {scenario_id}.{metric}")
                _integer(rule["value"], f"{scenario_id}.{metric}.value")
            elif mode == "maximum":
                rule = _closed_object(
                    raw_rule, MAXIMUM_RULE_KEYS, f"budget rule {scenario_id}.{metric}")
                _integer(rule["maximum"], f"{scenario_id}.{metric}.maximum")
            else:
                raise BudgetError(f"budget rule {scenario_id}.{metric} has unknown mode")
    return budget


def flatten_scenario(scenario: dict[str, Any]) -> dict[str, float]:
    metrics = scenario["metrics"]
    result: dict[str, float] = {
        "nodeCount": scenario["nodeCount"],
        "connectionCount": scenario["connectionCount"],
        "callbackDeadlineMissCount": metrics["callbackDeadlineMissCount"],
        "planStepCount": metrics["planStepCount"],
        "planBufferSlots": metrics["planBufferSlots"],
        "estimatedPreparedBytes": metrics["estimatedPreparedBytes"],
    }
    for name in ("compile", "prepare", "callback", "graphAdoption"):
        for percentile, value in metrics[name].items():
            result[f"{name}.{percentile}"] = value
    return result


def check_budget(
    reports: list[dict[str, Any]],
    budget: dict[str, Any],
    expected_commit: str,
    explain: bool,
) -> None:
    if len(reports) < budget["minimumReports"]:
        raise BudgetError(
            f"need at least {budget['minimumReports']} reports, got {len(reports)}")
    run_ids = [report["runId"] for report in reports]
    if len(set(run_ids)) != len(run_ids):
        raise BudgetError("reports must have unique runId values")
    first_environment = reports[0]["environment"]
    if first_environment["commit"] != expected_commit:
        raise BudgetError(
            "report commit does not match --expected-commit: "
            f"{first_environment['commit']} != {expected_commit}")
    first_campaign = reports[0]["campaign"]
    first_ids = [scenario["id"] for scenario in reports[0]["scenarios"]]
    if set(first_ids) != set(budget["scenarios"]):
        raise BudgetError("report and budget scenario sets differ")
    for key, expected in budget["environment"].items():
        if first_environment[key] != expected:
            raise BudgetError(
                f"environment mismatch for {key}: "
                f"{first_environment[key]!r} != {expected!r}")
    if first_campaign != budget["campaign"]:
        raise BudgetError("campaign does not match the accepted budget")
    for index, report in enumerate(reports[1:], 1):
        if report["environment"] != first_environment:
            raise BudgetError(f"report {index} environment differs from report 0")
        if report["campaign"] != first_campaign:
            raise BudgetError(f"report {index} campaign differs from report 0")
        if [scenario["id"] for scenario in report["scenarios"]] != first_ids:
            raise BudgetError(f"report {index} scenario order/set differs from report 0")

    flattened_reports = [
        {scenario["id"]: flatten_scenario(scenario) for scenario in report["scenarios"]}
        for report in reports
    ]
    for scenario_id, rules in budget["scenarios"].items():
        available = set(flattened_reports[0][scenario_id])
        if set(rules) != available:
            raise BudgetError(
                f"budget metric set differs for {scenario_id}: "
                f"missing={sorted(available - set(rules))} "
                f"unknown={sorted(set(rules) - available)}")
        for metric, rule in rules.items():
            values = [report[scenario_id][metric] for report in flattened_reports]
            representative = float(statistics.median(values))
            if rule["mode"] == "timing":
                spread = (max(values) - min(values)) / max(representative, 1.0)
                if spread > rule["maxSpread"]:
                    raise BudgetError(
                        f"unstable {scenario_id}.{metric}: spread={spread:.4f} "
                        f"> {rule['maxSpread']:.4f}")
                relative_limit = rule["baseline"] * (1.0 + rule["relativeMax"])
                if representative > rule["absoluteMax"]:
                    raise BudgetError(
                        f"absolute regression {scenario_id}.{metric}: "
                        f"observed={representative:.0f} max={rule['absoluteMax']}")
                if representative > relative_limit:
                    raise BudgetError(
                        f"relative regression {scenario_id}.{metric}: "
                        f"observed={representative:.0f} baseline={rule['baseline']} "
                        f"limit={relative_limit:.0f}")
                if explain:
                    print(
                        f"PASS {scenario_id}.{metric} observed={representative:.0f} "
                        f"spread={spread:.4f}")
            elif rule["mode"] == "exact":
                if any(value != rule["value"] for value in values):
                    raise BudgetError(
                        f"structural regression {scenario_id}.{metric}: "
                        f"observed={values} exact={rule['value']}")
            else:
                if any(value > rule["maximum"] for value in values):
                    raise BudgetError(
                        f"maximum regression {scenario_id}.{metric}: "
                        f"observed={values} max={rule['maximum']}")


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
        check_budget(
            reports, budget, arguments.expected_commit, arguments.explain)
    except BudgetError as error:
        print(f"Performance budget: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"Performance budget: PASS ({len(reports)} reports, "
        f"{len(budget['scenarios'])} scenarios)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
