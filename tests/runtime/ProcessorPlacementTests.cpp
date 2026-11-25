#include "talas/runtime/ProcessorPlacement.h"
#include "talas/runtime/Runtime.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::ParameterValue processor(double value)
{
    talas::runtime::ParameterValue result;
    result.id = "processor";
    result.value = value;
    return result;
}

talas::runtime::CompileStep step(
    std::string nodeId,
    std::string typeId,
    talas::runtime::RuntimeScopeId scopeId =
        talas::runtime::globalRuntimeScope,
    std::string path = {})
{
    talas::runtime::CompileStep result;
    result.nodeId = std::move(nodeId);
    result.typeId = std::move(typeId);
    result.scopeId = scopeId;
    result.structuralSourcePath = std::move(path);
    return result;
}

talas::runtime::InputBinding input(
    std::string inputPortId,
    std::string sourceNodeId,
    talas::runtime::RuntimeScopeId sourceScopeId,
    std::size_t bufferIndex = 0)
{
    return {
        std::move(inputPortId),
        std::move(sourceNodeId),
        "audioOut",
        bufferIndex,
        sourceScopeId,
    };
}

const talas::runtime::ProcessorPlacementDomain& requireDomain(
    const talas::runtime::ProcessorPlacementPlan& plan,
    talas::runtime::LogicalProcessorId processorId)
{
    for (const auto& domain : plan.domains)
    {
        if (domain.processorId == processorId)
            return domain;
    }
    throw std::runtime_error(
        "missing logical processor domain "
        + std::to_string(processorId));
}

talas::runtime::RuntimePlan makeSingleBoundaryPlan(double processorId = 2.0)
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("osc", "talas.oscillator"));
    auto forced = step("forced", "talas.forcedProcessorAssignment");
    forced.parameters.push_back(processor(processorId));
    forced.inputs.push_back(input("audioIn", "osc", 0));
    plan.steps.push_back(std::move(forced));
    auto output = step("out", "talas.audioOutput");
    output.inputs.push_back(input("audioIn", "forced", 0));
    plan.steps.push_back(std::move(output));
    return plan;
}

talas::runtime::RuntimePlan makeSplitFeedbackPlan()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("feedback", "talas.feedbackLoopOutput"));

    auto inner = step("inner", "talas.forcedProcessorAssignment");
    inner.parameters.push_back(processor(2.0));
    inner.inputs.push_back(input("audioIn", "feedback", 0));
    plan.steps.push_back(std::move(inner));

    auto outer = step("outer", "talas.forcedProcessorAssignment");
    outer.parameters.push_back(processor(3.0));
    outer.inputs.push_back(input("audioIn", "inner", 0));
    plan.steps.push_back(std::move(outer));

    auto sink = step("sink", "talas.feedbackLoopInput");
    sink.inputs.push_back(input("audioIn", "outer", 0));
    plan.steps.push_back(std::move(sink));

    talas::runtime::BufferSpec feedbackInput;
    feedbackInput.nodeId = "outer";
    feedbackInput.portId = "audioOut";
    feedbackInput.type = talas::core::PortType::Audio;
    plan.buffers.push_back(std::move(feedbackInput));

    talas::runtime::BufferSpec feedbackOutput;
    feedbackOutput.nodeId = "feedback";
    feedbackOutput.portId = "audioOut";
    feedbackOutput.type = talas::core::PortType::Audio;
    plan.buffers.push_back(std::move(feedbackOutput));

    plan.feedbackPairs.push_back({
        talas::runtime::globalRuntimeScope,
        "loop",
        3,
        0,
        0,
        1,
        1,
    });
    return plan;
}

void testHostOnlyPlanIsCanonical()
{
    talas::runtime::RuntimePlan runtimePlan;
    runtimePlan.steps.push_back(step("osc", "talas.oscillator"));
    auto output = step("out", "talas.audioOutput");
    output.inputs.push_back(input("audioIn", "osc", 0));
    runtimePlan.steps.push_back(std::move(output));

    const auto result =
        talas::runtime::buildProcessorPlacementPlan(runtimePlan);
    require(result.ok(), "host-only plan should build: " + result.error);
    require(
        result.plan->stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{0, 0},
        "host-only plan should assign every step to processor zero");
    require(
        result.plan->domains.size() == 1
            && result.plan->domains.front().processorId == 0
            && result.plan->domains.front().stepIndices
                == std::vector<std::size_t>{0, 1}
            && result.plan->domains.front().boundaryStepIndices.empty(),
        "host-only plan should publish one canonical domain");
    require(
        result.plan->transfers.empty(),
        "host-only plan should not publish transfers");
}

