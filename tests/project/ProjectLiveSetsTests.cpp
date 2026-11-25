#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectLiveSets.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

talas::project::ProjectLiveSetCell stopCell(
    std::string id = "stop",
    std::uint32_t row = 0,
    std::uint32_t column = 0)
{
    talas::project::ProjectLiveSetCell cell;
    cell.cellId = std::move(id);
    cell.displayName = "Stop";
    cell.row = row;
    cell.column = column;
    cell.target = talas::project::ProjectLiveSetStopTarget{};
    return cell;
}

talas::project::ProjectLiveSetLibrary validLibrary()
{
    talas::project::ProjectLiveSet set;
    set.liveSetId = "main";
    set.displayName = "Main Set";
    set.cells.push_back(stopCell());
    set.cells.front().mappings.push_back(
        talas::project::ProjectLiveSetWorkbenchMapping{});

    talas::project::ProjectLiveTool tool;
    tool.toolId = "transport";
    tool.displayName = "Transport";
    tool.initialStateId = "idle";
    tool.states.push_back({"idle", "Idle"});
    talas::project::ProjectLiveToolTransition transition;
    transition.transitionId = "stopNow";
    transition.fromStateId = "idle";
    transition.toStateId = "idle";
    transition.commands.push_back({});
    tool.transitions.push_back(std::move(transition));

    talas::project::ProjectLiveSetLibrary library;
    library.liveSets.push_back(std::move(set));
    library.tools.push_back(std::move(tool));
    return library;
}

talas::project::ProjectLiveTool validTool(std::string id)
{
    talas::project::ProjectLiveTool tool;
    tool.toolId = std::move(id);
    tool.displayName = "Tool";
    tool.initialStateId = "idle";
    tool.states.push_back({"idle", "Idle"});
    talas::project::ProjectLiveToolTransition transition;
    transition.transitionId = "stopNow";
    transition.fromStateId = "idle";
    transition.toStateId = "idle";
    transition.commands.push_back({});
    tool.transitions.push_back(std::move(transition));
    return tool;
}

void testTrackMuteRequiresSoundInItsColumn()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back({"voice", "Voice"});
    talas::project::ProjectLiveSet set;
    set.liveSetId = "parallel";
    set.displayName = "Parallel";
    auto sound = stopCell("sound", 0, 0);
    sound.target = talas::project::ProjectLiveSetSoundTarget{"voice", {}};
    auto mute = stopCell("mute", 1, 0);
    mute.target = talas::project::ProjectLiveSetTrackMuteTarget{true};
    set.cells = {sound, mute};
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    require(talas::project::validateProjectLiveSetLibrary(document).ok(),
            "track mute should validate beside a Sound in the same column");
    document.liveSetLibrary.liveSets.front().cells.back().column = 1;
    require(!talas::project::validateProjectLiveSetLibrary(document).ok(),
            "track mute without a Sound in its column should fail authoring validation");
}

