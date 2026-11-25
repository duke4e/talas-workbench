#include "app/workbench/WorkbenchComponentInternal.h"

#include "app/workbench/WorkbenchLiveGridEditor.h"

#include "talas/app/GraphBranchLayout.h"
#include "talas/app/SampleEditorModel.h"
#include "talas/app/SoundClassIcon.h"
#include "talas/app/SoundIcons.h"
#include "talas/app/SoundBrowserAudition.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/VariableValuePrompt.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"
#include "talas/runtime/ScriptExpansion.h"

#include <fstream>
#include <iterator>

// The Sound file window: the project's Sounds as icons, played as the Sound
// Browser plays a Sound, through its replacement input, and opened,
// renamed, duplicated and deleted.
namespace talas::app::workbench
{
namespace
{
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundFileAt(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    auto loaded = talas::project::parseProjectDocument(
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}});
    if (!loaded.ok())
        return std::nullopt;
    return std::move(loaded.document);
}
}

void WorkbenchComponent::showSoundFileWindow()
{
    if (soundFilePanel_ == nullptr)
    {
        SoundFileHost host;
        // Every Sound with an editor open is marked.
        host.icons = [this](const std::optional<std::string>& level) {
            syncSoundEditors();
            return talas::app::soundFileIcons(projectBridge_.editor().document(), level, talas::app::soundsBeingEdited(soundEditors_));
        };
        host.parentOf = [this](const std::string& collectionId) {
            return talas::app::soundFileParentCollection(projectBridge_.editor().document(), collectionId);
        };
        host.collectionName = [this](const std::string& collectionId) -> std::optional<std::string> {
            const auto& collections = projectBridge_.editor().document().soundCollections;
            const auto found = std::ranges::find(collections, collectionId, &talas::project::ProjectSoundCollection::collectionId);
            return found == collections.end() ? std::nullopt : std::optional<std::string>{found->name};
        };
        host.collect = [this](const std::optional<std::string>& level, const std::vector<std::string>& sounds,
                           const std::vector<std::string>& collections, const std::optional<std::string>& name) {
            collectSounds(level, sounds, collections, name);
        };
        host.renameCollection = [this](const std::string& collectionId) { renameSoundCollection(collectionId); };
        host.removeCollections = [this](const std::vector<std::string>& collectionIds) {
            for (const auto& collectionId : collectionIds)
            {
                if (const auto removed = projectBridge_.editor().removeSoundCollection(collectionId); !removed.ok())
                {
                    setStatus(juceString(removed.message));
                    return;
                }
            }
            documentChanged();
            setStatus("Deleted the collection; its Sounds stay in the Sound file");
        };
        host.duplicateCollections = [this](const std::vector<std::string>& collectionIds) {
            for (const auto& collectionId : collectionIds)
            {
                std::string duplicate;
                if (const auto duplicated = projectBridge_.editor().duplicateSoundCollection(collectionId, duplicate);
                    !duplicated.ok())
                {
                    setStatus(juceString(duplicated.message));
                    return;
                }
            }
            documentChanged();
            setStatus("Duplicated the collection and each of its Sounds");
        };
        host.describe = [this](const std::string& definitionId) {
            talas::project::ProjectNode instance;
            instance.instanceId = "soundFileIcon";
            instance.soundDefinitionId = definitionId;
            return talas::project::describeProjectNode(projectBridge_.editor().document(), instance, registry_);
        };
        host.open = [this](const std::string& definitionId) { showSoundDefinition(definitionId); };
        host.bringEditorToFront = [this](const std::string& definitionId) { bringSoundEditorToFront(definitionId); };
        host.play = [this](const std::string& definitionId) { playFromSoundFileWindow(definitionId); };
        host.playInTurn = [this](const std::vector<std::string>& selected) { playInTurnFromSoundFileWindow(selected); };
        host.pauseOrResume = [this](const std::string& definitionId) {
            // Space pauses and resumes the Sound playing, else plays the
            // selected one.
            if (soundBrowserAudition_ != nullptr && soundFileLastPlayed_ == definitionId)
                pauseOrResumeSoundBrowser();
            else
                playFromSoundFileWindow(definitionId);
        };
        host.stop = [this] { stopSoundBrowser(); };
        host.rename = [this](const std::string& definitionId) { renameSoundDefinition(definitionId); };
        host.remove = [this](const std::vector<std::string>& definitionIds) {
            const auto name = [this](const std::string& definitionId) {
                const auto& definitions = projectBridge_.editor().document().soundDefinitions;
                const auto found = std::ranges::find(definitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
                return juceString(talas::app::soundRemovalQuestion(
                    found == definitions.end() || found->displayName.empty() ? definitionId : found->displayName, {}));
            };
            askToRemove({{}, definitionIds}, name, [this](std::vector<std::string> removed) {
                for (auto& definitionId : removed)
                    deleteSoundDefinition(std::move(definitionId));
            });
        };
        host.duplicate = [this](const std::vector<std::string>& definitionIds) { duplicateSoundDefinitions(definitionIds); };
        host.move = [this](const std::vector<std::string>& definitionIds, const std::string& before) {
            auto moved = talas::app::soundFileWindowMoved(projectBridge_.editor().document(), definitionIds, before);
            if (!moved.has_value())
                return;
            if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(*moved)); !applied.ok())
            {
                setStatus("The Sounds could not move: " + juceString(applied.message));
                return;
            }
            documentChanged();
            setStatus("Moved the Sounds in the Sound file window");
        };
        // Copy and Paste of Sounds.
        host.copy = [this](const std::vector<std::string>& definitionIds) {
            setGraphClipboardText(juceString(talas::project::serializeProjectGraphFragment(talas::app::soundFileWindowCopied(definitionIds))));
            setStatus("Copied " + juce::String{static_cast<int>(definitionIds.size())} + " Sound(s)");
        };
        host.paste = [this] { pasteIntoSoundFile(); };
        host.expand = [this](const std::vector<std::string>& definitionIds) { expandSoundDefinitions(definitionIds); };
        host.addPrototype = [this](const std::string& typeId) { (void) addSoundForPrototype(typeId); };
        host.addDragged = [this](const std::string& description) { (void) importBrowserDrag(description); };
        host.classIcon = [this](const std::string& definitionId) -> std::optional<talas::project::ProjectSoundIcon> {
            const auto& definitions = projectBridge_.editor().document().soundDefinitions;
            const auto found = std::ranges::find(definitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
            return found == definitions.end() ? std::nullopt : found->icon;
        };
        host.classImage = [this](const std::string& definitionId) { return classIconImage(definitionId); };
        host.glyphs = [this](const std::string& definitionId) { return classIconGlyphs(definitionId); };
        soundFilePanel_ = makeSoundFilePanel(std::move(host));
    }
    soundFilePanel_->refresh();
    if (isShowing())
    {
        if (soundFileWindow_ == nullptr)
            soundFileWindow_ = makeToolWindow("Sound File", *soundFilePanel_);
        soundFileWindow_->setVisible(true);
        soundFileWindow_->toFront(true);
    }
    // The Sound file window in front, for
    // Undo and New Class from Example.
    frontWindowName_ = "Sound File";
    updateHistoryWindow();
    setStatus("Sound file: Ctrl+Space plays the selected Sound, a double-click opens it, Enter renames it");
}

