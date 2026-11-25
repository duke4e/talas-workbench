#pragma once

#include "talas/runtime/detail/CloudBankGrainState.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

namespace talas::runtime::checked_cloud_bank
{
struct State
{
    std::uint64_t renderedFrames = 0;
};

[[nodiscard]] inline bool exactAbsoluteFrameAsDouble(
    std::uint64_t frame,
    double& output) noexcept
{
    const auto candidate = static_cast<double>(frame);
    constexpr auto uint64LimitExclusive = 0x1p64;
    if (!std::isfinite(candidate)
        || candidate >= uint64LimitExclusive)
    {
        return false;
    }
    if (static_cast<std::uint64_t>(candidate) != frame)
        return false;
    output = candidate;
    return true;
}

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
    double bandwidth = 0.0;

    bool operator==(const Partial&) const = default;
};

struct Controls
{
    double trigger = 0.0;
    double pan = 0.5;
    double frequencyMod = 0.0;
    double durationMod = 0.0;
    double cascade = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    double grainDurationSeconds = 0.0;
    double cyclesPerGrain = 0.0;
    double spacingDivisor = 0.0;
    double initialDelayFrames = 0.0;
    std::size_t activeGrainLimit = 0;
    std::size_t selectedTrackCount = 0;
    bool nearestInterpolation = false;
};

struct StereoFrame
{
    double left = 0.0;
    double right = 0.0;

    bool operator==(const StereoFrame&) const = default;
};

struct SelectedSpectrumView
{
    std::span<const double> packed{};
    std::size_t firstIndex = 0;
    std::size_t selectedCount = 0;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return selectedCount;
    }

    [[nodiscard]] Partial partial(std::size_t index) const noexcept
    {
        const auto base = 1 + (firstIndex + index) * 3;
        return {
            packed[base],
            packed[base + 1],
            packed[base + 2],
        };
    }

    [[nodiscard]] bool operator==(
        const SelectedSpectrumView& other) const noexcept
    {
        return packed.data() == other.packed.data()
            && packed.size() == other.packed.size()
            && firstIndex == other.firstIndex
            && selectedCount == other.selectedCount;
    }
};

enum class Error
{
    None,
    TableInvalid,
    ControlsInvalid,
    ConfigInvalid,
    AbsoluteFrameInexact,
    DelayArithmeticNonFinite,
    PackedSpectrumInvalid,
    SelectedRangeInvalid,
    SpectrumInvalid,
    FrequencyArithmeticNonFinite,
    DurationArithmeticNonFinite,
    GrainIndexInexact,
    GrainArithmeticNonFinite,
    InterpolationNonFinite,
    MixArithmeticNonFinite,
    PanArithmeticNonFinite,
    OutputNonFinite,
};

