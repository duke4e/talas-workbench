#pragma once

#include "talas/project/ProjectMusicalTime.h"
#include "talas/runtime/HostTransport.h"

#include <cstddef>
#include <cstdint>
#include <optional>

// ADR-0030: how a plugin reads its host's playhead each block and turns it
// into the engine's musical transport. JUCE-free, so it is tested on its own;
// the processor fills a HostPlayheadReading from AudioPlayHead::PositionInfo.
namespace talas::plugin
{
// One block's reading of the host's playhead, each field missing when the
// host leaves it out.
struct HostPlayheadReading
{
    // The host has a playhead and it answered for this block.
    bool hasPlayhead = false;
    std::optional<double> bpm;
    // Quarter notes since the start of the song, at the block's first frame.
    std::optional<double> ppqPosition;
    std::optional<double> ppqPositionOfLastBarStart;
    std::optional<int> timeSignatureNumerator;
    std::optional<int> timeSignatureDenominator;
    bool playing = false;
    bool looping = false;
    std::optional<double> loopStartPpq;
    std::optional<double> loopEndPpq;
};

struct HostMeter
{
    std::uint8_t numerator = 4;
    std::uint8_t denominator = 4;

    bool operator==(const HostMeter&) const = default;
};

// What the plugin's transport does in one host block.
struct HostTransportBlock
{
    // The host gave a usable tempo. Otherwise the project's own transport
    // and tempo run, as they do without a host.
    bool followsHost = false;
    double tempoBpm = 0.0;
    // At the block's first frame, in quarter notes and in musical ticks
    // (960 a quarter, 0 before the song's start).
    double quarterPosition = 0.0;
    std::uint64_t musicalTick = 0;
    // Bar, beat (of the meter's denominator) and tick within the beat, from 1.
    talas::project::MusicalPosition position;
    HostMeter meter;
    // The host gave its time signature; otherwise 4/4.
    bool meterFromHost = false;
    bool playing = false;
    bool started = false;
    bool stopped = false;
    // The position is not where the previous block left off: the first block
    // followed, a locate, a loop's return or a start somewhere else.
    bool relocated = false;
    bool looping = false;
};

// A tempo the engine's musical map can hold: 1 to 60000 BPM.
[[nodiscard]] bool usableHostTempo(double bpm) noexcept;

// The musical tick (960 a quarter) nearest a position in quarter notes; 0
// for a position before the song's start.
[[nodiscard]] std::uint64_t musicalTickForQuarters(double quarters) noexcept;

// A time signature the engine's musical map can hold (1 to 64 beats of a
// power-of-two note up to a 64th), or nothing.
[[nodiscard]] std::optional<HostMeter> hostMeter(std::optional<int> numerator, std::optional<int> denominator) noexcept;

// The bar, beat and tick of a position under a meter. A bar's start from
// the host places the beats; bars are counted in bars of the meter from the
// song's start, which hosts do not say otherwise. A position before the
// song's start is at its start.
[[nodiscard]] talas::project::MusicalPosition hostMusicalPosition(
    double quarterPosition,
    std::optional<double> ppqPositionOfLastBarStart,
    HostMeter meter) noexcept;

class HostTransportFollower
{
public:
    // How far, in seconds at the host's tempo, a block may start from where
    // the previous one ended before it counts as a jump: hosts round their
    // positions, so the test is not exact.
    static constexpr double jumpToleranceSeconds = 0.005;

    void reset() noexcept;

    // Reads the playhead of a block of `frames` frames at `sampleRate`.
    // A host that gives a tempo but no position plays from the song's start
    // at its tempo.
    const HostTransportBlock& follow(
        const HostPlayheadReading& reading,
        std::size_t frames,
        double sampleRate) noexcept;

    [[nodiscard]] const HostTransportBlock& block() const noexcept { return block_; }

    // The block's transport `frameOffset` frames into it, for a block the
    // engine renders in parts; a jump belongs to its first part.
    [[nodiscard]] talas::runtime::HostMusicalTransport transportAt(
        std::size_t frameOffset,
        bool drivesSound) const noexcept;

private:
    HostTransportBlock block_;
    bool following_ = false;
    // Where the next block should start, in quarter notes.
    double expectedQuarter_ = 0.0;
    double sampleRate_ = 48000.0;
};
}
