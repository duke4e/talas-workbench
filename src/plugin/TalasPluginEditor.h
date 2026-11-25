#pragma once

#include "app/workbench/WorkbenchTheme.h"
#include "plugin/TalasPluginEditorView.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace talas::plugin
{
class TalasPluginProcessor;

// ADR-0029: a compact editor in the Workbench's look, its face the
// TalasPluginEditorView: the backdrop and glass panels of WorkbenchTheme,
// the plugin's name over the signature gradient, and a knob (fader) or pad
// (toggle, gate) per parameter.
class TalasPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TalasPluginEditor(TalasPluginProcessor& processor);
    ~TalasPluginEditor() override;

    void resized() override;

    // The layout's size for a parameter count, for tests.
    [[nodiscard]] static juce::Point<int> preferredSize(std::size_t parameterCount) noexcept;

private:
    talas::app::workbench::WorkbenchLookAndFeel lookAndFeel_;
    std::unique_ptr<TalasPluginEditorView> view_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TalasPluginEditor)
};
}
