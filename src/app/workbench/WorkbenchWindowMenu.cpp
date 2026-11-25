#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/project/ProjectClassFromExample.h"
#include "talas/project/ToolFile.h"

#include "talas/app/ClassEditorModel.h"
#include "talas/app/CompiledSounds.h"
#include "talas/app/DefaultSounds.h"
#include "talas/app/GraphBranchLayout.h"
#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/OnScreenKeyboardModel.h"
#include "talas/app/SoundBrowserModel.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectSnapshots.h"

#include <algorithm>
#include <fstream>
#include <iterator>

// The File and Edit menu items: Open
// any..., Play..., Choose window..., Ctrl+Up and Ctrl+Down through the open
// windows, and Large window... for the active field.
namespace talas::app::workbench
{
namespace
{
[[nodiscard]] std::vector<juce::TopLevelWindow*> openWindows()
{
    std::vector<juce::TopLevelWindow*> windows;
    for (int index = 0; index < juce::TopLevelWindow::getNumTopLevelWindows(); ++index)
    {
        auto* window = juce::TopLevelWindow::getTopLevelWindow(index);
        if (window != nullptr && window->isVisible() && window->getName().isNotEmpty())
            windows.push_back(window);
    }
    return windows;
}

[[nodiscard]] juce::String allWildcards(const std::vector<talas::app::TalasFileType>& types)
{
    juce::StringArray patterns;
    for (const auto& type : types)
        patterns.add(juceString(type.wildcard));
    return patterns.joinIntoString(";");
}
}

bool WorkbenchComponent::isWindowMenuCommand(juce::CommandID id) noexcept
{
    return id == commandOpenAny || id == commandPlayFile || id == commandChooseWindow || id == commandNextWindow
        || id == commandPreviousWindow || id == commandLargeWindow || id == commandZoomIn || id == commandZoomOut;
}

void WorkbenchComponent::windowMenuCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto set = [&info](const char* name, const char* description, const juce::String& category) {
        info.setInfo(name, description, category, 0);
    };
    switch (id)
    {
    case commandOpenAny:
        set("Open Any...", "Open a file of any kind Talas uses in its own editor", "File");
        break;
    case commandPlayFile:
        set("Play...", "Choose a kind of file and a file of it, and play it without opening an editor", "File");
        break;
    case commandChooseWindow:
        set("Choose Window...", "Bring an open window to the front", "File");
        break;
    case commandShowSoundFileWindow:
        set("Sound File Window", "The project's Sounds as icons, to play, open, rename, duplicate and delete", "File");
        break;
    case commandShowSystemPrototypes:
        set("System Prototypes", "Show the system prototypes, one example of each kind of Sound", "File");
        break;
    case commandOpenCustomPrototypes:
        set("Open Custom Prototypes...", "Open a Sound file as a strip of prototypes, its collections as categories", "File");
        break;
    case commandNewTool:
        set("New Tool", "Open the Tool editor on a new Tool", "File");
        break;
    case commandOpenTool:
        set("Open Tool...", "Open the Tool editor on a Tool file", "File");
        break;
    case commandUseTool:
        set("Use Tool...", "Run a Tool file in the Tool window", "File");
        break;
    case commandView:
        set("View...", "Set the front window's icon size, a Sound editor's connections or the organizer's folders", "Edit");
        break;
    case commandNewFile:
        set("New...", "Make a new file of a type chosen from a list: a Sound file, a Timeline, a Tool, a sample file or a global map",
            "File");
        info.addDefaultKeypress('n', juce::ModifierKeys::commandModifier);
        break;
    case commandFileArchivist:
        set("File Archivist...", "Archive a Sound file: it and a copy of each file it uses, together in one folder",
            menuFor(talas::app::TalasMenuOperation::FileArchivist));
        break;
    case commandShowMidiMessages:
        set("MIDI Messages...", "Show the keyboard and controller MIDI messages as they arrive", menuFor(talas::app::TalasMenuOperation::MidiMessages));
        break;
    case commandConfigureMidi:
        set("Configure MIDI...", "Choose the Default Input Channel, and check the MIDI messages that arrive",
            menuFor(talas::app::TalasMenuOperation::ConfigureMidi));
        break;
    case commandInitializeDsp:
        set("Initialize DSP", "Close the audio device and open it again, as when it stops answering",
            menuFor(talas::app::TalasMenuOperation::InitializeDsp));
        break;
    case commandNewClassFromExample:
    {
        // Only in a Sound file window.
        const auto applies = talas::app::talasMenuOperationApplies(talas::app::TalasMenuOperation::NewClassFromExample, talasMenuContext());
        set("New Class from Example...", "Make the Sound selected in the Sound file window a class of its own, its variables its parameters, and edit it",
            menuFor(talas::app::TalasMenuOperation::NewClassFromExample));
        if (!applies)
            info.description = juceString(talas::app::talasMenuOperationNeeds(talas::app::TalasMenuOperation::NewClassFromExample));
        info.setActive(applies);
        break;
    }
    case commandEditClass:
        set("Edit Class", "Open the Class Editor on the class of the selected Sound",
            menuFor(talas::app::TalasMenuOperation::EditClass));
        break;
    case commandCompileToDisk:
        set("Compile to Disk...", "Compile the grid in the Live Grid editor, else the selected Sound, into files that play by themselves",
            menuFor(talas::app::TalasMenuOperation::CompileToDisk));
        break;
    case commandRetrieveExampleFromClass:
        set("Retrieve Example from Class", "Put the Sound the class was made from, with its variables, among the project's Sounds",
            menuFor(talas::app::TalasMenuOperation::RetrieveExampleFromClass));
        break;
    case commandSetDefaultSound:
        set("Set Default Sound", "Make the selected Sound or prototype the default Sound a Variable can take",
            menuFor(talas::app::TalasMenuOperation::SetDefaultSound));
        break;
    case commandSetDefaultCollection:
        set("Set Default Collection", "Make the selected Sounds the default collection a SoundCollectionVariable can take",
            menuFor(talas::app::TalasMenuOperation::SetDefaultCollection));
        break;
    case commandShowVirtualControlSurface:
        set("Virtual Control Surface", "Bring the virtual control surface of the Sounds playing to the front",
            menuFor(talas::app::TalasMenuOperation::VirtualControlSurface));
        break;
    case commandRecordToDisk:
        // Record to disk..., the file's format,
        // channels and word size asked first.
        set("Record to Disk...", "Record what the Sound playing sends to the outputs into a sample file",
            menuFor(talas::app::TalasMenuOperation::RecordToDisk));
        break;
    case commandGetInfo:
        // Get info (Ctrl+I) on the selected Sound.
        set("Get Info", "The selected Sound's own parameters, inputs, Event Values, variables, files and complexity",
            menuFor(talas::app::TalasMenuOperation::GetInfo));
        info.addDefaultKeypress('i', juce::ModifierKeys::commandModifier);
        break;
    case commandDescribeSound:
        set("Describe Sound", "Describe the class and parameters of the prototype or Sound selected",
            menuFor(talas::app::TalasMenuOperation::DescribeSound));
        break;
    case commandChooseGlobalMap:
        set("Choose Global Map...", "Choose the global map file the project's Event Values follow", "File");
        break;
    case commandInfoOscilloscope:
        set("Oscilloscope", "Compile, load and start the selected Sound with an oscilloscope of its output",
            menuFor(talas::app::TalasMenuOperation::Oscilloscope));
        break;
    case commandInfoSpectrumAnalyzer:
        set("Spectrum Analyzer", "Compile, load and start the selected Sound with a spectrum analyzer of its output",
            menuFor(talas::app::TalasMenuOperation::SpectrumAnalyzer));
        break;
    case commandNextWindow:
        set("Next Window", "Bring the next open window to the front", "File");
        info.addDefaultKeypress(juce::KeyPress::downKey, juce::ModifierKeys::ctrlModifier);
        break;
    case commandPreviousWindow:
        set("Previous Window", "Bring the previous open window to the front", "File");
        info.addDefaultKeypress(juce::KeyPress::upKey, juce::ModifierKeys::ctrlModifier);
        break;
    case commandFocusPrototypeSearch:
    case commandFindText:
    case commandFindTextAgain:
    case commandReplaceText:
    case commandExpandSound:
    case commandMidiNotesOff:
    case commandCloseWindow:
    {
        const auto& keyed = talas::app::talasKeyEquivalent(id == commandFocusPrototypeSearch ? talas::app::TalasKeyCommand::FindPrototype
                : id == commandFindText                                                   ? talas::app::TalasKeyCommand::Find
                : id == commandFindTextAgain                                              ? talas::app::TalasKeyCommand::FindAgain
                : id == commandExpandSound                                                ? talas::app::TalasKeyCommand::Expand
                : id == commandMidiNotesOff                                               ? talas::app::TalasKeyCommand::MidiNotesOff
                : id == commandCloseWindow                                                ? talas::app::TalasKeyCommand::Close
                                                                                          : talas::app::TalasKeyCommand::Replace);
        info.setInfo(juceString(keyed.name), juceString(keyed.description), juceString(keyed.menu), 0);
        info.addDefaultKeypress(keyed.key, juce::ModifierKeys::commandModifier);
        if (id == commandFocusPrototypeSearch)
            info.setActive(talas::app::talasMenuOperationApplies(talas::app::TalasMenuOperation::FindPrototype, talasMenuContext()));
        break;
    }
    case commandZoomIn:
    case commandZoomOut:
    {
        // Ctrl+] and Ctrl+[.
        const auto& zoom = talas::app::talasKeyEquivalent(
            id == commandZoomIn ? talas::app::TalasKeyCommand::ZoomIn : talas::app::TalasKeyCommand::ZoomOut);
        info.setInfo(juceString(zoom.name), juceString(zoom.description), juceString(zoom.menu), 0);
        info.addDefaultKeypress(zoom.key, juce::ModifierKeys::commandModifier);
        break;
    }
    default:
    {
        const auto& large = talas::app::talasKeyEquivalent(talas::app::TalasKeyCommand::LargeWindow);
        info.setInfo(juceString(large.name), juceString(large.description), juceString(large.menu), 0);
        info.addDefaultKeypress(large.key, juce::ModifierKeys::commandModifier);
        info.setActive(dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr);
        break;
    }
    }
}

