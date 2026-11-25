#include "app/workbench/WorkbenchComponentInternal.h"

#include "app/workbench/WorkbenchLiveGridEditor.h"
#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavReader.h"
#include "talas/project/ToolFile.h"

#include <fstream>
#include <iterator>

// Tools: the Tool editor on a Tool file,
// saving it, and the Tool window running it, its Event Values written to
// the project playing and its Sound requests to the transport.
namespace talas::app::workbench
{
void WorkbenchComponent::showToolEditor(talas::project::tool::Definition tool, std::optional<std::filesystem::path> path)
{
    if (toolEditorPanel_ == nullptr)
    {
        ToolEditorHost host;
        host.run = [this](const talas::project::tool::Definition& running) { runTool(running); };
        host.save = [this](const talas::project::tool::Definition& saved, bool saveAs) { saveToolFile(saved, saveAs); };
        host.askName = [this](juce::String title, std::function<void(std::string)> named) {
            soundDialog_ = std::make_unique<juce::AlertWindow>("Tool editor", title, juce::MessageBoxIconType::NoIcon, this);
            soundDialog_->addTextEditor("name", {}, "Name");
            soundDialog_->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            soundDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
            soundDialog_->enterModalState(true,
                juce::ModalCallbackFunction::create([safeThis, named = std::move(named)](int result) {
                    if (safeThis == nullptr || safeThis->soundDialog_ == nullptr)
                        return;
                    const auto name = safeThis->soundDialog_->getTextEditorContents("name").trim().toStdString();
                    safeThis->soundDialog_.reset();
                    if (result == 1 && !name.empty())
                        named(name);
                }),
                false);
        };
        host.status = [this](const juce::String& status) { setStatus(status); };
        toolEditorPanel_ = makeToolEditorPanel(std::move(host));
    }
    toolEditorPanel_->setTool(std::move(tool));
    toolPath_ = std::move(path);
    if (isShowing())
    {
        if (toolEditorWindow_ == nullptr)
            toolEditorWindow_ = makeToolWindow("Tool editor", *toolEditorPanel_);
        toolEditorWindow_->setName("Tool editor: " + (toolPath_.has_value() ? jucePathString(toolPath_->filename()) : juce::String{"Untitled"}));
        toolEditorWindow_->setVisible(true);
        toolEditorWindow_->toFront(true);
    }
    setStatus("Tool editor: add variables, states and triggered responses, then Run");
}

// File > Open with Edit tool as the file type, or Use tool to run
// the Tool saved without its editor.
void WorkbenchComponent::openToolFile(bool use)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    fileChooser_ = std::make_unique<juce::FileChooser>(use ? "Use Tool" : "Open Tool", juce::File{},
        "*" + juce::String{std::string{talas::project::tool::toolFileExtension}});
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, use](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            if (chooser.getResult() != juce::File{})
                (void)safeThis->openToolAt(filesystemPath(chooser.getResult().getFullPathName()), use);
        });
}

// Tools placed in a Tools folder of the frequently used folders are on the
// Tools menu, each run as Use tool runs it.
void WorkbenchComponent::appendSavedToolsMenu(juce::PopupMenu& menu)
{
    savedTools_ = talas::app::toolsInToolsFolders(fileOrganizerRoots());
    if (savedTools_.empty())
        return;
    menu.addSeparator();
    for (std::size_t index = 0; index < savedTools_.size() && index < 256; ++index)
        menu.addItem(savedToolMenuItemBase + static_cast<int>(index), jucePathString(savedTools_[index].stem()));
}

bool WorkbenchComponent::selectSavedTool(int menuItemId)
{
    const auto index = menuItemId - savedToolMenuItemBase;
    if (index < 0 || static_cast<std::size_t>(index) >= savedTools_.size() || index >= 256)
        return false;
    (void)openToolAt(savedTools_[static_cast<std::size_t>(index)], true);
    return true;
}

bool WorkbenchComponent::openToolAt(const std::filesystem::path& path, bool use)
{
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto read = talas::project::tool::readToolFile(text);
    if (!read.ok())
    {
        setStatus("The Tool could not be read: " + juceString(read.error));
        return false;
    }
    if (use)
        runTool(*read.tool);
    else
        showToolEditor(std::move(*read.tool), path);
    return true;
}

