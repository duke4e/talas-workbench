#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

// The DSP status window: the time
// since the Sound started, incoming MIDI time code, the input and output
// level bars with their marks every 10 dB, and the peak level indicators.
namespace talas::app
{
// The time since the Sound started, as hours:minutes:seconds.
[[nodiscard]] std::string dspStatusElapsedText(double seconds);
// The DSP Status window's Sample Rate menu,
// the seven rates (the compact disc's 44100, 22050 and 11025, the DAT's
// 48000, 24000 and 12000, and extended-play DAT's 32000), those the audio
// device offers, highest first.
[[nodiscard]] std::vector<double> talasSampleRates();
[[nodiscard]] std::vector<double> dspStatusSampleRateChoices(std::span<const double> deviceRates);
// The signal processor's usage as the Sounds start
// and end, from 0 to 100 percent of the audio callback's time; Out of Real
// Time once the Sounds take all of it.
[[nodiscard]] std::string dspStatusProcessorText(double load);

// MIDI time code as hours:minutes:seconds.frames.
[[nodiscard]] std::string dspStatusTimeCodeText(unsigned hour, unsigned minute, unsigned second, unsigned frame);

// A level bar runs from its floor to 0 dB, marked every 10 dB.
inline constexpr double dspStatusMeterFloorDb = -60.0;
// The meters on each side: two, or all eight with
// Show Eight Channels in DSP Status, the channels the interface lacks
// registering nothing.
inline constexpr std::size_t dspStatusMaximumChannels = 8;
[[nodiscard]] constexpr std::size_t dspStatusMeterCount(bool eightChannels) noexcept { return eightChannels ? 8 : 2; }
[[nodiscard]] double dspStatusMeterFraction(double peak);
[[nodiscard]] std::vector<double> dspStatusMeterMarksDb();

// The elapsed time: reset as a Sound starts, counting while it plays and
// held once it is stopped.
class DspStatusClock
{
public:
    void start(double nowSeconds) noexcept;
    void stop(double nowSeconds) noexcept;
    [[nodiscard]] double elapsed(double nowSeconds) const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    double startedAt_ = 0.0;
    double heldElapsed_ = 0.0;
    bool running_ = false;
};

// A peak level indicator: the highest level since it was last reset,
// showing >0< once a peak reached full scale, where the signal may have
// clipped.
class DspPeakIndicator
{
public:
    void observe(double peak) noexcept;
    void reset() noexcept;
    [[nodiscard]] double held() const noexcept;
    [[nodiscard]] bool clipped() const noexcept;
    [[nodiscard]] std::string text() const;

private:
    double held_ = 0.0;
};

// The status window's memory bar: the share of
// the computer's memory in use, cyan for light use, green and yellow for
// moderate use and red for heavy use.
enum class MemoryUseColour
{
    Cyan,
    Green,
    Yellow,
    Red,
};

[[nodiscard]] MemoryUseColour memoryUseColour(double fraction) noexcept;

// The open status window recycles memory by itself
// when the use turns heavy (red), no more often than every half minute.
inline constexpr double automaticRecycleSeconds = 30.0;
[[nodiscard]] bool shouldRecycleAutomatically(double fraction, double secondsSinceRecycled) noexcept;

// The status window's account of the Sound last loaded: the global map that
// defines its Event Values, whether it takes input from the A/D (an
// AudioInput, or a GenericSource set to live input), the MIDI channels its
// MIDIVoices and MIDIMappers expect keyboard input on, and the Event Values
// that control its parameters.
struct LoadedSoundInputs
{
    std::string globalMap;
    bool audioInput = false;
    std::vector<std::string> midiChannels;
    std::vector<std::string> eventValues;
    // Whether it plays from or records to the disk (a DiskPlayer, a
    // GenericSource reading the disk, ...), which a Shift+click stops
    //.
    bool usesDisk = false;

    bool operator==(const LoadedSoundInputs&) const = default;
};

[[nodiscard]] LoadedSoundInputs loadedSoundInputs(const talas::project::ProjectDocument& document);
// A Sound that takes live input, from the A/D or a MIDI keyboard.
[[nodiscard]] bool takesLiveInput(const LoadedSoundInputs& inputs) noexcept;

// Record to disk's count-off: for a Sound with
// live input, a beep on each of the beats asked for at the tempo asked for,
// the recording beginning on the beat after; none for zero beats.
struct RecordingCountOff
{
    std::vector<double> beepSeconds;
    double recordingStartSeconds = 0.0;
};

[[nodiscard]] RecordingCountOff recordingCountOff(unsigned beats, double beatsPerMinute);
// The lines the window shows for them.
[[nodiscard]] std::vector<std::string> loadedSoundInputLines(const LoadedSoundInputs& inputs);

// Show MIDI Messages: each message as it
// arrives, the keyboard's and the controllers' named with their channel
// and values, others by their bytes in hexadecimal; and the window's last
// lines, the oldest leaving first.
[[nodiscard]] std::string midiMessageDescription(std::span<const std::uint8_t> bytes);

class MidiMessageLog
{
public:
    static constexpr std::size_t capacity = 200;

    void add(std::string line);
    [[nodiscard]] const std::deque<std::string>& lines() const noexcept { return lines_; }
    [[nodiscard]] std::string text() const;

private:
    std::deque<std::string> lines_;
};
}
