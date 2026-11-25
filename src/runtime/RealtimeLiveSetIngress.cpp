#include "talas/runtime/RealtimeLiveSetIngress.h"

#include <algorithm>
#include <limits>

namespace talas::runtime
{
struct RealtimeLiveSetIngress::Slot
{
    std::atomic<std::uint64_t> sequence = 0;
    RealtimeLiveSetRequest request;
};

RealtimeLiveSetIngress::RealtimeLiveSetIngress(std::size_t capacity)
{
    if (capacity < 2
        || capacity > maximumRealtimeLiveSetIngressCapacity
        || capacity > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max()))
    {
        return;
    }
    slots_ = std::make_unique<Slot[]>(capacity);
    capacity_ = capacity;
    for (std::size_t index = 0; index < capacity_; ++index)
    {
        slots_[index].sequence.store(
            static_cast<std::uint64_t>(index),
            std::memory_order_relaxed);
    }
}

RealtimeLiveSetIngress::~RealtimeLiveSetIngress() = default;

bool RealtimeLiveSetIngress::ready() const noexcept
{
    return slots_ != nullptr && capacity_ != 0;
}

std::size_t RealtimeLiveSetIngress::capacity() const noexcept
{
    return capacity_;
}

bool RealtimeLiveSetIngress::tryPush(
    const RealtimeLiveSetRequest& request) noexcept
{
    if (!ready())
        return false;
    auto position = enqueuePosition_.load(std::memory_order_relaxed);
    const auto maximumPosition =
        std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(capacity_);
    if (position > maximumPosition)
        return false;
    Slot* slot = nullptr;
    bool reserved = false;
    for (std::size_t attempt = 0;
         attempt < maximumProducerAttempts;
         ++attempt)
    {
        slot = &slots_[static_cast<std::size_t>(position % capacity_)];
        const auto sequence = slot->sequence.load(
            std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(
            sequence - position);
        if (difference == 0)
        {
            if (enqueuePosition_.compare_exchange_strong(
                    position,
                    position + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
            {
                reserved = true;
                break;
            }
            if (position > maximumPosition)
                return false;
        }
        else if (difference < 0)
        {
            return false;
        }
        else
        {
            position = enqueuePosition_.load(std::memory_order_relaxed);
            if (position > maximumPosition)
                return false;
        }
    }
    if (!reserved || slot == nullptr)
        return false;
    slot->request = request;
    slot->request.ingressSequence = position + 1;
    slot->sequence.store(position + 1, std::memory_order_release);
    return true;
}

bool RealtimeLiveSetIngress::tryPop(
    RealtimeLiveSetRequest& request) noexcept
{
    if (!ready())
        return false;
    if (dequeuePosition_ >
        std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(capacity_))
    {
        return false;
    }
    auto& slot = slots_[static_cast<std::size_t>(
        dequeuePosition_ % capacity_)];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);
    const auto difference = static_cast<std::int64_t>(
        sequence - (dequeuePosition_ + 1));
    if (difference != 0)
        return false;
    request = slot.request;
    slot.sequence.store(
        dequeuePosition_ + static_cast<std::uint64_t>(capacity_),
        std::memory_order_release);
    ++dequeuePosition_;
    return true;
}

void RealtimeLiveSetSourceAuthorityPublication::publish(
    std::uint64_t sourceGeneration,
    std::uint64_t routeGeneration) noexcept
{
    const auto current = publicationGeneration_.load(std::memory_order_seq_cst);
    if (current >= std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        publicationGeneration_.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_seq_cst);
        return;
    }
    publicationGeneration_.store(current + 1U, std::memory_order_seq_cst);
    routeGeneration_.store(routeGeneration, std::memory_order_seq_cst);
    sourceGeneration_.store(sourceGeneration, std::memory_order_seq_cst);
    publicationGeneration_.store(current + 2U, std::memory_order_seq_cst);
}

bool RealtimeLiveSetSourceAuthorityPublication::read(
    std::uint64_t& sourceGeneration,
    std::uint64_t& routeGeneration) const noexcept
{
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == std::numeric_limits<std::uint64_t>::max())
            return false;
        if ((before & 1U) != 0)
            continue;
        const auto source = sourceGeneration_.load(std::memory_order_seq_cst);
        const auto route = routeGeneration_.load(std::memory_order_seq_cst);
        const auto after = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0)
        {
            sourceGeneration = source;
            routeGeneration = route;
            return true;
        }
    }
    return false;
}

void RealtimeLiveSetSourceAuthorityPublication::clear() noexcept
{
    publish(0, 0);
}