void WorkbenchComponent::performWindowMenuCommand(juce::CommandID id)
{
    if (id == commandOpenAny)
        openAnyFile();
    else if (id == commandPlayFile)
        playFileChosen();
    else if (id == commandChooseWindow)
        chooseWindow();
    else if (id == commandNextWindow || id == commandPreviousWindow)
        cycleWindows(id == commandNextWindow ? 1 : -1);
    else if (id == commandLargeWindow)
        showLargeWindow();
    else if (id == commandZoomIn || id == commandZoomOut)
        zoomActiveDisplay(id == commandZoomIn);
    else if (id == commandShowSoundFileWindow)
        showSoundFileWindow();
    else if (id == commandShowSystemPrototypes)
        focusPrototypeSearch();
    else if (id == commandOpenCustomPrototypes)
        openCustomPrototypes();
    else if (id == commandNewTool)
        showToolEditor(talas::project::tool::newTool(), std::nullopt);
    else if (id == commandOpenTool || id == commandUseTool)
        openToolFile(id == commandUseTool);
    else if (id == commandView)
        showViewChoices();
    else if (id == commandFileArchivist)
        showFileArchivist();
    else if (id == commandMidiNotesOff)
        sendMidiNotesOff();
    else if (id == commandShowMidiMessages)
        showMidiMessages();
    else if (id == commandConfigureMidi)
        showConfigureMidi();
    else if (id == commandInitializeDsp)
        initializeDsp();
    else if (id == commandNewClassFromExample)
        newClassFromExample();
    else if (id == commandEditClass)
        editSelectedClass();
    else if (id == commandRetrieveExampleFromClass)
        retrieveExampleFromClass();
    else if (id == commandCompileToDisk)
        compileToDisk();
    else if (id == commandSetDefaultSound || id == commandSetDefaultCollection)
        setDefaultSounds(id == commandSetDefaultCollection);
    else if (id == commandShowVirtualControlSurface)
        showVirtualControlSurfaceInFront();
    else if (id == commandRecordToDisk)
        chooseAnalysisOutputTakeDestination();
    else if (id == commandGetInfo)
    {
        if (selectedNodeIds_.size() == 1)
            describeGraphNode(activeGraphContext_, selectedNodeIds_.front(), true);
        else
            setStatus("Select one Sound to get information on it");
    }
    else if (id == commandDescribeSound)
        describeFrontSound();
    else if (id == commandChooseGlobalMap)
        chooseGlobalMap();
    else if (id == commandCloseWindow)
    {
        // Close closes the window in front, as its
        // close box does; Quit closes the Workbench itself.
        auto* front = dynamic_cast<juce::DocumentWindow*>(juce::TopLevelWindow::getActiveTopLevelWindow());
        if (front != nullptr && !front->isParentOf(this))
            front->closeButtonPressed();
        else
            setStatus("Close closes the window in front of the Workbench; Quit closes the Workbench");
    }
    else if (id == commandExpandSound)
    {
        // The Sound file window in front expands its selection into a copy.
        if (soundFileWindow_ != nullptr && soundFileWindow_->isActiveWindow() && soundFilePanel_ != nullptr)
            (void) soundFilePanel_->keyPressed(juce::KeyPress{'e', juce::ModifierKeys::commandModifier, 0});
        else if (isShowing())
        {
            // The original cannot be recovered once replaced.
            juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
            (void) juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Expand",
                juceString(talas::app::expandReplacementWarning()), "Expand", "Cancel", this,
                juce::ModalCallbackFunction::create([safeThis](int result) {
                    if (safeThis != nullptr && result == 1)
                        safeThis->expandSelectedSound();
                }));
        }
        else
            expandSelectedSound();
    }
    else if (id == commandInfoOscilloscope || id == commandInfoSpectrumAnalyzer)
        showInfoDisplay(id == commandInfoOscilloscope ? talas::app::InfoDisplay::Oscilloscope : talas::app::InfoDisplay::SpectrumAnalyzer);
    else if (id == commandNewFile)
        chooseInput("New file of which type?", talas::app::talasNewFileTypes(), [this](std::string type) { newFileOfType(type); });
}

