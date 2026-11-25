#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"

#include "runtime/component_activation/ComponentActivationStageGate.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace talas::runtime::detail
{
struct ComponentOwnerActivationUseStateV1
{
    component_bundle::OwnedComponentApprovalFingerprint fingerprint;
    std::shared_ptr<ComponentActivationStageGateV1> stageGate;
    std::atomic<bool> active{true};
};

struct ComponentOwnerAuthorizationLiveEvidenceStateV1
{
    mutable std::mutex mutex;
    std::vector<component_bundle::OwnedComponentApprovalFingerprint> approvals;
    std::vector<component_bundle::OwnedComponentApprovalFingerprint> denials;
    mutable std::vector<std::weak_ptr<ComponentOwnerActivationUseStateV1>>
        activationUses;
    std::uint64_t generation = 1;
    bool active = true;
};
}

namespace talas::runtime
{
bool ComponentOwnerActivationUseV1::active() const noexcept
{
    return state_ && state_->active.load(std::memory_order_acquire);
}

ComponentOwnerApprovalMatch ComponentOwnerAuthorizationEvidenceV1::inspect(
    const ComponentApprovalFingerprint& candidate) const noexcept
{
    const auto validity =
        matchComponentOwnerApproval(candidate, &candidate);
    if (validity.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || validity.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return validity;
    }

    const auto liveState = liveState_.lock();
    if (!liveState)
        return matchComponentOwnerApproval(candidate, nullptr);

    const std::scoped_lock lock(liveState->mutex);
    if (!liveState->active)
        return matchComponentOwnerApproval(candidate, nullptr);
    const auto findExact = [&](const auto& records) {
        return std::ranges::find_if(records, [&](const auto& record) {
            return record.view() == candidate;
        });
    };
    const auto approved = findExact(liveState->approvals);
    const auto denied = findExact(liveState->denials);
    if (approved != liveState->approvals.end()
        && denied != liveState->denials.end())
    {
        return {
            ComponentOwnerApprovalState::Invalid,
            ComponentOwnerApprovalMatchCode::InvalidApproval};
    }
    if (approved != liveState->approvals.end())
    {
        const auto approvedView = approved->view();
        return matchComponentOwnerApproval(candidate, &approvedView);
    }
    if (denied != liveState->denials.end())
    {
        const auto deniedView = denied->view();
        auto match = matchComponentOwnerApproval(candidate, &deniedView);
        if (match.ownerApproval == ComponentOwnerApprovalState::ExactMatch
            && match.diagnostic
                == ComponentOwnerApprovalMatchCode::ExactMatch)
        {
            match.ownerApproval = ComponentOwnerApprovalState::Denied;
        }
        return match;
    }
    return matchComponentOwnerApproval(candidate, nullptr);
}

ComponentOwnerAuthorizationLeaseV1
ComponentOwnerAuthorizationEvidenceV1::acquireExactApprovalLease(
    const ComponentApprovalFingerprint& candidate) const noexcept
{
    const auto validity = matchComponentOwnerApproval(candidate, &candidate);
    if (validity.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || validity.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return {};
    }

    auto liveState = liveState_.lock();
    if (!liveState)
        return {};
    std::unique_lock lock(liveState->mutex);
    if (!liveState->active)
        return {};
    const auto findExact = [&](const auto& records) {
        return std::ranges::find_if(records, [&](const auto& record) {
            return record.view() == candidate;
        });
    };
    const auto approved = findExact(liveState->approvals);
    const auto denied = findExact(liveState->denials);
    const auto exactlyApproved = approved != liveState->approvals.end()
        && denied == liveState->denials.end();
    if (!exactlyApproved)
        return {};
    return ComponentOwnerAuthorizationLeaseV1{
        std::move(liveState),
        std::move(lock),
        true};
}

ComponentOwnerActivationUseV1
ComponentOwnerAuthorizationEvidenceV1::acquireExactActivationUse(
    const ComponentApprovalFingerprint& candidate) const noexcept
{
    const auto validity = matchComponentOwnerApproval(candidate, &candidate);
    if (validity.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || validity.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return {};
    }

    auto liveState = liveState_.lock();
    if (!liveState)
        return {};
    const std::scoped_lock lock(liveState->mutex);
    if (!liveState->active)
        return {};
    const auto findExact = [&](const auto& records) {
        return std::ranges::find_if(records, [&](const auto& record) {
            return record.view() == candidate;
        });
    };
    const auto approved = findExact(liveState->approvals);
    const auto denied = findExact(liveState->denials);
    if (approved == liveState->approvals.end()
        || denied != liveState->denials.end())
    {
        return {};
    }

    std::erase_if(liveState->activationUses, [](const auto& weakUse) {
        const auto use = weakUse.lock();
        return !use || !use->active.load(std::memory_order_acquire);
    });
    if (liveState->activationUses.size()
        >= component_bundle::maximumConcurrentComponentActivationUses)
    {
        return {};
    }

    try
    {
        auto state = std::make_shared<
            detail::ComponentOwnerActivationUseStateV1>();
        auto stageGate = std::make_shared<
            detail::ComponentActivationStageGateV1>();
        state->fingerprint = *approved;
        state->stageGate = stageGate;
        liveState->activationUses.push_back(state);
        return ComponentOwnerActivationUseV1{
            std::move(state),
            std::move(stageGate)};
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
}
}

namespace talas::runtime::component_bundle
{
namespace
{
[[nodiscard]] bool validAction(
    ComponentOwnerAuthorizationAction action) noexcept
{
    switch (action)
    {
        case ComponentOwnerAuthorizationAction::Approve:
        case ComponentOwnerAuthorizationAction::Deny:
            return true;
    }
    return false;
}

[[nodiscard]] std::optional<OwnedComponentApprovalFingerprint>
copyValidFingerprint(const ComponentApprovalFingerprint& fingerprint)
{
    const auto initial =
        matchComponentOwnerApproval(fingerprint, &fingerprint);
    if (initial.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || initial.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return std::nullopt;
    }

    OwnedComponentApprovalFingerprint owned;
    owned.bundleId = fingerprint.bundleId;
    owned.semanticVersion = fingerprint.semanticVersion;
    owned.contentDigest = fingerprint.contentDigest;
    owned.signerIdentityDigest = fingerprint.signerIdentityDigest;
    owned.requestedPermissions = fingerprint.requestedPermissions;
    owned.policyVersion = fingerprint.policyVersion;
    owned.hasContentDigest = fingerprint.hasContentDigest;
    owned.hasSignerIdentity = fingerprint.hasSignerIdentity;

    const auto copiedView = owned.view();
    const auto copied =
        matchComponentOwnerApproval(copiedView, &copiedView);
    if (copied.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || copied.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return std::nullopt;
    }
    return owned;
}

template <typename Records>
[[nodiscard]] auto findExact(
    Records& records,
    const ComponentApprovalFingerprint& fingerprint)
{
    return std::ranges::find_if(records, [&](const auto& record) {
        return record.view() == fingerprint;
    });
}

[[nodiscard]] ComponentOwnerAuthorizationMutationResult unchanged(
    ComponentOwnerAuthorizationMutationCode code,
    std::uint64_t generation) noexcept
{
    return {code, generation};
}

void invalidateActivationUses(
    detail::ComponentOwnerAuthorizationLiveEvidenceStateV1& liveState,
    const ComponentApprovalFingerprint* fingerprint) noexcept
{
    std::erase_if(liveState.activationUses, [&](const auto& weakUse) {
        const auto use = weakUse.lock();
        if (!use)
            return true;
        if (fingerprint == nullptr
            || use->fingerprint.view() == *fingerprint)
        {
            const std::unique_lock stageLock(use->stageGate->mutex);
            use->active.store(false, std::memory_order_release);
        }
        return false;
    });
}
}

ComponentApprovalFingerprint OwnedComponentApprovalFingerprint::view()
    const noexcept
{
    ComponentApprovalFingerprint fingerprint;
    fingerprint.bundleId = bundleId;
    fingerprint.semanticVersion = semanticVersion;
    fingerprint.contentDigest = contentDigest;
    fingerprint.signerIdentityDigest = signerIdentityDigest;
    fingerprint.requestedPermissions = requestedPermissions;
    fingerprint.policyVersion = policyVersion;
    fingerprint.hasContentDigest = hasContentDigest;
    fingerprint.hasSignerIdentity = hasSignerIdentity;
    return fingerprint;
}

ComponentOwnerAuthorizationStore::ComponentOwnerAuthorizationStore()
    : liveState_(std::make_shared<
          detail::ComponentOwnerAuthorizationLiveEvidenceStateV1>())
{
}

ComponentOwnerAuthorizationStore::~ComponentOwnerAuthorizationStore()
{
    const std::scoped_lock lock(liveState_->mutex);
    liveState_->active = false;
    invalidateActivationUses(*liveState_, nullptr);
    liveState_->approvals.clear();
    liveState_->denials.clear();
}

ComponentOwnerAuthorizationEvidenceV1
ComponentOwnerAuthorizationStore::authorizationEvidence() const noexcept
{
    return ComponentOwnerAuthorizationEvidenceV1{liveState_};
}

std::uint64_t ComponentOwnerAuthorizationStore::generation() const
{
    const std::scoped_lock lock(liveState_->mutex);
    return liveState_->generation;
}

std::size_t ComponentOwnerAuthorizationStore::size() const
{
    const std::scoped_lock lock(liveState_->mutex);
    return liveState_->approvals.size() + liveState_->denials.size();
}

ComponentOwnerAuthorizationMutationResult
ComponentOwnerAuthorizationStore::recordOwnerAction(
    const ComponentApprovalFingerprint& fingerprint,
    ComponentOwnerAuthorizationAction action,
    std::uint64_t expectedGeneration)
{
    const auto owned = copyValidFingerprint(fingerprint);
    if (!owned)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::InvalidFingerprint,
            generation());
    }
    if (!validAction(action))
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::InvalidAction,
            generation());
    }

    const std::scoped_lock lock(liveState_->mutex);
    if (expectedGeneration != liveState_->generation)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::StaleGeneration,
            liveState_->generation);
    }
    auto& selected = action == ComponentOwnerAuthorizationAction::Approve
        ? liveState_->approvals
        : liveState_->denials;
    auto& opposite = action == ComponentOwnerAuthorizationAction::Approve
        ? liveState_->denials
        : liveState_->approvals;
    const auto ownedView = owned->view();
    if (findExact(selected, ownedView) != selected.end())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::DuplicateAction,
            liveState_->generation);
    }
    if (findExact(opposite, ownedView) != opposite.end())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::ReplacementRequired,
            liveState_->generation);
    }
    if (liveState_->approvals.size() + liveState_->denials.size()
        >= maximumEphemeralComponentOwnerAuthorizations)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::CapacityExceeded,
            liveState_->generation);
    }
    if (liveState_->generation == std::numeric_limits<std::uint64_t>::max())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::GenerationExhausted,
            liveState_->generation);
    }

    selected.push_back(*owned);
    ++liveState_->generation;
    return {
        ComponentOwnerAuthorizationMutationCode::Applied,
        liveState_->generation};
}

