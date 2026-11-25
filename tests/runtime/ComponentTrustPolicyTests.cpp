#include "talas/runtime/ComponentTrustPolicy.h"
#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_bundle/ComponentBundleSignatureVerifier.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace talas::runtime;
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

ComponentApprovalFingerprint defaultFingerprint()
{
    ComponentApprovalFingerprint fingerprint;
    fingerprint.bundleId = "org.talas.example-filter";
    fingerprint.semanticVersion = "1.2.3-rc.1+local.7";
    fingerprint.contentDigest.fill(0x5a);
    fingerprint.signerIdentityDigest.fill(0xa5);
    fingerprint.hasContentDigest = true;
    fingerprint.hasSignerIdentity = true;
    fingerprint.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio);
    fingerprint.policyVersion = currentComponentTrustPolicyVersion;
    return fingerprint;
}

ComponentOwnerAuthorizationEvidenceV1 authorizationFor(
    const ComponentApprovalFingerprint& fingerprint,
    bundle::ComponentOwnerAuthorizationAction action =
        bundle::ComponentOwnerAuthorizationAction::Approve)
{
    static std::vector<
        std::unique_ptr<bundle::ComponentOwnerAuthorizationStore>> stores;
    auto store =
        std::make_unique<bundle::ComponentOwnerAuthorizationStore>();
    const auto recorded = store->recordOwnerAction(
        fingerprint,
        action,
        store->generation());
    require(recorded.ok(), "authorization fixture should apply");
    const auto evidence = store->authorizationEvidence();
    stores.push_back(std::move(store));
    return evidence;
}

void bindAuthorization(
    ComponentTrustEvidence& evidence,
    const ComponentApprovalFingerprint& fingerprint,
    bundle::ComponentOwnerAuthorizationAction action =
        bundle::ComponentOwnerAuthorizationAction::Approve)
{
    evidence.ownerAuthorizationEvidence = authorizationFor(fingerprint, action);
}

void bindVerifiedSignature(ComponentTrustEvidence& evidence)
{
    ComponentSignatureEvidenceView signature;
    signature.state = ComponentSignatureState::Verified;
    signature.contentDigest = evidence.candidate.contentDigest;
    signature.signerIdentityDigest =
        evidence.candidate.signerIdentityDigest;
    signature.profileVersion =
        currentComponentSignatureProfileVersion;
    signature.hasContentDigest = true;
    signature.hasSignerIdentity = true;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
}

std::vector<std::unique_ptr<probe::ComponentBundleProbeValidationSessionV1>>&
probeSessions()
{
    static std::vector<std::unique_ptr<
        probe::ComponentBundleProbeValidationSessionV1>> sessions;
    return sessions;
}

void bindProbeView(
    ComponentTrustEvidence& evidence,
    ComponentProbeEvidenceView view)
{
    probe::ComponentBundleProbeRequestV1 request;
    request.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.header.protocolVersion = static_cast<std::uint16_t>(
        view.protocolVersion);
    request.bundleIdBytes = view.candidateFingerprint.bundleId.size();
    request.semanticVersionBytes =
        view.candidateFingerprint.semanticVersion.size();
    std::copy(
        view.candidateFingerprint.bundleId.begin(),
        view.candidateFingerprint.bundleId.end(),
        request.bundleId.begin());
    std::copy(
        view.candidateFingerprint.semanticVersion.begin(),
        view.candidateFingerprint.semanticVersion.end(),
        request.semanticVersion.begin());
    request.contentDigest = view.candidateFingerprint.contentDigest;
    request.signerIdentityDigest =
        view.candidateFingerprint.signerIdentityDigest;
    request.requestedPermissions =
        view.candidateFingerprint.requestedPermissions;
    request.declaredRoles = evidence.declaredRoles;
    request.policyVersion = view.candidateFingerprint.policyVersion;
    request.entryByteCount = 4096;
    request.artifactGeneration = view.artifactGeneration;
    request.validationNonce = view.validationNonce;

    std::optional<probe::ComponentBundleProbeEvidenceAuthoringV1> authored;
    if (view.state == ComponentProbeState::AbnormalTermination
        || view.state == ComponentProbeState::TimedOut
        || view.state == ComponentProbeState::ProtocolError)
    {
        authored = probe::ComponentBundleProbeEvidenceFactory::
            makeParentClassifiedFailure(view.state, request);
    }
    else
    {
        probe::ComponentBundleProbeResultV1 result;
        result.header = probe::componentBundleProbeHeaderV1(
            probe::ComponentBundleProbeRecordKindV1::Result,
            sizeof(result));
        result.state = static_cast<std::uint32_t>(view.state);
        result.diagnostic = static_cast<std::uint32_t>(
            view.state == ComponentProbeState::Clean
                ? probe::ComponentBundleProbeResultDiagnosticV1::Ready
                : view.state == ComponentProbeState::Rejected
                ? probe::ComponentBundleProbeResultDiagnosticV1::
                      MetadataRejected
                : probe::ComponentBundleProbeResultDiagnosticV1::
                      ModuleControlledFatal);
        result.contentDigest = request.contentDigest;
        result.artifactGeneration = request.artifactGeneration;
        result.validationNonce = request.validationNonce;
        authored = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(result, request);
    }
    require(
        authored.has_value(),
        "probe evidence fixture should author state="
            + std::to_string(static_cast<unsigned>(view.state))
            + " protocol=" + std::to_string(view.protocolVersion)
            + " fingerprint="
            + std::to_string(view.hasCandidateFingerprint)
            + " nonce=" + std::to_string(view.hasValidationNonce)
            + " content="
            + std::to_string(
                view.candidateFingerprint.hasContentDigest)
            + " signer="
            + std::to_string(
                view.candidateFingerprint.hasSignerIdentity)
            + " permissions="
            + std::to_string(
                view.candidateFingerprint.requestedPermissions)
            + " policy="
            + std::to_string(view.candidateFingerprint.policyVersion)
            + " generation=" + std::to_string(view.artifactGeneration));
    evidence.probe = authored->evidence;
    probeSessions().push_back(std::make_unique<
        probe::ComponentBundleProbeValidationSessionV1>(
        std::move(authored->session)));
}