void testSingleBoundaryOwnsExactClosureAndTransfer()
{
    const auto result = talas::runtime::buildProcessorPlacementPlan(
        makeSingleBoundaryPlan());
    require(result.ok(), "single placement should build: " + result.error);
    require(
        result.plan->stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{2, 2, 0},
        "boundary should own itself and its upstream closure");
    const auto& placed = requireDomain(*result.plan, 2);
    require(
        placed.stepIndices == std::vector<std::size_t>{0, 1}
            && placed.boundaryStepIndices
                == std::vector<std::size_t>{1},
        "placed domain should retain stable step and boundary order");
    require(
        result.plan->transfers.size() == 1
            && result.plan->transfers.front().sourceStepIndex == 1
            && result.plan->transfers.front().targetStepIndex == 2
            && result.plan->transfers.front().inputBindingIndex == 0
            && result.plan->transfers.front().sourceProcessorId == 2
            && result.plan->transfers.front().targetProcessorId == 0,
        "boundary output should publish one exact transfer to host");
}

void testNestedBoundaryStopsOuterClosure()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("osc", "talas.oscillator"));
    auto inner = step("inner", "talas.forcedProcessorAssignment");
    inner.parameters.push_back(processor(3.0));
    inner.inputs.push_back(input("audioIn", "osc", 0));
    plan.steps.push_back(std::move(inner));
    auto outer = step("outer", "talas.forcedProcessorAssignment");
    outer.parameters.push_back(processor(7.0));
    outer.inputs.push_back(input("audioIn", "inner", 0));
    plan.steps.push_back(std::move(outer));
    auto output = step("out", "talas.audioOutput");
    output.inputs.push_back(input("audioIn", "outer", 0));
    plan.steps.push_back(std::move(output));

    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(result.ok(), "nested placement should build: " + result.error);
    require(
        result.plan->stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{3, 3, 7, 0},
        "inner boundary should retain its closure against the outer boundary");
    require(
        result.plan->transfers.size() == 2
            && result.plan->transfers[0].sourceProcessorId == 3
            && result.plan->transfers[0].targetProcessorId == 7
            && result.plan->transfers[1].sourceProcessorId == 7
            && result.plan->transfers[1].targetProcessorId == 0,
        "nested placement should publish inner-to-outer and outer-to-host transfers");
}

void testSameProcessorBoundariesCoalesce()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("left", "talas.oscillator"));
    auto left = step("leftForced", "talas.forcedProcessorAssignment");
    left.parameters.push_back(processor(4.0));
    left.inputs.push_back(input("audioIn", "left", 0));
    plan.steps.push_back(std::move(left));
    plan.steps.push_back(step("right", "talas.noise"));
    auto right = step("rightForced", "talas.forcedProcessorAssignment");
    right.parameters.push_back(processor(4.0));
    right.inputs.push_back(input("audioIn", "right", 0));
    plan.steps.push_back(std::move(right));

    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(result.ok(), "same-domain placements should build: " + result.error);
    const auto& domain = requireDomain(*result.plan, 4);
    require(
        domain.stepIndices == std::vector<std::size_t>{0, 1, 2, 3}
            && domain.boundaryStepIndices
                == std::vector<std::size_t>{1, 3},
        "same processor boundaries should coalesce canonically");
}

void testSharedClosureConflictFails()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("shared", "talas.oscillator"));
    auto left = step("left", "talas.forcedProcessorAssignment");
    left.parameters.push_back(processor(1.0));
    left.inputs.push_back(input("audioIn", "shared", 0));
    plan.steps.push_back(std::move(left));
    auto right = step("right", "talas.forcedProcessorAssignment");
    right.parameters.push_back(processor(2.0));
    right.inputs.push_back(input("audioIn", "shared", 0));
    plan.steps.push_back(std::move(right));

    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(!result.ok(), "conflicting shared closure should fail");
    require(
        result.stepIndex == 0
            && result.error.find("claimed by logical processors 1 and 2")
                != std::string::npos,
        "conflict should identify the exact shared step and processors");
}

