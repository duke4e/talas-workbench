#include "talas/plugin/PluginHostTransport.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::plugin
{
namespace
{
constexpr double ticksPerQuarter = static_cast<double>(talas::project::projectTicksPerQuarter);

[[nodiscard]] double quartersIn(std::size_t frames, double sampleRate, double tempoBpm) noexcept
{
    return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate * tempoBpm / 60.0 : 0.0;
}
}

bool usableHostTempo(double bpm) noexcept
{
    return std::isfinite(bpm) && bpm >= 1.0 && bpm <= 60000.0;
}

std::uint64_t musicalTickForQuarters(double quarters) noexcept
{
    if (!std::isfinite(quarters) || quarters <= 0.0)
        return 0;
    const auto ticks = std::floor(quarters * ticksPerQuarter + 0.5);
    constexpr auto largest = 1.0e18;
    return ticks >= largest ? static_cast<std::uint64_t>(largest) : static_cast<std::uint64_t>(ticks);
}

std::optional<HostMeter> hostMeter(std::optional<int> numerator, std::optional<int> denominator) noexcept
{
    if (!numerator.has_value() || !denominator.has_value())
        return std::nullopt;
    const auto top = *numerator;
    const auto bottom = *denominator;
    const auto powerOfTwo = bottom > 0 && (bottom & (bottom - 1)) == 0;
    if (top < 1 || top > 64 || !powerOfTwo || bottom > 64)
        return std::nullopt;
    return HostMeter{static_cast<std::uint8_t>(top), static_cast<std::uint8_t>(bottom)};
}

talas::project::MusicalPosition hostMusicalPosition(
    double quarterPosition,
    std::optional<double> ppqPositionOfLastBarStart,
    HostMeter meter) noexcept
{
    if (!std::isfinite(quarterPosition) || quarterPosition < 0.0 || meter.numerator == 0 || meter.denominator == 0)
        return {};
    const auto beatQuarters = 4.0 / static_cast<double>(meter.denominator);
    const auto barQuarters = beatQuarters * static_cast<double>(meter.numerator);
    // A position rounds to the nearest tick first, so 7.9999999 is bar 3 in
    // 4/4, not the last tick of bar 2.
    const auto tick = static_cast<double>(musicalTickForQuarters(quarterPosition));
    const auto position = tick / ticksPerQuarter;
    auto barStart = std::floor(position / barQuarters) * barQuarters;
    if (ppqPositionOfLastBarStart.has_value() && std::isfinite(*ppqPositionOfLastBarStart)
        && *ppqPositionOfLastBarStart >= 0.0 && *ppqPositionOfLastBarStart <= position + 1.0e-9
        && position - *ppqPositionOfLastBarStart < barQuarters)
    {
        barStart = *ppqPositionOfLastBarStart;
    }
    const auto bar = std::floor(barStart / barQuarters + 0.5);
    const auto inBar = std::max(0.0, position - barStart);
    const auto beat = std::min(
        static_cast<double>(meter.numerator - 1), std::floor(inBar / beatQuarters + 1.0e-9));
    const auto beatTicks = beatQuarters * ticksPerQuarter;
    const auto inBeat = std::clamp(
        std::floor((inBar - beat * beatQuarters) * ticksPerQuarter + 1.0e-6), 0.0, beatTicks - 1.0);
    return {
        static_cast<std::uint64_t>(bar) + 1,
        static_cast<std::uint32_t>(beat) + 1,
        static_cast<std::uint32_t>(inBeat),
    };
}

void HostTransportFollower::reset() noexcept
{
    block_ = {};
    following_ = false;
    expectedQuarter_ = 0.0;
}

const HostTransportBlock& HostTransportFollower::follow(
    const HostPlayheadReading& reading,
    std::size_t frames,
    double sampleRate) noexcept
{
    if (!reading.hasPlayhead || !reading.bpm.has_value() || !usableHostTempo(*reading.bpm)
        || !std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        reset();
        return block_;
    }
    const auto wasFollowing = following_;
    const auto wasPlaying = wasFollowing && block_.playing;

    HostTransportBlock next;
    next.followsHost = true;
    next.tempoBpm = *reading.bpm;
    next.playing = reading.playing;
    next.looping = reading.looping;
    next.quarterPosition = reading.ppqPosition.has_value() && std::isfinite(*reading.ppqPosition)
        ? *reading.ppqPosition
        : (wasFollowing ? expectedQuarter_ : 0.0);
    const auto tolerance = jumpToleranceSeconds * next.tempoBpm / 60.0;
    next.relocated = !wasFollowing || std::abs(next.quarterPosition - expectedQuarter_) > tolerance;
    next.started = next.playing && !wasPlaying;
    next.stopped = !next.playing && wasPlaying;
    if (const auto meter = hostMeter(reading.timeSignatureNumerator, reading.timeSignatureDenominator))
    {
        next.meter = *meter;
        next.meterFromHost = true;
    }
    next.musicalTick = musicalTickForQuarters(next.quarterPosition);
    next.position = hostMusicalPosition(next.quarterPosition, reading.ppqPositionOfLastBarStart, next.meter);

    expectedQuarter_ = next.quarterPosition
        + (next.playing ? quartersIn(frames, sampleRate, next.tempoBpm) : 0.0);
    sampleRate_ = sampleRate;
    following_ = true;
    block_ = next;
    return block_;
}

talas::runtime::HostMusicalTransport HostTransportFollower::transportAt(
    std::size_t frameOffset,
    bool drivesSound) const noexcept
{
    talas::runtime::HostMusicalTransport transport;
    transport.tempoBpm = block_.tempoBpm;
    transport.quarterPosition = block_.quarterPosition
        + (block_.playing ? quartersIn(frameOffset, sampleRate_, block_.tempoBpm) : 0.0);
    transport.playing = block_.playing;
    transport.relocated = frameOffset == 0 && block_.relocated;
    transport.drivesSound = drivesSound;
    return transport;
}
}
