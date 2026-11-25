#include "runtime/component_bundle/ComponentBundleReplacementController.h"
#include "runtime/component_activation/ComponentActivationManifestProfile.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ComponentActivationAudioDataPlane.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef TALAS_COMPONENT_REPLACEMENT_FIXTURE_PATH
#error TALAS_COMPONENT_REPLACEMENT_FIXTURE_PATH is required
#endif

extern char** environ;

namespace
{
namespace activation = talas::runtime::component_activation;
namespace bundle = talas::runtime::component_bundle;
using talas::runtime::ComponentExecutionPermission;
using talas::runtime::ComponentExecutionRole;
using talas::runtime::componentPermission;
using talas::runtime::componentRole;

constexpr std::string_view bundleId =
    "org.talas.activation.fixture";
constexpr std::string_view typeId =
    "org.talas.activation.fixture.gain";

void require(bool condition, std::string message)
{
    if (!condition)
        throw std::runtime_error(std::move(message));
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "replacement fixture should be readable");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void writeBytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "replacement fixture bytes should write exactly");
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "replacement fixture text should write exactly");
}

void runOpenSsl(const std::vector<std::string>& arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>("/usr/bin/openssl"));
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t process = 0;
    require(
        ::posix_spawn(
            &process,
            "/usr/bin/openssl",
            nullptr,
            nullptr,
            argv.data(),
            environ)
            == 0,
        "replacement fixture should launch OpenSSL");
    int status = 0;
    require(
        ::waitpid(process, &status, 0) == process && WIFEXITED(status)
            && WEXITSTATUS(status) == 0,
        "replacement fixture OpenSSL command should succeed");
}

talas::core::PortSignalContract audioSignal()
{
    talas::core::PortSignalContract signal;
    signal.rate = talas::core::PortSignalRate::Sample;
    signal.audioLayout = talas::core::AudioBusLayoutKind::Mono;
    signal.edge = talas::core::PortEdgeSemantics::Continuous;
    signal.spectralFormat = talas::core::SpectralFrameFormat::NotApplicable;
    signal.timelineBearing = true;
    return signal;
}

bundle::ComponentBundleSnapshot makeSnapshot()
{
    auto entry = readBytes(TALAS_COMPONENT_REPLACEMENT_FIXTURE_PATH);
    const auto entryDigest =
        activation::calculateComponentActivationByteDigestV1(entry);
    bundle::ComponentBundleManifest manifest;
    manifest.bundleId = std::string(bundleId);
    manifest.semanticVersion = "2.0.0";
    manifest.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    manifest.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    manifest.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    manifest.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    manifest.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    manifest.entry.path = "Payload/gain.bundle";
    manifest.entry.byteCount = entry.size();
    manifest.entry.sha256 = entryDigest;
    bundle::ComponentBundleNode node;
    node.typeId = std::string(typeId);
    node.stateSchemaVersion = 1;
    node.inputs.push_back({
        "in0", talas::core::PortType::Audio, true, audioSignal()});
    node.outputs.push_back({
        "out0", talas::core::PortType::Audio, true, audioSignal()});
    node.parameters.push_back({
        "gain",
        talas::core::ParameterKind::Expression,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    manifest.nodes.push_back(std::move(node));
    manifest.state.currentSchemaVersion = 1;
    manifest.provenance.publisher = "Talas replacement integration";
    manifest.provenance.licenseExpression = "Apache-2.0";
    const auto canonical =
        bundle::writeCanonicalComponentBundleManifest(manifest);
    require(canonical.ok(), "replacement manifest should canonicalize");
    bundle::ComponentBundleSnapshot result;
    result.manifest = std::move(manifest);
    result.canonicalManifest = canonical.canonicalJson;
    result.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        result.manifest.entry.path,
        entryDigest,
        std::move(entry),
        true,
    });
    result.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(result);
    require(
        bundle::validateComponentBundleSnapshotIdentity(result),
        "replacement snapshot should be exact");
    return result;
}

