#include "talas/runtime/ComponentTrustPolicy.h"

#include "component_isolation/ComponentIsolationLiveEvidence.h"
#include "talas/runtime/ComponentIsolationAudioDataPlane.h"

#include <string_view>

namespace talas::runtime
{
ComponentIsolationEvidenceViewV1 ComponentIsolationEvidenceV1::inspect()
    const noexcept
{
    ComponentIsolationEvidenceViewV1 view;
    if (!componentIsolationBackendAvailableV1)
        return view;
    const auto liveState = liveState_.lock();
    if (liveState == nullptr
        || !liveState->ready.load(std::memory_order_acquire))
    {
        return view;
    }
    const auto audioPlane = liveState->audioPlane.lock();
    const auto audioPlaneRevision = liveState->audioPlaneRevision.load(
        std::memory_order_acquire);
    if (audioPlane == nullptr || audioPlaneRevision == 0
        || audioPlane->isQuarantined()
        || audioPlane->lifecycleRevision() != audioPlaneRevision)
    {
        return view;
    }

    view.state = ComponentIsolationState::Ready;
    view.profileVersion = currentComponentIsolationProfileVersion;
    view.safeguards = allComponentIsolationSafeguardsV1;
    view.supportedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    view.sessionGeneration = liveState->sessionGeneration;
    view.sharedStorageIdentity = liveState->sharedStorageIdentity;
    view.validationNonce = liveState->validationNonce;
    view.hasValidationNonce = true;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!liveState->ready.load(std::memory_order_acquire)
        || audioPlane->isQuarantined()
        || audioPlane->lifecycleRevision() != audioPlaneRevision)
        return {};
    return view;
}

namespace
{
constexpr ComponentTrustDecision reject(
    ComponentTrustDiagnosticCode diagnostic) noexcept
{
    return {ComponentTrustDisposition::Reject, diagnostic};
}

constexpr ComponentTrustDecision preserve(
    ComponentTrustDiagnosticCode diagnostic) noexcept
{
    return {ComponentTrustDisposition::PreserveOnly, diagnostic};
}

constexpr bool known(ComponentSignatureState state) noexcept
{
    switch (state)
    {
        case ComponentSignatureState::Missing:
        case ComponentSignatureState::Verified:
        case ComponentSignatureState::Invalid:
        case ComponentSignatureState::Unsupported:
        case ComponentSignatureState::Expired:
        case ComponentSignatureState::Revoked:
            return true;
    }
    return false;
}

constexpr bool known(ComponentProbeState state) noexcept
{
    switch (state)
    {
        case ComponentProbeState::NotRun:
        case ComponentProbeState::Clean:
        case ComponentProbeState::Rejected:
        case ComponentProbeState::ControlledFatal:
        case ComponentProbeState::AbnormalTermination:
        case ComponentProbeState::TimedOut:
        case ComponentProbeState::ProtocolError:
            return true;
    }
    return false;
}

constexpr bool known(ComponentIsolationState state) noexcept
{
    switch (state)
    {
        case ComponentIsolationState::Unavailable:
        case ComponentIsolationState::Ready:
            return true;
    }
    return false;
}

constexpr bool asciiDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

constexpr bool asciiLower(char value) noexcept
{
    return value >= 'a' && value <= 'z';
}

constexpr bool asciiAlphaNumeric(char value) noexcept
{
    return asciiDigit(value)
        || (value >= 'A' && value <= 'Z')
        || asciiLower(value);
}

bool validBundleId(std::string_view value) noexcept
{
    if (value.empty() || value.size() > maximumComponentBundleIdBytes)
        return false;

    std::size_t segmentStart = 0;
    std::size_t segmentCount = 0;
    while (segmentStart < value.size())
    {
        const auto separator = value.find('.', segmentStart);
        const auto segmentEnd = separator == std::string_view::npos
            ? value.size()
            : separator;
        if (segmentEnd == segmentStart)
            return false;
        if (!(asciiLower(value[segmentStart])
                || asciiDigit(value[segmentStart]))
            || !(asciiLower(value[segmentEnd - 1])
                || asciiDigit(value[segmentEnd - 1])))
        {
            return false;
        }
        for (auto index = segmentStart; index < segmentEnd; ++index)
        {
            if (!(asciiLower(value[index]) || asciiDigit(value[index])
                    || value[index] == '-'))
            {
                return false;
            }
        }
        ++segmentCount;
        if (separator == std::string_view::npos)
            break;
        segmentStart = separator + 1;
    }
    return segmentCount >= 2;
}

bool validDotIdentifiers(
    std::string_view value,
    bool rejectNumericLeadingZero) noexcept
{
    if (value.empty())
        return false;

    std::size_t identifierStart = 0;
    while (identifierStart < value.size())
    {
        const auto separator = value.find('.', identifierStart);
        const auto identifierEnd = separator == std::string_view::npos
            ? value.size()
            : separator;
        if (identifierEnd == identifierStart)
            return false;

        bool numeric = true;
        for (auto index = identifierStart; index < identifierEnd; ++index)
        {
            const auto character = value[index];
            if (!(asciiAlphaNumeric(character) || character == '-'))
                return false;
            numeric = numeric && asciiDigit(character);
        }
        if (rejectNumericLeadingZero && numeric
            && identifierEnd - identifierStart > 1
            && value[identifierStart] == '0')
        {
            return false;
        }

        if (separator == std::string_view::npos)
            break;
        identifierStart = separator + 1;
    }
    return true;
}

bool validCoreNumber(std::string_view value) noexcept
{
    if (value.empty() || (value.size() > 1 && value.front() == '0'))
        return false;
    for (const auto character : value)
    {
        if (!asciiDigit(character))
            return false;
    }
    return true;
}

bool validSemanticVersion(std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > maximumComponentSemanticVersionBytes)
    {
        return false;
    }

