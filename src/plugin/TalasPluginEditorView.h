#pragma once

#include "app/workbench/WorkbenchTheme.h"
#include "talas/plugin/PluginIdentity.h"
#include "talas/plugin/PluginParameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

namespace talas::plugin
{
// One host parameter and the Event Value it plays.
struct PluginEditorParameter
{
    PluginParameterSpec spec;
    juce::RangedAudioParameter* parameter = nullptr;
};

// What the editor shows: the plugin's identity and kind, its parameters,
// and its status as the processor reports it.
struct PluginEditorModel
{
    PluginIdentity identity;
    PluginKind kind = PluginKind::Instrument;
    std::vector<PluginEditorParameter> parameters;
    std::function<juce::String()> status;
    std::function<bool()> ready;
};

// ADR-0029: the plugin editor's face, apart from its processor so the
// Workbench can show it too (its screenshots render one): the backdrop and
// glass panels of WorkbenchTheme, the Talas mark and the plugin's name over
// the signature gradient, a knob per fader and a pad per toggle or gate,
// and the status below.
class TalasPluginEditorView final : public juce::Component, private juce::Timer
{
public:
    explicit TalasPluginEditorView(PluginEditorModel model);
    ~TalasPluginEditorView() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

    // The layout's size for a parameter count.
    [[nodiscard]] static juce::Point<int> preferredSize(std::size_t parameterCount) noexcept;

private:
    struct Control;
    class ControlGrid;

    void timerCallback() override;

    PluginEditorModel model_;
    std::unique_ptr<ControlGrid> grid_;
    juce::Viewport viewport_;
    juce::Label status_;
    juce::String shownStatus_;
    bool problem_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TalasPluginEditorView)
};

// A host parameter for a spec: a toggle or gate as a switch, a fader with
// the spec's own range, taper and grid, so the host shows it as the Virtual
// Control Surface does.
[[nodiscard]] std::unique_ptr<juce::RangedAudioParameter> makePluginParameter(const PluginParameterSpec& spec);
}
