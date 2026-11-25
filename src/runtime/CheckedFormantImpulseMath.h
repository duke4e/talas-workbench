#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>

namespace talas::runtime::checked_formant_impulse
{
// Candidate interpretation of the documented impulse-response model:
// Frequency sets pulse repetition, while each Spectrum frequency is an
// independent absolute carrier center. Each pulse starts one table-shaped,
// cosine-carried response per formant. Bandwidth advances its normalized
// table position; zero bandwidth uses the caller's explicit fallback rate.
// The source documents do not specify these discrete-time mappings.
inline constexpr double twoPi =
    6.283185307179586476925286766559;
inline constexpr std::size_t maximumImpulseCells = 65536;
inline constexpr std::size_t maximumImpulseCount = 4096;

// Candidate fallback only.
inline constexpr std::array<double, 129> linearTriangularImpulseCandidate = [] {
    std::array<double, 129> table{};
    for (std::size_t index = 0; index <= 64; ++index)
    {
        const auto value = static_cast<double>(index) / 64.0;
        table[index] = value;
        table[128 - index] = value;
    }
    return table;
}();

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
    double bandwidth = 0.0;
};

struct Frame
{
    double sample = 0.0;
    double pulsePhase = 0.0;
    std::size_t nextImpulse = 0;
};

namespace detail
{
[[nodiscard]] inline bool spansOverlap(
    std::span<const double> first,
    std::span<const double> second) noexcept
{
    if (first.empty() || second.empty())
        return false;

    const std::less<const double*> before;
    return before(first.data(), second.data() + second.size())
        && before(second.data(), first.data() + first.size());
}

[[nodiscard]] inline bool interpolateImpulse(
    std::span<const double> table,
    double windowPosition,
    double& value) noexcept
{
    if (table.empty() || !std::isfinite(windowPosition)
        || windowPosition < 0.0 || windowPosition >= 1.0)
    {
        return false;
    }

    const auto tableLength = static_cast<double>(table.size());
    const auto tablePosition = windowPosition * tableLength;
    if (!std::isfinite(tablePosition) || tablePosition < 0.0)
        return false;

    // Rounding near the endpoint can produce exactly tableLength. The table
    // is zero-padded after its final sample.
    if (tablePosition >= tableLength)
    {
        value = 0.0;
        return true;
    }

    const auto lowerIndex = static_cast<std::size_t>(std::floor(tablePosition));
    if (lowerIndex >= table.size())
    {
        value = 0.0;
        return true;
    }

    const auto fraction = tablePosition - static_cast<double>(lowerIndex);
    const auto lowerSample = table[lowerIndex];
    const auto upperSample = lowerIndex + 1 < table.size()
        ? table[lowerIndex + 1]
        : 0.0;
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0
        || !std::isfinite(lowerSample) || !std::isfinite(upperSample))
    {
        return false;
    }

    const auto lowerProduct = lowerSample * (1.0 - fraction);
    const auto upperProduct = upperSample * fraction;
    if (!std::isfinite(lowerProduct) || !std::isfinite(upperProduct))
        return false;

    value = lowerProduct + upperProduct;
    return std::isfinite(value);
}
}