// Show MIDI Messages, so you can see that MIDI
// arrives.
void WorkbenchComponent::showMidiMessages()
{
    if (midiMessagesText_ == nullptr)
    {
        midiMessagesText_ = std::make_unique<juce::TextEditor>();
        midiMessagesText_->setMultiLine(true);
        midiMessagesText_->setReadOnly(true);
        midiMessagesText_->setFont(WorkbenchType::data());
        midiMessagesText_->setSize(420, 320);
    }
    midiMessagesText_->setText(juceString(midiMessageLog_.text()), false);
    midiMessagesShown_.store(true, std::memory_order_release);
    if (isShowing())
    {
        if (midiMessagesWindow_ == nullptr)
            midiMessagesWindow_ = makeToolWindow("MIDI Messages", *midiMessagesText_, [this] {
                midiMessagesShown_.store(false, std::memory_order_release);
            });
        midiMessagesWindow_->setVisible(true);
        midiMessagesWindow_->toFront(true);
    }
    setStatus("MIDI Messages: the keyboard and controller messages as they arrive");
}

// DSP > Configure MIDI...: the Default Input Channel for the Sounds with no MIDI channel of
// their own, kept in the preferences, and MIDI Messages... to check that
// MIDI arrives.
void WorkbenchComponent::showConfigureMidi()
{
    auto* window = new juce::AlertWindow("Configure MIDI", "The MIDI channel Sounds take when they name none",
        juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray channels;
    for (auto channel = 1; channel <= 16; ++channel)
        channels.add(juce::String(channel));
    window->addComboBox("channel", channels, "Default Input Channel");
    window->getComboBoxComponent("channel")->setSelectedItemIndex(preferences_.defaultMidiInputChannel - 1, juce::dontSendNotification);
    // The surface's own Program Changes channel, apart from
    // the Timeline's.
    auto vcsChannels = channels;
    vcsChannels.insert(0, "Any");
    window->addComboBox("vcsChannel", vcsChannels, "VCS Program Changes channel");
    window->getComboBoxComponent("vcsChannel")->setSelectedItemIndex(preferences_.vcsProgramChangeChannel, juce::dontSendNotification);
    window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("MIDI Messages...", 2);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, window](int answer) {
            if (safeThis == nullptr || answer == 0)
                return;
            auto next = safeThis->preferences_;
            next.vcsProgramChangeChannel = std::clamp(window->getComboBoxComponent("vcsChannel")->getSelectedItemIndex(), 0, 16);
            safeThis->setPreferences(std::move(next));
            safeThis->setDefaultMidiInputChannel(window->getComboBoxComponent("channel")->getSelectedItemIndex() + 1);
            if (answer == 2)
                safeThis->showMidiMessages();
        }),
        true);
}

void WorkbenchComponent::setDefaultMidiInputChannel(int channel)
{
    if (channel < 1 || channel > 16)
        return;
    auto next = preferences_;
    next.defaultMidiInputChannel = channel;
    setPreferences(std::move(next));
    setStatus("Default Input Channel " + juce::String(channel) + ": the Sounds that name no MIDI channel take it as they play");
}

// DSP > Initialize DSP: the audio device, our
// signal processor, closed and opened again with its settings.
void WorkbenchComponent::initializeDsp()
{
    if (!hardwareInitialised_)
    {
        setStatus("Initialize DSP: no audio device is in use");
        return;
    }
    const auto setup = deviceManager_.getAudioDeviceSetup();
    deviceManager_.closeAudioDevice();
    const auto error = deviceManager_.setAudioDeviceSetup(setup, true);
    dspProblemShown_ = false;
    if (auto* device = deviceManager_.getCurrentAudioDevice(); error.isEmpty() && device != nullptr)
        setStatus("Initialized the DSP: " + device->getName());
    else
        setStatus("The DSP could not be initialized: " + (error.isEmpty() ? juce::String{"no audio device"} : error));
}

// A dialog once the audio device stops answering, pointing to
// Initialize DSP; none again until it answers.
void WorkbenchComponent::watchDspDevice()
{
    if (!hardwareInitialised_)
        return;
    auto* device = deviceManager_.getCurrentAudioDevice();
    const auto answering = device != nullptr && device->isPlaying();
    if (answering || dspProblemShown_)
    {
        dspProblemShown_ = dspProblemShown_ && !answering;
        return;
    }
    dspProblemShown_ = true;
    if (isShowing())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "DSP",
            juceString(talas::app::dspProblemMessage(device == nullptr ? "no audio device" : "the audio device stopped")), "OK", this);
}

