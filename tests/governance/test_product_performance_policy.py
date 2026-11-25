import copy
import hashlib
import importlib.util
import itertools
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_product_performance_budget",
    ROOT / "tools" / "check_product_performance_budget.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

COLLECTOR_SPEC = importlib.util.spec_from_file_location(
    "collect_product_performance",
    ROOT / "tools" / "collect_product_performance.py")
COLLECTOR = importlib.util.module_from_spec(COLLECTOR_SPEC)
assert COLLECTOR_SPEC.loader is not None
COLLECTOR_SPEC.loader.exec_module(COLLECTOR)

RUN_IDS = itertools.count(1)


def artifact(artifact_id, path, size):
    return {
        "id": artifact_id,
        "path": path,
        "bytes": size,
        "sha256": f"{size:064x}"[-64:],
    }


def report():
    return {
        "format": "talas.product-performance-report",
        "version": 1,
        "environment": {
            "runId": f"{next(RUN_IDS):032x}",
            "commit": "a" * 40,
            "platform": "macos",
            "architecture": "arm64",
            "compilerId": "AppleClang",
            "compilerVersion": "21.0.0.21000101",
            "buildType": "Release",
            "preset": "workbench-release",
            "generator": "Unix Makefiles",
            "configurationFingerprint": "f" * 64,
            "buildEnvironmentFingerprint": "e" * 64,
            "collectorVersion": "1",
        },
        "build": {
            "availability": "available",
            "authority": "observational",
            "command": "cmake --preset workbench-release",
            "startedUtc": "2026-08-10T12:00:00Z",
            "finishedUtc": "2026-08-10T12:01:00Z",
            "elapsedNs": 60_000_000_000,
        },
        "artifacts": {
            "availability": "available",
            "authority": "budgeted",
            "bundle": artifact("app-bundle", ".", 1_000),
            "executables": [
                artifact("workbench-main", "Contents/MacOS/Workbench", 400),
                artifact("xpc-host", "Contents/XPCServices/Host", 100),
                artifact("xpc-probe", "Contents/XPCServices/Probe", 100),
                artifact("xpc-runtime", "Contents/XPCServices/Runtime", 200),
            ],
        },
        "package": {
            "availability": "unavailable",
            "authority": "observational",
            "reason": "not-collected",
        },
        "ui": {
            "availability": "unavailable",
            "authority": "observational",
            "reason": "not-collected",
        },
        "device": {
            "availability": "unavailable",
            "authority": "observational",
            "reason": "not-requested",
        },
    }


def budget():
    return {
        "format": "talas.product-performance-budget",
        "version": 1,
        "environment": {
            key: value for key, value in report()["environment"].items()
            if key not in {"runId", "commit"}
        },
        "minimumReports": 3,
        "authorities": {
            "build": "observational",
            "artifacts": "budgeted",
            "package": "observational",
            "ui": "observational",
            "device": "observational",
        },
        "artifacts": [
            {
                "id": artifact_id,
                "baselineBytes": size,
                "absoluteMaxBytes": size * 2,
                "relativeMax": 0.25,
            }
            for artifact_id, size in (
                ("app-bundle", 1_000), ("workbench-main", 400),
                ("xpc-host", 100), ("xpc-probe", 100),
                ("xpc-runtime", 200),
            )
        ],
    }