ComponentOwnerAuthorizationMutationResult
ComponentOwnerAuthorizationStore::replaceOwnerAction(
    const ComponentApprovalFingerprint& fingerprint,
    ComponentOwnerAuthorizationAction action,
    std::uint64_t expectedGeneration)
{
    const auto owned = copyValidFingerprint(fingerprint);
    if (!owned)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::InvalidFingerprint,
            generation());
    }
    if (!validAction(action))
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::InvalidAction,
            generation());
    }

    const std::scoped_lock lock(liveState_->mutex);
    if (expectedGeneration != liveState_->generation)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::StaleGeneration,
            liveState_->generation);
    }
    auto& selected = action == ComponentOwnerAuthorizationAction::Approve
        ? liveState_->approvals
        : liveState_->denials;
    auto& opposite = action == ComponentOwnerAuthorizationAction::Approve
        ? liveState_->denials
        : liveState_->approvals;
    const auto ownedView = owned->view();
    if (findExact(selected, ownedView) != selected.end())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::DuplicateAction,
            liveState_->generation);
    }
    const auto old = findExact(opposite, ownedView);
    if (old == opposite.end())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::MissingRecord,
            liveState_->generation);
    }
    if (liveState_->generation == std::numeric_limits<std::uint64_t>::max())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::GenerationExhausted,
            liveState_->generation);
    }

    selected.push_back(*owned);
    invalidateActivationUses(*liveState_, &ownedView);
    opposite.erase(old);
    ++liveState_->generation;
    return {
        ComponentOwnerAuthorizationMutationCode::Applied,
        liveState_->generation};
}