    const auto plus = value.find('+');
    if (plus != std::string_view::npos
        && (value.find('+', plus + 1) != std::string_view::npos
            || !validDotIdentifiers(value.substr(plus + 1), false)))
    {
        return false;
    }

    const auto withoutBuild = value.substr(0, plus);
    const auto dash = withoutBuild.find('-');
    if (dash != std::string_view::npos
        && !validDotIdentifiers(withoutBuild.substr(dash + 1), true))
    {
        return false;
    }

    const auto core = withoutBuild.substr(0, dash);
    const auto firstDot = core.find('.');
    if (firstDot == std::string_view::npos)
        return false;
    const auto secondDot = core.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos
        || core.find('.', secondDot + 1) != std::string_view::npos)
    {
        return false;
    }

    return validCoreNumber(core.substr(0, firstDot))
        && validCoreNumber(
            core.substr(firstDot + 1, secondDot - firstDot - 1))
        && validCoreNumber(core.substr(secondDot + 1));
}

bool validFingerprint(const ComponentApprovalFingerprint& value) noexcept
{
    return validBundleId(value.bundleId)
        && validSemanticVersion(value.semanticVersion)
        && value.hasContentDigest && value.hasSignerIdentity
        && value.requestedPermissions != 0
        && (value.requestedPermissions
                & ~allComponentExecutionPermissionsV1)
            == 0
        && value.policyVersion != 0;
}

template <std::size_t Size>
constexpr bool allZero(const std::array<std::uint8_t, Size>& value) noexcept
{
    for (const auto byte : value)
    {
        if (byte != 0)
            return false;
    }
    return true;
}

constexpr bool emptySignatureEvidence(
    const ComponentSignatureEvidenceView& evidence) noexcept
{
    return evidence.profileVersion == 0 && !evidence.hasContentDigest
        && !evidence.hasSignerIdentity && allZero(evidence.contentDigest)
        && allZero(evidence.signerIdentityDigest);
}