void testTrackRoutesRejectCyclesAndUnboundEffects()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectLiveSet set;
    set.liveSetId = "routes";
    set.displayName = "Routes";
    for (std::uint32_t column = 0; column < 3; ++column)
    {
        const auto suffix = std::to_string(column);
        talas::project::ProjectSoundDefinition effect;
        effect.definitionId = "effect" + suffix;
        effect.displayName = effect.definitionId;
        effect.inputs.push_back({
            "audioIn", talas::core::PortType::Audio, "vca", "audioIn"});
        effect.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
        effect.graph.nodes.push_back({"vca", "talas.vca", {}});
        document.soundDefinitions.push_back(std::move(effect));
        const auto inputId = "input" + suffix;
        const auto instanceId = "instance" + suffix;
        document.graph.nodes.push_back({inputId, "talas.audioInput", {}});
        document.graph.nodes.push_back({
            instanceId, "", {}, "effect" + suffix});
        document.graph.connections.push_back({
            inputId, "audioOut", instanceId, "audioIn"});
        set.cells.push_back({
            "cell" + suffix, "Cell " + suffix, 0, column, {},
            talas::project::ProjectLiveSetSoundTarget{
                "effect" + suffix, instanceId}, {}});
    }
    set.routes = {{0, 1}, {0, 2}, {1, 2}};
    document.liveSetLibrary.liveSets.push_back(set);
    require(talas::project::validateProjectLiveSetLibrary(document).ok(),
            "fan-out and fan-in between Audio-input Sounds should validate");
    document.liveSetLibrary.liveSets.front().routes.push_back({0, 2});
    const auto duplicate = talas::project::validateProjectLiveSetLibrary(document);
    require(std::ranges::any_of(
                duplicate.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectLiveSetValidationCode::InvalidRoute;
                }),
            "duplicate directed Live Set edges should fail validation");
    document.liveSetLibrary.liveSets.front().routes.pop_back();
    document.liveSetLibrary.liveSets.front().routes.push_back({2, 0});
    const auto cycle = talas::project::validateProjectLiveSetLibrary(document);
    require(std::ranges::any_of(
                cycle.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectLiveSetValidationCode::RouteCycle;
                }),
            "a feedback cycle between Live Set tracks should fail before preparation");
    document.liveSetLibrary.liveSets.front().routes.pop_back();
    document.graph.nodes[2].typeId = "talas.noise";
    require(!talas::project::validateProjectLiveSetLibrary(document).ok(),
            "a route must not reinterpret a non-input root source as a submix boundary");
}

void testDenseAcyclicRouteLimit()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition effect;
    effect.definitionId = "effect";
    effect.displayName = "Effect";
    effect.graph.nodes.push_back({"amp", "talas.vca", {}});
    effect.inputs.push_back({
        "audioIn", talas::core::PortType::Audio, "amp", "audioIn"});
    effect.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "amp", "audioOut"});
    document.soundDefinitions.push_back(std::move(effect));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "dense";
    set.displayName = "Dense";
    for (std::uint32_t column = 0;
         column < talas::project::projectLiveSetColumns; ++column)
    {
        const auto suffix = std::to_string(column);
        const auto inputId = "input" + suffix;
        const auto instanceId = "instance" + suffix;
        document.graph.nodes.push_back({inputId, "talas.audioInput", {}});
        document.graph.nodes.push_back({
            instanceId, "", {}, std::string{"effect"}});
        document.graph.connections.push_back({
            inputId, "audioOut", instanceId, "audioIn"});
        set.cells.push_back({
            "cell" + suffix, "Cell " + suffix, 0, column, {},
            talas::project::ProjectLiveSetSoundTarget{
                "effect", instanceId}, {}});
        for (std::uint32_t destination = column + 1U;
             destination < talas::project::projectLiveSetColumns;
             ++destination)
            set.routes.push_back({column, destination});
    }
    require(set.routes.size()
                == talas::project::maximumProjectLiveSetRoutes,
            "dense DAG should fill the declared route capacity exactly");
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    require(talas::project::validateProjectLiveSetLibrary(document).ok(),
            "the maximum acyclic route graph should validate");
    document.liveSetLibrary.liveSets.front().routes.push_back({15, 0});
    const auto invalid = talas::project::validateProjectLiveSetLibrary(document);
    require(std::ranges::any_of(
                invalid.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectLiveSetValidationCode::CapacityExceeded;
                }),
            "a route beyond the dense DAG limit should exceed capacity");
    require(std::ranges::any_of(
                invalid.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectLiveSetValidationCode::RouteCycle;
                }),
            "the extra back-edge should also report the resulting cycle");
}

