#include "talas/app/CompiledSounds.h"
#include "talas/app/LiveGridAuthoringModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireOk(const talas::project::EditResult& result, const std::string& message)
{
    require(result.ok(), message + ": " + result.message);
}

const talas::core::NodeRegistry& registry()
{
    static const talas::core::NodeRegistry builtins = [] {
        talas::core::NodeRegistry value;
        talas::core::registerBuiltinNodeTypes(value);
        return value;
    }();
    return builtins;
}

const talas::project::ProjectLiveSet& liveSet(
    const talas::project::ProjectEditor& editor,
    std::size_t index = 0)
{
    return editor.document().liveSetLibrary.liveSets.at(index);
}

const talas::app::LiveGridTargetChoice& choice(
    const talas::project::ProjectDocument& document,
    std::string_view label)
{
    static std::vector<talas::app::LiveGridTargetChoice> choices;
    choices = talas::app::liveGridTargetChoices(document);
    const auto found = std::ranges::find(
        choices, label, &talas::app::LiveGridTargetChoice::label);
    require(found != choices.end(), "missing target choice " + std::string{label});
    return *found;
}

std::vector<talas::project::ProjectLiveSetMidiProgramMapping> programs(
    const talas::project::ProjectLiveSetCell& cell)
{
    std::vector<talas::project::ProjectLiveSetMidiProgramMapping> found;
    for (const auto& mapping : cell.mappings)
    {
        if (const auto* program =
                std::get_if<talas::project::ProjectLiveSetMidiProgramMapping>(&mapping))
        {
            found.push_back(*program);
        }
    }
    return found;
}

bool clickable(const talas::project::ProjectLiveSetCell& cell)
{
    return std::ranges::any_of(cell.mappings, [](const auto& mapping) {
        return std::holds_alternative<talas::project::ProjectLiveSetWorkbenchMapping>(mapping);
    });
}

// The Compiled Sound Grid's 128 numbered boxes,
// each a MIDI program number; a Sound put into a box; a program channel;
// clicking a box loads and starts its Sound.
void testSoundsGoIntoNumberedBoxesThatAnswerClicksAndPrograms()
{
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    requireOk(editor.addSoundDefinition(std::move(pad)), "add a Sound");

    require(talas::app::liveGridProgramNumber(0, 0) == 0
            && talas::app::liveGridProgramNumber(0, 15) == 15
            && talas::app::liveGridProgramNumber(7, 15) == 127,
        "the 128 boxes should be the MIDI programs, 16 to a row");

    auto grid = talas::app::makeLiveGridSet(editor.document().liveSetLibrary, "Set A");
    require(grid.liveSetId == "grid1" && grid.displayName == "Set A" && grid.cells.empty(),
        "a new grid should be empty with a fresh id");
    requireOk(editor.upsertProjectLiveSet(grid), "add the grid");
    require(talas::app::makeLiveGridSet(editor.document().liveSetLibrary, "").liveSetId == "grid2",
        "the next grid should take the next id");

    const auto labels = [&] {
        std::vector<std::string> values;
        for (const auto& value : talas::app::liveGridTargetChoices(editor.document()))
            values.push_back(value.label);
        return values;
    }();
    require(labels == std::vector<std::string>{
                "Sound: Pad", "Mute track", "Unmute track", "Stop"},
        "a box should offer each Sound, track mute and unmute, and Stop");

    auto cell = talas::app::assignLiveGridCell(
        editor.document(), liveSet(editor), 0, 0, choice(editor.document(), "Sound: Pad"));
    require(cell.cellId == "box0" && cell.displayName == "Sound: Pad"
            && cell.row == 0 && cell.column == 0 && clickable(cell) && programs(cell).empty(),
        "a Sound put into box 0 should be a clickable cell named after it");
    requireOk(editor.upsertProjectLiveSetCell("grid1", cell), "store box 0");
    requireOk(editor.upsertProjectLiveSetCell("grid1",
                  talas::app::assignLiveGridCell(editor.document(), liveSet(editor), 1, 0,
                      choice(editor.document(), "Mute track"))),
        "a track mute below the Sound should be accepted");

    const auto soundsBefore = editor.document().soundDefinitions;
    const auto graphBefore = editor.document().graph;
    requireOk(editor.upsertProjectLiveSet(talas::app::withLiveGridProgramChannel(
                  liveSet(editor), 3, "midi-in")),
        "setting the grid's program channel should be accepted");
    // The program channel is for program
    // changes alone; the Sounds, and the channels they hear other MIDI on,
    // are left as they were.
    require(editor.document().soundDefinitions == soundsBefore && editor.document().graph == graphBefore,
        "the grid's program channel should leave the Sounds' own MIDI as it was");
    require(talas::app::liveGridProgramChannel(liveSet(editor)) == 3,
        "the grid should report its program channel");
    const auto* box0 = talas::app::liveGridCellAt(liveSet(editor), 0, 0);
    const auto* box16 = talas::app::liveGridCellAt(liveSet(editor), 1, 0);
    require(box0 != nullptr && box16 != nullptr
            && programs(*box0) == std::vector<talas::project::ProjectLiveSetMidiProgramMapping>{{3, 0, "midi-in"}}
            && programs(*box16) == std::vector<talas::project::ProjectLiveSetMidiProgramMapping>{{3, 16, "midi-in"}},
        "each box should answer to its own program on the grid's channel");
    const auto stop = talas::app::assignLiveGridCell(
        editor.document(), liveSet(editor), 0, 5, choice(editor.document(), "Stop"));
    require(programs(stop) == std::vector<talas::project::ProjectLiveSetMidiProgramMapping>{{3, 5, "midi-in"}},
        "a box filled later should answer to its program too");
    requireOk(editor.upsertProjectLiveSetCell("grid1", stop), "store box 5");

    auto renamed = *talas::app::liveGridCellAt(liveSet(editor), 0, 5);
    auto reassigned = talas::app::assignLiveGridCell(
        editor.document(), liveSet(editor), 0, 5, choice(editor.document(), "Sound: Pad"));
    require(reassigned.cellId == "box5" && reassigned.displayName == "Sound: Pad",
        "a box named after its target should take its new target's name");
    renamed.displayName = "Ending";
    requireOk(editor.upsertProjectLiveSetCell("grid1", renamed), "rename box 5");
    reassigned = talas::app::assignLiveGridCell(
        editor.document(), liveSet(editor), 0, 5, choice(editor.document(), "Sound: Pad"));
    require(reassigned.displayName == "Ending" && reassigned.cellId == "box5",
        "a box given a name should keep it when its target changes");
    requireOk(editor.upsertProjectLiveSetCell("grid1", reassigned), "reassign box 5");

    requireOk(editor.upsertProjectLiveSet(talas::app::withLiveGridProgramChannel(
                  liveSet(editor), std::nullopt, {})),
        "removing the program channel should be accepted");
    require(!talas::app::liveGridProgramChannel(liveSet(editor)).has_value()
            && std::ranges::all_of(liveSet(editor).cells, [](const auto& value) {
                   return programs(value).empty() && clickable(value);
               }),
        "without a channel the boxes should only answer clicks");
}

