#pragma once

#include "app/workbench/WorkbenchOscIngress.h"
#include "app/workbench/WorkbenchOscLoopbackProfile.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace talas::app::workbench
{
enum class WorkbenchOscLoopbackProfileState : std::uint8_t
{
    Disabled,
    Preparing,
    Ready,
    RetainedAfterFailure,
    RouteUnavailable,
    Failed,
};

struct WorkbenchOscLoopbackProfileTelemetry
{
    WorkbenchOscLoopbackProfileState state =
        WorkbenchOscLoopbackProfileState::Disabled;
    std::uint64_t profileGeneration = 0;
    std::uint64_t stageGeneration = 0;
    talas::project::ProjectOscIngressSettings requestedIngress;
    talas::project::ProjectOscEgressSettings requestedEgress;
    talas::project::ProjectOscIngressSettings activeIngress;
    talas::project::ProjectOscEgressSettings activeEgress;
    WorkbenchOscIngressTelemetry ingress;
    WorkbenchOscEgressTelemetry egress;
    std::uint64_t rejectedCandidates = 0;
};

struct WorkbenchOscLoopbackProfileResult
{
    bool accepted = false;
    WorkbenchOscLoopbackProfileState state =
        WorkbenchOscLoopbackProfileState::Disabled;
    std::uint64_t profileGeneration = 0;
    std::string message;
};

class WorkbenchOscLoopbackProfileCoordinator final
{
public:
    WorkbenchOscLoopbackProfileCoordinator(
        WorkbenchOscIngress& ingress,
        WorkbenchOscEgress& egress) noexcept;

    WorkbenchOscLoopbackProfileCoordinator(
        const WorkbenchOscLoopbackProfileCoordinator&) = delete;
    WorkbenchOscLoopbackProfileCoordinator& operator=(
        const WorkbenchOscLoopbackProfileCoordinator&) = delete;

    [[nodiscard]] WorkbenchOscLoopbackProfileResult apply(
        const PreparedWorkbenchOscLoopbackProfile& profile);
    void suspendRetainedAuthority() noexcept;
    void resumeRetainedAuthority() noexcept;
    void stop() noexcept;
    void refreshTelemetry() noexcept;

    [[nodiscard]] WorkbenchOscLoopbackProfileTelemetry telemetry()
        const noexcept;

private:
    [[nodiscard]] WorkbenchOscLoopbackProfileState retainedFailureState()
        const noexcept;
    void publishTelemetry() noexcept;
    void reject(
        WorkbenchOscLoopbackProfileState state,
        const PreparedWorkbenchOscLoopbackProfile& requested) noexcept;
    [[nodiscard]] WorkbenchOscLoopbackProfileResult result(
        bool accepted,
        std::string message) const;

    WorkbenchOscIngress& ingress_;
    WorkbenchOscEgress& egress_;
    WorkbenchOscLoopbackProfileState state_ =
        WorkbenchOscLoopbackProfileState::Disabled;
    WorkbenchOscLoopbackProfileState retainedState_ =
        WorkbenchOscLoopbackProfileState::Disabled;
    std::uint64_t profileGeneration_ = 0;
    std::uint64_t activeStageGeneration_ = 0;
    std::uint64_t rejectedCandidates_ = 0;
    talas::project::ProjectOscIngressSettings requestedIngress_;
    talas::project::ProjectOscEgressSettings requestedEgress_;
    talas::project::ProjectOscIngressSettings activeIngress_;
    talas::project::ProjectOscEgressSettings activeEgress_;
    bool authoritySuspended_ = false;

    mutable std::mutex telemetryMutex_;
    WorkbenchOscLoopbackProfileTelemetry telemetry_;
};
}
