#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/DefaultSounds.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectStructureText.h"
#include "talas/project/ProjectVariables.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/ClassEditorModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/TimelineMidiImport.h"
#include "talas/app/VariableValuePrompt.h"

#include <cmath>
#include <fstream>
#include <iterator>

#if JUCE_MAC
#include <malloc/malloc.h>
#include <mach/mach.h>
#endif

namespace talas::app::workbench
{
void WorkbenchComponent::showExpressionsLibrary()
{
    // One's own expressions files in the frequently used folders.
    if (expressionsLibraryPanel_ == nullptr)
        expressionsLibraryPanel_ = makeExpressionsLibraryPanel([this] {
            std::vector<std::filesystem::path> folders;
            for (const auto& folder : preferences_.frequentlyUsedFolders)
                folders.push_back(talas::project::pathFromUtf8(folder));
            return talas::app::expressionsFilesIn(folders);
        });
    // A Workbench that is not on screen (a smoke run) keeps the panel
    // without opening a window.
    if (isShowing())
    {
        if (expressionsLibraryWindow_ == nullptr)
            expressionsLibraryWindow_ = makeToolWindow("Expressions Library", *expressionsLibraryPanel_);
        expressionsLibraryWindow_->setVisible(true);
        expressionsLibraryWindow_->toFront(true);
    }
    setStatus("Expressions Library: drag an expression onto a parameter's value field");
}

void WorkbenchComponent::showSampleEditor()
{
    if (sampleEditorPanel_ == nullptr)
    {
        sampleEditorPanel_ = makeSampleEditorPanel(
            [this](const juce::String& status) { setStatus(status); },
            [this](const std::filesystem::path& file) { useEditedSample(file); },
            [this](std::vector<std::vector<double>> channels, double sampleRate) {
                playSampleBuffer(std::move(channels), sampleRate);
            },
            [this](const std::filesystem::path& source) {
                sampleClipSource_ = source;
                setGraphClipboardText(juceString(sampleClipboardMarker));
                timelinePanel_->forgetCopiedClips();
            });
    }
    if (isShowing())
    {
        if (sampleEditorWindow_ == nullptr)
            sampleEditorWindow_ = makeToolWindow("Sample Editor", *sampleEditorPanel_, {}, [this] { return sampleEditorMayClose(); });
        sampleEditorWindow_->setVisible(true);
        sampleEditorWindow_->toFront(true);
    }
    setStatus("Sample Editor: drag to select, Insert a template, Save to use the sample");
}

// Closing the editor on a changed file asks to save it
// first; Save goes through Save..., the window staying open until it is.
bool WorkbenchComponent::sampleEditorMayClose()
{
    if (sampleEditorPanel_ == nullptr || !sampleEditorPanel_->model().modified())
        return true;
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::QuestionIcon)
        .withTitle("Sample Editor")
        .withMessage("Save the changes to the sample file before closing?")
        .withButton("Save")
        .withButton("Don't Save")
        .withButton("Cancel")
        .withAssociatedComponent(sampleEditorPanel_.get());
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(options, [safeThis](int choice) {
        if (safeThis == nullptr || safeThis->sampleEditorPanel_ == nullptr)
            return;
        if (choice == 0)
            safeThis->sampleEditorPanel_->startSave();
        else if (choice == 1)
        {
            safeThis->sampleEditorPanel_->model().markSaved();
            if (safeThis->sampleEditorWindow_ != nullptr)
                safeThis->sampleEditorWindow_->setVisible(false);
        }
    });
    return false;
}

bool WorkbenchComponent::illegalValueMayClose()
{
    if (inspectorPanel_ == nullptr || !inspectorPanel_->holdsIllegalValue())
        return true;
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("A parameter field has a problem")
        .withMessage("Drop the changes to the parameter, or continue editing?")
        .withButton("Drop the Changes")
        .withButton("Continue Editing")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(options, [safeThis](int choice) {
        if (safeThis == nullptr || choice != 0 || safeThis->inspectorPanel_ == nullptr)
            return;
        safeThis->inspectorPanel_->dropIllegalValue();
        safeThis->requestQuit();
    });
    return false;
}