void WorkbenchComponent::playFromSoundFileWindow(const std::string& definitionId)
{
    stopSoundBrowser();
    const auto& document = projectBridge_.editor().document();
    const auto inputFile = soundBrowserInput_.kind == talas::app::SoundBrowserInputKind::Sound
        ? soundFileAt(soundBrowserInput_.path)
        : std::optional<talas::project::ProjectDocument>{};
    auto audition = talas::app::soundBrowserAuditionDocument(document, definitionId, soundBrowserInput_, inputFile, registry_);
    if (!audition.ok())
    {
        setStatus("The Sound cannot play: " + juceString(audition.diagnostic));
        return;
    }
    const auto start = [this, definitionId, outputs = audition.outputChannelCount](
                           talas::project::ProjectDocument played, std::vector<std::pair<std::string, double>> variables) {
        if (!startSoundBrowserAudition(std::move(played), projectPath().value_or(std::filesystem::path{}), outputs, std::move(variables)))
            return;
        soundFileLastPlayed_ = definitionId;
        const auto& sounds = projectBridge_.editor().document().soundDefinitions;
        const auto sound = std::ranges::find(sounds, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        setStatus("Playing " + juceString(sound == sounds.end() || sound->displayName.empty() ? definitionId : sound->displayName));
    };
    // The Sound file window
    // keeps no environment, so a lifted Sound asks its values each time.
    const auto asked = talas::app::variablesToAskFor(*audition.document, {}, talas::app::VariablePlayOrigin::SoundFileWindow);
    if (asked.empty())
    {
        start(std::move(*audition.document), {});
        return;
    }
    ++variableValueRequests_;
    if (smokeVariableValue_.has_value())
    {
        std::vector<std::pair<std::string, double>> given;
        for (const auto& name : asked)
            given.emplace_back(name, *smokeVariableValue_);
        start(std::move(*audition.document), std::move(given));
        return;
    }
    auto* window = new juce::AlertWindow("Variable values", "Give each variable a value to play the Sound with",
        juce::MessageBoxIconType::QuestionIcon, this);
    for (const auto& name : asked)
        window->addTextEditor(juceString(name), "0", juceString("?" + name));
    window->addButton("Value", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safeThis, window, asked, start, played = std::make_shared<talas::project::ProjectDocument>(std::move(*audition.document))](int result) {
                if (safeThis == nullptr || result == 0)
                    return;
                // A number, string, symbol, character or array, for this
                // play.
                std::vector<std::pair<std::string, double>> given;
                talas::project::VariableLiteralEnvironment literals;
                for (const auto& name : asked)
                {
                    const auto answer = talas::app::variableDialogAnswer(name,
                        window->getTextEditorContents(juceString(name)).toStdString(), talas::app::VariableDialogPress::ValueButton);
                    if (!answer.value.has_value())
                    {
                        safeThis->setStatus(juceString(answer.refusal));
                        return;
                    }
                    if (answer.value->kind == talas::project::VariableLiteralKind::Number)
                        given.emplace_back(name, answer.value->number);
                    else
                        literals.emplace_back(name, *answer.value);
                }
                start(talas::project::withVariableLiterals(std::move(*played), literals), std::move(given));
            }),
        true);
}