void testToolsStepThroughCells()
{
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    auto grid = talas::app::makeLiveGridSet(editor.document().liveSetLibrary, "Set");
    requireOk(editor.upsertProjectLiveSet(grid), "add the grid");
    requireOk(editor.upsertProjectLiveSetCell("grid1",
                  talas::app::assignLiveGridCell(editor.document(), liveSet(editor), 0, 0,
                      choice(editor.document(), "Stop"))),
        "add a Stop box");
    requireOk(editor.upsertProjectLiveSetCell("grid1",
                  talas::app::assignLiveGridCell(editor.document(), liveSet(editor), 0, 1,
                      choice(editor.document(), "Stop"))),
        "add a second Stop box");
    auto tool = talas::app::makeLiveGridTool(editor.document().liveSetLibrary, "Steps");
    require(tool.toolId == "tool1" && tool.initialStateId == "start"
            && tool.states.size() == 1 && tool.transitions.empty(),
        "a new tool should start in its one state");
    requireOk(editor.upsertProjectLiveTool(tool), "add the tool");
    tool = talas::app::withLiveGridToolStep(tool, "grid1", liveSet(editor).cells.at(0));
    tool = talas::app::withLiveGridToolStep(tool, "grid1", liveSet(editor).cells.at(1));
    requireOk(editor.upsertProjectLiveTool(tool), "a tool with two steps should be accepted");
    const auto& stored = editor.document().liveSetLibrary.tools.at(0);
    require(stored.states.size() == 3 && stored.transitions.size() == 2
            && stored.transitions[0].fromStateId == "start"
            && stored.transitions[0].toStateId == "step1"
            && stored.transitions[1].fromStateId == "step1"
            && stored.transitions[1].toStateId == "step2"
            && stored.transitions[1].commands.at(0).cellId == "box1"
            && stored.transitions[1].trigger.kind
                == talas::project::ProjectLiveToolTriggerKind::Manual,
        "each step should be a manual transition that activates its cell");
}
}