void WorkbenchComponent::useEditedSample(const std::filesystem::path& file)
{
    auto example = talas::app::sampleEditorExampleSound(projectBridge_.editor().document(), file);
    const auto definitionId = example.definition.definitionId;
    const auto displayName = example.definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(example.definition)); !added.ok())
    {
        setStatus("The sample's Sound was refused: " + juceString(added.message));
        return;
    }
    documentChanged();
    for (const auto& resource : example.resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    setStatus("Saved the sample and added the Sound " + juceString(displayName));
}

void WorkbenchComponent::showGlobalMap()
{
    if (globalMapPanel_ == nullptr)
    {
        globalMapPanel_ = makeGlobalMapEditorPanel(
            projectBridge_,
            [this] { documentChanged(); },
            [this](const juce::String& status) { setStatus(status); });
    }
    globalMapPanel_->refresh();
    if (isShowing())
    {
        if (globalMapWindow_ == nullptr)
            globalMapWindow_ = makeToolWindow("Global Map", *globalMapPanel_);
        globalMapWindow_->setVisible(true);
        globalMapWindow_->toFront(true);
    }
    setStatus("Global Map: one association a line; Apply to use it");
}

void WorkbenchComponent::openGlobalMapFile(const std::filesystem::path& path)
{
    showGlobalMap();
    (void) globalMapPanel_->openFile(path);
}

void WorkbenchComponent::chooseGlobalMap()
{
    // The Global Maps folder of the first frequently used folder, else of
    // the user's documents, with Default.map in it.
    const auto base = preferences_.frequentlyUsedFolders.empty()
        ? filesystemPath(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Talas").getFullPathName())
        : talas::project::pathFromUtf8(preferences_.frequentlyUsedFolders.front());
    const auto defaultMap = talas::app::defaultGlobalMapFile(base);
    auto chooser = std::make_shared<juce::FileChooser>("Choose the global map",
        defaultMap.has_value() ? juce::File{jucePathString(*defaultMap)} : juce::File{}, "*.map");
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& chosen) {
            if (safeThis != nullptr && chosen.getResult() != juce::File{})
                (void) safeThis->useGlobalMapFile(filesystemPath(chosen.getResult().getFullPathName()));
        });
}

bool WorkbenchComponent::useGlobalMapFile(const std::filesystem::path& path)
{
    const auto text = talas::app::globalMapFileText(path);
    const auto used = text.has_value() ? projectBridge_.editor().setGlobalMap(*text) : talas::project::EditResult{};
    if (!text.has_value() || !used.ok())
    {
        setStatus("The global map could not be used: " + juceString(text.has_value() ? used.message : "the file could not be read"));
        return false;
    }
    documentChanged();
    if (globalMapPanel_ != nullptr)
        globalMapPanel_->refresh();
    setStatus("The project's Event Values follow the global map " + juceString(path.stem().string()));
    return true;
}

void WorkbenchComponent::chooseMidiFileForTimeline()
{
    if (fileChooserActive_)
    {
        setStatus("Finish the active file dialog first");
        return;
    }
    fileChooser_ = std::make_unique<juce::FileChooser>("Import a MIDI File to the Timeline", juce::File{}, "*.mid;*.midi");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            if (chooser.getResult() == juce::File{})
                return;
            (void)safeThis->importMidiFileToTimeline(
                talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
        });
}

