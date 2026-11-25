#include "app/workbench/WorkbenchComponentInternal.h"

#include "app/workbench/WorkbenchLiveGridEditor.h"
#include "talas/app/SoundBrowserAudition.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/TimelineInfo.h"
#include "talas/audio/WavReader.h"
#include "talas/project/ProjectJson.h"

#include <fstream>
#include <iterator>
#include <thread>

// The Sound Browser: its windows, one for every
// folder spawned from it, what it plays, the one replacement input they
// share, and the Sounds dragged from it into the project.
namespace talas::app::workbench
{
namespace
{
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundFileDocument(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto loaded = talas::project::parseProjectDocument(text);
    if (!loaded.ok())
        return std::nullopt;
    return std::move(loaded.document);
}
}

// A Sound file's Sounds share its surface, so a Sound's widgets are its
// file's.
bool WorkbenchComponent::copyWidgetsFromSoundBrowser(const talas::app::SoundBrowserRow& row, bool toSystemClipboard)
{
    const auto document = row.kind == talas::app::SoundBrowserKind::Sound || row.kind == talas::app::SoundBrowserKind::SoundFile
        ? soundFileDocument(row.path)
        : std::nullopt;
    if (!document.has_value() || document->virtualControls.empty())
    {
        setStatus("Only a Sound with controls has widgets to copy");
        return false;
    }
    auto widgets = document->virtualControls;
    for (auto& widget : widgets)
    {
        widget.midiMapping.reset();
        widget.oscMapping.reset();
        widget.oscEgressMapping.reset();
    }
    setStatus("Copied the widgets of " + juceString(row.name) + "; paste them into an unlocked surface");
    copyVirtualControlWidgets(std::move(widgets), toSystemClipboard);
    return true;
}

void WorkbenchComponent::showSoundBrowser(std::optional<std::filesystem::path> folder)
{
    SoundBrowserHost host;
    host.play = [this](const talas::app::SoundBrowserRow& row) { (void) playFromSoundBrowser(row); };
    host.playOnce = [this](const talas::app::SoundBrowserRow& row) { (void) playFromSoundBrowser(row, true); };
    host.pauseOrResume = [this] { pauseOrResumeSoundBrowser(); };
    host.stop = [this] { stopSoundBrowser(); };
    host.replay = [this] {
        if (soundBrowserLastPlayed_.has_value())
            (void) playFromSoundBrowser(*soundBrowserLastPlayed_);
    };
    // A double-click on a Sound opens an editor on it.
    host.addToProject = [this](const talas::app::SoundBrowserRow& row) {
        if (const auto added = addSoundFromBrowser(row))
            showSoundDefinition(*added);
    };
    host.open = [this](const talas::app::SoundBrowserRow& row) { (void) openTalasFile(row.path); };
    // The project's own file shows its Sound file window;
    // another opens as the project (asking first about unsaved changes),
    // its Sound file window then showing.
    host.openSoundFileWindow = [this](const std::filesystem::path& path) {
        std::error_code error;
        if (projectPath().has_value() && std::filesystem::equivalent(*projectPath(), path, error))
        {
            showSoundFileWindow();
            return;
        }
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        requestAfterDiscardConfirmation([safeThis, path] {
            if (safeThis == nullptr)
                return;
            safeThis->loadProjectFromPath(path);
            safeThis->showSoundFileWindow();
        });
    };
    host.copy = [this](const talas::app::SoundBrowserRow& row) {
        // The Sound, for a Sound file window's Paste, as well
        // as its widgets.
        if (row.kind == talas::app::SoundBrowserKind::Sound)
            setGraphClipboardText(juceString(talas::app::soundBrowserDragDescription(row)));
        (void) copyWidgetsFromSoundBrowser(row);
    };
    host.spawn = [this](const std::filesystem::path& path) { showSoundBrowser(path); };
    host.input = [this] { return soundBrowserInput_; };
    host.setInput = [this](talas::app::SoundBrowserReplacementInput input) {
        soundBrowserInput_ = std::move(input);
        for (auto& browser : soundBrowsers_)
            browser.panel->showInput();
    };
    host.audioInputCount = [this] {
        auto* device = deviceManager_.getCurrentAudioDevice();
        const auto count = device == nullptr ? 0 : device->getActiveInputChannels().countNumberOfSetBits();
        return static_cast<unsigned>(std::clamp(count, 2, 8));
    };
    // The main browser is kept and shown again; a spawned one is added.
    if (!folder.has_value() && !soundBrowsers_.empty() && !soundBrowsers_.front().folder.has_value())
    {
        soundBrowsers_.front().panel->setRoots(fileOrganizerRoots());
        if (soundBrowsers_.front().window != nullptr)
        {
            soundBrowsers_.front().window->setVisible(true);
            soundBrowsers_.front().window->toFront(true);
        }
        return;
    }
    SoundBrowserWindow browser;
    browser.panel = makeSoundBrowserPanel(std::move(host));
    browser.panel->setRoots(folder.has_value() ? std::vector<std::filesystem::path>{*folder} : fileOrganizerRoots());
    const auto title = juceString(talas::app::soundBrowserTitle(folder));
    if (isShowing())
    {
        browser.window = makeToolWindow(title, *browser.panel);
        browser.window->setVisible(true);
        browser.window->toFront(true);
    }
    browser.folder = std::move(folder);
    soundBrowsers_.push_back(std::move(browser));
    setStatus("Sound Browser: select a Sound or file and press Play or Space");
}

bool WorkbenchComponent::playFromSoundBrowser(const talas::app::SoundBrowserRow& row, bool once)
{
    soundBrowserLastPlayed_ = row;
    stopSoundBrowser();
    if (row.kind == talas::app::SoundBrowserKind::Sample || row.kind == talas::app::SoundBrowserKind::Ga)
    {
        auto read = talas::audio::readSampleFile(row.path);
        if (!read.ok() || read.channels.empty() || read.channels.front().empty())
        {
            setStatus("The sample could not be read");
            return false;
        }
        // Samples shorter than 30 seconds loop, unless Play was
        // clicked with Control held.
        const auto seconds = static_cast<double>(read.channels.front().size()) / read.sampleRate;
        const auto loops = talas::app::soundBrowserLoops(seconds, once);
        playSampleBuffer(std::move(read.channels), read.sampleRate, loops);
        setStatus(juceString("Playing " + row.name) + (loops ? " · looping" : " · once"));
        return true;
    }
    // An analysis or MIDI file through the Sound that
    // plays it, a Sound file's Timeline as a Sound of that file.
    const auto player = talas::app::soundBrowserPlayer(row.kind);
    if (player == talas::app::SoundBrowserPlayer::FileSound)
        return playOrganizerFile(row.path);
    if (player == talas::app::SoundBrowserPlayer::Timeline)
    {
        auto timeline = soundFileDocument(row.path);
        const auto sound = timeline.has_value() ? talas::app::timelineAsSound(*timeline, row.name + " Timeline") : std::nullopt;
        if (!sound.has_value())
        {
            setStatus("The Sound file holds no Timeline to play");
            return false;
        }
        timeline->soundDefinitions.push_back(*sound);
        auto audition = talas::app::soundBrowserAuditionDocument(*timeline, sound->definitionId, soundBrowserInput_, std::nullopt, registry_);
        if (!audition.ok())
        {
            setStatus("The Timeline cannot play: " + juceString(audition.diagnostic));
            return false;
        }
        if (!startSoundBrowserAudition(std::move(*audition.document), row.path, audition.outputChannelCount))
            return false;
        setStatus(juceString("Playing the Timeline of " + row.name));
        return true;
    }
    if (row.kind != talas::app::SoundBrowserKind::Sound)
    {
        setStatus("Select a Sound or a sample to play it in the browser");
        return false;
    }
    const auto document = soundFileDocument(row.path);
    if (!document.has_value())
    {
        setStatus("The Sound file could not be read");
        return false;
    }
    const auto inputFile = soundBrowserInput_.kind == talas::app::SoundBrowserInputKind::Sound
        ? soundFileDocument(soundBrowserInput_.path)
        : std::optional<talas::project::ProjectDocument>{};
    auto audition = talas::app::soundBrowserAuditionDocument(*document, row.definitionId, soundBrowserInput_, inputFile, registry_);
    if (!audition.ok())
    {
        setStatus("The Sound cannot play: " + juceString(audition.diagnostic));
        return false;
    }
    if (!startSoundBrowserAudition(std::move(*audition.document), row.path, audition.outputChannelCount))
        return false;
    setStatus(juceString("Playing " + row.name)
        + (soundBrowserInput_.connected ? juceString(" through " + talas::app::soundBrowserInputText(soundBrowserInput_)) : ""));
    showSoundBrowserSurface(*document, row);
    return true;
}

bool WorkbenchComponent::startSoundBrowserAudition(talas::project::ProjectDocument document, const std::filesystem::path& sourcePath,
    std::size_t outputs, std::vector<std::pair<std::string, double>> variables)
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    const auto prepared = soundBrowserUsesPreparedProfileForSmoke_;
    if (!prepared && (device == nullptr || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0))
    {
        setStatus("Playing in the Sound Browser requires an active audio device");
        return false;
    }
    talas::runtime::RenderSettings settings;
    settings.sampleRate = prepared ? 8000.0 : device->getCurrentSampleRate();
    settings.maxBlockFrames = prepared ? 64 : static_cast<std::size_t>(device->getCurrentBufferSizeSamples());
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(document, registry_, sourcePath, {}, [] { return false; });
    if (!loaded.ok())
    {
        setStatus("The Sound's files could not be read: "
            + juceString(loaded.failure.has_value() ? loaded.failure->message : std::string{"asset load failed"}));
        return false;
    }
    settings.resourceSnapshots = std::move(loaded.snapshots);
    settings.variableEnvironment = std::move(variables);
    auto audition = std::make_unique<SoundBrowserAudition>();
    audition->document = std::move(document);
    audition->outputs = outputs;
    audition->bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(audition->document, registry_);
    const auto staged = audition->bridge->compileAndStageDocument(audition->document, std::move(settings));
    if (!staged.ok())
    {
        setStatus("The Sound did not compile: " + juceString(staged.message));
        return false;
    }
    audition->bridge->setPlaying(true);
    ++soundBrowserAuditionGeneration_;
    soundBrowserAudition_ = std::move(audition);
    soundBrowserAuditionLive_.store(soundBrowserAudition_.get(), std::memory_order_release);
    return true;
}

