#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace talas::runtime
{
inline constexpr std::uint32_t realtimeMetricsVersionV1 = 1;

struct RealtimeGraphMetricsSnapshot
{
    std::uint64_t renderCallbacks = 0;
    std::uint64_t renderFailures = 0;
    std::uint64_t stageRejections = 0;
};

struct RealtimeMetricsSnapshotV1
{
    std::uint32_t version = realtimeMetricsVersionV1;
    std::uint32_t reserved = 0;
    std::uint64_t epoch = 0;
    std::uint64_t activeStageId = 0;

    std::uint64_t renderCallbacks = 0;
    std::uint64_t renderFailures = 0;
    std::uint64_t stageRejections = 0;
    std::uint64_t discardedOutputChannels = 0;

    std::uint64_t preparedTypedEvents = 0;
    std::uint64_t controlQueueOverflow = 0;
    std::uint64_t staleGenerationOrStage = 0;
    std::uint64_t lateOrClampedTiming = 0;
    std::uint64_t invalidTargetOrPayload = 0;
    std::uint64_t typedEventBatchOverflow = 0;
    // Legacy overlapping signal; category counters below are not disjoint.
    std::uint64_t aggregateControlRejected = 0;
    std::uint64_t dspRejected = 0;

    std::uint64_t analysisProbeCount = 0;
    std::uint64_t analysisDroppedBlocks = 0;

    std::uint64_t streamConsumerCount = 0;
    std::uint64_t streamDecodedBlocks = 0;
    std::uint64_t streamDecodeFailures = 0;
    std::uint64_t streamUnderrunEvents = 0;
    std::uint64_t streamMissingFrames = 0;
    std::uint64_t streamRequestRejections = 0;
    std::uint64_t streamResponseQueueFull = 0;
    std::uint64_t streamStaleResponseBlocks = 0;

    std::uint64_t deviceCallbacks = 0;
    std::uint64_t deviceDeadlineOverruns = 0;
    std::uint64_t deviceMaximumCallbackNanoseconds = 0;
    std::uint64_t devicePeriodNanoseconds = 0;
    std::uint8_t deviceDeadlineAvailable = 0;
    // Uses one reserved V1 byte without changing the fixed 240-byte layout.
    std::uint8_t streamHealthAvailable = 0;
    std::uint8_t reservedBytes[6]{};
};

[[nodiscard]] constexpr std::uint64_t realtimeMetricDelta(
    std::uint64_t current,
    std::uint64_t baseline) noexcept
{
    return current >= baseline ? current - baseline : 0;
}

[[nodiscard]] constexpr std::uint64_t realtimeMetricAddSaturated(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] constexpr RealtimeMetricsSnapshotV1 realtimeMetricsSince(
    const RealtimeMetricsSnapshotV1& current,
    const RealtimeMetricsSnapshotV1& baseline,
    std::uint64_t epoch) noexcept
{
    auto result = current;
    result.epoch = epoch;
    result.renderCallbacks = realtimeMetricDelta(
        current.renderCallbacks, baseline.renderCallbacks);
    result.renderFailures = realtimeMetricDelta(
        current.renderFailures, baseline.renderFailures);
    result.stageRejections = realtimeMetricDelta(
        current.stageRejections, baseline.stageRejections);
    result.discardedOutputChannels = realtimeMetricDelta(
        current.discardedOutputChannels,
        baseline.discardedOutputChannels);
    result.preparedTypedEvents = realtimeMetricDelta(
        current.preparedTypedEvents, baseline.preparedTypedEvents);
    result.controlQueueOverflow = realtimeMetricDelta(
        current.controlQueueOverflow, baseline.controlQueueOverflow);
    result.staleGenerationOrStage = realtimeMetricDelta(
        current.staleGenerationOrStage,
        baseline.staleGenerationOrStage);
    result.lateOrClampedTiming = realtimeMetricDelta(
        current.lateOrClampedTiming, baseline.lateOrClampedTiming);
    result.invalidTargetOrPayload = realtimeMetricDelta(
        current.invalidTargetOrPayload,
        baseline.invalidTargetOrPayload);
    result.typedEventBatchOverflow = realtimeMetricDelta(
        current.typedEventBatchOverflow,
        baseline.typedEventBatchOverflow);
    result.aggregateControlRejected = realtimeMetricDelta(
        current.aggregateControlRejected,
        baseline.aggregateControlRejected);
    result.dspRejected = realtimeMetricDelta(
        current.dspRejected, baseline.dspRejected);
    result.analysisDroppedBlocks = realtimeMetricDelta(
        current.analysisDroppedBlocks,
        baseline.analysisDroppedBlocks);
    result.streamDecodedBlocks = realtimeMetricDelta(
        current.streamDecodedBlocks, baseline.streamDecodedBlocks);
    result.streamDecodeFailures = realtimeMetricDelta(
        current.streamDecodeFailures, baseline.streamDecodeFailures);
    result.streamUnderrunEvents = realtimeMetricDelta(
        current.streamUnderrunEvents, baseline.streamUnderrunEvents);
    result.streamMissingFrames = realtimeMetricDelta(
        current.streamMissingFrames, baseline.streamMissingFrames);
    result.streamRequestRejections = realtimeMetricDelta(
        current.streamRequestRejections,
        baseline.streamRequestRejections);
    result.streamResponseQueueFull = realtimeMetricDelta(
        current.streamResponseQueueFull,
        baseline.streamResponseQueueFull);
    result.streamStaleResponseBlocks = realtimeMetricDelta(
        current.streamStaleResponseBlocks,
        baseline.streamStaleResponseBlocks);
    if (current.streamHealthAvailable == 0)
    {
        result.streamConsumerCount = 0;
        result.streamDecodedBlocks = 0;
        result.streamDecodeFailures = 0;
        result.streamUnderrunEvents = 0;
        result.streamMissingFrames = 0;
        result.streamRequestRejections = 0;
        result.streamResponseQueueFull = 0;
        result.streamStaleResponseBlocks = 0;
    }
    result.deviceCallbacks = realtimeMetricDelta(
        current.deviceCallbacks, baseline.deviceCallbacks);
    result.deviceDeadlineOverruns = realtimeMetricDelta(
        current.deviceDeadlineOverruns,
        baseline.deviceDeadlineOverruns);
    // Maximum and period values are gauges; the Workbench maximum is
    // lifetime-monotonic for the current process rather than reset in place.
    return result;
}

[[nodiscard]] constexpr bool realtimeMetricsCountersRegressed(
    const RealtimeMetricsSnapshotV1& current,
    const RealtimeMetricsSnapshotV1& baseline) noexcept
{
    return current.renderCallbacks < baseline.renderCallbacks
        || current.renderFailures < baseline.renderFailures
        || current.stageRejections < baseline.stageRejections
        || current.discardedOutputChannels
            < baseline.discardedOutputChannels
        || current.preparedTypedEvents < baseline.preparedTypedEvents
        || current.controlQueueOverflow < baseline.controlQueueOverflow
        || current.staleGenerationOrStage
            < baseline.staleGenerationOrStage
        || current.lateOrClampedTiming < baseline.lateOrClampedTiming
        || current.invalidTargetOrPayload
            < baseline.invalidTargetOrPayload
        || current.typedEventBatchOverflow
            < baseline.typedEventBatchOverflow
        || current.aggregateControlRejected
            < baseline.aggregateControlRejected
        || current.dspRejected < baseline.dspRejected
        || current.analysisDroppedBlocks
            < baseline.analysisDroppedBlocks
        || (current.streamHealthAvailable != 0
            && baseline.streamHealthAvailable != 0
            && (current.streamDecodedBlocks
                    < baseline.streamDecodedBlocks
                || current.streamDecodeFailures
                    < baseline.streamDecodeFailures
                || current.streamUnderrunEvents
                    < baseline.streamUnderrunEvents
                || current.streamMissingFrames
                    < baseline.streamMissingFrames
                || current.streamRequestRejections
                    < baseline.streamRequestRejections
                || current.streamResponseQueueFull
                    < baseline.streamResponseQueueFull
                || current.streamStaleResponseBlocks
                    < baseline.streamStaleResponseBlocks))
        || current.deviceCallbacks < baseline.deviceCallbacks
        || current.deviceDeadlineOverruns
            < baseline.deviceDeadlineOverruns;
}

[[nodiscard]] constexpr RealtimeMetricsSnapshotV1
realtimeMetricsRegressionBaseline(
    const RealtimeMetricsSnapshotV1& current,
    const RealtimeMetricsSnapshotV1& previous) noexcept
{
    const auto baselineFor = [](std::uint64_t value,
                                std::uint64_t prior) constexpr noexcept {
        return value < prior ? value : prior;
    };
    auto result = current;
    result.renderCallbacks = baselineFor(
        current.renderCallbacks, previous.renderCallbacks);
    result.renderFailures = baselineFor(
        current.renderFailures, previous.renderFailures);
    result.stageRejections = baselineFor(
        current.stageRejections, previous.stageRejections);
    result.discardedOutputChannels = baselineFor(
        current.discardedOutputChannels,
        previous.discardedOutputChannels);
    result.preparedTypedEvents = baselineFor(
        current.preparedTypedEvents, previous.preparedTypedEvents);
    result.controlQueueOverflow = baselineFor(
        current.controlQueueOverflow, previous.controlQueueOverflow);
    result.staleGenerationOrStage = baselineFor(
        current.staleGenerationOrStage,
        previous.staleGenerationOrStage);
    result.lateOrClampedTiming = baselineFor(
        current.lateOrClampedTiming, previous.lateOrClampedTiming);
    result.invalidTargetOrPayload = baselineFor(
        current.invalidTargetOrPayload,
        previous.invalidTargetOrPayload);
    result.typedEventBatchOverflow = baselineFor(
        current.typedEventBatchOverflow,
        previous.typedEventBatchOverflow);
    result.aggregateControlRejected = baselineFor(
        current.aggregateControlRejected,
        previous.aggregateControlRejected);
    result.dspRejected = baselineFor(
        current.dspRejected, previous.dspRejected);
    result.analysisDroppedBlocks = baselineFor(
        current.analysisDroppedBlocks,
        previous.analysisDroppedBlocks);
    if (current.streamHealthAvailable != 0
        && previous.streamHealthAvailable != 0)
    {
        result.streamDecodedBlocks = baselineFor(
            current.streamDecodedBlocks,
            previous.streamDecodedBlocks);
        result.streamDecodeFailures = baselineFor(
            current.streamDecodeFailures,
            previous.streamDecodeFailures);
        result.streamUnderrunEvents = baselineFor(
            current.streamUnderrunEvents,
            previous.streamUnderrunEvents);
        result.streamMissingFrames = baselineFor(
            current.streamMissingFrames,
            previous.streamMissingFrames);
        result.streamRequestRejections = baselineFor(
            current.streamRequestRejections,
            previous.streamRequestRejections);
        result.streamResponseQueueFull = baselineFor(
            current.streamResponseQueueFull,
            previous.streamResponseQueueFull);
        result.streamStaleResponseBlocks = baselineFor(
            current.streamStaleResponseBlocks,
            previous.streamStaleResponseBlocks);
    }
    result.deviceCallbacks = baselineFor(
        current.deviceCallbacks, previous.deviceCallbacks);
    result.deviceDeadlineOverruns = baselineFor(
        current.deviceDeadlineOverruns,
        previous.deviceDeadlineOverruns);
    return result;
}

static_assert(std::is_trivially_copyable_v<RealtimeGraphMetricsSnapshot>);
static_assert(std::is_trivially_copyable_v<RealtimeMetricsSnapshotV1>);
static_assert(std::is_standard_layout_v<RealtimeGraphMetricsSnapshot>);
static_assert(std::is_standard_layout_v<RealtimeMetricsSnapshotV1>);
static_assert(sizeof(RealtimeGraphMetricsSnapshot) == 24);
static_assert(sizeof(RealtimeMetricsSnapshotV1) == 240);
static_assert(
    alignof(RealtimeGraphMetricsSnapshot) == alignof(std::uint64_t));
static_assert(
    alignof(RealtimeMetricsSnapshotV1) == alignof(std::uint64_t));
}