void testScopeIdentityPreventsLocalIdAliasing()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("osc", "talas.oscillator", 11, "left/osc"));
    auto left = step(
        "forced", "talas.forcedProcessorAssignment", 11, "left/forced");
    left.parameters.push_back(processor(5.0));
    left.inputs.push_back(input("audioIn", "osc", 11));
    plan.steps.push_back(std::move(left));
    plan.steps.push_back(step("osc", "talas.oscillator", 12, "right/osc"));
    auto right = step(
        "forced", "talas.forcedProcessorAssignment", 12, "right/forced");
    right.parameters.push_back(processor(6.0));
    right.inputs.push_back(input("audioIn", "osc", 12));
    plan.steps.push_back(std::move(right));

    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(result.ok(), "scoped local IDs should remain distinct: " + result.error);
    require(
        result.plan->stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{5, 5, 6, 6},
        "scope-aware resolution should not alias equal local IDs");
}

void testProcessorIdMustBeExactUint32()
{
    for (const auto invalid : {
             0.0,
             -1.0,
             1.5,
             1.0000000001,
             static_cast<double>(
                 std::numeric_limits<std::uint32_t>::max())
                 + 1.0,
         })
    {
        const auto result = talas::runtime::buildProcessorPlacementPlan(
            makeSingleBoundaryPlan(invalid));
        require(!result.ok(), "invalid processor ID should fail");
        require(
            result.stepIndex == 1
                && result.error.find("exact integer in 1...UINT32_MAX")
                    != std::string::npos,
            "invalid processor ID should identify exact bounds");
    }

    const auto maximum = talas::runtime::buildProcessorPlacementPlan(
        makeSingleBoundaryPlan(static_cast<double>(
            std::numeric_limits<std::uint32_t>::max())));
    require(maximum.ok(), "UINT32_MAX processor ID should be accepted");
}

void testValidatorReconstructsAndRejectsForgedMetadata()
{
    auto plan = makeSingleBoundaryPlan();
    const auto built = talas::runtime::buildProcessorPlacementPlan(plan);
    require(built.ok(), "validator fixture should build");
    plan.processorPlacement = *built.plan;
    require(
        talas::runtime::validateProcessorPlacementPlan(plan).ok(),
        "exact placement metadata should validate");

    const auto requireRejected = [&](auto mutate, std::string_view label) {
        auto forgedPlan = plan;
        mutate(forgedPlan.processorPlacement);
        const auto forged =
            talas::runtime::validateProcessorPlacementPlan(forgedPlan);
        require(!forged.ok(), std::string{label} + " should fail validation");
        require(
            forged.error.find("does not match") != std::string::npos,
            std::string{label}
                + " should report independently reconstructed mismatch");
    };

    requireRejected(
        [](auto& placement) { placement.stepProcessors.pop_back(); },
        "missing step assignment");
    requireRejected(
        [](auto& placement) { placement.domains.erase(placement.domains.begin()); },
        "missing domain");
    requireRejected(
        [](auto& placement) {
            placement.domains.back().stepIndices.pop_back();
        },
        "missing domain step");
    requireRejected(
        [](auto& placement) {
            placement.domains.back().boundaryStepIndices.clear();
        },
        "missing boundary");
    requireRejected(
        [](auto& placement) { placement.transfers.clear(); },
        "missing transfer");
    requireRejected(
        [](auto& placement) {
            placement.transfers.front().targetProcessorId = 9;
        },
        "forged transfer processor");
}

void testValidatorRequiresCanonicalHostAuthority()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.push_back(step("host", "talas.oscillator"));

    const auto missing =
        talas::runtime::validateProcessorPlacementPlan(plan);
    require(
        !missing.ok()
            && missing.error.find("does not match") != std::string::npos,
        "nonempty host-only plans must publish canonical processor-zero authority");

    talas::runtime::RuntimePlan empty;
    require(
        talas::runtime::validateProcessorPlacementPlan(empty).ok(),
        "the exact empty graph and empty placement authority should remain valid");
}

