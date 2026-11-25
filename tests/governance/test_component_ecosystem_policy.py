import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from check_component_ecosystem_policy import evaluate_policy


def approved_policy() -> dict:
    return {
        "schema": "talas.component-ecosystem-policy",
        "version": 4,
        "status": "isolated-probe-authorized",
        "publicSdk": False,
        "bundleDiscovery": False,
        "bundleInstallation": False,
        "dynamicLoading": "disabled",
        "componentActivation": "disabled",
        "inProcessUserCode": False,
        "userCodeExecution": "preserve-only",
        "isolationBackend": "macos-product-fixture",
        "probeBackend": "not-implemented",
        "probeLoadingPolicy": "macos-one-shot-xpc-v1",
        "activationBackend": "not-implemented",
        "activationLoadingPolicy": "disabled",
        "activationProfile": "disabled",
        "activationSurface": "disabled",
        "componentResourceAccess": "disabled",
        "componentReplacement": "disabled",
        "replacementSurface": "disabled",
        "trustPersistence": False,
        "registryPromotion": False,
        "projectFormatActivation": False,
        "workbenchActivation": False,
        "cliActivation": False,
    }


def active_probe_policy() -> dict:
    payload = approved_policy()
    payload.update(
        {
            "status": "isolated-probe-only",
            "dynamicLoading": "macos-isolated-probe-only",
            "userCodeExecution": "probe-only",
            "probeBackend": "macos-one-shot-xpc-v1",
        }
    )
    return payload


def activation_authorized_policy() -> dict:
    payload = active_probe_policy()
    payload.update(
        {
            "status": "isolated-activation-foundation-authorized",
            "componentActivation":
                "private-macos-stateless-audio-v1-authorized",
            "activationLoadingPolicy": "macos-sealed-runtime-xpc-v1",
            "activationProfile": "macos-stateless-realtime-audio-v1",
            "activationSurface": "private-runtime-api-only",
        }
    )
    return payload


def sealed_runtime_policy() -> dict:
    payload = activation_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-sandbox-only",
            "activationBackend": "macos-sealed-runtime-prewarm-xpc-v1",
        }
    )
    return payload


def runtime_validation_policy() -> dict:
    payload = activation_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-runtime-validation-only",
            "dynamicLoading":
                "macos-isolated-probe-and-runtime-validation-only",
            "userCodeExecution":
                "private-macos-isolated-runtime-validation-only",
            "activationBackend": "macos-sealed-runtime-validation-xpc-v1",
        }
    )
    return payload


def active_activation_policy() -> dict:
    payload = activation_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-foundation",
            "dynamicLoading": "macos-isolated-probe-and-runtime-only",
            "componentActivation": "private-macos-stateless-audio-v1",
            "userCodeExecution": "private-macos-isolated-runtime-only",
            "activationBackend": "macos-sealed-runtime-xpc-v1",
        }
    )
    return payload


def resource_authorized_policy() -> dict:
    payload = active_activation_policy()
    payload.update(
        {
            "version": 4,
            "status": "isolated-activation-immutable-resource-authorized",
            "componentActivation":
                "private-macos-stateless-audio-table-resource-v1-authorized",
            "activationProfile":
                "macos-stateless-realtime-audio-table-resource-v1",
            "componentResourceAccess":
                "single-table-snapshot-v1-authorized",
        }
    )
    return payload


def resource_validation_policy() -> dict:
    payload = resource_authorized_policy()
    payload.update(
        {
            "status":
                "isolated-activation-immutable-resource-validation-only",
            "componentActivation":
                "private-macos-stateless-audio-table-resource-v1-validation-only",
            "componentResourceAccess":
                "single-table-snapshot-v1-validation-only",
        }
    )
    return payload


def active_resource_policy() -> dict:
    payload = resource_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-immutable-resource",
            "componentActivation":
                "private-macos-stateless-audio-table-resource-v1",
            "componentResourceAccess": "single-table-snapshot-v1",
        }
    )
    return payload


def binary_resource_authorized_policy() -> dict:
    payload = active_resource_policy()
    payload.update(
        {
            "version": 5,
            "status":
                "isolated-activation-immutable-binary-resource-authorized",
            "componentActivation":
                "private-macos-stateless-audio-table-or-binary-resource-v1-authorized",
            "activationProfile":
                "macos-stateless-realtime-audio-table-or-binary-resource-v1",
            "componentResourceAccess":
                "single-table-or-binary-snapshot-v1-authorized",
        }
    )
    return payload


def binary_resource_validation_policy() -> dict:
    payload = binary_resource_authorized_policy()
    payload.update(
        {
            "status":
                "isolated-activation-immutable-binary-resource-validation-only",
            "componentActivation":
                "private-macos-stateless-audio-table-or-binary-resource-v1-validation-only",
            "componentResourceAccess":
                "single-table-or-binary-snapshot-v1-validation-only",
        }
    )
    return payload


def active_binary_resource_policy() -> dict:
    payload = binary_resource_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-immutable-binary-resource",
            "componentActivation":
                "private-macos-stateless-audio-table-or-binary-resource-v1",
            "componentResourceAccess":
                "single-table-or-binary-snapshot-v1",
        }
    )
    return payload


def audio_stream_authorized_policy() -> dict:
    payload = active_binary_resource_policy()
    payload.update(
        {
            "version": 6,
            "status": "isolated-activation-bounded-audio-stream-authorized",
            "componentActivation":
                "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1-authorized",
            "activationProfile":
                "macos-stateless-realtime-audio-table-binary-or-audio-stream-resource-v1",
            "componentResourceAccess":
                "single-table-binary-or-audio-stream-snapshot-v1-authorized",
        }
    )
    return payload


def audio_stream_validation_policy() -> dict:
    payload = audio_stream_authorized_policy()
    payload.update(
        {
            "status":
                "isolated-activation-bounded-audio-stream-validation-only",
            "componentActivation":
                "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1-validation-only",
            "componentResourceAccess":
                "single-table-binary-or-audio-stream-snapshot-v1-validation-only",
        }
    )
    return payload


def active_audio_stream_policy() -> dict:
    payload = audio_stream_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-bounded-audio-stream",
            "componentActivation":
                "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1",
            "componentResourceAccess":
                "single-table-binary-or-audio-stream-snapshot-v1",
        }
    )
    return payload


def multiple_resource_authorized_policy() -> dict:
    payload = active_audio_stream_policy()
    payload.update(
        {
            "version": 7,
            "status":
                "isolated-activation-multiple-immutable-resources-authorized",
            "componentActivation":
                "private-macos-stateless-audio-multiple-immutable-resources-v1-authorized",
            "activationProfile":
                "macos-stateless-realtime-audio-multiple-immutable-resources-v1",
            "componentResourceAccess":
                "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1-authorized",
        }
    )
    return payload


def multiple_resource_validation_policy() -> dict:
    payload = multiple_resource_authorized_policy()
    payload.update(
        {
            "status":
                "isolated-activation-multiple-immutable-resources-validation-only",
            "componentActivation":
                "private-macos-stateless-audio-multiple-immutable-resources-v1-validation-only",
            "componentResourceAccess":
                "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1-validation-only",
        }
    )
    return payload


def active_multiple_resource_policy() -> dict:
    payload = multiple_resource_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-multiple-immutable-resources",
            "componentActivation":
                "private-macos-stateless-audio-multiple-immutable-resources-v1",
            "componentResourceAccess":
                "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1",
        }
    )
    return payload


def replacement_authorized_policy() -> dict:
    payload = active_multiple_resource_policy()
    payload.update(
        {
            "version": 8,
            "status": "isolated-activation-explicit-replacement-authorized",
            "componentReplacement":
                "explicit-local-replacement-admission-v1-authorized",
            "replacementSurface":
                "private-preview-confirmation-and-stage-admission-only",
        }
    )
    return payload


def replacement_validation_policy() -> dict:
    payload = replacement_authorized_policy()
    payload.update(
        {
            "status":
                "isolated-activation-explicit-replacement-validation-only",
            "componentReplacement":
                "explicit-owner-confirmed-local-replacement-v1-validation-only",
            "replacementSurface":
                "tests-only-workbench-preview-and-controller",
        }
    )
    return payload


def active_replacement_policy() -> dict:
    payload = replacement_authorized_policy()
    payload.update(
        {
            "status": "isolated-activation-explicit-replacement",
            "componentReplacement":
                "explicit-owner-confirmed-local-replacement-v1",
            "replacementSurface": "owner-selected-workbench-only",
            "activationSurface":
                "private-runtime-and-owner-selected-workbench-replacement-only",
            "workbenchActivation": True,
        }
    )
    return payload