std::string formatThirtyThreeWithoutLiveSets(std::string current)
{
    const auto field = current.find("  \"liveSetLibrary\": {\n");
    require(field != std::string::npos, "current JSON should contain liveSetLibrary");
    const auto next = current.find("  \"metadata\": {\n", field);
    require(next != std::string::npos, "metadata should follow liveSetLibrary");
    current.erase(field, next - field);
    const auto version = current.find("\"version\": 156");
    require(version != std::string::npos, "current JSON should use format 34");
    current.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\":  33");
    return current;
}

void testClosedModelValidationAndReferences()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back({"voice", "Voice"});
    document.graph.nodes.push_back({"voiceInstance", {}, {}, "voice"});

    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "performance";
    bank.displayName = "Performance";
    bank.snapshots.push_back({"bright", "Bright", {}});
    document.snapshotLibrary.banks.push_back(std::move(bank));
    document.timeline.score.markers.push_back({
        "chorus",
        "Chorus",
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
        talas::project::ScoreTime{
            talas::project::ScoreTimeDomain::Seconds, 2.0, 0},
    });

    talas::project::ProjectLiveSet set;
    set.liveSetId = "main";
    set.displayName = "Main";
    auto sound = stopCell("sound", 0, 0);
    sound.target = talas::project::ProjectLiveSetSoundTarget{
        "voice", std::string{"voiceInstance"}};
    auto snapshot = stopCell("snapshot", 0, 1);
    snapshot.target = talas::project::ProjectLiveSetSnapshotTarget{
        "performance", "bright"};
    auto region = stopCell("region", 0, 2);
    region.target = talas::project::ProjectLiveSetScoreRegionTarget{"chorus"};
    set.cells = {sound, snapshot, region, stopCell("stop", 0, 3)};
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "all four closed live-set targets should resolve through shared authority");

    auto missing = document;
    missing.liveSetLibrary.liveSets.front().cells.front().target =
        talas::project::ProjectLiveSetSoundTarget{"missing", std::nullopt};
    const auto rejected = talas::project::validateProjectLiveSetLibrary(missing);
    require(
        !rejected.ok()
            && rejected.diagnostics.front().code
                == talas::project::ProjectLiveSetValidationCode::UnknownTarget,
        "missing target identity should fail closed");

    auto inputBound = document;
    inputBound.soundDefinitions.front().inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "input",
        "audioOut",
    });
    inputBound.liveSetLibrary.liveSets.front().cells.front().target =
        talas::project::ProjectLiveSetSoundTarget{"voice", std::nullopt};
    require(
        !talas::project::validateProjectLiveSetLibrary(inputBound).ok(),
        "Sound cells with boundary inputs should require an exact root source instance");
    inputBound.liveSetLibrary.liveSets.front().cells.front().target =
        talas::project::ProjectLiveSetSoundTarget{
            "voice", std::string{"voiceInstance"}};
    require(
        talas::project::validateProjectLiveSetLibrary(inputBound).ok(),
        "an exact root Sound instance should satisfy input-bound Sound authority");

    auto conflicting = document;
    conflicting.liveSetLibrary.liveSets.front().cells[0].mappings.push_back(
        talas::project::ProjectLiveSetOscMapping{"/launch"});
    conflicting.liveSetLibrary.liveSets.front().cells[1].mappings.push_back(
        talas::project::ProjectLiveSetOscMapping{"/launch"});
    const auto conflict =
        talas::project::validateProjectLiveSetLibrary(conflicting);
    require(
        !conflict.ok()
            && conflict.diagnostics.back().code
                == talas::project::ProjectLiveSetValidationCode::DuplicateMapping,
        "complete mapping set should reject ambiguous OSC authority");

    auto duplicateWorkbench = document;
    duplicateWorkbench.liveSetLibrary.liveSets.front().cells.front()
        .mappings = {
            talas::project::ProjectLiveSetWorkbenchMapping{},
            talas::project::ProjectLiveSetWorkbenchMapping{},
        };
    require(
        !talas::project::validateProjectLiveSetLibrary(duplicateWorkbench).ok(),
        "a cell should persist at most one explicit Workbench activation mapping");
}

