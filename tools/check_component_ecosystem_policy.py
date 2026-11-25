#!/usr/bin/env python3

import argparse
import json
import pathlib
import plistlib
import re
import sys


POLICY_PATH = pathlib.Path("docs/governance/component-ecosystem-policy.json")
HEADER_PATH = pathlib.Path("include/talas/runtime/ComponentTrustPolicy.h")

PROBE_AUTHORIZED_POLICY = {
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

ACTIVE_PROBE_POLICY = {
    **PROBE_AUTHORIZED_POLICY,
    "status": "isolated-probe-only",
    "dynamicLoading": "macos-isolated-probe-only",
    "userCodeExecution": "probe-only",
    "probeBackend": "macos-one-shot-xpc-v1",
}

ACTIVATION_AUTHORIZED_POLICY = {
    **ACTIVE_PROBE_POLICY,
    "status": "isolated-activation-foundation-authorized",
    "componentActivation": "private-macos-stateless-audio-v1-authorized",
    "activationLoadingPolicy": "macos-sealed-runtime-xpc-v1",
    "activationProfile": "macos-stateless-realtime-audio-v1",
    "activationSurface": "private-runtime-api-only",
}

SEALED_RUNTIME_POLICY = {
    **ACTIVATION_AUTHORIZED_POLICY,
    "status": "isolated-activation-sandbox-only",
    "activationBackend": "macos-sealed-runtime-prewarm-xpc-v1",
}

RUNTIME_VALIDATION_POLICY = {
    **ACTIVATION_AUTHORIZED_POLICY,
    "status": "isolated-activation-runtime-validation-only",
    "dynamicLoading": "macos-isolated-probe-and-runtime-validation-only",
    "userCodeExecution": "private-macos-isolated-runtime-validation-only",
    "activationBackend": "macos-sealed-runtime-validation-xpc-v1",
}

ACTIVE_ACTIVATION_POLICY = {
    **ACTIVATION_AUTHORIZED_POLICY,
    "status": "isolated-activation-foundation",
    "dynamicLoading": "macos-isolated-probe-and-runtime-only",
    "componentActivation": "private-macos-stateless-audio-v1",
    "userCodeExecution": "private-macos-isolated-runtime-only",
    "activationBackend": "macos-sealed-runtime-xpc-v1",
}

RESOURCE_AUTHORIZED_POLICY = {
    **ACTIVE_ACTIVATION_POLICY,
    "status": "isolated-activation-immutable-resource-authorized",
    "componentActivation":
        "private-macos-stateless-audio-table-resource-v1-authorized",
    "activationProfile":
        "macos-stateless-realtime-audio-table-resource-v1",
    "componentResourceAccess": "single-table-snapshot-v1-authorized",
}

RESOURCE_VALIDATION_POLICY = {
    **RESOURCE_AUTHORIZED_POLICY,
    "status": "isolated-activation-immutable-resource-validation-only",
    "componentActivation":
        "private-macos-stateless-audio-table-resource-v1-validation-only",
    "componentResourceAccess": "single-table-snapshot-v1-validation-only",
}

ACTIVE_RESOURCE_POLICY = {
    **RESOURCE_AUTHORIZED_POLICY,
    "status": "isolated-activation-immutable-resource",
    "componentActivation":
        "private-macos-stateless-audio-table-resource-v1",
    "componentResourceAccess": "single-table-snapshot-v1",
}

BINARY_RESOURCE_AUTHORIZED_POLICY = {
    **ACTIVE_RESOURCE_POLICY,
    "version": 5,
    "status": "isolated-activation-immutable-binary-resource-authorized",
    "componentActivation":
        "private-macos-stateless-audio-table-or-binary-resource-v1-authorized",
    "activationProfile":
        "macos-stateless-realtime-audio-table-or-binary-resource-v1",
    "componentResourceAccess":
        "single-table-or-binary-snapshot-v1-authorized",
}

BINARY_RESOURCE_VALIDATION_POLICY = {
    **BINARY_RESOURCE_AUTHORIZED_POLICY,
    "status": "isolated-activation-immutable-binary-resource-validation-only",
    "componentActivation":
        "private-macos-stateless-audio-table-or-binary-resource-v1-validation-only",
    "componentResourceAccess":
        "single-table-or-binary-snapshot-v1-validation-only",
}

ACTIVE_BINARY_RESOURCE_POLICY = {
    **BINARY_RESOURCE_AUTHORIZED_POLICY,
    "status": "isolated-activation-immutable-binary-resource",
    "componentActivation":
        "private-macos-stateless-audio-table-or-binary-resource-v1",
    "componentResourceAccess": "single-table-or-binary-snapshot-v1",
}

AUDIO_STREAM_AUTHORIZED_POLICY = {
    **ACTIVE_BINARY_RESOURCE_POLICY,
    "version": 6,
    "status": "isolated-activation-bounded-audio-stream-authorized",
    "componentActivation":
        "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1-authorized",
    "activationProfile":
        "macos-stateless-realtime-audio-table-binary-or-audio-stream-resource-v1",
    "componentResourceAccess":
        "single-table-binary-or-audio-stream-snapshot-v1-authorized",
}

AUDIO_STREAM_VALIDATION_POLICY = {
    **AUDIO_STREAM_AUTHORIZED_POLICY,
    "status": "isolated-activation-bounded-audio-stream-validation-only",
    "componentActivation":
        "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1-validation-only",
    "componentResourceAccess":
        "single-table-binary-or-audio-stream-snapshot-v1-validation-only",
}

ACTIVE_AUDIO_STREAM_POLICY = {
    **AUDIO_STREAM_AUTHORIZED_POLICY,
    "status": "isolated-activation-bounded-audio-stream",
    "componentActivation":
        "private-macos-stateless-audio-table-binary-or-audio-stream-resource-v1",
    "componentResourceAccess":
        "single-table-binary-or-audio-stream-snapshot-v1",
}

MULTIPLE_RESOURCE_AUTHORIZED_POLICY = {
    **ACTIVE_AUDIO_STREAM_POLICY,
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

MULTIPLE_RESOURCE_VALIDATION_POLICY = {
    **MULTIPLE_RESOURCE_AUTHORIZED_POLICY,
    "status":
        "isolated-activation-multiple-immutable-resources-validation-only",
    "componentActivation":
        "private-macos-stateless-audio-multiple-immutable-resources-v1-validation-only",
    "componentResourceAccess":
        "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1-validation-only",
}

ACTIVE_MULTIPLE_RESOURCE_POLICY = {
    **MULTIPLE_RESOURCE_AUTHORIZED_POLICY,
    "status": "isolated-activation-multiple-immutable-resources",
    "componentActivation":
        "private-macos-stateless-audio-multiple-immutable-resources-v1",
    "componentResourceAccess":
        "aggregate-budgeted-multiple-table-binary-or-audio-stream-snapshots-v1",
}

REPLACEMENT_AUTHORIZED_POLICY = {
    **ACTIVE_MULTIPLE_RESOURCE_POLICY,
    "version": 8,
    "status": "isolated-activation-explicit-replacement-authorized",
    "componentReplacement":
        "explicit-local-replacement-admission-v1-authorized",
    "replacementSurface":
        "private-preview-confirmation-and-stage-admission-only",
}

REPLACEMENT_VALIDATION_POLICY = {
    **REPLACEMENT_AUTHORIZED_POLICY,
    "status": "isolated-activation-explicit-replacement-validation-only",
    "componentReplacement":
        "explicit-owner-confirmed-local-replacement-v1-validation-only",
    "replacementSurface": "tests-only-workbench-preview-and-controller",
}

ACTIVE_REPLACEMENT_POLICY = {
    **REPLACEMENT_AUTHORIZED_POLICY,
    "status": "isolated-activation-explicit-replacement",
    "componentReplacement":
        "explicit-owner-confirmed-local-replacement-v1",
    "replacementSurface": "owner-selected-workbench-only",
    "activationSurface":
        "private-runtime-and-owner-selected-workbench-replacement-only",
    "workbenchActivation": True,
}

ACCEPTED_POLICIES = (
    PROBE_AUTHORIZED_POLICY,
    ACTIVE_PROBE_POLICY,
    ACTIVATION_AUTHORIZED_POLICY,
    SEALED_RUNTIME_POLICY,
    RUNTIME_VALIDATION_POLICY,
    ACTIVE_ACTIVATION_POLICY,
    RESOURCE_AUTHORIZED_POLICY,
    RESOURCE_VALIDATION_POLICY,
    ACTIVE_RESOURCE_POLICY,
    BINARY_RESOURCE_AUTHORIZED_POLICY,
    BINARY_RESOURCE_VALIDATION_POLICY,
    ACTIVE_BINARY_RESOURCE_POLICY,
    AUDIO_STREAM_AUTHORIZED_POLICY,
    AUDIO_STREAM_VALIDATION_POLICY,
    ACTIVE_AUDIO_STREAM_POLICY,
    MULTIPLE_RESOURCE_AUTHORIZED_POLICY,
    MULTIPLE_RESOURCE_VALIDATION_POLICY,
    ACTIVE_MULTIPLE_RESOURCE_POLICY,
    REPLACEMENT_AUTHORIZED_POLICY,
    REPLACEMENT_VALIDATION_POLICY,
    ACTIVE_REPLACEMENT_POLICY,
)

ENTITLEMENTS_PATH = pathlib.Path(
    "cmake/macos/TalasComponentHost.entitlements"
)
PROBE_ENTITLEMENTS_PATH = pathlib.Path(
    "cmake/macos/TalasComponentProbe.entitlements"
)
RUNTIME_ENTITLEMENTS_PATH = pathlib.Path(
    "cmake/macos/TalasComponentRuntime.entitlements"
)
HOST_HEADER_PATH = pathlib.Path(
    "include/talas/runtime/ComponentIsolationMacOSHost.h"
)
AUDIO_DATA_PLANE_HEADER_PATH = pathlib.Path(
    "include/talas/runtime/ComponentIsolationAudioDataPlane.h"
)
LIVE_EVIDENCE_PATH = pathlib.Path(
    "src/runtime/component_isolation/ComponentIsolationLiveEvidence.h"
)

REQUIRED_BACKEND_DECLARATIONS = (
    "if(APPLE)",
    "src/runtime/ComponentIsolationProductAudioWorker.cpp",
    "TalasComponentHost.entitlements",
    "add_library(talas_component_isolation_macos_host STATIC",
    "add_executable(talas_component_isolation_helper MACOSX_BUNDLE",
    "talas_sign_component_isolation_helper(talas_component_isolation_helper)",
    "target_link_libraries(talas_workbench PRIVATE talas_component_isolation_macos_host)",
    "add_dependencies(talas_workbench talas_component_isolation_helper)",
    "LINK_DEPENDS \"$<TARGET_FILE:talas_component_isolation_helper>\"",
    "TalasComponentHost.xpc",
    "if(TALAS_BUILD_TESTS)",
    "talas_component_isolation_product_audio_worker_tests",
    "talas_component_isolation_macos_host_integration_tests",
    "talas_component_isolation_sandbox_integration_tests",
    "TalasComponentHostUnsandboxed.xpc",
    "talas_component_isolation_watchdog_integration_tests",
)

PROBE_LOADER_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_probe/macos/ComponentBundleProbeLoader.cpp"
)
PROBE_COORDINATOR_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.cpp"
)
PROBE_MATERIALIZATION_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_probe/macos/ComponentBundleProbeMaterialization.cpp"
)
PROBE_VALIDATION_TARGET = "talas_component_probe_validation"
PROBE_CLEAN_FIXTURE_TARGET = "talas_component_bundle_probe_clean_fixture"
PROBE_CONTROLLED_FATAL_FIXTURE_TARGET = (
    "talas_component_bundle_probe_controlled_fatal_fixture"
)
PROBE_FIXTURE_TARGETS = frozenset(
    {
        PROBE_CLEAN_FIXTURE_TARGET,
        PROBE_CONTROLLED_FATAL_FIXTURE_TARGET,
        "talas_component_bundle_probe_missing_symbol_fixture",
        "talas_component_bundle_probe_metadata_mismatch_fixture",
        "talas_component_bundle_probe_constructor_crash_fixture",
        "talas_component_bundle_probe_entry_crash_fixture",
        "talas_component_bundle_probe_entry_hang_fixture",
        "talas_component_bundle_probe_unload_crash_fixture",
        "talas_component_bundle_probe_unload_hang_fixture",
        "talas_component_probe_coordinator_malformed_fixture",
        "talas_component_probe_coordinator_delayed_fixture",
        "talas_component_probe_coordinator_late_fixture",
        "talas_component_probe_coordinator_no_start_fixture",
        "talas_component_probe_coordinator_wrong_process_fixture",
        "talas_component_probe_coordinator_post_reply_crash_fixture",
        "talas_component_bundle_probe_sandbox_fixture",
    }
)
PROBE_FIXTURE_REFERENCE_TARGETS = frozenset(
    {
        "talas_component_bundle_macho_validation_tests",
        "talas_component_bundle_probe_helper_integration_tests",
        "talas_component_bundle_probe_sandbox_fixture",
        "talas_component_bundle_probe_sandbox_integration_tests",
        "talas_component_probe_trust_composition_tests",
        "talas_component_activation_helper_sandbox",
    }
)
REQUIRED_PROBE_BACKEND_DECLARATIONS = (
    "set(TALAS_COMPONENT_PROBE_HELPER_ENTITLEMENTS",
    "add_executable(talas_component_bundle_probe_helper MACOSX_BUNDLE",
    "src/runtime/component_probe/macos/ComponentBundleProbeHelperMain.cpp",
    PROBE_LOADER_SOURCE.as_posix(),
    "talas_sign_component_probe_helper(talas_component_bundle_probe_helper)",
    "OUTPUT_NAME TalasComponentProbe",
    "BUNDLE_EXTENSION xpc",
    "add_dependencies(talas_workbench talas_component_bundle_probe_helper)",
    "TalasComponentProbe.xpc",
    "add_library(talas_component_probe_macos_coordinator STATIC",
    PROBE_COORDINATOR_SOURCE.as_posix(),
    "add_library(talas_component_probe_macos_materialization STATIC",
    PROBE_MATERIALIZATION_SOURCE.as_posix(),
    "tests/runtime/ComponentBundleProbeMacOSCoordinatorTests.cpp",
    "tests/runtime/ComponentBundleProbeMacOSCoordinatorIntegrationTests.cpp",
    "tests/runtime/ComponentBundleProbeTrustCompositionIntegrationTests.cpp",
)

