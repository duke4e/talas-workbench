#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchProjectLookup.h"

#include "talas/app/DiskButton.h"
#include "talas/dsp/SystemWavetables.h"

#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
// The file a file name field names: the project's file of that name, or
// one of the built-in wavetables.
std::optional<WorkbenchComponent::FieldFile> WorkbenchComponent::fieldFile(
    const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId) const
{
    const auto* node = nodeForContext(definitionId, nodeId);
    if (node == nullptr)
        return std::nullopt;
    std::string id;
    if (const auto* parameter = findParameter(*node, parameterId); parameter != nullptr && parameter->resource.has_value())
        id = *parameter->resource;
    else if (const auto* type = registry_.findNodeType(node->typeId))
    {
        const auto descriptor = std::ranges::find(type->parameters, parameterId, &talas::core::ParameterDescriptor::id);
        if (descriptor != type->parameters.end())
            id = descriptor->defaultResource;
    }
    if (id.empty())
        return std::nullopt;
    FieldFile file;
    file.id = id;
    const auto& assets = projectBridge_.editor().document().assets;
    if (const auto asset = std::ranges::find(assets, id, &talas::project::ProjectAsset::id); asset != assets.end())
    {
        file.asset = *asset;
        file.path = talas::audio::resolveProjectAssetPath(*asset, projectPath());
    }
    return file;
}

// Command or Control and the disk button: the sample editor on a sample
// file or wavetable, the spectrum editor on a spectrum file.
bool WorkbenchComponent::openFieldFileEditor(
    const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId)
{
    const auto file = fieldFile(definitionId, nodeId, parameterId);
    if (!file)
    {
        setStatus("The field names no file to open");
        return false;
    }
    if (!file->asset)
    {
        auto table = talas::dsp::systemTable(file->id);
        if (!table)
        {
            setStatus("The project holds no file " + juceString(file->id));
            return false;
        }
        showSampleEditor();
        sampleEditorPanel_->showSamples(juceString(file->id), {std::move(*table)}, 48000.0);
        return true;
    }
    switch (talas::app::fileEditorFor(talas::project::pathToUtf8(file->path.filename()), false))
    {
    case talas::app::FileEditor::Sample:
        showSampleEditor();
        return sampleEditorPanel_->openFile(file->path);
    case talas::app::FileEditor::Spectrum:
        if (file->asset->kind != talas::project::ProjectAssetKind::PartialSpectrum)
            break;
        // The Analysis workspace's editor shows the project's spectrum.
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
        setStatus("Editing the spectrum " + juceString(file->id));
        return true;
    case talas::app::FileEditor::None:
        break;
    }
    setStatus("There is no editor for " + juceString(file->id));
    return false;
}

// Shift and the disk button: what the file's header says.
std::optional<std::string> WorkbenchComponent::fieldFileInformation(
    const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId) const
{
    const auto file = fieldFile(definitionId, nodeId, parameterId);
    if (!file)
        return std::nullopt;
    if (!file->asset)
    {
        const auto table = talas::dsp::systemTable(file->id);
        return table ? std::optional<std::string>{talas::app::wavetableInformationText(file->id, table->size())}
                     : std::nullopt;
    }
    const auto name = talas::project::pathToUtf8(file->path.filename());
    switch (talas::app::fileEditorFor(name, false))
    {
    case talas::app::FileEditor::Sample:
    {
        const auto read = talas::audio::readSampleFile(file->path);
        if (!read.ok() || read.channels.empty())
            return name + "\nThe file could not be read: " + read.error;
        talas::app::SampleFileInformation information;
        information.name = name;
        information.channels = read.channels.size();
        information.sampleRate = read.sampleRate;
        information.frames = read.channels.front().size();
        if (read.forwardLoop)
            information.loop = std::pair{read.forwardLoop->startFrame, read.forwardLoop->endFrameExclusive};
        if (read.samplePitch)
            information.pitch = static_cast<double>(read.samplePitch->midiUnityNote)
                + static_cast<double>(read.samplePitch->midiPitchFraction) / 4294967296.0;
        return talas::app::sampleFileInformationText(information);
    }
    case talas::app::FileEditor::Spectrum:
    {
        std::ifstream stream{file->path, std::ios::binary};
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        auto decoded = talas::project::decodePartialTrackAsset(bytes);
        if (!decoded.ok())
            return name + "\nThe file could not be read";
        return talas::app::spectrumFileInformationText(name, decoded.asset);
    }
    case talas::app::FileEditor::None:
        break;
    }
    return name + "\n" + talas::project::pathToUtf8(file->path);
}

void WorkbenchComponent::showFieldFileInformation(
    const std::optional<std::string>& definitionId, std::string_view nodeId, std::string_view parameterId)
{
    const auto information = fieldFileInformation(definitionId, nodeId, parameterId);
    if (!information)
    {
        setStatus("The field names no file");
        return;
    }
    // More than an information window, it takes
    // typing and evaluates what is typed.
    showHelpWindow("Information", juceString(*information), true);
}

// A file's name typed into a file name field:
// a project file or wavetable of that name is used as it is; otherwise the
// file is looked for among the files
// and folders used recently and then the frequently used folders and the
// project's own folder, and bound to the field.
bool WorkbenchComponent::locateResourceForField(const std::optional<std::string>& definitionId,
    const std::string& nodeId, const std::string& parameterId, const std::string& name)
{
    const auto& assets = projectBridge_.editor().document().assets;
    if (name.empty() || std::ranges::any_of(assets, [&](const auto& asset) { return asset.id == name; })
        || talas::dsp::systemTable(name).has_value())
        return false;
    std::vector<std::filesystem::path> folders;
    for (const auto& folder : preferences_.frequentlyUsedFolders)
        folders.push_back(talas::project::pathFromUtf8(folder));
    if (projectPath().has_value())
        folders.push_back(projectPath()->parent_path());
    const auto outcome = locateFileLooking(name, folders, 1);
    const auto& located = outcome.located;
    if (talas::app::fileSearchStep(outcome, false) != talas::app::FileSearchStep::Use)
    {
        // After searching for some time, or looking
        // everywhere in vain, asked to keep looking, use a file list or
        // cancel.
        askToLocateFile(definitionId, nodeId, parameterId, name, false);
        return false;
    }
    bindResourceFile(definitionId, nodeId, parameterId, located->path);
    return true;
}