void WorkbenchComponent::stopSoundBrowserAudition() noexcept
{
    (void) soundBrowserAuditionLive_.exchange(nullptr, std::memory_order_acq_rel);
    while (soundBrowserAuditionReaders_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    soundBrowserAudition_.reset();
}

void WorkbenchComponent::forwardMidiNoteToAudition(
    bool on, int channel, std::uint8_t note, std::uint8_t velocity, double arrivalTimeSeconds) noexcept
{
    soundBrowserAuditionReaders_.fetch_add(1, std::memory_order_acq_rel);
    if (auto* audition = soundBrowserAuditionLive_.load(std::memory_order_acquire); audition != nullptr)
    {
        (void)(on ? audition->bridge->enqueueMidiNoteOn(channel, note, velocity, arrivalTimeSeconds)
                  : audition->bridge->enqueueMidiNoteOff(channel, note, arrivalTimeSeconds));
    }
    soundBrowserAuditionReaders_.fetch_sub(1, std::memory_order_acq_rel);
}

void WorkbenchComponent::stopSoundBrowser()
{
    stopSoundBrowserAudition();
    stopSampleBuffer();
    sampleAuditionPaused_.store(false, std::memory_order_relaxed);
}

// Space pauses what the browser plays and resumes it.
void WorkbenchComponent::pauseOrResumeSoundBrowser()
{
    if (soundBrowserAudition_ != nullptr)
    {
        soundBrowserAudition_->playing = !soundBrowserAudition_->playing;
        soundBrowserAudition_->bridge->setPlaying(soundBrowserAudition_->playing);
        setStatus(soundBrowserAudition_->playing ? "Resumed" : "Paused");
        return;
    }
    if (sampleAuditionOwned_ != nullptr)
    {
        const auto paused = !sampleAuditionPaused_.load(std::memory_order_relaxed);
        sampleAuditionPaused_.store(paused, std::memory_order_relaxed);
        setStatus(paused ? "Paused" : "Resumed");
        return;
    }
    if (soundBrowserLastPlayed_.has_value())
        (void) playFromSoundBrowser(*soundBrowserLastPlayed_);
}

// A Sound dragged from the browser into the project: the Sound, or with the
// replacement input connected the two kept together as a new Sound.
std::optional<std::string> WorkbenchComponent::addSoundFromBrowser(const talas::app::SoundBrowserRow& row)
{
    const auto document = soundFileDocument(row.path);
    if (row.kind != talas::app::SoundBrowserKind::Sound || !document.has_value())
    {
        setStatus("Select a Sound to bring it into the project");
        return std::nullopt;
    }
    const auto inputFile = soundBrowserInput_.kind == talas::app::SoundBrowserInputKind::Sound
        ? soundFileDocument(soundBrowserInput_.path)
        : std::optional<talas::project::ProjectDocument>{};
    auto imported = talas::app::soundBrowserImport(
        projectBridge_.editor().document(), *document, row.definitionId, soundBrowserInput_, inputFile, registry_);
    if (!imported.ok())
    {
        setStatus("The Sound could not be brought in: " + juceString(imported.diagnostic));
        return std::nullopt;
    }
    if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(*imported.document)); !applied.ok())
    {
        setStatus("The Sound was refused: " + juceString(applied.message));
        return std::nullopt;
    }
    documentChanged();
    setStatus("Brought " + juceString(row.name) + " into the project's Sounds");
    return imported.rootDefinitionId;
}

