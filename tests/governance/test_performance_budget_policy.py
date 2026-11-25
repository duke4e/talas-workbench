import copy
import importlib.util
import itertools
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_performance_budget", ROOT / "tools" / "check_performance_budget.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


RUN_IDS = itertools.count(1)


def report():
    distribution = {"p50_ns": 10, "p95_ns": 20, "p99_ns": 30, "max_ns": 40}
    return {
        "format": "talas.performance-report",
        "version": 1,
        "runId": f"{next(RUN_IDS):032x}",
        "environment": {
            "platform": "macos",
            "architecture": "arm64",
            "compilerId": "AppleClang",
            "compilerVersion": "21.0.0.21000101",
            "buildType": "Release",
            "commit": "a" * 40,
            "preset": "core-bench-release",
            "buildFingerprint": "f" * 64,
        },
        "campaign": {
            "warmups": 8,
            "samples": 64,
            "callbackSamples": 2048,
            "sampleRate": 48000,
            "blockFrames": 256,
            "callbackDeadlineNs": 5333334,
            "seed": 5131095609121850191,
        },
        "scenarios": [{
            "id": "fixture",
            "nodeCount": 2,
            "connectionCount": 1,
            "metrics": {
                "compile": copy.deepcopy(distribution),
                "prepare": copy.deepcopy(distribution),
                "callback": copy.deepcopy(distribution),
                "graphAdoption": copy.deepcopy(distribution),
                "callbackDeadlineMissCount": 0,
                "planStepCount": 2,
                "planBufferSlots": 1,
                "estimatedPreparedBytes": 2048,
                "checksum": 1.0,
            },
        }],
    }


def budget():
    rules = {
        "nodeCount": {"mode": "exact", "value": 2},
        "connectionCount": {"mode": "exact", "value": 1},
        "callbackDeadlineMissCount": {"mode": "maximum", "maximum": 0},
        "planStepCount": {"mode": "exact", "value": 2},
        "planBufferSlots": {"mode": "exact", "value": 1},
        "estimatedPreparedBytes": {"mode": "exact", "value": 2048},
    }
    for name in ("compile", "prepare", "callback", "graphAdoption"):
        for percentile, baseline in (
            ("p50_ns", 10), ("p95_ns", 20), ("p99_ns", 30),
        ):
            rules[f"{name}.{percentile}"] = {
                "mode": "timing",
                "baseline": baseline,
                "absoluteMax": baseline * 2,
                "relativeMax": 0.5,
                "maxSpread": 0.25,
            }
        rules[f"{name}.max_ns"] = {"mode": "maximum", "maximum": 160}
    return {
        "format": "talas.performance-budget",
        "version": 1,
        "environment": {
            "platform": "macos",
            "architecture": "arm64",
            "compilerId": "AppleClang",
            "compilerVersion": "21.0.0.21000101",
            "buildType": "Release",
            "preset": "core-bench-release",
            "buildFingerprint": "f" * 64,
        },
        "campaign": copy.deepcopy(report()["campaign"]),
        "minimumReports": 5,
        "scenarios": {"fixture": rules},
    }


