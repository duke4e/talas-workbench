#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/project/ProjectJson.h"

#include <filesystem>
#include <fstream>

namespace talas::app::workbench
{
// The
// Sound file window's icons, a click, Shift+Click and a box selecting them,
// Ctrl+Space playing the selected Sound and Space pausing it, Enter
// renaming it, Ctrl+D duplicating it, Ctrl+E expanding it, a double-click
// opening its editor, an icon dragged out as the Sound, Delete, and Sound
// collections.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSoundFileSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSoundBrowser();
        soundBrowserUsesPreparedProfileForSmoke_ = false;
        soundFileLastPlayed_.reset();
        soundDialog_.reset();
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        showRootGraph();
        if (!restored.ok() && error.empty())
            error = "Sound file smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto document = [this]() -> const talas::project::ProjectDocument& {
        return projectBridge_.editor().document();
    };

    talas::project::ProjectDocument fixture;
    for (const auto& [id, name] : {std::pair{"vocoder", "Bucket Vocoder"}, std::pair{"pad", "Pad"}, std::pair{"bell", "Bell"}})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = name;
        sound.graph.nodes.push_back({"tone", "talas.oscillator", {}});
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        fixture.soundDefinitions.push_back(std::move(sound));
    }
    // The vocoder plays the Bell, for Expand.
    fixture.soundDefinitions.front().graph.nodes = {talas::project::ProjectNode{"voice", {}, {}, "bell"}};
    fixture.soundDefinitions.front().outputs = {{"audioOut", talas::core::PortType::Audio, "voice", "audioOut"}};
    fixture.graph.nodes = {{"osc", "talas.oscillator", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Sound file fixture was rejected: " + replaced.message);
    documentChanged();
    soundBrowserUsesPreparedProfileForSmoke_ = true;
    showSoundFileWindow();
    auto* window = soundFileForSmoke();
    if (window == nullptr)
        return finish("the Sound file window did not open");
    window->setSize(560, 360);
    const std::vector<talas::app::SoundFileIcon> icons{{"vocoder", "Bucket Vocoder", false}, {"pad", "Pad", false}, {"bell", "Bell", false}};
    if (window->iconsForSmoke() != icons)
        return finish("the Sound file window did not show an icon for each of the project's Sounds");

    // A click, Shift+Click and a box drawn in the background.
    const auto centre = [&](const std::string& id) { return window->iconBoundsForSmoke(id)->getCentre(); };
    using Ids = std::vector<std::string>;
    window->clickForSmoke(centre("vocoder"), false);
    window->clickForSmoke(centre("bell"), true);
    if (window->selectionForSmoke() != Ids{"vocoder", "bell"})
        return finish("Shift+Click did not add a Sound to the selection");
    window->clickForSmoke(centre("vocoder"), true);
    if (window->selectionForSmoke() != Ids{"bell"})
        return finish("Shift+Click did not take a selected Sound out of the selection");
    const auto padBounds = *window->iconBoundsForSmoke("pad");
    const auto vocoderBounds = *window->iconBoundsForSmoke("vocoder");
    window->boxForSmoke({vocoderBounds.getX() - 6, 4}, {padBounds.getRight() + 2, padBounds.getBottom() + 2}, false);
    if (window->selectionForSmoke() != Ids{"vocoder", "pad"})
        return finish("a box drawn round two Sounds did not select them");
    // Dragged with Control or Option, a Sound itself moves.
    const auto iconIds = [&] {
        Ids ids;
        for (const auto& icon : window->iconsForSmoke())
            ids.push_back(icon.definitionId);
        return ids;
    };
    window->clickForSmoke(centre("pad"), false);
    window->moveForSmoke(centre("vocoder"));
    const auto movedFirst = iconIds();
    window->clickForSmoke(centre("pad"), false);
    window->moveForSmoke(centre("bell"));
    if (movedFirst != Ids{"pad", "vocoder", "bell"} || iconIds() != Ids{"vocoder", "pad", "bell"})
        return finish("a Sound dragged with Control or Option did not move");

    // Ctrl+Space plays the selected Sound, Space pauses it.
    window->clickForSmoke(centre("pad"), false);
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey, juce::ModifierKeys::ctrlModifier, 0})
        || soundBrowserAudition_ == nullptr || soundFileLastPlayed_ != std::optional<std::string>{"pad"})
        return finish("Ctrl+Space did not play the selected Sound: " + statusLabel_.getText().toStdString());
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey}) || soundBrowserAudition_->playing)
        return finish("Space did not pause the Sound playing");
    if (!window->keyForSmoke(juce::KeyPress{'k', juce::ModifierKeys::commandModifier, 0}) || soundBrowserAudition_ != nullptr)
        return finish("Ctrl+K did not stop the Sound");
    // Several selected Sounds play one after another.
    window->clickForSmoke(centre("bell"), true);
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey, juce::ModifierKeys::ctrlModifier, 0})
        || soundBrowserAudition_ == nullptr || !statusLabel_.getText().contains("one after another"))
        return finish("Ctrl+Space did not play the selected Sounds one after another: " + statusLabel_.getText().toStdString());
    if (!window->keyForSmoke(juce::KeyPress{'k', juce::ModifierKeys::commandModifier, 0}) || soundBrowserAudition_ != nullptr)
        return finish("Ctrl+K did not stop the Sounds in turn");
    // Ctrl+Space plays the prototype selected, adding
    // nothing to the project.
    const auto soundsBeforePrototype = document().soundDefinitions.size();
    if (!prototypePanel_->selectForSmoke("talas.oscillator")
        || !prototypePanel_->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey, juce::ModifierKeys::ctrlModifier, 0})
        || soundBrowserAudition_ == nullptr || document().soundDefinitions.size() != soundsBeforePrototype)
        return finish("Ctrl+Space did not play the selected prototype: " + statusLabel_.getText().toStdString());
    // Ctrl+] makes the focused prototypes' entries larger,
    // Ctrl+[ smaller.
    const auto usual = prototypePanel_->rowHeightForSmoke();
    zoomActiveDisplay(true);
    const auto larger = prototypePanel_->rowHeightForSmoke();
    zoomActiveDisplay(false);
    zoomActiveDisplay(false);
    const auto smaller = prototypePanel_->rowHeightForSmoke();
    zoomActiveDisplay(true);
    if (larger <= usual || smaller >= usual || prototypePanel_->rowHeightForSmoke() != usual)
        return finish("Zoom In and Out did not make the prototypes' entries larger and smaller");
    // Describe Sound on the prototype selected.
    soundHelpText_.clear();
    if (!commandManager_.invokeDirectly(commandDescribeSound, false) || !soundHelpText_.startsWith("Oscillator"))
        return finish("Describe Sound did not describe the prototype selected: " + soundHelpText_.toStdString());
    // A double-click on a prototype opens an
    // editor on a numbered copy of it; undone, the smoke goes on as it was.
    const auto soundsBeforeOpen = document().soundDefinitions.size();
    const auto modeBeforeOpen = workspaceMode_;
    const auto contextBeforeOpen = activeGraphContext_;
    const auto opened = prototypePanel_->doubleClickForSmoke("talas.oscillator")
        && document().soundDefinitions.size() == soundsBeforeOpen + 1
        && activeGraphContext_ == std::optional<std::string>{document().soundDefinitions.back().definitionId}
        && document().soundDefinitions.back().displayName.starts_with("Oscillator");
    if (document().soundDefinitions.size() != soundsBeforeOpen)
        (void) projectBridge_.editor().undo();
    documentChanged();
    if (contextBeforeOpen.has_value())
        showSoundDefinition(*contextBeforeOpen);
    else
        showRootGraph();
    activateWorkspaceMode(modeBeforeOpen);
    if (!opened || document().soundDefinitions.size() != soundsBeforeOpen)
        return finish("a double-click on a prototype did not open an editor on a numbered copy of it");
    // Ctrl+C copies the prototype selected, and Ctrl+V
    // in the Sound file window pastes it as a Sound; undone after.
    (void) prototypePanel_->selectForSmoke("talas.oscillator");
    const auto soundsBeforePaste = document().soundDefinitions.size();
    const auto copiedPrototype = prototypePanel_->keyForSmoke(juce::KeyPress{'c', juce::ModifierKeys::commandModifier, 0})
        && graphClipboardText().contains("talas.oscillator");
    const auto pastedPrototype = window->keyForSmoke(juce::KeyPress{'v', juce::ModifierKeys::commandModifier, 0})
        && document().soundDefinitions.size() == soundsBeforePaste + 1;
    if (document().soundDefinitions.size() != soundsBeforePaste)
        (void) projectBridge_.editor().undo();
    documentChanged();
    if (!copiedPrototype || !pastedPrototype)
        return finish("Ctrl+C on a prototype and Ctrl+V in the Sound file window did not paste it as a Sound");
    // The Sound editor's close box on a changed Sound
    // asks Keep, Drop or Cancel; Drop gives the Sound back as it was.
    const auto padBefore = *std::ranges::find(document().soundDefinitions, std::string{"pad"},
        &talas::project::ProjectSoundDefinition::definitionId);
    const auto closeMode = workspaceMode_;
    showSoundDefinition("pad");
    auto changedPad = document();
    std::ranges::find(changedPad.soundDefinitions, std::string{"pad"}, &talas::project::ProjectSoundDefinition::definitionId)
        ->description = "changed in the editor";
    const auto edited = projectBridge_.editor().applyDocumentEdit(std::move(changedPad)).ok();
    documentChanged();
    smokeSoundEditorCloseAnswer_ = 2;
    closeSoundEditor();
    const auto closeCancelled = secondaryGraphContext_ == std::optional<std::string>{"pad"};
    smokeSoundEditorCloseAnswer_ = 1;
    closeSoundEditor();
    smokeSoundEditorCloseAnswer_.reset();
    const auto padAfter = *std::ranges::find(document().soundDefinitions, std::string{"pad"},
        &talas::project::ProjectSoundDefinition::definitionId);
    activateWorkspaceMode(closeMode);
    if (!edited || !closeCancelled || padAfter != padBefore || secondaryGraphContext_.has_value())
        return finish("the Sound editor's close box did not drop the changes to the Sound, or Cancel did not keep it open");
    (void) prototypePanel_->selectForSmoke({});
    stopSoundBrowser();
    window->clickForSmoke(centre("pad"), false);

    // Enter asks for a new name.
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::returnKey}) || soundDialog_ == nullptr
        || soundDialog_->getTextEditorContents("soundName") != "Pad")
        return finish("Enter did not ask for the selected Sound's new name");
    soundDialog_.reset();

    // Duplicate.
    if (!window->keyForSmoke(juce::KeyPress{'d', juce::ModifierKeys::commandModifier, 0})
        || document().soundDefinitions.size() != 4 || document().soundDefinitions.back().displayName != "Pad 1"
        || window->iconsForSmoke().size() != 4)
        return finish("Ctrl+D did not duplicate the selected Sound");

    // Expand (Ctrl+E) saves a copy expanded beside the Sound,
    // which it leaves; in its editor Expand replaces the Sound in it.
    window->clickForSmoke(centre("vocoder"), false);
    const auto vocoder = document().soundDefinitions.front();
    const auto holdsInstance = [](const talas::project::ProjectSoundDefinition& sound) {
        return std::ranges::any_of(sound.graph.nodes, [](const auto& node) { return node.soundDefinitionId.has_value(); });
    };
    if (!window->keyForSmoke(juce::KeyPress{'e', juce::ModifierKeys::commandModifier, 0})
        || document().soundDefinitions.size() != 5 || document().soundDefinitions.back().displayName != "Bucket Vocoder expanded"
        || document().soundDefinitions.front() != vocoder || holdsInstance(document().soundDefinitions.back())
        || window->iconsForSmoke().size() != 5)
        return finish("Ctrl+E did not save an expanded copy of the selected Sound: " + statusLabel_.getText().toStdString());
    window->clickForSmoke(centre("vocoder"), false, true);
    selectNode(std::optional<std::string>{"vocoder"}, "voice");
    inlineSelectedSound();
    if (activeGraphContext_ != std::optional<std::string>{"vocoder"} || holdsInstance(document().soundDefinitions.front()))
        return finish("Expand in the Sound's editor did not replace the Sound in it: " + statusLabel_.getText().toStdString());
    showSoundFileWindow();

    // An icon dragged out is the Sound; a double-click opens it.
    if (window->dragDescriptionForSmoke(centre("bell")).toString() != juceString(soundDragPrefix) + "bell")
        return finish("an icon dragged out did not carry its Sound");
    window->clickForSmoke(centre("bell"), false, true);
    if (activeGraphContext_ != std::optional<std::string>{"bell"})
        return finish("a double-click did not open the Sound's editor");
    // A Sound already being edited comes back to the front,
    // in the editor it has.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    showSoundFileWindow();
    window->clickForSmoke(centre("bell"), false, true);
    if (workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Patch || activeGraphContext_ != std::optional<std::string>{"bell"}
        || secondaryGraphContext_ != std::optional<std::string>{"bell"} || primaryGraphContext_.has_value())
        return finish("a double-click on a Sound being edited did not bring its editor back to the front");

    // Delete removes the selected Sounds, asking first: Cancel keeps
    // them, Yes removes one, Remove all the rest.
    window->clickForSmoke(centre("vocoder"), false);
    window->clickForSmoke(centre("vocoderExpanded"), true);
    smokeInputChoice_ = 2;
    const auto cancelled = window->keyForSmoke(juce::KeyPress{juce::KeyPress::deleteKey}) && document().soundDefinitions.size() == 5;
    smokeInputChoice_ = 1;
    const auto removedAll = window->keyForSmoke(juce::KeyPress{juce::KeyPress::deleteKey});
    smokeInputChoice_.reset();
    if (!cancelled || !removedAll
        || std::ranges::any_of(document().soundDefinitions,
            [](const auto& sound) { return sound.definitionId == "vocoder" || sound.definitionId == "vocoderExpanded"; })
        || window->iconsForSmoke().size() != 3)
        return finish("Delete did not ask before removing the selected Sounds, Remove all removing both: "
            + statusLabel_.getText().toStdString());

    // Collect... puts the selected
    // Sounds into a new collection, which a double-click opens and Escape
    // leaves; a duplicate holds a duplicate of each Sound; deleting it
    // leaves its Sounds at the top level.
    window->clickForSmoke(centre("pad"), false);
    window->clickForSmoke(centre("bell"), true);
    window->collectForSmoke("Tutorial Sounds");
    const auto& collections = document().soundCollections;
    if (collections.size() != 1 || collections.front().name != "Tutorial Sounds"
        || collections.front().soundIds != Ids{"pad", "bell"}
        || window->iconsForSmoke().front() != talas::app::SoundFileIcon{collections.front().collectionId, "Tutorial Sounds", true})
        return finish("Collect did not put the selected Sounds into a new collection");
    const auto collected = collections.front().collectionId;
    window->clickForSmoke(centre(collected), false, true);
    // Its Sounds by name; Bell, still open in its own Sound editor,
    // keeps its being-edited pattern.
    auto inCollection = window->iconsForSmoke();
    for (auto& icon : inCollection)
        icon.beingEdited = false;
    if (window->levelForSmoke() != std::optional<std::string>{collected}
        || inCollection != std::vector<talas::app::SoundFileIcon>{{"pad", "Pad", false}, {"bell", "Bell", false}})
        return finish("a double-click did not open the collection on its Sounds");
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::escapeKey}) || window->levelForSmoke().has_value()
        || window->selectionForSmoke() != Ids{collected})
        return finish("Escape did not go back up to the Sound file");
    if (!window->keyForSmoke(juce::KeyPress{'d', juce::ModifierKeys::commandModifier, 0})
        || document().soundCollections.size() != 2 || document().soundCollections.back().soundIds.size() != 2
        || document().soundCollections.back().soundIds.front() == "pad")
        return finish("Ctrl+D did not duplicate the collection with a duplicate of each Sound");
    window->clickForSmoke(centre(collected), false);
    if (!window->keyForSmoke(juce::KeyPress{juce::KeyPress::deleteKey}) || document().soundCollections.size() != 1
        || std::ranges::none_of(window->iconsForSmoke(), [](const auto& icon) { return icon.definitionId == "pad"; }))
        return finish("deleting the collection did not leave its Sounds at the top level");

    // A Sound file opened as a
    // custom prototype strip, its collections as categories and its other
    // Sounds under General; Return copies the selected one into the
    // project's Sounds.
    talas::project::ProjectDocument prototypes;
    for (const auto& [id, name] : {std::pair{"bank", "bank class"}, std::pair{"eq", "eq class"}, std::pair{"voice", "voice class"}})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = name;
        sound.graph.nodes.push_back({"tone", "talas.oscillator", {}});
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        prototypes.soundDefinitions.push_back(std::move(sound));
    }
    prototypes.soundCollections = {{"filters", "new filters", {"bank", "eq"}, {}}};
    talas::project::reconcileProjectPresentation(prototypes);
    const auto stripFile = std::filesystem::temp_directory_path() / "talas-custom-prototypes-smoke.talas.json";
    {
        std::ofstream output(stripFile, std::ios::binary);
        output << talas::project::serializeProjectDocument(prototypes);
    }
    const auto shown = showCustomPrototypes(stripFile);
    auto* strip = customPrototypesForSmoke();
    const std::vector<std::string> rows{"new filters", "  bank class", "  eq class", "General", "  voice class"};
    if (!shown || strip == nullptr || strip->rowsForSmoke() != rows)
    {
        std::filesystem::remove(stripFile);
        return finish("the custom prototype strip did not show the collections as categories and the rest under General");
    }
    const auto before = document().soundDefinitions.size();
    const auto copied = strip->selectRowForSmoke(1) && strip->keyForSmoke(juce::KeyPress{juce::KeyPress::returnKey})
        && document().soundDefinitions.size() == before + 1 && document().soundDefinitions.back().displayName == "bank class";
    std::filesystem::remove(stripFile);
    if (!copied)
        return finish("Return did not copy the custom prototype into the project's Sounds: " + statusLabel_.getText().toStdString());
    // A prototype dragged into the window is a new Sound.
    const auto soundsBefore = document().soundDefinitions.size();
    if (!window->dropForSmoke(juce::var{juceString(prototypeDragPrefix) + "talas.oscillator"})
        || document().soundDefinitions.size() != soundsBefore + 1
        || document().soundDefinitions.back().graph.nodes.front().typeId != "talas.oscillator"
        || std::ranges::none_of(window->iconsForSmoke(),
            [&](const auto& icon) { return icon.definitionId == document().soundDefinitions.back().definitionId; }))
        return finish("a prototype dragged into the Sound file window did not become a Sound there: "
            + statusLabel_.getText().toStdString());
    return finish();
}
}
