#include "app/workbench/WorkbenchSampleRegionInspector.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <array>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class SampleRegionInspectorImpl final : public SampleRegionInspector
{
public:
    explicit SampleRegionInspectorImpl(
        SampleRegionInspectorCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , applyButton_("Apply Region")
        , removeButton_("Remove")
        , relinkButton_("Relink...")
        , reloadButton_("Reload")
        , showButton_("Show")
        , embedButton_("Embed")
        , referenceButton_("Reference...")
    {
        describeControl(
            *this,
            "Sample region inspector",
            "Edit exact MIDI ranges, root note, direction, loop modes, loop frames, and accepted asset identity.");
        title_.setText("Region Inspector", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setFont(WorkbenchType::panelTitle());
        selection_.setColour(juce::Label::textColourId, secondaryTextColour);
        selection_.setFont(WorkbenchType::microStrong());
        asset_.setColour(juce::Label::textColourId, secondaryTextColour);
        asset_.setFont(WorkbenchType::micro());
        asset_.setMinimumHorizontalScale(1.0F);
        asset_.setJustificationType(juce::Justification::topLeft);

        configureEditor(rootNote_, "Root note", "MIDI root note, 0 through 127.");
        configureEditor(lowNote_, "Low note", "Inclusive lowest MIDI note.");
        configureEditor(highNote_, "High note", "Inclusive highest MIDI note.");
        configureEditor(lowVelocity_, "Low velocity", "Inclusive lowest MIDI velocity, 1 through 127.");
        configureEditor(highVelocity_, "High velocity", "Inclusive highest MIDI velocity, 1 through 127.");
        configureEditor(sustainStart_, "Sustain loop start", "Exact inclusive source frame.");
        configureEditor(sustainEnd_, "Sustain loop end", "Exact exclusive source frame.");
        configureEditor(releaseStart_, "Release loop start", "Exact inclusive source frame.");
        configureEditor(releaseEnd_, "Release loop end", "Exact exclusive source frame.");
        reverse_.setButtonText("Reverse playback");
        sustainEnabled_.setButtonText("Sustain loop");
        releaseEnabled_.setButtonText("Release loop");
        sustainMode_.addItem("Forward", 1);
        sustainMode_.addItem("Alternate", 2);
        releaseMode_.addItem("Forward", 1);
        releaseMode_.addItem("Alternate", 2);
        describeControl(reverse_, "Reverse playback", "Read this region from its end toward its start.");
        describeControl(
            sustainEnabled_,
            "Enable sustain loop",
            "Repeat the exact sustain frame range while the gate remains active.");
        describeControl(
            releaseEnabled_,
            "Enable release loop",
            "Repeat the exact release frame range after the gate closes.");
        describeControl(sustainMode_, "Sustain loop mode", "Choose forward or alternating endpoint traversal.");
        describeControl(releaseMode_, "Release loop mode", "Choose forward or alternating endpoint traversal.");

        for (auto* component : allComponents())
            addAndMakeVisible(*component);

        applyButton_.onClick = [this] { apply(); };
        removeButton_.onClick = [this] {
            if (callbacks_.removeSelection)
                callbacks_.removeSelection();
        };
        relinkButton_.onClick = [this] {
            if (callbacks_.relinkAsset && state_.asset.has_value())
                callbacks_.relinkAsset(state_.asset->id);
        };
        reloadButton_.onClick = [this] {
            if (callbacks_.reloadAsset && state_.asset.has_value())
                callbacks_.reloadAsset(state_.asset->id);
        };
        showButton_.onClick = [this] {
            if (callbacks_.showAsset && state_.asset.has_value())
                callbacks_.showAsset(state_.asset->id);
        };
        embedButton_.onClick = [this] {
            if (callbacks_.embedAsset && state_.asset.has_value())
                callbacks_.embedAsset(state_.asset->id);
        };
        referenceButton_.onClick = [this] {
            if (callbacks_.referenceAsset && state_.asset.has_value())
                callbacks_.referenceAsset(state_.asset->id);
        };
        sustainEnabled_.onClick = [this] { refreshEnabledState(); };
        releaseEnabled_.onClick = [this] { refreshEnabledState(); };
    }

    void setState(const SampleRegionInspectorState& state) override
    {
        state_ = state;
        selection_.setText(
            state_.selectionCount == 0
                ? "No region selected"
                : juce::String(static_cast<int>(state_.selectionCount))
                + (state_.selectionCount == 1
                           ? " region selected"
                           : juceString(" regions selected · mixed values")),
            juce::dontSendNotification);
        if (state_.primaryRegion.has_value()
            && state_.selectionCount == 1)
        {
            const auto& region = *state_.primaryRegion;
            setNumber(rootNote_, region.rootNote);
            setNumber(lowNote_, region.lowNote);
            setNumber(highNote_, region.highNote);
            setNumber(lowVelocity_, region.lowVelocity);
            setNumber(highVelocity_, region.highVelocity);
            reverse_.setToggleState(region.reverse, juce::dontSendNotification);
            setLoop(
                region.sustainLoop,
                sustainEnabled_,
                sustainMode_,
                sustainStart_,
                sustainEnd_);
            setLoop(
                region.releaseLoop,
                releaseEnabled_,
                releaseMode_,
                releaseStart_,
                releaseEnd_);
        }
        else
        {
            for (auto* editor : editors())
                editor->clear();
            reverse_.setToggleState(false, juce::dontSendNotification);
            sustainEnabled_.setToggleState(false, juce::dontSendNotification);
            releaseEnabled_.setToggleState(false, juce::dontSendNotification);
        }

        auto assetText = state_.asset.has_value()
            ? juceString(state_.asset->id)
                + juceString(" · gen ")
                + juce::String(static_cast<juce::int64>(state_.asset->generation))
                + "\n" + juceString(state_.asset->path)
            : juce::String{"No selected asset"};
        if (!state_.statusText.empty())
            assetText += "\n" + juceString(state_.statusText);
        asset_.setText(assetText, juce::dontSendNotification);
        asset_.setTitle(assetText);
        asset_.setColour(
            juce::Label::textColourId,
            state_.assetState == SampleAssetPresentation::Ready
                ? secondaryTextColour
                : errorColour);
        refreshEnabledState();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    [[nodiscard]] bool hasNumericEditorsForSmoke() const override
    {
        return state_.selectionCount == 1
            && rootNote_.isEnabled()
            && applyButton_.isEnabled();
    }

    [[nodiscard]] bool showsMultiSelectionForSmoke() const override
    {
        return state_.selectionCount > 1
            && selection_.getText().contains("regions selected")
            && !rootNote_.isEnabled()
            && removeButton_.isEnabled();
    }

    [[nodiscard]] bool hasAssetActionsForSmoke() const override
    {
        return state_.asset.has_value()
            && !showButton_.getButtonText().isEmpty()
            && !embedButton_.getButtonText().isEmpty()
            && !referenceButton_.getButtonText().isEmpty()
            && !showButton_.getBounds().isEmpty()
            && !embedButton_.getBounds().isEmpty()
            && !referenceButton_.getBounds().isEmpty()
            && showButton_.isEnabled()
                == (!state_.assetActionRunning && state_.canShowAsset)
            && embedButton_.isEnabled()
                == (!state_.assetActionRunning && state_.canEmbedAsset)
            && referenceButton_.isEnabled()
                == (!state_.assetActionRunning && state_.canReferenceAsset);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        title_.setBounds(bounds.removeFromTop(20));
        selection_.setBounds(bounds.removeFromTop(22));
        bounds.removeFromTop(6);
        auto dispositionActions = bounds.removeFromBottom(28);
        const auto dispositionWidth = (dispositionActions.getWidth() - 12) / 3;
        showButton_.setBounds(
            dispositionActions.removeFromLeft(dispositionWidth));
        dispositionActions.removeFromLeft(6);
        embedButton_.setBounds(
            dispositionActions.removeFromLeft(dispositionWidth));
        dispositionActions.removeFromLeft(6);
        referenceButton_.setBounds(dispositionActions);
        bounds.removeFromBottom(6);
        auto assetActions = bounds.removeFromBottom(28);
        const auto actionWidth = (assetActions.getWidth() - 6) / 2;
        relinkButton_.setBounds(assetActions.removeFromLeft(actionWidth));
        assetActions.removeFromLeft(6);
        reloadButton_.setBounds(assetActions);
        bounds.removeFromBottom(6);
        auto editActions = bounds.removeFromBottom(28);
        applyButton_.setBounds(editActions.removeFromLeft(
            std::max(90, editActions.getWidth() - 78)));
        editActions.removeFromLeft(6);
        removeButton_.setBounds(editActions);
        bounds.removeFromBottom(8);
        asset_.setBounds(bounds.removeFromBottom(72));
        bounds.removeFromBottom(8);

        layoutPair(bounds, "Root", rootNote_);
        layoutPair(bounds, "Low note", lowNote_);
        layoutPair(bounds, "High note", highNote_);
        layoutPair(bounds, "Low velocity", lowVelocity_);
        layoutPair(bounds, "High velocity", highVelocity_);
        reverse_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(4);
        layoutLoop(
            bounds,
            sustainEnabled_,
            sustainMode_,
            sustainStart_,
            sustainEnd_);
        bounds.removeFromTop(6);
        layoutLoop(
            bounds,
            releaseEnabled_,
            releaseMode_,
            releaseStart_,
            releaseEnd_);
    }

private:
    void configureEditor(
        juce::TextEditor& editor,
        const juce::String& title,
        const juce::String& description)
    {
        editor.setInputRestrictions(20, "0123456789");
        editor.setSelectAllWhenFocused(true);
        editor.setWantsKeyboardFocus(true);
        describeControl(editor, title, description);
    }

    [[nodiscard]] std::array<juce::Component*, 29> allComponents()
    {
        return {
            &title_, &selection_, &rootLabel_, &rootNote_,
            &lowNoteLabel_, &lowNote_, &highNoteLabel_, &highNote_,
            &lowVelocityLabel_, &lowVelocity_, &highVelocityLabel_,
            &highVelocity_, &reverse_, &sustainEnabled_, &sustainMode_,
            &sustainStart_, &sustainEnd_, &releaseEnabled_, &releaseMode_,
            &releaseStart_, &releaseEnd_, &asset_, &applyButton_,
            &removeButton_, &relinkButton_, &reloadButton_,
            &showButton_, &embedButton_, &referenceButton_,
        };
    }

    [[nodiscard]] std::array<juce::TextEditor*, 9> editors()
    {
        return {
            &rootNote_, &lowNote_, &highNote_, &lowVelocity_,
            &highVelocity_, &sustainStart_, &sustainEnd_,
            &releaseStart_, &releaseEnd_,
        };
    }

    void setNumber(juce::TextEditor& editor, std::uint64_t value)
    {
        editor.setText(
            juce::String(static_cast<juce::int64>(value)),
            false);
    }

    void setLoop(
        const std::optional<talas::project::ProjectSampleLoop>& loop,
        juce::ToggleButton& enabled,
        juce::ComboBox& mode,
        juce::TextEditor& start,
        juce::TextEditor& end)
    {
        enabled.setToggleState(loop.has_value(), juce::dontSendNotification);
        mode.setSelectedId(
            loop.has_value()
                    && loop->mode == talas::project::ProjectSampleLoopMode::Alternate
                ? 2
                : 1,
            juce::dontSendNotification);
        setNumber(start, loop.has_value() ? loop->startFrame : 0);
        setNumber(end, loop.has_value() ? loop->endFrameExclusive : 0);
    }

    void refreshEnabledState()
    {
        const auto single = state_.selectionCount == 1
            && state_.primaryRegion.has_value();
        for (auto* editor : editors())
            editor->setEnabled(single);
        reverse_.setEnabled(single);
        sustainEnabled_.setEnabled(single);
        releaseEnabled_.setEnabled(single);
        sustainMode_.setEnabled(single && sustainEnabled_.getToggleState());
        sustainStart_.setEnabled(single && sustainEnabled_.getToggleState());
        sustainEnd_.setEnabled(single && sustainEnabled_.getToggleState());
        releaseMode_.setEnabled(single && releaseEnabled_.getToggleState());
        releaseStart_.setEnabled(single && releaseEnabled_.getToggleState());
        releaseEnd_.setEnabled(single && releaseEnabled_.getToggleState());
        applyButton_.setEnabled(single);
        removeButton_.setEnabled(state_.selectionCount > 0);
        relinkButton_.setEnabled(
            !state_.assetActionRunning && state_.asset.has_value());
        reloadButton_.setEnabled(
            !state_.assetActionRunning && state_.asset.has_value()
            && state_.assetState == SampleAssetPresentation::Changed);
        showButton_.setEnabled(
            !state_.assetActionRunning && state_.canShowAsset);
        embedButton_.setEnabled(
            !state_.assetActionRunning && state_.canEmbedAsset);
        referenceButton_.setEnabled(
            !state_.assetActionRunning && state_.canReferenceAsset);
    }

    void layoutPair(
        juce::Rectangle<int>& bounds,
        const juce::String& text,
        juce::TextEditor& editor)
    {
        auto row = bounds.removeFromTop(26);
        auto* label = labelFor(editor);
        if (label != nullptr)
        {
            label->setText(text, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::microStrong());
            label->setBounds(row.removeFromLeft(94));
        }
        editor.setBounds(row);
        bounds.removeFromTop(4);
    }

    juce::Label* labelFor(juce::TextEditor& editor)
    {
        if (&editor == &rootNote_)
            return &rootLabel_;
        if (&editor == &lowNote_)
            return &lowNoteLabel_;
        if (&editor == &highNote_)
            return &highNoteLabel_;
        if (&editor == &lowVelocity_)
            return &lowVelocityLabel_;
        return &highVelocityLabel_;
    }

    void layoutLoop(
        juce::Rectangle<int>& bounds,
        juce::ToggleButton& enabled,
        juce::ComboBox& mode,
        juce::TextEditor& start,
        juce::TextEditor& end)
    {
        auto heading = bounds.removeFromTop(26);
        enabled.setBounds(heading.removeFromLeft(106));
        heading.removeFromLeft(4);
        mode.setBounds(heading);
        bounds.removeFromTop(4);
        auto frames = bounds.removeFromTop(26);
        const auto width = (frames.getWidth() - 6) / 2;
        start.setBounds(frames.removeFromLeft(width));
        frames.removeFromLeft(6);
        end.setBounds(frames);
    }

    void apply()
    {
        if (!state_.primaryRegion.has_value() || !callbacks_.applyRegion)
            return;
        auto region = *state_.primaryRegion;
        region.rootNote = static_cast<std::uint8_t>(std::clamp(rootNote_.getText().getIntValue(), 0, 127));
        region.lowNote = static_cast<std::uint8_t>(std::clamp(lowNote_.getText().getIntValue(), 0, 127));
        region.highNote = static_cast<std::uint8_t>(std::clamp(highNote_.getText().getIntValue(), 0, 127));
        region.lowVelocity = static_cast<std::uint8_t>(std::clamp(lowVelocity_.getText().getIntValue(), 1, 127));
        region.highVelocity = static_cast<std::uint8_t>(std::clamp(highVelocity_.getText().getIntValue(), 1, 127));
        region.reverse = reverse_.getToggleState();
        region.sustainLoop = readLoop(
            sustainEnabled_, sustainMode_, sustainStart_, sustainEnd_);
        region.releaseLoop = readLoop(
            releaseEnabled_, releaseMode_, releaseStart_, releaseEnd_);
        callbacks_.applyRegion(std::move(region));
    }

    std::optional<talas::project::ProjectSampleLoop> readLoop(
        const juce::ToggleButton& enabled,
        const juce::ComboBox& mode,
        const juce::TextEditor& start,
        const juce::TextEditor& end) const
    {
        if (!enabled.getToggleState())
            return std::nullopt;
        return talas::project::ProjectSampleLoop{
            mode.getSelectedId() == 2
                ? talas::project::ProjectSampleLoopMode::Alternate
                : talas::project::ProjectSampleLoopMode::Forward,
            static_cast<std::uint64_t>(std::max<juce::int64>(0, start.getText().getLargeIntValue())),
            static_cast<std::uint64_t>(std::max<juce::int64>(0, end.getText().getLargeIntValue())),
        };
    }

    SampleRegionInspectorCallbacks callbacks_;
    SampleRegionInspectorState state_;
    juce::Label title_;
    juce::Label selection_;
    juce::Label rootLabel_;
    juce::Label lowNoteLabel_;
    juce::Label highNoteLabel_;
    juce::Label lowVelocityLabel_;
    juce::Label highVelocityLabel_;
    juce::TextEditor rootNote_;
    juce::TextEditor lowNote_;
    juce::TextEditor highNote_;
    juce::TextEditor lowVelocity_;
    juce::TextEditor highVelocity_;
    juce::ToggleButton reverse_;
    juce::ToggleButton sustainEnabled_;
    juce::ComboBox sustainMode_;
    juce::TextEditor sustainStart_;
    juce::TextEditor sustainEnd_;
    juce::ToggleButton releaseEnabled_;
    juce::ComboBox releaseMode_;
    juce::TextEditor releaseStart_;
    juce::TextEditor releaseEnd_;
    juce::Label asset_;
    juce::TextButton applyButton_;
    juce::TextButton removeButton_;
    juce::TextButton relinkButton_;
    juce::TextButton reloadButton_;
    juce::TextButton showButton_;
    juce::TextButton embedButton_;
    juce::TextButton referenceButton_;
};
}

std::unique_ptr<SampleRegionInspector>
makeSampleRegionInspector(SampleRegionInspectorCallbacks callbacks)
{
    return std::make_unique<SampleRegionInspectorImpl>(
        std::move(callbacks));
}
}