class PerformanceBudgetPolicyTests(unittest.TestCase):
    def test_checked_in_budget_is_closed_and_platform_bound(self):
        path = (
            ROOT / "benchmarks" / "baselines"
            / "macos-arm64-appleclang21-release.json"
        )
        accepted = MODULE.validate_budget(path)
        self.assertEqual(accepted["minimumReports"], 5)
        self.assertEqual(
            set(accepted["scenarios"]),
            {"oscillator-small", "oscillator-chain", "spectral-roundtrip", "event-control"},
        )
        self.assertEqual(accepted["environment"]["platform"], "macos")
        self.assertEqual(accepted["environment"]["architecture"], "arm64")
        for rules in accepted["scenarios"].values():
            self.assertEqual(rules["callback.max_ns"]["mode"], "maximum")
            self.assertEqual(rules["callbackDeadlineMissCount"]["maximum"], 0)

    def test_accepts_stable_exact_campaign(self):
        MODULE.check_budget(
            [report() for _ in range(5)], budget(), "a" * 40, False)

    def test_rejects_duplicate_run_identity(self):
        repeated = report()
        with self.assertRaisesRegex(MODULE.BudgetError, "unique runId"):
            MODULE.check_budget(
                [copy.deepcopy(repeated) for _ in range(5)], budget(),
                "a" * 40, False)

    def test_rejects_unknown_report_field(self):
        value = report()
        value["unknown"] = 1
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.BudgetError, "fields mismatch"):
                MODULE.validate_report(path)

    def test_rejects_metadata_or_campaign_mismatch(self):
        reports = [report() for _ in range(5)]
        reports[0]["environment"]["architecture"] = "x86_64"
        with self.assertRaisesRegex(MODULE.BudgetError, "environment mismatch"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        reports = [report() for _ in range(5)]
        reports[4]["campaign"]["samples"] = 63
        with self.assertRaisesRegex(MODULE.BudgetError, "campaign differs"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        with self.assertRaisesRegex(MODULE.BudgetError, "expected-commit"):
            MODULE.check_budget(
                [report() for _ in range(5)], budget(), "b" * 40, False)

    def test_rejects_unstable_timing(self):
        reports = [report() for _ in range(5)]
        reports[4]["scenarios"][0]["metrics"]["callback"]["p50_ns"] = 14
        with self.assertRaisesRegex(MODULE.BudgetError, "unstable"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)

    def test_rejects_absolute_and_relative_regression(self):
        reports = [report() for _ in range(5)]
        for value in reports:
            value["scenarios"][0]["metrics"]["callback"]["p50_ns"] = 21
        with self.assertRaisesRegex(MODULE.BudgetError, "absolute regression"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        limited = budget()
        limited["scenarios"]["fixture"]["callback.p50_ns"]["absoluteMax"] = 30
        with self.assertRaisesRegex(MODULE.BudgetError, "relative regression"):
            MODULE.check_budget(reports, limited, "a" * 40, False)

    def test_rejects_structural_regression_and_incomplete_metric_rules(self):
        reports = [report() for _ in range(5)]
        reports[2]["scenarios"][0]["metrics"]["planBufferSlots"] = 2
        with self.assertRaisesRegex(MODULE.BudgetError, "structural regression"):
            MODULE.check_budget(reports, budget(), "a" * 40, False)
        incomplete = budget()
        del incomplete["scenarios"]["fixture"]["callback.max_ns"]
        with self.assertRaisesRegex(MODULE.BudgetError, "metric set differs"):
            MODULE.check_budget(
                [report() for _ in range(5)], incomplete, "a" * 40, False)

    def test_rejects_metric_rule_mode_substitution(self):
        substituted = budget()
        substituted["scenarios"]["fixture"]["callback.p99_ns"] = {
            "mode": "maximum", "maximum": 1000,
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "budget.json"
            path.write_text(json.dumps(substituted), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.BudgetError, "must use timing mode"):
                MODULE.validate_budget(path)

    def test_rejects_non_integer_schema_versions(self):
        for invalid in (True, 1.0):
            value = report()
            value["version"] = invalid
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "report.json"
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(MODULE.BudgetError, "format/version"):
                    MODULE.validate_report(path)
            value = budget()
            value["version"] = invalid
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "budget.json"
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(MODULE.BudgetError, "format/version"):
                    MODULE.validate_budget(path)

    def test_cli_rejects_abbreviated_and_duplicate_singular_options(self):
        checker = ROOT / "tools" / "check_performance_budget.py"
        abbreviated = subprocess.run(
            [
                "python3", str(checker),
                "--report", "missing", "--budget", "missing",
                "--expected-commit", "a" * 40, "--rep", "extra",
            ],
            text=True, capture_output=True, check=False)
        self.assertEqual(abbreviated.returncode, 2)
        self.assertIn("unrecognized arguments", abbreviated.stderr)
        duplicated = subprocess.run(
            [
                "python3", str(checker), "--report", "missing",
                "--budget", "one", "--budget", "two",
                "--expected-commit", "a" * 40,
            ],
            text=True, capture_output=True, check=False)
        self.assertEqual(duplicated.returncode, 2)
        self.assertIn("duplicate --budget option", duplicated.stderr)

    def test_build_manifest_fingerprint_tracks_actual_compile_commands(self):
        launcher = ROOT / "tools" / "capture_compiler_invocation.py"
        writer = ROOT / "tools" / "write_performance_build_manifest.py"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            build.mkdir()
            compile_path = build / "compile_commands.json"
            invocation_dir = build / "invocations"
            link_path = build / "link.txt"
            output = "CMakeFiles/talas_core.dir/src/core.cpp.o"
            entries = [
                {
                    "directory": str(build),
                    "command": f"/usr/bin/c++ -O3 -o {output} -c {source}/src/core.cpp",
                    "file": str(source / "src/core.cpp"),
                    "output": output,
                },
                {
                    "directory": str(build),
                    "command": (
                        f'/usr/bin/c++ -O3 -DTALAS_BENCHMARK_COMMIT=\\"{"a" * 40}\\" -o '
                        "CMakeFiles/talas_core_performance_benchmarks.dir/bench.cpp.o "
                        f"-c {source}/bench.cpp"),
                    "file": str(source / "bench.cpp"),
                    "output": (
                        "CMakeFiles/talas_core_performance_benchmarks.dir/bench.cpp.o"),
                },
                {
                    "directory": str(build),
                    "command": (
                        "/usr/bin/c++ -O3 -o "
                        "CMakeFiles/talas_extra.dir/src/extra.cpp.o "
                        f"-c {source}/src/extra.cpp"),
                    "file": str(source / "src/extra.cpp"),
                    "output": "CMakeFiles/talas_extra.dir/src/extra.cpp.o",
                },
            ]
            compile_path.write_text(json.dumps(entries), encoding="utf-8")
            link_path.write_text(
                "/usr/bin/c++ CMakeFiles/talas_core_performance_benchmarks.dir/bench.cpp.o "
                "-o talas_core_performance_benchmarks libtalas_core.a libtalas_extra.a\n",
                encoding="utf-8")

            def capture(entry, extra_flags=()):
                output_path = build / entry["output"]
                output_path.parent.mkdir(parents=True, exist_ok=True)
                program = (
                    "import pathlib,sys; "
                    "pathlib.Path(sys.argv[sys.argv.index('-o')+1]).write_text('object')")
                completed = subprocess.run([
                    "python3", str(launcher),
                    "--log-dir", str(invocation_dir), "--",
                    "python3", "-c", program, "--", *extra_flags,
                    "-o", str(output_path), "-c", entry["file"],
                ], text=True, capture_output=True, check=False, cwd=build)
                self.assertEqual(completed.returncode, 0, completed.stderr)

            for entry in entries:
                capture(entry)

            def write_fingerprint(suffix):
                manifest = build / f"manifest-{suffix}.json"
                fingerprint = build / f"fingerprint-{suffix}.txt"
                completed = subprocess.run([
                    "python3", str(writer),
                    "--compile-commands", str(compile_path),
                    "--invocation-log-dir", str(invocation_dir),
                    "--link-command", str(link_path),
                    "--source-root", str(source),
                    "--build-root", str(build),
                    "--generator", "Unix Makefiles",
                    "--configuration-fingerprint", "f" * 64,
                    "--manifest", str(manifest),
                    "--fingerprint", str(fingerprint),
                ], text=True, capture_output=True, check=False)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                parsed = json.loads(manifest.read_text(encoding="utf-8"))
                self.assertEqual(
                    parsed["targets"],
                    ["talas_core", "talas_core_performance_benchmarks", "talas_extra"])
                return fingerprint.read_text(encoding="utf-8").strip()

            first = write_fingerprint("first")
            entries[1]["command"] = entries[1]["command"].replace(
                "a" * 40, "b" * 40)
            compile_path.write_text(json.dumps(entries), encoding="utf-8")
            commit_only = write_fingerprint("commit-only")
            self.assertEqual(first, commit_only)
            capture(entries[0], ("-flto",))
            flag_change = write_fingerprint("flag-change")
            self.assertNotEqual(first, flag_change)

    def test_build_contract_is_opt_in_and_release_closed(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        self.assertIn(
            'option(TALAS_BUILD_BENCHMARKS "Build opt-in core performance benchmarks" OFF)',
            cmake,
        )
        self.assertIn("TALAS_BUILD_BENCHMARKS requires TALAS_BUILD_TESTS=ON", cmake)
        self.assertIn("Performance benchmark configure requires clean tracked sources", cmake)
        self.assertIn("TALAS_BENCHMARK_BUILD_FINGERPRINT", cmake)
        self.assertIn("get_cmake_property(TALAS_BENCHMARK_CACHE_VARIABLES CACHE_VARIABLES)", cmake)
        self.assertIn("INTERPROCEDURAL_OPTIMIZATION", cmake)
        self.assertIn("EXE_LINKER_", cmake)
        self.assertIn("TOOLCHAIN_FILE", cmake)
        self.assertIn("CXX_", cmake)
        self.assertIn("talas_core_performance_benchmark_provenance", cmake)
        self.assertIn("write_performance_build_manifest.py", cmake)
        self.assertIn("capture_compiler_invocation.py", cmake)
        self.assertIn("TALAS_BENCHMARK_COMPILER_INVOCATION_LOG_DIR", cmake)
        self.assertIn("compile_commands.json", cmake)
        self.assertIn("link.txt", cmake)
        self.assertNotIn("talas_core_performance_benchmarks", "\n".join(
            line for line in cmake.splitlines() if line.lstrip().startswith("install(")))
        configure = {item["name"]: item for item in presets["configurePresets"]}
        self.assertTrue(
            configure["core-bench-release"]["cacheVariables"]["TALAS_BUILD_BENCHMARKS"])
        self.assertEqual(
            configure["core-bench-release"]["cacheVariables"]["TALAS_BENCHMARK_PRESET_ID"],
            "core-bench-release")

    def test_workflow_documents_noise_and_non_claims(self):
        workflow = (
            ROOT / "docs" / "testing" / "performance-budgets.md"
        ).read_text(encoding="utf-8")
        self.assertIn("does not measure OS/device xruns", workflow)
        self.assertIn("does not update the baseline", workflow)


if __name__ == "__main__":
    unittest.main()