// The selected Sounds one after another, through a
// Concatenation of them in a copy of the project; one Sound plays alone.
void WorkbenchComponent::playInTurnFromSoundFileWindow(const std::vector<std::string>& selected)
{
    auto document = projectBridge_.editor().document();
    auto turn = talas::app::soundsInTurn(document, selected);
    if (!turn.has_value())
    {
        const auto single = std::ranges::find_if(selected, [&](const std::string& id) {
            return std::ranges::any_of(document.soundDefinitions, [&](const auto& sound) { return sound.definitionId == id; });
        });
        if (single != selected.end())
            playFromSoundFileWindow(*single);
        return;
    }
    const auto count = turn->graph.connections.size();
    if (playAddedSound(std::move(document), std::move(*turn)))
        setStatus("Playing " + juce::String{static_cast<int>(count)} + " Sounds one after another");
}

// Ctrl+Space in the prototypes plays the
// prototype, as the Sound it is dragged in as, without adding it.
void WorkbenchComponent::playPrototype(const std::string& typeId)
{
    auto document = projectBridge_.editor().document();
    auto sound = talas::app::soundForPrototype(document, registry_, typeId);
    if (!sound.has_value())
    {
        setStatus("The prototype has no audio output to play");
        return;
    }
    const auto name = juceString(sound->displayName);
    if (playAddedSound(std::move(document), std::move(*sound)))
        setStatus("Playing the prototype " + name);
}