RUNTIME_LOADER_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
)
RUNTIME_HOST_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
)
ACTIVATION_AUTHORIZATION_SOURCE = pathlib.PurePosixPath(
    "src/runtime/component_activation/ComponentActivationAuthorization.cpp"
)
ACTIVATION_AUTHORIZATION_FACTORY_ALLOWLIST = frozenset(
    {
        ACTIVATION_AUTHORIZATION_SOURCE,
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAuthorization.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAudioSourceFreezer.h"
        ),
    }
)
REQUIRED_SEALED_RUNTIME_DECLARATIONS = (
    "set(TALAS_COMPONENT_RUNTIME_HELPER_ENTITLEMENTS",
    "add_library(talas_component_activation_macos_host STATIC",
    RUNTIME_HOST_SOURCE.as_posix(),
    "add_executable(talas_component_activation_helper MACOSX_BUNDLE",
    "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp",
    "talas_sign_component_activation_helper(talas_component_activation_helper)",
    "OUTPUT_NAME TalasComponentRuntime",
    "BUNDLE_EXTENSION xpc",
    "target_link_libraries(talas_workbench PRIVATE talas_component_activation_macos_host",
    "add_dependencies(talas_workbench talas_component_activation_helper)",
    "TalasComponentRuntime.xpc",
    "talas_component_activation_sandbox_integration_tests",
)
REQUIRED_RUNTIME_VALIDATION_DECLARATIONS = (
    "add_library(talas_component_activation_authorization STATIC",
    ACTIVATION_AUTHORIZATION_SOURCE.as_posix(),
    "add_library(talas_component_activation_protocol STATIC",
    "src/runtime/component_activation/ComponentActivationProtocol.cpp",
    "src/runtime/component_activation/ComponentActivationAudioDataPlane.cpp",
    RUNTIME_LOADER_SOURCE.as_posix(),
    "src/runtime/component_activation/ComponentActivationWorker.cpp",
    "talas_component_activation_authorization_tests",
    "talas_component_activation_protocol_tests",
    "talas_component_activation_macos_host_integration_tests",
    "talas_component_activation_deadline_integration_tests",
    "talas_component_activation_quarantine_tests",
)
REQUIRED_ACTIVE_ACTIVATION_DECLARATIONS = (
    "talas_component_activation_transient_graph_tests",
)

LOADER_PATTERNS = (
    (
        "POSIX dynamic loader",
        re.compile(r"\b(?:dlopen|dlmopen|dlsym)\s*\("),
    ),
    (
        "Windows dynamic loader",
        re.compile(
            r"\b(?:LoadLibrary(?:Ex)?[AW]?|LoadPackagedLibrary|"
            r"LdrLoadDll|GetProcAddress)\s*\("
        ),
    ),
    (
        "JUCE DynamicLibrary loader",
        re.compile(r"\bjuce\s*::\s*DynamicLibrary\b"),
    ),
    (
        "CoreFoundation bundle loader",
        re.compile(
            r"\b(?:CFBundleLoadExecutable(?:AndReturnError)?|"
            r"CFBundleGet(?:Function|Data)Pointers?ForNames?)\s*\("
        ),
    ),
    (
        "Foundation bundle loader",
        re.compile(
            r"\bNSBundle\b[\s\S]*"
            r"\b(?:load(?:AndReturnError)?|principalClass|classNamed)\b"
        ),
    ),
    (
        "Mach-O legacy loader",
        re.compile(
            r"\b(?:NSCreateObjectFileImageFromFile|NSLinkModule|NSAddImage)\s*\("
        ),
    ),
    (
        "Swift Foundation bundle loader",
        re.compile(
            r"\bBundle\b[\s\S]*\.\s*"
            r"(?:load(?:AndReturnError)?\s*\(|principalClass\b|"
            r"classNamed\s*\()"
        ),
    ),
)
LOADER_LINKAGE_PATTERN = re.compile(
    r"(?:\$\{CMAKE_DL_LIBS\}|(?<![A-Za-z0-9_])-ldl\b|"
    r"(?<![A-Za-z0-9_])-l(?:dl\b|[,\s]+dl\b|"
    r":libdl\.(?:a|dylib|so(?:\.[0-9]+)*)\b)|"
    r"(?<![A-Za-z0-9_])--library(?:=|,|\s+)(?:dl\b|"
    r":libdl\.(?:a|dylib|so(?:\.[0-9]+)*)\b)|"
    r"target_link_libraries\s*\([^)]*\bdl\b|"
    r"link_libraries\s*\([^)]*\bdl\b|"
    r"find_library\s*\([^)]*\b(?:NAMES\s+[^)]*)?dl\b|"
    r"(?:^|[/\s])libdl\.(?:a|dylib|so(?:\.[0-9]+)*)\b)",
    re.IGNORECASE | re.MULTILINE,
)
PRODUCT_CLAIM_PATTERN = re.compile(
    r"\b(?:install|load|run)\s+(?:an?\s+)?"
    r"(?:user|third[- ]party)\s+(?:component|bundle)",
    re.IGNORECASE,
)
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".m", ".mm", ".swift"
}

