#include "runtime/component_bundle/ComponentBundlePreservationCompatibility.h"
#include "runtime/component_bundle/ComponentBundleReplacementConfirmation.h"
#include "runtime/component_bundle/ComponentBundleReplacementPolicy.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

constexpr std::string_view componentTypeId =
    "org.example.component.processor";

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::array<std::uint8_t, componentTrustDigestBytes> digest(
    std::string_view value)
{
    std::array<std::uint8_t, componentTrustDigestBytes> output{};
    const auto nibble = [](char character) {
        return character <= '9' ? character - '0' : character - 'a' + 10;
    };
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        output[index] = static_cast<std::uint8_t>(
            (nibble(value[index * 2U]) << 4U)
            | nibble(value[index * 2U + 1U]));
    }
    return output;
}

talas::project::ProjectPreservedComponent preservedComponent()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state.clear();
    component.displayName = "Preserved Processor";
    component.bundleId = "org.example.component";
    component.inputs.push_back({
        "audio-in",
        talas::core::PortType::Audio,
        true,
        {
            talas::core::PortSignalRate::Sample,
            talas::core::AudioBusLayoutKind::Stereo,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::NotApplicable,
            true,
        },
    });
    component.outputs.push_back({
        "audio-out",
        talas::core::PortType::Audio,
        false,
        {
            talas::core::PortSignalRate::Sample,
            talas::core::AudioBusLayoutKind::Stereo,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::NotApplicable,
            true,
        },
    });
    component.parameters.push_back({
        "mode",
        talas::core::ParameterKind::Choice,
        talas::expr::ValueDomain::Scalar,
        {"clean", "colour"},
    });
    component.recoveryHint = "Install the exact owner-approved component";
    return component;
}

talas::project::ProjectDocument preservedDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode node;
    node.instanceId = "preserved1";
    node.typeId = std::string(componentTypeId);
    node.parameters.push_back({
        "mode",
        std::nullopt,
        std::string("colour"),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    });
    node.preservedComponent = preservedComponent();
    document.graph.nodes.push_back(std::move(node));
    return document;
}

bundle::ComponentBundleSnapshot candidateSnapshot(
    std::string version = "1.2.3",
    std::uint32_t stateSchemaVersion = 4,
    bool renameOutput = false)
{
    const auto preserved = preservedComponent();
    bundle::ComponentBundleManifest manifest;
    manifest.bundleId = preserved.bundleId;
    manifest.semanticVersion = std::move(version);
    manifest.minimumEngineAbiVersion = 1;
    manifest.maximumEngineAbiVersion = 1;
    manifest.minimumEngineApiVersion = 1;
    manifest.maximumEngineApiVersion = 1;
    manifest.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    manifest.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    manifest.entry.path = "Payload/component.bin";
    manifest.entry.byteCount = 3;
    manifest.entry.sha256 = digest(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");

    bundle::ComponentBundleNode node;
    node.typeId = std::string(componentTypeId);
    node.stateSchemaVersion = stateSchemaVersion;
    for (const auto& port : preserved.inputs)
    {
        node.inputs.push_back({
            port.portId,
            port.type,
            port.required,
            port.signal,
        });
    }
    for (const auto& port : preserved.outputs)
    {
        node.outputs.push_back({
            renameOutput ? "renamed-output" : port.portId,
            port.type,
            port.required,
            port.signal,
        });
    }
    for (const auto& parameter : preserved.parameters)
    {
        node.parameters.push_back({
            parameter.parameterId,
            parameter.kind,
            parameter.domain,
            parameter.choices,
        });
    }
    manifest.nodes.push_back(std::move(node));
    manifest.state.currentSchemaVersion = stateSchemaVersion;
    manifest.provenance.publisher = "Example";
    manifest.provenance.licenseExpression = "Apache-2.0";

    const auto canonical =
        bundle::writeCanonicalComponentBundleManifest(manifest);
    std::string canonicalErrors;
    for (const auto& error : canonical.errors)
        canonicalErrors += " [" + error.path + ": " + error.summary + "]";
    require(
        canonical.ok(),
        "compatibility fixture should canonicalize" + canonicalErrors);
    bundle::ComponentBundleSnapshot result;
    result.manifest = std::move(manifest);
    result.canonicalManifest = canonical.canonicalJson;
    result.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        "Payload/component.bin",
        digest(
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad"),
        {'a', 'b', 'c'},
        true,
    });
    result.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(result);
    require(
        bundle::validateComponentBundleSnapshotIdentity(result),
        "compatibility fixture should be a valid immutable snapshot");
    return result;
}

