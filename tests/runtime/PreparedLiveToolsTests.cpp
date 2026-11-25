#include "talas/project/ProjectDocument.h"
#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/PreparedLiveTools.h"

#include <array>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument result;
    result.virtualControls.push_back({
        "Gate",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Toggle,
        0.0,
        0.0,
        1.0,
    });

    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "scene";
    liveSet.displayName = "Scene";
    talas::project::ProjectLiveSetCell cell;
    cell.cellId = "stop";
    cell.displayName = "Stop";
    cell.target = talas::project::ProjectLiveSetStopTarget{};
    cell.mappings.push_back(
        talas::project::ProjectLiveSetWorkbenchMapping{});
    liveSet.cells.push_back(std::move(cell));
    result.liveSetLibrary.liveSets.push_back(std::move(liveSet));

    talas::project::ProjectLiveTool tool;
    tool.toolId = "conductor";
    tool.displayName = "Conductor";
    tool.initialStateId = "idle";
    tool.states = {{"idle", "Idle"}, {"armed", "Armed"}};
    tool.variables.push_back({
        "enabled",
        "Enabled",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    tool.variables.push_back({
        "armedOnly",
        "Armed only",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::string{"armed"},
    });

    talas::project::ProjectLiveToolTransition arm;
    arm.transitionId = "arm";
    arm.fromStateId = "idle";
    arm.toStateId = "armed";
    arm.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    arm.trigger.variableId = "enabled";
    arm.guardExpression = "@enabled";
    talas::project::ProjectLiveToolCommand activate;
    activate.kind =
        talas::project::ProjectLiveToolCommandKind::ActivateCell;
    activate.liveSetId = "scene";
    activate.cellId = "stop";
    arm.commands.push_back(std::move(activate));
    tool.transitions.push_back(std::move(arm));

    talas::project::ProjectLiveToolTransition disarm;
    disarm.transitionId = "disarm";
    disarm.fromStateId = "armed";
    disarm.toStateId = "idle";
    disarm.trigger.kind = talas::project::ProjectLiveToolTriggerKind::Manual;
    disarm.guardExpression = "!Gate < 0.5";
    talas::project::ProjectLiveToolCommand stop;
    stop.kind = talas::project::ProjectLiveToolCommandKind::Stop;
    disarm.commands.push_back(stop);
    tool.transitions.push_back(std::move(disarm));

    talas::project::ProjectLiveToolTransition later;
    later.transitionId = "later";
    later.fromStateId = "armed";
    later.toStateId = "armed";
    later.trigger.kind = talas::project::ProjectLiveToolTriggerKind::Manual;
    later.commands.push_back(std::move(stop));
    tool.transitions.push_back(std::move(later));

    talas::project::ProjectLiveToolTransition setArmedLocal;
    setArmedLocal.transitionId = "set-armed-local";
    setArmedLocal.fromStateId = "armed";
    setArmedLocal.toStateId = "armed";
    setArmedLocal.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    setArmedLocal.trigger.variableId = "armedOnly";
    setArmedLocal.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    tool.transitions.push_back(std::move(setArmedLocal));
    result.liveSetLibrary.tools.push_back(std::move(tool));
    return result;
}

std::shared_ptr<const talas::runtime::PreparedLiveSet> prepareLiveSet(
    const talas::project::ProjectDocument& source)
{
    auto prepared = talas::runtime::prepareProjectLiveSet(
        source,
        {11, 17, 23, 48'000, true},
        {});
    require(prepared.ok(), "Live Tool fixture Live Set should prepare");
    return std::move(prepared.prepared);
}

struct Sink
{
    std::array<talas::project::ProjectLiveToolCommandKind, 64> kinds{};
    std::array<std::uint16_t, 64> cellIndices{};
    std::size_t count = 0;
    bool accept = true;
};

bool applyCommands(
    void* opaque,
    std::span<const talas::runtime::PreparedLiveToolCommand> commands) noexcept
{
    auto& sink = *static_cast<Sink*>(opaque);
    if (!sink.accept || commands.size() > sink.kinds.size() - sink.count)
        return false;
    for (std::size_t index = 0; index < commands.size(); ++index)
    {
        sink.kinds[sink.count + index] = commands[index].kind;
        sink.cellIndices[sink.count + index] =
            commands[index].preparedCellIndex;
    }
    sink.count += commands.size();
    return true;
}

bool resolveEvent(
    void*,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    if (name != "Gate")
        return false;
    output = {0.0, talas::expr::ValueDomain::Scalar};
    return true;
}

talas::runtime::PreparedLiveToolTriggerEvent trigger(
    talas::project::ProjectLiveToolTriggerKind kind,
    std::uint64_t sequence)
{
    talas::runtime::PreparedLiveToolTriggerEvent result;
    result.kind = kind;
    result.sequence = sequence;
    result.documentAuthorityGeneration = 11;
    result.stageGeneration = 17;
    result.stageId = 23;
    return result;
}

void testPreparedReducerIsOrderedBoundedAndFailureAtomic()
{
    const auto source = document();
    const auto liveSet = prepareLiveSet(source);
    const auto result = talas::runtime::prepareProjectLiveTools(
        source, *liveSet);
    require(result.ok() && result.prepared->tools().size() == 1,
        "valid closed Live Tool should compile beside its exact Live Set");

    talas::runtime::PreparedLiveToolRuntimeState state;
    require(talas::runtime::initializePreparedLiveToolRuntimeState(
                *result.prepared, state)
            && state.toolCount == 1 && state.variableCount == 2
            && state.stateIndices[0] == 0,
        "Live Tool runtime state should initialize into fixed storage");

    Sink sink;
    const talas::runtime::PreparedLiveToolEvaluationInputs inputs{
        nullptr, resolveEvent, 48'000.0, 120.0};
    auto unrelated = trigger(
        talas::project::ProjectLiveToolTriggerKind::Osc, 1);
    unrelated.oscAddress = "/unrelated";
    const auto noMatch = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        unrelated,
        inputs,
        {&sink, applyCommands},
        state);
    require(noMatch.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::NoMatchingTransition
            && state.stateIndices[0] == 0 && sink.count == 0,
        "unrelated source changes must not step a Live Tool");

    state.variables[0].boolean = true;
    const auto beforeGuardRejected = state;
    auto rejectedVariable = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 2);
    rejectedVariable.variableId = "enabled";
    rejectedVariable.hasVariableValue = true;
    rejectedVariable.variableBoolean = false;
    const auto guardRejected = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        rejectedVariable,
        inputs,
        {&sink, applyCommands},
        state);
    require(guardRejected.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::GuardRejected
            && state.stateIndices == beforeGuardRejected.stateIndices
            && state.variables == beforeGuardRejected.variables
            && state.lastTriggerSequences[0] == 2,
        "a rejected variable guard must roll back the candidate value while consuming its anti-replay sequence");

    auto enable = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 3);
    enable.variableId = "enabled";
    enable.hasVariableValue = true;
    enable.variableBoolean = true;
    const auto armed = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        enable,
        inputs,
        {&sink, applyCommands},
        state);
    require(armed.ok() && armed.commandCount == 1
            && state.stateIndices[0] == 1
            && state.variables[0].boolean
            && sink.kinds[0]
                == talas::project::ProjectLiveToolCommandKind::ActivateCell
            && sink.cellIndices[0] == 0,
        "variable trigger should evaluate its guard and emit one resolved cell command");

    auto armedLocal = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 4);
    armedLocal.variableId = "armedOnly";
    armedLocal.hasVariableValue = true;
    armedLocal.variableBoolean = true;
    const auto localApplied = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        armedLocal,
        inputs,
        {&sink, applyCommands},
        state);
    require(localApplied.ok() && state.variables[1].boolean,
        "state-scoped variable should be writable only while its owner state is active");

    const auto beforeRejected = state;
    sink.accept = false;
    const auto rejected = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        trigger(talas::project::ProjectLiveToolTriggerKind::Manual, 5),
        inputs,
        {&sink, applyCommands},
        state);
    require(rejected.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::CommandRejected
            && state.stateIndices[0] == beforeRejected.stateIndices[0]
            && state.variables == beforeRejected.variables,
        "rejected atomic command batch must preserve semantic tool state");
    const auto duplicate = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        trigger(talas::project::ProjectLiveToolTriggerKind::Manual, 5),
        inputs,
        {&sink, applyCommands},
        state);
    require(duplicate.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::InvalidTrigger,
        "one admitted trigger sequence must execute at most once after failure");

    sink.accept = true;
    sink.count = 0;
    const auto disarmed = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        trigger(talas::project::ProjectLiveToolTriggerKind::Manual, 6),
        inputs,
        {&sink, applyCommands},
        state);
    require(disarmed.ok() && state.stateIndices[0] == 0
            && sink.count == 1
            && sink.kinds[0]
                == talas::project::ProjectLiveToolCommandKind::Stop,
        "first true transition in document order should win exactly once");

    auto enableAgain = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 7);
    enableAgain.variableId = "enabled";
    enableAgain.hasVariableValue = true;
    enableAgain.variableBoolean = true;
    const auto reentered = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        enableAgain,
        inputs,
        {&sink, applyCommands},
        state);
    require(reentered.ok() && state.stateIndices[0] == 1
            && !state.variables[1].boolean,
        "entering a state must reset its scoped variables to their persisted initial values");

    auto inactiveLocal = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 8);
    inactiveLocal.variableId = "armedOnly";
    inactiveLocal.hasVariableValue = true;
    inactiveLocal.variableBoolean = true;
    state.stateIndices[0] = 0;
    const auto beforeInactive = state;
    const auto inactive = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        inactiveLocal,
        inputs,
        {&sink, applyCommands},
        state);
    require(inactive.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::InvalidTrigger
            && state == beforeInactive,
        "inactive state-scoped variables must not be writable or consume a trigger");
}