bool WorkbenchComponent::playAddedSound(talas::project::ProjectDocument document, talas::project::ProjectSoundDefinition sound)
{
    stopSoundBrowser();
    const auto definitionId = sound.definitionId;
    document.soundDefinitions.push_back(std::move(sound));
    const auto inputFile = soundBrowserInput_.kind == talas::app::SoundBrowserInputKind::Sound
        ? soundFileAt(soundBrowserInput_.path)
        : std::optional<talas::project::ProjectDocument>{};
    auto audition = talas::app::soundBrowserAuditionDocument(document, definitionId, soundBrowserInput_, inputFile, registry_);
    if (!audition.ok())
    {
        setStatus("The Sound cannot play: " + juceString(audition.diagnostic));
        return false;
    }
    if (!startSoundBrowserAudition(std::move(*audition.document), projectPath().value_or(std::filesystem::path{}),
            audition.outputChannelCount))
        return false;
    soundFileLastPlayed_.reset();
    return true;
}

// Open... with Custom prototypes as the file type:
// a Sound file shown as a strip of prototypes.
void WorkbenchComponent::openCustomPrototypes()
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    fileChooser_ = std::make_unique<juce::FileChooser>("Open Custom Prototypes",
        projectPath().has_value() ? juce::File{jucePathString(projectPath()->parent_path())} : juce::File{}, "*.talas.json");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            if (chooser.getResult() != juce::File{})
                (void)safeThis->showCustomPrototypes(filesystemPath(chooser.getResult().getFullPathName()));
        });
}

bool WorkbenchComponent::showCustomPrototypes(const std::filesystem::path& path)
{
    const auto file = soundFileAt(path);
    if (!file.has_value())
    {
        setStatus("The custom prototypes are not a Sound file Talas can read");
        return false;
    }
    if (customPrototypesPanel_ == nullptr)
    {
        CustomPrototypesHost host;
        // A prototype is copied into the project's Sounds to build on.
        host.add = [this](const std::string& definitionId) {
            const auto prototypes = soundFileAt(customPrototypesPath_);
            if (!prototypes.has_value())
            {
                setStatus("The custom prototypes' Sound file can no longer be read");
                return;
            }
            auto imported = talas::app::soundBrowserImport(projectBridge_.editor().document(), *prototypes, definitionId,
                talas::app::SoundBrowserReplacementInput{}, std::nullopt, registry_);
            if (!imported.ok())
            {
                setStatus("The prototype could not be copied: " + juceString(imported.diagnostic));
                return;
            }
            if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(*imported.document)); !applied.ok())
            {
                setStatus("The prototype was refused: " + juceString(applied.message));
                return;
            }
            documentChanged();
            setStatus("Copied the prototype " + juceString(imported.rootDefinitionId) + " into the project's Sounds");
        };
        host.play = [this](const std::string& definitionId) {
            (void)playFromSoundBrowser({customPrototypesPath_, definitionId, definitionId, 1, talas::app::SoundBrowserKind::Sound,
                false, false});
        };
        host.stop = [this] { stopSoundBrowser(); };
        customPrototypesPanel_ = makeCustomPrototypesPanel(std::move(host));
    }
    customPrototypesPath_ = path;
    customPrototypesPanel_->setStrip(juceString(path.filename().string()), talas::app::customPrototypeStrip(*file));
    if (isShowing())
    {
        if (customPrototypesWindow_ == nullptr)
            customPrototypesWindow_ = makeToolWindow("Custom Prototypes", *customPrototypesPanel_);
        customPrototypesWindow_->setName("Custom Prototypes: " + juceString(path.filename().string()));
        customPrototypesWindow_->setVisible(true);
        customPrototypesWindow_->toFront(true);
    }
    setStatus("Custom prototypes: Return copies the selected Sound into the project, Ctrl+Space plays it");
    return true;
}

