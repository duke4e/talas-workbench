#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::dsp
{
enum class TimeOffsetMode
{
    Forward,
    Reverse,
    Retrograde,
};

struct TimeOffsetGeometry
{
    std::size_t offsetFrames = 0;
    std::size_t capacity = 0;
    TimeOffsetMode mode = TimeOffsetMode::Forward;
};

struct TimeOffsetTransition
{
    double output = 0.0;
    double lineWrite = 0.0;
    std::size_t nextWriteIndex = 0;
    bool writesLine = false;
};

struct TimeOffsetFlags
{
    bool reverse = false;
    bool retrograde = false;
};

// A TimeOffset puts its silence after the Input when Reverse or
// Retrograde, but not both, is set, so the Input plays at once; with both,
// the silence comes first again. Projects
// before format 51 reversed the Input in windows of the offset when Reverse
// was set.
[[nodiscard]] constexpr TimeOffsetFlags effectiveTimeOffsetFlags(
    bool reverse,
    bool retrograde,
    bool legacyReverseWindows) noexcept
{
    if (legacyReverseWindows)
        return {reverse, retrograde};
    return {false, reverse != retrograde};
}

[[nodiscard]] inline bool checkedTimeOffsetFrames(
    double sampleRate,
    double offsetSeconds,
    std::size_t& frames) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(offsetSeconds) || offsetSeconds < 0.0)
    {
        return false;
    }

    const auto frameProduct = static_cast<long double>(sampleRate)
        * static_cast<long double>(offsetSeconds);
    const auto roundedFrames = std::round(frameProduct);
    const auto maximumFramesExclusive = std::ldexp(
        1.0L, std::numeric_limits<std::size_t>::digits);
    if (!std::isfinite(roundedFrames) || roundedFrames < 0.0L
        || roundedFrames >= maximumFramesExclusive)
    {
        return false;
    }

    frames = static_cast<std::size_t>(roundedFrames);
    return true;
}

[[nodiscard]] inline bool checkedTimeOffsetGeometry(
    double sampleRate,
    double offsetSeconds,
    bool reverse,
    bool retrograde,
    TimeOffsetGeometry& geometry) noexcept
{
    std::size_t offsetFrames = 0;
    if (!checkedTimeOffsetFrames(
            sampleRate, offsetSeconds, offsetFrames))
    {
        return false;
    }

    geometry = {
        .offsetFrames = offsetFrames,
        .capacity = offsetFrames,
        .mode = TimeOffsetMode::Forward,
    };
    if (retrograde)
    {
        geometry.capacity = 0;
        geometry.mode = TimeOffsetMode::Retrograde;
        return true;
    }
    if (!reverse)
        return true;

    geometry.mode = TimeOffsetMode::Reverse;
    if (offsetFrames == 0)
    {
        geometry.capacity = 0;
        return true;
    }
    if (offsetFrames > std::numeric_limits<std::size_t>::max() / 2)
        return false;
    geometry.capacity = offsetFrames * 2;
    return true;
}

[[nodiscard]] inline bool checkedTimeOffsetTransition(
    double input,
    std::size_t offsetFrames,
    TimeOffsetMode mode,
    std::span<const double> line,
    std::size_t writeIndex,
    TimeOffsetTransition& transition) noexcept
{
    if (!std::isfinite(input)
        || (line.empty() ? writeIndex != 0 : writeIndex >= line.size()))
    {
        return false;
    }

    auto output = input;
    if (mode == TimeOffsetMode::Retrograde)
    {
        if (!line.empty())
            return false;
    }
    else if (mode == TimeOffsetMode::Reverse && offsetFrames > 0)
    {
        if (line.size() % 2 != 0
            || line.size() / 2 != offsetFrames)
        {
            return false;
        }
        const auto positionInWindow = writeIndex % offsetFrames;
        const auto currentWindowStart =
            (writeIndex + line.size() - positionInWindow) % line.size();
        const auto previousWindowStart =
            (currentWindowStart + line.size() - offsetFrames) % line.size();
        const auto readIndex =
            (previousWindowStart + offsetFrames - 1 - positionInWindow)
            % line.size();
        output = line[readIndex];
    }
    else if (offsetFrames > 0)
    {
        if (mode != TimeOffsetMode::Forward
            || offsetFrames > line.size())
        {
            return false;
        }
        const auto readIndex =
            (writeIndex + line.size() - offsetFrames) % line.size();
        output = line[readIndex];
    }

    transition = {
        .output = output,
        .lineWrite = input,
        .nextWriteIndex = line.empty()
            ? std::size_t{0}
            : (writeIndex + 1 == line.size() ? 0 : writeIndex + 1),
        .writesLine = !line.empty(),
    };
    return true;
}
}
