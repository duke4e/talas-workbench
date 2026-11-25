import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class ReleaseConfigurationTests(unittest.TestCase):
    def test_spectrum_modifier_probability_profile_is_revalidated_only_at_preparation(
            self):
        runtime = (ROOT / "src" / "runtime" / "Runtime.cpp").read_text(
            encoding="utf-8"
        )
        helper = "hasExactSpectrumModifierProbabilityProfile("
        self.assertEqual(
            runtime.count(helper),
            2,
            "the 4096-entry profile check must have one definition and one "
            "offline-session preparation call",
        )
        render_start = runtime.index(
            "[[nodiscard]] bool renderSpectrumModifier("
        )
        render_end = runtime.index(
            "[[nodiscard]] bool renderSpectrumTrackSelector(",
            render_start,
        )
        self.assertNotIn(
            helper,
            runtime[render_start:render_end],
            "offline blocks must consume the already validated immutable "
            "profile by fixed indexed access",
        )

    def test_project_expression_parsers_require_explicit_dialect_authority(self):
        call_prefix = "talas::expr::parse("
        inspected = []
        for source_root in (
                ROOT / "src" / "project",
                ROOT / "src" / "runtime"):
            for path in sorted(source_root.rglob("*.cpp")):
                text = path.read_text(encoding="utf-8")
                offset = 0
                while True:
                    call = text.find(call_prefix, offset)
                    if call < 0:
                        break
                    cursor = call + len(call_prefix)
                    depth = 1
                    top_level_commas = 0
                    quote = None
                    escaped = False
                    while cursor < len(text) and depth > 0:
                        character = text[cursor]
                        if quote is not None:
                            if escaped:
                                escaped = False
                            elif character == "\\":
                                escaped = True
                            elif character == quote:
                                quote = None
                        elif character in ('"', "'"):
                            quote = character
                        elif character == "(":
                            depth += 1
                        elif character == ")":
                            depth -= 1
                        elif character == "," and depth == 1:
                            top_level_commas += 1
                        cursor += 1
                    self.assertEqual(
                        depth,
                        0,
                        f"unterminated expression parse call in {path}",
                    )
                    self.assertGreaterEqual(
                        top_level_commas,
                        1,
                        f"project/product parser silently defaults to LegacyV1 in {path}",
                    )
                    inspected.append(path.relative_to(ROOT).as_posix())
                    offset = cursor
        self.assertIn("src/project/ProjectExpressionAudit.cpp", inspected)
        self.assertIn("src/runtime/Runtime.cpp", inspected)
        self.assertIn("src/runtime/RealtimeRenderSession.cpp", inspected)

    def test_current_format_has_one_network_free_sample_map_authority(self):
        project_document = (
            ROOT / "include" / "talas" / "project" / "ProjectDocument.h"
        ).read_text(encoding="utf-8")
        project_json = (
            ROOT / "src" / "project" / "ProjectJson.cpp"
        ).read_text(encoding="utf-8")
        fixture = (
            ROOT
            / "tests"
            / "fixtures"
            / "project-format-13-sample-map.talas.json"
        ).read_text(encoding="utf-8")

        self.assertIn("inline constexpr int currentProjectFormatVersion = 156;", project_document)
        self.assertIn("int formatVersion = currentProjectFormatVersion;", project_document)
        self.assertIn(
            "ExpressionLanguageVersion expressionLanguage =",
            project_document,
        )
        self.assertIn("ExpressionLanguageVersion::VariablesV2", project_document)
        self.assertIn(
            "ExpressionDefinitionLibrary expressionDefinitions;",
            project_document,
        )
        self.assertIn(
            "ProjectSampleMapLibraryState sampleMapLibrary;",
            project_document,
        )
        self.assertEqual(
            project_json.count("constexpr int supportedVersion = currentProjectFormatVersion;"),
            1,
        )
        self.assertIn("constexpr int versionTwentyTwo = 22;", project_json)
        self.assertIn('"expressionLanguage"', project_json)
        self.assertIn('return "legacyV1";', project_json)
        self.assertIn('return "pureV2";', project_json)
        self.assertIn("constexpr int versionEleven = 11;", project_json)
        self.assertIn("appendSampleMapLibrary", project_json)
        self.assertIn("parseSampleMapLibrary", project_json)
        self.assertNotIn("#include <filesystem>", project_json)
        self.assertIn('"version": 13', fixture)
        self.assertIn('"sampleMapLibrary": {', fixture)

    def test_project_migration_policy_is_closed_and_ci_gated(self):
        policy = json.loads((
            ROOT
            / "docs"
            / "governance"
            / "project-format-migration-policy.json"
        ).read_text(encoding="utf-8"))
        self.assertEqual(
            policy,
            {
                "schemaVersion": 1,
                "format": "talas.project",
                "currentVersion": 156,
                "acceptedSourceVersions": list(range(1, 157)),
                "rejectedBoundaryVersions": [0, 157],
            },
        )

        project_document = (
            ROOT / "include" / "talas" / "project" / "ProjectDocument.h"
        ).read_text(encoding="utf-8")
        project_json = (
            ROOT / "src" / "project" / "ProjectJson.cpp"
        ).read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        migration_docs = (
            ROOT / "docs" / "testing" / "project-format-migrations.md"
        ).read_text(encoding="utf-8")
        normalized_migration_docs = " ".join(migration_docs.split())

        self.assertEqual(project_document.count("int formatVersion = currentProjectFormatVersion;"), 1)
        self.assertEqual(
            project_json.count("constexpr int supportedVersion = currentProjectFormatVersion;"),
            1,
        )
        self.assertIn(
            "docs/governance/project-format-migration-policy.json",
            cmake,
        )
        labeled_tests = []
        for block in re.findall(
                r"set_tests_properties\((.*?)\)", cmake, re.DOTALL):
            if 'LABELS "migration"' not in block:
                continue
            labeled_tests.extend(re.findall(
                r"\btalas_[a-z0-9_]+\b",
                block.split("PROPERTIES", 1)[0],
            ))
        self.assertEqual(
            labeled_tests,
            ["talas_project_migration_compatibility_tests"],
        )
        self.assertIn(
            f"1 through {policy['currentVersion']}",
            normalized_migration_docs,
        )
        self.assertIn(
            f"in-memory document to version {policy['currentVersion']}",
            normalized_migration_docs,
        )
        self.assertIn(
            f"writer always saves version {policy['currentVersion']}",
            normalized_migration_docs,
        )
        self.assertIn(
            f"versions {policy['rejectedBoundaryVersions'][0]} and "
            f"{policy['rejectedBoundaryVersions'][1]}",
            normalized_migration_docs,
        )
        self.assertIn(
            "one minimal, project-original document for each",
            normalized_migration_docs,
        )
        self.assertIn(
            "Versions 1 through 22 migrate losslessly to `legacyV1`",
            normalized_migration_docs,
        )
        self.assertIn(
            "New documents default to `variablesV2`",
            normalized_migration_docs,
        )
        self.assertIn(
            "not a claim that every malformed legacy",
            normalized_migration_docs,
        )
        for excluded_scope in (
            "private historical/reference files",
            "workspace state",
            "package containers",
            "component bundle schemas",
            "external assets",
            "future versions",
        ):
            self.assertIn(excluded_scope, normalized_migration_docs)
        self.assertIn(
            "presentation field when injected into a v10 source",
            normalized_migration_docs,
        )
        self.assertNotIn(
            "fields used before their schema introduction",
            normalized_migration_docs,
        )

    def test_cmake_owns_version_dependency_install_and_package_rules(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("project(Talas VERSION", cmake)
        self.assertIn("option(TALAS_FETCH_JUCE", cmake)
        self.assertIn("FetchContent_Declare", cmake)
        self.assertIn("FetchContent_MakeAvailable(JUCE)", cmake)
        self.assertIn("EXCLUDE_FROM_ALL", cmake)
        self.assertIn("8.0.15", cmake)
        self.assertIn("91ad83ae34a81e0833b1a2b0866f54846370ae53", cmake)
        self.assertIn("CMAKE_MSVC_RUNTIME_LIBRARY", cmake)
        self.assertIn("TARGETS talas_cli", cmake)
        self.assertIn("TARGETS talas_workbench", cmake)
        self.assertIn("NAME talas_workbench_app_smoke", cmake)
        self.assertIn("$<TARGET_FILE:talas_workbench>", cmake)
        self.assertIn("--smoke-test", cmake)
        self.assertIn("CPACK_RESOURCE_FILE_LICENSE", cmake)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE", cmake)
        self.assertIn("include(CPack)", cmake)

    def test_workbench_uses_private_extracted_ui_library(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_library(talas_workbench_ui", cmake)
        ui_target = re.search(
            r"add_library\(talas_workbench_ui STATIC(?P<sources>.*?)\n    \)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(ui_target)
        for source in (
            "WorkbenchCommands.cpp",
            "WorkbenchComponent.cpp",
            "WorkbenchProjectLifecycle.cpp",
            "WorkbenchRuntime.cpp",
            "WorkbenchTheme.cpp",
            "WorkbenchDiagnosticsPanel.cpp",
            "WorkbenchAssetRecoveryPanel.cpp",
            "WorkbenchAnalysisPanel.cpp",
            "WorkbenchGraphCanvas.cpp",
            "WorkbenchInspectorPanel.cpp",
            "WorkbenchPrototypePanel.cpp",
            "WorkbenchSoundDefinitionsPanel.cpp",
            "WorkbenchSnapshotPanel.cpp",
            "WorkbenchSplitHandle.cpp",
            "WorkbenchTimelinePanel.cpp",
            "WorkbenchVirtualControls.cpp",
        ):
            self.assertIn(
                f"src/app/workbench/{source}",
                ui_target.group("sources"),
            )
        self.assertIn(
            "target_link_libraries(talas_workbench\n"
            "        PRIVATE\n"
            "            talas_workbench_ui",
            cmake,
        )
        workbench_main = (
            ROOT / "src" / "app" / "WorkbenchMain.cpp"
        ).read_text(encoding="utf-8")
        self.assertTrue(
            (ROOT / "src" / "app" / "workbench" / "WorkbenchHost.h").is_file()
        )
        self.assertIn("makeWorkbenchHost", workbench_main)
        self.assertNotIn("class WorkbenchComponent", workbench_main)
        self.assertEqual(
            workbench_main.count('#include "app/workbench/'),
            1,
        )
        self.assertIn(
            '#include "app/workbench/WorkbenchHost.h"',
            workbench_main,
        )
        application_sources = re.search(
            r"target_sources\(talas_workbench PRIVATE(?P<sources>.*?)\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(application_sources)
        self.assertEqual(
            application_sources.group("sources").strip(),
            "src/app/WorkbenchMain.cpp",
        )
        install_rules = "\n".join(
            re.findall(r"install\((.*?)\n\)", cmake, re.DOTALL)
        )
        self.assertNotIn("src/app/workbench", install_rules)
        self.assertNotIn("WorkbenchHost.h", install_rules)
        self.assertNotIn("WorkbenchComponentInternal.h", install_rules)
        coordinator_root = ROOT / "src" / "app" / "workbench"
        coordinator_header = (
            coordinator_root / "WorkbenchComponentInternal.h"
        ).read_text(encoding="utf-8")
        self.assertIn("class WorkbenchComponent final", coordinator_header)
        for source in (
            "WorkbenchAnalysisAudition.cpp",
            "WorkbenchAnalysisRecording.cpp",
            "WorkbenchComponent.cpp",
            "WorkbenchCommands.cpp",
            "WorkbenchLayout.cpp",
            "WorkbenchProjectLifecycle.cpp",
            "WorkbenchRuntime.cpp",
            "WorkbenchSmoke.cpp",
        ):
            implementation = (coordinator_root / source).read_text(
                encoding="utf-8"
            )
            self.assertNotIn("class WorkbenchComponent final", implementation)
            self.assertNotRegex(
                implementation,
                r'#include\s+["<].*\.(?:cpp|inc)[">]',
            )
            self.assertLessEqual(len(implementation.splitlines()), 4500)

    def test_readme_records_the_selected_project_license(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("original Talas code is licensed under Apache-2.0", readme)
        self.assertIn("third_party/cdp/", readme)
        self.assertIn("LGPL-2.1", readme)
        self.assertNotIn("_References/", readme)
        self.assertNotIn(
            "Public distribution still requires an owner-selected project license",
            readme,
        )

    def test_presets_cover_core_workbench_test_and_package_flows(self):
        payload = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
        )
        configure = {preset["name"]: preset for preset in payload["configurePresets"]}
        build = {preset["name"]: preset for preset in payload["buildPresets"]}
        tests = {preset["name"]: preset for preset in payload["testPresets"]}
        packages = {preset["name"]: preset for preset in payload["packagePresets"]}

        self.assertIn("core-debug", configure)
        self.assertEqual(configure["core-debug"]["cacheVariables"]["TALAS_BUILD_WORKBENCH"], False)
        self.assertIn("workbench-release", configure)
        self.assertEqual(configure["workbench-release"]["cacheVariables"]["TALAS_FETCH_JUCE"], True)
        self.assertIn("core-debug", build)
        self.assertEqual(build["core-debug"]["configuration"], "Debug")
        self.assertIn("workbench-release", build)
        self.assertIn("core-debug", tests)
        self.assertEqual(tests["core-debug"]["configuration"], "Debug")
        self.assertIn("workbench-release", packages)

    def test_sanitizer_and_fuzz_presets_are_target_bounded_and_nonshipping(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        payload = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
        )
        configure = {preset["name"]: preset for preset in payload["configurePresets"]}
        build = {preset["name"]: preset for preset in payload["buildPresets"]}
        tests = {preset["name"]: preset for preset in payload["testPresets"]}

        self.assertIn(
            'option(TALAS_BUILD_FUZZERS "Build opt-in Clang sanitizer fuzz harnesses" OFF)',
            cmake,
        )
        self.assertIn("if(TALAS_BUILD_FUZZERS AND NOT TALAS_BUILD_TESTS)", cmake)
        self.assertIn('set(TALAS_FUZZ_ENGINE "REPLAY" CACHE STRING', cmake)
        self.assertIn("core-asan-ubsan", configure)
        for flag_variable in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"):
            asan_flags = configure["core-asan-ubsan"]["cacheVariables"][
                flag_variable]
            self.assertIn("-fsanitize=address,undefined", asan_flags)
            self.assertIn("-fno-sanitize-recover=undefined", asan_flags)
        libfuzzer_branch = cmake.split(
            'if(TALAS_FUZZ_ENGINE STREQUAL "LIBFUZZER")', 1)[1].split(
                'elseif(TALAS_FUZZ_ENGINE STREQUAL "REPLAY")', 1)[0]
        replay_branch = cmake.split(
            'elseif(TALAS_FUZZ_ENGINE STREQUAL "REPLAY")', 1)[1].split(
                "else()", 1)[0]
        for fuzz_branch in (libfuzzer_branch, replay_branch):
            compile_options = fuzz_branch.split(
                "set(TALAS_FUZZ_COMPILE_OPTIONS", 1)[1].split(")", 1)[0]
            self.assertIn("-fsanitize=", compile_options)
            self.assertIn(
                "-fno-sanitize-recover=undefined", compile_options)
        self.assertIn("core-tsan", configure)
        self.assertIn("-fsanitize=thread", configure["core-tsan"]["cacheVariables"]["CMAKE_CXX_FLAGS"])
        property_targets = [
            "talas_project_json_property_tests",
            "talas_expression_property_tests",
            "talas_context_free_generator_property_tests",
            "talas_graph_property_tests",
            "talas_project_package_property_tests",
            "talas_typed_event_tests",
        ]
        for preset_name, configure_name in (
            ("core-asan-ubsan-property", "core-asan-ubsan"),
            ("core-tsan-property", "core-tsan"),
        ):
            self.assertEqual(
                build[preset_name]["configurePreset"], configure_name)
            self.assertEqual(build[preset_name]["configuration"], "Debug")
            self.assertEqual(build[preset_name]["targets"], property_targets)
            self.assertEqual(
                tests[preset_name]["configurePreset"], configure_name)
            self.assertEqual(tests[preset_name]["configuration"], "Debug")
            self.assertEqual(
                tests[preset_name]["filter"]["include"]["label"],
                "^property$",
            )
            self.assertTrue(tests[preset_name]["output"]["outputOnFailure"])
            self.assertEqual(
                tests[preset_name]["execution"]["noTestsAction"],
                "error",
            )
        threaded_targets = build["core-tsan-threaded"]["targets"]
        self.assertEqual(
            build["core-tsan-threaded"]["configurePreset"], "core-tsan")
        self.assertEqual(
            tests["core-tsan-threaded"]["filter"]["include"]["label"],
            "^threaded$",
        )
        self.assertEqual(
            tests["core-tsan-threaded"]["execution"]["noTestsAction"],
            "error",
        )
        threaded_labels = re.search(
            r"foreach\(threaded_test IN ITEMS(?P<tests>[^)]*)\)", cmake)
        self.assertIsNotNone(threaded_labels)
        self.assertEqual(
            threaded_labels.group("tests").split(), threaded_targets)
        self.assertTrue(configure["core-fuzz"]["cacheVariables"]["TALAS_BUILD_FUZZERS"])
        self.assertEqual(configure["core-fuzz"]["cacheVariables"]["TALAS_FUZZ_ENGINE"], "REPLAY")
        self.assertEqual(
            build["core-fuzz"]["targets"],
            [
                "talas_project_json_fuzzer",
                "talas_expression_fuzzer",
                "talas_context_free_generator_fuzzer",
            ],
        )
        self.assertEqual(
            tests["core-fuzz"]["filter"]["include"]["label"],
            "fuzz-smoke",
        )
        labeled_tests = {}
        for block in re.findall(
                r"set_tests_properties\((.*?)\)", cmake, re.DOTALL):
            for label in ("property", "fuzz-smoke", "benchmark-smoke"):
                if f'LABELS "{label}"' in block:
                    names = re.findall(
                        r"\btalas_[a-z0-9_]+\b",
                        block.split("PROPERTIES", 1)[0],
                    )
                    labeled_tests.setdefault(label, []).extend(names)
        self.assertEqual(labeled_tests["property"], property_targets)
        self.assertEqual(
            labeled_tests["fuzz-smoke"],
            [
                "talas_project_json_fuzz_smoke",
                "talas_expression_fuzz_smoke",
                "talas_context_free_generator_fuzz_smoke",
            ],
        )
        self.assertEqual(
            labeled_tests["benchmark-smoke"],
            ["talas_core_performance_benchmark_smoke"],
        )
        self.assertNotIn(
            "TALAS_BUILD_FUZZERS",
            configure["workbench-release"]["cacheVariables"],
        )
        install_rules = "\n".join(
            re.findall(r"install\((.*?)\n\)", cmake, re.DOTALL)
        )
        self.assertNotIn("talas_project_json_fuzzer", install_rules)
        self.assertNotIn("talas_expression_fuzzer", install_rules)
        self.assertNotIn("talas_context_free_generator_fuzzer", install_rules)
        self.assertNotIn("tests/fuzz", install_rules)

    def test_performance_benchmark_is_smoke_only_and_nonshipping(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        payload = json.loads(
            (ROOT / "CMakePresets.json").read_text(encoding="utf-8")
        )
        configure = {preset["name"]: preset for preset in payload["configurePresets"]}
        build = {preset["name"]: preset for preset in payload["buildPresets"]}
        tests = {preset["name"]: preset for preset in payload["testPresets"]}

        self.assertIn(
            'option(TALAS_BUILD_BENCHMARKS "Build opt-in core performance benchmarks" OFF)',
            cmake,
        )
        self.assertIn("if(TALAS_BUILD_BENCHMARKS AND NOT TALAS_BUILD_TESTS)", cmake)
        self.assertTrue(
            configure["core-bench-release"]["cacheVariables"]["TALAS_BUILD_BENCHMARKS"]
        )
        self.assertEqual(
            build["core-bench-release"]["targets"],
            ["talas_core_performance_benchmarks"],
        )
        self.assertEqual(
            tests["core-bench-release"]["filter"]["include"]["label"],
            "benchmark-smoke",
        )
        self.assertNotIn(
            "TALAS_BUILD_BENCHMARKS",
            configure["workbench-release"]["cacheVariables"],
        )
        install_rules = "\n".join(
            re.findall(r"install\((.*?)\n\)", cmake, re.DOTALL)
        )
        self.assertNotIn("talas_core_performance_benchmarks", install_rules)

    def test_product_performance_probe_is_local_opt_in_only(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("option(TALAS_BUILD_PRODUCT_BENCHMARKS", cmake)
        self.assertIn("if(TALAS_BUILD_PRODUCT_BENCHMARKS)", cmake)
        install_rules = "\n".join(
            line for line in cmake.splitlines()
            if line.lstrip().startswith("install("))
        self.assertNotIn("talas_product_performance_probe", install_rules)
        self.assertNotIn("benchmarks/", install_rules)

    def test_generated_ci_gate_is_check_only_and_labeled(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        checker = (
            ROOT / "tools" / "check_generated_artifacts.py"
        ).read_text(encoding="utf-8")
        node_help_report = (
            ROOT / "tools" / "report_node_help.py"
        ).read_text(encoding="utf-8")
        self.assertIn("NAME talas_generated_artifact_checks", cmake)
        self.assertIn(
            "find_package(Python3 REQUIRED COMPONENTS Interpreter)",
            cmake,
        )
        self.assertIn('LABELS "generated"', cmake)
        self.assertIn('LABELS "conformance"', cmake)
        self.assertIn('LABELS "example-smoke"', cmake)
        self.assertEqual(checker.count('"--check"'), 2)
        self.assertIn('"catalog", "--format", "json"', checker)
        self.assertIn("report_node_help.py", checker)
        self.assertIn('mode.add_argument("--check"', node_help_report)
        self.assertIn('mode.add_argument("--write"', node_help_report)
        self.assertNotIn('"--write"', checker)
        self.assertEqual(checker.count('"-B"'), 2)
        self.assertNotIn("write_text", checker)
        self.assertNotIn("write_bytes", checker)
        generated_test = re.search(
            r"add_test\(\s*NAME talas_generated_artifact_checks"
            r"(?P<body>.*?)\n\s*\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(generated_test)
        self.assertEqual(
            " ".join(generated_test.group("body").split()),
            "COMMAND ${Python3_EXECUTABLE} "
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_generated_artifacts.py "
            "--cli $<TARGET_FILE:talas_cli>",
        )
        def generated_properties_in(source):
            return [
                " ".join(block.split())
                for block in re.findall(
                    r"set_tests_properties\((.*?)\)", source, re.DOTALL
                )
                if re.search(r"\btalas_generated_artifact_checks\b", block)
            ]

        generated_properties = generated_properties_in(cmake)
        self.assertEqual(generated_properties, [
            "talas_generated_artifact_checks PROPERTIES LABELS \"generated\" "
            "WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}",
        ])
        self.assertEqual(cmake.count("talas_generated_artifact_checks"), 2)
        disabled = cmake.replace(
            'LABELS "generated"',
            'LABELS "generated" DISABLED TRUE',
            1,
        )
        self.assertNotEqual(
            generated_properties_in(disabled), generated_properties
        )

    def test_node_help_policy_is_closed_generated_and_packaged_by_allowlist(self):
        policy = json.loads((
            ROOT / "docs/governance/node-help-policy.json"
        ).read_text(encoding="utf-8"))
        expected_examples = [
            ("examples/c100_phase_shift_by_90_allpass_transport.talas.json",
             "Phase Shift By 90 Allpass Transport", "renderable"),
            ("examples/c101_single_side_band_rm_quadrature_transport.talas.json",
             "Single Side Band RM Quadrature Transport", "renderable"),
            ("examples/c102_sos_oscillators_selected_wavetable_transport.talas.json",
             "SOS Oscillators Selected Wavetable Transport", "renderable"),
            ("examples/c103_low_pass_gate_control_state_transport.talas.json",
             "Low-Pass Gate Control State Transport", "renderable"),
            ("examples/c104_reverb_section_fractional_delay_transport.talas.json",
             "Reverb Section Fractional Delay Transport", "renderable"),
            ("examples/c105_monotonizer_known_pitch_transport.talas.json",
             "Monotonizer Known-Pitch Transport", "renderable"),
            ("examples/c106_multifile_disk_player_selection_transport.talas.json",
             "Multifile Disk Player Selection Transport", "renderable"),
            ("examples/c10_midi_mapper_modes.talas.json",
             "Bounded MIDI Mapper gate and velocity modes", "renderable"),
            ("examples/c110_sample_cloud_dynamic_controls.talas.json",
             "Sample Cloud Dynamic Controls", "renderable"),
            ("examples/c111_cloud_bank_checked_spectrum.talas.json",
             "Cloud Bank Checked Spectrum", "renderable"),
            ("examples/c112_cloud_bank_element_range_cascade_delay.talas.json",
             "Cloud Bank Element Range Cascade Delay", "renderable"),
            ("examples/c113_grain_cloud_dynamic_controls.talas.json",
             "Grain Cloud Dynamic Controls", "renderable"),
            ("examples/c114_chopper_dynamic_durations.talas.json",
             "Chopper Dynamic Durations", "renderable"),
            ("examples/c115_function_generator_trigger_duration.talas.json",
             "Function Generator Trigger Duration", "renderable"),
            ("examples/c116_multislope_connected_controls.talas.json",
             "Multislope Connected Controls", "renderable"),
            ("examples/c117_multisegment_loop_connected_controls.talas.json",
             "Multisegment Loop Connected Controls", "renderable"),
            ("examples/c118_noise_modes_connected_controls.talas.json",
             "Noise Modes Connected Controls", "renderable"),
            ("examples/c119_absolute_value_connected_magnitude.talas.json",
             "Absolute Value Connected Magnitude", "renderable"),
            ("examples/c11_disk_cache_capture.talas.json",
             "Bounded Disk Cache capture and playback", "renderable"),
            ("examples/c120_arctan_connected_curve.talas.json",
             "ArcTan Connected Curve", "renderable"),
            ("examples/c121_equality_connected_comparison.talas.json",
             "Equality Connected Comparison", "renderable"),
            ("examples/c122_set_range_connected_affine_mapping.talas.json",
             "Set Range Connected Affine Mapping", "renderable"),
            ("examples/c123_product_connected_finite_multiplication.talas.json",
             "Product Connected Finite Multiplication", "renderable"),
            ("examples/c124_difference_connected_finite_subtraction.talas.json",
             "Difference Connected Finite Subtraction", "renderable"),
            ("examples/c125_sqrt_magnitude_connected_saturated_norm.talas.json",
             "Sqrt Magnitude Connected Saturated Norm", "renderable"),
            ("examples/c126_vca_connected_finite_gain.talas.json",
             "VCA Connected Finite Gain", "renderable"),
            ("examples/c127_constant_finite_event_value_broadcast.talas.json",
             "Constant Finite Event Value Broadcast", "renderable"),
            ("examples/c128_add_finite_event_value_sum.talas.json",
             "Add Finite Event Value Sum", "renderable"),
            ("examples/c129_mixer_checked_finite_weighted_sum.talas.json",
             "Mixer Checked Finite Weighted Sum", "renderable"),
            ("examples/c12_portable_processor_placement.talas.json",
             "Portable Processor Placement", "renderable"),
            ("examples/c130_level_checked_finite_stereo_gain.talas.json",
             "Level Checked Finite Stereo Gain", "renderable"),
            ("examples/c131_channel_crosser_checked_finite_matrix.talas.json",
             "Channel Crosser Checked Finite Matrix", "renderable"),
            ("examples/c132_channel_join_checked_finite_dual_lane.talas.json",
             "Channel Join Checked Finite Dual Lane", "renderable"),
            ("examples/c133_channeller_checked_finite_live_selectors.talas.json",
             "Channeller Checked Finite Live Selectors", "renderable"),
            ("examples/c134_pan_checked_finite_live_controls.talas.json",
             "Pan Checked Finite Live Controls", "renderable"),
            ("examples/c135_crossfade_checked_finite_live_controls.talas.json",
             "Crossfade Checked Finite Live Controls", "renderable"),
            ("examples/c136_stereo_mix2_checked_live_controls.talas.json",
             "Stereo Mix 2 Checked Live Controls", "renderable"),
            ("examples/c137_stereo_mix4_checked_live_controls.talas.json",
             "Stereo Mix 4 Checked Live Controls", "renderable"),
            ("examples/c138_audio_output_exact_stereo_terminal.talas.json",
             "Audio Output Exact Stereo Terminal", "renderable"),
            ("examples/c139_averaging_low_pass_filter_dynamic_window.talas.json",
             "Averaging Low Pass Filter Dynamic Window", "renderable"),
            ("examples/c13_bounded_disk_recorder.talas.json",
             "Bounded Disk Recorder", "renderable"),
            ("examples/c140_delay_with_feedback_dynamic_controls.talas.json",
             "Delay With Feedback Dynamic Controls", "renderable"),
            ("examples/c141_set_duration_bounded_window.talas.json",
             "Set Duration Bounded Window", "renderable"),
            ("examples/c142_warped_time_index_scheduled_morph.talas.json",
             "Warped Time Index Scheduled Morph", "renderable"),
            ("examples/c143_amplitude_follower_scheduled_controls.talas.json",
             "Amplitude Follower Scheduled Controls", "renderable"),
            ("examples/c144_peak_detector_scheduled_controls.talas.json",
             "Peak Detector Scheduled Controls", "renderable"),
            ("examples/c145_rms_squared_scheduled_controls.talas.json",
             "RMS Squared Scheduled Controls", "renderable"),
            ("examples/c146_running_max_scheduled_reset.talas.json",
             "Running Max Scheduled Reset", "renderable"),
            ("examples/c147_running_min_scheduled_reset_baseline.talas.json",
             "Running Min Scheduled Reset Baseline", "renderable"),
            ("examples/c148_triggered_sample_and_hold_scheduled_edges.talas.json",
             "Triggered Sample And Hold Scheduled Edges", "renderable"),
            ("examples/c149_triggered_table_read_paired_schedules.talas.json",
             "Triggered Table Read Paired Schedules", "renderable"),
            ("examples/c14_bounded_analog_sequencer.talas.json",
             "Bounded Analog Sequencer", "renderable"),
            ("examples/c150_function_generator_scheduled_controls.talas.json",
             "Function Generator Scheduled Controls", "renderable"),
            ("examples/c151_multislope_scheduled_controls.talas.json",
             "Multislope Scheduled Controls", "renderable"),
            ("examples/c152_multisegment_scheduled_loop_controls.talas.json",
             "Multisegment Scheduled Loop Controls", "renderable"),
            ("examples/c153_graphical_envelope_scheduled_rate_controls.talas.json",
             "Graphical Envelope Scheduled Rate Controls", "renderable"),
            ("examples/c154_adsr_scheduled_typed_gate_controls.talas.json",
             "ADSR Scheduled Typed Gate Controls", "renderable"),
            ("examples/c155_oscillator_scheduled_phase_controls.talas.json",
             "Oscillator Scheduled Phase Controls", "renderable"),
            ("examples/c156_chopper_scheduled_cycle_controls.talas.json",
             "Chopper Scheduled Cycle Controls", "renderable"),
            ("examples/c157_analog_sequencer_scheduled_controls.talas.json",
             "Analog Sequencer Scheduled Controls", "renderable"),
            ("examples/c158_ar_scheduled_controls.talas.json",
             "AR Scheduled Controls", "renderable"),
            ("examples/c159_analysis_filter_scheduled_controls.talas.json",
             "Analysis Filter Scheduled Controls", "renderable"),
            ("examples/c15_stereo_routing_utilities.talas.json",
             "Stereo Routing Utilities", "renderable"),
            ("examples/c160_phase_shift_by_90_scheduled_frequency.talas.json",
             "Phase Shift By 90 Scheduled Frequency", "renderable"),
            ("examples/c161_single_side_band_rm_scheduled_quadrature.talas.json",
             "Single Side Band RM Scheduled Quadrature", "renderable"),
            ("examples/c162_sos_oscillators_static_spectrum_sine_bank.talas.json",
             "SOS Oscillators Static Spectrum Sine Bank", "renderable"),
            ("examples/c163_low_pass_gate_scheduled_control.talas.json",
             "Low-Pass Gate Scheduled Control", "renderable"),
            ("examples/c164_morph_filter_scheduled_controls.talas.json",
             "Morph Filter Scheduled Controls", "renderable"),
            ("examples/c165_quad_function_generator_scheduled_controls.talas.json",
             "Quad Function Generator Scheduled Controls", "renderable"),
            ("examples/c166_probability_chaos_generator_scheduled_controls.talas.json",
             "Probability Chaos Generator Scheduled Controls", "renderable"),
            ("examples/c167_stereo_dispersive_resonator_scheduled_controls.talas.json",
             "Stereo Dispersive Resonator Scheduled Controls", "renderable"),
            ("examples/c168_plucked_waveguide_scheduled_controls.talas.json",
             "Plucked Waveguide Scheduled Controls", "renderable"),
            ("examples/c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json",
             "Dynamic Breakpoint Oscillator Scheduled Controls", "renderable"),
            ("examples/c16_shelving_presence_filters.talas.json",
             "Shelving and Presence Filters", "renderable"),
            ("examples/c170_wave_terrain_oscillator_scheduled_controls.talas.json",
             "Wave Terrain Oscillator Scheduled Controls", "renderable"),
            ("examples/c171_scanned_mass_oscillator_scheduled_controls.talas.json",
             "Scanned Mass Oscillator Scheduled Controls", "renderable"),
            ("examples/c172_diffuse_spectral_stretch_scheduled_controls.talas.json",
             "Diffuse Spectral Stretch Scheduled Controls", "renderable"),
            ("examples/c173_dual_morph_oscillator_scheduled_controls.talas.json",
             "Dual Morph Oscillator Scheduled Controls", "renderable"),
            ("examples/c174_logic_scheduled_fallbacks.talas.json",
             "Logic Scheduled Fallbacks", "renderable"),
            ("examples/c175_annotation_passthrough.talas.json",
             "Annotation Pass-Through", "renderable"),
            ("examples/c17_checked_biquad_family.talas.json",
             "Checked Biquad Family", "renderable"),
            ("examples/c18_bounded_static_fir_family.talas.json",
             "Bounded Static FIR Family", "renderable"),
            ("examples/c191_filter_scheduled_controls.talas.json",
             "Filter Scheduled Controls", "renderable"),
            ("examples/c19_checked_vcf_tpt.talas.json",
             "Checked Four-Pole TPT VCF", "renderable"),
            ("examples/c200_polyphonic_midi_mapper_input.talas.json",
             "Polyphonic MIDI Mapper Input", "renderable"),
            ("examples/c201_triggered_global_controller_override.talas.json",
             "Triggered Global Controller Override", "renderable"),
            ("examples/c20_bounded_harmonic_resonator.talas.json",
             "Bounded Harmonic Resonator", "renderable"),
            ("examples/c21_bounded_dynamic_range_controller.talas.json",
             "Bounded Dynamic Range Controller", "renderable"),
            ("examples/c22_checked_quad_oscillator.talas.json",
             "Checked Quad Oscillator", "renderable"),
            ("examples/c23_bounded_overlapping_mixer.talas.json",
             "Bounded Overlapping Mixer", "renderable"),
            ("examples/c24_bounded_oscillator_bank.talas.json",
             "Bounded Oscillator Bank", "renderable"),
            ("examples/c25_bounded_formant_bank.talas.json",
             "Bounded Formant Bank", "renderable"),
            ("examples/c26_bounded_formant_bank_oscillator.talas.json",
             "Bounded Formant Bank Oscillator", "renderable"),
            ("examples/c27_bounded_spectrum_modifier.talas.json",
             "Bounded Spectrum Modifier", "renderable"),
            ("examples/c28_paired_stereo_output4.talas.json",
             "Paired Stereo Output 4", "renderable"),
            ("examples/c28_paired_stereo_output8.talas.json",
             "Paired Stereo Output 8", "renderable"),
            ("examples/c29_bounded_synthetic_spectrum_from_sounds.talas.json",
             "Bounded Synthetic Spectrum From Sounds", "renderable"),
            ("examples/c30_bounded_warped_time_index.talas.json",
             "Bounded Warped Time Index", "renderable"),
            ("examples/c31_bounded_spectrum_on_disk.talas.json",
             "Bounded Spectrum On Disk", "renderable"),
            ("examples/c32_bounded_ga_oscillators.talas.json",
             "Bounded GA Oscillators", "renderable"),
            ("examples/c33_bounded_generic_source_ram_sample.talas.json",
             "Bounded Generic Source RAM Sample", "renderable"),
            ("examples/c35_bounded_multisample.talas.json",
             "Bounded Multisample", "renderable"),
            ("examples/c36_bounded_key_mapped_multisample.talas.json",
             "Bounded Key-Mapped Multisample", "renderable"),
            ("examples/c37_bounded_multifile_disk_player.talas.json",
             "Bounded Multifile Disk Player", "renderable"),
            ("examples/c38_bounded_memory_writer.talas.json",
             "Bounded Memory Writer Monitor", "renderable"),
            ("examples/c39_bounded_raw_midi_output.talas.json",
             "Bounded Raw MIDI Output", "renderable"),
            ("examples/c40_bounded_midi_controller_output.talas.json",
             "Bounded MIDI Controller Output", "renderable"),
            ("examples/c41_bounded_midi_note_output.talas.json",
             "Bounded MIDI Note Output", "renderable"),
            ("examples/c42_bounded_midi_file_echo.talas.json",
             "Bounded MIDI File Echo", "renderable"),
            ("examples/c43_bounded_global_controller_output.talas.json",
             "Bounded Global Controller Output", "renderable"),
            ("examples/c44_dynamic_control_utilities.talas.json",
             "Dynamic Control Utilities", "renderable"),
            ("examples/c45_dynamic_slew_times.talas.json",
             "Dynamic Slew Times", "renderable"),
            ("examples/c46_dynamic_clock_parameters.talas.json",
             "Dynamic Clock Parameters", "renderable"),
            ("examples/c47_dynamic_clock_divider_parameters.talas.json",
             "Dynamic Clock Divider Parameters", "renderable"),
            ("examples/c48_dynamic_global_controller_fallback.talas.json",
             "Dynamic Global Controller Fallback", "renderable"),
            ("examples/c49_live_channeller_selectors.talas.json",
             "Live Channeller Selectors", "renderable"),
            ("examples/c50_live_spectrum_modifier_selectors.talas.json",
             "Live Spectrum Modifier Selectors", "renderable"),
            ("examples/c51_live_spectrum_modifier_seed.talas.json",
             "Live Spectrum Modifier Seed", "renderable"),
            ("examples/c52_live_spectrum_modifier_hysteresis.talas.json",
             "Live Spectrum Modifier Hysteresis", "renderable"),
            ("examples/c53_spectrum_modifier_probability_stream.talas.json",
             "Spectrum Modifier Probability Stream", "renderable"),
            ("examples/c54_spectrum_modifier_track_probability_profile.talas.json",
             "Spectrum Modifier Track Probability Profile", "renderable"),
            ("examples/c55_spectrum_modifier_remainder_probability_profile.talas.json",
             "Spectrum Modifier Remainder Probability Profile", "renderable"),
            ("examples/c56_spectrum_on_disk_frame_scan.talas.json",
             "Spectrum On Disk Frame Scan", "renderable"),
            ("examples/c57_spectrum_voiced_unvoiced_transition.talas.json",
             "Spectrum Voiced Unvoiced Transition", "renderable"),
            ("examples/c58_spectrum_in_ram_normalized_time_index.talas.json",
             "Spectrum In RAM Normalized TimeIndex", "renderable"),
            ("examples/c59_spectrum_in_ram_default_analysis_metadata.talas.json",
             "Spectrum In RAM Default Analysis Metadata", "renderable"),
            ("examples/c60_spectrum_on_disk_default_analysis_metadata.talas.json",
             "Spectrum On Disk Default Analysis Metadata", "renderable"),
            ("examples/c61_sum_of_sines_default_analysis_frequencies.talas.json",
             "Sum Of Sines Default Analysis Frequencies", "renderable"),
            ("examples/c62_sum_of_sines_controlled_time_frames.talas.json",
             "Sum Of Sines Controlled-Time Frames", "renderable"),
            ("examples/c63_sum_of_sines_automatic_gate_loop.talas.json",
             "Sum Of Sines Automatic Gate Loop", "renderable"),
            ("examples/c64_synthetic_spectrum_array_prefix.talas.json",
             "Synthetic Spectrum Array Prefix", "renderable"),
            ("examples/c65_synthetic_spectrum_static_expressions.talas.json",
             "Synthetic Spectrum Static Expressions", "renderable"),
            ("examples/c66_threshold_hysteresis_gate.talas.json",
             "Threshold Hysteresis Gate", "renderable"),
            ("examples/c67_time_control_half_speed.talas.json",
             "Time Control Half Speed", "renderable"),
            ("examples/c68_time_stopper_sample_hold.talas.json",
             "Time Stopper Sample Hold", "renderable"),
            ("examples/c69_time_offset_delay_line.talas.json",
             "Time Offset Delay Line", "renderable"),
            ("examples/c70_triggered_sample_and_hold_edges.talas.json",
             "Triggered Sample And Hold Edges", "renderable"),
            ("examples/c71_triggered_table_read_gate_reset.talas.json",
             "Triggered Table Read Gate Reset", "renderable"),
            ("examples/c72_tunable_vocoder_bank_reset.talas.json",
             "Tunable Vocoder Bank Reset", "renderable"),
            ("examples/c73_two_formant_pair_reset.talas.json",
             "Two Formant Pair Reset", "renderable"),
            ("examples/c74_sound_variable_selector_reset.talas.json",
             "Sound Variable Selector Reset", "renderable"),
            ("examples/c75_fixed_vocoder_bank_reset.talas.json",
             "Fixed Vocoder Bank Reset", "renderable"),
            ("examples/c76_vocoder_channel_bank_reset.talas.json",
             "Vocoder Channel Bank Reset", "renderable"),
            ("examples/c77_wait_until_buffered_resume.talas.json",
             "Wait Until Delayed Input", "renderable"),
            ("examples/c78_warped_time_index_restart.talas.json",
             "Warped Time Index Restart", "renderable"),
            ("examples/c79_waveshaper_polynomial_curve.talas.json",
             "Waveshaper Polynomial Curve", "renderable"),
            ("examples/c80_iterated_waveshaper_feedback.talas.json",
             "Iterated Waveshaper Feedback", "renderable"),
            ("examples/c81_multiplying_waveshaper_product.talas.json",
             "Multiplying Waveshaper Product", "renderable"),
            ("examples/c82_input_output_characteristic_curve.talas.json",
             "Input Output Characteristic Curve", "renderable"),
            ("examples/c83_scale_and_offset_affine_control.talas.json",
             "Scale and Offset Affine Control", "renderable"),
            ("examples/c84_interpolate_dual_lane_control.talas.json",
             "Interpolate Dual Lane Control", "renderable"),
            ("examples/c85_running_max_reset_control.talas.json",
             "Running Max Reset Control", "renderable"),
            ("examples/c86_running_min_reset_baseline.talas.json",
             "Running Min Reset Baseline", "renderable"),
            ("examples/c87_rms_squared_envelope.talas.json",
             "RMS Squared Envelope", "renderable"),
            ("examples/c88_amplitude_follower_envelope.talas.json",
             "Amplitude Follower Envelope", "renderable"),
            ("examples/c89_peak_detector_envelope.talas.json",
             "Peak Detector Envelope", "renderable"),
            ("examples/c90_frequency_tracker_history.talas.json",
             "Frequency Tracker History", "renderable"),
            ("examples/c91_ar_envelope_state.talas.json",
             "AR Envelope State", "renderable"),
            ("examples/c92_adsr_typed_gate_state.talas.json",
             "ADSR Gate State", "renderable"),
            ("examples/c93_pulse_train_phase_state.talas.json",
             "Pulse Train Phase State", "renderable"),
            ("examples/c94_pulse_generator_phase_state.talas.json",
             "Pulse Generator Phase State", "renderable"),
            ("examples/c95_oscillator_phase_state.talas.json",
             "Oscillator Phase State", "renderable"),
            ("examples/c96_lfo_phase_transport.talas.json",
             "LFO Phase and Transport", "renderable"),
            ("examples/c97_ramp_trigger_transport.talas.json",
             "Ramp Trigger and Transport", "renderable"),
            ("examples/c98_sample_and_hold_clock_transport.talas.json",
             "Sample And Hold Clock and Transport", "renderable"),
            ("examples/c99_samples_from_disk_single_step_transport.talas.json",
             "Samples From Disk Single Step Transport", "renderable"),
            ("examples/demos/arpeggiator.talas.json",
             "Arpeggiator", "renderable"),
            ("examples/demos/auto_filter.talas.json",
             "Auto-Filter", "renderable"),
            ("examples/demos/bus_compressor.talas.json",
             "Bus Compressor", "renderable"),
            ("examples/demos/chorus.talas.json",
             "Stereo Chorus", "renderable"),
            ("examples/demos/drawbar_organ.talas.json",
             "Drawbar Organ", "renderable"),
            ("examples/demos/drum_kit.talas.json",
             "Drum Kit", "renderable"),
            ("examples/demos/flanger.talas.json",
             "Flanger", "renderable"),
            ("examples/demos/fm_electric_piano.talas.json",
             "FM Electric Piano", "renderable"),
            ("examples/demos/frequency_shifter.talas.json",
             "Frequency Shifter", "renderable"),
            ("examples/demos/granular_freeze.talas.json",
             "Granular Freeze", "renderable"),
            ("examples/demos/granular_synth.talas.json",
             "Granular Synth", "renderable"),
            ("examples/demos/hall_reverb.talas.json",
             "Hall Reverb", "renderable"),
            ("examples/demos/modal_mallets.talas.json",
             "Modal Mallets", "renderable"),
            ("examples/demos/morph_lead.talas.json",
             "Morph Lead", "renderable"),
            ("examples/demos/phaser.talas.json",
             "Phaser", "renderable"),
            ("examples/demos/plucked_string.talas.json",
             "Plucked String", "renderable"),
            ("examples/demos/poly_pad.talas.json",
             "Poly Pad", "renderable"),
            ("examples/demos/ring_modulator.talas.json",
             "Ring Modulator", "renderable"),
            ("examples/demos/spectral_blur.talas.json",
             "Spectral Blur", "renderable"),
            ("examples/demos/stereo_delay.talas.json",
             "Stereo Delay", "renderable"),
            ("examples/demos/sub_bass.talas.json",
             "Sub Bass", "renderable"),
            ("examples/demos/tape_saturator.talas.json",
             "Tape Saturator", "renderable"),
            ("examples/demos/terrain_drone.talas.json",
             "Terrain Drone", "renderable"),
            ("examples/demos/vocoder.talas.json",
             "Vocoder", "renderable"),
            ("examples/m3_live_spectral_analysis.talas.json",
             "Live spectral analysis", "renderable"),
            ("examples/m3_matrix4_output4.talas.json",
             "Matrix4 to Output4", "renderable"),
            ("examples/m3_native_fft_round_trip.talas.json",
             "Native complex STFT round-trip", "renderable"),
            ("examples/m3_packed_fft_round_trip.talas.json",
             "Historical packed-Audio FFT round-trip", "renderable"),
            ("examples/m3_streaming_health.talas.json",
             "Immutable streaming health", "renderable"),
            ("examples/m4_missing_component_preservation.talas.json",
             "Missing-component preservation", "preserved-unavailable"),
            ("examples/m4_sample_map_instrument.talas.json",
             "Sample-map instrument", "renderable"),
            ("examples/m4_snapshot_performance.talas.json",
             "Snapshot performance", "validation-only"),
            ("examples/m4_state_layers.talas.json",
             "Portable state layers", "validation-only"),
            ("examples/oscillator.talas.json",
             "Minimal oscillator", "renderable"),
            ("examples/reusable_feedback_help.talas.json",
             "Reusable Sound feedback", "validation-only"),
            ("examples/sample_text_table.talas.json",
             "Text-backed sample table", "renderable"),
        ]
        expected_paths = [path for path, _, _ in expected_examples]
        self.assertEqual(set(policy), {
            "schemaVersion",
            "format",
            "catalogPath",
            "markdownOutput",
            "jsonOutput",
            "examples",
            "allowedUnregisteredTypeIds",
        })
        self.assertEqual(policy["schemaVersion"], 1)
        self.assertEqual(policy["format"], "talas.node-help-policy")
        self.assertEqual(
            policy["catalogPath"],
            "docs/coverage/builtin-node-catalog-v2.json",
        )
        self.assertEqual(
            policy["markdownOutput"],
            "docs/help/builtin-node-reference.md",
        )
        self.assertEqual(
            policy["jsonOutput"],
            "docs/help/node-example-index.json",
        )
        self.assertEqual([
            (example["path"], example["title"], example["expectedOutcome"])
            for example in policy["examples"]
        ], expected_examples)
        self.assertEqual(
            {example["expectedOutcome"] for example in policy["examples"]},
            {"renderable", "validation-only", "preserved-unavailable"},
        )
        self.assertEqual(
            policy["allowedUnregisteredTypeIds"],
            ["org.talas.example.spectral-colour"],
        )

        markdown = (
            ROOT / policy["markdownOutput"]
        ).read_text(encoding="utf-8")
        index = json.loads((
            ROOT / policy["jsonOutput"]
        ).read_text(encoding="utf-8"))
        examples_readme = (
            ROOT / "examples/README.md"
        ).read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        generator = (
            ROOT / "tools/report_node_help.py"
        ).read_text(encoding="utf-8")
        self.assertIn("Generated by tools/report_node_help.py", markdown)
        self.assertIn("not a parity claim", markdown)
        self.assertIn("placeholder", markdown)
        self.assertEqual(index["schemaVersion"], 1)
        self.assertEqual(index["generatedBy"], "tools/report_node_help.py")
        self.assertIn("semantic parity", index["nonClaims"])
        self.assertEqual(len(index["nodes"]), 364)
        self.assertEqual(
            [example["path"] for example in index["examples"]],
            expected_paths,
        )
        self.assertEqual(
            examples_readme.count("../docs/help/builtin-node-reference.md"),
            1,
        )
        self.assertIn(
            'default=pathlib.Path("docs/governance/node-help-policy.json")',
            generator,
        )
        self.assertIn("sys.dont_write_bytecode = True", generator)
        self.assertNotIn(".glob(", generator)
        self.assertIn(
            "docs/help/builtin-node-reference.md text eol=lf",
            attributes,
        )
        self.assertIn(
            "docs/help/node-example-index.json text eol=lf",
            attributes,
        )
        def install_rules_in(source):
            return [
                " ".join(block.split())
                for block in re.findall(
                    r"(?is)(?<![A-Za-z0-9_])install\s*\((?P<body>[^()]*)\)",
                    source,
                )
            ]

        install_rules = install_rules_in(cmake)
        self.assertEqual(install_rules, [
            "TARGETS talas_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}",
            "TARGETS talas_workbench BUNDLE DESTINATION . RUNTIME DESTINATION "
            "${CMAKE_INSTALL_BINDIR}",
            "FILES \"${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/${sound_library_file}\" "
            "DESTINATION \"${CMAKE_INSTALL_BINDIR}/resources/"
            "TalasSoundLibrary/${sound_library_directory}\"",
            "FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR}",
            "DIRECTORY docs/coverage/ DESTINATION "
            "${CMAKE_INSTALL_DOCDIR}/coverage PATTERN \".DS_Store\" EXCLUDE"
        ])
        for spelling in (
            "install(DIRECTORY docs/ DESTINATION share/docs)",
            "INSTALL ( DIRECTORY docs/ DESTINATION share/docs )",
        ):
            with self.subTest(spelling=spelling):
                self.assertEqual(
                    install_rules_in(cmake + "\n" + spelling)[-1],
                    "DIRECTORY docs/ DESTINATION share/docs",
                )

    def test_e8_sound_library_manifest_is_bound_to_original_example_authority(self):
        manifest = json.loads((
            ROOT / "examples" / "sound-library-manifest.json"
        ).read_text(encoding="utf-8"))
        index = json.loads((
            ROOT / "docs" / "help" / "node-example-index.json"
        ).read_text(encoding="utf-8"))
        self.assertEqual(set(manifest), {
            "schemaVersion", "authority", "entries",
        })
        self.assertEqual(manifest["schemaVersion"], 1)
        self.assertIn("repository-original", manifest["authority"])
        self.assertEqual(len(manifest["entries"]), 202)
        expected_entry_keys = {
            "id", "projectPath", "title", "summary", "category", "tags",
            "author", "origin", "helpAnchor", "expectedOutcome",
            "definitionId", "actions",
        }
        paths = []
        ids = []
        for entry in manifest["entries"]:
            self.assertEqual(set(entry), expected_entry_keys)
            self.assertEqual(entry["origin"], "repository-original")
            self.assertTrue(entry["projectPath"].startswith("examples/"))
            self.assertNotIn("..", entry["projectPath"])
            self.assertTrue(entry["helpAnchor"].startswith("#"))
            self.assertIn("openExample", entry["actions"])
            self.assertIn("openHelp", entry["actions"])
            self.assertEqual(len(entry["actions"]), len(set(entry["actions"])))
            if entry["expectedOutcome"] != "renderable":
                self.assertNotIn("audition", entry["actions"])
            if "useInProject" in entry["actions"]:
                self.assertIsInstance(entry["definitionId"], str)
                self.assertTrue(entry["definitionId"])
            else:
                self.assertIsNone(entry["definitionId"])
            paths.append(entry["projectPath"])
            ids.append(entry["id"])
            demo = entry["projectPath"].startswith("examples/demos/")
            self.assertEqual(entry["category"] == "Demos", demo)
            self.assertEqual("demo" in entry["tags"], demo)
            if demo:
                kinds = {"effect", "instrument"} & set(entry["tags"])
                self.assertEqual(len(kinds), 1)
                self.assertEqual(
                    "audition" in entry["actions"], kinds == {"instrument"}
                )
        self.assertEqual(len(paths), len(set(paths)))
        demo_paths = {
            path for path in paths if path.startswith("examples/demos/")
        }
        self.assertEqual(
            demo_paths,
            {
                path.relative_to(ROOT).as_posix()
                for path in (ROOT / "examples" / "demos").glob("*.talas.json")
            },
        )
        self.assertEqual(len(demo_paths), 24)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(
            set(paths),
            {example["path"] for example in index["examples"]},
        )
        indexed_outcomes = {
            example["path"]: example["expectedOutcome"]
            for example in index["examples"]
        }
        self.assertEqual(
            {
                entry["projectPath"]: entry["expectedOutcome"]
                for entry in manifest["entries"]
            },
            indexed_outcomes,
        )

        model = (
            ROOT / "src" / "app" / "SoundLibraryModel.cpp"
        ).read_text(encoding="utf-8")
        header = (
            ROOT / "include" / "talas" / "app" / "SoundLibraryModel.h"
        ).read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        bundle_verifier = (
            ROOT / "cmake" / "VerifySoundLibraryBundle.cmake"
        ).read_text(encoding="utf-8")
        bundle_match = re.search(
            r"set\(TALAS_SOUND_LIBRARY_BUNDLE_FILES\s+(.*?)\s+\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(bundle_match)
        bundled_paths = set(bundle_match.group(1).split())
        expected_bundled_paths = set(paths) | {
            "examples/sound-library-manifest.json",
            "examples/DiskCache/c11-disk-cache-loop.wav",
            "examples/resources/c18-re-kernel.txt",
            "examples/resources/c31-spectrum-frame.txt",
            "examples/resources/c32-ga-analysis-a.txt",
            "examples/resources/c32-ga-analysis-b.txt",
            "examples/resources/c33-generic-source-sample.txt",
            "examples/resources/c42-midi-file-echo.mid",
            "examples/resources/c56-spectrum-frames.txt",
            "examples/resources/c57-voiced-unvoiced-frames.txt",
            "examples/resources/c58-spectrum-in-ram-frames.txt",
            "examples/resources/c59-spectrum-in-ram-defaults.txt",
            "examples/resources/c60-spectrum-on-disk-defaults.txt",
            "examples/resources/c61-sum-of-sines-defaults.txt",
            "examples/resources/c62-sum-of-sines-controlled-frames.txt",
            "examples/resources/c62-sum-of-sines-time-index.txt",
            "examples/resources/c63-sum-of-sines-automatic-frames.txt",
            "examples/resources/c63-sum-of-sines-gate.txt",
            "examples/resources/c71-triggered-table-read.txt",
            "examples/resources/c149-triggered-table-read.txt",
            "examples/resources/c150-function-generator-envelope.txt",
            "examples/resources/c170-wave-terrain.txt",
            "examples/resources/c172-diffuse-spectral-source.txt",
            "examples/resources/c80-iterated-waveshaper-table.txt",
            "examples/resources/c81-multiplying-waveshaper-table.txt",
            "examples/resources/c95-oscillator-cyclic-table.txt",
            "examples/resources/c99-samples-from-disk-single-step.txt",
            "examples/resources/c102-sos-oscillators-wave.txt",
            "examples/resources/hit.txt",
            "examples/resources/m3-stream-source.wav",
            "examples/README.md",
            "examples/demos/README.md",
            "docs/help/node-example-index.json",
            "docs/help/builtin-node-reference.md",
        }
        self.assertEqual(bundled_paths, expected_bundled_paths)
        self.assertTrue(demo_paths <= bundled_paths)
        self.assertIn(
            '"${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/examples/demos"', cmake
        )
        self.assertRegex(
            cmake,
            r"NAME talas_sound_library_bundle_tests\s+"
            r"COMMAND talas_sound_library_model_tests\s+"
            r'--bundle-root "\$\{TALAS_SOUND_LIBRARY_BUNDLE_ROOT\}"',
        )
        self.assertIn("loadSoundLibraryManifest", header)
        self.assertIn("node-example-index.json", model)
        self.assertIn("parseProjectDocument", model)
        self.assertIn("talas_sound_library_model_tests", cmake)
        for bundled_path in expected_bundled_paths:
            self.assertIn(bundled_path, cmake)
        self.assertIn("TALAS_SOUND_LIBRARY_BUNDLE_FILES", cmake)
        self.assertIn("TalasSoundLibrary", cmake)
        self.assertIn("copy_if_different", cmake)
        self.assertIn("TALAS_SOUND_LIBRARY_BUNDLE_STAMP", cmake)
        self.assertIn("${TALAS_SOUND_LIBRARY_BUNDLE_SOURCE_FILES}", cmake)
        self.assertIn(
            'target_sources(talas_workbench PRIVATE "${TALAS_SOUND_LIBRARY_BUNDLE_STAMP}")',
            cmake,
        )
        self.assertIn("VerifySoundLibraryBundle.cmake", cmake)
        self.assertIn("talas_sound_library_bundle_verify ALL", cmake)
        self.assertIn("TALAS_SOUND_LIBRARY_INSTALL_RELATIVE_ROOT", cmake)
        self.assertIn("Installed Sound Library verification failed", cmake)
        self.assertIn(
            'FILES "${TALAS_SOUND_LIBRARY_BUNDLE_ROOT}/${sound_library_file}"',
            cmake,
        )
        self.assertIn("actual_files STREQUAL expected_files", bundle_verifier)
        self.assertIn("TALAS_SOUND_LIBRARY_REFERENCE_ROOT", bundle_verifier)
        self.assertIn("file(SHA256", bundle_verifier)
        self.assertIn("IS_SYMLINK", bundle_verifier)
        self.assertNotIn("copy_directory\n                \"${CMAKE_CURRENT_SOURCE_DIR}/examples", cmake)

    def test_macos_notarization_is_hardened_repeatable_and_secret_free(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        script = (
            ROOT / "tools" / "notarize_macos_release.sh"
        ).read_text(encoding="utf-8")
        entitlements = (
            ROOT / "cmake" / "macos" / "TalasWorkbench.entitlements"
        ).read_text(encoding="utf-8")
        helper_entitlements = (
            ROOT
            / "cmake"
            / "macos"
            / "TalasComponentHost.entitlements"
        ).read_text(encoding="utf-8")
        probe_entitlements = (
            ROOT
            / "cmake"
            / "macos"
            / "TalasComponentProbe.entitlements"
        ).read_text(encoding="utf-8")
        runtime_entitlements = (
            ROOT
            / "cmake"
            / "macos"
            / "TalasComponentRuntime.entitlements"
        ).read_text(encoding="utf-8")

        self.assertIn("com.apple.security.device.audio-input", entitlements)
        self.assertNotIn("com.apple.security.get-task-allow", entitlements)
        self.assertIn("com.apple.security.app-sandbox", helper_entitlements)
        self.assertEqual(helper_entitlements.count("<key>"), 1)
        self.assertIn("com.apple.security.app-sandbox", probe_entitlements)
        self.assertIn(
            "com.apple.security.cs.disable-library-validation",
            probe_entitlements,
        )
        self.assertEqual(probe_entitlements.count("<key>"), 2)
        self.assertIn("com.apple.security.app-sandbox", runtime_entitlements)
        self.assertIn(
            "com.apple.security.cs.disable-library-validation",
            runtime_entitlements,
        )
        self.assertEqual(runtime_entitlements.count("<key>"), 2)
        for declaration in (
            "add_executable(talas_component_activation_helper MACOSX_BUNDLE",
            "talas_sign_component_activation_helper("
            "talas_component_activation_helper)",
            "add_dependencies(talas_workbench talas_component_activation_helper)",
            'LINK_DEPENDS\n                '
            '"$<TARGET_FILE:talas_component_activation_helper>"',
            "TalasComponentRuntime.xpc",
        ):
            self.assertIn(declaration, cmake)
        for forbidden in (
            "network.client",
            "network.server",
            "files.user-selected",
            "files.downloads",
            "files.bookmarks",
            "device.",
            "get-task-allow",
            "temporary-exception",
            "inherit",
        ):
            self.assertNotIn(forbidden, helper_entitlements)
            self.assertNotIn(forbidden, runtime_entitlements)
        self.assertIn("--options runtime", script)
        self.assertIn("--timestamp", script)
        self.assertIn('HELPER="$APP/Contents/XPCServices/', script)
        self.assertIn('HELPER_ENTITLEMENTS=', script)
        self.assertIn(
            'PROBE_HELPER="$APP/Contents/XPCServices/TalasComponentProbe.xpc"',
            script,
        )
        self.assertIn('PROBE_HELPER_ENTITLEMENTS=', script)
        self.assertIn(
            'RUNTIME_HELPER="$APP/Contents/XPCServices/'
            'TalasComponentRuntime.xpc"',
            script,
        )
        self.assertIn('RUNTIME_HELPER_ENTITLEMENTS=', script)
        helper_sign = script.index('"$HELPER"\n')
        probe_helper_sign = script.index('"$PROBE_HELPER"\n', helper_sign)
        runtime_helper_sign = script.index(
            '"$RUNTIME_HELPER"\n',
            probe_helper_sign,
        )
        app_sign = script.index('"$APP"\n', runtime_helper_sign)
        template_sign = script.index('"$template"\n')
        self.assertLess(template_sign, app_sign)
        self.assertIn(
            'PLUGIN_TEMPLATES="$APP/Contents/Resources/PluginTemplates"',
            script,
        )
        self.assertIn("--juce-path", script)
        self.assertLess(helper_sign, app_sign)
        self.assertLess(probe_helper_sign, app_sign)
        self.assertLess(runtime_helper_sign, app_sign)
        self.assertIn(
            'codesign --verify --strict --verbose=4 "$HELPER"',
            script,
        )
        self.assertIn(
            'codesign -d --xml --entitlements "$HELPER_ENTITLEMENTS_ACTUAL"',
            script,
        )
        self.assertIn(
            'codesign --verify --strict --verbose=4 "$PROBE_HELPER"',
            script,
        )
        self.assertIn(
            'codesign -d --xml --entitlements '
            '"$PROBE_HELPER_ENTITLEMENTS_ACTUAL"',
            script,
        )
        self.assertIn(
            'codesign --verify --strict --verbose=4 "$RUNTIME_HELPER"',
            script,
        )
        self.assertIn(
            'codesign -d --xml --entitlements '
            '"$RUNTIME_HELPER_ENTITLEMENTS_ACTUAL"',
            script,
        )
        self.assertIn("'com\\.apple\\.security\\.app-sandbox'", script)
        self.assertIn(
            "'com\\.apple\\.security\\.cs\\.disable-library-validation'",
            script,
        )
        self.assertIn("notarytool submit", script)
        self.assertIn('stapler staple "$APP"', script)
        self.assertIn('stapler staple "$DMG"', script)
        self.assertIn("spctl", script)
        self.assertIn('--keychain-profile "$NOTARY_PROFILE"', script)
        self.assertIn('"$(basename "$DMG")"', script)
        self.assertIn('"$(basename "$PORTABLE_ZIP")" >SHA256SUMS', script)
        self.assertNotIn("jq ", script)
        self.assertNotIn("AC_PASSWORD", script)
        self.assertNotIn("52RY4VB4F6", script)
        self.assertNotIn("Woodsign", script)


if __name__ == "__main__":
    unittest.main()
