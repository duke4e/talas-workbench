#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_time_control
{
enum class Error
{
    None,
    StateInvalid,
    CapacityExceeded,
    InputFramesOverflow,
    RateInvalid,
    ReadPositionInvalid,
    HistoryUnderrun,
    HistorySampleNonFinite,
    OutputNonFinite,
};

struct State
{
    std::size_t readIndex = 0;
    std::size_t writeIndex = 0;
    std::size_t count = 0;
    std::size_t startIndex = 0;
    std::size_t inputFramesSeen = 0;
    double readPosition = 0.0;
};

struct OutputTransition
{
    double output = 0.0;
    double nextReadPosition = 0.0;
};

[[nodiscard]] inline double maximumIndexExclusive() noexcept
{
    return std::ldexp(
        1.0, std::numeric_limits<std::size_t>::digits);
}

[[nodiscard]] inline bool indexPositionRepresentable(
    double position) noexcept
{
    return std::isfinite(position) && position >= 0.0
        && position < maximumIndexExclusive();
}

[[nodiscard]] inline bool valid(
    std::size_t capacity,
    const State& state) noexcept
{
    if (capacity == 0 || state.readIndex >= capacity
        || state.writeIndex >= capacity || state.count > capacity
        || state.startIndex
            > std::numeric_limits<std::size_t>::max() - state.count
        || state.startIndex + state.count != state.inputFramesSeen
        || !std::isfinite(state.readPosition)
        || state.readPosition < 0.0)
    {
        return false;
    }
    const auto distanceToWrap = capacity - state.readIndex;
    const auto expectedWriteIndex = state.count >= distanceToWrap
        ? state.count - distanceToWrap
        : state.readIndex + state.count;
    return state.writeIndex == expectedWriteIndex;
}

[[nodiscard]] inline Error append(
    std::size_t capacity,
    const State& state,
    std::size_t frameCount,
    State& destination) noexcept
{
    if (!valid(capacity, state))
        return Error::StateInvalid;
    if (frameCount > capacity - state.count)
        return Error::CapacityExceeded;
    if (frameCount
        > std::numeric_limits<std::size_t>::max()
            - state.inputFramesSeen)
    {
        return Error::InputFramesOverflow;
    }
    destination = state;
    destination.writeIndex =
        (state.writeIndex + frameCount) % capacity;
    destination.count += frameCount;
    destination.inputFramesSeen += frameCount;
    return Error::None;
}

[[nodiscard]] inline Error advance(
    std::span<const double> history,
    const State& state,
    double rate,
    OutputTransition& destination) noexcept
{
    if (!valid(history.size(), state))
        return Error::StateInvalid;
    if (!std::isfinite(rate) || rate < 0.0)
        return Error::RateInvalid;
    const auto basePosition = std::floor(state.readPosition);
    if (!indexPositionRepresentable(basePosition))
    {
        return Error::ReadPositionInvalid;
    }
    const auto baseIndex = static_cast<std::size_t>(basePosition);
    const auto sampleAt = [&](std::size_t globalIndex)
        -> const double* {
        if (globalIndex < state.startIndex)
            return nullptr;
        const auto offset = globalIndex - state.startIndex;
        if (offset >= state.count)
            return nullptr;
        return &history[(state.readIndex + offset) % history.size()];
    };
    const auto* current = sampleAt(baseIndex);
    if (current == nullptr)
        return Error::HistoryUnderrun;
    if (!std::isfinite(*current))
        return Error::HistorySampleNonFinite;
    auto next = *current;
    const auto fraction = state.readPosition - basePosition;
    if (fraction > 0.0)
    {
        if (const auto* nextSample = sampleAt(baseIndex + 1U);
            nextSample != nullptr)
        {
            if (!std::isfinite(*nextSample))
                return Error::HistorySampleNonFinite;
            next = *nextSample;
        }
    }
    const auto output = std::lerp(*current, next, fraction);
    if (!std::isfinite(output))
        return Error::OutputNonFinite;
    const auto nextReadPosition = state.readPosition + rate;
    if (!indexPositionRepresentable(nextReadPosition))
    {
        return Error::ReadPositionInvalid;
    }
    destination = {output, nextReadPosition};
    return Error::None;
}

[[nodiscard]] inline Error discardConsumed(
    std::size_t capacity,
    const State& state,
    State& destination) noexcept
{
    if (!valid(capacity, state))
        return Error::StateInvalid;
    destination = state;
    const auto keepFromPosition = std::floor(state.readPosition);
    if (keepFromPosition <= 1.0)
        return Error::None;
    if (!indexPositionRepresentable(keepFromPosition))
    {
        return Error::ReadPositionInvalid;
    }
    const auto keepFrom =
        static_cast<std::size_t>(keepFromPosition) - 1U;
    if (keepFrom <= state.startIndex)
        return Error::None;
    const auto discardCount =
        std::min(keepFrom - state.startIndex, state.count);
    destination.readIndex =
        (state.readIndex + discardCount) % capacity;
    destination.count -= discardCount;
    destination.startIndex += discardCount;
    return Error::None;
}
}