ComponentTrustEvidence probeEligibleEvidence(
    const bundle::ComponentBundleSnapshot& snapshot,
    bundle::ComponentOwnerAuthorizationStore& authorizationStore,
    bundle::ComponentOwnerAuthorizationAction action =
        bundle::ComponentOwnerAuthorizationAction::Approve)
{
    ComponentTrustEvidence evidence;
    evidence.candidate.bundleId = snapshot.manifest.bundleId;
    evidence.candidate.semanticVersion = snapshot.manifest.semanticVersion;
    evidence.candidate.contentDigest = snapshot.contentDigest;
    evidence.candidate.signerIdentityDigest.fill(0x44);
    evidence.candidate.requestedPermissions =
        snapshot.manifest.requestedPermissions;
    evidence.candidate.policyVersion = currentComponentTrustPolicyVersion;
    evidence.candidate.hasContentDigest = true;
    evidence.candidate.hasSignerIdentity = true;
    const auto recorded = authorizationStore.recordOwnerAction(
        evidence.candidate,
        action,
        authorizationStore.generation());
    require(recorded.ok(), "authorization fixture should apply");
    evidence.ownerAuthorizationEvidence =
        authorizationStore.authorizationEvidence();
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = evidence.candidate.contentDigest;
    signature.signerIdentityDigest =
        evidence.candidate.signerIdentityDigest;
    signature.profileVersion = currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    evidence.artifactGeneration = 1;
    evidence.validationNonce.fill(0x55);
    evidence.hasValidationNonce = true;
    evidence.declaredRoles = snapshot.manifest.declaredRoles;
    return evidence;
}

std::unique_ptr<probe::ComponentBundleProbeValidationSessionV1>
bindTerminalProbe(
    ComponentTrustEvidence& evidence,
    ComponentProbeState state,
    probe::ComponentBundleProbeResultDiagnosticV1 diagnostic)
{
    probe::ComponentBundleProbeRequestV1 request;
    request.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.bundleIdBytes = evidence.candidate.bundleId.size();
    request.semanticVersionBytes = evidence.candidate.semanticVersion.size();
    std::copy(
        evidence.candidate.bundleId.begin(),
        evidence.candidate.bundleId.end(),
        request.bundleId.begin());
    std::copy(
        evidence.candidate.semanticVersion.begin(),
        evidence.candidate.semanticVersion.end(),
        request.semanticVersion.begin());
    request.contentDigest = evidence.candidate.contentDigest;
    request.signerIdentityDigest = evidence.candidate.signerIdentityDigest;
    request.requestedPermissions = evidence.candidate.requestedPermissions;
    request.declaredRoles = evidence.declaredRoles;
    request.policyVersion = evidence.candidate.policyVersion;
    request.entryByteCount = 3;
    request.artifactGeneration = evidence.artifactGeneration;
    request.validationNonce = evidence.validationNonce;

    std::optional<probe::ComponentBundleProbeEvidenceAuthoringV1> authored;
    if (state == ComponentProbeState::AbnormalTermination
        || state == ComponentProbeState::TimedOut
        || state == ComponentProbeState::ProtocolError)
    {
        authored = probe::ComponentBundleProbeEvidenceFactory::
            makeParentClassifiedFailure(state, request);
    }
    else
    {
        probe::ComponentBundleProbeResultV1 result;
        result.header = probe::componentBundleProbeHeaderV1(
            probe::ComponentBundleProbeRecordKindV1::Result,
            sizeof(result));
        result.state = static_cast<std::uint32_t>(state);
        result.diagnostic = static_cast<std::uint32_t>(diagnostic);
        result.contentDigest = request.contentDigest;
        result.artifactGeneration = request.artifactGeneration;
        result.validationNonce = request.validationNonce;
        authored = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(result, request);
    }
    require(authored.has_value(), "terminal probe fixture should author");
    evidence.probe = authored->evidence;
    return std::make_unique<
        probe::ComponentBundleProbeValidationSessionV1>(
        std::move(authored->session));
}

void requirePureProjection(
    talas::project::ProjectDocument& document,
    const bundle::ComponentBundleSnapshot* snapshot,
    const ComponentTrustEvidence* evidence,
    bundle::ComponentBundlePreservationCode expected,
    const std::string& message)
{
    const auto serializedBefore =
        talas::project::serializeProjectDocument(document);
    const auto capsuleBefore =
        *document.graph.nodes.front().preservedComponent;
    const auto snapshotBefore = snapshot != nullptr
        ? std::optional<bundle::ComponentBundleSnapshot>(*snapshot)
        : std::nullopt;
    const auto evidenceBefore = evidence != nullptr
        ? std::optional<ComponentTrustEvidence>(*evidence)
        : std::nullopt;

    const auto result = bundle::projectPreservedComponentCompatibility(
        document.graph.nodes.front().typeId,
        *document.graph.nodes.front().preservedComponent,
        snapshot,
        evidence);

    require(
        result.code == expected && !result.executable()
            && talas::project::serializeProjectDocument(document)
                == serializedBefore
            && *document.graph.nodes.front().preservedComponent
                == capsuleBefore
            && (snapshot == nullptr || *snapshot == *snapshotBefore)
            && (evidence == nullptr || *evidence == *evidenceBefore),
        message + "; projection must leave project, capsule, candidate, and evidence byte-stable");
}