constexpr bool emptyProbeResultEvidence(
    const ComponentProbeEvidenceView& evidence) noexcept
{
    return evidence.protocolVersion == 0 && evidence.artifactGeneration == 0
        && !evidence.hasCandidateFingerprint
        && !evidence.hasValidationNonce
        && evidence.candidateFingerprint == ComponentApprovalFingerprint{}
        && allZero(evidence.validationNonce) && !evidence.authored
        && !evidence.sessionActive;
}

constexpr std::string_view diagnosticName(
    ComponentTrustDiagnosticCode diagnostic) noexcept
{
    switch (diagnostic)
    {
        case ComponentTrustDiagnosticCode::Ready:
            return "ready";
        case ComponentTrustDiagnosticCode::UnknownEvidence:
            return "unknown_evidence";
        case ComponentTrustDiagnosticCode::SignatureMissing:
            return "signature_missing";
        case ComponentTrustDiagnosticCode::SignatureInvalid:
            return "signature_invalid";
        case ComponentTrustDiagnosticCode::SignatureUnsupported:
            return "signature_unsupported";
        case ComponentTrustDiagnosticCode::SignatureExpired:
            return "signature_expired";
        case ComponentTrustDiagnosticCode::SignatureRevoked:
            return "signature_revoked";
        case ComponentTrustDiagnosticCode::SignatureEvidenceMismatch:
            return "signature_evidence_mismatch";
        case ComponentTrustDiagnosticCode::SignatureProfileUnsupported:
            return "signature_profile_unsupported";
        case ComponentTrustDiagnosticCode::ApprovalMissing:
            return "approval_missing";
        case ComponentTrustDiagnosticCode::ApprovalStale:
            return "approval_stale";
        case ComponentTrustDiagnosticCode::ApprovalDenied:
            return "approval_denied";
        case ComponentTrustDiagnosticCode::ApprovalInvalid:
            return "approval_invalid";
        case ComponentTrustDiagnosticCode::RolesEmpty:
            return "roles_empty";
        case ComponentTrustDiagnosticCode::RolesUnknown:
            return "roles_unknown";
        case ComponentTrustDiagnosticCode::PermissionsEmpty:
            return "permissions_empty";
        case ComponentTrustDiagnosticCode::PermissionsUnknown:
            return "permissions_unknown";
        case ComponentTrustDiagnosticCode::PermissionsRoleMismatch:
            return "permissions_role_mismatch";
        case ComponentTrustDiagnosticCode::IsolationPermissionsUnknown:
            return "isolation_permissions_unknown";
        case ComponentTrustDiagnosticCode::IsolationPermissionsUnsupported:
            return "isolation_permissions_unsupported";
        case ComponentTrustDiagnosticCode::IsolationEvidenceContradictory:
            return "isolation_evidence_contradictory";
        case ComponentTrustDiagnosticCode::IsolationProfileUnsupported:
            return "isolation_profile_unsupported";
        case ComponentTrustDiagnosticCode::IsolationSafeguardsUnknown:
            return "isolation_safeguards_unknown";
        case ComponentTrustDiagnosticCode::IsolationSafeguardsIncomplete:
            return "isolation_safeguards_incomplete";
        case ComponentTrustDiagnosticCode::IsolationBackendUnavailable:
            return "isolation_backend_unavailable";
        case ComponentTrustDiagnosticCode::ProbeRequired:
            return "probe_required";
        case ComponentTrustDiagnosticCode::ProbeRejected:
            return "probe_rejected";
        case ComponentTrustDiagnosticCode::ProbeControlledFatal:
            return "probe_controlled_fatal";
        case ComponentTrustDiagnosticCode::ProbeCrashed:
            return "probe_crashed";
        case ComponentTrustDiagnosticCode::ProbeTimedOut:
            return "probe_timed_out";
        case ComponentTrustDiagnosticCode::ProbeProtocolError:
            return "probe_protocol_error";
        case ComponentTrustDiagnosticCode::ProbeEvidenceContradictory:
            return "probe_evidence_contradictory";
        case ComponentTrustDiagnosticCode::ProbeEvidenceMismatch:
            return "probe_evidence_mismatch";
        case ComponentTrustDiagnosticCode::ProbeProfileUnsupported:
            return "probe_profile_unsupported";
        case ComponentTrustDiagnosticCode::ProbeStale:
            return "probe_stale";
        case ComponentTrustDiagnosticCode::IsolationUnavailable:
            return "isolation_unavailable";
    }
    return "unknown_evidence";
}