void WorkbenchComponent::noteMidiMessageForWindow(const juce::MidiMessage& message)
{
    // Only the channel messages: keys, controllers, programs and the like.
    const auto* raw = message.getRawData();
    const auto size = message.getRawDataSize();
    if (!midiMessagesShown_.load(std::memory_order_acquire) || raw == nullptr || size <= 0 || raw[0] >= 0xF0)
        return;
    std::vector<std::uint8_t> bytes(raw, raw + size);
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<WorkbenchComponent>(this), bytes = std::move(bytes)] {
        if (safeThis != nullptr)
            safeThis->appendMidiMessageLine(bytes);
    });
}

void WorkbenchComponent::appendMidiMessageLine(std::span<const std::uint8_t> bytes)
{
    midiMessageLog_.add(talas::app::midiMessageDescription(bytes));
    if (midiMessagesText_ == nullptr)
        return;
    midiMessagesText_->setText(juceString(midiMessageLog_.text()), false);
    midiMessagesText_->moveCaretToEnd();
}

void WorkbenchComponent::sendMidiNotesOff(std::size_t from)
{
    // The notes go as the on-screen keyboard's do, to the playing Sound or
    // the Samples workspace's key selection.
    const auto notes = talas::app::midiNotesOff();
    const auto upTo = std::min(notes.size(), from + 256);
    for (auto index = from; index < upTo; ++index)
        playOnScreenKeyboardNote(notes[index]);
    if (upTo == notes.size())
    {
        setStatus("Sent a note-off for every note on every MIDI channel");
        return;
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::Timer::callAfterDelay(10, [safeThis, upTo] {
        if (safeThis != nullptr)
            safeThis->sendMidiNotesOff(upTo);
    });
}

talas::app::TalasTransportState WorkbenchComponent::talasTransportState() const noexcept
{
    if (transportStopped_)
        return talas::app::TalasTransportState::Stopped;
    if (!currentDocumentStaged_)
        return talas::app::TalasTransportState::Edited;
    return transportPlaying_ ? talas::app::TalasTransportState::Playing : talas::app::TalasTransportState::Paused;
}

// The project is the one Sound here, so the last Sound played and this one
// are the project; after an edit Ctrl+R compiles and plays the edited one,
// the Sound played before it not being kept apart.
void WorkbenchComponent::pressTransportKey(talas::app::TalasTransportPress press)
{
    switch (talas::app::talasTransportAction(talasTransportState(), press))
    {
    case talas::app::TalasTransportAction::CompileAndPlay:
        // The edited Sound compiled and played in place of the one playing.
        if (transportPlaying_)
            pauseCurrentProject();
        playCurrentProject();
        break;
    case talas::app::TalasTransportAction::Resume:
        playCurrentProject();
        break;
    case talas::app::TalasTransportAction::Pause:
        pauseCurrentProject();
        break;
    case talas::app::TalasTransportAction::ReplayLast:
    case talas::app::TalasTransportAction::ReplayThis:
        restartCurrentProject();
        break;
    case talas::app::TalasTransportAction::Stop:
        stopCurrentProject();
        break;
    }
}

// File > New...: a Sound file is a new project
// here, a Timeline one with the Timeline shown; a Tool opens the Tool
// editor, a sample file the sample editor, a global map its editor and a
// text file the text file editor.
void WorkbenchComponent::newFileOfType(const std::string& type)
{
    if (type == "Sound file" || type == "Timeline")
    {
        requestNewProject();
        if (type == "Timeline")
            activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
    }
    else if (type == "Tool")
        showToolEditor(talas::project::tool::newTool(), std::nullopt);
    else if (type == "Sample file")
        showSampleEditor();
    else if (type == "Global map")
        showGlobalMap();
    else if (type == "Text file")
        showTextFile(std::nullopt);
    else if (type == "Compiled Sound grid" && commandManager_.invokeDirectly(commandShowLiveGridEditor, false)
        && liveGridEditorPanel_ != nullptr)
        (void)liveGridEditorPanel_->addGrid();
}

// Tools > File Archivist: Archive Timeline
// finds the Sound file on the disk, a project or a package, and the place
// for its archive is asked next.
void WorkbenchComponent::showFileArchivist()
{
    if (packageRunning_ || fileChooserActive_)
    {
        setStatus("Finish the package or dialog in progress before archiving");
        return;
    }
    const auto initial = projectPath().has_value() ? juce::File(jucePathString(*projectPath()))
                                                   : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    fileChooser_ = std::make_unique<juce::FileChooser>("Archive Timeline", initial, "*.talas.json;*.talaspkg");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            const auto chosen = chooser.getResult();
            if (chosen == juce::File{})
                return;
            const auto path = filesystemPath(chosen.getFullPathName());
            safeThis->scheduleContinuation([safeThis, path] {
                if (safeThis != nullptr && safeThis->archiveProjectFile(path))
                    safeThis->choosePackageDestination();
            });
        });
}

// Info > Oscilloscope and Spectrum analyzer:
// the selected Sound, one of the project's structure, compiled, loaded and
// started with the display after it, in the Analysis workspace.
bool WorkbenchComponent::prepareInfoDisplay(talas::app::InfoDisplay display)
{
    // With the Sound file window in front, the Sound
    // selected there.
    const auto filed = soundFileWindow_ != nullptr && soundFileWindow_->isActiveWindow() && soundFilePanel_ != nullptr
        ? soundFilePanel_->selectedSounds()
        : std::vector<std::string>{};
    infoDisplayStage_ = filed.size() == 1
        ? talas::app::soundWithInfoDisplay(projectBridge_.editor().document(), registry_, filed.front(), display)
        : selectedNodeIds_.size() == 1 && !activeGraphContext_.has_value()
        ? talas::app::documentWithInfoDisplay(projectBridge_.editor().document(), registry_, selectedNodeIds_.front(), display)
        : std::nullopt;
    if (!infoDisplayStage_.has_value())
        setStatus("Select one Sound of the project's structure to show it on a display");
    return infoDisplayStage_.has_value();
}

void WorkbenchComponent::showInfoDisplay(talas::app::InfoDisplay display)
{
    if (!prepareInfoDisplay(display))
        return;
    currentDocumentStaged_ = false;
    playCurrentProject();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
}

