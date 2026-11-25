#include "talas/runtime/MtcRealtimeIngress.h"

#include <limits>

namespace talas::runtime
{
struct MtcRealtimeIngress::Slot
{
    std::atomic<std::uint64_t> sequence = 0;
    MtcTransportRecord record;
};

MtcRealtimeIngress::MtcRealtimeIngress(std::size_t capacity)
{
    if (capacity < 2
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

MtcRealtimeIngress::~MtcRealtimeIngress() = default;

bool MtcRealtimeIngress::ready() const noexcept
{
    return slots_ != nullptr && capacity_ != 0;
}

std::size_t MtcRealtimeIngress::capacity() const noexcept
{
    return capacity_;
}

bool MtcRealtimeIngress::tryPush(
    const MtcTransportRecord& record) noexcept
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
    slot->record = record;
    slot->record.ingressSequence = position + 1;
    slot->sequence.store(position + 1, std::memory_order_release);
    return true;
}

bool MtcRealtimeIngress::tryPop(MtcTransportRecord& record) noexcept
{
    if (!ready())
        return false;
    auto& slot = slots_[static_cast<std::size_t>(
        dequeuePosition_ % capacity_)];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);
    const auto difference = static_cast<std::int64_t>(
        sequence - (dequeuePosition_ + 1));
    if (difference != 0)
        return false;
    record = slot.record;
    slot.sequence.store(
        dequeuePosition_ + static_cast<std::uint64_t>(capacity_),
        std::memory_order_release);
    ++dequeuePosition_;
    return true;
}
}