// Collect...: the name asked for, then the
// selected Sounds and collections moved into a new collection of it.
void WorkbenchComponent::collectSounds(const std::optional<std::string>& level, const std::vector<std::string>& sounds,
    const std::vector<std::string>& collections, const std::optional<std::string>& name)
{
    const auto finish = [this, level, sounds, collections](const std::string& chosen) {
        std::string collectionId;
        const auto collected = projectBridge_.editor().collectSounds(
            level.has_value() ? std::optional<std::string_view>{*level} : std::nullopt, sounds, collections, chosen,
            collectionId);
        if (!collected.ok())
        {
            setStatus(juceString(collected.message));
            return;
        }
        documentChanged();
        setStatus("Collected " + juce::String{static_cast<int>(sounds.size() + collections.size())} + " into "
            + juceString(chosen));
    };
    if (name.has_value())
    {
        finish(*name);
        return;
    }
    soundDialog_ = std::make_unique<juce::AlertWindow>(
        "Collect", "The name of the new Sound collection", juce::MessageBoxIconType::NoIcon, this);
    soundDialog_->addTextEditor("collectionName", "Collection", "Name");
    soundDialog_->addButton("Collect", 1, juce::KeyPress(juce::KeyPress::returnKey));
    soundDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    soundDialog_->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, finish](int result) {
            if (safeThis == nullptr || safeThis->soundDialog_ == nullptr)
                return;
            const auto chosen = safeThis->soundDialog_->getTextEditorContents("collectionName").toStdString();
            safeThis->soundDialog_.reset();
            if (result == 1 && !chosen.empty())
                finish(chosen);
        }),
        false);
}

void WorkbenchComponent::renameSoundCollection(const std::string& collectionId)
{
    const auto& collections = projectBridge_.editor().document().soundCollections;
    const auto found = std::ranges::find(collections, collectionId, &talas::project::ProjectSoundCollection::collectionId);
    if (found == collections.end())
        return;
    soundDialog_ = std::make_unique<juce::AlertWindow>(
        "Rename Sound Collection", "The collection's new name", juce::MessageBoxIconType::NoIcon, this);
    soundDialog_->addTextEditor("collectionName", juceString(found->name), "Name");
    soundDialog_->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    soundDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    soundDialog_->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, collectionId](int result) {
            if (safeThis == nullptr || safeThis->soundDialog_ == nullptr)
                return;
            const auto chosen = safeThis->soundDialog_->getTextEditorContents("collectionName").toStdString();
            safeThis->soundDialog_.reset();
            if (result != 1 || chosen.empty())
                return;
            if (const auto renamed = safeThis->projectBridge_.editor().renameSoundCollection(collectionId, chosen); !renamed.ok())
            {
                safeThis->setStatus(juceString(renamed.message));
                return;
            }
            safeThis->documentChanged();
        }),
        false);
}

