#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavReader.h"

namespace talas::app::workbench
{
// The sample editor makes a new sample file,
// generates and changes segments with its templates, cuts, trims, draws,
// and saves a WAV the project plays.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSampleEditorSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "sample editor smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowSampleEditor;
    if (!listed || !commandManager_.invokeDirectly(commandShowSampleEditor, false) || sampleEditorPanel_ == nullptr)
        return finish("the Tools menu did not open the Sample Editor");
    auto& panel = *sampleEditorPanel_;
    auto& model = panel.model();
    // The graphic editor above, the generator templates below.
    if (!panel.templatesBelowGraphicEditorForSmoke())
        return finish("the Sample Editor's templates were not below its graphic editor");
    // A template's Program, its Smalltalk code changed to
    // an inharmonic partial and run, inserts the samples it answers.
    panel.newFile(48000.0, 1, 0.0);
    auto program = std::string{talas::app::sampleTemplateProgramSource(talas::app::SampleTemplateProgram::Fourier)};
    program.replace(program.find("#(1 3 5)"), 8, "#(1 2.5 4)");
    if (!panel.chooseProgramForSmoke(talas::app::SampleTemplateProgram::Fourier)
        || !panel.setTemplateFieldForSmoke("Duration", "512 samp") || !panel.setTemplateFieldForSmoke("Program", program)
        || !panel.insertTemplateForSmoke() || model.frames() != 512 || !(model.peak() > 0.99))
        return finish("the Fourier Program, changed and run, did not insert its samples");
    panel.newFile(48000.0, 1, 0.0);
    if (model.frames() != 0 || !panel.chooseTemplateForSmoke(talas::app::SampleTemplate::Fourier)
        || !panel.setTemplateFieldForSmoke("Duration", "4096 samp")
        || !panel.setTemplateFieldForSmoke("Harmonics", "1")
        || !panel.setTemplateFieldForSmoke("Amplitudes", "1")
        || !panel.setTemplateFieldForSmoke("Phases", "0")
        || !panel.insertTemplateForSmoke() || model.frames() != 4096)
        return finish("Insert did not put the Fourier template's cycle into the new file");
    // A drag in the detail view selects; Normalize Selection scales it.
    panel.dragForSmoke(0, 0.0, 2048, 0.0, false);
    if (model.selection() != talas::app::SampleSelection{0, 2048})
        return finish("a drag did not select the samples");
    if (!panel.chooseTemplateForSmoke(talas::app::SampleTemplate::NormalizeSelection)
        || !panel.setTemplateFieldForSmoke("Amplitude", "0.5") || !panel.insertTemplateForSmoke()
        || std::abs(model.peak() - 0.5) > 1e-9)
        return finish("Normalize Selection did not scale the selection");
    if (!panel.performForSmoke("Copy") || !panel.performForSmoke("Paste") || model.frames() != 4096
        || !panel.performForSmoke("Cut") || model.frames() != 2048 || !panel.performForSmoke("Undo")
        || model.frames() != 4096)
        return finish("Copy, Paste, Cut and Undo did not edit the selection");
    model.select(1024, 3072);
    if (!panel.performForSmoke("Trim") || model.frames() != 2048)
        return finish("Trim did not keep only the selection");
    panel.dragForSmoke(100, 0.25, 200, 0.25, true);
    if (std::abs(model.channels()[0][150] - 0.25) > 1e-6)
        return finish("the pencil did not draw the samples");
    const auto saved = smokeDirectory / "sample-editor.wav";
    const auto definitionsBefore = projectBridge_.editor().document().soundDefinitions.size();
    if (!panel.saveAndUseInProject(saved) || !std::filesystem::exists(saved) || model.modified())
        return finish("Save did not write the sample file");
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    if (definitions.size() != definitionsBefore + 1 || definitions.back().graph.nodes.empty()
        || definitions.back().graph.nodes.front().typeId != "talas.sample")
        return finish("Use in Project did not add a Sound playing the saved sample");
    const auto* resource = findParameter(definitions.back().graph.nodes.front(), "sample");
    if (resource == nullptr || !resource->resource.has_value() || resource->resource->empty())
        return finish("the saved sample's Sound has no sample bound");
    const auto read = talas::audio::readSampleFile(saved);
    if (!read.ok() || read.channels.size() != 1 || read.channels.front().size() != 2048
        || std::abs(read.channels.front()[150] - 0.25) > 1e-3)
        return finish("the saved WAV does not hold the edited samples");
    // The type, channels and bits chosen: a stereo AIFF of
    // 24 bits from the mono file.
    const auto aiff = smokeDirectory / "sample-editor.aif";
    if (!panel.saveFileAs(aiff, {talas::app::SampleFileType::Aiff, 2, 24}))
        return finish("Save did not write the sample as a 24-bit stereo AIFF");
    const auto readAiff = talas::audio::readSampleFile(aiff);
    std::error_code removed;
    std::filesystem::remove(aiff, removed);
    if (!readAiff.ok() || readAiff.channels.size() != 2 || readAiff.channels.back().size() != 2048
        || std::abs(readAiff.channels.back()[150] - 0.25) > 1e-4)
        return finish("the saved AIFF does not hold the samples in two channels");
    if (!panel.openFile(saved) || model.frames() != 2048)
        return finish("Open did not read the saved sample back");
    return finish();
}
}
