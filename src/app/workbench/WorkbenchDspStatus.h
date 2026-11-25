#pragma once

#include "talas/app/DspStatusModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// What the DSP status window reads: the time since the Sound started, the
// last MIDI time code received, the input and output peaks, and what the
// Sound last loaded takes its input from.
struct DspStatusReading
{
    double elapsedSeconds = 0.0;
    std::optional<std::array<unsigned, 4>> timeCode;
    std::array<float, talas::app::dspStatusMaximumChannels> input{};
    std::array<float, talas::app::dspStatusMaximumChannels> output{};
    // Show Eight Channels in DSP Status.
    bool eightChannels = false;
    std::vector<std::string> soundInputs;
    // The share of the computer's memory in use.
    double memoryFraction = 0.0;
    // The audio callback's share of its time.
    double processorLoad = 0.0;
    // The device's sample rate and the rates it offers.
    double sampleRate = 0.0;
    std::vector<double> deviceSampleRates;
    // Whether the window goes on updating: not while a Sound uses the disk
    // with Update display during disk operations off.
    bool animate = true;
};

// The DSP status window: elapsed time
// upper left, time code upper right, the input and output level bars marked
// every 10 dB, and a peak indicator between each pair, which a click resets.
class DspStatusPanel final : public juce::Component, private juce::Timer
{
public:
    // recycle: the Recycle button; setSampleRate: the
    // Sample Rate menu's choice.
    DspStatusPanel(std::function<DspStatusReading()> read, std::function<void()> recycle,
        std::function<void(double)> setSampleRate = {});
    [[nodiscard]] juce::StringArray sampleRateChoicesForSmoke() const;
    ~DspStatusPanel() override;

    void refresh();
    void resetPeaks();
    [[nodiscard]] std::string elapsedText() const;
    [[nodiscard]] std::string timeCodeText() const;
    [[nodiscard]] std::string peakText(bool output) const;
    [[nodiscard]] const std::vector<std::string>& soundInputLines() const noexcept { return reading_.soundInputs; }
    [[nodiscard]] double memoryFraction() const noexcept { return reading_.memoryFraction; }
    [[nodiscard]] std::size_t meterCount() const noexcept { return talas::app::dspStatusMeterCount(reading_.eightChannels); }
    void resized() override;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    void timerCallback() override;

    std::function<DspStatusReading()> read_;
    std::function<void(double)> setSampleRate_;
    juce::ComboBox sampleRate_;
    std::vector<double> sampleRateChoices_;
    void refreshSampleRates();
    std::function<void()> recycleMemory_;
    // When memory was last recycled, by the button or automatically.
    double recycledSeconds_ = 0.0;
    juce::TextButton recycle_{"Recycle"};
    DspStatusReading reading_;
    talas::app::DspPeakIndicator inputPeak_;
    talas::app::DspPeakIndicator outputPeak_;
};
}