bool WorkbenchComponent::importMidiFileToTimeline(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    const auto seconds = talas::app::standardMidiFileSeconds(bytes);
    if (!seconds.has_value())
    {
        setStatus("The file is not a Standard MIDI File");
        return false;
    }
    auto sound = talas::app::midiFileTimelineSound(projectBridge_.editor().document(), file);
    const auto definitionId = sound.definition.definitionId;
    const auto displayName = sound.definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(sound.definition)); !added.ok())
    {
        setStatus("The MIDI file's Sound was refused: " + juceString(added.message));
        return false;
    }
    documentChanged();
    for (const auto& resource : sound.resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    const auto edits = talas::app::midiFileTimelineEdits(projectBridge_.editor().document(), definitionId, displayName, *seconds);
    if (const auto placed = projectBridge_.editor().applyScoreEdits(edits); !placed.ok())
    {
        setStatus("The MIDI file's track was refused: " + juceString(placed.message));
        return false;
    }
    documentChanged();
    setStatus("Imported " + juceString(displayName) + " to a new Timeline track");
    return true;
}

void WorkbenchComponent::showClassEditor()
{
    if (classEditorPanel_ == nullptr)
    {
        classEditorPanel_ = makeClassEditorPanel(
            projectBridge_,
            registry_,
            [this] { documentChanged(); },
            [this](const juce::String& status) { setStatus(status); });
    }
    classEditorPanel_->refresh();
    if (isShowing())
    {
        if (classEditorWindow_ == nullptr)
            classEditorWindow_ = makeToolWindow("Class Editor", *classEditorPanel_, {}, [this] {
                return closeClassEditor(std::nullopt);
            });
        classEditorWindow_->setVisible(true);
        classEditorWindow_->toFront(true);
    }
    setStatus("Class Editor: choose a Sound and a parameter, then Apply");
}

// Playing a Sound whose variables no declaration names asks for their values
// and keeps them in the environment, so the next play does not ask; true
// while the request is open.
bool WorkbenchComponent::askForGenericSource(std::function<void()> then)
{
    if (std::exchange(genericSourceAnswered_, false))
        return false;
    const auto& document = infoDisplayStage_.has_value() ? infoDisplayStage_->document : projectBridge_.editor().document();
    if (talas::app::genericSourcesAsking(document).empty())
        return false;
    const auto answered = [this](talas::app::GenericSourceAnswer answer) {
        auto stage = infoDisplayStage_.value_or(talas::app::InfoDisplayDocument{projectBridge_.editor().document(), {}, {}});
        stage.document = talas::app::documentWithGenericSourceAnswer(std::move(stage.document), answer);
        infoDisplayStage_ = std::move(stage);
        currentDocumentStaged_ = false;
    };
    if (smokeGenericSourceAnswer_.has_value())
    {
        answered(*smokeGenericSourceAnswer_);
        return false;
    }
    auto* window = new juce::AlertWindow("GenericSource", "Play the source from the disk, the live input or a sample in RAM?",
        juce::MessageBoxIconType::QuestionIcon, this);
    window->addButton("Disk", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Live", 2);
    window->addButton("RAM", 3);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, answered, then](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            answered(result == 2 ? talas::app::GenericSourceAnswer::Live
                    : result == 3 ? talas::app::GenericSourceAnswer::Ram
                                  : talas::app::GenericSourceAnswer::Disk);
            safeThis->genericSourceAnswered_ = true;
            if (then)
                then();
            else
                safeThis->playCurrentProject();
        }),
        true);
    return true;
}

