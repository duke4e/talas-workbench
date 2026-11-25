#include "talas/runtime/PreparedLiveTools.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectVariables.h"
#include "talas/runtime/RealtimeAudioEngine.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>

namespace talas::runtime
{
namespace
{
PreparedLiveToolsResult failure(std::string error)
{
    return {{}, std::move(error)};
}

std::optional<std::uint8_t> stateIndex(
    const talas::project::ProjectLiveTool& tool,
    std::string_view stateId) noexcept
{
    const auto found = std::ranges::find(
        tool.states, stateId, &talas::project::ProjectLiveToolState::stateId);
    if (found == tool.states.end())
        return std::nullopt;
    const auto index = static_cast<std::size_t>(
        std::distance(tool.states.begin(), found));
    return index <= std::numeric_limits<std::uint8_t>::max()
        ? std::optional{static_cast<std::uint8_t>(index)}
        : std::nullopt;
}

bool triggerMatches(
    const talas::project::ProjectLiveToolTrigger& expected,
    const PreparedLiveToolTriggerEvent& actual) noexcept
{
    if (expected.kind != actual.kind)
        return false;
    switch (expected.kind)
    {
    case talas::project::ProjectLiveToolTriggerKind::Manual:
        return true;
    case talas::project::ProjectLiveToolTriggerKind::EventValue:
        return expected.eventName == actual.eventName
            && expected.eventDomain == actual.eventDomain;
    case talas::project::ProjectLiveToolTriggerKind::VariableChange:
        return expected.variableId == actual.variableId
            && actual.hasVariableValue
            && std::isfinite(actual.variableNumber);
    case talas::project::ProjectLiveToolTriggerKind::MidiProgram:
        return expected.midiProgram.sourceIdentifier
                == actual.midiSourceIdentifier
            && expected.midiProgram.channel == actual.midiChannel
            && expected.midiProgram.program == actual.midiProgram;
    case talas::project::ProjectLiveToolTriggerKind::Osc:
        return expected.osc.address == actual.oscAddress;
    case talas::project::ProjectLiveToolTriggerKind::PeriodicTicks:
        return expected.periodTicks != 0
            && actual.musicalTick % expected.periodTicks == 0;
    }
    return false;
}

struct GuardContext
{
    const PreparedLiveTool* tool = nullptr;
    const PreparedLiveToolRuntimeState* state = nullptr;
    std::size_t variableOffset = 0;
    std::uint8_t activeStateIndex = 0;
    const PreparedLiveToolEvaluationInputs* inputs = nullptr;
};

bool resolveGuardVariable(
    void* opaque,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    const auto* context = static_cast<const GuardContext*>(opaque);
    if (context == nullptr || context->tool == nullptr
        || context->state == nullptr)
    {
        return false;
    }
    const auto found = std::ranges::find(
        context->tool->variables,
        name,
        &PreparedLiveToolVariable::variableId);
    if (found == context->tool->variables.end())
        return false;
    if (found->stateScopeIndex.has_value()
        && *found->stateScopeIndex != context->activeStateIndex)
    {
        return false;
    }
    const auto index = static_cast<std::size_t>(
        std::distance(context->tool->variables.begin(), found));
    if (context->variableOffset + index
        >= context->state->variableCount)
    {
        return false;
    }
    const auto& value =
        context->state->variables[context->variableOffset + index];
    output = {
        found->kind == talas::project::ProjectLiveToolVariableKind::Boolean
            ? (value.boolean ? 1.0 : 0.0)
            : value.number,
        talas::expr::ValueDomain::Scalar,
    };
    return std::isfinite(output.value);
}

bool resolveGuardEvent(
    void* opaque,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    const auto* context = static_cast<const GuardContext*>(opaque);
    return context != nullptr && context->inputs != nullptr
        && context->inputs->resolveEvent != nullptr
        && context->inputs->resolveEvent(
            context->inputs->context, name, output);
}

bool guardAllows(
    const PreparedLiveToolTransition& transition,
    const PreparedLiveTool& tool,
    const PreparedLiveToolRuntimeState& state,
    std::size_t variableOffset,
    std::uint8_t activeStateIndex,
    const PreparedLiveToolEvaluationInputs& inputs) noexcept
{
    if (!transition.guard.has_value())
        return true;
    if (!std::isfinite(inputs.sampleRate) || inputs.sampleRate <= 0.0
        || !std::isfinite(inputs.tempoBpm) || inputs.tempoBpm <= 0.0)
    {
        return false;
    }
    GuardContext resolver{
        &tool,
        &state,
        variableOffset,
        activeStateIndex,
        &inputs,
    };
    talas::expr::EvaluationContext context;
    context.sampleRate = inputs.sampleRate;
    context.tempoBpm = inputs.tempoBpm;
    context.user = &resolver;
    context.resolveVariable = resolveGuardVariable;
    context.resolveEvent = resolveGuardEvent;
    talas::expr::ExpressionValue value;
    return talas::expr::tryEvaluate(
               *transition.guard,
               context,
               {talas::expr::ValueDomain::Scalar},
               value)
        && value.domain == talas::expr::ValueDomain::Scalar
        && std::isfinite(value.value) && value.value != 0.0;
}
}

const PreparedLiveSetBinding& PreparedLiveTools::binding() const noexcept
{
    return binding_;
}

std::span<const PreparedLiveTool> PreparedLiveTools::tools() const noexcept
{
    return tools_;
}

std::optional<std::size_t> PreparedLiveTools::findTool(
    std::string_view toolId) const noexcept
{
    const auto found = std::ranges::find(
        tools_, toolId, &PreparedLiveTool::toolId);
    return found == tools_.end()
        ? std::optional<std::size_t>{}
        : static_cast<std::size_t>(std::distance(tools_.begin(), found));
}

PreparedLiveToolsResult prepareProjectLiveTools(
    const talas::project::ProjectDocument& document,
    const PreparedLiveSet& liveSet)
{
    const auto validation =
        talas::project::validateProjectLiveSetLibrary(document);
    if (!validation.ok())
        return failure("Project Live Tool library is invalid");
    const auto& binding = liveSet.binding();
    if (binding.documentAuthorityGeneration == 0
        || binding.stageGeneration == 0 || binding.stageId == 0
        || binding.sampleRate == 0)
    {
        return failure("Prepared Live Tools require exact Live Set authority");
    }
    try
    {
        auto prepared = std::make_shared<PreparedLiveTools>();
        prepared->binding_ = binding;
        prepared->tools_.reserve(document.liveSetLibrary.tools.size());
        std::size_t instructionCount = 0;
        for (const auto& tool : document.liveSetLibrary.tools)
        {
            PreparedLiveTool output;
            output.toolId = tool.toolId;
            output.stateIds.reserve(tool.states.size());
            for (const auto& state : tool.states)
                output.stateIds.push_back(state.stateId);
            const auto initial = stateIndex(tool, tool.initialStateId);
            if (!initial.has_value())
                return failure("Prepared Live Tool initial state is invalid");
            output.initialStateIndex = *initial;
            output.variables.reserve(tool.variables.size());
            for (const auto& variable : tool.variables)
            {
                std::optional<std::uint8_t> stateScopeIndex;
                if (variable.stateId.has_value())
                {
                    stateScopeIndex = stateIndex(tool, *variable.stateId);
                    if (!stateScopeIndex.has_value())
                        return failure("Prepared Live Tool variable state is invalid");
                }
                output.variables.push_back({
                    variable.variableId,
                    variable.kind,
                    variable.initialNumber,
                    variable.initialBoolean,
                    stateScopeIndex,
                });
            }
            output.transitions.reserve(tool.transitions.size());
            for (const auto& transition : tool.transitions)
            {
                const auto from = stateIndex(tool, transition.fromStateId);
                const auto to = stateIndex(tool, transition.toStateId);
                if (!from.has_value() || !to.has_value())
                    return failure("Prepared Live Tool transition state is invalid");
                PreparedLiveToolTransition compiled;
                compiled.transitionId = transition.transitionId;
                compiled.fromStateIndex = *from;
                compiled.toStateIndex = *to;
                compiled.trigger = transition.trigger;
                if (!transition.guardExpression.empty())
                {
                    auto guard = talas::expr::parse(
                        transition.guardExpression,
                        talas::expr::ExpressionLanguageVersion::VariablesV1,
                        talas::project::liveToolGuardDefinitions(document));
                    if (!guard.ok() || guard.expression->isStateful())
                        return failure("Prepared Live Tool guard compilation failed");
                    if (guard.expression->instructionCount()
                        > talas::project::maximumPreparedLiveToolInstructions
                            - instructionCount)
                    {
                        return failure("Prepared Live Tool instruction budget exceeded");
                    }
                    instructionCount += guard.expression->instructionCount();
                    compiled.guard = std::move(*guard.expression);
                }
                compiled.commands.reserve(transition.commands.size());
                for (const auto& command : transition.commands)
                {
                    PreparedLiveToolCommand preparedCommand;
                    preparedCommand.kind = command.kind;
                    preparedCommand.variableId = command.variableId;
                    preparedCommand.eventName = command.eventName;
                    preparedCommand.eventDomain = command.eventDomain;
                    preparedCommand.value = command.value;
                    if (command.kind
                        == talas::project::ProjectLiveToolCommandKind::ActivateCell)
                    {
                        const auto cell = liveSet.findCell(
                            command.liveSetId, command.cellId);
                        if (!cell.has_value()
                            || *cell > std::numeric_limits<std::uint16_t>::max())
                        {
                            return failure("Prepared Live Tool cell command is invalid");
                        }
                        preparedCommand.preparedCellIndex =
                            static_cast<std::uint16_t>(*cell);
                    }
                    compiled.commands.push_back(std::move(preparedCommand));
                }
                output.transitions.push_back(std::move(compiled));
            }
            prepared->tools_.push_back(std::move(output));
        }
        return {std::move(prepared), {}};
    }
    catch (const std::bad_alloc&)
    {
        return failure("Prepared Live Tool allocation failed");
    }
    catch (const std::length_error&)
    {
        return failure("Prepared Live Tool capacity was exceeded");
    }
}

PreparedLiveToolsResult bindPreparedLiveToolTargets(
    const PreparedLiveTools& prepared,
    const RealtimeAudioEngine& engine)
{
    try
    {
        auto bound = std::make_shared<PreparedLiveTools>(prepared);
        for (auto& tool : bound->tools_)
        {
            for (auto& transition : tool.transitions)
            {
                for (auto& command : transition.commands)
                {
                    command.typedTarget.reset();
                    switch (command.kind)
                    {
                    case talas::project::ProjectLiveToolCommandKind::ActivateCell:
                    case talas::project::ProjectLiveToolCommandKind::Stop:
                        break;
                    case talas::project::ProjectLiveToolCommandKind::SetProjectVariable:
                    case talas::project::ProjectLiveToolCommandKind::SetWorkspaceVariable:
                    {
                        const talas::project::PreparedProjectVariableIdentity identity{
                            command.kind == talas::project::ProjectLiveToolCommandKind::SetProjectVariable
                                ? talas::project::ProjectVariableOwnerKind::Project
                                : talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                            command.variableId,
                            {},
                            {},
                        };
                        command.typedTarget = engine.variableTarget(
                            identity,
                            talas::expr::ValueDomain::Scalar,
                            prepared.binding().stageGeneration);
                        break;
                    }
                    case talas::project::ProjectLiveToolCommandKind::SetEventValue:
                        command.typedTarget = engine.eventValueTarget(
                            command.eventName,
                            command.eventDomain,
                            prepared.binding().stageGeneration);
                        break;
                    }
                    if ((command.kind
                            == talas::project::ProjectLiveToolCommandKind::SetProjectVariable
                         || command.kind
                            == talas::project::ProjectLiveToolCommandKind::SetWorkspaceVariable
                         || command.kind
                            == talas::project::ProjectLiveToolCommandKind::SetEventValue)
                        && !command.typedTarget.has_value())
                    {
                        return {
                            nullptr,
                            "Live Tool scalar command target is unavailable in the accepted realtime stage",
                        };
                    }
                }
            }
        }
        return {std::move(bound), {}};
    }
    catch (const std::bad_alloc&)
    {
        return {nullptr, "Live Tool target binding allocation failed"};
    }
    catch (const std::length_error&)
    {
        return {nullptr, "Live Tool target binding exceeded fixed limits"};
    }
}

bool initializePreparedLiveToolRuntimeState(
    const PreparedLiveTools& prepared,
    PreparedLiveToolRuntimeState& state) noexcept
{
    if (prepared.tools().size() > talas::project::maximumProjectLiveTools)
        return false;
    PreparedLiveToolRuntimeState candidate;
    candidate.documentAuthorityGeneration =
        prepared.binding().documentAuthorityGeneration;
    candidate.stageGeneration = prepared.binding().stageGeneration;
    candidate.stageId = prepared.binding().stageId;
    candidate.toolCount = prepared.tools().size();
    std::size_t variableOffset = 0;
    for (std::size_t toolIndex = 0;
         toolIndex < prepared.tools().size();
         ++toolIndex)
    {
        const auto& tool = prepared.tools()[toolIndex];
        if (tool.variables.size()
                > talas::project::maximumProjectLiveToolVariables
            || variableOffset
                > talas::project::maximumPreparedLiveToolVariables
                    - tool.variables.size())
        {
            return false;
        }
        candidate.stateIndices[toolIndex] = tool.initialStateIndex;
        candidate.variableOffsets[toolIndex] =
            static_cast<std::uint16_t>(variableOffset);
        candidate.variableCounts[toolIndex] =
            static_cast<std::uint8_t>(tool.variables.size());
        for (std::size_t variableIndex = 0;
             variableIndex < tool.variables.size();
             ++variableIndex)
        {
            const auto& variable = tool.variables[variableIndex];
            candidate.variables[variableOffset + variableIndex] = {
                variable.initialNumber,
                variable.initialBoolean,
            };
        }
        variableOffset += tool.variables.size();
    }
    candidate.variableCount = variableOffset;
    state = candidate;
    return true;
}

bool PreparedLiveToolRuntimeStatePublication::beginWrite(
    std::uint64_t& generation) noexcept
{
    generation = publicationGeneration_.load(std::memory_order_seq_cst);
    if (generation >= std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        publicationGeneration_.store(
            std::numeric_limits<std::uint64_t>::max(),
            std::memory_order_seq_cst);
        return false;
    }
    publicationGeneration_.store(
        generation + 1U, std::memory_order_seq_cst);
    return true;
}

void PreparedLiveToolRuntimeStatePublication::endWrite(
    std::uint64_t generation) noexcept
{
    publicationGeneration_.store(
        generation + 2U, std::memory_order_seq_cst);
}

bool PreparedLiveToolRuntimeStatePublication::publishAll(
    const PreparedLiveToolRuntimeState& state) noexcept
{
    if (state.toolCount > state.stateIndices.size()
        || state.variableCount > state.variables.size())
    {
        return false;
    }
    std::uint64_t generation = 0;
    if (!beginWrite(generation))
        return false;
    documentAuthorityGeneration_.store(
        state.documentAuthorityGeneration, std::memory_order_seq_cst);
    stageGeneration_.store(state.stageGeneration, std::memory_order_seq_cst);
    stageId_.store(state.stageId, std::memory_order_seq_cst);
    toolCount_.store(state.toolCount, std::memory_order_seq_cst);
    variableCount_.store(state.variableCount, std::memory_order_seq_cst);
    for (std::size_t index = 0; index < state.toolCount; ++index)
    {
        stateIndices_[index].store(
            state.stateIndices[index], std::memory_order_seq_cst);
        variableOffsets_[index].store(
            state.variableOffsets[index], std::memory_order_seq_cst);
        variableCounts_[index].store(
            state.variableCounts[index], std::memory_order_seq_cst);
        lastTriggerSequences_[index].store(
            state.lastTriggerSequences[index], std::memory_order_seq_cst);
    }
    for (std::size_t index = 0; index < state.variableCount; ++index)
    {
        variableNumbers_[index].store(
            std::bit_cast<std::uint64_t>(state.variables[index].number),
            std::memory_order_seq_cst);
        variableBooleans_[index].store(
            state.variables[index].boolean ? 1U : 0U,
            std::memory_order_seq_cst);
    }
    endWrite(generation);
    return true;
}

bool PreparedLiveToolRuntimeStatePublication::publishTool(
    const PreparedLiveToolRuntimeState& state,
    std::size_t toolIndex) noexcept
{
    if (toolIndex >= state.toolCount
        || toolIndex >= state.stateIndices.size())
    {
        return false;
    }
    const auto offset = state.variableOffsets[toolIndex];
    const auto count = state.variableCounts[toolIndex];
    if (offset > state.variableCount
        || count > state.variableCount - offset)
    {
        return false;
    }
    std::uint64_t generation = 0;
    if (!beginWrite(generation))
        return false;
    stateIndices_[toolIndex].store(
        state.stateIndices[toolIndex], std::memory_order_seq_cst);
    lastTriggerSequences_[toolIndex].store(
        state.lastTriggerSequences[toolIndex], std::memory_order_seq_cst);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto variableIndex = offset + index;
        variableNumbers_[variableIndex].store(
            std::bit_cast<std::uint64_t>(
                state.variables[variableIndex].number),
            std::memory_order_seq_cst);
        variableBooleans_[variableIndex].store(
            state.variables[variableIndex].boolean ? 1U : 0U,
            std::memory_order_seq_cst);
    }
    endWrite(generation);
    return true;
}

