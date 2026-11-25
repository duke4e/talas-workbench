#include "app/workbench/WorkbenchOscLoopbackProfileCoordinator.h"

#include <limits>

namespace talas::app::workbench
{
namespace
{
void incrementSaturating(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
}

WorkbenchOscLoopbackProfileCoordinator::
    WorkbenchOscLoopbackProfileCoordinator(
        WorkbenchOscIngress& ingress,
        WorkbenchOscEgress& egress) noexcept
    : ingress_(ingress), egress_(egress)
{
    publishTelemetry();
}

WorkbenchOscLoopbackProfileResult
WorkbenchOscLoopbackProfileCoordinator::apply(
    const PreparedWorkbenchOscLoopbackProfile& profile)
{
    requestedIngress_ = profile.ingressSettings;
    requestedEgress_ = profile.egressSettings;
    retainedState_ = state_;
    state_ = WorkbenchOscLoopbackProfileState::Preparing;
    publishTelemetry();

    if (profileGeneration_ == std::numeric_limits<std::uint64_t>::max())
    {
        resumeRetainedAuthority();
        reject(retainedFailureState(), profile);
        return result(false, "OSC loopback profile generation exhausted");
    }

    if (!talas::project::isValidOscEndpointPair(
            profile.ingressSettings, profile.egressSettings))
    {
        resumeRetainedAuthority();
        reject(retainedFailureState(), profile);
        return result(false, "Invalid OSC loopback endpoint pair");
    }
    if (!profile.ingressRouteSetValid || !profile.egressRouteSetValid)
    {
        ingress_.stop();
        egress_.stop();
        authoritySuspended_ = false;
        activeIngress_ = {};
        activeEgress_ = {};
        activeStageGeneration_ = profile.stageGeneration;
        reject(
            WorkbenchOscLoopbackProfileState::RouteUnavailable,
            profile);
        return result(
            false,
            "OSC loopback routes are unavailable for the exact active stage");
    }

    auto ingressPreparation = ingress_.prepareCandidate(
        profile.ingressSettings,
        std::span<const talas::project::PreparedOscControlRoute>{
            profile.ingressRoutes.data(), profile.ingressRouteCount},
        profile.liveGridIngressAddresses);
    if (!ingressPreparation.candidate.has_value())
    {
        resumeRetainedAuthority();
        reject(retainedFailureState(), profile);
        return result(false, ingressPreparation.result.message);
    }

    auto egressPreparation = egress_.prepareCandidate(
        profile.egressSettings,
        std::span<const PreparedWorkbenchOscEgressRoute>{
            profile.egressRoutes.data(), profile.egressRouteCount});
    if (!egressPreparation.candidate.has_value())
    {
        resumeRetainedAuthority();
        reject(retainedFailureState(), profile);
        return result(false, egressPreparation.result.message);
    }

    ingress_.suspend();
    egress_.suspend();
    authoritySuspended_ = true;
    if (!ingress_.commitCandidate(
            std::move(*ingressPreparation.candidate)))
    {
        resumeRetainedAuthority();
        const auto authorityExhausted = ingress_.routeGeneration() == 0;
        reject(
            authorityExhausted
                ? WorkbenchOscLoopbackProfileState::Failed
                : retainedFailureState(),
            profile);
        return result(
            false,
            authorityExhausted
                ? "OSC ingress authority generation exhausted"
                : "Could not discard the bounded prior OSC ingress backlog");
    }
    egress_.commitCandidate(std::move(*egressPreparation.candidate));

    ++profileGeneration_;
    activeStageGeneration_ = profile.stageGeneration;
    activeIngress_ = profile.ingressSettings;
    activeEgress_ = profile.egressSettings;
    if (!ingress_.resume())
    {
        egress_.stop();
        authoritySuspended_ = false;
        activeIngress_ = {};
        activeEgress_ = {};
        reject(WorkbenchOscLoopbackProfileState::Failed, profile);
        return result(false, "OSC ingress authority generation exhausted");
    }
    egress_.resume();
    authoritySuspended_ = false;
    const auto resynced = !profile.egressSettings.enabled || egress_.resync();
    state_ = profile.ingressSettings.enabled || profile.egressSettings.enabled
        ? WorkbenchOscLoopbackProfileState::Ready
        : WorkbenchOscLoopbackProfileState::Disabled;
    publishTelemetry();
    return result(
        true,
        resynced
            ? state_ == WorkbenchOscLoopbackProfileState::Ready
                ? "OSC loopback profile ready"
                : "OSC loopback profile disabled"
            : "OSC loopback profile ready; feedback resync is incomplete");
}

void WorkbenchOscLoopbackProfileCoordinator::suspendRetainedAuthority()
    noexcept
{
    if (authoritySuspended_)
        return;
    retainedState_ = state_;
    state_ = WorkbenchOscLoopbackProfileState::Preparing;
    ingress_.suspend();
    egress_.suspend();
    authoritySuspended_ = true;
    publishTelemetry();
}

void WorkbenchOscLoopbackProfileCoordinator::resumeRetainedAuthority()
    noexcept
{
    if (authoritySuspended_)
    {
        if (!ingress_.resume())
        {
            egress_.stop();
            authoritySuspended_ = false;
            activeIngress_ = {};
            activeEgress_ = {};
            state_ = WorkbenchOscLoopbackProfileState::Failed;
            publishTelemetry();
            return;
        }
        egress_.resume();
        authoritySuspended_ = false;
    }
    if (state_ == WorkbenchOscLoopbackProfileState::Preparing)
        state_ = retainedState_;
    publishTelemetry();
}

void WorkbenchOscLoopbackProfileCoordinator::stop() noexcept
{
    ingress_.stop();
    egress_.stop();
    authoritySuspended_ = false;
    activeIngress_ = {};
    activeEgress_ = {};
    activeStageGeneration_ = 0;
    state_ = WorkbenchOscLoopbackProfileState::Disabled;
    publishTelemetry();
}

void WorkbenchOscLoopbackProfileCoordinator::refreshTelemetry() noexcept
{
    publishTelemetry();
}

WorkbenchOscLoopbackProfileTelemetry
WorkbenchOscLoopbackProfileCoordinator::telemetry() const noexcept
{
    const std::lock_guard lock{telemetryMutex_};
    return telemetry_;
}

WorkbenchOscLoopbackProfileState
WorkbenchOscLoopbackProfileCoordinator::retainedFailureState() const noexcept
{
    return profileGeneration_ == 0
        ? WorkbenchOscLoopbackProfileState::Failed
        : WorkbenchOscLoopbackProfileState::RetainedAfterFailure;
}

void WorkbenchOscLoopbackProfileCoordinator::publishTelemetry() noexcept
{
    WorkbenchOscLoopbackProfileTelemetry snapshot;
    snapshot.state = state_;
    snapshot.profileGeneration = profileGeneration_;
    snapshot.stageGeneration = activeStageGeneration_;
    snapshot.requestedIngress = requestedIngress_;
    snapshot.requestedEgress = requestedEgress_;
    snapshot.activeIngress = activeIngress_;
    snapshot.activeEgress = activeEgress_;
    snapshot.ingress = ingress_.telemetry();
    snapshot.egress = egress_.telemetry();
    snapshot.rejectedCandidates = rejectedCandidates_;
    const std::lock_guard lock{telemetryMutex_};
    telemetry_ = snapshot;
}

void WorkbenchOscLoopbackProfileCoordinator::reject(
    WorkbenchOscLoopbackProfileState state,
    const PreparedWorkbenchOscLoopbackProfile& requested) noexcept
{
    requestedIngress_ = requested.ingressSettings;
    requestedEgress_ = requested.egressSettings;
    state_ = state;
    incrementSaturating(rejectedCandidates_);
    publishTelemetry();
}

WorkbenchOscLoopbackProfileResult
WorkbenchOscLoopbackProfileCoordinator::result(
    bool accepted,
    std::string message) const
{
    return {
        accepted,
        state_,
        profileGeneration_,
        std::move(message),
    };
}
}