# The policy gate forbids platform bundle-loader types in product source.
# A future resource-only use must enter this reviewed path allowlist and still
# remains subject to the explicit code-loading API patterns above.
BUNDLE_RESOURCE_ALLOWLIST: frozenset[pathlib.PurePosixPath] = frozenset()
SIGNATURE_TEST_SEAM_ALLOWLIST = frozenset(
    {
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleSignatureVerifier.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleSignatureVerifier.cpp"
        ),
    }
)
AUDIO_RESPONSE_TEST_SEAM_ALLOWLIST = frozenset(
    {
        pathlib.PurePosixPath(
            "include/talas/runtime/ComponentActivationAudioDataPlane.h"
        ),
        pathlib.PurePosixPath("include/talas/runtime/IsolatedComponentRuntime.h"),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAudioDataPlane.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAudioStream.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAudioStream.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationLoader.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp"
        ),
    }
)
SIGNATURE_VERIFIED_PRODUCER = pathlib.PurePosixPath(
    "src/runtime/component_bundle/ComponentBundleSignatureVerifier.cpp"
)
SIGNATURE_VERIFIED_TOKEN_ALLOWLIST = frozenset(
    {
        SIGNATURE_VERIFIED_PRODUCER,
        pathlib.PurePosixPath("src/runtime/ComponentTrustPolicy.cpp"),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/ComponentActivationAuthorization.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleSignatureVerifier.h"
        ),
    }
)
SIGNATURE_EVIDENCE_FACTORY_ALLOWLIST = frozenset(
    {
        pathlib.PurePosixPath("include/talas/runtime/ComponentTrustPolicy.h"),
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleSignatureVerifier.h"
        ),
        SIGNATURE_VERIFIED_PRODUCER,
    }
)
PROBE_EVIDENCE_FACTORY_ALLOWLIST = frozenset(
    {
        pathlib.PurePosixPath("include/talas/runtime/ComponentTrustPolicy.h"),
        pathlib.PurePosixPath(
            "src/runtime/component_probe/ComponentBundleProbeEvidence.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_probe/ComponentBundleProbeEvidence.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.cpp"
        ),
    }
)
OWNER_AUTHORIZATION_PERSISTENCE_PATTERN = re.compile(
    r"(?:#\s*include\s*<\s*(?:filesystem|fstream)\s*>|"
    r"\bstd::(?:filesystem|fstream|ofstream)\b|"
    r"\b(?:sqlite3_[A-Za-z0-9_]*|SecItem[A-Za-z0-9_]*|"
    r"SecKeychain[A-Za-z0-9_]*|CFPreferences[A-Za-z0-9_]*|"
    r"NSUserDefaults|fopen|openat?|write)\s*\(|"
    r"[\"'][^\"']*(?:approval|authorization)[^\"']*"
    r"\.(?:json|plist|db|sqlite)[\"'])",
    re.IGNORECASE,
)
COMPONENT_METADATA_AUTHORITY_PATTERN = re.compile(
    r"(?:#\s*include\s*<\s*(?:filesystem|fstream)\s*>|"
    r"\bstd::(?:filesystem|fstream|ifstream|ofstream)\b|"
    r"\b(?:fopen|openat?|read|write)\s*\(|"
    r"\b(?:execute|invoke|load|run)[A-Za-z0-9_]*Migration\s*\()",
    re.IGNORECASE,
)
PRESERVATION_COMPATIBILITY_AUTHORITY_PATTERN = re.compile(
    r"(?:\b(?:NodeRegistry|ProjectEditor|ComponentIsolationMacOSHost)\b|"
    r"\b(?:addNodeType|repairPreservedComponent|"
    r"send[A-Za-z0-9_]*Helper|activate[A-Za-z0-9_]*Component)\s*\()",
    re.IGNORECASE,
)
COMPONENT_RESOURCE_CONTRACT_PATTERN = re.compile(
    r"\b(?:ComponentActivationImmutableResourceGrantV1|"
    r"TalasKernelAbiImmutableResourceViewV2|TalasKernelAbiHostServicesV2|"
    r"ComponentActivationStaticParameterKindV1\s*::\s*Resource)\b"
)
COMPONENT_RESOURCE_RUNTIME_PATTERN = re.compile(
    r"\b(?:send|decode|map|validate|create)"
    r"ComponentActivationResourcePayload\w*\b"
)
COMPONENT_RESOURCE_GRAPH_PATTERN = re.compile(
    r"\b(?:compileAndStageIsolatedComponentResource|"
    r"authorIsolatedComponentResourceContract)\w*\b|"
    r"\bcontract\s*\.\s*immutableResource\s*="
)
COMPONENT_RESOURCE_BROAD_AUTHORITY_PATTERN = re.compile(
    r"\b(?:(?:stream|write|open|load|fetch|mutate)"
    r"ComponentResource\w*|componentResource(?:Path|File|Descriptor|"
    r"Bookmark|Url|URL)\w*|ComponentActivation(?:Binary|Audio|Writable|"
    r"Streaming)Resource\w*)\b"
)
COMPONENT_BINARY_RESOURCE_PATTERN = re.compile(
    r"\b(?:ComponentActivationImmutableResourceKindV1\s*::\s*Binary|"
    r"ComponentActivationImmutableResourceElementFormatV1\s*::\s*RawBytes|"
    r"TALAS_KERNEL_ABI_RESOURCE_BINARY|"
    r"TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES|"
    r"ImmutableBinaryResource)\b"
)
COMPONENT_BINARY_RESOURCE_GRAPH_PATTERN = re.compile(
    r"\b(?:compileAndStageIsolatedComponentBinaryResource|"
    r"authorIsolatedComponentBinaryResourceContract|"
    r"exactIsolatedBinaryResourceContract)\w*\b"
)
COMPONENT_BINARY_RESOURCE_CMAKE_PATTERN = re.compile(
    r"\btalas_[A-Za-z0-9_]*binary_resource[A-Za-z0-9_]*\b"
)
BINARY_RESOURCE_GRAPH_TARGET = (
    "talas_component_activation_binary_resource_"
    "transient_graph_macos_integration_tests"
)
BINARY_RESOURCE_TRANSFER_PATHS = frozenset(
    {
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationLoader.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationLoader.cpp"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp"
        ),
    }
)
COMPONENT_AUDIO_STREAM_CONTRACT_PATTERN = re.compile(
    r"\b(?:ComponentActivationAudioStreamGrantV1|"
    r"TalasKernelAbi(?:Immutable)?AudioStream(?:Prepare|Read|ReadResult|View)V3|"
    r"TalasKernelAbiHostServicesV3|TALAS_KERNEL_ABI_AUDIO_STREAM\w*)\b"
)
COMPONENT_AUDIO_STREAM_TRANSFER_PATTERN = re.compile(
    r"\b(?:send|decode|map|validate|create)"
    r"ComponentActivationAudio(?:Source|Stream)(?:Payload|SharedMemory)\w*\b|"
    r"\b(?:createComponentActivationXpcAudioSourceSharedMemory|"
    r"materializeComponentActivationPrivateAudioSource|"
    r"componentActivationAudioSourceSharedMemory)\w*\b"
)
COMPONENT_AUDIO_STREAM_RUNTIME_PATTERN = re.compile(
    r"\b(?:ComponentActivationAudioStream|"
    r"(?:resolve|read)ComponentActivationAudioStream)\b"
)
COMPONENT_AUDIO_STREAM_GRAPH_PATTERN = re.compile(
    r"\b(?:compileAndStageIsolatedComponentAudioStream|"
    r"authorIsolatedComponentAudioStreamContract|"
    r"exactIsolatedAudioStreamContract)\w*\b"
)
COMPONENT_AUDIO_STREAM_BROAD_AUTHORITY_PATTERN = re.compile(
    r"\b(?:(?:(?:std\s*::\s*)?(?:vector|deque|list|span|array))\s*<\s*"
    r"ComponentActivationAudioStreamGrantV1\b|"
    r"ComponentActivation(?:Writable|Network|Remote|Compressed|Reverse|Multiple)"
    r"AudioStream\w*\b|"
    r"ComponentActivationAudioStream(?:Broker|Path|File|Descriptor|Bookmark|"
    r"Url|URL|Socket|Decoder|Writer|Network|Reverse)\w*\b|"
    r"componentActivationAudioStream(?:Broker|Path|File|Descriptor|Bookmark|"
    r"Url|URL|Socket|Decoder|Writer|Network|Reverse)\w*\b|"
    r"TalasKernelAbi(?:Writable|Network|Remote|Compressed|Reverse|Multiple)"
    r"AudioStream\w*\b)"
)
COMPONENT_AUDIO_STREAM_CMAKE_PATTERN = re.compile(
    r"\btalas_component_activation_[A-Za-z0-9_]*audio_stream[A-Za-z0-9_]*\b"
)
AUDIO_STREAM_GRAPH_TARGET = (
    "talas_component_activation_audio_stream_"
    "transient_graph_macos_integration_tests"
)
COMPONENT_MULTIPLE_RESOURCE_CONTRACT_PATTERN = re.compile(
    r"\b(?:ComponentActivationResourceSetGrantV1|"
    r"TalasKernelAbiHostServicesV4)\b"
)
COMPONENT_MULTIPLE_RESOURCE_TRANSFER_PATTERN = re.compile(
    r"\b(?:(?:create|decode|materialize)"
    r"ComponentActivation(?:Xpc)?ResourceSetV1)\b"
)
COMPONENT_MULTIPLE_RESOURCE_RUNTIME_PATTERN = re.compile(
    r"\bComponentActivationPreparedResourceSetV1\b"
)
MULTIPLE_RESOURCE_RUNTIME_VALIDATION_ALLOWLIST = frozenset(
    {
        "src/runtime/component_activation/macos/ComponentActivationLoader.cpp",
        "src/runtime/component_activation/macos/ComponentActivationLoader.h",
    }
)
MULTIPLE_RESOURCE_RUNTIME_TEST_MACRO = (
    "TALAS_COMPONENT_ACTIVATION_MULTIPLE_RESOURCE_RUNTIME_TESTING"
)
MULTIPLE_RESOURCE_RUNTIME_TEST_HELPER = (
    "src/runtime/component_activation/macos/ComponentActivationHelperMain.cpp"
)
MULTIPLE_RESOURCE_RUNTIME_TEST_TARGET = (
    "talas_component_activation_multiple_resource_helper"
)
COMPONENT_MULTIPLE_RESOURCE_GRAPH_PATTERN = re.compile(
    r"\b(?:author|exact)IsolatedComponentResourceSetContract\w*\b"
)
MULTIPLE_RESOURCE_GRAPH_TARGET = (
    "talas_component_activation_multiple_resource_"
    "transient_graph_macos_integration_tests"
)
MULTIPLE_RESOURCE_GRAPH_TEST_MACRO = (
    "TALAS_COMPONENT_ACTIVATION_MULTIPLE_RESOURCE_GRAPH_TESTING"
)
MULTIPLE_RESOURCE_GRAPH_TEST_PATHS = frozenset(
    {
        "src/runtime/Runtime.cpp",
        "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp",
    }
)
COMPONENT_REPLACEMENT_CONTRACT_PATTERN = re.compile(
    r"\b(?:ComponentBundleReplacementPreview|"
    r"previewComponentBundleReplacement|"
    r"calculateProjectPreservedComponentDigest)\b"
)
COMPONENT_REPLACEMENT_AUTHORITY_PATTERN = re.compile(
    r"\b(?:ComponentBundleReplacementConfirmation|"
    r"ComponentBundleReplacementReceipt|"
    r"consumeComponentBundleReplacement|"
    r"compileAndStageIsolatedComponentReplacement)\b"
)
COMPONENT_REPLACEMENT_EXECUTION_PATTERN = re.compile(
    r"\b(?:ComponentBundleReplacementController|"
    r"PreservedNodeAction\s*::\s*Replace)\b"
)
REPLACEMENT_POLICY_HEADER = pathlib.Path(
    "src/runtime/component_bundle/ComponentBundleReplacementPolicy.h"
)
REPLACEMENT_POLICY_SOURCE = pathlib.Path(
    "src/runtime/component_bundle/ComponentBundleReplacementPolicy.cpp"
)
REPLACEMENT_CONFIRMATION_HEADER = pathlib.Path(
    "src/runtime/component_bundle/ComponentBundleReplacementConfirmation.h"
)
REPLACEMENT_CONFIRMATION_SOURCE = pathlib.Path(
    "src/runtime/component_bundle/ComponentBundleReplacementConfirmation.cpp"
)
REPLACEMENT_CONTROLLER_PATHS = frozenset(
    {
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleReplacementController.h"
        ),
        pathlib.PurePosixPath(
            "src/runtime/component_bundle/ComponentBundleReplacementController.cpp"
        ),
    }
)
OWNER_AUTHORIZATION_ACTUAL_PERSISTENCE_PATTERN = re.compile(
    r"(?:#\s*include\s*<\s*(?:fstream)\s*>|"
    r"\bstd::(?:fstream|ofstream)\b|"
    r"\b(?:sqlite3_[A-Za-z0-9_]*|SecItem[A-Za-z0-9_]*|"
    r"SecKeychain[A-Za-z0-9_]*|CFPreferences[A-Za-z0-9_]*|"
    r"NSUserDefaults|fopen|openat?|write)\s*\(|"
    r"[\"'][^\"']*(?:approval|authorization)[^\"']*"
    r"\.(?:json|plist|db|sqlite)[\"'])",
    re.IGNORECASE,
)


