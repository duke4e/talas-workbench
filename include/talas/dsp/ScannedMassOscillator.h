#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace talas::dsp
{
struct ScannedMassOscillatorTestAccess;

class ScannedMassOscillator
{
public:
    static constexpr double minimumSampleRate = 8000.0;
    static constexpr double maximumSampleRate = 384000.0;
    static constexpr std::size_t minimumMassCount = 8;
    static constexpr std::size_t maximumMassCount = 64;

    [[nodiscard]] bool prepare(
        double sampleRate,
        double massCount,
        double initialShape) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < minimumSampleRate
            || sampleRate > maximumSampleRate
            || !validMassCount(massCount)
            || !inUnitInterval(initialShape))
        {
            return false;
        }

        sampleRate_ = sampleRate;
        massCount_ = static_cast<std::size_t>(massCount);
        initialShape_ = initialShape;
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        initializePreparedShape(positions_, velocities_);
        scanPhase_ = 0.0;
        modelPhase_ = 0.0;
        previousResetHigh_ = false;
        previousExciteHigh_ = false;
    }

    [[nodiscard]] bool processSample(
        double resetValue,
        double exciteValue,
        double frequency,
        double modelRate,
        double coupling,
        double damping,
        double restoring,
        double excitationPosition,
        double excitationAmount,
        double scanOffset,
        double level,
        double& output) noexcept
    {
        if (!prepared_ || massCount_ < minimumMassCount
            || massCount_ > maximumMassCount
            || !std::isfinite(resetValue) || !std::isfinite(exciteValue)
            || !std::isfinite(frequency) || frequency < 0.0
            || frequency > sampleRate_ * 0.4
            || !std::isfinite(modelRate) || modelRate < 0.0
            || modelRate > 1000.0
            || !std::isfinite(coupling) || coupling < 0.0
            || coupling > 0.45
            || !std::isfinite(damping) || damping < 0.0
            || damping > 0.25
            || !std::isfinite(restoring) || restoring < 0.0
            || restoring > 0.2
            || 4.0 * coupling + restoring > 2.0
            || !inUnitInterval(excitationPosition)
            || !inBipolarInterval(excitationAmount)
            || !inBipolarInterval(scanOffset)
            || !std::isfinite(level) || level < 0.0 || level > 4.0)
        {
            return false;
        }

        const auto candidateResetHigh = resetValue > 0.5;
        const auto candidateExciteHigh = exciteValue > 0.5;
        const auto resetEdge = candidateResetHigh && !previousResetHigh_;
        const auto exciteEdge = candidateExciteHigh && !previousExciteHigh_;
        auto candidateScanPhase = resetEdge ? 0.0 : scanPhase_;
        auto candidateModelPhase = resetEdge ? 0.0 : modelPhase_;
        candidateModelPhase += modelRate / sampleRate_;
        const auto modelTick = candidateModelPhase >= 1.0;
        if (modelTick)
            candidateModelPhase -= 1.0;

        const auto stateUpdate = resetEdge || exciteEdge || modelTick;
        const std::array<double, maximumMassCount>* scanPositions = &positions_;
        const std::array<double, maximumMassCount>* committedVelocities = &velocities_;
        if (stateUpdate)
        {
            if (resetEdge)
            {
                initializePreparedShape(
                    candidatePositions_, candidateVelocities_);
            }
            else
            {
                std::copy_n(
                    positions_.begin(), massCount_,
                    candidatePositions_.begin());
                std::copy_n(
                    velocities_.begin(), massCount_,
                    candidateVelocities_.begin());
            }

            if (exciteEdge)
            {
                auto normalizedPosition = excitationPosition;
                if (normalizedPosition >= 1.0)
                    normalizedPosition = 0.0;
                const auto coordinate = normalizedPosition
                    * static_cast<double>(massCount_);
                const auto left = static_cast<std::size_t>(
                    std::floor(coordinate));
                const auto right = (left + 1) % massCount_;
                const auto fraction = coordinate
                    - static_cast<double>(left);
                candidatePositions_[left] = std::tanh(
                    candidatePositions_[left]
                    + excitationAmount * (1.0 - fraction));
                candidatePositions_[right] = std::tanh(
                    candidatePositions_[right]
                    + excitationAmount * fraction);
            }

            scanPositions = &candidatePositions_;
            committedVelocities = &candidateVelocities_;
            if (modelTick)
            {
                for (std::size_t index = 0; index < massCount_; ++index)
                {
                    const auto left = index == 0
                        ? massCount_ - 1
                        : index - 1;
                    const auto right = (index + 1) % massCount_;
                    const auto position = candidatePositions_[index];
                    const auto force = coupling
                            * (candidatePositions_[left]
                                + candidatePositions_[right]
                                - 2.0 * position)
                        - restoring * position;
                    const auto velocity = std::tanh(
                        (candidateVelocities_[index] + force)
                        * (1.0 - damping));
                    const auto nextPosition = std::tanh(position + velocity);
                    if (!std::isfinite(force) || !std::isfinite(velocity)
                        || !std::isfinite(nextPosition)
                        || nextPosition < -1.0 || nextPosition > 1.0)
                    {
                        return false;
                    }
                    nextPositions_[index] = nextPosition;
                    nextVelocities_[index] = velocity;
                }
                scanPositions = &nextPositions_;
                committedVelocities = &nextVelocities_;
            }

            for (std::size_t index = 0; index < massCount_; ++index)
            {
                if (!std::isfinite((*scanPositions)[index])
                    || (*scanPositions)[index] < -1.0
                    || (*scanPositions)[index] > 1.0
                    || !std::isfinite((*committedVelocities)[index]))
                {
                    return false;
                }
            }
        }

        auto readPhase = candidateScanPhase + scanOffset;
        if (readPhase < 0.0)
            readPhase += 1.0;
        else if (readPhase >= 1.0)
            readPhase -= 1.0;
        const auto coordinate = readPhase
            * static_cast<double>(massCount_);
        auto left = static_cast<std::size_t>(std::floor(coordinate));
        if (left >= massCount_)
            left = 0;
        const auto right = (left + 1) % massCount_;
        const auto fraction = coordinate
            - std::floor(coordinate);
        const auto shapeValue = std::lerp(
            (*scanPositions)[left],
            (*scanPositions)[right],
            fraction);
        const auto candidateOutput = level * shapeValue;
        auto nextScanPhase = candidateScanPhase + frequency / sampleRate_;
        if (nextScanPhase >= 1.0)
            nextScanPhase -= 1.0;
        if (!std::isfinite(candidateModelPhase)
            || candidateModelPhase < 0.0 || candidateModelPhase >= 1.0
            || !std::isfinite(readPhase) || readPhase < 0.0
            || readPhase >= 1.0 || !std::isfinite(shapeValue)
            || shapeValue < -1.0 || shapeValue > 1.0
            || !std::isfinite(candidateOutput)
            || !std::isfinite(nextScanPhase) || nextScanPhase < 0.0
            || nextScanPhase >= 1.0)
        {
            return false;
        }

        if (stateUpdate)
        {
            std::copy_n(
                scanPositions->begin(), massCount_, positions_.begin());
            std::copy_n(
                committedVelocities->begin(), massCount_,
                velocities_.begin());
        }
        scanPhase_ = nextScanPhase;
        modelPhase_ = candidateModelPhase;
        previousResetHigh_ = candidateResetHigh;
        previousExciteHigh_ = candidateExciteHigh;
        output = candidateOutput;
        return true;
    }

    [[nodiscard]] double scanPhase() const noexcept { return scanPhase_; }
    [[nodiscard]] double modelPhase() const noexcept { return modelPhase_; }
    [[nodiscard]] std::size_t massCount() const noexcept { return massCount_; }
    [[nodiscard]] double position(std::size_t index) const noexcept
    {
        return index < massCount_ ? positions_[index] : 0.0;
    }
    [[nodiscard]] double velocity(std::size_t index) const noexcept
    {
        return index < massCount_ ? velocities_[index] : 0.0;
    }

