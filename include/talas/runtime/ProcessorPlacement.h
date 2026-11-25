#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace talas::runtime
{
using LogicalProcessorId = std::uint32_t;

inline constexpr LogicalProcessorId hostLogicalProcessor = 0;
inline constexpr std::size_t maximumProcessorPlacementSteps = 8192;
inline constexpr std::size_t maximumProcessorPlacementBindings = 65536;
inline constexpr std::size_t invalidProcessorPlacementIndex =
    std::numeric_limits<std::size_t>::max();

struct ProcessorPlacementDomain
{
    LogicalProcessorId processorId = hostLogicalProcessor;
    std::vector<std::size_t> stepIndices;
    std::vector<std::size_t> boundaryStepIndices;

    bool operator==(const ProcessorPlacementDomain&) const = default;
};

struct ProcessorPlacementTransfer
{
    std::size_t sourceStepIndex = 0;
    std::size_t targetStepIndex = 0;
    std::size_t inputBindingIndex = 0;
    LogicalProcessorId sourceProcessorId = hostLogicalProcessor;
    LogicalProcessorId targetProcessorId = hostLogicalProcessor;

    bool operator==(const ProcessorPlacementTransfer&) const = default;
};

struct ProcessorPlacementPlan
{
    std::vector<LogicalProcessorId> stepProcessors;
    std::vector<ProcessorPlacementDomain> domains;
    std::vector<ProcessorPlacementTransfer> transfers;

    [[nodiscard]] bool empty() const noexcept
    {
        return stepProcessors.empty() && domains.empty()
            && transfers.empty();
    }

    bool operator==(const ProcessorPlacementPlan&) const = default;
};

struct ProcessorPlacementBuildResult
{
    std::optional<ProcessorPlacementPlan> plan;
    std::string error;
    std::size_t stepIndex = invalidProcessorPlacementIndex;
    std::size_t inputBindingIndex = invalidProcessorPlacementIndex;

    [[nodiscard]] bool ok() const noexcept
    {
        return plan.has_value() && error.empty();
    }
};

struct ProcessorPlacementValidationResult
{
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

struct RuntimePlan;

[[nodiscard]] ProcessorPlacementBuildResult
buildProcessorPlacementPlan(const RuntimePlan& runtimePlan);

[[nodiscard]] ProcessorPlacementValidationResult
validateProcessorPlacementPlan(const RuntimePlan& runtimePlan);
}
