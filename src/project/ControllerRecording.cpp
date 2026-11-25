#include "talas/project/ControllerRecording.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace talas::project
{
namespace
{
ControllerRecordingError failure(
    ControllerRecordingErrorCode code,
    std::string message)
{
    return {code, std::move(message)};
}

bool terminalAllowsApproval(ControllerRecordingTerminal terminal) noexcept
{
    return terminal == ControllerRecordingTerminal::Completed
        || terminal == ControllerRecordingTerminal::PunchComplete;
}

bool knownOrigin(ControllerRecordingOrigin origin) noexcept
{
    switch (origin)
    {
    case ControllerRecordingOrigin::Midi:
    case ControllerRecordingOrigin::Osc:
    case ControllerRecordingOrigin::Ui:
        return true;
    }
    return false;
}

bool knownScope(ScoreAutomationScope scope) noexcept
{
    switch (scope)
    {
    case ScoreAutomationScope::Clip:
    case ScoreAutomationScope::Track:
    case ScoreAutomationScope::Master:
        return true;
    }
    return false;
}

bool knownTimeDomain(ScoreTimeDomain domain) noexcept
{
    switch (domain)
    {
    case ScoreTimeDomain::Seconds:
    case ScoreTimeDomain::MusicalTicks:
        return true;
    }
    return false;
}

bool knownValueDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

bool validEventName(std::string_view value) noexcept
{
    const auto validStart = [](char character) {
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || character == '_';
    };
    return !value.empty()
        && value.size() <= maximumScoreIdentifierBytes
        && validStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [&](char character) {
                return validStart(character)
                    || (character >= '0' && character <= '9');
            });
}

std::optional<ScoreTime> scoreTimeAtFrame(
    std::uint64_t frame,
    ScoreTimeDomain domain,
    const ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate) noexcept
{
    if (domain == ScoreTimeDomain::Seconds)
    {
        const auto seconds = static_cast<double>(frame)
            / static_cast<double>(sampleRate);
        ScoreTime result{domain, seconds, 0};
        const auto roundTrip = scoreTimeToSampleFrame(
            result, musicalTime, sampleRate);
        return roundTrip == std::optional<std::uint64_t>{frame}
            ? std::optional<ScoreTime>{result}
            : std::nullopt;
    }
    const auto tick = sampleFrameToMusicalTick(
        musicalTime, frame, sampleRate);
    if (!tick.has_value())
        return std::nullopt;
    ScoreTime result{domain, 0.0, *tick};
    const auto roundTrip = scoreTimeToSampleFrame(
        result, musicalTime, sampleRate);
    return roundTrip.has_value()
            && *roundTrip != std::numeric_limits<std::uint64_t>::max()
        ? std::optional<ScoreTime>{result}
        : std::nullopt;
}

std::optional<ScoreTime> scoreTimeForEvent(
    const ControllerRecordingTake& take,
    const ControllerRecordingTarget& target,
    const ControllerRecordingEvent& event) noexcept
{
    if (target.scope == ScoreAutomationScope::Clip
        && event.absoluteFrame < target.ownerStartFrame)
    {
        return std::nullopt;
    }
    if (target.scope == ScoreAutomationScope::Clip
        && target.timeDomain == ScoreTimeDomain::MusicalTicks)
    {
        const auto absoluteTick = sampleFrameToMusicalTick(
            take.musicalTime,
            event.absoluteFrame,
            take.sampleRate);
        if (!absoluteTick.has_value()
            || *absoluteTick < target.ownerStartTick)
        {
            return std::nullopt;
        }
        return ScoreTime{
            ScoreTimeDomain::MusicalTicks,
            0.0,
            *absoluteTick - target.ownerStartTick,
        };
    }
    const auto relativeFrame = target.scope == ScoreAutomationScope::Clip
        ? event.absoluteFrame - target.ownerStartFrame
        : event.absoluteFrame;
    return scoreTimeAtFrame(
        relativeFrame,
        target.timeDomain,
        take.musicalTime,
        take.sampleRate);
}
}

