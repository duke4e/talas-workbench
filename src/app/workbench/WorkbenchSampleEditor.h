#pragma once

#include "talas/app/SampleEditorModel.h"
#include "talas/app/SampleTemplatePrograms.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// The sample editor: the whole file above, the
// visible part below where a drag selects (or the pencil draws), the Edit
// operations, and the generator templates; Save writes a WAV and Use in
// Project adds a Sound that plays it.
class SampleEditorPanel : public juce::Component
{
public:
    using StatusCallback = std::function<void(const juce::String&)>;
    using UseInProjectCallback = std::function<void(const std::filesystem::path&)>;
    // Play: the selection's channels (or the file's) at their sample rate.
    using PlayCallback = std::function<void(std::vector<std::vector<double>>, double)>;
    // Cut or Copy put samples on the clipboard, copied from the file given
    // (pasted in a Timeline as a clip).
    using CopiedCallback = std::function<void(const std::filesystem::path&)>;

    ~SampleEditorPanel() override = default;

    [[nodiscard]] virtual talas::app::SampleEditorModel& model() noexcept = 0;
    virtual void newFile(double sampleRate, std::size_t channels, double seconds) = 0;
    [[nodiscard]] virtual bool openFile(const std::filesystem::path& path) = 0;
    // Samples that are no file, as one of the built-in wavetables, shown as a new
    // file of that name.
    virtual void showSamples(const juce::String& name, std::vector<std::vector<double>> channels, double sampleRate) = 0;
    [[nodiscard]] virtual bool saveFile(const std::filesystem::path& path) = 0;
    // The file written as the type, channels and bits chosen.
    [[nodiscard]] virtual bool saveFileAs(const std::filesystem::path& path, talas::app::SampleFileFormat format) = 0;
    // Save..., asking the format and then the file.
    virtual void startSave() = 0;
    // Save, then the project's Sound plays the file.
    [[nodiscard]] virtual bool saveAndUseInProject(const std::filesystem::path& path) = 0;
    // The template menu and its fields, then Insert.
    [[nodiscard]] virtual bool chooseTemplateForSmoke(talas::app::SampleTemplate kind) = 0;
    [[nodiscard]] virtual bool setTemplateFieldForSmoke(const std::string& name, const std::string& text) = 0;
    // A template Program chosen from the list.
    [[nodiscard]] virtual bool chooseProgramForSmoke(talas::app::SampleTemplateProgram program) = 0;
    [[nodiscard]] virtual bool insertTemplateForSmoke() = 0;
    // The buttons and the Edit operations, as a click does them.
    [[nodiscard]] virtual bool performForSmoke(const juce::String& action) = 0;
    // A drag in the detail view from one frame to another (pencil off:
    // the selection; on: a line of the values given).
    virtual void dragForSmoke(std::size_t fromFrame, double fromValue, std::size_t toFrame, double toValue,
        bool pencil) = 0;
    // The numbers beside the overview, a click on them switching units, a
    // file pasted over the selection and the Lissajous display.
    [[nodiscard]] virtual juce::String readoutForSmoke() const = 0;
    virtual void switchReadoutUnitsForSmoke() = 0;
    [[nodiscard]] virtual bool pasteFileForSmoke(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual std::size_t lissajousPointsForSmoke(std::size_t delay) = 0;
    // The graphic editor above, the templates below it.
    [[nodiscard]] virtual bool templatesBelowGraphicEditorForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<SampleEditorPanel> makeSampleEditorPanel(
    SampleEditorPanel::StatusCallback onStatus,
    SampleEditorPanel::UseInProjectCallback onUseInProject,
    SampleEditorPanel::PlayCallback onPlay = {},
    SampleEditorPanel::CopiedCallback onCopied = {});
}