bool WorkbenchComponent::askForVariableValues(std::function<void()> then, bool askGenericSource)
{
    if (askGenericSource && askForGenericSource(then))
        return true;
    std::vector<std::string> missing;
    for (auto& name : talas::project::undeclaredProjectVariables(projectBridge_.editor().document()))
    {
        if (std::ranges::none_of(variableEnvironment_, [&](const auto& entry) { return entry.first == name; })
            && std::ranges::none_of(variableLiteralEnvironment_, [&](const auto& entry) { return entry.first == name; }))
            missing.push_back(std::move(name));
    }
    // A Variable or SoundCollectionVariable no declaration names takes a
    // Sound, not a typed value: the Default Sound or Default Collection
    //.
    std::vector<talas::app::UndeclaredSoundVariable> sounds;
    for (auto& variable : talas::app::undeclaredSoundVariables(projectBridge_.editor().document()))
    {
        if (std::ranges::none_of(soundVariableEnvironment_, [&](const auto& entry) { return entry.first == variable.variableId; }))
            sounds.push_back(std::move(variable));
    }
    if (missing.empty() && sounds.empty())
        return false;
    ++variableValueRequests_;
    const auto giveDefaults = [this, sounds] {
        const auto environment = talas::app::defaultSoundEnvironment(
            sounds, defaultSound_.value_or(talas::app::factoryDefaultSound(registry_)), defaultCollection_);
        soundVariableEnvironment_.insert(soundVariableEnvironment_.end(), environment.begin(), environment.end());
        currentDocumentStaged_ = false;
    };
    // A value typed and taken with Value or Enter is a number, string,
    // symbol, character or array: a number joins the numbers, the rest the literals.
    const auto give = [](WorkbenchComponent& workbench, const std::string& name, talas::project::VariableLiteral value) {
        if (value.kind == talas::project::VariableLiteralKind::Number)
            workbench.variableEnvironment_.emplace_back(name, value.number);
        else
            workbench.variableLiteralEnvironment_ = talas::app::withVariableValuesGiven(
                std::move(workbench.variableLiteralEnvironment_), {{name, std::move(value)}});
    };
    if (smokeVariableText_.has_value() || smokeVariableValue_.has_value())
    {
        for (const auto& name : missing)
        {
            const auto answer = smokeVariableText_.has_value()
                ? talas::app::variableDialogAnswer(name, *smokeVariableText_, talas::app::VariableDialogPress::Enter)
                : talas::app::VariableDialogAnswer{talas::project::numberLiteral(*smokeVariableValue_), {}};
            if (answer.value.has_value())
                give(*this, name, *answer.value);
        }
        giveDefaults();
        currentDocumentStaged_ = false;
        return false;
    }
    auto* window = new juce::AlertWindow("Variable values", "Give each variable a value to play the Sound with",
        juce::MessageBoxIconType::QuestionIcon, this);
    for (const auto& name : missing)
        window->addTextEditor(juceString(name), "0", juceString("?" + name));
    for (const auto& variable : sounds)
        window->addTextBlock(juceString("?" + variable.variableId
            + (variable.collection ? " takes the default collection" : " takes the default Sound")));
    if (!missing.empty())
        window->addButton("Value", 1, juce::KeyPress{juce::KeyPress::returnKey});
    if (std::ranges::any_of(sounds, [](const auto& variable) { return !variable.collection; }))
        window->addButton("Default Sound", 2, missing.empty() ? juce::KeyPress{juce::KeyPress::returnKey} : juce::KeyPress{});
    if (std::ranges::any_of(sounds, [](const auto& variable) { return variable.collection; }))
        window->addButton("Default Collection", 3);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window, missing, giveDefaults, give, then](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            std::vector<std::pair<std::string, talas::project::VariableLiteral>> given;
            for (const auto& name : missing)
            {
                const auto answer = talas::app::variableDialogAnswer(name,
                    window->getTextEditorContents(juceString(name)).toStdString(), talas::app::VariableDialogPress::ValueButton);
                if (!answer.value.has_value())
                {
                    safeThis->setStatus(juceString(answer.refusal));
                    return;
                }
                given.emplace_back(name, *answer.value);
            }
            for (auto& [name, value] : given)
                give(*safeThis, name, std::move(value));
            safeThis->currentDocumentStaged_ = false;
            giveDefaults();
            if (then)
                then();
            else
                safeThis->playCurrentProject();
        }),
        true);
    return true;
}

talas::project::VariableLiteralEnvironment WorkbenchComponent::wholeVariableEnvironment() const
{
    talas::project::VariableLiteralEnvironment environment;
    for (const auto& [name, value] : variableEnvironment_)
        environment.emplace_back(name, talas::project::numberLiteral(value));
    for (const auto& entry : variableLiteralEnvironment_)
    {
        if (std::ranges::none_of(environment, [&](const auto& given) { return given.first == entry.first; }))
            environment.push_back(entry);
    }
    return environment;
}

