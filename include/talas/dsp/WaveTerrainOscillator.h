#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::dsp
{
struct WaveTerrainOscillatorTestAccess;

class WaveTerrainOscillator
{
public:
    static constexpr double minimumSampleRate = 8000.0;
    static constexpr double maximumSampleRate = 384000.0;
    static constexpr std::size_t minimumDimension = 2;
    static constexpr std::size_t maximumDimension = 64;

    [[nodiscard]] bool prepare(
        double sampleRate,
        double width,
        double height,
        std::span<const double> terrain) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < minimumSampleRate
            || sampleRate > maximumSampleRate || !validDimension(width)
            || !validDimension(height))
        {
            return false;
        }

        const auto preparedWidth = static_cast<std::size_t>(width);
        const auto preparedHeight = static_cast<std::size_t>(height);
        if (terrain.size() != preparedWidth * preparedHeight
            || !std::ranges::all_of(terrain, [](double sample) {
                   return std::isfinite(sample)
                       && sample >= -1.0 && sample <= 1.0;
               }))
        {
            return false;
        }

        sampleRate_ = sampleRate;
        width_ = preparedWidth;
        height_ = preparedHeight;
        terrain_ = terrain;
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        phase_ = 0.0;
        previousResetHigh_ = false;
    }

    [[nodiscard]] bool processSample(
        double resetValue,
        double frequency,
        double xRadius,
        double yRadius,
        double yRatio,
        double rotation,
        double phaseOffset,
        double level,
        double& output) noexcept
    {
        if (!prepared_ || terrain_.size() != width_ * height_
            || !std::isfinite(resetValue) || !std::isfinite(frequency)
            || frequency < 0.0 || frequency > sampleRate_ * 0.4
            || !inUnitInterval(xRadius) || !inUnitInterval(yRadius)
            || !std::isfinite(yRatio) || yRatio < 0.25 || yRatio > 8.0
            || !inBipolarInterval(rotation)
            || !inBipolarInterval(phaseOffset)
            || !std::isfinite(level) || level < 0.0 || level > 4.0)
        {
            return false;
        }

        const auto candidateResetHigh = resetValue > 0.5;
        const auto samplePhase = candidateResetHigh && !previousResetHigh_
            ? 0.0
            : phase_;
        const auto theta = twoPi * samplePhase;
        const auto yTheta = twoPi * (samplePhase * yRatio + phaseOffset);
        const auto x = xRadius * std::cos(theta);
        const auto y = yRadius * std::sin(yTheta);
        const auto rotationRadians = twoPi * rotation;
        const auto cosine = std::cos(rotationRadians);
        const auto sine = std::sin(rotationRadians);
        const auto rotatedX = x * cosine - y * sine;
        const auto rotatedY = x * sine + y * cosine;
        const auto normalizedX = std::clamp(0.5 + 0.5 * rotatedX, 0.0, 1.0);
        const auto normalizedY = std::clamp(0.5 + 0.5 * rotatedY, 0.0, 1.0);
        if (!std::isfinite(theta) || !std::isfinite(yTheta)
            || !std::isfinite(x) || !std::isfinite(y)
            || !std::isfinite(rotationRadians) || !std::isfinite(cosine)
            || !std::isfinite(sine) || !std::isfinite(rotatedX)
            || !std::isfinite(rotatedY) || !std::isfinite(normalizedX)
            || !std::isfinite(normalizedY))
        {
            return false;
        }

        const auto terrainValue = bilinear(normalizedX, normalizedY);
        const auto candidateOutput = level * terrainValue;
        auto nextPhase = samplePhase + frequency / sampleRate_;
        if (nextPhase >= 1.0)
            nextPhase -= 1.0;
        if (!std::isfinite(terrainValue)
            || terrainValue < -1.0 || terrainValue > 1.0
            || !std::isfinite(candidateOutput) || !std::isfinite(nextPhase)
            || nextPhase < 0.0 || nextPhase >= 1.0)
        {
            return false;
        }

        phase_ = nextPhase;
        previousResetHigh_ = candidateResetHigh;
        output = candidateOutput;
        return true;
    }

    [[nodiscard]] double phase() const noexcept
    {
        return phase_;
    }

private:
    friend struct WaveTerrainOscillatorTestAccess;

    static constexpr double twoPi = 6.283185307179586476925286766559;

    [[nodiscard]] static bool validDimension(double value) noexcept
    {
        return std::isfinite(value)
            && value >= static_cast<double>(minimumDimension)
            && value <= static_cast<double>(maximumDimension)
            && std::floor(value) == value;
    }

    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] static bool inBipolarInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= -1.0 && value <= 1.0;
    }

    [[nodiscard]] double bilinear(double x, double y) const noexcept
    {
        const auto column = x * static_cast<double>(width_ - 1);
        const auto row = y * static_cast<double>(height_ - 1);
        const auto left = static_cast<std::size_t>(std::floor(column));
        const auto top = static_cast<std::size_t>(std::floor(row));
        const auto right = std::min(left + 1, width_ - 1);
        const auto bottom = std::min(top + 1, height_ - 1);
        const auto xFraction = column - static_cast<double>(left);
        const auto yFraction = row - static_cast<double>(top);
        const auto topValue = std::lerp(
            terrain_[top * width_ + left],
            terrain_[top * width_ + right],
            xFraction);
        const auto bottomValue = std::lerp(
            terrain_[bottom * width_ + left],
            terrain_[bottom * width_ + right],
            xFraction);
        return std::lerp(topValue, bottomValue, yFraction);
    }

    double sampleRate_ = 0.0;
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::span<const double> terrain_;
    double phase_ = 0.0;
    bool previousResetHigh_ = false;
    bool prepared_ = false;
};
}
