#pragma once

#include "app/workbench/WorkbenchTheme.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace talas::plugin
{
class TalasPluginProcessor;

// ADR-0029: a compact editor in the Workbench's look: the backdrop and glass
// panels of WorkbenchTheme, the plugin's name over the signature gradient,
// and a knob (fader) or switch (toggle, gate) per parameter.
class TalasPluginEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TalasPluginEditor(TalasPluginProcessor& processor);
    ~TalasPluginEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

    // The layout's size for a parameter count, for tests.
    [[nodiscard]] static juce::Point<int> preferredSize(std::size_t parameterCount) noexcept;

private:
    struct Control;
    class ControlGrid;

    void timerCallback() override;

    TalasPluginProcessor& processor_;
    talas::app::workbench::WorkbenchLookAndFeel lookAndFeel_;
    std::unique_ptr<ControlGrid> grid_;
    juce::Viewport viewport_;
    juce::Label status_;
    juce::String shownStatus_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TalasPluginEditor)
};
}