constexpr bool validDecisionPair(ComponentTrustDecision decision) noexcept
{
    switch (decision.diagnostic)
    {
        case ComponentTrustDiagnosticCode::Ready:
            return decision.disposition
                == ComponentTrustDisposition::EligibleForIsolatedActivation;
        case ComponentTrustDiagnosticCode::ProbeRequired:
            return decision.disposition
                == ComponentTrustDisposition::EligibleForProbe;
        case ComponentTrustDiagnosticCode::SignatureMissing:
        case ComponentTrustDiagnosticCode::ApprovalMissing:
        case ComponentTrustDiagnosticCode::ApprovalStale:
        case ComponentTrustDiagnosticCode::IsolationBackendUnavailable:
        case ComponentTrustDiagnosticCode::IsolationUnavailable:
            return decision.disposition
                == ComponentTrustDisposition::PreserveOnly;
        case ComponentTrustDiagnosticCode::UnknownEvidence:
        case ComponentTrustDiagnosticCode::SignatureInvalid:
        case ComponentTrustDiagnosticCode::SignatureUnsupported:
        case ComponentTrustDiagnosticCode::SignatureExpired:
        case ComponentTrustDiagnosticCode::SignatureRevoked:
        case ComponentTrustDiagnosticCode::SignatureEvidenceMismatch:
        case ComponentTrustDiagnosticCode::SignatureProfileUnsupported:
        case ComponentTrustDiagnosticCode::ApprovalDenied:
        case ComponentTrustDiagnosticCode::ApprovalInvalid:
        case ComponentTrustDiagnosticCode::RolesEmpty:
        case ComponentTrustDiagnosticCode::RolesUnknown:
        case ComponentTrustDiagnosticCode::PermissionsEmpty:
        case ComponentTrustDiagnosticCode::PermissionsUnknown:
        case ComponentTrustDiagnosticCode::PermissionsRoleMismatch:
        case ComponentTrustDiagnosticCode::IsolationPermissionsUnknown:
        case ComponentTrustDiagnosticCode::IsolationPermissionsUnsupported:
        case ComponentTrustDiagnosticCode::IsolationEvidenceContradictory:
        case ComponentTrustDiagnosticCode::IsolationProfileUnsupported:
        case ComponentTrustDiagnosticCode::IsolationSafeguardsUnknown:
        case ComponentTrustDiagnosticCode::IsolationSafeguardsIncomplete:
        case ComponentTrustDiagnosticCode::ProbeRejected:
        case ComponentTrustDiagnosticCode::ProbeControlledFatal:
        case ComponentTrustDiagnosticCode::ProbeCrashed:
        case ComponentTrustDiagnosticCode::ProbeTimedOut:
        case ComponentTrustDiagnosticCode::ProbeProtocolError:
        case ComponentTrustDiagnosticCode::ProbeEvidenceContradictory:
        case ComponentTrustDiagnosticCode::ProbeEvidenceMismatch:
        case ComponentTrustDiagnosticCode::ProbeProfileUnsupported:
        case ComponentTrustDiagnosticCode::ProbeStale:
            return decision.disposition == ComponentTrustDisposition::Reject;
    }
    return false;
}