std::optional<std::string> WorkbenchComponent::importBrowserDrag(const std::string& description)
{
    // A Sound dragged out of
    // a signal flow leaves a copy of itself.
    if (const auto flow = talas::app::signalFlowSoundDrag(description))
    {
        auto copy = talas::app::soundCopiedFromSignalFlow(projectBridge_.editor().document(), registry_, *flow);
        if (!copy.has_value())
        {
            setStatus("The Sound has no audio output to make a copy of");
            return std::nullopt;
        }
        auto document = projectBridge_.editor().document();
        const auto definitionId = copy->definition.definitionId;
        document.soundDefinitions.push_back(std::move(copy->definition));
        document.presentation.structureLayouts.push_back(std::move(copy->layout));
        talas::project::reconcileProjectPresentation(document);
        if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(document)); !applied.ok())
        {
            setStatus("The copy was refused: " + juceString(applied.message));
            return std::nullopt;
        }
        documentChanged();
        setStatus("Saved a copy of " + juceString(definitionId) + " among the project's Sounds");
        return definitionId;
    }
    const auto drag = talas::app::soundBrowserDrag(description);
    if (!drag.has_value())
        return std::nullopt;
    if (drag->definitionId.empty())
        return addSoundForTalasFile(drag->path);
    const auto document = soundFileDocument(drag->path);
    const auto sounds = document.has_value() ? talas::app::soundBrowserSounds(*document) : std::vector<talas::app::SoundBrowserSound>{};
    const auto sound = std::ranges::find(sounds, drag->definitionId, &talas::app::SoundBrowserSound::definitionId);
    return addSoundFromBrowser({drag->path, drag->definitionId, sound == sounds.end() ? drag->definitionId : sound->name, 1,
        talas::app::SoundBrowserKind::Sound});
}

