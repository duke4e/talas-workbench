#include "runtime/component_bundle/ComponentBundlePreservationCompatibility.h"

#include <algorithm>

namespace talas::runtime::component_bundle
{
namespace
{
[[nodiscard]] bool candidateMatchesSnapshot(
    const ComponentApprovalFingerprint& candidate,
    const ComponentBundleSnapshot& snapshot) noexcept
{
    return candidate.bundleId == snapshot.manifest.bundleId
        && candidate.semanticVersion == snapshot.manifest.semanticVersion
        && candidate.contentDigest == snapshot.contentDigest
        && candidate.requestedPermissions
            == snapshot.manifest.requestedPermissions
        && candidate.policyVersion == currentComponentTrustPolicyVersion
        && candidate.hasContentDigest && candidate.hasSignerIdentity;
}

[[nodiscard]] bool portsMatch(
    const std::vector<talas::project::ProjectPreservedPort>& preserved,
    const std::vector<ComponentBundlePort>& candidate) noexcept
{
    if (preserved.size() != candidate.size())
        return false;
    for (std::size_t index = 0; index < preserved.size(); ++index)
    {
        const auto& expected = preserved[index];
        const auto& actual = candidate[index];
        if (expected.portId != actual.id
            || expected.type != actual.type
            || expected.required != actual.required
            || expected.signal.rate != actual.signal.rate
            || expected.signal.audioLayout != actual.signal.audioLayout
            || expected.signal.edge != actual.signal.edge
            || expected.signal.spectralFormat
                != actual.signal.spectralFormat
            || expected.signal.timelineBearing
                != actual.signal.timelineBearing)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parametersMatch(
    const std::vector<talas::project::ProjectPreservedParameter>& preserved,
    const std::vector<ComponentBundleParameter>& candidate) noexcept
{
    if (preserved.size() != candidate.size())
        return false;
    for (std::size_t index = 0; index < preserved.size(); ++index)
    {
        const auto& expected = preserved[index];
        const auto& actual = candidate[index];
        if (expected.parameterId != actual.id
            || expected.kind != actual.kind
            || expected.domain != actual.domain
            || expected.choices != actual.choices)
        {
            return false;
        }
    }
    return true;
}
}

ComponentBundlePreservationCode classifyComponentTrustForPreservation(
    ComponentTrustDecision decision) noexcept
{
    const auto trust = projectComponentTrust(decision);
    if (trust.disposition == "eligible_for_isolated_activation")
        return ComponentBundlePreservationCode::ActivationForbidden;
    if (trust.disposition == "eligible_for_probe")
        return ComponentBundlePreservationCode::CompatibleForProbeOnly;
    if (trust.diagnostic == "approval_denied")
        return ComponentBundlePreservationCode::OwnerDenied;
    if (trust.diagnostic == "signature_evidence_mismatch")
        return ComponentBundlePreservationCode::SignerMismatch;
    if (trust.diagnostic == "approval_missing"
        || trust.diagnostic == "approval_stale")
    {
        return ComponentBundlePreservationCode::OwnerApprovalMissing;
    }
    return ComponentBundlePreservationCode::CandidateRejected;
}

ComponentBundlePreservationResult projectPreservedComponentCompatibility(
    std::string_view typeId,
    const talas::project::ProjectPreservedComponent& preserved,
    const ComponentBundleSnapshot* snapshot,
    const ComponentTrustEvidence* trustEvidence)
{
    if (talas::project::validateProjectPreservedComponent(preserved))
    {
        return {
            ComponentBundlePreservationCode::InvalidPreservedCapsule};
    }
    if (snapshot == nullptr)
        return {ComponentBundlePreservationCode::MissingCandidate};
    if (!validateComponentBundleSnapshotIdentity(*snapshot))
        return {ComponentBundlePreservationCode::InvalidSnapshot};
    if (trustEvidence == nullptr)
        return {ComponentBundlePreservationCode::TrustEvidenceMissing};
    if (!candidateMatchesSnapshot(trustEvidence->candidate, *snapshot))
    {
        return {
            ComponentBundlePreservationCode::CandidateFingerprintMismatch};
    }

    const auto trustCode = classifyComponentTrustForPreservation(
        evaluateComponentTrust(*trustEvidence));
    if (trustCode != ComponentBundlePreservationCode::CompatibleForProbeOnly)
        return {trustCode};

    if (snapshot->manifest.bundleId != preserved.bundleId)
        return {ComponentBundlePreservationCode::BundleMismatch};
    if (snapshot->manifest.semanticVersion != preserved.typeVersion)
        return {ComponentBundlePreservationCode::VersionMismatch};

    const auto node = std::ranges::find(
        snapshot->manifest.nodes,
        typeId,
        &ComponentBundleNode::typeId);
    if (node == snapshot->manifest.nodes.end())
        return {ComponentBundlePreservationCode::InterfaceMismatch};
    if (node->stateSchemaVersion != preserved.stateSchemaVersion)
        return {ComponentBundlePreservationCode::StateSchemaMismatch};
    if (!portsMatch(preserved.inputs, node->inputs)
        || !portsMatch(preserved.outputs, node->outputs)
        || !parametersMatch(preserved.parameters, node->parameters))
    {
        return {ComponentBundlePreservationCode::InterfaceMismatch};
    }

    return {ComponentBundlePreservationCode::CompatibleForProbeOnly};
}
}