class ProductPerformancePolicyTests(unittest.TestCase):
    @staticmethod
    def make_bundle(root):
        bundle = root / "Talas Workbench.app"
        paths = [
            bundle / "Contents" / "MacOS" / "Talas Workbench",
            bundle / "Contents" / "XPCServices" / "TalasComponentHost.xpc"
            / "Contents" / "MacOS" / "TalasComponentHost",
            bundle / "Contents" / "XPCServices" / "TalasComponentProbe.xpc"
            / "Contents" / "MacOS" / "TalasComponentProbe",
            bundle / "Contents" / "XPCServices" / "TalasComponentRuntime.xpc"
            / "Contents" / "MacOS" / "TalasComponentRuntime",
        ]
        for index, path in enumerate(paths, 1):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(bytes([index]) * index)
        return bundle

    def test_accepts_three_identical_artifact_reports(self):
        reports = [report() for _ in range(3)]
        MODULE.check_budget(reports, budget(), "a" * 40, False)

    def test_checked_in_product_budget_is_closed_and_artifact_only(self):
        value = MODULE.validate_budget(
            ROOT / "benchmarks" / "baselines"
            / "macos-arm64-workbench-release.json")
        self.assertEqual(value["minimumReports"], 3)
        self.assertEqual(
            {item["id"] for item in value["artifacts"]},
            {"app-bundle", "workbench-main", "xpc-host", "xpc-probe",
             "xpc-runtime"})
        self.assertEqual(value["authorities"]["artifacts"], "budgeted")
        for name in ("build", "package", "ui", "device"):
            self.assertEqual(value["authorities"][name], "observational")

    def test_rejects_duplicate_run_identity(self):
        value = report()
        with self.assertRaisesRegex(MODULE.BudgetError, "unique runId"):
            MODULE.check_budget(
                [copy.deepcopy(value) for _ in range(3)],
                budget(), "a" * 40, False)

    def test_budget_binds_collector_accounting_version(self):
        reports = [report() for _ in range(3)]
        reports[1]["environment"]["collectorVersion"] = "2"
        with self.assertRaisesRegex(MODULE.BudgetError, "environment differs"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        reports = [report() for _ in range(3)]
        expected = budget()
        expected["environment"]["collectorVersion"] = "2"
        with self.assertRaisesRegex(MODULE.BudgetError, "collectorVersion"):
            MODULE.check_budget(reports, expected, "a" * 40, False)

    def test_rejects_unknown_and_boolean_schema_values(self):
        value = report()
        value["unknown"] = 1
        with self.assertRaisesRegex(MODULE.BudgetError, "fields mismatch"):
            MODULE.validate_report_value(value)
        value = report()
        value["build"]["elapsedNs"] = True
        with self.assertRaisesRegex(MODULE.BudgetError, "integer"):
            MODULE.validate_report_value(value)

    def test_rejects_escaping_artifact_path(self):
        value = report()
        value["artifacts"]["executables"][0]["path"] = "../outside"
        with self.assertRaisesRegex(MODULE.BudgetError, "contained relative"):
            MODULE.validate_report_value(value)

    def test_rejects_timing_authority_promotion_in_v1(self):
        value = budget()
        value["authorities"]["ui"] = "budgeted"
        with self.assertRaisesRegex(MODULE.BudgetError, "keep V1"):
            MODULE.validate_budget_value(value)

    def test_rejects_artifact_identity_drift_between_reports(self):
        reports = [report() for _ in range(3)]
        reports[2]["artifacts"]["executables"][0]["sha256"] = "e" * 64
        with self.assertRaisesRegex(MODULE.BudgetError, "identities differ"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)

    def test_rejects_absolute_and_relative_artifact_regression(self):
        reports = [report() for _ in range(3)]
        for value in reports:
            value["artifacts"]["bundle"]["bytes"] = 2_001
        with self.assertRaisesRegex(MODULE.BudgetError, "absolute artifact"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        reports = [report() for _ in range(3)]
        for value in reports:
            value["artifacts"]["bundle"]["bytes"] = 1_300
        with self.assertRaisesRegex(MODULE.BudgetError, "relative artifact"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)

    def test_unavailable_observations_are_typed_but_artifacts_must_exist(self):
        value = report()
        MODULE.validate_report_value(value)
        value["artifacts"] = {
            "availability": "unavailable",
            "authority": "budgeted",
            "reason": "missing-app",
        }
        MODULE.validate_report_value(value)
        reports = [copy.deepcopy(value) for _ in range(3)]
        for item in reports:
            item["environment"]["runId"] = f"{next(RUN_IDS):032x}"
        with self.assertRaisesRegex(MODULE.BudgetError, "unavailable"):
            MODULE.check_budget(
                reports, budget(), "a" * 40, False)

    def test_available_device_observation_is_closed_and_observational(self):
        value = report()
        value["device"] = {
            "availability": "available",
            "authority": "observational",
            "name": "Built-in Output",
            "type": "CoreAudio",
            "sampleRate": 48_000,
            "blockFrames": 512,
            "inputChannels": 0,
            "outputChannels": 2,
            "durationMs": 2_000,
            "callbacks": 187,
            "callbackPeriodNs": 10_666_667,
            "maximumCallbackNs": 900_000,
            "deadlineOverruns": 0,
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        }
        value["ui"].update({
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        })
        MODULE.validate_report_value(value)
        value["device"]["xrun"] = 0
        with self.assertRaisesRegex(MODULE.BudgetError, "fields mismatch"):
            MODULE.validate_report_value(value)

    def test_device_observation_rejects_incoherent_cross_fields(self):
        value = report()
        value["device"] = {
            "availability": "available",
            "authority": "observational",
            "name": "Built-in Output",
            "type": "CoreAudio",
            "sampleRate": 48_000,
            "blockFrames": 512,
            "inputChannels": 0,
            "outputChannels": 2,
            "durationMs": 2_000,
            "callbacks": 187,
            "callbackPeriodNs": 10_666_667,
            "maximumCallbackNs": 900_000,
            "deadlineOverruns": 188,
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        }
        value["ui"].update({
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        })
        with self.assertRaisesRegex(MODULE.BudgetError, "exceeds callbacks"):
            MODULE.validate_report_value(value)
        value["device"]["deadlineOverruns"] = 1
        with self.assertRaisesRegex(MODULE.BudgetError, "inconsistent"):
            MODULE.validate_report_value(value)
        value["device"]["deadlineOverruns"] = 0
        value["device"]["callbackPeriodNs"] = 1
        value["device"]["maximumCallbackNs"] = 1
        with self.assertRaisesRegex(MODULE.BudgetError, "audio tuple"):
            MODULE.validate_report_value(value)

    def test_available_ui_requires_exact_probe_provenance(self):
        value = report()
        value["ui"] = {
            "availability": "available",
            "authority": "observational",
            "fixture": "large-graph-128",
            "width": 1440,
            "height": 900,
            "scale": 1.0,
            "warmups": 16,
            "samples": 512,
            "nodeCount": 128,
            "connectionCount": 127,
            "p50Ns": 7_000_000,
            "p95Ns": 8_000_000,
            "p99Ns": 9_000_000,
            "maxNs": 10_000_000,
            "rebuildSamples": 16,
            "rebuildP50Ns": 70_000_000,
            "rebuildP95Ns": 90_000_000,
            "rebuildMaxNs": 100_000_000,
            "pixelDigest": "b" * 64,
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        }
        value["device"].update({
            "probeRunId": "1" * 32,
            "probeBuildFingerprint": "c" * 64,
            "probeExecutableSha256": "d" * 64,
        })
        MODULE.validate_report_value(value)
        value["ui"]["rebuildP95Ns"] = 60_000_000
        with self.assertRaisesRegex(
                MODULE.BudgetError, "rebuild percentiles"):
            MODULE.validate_report_value(value)
        value["ui"]["rebuildP95Ns"] = 90_000_000
        del value["ui"]["probeExecutableSha256"]
        with self.assertRaisesRegex(MODULE.BudgetError, "fields mismatch"):
            MODULE.validate_report_value(value)

    def test_artifact_collector_is_deterministic_and_exact(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = self.make_bundle(Path(directory))
            first = COLLECTOR.collect_artifacts(bundle)
            second = COLLECTOR.collect_artifacts(bundle)
            self.assertEqual(first, second)
            self.assertEqual(first["bundle"]["bytes"], 1 + 2 + 3 + 4)
            self.assertEqual(
                [item["id"] for item in first["executables"]],
                ["workbench-main", "xpc-host", "xpc-probe", "xpc-runtime"])

            walked = list(os.walk(bundle.resolve(), followlinks=False))
            reversed_walk = [
                (current, list(reversed(directories)), list(reversed(files)))
                for current, directories, files in reversed(walked)
            ]
            with mock.patch.object(COLLECTOR.os, "walk", return_value=reversed_walk):
                self.assertEqual(first["bundle"], COLLECTOR.collect_bundle(bundle))

    def test_artifact_collector_rejects_xpc_drift_and_escaping_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = self.make_bundle(root)
            unexpected = (
                bundle / "Contents" / "XPCServices" / "Unexpected.xpc")
            unexpected.mkdir()
            with self.assertRaisesRegex(COLLECTOR.CollectorError, "XPC service set"):
                COLLECTOR.collect_artifacts(bundle)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = self.make_bundle(root)
            outside = root / "outside"
            outside.write_text("outside", encoding="utf-8")
            os.symlink(outside, bundle / "Contents" / "escape")
            with self.assertRaisesRegex(COLLECTOR.CollectorError, "escapes root"):
                COLLECTOR.collect_artifacts(bundle)

    def test_cache_fingerprint_normalizes_source_and_build_roots(self):
        with tempfile.TemporaryDirectory() as first_directory, \
             tempfile.TemporaryDirectory() as second_directory:
            fingerprints = []
            for directory in (first_directory, second_directory):
                root = Path(directory)
                source = root / "source"
                build = root / "build"
                source.mkdir()
                build.mkdir()
                cache = build / "CMakeCache.txt"
                cache.write_text(
                    f"A:PATH={source}/include\nB:PATH={build}/generated\n"
                    "C:STRING=Release\n",
                    encoding="utf-8")
                fingerprints.append(COLLECTOR.normalized_cache_fingerprint(
                    cache, source, build))
            self.assertEqual(fingerprints[0], fingerprints[1])

    def test_collector_canonicalizes_build_environment(self):
        value = COLLECTOR.deterministic_build_environment(Path("/talas-build"), {
            "PATH": "/usr/bin:/bin",
            "CXXFLAGS": "-Ofast",
            "LDFLAGS": "-Wl,-dead_strip",
            "MAKEFLAGS": "CXX_FLAGS=-Ofast",
            "CPATH": "/untrusted/include",
            "CPLUS_INCLUDE_PATH": "/untrusted/cxx-include",
            "LIBRARY_PATH": "/untrusted/lib",
            "SOURCE_DATE_EPOCH": "123",
            "SDKROOT": "/untrusted/sdk",
        }, sdk_root="/canonical/sdk")
        self.assertEqual(value["PATH"], COLLECTOR.CANONICAL_BUILD_PATH)
        self.assertEqual(value["HOME"], "/talas-build/.talas-home")
        self.assertEqual(value["TMPDIR"], "/talas-build/.talas-tmp")
        self.assertEqual(value["SOURCE_DATE_EPOCH"], "0")
        self.assertEqual(value["LC_ALL"], "C")
        self.assertEqual(value["SDKROOT"], "/canonical/sdk")
        for name in (
            "CXXFLAGS", "LDFLAGS", "MAKEFLAGS", "CPATH",
            "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
        ):
            self.assertNotIn(name, value)

    def test_collector_has_no_cleanup_or_notary_operation(self):
        source = (ROOT / "tools" / "collect_product_performance.py").read_text(
            encoding="utf-8")
        self.assertNotIn("rmtree", source)
        self.assertNotIn("git clean", source)
        self.assertNotIn("notarytool", source)
        self.assertIn("build directory must not already exist", source)

    def test_probe_composition_is_commit_bound_and_strict(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "probe.json"
            executable = root / "talas_product_performance_probe"
            executable.write_bytes(b"exact-probe")
            executable_sha256 = hashlib.sha256(b"exact-probe").hexdigest()
            manifest_payload = {
                "format": "talas.performance-build-manifest",
                "version": 1,
                "generator": "Unix Makefiles",
                "configurationFingerprint": "f" * 64,
                "targets": ["talas_product_performance_probe"],
                "compile": [{
                    "target": "talas_product_performance_probe",
                    "file": "$SOURCE/benchmarks/ProductPerformanceProbe.cpp",
                    "output": "$BUILD/probe.o",
                    "directory": "$BUILD",
                    "arguments": ["c++", "-c", "probe.cpp"],
                    "commit": "a" * 40,
                    "juceCommit": COLLECTOR.PINNED_JUCE_COMMIT,
                    "outputSha256": "e" * 64,
                }],
                "link": "c++ probe.o -o talas_product_performance_probe",
                "executable": {
                    "path": "$BUILD/talas_product_performance_probe",
                    "bytes": len(b"exact-probe"),
                    "sha256": executable_sha256,
                },
            }
            canonical = json.dumps(
                manifest_payload, sort_keys=True, separators=(",", ":"),
                ensure_ascii=True)
            manifest = dict(manifest_payload)
            manifest["fingerprint"] = hashlib.sha256(
                canonical.encode("utf-8")).hexdigest()
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            probe = {
                "format": "talas.product-probe-report",
                "version": 2,
                "commit": "a" * 40,
                "runId": "1" * 32,
                "buildFingerprint": manifest["fingerprint"],
                "executableSha256": executable_sha256,
                "ui": report()["ui"],
                "device": report()["device"],
            }
            path.write_text(json.dumps(probe), encoding="utf-8")
            self.assertEqual(
                set(COLLECTOR.load_probe(
                    path, "a" * 40, manifest_path, executable)),
                {"ui", "device"})
            wrong_juce = json.loads(json.dumps(manifest_payload))
            wrong_juce["compile"][0]["juceCommit"] = "b" * 40
            canonical = json.dumps(
                wrong_juce, sort_keys=True, separators=(",", ":"),
                ensure_ascii=True)
            wrong_juce["fingerprint"] = hashlib.sha256(
                canonical.encode("utf-8")).hexdigest()
            manifest_path.write_text(
                json.dumps(wrong_juce), encoding="utf-8")
            with self.assertRaisesRegex(
                    COLLECTOR.CollectorError, "compile entry"):
                COLLECTOR.load_probe(
                    path, "a" * 40, manifest_path, executable)
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(COLLECTOR.CollectorError, "commit"):
                COLLECTOR.load_probe(
                    path, "b" * 40, manifest_path, executable)
            executable.write_bytes(b"changed-probe")
            with self.assertRaisesRegex(
                    COLLECTOR.CollectorError, "executable"):
                COLLECTOR.load_probe(
                    path, "a" * 40, manifest_path, executable)
            executable.write_bytes(b"exact-probe")
            probe["unknown"] = True
            path.write_text(json.dumps(probe), encoding="utf-8")
            with self.assertRaisesRegex(
                    COLLECTOR.CollectorError, "invalid fields"):
                COLLECTOR.load_probe(
                    path, "a" * 40, manifest_path, executable)

    def test_report_destination_is_new_and_outside_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            build.mkdir()
            outside = root / "report.json"
            COLLECTOR.validate_report_destination(outside, source, build)
            outside.write_text("retained", encoding="utf-8")
            with self.assertRaisesRegex(COLLECTOR.CollectorError, "already exists"):
                COLLECTOR.validate_report_destination(outside, source, build)
            with self.assertRaisesRegex(COLLECTOR.CollectorError, "outside"):
                COLLECTOR.validate_report_destination(
                    source / "report.json", source, build)

    def test_collector_uses_only_explicit_pinned_local_juce(self):
        source = (ROOT / "tools" / "collect_product_performance.py").read_text(
            encoding="utf-8")
        self.assertIn("--juce-source", source)
        self.assertIn("-DTALAS_FETCH_JUCE=OFF", source)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", source)
        self.assertIn(COLLECTOR.PINNED_JUCE_COMMIT, source)

    def test_product_probe_is_opt_in_and_release_closed(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        presets = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        self.assertIn(
            'option(TALAS_BUILD_PRODUCT_BENCHMARKS', cmake)
        self.assertIn(
            "TALAS_BUILD_PRODUCT_BENCHMARKS requires TALAS_BUILD_TESTS=ON and TALAS_BUILD_WORKBENCH=ON",
            cmake)
        self.assertIn("talas_product_performance_probe", cmake)
        self.assertNotIn(
            "talas_product_performance_probe",
            "\n".join(
                line for line in cmake.splitlines()
                if line.lstrip().startswith("install(")))
        configure = {
            item["name"]: item for item in presets["configurePresets"]}
        self.assertTrue(configure["product-bench-release"]["cacheVariables"][
            "TALAS_BUILD_PRODUCT_BENCHMARKS"])
        self.assertTrue(configure["product-bench-release"]["cacheVariables"][
            "TALAS_BUILD_TESTS"])
        self.assertNotIn(
            "TALAS_BUILD_PRODUCT_BENCHMARKS",
            configure["workbench-release"]["cacheVariables"])
        self.assertIn(
            "TALAS_PRODUCT_PERFORMANCE_PROBE_SUPPORT=1", cmake)
        self.assertIn(
            r'\"juceCommit\": \"${TALAS_PRODUCT_PROBE_JUCE_COMMIT}\"', cmake)
        probe = (ROOT / "benchmarks" / "ProductPerformanceProbe.cpp").read_text(
            encoding="utf-8")
        self.assertIn("WorkbenchComponent", probe)
        self.assertIn("deviceDeadlineOverruns", probe)
        self.assertIn("device-initialisation-timeout", probe)
        self.assertIn("create_hard_link", probe)
        self.assertNotIn('"xrun"', probe)


if __name__ == "__main__":
    unittest.main()