// Save as...: when the surface keeps layouts and
// presets for controls and Sounds the project no longer has, the choice to
// remove them first, which makes the file smaller.
void WorkbenchComponent::offerObsoleteMaterialRemoval(std::function<void()> then)
{
    const auto material = talas::project::obsoleteVirtualControlMaterial(projectBridge_.editor().document(), registry_);
    if (material.empty())
    {
        then();
        return;
    }
    const auto entries = material.widgets + material.layoutEntries + material.presetValues;
    chooseInput("Remove the " + juce::String(static_cast<juce::int64>(entries))
            + " obsolete layout and preset entries kept for controls and Sounds no longer here?",
        {"Remove them", "Keep them"}, [this, then = std::move(then)](const std::string& choice) {
            if (choice == "Remove them")
            {
                auto cleaned = projectBridge_.editor().document();
                (void)talas::project::removeObsoleteVirtualControlMaterial(cleaned, registry_);
                const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(cleaned));
                if (!applied.ok())
                {
                    setStatus("The obsolete layouts and presets were not removed: " + juceString(applied.message));
                    return;
                }
                documentChanged();
            }
            then();
        });
}

// The Sound file to archive, read from the disk rather than the project
// open; the next package made is of it.
bool WorkbenchComponent::archiveProjectFile(const std::filesystem::path& path)
{
    archivedProject_.reset();
    const auto resolved = talas::project::resolveProjectPackagePath(path);
    auto loaded = talas::project::loadProjectFile(resolved.ok() ? resolved.projectFilePath : path);
    if (!loaded.ok())
    {
        setStatus("Cannot archive " + jucePathString(path.filename())
            + (loaded.errors.empty() ? juce::String{} : ": " + juceString(loaded.errors.front().message)));
        return false;
    }
    archivedProject_.emplace(std::move(*loaded.document), resolved.ok() ? resolved.projectFilePath : path);
    setStatus("Archiving " + jucePathString(path.filename()) + "; choose where to save the archive");
    return true;
}

void WorkbenchComponent::showTextFile(std::optional<std::filesystem::path> path)
{
    if (textFilePanel_ == nullptr)
        textFilePanel_ = makeTextFilePanel([this](const juce::String& status) { setStatus(status); });
    if (!path.has_value())
        textFilePanel_->newFile();
    else if (!textFilePanel_->open(*path))
        return;
    if (isShowing())
    {
        if (textFileWindow_ == nullptr)
            textFileWindow_ = makeToolWindow("Text", *textFilePanel_);
        textFileWindow_->setName(textFilePanel_->title());
        textFileWindow_->setVisible(true);
        textFileWindow_->toFront(true);
    }
}

// Edit > View...: for the window in front, a
// Sound file window's icon size, the organizer's folders, else the Sound
// editor's icons and connections.
talas::app::ViewedWindow WorkbenchComponent::viewedWindow() const
{
    const auto* front = juce::TopLevelWindow::getActiveTopLevelWindow();
    if (front != nullptr && front == soundFileWindow_.get())
        return talas::app::ViewedWindow::SoundFileWindow;
    if (front != nullptr && front == fileOrganizerWindow_.get())
        return talas::app::ViewedWindow::FileOrganizer;
    return talas::app::ViewedWindow::SoundEditor;
}

bool WorkbenchComponent::viewChoiceTicked(talas::app::ViewedWindow window, const std::string& name) const
{
    using talas::app::GraphLineContour;
    using talas::project::WorkspaceGraphViewMode;
    switch (window)
    {
    case talas::app::ViewedWindow::SoundFileWindow:
        return soundFilePanel_ != nullptr && soundFilePanel_->largeIcons() == (name == "Large icons");
    case talas::app::ViewedWindow::FileOrganizer:
        return fileOrganizerPanel_ != nullptr && fileOrganizerPanel_->foldersFirst() == (name == "Folders first");
    case talas::app::ViewedWindow::SoundEditor:
        break;
    }
    const auto mode = workspaceSession_.graphViewMode;
    const auto contour = graphCanvas_->lineContour();
    const auto scale = static_cast<double>(graphCanvas_->viewScaleForSmoke());
    return (name == "Large icons" && std::abs(scale - talas::app::signalFlowIconScale(true)) < 1.0e-3)
        || (name == "Small icons" && std::abs(scale - talas::app::signalFlowIconScale(false)) < 1.0e-3)
        || (name == "Compact" && mode == WorkspaceGraphViewMode::Compact) || (name == "With ports" && mode == WorkspaceGraphViewMode::Ports)
        || (name == "With ports and wires" && mode == WorkspaceGraphViewMode::PortsAndWires)
        || (name == "Curved" && contour == GraphLineContour::Curved) || (name == "Radial" && contour == GraphLineContour::Radial)
        || (name == "Right angle" && contour == GraphLineContour::RightAngle)
        || (name == "Parameters below the signal flow" && parametersBelow_)
        || (name == "Parameters beside the signal flow" && !parametersBelow_)
        || (name == "Prototypes across the top" && prototypeStripAtTop_)
        || (name == "Prototypes at the left" && !prototypeStripAtTop_);
}

bool WorkbenchComponent::applyViewChoice(talas::app::ViewedWindow window, const std::string& name)
{
    using talas::app::GraphLineContour;
    using talas::project::WorkspaceGraphViewMode;
    switch (window)
    {
    case talas::app::ViewedWindow::SoundFileWindow:
        if (soundFilePanel_ == nullptr)
            return false;
        soundFilePanel_->setLargeIcons(name == "Large icons");
        break;
    case talas::app::ViewedWindow::FileOrganizer:
        if (fileOrganizerPanel_ == nullptr)
            return false;
        fileOrganizerPanel_->setFoldersFirst(name == "Folders first");
        break;
    case talas::app::ViewedWindow::SoundEditor:
        if (name == "Large icons" || name == "Small icons")
            graphCanvas_->setIconScale(static_cast<float>(talas::app::signalFlowIconScale(name == "Large icons")));
        else if (name == "Parameters below the signal flow" || name == "Parameters beside the signal flow")
        {
            parametersBelow_ = name == "Parameters below the signal flow";
            resized();
        }
        else if (name == "Prototypes across the top" || name == "Prototypes at the left")
        {
            prototypeStripAtTop_ = name == "Prototypes across the top";
            resized();
        }
        else if (name == "Compact" || name == "With ports" || name == "With ports and wires")
            setWorkspaceGraphViewMode(name == "Compact" ? WorkspaceGraphViewMode::Compact
                    : name == "With ports"              ? WorkspaceGraphViewMode::Ports
                                                        : WorkspaceGraphViewMode::PortsAndWires);
        else
            graphCanvas_->setLineContour(name == "Radial" ? GraphLineContour::Radial
                    : name == "Right angle"               ? GraphLineContour::RightAngle
                                                          : GraphLineContour::Curved);
        break;
    }
    setStatus("View: " + juceString(name));
    return true;
}