void testEveryTrustAndCompatibilityOutcomePreservesTheProject()
{
    auto document = preservedDocument();
    auto candidate = candidateSnapshot();
    bundle::ComponentOwnerAuthorizationStore eligibleStore;
    auto eligible = probeEligibleEvidence(candidate, eligibleStore);

    requirePureProjection(
        document,
        nullptr,
        nullptr,
        bundle::ComponentBundlePreservationCode::MissingCandidate,
        "missing candidates should remain preserved");

    auto invalidSnapshot = candidate;
    invalidSnapshot.payloads.front().bytes.front() ^= 0xffU;
    requirePureProjection(
        document,
        &invalidSnapshot,
        &eligible,
        bundle::ComponentBundlePreservationCode::InvalidSnapshot,
        "invalid inspected identity should reject");

    requirePureProjection(
        document,
        &candidate,
        nullptr,
        bundle::ComponentBundlePreservationCode::TrustEvidenceMissing,
        "missing trust evidence should reject");

    auto invalidCapsuleDocument = preservedDocument();
    invalidCapsuleDocument.graph.nodes.front()
        .preservedComponent->typeVersion = "01.2.3";
    requirePureProjection(
        invalidCapsuleDocument,
        &candidate,
        &eligible,
        bundle::ComponentBundlePreservationCode::InvalidPreservedCapsule,
        "invalid preserved capsules should remain byte-stable");

    auto rejected = eligible;
    auto rejectedSignature = rejected.signature.inspect();
    rejectedSignature.state = ComponentSignatureState::Invalid;
    rejected.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(
            rejectedSignature);
    requirePureProjection(
        document,
        &candidate,
        &rejected,
        bundle::ComponentBundlePreservationCode::CandidateRejected,
        "rejected signature evidence should remain preserved");

    bundle::ComponentOwnerAuthorizationStore deniedStore;
    auto denied = probeEligibleEvidence(
        candidate,
        deniedStore,
        bundle::ComponentOwnerAuthorizationAction::Deny);
    requirePureProjection(
        document,
        &candidate,
        &denied,
        bundle::ComponentBundlePreservationCode::OwnerDenied,
        "owner denial should remain preserved");

    auto signerMismatch = eligible;
    auto mismatchSignature = signerMismatch.signature.inspect();
    mismatchSignature.signerIdentityDigest.front() ^= 0xffU;
    signerMismatch.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(
            mismatchSignature);
    requirePureProjection(
        document,
        &candidate,
        &signerMismatch,
        bundle::ComponentBundlePreservationCode::SignerMismatch,
        "signature evidence for another signer should remain preserved");

    auto unapproved = eligible;
    unapproved.ownerAuthorizationEvidence = {};
    requirePureProjection(
        document,
        &candidate,
        &unapproved,
        bundle::ComponentBundlePreservationCode::OwnerApprovalMissing,
        "unapproved candidates should remain preserved");

    auto otherCandidate = candidateSnapshot("1.2.4");
    bundle::ComponentOwnerAuthorizationStore otherStore;
    auto otherEvidence = probeEligibleEvidence(otherCandidate, otherStore);
    requirePureProjection(
        document,
        &otherCandidate,
        &otherEvidence,
        bundle::ComponentBundlePreservationCode::VersionMismatch,
        "semantic-version mismatch should remain preserved");

    auto otherBundleDocument = preservedDocument();
    otherBundleDocument.graph.nodes.front()
        .preservedComponent->bundleId = "org.example.other";
    requirePureProjection(
        otherBundleDocument,
        &candidate,
        &eligible,
        bundle::ComponentBundlePreservationCode::BundleMismatch,
        "bundle identity mismatch should remain preserved");

    auto stateMismatch = candidateSnapshot("1.2.3", 5);
    bundle::ComponentOwnerAuthorizationStore stateStore;
    auto stateEvidence = probeEligibleEvidence(stateMismatch, stateStore);
    requirePureProjection(
        document,
        &stateMismatch,
        &stateEvidence,
        bundle::ComponentBundlePreservationCode::StateSchemaMismatch,
        "state-schema mismatch should remain preserved");

    auto interfaceMismatch = candidateSnapshot("1.2.3", 4, true);
    bundle::ComponentOwnerAuthorizationStore interfaceStore;
    auto interfaceEvidence =
        probeEligibleEvidence(interfaceMismatch, interfaceStore);
    requirePureProjection(
        document,
        &interfaceMismatch,
        &interfaceEvidence,
        bundle::ComponentBundlePreservationCode::InterfaceMismatch,
        "frozen interface mismatch should remain preserved");

    auto wrongFingerprint = eligible;
    wrongFingerprint.candidate.contentDigest.front() ^= 0xffU;
    bundle::ComponentOwnerAuthorizationStore wrongStore;
    const auto wrongApproval = wrongStore.recordOwnerAction(
        wrongFingerprint.candidate,
        bundle::ComponentOwnerAuthorizationAction::Approve,
        wrongStore.generation());
    require(wrongApproval.ok(), "wrong-fingerprint fixture should approve");
    wrongFingerprint.ownerAuthorizationEvidence =
        wrongStore.authorizationEvidence();
    auto wrongSignature = wrongFingerprint.signature.inspect();
    wrongSignature.contentDigest = wrongFingerprint.candidate.contentDigest;
    wrongFingerprint.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(wrongSignature);
    requirePureProjection(
        document,
        &candidate,
        &wrongFingerprint,
        bundle::ComponentBundlePreservationCode::CandidateFingerprintMismatch,
        "trust evidence for another snapshot should remain preserved");

    requirePureProjection(
        document,
        &candidate,
        &eligible,
        bundle::ComponentBundlePreservationCode::CompatibleForProbeOnly,
        "exact compatible metadata must remain non-executable and preserved");
}