// Compile to disk
// keeps a Sound ready to play by itself, with copies of the files it reads;
// a compiled Sound goes out of date when its Sound, its files' contents,
// Talas or the system change, and the grid's Compile to disk compiles
// the boxes out of date, or all of them for All Sounds.
void testCompiledSoundsAndTheGrid()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-compiled-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto take = folder / "take.wav";
    const auto writeTake = [&](std::string_view bytes) {
        std::ofstream output(take, std::ios::binary | std::ios::trunc);
        output << bytes;
    };
    writeTake("first take");

    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"player", "talas.sample",
        {{.parameterId = "sample", .resource = std::string{"Samples/Take"}}}, {}, {}});
    pad.outputs.push_back({"out", talas::core::PortType::Audio, "player", "audioOut"});
    document.soundDefinitions.push_back(pad);
    talas::project::ProjectSoundDefinition other;
    other.definitionId = "other";
    other.displayName = "Other";
    other.graph.nodes.push_back({"osc", "talas.oscillator", {}, {}, {}});
    other.outputs.push_back({"out", talas::core::PortType::Audio, "osc", "audioOut"});
    document.soundDefinitions.push_back(other);
    talas::project::ProjectAsset sample;
    sample.id = "Samples/Take";
    sample.kind = talas::project::ProjectAssetKind::Audio;
    sample.path = "take.wav";
    document.assets.push_back(sample);
    document.globalMap = "!Volume is: `MIDIController07";
    const auto project = folder / "Project.talas.json";
    const talas::app::CompiledSoundTarget target{144, "Talas 1", "macOS 27"};

    auto compiled = talas::app::compileSound(document, "pad", project, target);
    require(compiled.ok() && compiled.compiled->name == "Pad" && compiled.compiled->target == target
            && compiled.compiled->document.globalMap == document.globalMap,
        "Compile to disk should compile the Sound with the global map: " + compiled.error);
    const auto& file = *std::ranges::find(compiled.compiled->document.assets, "Samples/Take", &talas::project::ProjectAsset::id);
    require(std::filesystem::path{file.path} == take && !file.contentHash.empty(),
        "the compiled Sound should name its sample by where it is and what it holds");
    require(!talas::app::compileSound(document, "missing", project, target).ok(),
        "a Sound the project lacks should not compile");

    const auto read = talas::app::readCompiledSound(talas::app::writeCompiledSound(*compiled.compiled));
    require(read.has_value() && read->soundDefinitionId == "pad" && read->name == "Pad" && read->target == target
            && talas::app::compiledSoundCurrent(*read, *compiled.compiled),
        "a compiled Sound file should read back as written");
    require(!talas::app::readCompiledSound("not a compiled Sound").has_value(),
        "other files should not read as compiled Sounds");

    auto copied = *compiled.compiled;
    const auto copies = talas::app::withFileCopies(copied, folder / "Pad.talascompiled");
    require(copies == std::vector<talas::app::CompiledSoundFileCopy>{{take, folder / "Pad files" / "take.wav"}}
            && std::ranges::find(copied.document.assets, "Samples/Take", &talas::project::ProjectAsset::id)->path
                == (folder / "Pad files" / "take.wav").string(),
        "compiling should keep copies of the samples the Sound reads");
    const auto now = [&](const talas::project::ProjectDocument& current, talas::app::CompiledSoundTarget on = {144, "Talas 1", "macOS 27"}) {
        return *talas::app::compileSound(current, "pad", project, std::move(on)).compiled;
    };
    require(talas::app::compiledSoundCurrent(copied, now(document)),
        "a compiled Sound should stay current wherever its copies are");

    auto unrelated = document;
    unrelated.soundDefinitions[1].displayName = "Renamed";
    unrelated.metadata.title = "Retitled";
    require(talas::app::compiledSoundCurrent(copied, now(unrelated)),
        "a change to a Sound it does not use should leave it current");
    auto edited = document;
    edited.soundDefinitions[0].graph.nodes[0].parameters.push_back({.parameterId = "frequency", .expression = std::string{"2 hz"}});
    require(!talas::app::compiledSoundCurrent(copied, now(edited)),
        "a compiled Sound should not follow changes to its Sound");
    auto remapped = document;
    remapped.globalMap = "!Volume is: `MIDIController08";
    require(!talas::app::compiledSoundCurrent(copied, now(remapped)),
        "a changed global map should need a recompile");
    require(!talas::app::compiledSoundCurrent(copied, now(document, {144, "Talas 2", "macOS 27"}))
            && !talas::app::compiledSoundCurrent(copied, now(document, {144, "Talas 1", "macOS 28"})),
        "a new Talas or system should need a recompile");
    writeTake("second take");
    require(!talas::app::compiledSoundCurrent(copied, now(document)),
        "a compiled Sound should not follow changes to its samples");

    talas::project::ProjectLiveSet grid;
    grid.liveSetId = "grid1";
    grid.cells.push_back({"box0", "Pad", 0, 0, {}, talas::project::ProjectLiveSetSoundTarget{"pad", {}}, {}});
    grid.cells.push_back({"box1", "Stop", 0, 1, {}, talas::project::ProjectLiveSetStopTarget{}, {}});
    grid.cells.push_back({"box2", "Other", 0, 2, {}, talas::project::ProjectLiveSetSoundTarget{"other", {}}, {}});
    require(talas::app::compiledGridSoundPath(folder, grid, grid.cells[0]) == folder / "grid1" / "box0.talascompiled",
        "each box's compiled Sound should have a file of its own");
    require(talas::app::gridCellsToCompile(grid, {{"box0", true}, {"box2", false}}, false) == std::vector<std::string>{"box2"}
            && talas::app::gridCellsToCompile(grid, {{"box0", true}}, false) == std::vector<std::string>{"box2"}
            && talas::app::gridCellsToCompile(grid, {{"box0", true}, {"box2", true}}, true)
                == std::vector<std::string>{"box0", "box2"},
        "Compile to disk should compile the boxes out of date, All Sounds every box with a Sound");
    grid.displayName = "Set A";
    document.liveSetLibrary.liveSets.push_back(grid);
    const auto first = talas::app::toolGridBox(document, "Set A.prg", 1, {});
    const auto named = talas::app::toolGridBox(document, "/grids/Set A.prg", 0, "Other");
    require(first.has_value() && first->cell->cellId == "box0" && named.has_value() && named->cell->cellId == "box2"
            && !talas::app::toolGridBox(document, "Set A.prg", 2, {}).has_value()
            && talas::app::toolGridBox(document, "elsewhere.prg", 3, {}).has_value(),
        "a Tool's grid plays the box in its place from 1, or the Sound of that name");
    std::error_code removed;
    std::filesystem::remove_all(folder, removed);
}

