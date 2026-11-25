#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"

#include "talas/project/ProjectMissingNodes.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <cstdint>
#include <string_view>

namespace talas::runtime::component_bundle
{
enum class ComponentBundlePreservationCode : std::uint8_t
{
    MissingCandidate,
    InvalidPreservedCapsule,
    InvalidSnapshot,
    TrustEvidenceMissing,
    CandidateFingerprintMismatch,
    CandidateRejected,
    OwnerDenied,
    SignerMismatch,
    OwnerApprovalMissing,
    BundleMismatch,
    VersionMismatch,
    StateSchemaMismatch,
    InterfaceMismatch,
    CompatibleForProbeOnly,
    ActivationForbidden,
};

struct ComponentBundlePreservationResult
{
    ComponentBundlePreservationCode code =
        ComponentBundlePreservationCode::MissingCandidate;

    [[nodiscard]] bool metadataCompatible() const noexcept
    {
        return code
            == ComponentBundlePreservationCode::CompatibleForProbeOnly;
    }

    [[nodiscard]] constexpr bool executable() const noexcept
    {
        return false;
    }
};

[[nodiscard]] ComponentBundlePreservationCode
classifyComponentTrustForPreservation(
    ComponentTrustDecision decision) noexcept;

[[nodiscard]] ComponentBundlePreservationResult
projectPreservedComponentCompatibility(
    std::string_view typeId,
    const talas::project::ProjectPreservedComponent& preserved,
    const ComponentBundleSnapshot* snapshot,
    const ComponentTrustEvidence* trustEvidence);
}
