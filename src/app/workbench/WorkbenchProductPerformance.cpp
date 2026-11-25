#include "app/workbench/WorkbenchComponentInternal.h"

#include <cmath>
#include <utility>

namespace talas::app::workbench
{
#if defined(TALAS_PRODUCT_PERFORMANCE_PROBE_SUPPORT)
bool WorkbenchComponent::installProductPerformanceDocumentForProbe(
    talas::project::ProjectDocument document)
{
    if (hardwareInitialisationStarted_)
        return false;
    prepareForDocumentReplacement();
    const auto replaced = projectBridge_.editor().replaceDocument(
        std::move(document));
    if (!replaced.ok())
    {
        rebuildOscEgress();
        return false;
    }
    resetForDocumentReplacement();
    documentChanged();
    return true;
}

bool WorkbenchComponent::productPerformanceDeviceReadyForProbe()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    return hardwareInitialised_
        && device != nullptr
        && device->getCurrentSampleRate() > 0.0
        && device->getCurrentBufferSizeSamples() > 0
        && device->getActiveOutputChannels().countNumberOfSetBits() > 0;
}

bool WorkbenchComponent::beginProductPerformancePlaybackForProbe()
{
    if (!productPerformanceDeviceReadyForProbe())
        return false;
    playCurrentProject();
    requestDeviceMetricsReset();
    return currentDocumentStaged_ && transportPlaying_;
}

std::optional<ProductDeviceProbeObservation>
WorkbenchComponent::productPerformanceDeviceObservationForProbe()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
        return std::nullopt;
    refreshPreparedGraphStatus();
    if (latestRealtimeMetrics_.deviceDeadlineAvailable == 0
        || latestRealtimeMetrics_.deviceCallbacks == 0
        || latestRealtimeMetrics_.devicePeriodNanoseconds == 0)
    {
        return std::nullopt;
    }
    ProductDeviceProbeObservation result;
    result.name = device->getName().toStdString();
    result.type = device->getTypeName().toStdString();
    result.sampleRate = static_cast<std::uint64_t>(std::llround(
        device->getCurrentSampleRate()));
    result.blockFrames = static_cast<std::uint64_t>(
        device->getCurrentBufferSizeSamples());
    result.inputChannels = static_cast<std::uint64_t>(
        device->getActiveInputChannels().countNumberOfSetBits());
    result.outputChannels = static_cast<std::uint64_t>(
        device->getActiveOutputChannels().countNumberOfSetBits());
    result.metrics = latestRealtimeMetrics_;
    return result;
}

void WorkbenchComponent::stopProductPerformancePlaybackForProbe()
{
    pauseCurrentProject();
}
#endif
}