struct SignedBundle
{
    std::filesystem::path bundleDirectory;
    std::vector<std::uint8_t> anchor;
    bundle::ComponentBundleSnapshot snapshot;
};

SignedBundle writeSignedBundle()
{
    const auto identity = std::to_string(::getpid()) + "-"
        + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path()
        / ("talas-replacement-controller-" + identity);
    const auto bundleDirectory = root / "Candidate.talascomponent";
    const auto signingDirectory = root / "Signing";
    std::filesystem::create_directories(bundleDirectory / "Payload");
    std::filesystem::create_directories(signingDirectory);

    auto snapshot = makeSnapshot();
    const auto message = bundle::makeComponentBundleSignatureMessage(
        snapshot.contentDigest);
    const auto messagePath = signingDirectory / "message.bin";
    const auto certificatePem = signingDirectory / "certificate.pem";
    const auto certificateDer = signingDirectory / "certificate.der";
    const auto privateKey = signingDirectory / "private-key.pem";
    const auto signaturePath = signingDirectory / "signature.der";
    writeBytes(messagePath, message);
    runOpenSsl({
        "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-subj", "/CN=KC Replacement Controller", "-keyout",
        privateKey.string(), "-out", certificatePem.string(), "-days",
        "3650",
    });
    runOpenSsl({
        "x509", "-in", certificatePem.string(), "-outform", "DER", "-out",
        certificateDer.string(),
    });
    runOpenSsl({
        "cms", "-sign", "-binary", "-in", messagePath.string(), "-signer",
        certificatePem.string(), "-inkey", privateKey.string(), "-outform",
        "DER", "-out", signaturePath.string(), "-nosmimecap",
    });
    snapshot.detachedSignatureCms = readBytes(signaturePath);
    writeText(bundleDirectory / "manifest.json", snapshot.canonicalManifest);
    writeBytes(
        bundleDirectory / snapshot.manifest.entry.path,
        snapshot.payloads.front().bytes);
    writeBytes(
        bundleDirectory / "signature.cms",
        *snapshot.detachedSignatureCms);
    return {
        bundleDirectory,
        readBytes(certificateDer),
        std::move(snapshot),
    };
}

talas::project::ProjectPreservedPort preservedPort(std::string id)
{
    return {
        std::move(id),
        talas::core::PortType::Audio,
        true,
        audioSignal(),
    };
}

