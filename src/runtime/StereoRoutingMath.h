#pragma once

#include <cmath>

namespace talas::runtime::stereo_routing
{

[[nodiscard]] inline bool level(
    double input,
    double leftGain,
    double rightGain,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(input)
        || !std::isfinite(leftGain)
        || !std::isfinite(rightGain))
    {
        return false;
    }
    const auto candidateLeft = input * leftGain;
    const auto candidateRight = input * rightGain;
    if (!std::isfinite(candidateLeft) || !std::isfinite(candidateRight))
        return false;
    leftOutput = candidateLeft;
    rightOutput = candidateRight;
    return true;
}

} // namespace talas::runtime::stereo_routing