// A class's PNG icon: the class's name in lowercase
// letters and .png, in a frequently used folder or the project's.
juce::Image WorkbenchComponent::classIconImage(const std::string& definitionId)
{
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    const auto found = std::ranges::find(definitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (found == definitions.end() || !found->icon.has_value() || !found->icon->png)
        return {};
    const auto path = talas::app::findSoundIconPng(found->displayName.empty() ? found->definitionId : found->displayName,
        fileOrganizerRoots());
    return path.has_value()
        ? classIconPictures_.get(*path, [](const std::filesystem::path& file) {
              return juce::ImageFileFormat::loadFrom(juce::File{jucePathString(file)});
          })
        : juce::Image{};
}

// The inner icon a Sound class shows: another Sound's when its icon is
// that one's, else its kind's.
std::vector<talas::app::IconGlyph> WorkbenchComponent::classIconGlyphs(const std::string& definitionId)
{
    const auto& document = projectBridge_.editor().document();
    talas::project::ProjectNode node;
    node.instanceId = "soundFileIcon";
    node.soundDefinitionId = definitionId;
    const auto found = std::ranges::find(document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (found != document.soundDefinitions.end() && found->icon.has_value() && !found->icon->sameAs.empty())
    {
        if (registry_.findNodeType(found->icon->sameAs) != nullptr)
        {
            node.typeId = found->icon->sameAs;
            node.soundDefinitionId.reset();
        }
        else
        {
            node.soundDefinitionId = found->icon->sameAs;
        }
    }
    const auto described = talas::project::describeProjectNode(document, node, registry_);
    return described.has_value() ? talas::app::soundIconGlyphs(*described) : std::vector<talas::app::IconGlyph>{};
}

// Duplicate: a copy of each selected Sound, a number after
// its name.
void WorkbenchComponent::duplicateSoundDefinitions(const std::vector<std::string>& definitionIds)
{
    for (const auto& definitionId : definitionIds)
    {
        const auto& document = projectBridge_.editor().document();
        const auto source = std::ranges::find(document.soundDefinitions, definitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (source == document.soundDefinitions.end())
            continue;
        const auto newId = talas::app::numberedCopyName(definitionId, [&](std::string_view candidate) {
            return std::ranges::any_of(document.soundDefinitions, [&](const auto& definition) {
                return definition.definitionId == candidate;
            });
        });
        const auto base = source->displayName.empty() ? definitionId : source->displayName;
        const auto name = talas::app::numberedCopyName(base + " ", [&](std::string_view candidate) {
            return std::ranges::any_of(document.soundDefinitions, [&](const auto& definition) {
                return definition.displayName == candidate;
            });
        });
        if (const auto duplicated = projectBridge_.editor().duplicateSoundDefinition(definitionId, newId, name);
            !duplicated.ok())
        {
            setStatus(juceString(duplicated.message));
            return;
        }
    }
    documentChanged();
    setStatus("Duplicated " + juce::String{static_cast<int>(definitionIds.size())} + " Sound(s)");
}

// The Sound editor's close box: a Sound
// changed since the editor opened on it asks Keep, Drop or Cancel; Keep
// leaves the changes, Drop gives the Sound back as it was, one undo step.
void WorkbenchComponent::closeSoundEditor()
{
    if (!secondaryGraphContext_.has_value())
        return;
    // The editor in front closes; the others stay open.
    syncSoundEditors();
    const auto* front = talas::app::frontSoundEditor(soundEditors_);
    const auto closing = front != nullptr ? front->definitionId : *secondaryGraphContext_;
    const auto opened = front != nullptr ? front->opened : std::nullopt;
    const auto close = [this, closing] {
        // The project's signal flow first, so that nothing opens the editor
        // again on the way.
        showRootGraph();
        talas::app::closeSoundEditor(soundEditors_, closing);
        refreshSoundEditors();
        if (soundFilePanel_ != nullptr)
            soundFilePanel_->refresh();
    };
    if (!opened.has_value() || opened->definitionId != *secondaryGraphContext_
        || !talas::app::soundEditorChanged(*opened, projectBridge_.editor().document()))
    {
        close();
        return;
    }
    const auto answer = [this, opened, close](int choice) {
        if (choice == 2)
            return;
        if (choice == 1)
        {
            if (auto dropped = talas::app::soundEditorDropped(projectBridge_.editor().document(), *opened))
            {
                if (const auto applied = projectBridge_.editor().applyDocumentEdit(std::move(*dropped)); !applied.ok())
                    return setStatus("The changes were not dropped: " + juceString(applied.message));
                documentChanged();
            }
        }
        close();
        setStatus(choice == 1 ? "Dropped the changes to the Sound" : "Kept the changes to the Sound");
    };
    if (smokeSoundEditorCloseAnswer_.has_value())
        return answer(*smokeSoundEditorCloseAnswer_);
    const auto name = opened->displayName.empty() ? opened->definitionId : opened->displayName;
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::QuestionIcon)
        .withTitle("Close the Sound editor")
        .withMessage(juceString("Keep the changes made to " + name + "?"))
        .withButton("Keep")
        .withButton("Drop")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(options, [safeThis, answer](int choice) {
        if (safeThis != nullptr)
            answer(choice);
    });
}

// Save as... with a Sound's editor in front: a copy
// of the Sound as it is now, saved with the project, the editor left on the
// Sound to go on developing it.
void WorkbenchComponent::saveSoundCopy(const std::string& definitionId)
{
    const auto before = projectBridge_.editor().document().soundDefinitions.size();
    duplicateSoundDefinitions({definitionId});
    if (projectBridge_.editor().document().soundDefinitions.size() != before)
        saveProject();
}

void WorkbenchComponent::askToRemove(talas::app::SoundRemoval removal, std::function<juce::String(const std::string&)> name,
    std::function<void(std::vector<std::string>)> remove)
{
    if (removal.pending.empty())
    {
        if (removal.removed.empty())
            setStatus("Nothing removed");
        else
            remove(std::move(removal.removed));
        return;
    }
    const auto question = name(removal.pending.front());
    const auto choices = talas::app::soundRemovalChoices(removal.pending.size());
    chooseInput(question, choices, [this, removal, name = std::move(name), remove = std::move(remove)](std::string choice) mutable {
        if (const auto answer = talas::app::soundRemovalAnswer(choice))
            askToRemove(talas::app::soundRemovalAnswered(std::move(removal), *answer), std::move(name), std::move(remove));
    });
}

std::function<juce::String(const std::string&)> WorkbenchComponent::graphRemovalQuestion(
    const std::optional<std::string>& definitionId, bool alone)
{
    return [this, definitionId, alone](const std::string& nodeId) {
        const auto inputs = alone ? projectBridge_.editor().inputsReplacingNode(optionalStringView(definitionId), nodeId)
                                  : std::vector<std::string>{};
        return juceString(talas::app::soundRemovalQuestion(nodeId, inputs));
    };
}

void WorkbenchComponent::inlineSelectedSound()
{
    if (selectedNodeId_.empty())
    {
        setStatus("Select a reusable Sound instance to inline");
        return;
    }
    // Expand in a Sound editor, the project's or a Sound's.
    const auto result = activeGraphContext_.has_value()
        ? projectBridge_.editor().inlineSoundInstance(*activeGraphContext_, selectedNodeId_)
        : projectBridge_.editor().inlineSoundInstance(selectedNodeId_);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    selectNode(activeGraphContext_, {});
    documentChanged();
    setStatus("Inlined reusable Sound instance");
}

// Action > Expand: the selected Sound replaced
// by its Sounds, whose fields show the global map's Event Sources where they
// had its Event Values.
void WorkbenchComponent::expandSelectedSound()
{
    if (selectedNodeId_.empty())
    {
        setStatus("Select a Sound of a class to expand");
        return;
    }
    const auto result = activeGraphContext_.has_value()
        ? projectBridge_.editor().expandSoundInstance(*activeGraphContext_, selectedNodeId_)
        : projectBridge_.editor().expandSoundInstance(selectedNodeId_);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    selectNode(activeGraphContext_, {});
    documentChanged();
    setStatus("Expanded the Sound into its Sounds");
}

// Expand in the Sound file window: beside each
// Sound a copy of it expanded, named for it with " expanded".
void WorkbenchComponent::expandSoundDefinitions(const std::vector<std::string>& definitionIds)
{
    std::size_t expanded = 0;
    for (const auto& definitionId : definitionIds)
    {
        const auto& document = projectBridge_.editor().document();
        const auto source = std::ranges::find(document.soundDefinitions, definitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (source == document.soundDefinitions.end())
            continue;
        const auto free = [&](const std::string& base, auto field) {
            const auto taken = [&](std::string_view candidate) {
                return std::ranges::any_of(document.soundDefinitions, [&](const auto& definition) {
                    return definition.*field == candidate;
                });
            };
            return taken(base) ? talas::app::numberedCopyName(base + " ", taken) : base;
        };
        const auto newId = free(definitionId + "Expanded", &talas::project::ProjectSoundDefinition::definitionId);
        const auto name = free((source->displayName.empty() ? definitionId : source->displayName) + " expanded",
            &talas::project::ProjectSoundDefinition::displayName);
        if (const auto result = projectBridge_.editor().expandSoundDefinition(definitionId, newId, name); !result.ok())
        {
            setStatus("Expand: " + juceString(result.message));
            if (expanded > 0)
                documentChanged();
            return;
        }
        // A Script in it shows the Sound it makes.
        const auto& expandedDocument = projectBridge_.editor().document();
        const auto copy = std::ranges::find(
            expandedDocument.soundDefinitions, newId, &talas::project::ProjectSoundDefinition::definitionId);
        if (copy != expandedDocument.soundDefinitions.end())
        {
            if (auto scripted = talas::runtime::soundWithScriptsExpanded(expandedDocument, *copy))
                (void) projectBridge_.editor().replaceSoundDefinition(newId, std::move(*scripted));
        }
        ++expanded;
    }
    if (expanded == 0)
        return;
    documentChanged();
    setStatus("Expanded " + juce::String{static_cast<int>(expanded)} + " Sound(s) into a copy beside each");
}

// Sounds copied in a signal flow and pasted where Sounds go: the copy's
// last Sound with those feeding it, a Sound of the project of its own.
std::optional<std::string> WorkbenchComponent::addSoundCopiedOnClipboard()
{
    // Samples copied in the sample editor paste as a
    // clip, saved as a file of their own beside the project (else the
    // sample they came from) and played by a Sound of the project.
    if (graphClipboardText() == juceString(sampleClipboardMarker) && sampleEditorPanel_ != nullptr
        && sampleEditorPanel_->model().hasClipboard())
    {
        const auto& copied = sampleEditorPanel_->model();
        const auto folder = projectPath().has_value() ? projectPath()->parent_path()
            : !sampleClipSource_.empty()             ? sampleClipSource_.parent_path()
                                                     : filesystemPath(juce::File::getSpecialLocation(
                                                           juce::File::userDocumentsDirectory).getFullPathName());
        const auto path = talas::app::pastedSampleClipPath(folder, sampleClipSource_);
        std::string error;
        if (!talas::audio::writePcm16Wav(path, copied.sampleRate(), copied.clipboard(), error))
        {
            setStatus("The copied samples could not be saved: " + juceString(error));
            return std::nullopt;
        }
        return addSoundForTalasFile(path);
    }
    const auto parsed = talas::project::parseProjectGraphFragment(graphClipboardText().toStdString());
    auto copy = parsed.fragment.has_value()
        ? talas::app::soundCopiedFromFragment(projectBridge_.editor().document(), registry_, *parsed.fragment)
        : std::nullopt;
    if (!copy.has_value())
        return std::nullopt;
    auto document = projectBridge_.editor().document();
    const auto definitionId = copy->definition.definitionId;
    document.soundDefinitions.push_back(std::move(copy->definition));
    document.presentation.structureLayouts.push_back(std::move(copy->layout));
    talas::project::reconcileProjectPresentation(document);
    if (!projectBridge_.editor().applyDocumentEdit(std::move(document)).ok())
        return std::nullopt;
    documentChanged();
    return definitionId;
}

// Paste in the Sound file window: Sounds copied from
// a Sound file window as copies of themselves, a prototype or Sounds of a
// signal flow as a Sound of their own.
void WorkbenchComponent::pasteIntoSoundFile()
{
    // A Sound copied in a Sound Browser joins as dragged.
    if (const auto copied = graphClipboardText().toStdString(); talas::app::soundBrowserDrag(copied).has_value())
    {
        if (importBrowserDrag(copied).has_value())
            setStatus("Pasted the Sound copied in the Sound Browser");
        return;
    }
    const auto parsed = talas::project::parseProjectGraphFragment(graphClipboardText().toStdString());
    if (!parsed.fragment.has_value())
        return setStatus("Copy a Sound to paste first");
    if (const auto sounds = talas::app::soundsOfCopy(projectBridge_.editor().document(), *parsed.fragment); !sounds.empty())
        return duplicateSoundDefinitions(sounds);
    if (!addSoundCopiedOnClipboard().has_value())
        return setStatus("The copy has no Sound to paste into the Sound file");
    setStatus("Pasted the copy as a Sound of its own");
}

// A prototype dragged into the Sound file window:
// a new Sound of the project, the prototype alone in its signal flow.
std::optional<std::string> WorkbenchComponent::addSoundForPrototype(const std::string& typeId)
{
    // Numbered on from the preferences' count, while they are
    // kept.
    auto sound = talas::app::soundForPrototype(projectBridge_.editor().document(), registry_, typeId,
        persistentStateEnabled_ ? &preferences_.prototypeCopyNumbers : nullptr);
    if (!sound.has_value())
    {
        setStatus("The prototype has no audio output to make a Sound of");
        return std::nullopt;
    }
    const auto name = juceString(sound->displayName);
    auto definitionId = sound->definitionId;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(*sound)); !added.ok())
    {
        setStatus(juceString(added.message));
        return std::nullopt;
    }
    documentChanged();
    setStatus("Added " + name + " to the Sound file");
    return definitionId;
}
}