void testFormatThirtyFourRoundTripAndMigration()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.liveSetLibrary = validLibrary();
    const auto canonical = talas::project::serializeProjectDocument(document);
    require(
        canonical.find("\"version\": 156") != std::string::npos
            && canonical.find("\"liveSetLibrary\"") != std::string::npos,
        "canonical writer should publish format 34 live-set authority");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok() && reopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && reopened.document.liveSetLibrary == document.liveSetLibrary
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "format 34 live sets and tools should round-trip canonically");

    const auto legacySource = formatThirtyThreeWithoutLiveSets(canonical);
    const auto migrated = talas::project::parseProjectDocument(legacySource);
    require(
        migrated.ok() && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.liveSetLibrary.liveSets.empty()
            && migrated.document.liveSetLibrary.tools.empty(),
        "format 33 should migrate to an empty format 34 live-set library");

    auto missingCurrent = canonical;
    const auto field = missingCurrent.find("  \"liveSetLibrary\": {\n");
    const auto next = missingCurrent.find("  \"metadata\": {\n", field);
    missingCurrent.erase(field, next - field);
    require(
        !talas::project::parseProjectDocument(missingCurrent).ok(),
        "format 34 should require the closed live-set library field");

    auto premature = legacySource;
    const auto insertion = premature.find("  \"metadata\": {\n");
    const auto sourceFieldStart = canonical.find("  \"liveSetLibrary\": {\n");
    const auto sourceFieldEnd = canonical.find("  \"metadata\": {\n", sourceFieldStart);
    premature.insert(
        insertion,
        canonical.substr(sourceFieldStart, sourceFieldEnd - sourceFieldStart));
    require(
        !talas::project::parseProjectDocument(premature).ok(),
        "historical formats should reject premature live-set authority");
}

void testParserBoundsAndExactNumbers()
{
    talas::project::ProjectDocument document;
    auto library = validLibrary();
    library.liveSets.front().cells.front().mappings.push_back(
        talas::project::ProjectLiveSetMidiProgramMapping{1, 7, "device-a"});
    document.liveSetLibrary = std::move(library);
    const auto canonical = talas::project::serializeProjectDocument(document);

    auto fractional = canonical;
    const auto channel = fractional.find("\"channel\": 1");
    require(channel != std::string::npos, "MIDI channel fixture should exist");
    fractional.replace(
        channel,
        std::string_view{"\"channel\": 1"}.size(),
        "\"channel\": 1.0000000000000001");
    require(
        !talas::project::parseProjectDocument(fractional).ok(),
        "binary64 rounding traps should not become exact MIDI integers");

    auto overlong = canonical;
    const auto cellId = overlong.find("\"cellId\": \"stop\"");
    require(cellId != std::string::npos, "cell identity fixture should exist");
    overlong.replace(
        cellId,
        std::string_view{"\"cellId\": \"stop\""}.size(),
        "\"cellId\": \"" + std::string(129, 'x') + "\"");
    require(
        !talas::project::parseProjectDocument(overlong).ok(),
        "oversized live-set strings should fail during bounded JSON parsing");

    auto oversizedUnknownKey = canonical;
    const auto keyInsertion = oversizedUnknownKey.find("\"cellId\": \"stop\"");
    require(keyInsertion != std::string::npos, "cell object fixture should exist");
    oversizedUnknownKey.insert(
        keyInsertion,
        "\"" + std::string(65, 'x') + "\": 0,\n        ");
    const auto unknownKeyResult =
        talas::project::parseProjectDocument(oversizedUnknownKey);
    require(
        !unknownKeyResult.ok()
            && unknownKeyResult.errors.front().message.find(
                   "object member string byte limit") != std::string::npos,
        "unknown live-set object keys should be bounded before DOM retention");

    auto nestedUnknown = canonical;
    const auto nestedInsertion = nestedUnknown.find("\"cellId\": \"stop\"");
    std::string nestedValue(16, '[');
    nestedValue += '0';
    nestedValue.append(16, ']');
    nestedUnknown.insert(
        nestedInsertion,
        "\"unknown\": " + nestedValue + ",\n        ");
    const auto nestedResult = talas::project::parseProjectDocument(nestedUnknown);
    require(
        !nestedResult.ok()
            && nestedResult.errors.front().message.find(
                   "bounded subtree nesting") != std::string::npos,
        "unknown live-set values should hit the local nesting bound before semantic closedness");

    auto oversizedNumber = canonical;
    const auto row = oversizedNumber.find("\"row\": 0");
    require(row != std::string::npos, "row token fixture should exist");
    oversizedNumber.replace(
        row,
        std::string_view{"\"row\": 0"}.size(),
        "\"row\": " + std::string(65, '1'));
    const auto numberResult = talas::project::parseProjectDocument(oversizedNumber);
    require(
        !numberResult.ok()
            && numberResult.errors.front().message.find(
                   "bounded subtree scalar token") != std::string::npos,
        "wrong-shaped live-set numeric tokens should fail within a fixed scan bound");
}