std::optional<ControllerRecordingError>
controllerRecordingTakeValidationError(
    const ControllerRecordingTake& take)
{
    if (take.takeGeneration == 0 || take.stageGeneration == 0
        || take.sampleRate == 0 || take.endFrame < take.startFrame
        || !isValidProjectMusicalTime(take.musicalTime))
    {
        return failure(
            ControllerRecordingErrorCode::InvalidTake,
            "Controller recording take identity, rate, or frame range is invalid");
    }
    if (take.targets.empty()
        || take.targets.size() > maximumControllerRecordingTargets
        || take.events.size() > maximumControllerRecordingEvents)
    {
        return failure(
            ControllerRecordingErrorCode::CapacityExceeded,
            "Controller recording target or event capacity is invalid");
    }
    std::unordered_set<std::uint32_t> eventIds;
    std::unordered_set<std::string_view> laneIds;
    eventIds.reserve(take.targets.size());
    laneIds.reserve(take.targets.size());
    for (const auto& target : take.targets)
    {
        if (target.eventId == 0
            || !knownScope(target.scope)
            || !knownTimeDomain(target.timeDomain)
            || !knownValueDomain(target.domain)
            || !isValidScoreIdentifier(target.laneId)
            || !isValidScoreIdentifier(target.ownerId)
            || !validEventName(target.eventName)
            || !eventIds.insert(target.eventId).second
            || !laneIds.insert(target.laneId).second
            || (target.scope != ScoreAutomationScope::Clip
                && (target.ownerStartFrame != 0
                    || target.ownerStartTick != 0))
            || (target.timeDomain == ScoreTimeDomain::Seconds
                && target.ownerStartTick != 0))
        {
            return failure(
                ControllerRecordingErrorCode::InvalidTarget,
                "Controller recording target identity is invalid or ambiguous");
        }
        if (target.scope == ScoreAutomationScope::Clip
            && target.timeDomain == ScoreTimeDomain::MusicalTicks)
        {
            const auto ownerFrame = scoreTimeToSampleFrame(
                ScoreTime{
                    ScoreTimeDomain::MusicalTicks,
                    0.0,
                    target.ownerStartTick,
                },
                take.musicalTime,
                take.sampleRate);
            if (ownerFrame != std::optional<std::uint64_t>{
                    target.ownerStartFrame})
            {
                return failure(
                    ControllerRecordingErrorCode::InvalidTarget,
                    "Controller recording clip owner time is inconsistent with the frozen musical map");
            }
        }
    }
    std::uint64_t previousSequence = 0;
    for (const auto& event : take.events)
    {
        const auto target = std::ranges::find(
            take.targets, event.eventId,
            &ControllerRecordingTarget::eventId);
        const auto originAuthorityValid = event.origin
                == ControllerRecordingOrigin::Ui
            ? event.routeGeneration != 0
            : event.routeGeneration != 0 && event.sourceGeneration != 0;
        if (target == take.targets.end()
            || !knownOrigin(event.origin)
            || event.sequence == 0
            || event.sequence <= previousSequence
            || !std::isfinite(event.value)
            || event.absoluteFrame < take.startFrame
            || event.absoluteFrame > take.endFrame
            || event.sourceTimestampNanoseconds <= 0
            || event.stageGeneration != take.stageGeneration
            || !originAuthorityValid)
        {
            return failure(
                ControllerRecordingErrorCode::InvalidEvent,
                "Controller recording event evidence is invalid or stale");
        }
        previousSequence = event.sequence;
    }
    return std::nullopt;
}

bool controllerRecordingTakeAllowsApproval(
    const ControllerRecordingTake& take) noexcept
{
    return terminalAllowsApproval(take.terminal) && !take.events.empty();
}

ControllerRecordingPreviewResult
previewControllerRecordingSimplification(
    const ControllerRecordingTake& take,
    double tolerance,
    std::uint64_t previewGeneration)
{
    ControllerRecordingPreviewResult result;
    try
    {
        if (const auto error = controllerRecordingTakeValidationError(take))
        {
            result.error = *error;
            return result;
        }
        if (!controllerRecordingTakeAllowsApproval(take))
        {
            result.error = failure(
                ControllerRecordingErrorCode::IncompleteTake,
                "Empty, cancelled, or incomplete controller recordings cannot be simplified");
            return result;
        }
        if (!std::isfinite(tolerance) || tolerance < 0.0
            || previewGeneration == 0)
        {
            result.error = failure(
                ControllerRecordingErrorCode::InvalidTolerance,
                "Controller recording simplification tolerance or generation is invalid");
            return result;
        }
        ControllerRecordingSimplificationPreview preview;
        preview.takeGeneration = take.takeGeneration;
        preview.stageGeneration = take.stageGeneration;
        preview.previewGeneration = previewGeneration;
        preview.tolerance = tolerance;
        preview.rawEventCount = take.events.size();
        preview.events.reserve(take.events.size());

        auto ordered = take.events;
        std::ranges::sort(
            ordered,
            [](const auto& left, const auto& right) {
                if (left.eventId != right.eventId)
                    return left.eventId < right.eventId;
                if (left.absoluteFrame != right.absoluteFrame)
                    return left.absoluteFrame < right.absoluteFrame;
                return left.sequence < right.sequence;
            });
        std::vector<ControllerRecordingEvent> canonical;
        canonical.reserve(ordered.size());
        for (const auto& event : ordered)
        {
            if (!canonical.empty()
                && canonical.back().eventId == event.eventId
                && canonical.back().absoluteFrame == event.absoluteFrame)
            {
                canonical.back() = event;
            }
            else
            {
                canonical.push_back(event);
            }
        }
        std::vector<ControllerRecordingEvent> timeCanonical;
        timeCanonical.reserve(canonical.size());
        std::optional<ScoreTime> previousTime;
        std::uint32_t previousEventId = 0;
        for (const auto& event : canonical)
        {
            const auto target = std::ranges::find(
                take.targets,
                event.eventId,
                &ControllerRecordingTarget::eventId);
            const auto time = target == take.targets.end()
                ? std::optional<ScoreTime>{}
                : scoreTimeForEvent(take, *target, event);
            if (!time.has_value())
            {
                result.error = failure(
                    ControllerRecordingErrorCode::TimeConversionFailed,
                    "Controller recording timestamp cannot be represented by the target lane");
                return result;
            }
            if (!timeCanonical.empty()
                && previousEventId == event.eventId
                && previousTime == time)
            {
                timeCanonical.back() = event;
            }
            else
            {
                timeCanonical.push_back(event);
            }
            previousEventId = event.eventId;
            previousTime = *time;
        }

        std::unordered_map<std::uint32_t, std::size_t> finalIndex;
        finalIndex.reserve(take.targets.size());
        for (std::size_t index = 0; index < timeCanonical.size(); ++index)
            finalIndex[timeCanonical[index].eventId] = index;
        std::unordered_map<std::uint32_t, double> lastKept;
        lastKept.reserve(take.targets.size());
        for (std::size_t index = 0; index < timeCanonical.size(); ++index)
        {
            const auto& event = timeCanonical[index];
            const auto previous = lastKept.find(event.eventId);
            const auto keep = previous == lastKept.end()
                || finalIndex.at(event.eventId) == index
                || std::abs(event.value - previous->second) > tolerance;
            if (!keep)
                continue;
            preview.events.push_back(event);
            lastKept[event.eventId] = event.value;
        }
        result.preview = std::move(preview);
    }
    catch (const std::bad_alloc&)
    {
        result.error = failure(
            ControllerRecordingErrorCode::CapacityExceeded,
            "Controller recording simplification allocation failed");
    }
    catch (const std::length_error&)
    {
        result.error = failure(
            ControllerRecordingErrorCode::CapacityExceeded,
            "Controller recording simplification exceeded its fixed capacity");
    }
    return result;
}