void RealtimeLiveSetActiveStatePublication::publish(
    RealtimeLiveSetActiveState state) noexcept
{
    const auto current = publicationGeneration_.load(std::memory_order_seq_cst);
    if (current >= std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        publicationGeneration_.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_seq_cst);
        return;
    }
    publicationGeneration_.store(current + 1U, std::memory_order_seq_cst);
    requestGeneration_.store(state.requestGeneration, std::memory_order_seq_cst);
    cellIndexPlusOne_.store(state.cellIndexPlusOne, std::memory_order_seq_cst);
    scoreRegionState_.store(state.scoreRegionActive ? 1U : 0U,
                            std::memory_order_seq_cst);
    for (std::size_t column = 0;
         column < soundCellIndexPlusOneByColumn_.size(); ++column)
        soundCellIndexPlusOneByColumn_[column].store(
            state.soundCellIndexPlusOneByColumn[column],
            std::memory_order_seq_cst);
    mutedSoundColumnsMask_.store(
        state.mutedSoundColumnsMask, std::memory_order_seq_cst);
    publicationGeneration_.store(current + 2U, std::memory_order_seq_cst);
}

bool RealtimeLiveSetActiveStatePublication::read(
    RealtimeLiveSetActiveState& state) const noexcept
{
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == std::numeric_limits<std::uint64_t>::max())
            return false;
        if ((before & 1U) != 0)
            continue;
        RealtimeLiveSetActiveState candidate{
            requestGeneration_.load(std::memory_order_seq_cst),
            static_cast<std::uint32_t>(
                cellIndexPlusOne_.load(std::memory_order_seq_cst)),
            scoreRegionState_.load(std::memory_order_seq_cst) != 0,
        };
        for (std::size_t column = 0;
             column < soundCellIndexPlusOneByColumn_.size(); ++column)
            candidate.soundCellIndexPlusOneByColumn[column] =
                soundCellIndexPlusOneByColumn_[column].load(
                    std::memory_order_seq_cst);
        candidate.mutedSoundColumnsMask = mutedSoundColumnsMask_.load(
            std::memory_order_seq_cst);
        const auto after = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0)
        {
            state = candidate;
            return true;
        }
    }
    return false;
}

void RealtimeLiveSetActiveStatePublication::clear() noexcept
{
    publish({});
}

void RealtimeLiveSetTerminalStatePublication::publish(
    RealtimeLiveSetTerminalState state) noexcept
{
    const auto current = publicationGeneration_.load(std::memory_order_seq_cst);
    if (current >= std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        publicationGeneration_.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_seq_cst);
        return;
    }
    publicationGeneration_.store(current + 1U, std::memory_order_seq_cst);
    requestGeneration_.store(
        state.requestGeneration, std::memory_order_seq_cst);
    outcome_.store(
        static_cast<std::uint8_t>(state.outcome), std::memory_order_seq_cst);
    publicationGeneration_.store(current + 2U, std::memory_order_seq_cst);
}

bool RealtimeLiveSetTerminalStatePublication::read(
    RealtimeLiveSetTerminalState& state) const noexcept
{
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == std::numeric_limits<std::uint64_t>::max())
            return false;
        if ((before & 1U) != 0)
            continue;
        const RealtimeLiveSetTerminalState candidate{
            requestGeneration_.load(std::memory_order_seq_cst),
            static_cast<RealtimeLiveSetTerminalOutcome>(
                outcome_.load(std::memory_order_seq_cst)),
        };
        const auto after = publicationGeneration_.load(std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0)
        {
            state = candidate;
            return true;
        }
    }
    return false;
}

bool realtimeLiveSetRequestMatchesAuthority(
    const RealtimeLiveSetRequest& request,
    const RealtimeLiveSetAuthority& authority) noexcept
{
    const auto sourceIndex = static_cast<std::size_t>(request.sourceKind);
    if (sourceIndex >= authority.sourceGenerations.size())
        return false;
    const auto selectedSource = authority.sourceGenerations[sourceIndex] != 0 ||
                                        authority.routeGenerations[sourceIndex] != 0
                                    ? authority.sourceGenerations[sourceIndex]
                                    : authority.sourceGeneration;
    const auto selectedRoute = authority.sourceGenerations[sourceIndex] != 0 ||
                                       authority.routeGenerations[sourceIndex] != 0
                                   ? authority.routeGenerations[sourceIndex]
                                   : authority.routeGeneration;
    return request.documentAuthorityGeneration != 0 &&
           request.documentAuthorityGeneration == authority.documentAuthorityGeneration &&
           request.stageGeneration != 0 && request.stageGeneration == authority.stageGeneration &&
           request.stageId != 0 && request.stageId == authority.stageId &&
           request.transportEpoch != 0 && request.transportEpoch == authority.transportEpoch &&
           request.transportDiscontinuitySequence == authority.transportDiscontinuitySequence &&
           request.sourceGeneration != 0 && request.sourceGeneration == selectedSource &&
           request.routeGeneration != 0 && request.routeGeneration == selectedRoute;
}

RealtimeLiveSetScheduler::RealtimeLiveSetScheduler(std::size_t capacity)
{
    if (capacity < 2
        || capacity > maximumRealtimeLiveSetIngressCapacity)
    {
        return;
    }
    pending_ = std::make_unique<RealtimeLiveSetRequest[]>(capacity);
    capacity_ = capacity;
}

RealtimeLiveSetScheduler::~RealtimeLiveSetScheduler() = default;

