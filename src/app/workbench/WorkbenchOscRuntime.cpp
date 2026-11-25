#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchOscDiagnostics.h"

#include <string>

namespace talas::app::workbench
{
namespace
{
juce::String realtimeMetricText(std::uint64_t value)
{
    return juce::String(std::to_string(value));
}
}

juce::String WorkbenchComponent::oscProfileAccessibleDetail() const
{
    const auto status = oscProfileStatusText();
    if (status.isNotEmpty())
        return status;
    return juceString("OSC loopback profile disabled · generation ")
        + juce::String(static_cast<juce::int64>(
            latestOscLoopbackTelemetry_.profileGeneration));
}

juce::String WorkbenchComponent::oscProfileStatusText() const
{
    const auto& telemetry = latestOscLoopbackTelemetry_;
    juce::String state;
    switch (telemetry.state)
    {
    case WorkbenchOscLoopbackProfileState::Ready:
        state = "ready";
        break;
    case WorkbenchOscLoopbackProfileState::Preparing:
        state = "preparing";
        break;
    case WorkbenchOscLoopbackProfileState::RetainedAfterFailure:
        state = "retained after failed request";
        break;
    case WorkbenchOscLoopbackProfileState::RouteUnavailable:
        state = "route unavailable";
        break;
    case WorkbenchOscLoopbackProfileState::Failed:
        state = "failed";
        break;
    case WorkbenchOscLoopbackProfileState::Disabled:
        break;
    }
    if (state.isEmpty())
        return {};
    auto result = "OSC profile " + state
        + juceString(" · generation ")
        + juce::String(static_cast<juce::int64>(
            telemetry.profileGeneration));
    if (telemetry.ingress.scheduledPending != 0)
    {
        result += juceString(" · NTP pending ")
            + realtimeMetricText(telemetry.ingress.scheduledPending);
    }
    if (telemetry.ingress.lateMessages != 0
        || telemetry.ingress.scheduledLateApplied != 0)
    {
        result += juceString(" · late accepted/applied ")
            + realtimeMetricText(telemetry.ingress.lateMessages)
            + "/"
            + realtimeMetricText(
                telemetry.ingress.scheduledLateApplied);
    }
    auto timedRejections = talas::runtime::realtimeMetricAddSaturated(
        telemetry.ingress.timetagsTooLate,
        telemetry.ingress.timetagsTooFar);
    timedRejections = talas::runtime::realtimeMetricAddSaturated(
        timedRejections,
        telemetry.ingress.timetagConversionFailures);
    timedRejections = talas::runtime::realtimeMetricAddSaturated(
        timedRejections,
        telemetry.ingress.scheduleFull);
    if (timedRejections != 0)
    {
        result += juceString(" · NTP rejected ")
            + realtimeMetricText(timedRejections);
    }
    return result + juceString(" · local clock · no clock synchronization");
}

void WorkbenchComponent::publishOscEgressValue(
    std::string_view eventName,
    double persistedValue) noexcept
{
    if (!oscEgressRoutesAvailable_)
        return;
    (void)oscEgress_.publish(
        eventName,
        persistedValue,
        projectBridge_.latestStageGeneration());
}

void WorkbenchComponent::rebuildOscIngress()
{
    const auto& document = projectBridge_.editor().document();
    const auto profile = prepareWorkbenchOscLoopbackProfile(
        projectBridge_, document, stagedVirtualControls_, stagedMidiRoutesValid_);
    rebuildOscEgress(profile);
}

void WorkbenchComponent::suspendOscLoopbackProfile() noexcept
{
    projectBridge_.revokeLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Osc);
    projectBridge_.revokeLiveToolSourceAuthority(
        talas::runtime::RealtimeLiveToolSourceKind::Osc);
    oscLoopbackProfile_.suspendRetainedAuthority();
    oscRoutesAvailable_ = false;
    oscEgressRoutesAvailable_ = false;
    oscProfileAvailabilityInvalidated_ = true;
}

void WorkbenchComponent::rebuildOscEgress()
{
    rebuildOscIngress();
}

void WorkbenchComponent::rebuildOscEgress(
    const PreparedWorkbenchOscLoopbackProfile& profile)
{
    const auto ingressRoutesWereAvailable = oscRoutesAvailable_;
    const auto egressRoutesWereAvailable = oscEgressRoutesAvailable_;
    const auto result = oscLoopbackProfile_.apply(profile);
    latestOscLoopbackTelemetry_ = oscLoopbackProfile_.telemetry();
    latestOscIngressTelemetry_ = latestOscLoopbackTelemetry_.ingress;
    latestOscEgressTelemetry_ = latestOscLoopbackTelemetry_.egress;

    const auto exactIngress = result.accepted
        && profile.ingressSettings.enabled
        && latestOscLoopbackTelemetry_.activeIngress
            == profile.ingressSettings
        && latestOscIngressTelemetry_.state
            == WorkbenchOscIngressState::Listening;
    const auto exactEgress = result.accepted
        && profile.egressSettings.enabled
        && latestOscLoopbackTelemetry_.activeEgress
            == profile.egressSettings
        && latestOscEgressTelemetry_.state
            == WorkbenchOscEgressState::Ready;
    oscRoutesAvailable_ = profile.ingressRouteSetValid && exactIngress;
    oscEgressRoutesAvailable_ = profile.egressRouteSetValid && exactEgress;
    oscRouteStageGeneration_ = profile.stageGeneration;
    oscEgressRouteStageGeneration_ = profile.stageGeneration;

    if (!profile.ingressRouteSetValid)
    {
        oscIngressDiagnostic_ =
            "OSC routes unavailable: mapped Event Value is not in the exact active stage";
    }
    else if (!result.accepted && profile.ingressSettings.enabled)
    {
        oscIngressDiagnostic_ = juceString(result.message);
    }
    else if (exactIngress)
    {
        oscIngressDiagnostic_ = "OSC listening on 127.0.0.1:"
            + juce::String(static_cast<int>(
                latestOscIngressTelemetry_.activePort));
    }
    else
    {
        oscIngressDiagnostic_.clear();
    }

    if (!profile.egressRouteSetValid)
    {
        oscEgressDiagnostic_ =
            "OSC feedback unavailable: mapped Event Value is not in the exact active stage";
    }
    else if (!result.accepted && profile.egressSettings.enabled)
    {
        oscEgressDiagnostic_ = juceString(result.message);
    }
    else if (exactEgress)
    {
        oscEgressDiagnostic_ = "OSC feedback to 127.0.0.1:"
            + juce::String(static_cast<int>(
                latestOscEgressTelemetry_.activePort));
    }
    else
    {
        oscEgressDiagnostic_.clear();
    }

    if (virtualControlSurface_ != nullptr
        && (oscProfileAvailabilityInvalidated_
            || ingressRoutesWereAvailable != oscRoutesAvailable_
            || egressRoutesWereAvailable != oscEgressRoutesAvailable_))
    {
        virtualControlSurface_->refresh();
    }
    oscProfileAvailabilityInvalidated_ = false;
    refreshPersistentStatus();
}
}
