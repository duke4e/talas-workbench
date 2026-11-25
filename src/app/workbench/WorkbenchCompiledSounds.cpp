#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/CompiledSounds.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"

#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

namespace talas::app::workbench
{
namespace
{
// What the Sounds are compiled for: this Talas on this system.
talas::app::CompiledSoundTarget compiledSoundTarget()
{
    auto* application = juce::JUCEApplicationBase::getInstance();
    return {talas::project::currentProjectFormatVersion,
        application == nullptr ? std::string{"Talas"}
                               : (application->getApplicationName() + " " + application->getApplicationVersion()).toStdString(),
        juce::SystemStats::getOperatingSystemName().toStdString()};
}

std::optional<talas::app::CompiledSound> readCompiledSoundFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return talas::app::readCompiledSound(text);
}

// The compiled Sound into its file, its files' copies beside it.
std::string writeCompiledSoundFile(talas::app::CompiledSound compiled, const std::filesystem::path& file)
{
    std::error_code error;
    for (const auto& copy : talas::app::withFileCopies(compiled, file))
    {
        std::filesystem::create_directories(copy.to.parent_path(), error);
        std::filesystem::copy_file(copy.from, copy.to, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            return "a copy of " + talas::project::pathToUtf8(copy.from.filename()) + " could not be made";
    }
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << talas::app::writeCompiledSound(compiled);
    return output ? std::string{} : "the compiled Sound could not be written";
}
}

// The temp folder for a grid's compiled Sounds, one folder per project.
std::filesystem::path WorkbenchComponent::compiledGridFolder() const
{
    const auto& path = projectPath();
    const auto name = path.has_value() ? talas::project::pathToUtf8(path->stem()) : std::string{"Untitled"};
    std::ostringstream key;
    key << std::hex << std::hash<std::string>{}(path.has_value() ? talas::project::pathToUtf8(*path) : name);
    return filesystemPath(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName())
        / "Talas Compiled Sounds" / (name + " " + key.str());
}

void WorkbenchComponent::compileToDisk()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    // The grid in the Live Grid editor: its Sounds changed since they were
    // compiled, or all of them.
    if (liveGridEditorPanel_ != nullptr && liveGridEditorWindow_ != nullptr && liveGridEditorWindow_->isVisible()
        && !liveGridEditorPanel_->selectedLiveSetId().empty())
    {
        const auto liveSetId = liveGridEditorPanel_->selectedLiveSetId();
        auto* window = new juce::AlertWindow("Compile to disk",
            "Compile the grid's Sounds that changed since they were last compiled, or all of them.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addButton("Changed Sounds", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("All Sounds", 2);
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        window->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, liveSetId](int result) {
            if (safeThis != nullptr && result != 0)
                (void)safeThis->compileGridToDisk(liveSetId, result == 2);
        }),
            true);
        return;
    }
    // Compiling takes place within the variables
    // environment, asking first for the values it lacks.
    if (askForVariableValues([safeThis] {
            if (safeThis != nullptr)
                safeThis->compileToDisk();
        }, false))
        return;
    // Else the selected Sound, or the Sound being edited, into a file.
    std::optional<std::string> definitionId;
    if (selectedNodeIds_.size() == 1)
    {
        if (const auto* node = nodeForContext(activeGraphContext_, selectedNodeIds_.front());
            node != nullptr && node->soundDefinitionId.has_value())
            definitionId = node->soundDefinitionId;
    }
    if (!definitionId.has_value())
        definitionId = activeGraphContext_;
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    const auto definition = definitionId.has_value()
        ? std::ranges::find(definitions, *definitionId, &talas::project::ProjectSoundDefinition::definitionId)
        : definitions.end();
    if (definition == definitions.end())
    {
        setStatus("Select a Sound to compile to disk");
        return;
    }
    auto chooser = std::make_shared<juce::FileChooser>("Compile to disk",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(juceString(definition->displayName.empty() ? definition->definitionId : definition->displayName)
                + talas::app::compiledSoundExtension),
        juce::String{"*"} + talas::app::compiledSoundExtension);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser, id = *definitionId](const juce::FileChooser& chosen) {
            if (safeThis != nullptr && chosen.getResult() != juce::File{})
                (void)safeThis->compileSoundToDisk(id,
                    filesystemPath(chosen.getResult().withFileExtension(talas::app::compiledSoundExtension).getFullPathName()));
        });
}

bool WorkbenchComponent::compileSoundToDisk(const std::string& definitionId, const std::filesystem::path& file)
{
    // The environment's values go with it.
    const auto compiled = talas::app::compileSound(
        talas::project::withVariableEnvironment(projectBridge_.editor().document(), wholeVariableEnvironment()), definitionId,
        projectPath(), compiledSoundTarget());
    if (!compiled.ok())
    {
        setStatus("The Sound did not compile: " + juceString(compiled.error));
        return false;
    }
    if (const auto error = writeCompiledSoundFile(*compiled.compiled, file); !error.empty())
    {
        setStatus("Compile to disk failed: " + juceString(error));
        return false;
    }
    setStatus("Compiled " + juceString(compiled.compiled->name) + " to " + juceString(talas::project::pathToUtf8(file))
        + "; Play... in the File menu plays it");
    return true;
}

