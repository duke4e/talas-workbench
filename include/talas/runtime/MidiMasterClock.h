#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace talas::runtime
{

inline constexpr std::uint8_t midiTimingClockByte = 0xF8;
inline constexpr std::uint8_t midiStartByte = 0xFA;
inline constexpr std::uint8_t midiContinueByte = 0xFB;
inline constexpr std::uint8_t midiStopByte = 0xFC;

struct MidiMasterClockState
{
    std::uint64_t runningFrames = 0;
    std::uint64_t nextPulse = 0;
    bool running = false;
    bool everStarted = false;
    bool restartWasPositive = false;
};

[[nodiscard]] inline bool validMidiMasterClockRate(
    double sampleRate, double tempoBpm) noexcept
{
    return std::isfinite(sampleRate) && sampleRate > 0.0
        && std::isfinite(tempoBpm) && tempoBpm >= 20.0
        && tempoBpm <= 300.0;
}

// The emitter receives one MIDI real-time status byte. Advance the candidate
// state only after the caller has accepted every event in the frame.
template <typename Emit>
[[nodiscard]] bool advanceMidiMasterClock(
    MidiMasterClockState& state, bool run, bool restart,
    bool sendTransport, double sampleRate, double tempoBpm,
    Emit&& emit)
{
    if (!validMidiMasterClockRate(sampleRate, tempoBpm))
        return false;

    if (!run)
    {
        if (state.running && sendTransport && !emit(midiStopByte))
            return false;
        state.running = false;
        state.restartWasPositive = restart;
        return true;
    }

    const bool wasRunning = state.running;
    if (!wasRunning)
    {
        const bool start = !state.everStarted || restart;
        if (sendTransport
            && !emit(start ? midiStartByte : midiContinueByte))
            return false;
        if (start)
        {
            state.runningFrames = 0;
            state.nextPulse = 0;
        }
        state.running = true;
        state.everStarted = true;
    }
    else if (restart && !state.restartWasPositive && sendTransport)
    {
        if (!emit(midiStartByte))
            return false;
        state.runningFrames = 0;
        state.nextPulse = 0;
    }
    state.restartWasPositive = restart;

    const long double framesPerPulse =
        static_cast<long double>(sampleRate) * 60.0L
        / (static_cast<long double>(tempoBpm) * 24.0L);
    while (true)
    {
        const long double target = std::round(
            static_cast<long double>(state.nextPulse) * framesPerPulse);
        if (target > static_cast<long double>(state.runningFrames))
            break;
        if (state.nextPulse == std::numeric_limits<std::uint64_t>::max()
            || !emit(midiTimingClockByte))
            return false;
        ++state.nextPulse;
    }
    if (state.runningFrames == std::numeric_limits<std::uint64_t>::max())
        return false;
    ++state.runningFrames;
    return true;
}

} // namespace talas::runtime