void testNewerDefinitionsDoNotInvalidateToolGuards()
{
    talas::project::ProjectDocument document;
    document.liveSetLibrary = validLibrary();
    auto& tool = document.liveSetLibrary.tools.front();
    tool.variables.push_back({
        "gain",
        "Gain",
        talas::project::ProjectLiveToolVariableKind::Number,
        0.5,
        false,
        std::nullopt,
    });
    tool.transitions.front().guardExpression = "@gain > 0";
    tool.transitions.front().trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    tool.transitions.front().trigger.variableId = "gain";
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV2;
    document.expressionDefinitions.definitions.push_back({
        "wrap",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value rem: 2",
    });
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "a project definition in a newer language must not invalidate a VariablesV1 guard");
}

void testToolGuardsAndClosedEnumShapes()
{
    talas::project::ProjectDocument document;
    document.liveSetLibrary = validLibrary();
    auto& tool = document.liveSetLibrary.tools.front();
    tool.variables.push_back({
        "gain",
        "Gain",
        talas::project::ProjectLiveToolVariableKind::Number,
        0.5,
        false,
        std::nullopt,
    });
    tool.transitions.front().guardExpression = "@gain > 0";
    tool.transitions.front().trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    tool.transitions.front().trigger.variableId = "gain";
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "a bounded VariablesV1 guard and variable-change trigger should use exact local tool authority independent of the project dialect");

    auto booleanGuard = document;
    auto& booleanTool = booleanGuard.liveSetLibrary.tools.front();
    booleanTool.variables.push_back({
        "flag",
        "Flag",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    booleanTool.transitions.front().guardExpression =
        "[0, 1][@flag] > 0";
    booleanTool.transitions.front().trigger.variableId = "flag";
    require(
        talas::project::validateProjectLiveSetLibrary(booleanGuard).ok(),
        "boolean tool locals should use exact zero/one guard witnesses rather than fractional number witnesses");

    auto malformed = document;
    malformed.liveSetLibrary.tools.front().transitions.front()
        .guardExpression = "1 +";
    require(
        !talas::project::validateProjectLiveSetLibrary(malformed).ok(),
        "malformed guard expressions should fail before persistence");

    auto wrongDomain = document;
    wrongDomain.liveSetLibrary.tools.front().transitions.front()
        .guardExpression = "1 hz";
    require(
        !talas::project::validateProjectLiveSetLibrary(wrongDomain).ok(),
        "live-tool guards should evaluate through the exact scalar boolean authority");

    auto stateful = document;
    stateful.liveSetLibrary.tools.front().transitions.front()
        .guardExpression = "previous(@gain, 0)";
    require(
        !talas::project::validateProjectLiveSetLibrary(stateful).ok(),
        "live-tool guards should reject stateful VariablesV1 programs");

    auto missingVariable = document;
    missingVariable.liveSetLibrary.tools.front().transitions.front()
        .guardExpression = "@missing > 0";
    require(
        !talas::project::validateProjectLiveSetLibrary(missingVariable).ok(),
        "guard expressions should not escape the tool-local variable authority");

    auto missingTriggerVariable = document;
    missingTriggerVariable.liveSetLibrary.tools.front().transitions.front()
        .trigger.variableId = "missing";
    require(
        !talas::project::validateProjectLiveSetLibrary(missingTriggerVariable).ok(),
        "variable-change triggers should not substitute project-owned declarations for tool-local state");

    auto invalidVariable = document;
    invalidVariable.liveSetLibrary.tools.front().variables.front()
        .initialBoolean = true;
    require(
        !talas::project::validateProjectLiveSetLibrary(invalidVariable).ok(),
        "number variables should reject noncanonical unused boolean state");

    auto invalidQuantization = document;
    invalidQuantization.liveSetLibrary.liveSets.front().cells.front()
        .quantization.kind =
            static_cast<talas::project::ProjectLiveSetQuantizationKind>(99);
    require(
        !talas::project::validateProjectLiveSetLibrary(invalidQuantization).ok(),
        "unknown in-memory quantization enum values should fail closed");

    auto oversizedIdentity = document;
    oversizedIdentity.liveSetLibrary.tools.front().initialStateId.assign(
        talas::project::maximumProjectLiveSetIdentifierBytes + 1U,
        'x');
    oversizedIdentity.liveSetLibrary.liveSets.front().cells.front()
        .mappings.push_back(talas::project::ProjectLiveSetMidiProgramMapping{
            1,
            1,
            std::string(
                talas::project::maximumProjectLiveSetIdentifierBytes * 8U,
                'x'),
        });
    require(
        !talas::project::validateProjectLiveSetLibrary(oversizedIdentity).ok(),
        "oversized in-memory identities and mapping sources should fail before conflict-key copying");
}

void testAggregateRecordBudget()
{
    talas::project::ProjectDocument document;
    document.variableLibrary.variables.push_back({
        .variableId = "sessionValue",
        .displayName = "Session value",
        .owner = {
            talas::project::ProjectVariableOwnerKind::WorkspaceSession},
        .snapshotEligible = false,
    });
    const auto appendDenseTool = [&](std::size_t toolIndex) {
        talas::project::ProjectLiveTool tool;
        tool.toolId = "tool" + std::to_string(toolIndex);
        tool.displayName = "Dense Tool";
        tool.initialStateId = "idle";
        tool.states.push_back({"idle", "Idle"});
        for (std::size_t transitionIndex = 0;
             transitionIndex
                 < talas::project::maximumProjectLiveToolTransitions;
             ++transitionIndex)
        {
            talas::project::ProjectLiveToolTransition transition;
            transition.transitionId =
                "transition" + std::to_string(transitionIndex);
            transition.fromStateId = "idle";
            transition.toStateId = "idle";
            transition.commands.resize(
                talas::project::maximumProjectLiveToolCommandsPerTransition);
            for (auto& command : transition.commands)
            {
                command.kind = talas::project::ProjectLiveToolCommandKind::SetWorkspaceVariable;
                command.variableId = "sessionValue";
            }
            tool.transitions.push_back(std::move(transition));
        }
        document.liveSetLibrary.tools.push_back(std::move(tool));
    };

    appendDenseTool(0);
    appendDenseTool(1);
    appendDenseTool(2);
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "a dense live-tool library should accept records below the aggregate limit");
    appendDenseTool(3);
    const auto rejected =
        talas::project::validateProjectLiveSetLibrary(document);
    require(
        !rejected.ok()
            && std::ranges::any_of(
                rejected.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectLiveSetValidationCode::CapacityExceeded;
                }),
        "the first record beyond the shared C9 inspection budget should fail closed");
}

