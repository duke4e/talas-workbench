#pragma once

#include <cmath>
#include <cstddef>
#include <optional>

namespace talas::runtime
{
struct EqualPowerRingPan
{
    std::size_t firstChannel = 0;
    std::size_t secondChannel = 0;
    double firstGain = 0.0;
    double secondGain = 0.0;
};

struct HorizontalAmbisonicEncoding
{
    double w = 0.0;
    double x = 0.0;
    double y = 0.0;
};

struct HorizontalAmbisonicDecoding
{
    double w = 0.0;
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] inline std::optional<EqualPowerRingPan>
equalPowerRingPan(double normalizedPosition, std::size_t channelCount) noexcept
{
    if (!std::isfinite(normalizedPosition)
        || normalizedPosition < 0.0
        || normalizedPosition > 1.0
        || (channelCount != 4 && channelCount != 8))
    {
        return std::nullopt;
    }

    const auto wrappedPosition = normalizedPosition == 1.0
        ? 0.0
        : normalizedPosition;
    const auto speakerPosition =
        wrappedPosition * static_cast<double>(channelCount);
    const auto firstChannel = static_cast<std::size_t>(
        std::floor(speakerPosition));
    const auto localPosition =
        speakerPosition - static_cast<double>(firstChannel);
    constexpr double halfPi = 1.57079632679489661923;
    return EqualPowerRingPan{
        firstChannel,
        (firstChannel + 1) % channelCount,
        std::cos(localPosition * halfPi),
        std::sin(localPosition * halfPi),
    };
}

[[nodiscard]] inline std::optional<HorizontalAmbisonicEncoding>
horizontalAmbisonicEncoding(double normalizedAzimuth) noexcept
{
    if (!std::isfinite(normalizedAzimuth)
        || normalizedAzimuth < 0.0
        || normalizedAzimuth > 1.0)
    {
        return std::nullopt;
    }

    constexpr double inverseSqrtTwo = 0.70710678118654752440;
    constexpr double twoPi = 6.28318530717958647693;
    const auto wrappedAzimuth = normalizedAzimuth == 1.0
        ? 0.0
        : normalizedAzimuth;
    const auto angle = wrappedAzimuth * twoPi;
    return HorizontalAmbisonicEncoding{
        inverseSqrtTwo,
        std::cos(angle),
        std::sin(angle),
    };
}

[[nodiscard]] inline std::optional<HorizontalAmbisonicDecoding>
horizontalAmbisonicDecoding(
    std::size_t channel,
    std::size_t channelCount) noexcept
{
    if ((channelCount != 4 && channelCount != 8)
        || channel >= channelCount)
    {
        return std::nullopt;
    }

    constexpr double inverseSqrtTwo = 0.70710678118654752440;
    constexpr double twoPi = 6.28318530717958647693;
    const auto normalization = std::sqrt(
        4.0 / (3.0 * static_cast<double>(channelCount)));
    const auto angle = twoPi * static_cast<double>(channel)
        / static_cast<double>(channelCount);
    return HorizontalAmbisonicDecoding{
        inverseSqrtTwo * normalization,
        std::cos(angle) * normalization,
        std::sin(angle) * normalization,
    };
}
}
