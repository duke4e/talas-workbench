#pragma once

#include "talas/runtime/MtcTransportFollower.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace talas::runtime
{
class MtcRealtimeIngress
{
public:
    static constexpr std::size_t maximumProducerAttempts = 8;

    explicit MtcRealtimeIngress(std::size_t capacity);
    ~MtcRealtimeIngress();

    MtcRealtimeIngress(const MtcRealtimeIngress&) = delete;
    MtcRealtimeIngress& operator=(const MtcRealtimeIngress&) = delete;
    MtcRealtimeIngress(MtcRealtimeIngress&&) = delete;
    MtcRealtimeIngress& operator=(MtcRealtimeIngress&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool tryPush(const MtcTransportRecord& record) noexcept;
    [[nodiscard]] bool tryPop(MtcTransportRecord& record) noexcept;

private:
    struct Slot;

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> enqueuePosition_ = 0;
    std::uint64_t dequeuePosition_ = 0;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