void bindProbeState(
    ComponentTrustEvidence& evidence,
    ComponentProbeState state)
{
    ComponentProbeEvidenceView view;
    view.state = state;
    view.candidateFingerprint = evidence.candidate;
    view.validationNonce = evidence.validationNonce;
    view.artifactGeneration = evidence.artifactGeneration;
    view.protocolVersion = currentComponentProbeProtocolVersion;
    view.hasCandidateFingerprint = true;
    view.hasValidationNonce = true;
    bindProbeView(evidence, view);
}

void bindCleanProbe(ComponentTrustEvidence& evidence)
{
    bindProbeState(evidence, ComponentProbeState::Clean);
}

ComponentTrustEvidence defaultEvidence()
{
    ComponentTrustEvidence evidence;
    evidence.candidate = defaultFingerprint();
    bindAuthorization(evidence, evidence.candidate);
    bindVerifiedSignature(evidence);
    evidence.artifactGeneration = 17;
    evidence.validationNonce.fill(0x3c);
    evidence.hasValidationNonce = true;
    evidence.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor);
    return evidence;
}

void requireDecision(
    const ComponentTrustEvidence& evidence,
    ComponentTrustDisposition disposition,
    ComponentTrustDiagnosticCode diagnostic,
    const std::string& message)
{
    const auto result = evaluateComponentTrust(evidence);
    require(
        result.disposition == disposition && result.diagnostic == diagnostic,
        message);
    require(
        evaluateComponentTrust(evidence) == result,
        "the pure evaluator should return an identical repeated decision");
}

void testValidEvidenceAdvancesOnlyThroughClosedStates()
{
    auto evidence = defaultEvidence();
    requireDecision(
        evidence,
        ComponentTrustDisposition::EligibleForProbe,
        ComponentTrustDiagnosticCode::ProbeRequired,
        "verified and exactly approved evidence should be probe-eligible only");

    bindCleanProbe(evidence);
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::IsolationUnavailable,
        "a clean probe must not activate without real isolation");

}

void testSignatureEvidenceNeverAuthorizesByItself()
{
    auto evidence = defaultEvidence();
    evidence.ownerAuthorizationEvidence = {};
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::ApprovalMissing,
        "verified signatures without approval should preserve only");

    bindAuthorization(evidence, evidence.candidate);
    evidence.signature = {};
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::SignatureMissing,
        "approval without a verified signature should preserve only");

    for (const auto [state, code] : {
             std::pair{
                 ComponentSignatureState::Invalid,
                 ComponentTrustDiagnosticCode::SignatureInvalid},
             std::pair{
                 ComponentSignatureState::Unsupported,
                 ComponentTrustDiagnosticCode::SignatureUnsupported},
             std::pair{
                 ComponentSignatureState::Expired,
                 ComponentTrustDiagnosticCode::SignatureExpired},
             std::pair{
                 ComponentSignatureState::Revoked,
                 ComponentTrustDiagnosticCode::SignatureRevoked},
         })
    {
        auto signature = evidence.signature.inspect();
        signature.state = state;
        evidence.signature =
            bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
        requireDecision(
            evidence,
            ComponentTrustDisposition::Reject,
            code,
            "invalid signature evidence should reject with a stable code");
    }
}