talas::project::ProjectDocument replacementDocument()
{
    talas::project::ProjectPreservedComponent preserved;
    preserved.typeVersion = "1.0.0";
    preserved.stateSchemaVersion = 1;
    preserved.displayName = "Preserved gain";
    preserved.bundleId = std::string(bundleId);
    preserved.inputs = {preservedPort("in0")};
    preserved.outputs = {preservedPort("out0")};
    preserved.parameters.push_back({
        "gain",
        talas::core::ParameterKind::Expression,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    talas::project::ProjectNode component;
    component.instanceId = "component";
    component.typeId = std::string(typeId);
    component.parameters.push_back({
        "gain",
        std::string{"series(0.25, 0.25, 3)[1]"},
    });
    component.preservedComponent = std::move(preserved);
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
        std::move(component),
    };
    document.graph.connections = {
        {"input", "audioOut", "component", "in0"},
        {"component", "out0", "out", "audioIn"},
    };
    return document;
}

bundle::OwnedComponentApprovalFingerprint fingerprint(
    const bundle::ComponentBundleSnapshot& snapshot,
    const talas::runtime::ComponentSignatureEvidence& signature)
{
    bundle::OwnedComponentApprovalFingerprint result;
    result.bundleId = snapshot.manifest.bundleId;
    result.semanticVersion = snapshot.manifest.semanticVersion;
    result.contentDigest = snapshot.contentDigest;
    result.signerIdentityDigest = signature.inspect().signerIdentityDigest;
    result.requestedPermissions = snapshot.manifest.requestedPermissions;
    result.policyVersion = talas::runtime::currentComponentTrustPolicyVersion;
    result.hasContentDigest = true;
    result.hasSignerIdentity = true;
    return result;
}

bundle::ComponentBundleReplacementControllerSnapshot waitForState(
    bundle::ComponentBundleReplacementController& controller,
    std::span<const bundle::ComponentBundleReplacementControllerState>
        expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto current = controller.snapshot();
        if (std::ranges::find(expected, current.state) != expected.end())
            return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return controller.snapshot();
}

void testSignedExactReplacementRunsThroughBothXpcPeers()
{
    auto signedBundle = writeSignedBundle();
    const auto verified =
        bundle::detail::verifyComponentBundleSignatureWithAnchorForTesting(
            signedBundle.snapshot, signedBundle.anchor);
    require(verified.ok(), "replacement signature should verify exactly");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto candidate = fingerprint(signedBundle.snapshot, verified.evidence);
    require(
        owner.recordOwnerAction(
                 candidate.view(),
                 bundle::ComponentOwnerAuthorizationAction::Approve,
                 owner.generation())
            .ok(),
        "replacement owner approval should apply");

    auto document = replacementDocument();
    const auto canonicalBefore = talas::project::serializeProjectDocument(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48'000;
    settings.maxBlockFrames = 256;
    bundle::ComponentBundleReplacementControllerInputs inputs;
    inputs.bridge = &bridge;
    inputs.ownerAuthorization = &owner;
    inputs.document = document;
    inputs.renderSettings = settings;
    inputs.documentGeneration = 17;
    auto liveDocumentGeneration =
        std::make_shared<std::atomic<std::uint64_t>>(17);
    inputs.liveDocumentGeneration = liveDocumentGeneration;
    inputs.nodeId = "component";
    inputs.signatureVerifierForTesting =
        [anchor = signedBundle.anchor](const auto& snapshot) {
            return bundle::detail::
                verifyComponentBundleSignatureWithAnchorForTesting(
                    snapshot, anchor);
        };
    inputs.probeDeadlineProfileForTesting =
        talas::runtime::component_probe::
            ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::TestShort;
    auto prepared = bundle::ComponentBundleReplacementController::prepare(
        std::move(inputs));
    require(prepared.ok(), "replacement controller should prepare");
    require(
        prepared.controller->inspect(signedBundle.bundleDirectory)
            == bundle::ComponentBundleReplacementControllerOperation::Scheduled,
        "replacement inspection should schedule");
    constexpr std::array awaiting{
        bundle::ComponentBundleReplacementControllerState::
            AwaitingConfirmation,
        bundle::ComponentBundleReplacementControllerState::Failed,
    };
    auto preview = waitForState(*prepared.controller, awaiting);
    require(
        preview.state
                == bundle::ComponentBundleReplacementControllerState::
                    AwaitingConfirmation
            && preview.preview != nullptr && preview.preview->compatible()
            && preview.preview->candidateVersion == "2.0.0"
            && preview.preview->preservedVersion == "1.0.0",
        "exact replacement preview should await owner confirmation: "
            + preview.message);
    require(
        prepared.controller->confirm(preview.previewGeneration + 1U)
            == bundle::ComponentBundleReplacementControllerOperation::Rejected,
        "stale replacement confirmation must reject without consuming preview");
    require(
        prepared.controller->confirm(preview.previewGeneration)
            == bundle::ComponentBundleReplacementControllerOperation::Scheduled,
        "exact replacement confirmation should start orchestration");
    constexpr std::array preparedForStage{
        bundle::ComponentBundleReplacementControllerState::Staging,
        bundle::ComponentBundleReplacementControllerState::Failed,
        bundle::ComponentBundleReplacementControllerState::Cancelled,
    };
    const auto stagedBinding = waitForState(
        *prepared.controller, preparedForStage);
    require(
        stagedBinding.state
                == bundle::ComponentBundleReplacementControllerState::Staging
            && prepared.controller->stage()
                == bundle::ComponentBundleReplacementControllerOperation::
                    Scheduled,
        "signed replacement should reach its control-thread stage point: "
            + stagedBinding.message);
    constexpr std::array terminal{
        bundle::ComponentBundleReplacementControllerState::Succeeded,
        bundle::ComponentBundleReplacementControllerState::Failed,
        bundle::ComponentBundleReplacementControllerState::Cancelled,
    };
    const auto completed = waitForState(*prepared.controller, terminal);
    require(
        completed.state
                == bundle::ComponentBundleReplacementControllerState::Succeeded
            && completed.stageOutcome
                == talas::runtime::ProjectStageOutcome::Staged,
        "signed replacement should probe, activate, and stage exactly: "
            + completed.message);
    require(
        talas::project::serializeProjectDocument(document) == canonicalBefore
            && talas::project::serializeProjectDocument(bridge.editor().document())
                == canonicalBefore,
        "controller must never mutate canonical project bytes");
    require(
        prepared.controller->cancel()
            == bundle::ComponentBundleReplacementControllerOperation::Scheduled
            && prepared.controller->snapshot().state
                == bundle::ComponentBundleReplacementControllerState::Cancelled,
        "post-success cancellation should revoke retained probe liveness");
}

void testChangedDocumentGenerationCancelsLateConfirmation()
{
    auto signedBundle = writeSignedBundle();
    bundle::ComponentOwnerAuthorizationStore owner;
    auto document = replacementDocument();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bundle::ComponentBundleReplacementControllerInputs inputs;
    inputs.bridge = &bridge;
    inputs.ownerAuthorization = &owner;
    inputs.document = document;
    inputs.documentGeneration = 23;
    auto liveDocumentGeneration =
        std::make_shared<std::atomic<std::uint64_t>>(23);
    inputs.liveDocumentGeneration = liveDocumentGeneration;
    inputs.nodeId = "component";
    inputs.signatureVerifierForTesting =
        [anchor = signedBundle.anchor](const auto& snapshot) {
            return bundle::detail::
                verifyComponentBundleSignatureWithAnchorForTesting(
                    snapshot, anchor);
        };
    auto prepared = bundle::ComponentBundleReplacementController::prepare(
        std::move(inputs));
    require(prepared.ok(), "cancellation controller should prepare");
    require(
        prepared.controller->inspect(signedBundle.bundleDirectory)
            == bundle::ComponentBundleReplacementControllerOperation::Scheduled,
        "generation-check inspection should schedule");
    constexpr std::array approvalStates{
        bundle::ComponentBundleReplacementControllerState::
            AwaitingOwnerApproval,
        bundle::ComponentBundleReplacementControllerState::Failed,
    };
    const auto approval = waitForState(*prepared.controller, approvalStates);
    require(
        approval.state
                == bundle::ComponentBundleReplacementControllerState::
                    AwaitingOwnerApproval
            && approval.candidate.available
            && approval.candidate.bundleId
                == signedBundle.snapshot.manifest.bundleId
            && approval.candidate.semanticVersion == "2.0.0"
            && approval.candidate.contentDigest
                == signedBundle.snapshot.contentDigest,
        "unapproved exact candidate should expose only its bounded owner-approval projection");
    require(
        prepared.controller->approveCandidate()
            == bundle::ComponentBundleReplacementControllerOperation::Scheduled,
        "exact session-local owner approval should prepare the compatibility preview");
    constexpr std::array awaiting{
        bundle::ComponentBundleReplacementControllerState::
            AwaitingConfirmation,
        bundle::ComponentBundleReplacementControllerState::Failed,
    };
    const auto preview = waitForState(*prepared.controller, awaiting);
    require(
        preview.state
            == bundle::ComponentBundleReplacementControllerState::
                AwaitingConfirmation,
        "generation-check preview should become ready");
    liveDocumentGeneration->store(24, std::memory_order_release);
    require(
        prepared.controller->confirm(preview.previewGeneration)
            == bundle::ComponentBundleReplacementControllerOperation::Rejected,
        "changed document generation must reject late confirmation");
    constexpr std::array terminal{
        bundle::ComponentBundleReplacementControllerState::Cancelled,
        bundle::ComponentBundleReplacementControllerState::Failed,
    };
    const auto cancelled = waitForState(*prepared.controller, terminal);
    require(
        cancelled.state
            == bundle::ComponentBundleReplacementControllerState::Cancelled,
        "changed document generation must cancel without a late stage");
}

void testClosedRenderProfileRejectsBeforeOwnerConfirmation()
{
    const auto document = replacementDocument();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    bundle::ComponentOwnerAuthorizationStore owner;
    auto prepareWith = [&](double sampleRate, std::size_t blockFrames) {
        auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(
            document, registry);
        bundle::ComponentBundleReplacementControllerInputs inputs;
        inputs.bridge = bridge.get();
        inputs.ownerAuthorization = &owner;
        inputs.document = document;
        inputs.renderSettings.sampleRate = sampleRate;
        inputs.renderSettings.maxBlockFrames = blockFrames;
        inputs.documentGeneration = 31;
        auto liveDocumentGeneration =
            std::make_shared<std::atomic<std::uint64_t>>(31);
        inputs.liveDocumentGeneration = liveDocumentGeneration;
        inputs.nodeId = "component";
        auto prepared = bundle::ComponentBundleReplacementController::prepare(
            std::move(inputs));
        return std::pair{std::move(bridge), std::move(prepared)};
    };
    require(
        prepareWith(48'000.0, 256).second.ok(),
        "the closed 48 kHz / 256-frame render profile must prepare");
    require(
        !prepareWith(96'000.0, 256).second.ok(),
        "96 kHz must reject before replacement inspection or confirmation");
    require(
        !prepareWith(48'000.0, 128).second.ok(),
        "128-frame blocks must reject before replacement inspection or confirmation");

    auto threeInputSnapshot = makeSnapshot();
    threeInputSnapshot.manifest.nodes.front().inputs.push_back({
        "in1", talas::core::PortType::Audio, true, audioSignal()});
    threeInputSnapshot.manifest.nodes.front().inputs.push_back({
        "in2", talas::core::PortType::Audio, true, audioSignal()});
    require(
        activation::supportedComponentActivationManifestProfileV1(
            threeInputSnapshot, 3, 1, 0)
            == activation::ComponentActivationManifestProfileV1::Unsupported,
        "three-channel candidates must reject before owner confirmation");

    auto profileMismatch = makeSnapshot();
    profileMismatch.manifest.nodes.front().parameters.push_back({
        "tableA",
        talas::core::ParameterKind::Resource,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    profileMismatch.manifest.nodes.front().parameters.push_back({
        "tableB",
        talas::core::ParameterKind::Resource,
        talas::expr::ValueDomain::Scalar,
        {},
    });
    profileMismatch.manifest.minimumEngineApiVersion =
        TALAS_KERNEL_API_VERSION_V4;
    profileMismatch.manifest.maximumEngineApiVersion =
        TALAS_KERNEL_API_VERSION_V4;
    profileMismatch.manifest.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor)
        | componentRole(ComponentExecutionRole::BoundedAudioStreamConsumer);
    profileMismatch.manifest.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    require(
        activation::supportedComponentActivationManifestProfileV1(
            profileMismatch, 1, 1, 0)
            == activation::ComponentActivationManifestProfileV1::
                MultipleImmutableResources
            && !activation::
                componentActivationManifestClaimsMatchRuntimeProfileV1(
                    profileMismatch,
                    activation::ComponentActivationMacOSProfileV1::
                        MultipleImmutableOnlyResources),
        "two immutable resources must reject an audio-stream-only manifest before owner confirmation");
}
}

int main()
try
{
    testSignedExactReplacementRunsThroughBothXpcPeers();
    testChangedDocumentGenerationCancelsLateConfirmation();
    testClosedRenderProfileRejectsBeforeOwnerConfirmation();
    std::cout << "component bundle replacement controller integration tests passed\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