void testEditorReplacementIsAtomicAndUndoable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(
        talas::project::ProjectDocument{}, registry);
    const auto accepted = validLibrary();
    require(
        editor.replaceLiveSetLibrary(accepted).ok()
            && editor.document().liveSetLibrary == accepted
            && editor.canUndo(),
        "one valid live-set replacement should create one Undo transaction");

    auto invalid = accepted;
    invalid.liveSets.front().cells.front().column =
        talas::project::projectLiveSetColumns;
    require(
        !editor.replaceLiveSetLibrary(std::move(invalid)).ok()
            && editor.document().liveSetLibrary == accepted,
        "invalid live-set replacement should leave document authority unchanged");
    require(
        editor.undo().ok()
            && editor.document().liveSetLibrary.liveSets.empty()
            && !editor.canUndo(),
        "live-set replacement should undo in exactly one step");
}

void testInverseDependencyRemovalIsAtomic()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back({"voice", "Voice"});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "performance";
    bank.displayName = "Performance";
    bank.snapshots.push_back({"bright", "Bright", {}});
    document.snapshotLibrary.banks.push_back(std::move(bank));
    document.timeline.score.markers.push_back({
        "region",
        "Region",
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
        talas::project::ScoreTime{
            talas::project::ScoreTimeDomain::Seconds, 2.0, 0},
    });
    document.virtualControls.push_back({"Trigger"});

    talas::project::ProjectLiveSet set;
    set.liveSetId = "main";
    set.displayName = "Main";
    auto sound = stopCell("sound", 0, 0);
    sound.target = talas::project::ProjectLiveSetSoundTarget{
        "voice", std::nullopt};
    auto snapshot = stopCell("snapshot", 0, 1);
    snapshot.target = talas::project::ProjectLiveSetSnapshotTarget{
        "performance", "bright"};
    auto region = stopCell("region", 0, 2);
    region.target = talas::project::ProjectLiveSetScoreRegionTarget{"region"};
    set.cells = {std::move(sound), std::move(snapshot), std::move(region)};
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    auto tool = validTool("events");
    tool.transitions.front().trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::EventValue;
    tool.transitions.front().trigger.eventName = "Trigger";
    document.liveSetLibrary.tools.push_back(std::move(tool));
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "inverse dependency fixture should begin valid");

    talas::project::ProjectEditor editor(document, registry);
    const auto authorityBefore = editor.documentAuthorityGeneration();
    const auto edit = [](std::string_view name, auto operation) {
        try
        {
            return operation();
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                std::string{name} + ": " + exception.what());
        }
    };
    require(
        !edit("remove Sound", [&] {
             return editor.removeSoundDefinition("voice");
         }).ok()
            && !edit("remove Snapshot", [&] {
                 return editor.removeSnapshot("performance", "bright");
             }).ok()
            && !edit("remove Snapshot bank", [&] {
                 return editor.removeSnapshotBank("performance");
             }).ok()
            && !edit("remove Event Value", [&] {
                 return editor.removeVirtualControl("Trigger");
             }).ok()
            && editor.document().soundDefinitions.size() == 1U
            && editor.document().snapshotLibrary.banks.size() == 1U
            && editor.document().snapshotLibrary.banks.front()
                   .snapshots.size() == 1U
            && editor.document().virtualControls.size() == 1U
            && editor.documentAuthorityGeneration() == authorityBefore
            && !editor.canUndo(),
        "deleting Sound, Snapshot, bank, or Event authority should not commit dangling live-set references");

    talas::project::ProjectScoreEditBatch removeMarker;
    removeMarker.removeMarkerIds.push_back("region");
    require(
        !edit("remove Score marker", [&] {
             return editor.applyScoreEdits(std::move(removeMarker));
         }).ok()
            && editor.document().timeline.score.markers.size() == 1U
            && editor.documentAuthorityGeneration() == authorityBefore
            && !editor.canUndo(),
        "Score marker removal should be rejected before dangling live-region publication");
}