void testMalformedCandidateRejectsBeforeMissingSignaturePreservation()
{
    auto evidence = defaultEvidence();
    evidence.candidate.semanticVersion = "01.2.3";
    evidence.signature = {};
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ApprovalInvalid,
        "malformed candidates must reject before missing signature handling");
}

void testVerifiedSignatureIsBoundToTheExactCandidate()
{
    auto evidence = defaultEvidence();
    auto signature = evidence.signature.inspect();
    signature.hasContentDigest = false;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::SignatureEvidenceMismatch,
        "verified signatures require authenticated content evidence");

    evidence = defaultEvidence();
    signature = evidence.signature.inspect();
    signature.contentDigest[3] ^= 0xff;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::SignatureEvidenceMismatch,
        "signature evidence for another artifact must reject");

    evidence = defaultEvidence();
    signature = evidence.signature.inspect();
    signature.signerIdentityDigest[5] ^= 0xff;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::SignatureEvidenceMismatch,
        "signature evidence for another signer must reject");

    evidence = defaultEvidence();
    signature = evidence.signature.inspect();
    ++signature.profileVersion;
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::SignatureProfileUnsupported,
        "unsupported signature-verifier profiles must reject");

    evidence = defaultEvidence();
    evidence.candidate.contentDigest[7] ^= 0xff;
    bindAuthorization(evidence, evidence.candidate);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::SignatureEvidenceMismatch,
        "a candidate replacement cannot reuse old verified signature evidence");
}

void testEvaluatorDerivesExactOwnerApprovalInsteadOfTrustingAClaim()
{
    auto evidence = defaultEvidence();
    auto staleApproval = evidence.candidate;
    staleApproval.contentDigest[11] ^= 0xff;
    bindAuthorization(evidence, staleApproval);
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::ApprovalMissing,
        "approval for different bytes must require exact reapproval");

    evidence = defaultEvidence();
    staleApproval = evidence.candidate;
    staleApproval.signerIdentityDigest[13] ^= 0xff;
    bindAuthorization(evidence, staleApproval);
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::ApprovalMissing,
        "approval for a different signer must not advance");

}

void testApprovalAndProbeFailuresFailClosed()
{
    auto evidence = defaultEvidence();
    auto staleApproval = evidence.candidate;
    staleApproval.semanticVersion = "1.2.4";
    bindAuthorization(evidence, staleApproval);
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::ApprovalMissing,
        "stale approval should require preservation and reapproval");

    evidence = defaultEvidence();
    bindAuthorization(
        evidence,
        evidence.candidate,
        bundle::ComponentOwnerAuthorizationAction::Deny);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ApprovalDenied,
        "explicit owner denial should reject");

    evidence = defaultEvidence();
    for (const auto [state, code] : {
             std::pair{
                 ComponentProbeState::Rejected,
                 ComponentTrustDiagnosticCode::ProbeRejected},
             std::pair{
                 ComponentProbeState::ControlledFatal,
                 ComponentTrustDiagnosticCode::ProbeControlledFatal},
             std::pair{
                 ComponentProbeState::AbnormalTermination,
                 ComponentTrustDiagnosticCode::ProbeCrashed},
             std::pair{
                 ComponentProbeState::TimedOut,
                 ComponentTrustDiagnosticCode::ProbeTimedOut},
             std::pair{
                 ComponentProbeState::ProtocolError,
                 ComponentTrustDiagnosticCode::ProbeProtocolError},
         })
    {
        bindProbeState(evidence, state);
        requireDecision(
            evidence,
            ComponentTrustDisposition::Reject,
            code,
            "failed probe evidence should reject with a stable typed code");
    }
}

