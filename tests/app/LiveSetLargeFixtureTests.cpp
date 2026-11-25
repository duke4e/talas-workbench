#include "talas/app/LiveGridWorkbenchModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/runtime/PreparedLiveTools.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

constexpr std::size_t fullGridCount = 8;
constexpr std::size_t cellsPerGrid =
    talas::project::projectLiveSetRows
    * talas::project::projectLiveSetColumns;
constexpr std::size_t denseMappingCellCount = 363;
constexpr std::uint64_t validationBudgetNs = 2'000'000'000;
constexpr std::uint64_t codecBudgetNs = 5'000'000'000;
constexpr std::uint64_t preparationBudgetNs = 10'000'000'000;
constexpr std::uint64_t projectionP95BudgetNs = 10'000'000;
constexpr std::uint64_t callbackP99BudgetNs = 5'333'334;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::uint64_t elapsedNs(Clock::time_point started)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started)
            .count());
}

std::uint64_t nearestRank(
    std::vector<std::uint64_t> values,
    std::size_t numerator,
    std::size_t denominator)
{
    std::ranges::sort(values);
    const auto rank = (values.size() * numerator + denominator - 1U)
        / denominator;
    return values[std::max<std::size_t>(1, rank) - 1U];
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

talas::project::ProjectSoundDefinition voiceDefinition()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "c9Voice";
    definition.displayName = "C9 Large Voice";
    definition.graph.nodes.push_back({
        "tone",
        "talas.oscillator",
        {expressionParameter("frequency", "330 hz"),
         expressionParameter("amplitude", "0.25")},
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "tone",
        "audioOut",
    });
    return definition;
}

