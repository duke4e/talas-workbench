#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>

namespace talas::runtime::checked_overlapping_mixer
{
inline constexpr std::size_t maximumInputCount = 4;

struct InputSelection
{
    std::array<std::size_t, maximumInputCount> zeroBasedPortIndices{};
    std::size_t count = 0;
};

[[nodiscard]] inline bool parseInputSelection(
    std::string_view text,
    InputSelection& result) noexcept
{
    auto parsed = InputSelection{};
    std::array<bool, maximumInputCount> seen{};
    std::array<char, 3> token{};
    std::size_t tokenSize = 0;
    const auto flushToken = [&]() noexcept {
        if (tokenSize == 0)
            return true;
        if (tokenSize != token.size()
            || token[0] != 'i'
            || token[1] != 'n'
            || token[2] < '1'
            || token[2] > '4')
        {
            return false;
        }
        const auto index = static_cast<std::size_t>(token[2] - '1');
        if (seen[index] || parsed.count >= parsed.zeroBasedPortIndices.size())
            return false;
        seen[index] = true;
        parsed.zeroBasedPortIndices[parsed.count++] = index;
        tokenSize = 0;
        return true;
    };

    for (const auto ch : text)
    {
        if (std::isspace(static_cast<unsigned char>(ch))
            || ch == ','
            || ch == ';')
        {
            if (!flushToken())
                return false;
            continue;
        }
        if (tokenSize >= token.size())
            return false;
        token[tokenSize++] = ch;
    }
    if (!flushToken())
        return false;

    result = parsed;
    return true;
}

struct Frame
{
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] inline bool processFrame(
    std::span<const double> inputs,
    double leftGain,
    double rightGain,
    Frame& result) noexcept
{
    double mixed = 0.0;
    for (const auto input : inputs)
    {
        if (!std::isfinite(input))
            return false;
        mixed += input;
        if (!std::isfinite(mixed))
            return false;
    }

    if (!std::isfinite(leftGain)
        || leftGain < -1.0
        || leftGain > 1.0
        || !std::isfinite(rightGain)
        || rightGain < -1.0
        || rightGain > 1.0)
    {
        return false;
    }

    const auto left = mixed * leftGain;
    if (!std::isfinite(left))
        return false;
    const auto right = mixed * rightGain;
    if (!std::isfinite(right))
        return false;

    result = {left, right};
    return true;
}
}