void testCleanProbeIsBoundToCandidateAndValidationSession()
{
    auto evidence = defaultEvidence();
    auto bare = ComponentProbeEvidenceView{};
    bare.state = ComponentProbeState::Clean;
    require(
        !std::is_constructible_v<
            ComponentProbeEvidence,
            ComponentProbeEvidenceView>,
        "a bare clean enum must not be authorable as evidence");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    auto probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.contentDigest[17] ^= 0xff;
    bindProbeView(evidence, probeView);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeEvidenceMismatch,
        "clean evidence for another artifact must reject");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.signerIdentityDigest[19] ^= 0xff;
    bindProbeView(evidence, probeView);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeEvidenceMismatch,
        "clean evidence for another signer must reject");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.bundleId = "org.talas.other-filter";
    bindProbeView(evidence, probeView);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeEvidenceMismatch,
        "clean evidence for another bundle identity must reject");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.semanticVersion = "1.2.4";
    bindProbeView(evidence, probeView);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeEvidenceMismatch,
        "clean evidence for another candidate version must reject");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.requestedPermissions |=
        componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
    bool permissionsRejected = false;
    try
    {
        bindProbeView(evidence, probeView);
    }
    catch (const std::runtime_error&)
    {
        permissionsRejected = true;
    }
    require(
        permissionsRejected,
        "clean evidence cannot float across requested-authority changes");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    ++probeView.candidateFingerprint.policyVersion;
    bool policyRejected = false;
    try
    {
        bindProbeView(evidence, probeView);
    }
    catch (const std::runtime_error&)
    {
        policyRejected = true;
    }
    require(
        policyRejected,
        "clean evidence cannot float across policy versions");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.hasContentDigest = false;
    require(
        !std::is_constructible_v<ComponentProbeEvidence, ComponentProbeEvidenceView>,
        "clean evidence requires the complete content fingerprint");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.candidateFingerprint.hasSignerIdentity = false;
    require(
        !std::is_constructible_v<ComponentProbeEvidence, ComponentProbeEvidenceView>,
        "clean evidence requires the complete signer fingerprint");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.hasCandidateFingerprint = false;
    require(
        !std::is_constructible_v<ComponentProbeEvidence, ComponentProbeEvidenceView>,
        "clean evidence requires an explicit candidate-fingerprint binding");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    probeView.hasValidationNonce = false;
    require(
        !std::is_constructible_v<ComponentProbeEvidence, ComponentProbeEvidenceView>,
        "clean evidence requires an explicit validation-session binding");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    probeView = evidence.probe.inspect();
    ++probeView.protocolVersion;
    auto protocolEvidence = evidence;
    bool protocolRejected = false;
    try
    {
        bindProbeView(protocolEvidence, probeView);
    }
    catch (const std::runtime_error&)
    {
        protocolRejected = true;
    }
    require(protocolRejected, "unsupported probe protocols must reject");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    ++evidence.artifactGeneration;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "clean evidence cannot float across artifact generations");

    evidence = defaultEvidence();
    bindCleanProbe(evidence);
    evidence.validationNonce[0] ^= 0xff;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "clean evidence cannot be replayed into another validation session");

    evidence = defaultEvidence();
    auto contradictory = ComponentProbeEvidenceView{};
    contradictory.protocolVersion = currentComponentProbeProtocolVersion;
    require(
        !std::is_constructible_v<ComponentProbeEvidence, ComponentProbeEvidenceView>,
        "not-run probe evidence cannot claim a protocol result");

    evidence = defaultEvidence();
    evidence.artifactGeneration = 0;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "probe eligibility requires a nonzero artifact generation");

    evidence = defaultEvidence();
    evidence.hasValidationNonce = false;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "probe eligibility requires a validation-session nonce");
}

void testRevokedOrExpiredProbeSessionCannotAdvance()
{
    auto evidence = defaultEvidence();
    ComponentProbeEvidenceView view;
    view.state = ComponentProbeState::Clean;
    view.candidateFingerprint = evidence.candidate;
    view.validationNonce = evidence.validationNonce;
    view.artifactGeneration = evidence.artifactGeneration;
    view.protocolVersion = currentComponentProbeProtocolVersion;
    view.hasCandidateFingerprint = true;
    view.hasValidationNonce = true;
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
    request.entryByteCount = 4096;
    request.artifactGeneration = evidence.artifactGeneration;
    request.validationNonce = evidence.validationNonce;
    probe::ComponentBundleProbeResultV1 result;
    result.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(ComponentProbeState::Clean);
    result.diagnostic = static_cast<std::uint32_t>(
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    auto authored = probe::ComponentBundleProbeEvidenceFactory::
        makeFromValidatedResult(result, request);
    require(authored.has_value(), "complete live probe should author");
    evidence.probe = authored->evidence;
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::IsolationUnavailable,
        "a live clean probe should reach only the missing-isolation boundary");
    authored->session.revoke();
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "revoked probe authority must reject before isolation");

    evidence = defaultEvidence();
    {
        auto expiring = probe::ComponentBundleProbeEvidenceFactory::
            makeFromValidatedResult(result, request);
        require(expiring.has_value(), "expiry fixture should author");
        evidence.probe = expiring->evidence;
    }
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::ProbeStale,
        "destroying the validation session must stale retained evidence");
}