// While a file is looked for, the cursor is a pair of
// eyeglasses whose eyeballs move as it searches the folders.
talas::app::FileSearchOutcome WorkbenchComponent::locateFileLooking(
    std::string_view name, const std::vector<std::filesystem::path>& folders, std::size_t looks)
{
    // Some time's search before it asks.
    constexpr std::size_t entriesALook = 20000;
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    source.showMouseCursor(workbenchLocatingMouseCursor(0));
    auto outcome = talas::app::searchForFile(name, folders, fileLocatorMemory_, entriesALook * std::max<std::size_t>(1, looks),
        [&source](std::size_t searched) {
            source.showMouseCursor(workbenchLocatingMouseCursor(static_cast<int>(searched / 64)));
        });
    source.revealCursor();
    return outcome;
}

// A file no folder holds asks Keep looking, Use file dialog
// or Cancel; looked for again in vain, the file list opens. The smokes give
// their answer, or are not asked.
void WorkbenchComponent::askToLocateFile(const std::optional<std::string>& definitionId, const std::string& nodeId,
    const std::string& parameterId, const std::string& name, bool searchedAgain, std::size_t looks)
{
    using talas::app::FileSearchAnswer;
    const auto answered = [this, definitionId, nodeId, parameterId, name, searchedAgain, looks](FileSearchAnswer answer) {
        answer = talas::app::fileSearchAfterAnotherLook(searchedAgain, answer);
        if (answer == FileSearchAnswer::KeepLooking)
        {
            std::vector<std::filesystem::path> folders;
            for (const auto& folder : preferences_.frequentlyUsedFolders)
                folders.push_back(talas::project::pathFromUtf8(folder));
            if (projectPath().has_value())
                folders.push_back(projectPath()->parent_path());
            // Searching further, a while more each time it is asked.
            const auto outcome = locateFileLooking(name, folders, looks + 2);
            switch (talas::app::fileSearchStep(outcome, true))
            {
            case talas::app::FileSearchStep::Use:
                (void)bindResourceFile(definitionId, nodeId, parameterId, outcome.located->path);
                break;
            case talas::app::FileSearchStep::FileList:
                askToLocateFile(definitionId, nodeId, parameterId, name, true);
                break;
            case talas::app::FileSearchStep::Ask:
                askToLocateFile(definitionId, nodeId, parameterId, name, false, looks + 1);
                break;
            }
            return;
        }
        if (answer != FileSearchAnswer::UseFileDialog)
            return;
        locateFileChooser_ = std::make_unique<juce::FileChooser>("Locate " + juceString(name));
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        locateFileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, definitionId, nodeId, parameterId](const juce::FileChooser& chooser) {
                if (safeThis != nullptr && chooser.getResult() != juce::File{})
                    (void)safeThis->bindResourceFile(definitionId, nodeId, parameterId,
                        talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
            });
    };
    if (searchedAgain)
    {
        answered(FileSearchAnswer::KeepLooking);
        return;
    }
    if (smokeFileSearchAnswers_.has_value())
    {
        const auto answer = *smokeFileSearchAnswers_;
        smokeFileSearchAnswers_.reset();
        answered(answer);
        return;
    }
    if (!persistentStateEnabled_)
        return;
    const auto choices = talas::app::fileSearchChoices();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::QuestionIcon)
        .withTitle("Locate a file")
        .withMessage(juceString(talas::app::fileNotFoundQuestion(name)))
        .withButton(juceString(choices[0]))
        .withButton(juceString(choices[1]))
        .withButton(juceString(choices[2]))
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(options, [safeThis, answered](int choice) {
        if (safeThis != nullptr)
            answered(choice == 0 ? FileSearchAnswer::KeepLooking
                    : choice == 1 ? FileSearchAnswer::UseFileDialog
                                  : FileSearchAnswer::Cancel);
    });
}

// A Sound copied in the graph and pasted into a field: its name; one from
// elsewhere is pasted into this graph first. The
// field's edit then shows it in the graph.
std::optional<std::string> WorkbenchComponent::soundForPastedField()
{
    const auto parsed = talas::project::parseProjectGraphFragment(graphClipboardText().toStdString());
    const auto* graph = graphForContext(activeGraphContext_);
    if (!parsed.fragment.has_value() || parsed.fragment->nodes.empty() || graph == nullptr)
        return std::nullopt;
    const auto& first = parsed.fragment->nodes.front();
    if (std::ranges::any_of(graph->nodes, [&](const talas::project::ProjectNode& node) {
            return node.instanceId == first.instanceId && node.typeId == first.typeId;
        }))
        return first.instanceId;
    std::vector<std::string> pasted;
    const auto position = parsed.fragment->positions.empty() ? talas::project::CanvasPosition{}
                                                             : parsed.fragment->positions.front();
    if (!projectBridge_.editor()
             .pasteGraphFragment(optionalStringView(activeGraphContext_), *parsed.fragment, position, pasted)
             .ok()
        || pasted.empty())
        return std::nullopt;
    return pasted.front();
}
}