class ComponentEcosystemPolicyTests(unittest.TestCase):
    def make_repo(self) -> tuple[tempfile.TemporaryDirectory, pathlib.Path]:
        temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(temporary.name)
        (root / "docs/governance").mkdir(parents=True)
        (root / "include/talas/runtime").mkdir(parents=True)
        (root / "src/runtime/component_isolation").mkdir(parents=True)
        (root / "cmake/macos").mkdir(parents=True)
        (root / "README.md").write_text(
            "Executable user components are not available.\n",
            encoding="utf-8",
        )
        (root / "CMakeLists.txt").write_text(
            "project(Talas)\n"
            "if(APPLE)\n"
            "add_library(talas_component_isolation_support "
            "src/runtime/ComponentIsolationProductAudioWorker.cpp)\n"
            "set(TALAS_COMPONENT_ISOLATION_HELPER_ENTITLEMENTS "
            "cmake/macos/TalasComponentHost.entitlements)\n"
            "add_library(talas_component_isolation_macos_host STATIC Host.cpp)\n"
            "add_executable(talas_component_isolation_helper MACOSX_BUNDLE Helper.cpp)\n"
            "talas_sign_component_isolation_helper(talas_component_isolation_helper)\n"
            "target_link_libraries(talas_workbench PRIVATE talas_component_isolation_macos_host)\n"
            "add_dependencies(talas_workbench talas_component_isolation_helper)\n"
            "set_property(TARGET talas_workbench APPEND PROPERTY LINK_DEPENDS "
            '"$<TARGET_FILE:talas_component_isolation_helper>")\n'
            "copy_directory TalasComponentHost.xpc\n"
            "if(TALAS_BUILD_TESTS)\n"
            "add_executable(talas_component_isolation_product_audio_worker_tests test.cpp)\n"
            "add_executable(talas_component_isolation_macos_host_integration_tests test.cpp)\n"
            "add_executable(talas_component_isolation_sandbox_integration_tests test.cpp)\n"
            "TalasComponentHostUnsandboxed.xpc\n"
            "add_executable(talas_component_isolation_watchdog_integration_tests test.cpp)\n"
            "endif()\n"
            "endif()\n",
            encoding="utf-8",
        )
        (root / "include/talas/runtime/ComponentTrustPolicy.h").write_text(
            "#if defined(__APPLE__) && defined(__MACH__) && TARGET_OS_OSX\n"
            "inline constexpr bool componentIsolationBackendAvailableV1 = true;\n"
            "#else\n"
            "inline constexpr bool componentIsolationBackendAvailableV1 = false;\n"
            "#endif\n"
            "class ComponentIsolationEvidenceV1 {\n"
            "private:\n"
            "friend class ComponentIsolationMacOSHost;\n"
            "};\n"
            "class ComponentProbeEvidence {\n"
            "private:\n"
            "friend class ComponentBundleProbeEvidenceFactory;\n"
            "};\n",
            encoding="utf-8",
        )
        (root / "include/talas/runtime/ComponentIsolationMacOSHost.h").write_text(
            "ComponentIsolationEvidenceV1 isolationEvidence() const noexcept;\n",
            encoding="utf-8",
        )
        (root / "include/talas/runtime/ComponentIsolationAudioDataPlane.h").write_text(
            "inline constexpr unsigned "
            "componentIsolationTransportPipelineLatencyQuantaV1 = 1;\n",
            encoding="utf-8",
        )
        (root / "src/runtime/component_isolation/ComponentIsolationLiveEvidence.h").write_text(
            "struct ComponentIsolationLiveEvidenceStateV1 {};\n",
            encoding="utf-8",
        )
        (root / "cmake/macos/TalasComponentHost.entitlements").write_text(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<plist version=\"1.0\"><dict>"
            "<key>com.apple.security.app-sandbox</key><true/>"
            "</dict></plist>\n",
            encoding="utf-8",
        )
        (root / "src/runtime/ComponentTrustPolicy.cpp").write_text(
            "// policy evaluator only\n",
            encoding="utf-8",
        )
        self.write_policy(root, approved_policy())
        return temporary, root

    def write_policy(self, root: pathlib.Path, payload: dict) -> None:
        (root / "docs/governance/component-ecosystem-policy.json").write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )

    def enable_probe_backend(self, root: pathlib.Path) -> None:
        self.write_policy(root, active_probe_policy())
        loader = (
            root
            / "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp"
        )
        loader.parent.mkdir(parents=True, exist_ok=True)
        loader.write_text(
            '#include <dlfcn.h>\n'
            'void probe() { auto* image = dlopen("sealed", RTLD_LOCAL); '
            'dlsym(image, "talas_component_bundle_entry_v1"); }\n',
            encoding="utf-8",
        )
        (loader.parent / "ComponentBundleProbeHelperMain.cpp").write_text(
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        (loader.parent / "ComponentBundleProbeMacOSCoordinator.cpp").write_text(
            "void coordinate_component_probe() {}\n",
            encoding="utf-8",
        )
        (loader.parent / "ComponentBundleProbeMaterialization.cpp").write_text(
            "void materialize_component_probe() {}\n",
            encoding="utf-8",
        )
        (root / "cmake/macos/TalasComponentProbe.entitlements").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '<key>com.apple.security.cs.disable-library-validation</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "set(TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS "
            "cmake/macos/TalasComponentProbe.entitlements)\n"
            "add_executable(talas_component_bundle_probe_helper MACOSX_BUNDLE\n"
            "  src/runtime/component_probe/macos/ComponentBundleProbeHelperMain.cpp\n"
            "  src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp)\n"
            "add_library(talas_component_probe_macos_coordinator STATIC\n"
            "  src/runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.cpp)\n"
            "add_library(talas_component_probe_macos_materialization STATIC\n"
            "  src/runtime/component_probe/macos/ComponentBundleProbeMaterialization.cpp)\n"
            "add_executable(talas_component_bundle_probe_macos_coordinator_tests\n"
            "  tests/runtime/ComponentBundleProbeMacOSCoordinatorTests.cpp)\n"
            "add_executable(talas_component_probe_coordinator_clean_tests\n"
            "  tests/runtime/ComponentBundleProbeMacOSCoordinatorIntegrationTests.cpp)\n"
            "add_executable(talas_component_probe_trust_composition_tests\n"
            "  tests/runtime/ComponentBundleProbeTrustCompositionIntegrationTests.cpp)\n"
            "talas_sign_component_probe_helper(talas_component_bundle_probe_helper)\n"
            "set_target_properties(talas_component_bundle_probe_helper PROPERTIES "
            "OUTPUT_NAME TalasComponentProbe BUNDLE_EXTENSION xpc)\n"
            "add_dependencies(talas_workbench talas_component_bundle_probe_helper)\n"
            "copy_directory TalasComponentProbe.xpc\n",
            encoding="utf-8",
        )

    def authorize_activation_foundation(self, root: pathlib.Path) -> None:
        self.write_policy(root, activation_authorized_policy())

    def enable_sealed_runtime_backend(self, root: pathlib.Path) -> None:
        self.enable_probe_backend(root)
        self.write_policy(root, sealed_runtime_policy())
        macos_root = root / "src/runtime/component_activation/macos"
        macos_root.mkdir(parents=True, exist_ok=True)
        (macos_root / "ComponentActivationHelperMain.cpp").write_text(
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        (macos_root / "ComponentActivationMacOSHost.cpp").write_text(
            "void seal() { sealRuntimeBeforeComponentBytes(); }\n",
            encoding="utf-8",
        )
        (root / "cmake/macos/TalasComponentRuntime.entitlements").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "set(TALAS_COMPONENT_RUNTIME_HELPER_ENTITLEMENTS "
            "cmake/macos/TalasComponentRuntime.entitlements)\n"
            "add_library(talas_component_activation_macos_host STATIC\n"
            "  src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp)\n"
            "add_executable(talas_component_activation_helper MACOSX_BUNDLE\n"
            "  src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp)\n"
            "talas_sign_component_activation_helper(talas_component_activation_helper)\n"
            "set_target_properties(talas_component_activation_helper PROPERTIES "
            "OUTPUT_NAME TalasComponentRuntime BUNDLE_EXTENSION xpc)\n"
            "target_link_libraries(talas_workbench PRIVATE "
            "talas_component_activation_macos_host)\n"
            "add_dependencies(talas_workbench talas_component_activation_helper)\n"
            "copy_directory TalasComponentRuntime.xpc\n"
            "add_executable(talas_component_activation_sandbox_integration_tests test.cpp)\n",
            encoding="utf-8",
        )

    def enable_activation_backend(self, root: pathlib.Path) -> None:
        self.enable_probe_backend(root)
        self.write_policy(root, active_activation_policy())
        activation_root = root / "src/runtime/component_activation"
        macos_root = activation_root / "macos"
        macos_root.mkdir(parents=True, exist_ok=True)
        (activation_root / "ComponentActivationAuthorization.cpp").write_text(
            "class ComponentActivationAuthorizationFactory {};\n",
            encoding="utf-8",
        )
        (activation_root / "ComponentActivationProtocol.cpp").write_text(
            "void validateComponentActivationProtocol() {}\n",
            encoding="utf-8",
        )
        (activation_root / "ComponentActivationAudioDataPlane.cpp").write_text(
            "constexpr unsigned componentActivationPipelineLatencyBlocksV1 = 1;\n",
            encoding="utf-8",
        )
        (activation_root / "ComponentActivationWorker.cpp").write_text(
            "void runComponentActivationCanary() {}\n",
            encoding="utf-8",
        )
        (macos_root / "ComponentActivationHelperMain.cpp").write_text(
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        (macos_root / "ComponentActivationMacOSHost.cpp").write_text(
            "void activate() {\n"
            "  sealRuntimeBeforeComponentBytes();\n"
            "  consumeComponentActivationAuthorization();\n"
            "  sendFrozenComponentEntryBytes();\n"
            "}\n",
            encoding="utf-8",
        )
        (macos_root / "ComponentActivationLoader.cpp").write_text(
            '#include <dlfcn.h>\n'
            'void activate() { auto* image = dlopen("sealed", RTLD_LOCAL); '
            'dlsym(image, "talas_component_bundle_entry_v1"); }\n',
            encoding="utf-8",
        )
        (root / "cmake/macos/TalasComponentRuntime.entitlements").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '<key>com.apple.security.cs.disable-library-validation</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "set(TALAS_COMPONENT_RUNTIME_HELPER_ENTITLEMENTS "
            "cmake/macos/TalasComponentRuntime.entitlements)\n"
            "add_library(talas_component_activation_authorization STATIC\n"
            "  src/runtime/component_activation/ComponentActivationAuthorization.cpp)\n"
            "add_library(talas_component_activation_protocol STATIC\n"
            "  src/runtime/component_activation/ComponentActivationProtocol.cpp\n"
            "  src/runtime/component_activation/ComponentActivationAudioDataPlane.cpp)\n"
            "add_library(talas_component_activation_macos_host STATIC\n"
            "  src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp)\n"
            "add_executable(talas_component_activation_helper MACOSX_BUNDLE\n"
            "  src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp\n"
            "  src/runtime/component_activation/macos/ComponentActivationLoader.cpp\n"
            "  src/runtime/component_activation/ComponentActivationWorker.cpp)\n"
            "talas_sign_component_activation_helper(talas_component_activation_helper)\n"
            "set_target_properties(talas_component_activation_helper PROPERTIES "
            "OUTPUT_NAME TalasComponentRuntime BUNDLE_EXTENSION xpc)\n"
            "target_link_libraries(talas_workbench PRIVATE "
            "talas_component_activation_macos_host "
            "talas_component_activation_authorization)\n"
            "add_dependencies(talas_workbench talas_component_activation_helper)\n"
            "copy_directory TalasComponentRuntime.xpc\n"
            "add_executable(talas_component_activation_authorization_tests test.cpp)\n"
            "add_executable(talas_component_activation_protocol_tests test.cpp)\n"
            "add_executable(talas_component_activation_macos_host_integration_tests test.cpp)\n"
            "add_executable(talas_component_activation_sandbox_integration_tests test.cpp)\n"
            "add_executable(talas_component_activation_deadline_integration_tests test.cpp)\n"
            "add_executable(talas_component_activation_quarantine_tests test.cpp)\n"
            "add_executable(talas_component_activation_transient_graph_tests test.cpp)\n",
            encoding="utf-8",
        )

    def enable_active_resource_backend(self, root: pathlib.Path) -> None:
        self.enable_activation_backend(root)
        self.write_policy(root, active_resource_policy())
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentResourceContract() {\n"
            "  contract.resources = std::move(resources);\n"
            "}\n",
            encoding="utf-8",
        )
        runtime = root / "src/runtime/Runtime.cpp"
        runtime.write_text(
            "bool exactIsolatedResourceContract();\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "add_executable("
            "talas_component_activation_resource_"
            "transient_graph_macos_integration_tests test.cpp)\n",
            encoding="utf-8",
        )

    def enable_active_binary_resource_declarations(
        self, root: pathlib.Path
    ) -> None:
        loader = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        )
        loader.write_text(
            loader.read_text(encoding="utf-8")
            + "auto binaryView = TALAS_KERNEL_ABI_RESOURCE_BINARY;\n",
            encoding="utf-8",
        )
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentBinaryResourceContract();\n",
            encoding="utf-8",
        )
        runtime = root / "src/runtime/Runtime.cpp"
        runtime.write_text(
            runtime.read_text(encoding="utf-8")
            + "bool exactIsolatedBinaryResourceContract();\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "add_executable("
            "talas_component_activation_binary_resource_"
            "transient_graph_macos_integration_tests test.cpp)\n",
            encoding="utf-8",
        )

    def add_audio_stream_contract_declarations(
        self, root: pathlib.Path
    ) -> None:
        protocol = root / "include/talas/runtime/ComponentActivationProtocol.h"
        protocol.write_text(
            "struct ComponentActivationAudioStreamGrantV1 {};\n"
            "struct TalasKernelAbiImmutableAudioStreamViewV3 {};\n"
            "struct TalasKernelAbiHostServicesV3 {};\n",
            encoding="utf-8",
        )

    def add_audio_stream_transfer_declarations(
        self, root: pathlib.Path
    ) -> None:
        loader = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        )
        loader.write_text(
            loader.read_text(encoding="utf-8")
            + "void mapComponentActivationAudioSourceSharedMemory();\n"
            + "class ComponentActivationAudioStreamV1;\n"
            + "void createComponentActivationXpcAudioSourceSharedMemoryV1();\n"
            + "void materializeComponentActivationPrivateAudioSourceV1();\n",
            encoding="utf-8",
        )
        envelope = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.cpp"
        )
        envelope.write_text(
            "void materializeComponentActivationPrivateAudioSourceV1();\n",
            encoding="utf-8",
        )

    def add_audio_stream_runtime_declarations(
        self, root: pathlib.Path
    ) -> None:
        stream = (
            root
            / "src/runtime/component_activation/ComponentActivationAudioStream.cpp"
        )
        stream.write_text(
            "class ComponentActivationAudioStream {};\n"
            "void resolveComponentActivationAudioStream();\n"
            "void readComponentActivationAudioStream();\n",
            encoding="utf-8",
        )

    def add_audio_stream_graph_declarations(
        self, root: pathlib.Path
    ) -> None:
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentAudioStreamContract();\n",
            encoding="utf-8",
        )
        runtime = root / "src/runtime/Runtime.cpp"
        runtime.write_text(
            runtime.read_text(encoding="utf-8")
            + "bool exactIsolatedAudioStreamContract();\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "add_executable(talas_component_activation_audio_stream_"
            "transient_graph_macos_integration_tests test.cpp)\n",
            encoding="utf-8",
        )

    def enable_active_audio_stream_declarations(
        self, root: pathlib.Path
    ) -> None:
        self.add_audio_stream_contract_declarations(root)
        self.add_audio_stream_transfer_declarations(root)
        self.add_audio_stream_runtime_declarations(root)
        self.add_audio_stream_graph_declarations(root)

    def add_multiple_resource_contract_declarations(
        self, root: pathlib.Path
    ) -> None:
        protocol = root / "include/talas/runtime/ComponentActivationProtocol.h"
        protocol.write_text(
            protocol.read_text(encoding="utf-8")
            + "constexpr unsigned maximumComponentActivationResourceSetMembersV1 = 8;\n"
            + "constexpr unsigned maximumComponentActivationResourceSetAudioStreamsV1 = 2;\n"
            + "constexpr unsigned long long maximumComponentActivationResourceSetSourceBytesV1 = 64U * 1024U * 1024U;\n"
            + "struct ComponentActivationResourceSetGrantV1 {};\n",
            encoding="utf-8",
        )
        kernel_abi = root / "include/talas/runtime/KernelAbi.h"
        kernel_abi.parent.mkdir(parents=True, exist_ok=True)
        kernel_abi.write_text(
            "struct TalasKernelAbiHostServicesV4 {};\n",
            encoding="utf-8",
        )

    def add_multiple_resource_transfer_declarations(
        self, root: pathlib.Path
    ) -> None:
        envelope = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.cpp"
        )
        envelope.write_text(
            envelope.read_text(encoding="utf-8")
            + "void createComponentActivationXpcResourceSetV1();\n"
            + "void materializeComponentActivationResourceSetV1();\n",
            encoding="utf-8",
        )

    def add_multiple_resource_runtime_declarations(
        self, root: pathlib.Path
    ) -> None:
        loader = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        )
        loader.write_text(
            loader.read_text(encoding="utf-8")
            + "class ComponentActivationPreparedResourceSetV1 {};\n",
            encoding="utf-8",
        )

    def add_multiple_resource_graph_declarations(
        self, root: pathlib.Path
    ) -> None:
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentResourceSetContract();\n",
            encoding="utf-8",
        )
        runtime = root / "src/runtime/Runtime.cpp"
        runtime.write_text(
            runtime.read_text(encoding="utf-8")
            + "bool exactIsolatedComponentResourceSetContract();\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "add_executable("
            "talas_component_activation_multiple_resource_"
            "transient_graph_macos_integration_tests test.cpp)\n",
            encoding="utf-8",
        )

    def add_multiple_resource_graph_validation_declarations(
        self, root: pathlib.Path
    ) -> None:
        macro = "TALAS_COMPONENT_ACTIVATION_MULTIPLE_RESOURCE_GRAPH_TESTING"
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + f"#if defined({macro})\n"
            + "void authorIsolatedComponentResourceSetContract();\n"
            + "#endif\n",
            encoding="utf-8",
        )
        runtime = root / "src/runtime/Runtime.cpp"
        runtime.write_text(
            runtime.read_text(encoding="utf-8")
            + f"#if defined({macro})\n"
            + "bool exactIsolatedComponentResourceSetContract();\n"
            + "#endif\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "target_compile_definitions(talas_runtime PUBLIC "
            + f"{macro}=1)\n"
            + "target_compile_definitions(talas_component_activation_macos_host "
            + f"PUBLIC {macro}=1)\n"
            + "if(TALAS_BUILD_TESTS)\n"
            + "add_executable(talas_component_activation_multiple_resource_"
            + "transient_graph_macos_integration_tests test.cpp)\n"
            + "target_compile_definitions("
            + "talas_component_activation_multiple_resource_"
            + "transient_graph_macos_integration_tests PRIVATE "
            + "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH=1)\n"
            + "endif()\n",
            encoding="utf-8",
        )

    def enable_active_multiple_resource_declarations(
        self, root: pathlib.Path
    ) -> None:
        self.add_multiple_resource_contract_declarations(root)
        self.add_multiple_resource_transfer_declarations(root)
        self.add_multiple_resource_runtime_declarations(root)
        self.add_multiple_resource_graph_declarations(root)
        helper = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp"
        )
        helper.write_text(
            helper.read_text(encoding="utf-8")
            + "void use(ComponentActivationPreparedResourceSetV1);\n",
            encoding="utf-8",
        )

    def add_replacement_preview_declarations(
        self, root: pathlib.Path
    ) -> None:
        replacement_root = root / "src/runtime/component_bundle"
        replacement_root.mkdir(parents=True, exist_ok=True)
        (replacement_root / "ComponentBundleReplacementPolicy.h").write_text(
            "struct ComponentBundleReplacementPreview {\n"
            "  bool executable() const noexcept { return false; }\n"
            "};\n"
            "void previewComponentBundleReplacement();\n"
            "void calculateProjectPreservedComponentDigest();\n",
            encoding="utf-8",
        )
        (replacement_root / "ComponentBundleReplacementPolicy.cpp").write_text(
            "// pure replacement preview policy only\n",
            encoding="utf-8",
        )
        (replacement_root / "ComponentBundleReplacementConfirmation.h").write_text(
            "class ComponentBundleReplacementReceipt {\n"
            "  void tryConsume();\n"
            "};\n"
            "class ComponentBundleReplacementConsumptionLease {};\n"
            "class ComponentBundleReplacementConfirmationStore {};\n",
            encoding="utf-8",
        )
        (replacement_root / "ComponentBundleReplacementConfirmation.cpp").write_text(
            "// opaque one-use confirmation authority only\n",
            encoding="utf-8",
        )
        bridge_header = root / "include/talas/runtime/ProjectRuntimeBridge.h"
        bridge_header.write_text(
            "void compileAndStageIsolatedComponentReplacement();\n",
            encoding="utf-8",
        )
        (root / "src/runtime/ProjectRuntimeBridge.cpp").write_text(
            "void compileAndStageIsolatedComponentReplacement() {}\n",
            encoding="utf-8",
        )

    def add_replacement_controller_declarations(
        self, root: pathlib.Path
    ) -> None:
        replacement_root = root / "src/runtime/component_bundle"
        (replacement_root / "ComponentBundleReplacementController.h").write_text(
            "class ComponentBundleReplacementController {};\n",
            encoding="utf-8",
        )
        (replacement_root / "ComponentBundleReplacementController.cpp").write_text(
            "// bounded tests-only replacement controller\n",
            encoding="utf-8",
        )
        workbench_root = root / "src/app/workbench"
        workbench_root.mkdir(parents=True, exist_ok=True)
        (workbench_root / "WorkbenchComponentReplacementPanel.h").write_text(
            "class ComponentReplacementPanel {};\n",
            encoding="utf-8",
        )
        (workbench_root / "WorkbenchComponentReplacementPanel.cpp").write_text(
            "// accessible tests-only Workbench preview\n",
            encoding="utf-8",
        )
        (workbench_root / "WorkbenchComponentReplacement.cpp").write_text(
            "void beginComponentReplacement();\n",
            encoding="utf-8",
        )
        (workbench_root / "WorkbenchInspectorPanel.h").write_text(
            "void action() { PreservedNodeAction::Replace; }\n",
            encoding="utf-8",
        )
        (workbench_root / "WorkbenchCommands.cpp").write_text(
            "void action() { PreservedNodeAction::Replace; }\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "if(TALAS_BUILD_TESTS)\n"
            "add_library(talas_component_bundle_replacement_controller "
            "src/runtime/component_bundle/ComponentBundleReplacementController.cpp)\n"
            "endif()\n"
            "if(APPLE AND TALAS_BUILD_TESTS)\n"
            "target_link_libraries(talas_workbench_ui PRIVATE "
            "talas_component_bundle_replacement_controller)\n"
            "endif()\n",
            encoding="utf-8",
        )

    def promote_replacement_controller_to_active(
        self, root: pathlib.Path
    ) -> None:
        workbench = root / "src/app/workbench/WorkbenchComponentReplacement.cpp"
        workbench.write_text(
            "#if defined(TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT)\n"
            "void beginComponentReplacement() { approveCandidate(); confirm(); }\n"
            "#endif\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        validation = (
            "if(TALAS_BUILD_TESTS)\n"
            "add_library(talas_component_bundle_replacement_controller "
            "src/runtime/component_bundle/ComponentBundleReplacementController.cpp)\n"
            "endif()\n"
            "if(APPLE AND TALAS_BUILD_TESTS)\n"
            "target_link_libraries(talas_workbench_ui PRIVATE "
            "talas_component_bundle_replacement_controller)\n"
            "endif()\n"
        )
        active = (
            "add_library(talas_component_bundle_replacement_controller "
            "src/runtime/component_bundle/ComponentBundleReplacementController.cpp)\n"
            "target_compile_definitions(talas_component_bundle_replacement_controller "
            "PUBLIC TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT=1)\n"
            "if(TALAS_BUILD_TESTS)\n"
            "target_compile_definitions(talas_component_bundle_replacement_controller "
            "PUBLIC TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT=1)\n"
            "endif()\n"
            "if(APPLE)\n"
            "target_link_libraries(talas_workbench_ui PRIVATE "
            "talas_component_bundle_replacement_controller)\n"
            "endif()\n"
        )
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(validation, active),
            encoding="utf-8",
        )

    def test_current_closed_repository_policy_passes(self):
        self.assertEqual(evaluate_policy(ROOT), [])

    def test_activation_foundation_authorization_remains_probe_only(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)
        self.authorize_activation_foundation(root)

        self.assertEqual(evaluate_policy(root), [])

        payload = activation_authorized_policy()
        self.assertEqual(payload["dynamicLoading"], "macos-isolated-probe-only")
        self.assertEqual(payload["userCodeExecution"], "probe-only")
        self.assertEqual(payload["activationBackend"], "not-implemented")

    def test_active_activation_policy_accepts_two_exact_loader_boundaries(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)

        self.assertEqual(evaluate_policy(root), [])

    def test_step4_policy_explicitly_disables_component_resources(self):
        payload = active_activation_policy()
        self.assertEqual(payload["version"], 4)
        self.assertEqual(payload["componentResourceAccess"], "disabled")

    def test_step4_policy_rejects_resource_contract_declarations(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        contract = (
            root
            / "src/runtime/component_activation/ComponentActivationResourceContract.h"
        )
        contract.write_text(
            "struct ComponentActivationImmutableResourceGrantV1 {};\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("resource mediation is disabled" in error for error in errors),
            errors,
        )

    def test_closed_resource_policy_states_are_accepted_without_mixing(self):
        for policy in (
            resource_authorized_policy(),
            resource_validation_policy(),
            active_resource_policy(),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                if policy == active_resource_policy():
                    self.enable_active_resource_backend(root)
                else:
                    self.enable_activation_backend(root)
                    self.write_policy(root, policy)

                self.assertEqual(evaluate_policy(root), [])

        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        mixed = resource_authorized_policy()
        mixed["componentResourceAccess"] = "single-table-snapshot-v1"
        self.write_policy(root, mixed)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("without mixing" in error for error in errors),
            errors,
        )

    def test_table_only_policy_rejects_binary_resource_declarations(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        protocol = root / "include/talas/runtime/ComponentActivationProtocol.h"
        protocol.write_text(
            "auto kind = ComponentActivationImmutableResourceKindV1::Binary;\n"
            "auto format = "
            "ComponentActivationImmutableResourceElementFormatV1::RawBytes;\n",
            encoding="utf-8",
        )
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "add_executable(talas_component_activation_binary_resource_"
            "fixture_tests test.cpp)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("binary resource contract requires" in error for error in errors),
            errors,
        )

    def test_every_earlier_policy_rejects_binary_contract_transfer_and_graph(self):
        earlier_policies = (
            approved_policy(),
            active_probe_policy(),
            activation_authorized_policy(),
            sealed_runtime_policy(),
            runtime_validation_policy(),
            active_activation_policy(),
            resource_authorized_policy(),
            resource_validation_policy(),
            active_resource_policy(),
        )
        for policy in earlier_policies:
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.write_policy(root, policy)
                protocol = (
                    root / "include/talas/runtime/ComponentActivationProtocol.h"
                )
                protocol.write_text(
                    "auto kind = "
                    "ComponentActivationImmutableResourceKindV1::Binary;\n"
                    "auto format = "
                    "ComponentActivationImmutableResourceElementFormatV1::"
                    "RawBytes;\n",
                    encoding="utf-8",
                )
                loader = (
                    root
                    / "src/runtime/component_activation/macos/"
                    "ComponentActivationLoader.cpp"
                )
                loader.write_text(
                    loader.read_text(encoding="utf-8")
                    + "auto binaryView = TALAS_KERNEL_ABI_RESOURCE_BINARY;\n",
                    encoding="utf-8",
                )
                host = (
                    root
                    / "src/runtime/component_activation/macos/"
                    "ComponentActivationMacOSHost.cpp"
                )
                host.write_text(
                    host.read_text(encoding="utf-8")
                    + "void authorIsolatedComponentBinaryResourceContract();\n",
                    encoding="utf-8",
                )
                cmake = root / "CMakeLists.txt"
                cmake.write_text(
                    cmake.read_text(encoding="utf-8")
                    + "add_executable("
                    "talas_component_activation_binary_resource_"
                    "transient_graph_macos_integration_tests test.cpp)\n",
                    encoding="utf-8",
                )

                errors = evaluate_policy(root)

                self.assertTrue(
                    any(
                        "binary resource contract requires" in error
                        for error in errors
                    ),
                    errors,
                )
                self.assertTrue(
                    any(
                        "binary resource transfer requires" in error
                        for error in errors
                    ),
                    errors,
                )
                self.assertTrue(
                    any(
                        "binary resource graph activation" in error
                        for error in errors
                    ),
                    errors,
                )

    def test_closed_binary_resource_policy_states_are_accepted_without_mixing(self):
        for policy in (
            binary_resource_authorized_policy(),
            binary_resource_validation_policy(),
            active_binary_resource_policy(),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.write_policy(root, policy)
                if policy == active_binary_resource_policy():
                    self.enable_active_binary_resource_declarations(root)

                self.assertEqual(evaluate_policy(root), [])

        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        mixed = binary_resource_authorized_policy()
        mixed["componentResourceAccess"] = (
            "single-table-or-binary-snapshot-v1"
        )
        self.write_policy(root, mixed)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("without mixing" in error for error in errors),
            errors,
        )

    def test_binary_authorized_policy_allows_contract_but_rejects_transfer(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.write_policy(root, binary_resource_authorized_policy())
        protocol = root / "include/talas/runtime/ComponentActivationProtocol.h"
        protocol.write_text(
            "auto kind = ComponentActivationImmutableResourceKindV1::Binary;\n"
            "auto format = "
            "ComponentActivationImmutableResourceElementFormatV1::RawBytes;\n",
            encoding="utf-8",
        )

        self.assertEqual(evaluate_policy(root), [])

        loader = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        )
        loader.write_text(
            loader.read_text(encoding="utf-8")
            + "auto binaryView = TALAS_KERNEL_ABI_RESOURCE_BINARY;\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("binary resource transfer requires" in error for error in errors),
            errors,
        )

    def test_binary_validation_policy_allows_transfer_but_rejects_graph(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.write_policy(root, binary_resource_validation_policy())
        loader = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        )
        loader.write_text(
            loader.read_text(encoding="utf-8")
            + "auto binaryView = TALAS_KERNEL_ABI_RESOURCE_BINARY;\n",
            encoding="utf-8",
        )

        self.assertEqual(evaluate_policy(root), [])

        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentBinaryResourceContract();\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("binary resource graph activation" in error for error in errors),
            errors,
        )

    def test_closed_audio_stream_policy_states_are_accepted_without_mixing(self):
        for policy in (
            audio_stream_authorized_policy(),
            audio_stream_validation_policy(),
            active_audio_stream_policy(),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.write_policy(root, policy)
                if policy == active_audio_stream_policy():
                    self.enable_active_audio_stream_declarations(root)

                self.assertEqual(evaluate_policy(root), [])

        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        mixed = audio_stream_authorized_policy()
        mixed["componentResourceAccess"] = (
            "single-table-binary-or-audio-stream-snapshot-v1"
        )
        self.write_policy(root, mixed)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("without mixing" in error for error in errors),
            errors,
        )

    def test_closed_multiple_resource_policy_states_are_dormant_and_exact(self):
        for policy, declarations in (
            (
                multiple_resource_authorized_policy(),
                self.add_multiple_resource_contract_declarations,
            ),
            (
                multiple_resource_validation_policy(),
                lambda root: (
                    self.add_multiple_resource_contract_declarations(root),
                    self.add_multiple_resource_transfer_declarations(root),
                ),
            ),
            (
                active_multiple_resource_policy(),
                self.enable_active_multiple_resource_declarations,
            ),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.enable_active_audio_stream_declarations(root)
                self.write_policy(root, policy)
                declarations(root)

                self.assertEqual(evaluate_policy(root), [])

        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.add_multiple_resource_contract_declarations(root)
        mixed = multiple_resource_authorized_policy()
        mixed["componentResourceAccess"] = (
            "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1"
        )
        self.write_policy(root, mixed)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("without mixing" in error for error in errors),
            errors,
        )

    def test_replacement_preview_authorization_is_exact_and_non_executable(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.enable_active_multiple_resource_declarations(root)
        self.add_replacement_preview_declarations(root)
        self.write_policy(root, replacement_authorized_policy())

        self.assertEqual(evaluate_policy(root), [])

        self.write_policy(root, active_multiple_resource_policy())
        errors = evaluate_policy(root)
        self.assertTrue(
            any(
                "replacement-authorized policy" in error
                for error in errors
            ),
            errors,
        )

    def test_replacement_policy_states_do_not_mix_or_skip_dependencies(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.enable_active_multiple_resource_declarations(root)
        self.add_replacement_preview_declarations(root)

        mixed = replacement_authorized_policy()
        mixed["replacementSurface"] = "owner-selected-workbench-only"
        self.write_policy(root, mixed)
        errors = evaluate_policy(root)
        self.assertTrue(any("without mixing" in error for error in errors), errors)

        self.write_policy(root, replacement_validation_policy())
        errors = evaluate_policy(root)
        self.assertTrue(
            any("bounded controller" in error for error in errors),
            errors,
        )

        self.add_replacement_controller_declarations(root)
        self.assertEqual(evaluate_policy(root), [])

        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "target_link_libraries(talas_workbench PRIVATE "
            "talas_component_bundle_replacement_controller)\n",
            encoding="utf-8",
        )
        errors = evaluate_policy(root)
        self.assertTrue(
            any("tests-off Workbench" in error for error in errors),
            errors,
        )

        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "target_link_libraries(talas_workbench PRIVATE "
                "talas_component_bundle_replacement_controller)\n",
                "",
            ),
            encoding="utf-8",
        )
        controller = (
            root
            / "src/runtime/component_bundle/ComponentBundleReplacementController.h"
        )
        controller.write_text(
            controller.read_text(encoding="utf-8")
            + "#include <fstream>\n"
            "void persist(ComponentOwnerAuthorizationStore& store) {\n"
            "  std::ofstream out(\"authorization.json\");\n"
            "}\n",
            encoding="utf-8",
        )
        errors = evaluate_policy(root)
        self.assertTrue(
            any("must not use file" in error for error in errors),
            errors,
        )

    def test_active_replacement_requires_tests_off_product_linkage(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.enable_active_multiple_resource_declarations(root)
        self.add_replacement_preview_declarations(root)
        self.add_replacement_controller_declarations(root)
        self.promote_replacement_controller_to_active(root)
        self.write_policy(root, active_replacement_policy())
        self.assertEqual(evaluate_policy(root), [])

        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "if(APPLE)\n"
                "target_link_libraries(talas_workbench_ui PRIVATE "
                "talas_component_bundle_replacement_controller)\n",
                "if(APPLE AND TALAS_BUILD_TESTS)\n"
                "target_link_libraries(talas_workbench_ui PRIVATE "
                "talas_component_bundle_replacement_controller)\n",
            ),
            encoding="utf-8",
        )
        errors = evaluate_policy(root)
        self.assertTrue(
            any(
                "must not confine Workbench linkage" in error
                for error in errors
            ),
            errors,
        )

    def test_multiple_resource_states_require_their_exact_ordered_surface(self):
        for policy, expected in (
            (
                multiple_resource_authorized_policy(),
                "multiple-resource authorization requires",
            ),
            (
                multiple_resource_validation_policy(),
                "multiple-resource validation requires",
            ),
            (
                active_multiple_resource_policy(),
                "multiple-resource activation requires",
            ),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.enable_active_audio_stream_declarations(root)
                self.write_policy(root, policy)

                errors = evaluate_policy(root)

                self.assertTrue(any(expected in error for error in errors), errors)

    def test_multiple_resource_policy_rejects_budget_limit_drift(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.add_multiple_resource_contract_declarations(root)
        self.write_policy(root, multiple_resource_authorized_policy())
        protocol = root / "include/talas/runtime/ComponentActivationProtocol.h"
        protocol.write_text(
            protocol.read_text(encoding="utf-8").replace(
                "maximumComponentActivationResourceSetMembersV1 = 8",
                "maximumComponentActivationResourceSetMembersV1 = 9",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("exact 8/2/64 MiB aggregate limits" in error for error in errors),
            errors,
        )

    def test_multiple_resource_validation_allows_only_loader_runtime_seam(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.write_policy(root, multiple_resource_validation_policy())
        self.add_multiple_resource_contract_declarations(root)
        self.add_multiple_resource_transfer_declarations(root)
        self.add_multiple_resource_runtime_declarations(root)

        self.assertEqual(evaluate_policy(root), [])

        helper = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp"
        )
        helper.write_text(
            helper.read_text(encoding="utf-8")
            + "void use(ComponentActivationPreparedResourceSetV1);\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any(
                "multiple resource runtime execution requires" in error
                for error in errors
            ),
            errors,
        )

    def test_multiple_resource_validation_allows_only_guarded_graph_seam(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.enable_active_audio_stream_declarations(root)
        self.write_policy(root, multiple_resource_validation_policy())
        self.add_multiple_resource_contract_declarations(root)
        self.add_multiple_resource_transfer_declarations(root)
        self.add_multiple_resource_runtime_declarations(root)
        self.add_multiple_resource_graph_validation_declarations(root)

        self.assertEqual(evaluate_policy(root), [])

        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentResourceSetContractUnguarded();\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any(
                "multiple resource graph activation requires" in error
                for error in errors
            ),
            errors,
        )

    def test_every_earlier_policy_rejects_multiple_resource_surface(self):
        for policy in (
            approved_policy(),
            active_probe_policy(),
            activation_authorized_policy(),
            sealed_runtime_policy(),
            runtime_validation_policy(),
            active_activation_policy(),
            resource_authorized_policy(),
            resource_validation_policy(),
            active_resource_policy(),
            binary_resource_authorized_policy(),
            binary_resource_validation_policy(),
            active_binary_resource_policy(),
            audio_stream_authorized_policy(),
            audio_stream_validation_policy(),
            active_audio_stream_policy(),
        ):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.enable_active_audio_stream_declarations(root)
                self.write_policy(root, policy)
                self.enable_active_multiple_resource_declarations(root)

                errors = evaluate_policy(root)

                self.assertTrue(
                    any("multiple resource contract requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any("multiple resource transfer requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any("multiple resource runtime requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any("multiple resource graph activation" in error for error in errors),
                    errors,
                )

    def test_audio_stream_authorized_policy_allows_only_contract(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.write_policy(root, audio_stream_authorized_policy())
        self.add_audio_stream_contract_declarations(root)

        self.assertEqual(evaluate_policy(root), [])

        self.add_audio_stream_transfer_declarations(root)
        self.add_audio_stream_runtime_declarations(root)
        self.add_audio_stream_graph_declarations(root)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("audio stream transfer requires" in error for error in errors),
            errors,
        )
        self.assertTrue(
            any("audio stream runtime requires" in error for error in errors),
            errors,
        )
        self.assertTrue(
            any("audio stream graph activation" in error for error in errors),
            errors,
        )

    def test_audio_stream_validation_policy_allows_transfer_not_runtime_or_graph(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_active_resource_backend(root)
        self.enable_active_binary_resource_declarations(root)
        self.write_policy(root, audio_stream_validation_policy())
        self.add_audio_stream_contract_declarations(root)
        self.add_audio_stream_transfer_declarations(root)

        self.assertEqual(evaluate_policy(root), [])

        self.add_audio_stream_runtime_declarations(root)
        self.add_audio_stream_graph_declarations(root)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("audio stream runtime requires" in error for error in errors),
            errors,
        )
        self.assertTrue(
            any("audio stream graph activation" in error for error in errors),
            errors,
        )

    def test_every_earlier_policy_rejects_audio_stream_surface(self):
        earlier_policies = (
            approved_policy(),
            active_probe_policy(),
            activation_authorized_policy(),
            sealed_runtime_policy(),
            runtime_validation_policy(),
            active_activation_policy(),
            resource_authorized_policy(),
            resource_validation_policy(),
            active_resource_policy(),
            binary_resource_authorized_policy(),
            binary_resource_validation_policy(),
            active_binary_resource_policy(),
        )
        for policy in earlier_policies:
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.write_policy(root, policy)
                self.enable_active_audio_stream_declarations(root)

                errors = evaluate_policy(root)

                self.assertTrue(
                    any("audio stream contract requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any("audio stream transfer requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any("audio stream runtime requires" in error for error in errors),
                    errors,
                )
                self.assertTrue(
                    any(
                        "audio stream graph activation" in error
                        for error in errors
                    ),
                    errors,
                )

    def test_active_audio_stream_policy_rejects_broader_authority(self):
        forbidden_declarations = (
            "std::vector<ComponentActivationAudioStreamGrantV1> streams;",
            "ComponentActivationWritableAudioStream writable;",
            "ComponentActivationNetworkAudioStream network;",
            "ComponentActivationCompressedAudioStream compressed;",
            "ComponentActivationReverseAudioStream reverse;",
            "auto componentActivationAudioStreamPath = path;",
            "auto componentActivationAudioStreamDescriptor = descriptor;",
            "ComponentActivationAudioStreamBroker broker;",
        )
        for declaration in forbidden_declarations:
            with self.subTest(declaration=declaration):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.enable_active_binary_resource_declarations(root)
                self.write_policy(root, active_audio_stream_policy())
                self.enable_active_audio_stream_declarations(root)
                path = (
                    root
                    / "src/runtime/component_activation/BroaderAudioStream.cpp"
                )
                path.write_text(declaration + "\n", encoding="utf-8")

                errors = evaluate_policy(root)

                self.assertTrue(
                    any("broader audio stream authority" in error for error in errors),
                    errors,
                )

    def test_resource_validation_allows_runtime_contract_but_not_graph_use(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        self.write_policy(root, resource_validation_policy())
        contract = (
            root
            / "src/runtime/component_activation/ComponentActivationResourceContract.h"
        )
        contract.write_text(
            "struct ComponentActivationImmutableResourceGrantV1 {};\n"
            "struct TalasKernelAbiImmutableResourceViewV2 {};\n",
            encoding="utf-8",
        )
        helper = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp"
        )
        helper.write_text(
            helper.read_text(encoding="utf-8")
            + "void sendComponentActivationResourcePayload();\n",
            encoding="utf-8",
        )

        self.assertEqual(evaluate_policy(root), [])

        bridge = root / "src/runtime/ProjectRuntimeBridge.cpp"
        bridge.write_text(
            "void stage() { compileAndStageIsolatedComponentResource(); }\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("resource graph activation" in error for error in errors),
            errors,
        )

    def test_active_resource_policies_reject_broader_resource_authority(self):
        for policy in (active_resource_policy(), active_binary_resource_policy()):
            with self.subTest(status=policy["status"]):
                temporary, root = self.make_repo()
                self.addCleanup(temporary.cleanup)
                self.enable_active_resource_backend(root)
                self.write_policy(root, policy)
                if policy == active_binary_resource_policy():
                    self.enable_active_binary_resource_declarations(root)
                source = (
                    root
                    / "src/runtime/component_activation/ResourceEscape.cpp"
                )
                source.write_text(
                    "void streamComponentResource();\n"
                    "void writeComponentResource();\n"
                    "const char* componentResourcePath;\n"
                    "struct ComponentActivationBinaryResourceGrantV1 {};\n"
                    "struct ComponentActivationAudioResourceGrantV1 {};\n"
                    "struct ComponentActivationWritableResourceV1 {};\n",
                    encoding="utf-8",
                )

                errors = evaluate_policy(root)

                self.assertTrue(
                    any(
                        "broader component resource authority" in error
                        for error in errors
                    ),
                    errors,
                )

    def test_resource_validation_rejects_host_authored_graph_contract(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        self.write_policy(root, resource_validation_policy())
        host = (
            root
            / "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            host.read_text(encoding="utf-8")
            + "void authorIsolatedComponentResourceContract() {\n"
            "  contract.immutableResource = std::move(resource);\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("resource graph activation" in error for error in errors),
            errors,
        )

    def test_sealed_runtime_stage_has_no_loader_or_library_exception(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_sealed_runtime_backend(root)

        self.assertEqual(evaluate_policy(root), [])

        entitlements = root / "cmake/macos/TalasComponentRuntime.entitlements"
        entitlements.write_text(
            '<?xml version="1.0"?><plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '<key>com.apple.security.cs.disable-library-validation</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("closed sandbox/loading stage" in error for error in errors),
            errors,
        )

    def test_runtime_validation_stage_still_has_no_graph_activation_claim(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        self.write_policy(root, runtime_validation_policy())

        self.assertEqual(evaluate_policy(root), [])

        payload = runtime_validation_policy()
        self.assertEqual(
            payload["componentActivation"],
            "private-macos-stateless-audio-v1-authorized",
        )
        self.assertEqual(
            payload["userCodeExecution"],
            "private-macos-isolated-runtime-validation-only",
        )

        bridge = root / "src/runtime/ProjectRuntimeBridge.cpp"
        bridge.parent.mkdir(parents=True, exist_ok=True)
        bridge.write_text(
            "void stage() { compileAndStageIsolatedComponent(); }\n",
            encoding="utf-8",
        )
        errors = evaluate_policy(root)
        self.assertTrue(
            any("transient graph activation" in error for error in errors),
            errors,
        )

    def test_activation_policy_rejects_mixed_authorized_and_active_claims(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)
        payload = activation_authorized_policy()
        payload["userCodeExecution"] = "private-macos-isolated-runtime-only"
        self.write_policy(root, payload)

        errors = evaluate_policy(root)

        self.assertTrue(
            any("without mixing" in error for error in errors),
            errors,
        )

    def test_active_activation_requires_exact_runtime_entitlements(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        entitlements = root / "cmake/macos/TalasComponentRuntime.entitlements"
        entitlements.write_text(
            '<?xml version="1.0"?><plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '<key>com.apple.security.cs.disable-library-validation</key><true/>'
            '<key>com.apple.security.network.client</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("runtime-helper entitlements" in error for error in errors),
            errors,
        )

    def test_runtime_loader_is_single_owner_and_cannot_be_reused(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "target_sources(talas_runtime PRIVATE "
            "src/runtime/component_activation/macos/ComponentActivationLoader.cpp)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("runtime loader target" in error for error in errors),
            errors,
        )

    def test_private_runtime_loader_library_rejects_product_consumers(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        cmake = root / "CMakeLists.txt"
        text = cmake.read_text(encoding="utf-8")
        text = text.replace(
            "  src/runtime/component_activation/macos/"
            "ComponentActivationLoader.cpp\n",
            "",
        )
        text += (
            "add_library(talas_component_activation_loader STATIC\n"
            "  src/runtime/component_activation/macos/"
            "ComponentActivationLoader.cpp)\n"
            "target_link_libraries(talas_component_activation_helper PRIVATE "
            "talas_component_activation_loader)\n"
            "target_link_libraries(talas_runtime PRIVATE "
            "talas_component_activation_loader)\n"
        )
        cmake.write_text(text, encoding="utf-8")

        errors = evaluate_policy(root)

        self.assertTrue(
            any("unauthorized consumer" in error for error in errors),
            errors,
        )

    def test_component_bytes_cannot_cross_before_seal_and_authorization(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_activation_backend(root)
        host = (
            root
            / "src/runtime/component_activation/macos/"
            "ComponentActivationMacOSHost.cpp"
        )
        host.write_text(
            "void activate() {\n"
            "  sendFrozenComponentEntryBytes();\n"
            "  sealRuntimeBeforeComponentBytes();\n"
            "  consumeComponentActivationAuthorization();\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("sealed-before-bytes" in error for error in errors),
            errors,
        )

    def test_activation_authority_and_transient_boundary_are_private(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "auto authority = ComponentActivationAuthorizationFactory::make();\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("activation authorization factory" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        path = root2 / "src/runtime/component_activation/RegistryPromotion.cpp"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "void promote(NodeRegistry& registry) { registry.addNodeType(node); }\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("canonical registry" in error for error in errors),
            errors,
        )

        temporary3, root3 = self.make_repo()
        self.addCleanup(temporary3.cleanup)
        project_file = root3 / "src/project/ProjectFile.cpp"
        project_file.parent.mkdir(parents=True, exist_ok=True)
        project_file.write_text(
            "ComponentActivationAuthorization persistedActivation;\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root3)

        self.assertTrue(
            any("project format" in error for error in errors),
            errors,
        )

    def test_activation_foundation_cannot_gain_catalog_ui_or_sdk_surface(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        catalog = root / "src/core/BuiltinNodes.cpp"
        catalog.parent.mkdir(parents=True, exist_ok=True)
        catalog.write_text(
            "ComponentActivationDescriptor builtInComponent;\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("canonical node catalog" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        commands = root2 / "src/app/WorkbenchCommands.cpp"
        commands.parent.mkdir(parents=True, exist_ok=True)
        commands.write_text(
            "void activateUserComponent();\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("Workbench" in error and "component action" in error for error in errors),
            errors,
        )

        temporary3, root3 = self.make_repo()
        self.addCleanup(temporary3.cleanup)
        (root3 / "CMakeLists.txt").write_text(
            "install(FILES include/talas/runtime/ComponentActivationProtocol.h "
            "DESTINATION include)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root3)

        self.assertTrue(
            any("component-activation headers" in error for error in errors),
            errors,
        )

    def test_public_or_executable_claims_remain_forbidden_at_foundation(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        payload = approved_policy()
        payload["publicSdk"] = True
        payload["bundleDiscovery"] = True
        payload["bundleInstallation"] = True
        payload["dynamicLoading"] = "unrestricted"
        payload["componentActivation"] = True
        payload["userCodeExecution"] = "in-process"
        self.write_policy(root, payload)

        errors = evaluate_policy(root)

        self.assertTrue(any("publicSdk" in error for error in errors), errors)
        self.assertTrue(
            any("bundleDiscovery" in error for error in errors), errors
        )
        self.assertTrue(
            any("bundleInstallation" in error for error in errors), errors
        )
        self.assertTrue(
            any("dynamicLoading" in error for error in errors), errors
        )
        self.assertTrue(
            any("componentActivation" in error for error in errors), errors
        )
        self.assertTrue(
            any("userCodeExecution" in error for error in errors), errors
        )

    def test_active_probe_policy_accepts_one_exact_loader_boundary(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)

        self.assertEqual(evaluate_policy(root), [])

    def test_active_probe_policy_requires_parent_cleanable_materialization(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "add_library(talas_component_probe_macos_materialization STATIC\n"
                "  src/runtime/component_probe/macos/"
                "ComponentBundleProbeMaterialization.cpp)\n",
                "",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any(
                "component-probe packaging declaration" in error
                and "materialization" in error
                for error in errors
            ),
            errors,
        )

    def test_active_probe_policy_requires_nonactivating_trust_composition(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "add_executable(talas_component_probe_trust_composition_tests\n"
                "  tests/runtime/"
                "ComponentBundleProbeTrustCompositionIntegrationTests.cpp)\n",
                "",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any(
                "component-probe packaging declaration" in error
                and "TrustComposition" in error
                for error in errors
            ),
            errors,
        )

    def test_loader_boundary_rejects_inactive_nearby_and_reused_sources(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        loader = (
            root
            / "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp"
        )
        loader.parent.mkdir(parents=True, exist_ok=True)
        loader.write_text(
            'void probe() { dlopen("sealed", 0); }\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("inactive probe loader" in error for error in errors), errors)

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        self.enable_probe_backend(root2)
        nearby = (
            root2
            / "src/runtime/component_probe/macos/ComponentBundleProbeLoaderCopy.cpp"
        )
        nearby.write_text(
            'void probe() { dlopen("candidate", 0); }\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(any("loader" in error and "Copy" in error for error in errors), errors)

        temporary3, root3 = self.make_repo()
        self.addCleanup(temporary3.cleanup)
        self.enable_probe_backend(root3)
        cmake = root3 / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "target_sources(talas_runtime PRIVATE "
            "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root3)

        self.assertTrue(any("probe loader target" in error for error in errors), errors)

        temporary4, root4 = self.make_repo()
        self.addCleanup(temporary4.cleanup)
        self.enable_probe_backend(root4)
        (
            root4
            / "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp"
        ).unlink()

        errors = evaluate_policy(root4)

        self.assertTrue(any("probe loader source is missing" in error for error in errors), errors)

        temporary5, root5 = self.make_repo()
        self.addCleanup(temporary5.cleanup)
        self.enable_probe_backend(root5)
        loader5 = (
            root5
            / "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp"
        )
        loader5.write_text(
            loader5.read_text(encoding="utf-8")
            + 'void* bypass() { return LoadLibraryA("candidate"); }\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root5)

        self.assertTrue(any("Windows dynamic loader" in error for error in errors), errors)

    def test_active_probe_policy_requires_exact_narrow_entitlements(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        self.enable_probe_backend(root)
        entitlements = root / "cmake/macos/TalasComponentProbe.entitlements"
        entitlements.write_text(
            '<?xml version="1.0"?><plist version="1.0"><dict>'
            '<key>com.apple.security.app-sandbox</key><true/>'
            '<key>com.apple.security.cs.disable-library-validation</key><true/>'
            '<key>com.apple.security.network.client</key><true/>'
            '</dict></plist>\n',
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("probe-helper entitlements" in error for error in errors), errors)

    def test_loader_primitive_and_product_install_claim_fail(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Loader.cpp").write_text(
            'void load() { dlopen("candidate", 0); }\n',
            encoding="utf-8",
        )
        (root / "README.md").write_text(
            "Install user component bundle from the Workbench.\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("dlopen" in error for error in errors), errors)
        self.assertTrue(any("product claim" in error for error in errors), errors)

    def test_framework_and_platform_loader_families_fail(self):
        fixtures = {
            "src/runtime/JuceLoader.cpp":
                'void load() { juce::DynamicLibrary::open("candidate"); }\n',
            "src/runtime/CoreFoundationLoader.mm":
                "void load(CFBundleRef bundle) { "
                "CFBundleLoadExecutableAndReturnError(bundle, nullptr); }\n",
            "src/runtime/CoreFoundationFunctions.mm":
                "void load(CFBundleRef bundle, CFArrayRef names) { "
                "CFBundleGetFunctionPointersForNames(bundle, names, 0); }\n",
            "src/runtime/CoreFoundationData.mm":
                "void load(CFBundleRef bundle, CFStringRef name) { "
                "CFBundleGetDataPointerForName(bundle, name); }\n",
            "src/runtime/CoreFoundationDataPlural.mm":
                "void load(CFBundleRef bundle, CFArrayRef names) { "
                "CFBundleGetDataPointersForNames(bundle, names, 0); }\n",
            "src/runtime/FoundationLoader.m":
                "void load(NSBundle* bundle) { [bundle loadAndReturnError:0]; }\n",
            "src/runtime/FoundationPrincipalClass.m":
                "Class load(NSBundle* bundle) { return [bundle principalClass]; }\n",
            "src/runtime/FoundationNamedClass.m":
                'Class load(NSBundle* bundle) { return [bundle classNamed:@"Entry"]; }\n',
            "src/runtime/WindowsLoader.cpp":
                'void load() { LoadPackagedLibrary(L"candidate", 0); }\n',
            "src/runtime/PosixSymbols.cpp":
                'void* symbol(void* handle) { return dlsym(handle, "entry"); }\n',
            "src/runtime/SwiftLoader.swift":
                'func load(_ path: String) { Bundle(path: path)?.load() }\n',
            "src/runtime/SplitSwiftLoader.swift":
                "let candidate = Bundle(url: url)!\n"
                "candidate.load()\n",
            "src/runtime/SwiftPrincipalClass.swift":
                "let candidate = Bundle(url: url)!\n"
                "let entry = candidate.principalClass\n",
            "src/runtime/SwiftResourceBundle.swift":
                'let resource = Bundle.main.url(forResource: "data", '
                'withExtension: "json")\n',
            "src/runtime/MachImageLoader.cpp":
                'void load() { NSAddImage("candidate", 0); }\n',
        }

        for relative_path, source in fixtures.items():
            with self.subTest(relative_path=relative_path):
                temporary, root = self.make_repo()
                try:
                    path = root / relative_path
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(source, encoding="utf-8")

                    errors = evaluate_policy(root)

                    self.assertTrue(
                        any("loader" in error for error in errors), errors
                    )
                finally:
                    temporary.cleanup()

    def test_dynamic_loader_build_linkage_fails(self):
        for statement in (
            "target_link_libraries(talas_runtime PRIVATE ${CMAKE_DL_LIBS})",
            "target_link_libraries(talas_runtime PRIVATE dl)",
            "target_link_libraries(talas_runtime PRIVATE -ldl)",
            "link_libraries(dl)",
            "target_link_libraries(talas_runtime PRIVATE /usr/lib/libdl.so)",
            'target_link_options(talas_runtime PRIVATE "LINKER:-l,dl")',
            'add_link_options("LINKER:SHELL:-l dl")',
            'target_link_options(talas_runtime PRIVATE "-Wl,-l:libdl.so")',
            'add_link_options("-Wl,--library=dl")',
            'add_link_options("-Wl,--library=:libdl.so")',
            'target_link_options(talas_runtime PRIVATE '
            '"LINKER:--library,:libdl.so")',
        ):
            with self.subTest(statement=statement):
                temporary, root = self.make_repo()
                try:
                    (root / "CMakeLists.txt").write_text(
                        statement + "\n",
                        encoding="utf-8",
                    )

                    errors = evaluate_policy(root)

                    self.assertTrue(
                        any("loader linkage" in error for error in errors), errors
                    )
                finally:
                    temporary.cleanup()

    def test_indirect_dynamic_loader_lookup_fails(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "CMakeLists.txt").write_text(
            "find_library(TALAS_DL NAMES dl)\n"
            "target_link_libraries(talas_runtime PRIVATE ${TALAS_DL})\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("loader linkage" in error for error in errors), errors)

    def test_compiled_backend_flag_must_be_platform_gated(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        header = root / "include/talas/runtime/ComponentTrustPolicy.h"
        header.write_text(
            "inline constexpr bool componentIsolationBackendAvailableV1 = true;\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("componentIsolationBackendAvailableV1" in error for error in errors),
            errors,
        )

    def test_compiled_backend_flag_must_remain_macos_only(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        header = root / "include/talas/runtime/ComponentTrustPolicy.h"
        header.write_text(
            header.read_text(encoding="utf-8").replace(
                "defined(__APPLE__) && defined(__MACH__) && TARGET_OS_OSX",
                "defined(__APPLE__)",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("componentIsolationBackendAvailableV1" in error for error in errors),
            errors,
        )

    def test_backend_requires_exact_sandbox_and_packaging_declarations(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        entitlements = root / "cmake/macos/TalasComponentHost.entitlements"
        entitlements.write_text(
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>com.apple.security.network.client</key><true/>"
            "</dict></plist>\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("App Sandbox entitlement" in error for error in errors), errors)

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        cmake = root2 / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "add_dependencies(talas_workbench talas_component_isolation_helper)\n",
                "",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(any("packaging declaration" in error for error in errors), errors)

    def test_backend_requires_product_audio_worker_and_latency_declarations(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8").replace(
                "src/runtime/ComponentIsolationProductAudioWorker.cpp",
                "src/runtime/RemovedWorker.cpp",
            ),
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("packaging declaration" in error for error in errors), errors)

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        data_plane = root2 / "include/talas/runtime/ComponentIsolationAudioDataPlane.h"
        data_plane.write_text(
            data_plane.read_text(encoding="utf-8").replace("= 1", "= 2"),
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("pipeline quantum" in error for error in errors), errors
        )

    def test_ready_isolation_fields_cannot_be_publicly_constructed(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        header = root / "include/talas/runtime/ComponentTrustPolicy.h"
        header.write_text(
            header.read_text(encoding="utf-8")
            + "struct ComponentTrustEvidence {\n"
            + "ComponentIsolationState isolation = ComponentIsolationState::Ready;\n"
            + "unsigned isolationProfileVersion = 1;\n"
            + "unsigned isolationSafeguards = 255;\n"
            + "unsigned isolationSupportedPermissions = 1;\n"
            + "};\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("opaque isolation evidence" in error for error in errors), errors)

    def test_internal_policy_header_cannot_be_installed_as_sdk_surface(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "CMakeLists.txt").write_text(
            "install(FILES include/talas/runtime/ComponentTrustPolicy.h "
            "DESTINATION include)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(any("install/export" in error for error in errors), errors)

    def test_component_bundle_inspection_headers_remain_private(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "include/talas/runtime/ComponentBundleManifest.h").write_text(
            "struct ComponentBundleManifest {};\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("bundle inspection header" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        (root2 / "CMakeLists.txt").write_text(
            "install(DIRECTORY src/runtime/component_bundle "
            "DESTINATION include)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("bundle inspection header" in error for error in errors),
            errors,
        )

    def test_signature_test_anchor_cannot_enter_product_source(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "verifyComponentBundleSignatureWithAnchorForTesting(snapshot, anchor);\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("test-only component signature trust override" in error for error in errors),
            errors,
        )

    def test_audio_response_readiness_test_seam_cannot_enter_product_source(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)\n"
            "if (binding.publishedAudioResponseReadyForTesting()) render();\n"
            "if (audioHost.publishedResponseReady()) render();\n"
            "#endif\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("test-only audio response readiness" in error for error in errors),
            errors,
        )

    def test_verified_signature_evidence_has_one_product_producer(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "evidence.state = ComponentSignatureState::Verified;\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("private verifier adapter" in error for error in errors),
            errors,
        )

        temporary3, root3 = self.make_repo()
        self.addCleanup(temporary3.cleanup)
        (root3 / "src/runtime/Runtime.cpp").write_text(
            "auto evidence = ComponentBundleSignatureEvidenceFactory::make(view);\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root3)

        self.assertTrue(
            any("opaque component signature evidence factory" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        (root2 / "src/runtime/Runtime.cpp").write_text(
            "auto evidence = ComponentSignatureEvidence{"
            "ComponentSignatureState::Verified, digest, signer, 1, true, true};\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("private verifier adapter" in error for error in errors),
            errors,
        )

    def test_probe_evidence_has_one_opaque_product_authority(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "auto authored = ComponentBundleProbeEvidenceFactory::"
            "makeFromValidatedResult(result, request);\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("opaque component probe evidence factory" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        header = root2 / "include/talas/runtime/ComponentTrustPolicy.h"
        header.write_text(
            header.read_text(encoding="utf-8")
            + "struct ComponentProbeEvidence {\n"
            + "  ComponentProbeState state = ComponentProbeState::Clean;\n"
            + "};\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("opaque probe evidence" in error for error in errors),
            errors,
        )

    def test_probe_validation_and_fixtures_remain_private(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        cmake = root / "CMakeLists.txt"
        cmake.write_text(
            cmake.read_text(encoding="utf-8")
            + "\ntarget_link_libraries(talas_runtime PRIVATE "
            + "talas_component_bundle_probe_clean_fixture)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("test-only component probe fixtures" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        cmake2 = root2 / "CMakeLists.txt"
        cmake2.write_text(
            cmake2.read_text(encoding="utf-8")
            + "\ninstall(TARGETS talas_component_probe_validation)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("private component probe validation" in error for error in errors),
            errors,
        )

        temporary3, root3 = self.make_repo()
        self.addCleanup(temporary3.cleanup)
        cmake3 = root3 / "CMakeLists.txt"
        cmake3.write_text(
            cmake3.read_text(encoding="utf-8")
            + "\ntarget_link_libraries(talas_runtime PRIVATE "
            + "talas_component_bundle_probe_entry_crash_fixture)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root3)

        self.assertTrue(
            any("test-only component probe fixtures" in error for error in errors),
            errors,
        )

        temporary4, root4 = self.make_repo()
        self.addCleanup(temporary4.cleanup)
        cmake4 = root4 / "CMakeLists.txt"
        cmake4.write_text(
            cmake4.read_text(encoding="utf-8")
            + "\ninstall(TARGETS "
            + "talas_component_probe_coordinator_delayed_fixture)\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root4)

        self.assertTrue(
            any("private component probe validation" in error for error in errors),
            errors,
        )

    def test_owner_authorization_cannot_gain_plain_persistence(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "#include <fstream>\n"
            "void save(ComponentOwnerAuthorizationStore& store) {\n"
            "  std::ofstream output(\"approval.json\");\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("owner authorization" in error and "persistence" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        (root2 / "src/runtime/Runtime.cpp").write_text(
            "void save(ComponentOwnerAuthorizationStore& store) {\n"
            "  SecItemAdd(query, result);\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("owner authorization" in error and "persistence" in error for error in errors),
            errors,
        )

    def test_dependency_and_migration_metadata_cannot_gain_authority(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "#include <fstream>\n"
            "void resolveComponentBundleDependencies() {\n"
            "  std::ifstream manifest(\"candidate.json\");\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("immutable in-memory metadata" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        (root2 / "src/runtime/Runtime.cpp").write_text(
            "void validateDeclaredComponentBundleMigrationRoute() {\n"
            "  executeStateMigration();\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("immutable in-memory metadata" in error for error in errors),
            errors,
        )

    def test_preservation_compatibility_cannot_resolve_or_activate(self):
        temporary, root = self.make_repo()
        self.addCleanup(temporary.cleanup)
        (root / "src/runtime/Runtime.cpp").write_text(
            "void projectPreservedComponentCompatibility() {\n"
            "  registry.addNodeType(candidate);\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root)

        self.assertTrue(
            any("compatibility must not mutate" in error for error in errors),
            errors,
        )

        temporary2, root2 = self.make_repo()
        self.addCleanup(temporary2.cleanup)
        (root2 / "src/runtime/Runtime.cpp").write_text(
            "void projectPreservedComponentCompatibility() {\n"
            "  sendPayloadToHelper(snapshot.payloads);\n"
            "}\n",
            encoding="utf-8",
        )

        errors = evaluate_policy(root2)

        self.assertTrue(
            any("activation helpers" in error for error in errors),
            errors,
        )


if __name__ == "__main__":
    unittest.main()