constexpr ComponentTrustProjection rejectedProjection() noexcept
{
    return {
        "reject",
        "unknown_evidence",
        "Candidate rejected by component security policy.",
        false,
        false,
    };
}
}

ComponentTrustProjection projectComponentTrust(
    ComponentTrustDecision decision) noexcept
{
    if (!validDecisionPair(decision))
        return rejectedProjection();

    switch (decision.disposition)
    {
        case ComponentTrustDisposition::Reject:
            return {
                "reject",
                diagnosticName(decision.diagnostic),
                "Candidate rejected by component security policy.",
                false,
                false,
            };
        case ComponentTrustDisposition::PreserveOnly:
            return {
                "preserve_only",
                diagnosticName(decision.diagnostic),
                "Component data is preserved; executable code remains inactive.",
                false,
                false,
            };
        case ComponentTrustDisposition::EligibleForProbe:
            return {
                "eligible_for_probe",
                diagnosticName(decision.diagnostic),
                "Candidate may enter bounded probing; executable code remains inactive.",
                false,
                false,
            };
        case ComponentTrustDisposition::EligibleForIsolatedActivation:
            return {
                "eligible_for_isolated_activation",
                diagnosticName(decision.diagnostic),
                "Candidate meets live product-fixture containment policy; no user code is active.",
                false,
                false,
            };
    }
    return rejectedProjection();
}

ComponentOwnerApprovalMatch matchComponentOwnerApproval(
    const ComponentApprovalFingerprint& candidate,
    const ComponentApprovalFingerprint* approved) noexcept
{
    if (!validFingerprint(candidate)
        || candidate.policyVersion != currentComponentTrustPolicyVersion)
    {
        return {
            ComponentOwnerApprovalState::Invalid,
            ComponentOwnerApprovalMatchCode::InvalidCandidate,
        };
    }
    if (approved == nullptr)
    {
        return {
            ComponentOwnerApprovalState::Missing,
            ComponentOwnerApprovalMatchCode::MissingApproval,
        };
    }
    if (!validFingerprint(*approved))
    {
        return {
            ComponentOwnerApprovalState::Invalid,
            ComponentOwnerApprovalMatchCode::InvalidApproval,
        };
    }

    const auto stale = [](ComponentOwnerApprovalMatchCode diagnostic) {
        return ComponentOwnerApprovalMatch{
            ComponentOwnerApprovalState::Stale, diagnostic};
    };
    if (candidate.bundleId != approved->bundleId)
        return stale(ComponentOwnerApprovalMatchCode::BundleIdMismatch);
    if (candidate.semanticVersion != approved->semanticVersion)
        return stale(ComponentOwnerApprovalMatchCode::VersionMismatch);
    if (candidate.contentDigest != approved->contentDigest)
        return stale(ComponentOwnerApprovalMatchCode::ContentDigestMismatch);
    if (candidate.signerIdentityDigest != approved->signerIdentityDigest)
        return stale(ComponentOwnerApprovalMatchCode::SignerMismatch);
    if (candidate.requestedPermissions != approved->requestedPermissions)
        return stale(ComponentOwnerApprovalMatchCode::PermissionsMismatch);
    if (candidate.policyVersion != approved->policyVersion)
        return stale(ComponentOwnerApprovalMatchCode::PolicyVersionMismatch);

    return {
        ComponentOwnerApprovalState::ExactMatch,
        ComponentOwnerApprovalMatchCode::ExactMatch,
    };
}

