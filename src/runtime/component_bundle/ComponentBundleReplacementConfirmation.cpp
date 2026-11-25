#include "runtime/component_bundle/ComponentBundleReplacementConfirmation.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace talas::runtime::detail
{
enum class ComponentBundleReplacementConfirmationStatusV1 : std::uint8_t
{
    Empty,
    Prepared,
    Confirmed,
    InUse,
    Consumed,
    Cancelled,
};

struct ComponentBundleReplacementConfirmationRecordV1
{
    std::uint64_t documentGeneration = 0;
    std::uint64_t previewGeneration = 0;
    std::string nodeId;
    component_bundle::OwnedComponentApprovalFingerprint candidate;
    ComponentOwnerAuthorizationEvidenceV1 ownerAuthorization;
    std::shared_ptr<const component_bundle::ComponentBundleReplacementPreview>
        preview;
    ComponentBundleReplacementConfirmationStatusV1 status =
        ComponentBundleReplacementConfirmationStatusV1::Empty;
};

struct ComponentBundleReplacementConfirmationStateV1
{
    mutable std::mutex mutex;
    std::array<
        std::uint8_t,
        component_bundle::componentBundleReplacementSessionNonceBytes>
        sessionNonce{};
    ComponentBundleReplacementConfirmationRecordV1 record;
    std::uint64_t nextPreviewGeneration = 1;
    std::atomic<std::uint64_t> liveReceiptState{0};
    bool active = false;
};
}