bool PreparedLiveToolRuntimeStatePublication::read(
    PreparedLiveToolRuntimeState& state) const noexcept
{
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publicationGeneration_.load(
            std::memory_order_seq_cst);
        if (before == std::numeric_limits<std::uint64_t>::max())
            return false;
        if ((before & 1U) != 0)
            continue;
        PreparedLiveToolRuntimeState candidate;
        candidate.documentAuthorityGeneration =
            documentAuthorityGeneration_.load(std::memory_order_seq_cst);
        candidate.stageGeneration = stageGeneration_.load(
            std::memory_order_seq_cst);
        candidate.stageId = stageId_.load(std::memory_order_seq_cst);
        candidate.toolCount = toolCount_.load(std::memory_order_seq_cst);
        candidate.variableCount = variableCount_.load(
            std::memory_order_seq_cst);
        if (candidate.toolCount > candidate.stateIndices.size()
            || candidate.variableCount > candidate.variables.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < candidate.toolCount; ++index)
        {
            candidate.stateIndices[index] = static_cast<std::uint8_t>(
                stateIndices_[index].load(std::memory_order_seq_cst));
            candidate.variableOffsets[index] = static_cast<std::uint16_t>(
                variableOffsets_[index].load(std::memory_order_seq_cst));
            candidate.variableCounts[index] = static_cast<std::uint8_t>(
                variableCounts_[index].load(std::memory_order_seq_cst));
            candidate.lastTriggerSequences[index] =
                lastTriggerSequences_[index].load(std::memory_order_seq_cst);
        }
        for (std::size_t index = 0; index < candidate.variableCount; ++index)
        {
            candidate.variables[index].number = std::bit_cast<double>(
                variableNumbers_[index].load(std::memory_order_seq_cst));
            candidate.variables[index].boolean =
                variableBooleans_[index].load(std::memory_order_seq_cst) != 0;
        }
        const auto after = publicationGeneration_.load(
            std::memory_order_seq_cst);
        if (before == after && (after & 1U) == 0)
        {
            state = candidate;
            return true;
        }
    }
    return false;
}