private:
    friend struct ScannedMassOscillatorTestAccess;

    static constexpr double twoPi = 6.283185307179586476925286766559;

    [[nodiscard]] static bool validMassCount(double value) noexcept
    {
        return std::isfinite(value)
            && value >= static_cast<double>(minimumMassCount)
            && value <= static_cast<double>(maximumMassCount)
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

    void initializePreparedShape(
        std::array<double, maximumMassCount>& positions,
        std::array<double, maximumMassCount>& velocities) const noexcept
    {
        for (std::size_t index = 0; index < massCount_; ++index)
        {
            positions[index] = initialShape_ * std::sin(
                twoPi * static_cast<double>(index)
                / static_cast<double>(massCount_));
            velocities[index] = 0.0;
        }
    }

    double sampleRate_ = 0.0;
    std::size_t massCount_ = 0;
    double initialShape_ = 0.0;
    std::array<double, maximumMassCount> positions_{};
    std::array<double, maximumMassCount> velocities_{};
    std::array<double, maximumMassCount> candidatePositions_{};
    std::array<double, maximumMassCount> candidateVelocities_{};
    std::array<double, maximumMassCount> nextPositions_{};
    std::array<double, maximumMassCount> nextVelocities_{};
    double scanPhase_ = 0.0;
    double modelPhase_ = 0.0;
    bool previousResetHigh_ = false;
    bool previousExciteHigh_ = false;
    bool prepared_ = false;
};
}