void testPermissionsAreClosedAndRequired()
{
    auto evidence = defaultEvidence();
    evidence.candidate.requestedPermissions = 0;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::PermissionsEmpty,
        "an executable candidate must request a known nonempty permission set");

    evidence.candidate.requestedPermissions =
        allComponentExecutionPermissionsV1 | (std::uint32_t{1} << 31);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::PermissionsUnknown,
        "unknown permission bits should reject");

    evidence = defaultEvidence();
    evidence.candidate.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio)
        | componentPermission(ComponentExecutionPermission::ImmutableResourceRead)
        | componentPermission(ComponentExecutionPermission::StateMigration)
        | componentPermission(ComponentExecutionPermission::DeclarativeUi)
        | componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
    evidence.declaredRoles = allComponentExecutionRolesV1;
    bindAuthorization(evidence, evidence.candidate);
    requireDecision(
        evidence,
        ComponentTrustDisposition::EligibleForProbe,
        ComponentTrustDiagnosticCode::ProbeRequired,
        "the complete known v1 permission set should remain valid");
}

void testPermissionsMatchDeclaredRolesExactly()
{
    auto evidence = defaultEvidence();
    evidence.declaredRoles = 0;
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::RolesEmpty,
        "an executable candidate must declare at least one known role");

    evidence = defaultEvidence();
    evidence.declaredRoles =
        allComponentExecutionRolesV1 | (std::uint32_t{1} << 31);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::RolesUnknown,
        "unknown role bits should reject");

    evidence = defaultEvidence();
    evidence.candidate.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::PermissionsRoleMismatch,
        "a permission not justified by a declared role should reject");

    evidence = defaultEvidence();
    evidence.declaredRoles |= componentRole(
        ComponentExecutionRole::ImmutableResourceConsumer);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::PermissionsRoleMismatch,
        "a declared role whose permission was omitted should reject");

    evidence.candidate.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    bindAuthorization(evidence, evidence.candidate);
    requireDecision(
        evidence,
        ComponentTrustDisposition::EligibleForProbe,
        ComponentTrustDiagnosticCode::ProbeRequired,
        "the exact compositional role permission set should be probe-eligible");
}

void testEveryV1RoleMapsToOneDistinctPermissionAuthority()
{
    for (const auto [role, permission] : {
             std::pair{
                 ComponentExecutionRole::RealtimeProcessor,
                 ComponentExecutionPermission::RealtimeAudio},
             std::pair{
                 ComponentExecutionRole::ImmutableResourceConsumer,
                 ComponentExecutionPermission::ImmutableResourceRead},
             std::pair{
                 ComponentExecutionRole::StateMigrator,
                 ComponentExecutionPermission::StateMigration},
             std::pair{
                 ComponentExecutionRole::DeclarativeUiProvider,
                 ComponentExecutionPermission::DeclarativeUi},
             std::pair{
                 ComponentExecutionRole::BoundedAudioStreamConsumer,
                 ComponentExecutionPermission::BoundedAudioStreamRead},
         })
    {
        const auto roles = componentRole(role);
        require(
            componentPermissionsForRoles(roles)
                == componentPermission(permission),
            "each declared role should map to exactly its closed v1 permission");
    }
}

void testIsolationEvidenceIsOpaqueAndUnavailableByDefault()
{
    const ComponentIsolationEvidenceV1 isolation;
    const auto view = isolation.inspect();
    require(
        view == ComponentIsolationEvidenceViewV1{},
        "caller-constructible isolation evidence must be an empty unavailable view");

    auto evidence = defaultEvidence();
    bindCleanProbe(evidence);
    evidence.isolationEvidence = isolation;
    requireDecision(
        evidence,
        ComponentTrustDisposition::PreserveOnly,
        ComponentTrustDiagnosticCode::IsolationUnavailable,
        "default opaque evidence cannot satisfy the containment leg");
}

void testNoCallerConstructibleEvidenceCombinationCanActivateUserCode()
{
    for (const auto signature : {
             ComponentSignatureState::Missing,
             ComponentSignatureState::Verified,
             ComponentSignatureState::Invalid,
             ComponentSignatureState::Unsupported,
             ComponentSignatureState::Expired,
             ComponentSignatureState::Revoked,
         })
    {
        for (const auto probe : {
                 ComponentProbeState::NotRun,
                 ComponentProbeState::Clean,
                 ComponentProbeState::Rejected,
                 ComponentProbeState::ControlledFatal,
                 ComponentProbeState::AbnormalTermination,
                 ComponentProbeState::TimedOut,
                 ComponentProbeState::ProtocolError,
             })
        {
            auto evidence = defaultEvidence();
            auto signatureView = evidence.signature.inspect();
            signatureView.state = signature;
            evidence.signature =
                bundle::ComponentBundleSignatureEvidenceFactory::make(
                    signatureView);
            if (probe != ComponentProbeState::NotRun)
                bindProbeState(evidence, probe);
            require(
                evaluateComponentTrust(evidence).disposition
                    != ComponentTrustDisposition::
                        EligibleForIsolatedActivation,
                "caller-constructible evidence must make activation unreachable");
        }
    }
}

