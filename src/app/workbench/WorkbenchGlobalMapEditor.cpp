#include "app/workbench/WorkbenchGlobalMapEditor.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/app/TalasFiles.h"
#include "talas/project/ProjectGlobalMap.h"

#include <fstream>

namespace talas::app::workbench
{
namespace
{
class GlobalMapEditorPanelImpl final : public GlobalMapEditorPanel
{
public:
    GlobalMapEditorPanelImpl(talas::runtime::ProjectRuntimeBridge& bridge, DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
    {
        setTitle("Global Map");
        text_.setMultiLine(true, false);
        text_.setReturnKeyStartsNewLine(true);
        text_.setFont(WorkbenchType::body());
        text_.setTitle("Global map text");
        explanation_.setText(
            "One association a line: !Name is: `MIDIControllerNN. or (`MIDIControllerNN channel: 2; "
            "min: 0 max: 10 grid: 1; taper: #log; displayAs: #gate). or `MIDIPitchBend.",
            juce::dontSendNotification);
        explanation_.setColour(juce::Label::textColourId, secondaryTextColour);
        apply_.onClick = [this] { (void)apply(); };
        useDefault_.onClick = [this] { (void)useDefault(); };
        save_.onClick = [this] { (void)save(); };
        for (auto* component : std::initializer_list<juce::Component*>{&text_, &explanation_, &apply_, &useDefault_})
            addAndMakeVisible(*component);
        addChildComponent(save_);
        setSize(560, 420);
        refresh();
    }

    void refresh() override
    {
        file_.reset();
        save_.setVisible(false);
        const auto& map = bridge_.editor().document().globalMap;
        text_.setText(juce::String{map.empty() ? talas::project::defaultGlobalMapText() : map}, false);
    }

    bool openFile(const std::filesystem::path& path) override
    {
        const auto text = talas::app::globalMapFileText(path);
        if (!text.has_value())
        {
            onStatus_("The global map file could not be read");
            return false;
        }
        file_ = path;
        save_.setVisible(true);
        text_.setText(juce::String::fromUTF8(text->data(), static_cast<int>(text->size())), false);
        onStatus_("Opened the global map " + juce::String{path.stem().string()} + "; Apply makes it the project's");
        return true;
    }

    [[nodiscard]] juce::String textForSmoke() const override
    {
        return text_.getText();
    }

    void setTextForSmoke(const juce::String& text) override
    {
        text_.setText(text, false);
    }

    [[nodiscard]] bool applyForSmoke() override
    {
        return apply();
    }

    [[nodiscard]] bool useDefaultForSmoke() override
    {
        return useDefault();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto buttons = bounds.removeFromBottom(28);
        useDefault_.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(6);
        apply_.setBounds(buttons.removeFromRight(80));
        buttons.removeFromRight(6);
        save_.setBounds(buttons.removeFromRight(80));
        bounds.removeFromBottom(6);
        explanation_.setBounds(bounds.removeFromTop(36));
        text_.setBounds(bounds);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

private:
    bool apply()
    {
        const auto result = bridge_.editor().setGlobalMap(text_.getText().toStdString());
        if (!result.ok())
        {
            onStatus_(juce::String{result.message});
            return false;
        }
        onDocumentChange_();
        refresh();
        onStatus_(bridge_.editor().document().globalMap.empty() ? "The project uses the Default global map"
                                                                : "Global map applied");
        return true;
    }

    bool save()
    {
        if (!file_.has_value())
            return false;
        std::ofstream output(*file_, std::ios::binary | std::ios::trunc);
        output << text_.getText().toStdString();
        onStatus_(output ? "Saved the global map " + juce::String{file_->stem().string()} : juce::String{"The global map could not be saved"});
        return static_cast<bool>(output);
    }

    bool useDefault()
    {
        text_.setText(juce::String{talas::project::defaultGlobalMapText()}, false);
        return apply();
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    juce::TextEditor text_;
    juce::Label explanation_;
    juce::TextButton apply_{"Apply"};
    juce::TextButton useDefault_{"Use Default"};
    juce::TextButton save_{"Save"};
    std::optional<std::filesystem::path> file_;
};
}

std::unique_ptr<GlobalMapEditorPanel> makeGlobalMapEditorPanel(talas::runtime::ProjectRuntimeBridge& bridge,
    GlobalMapEditorPanel::DocumentChangeCallback onDocumentChange, GlobalMapEditorPanel::StatusCallback onStatus)
{
    return std::make_unique<GlobalMapEditorPanelImpl>(bridge, std::move(onDocumentChange), std::move(onStatus));
}
}