void WorkbenchComponent::saveToolFile(const talas::project::tool::Definition& tool, bool saveAs)
{
    const auto write = [this, tool](const std::filesystem::path& path) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << talas::project::tool::writeToolFile(tool);
        if (!output)
        {
            setStatus("The Tool could not be saved");
            return;
        }
        toolPath_ = path;
        if (toolEditorWindow_ != nullptr)
            toolEditorWindow_->setName("Tool editor: " + jucePathString(path.filename()));
        setStatus("Saved the Tool " + jucePathString(path.filename()));
    };
    if (!saveAs && toolPath_.has_value())
    {
        write(*toolPath_);
        return;
    }
    if (fileChooserActive_)
        return;
    fileChooser_ = std::make_unique<juce::FileChooser>("Save Tool", juce::File{}, "*" + juce::String{std::string{talas::project::tool::toolFileExtension}});
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, write](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            if (chooser.getResult() == juce::File{})
                return;
            auto file = chooser.getResult();
            if (file.getFileExtension().isEmpty())
                file = file.withFileExtension(juce::String{std::string{talas::project::tool::toolFileExtension}});
            write(filesystemPath(file.getFullPathName()));
        });
}

// The Run button: the Tool window opens and the Tool starts.
void WorkbenchComponent::runTool(const talas::project::tool::Definition& tool)
{
    if (toolRunPanel_ == nullptr)
    {
        ToolRunHost host;
        host.writeEventValue = [this](const talas::project::tool::EventWrite& write) {
            // A Sound playing that reads the Event Value hears it.
            if (currentDocumentStaged_)
                (void)projectBridge_.updateEventValue(write.name, write.value, talas::expr::ValueDomain::Scalar);
        };
        host.sound = [this](const talas::project::tool::SoundRequest& request) {
            if (request.stop)
                stopCurrentProject();
            else if (request.replay)
                restartCurrentProject();
            else if (request.pause)
                pauseCurrentProject();
            else if (request.resume)
                playCurrentProject();
            else if (request.openControllerSurface)
            {
                // The virtual control surface, if the Sound
                // loaded has controls on it.
                if (currentDocumentStaged_ && !projectBridge_.editor().document().virtualControls.empty()
                    && (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis
                        || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance))
                    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
            }
            else if (!request.grid.empty())
                (void)playToolGridSound(request.grid, request.number, request.name, request.compile);
            else if (!request.sampleFile.empty())
            {
                // A SamplesFile's play, as File > Play... plays it.
                const auto& assets = projectBridge_.editor().document().assets;
                const auto asset = std::ranges::find(assets, request.sampleFile, &talas::project::ProjectAsset::id);
                if (asset == assets.end() || !playFileAtPath(talas::audio::resolveProjectAssetPath(*asset, projectPath())))
                    setStatus("The Tool's sample " + juceString(request.sampleFile) + " could not be played");
            }
        };
        // The project's sample files, by id, the id's last
        // part, or that without its extension, as a script's are.
        host.sampleFile = [this](std::string_view name) -> std::optional<talas::project::smalltalk::SampleFileInfo> {
            const auto stem = [](std::string_view file) {
                const auto dot = file.find_last_of('.');
                return dot == std::string_view::npos ? file : file.substr(0, dot);
            };
            for (const auto& asset : projectBridge_.editor().document().assets)
            {
                const std::string_view id = asset.id;
                const auto file = id.substr(id.find_last_of('/') + 1);
                if (asset.kind != talas::project::ProjectAssetKind::Table
                    || (id != name && file != name && stem(file) != stem(name)))
                    continue;
                auto read = std::make_shared<talas::audio::WavReadResult>(
                    talas::audio::readSampleFile(talas::audio::resolveProjectAssetPath(asset, projectPath())));
                if (!read->ok() || read->channels.empty())
                    return std::nullopt;
                talas::project::smalltalk::SampleFileInfo info;
                info.name = asset.id;
                info.frames = static_cast<double>(read->channels.front().size());
                info.sampleRate = static_cast<double>(read->sampleRate);
                info.loop = read->forwardLoop;
                info.pitch = read->samplePitch;
                info.keys = read->keyRange;
                info.markers = read->markers;
                info.monoSamples = [read](std::size_t start, std::size_t length) -> std::optional<std::vector<double>> {
                    const auto& samples = read->channels.front();
                    if (read->channels.size() != 1 || start > samples.size() || length > samples.size() - start)
                        return std::nullopt;
                    return std::vector<double>(samples.begin() + static_cast<std::ptrdiff_t>(start),
                        samples.begin() + static_cast<std::ptrdiff_t>(start + length));
                };
                return info;
            }
            return std::nullopt;
        };
        host.status = [this](const juce::String& status) { setStatus(status); };
        host.displayableAudioChannels = [this] { return std::size_t{preferences_.showEightChannels ? 8U : 2U}; };
        toolRunPanel_ = makeToolRunPanel(std::move(host));
    }
    if (!toolRunPanel_->run(tool))
        return;
    if (isShowing())
    {
        if (toolRunWindow_ == nullptr)
            toolRunWindow_ = makeToolWindow("Tool", *toolRunPanel_);
        toolRunWindow_->setName(toolRunPanel_->title());
        toolRunWindow_->setResizable(!toolRunPanel_->fixedSize(), false);
        toolRunWindow_->setVisible(true);
        toolRunWindow_->toFront(true);
    }
}
}