void testReadOnlyProjectionUsesOnlyBoundedProductAuthoredText()
{
    for (const auto decision : {
             ComponentTrustDecision{
                 ComponentTrustDisposition::Reject,
                 ComponentTrustDiagnosticCode::SignatureInvalid},
             ComponentTrustDecision{
                 ComponentTrustDisposition::PreserveOnly,
                 ComponentTrustDiagnosticCode::IsolationBackendUnavailable},
             ComponentTrustDecision{
                 ComponentTrustDisposition::EligibleForProbe,
                 ComponentTrustDiagnosticCode::ProbeRequired},
             ComponentTrustDecision{
                 ComponentTrustDisposition::EligibleForIsolatedActivation,
                 ComponentTrustDiagnosticCode::Ready},
         })
    {
        const auto projection = projectComponentTrust(decision);
        require(
            !projection.disposition.empty()
                && projection.disposition.size()
                    <= maximumComponentTrustProjectionNameBytes
                && !projection.diagnostic.empty()
                && projection.diagnostic.size()
                    <= maximumComponentTrustProjectionNameBytes
                && !projection.summary.empty()
                && projection.summary.size()
                    <= maximumComponentTrustProjectionSummaryBytes,
            "policy projections should use fixed bounded product-authored text");
        require(
            !projection.canExecute && !projection.canInstall,
            "policy-only projections must expose no execution or install action");
        for (const auto forbidden : {
                 "installed",
                 "trusted",
                 "loaded",
                 "runnable",
             })
        {
            require(
                projection.summary.find(forbidden) == std::string_view::npos,
                "policy summaries should not imply unavailable product state");
        }
    }
}

void testProjectionDistinguishesEveryClosedDispositionHonestly()
{
    require(
        projectComponentTrust({
            ComponentTrustDisposition::Reject,
            ComponentTrustDiagnosticCode::SignatureInvalid})
                .disposition
            == "reject",
        "rejected candidates should remain visibly rejected");
    require(
        projectComponentTrust({
            ComponentTrustDisposition::PreserveOnly,
            ComponentTrustDiagnosticCode::IsolationUnavailable})
                .disposition
            == "preserve_only",
        "preservation should not be presented as execution");
    require(
        projectComponentTrust({
            ComponentTrustDisposition::EligibleForProbe,
            ComponentTrustDiagnosticCode::ProbeRequired})
                .disposition
            == "eligible_for_probe",
        "probe eligibility should remain distinct from activation");
    require(
        projectComponentTrust({
            ComponentTrustDisposition::EligibleForIsolatedActivation,
            ComponentTrustDiagnosticCode::Ready})
                .disposition
            == "eligible_for_isolated_activation",
        "future isolated eligibility should remain explicit and non-executing");
}

void testUnknownOrContradictoryProjectionInputFailsClosed()
{
    const auto unknownDisposition = projectComponentTrust({
        static_cast<ComponentTrustDisposition>(0xff),
        ComponentTrustDiagnosticCode::Ready,
    });
    require(
        unknownDisposition.disposition == "reject"
            && unknownDisposition.diagnostic == "unknown_evidence",
        "unknown dispositions should project as rejected unknown evidence");

    const auto unknownDiagnostic = projectComponentTrust({
        ComponentTrustDisposition::EligibleForProbe,
        static_cast<ComponentTrustDiagnosticCode>(0xff),
    });
    require(
        unknownDiagnostic.disposition == "reject"
            && unknownDiagnostic.diagnostic == "unknown_evidence",
        "unknown diagnostics should project as rejected unknown evidence");

    const auto contradictory = projectComponentTrust({
        ComponentTrustDisposition::EligibleForIsolatedActivation,
        ComponentTrustDiagnosticCode::SignatureMissing,
    });
    require(
        contradictory.disposition == "reject"
            && contradictory.diagnostic == "unknown_evidence",
        "contradictory decision pairs should fail closed in projections");
}