ComponentOwnerAuthorizationMutationResult
ComponentOwnerAuthorizationStore::revokeOwnerAction(
    const ComponentApprovalFingerprint& fingerprint,
    std::uint64_t expectedGeneration)
{
    const auto owned = copyValidFingerprint(fingerprint);
    if (!owned)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::InvalidFingerprint,
            generation());
    }

    const std::scoped_lock lock(liveState_->mutex);
    if (expectedGeneration != liveState_->generation)
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::StaleGeneration,
            liveState_->generation);
    }
    const auto ownedView = owned->view();
    const auto approved = findExact(liveState_->approvals, ownedView);
    const auto denied = findExact(liveState_->denials, ownedView);
    if (approved == liveState_->approvals.end()
        && denied == liveState_->denials.end())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::MissingRecord,
            liveState_->generation);
    }
    if (liveState_->generation == std::numeric_limits<std::uint64_t>::max())
    {
        return unchanged(
            ComponentOwnerAuthorizationMutationCode::GenerationExhausted,
            liveState_->generation);
    }

    if (approved != liveState_->approvals.end())
        liveState_->approvals.erase(approved);
    if (denied != liveState_->denials.end())
        liveState_->denials.erase(denied);
    invalidateActivationUses(*liveState_, &ownedView);
    ++liveState_->generation;
    return {
        ComponentOwnerAuthorizationMutationCode::Applied,
        liveState_->generation};
}

