#pragma once

#include "talas/project/ProjectScore.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumControllerRecordingTargets = 1024;
inline constexpr std::size_t maximumControllerRecordingEvents =
    maximumScoreAutomationPoints;

enum class ControllerRecordingOrigin : std::uint8_t
{
    Midi,
    Osc,
    Ui,
};

enum class ControllerRecordingTerminal : std::uint8_t
{
    Completed,
    PunchComplete,
    Cancelled,
    SourceLost,
    DeviceChanged,
    Seeked,
    StageChanged,
    CapacityExceeded,
};

struct ControllerRecordingTarget
{
    std::uint32_t eventId = 0;
    std::string laneId;
    ScoreAutomationScope scope = ScoreAutomationScope::Track;
    std::string ownerId;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    ScoreTimeDomain timeDomain = ScoreTimeDomain::Seconds;
    std::uint64_t ownerStartFrame = 0;
    std::uint64_t ownerStartTick = 0;

    bool operator==(const ControllerRecordingTarget&) const = default;
};

struct ControllerRecordingEvent
{
    std::uint64_t sequence = 0;
    std::uint32_t eventId = 0;
    double value = 0.0;
    std::uint64_t absoluteFrame = 0;
    std::int64_t sourceTimestampNanoseconds = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t routeGeneration = 0;
    std::uint64_t sourceGeneration = 0;
    ControllerRecordingOrigin origin = ControllerRecordingOrigin::Ui;

    bool operator==(const ControllerRecordingEvent&) const = default;
};

struct ControllerRecordingTake
{
    std::uint64_t takeGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint32_t sampleRate = 0;
    ProjectMusicalTime musicalTime;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    ControllerRecordingTerminal terminal =
        ControllerRecordingTerminal::Cancelled;
    std::vector<ControllerRecordingTarget> targets;
    std::vector<ControllerRecordingEvent> events;

    bool operator==(const ControllerRecordingTake&) const = default;
};

enum class ControllerRecordingErrorCode : std::uint8_t
{
    InvalidTake,
    InvalidTarget,
    InvalidEvent,
    IncompleteTake,
    InvalidTolerance,
    StalePreview,
    CapacityExceeded,
    TimeConversionFailed,
};

struct ControllerRecordingError
{
    ControllerRecordingErrorCode code =
        ControllerRecordingErrorCode::InvalidTake;
    std::string message;

    bool operator==(const ControllerRecordingError&) const = default;
};

struct ControllerRecordingSimplificationPreview
{
    std::uint64_t takeGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t previewGeneration = 0;
    double tolerance = 0.0;
    std::size_t rawEventCount = 0;
    std::vector<ControllerRecordingEvent> events;

    bool operator==(const ControllerRecordingSimplificationPreview&) const = default;
};

struct ControllerRecordingPreviewResult
{
    std::optional<ControllerRecordingSimplificationPreview> preview;
    std::optional<ControllerRecordingError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return preview.has_value() && !error.has_value();
    }
};

struct ControllerRecordingApprovalResult
{
    std::optional<ProjectScoreEditBatch> edits;
    std::optional<ControllerRecordingError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return edits.has_value() && !error.has_value();
    }
};

[[nodiscard]] std::optional<ControllerRecordingError>
controllerRecordingTakeValidationError(
    const ControllerRecordingTake& take);

[[nodiscard]] bool controllerRecordingTakeAllowsApproval(
    const ControllerRecordingTake& take) noexcept;

[[nodiscard]] ControllerRecordingPreviewResult
previewControllerRecordingSimplification(
    const ControllerRecordingTake& take,
    double tolerance,
    std::uint64_t previewGeneration);

[[nodiscard]] ControllerRecordingApprovalResult
approveControllerRecordingSimplification(
    const ControllerRecordingTake& take,
    const ControllerRecordingSimplificationPreview& preview);
}