namespace talas::runtime::component_bundle
{
namespace
{
using Status = detail::ComponentBundleReplacementConfirmationStatusV1;
inline constexpr std::uint64_t receiptInUseFlag =
    std::uint64_t{1} << 63U;
inline constexpr std::uint64_t receiptCancelRequestedFlag =
    std::uint64_t{1} << 62U;
inline constexpr std::uint64_t maximumReceiptGeneration =
    receiptCancelRequestedFlag - 1U;

[[nodiscard]] bool nonZeroNonce(
    const std::array<
        std::uint8_t,
        componentBundleReplacementSessionNonceBytes>& nonce) noexcept
{
    return std::ranges::any_of(nonce, [](std::uint8_t value) {
        return value != 0;
    });
}

[[nodiscard]] std::optional<OwnedComponentApprovalFingerprint>
copyFingerprint(const ComponentApprovalFingerprint& fingerprint)
{
    const auto match = matchComponentOwnerApproval(fingerprint, &fingerprint);
    if (match.ownerApproval != ComponentOwnerApprovalState::ExactMatch
        || match.diagnostic != ComponentOwnerApprovalMatchCode::ExactMatch)
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
    return owned;
}

[[nodiscard]] bool exactApproval(
    const detail::ComponentBundleReplacementConfirmationRecordV1& record)
    noexcept
{
    const auto match = record.ownerAuthorization.inspect(
        record.candidate.view());
    return match.ownerApproval == ComponentOwnerApprovalState::ExactMatch
        && match.diagnostic == ComponentOwnerApprovalMatchCode::ExactMatch;
}

void reconcileCancelledConfirmedReceipt(
    detail::ComponentBundleReplacementConfirmationStateV1& state) noexcept
{
    if (state.record.status == Status::Confirmed
        && state.liveReceiptState.load(std::memory_order_acquire)
            != state.record.previewGeneration)
    {
        state.record.status = Status::Cancelled;
    }
}

void cancelConfirmedReceipt(
    const std::shared_ptr<
        detail::ComponentBundleReplacementConfirmationStateV1>& state,
    const std::array<
        std::uint8_t,
        componentBundleReplacementSessionNonceBytes>& sessionNonce,
    std::uint64_t previewGeneration) noexcept
{
    if (!state || previewGeneration == 0)
        return;
    auto observed = state->liveReceiptState.load(std::memory_order_acquire);
    for (;;)
    {
        if (observed == previewGeneration)
        {
            if (state->liveReceiptState.compare_exchange_weak(
                    observed,
                    0,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                break;
            }
            continue;
        }
        if (observed == (previewGeneration | receiptInUseFlag))
        {
            const auto cancelled = observed
                | receiptCancelRequestedFlag;
            if (state->liveReceiptState.compare_exchange_weak(
                    observed,
                    cancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }
            continue;
        }
        return;
    }
    std::unique_lock lock(state->mutex, std::try_to_lock);
    if (lock.owns_lock() && state->active
        && state->sessionNonce == sessionNonce
        && state->record.previewGeneration == previewGeneration
        && state->record.status == Status::Confirmed)
    {
        state->record.status = Status::Cancelled;
    }
}
}

ComponentBundleReplacementConsumptionLease::
    ComponentBundleReplacementConsumptionLease(
        std::shared_ptr<
            detail::ComponentBundleReplacementConfirmationStateV1> state,
        std::unique_lock<std::mutex> stateLock,
        ComponentOwnerAuthorizationLeaseV1 ownerLease,
        std::uint64_t previewGeneration) noexcept
    : state_(std::move(state)),
      stateLock_(std::move(stateLock)),
      ownerLease_(std::move(ownerLease)),
      previewGeneration_(previewGeneration)
{
}

ComponentBundleReplacementConsumptionLease::
    ~ComponentBundleReplacementConsumptionLease()
{
    release(committed_);
}

ComponentBundleReplacementConsumptionLease::
    ComponentBundleReplacementConsumptionLease(
        ComponentBundleReplacementConsumptionLease&& other) noexcept
    : state_(std::move(other.state_)),
      stateLock_(std::move(other.stateLock_)),
      ownerLease_(std::move(other.ownerLease_)),
      previewGeneration_(std::exchange(other.previewGeneration_, 0)),
      committed_(std::exchange(other.committed_, false))
{
}

ComponentBundleReplacementConsumptionLease&
ComponentBundleReplacementConsumptionLease::operator=(
    ComponentBundleReplacementConsumptionLease&& other) noexcept
{
    if (this == &other)
        return *this;
    release(committed_);
    state_ = std::move(other.state_);
    stateLock_ = std::move(other.stateLock_);
    ownerLease_ = std::move(other.ownerLease_);
    previewGeneration_ = std::exchange(other.previewGeneration_, 0);
    committed_ = std::exchange(other.committed_, false);
    return *this;
}

bool ComponentBundleReplacementConsumptionLease::approved() const noexcept
{
    return state_ && stateLock_.owns_lock() && ownerLease_.approved()
        && previewGeneration_ != 0
        && state_->active
        && state_->record.previewGeneration == previewGeneration_
        && state_->record.status == Status::InUse;
}

void ComponentBundleReplacementConsumptionLease::commit() noexcept
{
    if (approved())
        committed_ = true;
}

void ComponentBundleReplacementConsumptionLease::release(
    bool consumed) noexcept
{
    if (state_ && stateLock_.owns_lock()
        && state_->record.previewGeneration == previewGeneration_
        && state_->record.status == Status::InUse)
    {
        if (consumed)
        {
            state_->record.status = Status::Consumed;
            state_->liveReceiptState.store(
                0, std::memory_order_release);
        }
        else
        {
            const auto receiptState = state_->liveReceiptState.load(
                std::memory_order_acquire);
            if (receiptState
                == (previewGeneration_ | receiptInUseFlag))
            {
                state_->record.status = Status::Confirmed;
                state_->liveReceiptState.store(
                    previewGeneration_, std::memory_order_release);
            }
            else
            {
                state_->record.status = Status::Cancelled;
                state_->liveReceiptState.store(
                    0, std::memory_order_release);
            }
        }
    }
    ownerLease_ = {};
    if (stateLock_.owns_lock())
        stateLock_.unlock();
    state_.reset();
    previewGeneration_ = 0;
    committed_ = false;
}

ComponentBundleReplacementReceipt::ComponentBundleReplacementReceipt(
    std::shared_ptr<detail::ComponentBundleReplacementConfirmationStateV1>
        state,
    std::array<
        std::uint8_t,
        componentBundleReplacementSessionNonceBytes> sessionNonce,
    std::uint64_t previewGeneration) noexcept
    : state_(std::move(state)),
      sessionNonce_(sessionNonce),
      previewGeneration_(previewGeneration)
{
}

ComponentBundleReplacementReceipt::~ComponentBundleReplacementReceipt()
{
    cancel();
}

ComponentBundleReplacementReceipt::ComponentBundleReplacementReceipt(
    ComponentBundleReplacementReceipt&& other) noexcept
    : state_(std::move(other.state_)),
      sessionNonce_(other.sessionNonce_),
      previewGeneration_(std::exchange(other.previewGeneration_, 0))
{
    other.sessionNonce_.fill(0);
}

ComponentBundleReplacementReceipt&
ComponentBundleReplacementReceipt::operator=(
    ComponentBundleReplacementReceipt&& other) noexcept
{
    if (this == &other)
        return *this;
    cancel();
    state_ = std::move(other.state_);
    sessionNonce_ = other.sessionNonce_;
    previewGeneration_ = std::exchange(other.previewGeneration_, 0);
    other.sessionNonce_.fill(0);
    return *this;
}

bool ComponentBundleReplacementReceipt::active() const noexcept
{
    if (!state_ || previewGeneration_ == 0)
        return false;
    if (state_->liveReceiptState.load(std::memory_order_acquire)
        != previewGeneration_)
    {
        return false;
    }
    const std::scoped_lock lock(state_->mutex);
    return state_->active && state_->sessionNonce == sessionNonce_
        && state_->record.previewGeneration == previewGeneration_
        && state_->record.status == Status::Confirmed
        && state_->liveReceiptState.load(std::memory_order_relaxed)
            == previewGeneration_
        && exactApproval(state_->record);
}

ComponentBundleReplacementConsumptionLease
ComponentBundleReplacementReceipt::tryConsume(
    const ComponentBundleReplacementConsumptionExpectation& expectation)
    noexcept
{
    if (!state_ || previewGeneration_ == 0)
        return {};
    if (state_->liveReceiptState.load(std::memory_order_acquire)
        != previewGeneration_)
    {
        return {};
    }
    std::unique_lock lock(state_->mutex);
    auto& record = state_->record;
    if (!state_->active || state_->sessionNonce != sessionNonce_
        || record.previewGeneration != previewGeneration_
        || record.status != Status::Confirmed
        || state_->liveReceiptState.load(std::memory_order_relaxed)
            != previewGeneration_
        || expectation.documentGeneration == 0
        || expectation.documentGeneration != record.documentGeneration
        || expectation.nodeId != record.nodeId
        || record.preview == nullptr
        || expectation.preservedCapsuleDigest
            != record.preview->preservedCapsuleDigest
        || expectation.candidate != record.candidate.view()
        || !std::ranges::equal(
            expectation.resourceTransitions,
            record.preview->resourceTransitions))
    {
        return {};
    }
    auto ownerLease = record.ownerAuthorization.acquireExactApprovalLease(
        record.candidate.view());
    if (!ownerLease.approved())
        return {};
    auto expectedReceiptState = previewGeneration_;
    if (!state_->liveReceiptState.compare_exchange_strong(
            expectedReceiptState,
            previewGeneration_ | receiptInUseFlag,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return {};
    }
    record.status = Status::InUse;
    return ComponentBundleReplacementConsumptionLease{
        state_,
        std::move(lock),
        std::move(ownerLease),
        previewGeneration_};
}

void ComponentBundleReplacementReceipt::cancel() noexcept
{
    cancelConfirmedReceipt(
        state_, sessionNonce_, previewGeneration_);
    state_.reset();
    sessionNonce_.fill(0);
    previewGeneration_ = 0;
}

ComponentBundleReplacementConfirmationStore::
    ComponentBundleReplacementConfirmationStore(
        std::array<
            std::uint8_t,
            componentBundleReplacementSessionNonceBytes> sessionNonce)
{
    try
    {
        state_ = std::make_shared<
            detail::ComponentBundleReplacementConfirmationStateV1>();
        state_->sessionNonce = sessionNonce;
        state_->active = nonZeroNonce(sessionNonce);
    }
    catch (const std::bad_alloc&)
    {
    }
}

ComponentBundleReplacementConfirmationStore::
    ~ComponentBundleReplacementConfirmationStore()
{
    if (!state_)
        return;
    const std::scoped_lock lock(state_->mutex);
    state_->active = false;
    state_->liveReceiptState.store(0, std::memory_order_release);
    state_->record.status = Status::Cancelled;
    state_->record.preview.reset();
}

ComponentBundleReplacementPrepareResult
ComponentBundleReplacementConfirmationStore::prepare(
    const ComponentBundleReplacementPrepareInputs& inputs) noexcept
{
    ComponentBundleReplacementPrepareResult result;
    if (!state_ || inputs.documentGeneration == 0 || inputs.nodeId.empty()
        || inputs.replacement.trustEvidence == nullptr)
    {
        return result;
    }
    try
    {
        auto previewValue = previewComponentBundleReplacement(
            inputs.replacement);
        auto preview = std::make_shared<ComponentBundleReplacementPreview>(
            std::move(previewValue));
        result.preview = preview;
        if (!preview->compatible())
        {
            result.code = ComponentBundleReplacementPrepareCode::Incompatible;
            return result;
        }
        auto fingerprint = copyFingerprint(
            inputs.replacement.trustEvidence->candidate);
        if (!fingerprint.has_value())
            return result;

        std::scoped_lock lock(state_->mutex);
        if (!state_->active)
            return result;
        reconcileCancelledConfirmedReceipt(*state_);
        if (state_->record.status == Status::Prepared
            || state_->record.status == Status::Confirmed
            || state_->record.status == Status::InUse)
        {
            result.code = ComponentBundleReplacementPrepareCode::Busy;
            return result;
        }
        if (state_->nextPreviewGeneration > maximumReceiptGeneration)
        {
            result.code =
                ComponentBundleReplacementPrepareCode::GenerationExhausted;
            return result;
        }
        const auto ownerAuthorization =
            inputs.replacement.trustEvidence->ownerAuthorizationEvidence;
        const auto ownerMatch = ownerAuthorization.inspect(fingerprint->view());
        if (ownerMatch.ownerApproval
                != ComponentOwnerApprovalState::ExactMatch
            || ownerMatch.diagnostic
                != ComponentOwnerApprovalMatchCode::ExactMatch)
        {
            return result;
        }

        detail::ComponentBundleReplacementConfirmationRecordV1 next;
        next.documentGeneration = inputs.documentGeneration;
        next.previewGeneration = state_->nextPreviewGeneration++;
        next.nodeId = inputs.nodeId;
        next.candidate = std::move(*fingerprint);
        next.ownerAuthorization = ownerAuthorization;
        next.preview = preview;
        next.status = Status::Prepared;
        state_->record = std::move(next);
        result.code = ComponentBundleReplacementPrepareCode::Ready;
        result.previewGeneration = state_->record.previewGeneration;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.code = ComponentBundleReplacementPrepareCode::AllocationFailed;
        result.preview.reset();
        return result;
    }
    catch (const std::length_error&)
    {
        result.code = ComponentBundleReplacementPrepareCode::AllocationFailed;
        result.preview.reset();
        return result;
    }
}

std::optional<ComponentBundleReplacementReceipt>
ComponentBundleReplacementConfirmationStore::confirm(
    std::uint64_t previewGeneration) noexcept
{
    if (!state_ || previewGeneration == 0)
        return std::nullopt;
    const std::scoped_lock lock(state_->mutex);
    if (!state_->active
        || state_->record.previewGeneration != previewGeneration
        || state_->record.status != Status::Prepared
        || !exactApproval(state_->record))
    {
        return std::nullopt;
    }
    state_->record.status = Status::Confirmed;
    state_->liveReceiptState.store(
        previewGeneration, std::memory_order_release);
    return ComponentBundleReplacementReceipt{
        state_, state_->sessionNonce, previewGeneration};
}

bool ComponentBundleReplacementConfirmationStore::cancel(
    std::uint64_t previewGeneration) noexcept
{
    if (!state_ || previewGeneration == 0)
        return false;
    const std::scoped_lock lock(state_->mutex);
    if (!state_->active
        || state_->record.previewGeneration != previewGeneration
        || (state_->record.status != Status::Prepared
            && state_->record.status != Status::Confirmed))
    {
        return false;
    }
    state_->record.status = Status::Cancelled;
    state_->liveReceiptState.store(0, std::memory_order_release);
    return true;
}

std::shared_ptr<const ComponentBundleReplacementPreview>
ComponentBundleReplacementConfirmationStore::preview(
    std::uint64_t previewGeneration) const noexcept
{
    if (!state_ || previewGeneration == 0)
        return {};
    const std::scoped_lock lock(state_->mutex);
    reconcileCancelledConfirmedReceipt(*state_);
    return state_->active
            && state_->record.previewGeneration == previewGeneration
            && (state_->record.status == Status::Prepared
                || state_->record.status == Status::Confirmed)
        ? state_->record.preview
        : std::shared_ptr<const ComponentBundleReplacementPreview>{};
}

#if defined(TALAS_COMPONENT_BUNDLE_CONFIRMATION_TEST_SUPPORT)
bool ComponentBundleReplacementConfirmationTestPeer::
    cancelReceiptWhileStoreLocked(
        ComponentBundleReplacementConfirmationStore& store,
        ComponentBundleReplacementReceipt& receipt) noexcept
{
    if (!store.state_)
        return false;
    try
    {
        const std::scoped_lock lock(store.state_->mutex);
        std::thread canceller([&receipt] { receipt.cancel(); });
        canceller.join();
        return store.state_->record.status == Status::Confirmed
            && store.state_->liveReceiptState.load(std::memory_order_acquire)
                == 0;
    }
    catch (...)
    {
        return false;
    }
}
#endif
}