// Info > Environment: each variable and its value.
void WorkbenchComponent::showVariableEnvironment()
{
    if (variableEnvironment_.empty() && variableLiteralEnvironment_.empty() && soundVariableEnvironment_.empty())
    {
        setStatus("Environment: no variable has a value yet");
        return;
    }
    juce::String text;
    for (const auto& [name, value] : variableEnvironment_)
        text << "?" << juceString(name) << " -> " << juce::String(value) << "\n";
    // Strings, symbols, characters and arrays as Smalltalk prints them.
    for (const auto& line : talas::app::variableEnvironmentLines(variableLiteralEnvironment_))
        text << juceString(line) << "\n";
    for (const auto& [name, sounds] : soundVariableEnvironment_)
    {
        juce::StringArray names;
        for (const auto& sound : sounds)
            names.add(juceString(sound.displayName));
        text << "?" << juceString(name) << " -> " << names.joinIntoString(", ") << "\n";
    }
    setStatus("Environment: " + text.trim().replace("\n", ", "));
    if (isShowing())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Environment", text.trim(), {}, this);
}

// Reset environment clears the values; the next play asks for them again.
void WorkbenchComponent::resetVariableEnvironment()
{
    if (!variableEnvironment_.empty() || !variableLiteralEnvironment_.empty() || !soundVariableEnvironment_.empty())
    {
        variableEnvironment_.clear();
        variableLiteralEnvironment_.clear();
        soundVariableEnvironment_.clear();
        currentDocumentStaged_ = false;
    }
    setStatus("Environment reset: playing asks for the variables' values again");
}

namespace
{
// The memory this program holds, its footprint as the system counts it.
[[nodiscard]] double programMemoryBytes()
{
#if JUCE_MAC
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<double>(info.phys_footprint);
#endif
    return 0.0;
}
}

// The Recycle button: what the runtime retired is
// freed, and the allocator gives back what it can.
void WorkbenchComponent::recycleMemory()
{
    (void)projectBridge_.collectRetired();
    // And the files found recently are looked for again.
    talas::app::forgetLocatedFiles(fileLocatorMemory_);
    // And the icons' pictures are loaded again.
    classIconPictures_.recycle();
    repaint();
#if JUCE_MAC
    (void)malloc_zone_pressure_relief(nullptr, 0);
#endif
    setStatus("Memory recycled");
}

DspStatusReading WorkbenchComponent::dspStatusReading() const
{
    DspStatusReading reading;
    reading.elapsedSeconds = dspStatusClock_.elapsed(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    if (latestMtcTelemetry_.follower.hasTimeCode)
    {
        const auto& code = latestMtcTelemetry_.follower.timeCode;
        reading.timeCode = std::array<unsigned, 4>{code.hour, code.minute, code.second, code.frame};
    }
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        reading.input[channel] = inputMeterPeaks_[channel].load(std::memory_order_relaxed);
        reading.output[channel] = std::max(0.0F, outputMeterPeaks_[channel].load(std::memory_order_relaxed));
    }
    reading.soundInputs = loadedSoundInputLines_;
    reading.eightChannels = preferences_.showEightChannels;
    const auto computer = static_cast<double>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024.0 * 1024.0;
    reading.memoryFraction = computer > 0.0 ? programMemoryBytes() / computer : 0.0;
    reading.processorLoad = deviceManager_.getCpuUsage();
    if (auto* device = deviceManager_.getCurrentAudioDevice())
    {
        reading.sampleRate = device->getCurrentSampleRate();
        for (const auto rate : device->getAvailableSampleRates())
            reading.deviceSampleRates.push_back(rate);
    }
    reading.animate = talas::app::interfaceRunsDuringDiskOperation(
        transportPlaying_ && talas::app::loadedSoundInputs(projectBridge_.editor().document()).usesDisk,
        preferences_.updateDisplayDuringDiskOperations);
    return reading;
}