void testTrustClassificationNeverPromotesActivation()
{
    require(
        bundle::classifyComponentTrustForPreservation({
            ComponentTrustDisposition::EligibleForProbe,
            ComponentTrustDiagnosticCode::ProbeRequired})
                == bundle::ComponentBundlePreservationCode::CompatibleForProbeOnly
            && bundle::classifyComponentTrustForPreservation({
                ComponentTrustDisposition::EligibleForIsolatedActivation,
                ComponentTrustDiagnosticCode::Ready})
                == bundle::ComponentBundlePreservationCode::ActivationForbidden
            && bundle::classifyComponentTrustForPreservation({
                ComponentTrustDisposition::EligibleForProbe,
                ComponentTrustDiagnosticCode::ApprovalDenied})
                == bundle::ComponentBundlePreservationCode::CandidateRejected,
        "preservation classification must reject activation and malformed decision pairs");
}

void testTerminalProbeOutcomesAndRevocationPreserveProjectBytes()
{
    auto document = preservedDocument();
    const auto candidate = candidateSnapshot();

    const auto requireTerminal = [&](
                                     ComponentProbeState state,
                                     probe::ComponentBundleProbeResultDiagnosticV1
                                         diagnostic,
                                     std::string_view label) {
        bundle::ComponentOwnerAuthorizationStore store;
        auto evidence = probeEligibleEvidence(candidate, store);
        const auto session = bindTerminalProbe(evidence, state, diagnostic);
        requirePureProjection(
            document,
            &candidate,
            &evidence,
            bundle::ComponentBundlePreservationCode::CandidateRejected,
            std::string(label));
        require(
            session != nullptr,
            std::string(label) + " should retain live evidence while projected");
    };

    requireTerminal(
        ComponentProbeState::Rejected,
        probe::ComponentBundleProbeResultDiagnosticV1::MetadataRejected,
        "rejected probes should preserve project bytes");
    requireTerminal(
        ComponentProbeState::AbnormalTermination,
        probe::ComponentBundleProbeResultDiagnosticV1::ProcessCrashed,
        "crashed probes should preserve project bytes");
    requireTerminal(
        ComponentProbeState::TimedOut,
        probe::ComponentBundleProbeResultDiagnosticV1::DeadlineExpired,
        "timed-out probes should preserve project bytes");
    requireTerminal(
        ComponentProbeState::Rejected,
        probe::ComponentBundleProbeResultDiagnosticV1::EntryRejected,
        "unsupported-platform image probes should preserve project bytes");

    bundle::ComponentOwnerAuthorizationStore staleStore;
    auto stale = probeEligibleEvidence(candidate, staleStore);
    auto staleSession = bindTerminalProbe(
        stale,
        ComponentProbeState::Clean,
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    staleSession->revoke();
    requirePureProjection(
        document,
        &candidate,
        &stale,
        bundle::ComponentBundlePreservationCode::CandidateRejected,
        "stale probe sessions should preserve project bytes");

    bundle::ComponentOwnerAuthorizationStore revokedStore;
    auto revoked = probeEligibleEvidence(candidate, revokedStore);
    const auto revokedSession = bindTerminalProbe(
        revoked,
        ComponentProbeState::Clean,
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    const auto revokedAction = revokedStore.revokeOwnerAction(
        revoked.candidate,
        revokedStore.generation());
    require(
        revokedAction.ok() && revokedSession != nullptr,
        "live owner approval should revoke before projection");
    requirePureProjection(
        document,
        &candidate,
        &revoked,
        bundle::ComponentBundlePreservationCode::OwnerApprovalMissing,
        "owner-revoked probes should preserve project bytes");
}

void recanonicalize(bundle::ComponentBundleSnapshot& snapshot)
{
    const auto canonical =
        bundle::writeCanonicalComponentBundleManifest(snapshot.manifest);
    require(canonical.ok(), "replacement fixture should canonicalize");
    snapshot.canonicalManifest = canonical.canonicalJson;
    snapshot.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(snapshot);
    require(
        bundle::validateComponentBundleSnapshotIdentity(snapshot),
        "replacement fixture should remain an exact immutable snapshot");
}

bundle::ComponentBundleReplacementPreview replacementPreview(
    const talas::project::ProjectPreservedComponent& preserved,
    const bundle::ComponentBundleSnapshot& candidate,
    const ComponentTrustEvidence& evidence,
    std::span<const bundle::ComponentBundleReplacementResourceTransition>
        transitions = {})
{
    return bundle::previewComponentBundleReplacement({
        componentTypeId,
        &preserved,
        &candidate,
        &evidence,
        transitions,
    });
}

void testReplacementPreviewIsPureExactAndVersionAware()
{
    auto preserved = preservedComponent();
    auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore store;
    const auto evidence = probeEligibleEvidence(candidate, store);
    const auto preview = replacementPreview(
        preserved,
        candidate,
        evidence);
    require(
        preview.compatible() && !preview.executable()
            && preview.versionChanged
            && preview.preservedBundleId == preserved.bundleId
            && preview.candidateBundleId == preserved.bundleId
            && preview.preservedVersion == "1.2.3"
            && preview.candidateVersion == "2.0.0"
            && preview.candidateContentDigest == candidate.contentDigest
            && preview.candidateSignerIdentityDigest
                == evidence.candidate.signerIdentityDigest
            && preview.stateSchemaVersion == preserved.stateSchemaVersion
            && preview.inputs == candidate.manifest.nodes.front().inputs
            && preview.outputs == candidate.manifest.nodes.front().outputs
            && preview.parameters
                == candidate.manifest.nodes.front().parameters,
        "an exact same-identity version change should produce a non-executable owner-confirmation preview");

    const auto digestOne =
        bundle::calculateProjectPreservedComponentDigest(
            componentTypeId,
            preserved);
    const auto digestAgain =
        bundle::calculateProjectPreservedComponentDigest(
            componentTypeId,
            preserved);
    preserved.state.push_back(0xffU);
    const auto changedDigest =
        bundle::calculateProjectPreservedComponentDigest(
            componentTypeId,
            preserved);
    require(
        digestOne == digestAgain && digestOne != changedDigest,
        "preserved capsule digest should be deterministic and bind opaque state bytes");
}

void testReplacementPreviewRejectsIdentityInterfaceStateAndTrustDrift()
{
    const auto preserved = preservedComponent();

    auto otherBundle = candidateSnapshot("2.0.0");
    otherBundle.manifest.bundleId = "org.example";
    recanonicalize(otherBundle);
    bundle::ComponentOwnerAuthorizationStore otherBundleStore;
    const auto otherBundleEvidence = probeEligibleEvidence(
        otherBundle,
        otherBundleStore);
    require(
        replacementPreview(
            preserved,
            otherBundle,
            otherBundleEvidence).code
            == bundle::ComponentBundleReplacementCode::BundleMismatch,
        "an explicit replacement must not substitute another bundle identity");

    auto interfaceDrift = candidateSnapshot("2.0.0", 4, true);
    bundle::ComponentOwnerAuthorizationStore interfaceStore;
    const auto interfaceEvidence = probeEligibleEvidence(
        interfaceDrift,
        interfaceStore);
    require(
        replacementPreview(
            preserved,
            interfaceDrift,
            interfaceEvidence).code
            == bundle::ComponentBundleReplacementCode::InterfaceMismatch,
        "replacement must reject renamed stable ports");

    auto stateDrift = candidateSnapshot("2.0.0", 5);
    bundle::ComponentOwnerAuthorizationStore stateStore;
    const auto stateEvidence = probeEligibleEvidence(stateDrift, stateStore);
    require(
        replacementPreview(
            preserved,
            stateDrift,
            stateEvidence).code
            == bundle::ComponentBundleReplacementCode::StateSchemaMismatch,
        "replacement must reject state migration while the gate is closed");

    auto opaqueState = preserved;
    opaqueState.state = {0x41};
    auto opaqueCandidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore opaqueStore;
    const auto opaqueEvidence = probeEligibleEvidence(
        opaqueCandidate, opaqueStore);
    require(
        replacementPreview(
            opaqueState,
            opaqueCandidate,
            opaqueEvidence).code
            == bundle::ComponentBundleReplacementCode::UnsupportedManifestSurface,
        "preview compatibility must reject opaque state that active authorization cannot admit");

    auto nonAudioPreserved = preserved;
    nonAudioPreserved.inputs.front().signal.timelineBearing = false;
    auto nonAudioCandidate = candidateSnapshot("2.0.0");
    nonAudioCandidate.manifest.nodes.front().inputs.front()
        .signal.timelineBearing = false;
    recanonicalize(nonAudioCandidate);
    bundle::ComponentOwnerAuthorizationStore nonAudioStore;
    const auto nonAudioEvidence = probeEligibleEvidence(
        nonAudioCandidate, nonAudioStore);
    require(
        replacementPreview(
            nonAudioPreserved,
            nonAudioCandidate,
            nonAudioEvidence).code
            == bundle::ComponentBundleReplacementCode::UnsupportedManifestSurface,
        "preview compatibility must use the active Audio-port profile");

    auto dependencyDrift = candidateSnapshot("2.0.0");
    dependencyDrift.manifest.dependencies.push_back({
        "org.example.dependency", "1.0.0", "2.0.0", false});
    recanonicalize(dependencyDrift);
    bundle::ComponentOwnerAuthorizationStore dependencyStore;
    const auto dependencyEvidence = probeEligibleEvidence(
        dependencyDrift,
        dependencyStore);
    require(
        replacementPreview(
            preserved,
            dependencyDrift,
            dependencyEvidence).code
            == bundle::ComponentBundleReplacementCode::UnsupportedManifestSurface,
        "replacement must not resolve or install dependencies");

    auto permissionDrift = candidateSnapshot("2.0.0");
    permissionDrift.manifest.declaredRoles |= componentRole(
        ComponentExecutionRole::StateMigrator);
    permissionDrift.manifest.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::StateMigration);
    recanonicalize(permissionDrift);
    bundle::ComponentOwnerAuthorizationStore permissionStore;
    const auto permissionEvidence = probeEligibleEvidence(
        permissionDrift,
        permissionStore);
    require(
        replacementPreview(
            preserved,
            permissionDrift,
            permissionEvidence).code
            == bundle::ComponentBundleReplacementCode::UnsupportedManifestSurface,
        "replacement must not gain state-migration permission through a preview");

    auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore trustStore;
    auto mismatchedEvidence = probeEligibleEvidence(candidate, trustStore);
    mismatchedEvidence.candidate.contentDigest.front() ^= 0xffU;
    require(
        replacementPreview(
            preserved,
            candidate,
            mismatchedEvidence).code
            == bundle::ComponentBundleReplacementCode::CandidateFingerprintMismatch,
        "replacement must reject trust evidence for different bytes");
}

void testReplacementPreviewBindsClosedResourceTransitions()
{
    const auto preserved = preservedComponent();
    const auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore store;
    const auto evidence = probeEligibleEvidence(candidate, store);
    const std::string oldHash(64, 'a');
    const std::string newHash(64, 'b');
    std::array<bundle::ComponentBundleReplacementResourceTransition, 2>
        transitions{{
            {
                {"resource-table", talas::core::ResourceSnapshotKind::Table, 1, oldHash},
                {"resource-table", talas::core::ResourceSnapshotKind::Binary, 2, newHash},
                false,
            },
            {
                {"resource-audio", talas::core::ResourceSnapshotKind::Audio, 3, oldHash},
                {"resource-audio", talas::core::ResourceSnapshotKind::Table, 4, newHash},
                true,
            },
        }};
    const auto preview = replacementPreview(
        preserved,
        candidate,
        evidence,
        transitions);
    require(
        preview.compatible()
            && preview.resourceTransitions == std::vector(
                transitions.begin(), transitions.end()),
        "replacement preview should own the complete bounded transition set");

    transitions[1].requiresExplicitConfirmation = false;
    require(
        replacementPreview(
            preserved,
            candidate,
            evidence,
            transitions).code
            == bundle::ComponentBundleReplacementCode::ResourceTransitionInvalid,
        "Audio cross-kind transition must be explicitly marked for confirmation");

    transitions[1].requiresExplicitConfirmation = true;
    transitions[1].candidate.generation = 3;
    require(
        replacementPreview(
            preserved,
            candidate,
            evidence,
            transitions).code
            == bundle::ComponentBundleReplacementCode::ResourceTransitionInvalid,
        "cross-kind transition without a generation increment must reject");
}

std::array<
    std::uint8_t,
    bundle::componentBundleReplacementSessionNonceBytes>
replacementSessionNonce()
{
    std::array<
        std::uint8_t,
        bundle::componentBundleReplacementSessionNonceBytes> nonce{};
    nonce.fill(0x6d);
    return nonce;
}

bundle::ComponentBundleReplacementPrepareResult prepareReplacement(
    bundle::ComponentBundleReplacementConfirmationStore& confirmation,
    const talas::project::ProjectPreservedComponent& preserved,
    const bundle::ComponentBundleSnapshot& candidate,
    const ComponentTrustEvidence& evidence,
    std::uint64_t documentGeneration = 7)
{
    return confirmation.prepare({
        documentGeneration,
        "preserved1",
        {
            componentTypeId,
            &preserved,
            &candidate,
            &evidence,
            {},
        },
    });
}

bundle::ComponentBundleReplacementConsumptionExpectation
replacementExpectation(
    const bundle::ComponentBundleReplacementPrepareResult& prepared,
    const ComponentTrustEvidence& evidence,
    std::uint64_t documentGeneration = 7)
{
    return {
        documentGeneration,
        "preserved1",
        prepared.preview->preservedCapsuleDigest,
        evidence.candidate,
        prepared.preview->resourceTransitions,
    };
}

void testReplacementConfirmationIsExactOneUseAndFailureAtomic()
{
    const auto preserved = preservedComponent();
    const auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto evidence = probeEligibleEvidence(candidate, owner);
    bundle::ComponentBundleReplacementConfirmationStore confirmation(
        replacementSessionNonce());
    const auto prepared = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence);
    require(
        prepared.ready()
            && confirmation.preview(prepared.previewGeneration)
                == prepared.preview
            && !confirmation.confirm(prepared.previewGeneration + 1U),
        "confirmation store should expose only its exact prepared preview generation");

    auto receipt = confirmation.confirm(prepared.previewGeneration);
    require(
        receipt.has_value() && receipt->active()
            && !confirmation.confirm(prepared.previewGeneration),
        "one exact preview should issue one move-only active receipt");

    auto wrong = replacementExpectation(prepared, evidence, 8);
    require(
        !receipt->tryConsume(wrong).approved() && receipt->active(),
        "wrong document generation must not consume or invalidate the receipt");

    const auto expected = replacementExpectation(prepared, evidence);
    {
        auto attempt = receipt->tryConsume(expected);
        require(
            attempt.approved(),
            "exact expectation should acquire a short replacement stage lease");
    }
    require(
        receipt->active(),
        "an uncommitted failed stage must return the receipt to confirmed state");
    {
        auto accepted = receipt->tryConsume(expected);
        require(accepted.approved(), "exact retry should remain authorized");
        accepted.commit();
    }
    require(
        !receipt->active() && !receipt->tryConsume(expected).approved(),
        "a committed receipt must be consumed exactly once");
}