// A SpectrumSource may be an amplitude
// (left) and frequency (right) pair, a partial on each sample, the first on
// each multiple of the frame's partials, its frequency a fraction of half
// the sample rate. Writes each block's pairs into a packed spectrum of
// `packed.front()` partials, holding the partials the block does not reach.
[[nodiscard]] inline bool updatePairedSpectrum(
    std::span<double> packed,
    std::span<const double> amplitudes,
    std::span<const double> frequencies,
    std::uint64_t firstFrame,
    double sampleRate) noexcept
{
    if (packed.empty() || amplitudes.size() != frequencies.size() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return false;
    const auto count = static_cast<std::size_t>(packed.front());
    if (count == 0 || packed.size() < 1 + count * 3)
        return false;
    auto partial = static_cast<std::size_t>(firstFrame % count);
    for (std::size_t frame = 0; frame < amplitudes.size(); ++frame)
    {
        const auto frequency = frequencies[frame] * 0.5 * sampleRate;
        if (!std::isfinite(amplitudes[frame]) || !std::isfinite(frequency))
            return false;
        packed[1 + partial * 3] = amplitudes[frame];
        packed[2 + partial * 3] = frequency;
        packed[3 + partial * 3] = 0.0;
        if (++partial == count)
            partial = 0;
    }
    return true;
}

[[nodiscard]] inline Error selectSpectrumRange(
    std::span<const double> packed,
    std::size_t firstIndex,
    std::size_t selectedCount,
    SelectedSpectrumView& output) noexcept
{
    constexpr std::size_t maximumPartials = 4096;
    if (packed.empty()
        || !std::isfinite(packed.front())
        || packed.front() < 0.0
        || packed.front() != std::floor(packed.front())
        || packed.front() > static_cast<double>(maximumPartials))
    {
        return Error::PackedSpectrumInvalid;
    }
    const auto partialCount = static_cast<std::size_t>(packed.front());
    const auto requiredValues = 1 + partialCount * 3;
    if (packed.size() < requiredValues)
        return Error::PackedSpectrumInvalid;
    if (selectedCount == 0
        || firstIndex > partialCount
        || selectedCount > partialCount - firstIndex)
    {
        return Error::SelectedRangeInvalid;
    }
    for (std::size_t index = 0; index < selectedCount; ++index)
    {
        const auto base = 1 + (firstIndex + index) * 3;
        if (!std::isfinite(packed[base])
            || !std::isfinite(packed[base + 1])
            || packed[base + 1] < 0.0
            || !std::isfinite(packed[base + 2]))
        {
            return Error::SpectrumInvalid;
        }
    }
    output = {packed, firstIndex, selectedCount};
    return Error::None;
}

[[nodiscard]] inline Error sampleTable(
    std::span<const double> table,
    double normalizedIndex,
    bool nearest,
    double& output) noexcept
{
    if (table.empty() || !std::isfinite(normalizedIndex))
        return Error::TableInvalid;
    if (table.size() == 1)
    {
        if (!std::isfinite(table.front()))
            return Error::InterpolationNonFinite;
        output = table.front();
        return Error::None;
    }

    const auto clampedIndex = std::clamp(normalizedIndex, 0.0, 1.0);
    const auto lastIndex = static_cast<double>(table.size() - 1);
    const auto scaledIndex = clampedIndex * lastIndex;
    if (!std::isfinite(clampedIndex) || !std::isfinite(lastIndex)
        || !std::isfinite(scaledIndex))
    {
        return Error::InterpolationNonFinite;
    }
    if (nearest)
    {
        const auto roundedIndex = std::round(scaledIndex);
        if (!std::isfinite(roundedIndex) || roundedIndex < 0.0
            || roundedIndex > lastIndex)
        {
            return Error::InterpolationNonFinite;
        }
        const auto sampled = table[static_cast<std::size_t>(roundedIndex)];
        if (!std::isfinite(sampled))
            return Error::InterpolationNonFinite;
        output = sampled;
        return Error::None;
    }

    const auto leftIndex = static_cast<std::size_t>(
        std::floor(scaledIndex));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = scaledIndex - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(amount) || !std::isfinite(left)
        || !std::isfinite(right) || !std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

template <typename PartialAccessor>
[[nodiscard]] inline Error renderFrame(
    std::span<const double> waveform,
    std::span<const double> envelope,
    PartialAccessor&& partialAt,
    Controls controls,
    Config config,
    std::uint64_t absoluteFrameIndex,
    StereoFrame& output) noexcept
{
    if (waveform.empty() || envelope.empty())
        return Error::TableInvalid;
    if (!std::isfinite(controls.trigger)
        || !std::isfinite(controls.pan)
        || controls.pan < 0.0 || controls.pan > 1.0
        || !std::isfinite(controls.frequencyMod)
        || !std::isfinite(controls.durationMod)
        || !std::isfinite(controls.cascade))
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.grainDurationSeconds)
        || config.grainDurationSeconds < 0.0
        || !std::isfinite(config.cyclesPerGrain)
        || config.cyclesPerGrain < 0.0
        || (config.grainDurationSeconds <= 0.0
            && config.cyclesPerGrain <= 0.0)
        || !std::isfinite(config.spacingDivisor)
        || config.spacingDivisor <= 0.0
        || !std::isfinite(config.initialDelayFrames)
        || config.initialDelayFrames < 0.0
        || config.activeGrainLimit == 0
        || config.selectedTrackCount == 0)
    {
        return Error::ConfigInvalid;
    }

    const auto durationMultiplier = 1.0 + controls.durationMod;
    const auto frequencyMultiplier = 1.0 + controls.frequencyMod;
    if (!std::isfinite(durationMultiplier) || durationMultiplier <= 0.0)
        return Error::DurationArithmeticNonFinite;
    if (!std::isfinite(frequencyMultiplier))
        return Error::FrequencyArithmeticNonFinite;

    double absoluteFrame = 0.0;
    if (!exactAbsoluteFrameAsDouble(absoluteFrameIndex, absoluteFrame))
        return Error::AbsoluteFrameInexact;
    const auto delayedFrame = absoluteFrame - config.initialDelayFrames;
    if (!std::isfinite(delayedFrame))
        return Error::DelayArithmeticNonFinite;

    double mixed = 0.0;
    for (std::size_t partialIndex = 0;
         partialIndex < config.selectedTrackCount;
         ++partialIndex)
    {
        const auto partial = partialAt(partialIndex);
        if (!std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequency)
            || partial.frequency < 0.0
            || !std::isfinite(partial.bandwidth))
        {
            return Error::SpectrumInvalid;
        }
        const auto effectiveFrequency = partial.frequency
            * frequencyMultiplier;
        if (!std::isfinite(effectiveFrequency)
            || effectiveFrequency < 0.0)
        {
            return Error::FrequencyArithmeticNonFinite;
        }

        double durationSeconds = 0.0;
        if (config.grainDurationSeconds > 0.0)
        {
            durationSeconds = config.grainDurationSeconds
                * durationMultiplier;
            if (!std::isfinite(durationSeconds)
                || durationSeconds <= 0.0)
            {
                return Error::DurationArithmeticNonFinite;
            }
        }
        else
        {
            if (effectiveFrequency <= 0.0)
                return Error::DurationArithmeticNonFinite;
            const auto modulatedCycles = config.cyclesPerGrain
                * durationMultiplier;
            if (!std::isfinite(modulatedCycles)
                || modulatedCycles <= 0.0)
            {
                return Error::DurationArithmeticNonFinite;
            }
            durationSeconds = modulatedCycles / effectiveFrequency;
            if (!std::isfinite(durationSeconds)
                || durationSeconds <= 0.0)
            {
                return Error::DurationArithmeticNonFinite;
            }
        }

        const auto durationFrames = durationSeconds * config.sampleRate;
        const auto spacingFrames = durationFrames / config.spacingDivisor;
        if (!std::isfinite(durationFrames) || durationFrames <= 0.0
            || !std::isfinite(spacingFrames) || spacingFrames <= 0.0)
        {
            return Error::DurationArithmeticNonFinite;
        }
        if (controls.trigger <= 0.0 || delayedFrame < 0.0)
            continue;

        const auto quotient = delayedFrame / spacingFrames;
        const auto mostRecent = std::floor(quotient);
        if (!std::isfinite(quotient) || !std::isfinite(mostRecent)
            || mostRecent < 0.0 || mostRecent >= 0x1p63)
        {
            return Error::GrainIndexInexact;
        }
        const auto mostRecentIndex = static_cast<std::int64_t>(mostRecent);
        if (static_cast<double>(mostRecentIndex) != mostRecent)
            return Error::GrainIndexInexact;

        for (std::size_t offset = 0;
             offset < config.activeGrainLimit;
             ++offset)
        {
            if (offset > static_cast<std::size_t>(mostRecentIndex))
                break;
            const auto grainIndex = mostRecentIndex
                - static_cast<std::int64_t>(offset);
            const auto grainIndexAsDouble =
                static_cast<double>(grainIndex);
            if (!std::isfinite(grainIndexAsDouble)
                || static_cast<std::int64_t>(grainIndexAsDouble)
                    != grainIndex)
            {
                return Error::GrainIndexInexact;
            }
            const auto startFrame = grainIndexAsDouble * spacingFrames;
            const auto ageFrames = delayedFrame - startFrame;
            if (!std::isfinite(startFrame) || !std::isfinite(ageFrames))
                return Error::GrainArithmeticNonFinite;
            if (ageFrames < 0.0 || ageFrames >= durationFrames)
                continue;

            const auto ageSeconds = ageFrames / config.sampleRate;
            const auto phaseCycles = ageSeconds * effectiveFrequency;
            auto phase = std::fmod(phaseCycles, 1.0);
            const auto envelopePosition = ageFrames / durationFrames;
            if (!std::isfinite(ageSeconds)
                || !std::isfinite(phaseCycles) || !std::isfinite(phase)
                || !std::isfinite(envelopePosition))
            {
                return Error::GrainArithmeticNonFinite;
            }
            if (phase < 0.0)
                phase += 1.0;

            double waveformSample = 0.0;
            auto error = sampleTable(
                waveform,
                phase,
                config.nearestInterpolation,
                waveformSample);
            if (error != Error::None)
                return error;
            double envelopeSample = 0.0;
            error = sampleTable(
                envelope,
                envelopePosition,
                config.nearestInterpolation,
                envelopeSample);
            if (error != Error::None)
                return error;

            const auto shaped = waveformSample * envelopeSample;
            const auto grainSample = shaped * partial.amplitude;
            const auto nextMixed = mixed + grainSample;
            if (!std::isfinite(shaped) || !std::isfinite(grainSample)
                || !std::isfinite(nextMixed))
            {
                return Error::MixArithmeticNonFinite;
            }
            mixed = nextMixed;
        }
    }

    const auto leftGain = controls.pan <= 0.5
        ? 1.0 : 2.0 * (1.0 - controls.pan);
    const auto rightGain = controls.pan >= 0.5
        ? 1.0 : 2.0 * controls.pan;
    const auto leftProduct = mixed * leftGain;
    const auto rightProduct = mixed * rightGain;
    if (!std::isfinite(leftGain) || !std::isfinite(rightGain)
        || !std::isfinite(leftProduct) || !std::isfinite(rightProduct))
    {
        return Error::PanArithmeticNonFinite;
    }
    const auto nextLeft = controls.cascade + leftProduct;
    const auto nextRight = controls.cascade + rightProduct;
    if (!std::isfinite(nextLeft) || !std::isfinite(nextRight))
        return Error::OutputNonFinite;

    output = {nextLeft, nextRight};
    return Error::None;
}