void WorkbenchComponent::showDspStatus()
{
    if (dspStatusPanel_ == nullptr)
        dspStatusPanel_ = std::make_unique<DspStatusPanel>([this] { return dspStatusReading(); }, [this] { recycleMemory(); },
            [this](double rate) {
                // The device runs at the rate chosen.
                auto setup = deviceManager_.getAudioDeviceSetup();
                setup.sampleRate = rate;
                const auto error = deviceManager_.setAudioDeviceSetup(setup, true);
                setStatus(error.isEmpty() ? "Sample rate " + juce::String(rate, 0) + " Hz" : "The sample rate was refused: " + error);
            });
    // Memory is recycled each time the window opens.
    recycleMemory();
    dspStatusPanel_->refresh();
    if (isShowing())
    {
        if (dspStatusWindow_ == nullptr)
            dspStatusWindow_ = makeToolWindow("DSP Status", *dspStatusPanel_);
        dspStatusWindow_->setVisible(true);
        dspStatusWindow_->toFront(true);
    }
    setStatus("DSP Status: time since the Sound started, time code, levels; click to reset the peaks");
}

// The Info menu's Structure as Text: the selected
// Sound's structure, or else that of the Sound the graph in view outputs.
// The Sound selected in the Sound file window or a
// Sound Browser in front, or among the prototypes, read without opening it.
std::optional<std::string> WorkbenchComponent::frontWindowStructureText()
{
    const auto textOf = [](const talas::project::ProjectDocument& document, std::string_view definitionId) {
        const auto found = std::ranges::find(document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        return found == document.soundDefinitions.end() ? std::string{} : talas::project::soundStructureAsText(*found);
    };
    if (soundFileWindow_ != nullptr && soundFileWindow_->isActiveWindow() && soundFilePanel_ != nullptr)
    {
        const auto selected = soundFilePanel_->selectedSounds();
        if (selected.size() == 1)
            return textOf(projectBridge_.editor().document(), selected.front());
    }
    // The prototype selected while the prototypes have the focus, as the
    // Sound it would be dragged in as.
    if (const auto typeId = prototypePanel_->focusedPrototype())
    {
        const auto sound = talas::app::soundForPrototype(projectBridge_.editor().document(), registry_, *typeId);
        return sound.has_value() ? talas::project::soundStructureAsText(*sound) : std::string{};
    }
    for (const auto& browser : soundBrowsers_)
    {
        if (browser.window == nullptr || !browser.window->isActiveWindow())
            continue;
        const auto row = browser.panel->selectedRow();
        if (!row.has_value() || row->kind != talas::app::SoundBrowserKind::Sound)
            return std::string{};
        const auto file = talas::project::loadProjectFile(row->path);
        return file.ok() ? textOf(*file.document, row->definitionId) : std::string{};
    }
    return std::nullopt;
}

void WorkbenchComponent::showStructureAsText()
{
    if (const auto front = frontWindowStructureText())
    {
        showStructureText(*front);
        return;
    }
    const auto& document = projectBridge_.editor().document();
    const talas::project::ProjectGraph* graph = &document.graph;
    std::string soundId;
    if (activeGraphContext_.has_value())
    {
        const auto definition = std::ranges::find(document.soundDefinitions, *activeGraphContext_,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition != document.soundDefinitions.end())
        {
            graph = &definition->graph;
            if (!definition->outputs.empty())
                soundId = definition->outputs.front().nodeId;
        }
    }
    if (selectedNodeIds_.size() == 1)
        soundId = selectedNodeIds_.front();
    showStructureText(talas::project::projectStructureAsText(*graph, soundId));
}

void WorkbenchComponent::showStructureText(const std::string& text)
{
    if (text.empty())
    {
        setStatus("Structure as Text: select a Sound, or connect one to the output");
        return;
    }
    if (structureText_ == nullptr)
    {
        structureText_ = std::make_unique<juce::TextEditor>();
        structureText_->setMultiLine(true);
        structureText_->setReadOnly(true);
        structureText_->setFont(WorkbenchType::data());
        structureText_->setSize(480, 360);
    }
    structureText_->setText(juceString(text), false);
    if (isShowing())
    {
        if (structureWindow_ == nullptr)
            structureWindow_ = makeToolWindow("Structure as Text", *structureText_);
        structureWindow_->setVisible(true);
        structureWindow_->toFront(true);
    }
    setStatus("Structure as Text: each Sound with its inputs, indented by level");
}

void WorkbenchComponent::showAlternateTunings()
{
    if (alternateTuningsPanel_ == nullptr)
    {
        alternateTuningsPanel_ = makeAlternateTuningsPanel(
            [this](const juce::String& status) { setStatus(status); },
            [this](const talas::project::tunings::Tuning& tuning, const talas::project::tunings::KeyFrequencies& frequencies,
                talas::app::TuningExampleInstrument instrument, const std::filesystem::path& sample) {
                return addTuningExample(tuning, frequencies, instrument, sample);
            });
    }
    if (isShowing())
    {
        if (alternateTuningsWindow_ == nullptr)
            alternateTuningsWindow_ = makeToolWindow("Design Alternate Tunings", *alternateTuningsPanel_, [this] { stopTuningTryOut(); });
        alternateTuningsWindow_->setVisible(true);
        alternateTuningsWindow_->toFront(true);
    }
    // Throughout the Tool the MIDI keyboard tries the tuning.
    alternateTuningsPanel_->setTryOut([this](const talas::project::tunings::Tuning& tuning,
                                          const talas::project::tunings::KeyFrequencies& frequencies,
                                          talas::app::TuningExampleInstrument instrument, const std::filesystem::path& sample) {
        tryOutTuning(tuning, frequencies, instrument, sample);
    });
    setStatus("Design Alternate Tunings: play the MIDI keyboard to try the tuning; Create Example to keep it");
}

void WorkbenchComponent::tryOutTuning(const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies, talas::app::TuningExampleInstrument instrument,
    const std::filesystem::path& sample)
{
    std::string error;
    auto tryOut = talas::app::tuningTryOut(projectBridge_.editor().document(), tuning, frequencies, instrument, sample, error);
    if (!tryOut)
    {
        setStatus(juceString(error));
        return;
    }
    // The sample instrument's file, bound to its node's field.
    for (const auto& resource : tryOut->resources)
    {
        const auto asset = makeAssetReference(resource.file);
        for (auto& node : tryOut->document.graph.nodes)
        {
            if (node.instanceId != resource.nodeId)
                continue;
            talas::project::ProjectParameterExpression field;
            field.parameterId = resource.parameterId;
            field.resource = asset.id;
            std::erase_if(node.parameters, [&](const auto& parameter) { return parameter.parameterId == resource.parameterId; });
            node.parameters.push_back(std::move(field));
        }
        tryOut->document.assets.push_back(asset);
    }
    stopSoundBrowserAudition();
    tuningTryOutAudition_ = nullptr;
    if (startSoundBrowserAudition(std::move(tryOut->document), projectPath().value_or(std::filesystem::path{}), 2))
        tuningTryOutAudition_ = soundBrowserAudition_.get();
}

void WorkbenchComponent::stopTuningTryOut() noexcept
{
    // Only the audition the Tool started; the Sound Browser may have
    // started another since.
    if (tuningTryOutAudition_ != nullptr && soundBrowserAudition_.get() == tuningTryOutAudition_)
        stopSoundBrowserAudition();
    tuningTryOutAudition_ = nullptr;
}

bool WorkbenchComponent::addTuningExample(const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies, talas::app::TuningExampleInstrument instrument,
    const std::filesystem::path& sample)
{
    std::string error;
    auto example = talas::app::tuningExampleSound(
        projectBridge_.editor().document(), tuning, frequencies, instrument, sample, error);
    if (!example)
    {
        setStatus(juceString(error));
        return false;
    }
    const auto definitionId = example->definition.definitionId;
    const auto displayName = example->definition.displayName;
    const auto held = [this](std::string_view name) {
        return std::ranges::any_of(projectBridge_.editor().document().virtualControls,
            [&](const auto& existing) { return existing.eventName == name; });
    };
    std::vector<bool> heldBefore;
    for (const auto& control : example->controls)
        heldBefore.push_back(held(control.eventName));
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(example->definition)); !added.ok())
    {
        setStatus("The tuning's example was refused: " + juceString(added.message));
        return false;
    }
    // Its Volume fader starts at its top, unless the project had a Volume;
    // one the Sound's !Volume has just brought starts there too.
    for (std::size_t index = 0; index < example->controls.size(); ++index)
    {
        auto& control = example->controls[index];
        if (heldBefore[index])
            continue;
        if (held(control.eventName))
            (void) projectBridge_.editor().setVirtualControlValue(control.eventName, control.value);
        else
            (void) projectBridge_.editor().addVirtualControl(std::move(control));
    }
    documentChanged();
    for (const auto& resource : example->resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    setStatus("Added the Sound " + juceString(displayName) + ": play it from the MIDI keyboard");
    return true;
}