void testEditorEntityOperationsAreOneUndoEach()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(
        talas::project::ProjectDocument{}, registry);
    talas::project::ProjectLiveSet first;
    first.liveSetId = "first";
    first.displayName = "First";
    first.cells.push_back(stopCell("firstStop"));
    talas::project::ProjectLiveSet second;
    second.liveSetId = "second";
    second.displayName = "Second";

    require(
        editor.upsertProjectLiveSet(first).ok()
            && editor.upsertProjectLiveSet(second).ok(),
        "live sets should support bounded add operations");
    require(
        editor.moveProjectLiveSet("second", 0).ok()
            && editor.document().liveSetLibrary.liveSets.front().liveSetId
                == "second"
            && editor.undo().ok()
            && editor.document().liveSetLibrary.liveSets.front().liveSetId
                == "first",
        "live-set move should consume exactly one Undo entry");

    require(
        editor.upsertProjectLiveSetCell(
            "first", stopCell("next", 0, 1)).ok()
            && editor.document().liveSetLibrary.liveSets.front().cells.size()
                == 2
            && editor.undo().ok()
            && editor.document().liveSetLibrary.liveSets.front().cells.size()
                == 1,
        "cell upsert should be one Undo transaction");
    require(
        editor.removeProjectLiveSetCell("first", "firstStop").ok()
            && editor.document().liveSetLibrary.liveSets.front().cells.empty()
            && editor.undo().ok()
            && editor.document().liveSetLibrary.liveSets.front().cells.size()
                == 1,
        "cell removal should be one Undo transaction");

    require(
        editor.upsertProjectLiveTool(validTool("one")).ok()
            && editor.upsertProjectLiveTool(validTool("two")).ok()
            && editor.moveProjectLiveTool("two", 0).ok()
            && editor.document().liveSetLibrary.tools.front().toolId == "two"
            && editor.undo().ok()
            && editor.document().liveSetLibrary.tools.front().toolId == "one",
        "tool add and move operations should preserve exact order and Undo");
    require(
        editor.removeProjectLiveTool("one").ok()
            && editor.document().liveSetLibrary.tools.size() == 1
            && editor.undo().ok()
            && editor.document().liveSetLibrary.tools.size() == 2,
        "tool removal should be one Undo transaction");
    require(
        editor.removeProjectLiveSet("second").ok()
            && editor.document().liveSetLibrary.liveSets.size() == 1
            && editor.undo().ok()
            && editor.document().liveSetLibrary.liveSets.size() == 2,
        "live-set removal should be one Undo transaction");
}
}

int main()
{
    const auto run = [](std::string_view name, auto test) {
        try
        {
            test();
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                std::string{name} + ": " + exception.what());
        }
    };
    run("closed model", testClosedModelValidationAndReferences);
    run("track mute column", testTrackMuteRequiresSoundInItsColumn);
    run("track route topology", testTrackRoutesRejectCyclesAndUnboundEffects);
    run("dense route boundary", testDenseAcyclicRouteLimit);
    run("format 34", testFormatThirtyFourRoundTripAndMigration);
    run("parser bounds", testParserBoundsAndExactNumbers);
    run("tool guards", testToolGuardsAndClosedEnumShapes);
    run("newer definitions keep guards valid", testNewerDefinitionsDoNotInvalidateToolGuards);
    run("aggregate budget", testAggregateRecordBudget);
    run("editor replacement", testEditorReplacementIsAtomicAndUndoable);
    run("inverse dependency", testInverseDependencyRemovalIsAtomic);
    run("editor entities", testEditorEntityOperationsAreOneUndoEach);
    return 0;
}
