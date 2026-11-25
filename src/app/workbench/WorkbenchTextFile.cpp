#include "app/workbench/WorkbenchTextFile.h"

#include "app/workbench/WorkbenchTheme.h"

#include <fstream>
#include <iterator>
#include <string>

namespace talas::app::workbench
{
namespace
{
class TextFilePanelImpl final : public TextFilePanel
{
public:
    explicit TextFilePanelImpl(StatusCallback onStatus)
        : onStatus_(std::move(onStatus))
    {
        setTitle("Text file");
        editor_.setMultiLine(true, false);
        editor_.setReturnKeyStartsNewLine(true);
        editor_.setTabKeyUsedAsCharacter(true);
        editor_.setScrollbarsShown(true);
        editor_.setFont(WorkbenchType::data());
        editor_.setTitle("Text");
        save_.onClick = [this] { save(); };
        addAndMakeVisible(editor_);
        addAndMakeVisible(save_);
        setSize(560, 420);
    }

    void newFile() override
    {
        file_.reset();
        editor_.setText({}, false);
        onStatus_("A new text file; Save asks where it goes");
    }

    bool open(const std::filesystem::path& path) override
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            onStatus_("The text file could not be read");
            return false;
        }
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        editor_.setText(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), false);
        file_ = path;
        onStatus_("Opened " + juce::String{path.filename().string()});
        return true;
    }

    void save() override
    {
        if (file_.has_value())
        {
            (void) write(*file_);
            return;
        }
        chooser_ = std::make_unique<juce::FileChooser>("Save the text file",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.txt"), "*.txt");
        juce::Component::SafePointer<TextFilePanelImpl> safeThis(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                auto chosen = chooser.getResult();
                if (chosen.getFileExtension().isEmpty())
                    chosen = chosen.withFileExtension(".txt");
                (void) safeThis->write(std::filesystem::path{chosen.getFullPathName().toStdString()});
            });
    }

    [[nodiscard]] std::optional<std::filesystem::path> file() const override { return file_; }

    [[nodiscard]] juce::String title() const override
    {
        return file_.has_value() ? juce::String{file_->filename().string()} : juce::String{"Untitled text"};
    }

    [[nodiscard]] juce::String textForSmoke() const override { return editor_.getText(); }
    void setTextForSmoke(const juce::String& text) override { editor_.setText(text, false); }
    bool saveToForSmoke(const std::filesystem::path& path) override { return write(path); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        save_.setBounds(bounds.removeFromBottom(28).removeFromRight(90));
        bounds.removeFromBottom(6);
        editor_.setBounds(bounds);
    }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(panelColour); }

private:
    bool write(const std::filesystem::path& path)
    {
        const auto text = editor_.getText().toStdString();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        if (!output)
        {
            onStatus_("The text file could not be saved");
            return false;
        }
        file_ = path;
        onStatus_("Saved " + juce::String{path.filename().string()});
        return true;
    }

    StatusCallback onStatus_;
    juce::TextEditor editor_;
    juce::TextButton save_{"Save"};
    std::optional<std::filesystem::path> file_;
    std::unique_ptr<juce::FileChooser> chooser_;
};
}

std::unique_ptr<TextFilePanel> makeTextFilePanel(TextFilePanel::StatusCallback onStatus)
{
    return std::make_unique<TextFilePanelImpl>(std::move(onStatus));
}
}