def _read_text(path: pathlib.Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        errors.append(f"cannot read {path}: {error}")
        return ""


def _without_multiple_resource_runtime_test_blocks(text: str) -> str:
    return re.sub(
        rf"#if\s+defined\(\s*{MULTIPLE_RESOURCE_RUNTIME_TEST_MACRO}\s*\)"
        rf"[\s\S]*?#endif",
        "",
        text,
    )


def _without_multiple_resource_graph_test_blocks(text: str) -> str:
    return re.sub(
        rf"#if\s+defined\(\s*{MULTIPLE_RESOURCE_GRAPH_TEST_MACRO}\s*\)"
        rf"[\s\S]*?#endif",
        "",
        text,
    )


def evaluate_policy(root: pathlib.Path) -> list[str]:
    root = root.resolve()
    errors: list[str] = []

    policy_path = root / POLICY_PATH
    try:
        payload = json.loads(policy_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot read component ecosystem policy: {error}"]

    if not isinstance(payload, dict):
        return ["component ecosystem policy must be a JSON object"]
    policy_fields = set(PROBE_AUTHORIZED_POLICY)
    unknown = sorted(set(payload) - policy_fields)
    missing = sorted(policy_fields - set(payload))
    if unknown:
        errors.append(f"component ecosystem policy has unknown fields: {unknown}")
    if missing:
        errors.append(f"component ecosystem policy is missing fields: {missing}")
    for key in PROBE_AUTHORIZED_POLICY:
        if key not in payload:
            continue
        allowed_values = {repr(policy[key]) for policy in ACCEPTED_POLICIES}
        if repr(payload[key]) not in allowed_values:
            errors.append(
                f"component ecosystem policy {key} has unsupported value "
                f"{payload[key]!r}; allowed values are "
                f"{sorted(allowed_values)}"
            )
    policy_exact = any(payload == policy for policy in ACCEPTED_POLICIES)
    if not unknown and not missing and not policy_exact:
        errors.append(
            "component ecosystem policy fields must match one complete "
            "authorized state without mixing inactive and active claims"
        )
    replacement_policies = (
        REPLACEMENT_AUTHORIZED_POLICY,
        REPLACEMENT_VALIDATION_POLICY,
        ACTIVE_REPLACEMENT_POLICY,
    )
    multiple_resource_policies = (
        MULTIPLE_RESOURCE_AUTHORIZED_POLICY,
        MULTIPLE_RESOURCE_VALIDATION_POLICY,
        ACTIVE_MULTIPLE_RESOURCE_POLICY,
        *replacement_policies,
    )
    audio_policies = (
        AUDIO_STREAM_AUTHORIZED_POLICY,
        AUDIO_STREAM_VALIDATION_POLICY,
        ACTIVE_AUDIO_STREAM_POLICY,
        *multiple_resource_policies,
    )
    probe_active = payload in (
        ACTIVE_PROBE_POLICY,
        ACTIVATION_AUTHORIZED_POLICY,
        SEALED_RUNTIME_POLICY,
        RUNTIME_VALIDATION_POLICY,
        ACTIVE_ACTIVATION_POLICY,
        RESOURCE_AUTHORIZED_POLICY,
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    runtime_packaged = payload in (
        SEALED_RUNTIME_POLICY,
        RUNTIME_VALIDATION_POLICY,
        ACTIVE_ACTIVATION_POLICY,
        RESOURCE_AUTHORIZED_POLICY,
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    runtime_loading_active = payload in (
        RUNTIME_VALIDATION_POLICY,
        ACTIVE_ACTIVATION_POLICY,
        RESOURCE_AUTHORIZED_POLICY,
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    activation_active = payload in (
        ACTIVE_ACTIVATION_POLICY,
        RESOURCE_AUTHORIZED_POLICY,
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    resource_authorized = payload in (
        RESOURCE_AUTHORIZED_POLICY,
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    resource_runtime_validation = payload in (
        RESOURCE_VALIDATION_POLICY,
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    resource_activation_active = payload in (
        ACTIVE_RESOURCE_POLICY,
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    binary_resource_authorized = payload in (
        BINARY_RESOURCE_AUTHORIZED_POLICY,
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    binary_resource_runtime_validation = payload in (
        BINARY_RESOURCE_VALIDATION_POLICY,
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    binary_resource_activation_active = payload in (
        ACTIVE_BINARY_RESOURCE_POLICY,
        *audio_policies,
    )
    audio_stream_authorized = payload in audio_policies
    audio_stream_runtime_validation = payload in (
        AUDIO_STREAM_VALIDATION_POLICY,
        ACTIVE_AUDIO_STREAM_POLICY,
        *multiple_resource_policies,
    )
    audio_stream_activation_active = payload in (
        ACTIVE_AUDIO_STREAM_POLICY,
        *multiple_resource_policies,
    )
    multiple_resource_authorized = payload in multiple_resource_policies
    multiple_resource_runtime_validation = payload in (
        MULTIPLE_RESOURCE_VALIDATION_POLICY,
        ACTIVE_MULTIPLE_RESOURCE_POLICY,
        *replacement_policies,
    )
    multiple_resource_activation_active = payload in (
        ACTIVE_MULTIPLE_RESOURCE_POLICY,
        *replacement_policies,
    )
    replacement_authorized = payload in replacement_policies
    replacement_runtime_validation = payload in (
        REPLACEMENT_VALIDATION_POLICY,
        ACTIVE_REPLACEMENT_POLICY,
    )
    replacement_validation_only = payload == REPLACEMENT_VALIDATION_POLICY
    replacement_active = payload == ACTIVE_REPLACEMENT_POLICY

    if replacement_authorized:
        replacement_header = _read_text(root / REPLACEMENT_POLICY_HEADER, errors)
        replacement_source = _read_text(root / REPLACEMENT_POLICY_SOURCE, errors)
        replacement_contract = replacement_header + "\n" + replacement_source
        for required in (
            "ComponentBundleReplacementPreview",
            "previewComponentBundleReplacement",
            "calculateProjectPreservedComponentDigest",
            "executable() const noexcept",
        ):
            if required not in replacement_contract:
                errors.append(
                    "explicit replacement authorization requires the pure "
                    f"preview contract token {required!r}"
                )
        if COMPONENT_REPLACEMENT_EXECUTION_PATTERN.search(replacement_contract):
            errors.append(
                "the authorized replacement preview policy must not contain "
                "controller or execution declarations"
            )
        confirmation_contract = "\n".join(
            _read_text(path, errors)
            for path in (
                root / REPLACEMENT_CONFIRMATION_HEADER,
                root / REPLACEMENT_CONFIRMATION_SOURCE,
            )
        )
        for required in (
            "ComponentBundleReplacementReceipt",
            "ComponentBundleReplacementConsumptionLease",
            "ComponentBundleReplacementConfirmationStore",
            "tryConsume",
        ):
            if required not in confirmation_contract:
                errors.append(
                    "explicit replacement authorization requires the opaque "
                    f"one-use confirmation token {required!r}"
                )
        if COMPONENT_REPLACEMENT_EXECUTION_PATTERN.search(confirmation_contract):
            errors.append(
                "replacement confirmation authority must not contain controller "
                "or graph execution declarations"
            )
        replacement_bridge = "\n".join(
            _read_text(path, errors)
            for path in (
                root / "include/talas/runtime/ProjectRuntimeBridge.h",
                root / "src/runtime/ProjectRuntimeBridge.cpp",
            )
        )
        if "compileAndStageIsolatedComponentReplacement" not in replacement_bridge:
            errors.append(
                "explicit replacement authorization requires the exact private "
                "receipt-aware stage-admission seam"
            )
    if replacement_runtime_validation:
        controller_text = "\n".join(
            _read_text(path, errors)
            for path in (
                root / "src/runtime/component_bundle/ComponentBundleReplacementController.h",
                root / "src/runtime/component_bundle/ComponentBundleReplacementController.cpp",
            )
        )
        if not COMPONENT_REPLACEMENT_EXECUTION_PATTERN.search(controller_text):
            errors.append(
                "executable explicit replacement requires the bounded controller"
            )
        if replacement_validation_only:
            workbench_text = "\n".join(
                _read_text(root / path, errors)
                for path in (
                    "src/app/workbench/WorkbenchComponentReplacementPanel.h",
                    "src/app/workbench/WorkbenchComponentReplacementPanel.cpp",
                    "src/app/workbench/WorkbenchComponentReplacement.cpp",
                    "src/app/workbench/WorkbenchInspectorPanel.h",
                    "src/app/workbench/WorkbenchCommands.cpp",
                )
            )
            for required in (
                "ComponentReplacementPanel",
                "beginComponentReplacement",
                "PreservedNodeAction::Replace",
            ):
                if required not in workbench_text:
                    errors.append(
                        "replacement validation requires the tests-only "
                        f"Workbench preview token {required!r}"
                    )
            cmake_text = _read_text(root / "CMakeLists.txt", errors)
            if not re.search(
                r"if\s*\(\s*TALAS_BUILD_TESTS\s*\)[\s\S]*?"
                r"add_library\s*\(\s*talas_component_bundle_replacement_controller\b"
                r"[\s\S]*?ComponentBundleReplacementController\.cpp"
                r"[\s\S]*?endif\s*\(\s*\)",
                cmake_text,
            ):
                errors.append(
                    "replacement validation requires the controller target to be "
                    "confined to TALAS_BUILD_TESTS"
                )
            validation_link_pattern = re.compile(
                r"if\s*\(\s*APPLE\s+AND\s+TALAS_BUILD_TESTS\s*\)"
                r"[\s\S]*?target_link_libraries\s*\(\s*talas_workbench_ui"
                r"[^)]*talas_component_bundle_replacement_controller[^)]*\)"
                r"[\s\S]*?endif\s*\(\s*\)"
            )
            if not validation_link_pattern.search(cmake_text):
                errors.append(
                    "replacement validation requires the Workbench preview "
                    "link to remain confined to APPLE AND TALAS_BUILD_TESTS"
                )
            cmake_without_validation_link = validation_link_pattern.sub(
                "", cmake_text
            )
            if re.search(
                r"target_link_libraries\s*\(\s*talas_workbench(?:_ui)?[^)]*"
                r"talas_component_bundle_replacement_controller",
                cmake_without_validation_link,
            ):
                errors.append(
                    "replacement validation must not link the tests-only "
                    "controller into a tests-off Workbench target"
                )
    if replacement_active:
        workbench_paths = (
            "src/app/workbench/WorkbenchComponentReplacementPanel.h",
            "src/app/workbench/WorkbenchComponentReplacementPanel.cpp",
            "src/app/workbench/WorkbenchComponentReplacement.cpp",
            "src/app/workbench/WorkbenchInspectorPanel.h",
            "src/app/workbench/WorkbenchCommands.cpp",
        )
        workbench_text = "\n".join(
            _read_text(root / path, errors) for path in workbench_paths)
        for required in (
            "ComponentReplacementPanel",
            "beginComponentReplacement",
            "approveCandidate",
            "PreservedNodeAction::Replace",
            "TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT",
        ):
            if required not in workbench_text:
                errors.append(
                    "active explicit replacement requires the owner-selected "
                    f"Workbench token {required!r}"
                )
        product_replacement_source = _read_text(
            root / "src/app/workbench/WorkbenchComponentReplacement.cpp",
            errors,
        )
        if "TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT" in product_replacement_source:
            errors.append(
                "active explicit replacement must not guard the product "
                "Workbench workflow with test support"
            )
        cmake_text = _read_text(root / "CMakeLists.txt", errors)
        if not re.search(
            r"add_library\s*\(\s*talas_component_bundle_replacement_controller\b"
            r"[\s\S]*?ComponentBundleReplacementController\.cpp",
            cmake_text,
        ):
            errors.append(
                "active explicit replacement requires the production controller target"
            )
        if re.search(
            r"if\s*\(\s*TALAS_BUILD_TESTS\s*\)"
            r"(?:(?!endif\s*\(\s*\))[\s\S])*?"
            r"add_library\s*\(\s*talas_component_bundle_replacement_controller\b",
            cmake_text,
        ):
            errors.append(
                "active explicit replacement must not confine the controller "
                "target to TALAS_BUILD_TESTS"
            )
        if "TALAS_COMPONENT_BUNDLE_REPLACEMENT_SUPPORT=1" not in cmake_text:
            errors.append(
                "active explicit replacement requires the production feature definition"
            )
        if not re.search(
            r"if\s*\(\s*TALAS_BUILD_TESTS\s*\)[\s\S]*?"
            r"TALAS_COMPONENT_BUNDLE_REPLACEMENT_TEST_SUPPORT=1"
            r"[\s\S]*?endif\s*\(\s*\)",
            cmake_text,
        ):
            errors.append(
                "active explicit replacement must confine replacement test seams "
                "to TALAS_BUILD_TESTS"
            )
        active_link_pattern = re.compile(
            r"if\s*\(\s*APPLE\s*\)[\s\S]*?"
            r"target_link_libraries\s*\(\s*talas_workbench_ui"
            r"[^)]*talas_component_bundle_replacement_controller[^)]*\)"
            r"[\s\S]*?endif\s*\(\s*\)"
        )
        if not active_link_pattern.search(cmake_text):
            errors.append(
                "active explicit replacement requires tests-off macOS "
                "Workbench linkage"
            )
        if re.search(
            r"if\s*\(\s*APPLE\s+AND\s+TALAS_BUILD_TESTS\s*\)"
            r"[\s\S]*?target_link_libraries\s*\(\s*talas_workbench_ui"
            r"[^)]*talas_component_bundle_replacement_controller",
            cmake_text,
        ):
            errors.append(
                "active explicit replacement must not confine Workbench "
                "linkage to TALAS_BUILD_TESTS"
            )

    header = _read_text(root / HEADER_PATH, errors)
    if not re.search(
        r"#if\s+defined\(__APPLE__\)\s*&&\s*"
        r"defined\(__MACH__\)\s*&&\s*TARGET_OS_OSX[\s\S]*?"
        r"componentIsolationBackendAvailableV1\s*=\s*true\s*;[\s\S]*?"
        r"#else[\s\S]*?"
        r"componentIsolationBackendAvailableV1\s*=\s*false\s*;[\s\S]*?"
        r"#endif",
        header,
    ):
        errors.append(
            "componentIsolationBackendAvailableV1 must be true only for "
            "macOS and false on unsupported platforms"
        )
    if (
        "class ComponentIsolationEvidenceV1" not in header
        or "friend class ComponentIsolationMacOSHost" not in header
        or re.search(
            r"\b(?:ComponentIsolationState\s+isolation|"
            r"isolationProfileVersion|isolationSafeguards|"
            r"isolationSupportedPermissions)\b",
            header,
        )
    ):
        errors.append(
            "ready claims must use opaque isolation evidence constructible "
            "only by ComponentIsolationMacOSHost"
        )
    if (
        "class ComponentProbeEvidence" not in header
        or re.search(r"\bstruct\s+ComponentProbeEvidence\b", header)
    ):
        errors.append(
            "terminal probe claims must use opaque probe evidence rather "
            "than caller-writable fields"
        )

    host_header = _read_text(root / HOST_HEADER_PATH, errors)
    if not re.search(
        r"ComponentIsolationEvidenceV1\s+isolationEvidence\s*\(\s*\)"
        r"\s*const\s+noexcept",
        host_header,
    ):
        errors.append(
            "the macOS host must expose one product-owned isolation evidence adapter"
        )
    audio_data_plane_header = _read_text(
        root / AUDIO_DATA_PLANE_HEADER_PATH,
        errors,
    )
    if not re.search(
        r"componentIsolationTransportPipelineLatencyQuantaV1\s*=\s*1\s*;",
        audio_data_plane_header,
    ):
        errors.append(
            "the product isolation transport must declare exactly one structural pipeline quantum"
        )
    live_evidence = _read_text(root / LIVE_EVIDENCE_PATH, errors)
    if "ComponentIsolationLiveEvidenceStateV1" not in live_evidence:
        errors.append(
            "the backend must bind isolation evidence to one live session state"
        )

    entitlements_path = root / ENTITLEMENTS_PATH
    try:
        entitlements = plistlib.loads(entitlements_path.read_bytes())
    except (OSError, plistlib.InvalidFileException, ValueError) as error:
        errors.append(f"cannot read component-host entitlements: {error}")
    else:
        if entitlements != {"com.apple.security.app-sandbox": True}:
            errors.append(
                "component host must have exactly the App Sandbox entitlement"
            )

    probe_entitlements_path = root / PROBE_ENTITLEMENTS_PATH
    if probe_active:
        try:
            probe_entitlements = plistlib.loads(
                probe_entitlements_path.read_bytes()
            )
        except (OSError, plistlib.InvalidFileException, ValueError) as error:
            errors.append(f"cannot read component-probe entitlements: {error}")
        else:
            if probe_entitlements != {
                "com.apple.security.app-sandbox": True,
                "com.apple.security.cs.disable-library-validation": True,
            }:
                errors.append(
                    "component probe-helper entitlements must contain exactly "
                    "App Sandbox and the reviewed library-validation exception"
                )
    elif probe_entitlements_path.exists():
        errors.append(
            "inactive probe loader policy must not ship probe-helper entitlements"
        )

    runtime_entitlements_path = root / RUNTIME_ENTITLEMENTS_PATH
    if runtime_packaged:
        try:
            runtime_entitlements = plistlib.loads(
                runtime_entitlements_path.read_bytes()
            )
        except (OSError, plistlib.InvalidFileException, ValueError) as error:
            errors.append(f"cannot read component-runtime entitlements: {error}")
        else:
            expected_runtime_entitlements = {
                "com.apple.security.app-sandbox": True,
            }
            if runtime_loading_active:
                expected_runtime_entitlements[
                    "com.apple.security.cs.disable-library-validation"
                ] = True
            if runtime_entitlements != expected_runtime_entitlements:
                errors.append(
                    "component runtime-helper entitlements must match exactly "
                    "the closed sandbox/loading stage"
                )
    elif runtime_entitlements_path.exists():
        errors.append(
            "inactive activation policy must not ship runtime-helper entitlements"
        )

    cmake = _read_text(root / "CMakeLists.txt", errors)
    normalized_cmake = re.sub(r"\(\s+", "(", re.sub(r"\s+", " ", cmake))
    if (
        COMPONENT_BINARY_RESOURCE_CMAKE_PATTERN.search(cmake)
        and not binary_resource_authorized
    ):
        errors.append(
            "binary resource contract requires the exact closed binary resource "
            "policy: CMakeLists.txt"
        )
    if (
        BINARY_RESOURCE_GRAPH_TARGET in cmake
        and not binary_resource_activation_active
    ):
        errors.append(
            "binary resource graph activation requires the exact active binary "
            "resource policy: CMakeLists.txt"
        )
    if (
        COMPONENT_AUDIO_STREAM_CMAKE_PATTERN.search(cmake)
        and not audio_stream_authorized
    ):
        errors.append(
            "audio stream declarations require the exact closed audio stream "
            "policy: CMakeLists.txt"
        )
    if (
        AUDIO_STREAM_GRAPH_TARGET in cmake
        and not audio_stream_activation_active
    ):
        errors.append(
            "audio stream graph activation requires the exact active audio "
            "stream policy: CMakeLists.txt"
        )
    if (
        MULTIPLE_RESOURCE_GRAPH_TARGET in cmake
        and not multiple_resource_activation_active
        and (
            not multiple_resource_runtime_validation
            or MULTIPLE_RESOURCE_GRAPH_TEST_MACRO not in cmake
            or "TALAS_COMPONENT_ACTIVATION_RESOURCE_SET_TRANSIENT_GRAPH=1"
                not in cmake
        )
    ):
        errors.append(
            "multiple resource graph activation requires the exact active "
            "multiple-resource policy: CMakeLists.txt"
        )
    if resource_activation_active:
        active_resource_declarations = {
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp":
                "contract.resources = std::move(resources)",
            "src/runtime/Runtime.cpp": "exactIsolatedResourceContract",
        }
        for relative, declaration in active_resource_declarations.items():
            text = _read_text(root / relative, errors)
            if declaration not in text:
                errors.append(
                    "active immutable-resource policy is missing its exact "
                    f"graph declaration: {relative}"
                )
        if (
            "talas_component_activation_resource_"
            "transient_graph_macos_integration_tests" not in cmake
        ):
            errors.append(
                "active immutable-resource policy requires the real "
                "resource transient-graph integration target"
            )
    if binary_resource_activation_active:
        active_binary_resource_declarations = {
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp":
                "authorIsolatedComponentBinaryResourceContract",
            "src/runtime/Runtime.cpp": "exactIsolatedBinaryResourceContract",
        }
        for relative, declaration in active_binary_resource_declarations.items():
            text = _read_text(root / relative, errors)
            if declaration not in text:
                errors.append(
                    "active immutable-binary-resource policy is missing its exact "
                    f"graph declaration: {relative}"
                )
        if BINARY_RESOURCE_GRAPH_TARGET not in cmake:
            errors.append(
                "active immutable-binary-resource policy requires the real "
                "binary-resource transient-graph integration target"
            )
    if audio_stream_activation_active:
        active_audio_stream_declarations = {
            "include/talas/runtime/ComponentActivationProtocol.h":
                "ComponentActivationAudioStreamGrantV1",
            "src/runtime/component_activation/macos/ComponentActivationXpcEnvelope.cpp":
                "materializeComponentActivationPrivateAudioSourceV1",
            "src/runtime/component_activation/ComponentActivationAudioStream.cpp":
                "class ComponentActivationAudioStream",
            "src/runtime/component_activation/macos/ComponentActivationLoader.cpp":
                "ComponentActivationAudioStreamV1",
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp":
                "authorIsolatedComponentAudioStreamContract",
            "src/runtime/Runtime.cpp": "exactIsolatedAudioStreamContract",
        }
        for relative, declaration in active_audio_stream_declarations.items():
            text = _read_text(root / relative, errors)
            if declaration not in text:
                errors.append(
                    "active bounded-audio-stream policy is missing its exact "
                    f"runtime declaration: {relative}"
                )
        if AUDIO_STREAM_GRAPH_TARGET not in cmake:
            errors.append(
                "active bounded-audio-stream policy requires the real Audio "
                "stream transient-graph integration target"
            )
    if multiple_resource_authorized:
        multiple_resource_contract_declarations = {
            "include/talas/runtime/ComponentActivationProtocol.h": (
                "ComponentActivationResourceSetGrantV1",
            ),
            "include/talas/runtime/KernelAbi.h": (
                "TalasKernelAbiHostServicesV4",
            ),
        }
        for relative, declarations in (
            multiple_resource_contract_declarations.items()
        ):
            text = _read_text(root / relative, errors)
            if any(declaration not in text for declaration in declarations):
                errors.append(
                    "multiple-resource authorization requires its exact "
                    f"closed contract declarations: {relative}"
                )
        protocol = _read_text(
            root / "include/talas/runtime/ComponentActivationProtocol.h",
            errors,
        )
        exact_limit_patterns = (
            r"maximumComponentActivationResourceSetMembersV1\s*=\s*8\s*;",
            r"maximumComponentActivationResourceSetAudioStreamsV1\s*=\s*2\s*;",
            r"maximumComponentActivationResourceSetSourceBytesV1\s*=\s*"
            r"(?:maximumComponentActivationAudioSourceBytesV1|"
            r"64U\s*\*\s*1024U\s*\*\s*1024U)\s*;",
        )
        if any(
            re.search(pattern, protocol) is None
            for pattern in exact_limit_patterns
        ):
            errors.append(
                "multiple-resource authorization requires the exact "
                "8/2/64 MiB aggregate limits: "
                "include/talas/runtime/ComponentActivationProtocol.h"
            )
    if multiple_resource_runtime_validation:
        relative = (
            "src/runtime/component_activation/macos/"
            "ComponentActivationXpcEnvelope.cpp"
        )
        text = _read_text(root / relative, errors)
        if (
            "createComponentActivationXpcResourceSetV1" not in text
            or "materializeComponentActivationResourceSetV1" not in text
        ):
            errors.append(
                "multiple-resource validation requires its exact bounded "
                f"XPC transfer declarations: {relative}"
            )
        helper = _read_text(
            root / MULTIPLE_RESOURCE_RUNTIME_TEST_HELPER,
            errors,
        )
        if (
            COMPONENT_MULTIPLE_RESOURCE_RUNTIME_PATTERN.search(helper)
            and not multiple_resource_activation_active
            and (
                MULTIPLE_RESOURCE_RUNTIME_TEST_MACRO not in helper
                or MULTIPLE_RESOURCE_RUNTIME_TEST_TARGET not in cmake
                or re.search(
                    rf"target_compile_definitions\s*\(\s*"
                    rf"{MULTIPLE_RESOURCE_RUNTIME_TEST_TARGET}\b"
                    rf"[\s\S]*?{MULTIPLE_RESOURCE_RUNTIME_TEST_MACRO}\s*=\s*1"
                    rf"\s*\)",
                    cmake,
                )
                is None
            )
        ):
            errors.append(
                "multiple-resource validation helper execution must remain "
                "inside its exact tests-only target and compile guard"
            )
        if not multiple_resource_activation_active:
            graph_validation_declared = (
                MULTIPLE_RESOURCE_GRAPH_TARGET in cmake
            )
            for relative in sorted(MULTIPLE_RESOURCE_GRAPH_TEST_PATHS):
                text = _read_text(root / relative, errors)
                declared = COMPONENT_MULTIPLE_RESOURCE_GRAPH_PATTERN.search(
                    text
                )
                graph_validation_declared = (
                    graph_validation_declared or declared is not None
                )
                if declared and MULTIPLE_RESOURCE_GRAPH_TEST_MACRO not in text:
                    errors.append(
                        "multiple-resource graph validation must remain inside "
                        f"its exact tests-only compile guard: {relative}"
                    )
            if graph_validation_declared:
                for target in (
                    "talas_runtime",
                    "talas_component_activation_macos_host",
                ):
                    if re.search(
                        rf"target_compile_definitions\s*\(\s*{target}\b"
                        rf"[\s\S]*?{MULTIPLE_RESOURCE_GRAPH_TEST_MACRO}\s*=\s*1"
                        rf"\s*\)",
                        cmake,
                    ) is None:
                        errors.append(
                            "multiple-resource graph validation requires its exact "
                            f"tests-only compile definition on {target}"
                        )
    if multiple_resource_activation_active:
        active_multiple_resource_declarations = {
            "src/runtime/component_activation/macos/ComponentActivationLoader.cpp":
                "ComponentActivationPreparedResourceSetV1",
            "src/runtime/component_activation/macos/ComponentActivationMacOSHost.cpp":
                "authorIsolatedComponentResourceSetContract",
            "src/runtime/Runtime.cpp":
                "exactIsolatedComponentResourceSetContract",
        }
        for relative, declaration in (
            active_multiple_resource_declarations.items()
        ):
            text = _read_text(root / relative, errors)
            if declaration not in _without_multiple_resource_graph_test_blocks(
                text
            ):
                errors.append(
                    "multiple-resource activation requires its exact runtime "
                    f"and graph declaration: {relative}"
                )
        helper = _read_text(
            root / MULTIPLE_RESOURCE_RUNTIME_TEST_HELPER,
            errors,
        )
        if COMPONENT_MULTIPLE_RESOURCE_RUNTIME_PATTERN.search(
            _without_multiple_resource_runtime_test_blocks(helper)
        ) is None:
            errors.append(
                "multiple-resource activation requires unguarded product "
                f"Runtime helper wiring: {MULTIPLE_RESOURCE_RUNTIME_TEST_HELPER}"
            )
        if MULTIPLE_RESOURCE_GRAPH_TARGET not in cmake:
            errors.append(
                "multiple-resource activation requires the real mixed-resource "
                "transient-graph integration target"
            )
    coordinator_integration_function_spans = [
        (match.start(), match.end())
        for match in re.finditer(
            r"\bfunction\s*\(\s*"
            r"talas_add_component_probe_coordinator_integration_test\b"
            r"[\s\S]*?\bendfunction\s*\(\s*\)",
            cmake,
            re.IGNORECASE,
        )
    ]
    fixture_pattern = "|".join(
        re.escape(target) for target in sorted(PROBE_FIXTURE_TARGETS)
    )
    for command_match in re.finditer(
        r"\b(target_link_libraries|add_dependencies)\s*\(\s*"
        r"([^\s)]+)([^)]*)\)",
        cmake,
        re.IGNORECASE | re.DOTALL,
    ):
        command, target, arguments = command_match.groups()
        fixture_used = re.search(
            rf"\b(?:{fixture_pattern})\b",
            arguments,
        )
        coordinator_test_function_target = (
            target == "${target_name}"
            and any(
                start <= command_match.start() < end
                for start, end in coordinator_integration_function_spans
            )
        )
        allowed_fixture_reference = (
            target in PROBE_FIXTURE_REFERENCE_TARGETS
            or target in PROBE_FIXTURE_TARGETS
            or coordinator_test_function_target
        )
        if fixture_used and not allowed_fixture_reference:
            errors.append(
                "test-only component probe fixtures must not enter product "
                f"or non-probe-test targets: {target}"
            )
    if re.search(
        rf"install\s*\([^)]*\b(?:{re.escape(PROBE_VALIDATION_TARGET)}|"
        rf"{fixture_pattern})\b",
        cmake,
        re.IGNORECASE | re.DOTALL,
    ):
        errors.append(
            "private component probe validation must not enter an installed "
            "or exported SDK/product surface"
        )
    for declaration in REQUIRED_BACKEND_DECLARATIONS:
        if declaration not in normalized_cmake:
            errors.append(
                "missing required component-backend packaging declaration: "
                f"{declaration}"
            )
    if probe_active:
        probe_loader_path = root / pathlib.Path(PROBE_LOADER_SOURCE.as_posix())
        if not probe_loader_path.is_file():
            errors.append(
                "the exact component probe loader source is missing"
            )
        probe_coordinator_path = root / pathlib.Path(
            PROBE_COORDINATOR_SOURCE.as_posix()
        )
        if not probe_coordinator_path.is_file():
            errors.append(
                "the exact component probe coordinator source is missing"
            )
        for declaration in REQUIRED_PROBE_BACKEND_DECLARATIONS:
            if declaration not in normalized_cmake:
                errors.append(
                    "missing required component-probe packaging declaration: "
                    f"{declaration}"
                )
        loader_source_occurrences = cmake.count(PROBE_LOADER_SOURCE.as_posix())
        if loader_source_occurrences != 1:
            errors.append(
                "the component probe loader target must own the exact loader "
                "source exactly once"
            )
        probe_target = re.search(
            r"add_executable\s*\(\s*"
            r"talas_component_bundle_probe_helper\s+MACOSX_BUNDLE"
            r"[\s\S]*?\)",
            cmake,
        )
        if (
            probe_target is None
            or PROBE_LOADER_SOURCE.as_posix() not in probe_target.group(0)
        ):
            errors.append(
                "the exact probe loader source must belong only to the "
                "talas_component_bundle_probe_helper target"
            )
    elif (
        PROBE_LOADER_SOURCE.as_posix() in cmake
        or "talas_component_bundle_probe_helper" in cmake
        or "TalasComponentProbe.xpc" in cmake
    ):
        errors.append(
            "inactive probe loader policy must not package a probe loader target"
        )
    if runtime_packaged:
        runtime_loader_path = root / pathlib.Path(
            RUNTIME_LOADER_SOURCE.as_posix()
        )
        if runtime_loading_active and not runtime_loader_path.is_file():
            errors.append("the exact component runtime loader source is missing")
        runtime_host_path = root / pathlib.Path(RUNTIME_HOST_SOURCE.as_posix())
        if not runtime_host_path.is_file():
            errors.append("the exact component runtime host source is missing")
        authorization_path = root / pathlib.Path(
            ACTIVATION_AUTHORIZATION_SOURCE.as_posix()
        )
        if runtime_loading_active and not authorization_path.is_file():
            errors.append(
                "the exact component activation authorization source is missing"
            )
        required_runtime_declarations = REQUIRED_SEALED_RUNTIME_DECLARATIONS
        if runtime_loading_active:
            required_runtime_declarations += (
                REQUIRED_RUNTIME_VALIDATION_DECLARATIONS
            )
        if activation_active:
            required_runtime_declarations += (
                REQUIRED_ACTIVE_ACTIVATION_DECLARATIONS
            )
        for declaration in required_runtime_declarations:
            if declaration not in normalized_cmake:
                errors.append(
                    "missing required component-activation packaging declaration: "
                    f"{declaration}"
                )
        loader_source_occurrences = cmake.count(RUNTIME_LOADER_SOURCE.as_posix())
        if runtime_loading_active:
            if loader_source_occurrences != 1:
                errors.append(
                    "the component runtime loader target must own the exact loader "
                    "source exactly once"
                )
        elif loader_source_occurrences != 0:
            errors.append(
                "the sealed runtime sandbox stage must not own a loader source"
            )
        runtime_target = re.search(
            r"add_executable\s*\(\s*"
            r"talas_component_activation_helper\s+MACOSX_BUNDLE"
            r"[\s\S]*?\)",
            cmake,
        )
        if runtime_target is None:
            errors.append(
                "the sealed runtime target must be the exact "
                "talas_component_activation_helper target"
            )
        elif runtime_loading_active:
            loader_library = re.search(
                r"add_library\s*\(\s*"
                r"talas_component_activation_loader\s+STATIC"
                r"[\s\S]*?"
                + re.escape(RUNTIME_LOADER_SOURCE.as_posix())
                + r"[\s\S]*?\)",
                cmake,
            )
            helper_links_loader = re.search(
                r"target_link_libraries\s*\(\s*"
                r"talas_component_activation_helper\b"
                r"[^)]*\btalas_component_activation_loader\b"
                r"[^)]*\)",
                cmake,
            )
            direct_helper_owner = (
                RUNTIME_LOADER_SOURCE.as_posix() in runtime_target.group(0)
            )
            if loader_library is None and not direct_helper_owner:
                errors.append(
                    "the exact runtime loader source must belong only to the "
                    "talas_component_activation_helper target or its private "
                    "single-source loader library"
                )
            if loader_library is not None and helper_links_loader is None:
                errors.append(
                    "the private runtime loader library must be linked by the "
                    "talas_component_activation_helper target"
                )
            if loader_library is not None:
                loader_consumers = {
                    match.group(1)
                    for match in re.finditer(
                        r"target_link_libraries\s*\(\s*([A-Za-z0-9_]+)"
                        r"[^)]*\btalas_component_activation_loader\b"
                        r"[^)]*\)",
                        cmake,
                    )
                }
                allowed_loader_consumers = {
                    "talas_component_activation_helper",
                    "talas_component_activation_worker",
                    "talas_component_activation_resource_loader_tests",
                    "talas_component_activation_audio_loader_tests",
                    "talas_component_activation_checkpoint_loader_tests",
                }
                if (
                    multiple_resource_runtime_validation
                    and MULTIPLE_RESOURCE_RUNTIME_TEST_TARGET in cmake
                    and MULTIPLE_RESOURCE_RUNTIME_TEST_MACRO in cmake
                ):
                    allowed_loader_consumers.add(
                        MULTIPLE_RESOURCE_RUNTIME_TEST_TARGET
                    )
                unexpected_consumers = sorted(
                    loader_consumers - allowed_loader_consumers
                )
                if unexpected_consumers:
                    errors.append(
                        "the private runtime loader library has an unauthorized "
                        "consumer: " + ", ".join(unexpected_consumers)
                    )
        runtime_host = _read_text(runtime_host_path, errors)
        if "sealRuntimeBeforeComponentBytes" not in runtime_host:
            errors.append(
                "the runtime host must seal its sandbox before component bytes"
            )
        if runtime_loading_active and not re.search(
            r"sealRuntimeBeforeComponentBytes\s*\([^;]*;[\s\S]*"
            r"consumeComponentActivationAuthorization\s*\([^;]*;[\s\S]*"
            r"sendFrozenComponentEntryBytes\s*\([^;]*;",
            runtime_host,
        ):
            errors.append(
                "the activation host must enforce sealed-before-bytes, "
                "single-use authorization, then frozen-byte transfer order"
            )
        authorization = (
            _read_text(authorization_path, errors)
            if runtime_loading_active
            else ""
        )
        if (
            runtime_loading_active
            and "ComponentActivationAuthorizationFactory" not in authorization
        ):
            errors.append(
                "the activation backend must have one private authorization factory"
            )
    elif (
        RUNTIME_LOADER_SOURCE.as_posix() in cmake
        or "talas_component_activation_helper" in cmake
        or "TalasComponentRuntime.xpc" in cmake
    ):
        errors.append(
            "inactive activation policy must not package a runtime loader target"
        )
    if re.search(
        r"install\s*\([^)]*(?:ComponentTrustPolicy\.h|"
        r"DIRECTORY\s+(?:\$\{[^}]+\}/)?include(?:/|\s))",
        cmake,
        re.IGNORECASE | re.DOTALL,
    ) or re.search(
        r"PUBLIC_HEADER[^\n]*ComponentTrustPolicy\.h",
        cmake,
        re.IGNORECASE,
    ):
        errors.append(
            "ComponentTrustPolicy.h must not enter an install/export SDK surface"
        )
    if re.search(
        r"install\s*\([^)]*(?:"
        r"ComponentBundle[A-Za-z0-9_]*\.(?:h|hpp)|"
        r"src/runtime/component_bundle)",
        cmake,
        re.IGNORECASE | re.DOTALL,
    ) or re.search(
        r"PUBLIC_HEADER[^\n]*"
        r"ComponentBundle[A-Za-z0-9_]*\.(?:h|hpp)",
        cmake,
        re.IGNORECASE,
    ):
        errors.append(
            "private component-bundle inspection headers must not enter "
            "an install/export SDK surface"
        )
    if re.search(
        r"install\s*\([^)]*(?:"
        r"(?:ComponentActivation|IsolatedComponentRuntime)"
        r"[A-Za-z0-9_]*\.(?:h|hpp)|"
        r"src/runtime/component_activation)",
        cmake,
        re.IGNORECASE | re.DOTALL,
    ) or re.search(
        r"PUBLIC_HEADER[^\n]*(?:"
        r"ComponentActivation|IsolatedComponentRuntime)"
        r"[A-Za-z0-9_]*\.(?:h|hpp)",
        cmake,
        re.IGNORECASE,
    ):
        errors.append(
            "private component-activation headers must not enter an "
            "install/export SDK surface"
        )

    public_include_root = root / "include"
    if public_include_root.exists():
        for path in sorted(public_include_root.rglob("ComponentBundle*")):
            if path.is_file() and path.suffix in {".h", ".hpp"}:
                errors.append(
                    "private component-bundle inspection header found in "
                    f"public include surface: {path.relative_to(root)}"
                )

    scan_roots = [root / "include", root / "src"]
    for scan_root in scan_roots:
        if not scan_root.exists():
            continue
        for path in sorted(scan_root.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            text = _read_text(path, errors)
            relative_path = path.relative_to(root)
            relative_key = pathlib.PurePosixPath(relative_path.as_posix())
            multiple_resource_runtime_text = text
            multiple_resource_graph_text = text
            if (
                relative_path.as_posix()
                == MULTIPLE_RESOURCE_RUNTIME_TEST_HELPER
                and not multiple_resource_activation_active
            ):
                multiple_resource_runtime_text = (
                    _without_multiple_resource_runtime_test_blocks(text)
                )
            if (
                relative_path.as_posix()
                    in MULTIPLE_RESOURCE_GRAPH_TEST_PATHS
                and not multiple_resource_activation_active
            ):
                multiple_resource_graph_text = (
                    _without_multiple_resource_graph_test_blocks(text)
                )
            is_probe_loader = relative_key == PROBE_LOADER_SOURCE
            is_runtime_loader = relative_key == RUNTIME_LOADER_SOURCE
            if (
                "ComponentActivationAuthorizationFactory" in text
                and relative_key
                not in ACTIVATION_AUTHORIZATION_FACTORY_ALLOWLIST
                and not (
                    replacement_runtime_validation
                    and relative_key
                        == pathlib.PurePosixPath(
                            "src/runtime/component_bundle/ComponentBundleReplacementController.cpp"
                        )
                )
            ):
                errors.append(
                    "the component activation authorization factory must remain "
                    f"inside its private authority adapter: {relative_path}"
                )
            if (
                not resource_authorized
                and COMPONENT_RESOURCE_CONTRACT_PATTERN.search(text)
            ):
                errors.append(
                    "component resource mediation is disabled by the current "
                    f"policy: {relative_path}"
                )
            if (
                not resource_runtime_validation
                and COMPONENT_RESOURCE_RUNTIME_PATTERN.search(text)
            ):
                errors.append(
                    "component resource runtime transfer requires the closed "
                    f"validation state: {relative_path}"
                )
            if (
                not resource_activation_active
                and COMPONENT_RESOURCE_GRAPH_PATTERN.search(text)
            ):
                errors.append(
                    "component resource graph activation requires the exact "
                    f"active resource policy: {relative_path}"
                )
            binary_resource_declaration = COMPONENT_BINARY_RESOURCE_PATTERN.search(
                text
            )
            if binary_resource_declaration and not binary_resource_authorized:
                errors.append(
                    "binary resource contract requires the exact closed binary "
                    f"resource policy: {relative_path}"
                )
            if (
                binary_resource_declaration
                and relative_key in BINARY_RESOURCE_TRANSFER_PATHS
                and not binary_resource_runtime_validation
            ):
                errors.append(
                    "binary resource transfer requires the exact closed validation "
                    f"state: {relative_path}"
                )
            if (
                COMPONENT_BINARY_RESOURCE_GRAPH_PATTERN.search(text)
                and not binary_resource_activation_active
            ):
                errors.append(
                    "binary resource graph activation requires the exact active "
                    f"binary resource policy: {relative_path}"
                )
            if (
                COMPONENT_AUDIO_STREAM_CONTRACT_PATTERN.search(text)
                and not audio_stream_authorized
            ):
                errors.append(
                    "audio stream contract requires the exact closed audio "
                    f"stream policy: {relative_path}"
                )
            if (
                COMPONENT_AUDIO_STREAM_TRANSFER_PATTERN.search(text)
                and not audio_stream_runtime_validation
            ):
                errors.append(
                    "audio stream transfer requires the exact closed validation "
                    f"state: {relative_path}"
                )
            if (
                COMPONENT_AUDIO_STREAM_RUNTIME_PATTERN.search(text)
                and not audio_stream_activation_active
            ):
                errors.append(
                    "audio stream runtime requires the exact active audio stream "
                    f"policy: {relative_path}"
                )
            if (
                COMPONENT_AUDIO_STREAM_GRAPH_PATTERN.search(text)
                and not audio_stream_activation_active
            ):
                errors.append(
                    "audio stream graph activation requires the exact active "
                    f"audio stream policy: {relative_path}"
                )
            if (
                COMPONENT_MULTIPLE_RESOURCE_CONTRACT_PATTERN.search(text)
                and not multiple_resource_authorized
            ):
                errors.append(
                    "multiple resource contract requires the exact closed "
                    f"multiple-resource policy: {relative_path}"
                )
            if (
                COMPONENT_MULTIPLE_RESOURCE_TRANSFER_PATTERN.search(text)
                and not multiple_resource_runtime_validation
            ):
                errors.append(
                    "multiple resource transfer requires the exact closed "
                    f"validation state: {relative_path}"
                )
            if COMPONENT_MULTIPLE_RESOURCE_RUNTIME_PATTERN.search(
                multiple_resource_runtime_text
            ):
                if not multiple_resource_runtime_validation:
                    errors.append(
                        "multiple resource runtime requires the exact closed "
                        f"validation state: {relative_path}"
                    )
                elif (
                    not multiple_resource_activation_active
                    and relative_path.as_posix()
                    not in MULTIPLE_RESOURCE_RUNTIME_VALIDATION_ALLOWLIST
                ):
                    errors.append(
                        "multiple resource runtime execution requires the exact "
                        f"active multiple-resource policy: {relative_path}"
                    )
            if (
                COMPONENT_MULTIPLE_RESOURCE_GRAPH_PATTERN.search(
                    multiple_resource_graph_text
                )
                and not multiple_resource_activation_active
            ):
                errors.append(
                    "multiple resource graph activation requires the exact "
                    f"active multiple-resource policy: {relative_path}"
                )
            if (
                COMPONENT_REPLACEMENT_CONTRACT_PATTERN.search(text)
                and not replacement_authorized
            ):
                errors.append(
                    "explicit replacement preview contract requires the exact "
                    f"replacement-authorized policy: {relative_path}"
                )
            if (
                COMPONENT_REPLACEMENT_AUTHORITY_PATTERN.search(text)
                and not replacement_authorized
            ):
                errors.append(
                    "explicit replacement authority requires the exact "
                    f"replacement-authorized policy: {relative_path}"
                )
            if (
                COMPONENT_REPLACEMENT_EXECUTION_PATTERN.search(text)
                and not replacement_runtime_validation
            ):
                errors.append(
                    "explicit replacement execution requires the exact "
                    f"replacement validation or active policy: {relative_path}"
                )
            if COMPONENT_RESOURCE_BROAD_AUTHORITY_PATTERN.search(text):
                errors.append(
                    "broader component resource authority is forbidden by "
                    f"the immutable single-resource gate: {relative_path}"
                )
            if COMPONENT_AUDIO_STREAM_BROAD_AUTHORITY_PATTERN.search(text):
                errors.append(
                    "broader audio stream authority is forbidden by the "
                    f"single bounded PCM16 read gate: {relative_path}"
                )
            if (
                not activation_active
                and re.search(
                    r"\b(?:compileAndStageIsolatedComponent|"
                    r"compileProjectWithIsolatedComponent)\s*\(",
                    text,
                )
            ):
                errors.append(
                    "runtime validation-only policy must reject executable "
                    f"transient graph activation: {relative_path}"
                )
            if (
                relative_key.parts[:3]
                == ("src", "runtime", "component_activation")
                and re.search(
                    r"\b(?:NodeRegistry|registerBuiltinNodeTypes|"
                    r"makeBuiltinNodeRegistry|addNodeType)\b",
                    text,
                )
            ):
                errors.append(
                    "component activation must use a transient descriptor and "
                    f"must not promote into the canonical registry: {relative_path}"
                )
            if (
                relative_key
                in {
                    pathlib.PurePosixPath("src/project/ProjectFile.cpp"),
                    pathlib.PurePosixPath("include/talas/project/ProjectFile.h"),
                }
                and re.search(
                    r"\b(?:ComponentActivation[A-Za-z0-9_]*|"
                    r"IsolatedComponentRuntime[A-Za-z0-9_]*)\b",
                    text,
                )
            ):
                errors.append(
                    "the isolated activation foundation must not change the "
                    f"canonical project format: {relative_path}"
                )
            if (
                relative_key
                in {
                    pathlib.PurePosixPath("src/core/BuiltinNodes.cpp"),
                    pathlib.PurePosixPath("include/talas/core/BuiltinNodes.h"),
                    pathlib.PurePosixPath("src/core/NodeRegistry.cpp"),
                    pathlib.PurePosixPath("include/talas/core/NodeRegistry.h"),
                    pathlib.PurePosixPath("src/core/NodeCatalogJson.cpp"),
                    pathlib.PurePosixPath("include/talas/core/NodeCatalogJson.h"),
                }
                and re.search(
                    r"\b(?:ComponentActivation[A-Za-z0-9_]*|"
                    r"IsolatedComponentRuntime[A-Za-z0-9_]*)\b",
                    text,
                )
            ):
                errors.append(
                    "the isolated activation foundation must not enter the "
                    f"canonical node catalog: {relative_path}"
                )
            if (
                (
                    relative_key.parts[:2]
                    in {("src", "app"), ("src", "cli")}
                    or relative_key.parts[:3]
                    in {("include", "talas", "app"), ("include", "talas", "cli")}
                )
                and re.search(
                    r"\b(?:activate|install|load)[A-Za-z0-9_]*Component\s*\(",
                    text,
                    re.IGNORECASE,
                )
            ):
                errors.append(
                    "the activation foundation must not expose a Workbench, "
                    f"CLI, or public component action: {relative_path}"
                )
            if (
                "verifyComponentBundleSignatureWithAnchorForTesting" in text
                or "verifyComponentBundleSignatureAtUnixTimeForTesting" in text
            ) and relative_key not in SIGNATURE_TEST_SEAM_ALLOWLIST:
                errors.append(
                    "test-only component signature trust override used in "
                    f"product source: {relative_path}"
                )
            if (
                (
                    "TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT" in text
                    or "publishedAudioResponseReadyForTesting" in text
                    or "publishedResponseReady" in text
                )
                and relative_key not in AUDIO_RESPONSE_TEST_SEAM_ALLOWLIST
            ):
                errors.append(
                    "test-only audio response readiness must not enter "
                    f"product source: {relative_path}"
                )
            if (
                "ComponentSignatureState::Verified" in text
                and relative_key not in SIGNATURE_VERIFIED_TOKEN_ALLOWLIST
            ):
                errors.append(
                    "Verified component signature evidence must be produced "
                    f"only by the private verifier adapter: {relative_path}"
                )
            if (
                "ComponentBundleSignatureEvidenceFactory" in text
                and relative_key not in SIGNATURE_EVIDENCE_FACTORY_ALLOWLIST
            ):
                errors.append(
                    "opaque component signature evidence factory must remain "
                    f"inside the private verifier adapter: {relative_path}"
                )
            if (
                "ComponentBundleProbeEvidenceFactory" in text
                and relative_key not in PROBE_EVIDENCE_FACTORY_ALLOWLIST
            ):
                errors.append(
                    "opaque component probe evidence factory must remain "
                    f"inside the private probe adapter: {relative_path}"
                )
            if (
                "ComponentOwnerAuthorizationStore" in text
                and OWNER_AUTHORIZATION_PERSISTENCE_PATTERN.search(text)
                and not (
                    replacement_runtime_validation
                    and relative_key in REPLACEMENT_CONTROLLER_PATHS
                    and not OWNER_AUTHORIZATION_ACTUAL_PERSISTENCE_PATTERN.search(
                        text
                    )
                )
            ):
                errors.append(
                    "ephemeral component owner authorization must not use "
                    f"file, database, plist, or keychain persistence: {relative_path}"
                )
            if (
                (
                    "resolveComponentBundleDependencies" in text
                    or "validateDeclaredComponentBundleMigrationRoute" in text
                )
                and COMPONENT_METADATA_AUTHORITY_PATTERN.search(text)
            ):
                errors.append(
                    "component dependency and migration decisions must consume "
                    f"immutable in-memory metadata without I/O or execution: {relative_path}"
                )
            if (
                "projectPreservedComponentCompatibility" in text
                and PRESERVATION_COMPATIBILITY_AUTHORITY_PATTERN.search(text)
            ):
                errors.append(
                    "preserved-component compatibility must not mutate the "
                    f"registry/project or contact activation helpers: {relative_path}"
                )
            if relative_key not in BUNDLE_RESOURCE_ALLOWLIST:
                uses_platform_bundle_type = bool(
                    re.search(r"\b(?:NSBundle|CFBundle(?:Ref)?)\b", text)
                ) or (
                    path.suffix == ".swift"
                    and re.search(r"\bBundle\b", text) is not None
                )
                if uses_platform_bundle_type:
                    errors.append(
                        "forbidden platform bundle-loader type in "
                        f"{relative_path}; resource-only use requires an "
                        "explicit reviewed allowlist entry"
                    )
                    continue
            for label, pattern in LOADER_PATTERNS:
                match = pattern.search(text)
                if match:
                    if is_probe_loader or is_runtime_loader:
                        loader_active = (
                            probe_active
                            if is_probe_loader
                            else runtime_loading_active
                        )
                        loader_name = "probe" if is_probe_loader else "runtime"
                        if not loader_active:
                            errors.append(
                                f"inactive {loader_name} loader policy forbids loader "
                                f"primitive {match.group(0).strip()} in "
                                f"{relative_path}"
                            )
                            break
                        if label == "POSIX dynamic loader":
                            if re.search(r"\bdlmopen\s*\(", text):
                                errors.append(
                                    f"the component {loader_name} loader may use only "
                                    "dlopen/dlsym/dlclose, not dlmopen"
                                )
                            continue
                    errors.append(
                        f"forbidden {label} primitive "
                        f"{match.group(0).strip()} in {relative_path}"
                    )
                    break
            if is_probe_loader and probe_active:
                for required_token in ("dlopen", "dlsym"):
                    if required_token not in text:
                        errors.append(
                            "the exact component probe loader must contain "
                            f"the fixed {required_token} primitive"
                        )
            if is_runtime_loader and runtime_loading_active:
                for required_token in ("dlopen", "dlsym"):
                    if required_token not in text:
                        errors.append(
                            "the exact component runtime loader must contain "
                            f"the fixed {required_token} primitive"
                        )

    build_declarations = [root / "CMakeLists.txt"]
    for directory in (root / "cmake", root / "src", root / "include"):
        if directory.exists():
            build_declarations.extend(directory.rglob("CMakeLists.txt"))
            build_declarations.extend(directory.rglob("*.cmake"))
    for path in sorted(set(build_declarations)):
        if not path.is_file():
            continue
        match = LOADER_LINKAGE_PATTERN.search(_read_text(path, errors))
        if match:
            errors.append(
                f"forbidden dynamic loader linkage {match.group(0)!r} "
                f"in {path.relative_to(root)}"
            )

    claim_paths = [root / "README.md", root / "CMakeLists.txt"]
    claim_paths.extend(
        path
        for directory in (root / "src/cli", root / "src/app")
        if directory.exists()
        for path in sorted(directory.rglob("*"))
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )
    for path in claim_paths:
        if not path.is_file():
            continue
        match = PRODUCT_CLAIM_PATTERN.search(_read_text(path, errors))
        if match:
            errors.append(
                f"unsupported executable-component product claim "
                f"{match.group(0)!r} in {path.relative_to(root)}"
            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check the policy-only component ecosystem gate."
    )
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    arguments = parser.parse_args()

    errors = evaluate_policy(arguments.root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Component ecosystem policy gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
