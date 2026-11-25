#pragma once

#include "talas/app/PerformanceWorkbenchModel.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class PerformanceDispatchOutcome
{
    Accepted,
    InvalidControl,
    InvalidValue,
    StaleAuthority,
    TargetUnavailable,
    RuntimeRejected,
    AuthoringRejected
};

struct PerformanceDispatchResult
{
    PerformanceDispatchOutcome outcome = PerformanceDispatchOutcome::InvalidControl;
    std::vector<talas::project::SnapshotValueEntry> appliedValues;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome == PerformanceDispatchOutcome::Accepted;
    }
};

class PerformanceRuntimeDispatcher
{
  public:
    explicit PerformanceRuntimeDispatcher(talas::runtime::ProjectRuntimeBridge& bridge) noexcept;

    [[nodiscard]] PerformanceDispatchResult dispatch(std::string_view controlId, double value,
                                                     std::uint64_t expectedStageGeneration,
                                                     double smoothingSeconds = 0.0);

  private:
    talas::runtime::ProjectRuntimeBridge& bridge_;
};
} // namespace talas::app
