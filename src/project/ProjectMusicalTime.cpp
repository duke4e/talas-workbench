#include "talas/project/ProjectMusicalTime.h"

#include <cmath>
#include <limits>

namespace talas::project
{
namespace
{
enum class ValidationCode
{
    Valid,
    TicksPerQuarter,
    TempoCount,
    MeterCount,
    TempoOrigin,
    MeterOrigin,
    TempoRange,
    TempoOrder,
    MeterRange,
    MeterOrder,
    MeterBoundary
};

[[nodiscard]] bool isPowerOfTwo(std::uint8_t value) noexcept
{
    return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] std::optional<std::uint64_t> checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept
{
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
        return std::nullopt;
    return lhs + rhs;
}

[[nodiscard]] std::optional<std::uint64_t> checkedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs) noexcept
{
    if (lhs != 0
        && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    {
        return std::nullopt;
    }
    return lhs * rhs;
}

[[nodiscard]] std::optional<std::uint64_t> ticksPerBar(
    const ProjectMusicalTime& musicalTime,
    const MusicalMeterEvent& meter) noexcept
{
    const auto quarterTicks = checkedMultiply(
        musicalTime.ticksPerQuarter,
        4);
    if (!quarterTicks.has_value()
        || *quarterTicks % meter.denominator != 0)
    {
        return std::nullopt;
    }
    return checkedMultiply(
        *quarterTicks / meter.denominator,
        meter.numerator);
}

[[nodiscard]] ValidationCode validationCode(
    const ProjectMusicalTime& musicalTime) noexcept
{
    if (musicalTime.ticksPerQuarter != projectTicksPerQuarter)
        return ValidationCode::TicksPerQuarter;
    if (musicalTime.tempo.empty()
        || musicalTime.tempo.size() > maximumMusicalMapEvents)
        return ValidationCode::TempoCount;
    if (musicalTime.meter.empty()
        || musicalTime.meter.size() > maximumMusicalMapEvents)
        return ValidationCode::MeterCount;
    if (musicalTime.tempo.front().tick != 0)
        return ValidationCode::TempoOrigin;
    if (musicalTime.meter.front().tick != 0)
        return ValidationCode::MeterOrigin;
    for (std::size_t index = 0; index < musicalTime.tempo.size(); ++index)
    {
        const auto& event = musicalTime.tempo[index];
        if (event.microsecondsPerQuarter < minimumMicrosecondsPerQuarter
            || event.microsecondsPerQuarter > maximumMicrosecondsPerQuarter)
            return ValidationCode::TempoRange;
        if (index != 0 && event.tick <= musicalTime.tempo[index - 1].tick)
            return ValidationCode::TempoOrder;
    }
    for (std::size_t index = 0; index < musicalTime.meter.size(); ++index)
    {
        const auto& event = musicalTime.meter[index];
        if (event.numerator == 0 || event.numerator > 64
            || event.denominator > 64 || !isPowerOfTwo(event.denominator))
            return ValidationCode::MeterRange;
        if (index == 0)
            continue;
        const auto& previous = musicalTime.meter[index - 1];
        if (event.tick <= previous.tick)
            return ValidationCode::MeterOrder;
        const auto previousBarTicks = ticksPerBar(musicalTime, previous);
        if (!previousBarTicks.has_value()
            || (event.tick - previous.tick) % *previousBarTicks != 0)
            return ValidationCode::MeterBoundary;
    }
    return ValidationCode::Valid;
}
}

std::optional<std::string>
projectMusicalTimeValidationError(const ProjectMusicalTime& musicalTime)
{
    switch (validationCode(musicalTime))
    {
    case ValidationCode::Valid:
        return std::nullopt;
    case ValidationCode::TicksPerQuarter:
        return "Musical time requires exactly 960 ticks per quarter note";
    case ValidationCode::TempoCount:
        return "Tempo map requires between 1 and 1024 events";
    case ValidationCode::MeterCount:
        return "Meter map requires between 1 and 1024 events";
    case ValidationCode::TempoOrigin:
        return "Tempo map requires an event at tick zero";
    case ValidationCode::MeterOrigin:
        return "Meter map requires an event at tick zero";
    case ValidationCode::TempoRange:
        return "Tempo must be between 1 and 60000 BPM";
    case ValidationCode::TempoOrder:
        return "Tempo events must use unique increasing ticks";
    case ValidationCode::MeterRange:
        return "Meter requires a 1..64 numerator and power-of-two denominator up to 64";
    case ValidationCode::MeterOrder:
        return "Meter events must use unique increasing ticks";
    case ValidationCode::MeterBoundary:
        return "Meter changes must begin on a bar boundary";
    }
    return std::nullopt;
}

bool isValidProjectMusicalTime(
    const ProjectMusicalTime& musicalTime) noexcept
{
    return validationCode(musicalTime) == ValidationCode::Valid;
}

std::optional<std::uint64_t> musicalTickToSampleFrame(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t tick,
    std::uint32_t sampleRate) noexcept
{
    if (sampleRate == 0 || !isValidProjectMusicalTime(musicalTime))
    {
        return std::nullopt;
    }

    std::uint64_t accumulatedUnits = 0;
    for (std::size_t index = 0; index < musicalTime.tempo.size(); ++index)
    {
        const auto& event = musicalTime.tempo[index];
        if (event.tick >= tick)
            break;
        const auto segmentEnd = index + 1 < musicalTime.tempo.size()
            ? musicalTime.tempo[index + 1].tick
            : tick;
        const auto boundedEnd = segmentEnd < tick ? segmentEnd : tick;
        const auto segmentTicks = boundedEnd - event.tick;
        const auto segmentUnits = checkedMultiply(
            segmentTicks,
            event.microsecondsPerQuarter);
        if (!segmentUnits.has_value())
            return std::nullopt;
        const auto total = checkedAdd(accumulatedUnits, *segmentUnits);
        if (!total.has_value())
            return std::nullopt;
        accumulatedUnits = *total;
        if (boundedEnd == tick)
            break;
    }

    constexpr std::uint64_t microsecondsPerSecond = 1000000;
    const auto denominator = checkedMultiply(
        musicalTime.ticksPerQuarter,
        microsecondsPerSecond);
    if (!denominator.has_value())
        return std::nullopt;
    const auto wholeSeconds = accumulatedUnits / *denominator;
    const auto remainder = accumulatedUnits % *denominator;
    const auto wholeFrames = checkedMultiply(wholeSeconds, sampleRate);
    const auto fractionalNumerator = checkedMultiply(remainder, sampleRate);
    if (!wholeFrames.has_value() || !fractionalNumerator.has_value())
        return std::nullopt;
    const auto roundedFraction = (*fractionalNumerator + *denominator / 2)
        / *denominator;
    return checkedAdd(*wholeFrames, roundedFraction);
}

std::optional<std::uint64_t> midiSongPositionToMusicalTick(
    std::uint16_t songPosition) noexcept
{
    if (songPosition > 0x3fffU)
        return std::nullopt;
    constexpr std::uint64_t ticksPerSongPosition =
        projectTicksPerQuarter / 4;
    return checkedMultiply(songPosition, ticksPerSongPosition);
}

std::optional<std::uint64_t> sampleFrameToMusicalTick(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t sampleFrame,
    std::uint32_t sampleRate) noexcept
{
    if (sampleRate == 0 || !isValidProjectMusicalTime(musicalTime))
        return std::nullopt;
    std::uint64_t low = 0;
    std::uint64_t high = 1;
    for (std::size_t attempt = 0; attempt < 64; ++attempt)
    {
        const auto frame = musicalTickToSampleFrame(
            musicalTime, high, sampleRate);
        if (!frame.has_value() || *frame > sampleFrame)
            break;
        low = high;
        if (high > std::numeric_limits<std::uint64_t>::max() / 2)
        {
            high = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        high *= 2;
    }
    for (std::size_t attempt = 0; attempt < 64 && low < high; ++attempt)
    {
        const auto distance = high - low;
        const auto middle = low + distance / 2 + distance % 2;
        const auto frame = musicalTickToSampleFrame(
            musicalTime, middle, sampleRate);
        if (frame.has_value() && *frame <= sampleFrame)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}

std::optional<std::uint64_t> midiSongPositionToSampleFrame(
    const ProjectMusicalTime& musicalTime,
    std::uint16_t songPosition,
    std::uint32_t sampleRate) noexcept
{
    const auto tick = midiSongPositionToMusicalTick(songPosition);
    if (!tick.has_value())
        return std::nullopt;
    return musicalTickToSampleFrame(musicalTime, *tick, sampleRate);
}

std::optional<MusicalPosition> musicalTickToPosition(
    const ProjectMusicalTime& musicalTime,
    std::uint64_t tick) noexcept
{
    if (!isValidProjectMusicalTime(musicalTime))
        return std::nullopt;

    std::uint64_t completedBars = 0;
    for (std::size_t index = 0; index < musicalTime.meter.size(); ++index)
    {
        const auto& event = musicalTime.meter[index];
        const auto segmentEnd = index + 1 < musicalTime.meter.size()
            ? musicalTime.meter[index + 1].tick
            : tick;
        const auto barTicks = ticksPerBar(musicalTime, event);
        const auto beatTicks = checkedMultiply(
            musicalTime.ticksPerQuarter,
            4);
        if (!barTicks.has_value() || !beatTicks.has_value())
            return std::nullopt;
        const auto ticksPerBeat = *beatTicks / event.denominator;
        if (tick >= segmentEnd && index + 1 < musicalTime.meter.size())
        {
            const auto totalBars = checkedAdd(
                completedBars,
                (segmentEnd - event.tick) / *barTicks);
            if (!totalBars.has_value())
                return std::nullopt;
            completedBars = *totalBars;
            continue;
        }
        const auto localTick = tick - event.tick;
        const auto zeroBasedBar = checkedAdd(
            completedBars,
            localTick / *barTicks);
        const auto oneBasedBar = zeroBasedBar.has_value()
            ? checkedAdd(*zeroBasedBar, 1)
            : std::optional<std::uint64_t>{};
        if (!oneBasedBar.has_value())
            return std::nullopt;
        return MusicalPosition{
            .bar = *oneBasedBar,
            .beat = static_cast<std::uint32_t>(
                (localTick % *barTicks) / ticksPerBeat + 1),
            .tick = static_cast<std::uint32_t>(
                (localTick % *barTicks) % ticksPerBeat),
        };
    }
    return std::nullopt;
}

std::optional<std::uint32_t> microsecondsPerQuarterForLoop(double durationSeconds, double beats) noexcept
{
    if (!std::isfinite(durationSeconds) || !std::isfinite(beats) || !(durationSeconds > 0.0) || !(beats > 0.0))
        return std::nullopt;
    const auto microseconds = std::round(durationSeconds * 1'000'000.0 / beats);
    if (microseconds < minimumMicrosecondsPerQuarter || microseconds > maximumMicrosecondsPerQuarter)
        return std::nullopt;
    return static_cast<std::uint32_t>(microseconds);
}
}