void testReplacementConfirmationTracksOwnerRevocationAndStoreLifetime()
{
    const auto preserved = preservedComponent();
    const auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto evidence = probeEligibleEvidence(candidate, owner);

    std::optional<bundle::ComponentBundleReplacementReceipt> receipt;
    bundle::ComponentBundleReplacementPrepareResult prepared;
    {
        auto confirmation = std::make_unique<
            bundle::ComponentBundleReplacementConfirmationStore>(
                replacementSessionNonce());
        prepared = prepareReplacement(
            *confirmation,
            preserved,
            candidate,
            evidence);
        receipt = confirmation->confirm(prepared.previewGeneration);
        require(
            receipt.has_value() && receipt->active(),
            "fixture receipt should be active before store destruction");
    }
    require(
        !receipt->active(),
        "confirmation store destruction must invalidate outstanding receipts");

    bundle::ComponentBundleReplacementConfirmationStore confirmation(
        replacementSessionNonce());
    prepared = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence,
        9);
    receipt = confirmation.confirm(prepared.previewGeneration);
    require(receipt.has_value(), "revocation fixture should confirm");
    const auto revoked = owner.revokeOwnerAction(
        evidence.candidate,
        owner.generation());
    require(
        revoked.ok() && !receipt->active()
            && !receipt->tryConsume(
                replacementExpectation(prepared, evidence, 9)).approved(),
        "owner revocation must invalidate an already returned receipt");
}

