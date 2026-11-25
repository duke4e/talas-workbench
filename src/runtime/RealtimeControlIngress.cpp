#include "talas/runtime/RealtimeControlIngress.h"

#include <limits>

namespace talas::runtime
{
struct RealtimeControlIngress::Slot
{
    std::atomic<std::uint64_t> sequence = 0;
    RealtimeControlCommand command;
};

RealtimeControlIngress::RealtimeControlIngress(std::size_t capacity)
{
    if (capacity < 2
        || capacity > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return;
    }

    slots_ = std::make_unique<Slot[]>(capacity);
    capacity_ = capacity;
    for (std::size_t index = 0; index < capacity_; ++index)
        slots_[index].sequence.store(static_cast<std::uint64_t>(index), std::memory_order_relaxed);
}

RealtimeControlIngress::~RealtimeControlIngress() = default;

bool RealtimeControlIngress::ready() const noexcept
{
    return slots_ != nullptr && capacity_ != 0;
}

std::size_t RealtimeControlIngress::capacity() const noexcept
{
    return capacity_;
}

bool RealtimeControlIngress::tryPush(const RealtimeControlCommand& command) noexcept
{
    if (!ready())
        return false;

    auto position = enqueuePosition_.load(std::memory_order_relaxed);
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
                    position + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
            {
                reserved = true;
                break;
            }
        }
        else if (difference < 0)
        {
            return false;
        }
        else
        {
            position = enqueuePosition_.load(std::memory_order_relaxed);
        }
    }

    if (!reserved || slot == nullptr)
        return false;

    slot->command = command;
    slot->command.sequence = position;
    slot->sequence.store(position + 1, std::memory_order_release);
    return true;
}

bool RealtimeControlIngress::tryPop(RealtimeControlCommand& command) noexcept
{
    if (!ready())
        return false;

    auto& slot = slots_[static_cast<std::size_t>(dequeuePosition_ % capacity_)];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);
    const auto difference = static_cast<std::int64_t>(sequence - (dequeuePosition_ + 1));
    if (difference != 0)
        return false;

    command = slot.command;
    slot.sequence.store(dequeuePosition_ + static_cast<std::uint64_t>(capacity_), std::memory_order_release);
    ++dequeuePosition_;
    return true;
}
}