void WorkbenchComponent::dropDraggedItemAt(std::optional<std::string> definitionId, GraphDropItem item,
    talas::project::CanvasPosition position, GraphDropTarget target)
{
    // With Control or Option held
    // a Sound of this signal flow dropped on another takes its place
    // itself, not a copy.
    if (const auto flow = talas::app::signalFlowSoundDrag(item.browserDrag);
        flow.has_value() && target.shared && flow->scope == definitionId && !target.nodeId.empty()
        && !target.connection.has_value())
    {
        auto shared = talas::app::soundSharedInPlaceOf(projectBridge_.editor().document(), registry_, *flow, target.nodeId);
        if (!shared.has_value())
        {
            setStatus("The Sound cannot take the place of " + juceString(target.nodeId) + " itself");
            return;
        }
        if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(*shared)); !applied.ok())
        {
            setStatus("The Sound was refused: " + juceString(applied.message));
            return;
        }
        documentChanged();
        selectNode(definitionId, flow->nodeId);
        setStatus("Replaced " + juceString(target.nodeId) + " with " + juceString(flow->nodeId) + " itself, not a copy");
        return;
    }
    if (!item.browserDrag.empty())
    {
        const auto imported = importBrowserDrag(item.browserDrag);
        if (!imported.has_value())
            return;
        item = GraphDropItem{{}, *imported};
    }
    if (target.mix && target.connection.has_value())
    {
        mixDroppedItemIntoLine(std::move(definitionId), std::move(item), position, *target.connection);
        return;
    }
    dropItemAt(std::move(definitionId), std::move(item), position, std::move(target));
}