template <typename PartialAt>
[[nodiscard]] inline bool processFrame(
    double sampleRate,
    double fundamental,
    double initialSample,
    bool allowDc,
    std::size_t formantCount,
    std::size_t impulseCount,
    std::span<const double> table,
    double zeroBandwidthWindowRateHz,
    double pulsePhase,
    std::size_t nextImpulse,
    std::span<const double> carrierPhases,
    std::span<const double> windowPositions,
    std::span<double> candidateCarrierPhases,
    std::span<double> candidateWindowPositions,
    PartialAt&& partialAt,
    Frame& result) noexcept
{
    // State arrays use formant-major, impulse-minor layout. Candidate arrays
    // can be dirty after failure; current state and result remain untouched.
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(fundamental) || fundamental <= 0.0
        || !std::isfinite(initialSample)
        || !std::isfinite(zeroBandwidthWindowRateHz)
        || zeroBandwidthWindowRateHz <= 0.0
        || !std::isfinite(pulsePhase) || pulsePhase < 0.0
        || pulsePhase >= 2.0
        || formantCount == 0 || impulseCount == 0
        || impulseCount > maximumImpulseCount
        || formantCount > maximumImpulseCells / impulseCount
        || nextImpulse >= impulseCount
        || table.empty())
    {
        return false;
    }

    const auto cellCount = formantCount * impulseCount;
    if (carrierPhases.size() != cellCount
        || windowPositions.size() != cellCount
        || candidateCarrierPhases.size() != cellCount
        || candidateWindowPositions.size() != cellCount)
    {
        return false;
    }

    const auto candidateCarrierView =
        std::span<const double>{candidateCarrierPhases};
    const auto candidateWindowView =
        std::span<const double>{candidateWindowPositions};
    if (detail::spansOverlap(carrierPhases, windowPositions)
        || detail::spansOverlap(carrierPhases, candidateCarrierView)
        || detail::spansOverlap(carrierPhases, candidateWindowView)
        || detail::spansOverlap(windowPositions, candidateCarrierView)
        || detail::spansOverlap(windowPositions, candidateWindowView)
        || detail::spansOverlap(candidateCarrierView, candidateWindowView)
        || detail::spansOverlap(table, candidateCarrierView)
        || detail::spansOverlap(table, candidateWindowView))
    {
        return false;
    }

    const auto nyquist = sampleRate * 0.5;
    if (!std::isfinite(nyquist) || fundamental >= nyquist)
        return false;

    const auto pulseIncrement = fundamental / sampleRate;
    if (!std::isfinite(pulseIncrement) || pulseIncrement <= 0.0
        || pulseIncrement >= 0.5)
    {
        return false;
    }

    for (std::size_t cell = 0; cell < cellCount; ++cell)
    {
        const auto phase = carrierPhases[cell];
        const auto position = windowPositions[cell];
        if (!std::isfinite(phase) || phase < 0.0 || phase >= twoPi
            || !std::isfinite(position) || position < 0.0 || position > 1.0)
        {
            return false;
        }
    }

    std::copy(carrierPhases.begin(), carrierPhases.end(),
        candidateCarrierPhases.begin());
    std::copy(windowPositions.begin(), windowPositions.end(),
        candidateWindowPositions.begin());

    // A phase of one is the caller's initial launch sentinel. Later pulses
    // replace the next ring slot, retiring its previous response if active.
    auto candidatePulsePhase = pulsePhase;
    auto candidateNextImpulse = nextImpulse;
    if (candidatePulsePhase >= 1.0)
    {
        candidatePulsePhase -= 1.0;
        const auto slot = candidateNextImpulse;
        for (std::size_t formant = 0; formant < formantCount; ++formant)
        {
            const auto cell = formant * impulseCount + slot;
            candidateCarrierPhases[cell] = 0.0;
            candidateWindowPositions[cell] = 0.0;
        }
        candidateNextImpulse = slot + 1 == impulseCount ? 0 : slot + 1;
    }

    auto sample = initialSample;
    for (std::size_t formant = 0; formant < formantCount; ++formant)
    {
        // Live partial values also control already-active tails; capture-at-
        // launch semantics are not specified by the reference entries.
        const auto partial = partialAt(formant);
        if (!std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequency)
            || !std::isfinite(partial.bandwidth)
            || partial.frequency < 0.0 || partial.frequency >= nyquist
            || partial.bandwidth < 0.0)
        {
            return false;
        }

        const auto normalizedFrequency = partial.frequency / sampleRate;
        const auto carrierIncrement = twoPi * normalizedFrequency;
        const auto windowRate = partial.bandwidth > 0.0
            ? partial.bandwidth
            : zeroBandwidthWindowRateHz;
        const auto windowIncrement = windowRate / sampleRate;
        if (!std::isfinite(normalizedFrequency)
            || !std::isfinite(carrierIncrement)
            || !std::isfinite(windowIncrement)
            || windowIncrement <= 0.0)
        {
            return false;
        }

        const auto contributes = partial.frequency != 0.0 || allowDc;
        for (std::size_t slot = 0; slot < impulseCount; ++slot)
        {
            const auto cell = formant * impulseCount + slot;
            const auto position = candidateWindowPositions[cell];
            if (position >= 1.0)
                continue;

            if (contributes)
            {
                double impulseValue = 0.0;
                if (!detail::interpolateImpulse(table, position, impulseValue))
                    return false;

                const auto carrier = std::cos(candidateCarrierPhases[cell]);
                if (!std::isfinite(carrier))
                    return false;
                const auto shapedCarrier = carrier * impulseValue;
                if (!std::isfinite(shapedCarrier))
                    return false;
                const auto contribution = partial.amplitude * shapedCarrier;
                if (!std::isfinite(contribution))
                    return false;
                sample += contribution;
                if (!std::isfinite(sample))
                    return false;
            }

            auto advancedCarrier =
                candidateCarrierPhases[cell] + carrierIncrement;
            if (!std::isfinite(advancedCarrier))
                return false;
            if (advancedCarrier >= twoPi)
                advancedCarrier -= twoPi;
            candidateCarrierPhases[cell] = advancedCarrier;

            const auto advancedPosition = position + windowIncrement;
            if (!std::isfinite(advancedPosition))
                return false;
            candidateWindowPositions[cell] = advancedPosition >= 1.0
                ? 1.0
                : advancedPosition;
        }
    }

    candidatePulsePhase += pulseIncrement;
    if (!std::isfinite(candidatePulsePhase)
        || candidatePulsePhase < 0.0 || candidatePulsePhase >= 2.0)
    {
        return false;
    }

    result = {sample, candidatePulsePhase, candidateNextImpulse};
    return true;
}
}
