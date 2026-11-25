#include "talas/runtime/ProcessorPlacement.h"

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace talas::runtime
{
namespace
{
constexpr std::string_view forcedProcessorAssignmentType =
    "talas.forcedProcessorAssignment";

struct StepIdentity
{
    std::string_view nodeId;
    RuntimeScopeId scopeId = globalRuntimeScope;

    bool operator==(const StepIdentity&) const = default;
};

struct StepIdentityHash
{
    std::size_t operator()(const StepIdentity& identity) const noexcept
    {
        auto seed = std::hash<std::string_view>{}(identity.nodeId);
        seed ^= std::hash<RuntimeScopeId>{}(identity.scopeId)
            + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct Boundary
{
    std::size_t stepIndex = 0;
    LogicalProcessorId processorId = hostLogicalProcessor;
};

ProcessorPlacementBuildResult fail(
    std::string error,
    std::size_t stepIndex = invalidProcessorPlacementIndex,
    std::size_t inputBindingIndex = invalidProcessorPlacementIndex)
{
    return {
        std::nullopt,
        std::move(error),
        stepIndex,
        inputBindingIndex,
    };
}

[[nodiscard]] std::optional<LogicalProcessorId>
exactProcessorId(const CompileStep& step)
{
    const ParameterValue* processor = nullptr;
    for (const auto& parameter : step.parameters)
    {
        if (parameter.id != "processor")
            continue;
        if (processor != nullptr)
            return std::nullopt;
        processor = &parameter;
    }
    if (processor == nullptr || !std::isfinite(processor->value)
        || std::floor(processor->value) != processor->value
        || processor->value < 1.0
        || processor->value
            > static_cast<double>(
                std::numeric_limits<LogicalProcessorId>::max()))
    {
        return std::nullopt;
    }
    return static_cast<LogicalProcessorId>(processor->value);
}

[[nodiscard]] bool hasDynamicProcessorExpression(
    const CompileStep& step) noexcept
{
    const auto processor = std::ranges::find(
        step.parameters,
        std::string_view{"processor"},
        &ParameterValue::id);
    if (processor == step.parameters.end()
        || !processor->expression.has_value())
    {
        return false;
    }
    const auto& expression = *processor->expression;
    return expression.isStateful()
        || !expression.eventReferences().empty()
        || !expression.variableReferences().empty();
}
}

ProcessorPlacementBuildResult
buildProcessorPlacementPlan(const RuntimePlan& runtimePlan)
{
    try
    {
        if (runtimePlan.steps.size() > maximumProcessorPlacementSteps)
        {
            return fail(
                "Processor placement supports at most 8,192 reachable steps");
        }

        std::size_t bindingCount = 0;
        for (std::size_t stepIndex = 0;
             stepIndex < runtimePlan.steps.size();
             ++stepIndex)
        {
            const auto inputCount =
                runtimePlan.steps[stepIndex].inputs.size();
            if (inputCount
                > maximumProcessorPlacementBindings - bindingCount)
            {
                return fail(
                    "Processor placement supports at most 65,536 input bindings",
                    stepIndex);
            }
            bindingCount += inputCount;
        }

        std::unordered_map<StepIdentity, std::size_t, StepIdentityHash>
            stepIndices;
        stepIndices.reserve(runtimePlan.steps.size());
        for (std::size_t stepIndex = 0;
             stepIndex < runtimePlan.steps.size();
             ++stepIndex)
        {
            const auto& step = runtimePlan.steps[stepIndex];
            if (!stepIndices.emplace(
                    StepIdentity{step.nodeId, step.scopeId},
                    stepIndex)
                     .second)
            {
                return fail(
                    "Processor placement requires unique node and scope identities",
                    stepIndex);
            }
        }

        std::vector<std::vector<std::size_t>> sourceStepIndices;
        sourceStepIndices.resize(runtimePlan.steps.size());
        for (std::size_t targetStepIndex = 0;
             targetStepIndex < runtimePlan.steps.size();
             ++targetStepIndex)
        {
            const auto& target = runtimePlan.steps[targetStepIndex];
            auto& sources = sourceStepIndices[targetStepIndex];
            sources.reserve(target.inputs.size());
            for (std::size_t bindingIndex = 0;
                 bindingIndex < target.inputs.size();
                 ++bindingIndex)
            {
                const auto& binding = target.inputs[bindingIndex];
                const auto source = stepIndices.find({
                    binding.sourceNodeId,
                    binding.sourceScopeId,
                });
                if (source == stepIndices.end())
                {
                    return fail(
                        "Processor placement cannot resolve an exact input source step",
                        targetStepIndex,
                        bindingIndex);
                }
                sources.push_back(source->second);
            }
        }

        std::vector<std::vector<std::size_t>> adjacency(
            runtimePlan.steps.size());
        for (std::size_t targetStepIndex = 0;
             targetStepIndex < sourceStepIndices.size();
             ++targetStepIndex)
        {
            for (const auto sourceStepIndex :
                 sourceStepIndices[targetStepIndex])
            {
                adjacency[sourceStepIndex].push_back(
                    targetStepIndex);
            }
        }
        for (std::size_t pairIndex = 0;
             pairIndex < runtimePlan.feedbackPairs.size();
             ++pairIndex)
        {
            const auto& pair = runtimePlan.feedbackPairs[pairIndex];
            if (pair.inputBufferIndex >= runtimePlan.buffers.size()
                || pair.outputStepIndex >= runtimePlan.steps.size())
            {
                return fail(
                    "Processor placement received invalid feedback pair authority");
            }
            const auto& inputBuffer =
                runtimePlan.buffers[pair.inputBufferIndex];
            const auto source = stepIndices.find({
                inputBuffer.nodeId,
                inputBuffer.scopeId,
            });
            if (source == stepIndices.end())
            {
                return fail(
                    "Processor placement cannot resolve the feedback input source step");
            }
            adjacency[source->second].push_back(pair.outputStepIndex);
        }
        for (auto& targets : adjacency)
        {
            std::ranges::sort(targets);
            targets.erase(
                std::unique(targets.begin(), targets.end()),
                targets.end());
        }

        std::vector<std::vector<std::size_t>> reverseAdjacency(
            adjacency.size());
        for (std::size_t sourceStepIndex = 0;
             sourceStepIndex < adjacency.size();
             ++sourceStepIndex)
        {
            for (const auto targetStepIndex : adjacency[sourceStepIndex])
                reverseAdjacency[targetStepIndex].push_back(sourceStepIndex);
        }
        for (auto& sources : reverseAdjacency)
            std::ranges::sort(sources);

        struct DfsFrame
        {
            std::size_t stepIndex = 0;
            std::size_t nextTargetIndex = 0;
        };
        std::vector<bool> visited(adjacency.size(), false);
        std::vector<DfsFrame> dfs;
        dfs.reserve(adjacency.size());
        std::vector<std::size_t> finishOrder;
        finishOrder.reserve(adjacency.size());
        for (std::size_t root = 0; root < adjacency.size(); ++root)
        {
            if (visited[root])
                continue;
            visited[root] = true;
            dfs.push_back({root, 0});
            while (!dfs.empty())
            {
                auto& frame = dfs.back();
                const auto& targets = adjacency[frame.stepIndex];
                if (frame.nextTargetIndex < targets.size())
                {
                    const auto target = targets[frame.nextTargetIndex++];
                    if (!visited[target])
                    {
                        visited[target] = true;
                        dfs.push_back({target, 0});
                    }
                    continue;
                }
                finishOrder.push_back(frame.stepIndex);
                dfs.pop_back();
            }
        }

        std::vector<FeedbackComponentPlan> feedbackComponents;
        std::fill(visited.begin(), visited.end(), false);
        std::vector<std::size_t> sccWorklist;
        sccWorklist.reserve(adjacency.size());
        for (auto finish = finishOrder.rbegin();
             finish != finishOrder.rend();
             ++finish)
        {
            if (visited[*finish])
                continue;
            FeedbackComponentPlan component;
            visited[*finish] = true;
            sccWorklist.push_back(*finish);
            while (!sccWorklist.empty())
            {
                const auto member = sccWorklist.back();
                sccWorklist.pop_back();
                component.stepIndices.push_back(member);
                for (const auto source : reverseAdjacency[member])
                {
                    if (!visited[source])
                    {
                        visited[source] = true;
                        sccWorklist.push_back(source);
                    }
                }
            }
            std::ranges::sort(component.stepIndices);

            const auto containsStep = [&](std::size_t member) {
                return std::ranges::binary_search(
                    component.stepIndices,
                    member);
            };
            for (std::size_t pairIndex = 0;
                 pairIndex < runtimePlan.feedbackPairs.size();
                 ++pairIndex)
            {
                const auto& pair = runtimePlan.feedbackPairs[pairIndex];
                const auto& inputBuffer =
                    runtimePlan.buffers[pair.inputBufferIndex];
                const auto source = stepIndices.find({
                    inputBuffer.nodeId,
                    inputBuffer.scopeId,
                });
                if (source != stepIndices.end()
                    && containsStep(source->second)
                    && containsStep(pair.outputStepIndex))
                {
                    component.feedbackPairIndices.push_back(pairIndex);
                }
            }

            const auto hasSelfEdge =
                component.stepIndices.size() == 1
                && std::ranges::binary_search(
                    adjacency[component.stepIndices.front()],
                    component.stepIndices.front());
            const auto isCyclic =
                component.stepIndices.size() > 1 || hasSelfEdge;
            if (isCyclic && component.feedbackPairIndices.empty())
            {
                return fail(
                    "Processor placement graph contains a cyclic component without explicit feedback authority",
                    component.stepIndices.front());
            }
            if (isCyclic)
                feedbackComponents.push_back(std::move(component));
        }
        std::ranges::sort(
            feedbackComponents,
            {},
            [](const FeedbackComponentPlan& component) {
                return component.stepIndices.front();
            });
        const auto sameFeedbackComponent = [](
            const FeedbackComponentPlan& left,
            const FeedbackComponentPlan& right) {
            return left.stepIndices == right.stepIndices
                && left.feedbackPairIndices
                    == right.feedbackPairIndices;
        };
        if (!std::ranges::equal(
                feedbackComponents,
                runtimePlan.feedbackComponents,
                sameFeedbackComponent))
        {
            return fail(
                "Processor placement feedback component authority does not match the independently reconstructed graph");
        }

        std::vector<Boundary> boundaries;
        boundaries.reserve(runtimePlan.steps.size());
        for (std::size_t stepIndex = 0;
             stepIndex < runtimePlan.steps.size();
             ++stepIndex)
        {
            const auto& step = runtimePlan.steps[stepIndex];
            if (step.typeId != forcedProcessorAssignmentType)
                continue;

            if (hasDynamicProcessorExpression(step))
            {
                return fail(
                    "ForcedProcessorAssignment processor must be static and cannot reference Event Values, variables, or state",
                    stepIndex);
            }
            const auto processorId = exactProcessorId(step);
            if (!processorId.has_value())
            {
                return fail(
                    "ForcedProcessorAssignment processor must be an exact integer in 1...UINT32_MAX",
                    stepIndex);
            }
            const auto audioInput = std::ranges::count(
                step.inputs,
                std::string_view{"audioIn"},
                &InputBinding::inputPortId);
            if (audioInput != 1 || step.inputs.size() != 1)
            {
                return fail(
                    "ForcedProcessorAssignment placement requires exactly one bound audioIn",
                    stepIndex);
            }
            boundaries.push_back({stepIndex, *processorId});
        }

        ProcessorPlacementPlan placement;
        placement.stepProcessors.assign(
            runtimePlan.steps.size(), hostLogicalProcessor);
        std::vector<std::size_t> worklist;
        worklist.reserve(runtimePlan.steps.size());

        for (const auto& boundary : boundaries)
        {
            worklist.clear();
            worklist.push_back(boundary.stepIndex);
            while (!worklist.empty())
            {
                const auto stepIndex = worklist.back();
                worklist.pop_back();
                const auto& step = runtimePlan.steps[stepIndex];

                if (step.typeId == forcedProcessorAssignmentType
                    && stepIndex != boundary.stepIndex)
                {
                    continue;
                }

                const auto previousProcessor =
                    placement.stepProcessors[stepIndex];
                if (previousProcessor != hostLogicalProcessor)
                {
                    if (previousProcessor != boundary.processorId)
                    {
                        return fail(
                            "Processor placement step is claimed by logical processors "
                                + std::to_string(previousProcessor)
                                + " and "
                                + std::to_string(boundary.processorId),
                            stepIndex);
                    }
                    continue;
                }

                placement.stepProcessors[stepIndex] =
                    boundary.processorId;
                const auto& sources = sourceStepIndices[stepIndex];
                for (auto source = sources.rbegin();
                     source != sources.rend();
                     ++source)
                {
                    worklist.push_back(*source);
                }
            }
        }

        for (const auto& component : feedbackComponents)
        {
            if (component.stepIndices.empty()
                || component.stepIndices.front()
                    >= placement.stepProcessors.size())
            {
                return fail(
                    "Processor placement received invalid feedback component authority");
            }
            const auto processorId = placement.stepProcessors[
                component.stepIndices.front()];
            for (const auto stepIndex : component.stepIndices)
            {
                if (stepIndex >= placement.stepProcessors.size())
                {
                    return fail(
                        "Processor placement received invalid feedback step authority",
                        stepIndex);
                }
                if (placement.stepProcessors[stepIndex] != processorId)
                {
                    return fail(
                        "ForcedProcessorAssignment cannot split one explicit feedback component across logical processors",
                        stepIndex);
                }
            }
        }

        std::map<LogicalProcessorId, ProcessorPlacementDomain> domains;
        for (std::size_t stepIndex = 0;
             stepIndex < placement.stepProcessors.size();
             ++stepIndex)
        {
            const auto processorId =
                placement.stepProcessors[stepIndex];
            auto& domain = domains[processorId];
            domain.processorId = processorId;
            domain.stepIndices.push_back(stepIndex);
        }
        for (const auto& boundary : boundaries)
        {
            domains[boundary.processorId]
                .boundaryStepIndices.push_back(boundary.stepIndex);
        }
        placement.domains.reserve(domains.size());
        for (auto& [processorId, domain] : domains)
        {
            (void) processorId;
            placement.domains.push_back(std::move(domain));
        }

        placement.transfers.reserve(bindingCount);
        for (std::size_t targetStepIndex = 0;
             targetStepIndex < runtimePlan.steps.size();
             ++targetStepIndex)
        {
            const auto targetProcessor =
                placement.stepProcessors[targetStepIndex];
            for (std::size_t bindingIndex = 0;
                 bindingIndex
                    < sourceStepIndices[targetStepIndex].size();
                 ++bindingIndex)
            {
                const auto sourceStepIndex =
                    sourceStepIndices[targetStepIndex][bindingIndex];
                const auto sourceProcessor =
                    placement.stepProcessors[sourceStepIndex];
                if (sourceProcessor == targetProcessor)
                    continue;
                placement.transfers.push_back({
                    sourceStepIndex,
                    targetStepIndex,
                    bindingIndex,
                    sourceProcessor,
                    targetProcessor,
                });
            }
        }

        return {std::move(placement), {}};
    }
    catch (const std::bad_alloc&)
    {
        return fail("Processor placement allocation failed");
    }
    catch (const std::length_error&)
    {
        return fail("Processor placement exceeded supported limits");
    }
}

ProcessorPlacementValidationResult
validateProcessorPlacementPlan(const RuntimePlan& runtimePlan)
{
    const auto expected = buildProcessorPlacementPlan(runtimePlan);
    if (!expected.ok())
        return {expected.error};
    if (runtimePlan.processorPlacement != *expected.plan)
    {
        return {
            "Processor placement metadata does not match the independently reconstructed graph authority",
        };
    }
    return {};
}
}