talas::project::ProjectDocument maximumDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"tone",
         "talas.oscillator",
         {expressionParameter("frequency", "220 hz"),
          expressionParameter("amplitude", "!Gain")}},
    };
    document.graph.connections.push_back(
        {"tone", "audioOut", "out", "audioIn"});
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.25,
        0.0,
        1.0,
    });
    document.variableLibrary.variables.push_back({
        .variableId = "c9SessionValue",
        .displayName = "C9 Session Value",
        .owner = {
            talas::project::ProjectVariableOwnerKind::WorkspaceSession},
        .snapshotEligible = false,
    });
    document.soundDefinitions.push_back(voiceDefinition());

    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    gainTarget.eventName = "Gain";
    gainTarget.domain = talas::expr::ValueDomain::Scalar;
    talas::project::SnapshotValue gainValue;
    gainValue.number = 0.5;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "c9Bank";
    bank.displayName = "C9 Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    bank.parameterPolicies.push_back({
        gainTarget,
        false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots.push_back({
        "half",
        "Half",
        {{gainTarget, gainValue}},
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));

    document.timeline.score.tracks.push_back({
        "master",
        "Master",
        talas::project::ScoreTrackKind::Master,
        talas::project::ScoreTimeDomain::Seconds,
        {},
        1,
        talas::project::ScoreOverlapMode::Layer,
    });
    document.timeline.score.markers.push_back({
        "c9Region",
        "C9 Region",
        {talas::project::ScoreTimeDomain::Seconds, 0.01, 0},
        talas::project::ScoreTime{
            talas::project::ScoreTimeDomain::Seconds, 0.02, 0},
    });

    std::size_t globalCellIndex = 0;
    std::size_t mappingCount = 0;
    document.liveSetLibrary.liveSets.reserve(
        talas::project::maximumProjectLiveSets);
    for (std::size_t setIndex = 0;
         setIndex < talas::project::maximumProjectLiveSets;
         ++setIndex)
    {
        talas::project::ProjectLiveSet set;
        set.liveSetId = "set" + std::to_string(setIndex);
        set.displayName = "Set " + std::to_string(setIndex);
        if (setIndex < fullGridCount)
        {
            set.cells.reserve(cellsPerGrid);
            for (std::size_t position = 0;
                 position < cellsPerGrid;
                 ++position, ++globalCellIndex)
            {
                talas::project::ProjectLiveSetCell cell;
                cell.cellId = "cell" + std::to_string(globalCellIndex);
                cell.displayName = "Cell " + std::to_string(globalCellIndex);
                cell.row = static_cast<std::uint32_t>(
                    position / talas::project::projectLiveSetColumns);
                cell.column = static_cast<std::uint32_t>(
                    position % talas::project::projectLiveSetColumns);
                if (globalCellIndex == 0)
                {
                    cell.target = talas::project::ProjectLiveSetSoundTarget{
                        "c9Voice", std::nullopt};
                }
                else if (globalCellIndex == 1)
                {
                    cell.target = talas::project::ProjectLiveSetSnapshotTarget{
                        "c9Bank", "half"};
                }
                else if (globalCellIndex == 2)
                {
                    cell.target =
                        talas::project::ProjectLiveSetScoreRegionTarget{
                            "c9Region"};
                }
                else
                {
                    cell.target = talas::project::ProjectLiveSetStopTarget{};
                }
                cell.mappings.push_back(
                    talas::project::ProjectLiveSetWorkbenchMapping{});
                const auto oscMappings = globalCellIndex
                        < denseMappingCellCount
                    ? 4U
                    : 3U;
                for (std::size_t mapping = 0;
                     mapping < oscMappings;
                     ++mapping)
                {
                    cell.mappings.push_back(
                        talas::project::ProjectLiveSetOscMapping{
                            "/c9/c" + std::to_string(globalCellIndex)
                            + "/m" + std::to_string(mapping)});
                }
                mappingCount += cell.mappings.size();
                set.cells.push_back(std::move(cell));
            }
        }
        document.liveSetLibrary.liveSets.push_back(std::move(set));
    }

    document.liveSetLibrary.tools.reserve(
        talas::project::maximumProjectLiveTools);
    for (std::size_t toolIndex = 0;
         toolIndex < talas::project::maximumProjectLiveTools;
         ++toolIndex)
    {
        talas::project::ProjectLiveTool tool;
        tool.toolId = "tool" + std::to_string(toolIndex);
        tool.displayName = "Tool " + std::to_string(toolIndex);
        tool.initialStateId = "ready";
        tool.states.push_back({"ready", "Ready"});
        tool.variables.reserve(
            talas::project::maximumProjectLiveToolVariables);
        for (std::size_t variableIndex = 0;
             variableIndex < talas::project::maximumProjectLiveToolVariables;
             ++variableIndex)
        {
            tool.variables.push_back({
                "v" + std::to_string(variableIndex),
                "Variable " + std::to_string(variableIndex),
                talas::project::ProjectLiveToolVariableKind::Number,
                0.0,
                false,
                std::nullopt,
            });
        }
        const auto transitionCount = toolIndex == 0
            ? talas::project::maximumProjectLiveToolTransitions
            : 1U;
        tool.transitions.reserve(transitionCount);
        for (std::size_t transitionIndex = 0;
             transitionIndex < transitionCount;
             ++transitionIndex)
        {
            talas::project::ProjectLiveToolTransition transition;
            transition.transitionId =
                "transition" + std::to_string(transitionIndex);
            transition.fromStateId = "ready";
            transition.toStateId = "ready";
            transition.trigger.kind =
                talas::project::ProjectLiveToolTriggerKind::Manual;
            const auto finalCandidate =
                transitionIndex + 1U == transitionCount;
            if (!finalCandidate)
                transition.guardExpression = "0 > 1";
            const auto commandCount = toolIndex == 0 && finalCandidate
                ? talas::project::maximumProjectLiveToolCommandsPerTransition
                : 1U;
            transition.commands.reserve(commandCount);
            for (std::size_t commandIndex = 0;
                 commandIndex < commandCount;
                 ++commandIndex)
            {
                if (commandIndex + 1U == commandCount)
                {
                    transition.commands.push_back({
                        talas::project::ProjectLiveToolCommandKind::Stop,
                    });
                }
                else
                {
                    talas::project::ProjectLiveToolCommand command;
                    command.kind = talas::project::ProjectLiveToolCommandKind::SetWorkspaceVariable;
                    command.variableId = "c9SessionValue";
                    command.value = static_cast<double>(commandIndex + 1U);
                    transition.commands.push_back(std::move(command));
                }
            }
            tool.transitions.push_back(std::move(transition));
        }
        document.liveSetLibrary.tools.push_back(std::move(tool));
    }

    const auto expectedRecords =
        document.liveSetLibrary.liveSets.size()
        + globalCellIndex
        + mappingCount
        + (1U + 1U
            + talas::project::maximumProjectLiveToolVariables
            + talas::project::maximumProjectLiveToolTransitions
            + (talas::project::maximumProjectLiveToolTransitions - 1U)
            + talas::project::maximumProjectLiveToolCommandsPerTransition)
        + (document.liveSetLibrary.tools.size() - 1U)
            * (1U + 1U
                + talas::project::maximumProjectLiveToolVariables
                + 1U + 1U);
    require(globalCellIndex == talas::project::maximumProjectLiveSetCells
            && mappingCount == 4'459
            && expectedRecords
                == talas::project::maximumProjectLiveSetInspectedRecords,
        "C9 maximum fixture must exactly consume the aggregate cell, mapping, tool-state, and inspection authorities");
    return document;
}