void PreparedLiveToolRuntimeStatePublication::clear() noexcept
{
    static_cast<void>(publishAll({}));
}

PreparedLiveToolStepResult stepPreparedLiveTool(
    const PreparedLiveTools& prepared,
    std::size_t toolIndex,
    const PreparedLiveToolTriggerEvent& trigger,
    const PreparedLiveToolEvaluationInputs& inputs,
    const PreparedLiveToolCommandSink& sink,
    PreparedLiveToolRuntimeState& state) noexcept
{
    if (toolIndex >= prepared.tools().size()
        || toolIndex >= state.toolCount || trigger.sequence == 0)
    {
        return {PreparedLiveToolStepOutcome::InvalidTrigger};
    }
    if (trigger.documentAuthorityGeneration
            != prepared.binding().documentAuthorityGeneration
        || trigger.stageGeneration != prepared.binding().stageGeneration
        || trigger.stageId != prepared.binding().stageId
        || state.documentAuthorityGeneration
            != prepared.binding().documentAuthorityGeneration
        || state.stageGeneration != prepared.binding().stageGeneration
        || state.stageId != prepared.binding().stageId)
    {
        return {PreparedLiveToolStepOutcome::StaleAuthority};
    }
    if (trigger.sequence <= state.lastTriggerSequences[toolIndex])
        return {PreparedLiveToolStepOutcome::InvalidTrigger};

    const auto& tool = prepared.tools()[toolIndex];
    const auto variableOffset = state.variableOffsets[toolIndex];
    std::optional<std::size_t> changedVariableIndex;
    PreparedLiveToolRuntimeVariable previousVariableValue;
    if (trigger.kind
            == talas::project::ProjectLiveToolTriggerKind::VariableChange
        && trigger.hasVariableValue)
    {
        const auto variable = std::ranges::find(
            tool.variables,
            trigger.variableId,
            &PreparedLiveToolVariable::variableId);
        if (variable == tool.variables.end())
            return {PreparedLiveToolStepOutcome::InvalidTrigger};
        if (variable->stateScopeIndex.has_value()
            && *variable->stateScopeIndex
                != state.stateIndices[toolIndex])
        {
            return {PreparedLiveToolStepOutcome::InvalidTrigger};
        }
        const auto variableIndex = static_cast<std::size_t>(
            std::distance(tool.variables.begin(), variable));
        if (variableOffset + variableIndex >= state.variableCount)
            return {PreparedLiveToolStepOutcome::InvalidTrigger};
        changedVariableIndex = variableIndex;
        previousVariableValue =
            state.variables[variableOffset + variableIndex];
        auto& value = state.variables[variableOffset + variableIndex];
        if (variable->kind
            == talas::project::ProjectLiveToolVariableKind::Boolean)
        {
            value.boolean = trigger.variableBoolean;
            value.number = 0.0;
        }
        else
        {
            if (!std::isfinite(trigger.variableNumber))
                return {PreparedLiveToolStepOutcome::InvalidTrigger};
            value.number = trigger.variableNumber;
            value.boolean = false;
        }
    }

    auto rejectedGuard = false;
    for (const auto& transition : tool.transitions)
    {
        if (transition.fromStateIndex != state.stateIndices[toolIndex]
            || !triggerMatches(transition.trigger, trigger))
        {
            continue;
        }
        if (!guardAllows(
                transition,
                tool,
                state,
                variableOffset,
                state.stateIndices[toolIndex],
                inputs))
        {
            rejectedGuard = true;
            continue;
        }
        if (transition.commands.empty()
            || transition.commands.size()
                > talas::project::maximumProjectLiveToolCommandsPerTransition
            || sink.apply == nullptr
            || !sink.apply(sink.context, transition.commands))
        {
            if (changedVariableIndex.has_value())
            {
                state.variables[variableOffset + *changedVariableIndex] =
                    previousVariableValue;
            }
            state.lastTriggerSequences[toolIndex] = trigger.sequence;
            return {
                PreparedLiveToolStepOutcome::CommandRejected,
                state.stateIndices[toolIndex],
                0,
            };
        }
        if (transition.toStateIndex != state.stateIndices[toolIndex])
        {
            state.stateIndices[toolIndex] = transition.toStateIndex;
            for (std::size_t variableIndex = 0;
                 variableIndex < tool.variables.size();
                 ++variableIndex)
            {
                const auto& variable = tool.variables[variableIndex];
                if (!variable.stateScopeIndex.has_value()
                    || *variable.stateScopeIndex
                        != transition.toStateIndex)
                {
                    continue;
                }
                auto& runtimeVariable =
                    state.variables[variableOffset + variableIndex];
                runtimeVariable.number = variable.initialNumber;
                runtimeVariable.boolean = variable.initialBoolean;
            }
        }
        state.lastTriggerSequences[toolIndex] = trigger.sequence;
        return {
            PreparedLiveToolStepOutcome::Applied,
            state.stateIndices[toolIndex],
            static_cast<std::uint8_t>(transition.commands.size()),
        };
    }
    if (changedVariableIndex.has_value())
    {
        state.variables[variableOffset + *changedVariableIndex] =
            previousVariableValue;
    }
    state.lastTriggerSequences[toolIndex] = trigger.sequence;
    return {
        rejectedGuard
            ? PreparedLiveToolStepOutcome::GuardRejected
            : PreparedLiveToolStepOutcome::NoMatchingTransition,
        state.stateIndices[toolIndex],
        0,
    };
}
}