void testUnknownEnumValuesRejectBeforeAnyProgress()
{
    auto evidence = defaultEvidence();
    auto signature = evidence.signature.inspect();
    signature.state = static_cast<ComponentSignatureState>(0xff);
    evidence.signature =
        bundle::ComponentBundleSignatureEvidenceFactory::make(signature);
    requireDecision(
        evidence,
        ComponentTrustDisposition::Reject,
        ComponentTrustDiagnosticCode::UnknownEvidence,
        "unknown signature state should reject");

    evidence = defaultEvidence();
    auto unknownProbe = ComponentProbeEvidenceView{};
    unknownProbe.state = static_cast<ComponentProbeState>(0xff);
    require(
        !std::is_constructible_v<
            ComponentProbeEvidence,
            ComponentProbeEvidenceView>,
        "unknown probe state should be un-authorable");

}

void testOwnerApprovalMatchesOnlyTheExactNormalizedFingerprint()
{
    const auto candidate = defaultFingerprint();
    const auto exact = matchComponentOwnerApproval(candidate, &candidate);
    require(
        exact.ownerApproval == ComponentOwnerApprovalState::ExactMatch
            && exact.diagnostic
                == ComponentOwnerApprovalMatchCode::ExactMatch,
        "an exact valid fingerprint should match owner approval");
    require(
        matchComponentOwnerApproval(candidate, &candidate) == exact,
        "approval matching should be deterministic and side-effect free");

    const auto missing = matchComponentOwnerApproval(candidate, nullptr);
    require(
        missing.ownerApproval == ComponentOwnerApprovalState::Missing
            && missing.diagnostic
                == ComponentOwnerApprovalMatchCode::MissingApproval,
        "an absent approval should not match");
}

void testEveryApprovalFingerprintFieldPreventsReplay()
{
    const auto candidate = defaultFingerprint();

    auto changed = candidate;
    changed.bundleId = "org.talas.other-filter";
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::BundleIdMismatch,
        "bundle identity changes should make approval stale");

    changed = candidate;
    changed.semanticVersion = "1.2.4";
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::VersionMismatch,
        "version changes should make approval stale");

    changed = candidate;
    changed.contentDigest[17] ^= 0xff;
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::ContentDigestMismatch,
        "content changes should make approval stale");

    changed = candidate;
    changed.signerIdentityDigest[9] ^= 0xff;
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::SignerMismatch,
        "signer changes should make approval stale");

    changed = candidate;
    changed.requestedPermissions |= componentPermission(
        ComponentExecutionPermission::ImmutableResourceRead);
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::PermissionsMismatch,
        "permission expansion should make approval stale");

    changed = candidate;
    ++changed.policyVersion;
    require(
        matchComponentOwnerApproval(candidate, &changed).diagnostic
            == ComponentOwnerApprovalMatchCode::PolicyVersionMismatch,
        "policy-version changes should make approval stale");

    for (auto diagnostic : {
             ComponentOwnerApprovalMatchCode::BundleIdMismatch,
             ComponentOwnerApprovalMatchCode::VersionMismatch,
             ComponentOwnerApprovalMatchCode::ContentDigestMismatch,
             ComponentOwnerApprovalMatchCode::SignerMismatch,
             ComponentOwnerApprovalMatchCode::PermissionsMismatch,
             ComponentOwnerApprovalMatchCode::PolicyVersionMismatch,
         })
    {
        ComponentOwnerApprovalMatch match{
            ComponentOwnerApprovalState::Stale, diagnostic};
        require(
            match.ownerApproval == ComponentOwnerApprovalState::Stale,
            "every exact-field mismatch should map to stale approval");
    }
}

void testMalformedApprovalEvidenceRejectsBeforeComparison()
{
    const auto valid = defaultFingerprint();
    for (const auto malformedVersion : {
             "1.2",
             "01.2.3",
             "1.02.3",
             "1.2.03",
             "1.2.3-",
             "1.2.3+",
             "1.2.3-01",
             "1.2.3 bad",
         })
    {
        auto malformed = valid;
        malformed.semanticVersion = malformedVersion;
        const auto result = matchComponentOwnerApproval(malformed, &valid);
        require(
            result.ownerApproval == ComponentOwnerApprovalState::Invalid
                && result.diagnostic
                    == ComponentOwnerApprovalMatchCode::InvalidCandidate,
            "non-canonical semantic versions should invalidate candidates");
    }

    for (const auto malformedBundle : {
             "Org.Talas.Filter",
             ".org.talas.filter",
             "org..filter",
             "org.talas.filter-",
             "org/talas/filter",
         })
    {
        auto malformed = valid;
        malformed.bundleId = malformedBundle;
        require(
            matchComponentOwnerApproval(malformed, &valid).diagnostic
                == ComponentOwnerApprovalMatchCode::InvalidCandidate,
            "non-canonical bundle identifiers should invalidate candidates");
    }

    auto malformed = valid;
    malformed.hasContentDigest = false;
    require(
        matchComponentOwnerApproval(malformed, &valid).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidCandidate,
        "missing authenticated content digests should invalidate candidates");

    malformed = valid;
    malformed.hasSignerIdentity = false;
    require(
        matchComponentOwnerApproval(malformed, &valid).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidCandidate,
        "missing signer identities should invalidate candidates");

    malformed = valid;
    malformed.requestedPermissions = 0;
    require(
        matchComponentOwnerApproval(malformed, &valid).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidCandidate,
        "empty permissions should invalidate candidates");

    malformed = valid;
    malformed.policyVersion = 0;
    require(
        matchComponentOwnerApproval(malformed, &valid).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidCandidate,
        "missing policy versions should invalidate candidates");

    malformed = valid;
    ++malformed.policyVersion;
    require(
        matchComponentOwnerApproval(malformed, &malformed).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidCandidate,
        "unsupported future policy versions must not self-authorize");

    malformed = valid;
    const std::string oversizedBundleId(
        maximumComponentBundleIdBytes + 1, 'a');
    malformed.bundleId = oversizedBundleId;
    require(
        matchComponentOwnerApproval(valid, &malformed).diagnostic
            == ComponentOwnerApprovalMatchCode::InvalidApproval,
        "malformed stored approvals should reject rather than compare");
}