bool render(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::array<float, 256>& output)
{
    std::ranges::fill(output, 0.0F);
    std::array<float*, 1> channels{output.data()};
    return bridge.render(
        {},
        0.0,
        channels.data(),
        channels.size(),
        output.size());
}

void testMaximumLiveSetBudgetsAndPositiveExecution()
{
    const auto document = maximumDocument();
    const auto validationStarted = Clock::now();
    const auto validation =
        talas::project::validateProjectLiveSetLibrary(document);
    const auto validationNs = elapsedNs(validationStarted);
    require(validation.ok(),
        validation.diagnostics.empty()
            ? "maximum C9 fixture should validate"
            : validation.diagnostics.front().path + ": "
                + validation.diagnostics.front().message);
    require(sanitizerInstrumented || validationNs <= validationBudgetNs,
        "maximum C9 validation exceeded its 2 s Debug budget");
    auto overBound = document;
    overBound.liveSetLibrary.liveSets[7].cells.back().mappings.push_back(
        talas::project::ProjectLiveSetOscMapping{"/c9/over-bound"});
    const auto overBoundStarted = Clock::now();
    const auto overBoundValidation =
        talas::project::validateProjectLiveSetLibrary(overBound);
    const auto overBoundNs = elapsedNs(overBoundStarted);
    require(!overBoundValidation.ok()
                && std::ranges::any_of(
                    overBoundValidation.diagnostics,
                    [](const auto& diagnostic) {
                        return diagnostic.code
                            == talas::project::ProjectLiveSetValidationCode::CapacityExceeded;
                    })
                && (sanitizerInstrumented
                    || overBoundNs <= validationBudgetNs),
        "the first C9 record beyond the aggregate authority must fail within the bounded validation budget");

    const auto encodeStarted = Clock::now();
    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto encodeNs = elapsedNs(encodeStarted);
    const auto decodeStarted = Clock::now();
    const auto reopened = talas::project::parseProjectDocument(canonical);
    const auto decodeNs = elapsedNs(decodeStarted);
    require(reopened.ok()
                && reopened.document.liveSetLibrary
                    == document.liveSetLibrary
                && talas::project::serializeProjectDocument(reopened.document)
                    == canonical,
        "maximum C9 fixture must round-trip canonical format-34 bytes");
    require(sanitizerInstrumented
            || (encodeNs <= codecBudgetNs && decodeNs <= codecBudgetNs),
        "maximum C9 format-34 codec exceeded its 5 s Debug budget");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48'000.0;
    settings.maxBlockFrames = 256;
    const auto preparationStarted = Clock::now();
    const auto staged = bridge.compileAndStage(std::move(settings));
    const auto preparationNs = elapsedNs(preparationStarted);
    require(staged.ok(), staged.message);
    require(sanitizerInstrumented
            || preparationNs <= preparationBudgetNs,
        "maximum C9 preparation exceeded its 10 s Debug budget");

    bridge.setPlaying(true);
    std::array<float, 256> output{};
    require(render(bridge, output),
        "maximum C9 fixture should adopt its prepared graph");
    const auto prepared = bridge.latestPreparedLiveSet();
    const auto tools = bridge.latestPreparedLiveTools();
    require(prepared != nullptr
                && prepared->cells().size()
                    == talas::project::maximumProjectLiveSetCells
                && std::ranges::all_of(
                    prepared->cells(),
                    &talas::runtime::PreparedLiveSetCell::ready)
                && tools != nullptr
                && tools->tools().size()
                    == talas::project::maximumProjectLiveTools,
        "maximum C9 preparation must publish every cell and tool, not an empty timing path");
    talas::runtime::PreparedLiveToolRuntimeState initialToolState;
    require(talas::runtime::initializePreparedLiveToolRuntimeState(
                *tools, initialToolState)
                && initialToolState.variableCount
                    == talas::project::maximumPreparedLiveToolVariables,
        "maximum C9 tool state must fill the fixed runtime variable authority exactly");

    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Workbench, 1001, 1003);
    const talas::app::LiveGridSourceContext source{
        talas::runtime::RealtimeLiveSetSourceKind::Workbench,
        {},
        {},
        1,
        0,
        1001,
        1003,
        true,
    };
    talas::app::LiveGridProjectionEnvironment environment;
    environment.projectedLiveSetId = "set0";
    std::vector<std::uint64_t> projectionSamples;
    projectionSamples.reserve(64);
    std::vector<talas::app::LiveGridCellAction> projection;
    for (std::size_t sample = 0; sample < 68; ++sample)
    {
        const auto started = Clock::now();
        projection = talas::app::liveGridProjection(
            bridge, source, environment);
        const auto duration = elapsedNs(started);
        if (sample >= 4)
            projectionSamples.push_back(duration);
    }
    const auto projectionP95 = nearestRank(projectionSamples, 95, 100);
    require(projection.size() == cellsPerGrid
                && std::ranges::all_of(
                    projection,
                    [](const auto& action) {
                        return action.state
                                == talas::app::LiveGridCellState::Ready
                            && action.canActivate
                            && action.workbenchMapped;
                    })
                && projection[0].targetSummary.starts_with("Sound")
                && projection[1].targetSummary.starts_with("Snapshot")
                && projection[2].targetSummary.starts_with("Score")
                && projection[3].targetSummary == "Stop",
        "maximum C9 UI projection must expose one complete positive 8 by 16 grid with all four target kinds");
    require(sanitizerInstrumented
            || projectionP95 <= projectionP95BudgetNs,
        "maximum C9 UI projection exceeded its 10 ms p95 Debug budget");

    for (const auto* cellId : {
             "cell0", "cell1", "cell2", "cell3"})
    {
        bridge.setPlaying(true);
        const auto dispatched = talas::app::dispatchLiveGridCell(
            bridge, "set0", cellId, source, environment);
        const auto rendered = render(bridge, output);
        require(dispatched.activation.ok()
                    && (std::string_view{cellId} == "cell3" || rendered),
            std::string{"maximum C9 positive execution failed for "}
                + cellId + ": " + dispatched.action.reason + " / "
                + std::string{dispatched.activation.message});
    }
    const auto liveSetTelemetry = bridge.realtimeLiveSetTelemetry();
    require(liveSetTelemetry.completedActivations == 4
                && liveSetTelemetry.failedActivations == 0
                && liveSetTelemetry.activeCellIndexPlusOne == 0
                && !liveSetTelemetry.scoreRegionActive,
        "Sound, Snapshot, Score-region, and Stop cells must each execute before the final Stop holds the program");

    std::vector<std::uint64_t> callbackSamples;
    callbackSamples.reserve(64);
    const auto stageGeneration = bridge.latestStageGeneration();
    for (std::size_t sample = 0; sample < 64; ++sample)
    {
        bridge.setPlaying(true);
        const auto request = bridge.requestLiveToolTrigger({
            .toolId = "tool0",
            .triggerKind =
                talas::project::ProjectLiveToolTriggerKind::Manual,
            .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Offline,
            .sourceGeneration = stageGeneration,
            .routeGeneration = stageGeneration,
        });
        require(request.ok(),
            "maximum C9 tool request should enter the exact offline source authority");
        const auto started = Clock::now();
        require(render(bridge, output),
            "maximum C9 tool callback should render successfully");
        callbackSamples.push_back(elapsedNs(started));
    }
    const auto callbackP99 = nearestRank(callbackSamples, 99, 100);
    const auto toolTelemetry = bridge.realtimeLiveToolTelemetry();
    require(toolTelemetry.appliedTransitions == 64
                && toolTelemetry.stopCommands == 64
                && toolTelemetry.setWorkspaceVariableCommands
                    == 64U * 7U
                && toolTelemetry.rejectedTriggers == 0,
        "maximum C9 callback measurement must scan 256 candidates and execute eight bounded commands on every sample");
    require(sanitizerInstrumented
            || callbackP99 <= callbackP99BudgetNs,
        "maximum C9 callback exceeded one 48 kHz / 256-frame realtime deadline");

    std::cout
        << "C9 maximum budgets (ns): validation=" << validationNs
        << " over-bound=" << overBoundNs
        << " encode=" << encodeNs
        << " decode=" << decodeNs
        << " prepare=" << preparationNs
        << " projection-p95=" << projectionP95
        << " callback-p99=" << callbackP99
        << " cells=" << prepared->cells().size()
        << " tools=" << tools->tools().size()
        << " variables=" << initialToolState.variableCount << '\n';
}
}

int main()
{
    testMaximumLiveSetBudgetsAndPositiveExecution();
    return 0;
}