void WorkbenchComponent::mixDroppedItemIntoLine(std::optional<std::string> definitionId, GraphDropItem item,
    talas::project::CanvasPosition position, const talas::core::Connection& line)
{
    std::optional<talas::project::ProjectNode> node;
    if (!item.prototypeTypeId.empty())
        node = makePrototypeNode(definitionId, item.prototypeTypeId);
    else
        node = talas::project::ProjectNode{nextSoundInstanceId(definitionId, item.soundDefinitionId), {}, {}, item.soundDefinitionId, {}};
    auto mixer = makePrototypeNode(definitionId, "talas.mixerV2");
    if (!node.has_value() || !mixer.has_value())
    {
        setStatus("The dropped Sound could not be mixed into the line");
        return;
    }
    // The Mixer's name and the dropped Sound's are both fresh.
    while (mixer->instanceId == node->instanceId)
        mixer->instanceId += "Mix";
    const auto nodeId = node->instanceId;
    const auto result = projectBridge_.editor().mixNodeOnConnection(
        optionalStringView(definitionId), std::move(*mixer), std::move(*node), position, line);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    selectNode(definitionId, nodeId);
    setStatus("Mixed " + juceString(nodeId) + " with " + juceString(line.fromNode));
}

// AR Drag&Drop dropped on a line shapes the Sound
// the line carries; added on the background it waits for one. It takes no
// Sound's place.
void WorkbenchComponent::placePrototypeAssembly(const std::optional<std::string>& definitionId, std::string_view typeId,
    talas::project::CanvasPosition position, const GraphDropTarget& target)
{
    if (!target.connection.has_value() && !target.nodeId.empty())
    {
        setStatus("Drop AR Drag&Drop on a line or on the background");
        return;
    }
    std::string error;
    std::string headId;
    auto placed = talas::app::documentWithPrototypeAssembly(projectBridge_.editor().document(), registry_, typeId,
        optionalStringView(definitionId), position, target.connection, error, &headId);
    const auto result = placed.has_value() ? projectBridge_.editor().applyDocumentEdit(std::move(*placed))
                                           : talas::project::EditResult{talas::project::EditError::InvalidGraph, error};
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    selectNode(definitionId, headId);
    documentChanged();
    const auto& line = target.connection;
    setStatus(line.has_value()
            ? "Inserted AR Drag&Drop between " + juceString(line->fromNode) + " and " + juceString(line->toNode)
            : juce::String{"Added AR Drag&Drop"});
}

std::optional<bool> WorkbenchComponent::renderSoundBrowserAudition(double callbackTimeSeconds,
    float* const* outputChannels,
    std::size_t outputChannelCount,
    std::size_t frameCount,
    talas::runtime::RealtimeAudioInputBlock inputs) noexcept
{
    if (soundBrowserAuditionLive_.load(std::memory_order_acquire) == nullptr)
        return std::nullopt;
    soundBrowserAuditionReaders_.fetch_add(1, std::memory_order_acq_rel);
    std::optional<bool> rendered;
    if (auto* audition = soundBrowserAuditionLive_.load(std::memory_order_acquire); audition != nullptr)
        rendered = audition->bridge->renderOrdered(inputs, callbackTimeSeconds, outputChannels, outputChannelCount, frameCount);
    soundBrowserAuditionReaders_.fetch_sub(1, std::memory_order_acq_rel);
    return rendered;
}

std::size_t WorkbenchComponent::soundBrowserAuditionOutputs() const noexcept
{
    const auto* audition = soundBrowserAuditionLive_.load(std::memory_order_acquire);
    return audition == nullptr ? 0 : audition->outputs;
}

// The Sample Editor's and the browser's samples, added to the first two
// device outputs at their own rate, a short one looping, paused by Space.
void WorkbenchComponent::mixSampleAudition(float* const* outputChannelData, int numOutputChannels, std::size_t outputFrames) noexcept
{
    sampleAuditionReaders_.fetch_add(1, std::memory_order_acq_rel);
    if (const auto* audition = sampleAudition_.load(std::memory_order_acquire);
        audition != nullptr && outputChannelData != nullptr && !audition->channels.empty()
        && !sampleAuditionPaused_.load(std::memory_order_relaxed))
    {
        auto position = sampleAuditionPosition_.load(std::memory_order_relaxed);
        const auto frames = static_cast<double>(audition->channels.front().size());
        for (std::size_t frame = 0; frame < outputFrames; ++frame)
        {
            if (position >= frames)
            {
                if (!audition->loop)
                    break;
                position -= frames;
            }
            const auto index = static_cast<std::size_t>(position);
            for (int channel = 0; channel < std::min(numOutputChannels, 2); ++channel)
            {
                if (outputChannelData[channel] == nullptr)
                    continue;
                const auto& source = audition->channels[std::min<std::size_t>(static_cast<std::size_t>(channel), audition->channels.size() - 1)];
                outputChannelData[channel][frame] += source[index];
            }
            position += audition->step;
        }
        sampleAuditionPosition_.store(position, std::memory_order_relaxed);
    }
    sampleAuditionReaders_.fetch_sub(1, std::memory_order_acq_rel);
}