void testValidatorEnforcesBoundsWithoutForcedAssignments()
{
    talas::runtime::RuntimePlan tooManySteps;
    tooManySteps.steps.resize(
        talas::runtime::maximumProcessorPlacementSteps + 1);
    const auto stepResult =
        talas::runtime::validateProcessorPlacementPlan(tooManySteps);
    require(
        !stepResult.ok()
            && stepResult.error.find("8,192") != std::string::npos,
        "empty host metadata must not bypass the first over-bound step limit");

    talas::runtime::RuntimePlan tooManyBindings;
    tooManyBindings.steps.push_back(step("source", "talas.oscillator"));
    auto target = step("target", "talas.maximumPlacementFixture");
    target.inputs.reserve(
        talas::runtime::maximumProcessorPlacementBindings + 1);
    for (std::size_t index = 0;
         index < talas::runtime::maximumProcessorPlacementBindings + 1;
         ++index)
    {
        target.inputs.push_back(input(
            "audioIn" + std::to_string(index),
            "source",
            talas::runtime::globalRuntimeScope));
    }
    tooManyBindings.steps.push_back(std::move(target));
    const auto bindingResult =
        talas::runtime::validateProcessorPlacementPlan(tooManyBindings);
    require(
        !bindingResult.ok()
            && bindingResult.error.find("65,536") != std::string::npos,
        "empty host metadata must not bypass the first over-bound binding limit");
}

void testBuilderReconstructsOmittedFeedbackAuthority()
{
    const auto result = talas::runtime::buildProcessorPlacementPlan(
        makeSplitFeedbackPlan());
    require(
        !result.ok()
            && result.error.find("feedback component authority")
                != std::string::npos,
        "omitting a real split feedback component must not bypass independently reconstructed authority");
}

void testBuilderRejectsFeedbackAuthorityWithOmittedMember()
{
    auto plan = makeSplitFeedbackPlan();
    plan.feedbackComponents.push_back({{0, 1}, {0}});
    const auto result =
        talas::runtime::buildProcessorPlacementPlan(plan);
    require(
        !result.ok()
            && result.error.find("feedback component authority")
                != std::string::npos,
        "omitting the cross-domain member from a real feedback component must not bypass reconstruction");
}

void testBuilderRejectsCanonicalFeedbackDomainSplit()
{
    auto plan = makeSplitFeedbackPlan();
    plan.feedbackComponents.push_back({{0, 1, 2}, {0}});
    const auto result =
        talas::runtime::buildProcessorPlacementPlan(plan);
    require(
        !result.ok()
            && result.error.find(
                "cannot split one explicit feedback component")
                != std::string::npos,
        "canonical feedback authority must reject a component split across logical processors");
}

void testDynamicProcessorExpressionIsRejected()
{
    auto plan = makeSingleBoundaryPlan(7.0);
    const auto parsed = talas::expr::parse("!P");
    require(parsed.ok(), "dynamic processor fixture should parse");
    plan.steps[1].parameters.front().expression = *parsed.expression;

    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(
        !result.ok()
            && result.stepIndex == 1
            && result.error.find("must be static") != std::string::npos,
        "processor placement must reject retained Event Value expressions even when their current value is exact");
}

void testPlacementStepLimitIsBounded()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.resize(talas::runtime::maximumProcessorPlacementSteps + 1);
    const auto result = talas::runtime::buildProcessorPlacementPlan(plan);
    require(!result.ok(), "first over-bound placement plan should fail");
    require(
        result.error.find("8,192") != std::string::npos,
        "step bound should be explicit");
}

void testExactMaximumStepsAndDomainsAreAdmitted()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.reserve(talas::runtime::maximumProcessorPlacementSteps);
    plan.steps.push_back(step("source", "talas.oscillator"));
    std::string sourceId = "source";
    for (std::size_t index = 1;
         index < talas::runtime::maximumProcessorPlacementSteps;
         ++index)
    {
        auto nodeId = "forced" + std::to_string(index);
        auto forced = step(nodeId, "talas.forcedProcessorAssignment");
        forced.parameters.push_back(processor(static_cast<double>(index)));
        forced.inputs.push_back(input("audioIn", sourceId, 0));
        sourceId = std::move(nodeId);
        plan.steps.push_back(std::move(forced));
    }

    const auto built = talas::runtime::buildProcessorPlacementPlan(plan);
    require(built.ok(), "exact maximum placement steps should be admitted");
    require(
        built.plan->stepProcessors.size()
                == talas::runtime::maximumProcessorPlacementSteps
            && built.plan->domains.size()
                == talas::runtime::maximumProcessorPlacementSteps - 1
            && built.plan->transfers.size()
                == talas::runtime::maximumProcessorPlacementSteps - 2,
        "exact maximum step chain should retain every domain and transfer");
    plan.processorPlacement = *built.plan;
    require(
        talas::runtime::validateProcessorPlacementPlan(plan).ok(),
        "exact maximum step chain should validate independently");
}