// A CloudBank starts grains on its Trigger; projects before format 59 laid
// grains out regularly by GrainsPerTrack.
[[nodiscard]] inline bool startsGrainsOnTrigger(
    const std::optional<std::string>& grainStarts) noexcept
{
    return !(grainStarts.has_value() && *grainStarts == "legacyRegular");
}

// A Seed is an integer; one too large to round exactly keeps its bits.
[[nodiscard]] inline std::int64_t triggeredSeed(double seed) noexcept
{
    if (std::isfinite(seed) && std::abs(seed) < 0x1p53)
        return static_cast<std::int64_t>(std::llround(seed));
    std::int64_t bits = 0;
    std::memcpy(&bits, &seed, sizeof bits);
    return bits == 0 ? 1 : bits;
}

// The most grains a CloudBank may keep sounding.
inline constexpr std::size_t maximumTriggeredGrains = 4096;

struct TriggeredConfig
{
    double sampleRate = 0.0;
    double grainDurationSeconds = 0.0;
    double cyclesPerGrain = 0.0;
    double initialDelayFrames = 0.0;
    std::size_t selectedTrackCount = 0;
    std::int64_t seed = 0;
    bool nearestInterpolation = false;
};

// The track of the grain that starts `started`-th: in ascending order for
// Seed 0, otherwise drawn from the Seed.
[[nodiscard]] inline std::size_t triggeredTrack(
    std::int64_t seed,
    std::uint64_t started,
    std::size_t trackCount) noexcept
{
    if (seed == 0)
        return static_cast<std::size_t>(started % trackCount);
    const auto mix = [](std::uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    };
    return static_cast<std::size_t>(
        mix(static_cast<std::uint64_t>(seed) ^ mix(started)) % trackCount);
}