bool WorkbenchComponent::compileGridToDisk(const std::string& liveSetId, bool allSounds)
{
    const auto& document = projectBridge_.editor().document();
    const auto set = std::ranges::find(document.liveSetLibrary.liveSets, liveSetId, &talas::project::ProjectLiveSet::liveSetId);
    if (set == document.liveSetLibrary.liveSets.end())
        return false;
    const auto folder = compiledGridFolder();
    std::map<std::string, bool> current;
    std::map<std::string, talas::app::CompiledSound> fresh;
    std::size_t sounds = 0;
    for (const auto& cell : set->cells)
    {
        const auto* sound = std::get_if<talas::project::ProjectLiveSetSoundTarget>(&cell.target);
        if (sound == nullptr)
            continue;
        ++sounds;
        auto now = talas::app::compileSound(document, sound->soundDefinitionId, projectPath(), compiledSoundTarget(),
            sound->sourceInstanceId.has_value() ? std::optional<std::string_view>{*sound->sourceInstanceId}
                                                : std::optional<std::string_view>{});
        if (!now.ok())
        {
            setStatus("Box " + juceString(cell.displayName) + " did not compile: " + juceString(now.error));
            return false;
        }
        const auto kept = readCompiledSoundFile(talas::app::compiledGridSoundPath(folder, *set, cell));
        current[cell.cellId] = kept.has_value() && talas::app::compiledSoundCurrent(*kept, *now.compiled);
        fresh.emplace(cell.cellId, std::move(*now.compiled));
    }
    const auto cells = talas::app::gridCellsToCompile(*set, current, allSounds);
    for (const auto& cellId : cells)
    {
        const auto cell = std::ranges::find(set->cells, cellId, &talas::project::ProjectLiveSetCell::cellId);
        if (const auto error = writeCompiledSoundFile(fresh.at(cellId), talas::app::compiledGridSoundPath(folder, *set, *cell));
            !error.empty())
        {
            setStatus("Compile to disk failed: " + juceString(error));
            return false;
        }
    }
    setStatus(cells.empty() ? juce::String{"The grid's compiled Sounds are up to date"}
                            : "Compiled " + juce::String(static_cast<int>(cells.size())) + " of "
            + juce::String(static_cast<int>(sounds)) + " Sounds of " + juceString(set->displayName)
            + " to disk; check On to play them");
    return true;
}

bool WorkbenchComponent::playCompiledSoundFile(const std::filesystem::path& path)
{
    const auto compiled = readCompiledSoundFile(path);
    if (!compiled.has_value())
    {
        setStatus("The file is not a compiled Sound Talas plays");
        return false;
    }
    const auto folder = filesystemPath(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName())
        / "talas-organizer";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto soundFile = folder / (talas::project::pathToUtf8(path.stem()) + ".talas.json");
    {
        std::ofstream output(soundFile, std::ios::binary | std::ios::trunc);
        output << talas::project::serializeProjectDocument(compiled->document);
        if (!output)
        {
            setStatus("The compiled Sound could not be readied to play");
            return false;
        }
    }
    return playFromSoundBrowser(
        {soundFile, compiled->soundDefinitionId, compiled->name, 1, talas::app::SoundBrowserKind::Sound, false, false});
}

// On: the grid's Sounds play their compiled copies,
// which do not follow later changes to the Sounds until compiled again.
bool WorkbenchComponent::setLiveGridOn(const std::string& liveSetId, bool on)
{
    if (!on)
    {
        projectBridge_.setCompiledLiveSetSounds({});
        currentDocumentStaged_ = false;
        stopCurrentProject();
        setStatus("The grid is off; double-click a box to edit its Sound");
        return true;
    }
    const auto& document = projectBridge_.editor().document();
    const auto set = std::ranges::find(document.liveSetLibrary.liveSets, liveSetId, &talas::project::ProjectLiveSet::liveSetId);
    if (set == document.liveSetLibrary.liveSets.end())
        return false;
    std::unordered_map<std::string, talas::project::ProjectDocument> programs;
    for (const auto& cell : set->cells)
    {
        const auto* sound = std::get_if<talas::project::ProjectLiveSetSoundTarget>(&cell.target);
        if (sound == nullptr)
            continue;
        auto compiled = readCompiledSoundFile(talas::app::compiledGridSoundPath(compiledGridFolder(), *set, cell));
        if (!compiled.has_value())
        {
            setStatus("Compile the grid to disk first (Compile to Disk... in the Action menu)");
            return false;
        }
        programs.insert_or_assign(sound->soundDefinitionId, std::move(compiled->document));
    }
    projectBridge_.setCompiledLiveSetSounds(std::move(programs));
    currentDocumentStaged_ = false;
    playCurrentProject();
    setStatus("The grid plays its compiled Sounds: click a box or send its program change to start it");
    return true;
}

bool WorkbenchComponent::playToolGridSound(const std::string& grid, std::size_t number, const std::string& name, bool compile)
{
    if (compile)
    {
        setStatus("The Tool asked the grid to compile a Sound with substitutions; Talas plays a grid's compiled Sounds");
        return false;
    }
    const auto box = talas::app::toolGridBox(projectBridge_.editor().document(), grid, number, name);
    if (!box.has_value())
    {
        setStatus("The Tool's grid has no such Sound");
        return false;
    }
    const auto file = talas::app::compiledGridSoundPath(compiledGridFolder(), *box->set, *box->cell);
    std::error_code error;
    if (!std::filesystem::exists(file, error))
    {
        setStatus("The Sound grid must be precompiled: Compile to Disk... in the Action menu");
        return false;
    }
    return playCompiledSoundFile(file);
}
}