// A Compiled Sound Grid has
// a file of its own, named with .prg; opened, the grid joins the project
// with the Sounds its boxes play, renamed where they would clash.
void testCompiledSoundGridFiles()
{
    const auto oscillator = [](std::string id, std::string frequency) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({"osc", "talas.oscillator",
            {{.parameterId = "frequency", .expression = std::move(frequency)}}, {}, {}});
        sound.outputs.push_back({"out", talas::core::PortType::Audio, "osc", "audioOut"});
        return sound;
    };
    talas::project::ProjectDocument made;
    made.soundDefinitions = {oscillator("pad", "440 hz")};
    talas::project::ProjectLiveSet grid;
    grid.liveSetId = "grid1";
    grid.displayName = "Set A";
    grid.cells.push_back({"box0", "Pad", 0, 0, {}, talas::project::ProjectLiveSetSoundTarget{"pad", {}}, {}});
    made.liveSetLibrary.liveSets = {grid};
    const auto file = talas::app::compiledSoundGridFile(made, "grid1");
    require(talas::app::compiledSoundGridFileName("Set A") == std::filesystem::path{"Set A.prg"} && file.has_value()
            && file->liveSetLibrary.liveSets == std::vector<talas::project::ProjectLiveSet>{grid}
            && !talas::app::compiledSoundGridFile(made, "missing").has_value(),
        "a Compiled Sound Grid's file should end in .prg and hold the grid");
    const auto reread = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(*file));
    require(reread.ok(), "the grid's file should read back: " + (reread.errors.empty() ? std::string{} : reread.errors.front().message));

    // Opened in a project with a Sound pad and a grid grid1 of its own.
    talas::project::ProjectDocument project;
    project.soundDefinitions = {oscillator("pad", "220 hz")};
    project.liveSetLibrary.liveSets = {talas::project::ProjectLiveSet{"grid1", "Mine", {}, {}}};
    const auto opened = talas::app::withCompiledSoundGridFile(project, reread.document, registry());
    require(opened.ok(), "a Compiled Sound Grid's file should open: " + opened.diagnostic);
    const auto& sets = opened.document->liveSetLibrary.liveSets;
    const auto joined = std::ranges::find(sets, opened.liveSetId, &talas::project::ProjectLiveSet::liveSetId);
    require(sets.size() == 2 && opened.liveSetId != "grid1" && joined != sets.end() && joined->displayName == "Set A",
        "the grid should join the project with an id of its own");
    const auto* target = std::get_if<talas::project::ProjectLiveSetSoundTarget>(&joined->cells.front().target);
    const auto& sounds = opened.document->soundDefinitions;
    const auto played = target == nullptr ? sounds.end()
                                          : std::ranges::find(sounds, target->soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    require(played != sounds.end() && target->soundDefinitionId != "pad"
            && played->graph.nodes.front().parameters.front().expression == std::optional<std::string>{"440 hz"}
            && sounds.front().graph.nodes.front().parameters.front().expression == std::optional<std::string>{"220 hz"},
        "the grid's Sound should come in beside the project's own pad, renamed");
}

int main()
{
    try
    {
        testSoundsGoIntoNumberedBoxesThatAnswerClicksAndPrograms();
        testToolsStepThroughCells();
        testCompiledSoundsAndTheGrid();
        testCompiledSoundGridFiles();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
