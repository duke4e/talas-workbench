#pragma once

#include "talas/runtime/RealtimeMetrics.h"

namespace talas::app::workbench
{
// Whether two realtime metrics snapshots show the same thing in the
// status line (the Workbench's runtime and its smoke fixture share it).
[[nodiscard]] inline bool sameRealtimeMetricsPresentation(
    const talas::runtime::RealtimeMetricsSnapshotV1& left,
    const talas::runtime::RealtimeMetricsSnapshotV1& right) noexcept
{
    return left.epoch == right.epoch
        && left.activeStageId == right.activeStageId
        && left.renderFailures == right.renderFailures
        && left.stageRejections == right.stageRejections
        && left.discardedOutputChannels
            == right.discardedOutputChannels
        && left.controlQueueOverflow == right.controlQueueOverflow
        && left.staleGenerationOrStage
            == right.staleGenerationOrStage
        && left.lateOrClampedTiming == right.lateOrClampedTiming
        && left.invalidTargetOrPayload
            == right.invalidTargetOrPayload
        && left.typedEventBatchOverflow
            == right.typedEventBatchOverflow
        && left.aggregateControlRejected
            == right.aggregateControlRejected
        && left.dspRejected == right.dspRejected
        && left.analysisDroppedBlocks == right.analysisDroppedBlocks
        && left.streamDecodeFailures == right.streamDecodeFailures
        && left.streamUnderrunEvents == right.streamUnderrunEvents
        && left.streamMissingFrames == right.streamMissingFrames
        && left.streamRequestRejections
            == right.streamRequestRejections
        && left.streamResponseQueueFull
            == right.streamResponseQueueFull
        && left.streamStaleResponseBlocks
            == right.streamStaleResponseBlocks
        && left.deviceDeadlineAvailable
            == right.deviceDeadlineAvailable
        && left.deviceDeadlineOverruns
            == right.deviceDeadlineOverruns
        && (left.deviceDeadlineOverruns == 0
            || left.deviceMaximumCallbackNanoseconds
                == right.deviceMaximumCallbackNanoseconds)
        && left.devicePeriodNanoseconds
            == right.devicePeriodNanoseconds;
}
}