ComponentTrustDecision evaluateComponentTrust(
    const ComponentTrustEvidence& evidence) noexcept
{
    const auto signature = evidence.signature.inspect();
    const auto probe = evidence.probe.inspect();
    const auto isolation = evidence.isolationEvidence.inspect();
    if (!known(signature.state) || !known(probe.state)
        || !known(isolation.state))
    {
        return reject(ComponentTrustDiagnosticCode::UnknownEvidence);
    }

    if (evidence.declaredRoles == 0)
        return reject(ComponentTrustDiagnosticCode::RolesEmpty);
    if ((evidence.declaredRoles & ~allComponentExecutionRolesV1) != 0)
        return reject(ComponentTrustDiagnosticCode::RolesUnknown);
    const auto requestedPermissions = evidence.candidate.requestedPermissions;
    if (requestedPermissions == 0)
        return reject(ComponentTrustDiagnosticCode::PermissionsEmpty);
    if ((requestedPermissions & ~allComponentExecutionPermissionsV1) != 0)
    {
        return reject(ComponentTrustDiagnosticCode::PermissionsUnknown);
    }
    if (requestedPermissions
        != componentPermissionsForRoles(evidence.declaredRoles))
    {
        return reject(ComponentTrustDiagnosticCode::PermissionsRoleMismatch);
    }
    if ((isolation.safeguards
            & ~allComponentIsolationSafeguardsV1)
        != 0)
    {
        return reject(
            ComponentTrustDiagnosticCode::IsolationSafeguardsUnknown);
    }
    if (isolation.state == ComponentIsolationState::Unavailable)
    {
        if (isolation.profileVersion != 0 || isolation.safeguards != 0
            || isolation.supportedPermissions != 0
            || isolation.sessionGeneration != 0
            || isolation.sharedStorageIdentity != 0
            || isolation.hasValidationNonce
            || !allZero(isolation.validationNonce))
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationEvidenceContradictory);
        }
    }
    else
    {
        if ((isolation.supportedPermissions
                & ~allComponentExecutionPermissionsV1)
            != 0)
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationPermissionsUnknown);
        }
        if ((requestedPermissions
                & ~isolation.supportedPermissions)
            != 0)
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationPermissionsUnsupported);
        }
        if (isolation.profileVersion
            != currentComponentIsolationProfileVersion)
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationProfileUnsupported);
        }
        if (isolation.safeguards
            != allComponentIsolationSafeguardsV1)
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationSafeguardsIncomplete);
        }
        if (isolation.sessionGeneration == 0
            || isolation.sharedStorageIdentity == 0
            || !isolation.hasValidationNonce
            || allZero(isolation.validationNonce))
        {
            return reject(
                ComponentTrustDiagnosticCode::IsolationEvidenceContradictory);
        }
    }

    const auto ownerApproval =
        evidence.ownerAuthorizationEvidence.inspect(evidence.candidate);
    if (ownerApproval.ownerApproval == ComponentOwnerApprovalState::Invalid)
        return reject(ComponentTrustDiagnosticCode::ApprovalInvalid);
    if (ownerApproval.ownerApproval == ComponentOwnerApprovalState::Denied)
        return reject(ComponentTrustDiagnosticCode::ApprovalDenied);

    switch (signature.state)
    {
        case ComponentSignatureState::Missing:
            if (!emptySignatureEvidence(signature))
            {
                return reject(
                    ComponentTrustDiagnosticCode::SignatureEvidenceMismatch);
            }
            return preserve(ComponentTrustDiagnosticCode::SignatureMissing);
        case ComponentSignatureState::Invalid:
            return reject(ComponentTrustDiagnosticCode::SignatureInvalid);
        case ComponentSignatureState::Unsupported:
            return reject(ComponentTrustDiagnosticCode::SignatureUnsupported);
        case ComponentSignatureState::Expired:
            return reject(ComponentTrustDiagnosticCode::SignatureExpired);
        case ComponentSignatureState::Revoked:
            return reject(ComponentTrustDiagnosticCode::SignatureRevoked);
        case ComponentSignatureState::Verified:
            break;
    }

    if (!signature.hasContentDigest || !signature.hasSignerIdentity)
    {
        return reject(
            ComponentTrustDiagnosticCode::SignatureEvidenceMismatch);
    }
    if (signature.profileVersion != currentComponentSignatureProfileVersion)
    {
        return reject(
            ComponentTrustDiagnosticCode::SignatureProfileUnsupported);
    }
    if (signature.contentDigest != evidence.candidate.contentDigest
        || signature.signerIdentityDigest
            != evidence.candidate.signerIdentityDigest)
    {
        return reject(
            ComponentTrustDiagnosticCode::SignatureEvidenceMismatch);
    }

    switch (ownerApproval.ownerApproval)
    {
        case ComponentOwnerApprovalState::Missing:
            return preserve(ComponentTrustDiagnosticCode::ApprovalMissing);
        case ComponentOwnerApprovalState::Stale:
            return preserve(ComponentTrustDiagnosticCode::ApprovalStale);
        case ComponentOwnerApprovalState::Denied:
            return reject(ComponentTrustDiagnosticCode::ApprovalDenied);
        case ComponentOwnerApprovalState::Invalid:
            return reject(ComponentTrustDiagnosticCode::ApprovalInvalid);
        case ComponentOwnerApprovalState::ExactMatch:
            break;
    }

    if (evidence.artifactGeneration == 0 || !evidence.hasValidationNonce
        || allZero(evidence.validationNonce))
    {
        return reject(ComponentTrustDiagnosticCode::ProbeStale);
    }

    if (probe.authored && !probe.sessionActive)
        return reject(ComponentTrustDiagnosticCode::ProbeStale);
    if (!probe.authored && probe.sessionActive)
    {
        return reject(
            ComponentTrustDiagnosticCode::ProbeEvidenceContradictory);
    }

    switch (probe.state)
    {
        case ComponentProbeState::NotRun:
            if (!emptyProbeResultEvidence(probe))
            {
                return reject(
                    ComponentTrustDiagnosticCode::ProbeEvidenceContradictory);
            }
            return {
                ComponentTrustDisposition::EligibleForProbe,
                ComponentTrustDiagnosticCode::ProbeRequired,
            };
        case ComponentProbeState::Rejected:
            return reject(ComponentTrustDiagnosticCode::ProbeRejected);
        case ComponentProbeState::ControlledFatal:
            return reject(ComponentTrustDiagnosticCode::ProbeControlledFatal);
        case ComponentProbeState::AbnormalTermination:
            return reject(ComponentTrustDiagnosticCode::ProbeCrashed);
        case ComponentProbeState::TimedOut:
            return reject(ComponentTrustDiagnosticCode::ProbeTimedOut);
        case ComponentProbeState::ProtocolError:
            return reject(ComponentTrustDiagnosticCode::ProbeProtocolError);
        case ComponentProbeState::Clean:
            break;
    }

    if (!probe.authored || !probe.hasCandidateFingerprint
        || !probe.hasValidationNonce)
    {
        return reject(ComponentTrustDiagnosticCode::ProbeEvidenceMismatch);
    }
    if (probe.protocolVersion
        != currentComponentProbeProtocolVersion)
    {
        return reject(ComponentTrustDiagnosticCode::ProbeProfileUnsupported);
    }
    if (probe.candidateFingerprint != evidence.candidate)
    {
        return reject(ComponentTrustDiagnosticCode::ProbeEvidenceMismatch);
    }
    if (probe.artifactGeneration != evidence.artifactGeneration
        || probe.validationNonce != evidence.validationNonce)
    {
        return reject(ComponentTrustDiagnosticCode::ProbeStale);
    }

    if (isolation.state != ComponentIsolationState::Ready)
        return preserve(ComponentTrustDiagnosticCode::IsolationUnavailable);

    if (!componentIsolationBackendAvailableV1)
    {
        return preserve(
            ComponentTrustDiagnosticCode::IsolationBackendUnavailable);
    }

    return {
        ComponentTrustDisposition::EligibleForIsolatedActivation,
        ComponentTrustDiagnosticCode::Ready,
    };
}
}