void testExactMaximumChainBuildsOnOrdinaryWorkerStack()
{
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            talas::runtime::RuntimePlan plan;
            plan.steps.reserve(
                talas::runtime::maximumProcessorPlacementSteps);
            plan.steps.push_back(step("source", "talas.oscillator"));
            std::string sourceId = "source";
            for (std::size_t index = 1;
                 index < talas::runtime::maximumProcessorPlacementSteps;
                 ++index)
            {
                auto nodeId = "step" + std::to_string(index);
                auto target = step(
                    nodeId,
                    "talas.maximumPlacementFixture");
                target.inputs.push_back(input(
                    "audioIn",
                    sourceId,
                    talas::runtime::globalRuntimeScope));
                sourceId = std::move(nodeId);
                plan.steps.push_back(std::move(target));
            }

            const auto built =
                talas::runtime::buildProcessorPlacementPlan(plan);
            require(
                built.ok()
                    && built.plan->stepProcessors.size()
                        == talas::runtime::maximumProcessorPlacementSteps,
                "exact maximum placement chain must fit an ordinary worker stack");
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure != nullptr)
        std::rethrow_exception(failure);
}

void testExactMaximumBindingsAndTransfersAreAdmitted()
{
    talas::runtime::RuntimePlan plan;
    plan.steps.reserve(talas::runtime::maximumProcessorPlacementSteps);
    plan.steps.push_back(step("source", "talas.oscillator"));
    auto forced = step("forced", "talas.forcedProcessorAssignment");
    forced.parameters.push_back(processor(1.0));
    forced.inputs.push_back(input("audioIn", "source", 0));
    plan.steps.push_back(std::move(forced));

    auto remainingBindings =
        talas::runtime::maximumProcessorPlacementBindings - 1;
    for (std::size_t stepIndex = 2;
         stepIndex < talas::runtime::maximumProcessorPlacementSteps;
         ++stepIndex)
    {
        auto target = step(
            "target" + std::to_string(stepIndex),
            "talas.maximumPlacementFixture");
        const auto remainingSteps =
            talas::runtime::maximumProcessorPlacementSteps - stepIndex;
        const auto inputCount =
            (remainingBindings + remainingSteps - 1) / remainingSteps;
        target.inputs.reserve(inputCount);
        for (std::size_t inputIndex = 0;
             inputIndex < inputCount;
             ++inputIndex)
        {
            target.inputs.push_back(input(
                "audioIn" + std::to_string(inputIndex),
                "forced",
                0));
        }
        remainingBindings -= inputCount;
        plan.steps.push_back(std::move(target));
    }
    require(
        remainingBindings == 0,
        "maximum binding fixture should distribute every exact binding");

    const auto built = talas::runtime::buildProcessorPlacementPlan(plan);
    require(built.ok(), "exact maximum placement bindings should be admitted");
    require(
        built.plan->transfers.size()
            == talas::runtime::maximumProcessorPlacementBindings - 1,
        "every maximum-fixture cross-domain binding should become a transfer");
    plan.processorPlacement = *built.plan;
    require(
        talas::runtime::validateProcessorPlacementPlan(plan).ok(),
        "exact maximum binding and transfer plan should validate independently");
}
}

int main()
{
    try
    {
        testHostOnlyPlanIsCanonical();
        testSingleBoundaryOwnsExactClosureAndTransfer();
        testNestedBoundaryStopsOuterClosure();
        testSameProcessorBoundariesCoalesce();
        testSharedClosureConflictFails();
        testScopeIdentityPreventsLocalIdAliasing();
        testProcessorIdMustBeExactUint32();
        testValidatorReconstructsAndRejectsForgedMetadata();
        testValidatorRequiresCanonicalHostAuthority();
        testValidatorEnforcesBoundsWithoutForcedAssignments();
        testBuilderReconstructsOmittedFeedbackAuthority();
        testBuilderRejectsFeedbackAuthorityWithOmittedMember();
        testBuilderRejectsCanonicalFeedbackDomainSplit();
        testDynamicProcessorExpressionIsRejected();
        testPlacementStepLimitIsBounded();
        testExactMaximumStepsAndDomainsAreAdmitted();
        testExactMaximumChainBuildsOnOrdinaryWorkerStack();
        testExactMaximumBindingsAndTransfersAreAdmitted();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