void WorkbenchComponent::showViewChoices()
{
    const auto window = viewedWindow();
    const auto choices = talas::app::viewChoices(window);
    juce::PopupMenu menu;
    std::string group;
    for (std::size_t index = 0; index < choices.size(); ++index)
    {
        if (choices[index].group != group)
        {
            group = choices[index].group;
            menu.addSectionHeader(juceString(group));
        }
        menu.addItem(static_cast<int>(index) + 1, juceString(choices[index].name), true, viewChoiceTicked(window, choices[index].name));
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options{}, [safeThis, window, choices](int result) {
        if (safeThis != nullptr && result > 0 && static_cast<std::size_t>(result) <= choices.size())
            (void)safeThis->applyViewChoice(window, choices[static_cast<std::size_t>(result) - 1].name);
    });
}

// One source replacing another asks to confirm; true
// when it asked, `confirmed` running on Replace.
bool WorkbenchComponent::confirmSourceReplacement(std::optional<std::string_view> scope, std::string_view replacedNodeId,
    const talas::project::ProjectNode& replacement, std::function<void()> confirmed)
{
    const auto descriptor = talas::project::describeProjectNode(projectBridge_.editor().document(), replacement, registry_);
    const auto takesInputs = descriptor.has_value() && std::ranges::any_of(descriptor->inputs, [](const talas::core::PortDescriptor& input) {
        return input.required && !input.compilerPopulated && input.type == talas::core::PortType::Audio;
    });
    if (!talas::app::replacementAsksToConfirm(!projectBridge_.editor().inputsReplacingNode(scope, replacedNodeId).empty(), takesInputs))
        return false;
    chooseInput(juceString(talas::app::replacementConfirmation(replacedNodeId, replacement.instanceId)), {"Replace", "Cancel"},
        [confirmed = std::move(confirmed)](std::string choice) {
            if (choice == "Replace")
                confirmed();
        });
    return true;
}

void WorkbenchComponent::chooseInput(
    juce::String question,
    std::vector<std::string> inputs,
    std::function<void(std::string)> chosen)
{
    if (smokeInputChoice_.has_value())
    {
        if (*smokeInputChoice_ < inputs.size())
            chosen(inputs[*smokeInputChoice_]);
        return;
    }
    juce::PopupMenu menu;
    menu.addSectionHeader(question);
    for (std::size_t index = 0; index < inputs.size(); ++index)
        menu.addItem(static_cast<int>(index) + 1, juceString(inputs[index]));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options{},
        [safeThis, inputs = std::move(inputs), chosen = std::move(chosen)](int result) {
            if (safeThis == nullptr || result <= 0 || static_cast<std::size_t>(result) > inputs.size())
                return;
            chosen(inputs[static_cast<std::size_t>(result) - 1]);
        });
}

void WorkbenchComponent::zoomActiveDisplay(bool in)
{
    if (prototypePanel_->zoomIfFocused(in) || inspectorPanel_->zoomActiveField(in))
        return;
    if (activeGraphContext_ == secondaryGraphContext_ && secondaryGraphContext_.has_value())
        secondaryGraphCanvas_->zoomStep(in);
    else
        graphCanvas_->zoomStep(in);
}

// Open any...: any kind of file, in the editor for its kind.
void WorkbenchComponent::openAnyFile()
{
    auto chooser = std::make_shared<juce::FileChooser>("Open any file", juce::File{}, allWildcards(talas::app::talasFileTypes(false)));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& chosen) {
            if (safeThis == nullptr || chosen.getResult() == juce::File{})
                return;
            const auto path = filesystemPath(chosen.getResult().getFullPathName());
            const auto name = path.filename().string();
            if (name.ends_with(".talas.json") || name.ends_with(".talaspkg"))
                safeThis->loadProjectFromPath(path);
            // A Timeline's file opens with its Timeline.
            else if (talas::app::isTimelineFile(path))
            {
                safeThis->loadProjectFromPath(path);
                safeThis->activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
            }
            else if (name.ends_with(talas::project::tool::toolFileExtension))
                (void) safeThis->openToolAt(path, true);
            else if (name.ends_with(talas::app::compiledSoundGridExtension))
                safeThis->openCompiledSoundGridFile(path);
            else if (talas::app::talasFileOpensAsText(path))
                safeThis->showTextFile(path);
            else if (talas::app::talasFileIsGlobalMap(path))
                safeThis->openGlobalMapFile(path);
            else
                (void) safeThis->openTalasFile(path, true);
        });
}

// File > Open...: the file
// type first, a Sound file chosen unless another is, then a file of it: a
// Sound file or Timeline opens as the project, Edit Tool opens a Tool in the
// Tool editor, Use Tool runs it, and Any openable file is Open any....
void WorkbenchComponent::openFileOfChosenType()
{
    const auto types = talas::app::talasOpenFileTypes();
    auto* window = new juce::AlertWindow("Open", "Choose the type of file to open.", juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray names;
    for (const auto& type : types)
        names.add(juceString(type.name));
    window->addComboBox("type", names, "File type");
    window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, window, types](int result) {
            if (safeThis == nullptr || result != 1 || types.empty())
                return;
            const auto index = std::max(0, window->getComboBoxComponent("type")->getSelectedItemIndex());
            const auto& type = types[static_cast<std::size_t>(std::min<int>(index, static_cast<int>(types.size()) - 1))];
            switch (type.action)
            {
            case talas::app::TalasOpenAction::Project:
                safeThis->requestOpenProject();
                break;
            case talas::app::TalasOpenAction::EditTool:
                safeThis->openToolFile(false);
                break;
            case talas::app::TalasOpenAction::UseTool:
                safeThis->openToolFile(true);
                break;
            case talas::app::TalasOpenAction::AnyOpenableFile:
                safeThis->openAnyFile();
                break;
            }
        }),
        true);
}