static_assert(std::is_copy_constructible_v<ComponentTrustEvidence>);
static_assert(std::is_copy_assignable_v<ComponentTrustEvidence>);
static_assert(std::is_default_constructible_v<ComponentIsolationEvidenceV1>);
static_assert(std::is_copy_constructible_v<ComponentIsolationEvidenceV1>);
static_assert(!std::is_aggregate_v<ComponentIsolationEvidenceV1>);
static_assert(std::is_trivially_copyable_v<ComponentTrustDecision>);
static_assert(std::is_trivially_copyable_v<ComponentApprovalFingerprint>);
static_assert(std::is_trivially_copyable_v<ComponentOwnerApprovalMatch>);
static_assert(std::is_trivially_copyable_v<ComponentSignatureEvidence>);
static_assert(std::is_default_constructible_v<ComponentSignatureEvidence>);
static_assert(!std::is_aggregate_v<ComponentSignatureEvidence>);
static_assert(!std::is_constructible_v<
    ComponentSignatureEvidence,
    ComponentSignatureEvidenceView>);
static_assert(std::is_copy_constructible_v<ComponentProbeEvidence>);
static_assert(std::is_default_constructible_v<ComponentProbeEvidence>);
static_assert(!std::is_aggregate_v<ComponentProbeEvidence>);
static_assert(std::is_trivially_copyable_v<ComponentTrustProjection>);
static_assert(noexcept(evaluateComponentTrust(ComponentTrustEvidence{})));
static_assert(noexcept(matchComponentOwnerApproval(
    ComponentApprovalFingerprint{}, nullptr)));
static_assert(noexcept(componentPermissionsForRoles(0)));
#if defined(__APPLE__) && defined(__MACH__) && TARGET_OS_OSX
static_assert(componentIsolationBackendAvailableV1);
#else
static_assert(!componentIsolationBackendAvailableV1);
#endif
static_assert(noexcept(projectComponentTrust(ComponentTrustDecision{})));
}

int main()
{
    testValidEvidenceAdvancesOnlyThroughClosedStates();
    testSignatureEvidenceNeverAuthorizesByItself();
    testMalformedCandidateRejectsBeforeMissingSignaturePreservation();
    testVerifiedSignatureIsBoundToTheExactCandidate();
    testEvaluatorDerivesExactOwnerApprovalInsteadOfTrustingAClaim();
    testApprovalAndProbeFailuresFailClosed();
    testCleanProbeIsBoundToCandidateAndValidationSession();
    testRevokedOrExpiredProbeSessionCannotAdvance();
    testPermissionsAreClosedAndRequired();
    testPermissionsMatchDeclaredRolesExactly();
    testEveryV1RoleMapsToOneDistinctPermissionAuthority();
    testIsolationEvidenceIsOpaqueAndUnavailableByDefault();
    testNoCallerConstructibleEvidenceCombinationCanActivateUserCode();
    testReadOnlyProjectionUsesOnlyBoundedProductAuthoredText();
    testProjectionDistinguishesEveryClosedDispositionHonestly();
    testUnknownOrContradictoryProjectionInputFailsClosed();
    testUnknownEnumValuesRejectBeforeAnyProgress();
    testOwnerApprovalMatchesOnlyTheExactNormalizedFingerprint();
    testEveryApprovalFingerprintFieldPreventsReplay();
    testMalformedApprovalEvidenceRejectsBeforeComparison();
    return 0;
}