bool RealtimeLiveSetScheduler::ready() const noexcept
{
    return pending_ != nullptr && capacity_ != 0;
}

std::size_t RealtimeLiveSetScheduler::capacity() const noexcept
{
    return capacity_;
}

std::size_t RealtimeLiveSetScheduler::size() const noexcept
{
    return size_;
}

bool RealtimeLiveSetScheduler::schedule(
    const RealtimeLiveSetRequest& request) noexcept
{
    if (!ready()
        || request.documentAuthorityGeneration == 0
        || request.stageGeneration == 0
        || request.stageId == 0
        || request.transportEpoch == 0
        || request.sourceGeneration == 0
        || request.routeGeneration == 0
        || request.requestGeneration == 0)
    {
        return false;
    }
    for (std::size_t index = 0; index < size_; ++index)
    {
        auto& existing = pending_[index];
        const auto sameAuthority =
            existing.resolvedFrame == request.resolvedFrame
            && existing.documentAuthorityGeneration
                == request.documentAuthorityGeneration
            && existing.stageGeneration == request.stageGeneration
            && existing.stageId == request.stageId
            && existing.transportEpoch == request.transportEpoch
            && existing.transportDiscontinuitySequence
                == request.transportDiscontinuitySequence;
        if (!sameAuthority)
            continue;
        const auto newer = request.requestGeneration
                > existing.requestGeneration
            || (request.requestGeneration == existing.requestGeneration
                && request.ingressSequence > existing.ingressSequence);
        if (newer)
            existing = request;
        return true;
    }
    if (size_ >= capacity_)
        return false;
    std::size_t insertion = size_;
    while (insertion != 0)
    {
        const auto& previous = pending_[insertion - 1U];
        if (previous.resolvedFrame < request.resolvedFrame
            || (previous.resolvedFrame == request.resolvedFrame
                && previous.requestGeneration
                    <= request.requestGeneration))
        {
            break;
        }
        pending_[insertion] = previous;
        --insertion;
    }
    pending_[insertion] = request;
    ++size_;
    return true;
}

std::optional<RealtimeLiveSetRequest>
RealtimeLiveSetScheduler::takeNextInRange(
    std::uint64_t callbackStart,
    std::uint64_t callbackEndExclusive) noexcept
{
    if (!ready() || callbackEndExclusive <= callbackStart)
        return std::nullopt;
    while (size_ != 0 && pending_[0].resolvedFrame < callbackStart)
    {
        for (std::size_t index = 1; index < size_; ++index)
            pending_[index - 1U] = pending_[index];
        --size_;
    }
    if (size_ == 0
        || pending_[0].resolvedFrame >= callbackEndExclusive)
    {
        return std::nullopt;
    }
    const auto result = pending_[0];
    for (std::size_t index = 1; index < size_; ++index)
        pending_[index - 1U] = pending_[index];
    --size_;
    return result;
}

std::optional<std::uint64_t> RealtimeLiveSetScheduler::nextFrame() const noexcept
{
    return ready() && size_ != 0 ? std::optional<std::uint64_t>{pending_[0].resolvedFrame}
                                 : std::nullopt;
}

std::size_t RealtimeLiveSetScheduler::discardBefore(
    std::uint64_t frame,
    std::uint64_t* latestDiscardedRequestGeneration) noexcept
{
    std::size_t discarded = 0;
    while (size_ != 0 && pending_[0].resolvedFrame < frame)
    {
        if (latestDiscardedRequestGeneration != nullptr)
        {
            *latestDiscardedRequestGeneration = std::max(
                *latestDiscardedRequestGeneration,
                pending_[0].requestGeneration);
        }
        for (std::size_t index = 1; index < size_; ++index)
            pending_[index - 1U] = pending_[index];
        --size_;
        ++discarded;
    }
    return discarded;
}

std::size_t RealtimeLiveSetScheduler::discardStale(
    const RealtimeLiveSetAuthority& authority,
    std::uint64_t* latestDiscardedRequestGeneration) noexcept
{
    std::size_t retained = 0;
    for (std::size_t index = 0; index < size_; ++index)
    {
        if (realtimeLiveSetRequestMatchesAuthority(pending_[index], authority))
        {
            if (retained != index)
                pending_[retained] = pending_[index];
            ++retained;
        }
        else if (latestDiscardedRequestGeneration != nullptr)
        {
            *latestDiscardedRequestGeneration = std::max(
                *latestDiscardedRequestGeneration,
                pending_[index].requestGeneration);
        }
    }
    const auto discarded = size_ - retained;
    size_ = retained;
    return discarded;
}

void RealtimeLiveSetScheduler::clear(
    std::uint64_t* latestDiscardedRequestGeneration) noexcept
{
    if (latestDiscardedRequestGeneration != nullptr)
    {
        for (std::size_t index = 0; index < size_; ++index)
        {
            *latestDiscardedRequestGeneration = std::max(
                *latestDiscardedRequestGeneration,
                pending_[index].requestGeneration);
        }
    }
    size_ = 0;
}
}
