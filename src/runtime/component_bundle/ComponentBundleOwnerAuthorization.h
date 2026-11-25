#pragma once

#include "talas/runtime/ComponentTrustPolicy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace talas::runtime::component_bundle
{
inline constexpr std::size_t maximumEphemeralComponentOwnerAuthorizations =
    256;
inline constexpr std::size_t maximumConcurrentComponentActivationUses = 256;

struct OwnedComponentApprovalFingerprint
{
    std::string bundleId;
    std::string semanticVersion;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t policyVersion = 0;
    bool hasContentDigest = false;
    bool hasSignerIdentity = false;

    [[nodiscard]] ComponentApprovalFingerprint view() const noexcept;

    bool operator==(const OwnedComponentApprovalFingerprint&) const = default;
};

enum class ComponentOwnerAuthorizationAction : std::uint8_t
{
    Approve,
    Deny,
};

enum class ComponentOwnerAuthorizationMutationCode : std::uint8_t
{
    Applied,
    InvalidFingerprint,
    DuplicateAction,
    ReplacementRequired,
    MissingRecord,
    StaleGeneration,
    CapacityExceeded,
    GenerationExhausted,
    InvalidAction,
};

struct ComponentOwnerAuthorizationMutationResult
{
    ComponentOwnerAuthorizationMutationCode code =
        ComponentOwnerAuthorizationMutationCode::InvalidFingerprint;
    std::uint64_t generation = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ComponentOwnerAuthorizationMutationCode::Applied;
    }
};

struct ComponentOwnerAuthorizationLookup
{
    ComponentOwnerApprovalMatch match;
    std::optional<OwnedComponentApprovalFingerprint> record;
    std::uint64_t generation = 0;

    [[nodiscard]] bool approved() const noexcept
    {
        return match.ownerApproval == ComponentOwnerApprovalState::ExactMatch
            && match.diagnostic
                == ComponentOwnerApprovalMatchCode::ExactMatch
            && record.has_value();
    }

    [[nodiscard]] bool denied() const noexcept
    {
        return match.ownerApproval == ComponentOwnerApprovalState::Denied
            && match.diagnostic
                == ComponentOwnerApprovalMatchCode::ExactMatch
            && record.has_value();
    }
};

class ComponentOwnerAuthorizationStore
{
public:
    ComponentOwnerAuthorizationStore();
    ~ComponentOwnerAuthorizationStore();
    ComponentOwnerAuthorizationStore(
        const ComponentOwnerAuthorizationStore&) = delete;
    ComponentOwnerAuthorizationStore& operator=(
        const ComponentOwnerAuthorizationStore&) = delete;

    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] ComponentOwnerAuthorizationMutationResult recordOwnerAction(
        const ComponentApprovalFingerprint& fingerprint,
        ComponentOwnerAuthorizationAction action,
        std::uint64_t expectedGeneration);

    [[nodiscard]] ComponentOwnerAuthorizationMutationResult replaceOwnerAction(
        const ComponentApprovalFingerprint& fingerprint,
        ComponentOwnerAuthorizationAction action,
        std::uint64_t expectedGeneration);

    [[nodiscard]] ComponentOwnerAuthorizationMutationResult revokeOwnerAction(
        const ComponentApprovalFingerprint& fingerprint,
        std::uint64_t expectedGeneration);

    [[nodiscard]] ComponentOwnerAuthorizationLookup lookup(
        const ComponentApprovalFingerprint& fingerprint) const;

    [[nodiscard]] ComponentOwnerAuthorizationEvidenceV1
    authorizationEvidence() const noexcept;

private:
    std::shared_ptr<detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>
        liveState_;
};
}