ComponentOwnerAuthorizationLookup ComponentOwnerAuthorizationStore::lookup(
    const ComponentApprovalFingerprint& fingerprint) const
{
    const auto validity =
        matchComponentOwnerApproval(fingerprint, &fingerprint);
    const std::scoped_lock lock(liveState_->mutex);
    if (validity.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || validity.diagnostic
            != ComponentOwnerApprovalMatchCode::ExactMatch)
    {
        return {validity, std::nullopt, liveState_->generation};
    }

    const auto approved = findExact(liveState_->approvals, fingerprint);
    const auto denied = findExact(liveState_->denials, fingerprint);
    if (approved != liveState_->approvals.end()
        && denied != liveState_->denials.end())
    {
        return {
            {ComponentOwnerApprovalState::Invalid,
             ComponentOwnerApprovalMatchCode::InvalidApproval},
            std::nullopt,
            liveState_->generation};
    }
    if (approved != liveState_->approvals.end())
    {
        const auto record = *approved;
        const auto recordView = record.view();
        return {
            matchComponentOwnerApproval(fingerprint, &recordView),
            record,
            liveState_->generation};
    }
    if (denied != liveState_->denials.end())
    {
        const auto record = *denied;
        const auto recordView = record.view();
        auto match = matchComponentOwnerApproval(fingerprint, &recordView);
        if (match.ownerApproval == ComponentOwnerApprovalState::ExactMatch
            && match.diagnostic
                == ComponentOwnerApprovalMatchCode::ExactMatch)
        {
            match.ownerApproval = ComponentOwnerApprovalState::Denied;
        }
        return {match, record, liveState_->generation};
    }

    return {
        matchComponentOwnerApproval(fingerprint, nullptr),
        std::nullopt,
        liveState_->generation};
}
}