ControllerRecordingApprovalResult
approveControllerRecordingSimplification(
    const ControllerRecordingTake& take,
    const ControllerRecordingSimplificationPreview& preview)
{
    ControllerRecordingApprovalResult result;
    try
    {
        if (const auto error = controllerRecordingTakeValidationError(take))
        {
            result.error = *error;
            return result;
        }
        if (!controllerRecordingTakeAllowsApproval(take))
        {
            result.error = failure(
                ControllerRecordingErrorCode::IncompleteTake,
                "Incomplete controller recording cannot be approved");
            return result;
        }
        if (preview.previewGeneration == 0
            || preview.takeGeneration != take.takeGeneration
            || preview.stageGeneration != take.stageGeneration
            || preview.rawEventCount != take.events.size()
            || preview.events.size() > take.events.size()
            || !std::isfinite(preview.tolerance)
            || preview.tolerance < 0.0)
        {
            result.error = failure(
                ControllerRecordingErrorCode::StalePreview,
                "Controller recording preview is stale or malformed");
            return result;
        }
        const auto expected = previewControllerRecordingSimplification(
            take, preview.tolerance, preview.previewGeneration);
        if (!expected.ok() || *expected.preview != preview)
        {
            result.error = failure(
                ControllerRecordingErrorCode::StalePreview,
                "Controller recording preview does not match the retained raw take");
            return result;
        }
        ProjectScoreEditBatch edits;
        edits.automation.reserve(take.targets.size());
        for (const auto& target : take.targets)
        {
            ScoreAutomationLane lane;
            lane.laneId = target.laneId;
            lane.scope = target.scope;
            lane.ownerId = target.ownerId;
            lane.eventName = target.eventName;
            lane.domain = target.domain;
            const auto targetCount = std::ranges::count(
                preview.events, target.eventId,
                &ControllerRecordingEvent::eventId);
            lane.points.reserve(static_cast<std::size_t>(targetCount));
            for (const auto& event : preview.events)
            {
                if (event.eventId != target.eventId)
                    continue;
                if (event.stageGeneration != take.stageGeneration)
                {
                    result.error = failure(
                        ControllerRecordingErrorCode::StalePreview,
                        "Controller recording preview contains stale stage evidence");
                    return result;
                }
                const auto time = scoreTimeForEvent(take, target, event);
                if (!time.has_value())
                {
                    result.error = failure(
                        ControllerRecordingErrorCode::TimeConversionFailed,
                        "Controller recording timestamp cannot be represented by the target lane");
                    return result;
                }
                lane.points.push_back({
                    *time,
                    event.value,
                    talas::core::AutomationInterpolation::Linear,
                });
            }
            if (!lane.points.empty())
                edits.automation.push_back(std::move(lane));
        }
        if (edits.automation.empty())
        {
            result.error = failure(
                ControllerRecordingErrorCode::InvalidEvent,
                "Controller recording preview contains no approvable events");
            return result;
        }
        result.edits = std::move(edits);
    }
    catch (const std::bad_alloc&)
    {
        result.error = failure(
            ControllerRecordingErrorCode::CapacityExceeded,
            "Controller recording approval allocation failed");
    }
    catch (const std::length_error&)
    {
        result.error = failure(
            ControllerRecordingErrorCode::CapacityExceeded,
            "Controller recording approval exceeded its fixed capacity");
    }
    return result;
}
}
