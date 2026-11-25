#pragma once

#include "talas/audio/TapeRecorder.h"
#include "talas/audio/TimelineRecording.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace talas::app::workbench
{
enum class TapeRecorderButton
{
    Stop,
    Play,
    Pause,
    Record,
    Reset,
};

// The Tape Recorder: a tape recorder's control panel recording the
// first two inputs. The transport buttons, stop at the left and record at
// the right; Filename for the file, its format (WAV or AIFF) and its bits
// (8, 16 or 24); the Left and Right buttons choosing the inputs recorded,
// and so the file's channels; the meters with Max(L), Max(R) and Reset;
// the Input faders; and the time counter.
class TapeRecorderPanel : public juce::Component
{
public:
    // The audio device's sample rate, 0 without one.
    using SampleRateQuery = std::function<double()>;
    // Whether a recording may be saved at this path: not over the open
    // project, its package or a source asset.
    using DestinationCheck = std::function<bool(const std::filesystem::path&)>;

    ~TapeRecorderPanel() override = default;

    // The stop button, as closing the window presses it: a recording ends
    // and is saved, a playback stops.
    virtual void stop() = 0;
    [[nodiscard]] virtual talas::audio::TapeRecorderState state() const noexcept = 0;
    [[nodiscard]] virtual bool hasRecording() const noexcept = 0;
    [[nodiscard]] virtual std::filesystem::path destination() const = 0;
    [[nodiscard]] virtual juce::String statusText() const = 0;
    [[nodiscard]] virtual juce::String counterText() const = 0;
    [[nodiscard]] virtual juce::String maxLevelText(std::size_t channel) const = 0;

    // Smoke access to the controls.
    virtual void pressForSmoke(TapeRecorderButton button) = 0;
    virtual void setFileForSmoke(
        const std::filesystem::path& destination, talas::audio::TimelineRecordingFormat format, std::uint16_t bits) = 0;
    virtual void setChannelForSmoke(std::size_t channel, bool enabled) = 0;
    virtual void setInputLevelForSmoke(std::size_t channel, double level) = 0;
    virtual void refreshForSmoke() = 0;
    // A sample rate in place of the device's, for a Workbench without one.
    virtual void setSampleRateForSmoke(double sampleRate) = 0;
};

[[nodiscard]] std::unique_ptr<TapeRecorderPanel> makeTapeRecorderPanel(
    talas::audio::TapeRecorderEngine& engine,
    TapeRecorderPanel::SampleRateQuery sampleRate,
    TapeRecorderPanel::DestinationCheck destinationAllowed);

// A window for the panel; its close box stops the recorder and hides it.
[[nodiscard]] std::unique_ptr<juce::DocumentWindow> makeTapeRecorderWindow(TapeRecorderPanel& panel);
}
