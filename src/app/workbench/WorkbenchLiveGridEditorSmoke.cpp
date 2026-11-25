#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/CompiledSounds.h"
#include "talas/app/LiveGridWorkbenchModel.h"

namespace talas::app::workbench
{
// The Compiled Sound Grid's numbered boxes, a
// Sound put into a box, a program channel; Live Tools stepping through
// boxes. The Live Grid editor in the Tools menu authors them.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runLiveGridEditorSmoke()
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
            error = "Live Grid editor smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto library = [this]() -> const talas::project::ProjectLiveSetLibrary& {
        return projectBridge_.editor().document().liveSetLibrary;
    };
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowLiveGridEditor;
    if (!listed)
        return finish("the Tools menu does not offer the Live Grid editor");

    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    pad.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    fixture.soundDefinitions.push_back(std::move(pad));
    fixture.deviceSettings.midiInputDeviceIdentifiers = {"smoke-grid-input"};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("Live Grid editor fixture was rejected: " + replaced.message);
    }
    documentChanged();
    if (!commandManager_.invokeDirectly(commandShowLiveGridEditor, false)
        || liveGridEditorPanel_ == nullptr || liveGridEditorWindow_ != nullptr)
        return finish("the Live Grid editor command did not make the editor (without a window off screen)");
    auto& editor = *liveGridEditorPanel_;
    if (!editor.newLiveSetForSmoke() || library().liveSets.size() != 1
        || editor.selectedLiveSetId() != library().liveSets.front().liveSetId)
        return finish("New Grid did not add and select a Live Set");
    const auto setId = editor.selectedLiveSetId();

    const auto menu = editor.boxMenuForSmoke(0, 0);
    std::vector<juce::String> targets;
    for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
    {
        if (item.getItem().itemID >= LiveGridEditorPanel::boxTargetBase)
            targets.push_back(item.getItem().text);
    }
    if (targets != std::vector<juce::String>{
            "Sound: Pad", "Mute track", "Unmute track", "Stop"})
        return finish("a box's menu did not offer the project's targets");
    if (!editor.chooseBoxMenuItemForSmoke(0, 0, LiveGridEditorPanel::boxTargetBase)
        || editor.boxTextForSmoke(0, 0) != "0\nSound: Pad")
        return finish("choosing a Sound for box 0 did not fill it");
    if (!editor.chooseBoxMenuItemForSmoke(0, 3, LiveGridEditorPanel::boxTargetBase + 3)
        || !editor.renameBoxForSmoke(0, 3, "End")
        || editor.boxTextForSmoke(0, 3) != "3\nEnd")
        return finish("a Stop box could not be filled and renamed");
    if (!editor.setProgramChannelForSmoke(2))
        return finish("the grid's program channel was refused");
    const auto& cells = library().liveSets.front().cells;
    const auto programOf = [&](std::string_view cellId) -> std::optional<int> {
        const auto cell = std::ranges::find(cells, cellId, &talas::project::ProjectLiveSetCell::cellId);
        if (cell == cells.end())
            return std::nullopt;
        for (const auto& mapping : cell->mappings)
        {
            if (const auto* program =
                    std::get_if<talas::project::ProjectLiveSetMidiProgramMapping>(&mapping);
                program != nullptr && program->channel == 2
                && program->sourceIdentifier == "smoke-grid-input")
            {
                return program->program;
            }
        }
        return std::nullopt;
    };
    if (programOf("box0") != 0 || programOf("box3") != 3)
        return finish("the boxes did not answer to their programs on the grid's channel");
    if (!editor.chooseBoxMenuItemForSmoke(0, 0, LiveGridEditorPanel::boxQuantizeNextBar)
        || library().liveSets.front().cells.front().quantization.kind
            != talas::project::ProjectLiveSetQuantizationKind::NextBar)
        return finish("a box's start could not be quantized to the next bar");
    if (!editor.newToolForSmoke()
        || !editor.chooseBoxMenuItemForSmoke(0, 0, LiveGridEditorPanel::boxAddToolStep)
        || library().tools.size() != 1
        || library().tools.front().transitions.size() != 1
        || library().tools.front().transitions.front().commands.front().cellId != "box0")
        return finish("a box could not be added as a Live Tool's step");

    // The Performance Live Grid offers what the editor made.
    const auto projection = talas::app::liveGridProjection(
        projectBridge_, talas::app::LiveGridSourceContext{});
    if (std::ranges::none_of(projection, [&](const auto& action) {
            return action.liveSetId == setId && action.cellId == "box0"
                && action.workbenchMapped && action.midiMapped;
        }))
        return finish("the Performance Live Grid did not offer the edited box");

    // Compile to disk and On: On waits for the grid's compiled Sounds; Changed Sounds
    // compiles only what changed, All Sounds everything; a compiled Sound
    // plays from Play...; a double-click edits a box's Sound only while the
    // grid is off.
    const auto folder = compiledGridFolder();
    std::error_code removed;
    std::filesystem::remove_all(folder, removed);
    const auto grid = [&]() -> const talas::project::ProjectLiveSet& { return library().liveSets.front(); };
    if (editor.setOnForSmoke(true) || editor.gridOn() || playToolGridSound(grid().displayName + ".prg", 1, {}, false))
        return finish("On, or a Tool, started a grid with no compiled Sounds");
    const auto boxFile = talas::app::compiledGridSoundPath(folder, grid(),
        *std::ranges::find(grid().cells, std::string{"box0"}, &talas::project::ProjectLiveSetCell::cellId));
    if (!compileGridToDisk(setId, false) || !std::filesystem::exists(boxFile))
        return finish("Compile to disk did not compile the grid's Sound into its folder");
    const auto written = std::filesystem::last_write_time(boxFile);
    if (!compileGridToDisk(setId, false) || std::filesystem::last_write_time(boxFile) != written)
        return finish("Changed Sounds compiled a Sound that had not changed");
    if (!compileGridToDisk(setId, true) || std::filesystem::last_write_time(boxFile) == written)
        return finish("All Sounds did not compile every Sound again");
    if (!editor.setOnForSmoke(true) || !editor.gridOn())
        return finish("On did not start the compiled grid");
    if (editor.doubleClickBoxForSmoke(0, 0))
        return finish("a double-click edited a Sound while the grid was on");
    if (!editor.setOnForSmoke(false) || editor.gridOn())
        return finish("unchecking On did not stop the grid");
    if (!editor.doubleClickBoxForSmoke(0, 0) || activeGraphContext_ != std::optional<std::string>{"pad"})
        return finish("a double-click did not edit the box's Sound");
    showRootGraph();
    const auto prepared = soundBrowserUsesPreparedProfileForSmoke_;
    soundBrowserUsesPreparedProfileForSmoke_ = true;
    const auto played = playCompiledSoundFile(boxFile) && playToolGridSound(grid().displayName + ".prg", 0, "Pad", false);
    stopSoundLibraryAudition();
    soundBrowserUsesPreparedProfileForSmoke_ = prepared;
    if (!played)
        return finish("Play... did not play the compiled Sound file: " + statusLabel_.getText().toStdString());
    std::filesystem::remove_all(folder, removed);

    const auto beforeClear = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    if (!editor.chooseBoxMenuItemForSmoke(0, 3, LiveGridEditorPanel::boxClear)
        || editor.boxTextForSmoke(0, 3) != "3")
        return finish("Clear Box did not empty the box");
    undoProject();
    if (talas::project::serializeProjectDocument(projectBridge_.editor().document()) != beforeClear
        || editor.boxTextForSmoke(0, 3) != "3\nEnd")
        return finish("one Undo did not bring a cleared box back");
    // A grid a tool steps through stays until the tool goes.
    if (editor.deleteLiveSetForSmoke() || library().liveSets.size() != 1)
        return finish("Delete removed a Live Set a Live Tool still uses");
    if (!editor.deleteToolForSmoke() || !library().tools.empty()
        || !editor.deleteLiveSetForSmoke() || !library().liveSets.empty())
        return finish("Delete Tool and Delete did not remove the tool and the Live Set");
    undoProject();
    if (library().liveSets.size() != 1)
        return finish("one Undo did not bring a deleted Live Set back");
    newFileOfType("Compiled Sound grid");
    if (library().liveSets.size() != 2 || editor.selectedLiveSetId() != library().liveSets.back().liveSetId)
        return finish("New... did not make and select a Compiled Sound grid");
    return finish();
}
}