bool WorkbenchComponent::closeClassEditor(std::optional<bool> answer)
{
    if (newClassStart_.has_value() && classEditorPanel_ != nullptr)
    {
        const auto& definitions = projectBridge_.editor().document().soundDefinitions;
        const auto made = std::ranges::find(definitions, classEditorPanel_->selectedDefinition(),
            &talas::project::ProjectSoundDefinition::definitionId);
        const auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Close the Class Editor")
            .withMessage(juceString(talas::app::newClassCloseQuestion(made == definitions.end() ? "" : made->displayName)))
            .withButton("Yes")
            .withButton("No")
            .withButton("Cancel")
            .withAssociatedComponent(this);
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        juce::NativeMessageBox::showAsync(options, [safeThis, answer](int choice) {
            if (safeThis == nullptr || choice == 2)
                return;
            if (safeThis->keepNewClass(choice == 0) && choice == 0)
                (void) safeThis->closeClassEditor(answer);
        });
        return false;
    }
    if (classEditorPanel_ == nullptr || !classEditorPanel_->hasChangesNotKept())
    {
        if (classEditorWindow_ != nullptr)
            classEditorWindow_->setVisible(false);
        return true;
    }
    if (answer.has_value())
        return keepOrDropClassChanges(*answer);
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::QuestionIcon)
        .withTitle("Close the Class Editor")
        .withMessage("Save the changes made to the class?")
        .withButton("Save")
        .withButton("Discard")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(options, [safeThis](int choice) {
        if (safeThis != nullptr && (choice == 0 || choice == 1))
            (void) safeThis->keepOrDropClassChanges(choice == 0);
    });
    return false;
}