// A Compiled Sound Grid's file: the grid joins the
// project with its Sounds, shown in the Live Grid editor.
void WorkbenchComponent::openCompiledSoundGridFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto file = talas::project::parseProjectDocument(text);
    const auto opened = file.ok() ? talas::app::withCompiledSoundGridFile(projectBridge_.editor().document(), file.document, registry_)
                                  : talas::app::CompiledSoundGridOpened{{}, {}, "The file is no Compiled Sound Grid"};
    if (!opened.ok())
    {
        setStatus(juceString(opened.diagnostic));
        return;
    }
    if (const auto replaced = projectBridge_.editor().replaceDocument(*opened.document); !replaced.ok())
    {
        setStatus("The grid could not join the project: " + juceString(replaced.message));
        return;
    }
    documentChanged();
    (void) commandManager_.invokeDirectly(commandShowLiveGridEditor, false);
    setStatus("The grid " + jucePathString(path.filename()) + " joined the project");
}

// Play...: the kind of file first, then one of that kind, played
// as the Sound Browser plays it: a sample, or a Sound file's first Sound.
void WorkbenchComponent::playFileChosen()
{
    const auto types = talas::app::talasFileTypes(true);
    auto* window = new juce::AlertWindow("Play", "Choose the kind of file to play.", juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray names;
    for (const auto& type : types)
        names.add(juceString(type.name));
    window->addComboBox("type", names, "File type");
    window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, window, types](int result) {
            if (safeThis == nullptr || result != 1)
                return;
            const auto index = std::max(0, window->getComboBoxComponent("type")->getSelectedItemIndex());
            const auto type = types[static_cast<std::size_t>(std::min<int>(index, static_cast<int>(types.size()) - 1))];
            auto chooser = std::make_shared<juce::FileChooser>("Play " + juceString(type.name), juce::File{}, juceString(type.wildcard));
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [safeThis, chooser, type](const juce::FileChooser& chosen) {
                    if (safeThis == nullptr || chosen.getResult() == juce::File{})
                        return;
                    (void) safeThis->playOrganizerFile(filesystemPath(chosen.getResult().getFullPathName()));
                });
        }),
        true);
}

bool WorkbenchComponent::playFileAtPath(const std::filesystem::path& path)
{
    const auto kind = talas::app::soundBrowserKindOf(path);
    if (kind == talas::app::SoundBrowserKind::Sample || kind == talas::app::SoundBrowserKind::Ga)
        return playFromSoundBrowser({path, {}, path.filename().string(), 0, *kind, false, false});
    if (kind != talas::app::SoundBrowserKind::SoundFile)
    {
        setStatus("Play... plays samples and the Sounds of Sound files");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto loaded = talas::project::parseProjectDocument(text);
    const auto sounds = loaded.ok() ? talas::app::soundBrowserSounds(loaded.document) : std::vector<talas::app::SoundBrowserSound>{};
    if (sounds.empty())
    {
        setStatus("The Sound file holds no Sound to play");
        return false;
    }
    return playFromSoundBrowser(
        {path, sounds.front().definitionId, sounds.front().name, 1, talas::app::SoundBrowserKind::Sound, false, sounds.front().processesInput});
}

// Choose window...: a list of the open windows; OK brings the one
// chosen to the front.
void WorkbenchComponent::chooseWindow()
{
    const auto windows = openWindows();
    if (windows.empty())
        return;
    auto* window = new juce::AlertWindow("Choose Window", {}, juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray names;
    for (auto* open : windows)
        names.add(open->getName());
    window->addComboBox("window", names, "Window");
    window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([window, names](int result) {
            if (result != 1)
                return;
            const auto chosen = window->getComboBoxComponent("window")->getText();
            for (auto* open : openWindows())
            {
                if (open->getName() == chosen)
                {
                    open->toFront(true);
                    return;
                }
            }
        }),
        true);
}

// Ctrl+Up and Ctrl+Down: each open window to the front in turn.
void WorkbenchComponent::cycleWindows(int step)
{
    const auto windows = openWindows();
    if (windows.size() < 2)
        return;
    const auto active = std::ranges::find_if(windows, [](juce::TopLevelWindow* window) { return window->isActiveWindow(); });
    const auto current = active == windows.end() ? 0 : static_cast<std::size_t>(active - windows.begin());
    windows[talas::app::cycledWindowIndex(windows.size(), current, step)]->toFront(true);
}

// Large window...: the active field in a window filling the screen;
// Accept puts its text back into the field, Cancel leaves the field as it was.
void WorkbenchComponent::showLargeWindow()
{
    auto* field = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
    if (field == nullptr)
    {
        setStatus("Click in a field before choosing Large window");
        return;
    }
    auto* window = new juce::AlertWindow("Large Window", field->getTitle(), juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("text", field->getText());
    if (auto* editor = window->getTextEditor("text"))
    {
        editor->setMultiLine(true, true);
        editor->setReturnKeyStartsNewLine(true);
        const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto area = screen == nullptr ? juce::Rectangle<int>{0, 0, 900, 600} : screen->userBounds.toNearestInt().reduced(80);
        editor->setSize(area.getWidth(), area.getHeight() - 120);
    }
    window->addButton("Accept", 1);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<juce::TextEditor> safeField(field);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeField, window](int result) {
            if (safeField == nullptr)
                return;
            // Accept puts the window's text into the field, Cancel leaves it.
            const talas::app::LargeWindowField large{
                safeField->getText().toStdString(), window->getTextEditorContents("text").toStdString()};
            const auto text = talas::app::largeWindowResult(large, result == 1);
            if (text == large.original)
                return;
            safeField->setText(juceString(text), true);
            if (safeField->onReturnKey)
                safeField->onReturnKey();
        }),
        true);
}

// File > Virtual Control Surface: the
// surface of the Sounds playing, in the Patch workspace and in front.
void WorkbenchComponent::showVirtualControlSurfaceInFront()
{
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    if (virtualControlSurface_->isShowing())
        virtualControlSurface_->grabKeyboardFocus();
    setStatus(virtualControlSurface_->isVisible() ? "The virtual control surface is in front"
                                                  : "Close the help below to see the virtual control surface");
}

void WorkbenchComponent::newClassFromExample()
{
    // Only with the Sound file
    // window in front, the class made from the Sound selected there; with
    // the Sound in a Sound editor or a Sound Browser, put it in a Sound
    // file window first.
    if (!soundFileWindowInFront())
    {
        setStatus(juceString(talas::app::talasMenuOperationNeeds(talas::app::TalasMenuOperation::NewClassFromExample)));
        return;
    }
    const auto filed = soundFilePanel_->selectedSounds();
    if (filed.size() == 1)
        newClassFromSoundFileExample(filed.front());
    else
        setStatus("Select one Sound in the Sound file window to make a class of it");
}

