#include "plugin/TalasPluginEditor.h"

#include "plugin/TalasPluginProcessor.h"

namespace talas::plugin
{
juce::Point<int> TalasPluginEditor::preferredSize(std::size_t parameterCount) noexcept
{
    return TalasPluginEditorView::preferredSize(parameterCount);
}

TalasPluginEditor::TalasPluginEditor(TalasPluginProcessor& processor)
    : juce::AudioProcessorEditor(processor)
{
    setLookAndFeel(&lookAndFeel_);
    PluginEditorModel model;
    model.identity = processor.identity();
    model.kind = processor.kind();
    const auto& specs = processor.parameterSpecs();
    for (std::size_t index = 0; index < specs.size(); ++index)
        model.parameters.push_back({specs[index], processor.parameterFor(index)});
    model.status = [&processor] { return processor.statusText(); };
    model.ready = [&processor] { return processor.engineReady(); };
    view_ = std::make_unique<TalasPluginEditorView>(std::move(model));
    addAndMakeVisible(*view_);
    setSize(view_->getWidth(), view_->getHeight());
}

TalasPluginEditor::~TalasPluginEditor()
{
    view_.reset();
    setLookAndFeel(nullptr);
}

void TalasPluginEditor::resized()
{
    if (view_ != nullptr)
        view_->setBounds(getLocalBounds());
}
}