bool WorkbenchComponent::keepOrDropClassChanges(bool save)
{
    if (save && !classEditorPanel_->applyChanges())
        return false;
    if (!save)
    {
        classEditorPanel_->refresh();
        setStatus("Dropped the class's changes");
    }
    if (classEditorWindow_ != nullptr)
        classEditorWindow_->setVisible(false);
    return true;
}

bool WorkbenchComponent::keepNewClass(bool keep)
{
    if (!newClassStart_.has_value())
        return false;
    auto start = std::move(*newClassStart_);
    newClassStart_.reset();
    const auto inSoundFile = std::exchange(newClassInSoundFile_, std::nullopt);
    if (keep)
    {
        if (inSoundFile.has_value())
            saveNewClassInstanceInSoundFile(*inSoundFile);
        else
            setStatus("The new class's instance stays where the example was; double-click it to change its values");
        return true;
    }
    if (const auto restored = projectBridge_.editor().applyDocumentEdit(std::move(start)); !restored.ok())
    {
        setStatus("Could not start over: " + juceString(restored.message));
        return false;
    }
    documentChanged();
    if (classEditorPanel_ != nullptr)
        classEditorPanel_->refresh();
    if (classEditorWindow_ != nullptr)
        classEditorWindow_->setVisible(false);
    setStatus("Started over without the new class");
    return true;
}
}