// One frame of a CloudBank: while
// Trigger is above 0 and one of its grains is free, a grain starts on the
// next track, at the track's frequency times 1 + FreqMod, lasting GrainDur,
// or CyclesPerGrain cycles when GrainDur is 0, times 1 + GrainDurMod, at
// the Pan of its start; none starts before InitialDelay. Each sounding
// grain follows its track's amplitude. A grain of no length does not start.
template <typename PartialAccessor>
[[nodiscard]] inline Error renderTriggeredFrame(
    std::span<const double> waveform,
    std::span<const double> envelope,
    PartialAccessor&& partialAt,
    Controls controls,
    const TriggeredConfig& config,
    detail::cloud_bank::GrainPool& pool,
    std::uint64_t absoluteFrameIndex,
    StereoFrame& output) noexcept
{
    if (waveform.empty() || envelope.empty())
        return Error::TableInvalid;
    if (!std::isfinite(controls.trigger)
        || !std::isfinite(controls.pan)
        || controls.pan < 0.0 || controls.pan > 1.0
        || !std::isfinite(controls.frequencyMod)
        || !std::isfinite(controls.durationMod)
        || !std::isfinite(controls.cascade))
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.grainDurationSeconds)
        || config.grainDurationSeconds < 0.0
        || !std::isfinite(config.cyclesPerGrain)
        || config.cyclesPerGrain < 0.0
        || (config.grainDurationSeconds <= 0.0
            && config.cyclesPerGrain <= 0.0)
        || !std::isfinite(config.initialDelayFrames)
        || config.initialDelayFrames < 0.0
        || config.selectedTrackCount == 0
        || pool.grains.empty()
        || pool.active > pool.grains.size())
    {
        return Error::ConfigInvalid;
    }
    double absoluteFrame = 0.0;
    if (!exactAbsoluteFrameAsDouble(absoluteFrameIndex, absoluteFrame))
        return Error::AbsoluteFrameInexact;

    double left = 0.0;
    double right = 0.0;
    const auto sound = [&](const detail::cloud_bank::Grain& grain,
                           double ageFrames) noexcept {
        const auto phaseCycles =
            ageFrames / config.sampleRate * grain.frequency;
        auto phase = std::fmod(phaseCycles, 1.0);
        const auto envelopePosition = ageFrames / grain.durationFrames;
        if (!std::isfinite(phaseCycles) || !std::isfinite(phase)
            || !std::isfinite(envelopePosition))
        {
            return Error::GrainArithmeticNonFinite;
        }
        if (phase < 0.0)
            phase += 1.0;
        double waveformSample = 0.0;
        auto error = sampleTable(
            waveform, phase, config.nearestInterpolation, waveformSample);
        if (error != Error::None)
            return error;
        double envelopeSample = 0.0;
        error = sampleTable(
            envelope, envelopePosition, config.nearestInterpolation,
            envelopeSample);
        if (error != Error::None)
            return error;
        const auto amplitude = partialAt(grain.track).amplitude;
        const auto grainSample = waveformSample * envelopeSample * amplitude;
        const auto leftGain = grain.pan <= 0.5 ? 1.0 : 2.0 * (1.0 - grain.pan);
        const auto rightGain = grain.pan >= 0.5 ? 1.0 : 2.0 * grain.pan;
        left += grainSample * leftGain;
        right += grainSample * rightGain;
        if (!std::isfinite(left) || !std::isfinite(right))
            return Error::MixArithmeticNonFinite;
        return Error::None;
    };

    for (std::size_t index = 0; index < pool.active;)
    {
        auto& grain = pool.grains[index];
        if (grain.startFrame > absoluteFrameIndex
            || grain.track >= config.selectedTrackCount)
        {
            return Error::GrainArithmeticNonFinite;
        }
        const auto ageFrames =
            static_cast<double>(absoluteFrameIndex - grain.startFrame);
        if (ageFrames >= grain.durationFrames)
        {
            // An ended grain frees its place for the last sounding one.
            grain = pool.grains[pool.active - 1];
            --pool.active;
            continue;
        }
        if (const auto error = sound(grain, ageFrames); error != Error::None)
            return error;
        ++index;
    }

    // A grain may start on the frame another ends.
    if (controls.trigger > 0.0
        && absoluteFrame >= config.initialDelayFrames
        && pool.active < pool.grains.size())
    {
        const auto track = triggeredTrack(
            config.seed, pool.started, config.selectedTrackCount);
        const auto partial = partialAt(track);
        if (!std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequency)
            || partial.frequency < 0.0)
        {
            return Error::SpectrumInvalid;
        }
        const auto frequency = partial.frequency
            * (1.0 + controls.frequencyMod);
        const auto durationMultiplier = 1.0 + controls.durationMod;
        if (!std::isfinite(frequency))
            return Error::FrequencyArithmeticNonFinite;
        double durationSeconds = 0.0;
        if (config.grainDurationSeconds > 0.0)
            durationSeconds = config.grainDurationSeconds * durationMultiplier;
        else if (frequency != 0.0)
            durationSeconds = config.cyclesPerGrain * durationMultiplier
                / std::abs(frequency);
        const auto durationFrames = durationSeconds * config.sampleRate;
        if (!std::isfinite(durationFrames))
            return Error::DurationArithmeticNonFinite;
        if (durationFrames > 0.0)
        {
            const detail::cloud_bank::Grain grain{
                track,
                absoluteFrameIndex,
                frequency,
                durationFrames,
                controls.pan,
            };
            if (const auto error = sound(grain, 0.0); error != Error::None)
                return error;
            pool.grains[pool.active] = grain;
            ++pool.active;
            ++pool.started;
        }
    }

    const auto nextLeft = controls.cascade + left;
    const auto nextRight = controls.cascade + right;
    if (!std::isfinite(nextLeft) || !std::isfinite(nextRight))
        return Error::OutputNonFinite;
    output = {nextLeft, nextRight};
    return Error::None;
}
}