void testReplacementConsumptionLeaseLinearizesOwnerRevocation()
{
    const auto preserved = preservedComponent();
    const auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto evidence = probeEligibleEvidence(candidate, owner);
    bundle::ComponentBundleReplacementConfirmationStore confirmation(
        replacementSessionNonce());
    const auto prepared = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence,
        11);
    auto receipt = confirmation.confirm(prepared.previewGeneration);
    require(receipt.has_value(), "linearization fixture should confirm");
    auto lease = receipt->tryConsume(
        replacementExpectation(prepared, evidence, 11));
    require(lease.approved(), "linearization fixture should acquire lease");

    auto revocation = std::async(std::launch::async, [&] {
        return owner.revokeOwnerAction(
            evidence.candidate,
            owner.generation());
    });
    require(
        revocation.wait_for(std::chrono::milliseconds(20))
            == std::future_status::timeout,
        "owner revoke must wait while the exact replacement stage lease is held");
    lease = {};
    require(
        revocation.wait_for(std::chrono::seconds(1))
                == std::future_status::ready
            && revocation.get().ok() && !receipt->active(),
        "owner revoke should linearize immediately after a failed stage releases its lease");
}

void testReplacementCancellationOrdersAgainstConsumption()
{
    const auto preserved = preservedComponent();
    const auto candidate = candidateSnapshot("2.0.0");
    bundle::ComponentOwnerAuthorizationStore owner;
    const auto evidence = probeEligibleEvidence(candidate, owner);
    bundle::ComponentBundleReplacementConfirmationStore confirmation(
        replacementSessionNonce());
    const auto prepared = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence,
        13);
    require(
        prepared.ready()
            && prepareReplacement(
                   confirmation,
                   preserved,
                   candidate,
                   evidence,
                   14).code
                == bundle::ComponentBundleReplacementPrepareCode::Busy,
        "one confirmation store must admit only one active preview");
    auto receipt = confirmation.confirm(prepared.previewGeneration);
    require(receipt.has_value(), "cancellation fixture should confirm");
    auto lease = receipt->tryConsume(
        replacementExpectation(prepared, evidence, 13));
    require(lease.approved(), "cancellation fixture should acquire stage lease");
    receipt->cancel();
    require(
        lease.approved(),
        "cancel after stage linearization must not revoke the in-flight short lease");
    lease = {};
    require(
        !confirmation.preview(prepared.previewGeneration),
        "cancelled failed stage must not restore an orphan confirmed receipt");

    const auto replacement = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence,
        14);
    require(
        replacement.ready(),
        "a fully cancelled receipt should release the single-preview slot");

    auto contendedReceipt = confirmation.confirm(
        replacement.previewGeneration);
    require(
        contendedReceipt.has_value()
            && bundle::ComponentBundleReplacementConfirmationTestPeer::
                cancelReceiptWhileStoreLocked(
                    confirmation, *contendedReceipt),
        "fixture should reproduce cancellation after the live-state CAS while the store mutex is unavailable");
    const auto recovered = prepareReplacement(
        confirmation,
        preserved,
        candidate,
        evidence,
        15);
    require(
        recovered.ready(),
        "the next preparation must reconcile a contended cancelled receipt instead of remaining Busy");

    std::array<
        std::uint8_t,
        bundle::componentBundleReplacementSessionNonceBytes> zeroNonce{};
    bundle::ComponentBundleReplacementConfirmationStore invalid(zeroNonce);
    require(
        invalid.prepare({
            1,
            "preserved1",
            {
                componentTypeId,
                &preserved,
                &candidate,
                &evidence,
                {},
            },
        }).code == bundle::ComponentBundleReplacementPrepareCode::InvalidInput,
        "an all-zero session binding must never author a preview receipt");
}
}

int main()
{
    testEveryTrustAndCompatibilityOutcomePreservesTheProject();
    testTrustClassificationNeverPromotesActivation();
    testTerminalProbeOutcomesAndRevocationPreserveProjectBytes();
    testReplacementPreviewIsPureExactAndVersionAware();
    testReplacementPreviewRejectsIdentityInterfaceStateAndTrustDrift();
    testReplacementPreviewBindsClosedResourceTransitions();
    testReplacementConfirmationIsExactOneUseAndFailureAtomic();
    testReplacementConfirmationTracksOwnerRevocationAndStoreLifetime();
    testReplacementConsumptionLeaseLinearizesOwnerRevocation();
    testReplacementCancellationOrdersAgainstConsumption();
    return 0;
}
