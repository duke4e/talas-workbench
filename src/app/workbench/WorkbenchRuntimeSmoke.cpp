#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchAnalysisDiagnostics.h"
#include "app/workbench/WorkbenchRealtimeMetricsPresentation.h"

#include <limits>

// The realtime metrics fixture the Workbench smoke runs, apart from
// WorkbenchRuntime.cpp to keep that file under its line cap.
namespace talas::app::workbench
{
bool WorkbenchComponent::realtimeMetricsFixtureIsValidForSmoke()
{
    auto callbackOnlyMetrics = latestRealtimeMetrics_;
    callbackOnlyMetrics.renderCallbacks =
        talas::runtime::realtimeMetricAddSaturated(
            callbackOnlyMetrics.renderCallbacks, 1);
    auto pressureMetrics = latestRealtimeMetrics_;
    pressureMetrics.renderFailures =
        std::numeric_limits<std::uint64_t>::max();
    pressureMetrics.stageRejections = 1;
    pressureMetrics.controlQueueOverflow = 3;
    pressureMetrics.aggregateControlRejected = 4;
    pressureMetrics.dspRejected = 2;
    pressureMetrics.analysisDroppedBlocks = 4;
    pressureMetrics.discardedOutputChannels = 5;
    pressureMetrics.deviceDeadlineAvailable = 1;
    pressureMetrics.deviceDeadlineOverruns = 2;
    pressureMetrics.devicePeriodNanoseconds = 5'000'000;
    pressureMetrics.deviceMaximumCallbackNanoseconds = 7'500'000;
    const auto ingressRecords = oscIngress_.recentRecords();
    const auto egressRecords = oscEgress_.recentRecords();
    std::uint64_t midiDroppedRecords = 0;
    for (const auto& dropped : midiUiDroppedBySource_)
    {
        midiDroppedRecords = talas::runtime::realtimeMetricAddSaturated(
            midiDroppedRecords,
            dropped.load(std::memory_order_acquire));
    }
    const auto snapshot = makeWorkbenchAnalysisDiagnosticsSnapshot(
        latestPreparedGraphStatus_,
        pressureMetrics,
        projectBridge_.preparedNodeTiming(),
        7'500'000,
        assetRecoveryCoordinator_.state(),
        analysisPanel_->eventMonitorSnapshot(),
        std::span<const talas::app::AnalysisMonitorRecordInput>{
            midiMonitorRecords_.data(),
            midiMonitorRecordCount_},
        nextMidiMonitorSequence_,
        midiDroppedRecords,
        latestOscIngressTelemetry_,
        ingressRecords,
        oscIngress_.routeGeneration(),
        latestOscEgressTelemetry_,
        egressRecords,
        oscEgress_.routeGeneration());
    std::vector<WorkbenchStatusDiagnostic> diagnostics;
    appendWorkbenchAnalysisDiagnostics(
        diagnostics,
        talas::app::buildAnalysisDiagnosticsProjection(snapshot));
    const auto hasDiagnostic = [&diagnostics](juce::StringRef title) {
        return std::ranges::any_of(
            diagnostics,
            [title](const auto& diagnostic) {
                return diagnostic.title == title;
            });
    };
    const auto diagnosticContains = [&diagnostics](
                                        juce::StringRef title,
                                        juce::StringRef text) {
        return std::ranges::any_of(
            diagnostics,
            [title, text](const auto& diagnostic) {
                return diagnostic.title == title
                    && diagnostic.detail.contains(text);
            });
    };
    const auto presentationIsValid =
        sameRealtimeMetricsPresentation(
            latestRealtimeMetrics_, callbackOnlyMetrics)
        && !sameRealtimeMetricsPresentation(
            latestRealtimeMetrics_, pressureMetrics)
        && hasDiagnostic("Audio render failures")
        && hasDiagnostic("Device callback duration")
        && hasDiagnostic("Control queue overflow")
        && hasDiagnostic("Analysis capture drops")
        && hasDiagnostic("Graph stage rejections")
        && hasDiagnostic("Discarded output channels")
        && diagnosticContains(
            "Audio render failures", "18446744073709551615")
        && diagnosticContains(
            "Control rejections", "4")
        && diagnosticContains(
            "DSP control rejections", "2");

    const auto generationBeforeConcurrentReset =
        deviceMetricsResetGeneration_.load(std::memory_order_acquire);
    std::thread firstResetter([this] {
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
            requestDeviceMetricsReset();
    });
    std::thread secondResetter([this] {
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
            requestDeviceMetricsReset();
    });
    firstResetter.join();
    secondResetter.join();
    const auto resetGenerationIsValid =
        deviceMetricsResetGeneration_.load(std::memory_order_acquire)
        == talas::runtime::realtimeMetricAddSaturated(
            generationBeforeConcurrentReset, 128);

    devicePeriodNanoseconds_.store(
        5'000'000, std::memory_order_release);
    const auto callbacksBeforeReset =
        deviceCallbackCount_.load(std::memory_order_relaxed);
    const auto overrunsBeforeReset =
        deviceDeadlineOverrunCount_.load(
            std::memory_order_relaxed);
    requestDeviceMetricsReset();
    const auto fixtureGeneration =
        deviceMetricsResetGeneration_.load(
            std::memory_order_acquire);
    publishDeviceCallbackDuration(7'500'000);
    const auto producerIsValid =
        publishedDeviceMetricsGeneration_.load(
            std::memory_order_acquire)
                == fixtureGeneration
        && deviceBaselineCallbackCount_.load(
            std::memory_order_relaxed)
                == callbacksBeforeReset
        && deviceBaselineDeadlineOverrunCount_.load(
            std::memory_order_relaxed)
                == overrunsBeforeReset
        && deviceCallbackCount_.load(std::memory_order_relaxed)
                == talas::runtime::realtimeMetricAddSaturated(
                    callbacksBeforeReset, 1)
        && deviceDeadlineOverrunCount_.load(
            std::memory_order_relaxed)
                == talas::runtime::realtimeMetricAddSaturated(
                    overrunsBeforeReset, 1)
        && talas::runtime::realtimeMetricDelta(
            deviceCallbackCount_.load(std::memory_order_relaxed),
            deviceBaselineCallbackCount_.load(
                std::memory_order_relaxed))
                == 1
        && talas::runtime::realtimeMetricDelta(
            deviceDeadlineOverrunCount_.load(
                std::memory_order_relaxed),
            deviceBaselineDeadlineOverrunCount_.load(
                std::memory_order_relaxed))
                == 1
        && deviceMaximumCallbackNanoseconds_.load(
            std::memory_order_relaxed)
                >= 7'500'000
        && publishedDevicePeriodNanoseconds_.load(
            std::memory_order_relaxed)
                == 5'000'000;
    devicePeriodNanoseconds_.store(0, std::memory_order_release);
    requestDeviceMetricsReset();
    return presentationIsValid
        && resetGenerationIsValid
        && producerIsValid;
}
}
