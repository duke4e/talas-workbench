#include "talas/runtime/RealtimeLiveToolIngress.h"

#include <algorithm>
#include <limits>

namespace talas::runtime
{
struct RealtimeLiveToolIngress::Slot
{
    std::atomic<std::uint64_t> sequence = 0;
    RealtimeLiveToolTriggerRequest request;
};

bool setRealtimeLiveToolTriggerIdentifier(
    RealtimeLiveToolTriggerRequest& request,
    std::string_view identifier) noexcept
{
    if (identifier.size() > maximumRealtimeLiveToolIdentifierBytes)
        return false;
    std::fill(request.identifier.begin(), request.identifier.end(), '\0');
    std::copy(identifier.begin(), identifier.end(), request.identifier.begin());
    request.identifierSize = static_cast<std::uint16_t>(identifier.size());
    return true;
}

bool realtimeLiveToolTriggerMatchesAuthority(
    const RealtimeLiveToolTriggerRequest& request,
    const RealtimeLiveToolAuthority& authority) noexcept
{
    const auto sourceIndex = static_cast<std::size_t>(request.sourceKind);
    return sourceIndex < authority.sourceGenerations.size()
        && request.requestGeneration != 0
        && request.documentAuthorityGeneration != 0
        && request.documentAuthorityGeneration
            == authority.documentAuthorityGeneration
        && request.stageGeneration != 0
        && request.stageGeneration == authority.stageGeneration
        && request.stageId != 0 && request.stageId == authority.stageId
        && request.sourceGeneration != 0
        && request.sourceGeneration
            == authority.sourceGenerations[sourceIndex]
        && request.routeGeneration != 0
        && request.routeGeneration
            == authority.routeGenerations[sourceIndex];
}

RealtimeLiveToolIngress::RealtimeLiveToolIngress(std::size_t capacity)
{
    if (capacity < 2
        || capacity > talas::project::maximumRealtimeLiveToolTriggerQueue
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

RealtimeLiveToolIngress::~RealtimeLiveToolIngress() = default;

bool RealtimeLiveToolIngress::ready() const noexcept
{
    return slots_ != nullptr && capacity_ != 0;
}

std::size_t RealtimeLiveToolIngress::capacity() const noexcept
{
    return capacity_;
}

bool RealtimeLiveToolIngress::tryPush(
    const RealtimeLiveToolTriggerRequest& request) noexcept
{
    if (!ready() || request.identifierSize > maximumRealtimeLiveToolIdentifierBytes)
        return false;
    auto position = enqueuePosition_.load(std::memory_order_relaxed);
    const auto maximumPosition = std::numeric_limits<std::uint64_t>::max()
        - static_cast<std::uint64_t>(capacity_);
    if (position > maximumPosition)
        return false;
    Slot* slot = nullptr;
    bool reserved = false;
    for (std::size_t attempt = 0;
         attempt < maximumProducerAttempts;
         ++attempt)
    {
        slot = &slots_[static_cast<std::size_t>(position % capacity_)];
        const auto sequence = slot->sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence - position);
        if (difference == 0)
        {
            if (enqueuePosition_.compare_exchange_strong(
                    position,
                    position + 1U,
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
    slot->request.ingressSequence = position + 1U;
    slot->sequence.store(position + 1U, std::memory_order_release);
    return true;
}

bool RealtimeLiveToolIngress::tryPop(
    RealtimeLiveToolTriggerRequest& request) noexcept
{
    if (!ready()
        || dequeuePosition_ > std::numeric_limits<std::uint64_t>::max()
                - static_cast<std::uint64_t>(capacity_))
    {
        return false;
    }
    auto& slot = slots_[static_cast<std::size_t>(dequeuePosition_ % capacity_)];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);
    const auto difference = static_cast<std::int64_t>(
        sequence - (dequeuePosition_ + 1U));
    if (difference != 0)
        return false;
    request = slot.request;
    slot.sequence.store(
        dequeuePosition_ + static_cast<std::uint64_t>(capacity_),
        std::memory_order_release);
    ++dequeuePosition_;
    return true;
}

void RealtimeLiveToolSourceAuthorityPublication::publish(
    std::uint64_t sourceGeneration,
    std::uint64_t routeGeneration) noexcept
{
    const auto current = publicationGeneration_.load(
        std::memory_order_seq_cst);
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

bool RealtimeLiveToolSourceAuthorityPublication::read(
    std::uint64_t& sourceGeneration,
    std::uint64_t& routeGeneration) const noexcept
{
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publicationGeneration_.load(
            std::memory_order_seq_cst);
        if (before == std::numeric_limits<std::uint64_t>::max())
            return false;
        if ((before & 1U) != 0)
            continue;
        const auto source = sourceGeneration_.load(
            std::memory_order_seq_cst);
        const auto route = routeGeneration_.load(
            std::memory_order_seq_cst);
        const auto after = publicationGeneration_.load(
            std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0)
        {
            sourceGeneration = source;
            routeGeneration = route;
            return true;
        }
    }
    return false;
}

void RealtimeLiveToolSourceAuthorityPublication::clear() noexcept
{
    publish(0, 0);
}
}