void testStaleAuthorityAndPreparationFailureHoldState()
{
    const auto source = document();
    const auto liveSet = prepareLiveSet(source);
    const auto result = talas::runtime::prepareProjectLiveTools(
        source, *liveSet);
    require(result.ok(), "Live Tool stale-authority fixture should prepare");
    talas::runtime::PreparedLiveToolRuntimeState state;
    require(talas::runtime::initializePreparedLiveToolRuntimeState(
                *result.prepared, state),
        "Live Tool stale-authority state should initialize");
    const auto before = state;
    auto stale = trigger(
        talas::project::ProjectLiveToolTriggerKind::Manual, 1);
    stale.stageGeneration = 18;
    Sink sink;
    const auto outcome = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        stale,
        {nullptr, resolveEvent, 48'000.0, 120.0},
        {&sink, applyCommands},
        state);
    require(outcome.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::StaleAuthority
            && state == before,
        "stale document/stage authority must preserve the entire runtime state");

    auto staleStageId = trigger(
        talas::project::ProjectLiveToolTriggerKind::Manual, 1);
    staleStageId.stageId = 24;
    const auto stageIdOutcome = talas::runtime::stepPreparedLiveTool(
        *result.prepared,
        0,
        staleStageId,
        {nullptr, resolveEvent, 48'000.0, 120.0},
        {&sink, applyCommands},
        state);
    require(stageIdOutcome.outcome
                == talas::runtime::PreparedLiveToolStepOutcome::StaleAuthority
            && state == before,
        "replacement stage identity must invalidate old Live Tool triggers even when generations match");

    auto invalid = source;
    invalid.liveSetLibrary.tools[0].transitions[0].guardExpression = "@missing";
    require(!talas::runtime::prepareProjectLiveTools(invalid, *liveSet).ok(),
        "invalid guard authority must fail before runtime publication");
}

void testRuntimeStatePublicationIsCoherentAndIncremental()
{
    const auto source = document();
    const auto liveSet = prepareLiveSet(source);
    const auto prepared = talas::runtime::prepareProjectLiveTools(
        source, *liveSet);
    require(prepared.ok(), "Live Tool publication fixture should prepare");
    talas::runtime::PreparedLiveToolRuntimeState state;
    require(talas::runtime::initializePreparedLiveToolRuntimeState(
                *prepared.prepared, state),
        "Live Tool publication fixture should initialize");
    talas::runtime::PreparedLiveToolRuntimeStatePublication publication;
    require(publication.publishAll(state),
        "initial Live Tool state should publish through fixed atomic storage");
    talas::runtime::PreparedLiveToolRuntimeState observed;
    require(publication.read(observed) && observed == state,
        "control reader should observe one coherent initial Live Tool snapshot");
    state.stateIndices[0] = 1;
    state.variables[0].boolean = true;
    state.lastTriggerSequences[0] = 9;
    require(publication.publishTool(state, 0)
                && publication.read(observed)
                && observed == state,
        "audio writer should update only one bounded tool while preserving a coherent full snapshot");
    publication.clear();
    require(publication.read(observed)
                && observed == talas::runtime::PreparedLiveToolRuntimeState{},
        "Live Tool state publication should revoke to one coherent empty snapshot");
}

void testAllTriggerKindsProduceOneDeterministicTrace()
{
    auto source = document();
    talas::project::ProjectLiveTool tool;
    tool.toolId = "trace";
    tool.displayName = "Trace";
    tool.initialStateId = "s0";
    for (int index = 0; index <= 6; ++index)
    {
        tool.states.push_back({
            "s" + std::to_string(index),
            "State " + std::to_string(index),
        });
    }
    tool.variables.push_back({
        "enabled",
        "Enabled",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    const auto addTransition = [&](std::string id,
                                   int from,
                                   talas::project::ProjectLiveToolTrigger expected) {
        talas::project::ProjectLiveToolTransition transition;
        transition.transitionId = std::move(id);
        transition.fromStateId = "s" + std::to_string(from);
        transition.toStateId = "s" + std::to_string(from + 1);
        transition.trigger = std::move(expected);
        transition.commands.push_back({
            talas::project::ProjectLiveToolCommandKind::Stop,
        });
        tool.transitions.push_back(std::move(transition));
    };
    talas::project::ProjectLiveToolTrigger manual;
    manual.kind = talas::project::ProjectLiveToolTriggerKind::Manual;
    addTransition("manual", 0, std::move(manual));
    talas::project::ProjectLiveToolTrigger event;
    event.kind = talas::project::ProjectLiveToolTriggerKind::EventValue;
    event.eventName = "Gate";
    event.eventDomain = talas::expr::ValueDomain::Scalar;
    addTransition("event", 1, std::move(event));
    talas::project::ProjectLiveToolTrigger variable;
    variable.kind = talas::project::ProjectLiveToolTriggerKind::VariableChange;
    variable.variableId = "enabled";
    addTransition("variable", 2, std::move(variable));
    talas::project::ProjectLiveToolTrigger midi;
    midi.kind = talas::project::ProjectLiveToolTriggerKind::MidiProgram;
    midi.midiProgram = {2, 5, "exact-midi"};
    addTransition("midi", 3, std::move(midi));
    talas::project::ProjectLiveToolTrigger osc;
    osc.kind = talas::project::ProjectLiveToolTriggerKind::Osc;
    osc.osc.address = "/trace";
    addTransition("osc", 4, std::move(osc));
    talas::project::ProjectLiveToolTrigger periodic;
    periodic.kind = talas::project::ProjectLiveToolTriggerKind::PeriodicTicks;
    periodic.periodTicks = 4;
    addTransition("periodic", 5, std::move(periodic));
    source.liveSetLibrary.tools = {std::move(tool)};

    const auto liveSet = prepareLiveSet(source);
    const auto prepared = talas::runtime::prepareProjectLiveTools(
        source, *liveSet);
    require(prepared.ok(),
        "all six closed Live Tool trigger kinds should prepare together");
    talas::runtime::PreparedLiveToolRuntimeState firstReplay;
    talas::runtime::PreparedLiveToolRuntimeState secondReplay;
    require(talas::runtime::initializePreparedLiveToolRuntimeState(
                *prepared.prepared, firstReplay)
            && talas::runtime::initializePreparedLiveToolRuntimeState(
                *prepared.prepared, secondReplay),
        "independent trigger trace replays should initialize identically");
    std::array<talas::runtime::PreparedLiveToolTriggerEvent, 6> events;
    events[0] = trigger(talas::project::ProjectLiveToolTriggerKind::Manual, 1);
    events[1] = trigger(talas::project::ProjectLiveToolTriggerKind::EventValue, 2);
    events[1].eventName = "Gate";
    events[1].eventDomain = talas::expr::ValueDomain::Scalar;
    events[2] = trigger(
        talas::project::ProjectLiveToolTriggerKind::VariableChange, 3);
    events[2].variableId = "enabled";
    events[2].hasVariableValue = true;
    events[2].variableBoolean = true;
    events[3] = trigger(
        talas::project::ProjectLiveToolTriggerKind::MidiProgram, 4);
    events[3].midiSourceIdentifier = "exact-midi";
    events[3].midiChannel = 2;
    events[3].midiProgram = 5;
    events[4] = trigger(talas::project::ProjectLiveToolTriggerKind::Osc, 5);
    events[4].oscAddress = "/trace";
    events[5] = trigger(
        talas::project::ProjectLiveToolTriggerKind::PeriodicTicks, 6);
    events[5].musicalTick = 8;
    Sink firstSink;
    Sink secondSink;
    const talas::runtime::PreparedLiveToolEvaluationInputs inputs{
        nullptr, resolveEvent, 48'000.0, 120.0};
    for (const auto& event : events)
    {
        const auto firstResult = talas::runtime::stepPreparedLiveTool(
            *prepared.prepared,
            0,
            event,
            inputs,
            {&firstSink, applyCommands},
            firstReplay);
        const auto secondResult = talas::runtime::stepPreparedLiveTool(
            *prepared.prepared,
            0,
            event,
            inputs,
            {&secondSink, applyCommands},
            secondReplay);
        require(firstResult.ok() && secondResult.ok()
                    && firstResult.stateIndex == secondResult.stateIndex
                    && firstResult.commandCount == secondResult.commandCount
                    && firstReplay == secondReplay,
            "independent portable replays should produce the same first-true state for every closed trigger kind");
    }
    require(firstReplay.stateIndices[0] == 6
                && firstReplay.variables[0].boolean
                && firstSink.count == 6
                && secondSink.count == 6
                && firstSink.kinds == secondSink.kinds
                && firstSink.cellIndices == secondSink.cellIndices,
        "all closed trigger kinds should advance exactly once and preserve one cumulative deterministic command trace");
}
}

int main()
{
    testPreparedReducerIsOrderedBoundedAndFailureAtomic();
    testStaleAuthorityAndPreparationFailureHoldState();
    testRuntimeStatePublicationIsCoherentAndIncremental();
    testAllTriggerKindsProduceOneDeterministicTrace();
}
