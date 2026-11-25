#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace talas::app::workbench
{
// These authorities have one message/control-thread writer and concurrent
// readers. UINT64_MAX is a permanent poisoned sentinel: once the identity
// space is exhausted, zero is returned forever and no generation can rebind.
[[nodiscard]] inline std::uint64_t advanceWorkbenchAuthorityGeneration(
    std::atomic<std::uint64_t>& generation) noexcept
{
    const auto current = generation.load(std::memory_order_acquire);
    if (current == std::numeric_limits<std::uint64_t>::max())
        return 0;
    const auto next = current + 1U;
    generation.store(next, std::memory_order_release);
    return next == std::numeric_limits<std::uint64_t>::max() ? 0 : next;
}

[[nodiscard]] inline std::uint64_t currentWorkbenchAuthorityGeneration(
    const std::atomic<std::uint64_t>& generation) noexcept
{
    const auto current = generation.load(std::memory_order_acquire);
    return current == std::numeric_limits<std::uint64_t>::max() ? 0 : current;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
