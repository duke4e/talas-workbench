#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"

#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectMissingNodes.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::component_bundle
{
inline constexpr std::size_t maximumComponentBundleReplacementTransitions = 8;

enum class ComponentBundleReplacementCode : std::uint8_t
{
    Compatible,
    MissingCandidate,
    InvalidPreservedCapsule,
    InvalidSnapshot,
    TrustEvidenceMissing,
    CandidateFingerprintMismatch,
    CandidateRejected,
    BundleMismatch,
    NodeMissing,
    InterfaceMismatch,
    StateSchemaMismatch,
    UnsupportedManifestSurface,
    ResourceTransitionInvalid,
    AllocationFailed,
};

struct ComponentBundleReplacementResourceIdentity
{
    std::string resourceId;
    talas::core::ResourceSnapshotKind kind =
        talas::core::ResourceSnapshotKind::Table;
    std::uint64_t generation = 0;
    std::string contentHash;

    bool operator==(
        const ComponentBundleReplacementResourceIdentity&) const = default;
};

struct ComponentBundleReplacementResourceTransition
{
    ComponentBundleReplacementResourceIdentity previous;
    ComponentBundleReplacementResourceIdentity candidate;
    bool requiresExplicitConfirmation = false;

    bool operator==(
        const ComponentBundleReplacementResourceTransition&) const = default;
};

struct ComponentBundleReplacementRequest
{
    std::string_view nodeTypeId;
    const talas::project::ProjectPreservedComponent* preserved = nullptr;
    const ComponentBundleSnapshot* candidate = nullptr;
    const ComponentTrustEvidence* trustEvidence = nullptr;
    std::span<const ComponentBundleReplacementResourceTransition>
        resourceTransitions;
};

// Pure, owning preview projection. It carries no execution method and grants no
// approval, probe, activation, stage, registry, installation, or filesystem
// authority.
struct ComponentBundleReplacementPreview
{
    ComponentBundleReplacementCode code =
        ComponentBundleReplacementCode::MissingCandidate;
    std::string diagnostic;
    std::string nodeTypeId;
    std::string preservedBundleId;
    std::string preservedVersion;
    std::string candidateBundleId;
    std::string candidateVersion;
    std::string publisher;
    std::string licenseExpression;
    std::array<std::uint8_t, componentTrustDigestBytes>
        preservedCapsuleDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        candidateContentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        candidateSignerIdentityDigest{};
    ComponentExecutionRoles declaredRoles = 0;
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t stateSchemaVersion = 0;
    bool versionChanged = false;
    std::vector<ComponentBundlePort> inputs;
    std::vector<ComponentBundlePort> outputs;
    std::vector<ComponentBundleParameter> parameters;
    std::vector<ComponentBundleReplacementResourceTransition>
        resourceTransitions;

    [[nodiscard]] bool compatible() const noexcept
    {
        return code == ComponentBundleReplacementCode::Compatible;
    }

    [[nodiscard]] constexpr bool executable() const noexcept
    {
        return false;
    }
};

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateProjectPreservedComponentDigest(
    std::string_view nodeTypeId,
    const talas::project::ProjectPreservedComponent& preserved);

[[nodiscard]] ComponentBundleReplacementPreview
previewComponentBundleReplacement(
    const ComponentBundleReplacementRequest& request) noexcept;
}