void WorkbenchComponent::newClassFromSoundFileExample(const std::string& exampleId)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    if (askForVariableValues([safeThis, exampleId] {
            if (safeThis != nullptr)
                safeThis->newClassFromSoundFileExample(exampleId);
        }))
        return;
    auto start = projectBridge_.editor().document();
    const auto [classId, className] = nextSoundDefinitionIdentity();
    auto made = talas::project::classFromSoundFileExample(start, registry_, exampleId, classId, className);
    if (!made.has_value())
    {
        setStatus("Select one Sound in the Sound file window to make a class of it");
        return;
    }
    if (const auto added = projectBridge_.editor().addSoundDefinition(talas::app::classWithDefaults(std::move(*made), variableEnvironment_));
        !added.ok())
    {
        setStatus("The class was refused: " + juceString(added.message));
        return;
    }
    documentChanged();
    newClassStart_ = std::move(start);
    newClassInSoundFile_ = classId;
    showClassEditor();
    (void) classEditorPanel_->selectDefinitionForSmoke(classId);
    setStatus("New class " + juceString(className) + " from the Sound file window's Sound: its parameters in the Class Editor");
}

// The new class's instance, named as the class, highlighted
// in the Sound file window.
void WorkbenchComponent::saveNewClassInstanceInSoundFile(const std::string& classId)
{
    auto saved = talas::app::classInstanceSavedInSoundFile(projectBridge_.editor().document(), classId);
    if (!saved.has_value())
    {
        setStatus("The new class is gone; no instance was saved");
        return;
    }
    const auto selection = saved->selection;
    if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(saved->document)); !applied.ok())
    {
        setStatus("The instance was refused: " + juceString(applied.message));
        return;
    }
    documentChanged();
    showSoundFileWindow();
    if (soundFilePanel_ != nullptr)
        soundFilePanel_->selectSounds(selection);
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    const auto instance = std::ranges::find(definitions, selection.front(), &talas::project::ProjectSoundDefinition::definitionId);
    setStatus("Saved " + juceString(instance == definitions.end() ? selection.front() : instance->displayName)
        + ", an instance of the new class, in the Sound file window");
}

void WorkbenchComponent::editSelectedClass()
{
    const auto definition = talas::app::classOfSelection(projectBridge_.editor().document(), activeGraphContext_,
        selectedNodeIds_.size() == 1 ? selectedNodeIds_.front() : std::string{});
    if (!definition.has_value())
    {
        setStatus("Select a Sound made from a class to edit its class");
        return;
    }
    showClassEditor();
    (void) classEditorPanel_->selectDefinitionForSmoke(*definition);
}

talas::app::TalasMenuContext WorkbenchComponent::talasMenuContext() const
{
    return {workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch,
        revertSnapshot_.has_value() && revertSnapshot_->nodeId == selectedNodeId_
            && revertSnapshot_->definitionId == activeGraphContext_,
        talas::app::workbenchWorkspaceVisibility(workspaceMode_).library, soundFileWindowInFront()};
}

void WorkbenchComponent::retrieveExampleFromClass()
{
    const auto& document = projectBridge_.editor().document();
    auto classId = classEditorPanel_ != nullptr && classEditorWindow_ != nullptr && classEditorWindow_->isVisible()
        ? std::optional<std::string>{classEditorPanel_->selectedDefinition()}
        : std::nullopt;
    // A few System classes have an example too: the
    // prototype selected while the Prototypes have the focus, else the
    // System Sound selected in the signal flow.
    std::optional<std::string> systemClass;
    const auto selectedNode = selectedNodeIds_.size() == 1 ? selectedNodeIds_.front() : std::string{};
    if (!classId.has_value() || classId->empty())
    {
        systemClass = prototypePanel_->focusedPrototype();
        if (!systemClass.has_value())
            classId = talas::app::classOfSelection(document, activeGraphContext_, selectedNode);
        if (!systemClass.has_value() && !classId.has_value())
            systemClass = talas::app::systemClassOfSelection(document, activeGraphContext_, selectedNode);
    }
    std::string systemError;
    const auto example = systemClass.has_value() ? talas::app::systemClassExample(document, registry_, *systemClass, systemError)
        : classId.has_value()                    ? talas::app::classExample(document, *classId)
                                                 : std::nullopt;
    if (!example.has_value())
    {
        setStatus(systemError.empty() ? juce::String{"Edit a class to retrieve the Sound it was made from"} : juceString(systemError));
        return;
    }
    const auto name = example->displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(*example); !added.ok())
    {
        setStatus("The example was refused: " + juceString(added.message));
        return;
    }
    documentChanged();
    setStatus("Saved " + juceString(name) + ", the Sound the class was made from, among the project's Sounds");
}

// Action > Set default Sound and Set default collection: the prototype
// selected, else each
// Sound selected in the signal flow with the Sounds feeding it.
void WorkbenchComponent::setDefaultSounds(bool collection)
{
    const auto& document = projectBridge_.editor().document();
    std::vector<talas::project::ProjectSoundDefinition> chosen;
    if (const auto prototype = prototypePanel_->focusedPrototype(); prototype.has_value())
    {
        if (const auto node = makePrototypeNode(std::nullopt, *prototype); node.has_value())
        {
            talas::project::ProjectGraph alone;
            alone.nodes.push_back(*node);
            chosen.push_back(talas::app::defaultSoundOfNode(document, alone, node->instanceId, registry_));
        }
    }
    else if (const auto* graph = graphForContext(activeGraphContext_); graph != nullptr)
    {
        for (const auto& nodeId : selectedNodeIds_)
            chosen.push_back(talas::app::defaultSoundOfNode(document, *graph, nodeId, registry_));
    }
    std::erase_if(chosen, [](const auto& sound) { return sound.outputs.empty(); });
    if (chosen.empty())
    {
        setStatus("Select a Sound or a prototype to make it the default");
        return;
    }
    if (collection)
    {
        defaultCollection_ = std::move(chosen);
        setStatus("Default collection: " + juce::String{static_cast<int>(defaultCollection_.size())} + " Sounds");
        return;
    }
    defaultSound_ = std::move(chosen.front());
    setStatus("Default Sound: " + juceString(defaultSound_->displayName));
}
}
