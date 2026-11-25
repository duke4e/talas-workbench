#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace talas::runtime::checked_key_mapped_multisample
{
struct State
{
    double playbackPosition = 0.0;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool gateWasPositive = false;
    std::size_t playbackFramesElapsed = 0;
    // The sample of the bank the note plays.
    std::size_t sample = 0;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double frequency = 440.0;
    double loFrequency = 0.0;
    double hiFrequency = 20'000.0;
    double gate = 1.0;
    double scale = 1.0;
    double velocity = 1.0;
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    bool byPitchRange = false;
    bool loop = false;
    bool noTransposition = false;
    // The Frequency that reads one recorded frame per rendered frame.
    double unityFrequency = 440.0;
    // The loop, as proportions of the last frame: the one in the file's
    // header, else the whole file.
    double loopStart = 0.0;
    double loopEnd = 1.0;
    // The step that plays the file at its own rate.
    double originalRateStep = 1.0;
};

// One sample of a bank, with what a Config gives for a single one: the
// Frequency that reads one of its frames per rendered frame, its loop and
// the step that plays it at its own rate.
struct Sample
{
    std::span<const double> table;
    double unityFrequency = 440.0;
    double loopStart = 0.0;
    double loopEnd = 1.0;
    double originalRateStep = 1.0;
};

// How a bank's samples share the keyboard.
enum class Mapping : std::uint8_t
{
    OnePerHalfStep,
    EquallySpaced,
    ByBasePitch,
    ByPitchRange,
    // OnePerHalfStep counted from MIDI key 1: the first sample on key 1,
    // the next on key 2, whatever key LoFreq gives.
    OnePerHalfStepFromKeyOne,
};

// Where a sample's header places it: its base pitch as a note number, and
// the notes and velocities of its INST or inst chunk.
struct SampleKeys
{
    double basePitch = 60.0;
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t lowVelocity = 1;
    std::uint8_t highVelocity = 127;
};

enum class Error
{
    None,
    ConfigInvalid,
    FrequencyInvalid,
    RangeInvalid,
    GateNonFinite,
    ScaleNonFinite,
    VelocityInvalid,
    AttackInvalid,
    ReleaseInvalid,
    TableInvalid,
    PlaybackStepNonFinite,
    StateInvalid,
    LoopArithmeticNonFinite,
    GainNonFinite,
    EnvelopeArithmeticNonFinite,
    InterpolationNonFinite,
    OutputNonFinite,
    PositionNonFinite,
    ElapsedFramesOverflow,
};

inline constexpr std::size_t maximumTableSamples = 1'048'576;

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double position,
    double& output) noexcept
{
    if (table.empty()
        || table.size() > maximumTableSamples
        || !std::isfinite(position))
    {
        return Error::TableInvalid;
    }
    const auto lastIndex = static_cast<double>(table.size() - 1);
    const auto clamped = std::clamp(position, 0.0, lastIndex);
    const auto leftIndex = static_cast<std::size_t>(
        std::floor(clamped));
    const auto rightIndex = std::min(
        leftIndex + 1, table.size() - 1);
    const auto amount = clamped - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    if (!std::isfinite(left)
        || !std::isfinite(right)
        || !std::isfinite(amount))
    {
        return Error::InterpolationNonFinite;
    }
    if (amount == 0.0 || leftIndex == rightIndex)
    {
        output = left;
        return Error::None;
    }
    if (amount == 1.0)
    {
        output = right;
        return Error::None;
    }
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

[[nodiscard]] inline double noteNumber(double frequency) noexcept
{
    return 69.0 + 12.0 * std::log2(frequency / 440.0);
}

// The sample of a bank, in the order of its file names, that a note of
// `controls.frequency` and `controls.velocity` (0 to 1) plays, or
// nothing where no sample maps: below LoFreq,
// above HiFreq, or outside every sample's pitch range. A Frequency of 0,
// which plays a recording at its own rate, takes the first.
[[nodiscard]] inline std::optional<std::size_t> selectSample(
    Mapping mapping,
    std::span<const SampleKeys> samples,
    const Controls& controls) noexcept
{
    if (samples.empty()
        || controls.frequency < controls.loFrequency
        || controls.frequency > controls.hiFrequency)
    {
        return std::nullopt;
    }
    if (!(controls.frequency > 0.0))
        return std::size_t{0};
    const auto note = noteNumber(controls.frequency);
    const auto key = std::lround(note);
    const auto count = static_cast<long>(samples.size());
    // The keys LoFreq and HiFreq bound, within MIDI's.
    const auto firstKey = controls.loFrequency > 0.0
        ? std::clamp(std::lround(noteNumber(controls.loFrequency)), 0L, 127L)
        : 0L;
    const auto lastKey = std::clamp(
        std::lround(noteNumber(controls.hiFrequency)), firstKey, 127L);
    switch (mapping)
    {
    case Mapping::OnePerHalfStep:
    {
        // Each sample in turn takes the next half step, starting over after
        // the last.
        const auto step = (key - firstKey) % count;
        return static_cast<std::size_t>(step < 0 ? step + count : step);
    }
    case Mapping::OnePerHalfStepFromKeyOne:
    {
        // Key 1 the first sample, key 2 the next, starting over after the
        // last; key 0 takes the last.
        const auto step = (key - 1L) % count;
        return static_cast<std::size_t>(step < 0 ? step + count : step);
    }
    case Mapping::EquallySpaced:
    {
        // Each sample takes an equal block of keys, the first the lowest.
        const auto block = std::floor(static_cast<double>(key - firstKey)
            * static_cast<double>(count)
            / static_cast<double>(lastKey - firstKey + 1));
        return static_cast<std::size_t>(
            std::clamp(block, 0.0, static_cast<double>(count - 1)));
    }
    case Mapping::ByBasePitch:
    {
        // The sample recorded nearest the note; of several recorded at that
        // pitch, the first whose velocities hold Velocity.
        std::size_t nearest = 0;
        for (std::size_t index = 1; index < samples.size(); ++index)
        {
            if (std::abs(samples[index].basePitch - note)
                < std::abs(samples[nearest].basePitch - note))
            {
                nearest = index;
            }
        }
        const auto velocity = std::clamp(
            std::lround(controls.velocity * 127.0), 0L, 127L);
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            if (samples[index].basePitch == samples[nearest].basePitch
                && samples[index].lowVelocity <= velocity
                && velocity <= samples[index].highVelocity)
            {
                return index;
            }
        }
        return nearest;
    }
    case Mapping::ByPitchRange:
        // The sample whose range holds the note, a later name taking
        // precedence where ranges overlap.
        for (auto index = samples.size(); index-- > 0;)
        {
            if (samples[index].lowNote <= key && key <= samples[index].highNote)
                return index;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Plays a bank of samples: when a note starts, `select(controls)` picks the
// sample it plays, and a note no sample takes stays silent until the next.
template <typename ControlsAt, typename SelectSample>
[[nodiscard]] Error renderBank(
    const State& initial,
    const Config& config,
    std::span<const Sample> samples,
    std::span<double> output,
    ControlsAt&& controlsAt,
    SelectSample&& select,
    State& next) noexcept
{
    std::fill(output.begin(), output.end(), 0.0);
    const auto reject = [&] (Error error) noexcept {
        std::fill(output.begin(), output.end(), 0.0);
        return error;
    };

    if (!std::isfinite(config.sampleRate)
        || config.sampleRate <= 0.0
        || samples.empty())
    {
        return reject(Error::ConfigInvalid);
    }
    for (const auto& sample : samples)
    {
        if (!std::isfinite(sample.unityFrequency)
            || sample.unityFrequency <= 0.0)
        {
            return reject(Error::ConfigInvalid);
        }
        if (sample.table.empty() || sample.table.size() > maximumTableSamples)
            return reject(Error::TableInvalid);
    }
    if (!std::isfinite(initial.playbackPosition)
        || initial.playbackPosition < 0.0
        || initial.sample >= samples.size())
    {
        return reject(Error::StateInvalid);
    }

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        if (!std::isfinite(controls.frequency)
            || controls.frequency < 0.0)
        {
            return reject(Error::FrequencyInvalid);
        }
        if (!std::isfinite(controls.loFrequency)
            || controls.loFrequency < 0.0
            || !std::isfinite(controls.hiFrequency)
            || controls.hiFrequency < controls.loFrequency)
        {
            return reject(Error::RangeInvalid);
        }
        if (!std::isfinite(controls.gate))
            return reject(Error::GateNonFinite);
        if (!std::isfinite(controls.scale))
            return reject(Error::ScaleNonFinite);
        if (!std::isfinite(controls.velocity)
            || controls.velocity < 0.0)
        {
            return reject(Error::VelocityInvalid);
        }
        if (!std::isfinite(controls.attackSeconds)
            || controls.attackSeconds < 0.0
            || !std::isfinite(
                controls.attackSeconds * config.sampleRate))
        {
            return reject(Error::AttackInvalid);
        }
        if (!std::isfinite(controls.releaseSeconds)
            || controls.releaseSeconds < 0.0
            || !std::isfinite(
                controls.releaseSeconds * config.sampleRate))
        {
            return reject(Error::ReleaseInvalid);
        }
        for (const auto& sample : samples)
        {
            const auto playbackStep =
                config.noTransposition || controls.frequency == 0.0
                ? sample.originalRateStep
                : controls.frequency / sample.unityFrequency;
            if (!std::isfinite(playbackStep))
                return reject(Error::PlaybackStepNonFinite);
        }
        const auto gain = controls.velocity * controls.scale;
        if (!std::isfinite(gain))
            return reject(Error::GainNonFinite);
    }

    if (output.empty())
    {
        next = initial;
        return Error::None;
    }

    for (const auto& sample : samples)
    {
        const auto lastIndex = static_cast<double>(sample.table.size() - 1);
        const auto loopSpan = (sample.loopEnd - sample.loopStart) * lastIndex + 1.0;
        if (!std::isfinite(sample.loopStart) || !std::isfinite(sample.loopEnd)
            || sample.loopStart < 0.0 || sample.loopEnd > 1.0
            || sample.loopEnd < sample.loopStart
            || !std::isfinite(loopSpan)
            || loopSpan <= 0.0)
        {
            return reject(Error::LoopArithmeticNonFinite);
        }
    }
    auto candidate = initial;
    const auto wrapPosition = [] (
        double position,
        double loopStartIndex,
        double loopSpan,
        double& wrapped) noexcept {
        auto remainder = std::fmod(position - loopStartIndex, loopSpan);
        if (!std::isfinite(remainder))
            return false;
        if (remainder < 0.0)
            remainder += loopSpan;
        wrapped = loopStartIndex + remainder;
        return std::isfinite(wrapped);
    };

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        if (config.byPitchRange
            && (controls.frequency < controls.loFrequency
                || controls.frequency > controls.hiFrequency))
        {
            candidate = {};
            continue;
        }

        const auto gatePositive = controls.gate > 0.0;
        if (gatePositive)
        {
            if (!candidate.gateWasPositive
                || !candidate.playbackStarted)
            {
                // A note starts: the bank's mapping picks its sample.
                const auto chosen = select(controls);
                candidate.playbackPosition = 0.0;
                candidate.playbackActive = chosen.has_value() && *chosen < samples.size();
                candidate.playbackStarted = true;
                candidate.playbackFramesElapsed = 0;
                candidate.sample = candidate.playbackActive ? *chosen : 0;
            }
            candidate.gateWasPositive = true;
        }
        else
        {
            candidate.gateWasPositive = false;
            if (!candidate.playbackStarted
                || !candidate.playbackActive)
            {
                candidate.playbackActive = false;
                continue;
            }
        }

        const auto& sample = samples[candidate.sample];
        const auto lastIndex = static_cast<double>(sample.table.size() - 1);
        const auto totalPlaybackFrames = static_cast<double>(sample.table.size());
        const auto loopStartIndex = sample.loopStart * lastIndex;
        const auto loopEndIndex = sample.loopEnd * lastIndex;
        const auto loopSpan = loopEndIndex - loopStartIndex + 1.0;
        const auto loopActive = config.loop && gatePositive;
        if (!candidate.playbackActive
            || (!loopActive
                && candidate.playbackPosition > lastIndex))
        {
            candidate.playbackActive = false;
            continue;
        }
        if (loopActive
            && candidate.playbackPosition > loopEndIndex
            && !wrapPosition(
                candidate.playbackPosition,
                loopStartIndex,
                loopSpan,
                candidate.playbackPosition))
        {
            return reject(Error::LoopArithmeticNonFinite);
        }

        const auto gain = controls.velocity * controls.scale;
        const auto attackFrames =
            controls.attackSeconds * config.sampleRate;
        const auto releaseFrames =
            controls.releaseSeconds * config.sampleRate;
        auto envelope = 1.0;
        if (attackFrames > 0.0)
        {
            const auto attackRatio = static_cast<double>(
                candidate.playbackFramesElapsed) / attackFrames;
            if (!std::isfinite(attackRatio))
                return reject(Error::EnvelopeArithmeticNonFinite);
            envelope *= std::min(1.0, attackRatio);
        }
        if (!loopActive && releaseFrames > 0.0)
        {
            const auto elapsed = static_cast<double>(
                candidate.playbackFramesElapsed);
            const auto remainingDifference =
                totalPlaybackFrames - elapsed;
            const auto remainingFrames = std::max(
                0.0, remainingDifference - 1.0);
            const auto releaseRatio =
                remainingFrames / releaseFrames;
            if (!std::isfinite(elapsed)
                || !std::isfinite(remainingDifference)
                || !std::isfinite(remainingFrames)
                || !std::isfinite(releaseRatio))
            {
                return reject(Error::EnvelopeArithmeticNonFinite);
            }
            envelope *= std::min(1.0, releaseRatio);
        }
        if (!std::isfinite(envelope))
            return reject(Error::EnvelopeArithmeticNonFinite);

        double sampled = 0.0;
        if (const auto error = sampleLinear(
                sample.table, candidate.playbackPosition, sampled);
            error != Error::None)
        {
            return reject(error);
        }
        const auto scaled = sampled * gain;
        const auto rendered = scaled * envelope;
        if (!std::isfinite(scaled)
            || !std::isfinite(rendered))
        {
            return reject(Error::OutputNonFinite);
        }
        output[frame] = rendered;

        if (candidate.playbackFramesElapsed
            == std::numeric_limits<std::size_t>::max())
        {
            return reject(Error::ElapsedFramesOverflow);
        }
        const auto playbackStep =
            config.noTransposition || controls.frequency == 0.0
            ? sample.originalRateStep
            : controls.frequency / sample.unityFrequency;
        const auto nextPosition =
            candidate.playbackPosition + playbackStep;
        if (!std::isfinite(nextPosition))
            return reject(Error::PositionNonFinite);
        candidate.playbackPosition = nextPosition;
        ++candidate.playbackFramesElapsed;
        if (loopActive
            && candidate.playbackPosition > loopEndIndex)
        {
            if (!wrapPosition(
                    candidate.playbackPosition,
                    loopStartIndex,
                    loopSpan,
                    candidate.playbackPosition))
            {
                return reject(Error::LoopArithmeticNonFinite);
            }
        }
        else if (candidate.playbackPosition > lastIndex)
        {
            candidate.playbackActive = false;
        }
    }

    next = candidate;
    return Error::None;
}

// One sample, as a Config describes it.
template <typename ControlsAt>
[[nodiscard]] Error render(
    const State& initial,
    const Config& config,
    std::span<const double> table,
    std::span<double> output,
    ControlsAt&& controlsAt,
    State& next) noexcept
{
    const Sample sample{
        table,
        config.unityFrequency,
        config.loopStart,
        config.loopEnd,
        config.originalRateStep,
    };
    return renderBank(
        initial,
        config,
        std::span<const Sample>{&sample, 1},
        output,
        std::forward<ControlsAt>(controlsAt),
        [] (const Controls&) noexcept { return std::optional<std::size_t>{0}; },
        next);
}
}