SoundBrowserPanel* WorkbenchComponent::soundBrowserForSmoke(std::size_t index) const noexcept
{
    return index < soundBrowsers_.size() ? soundBrowsers_[index].panel.get() : nullptr;
}

// A Sound played from the browser shows its surface.
// Played through the substitute input it takes no changes; with the button
// disengaged and the Sound played again, it lets you make them.
void WorkbenchComponent::showSoundBrowserSurface(const talas::project::ProjectDocument& soundFile, const talas::app::SoundBrowserRow& row)
{
    if (soundBrowserAudition_ == nullptr)
        return;
    auto surface = talas::app::soundBrowserSurface(soundFile, row.definitionId, soundBrowserInput_, soundBrowserAudition_->document, registry_);
    if (!surface.ok())
    {
        setStatus("The Sound's surface cannot be shown: " + juceString(surface.diagnostic));
        return;
    }
    if (soundBrowserSurfacePanel_ == nullptr)
    {
        SoundBrowserSurfaceHost host;
        host.sendValue = [this](const talas::project::VirtualControl& widget) {
            return soundBrowserAudition_ != nullptr && soundBrowserSurfaceGeneration_ == soundBrowserAuditionGeneration_
                && soundBrowserAudition_->bridge->updateEventValue(widget.eventName, widget.value, widget.domain);
        };
        host.save = [this](const talas::app::SoundBrowserSurface& shown) { return saveSoundBrowserSurface(shown); };
        host.status = [this](const juce::String& text) { setStatus(text); };
        soundBrowserSurfacePanel_ = makeSoundBrowserSurfacePanel(std::move(host));
    }
    soundBrowserSurfaceFile_ = row.path;
    soundBrowserSurfaceGeneration_ = soundBrowserAuditionGeneration_;
    const auto title = juceString(surface.surface->name) + " - Sound Browser surface";
    soundBrowserSurfacePanel_->show(std::move(*surface.surface));
    if (!isShowing())
        return;
    if (soundBrowserSurfaceWindow_ == nullptr)
        soundBrowserSurfaceWindow_ = makeToolWindow(title, *soundBrowserSurfacePanel_);
    soundBrowserSurfaceWindow_->setName(title);
    soundBrowserSurfaceWindow_->setVisible(true);
    // The browser keeps the keyboard, Space still pausing what it plays.
    soundBrowserSurfaceWindow_->toFront(false);
}

// The new arrangement saved along with the Sound on the disk.
bool WorkbenchComponent::saveSoundBrowserSurface(const talas::app::SoundBrowserSurface& surface)
{
    std::error_code error;
    if (projectPath().has_value() && std::filesystem::equivalent(*projectPath(), soundBrowserSurfaceFile_, error))
    {
        setStatus("This is the open project's Sound file: change its surface in the Virtual Control Surface and save the project");
        return false;
    }
    const auto document = soundFileDocument(soundBrowserSurfaceFile_);
    if (!document.has_value())
    {
        setStatus("The Sound file could not be read");
        return false;
    }
    const auto saved = talas::app::soundFileWithBrowserSurface(*document, surface);
    if (!saved.has_value())
    {
        setStatus(juceString(talas::app::soundBrowserSurfaceNote(surface)));
        return false;
    }
    std::string text;
    try
    {
        text = talas::project::serializeProjectDocument(*saved);
    }
    catch (const std::exception& failure)
    {
        setStatus("The surface could not be saved: " + juceString(failure.what()));
        return false;
    }
    if (!juce::File{juceString(soundBrowserSurfaceFile_.string())}.replaceWithText(juceString(text), false, false, "\n"))
    {
        setStatus("The Sound file could not be written");
        return false;
    }
    setStatus("Saved the surface of " + juceString(surface.name) + " with it in "
        + juceString(soundBrowserSurfaceFile_.filename().string()));
    return true;
}
}
