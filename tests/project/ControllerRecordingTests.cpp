#include "talas/project/ControllerRecording.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

talas::project::ControllerRecordingTake take()
{
    using namespace talas::project;
    ControllerRecordingTake result;
    result.takeGeneration = 7;
    result.stageGeneration = 11;
    result.sampleRate = 48000;
    result.startFrame = 48000;
    result.endFrame = 96000;
    result.terminal = ControllerRecordingTerminal::Completed;
    result.targets = {{
        3,
        "recordedGain",
        ScoreAutomationScope::Track,
        "voice",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        ScoreTimeDomain::Seconds,
        0,
    }};
    result.events = {
        {1, 3, 0.0, 48000, 1'000'000'000, 11, 13, 17,
         ControllerRecordingOrigin::Midi},
        {2, 3, 0.01, 60000, 1'250'000'000, 11, 13, 17,
         ControllerRecordingOrigin::Midi},
        {3, 3, 0.02, 72000, 1'500'000'000, 11, 13, 17,
         ControllerRecordingOrigin::Midi},
        {4, 3, 1.0, 96000, 2'000'000'000, 11, 13, 17,
         ControllerRecordingOrigin::Midi},
    };
    return result;
}

void testRawTakeRetainsExactEvidenceAndPreviewIsNonMutating()
{
    const auto raw = take();
    require(!talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "exact controller recording take should validate");
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.05, 19);
    require(preview.ok()
                && preview.preview->rawEventCount == 4
                && preview.preview->events.size() == 2
                && preview.preview->events.front() == raw.events.front()
                && preview.preview->events.back() == raw.events.back()
                && raw.events.size() == 4,
            "preview should simplify deterministic intermediate values without mutating exact raw evidence");
}

void testApprovalRequiresExactPreviewGenerationAndBuildsOneLane()
{
    const auto raw = take();
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.05, 19);
    require(preview.ok(), "approval fixture preview should succeed");
    const auto approved = talas::project::approveControllerRecordingSimplification(
        raw, *preview.preview);
    require(approved.ok()
                && approved.edits->automation.size() == 1
                && approved.edits->automation.front().points.size() == 2
                && approved.edits->automation.front().points.front()
                       .position.seconds == 1.0
                && approved.edits->automation.front().points.back()
                       .position.seconds == 2.0,
            "explicit approval should convert only the retained preview into exact lane points");

    auto stale = *preview.preview;
    ++stale.takeGeneration;
    require(!talas::project::approveControllerRecordingSimplification(
                 raw, stale).ok(),
            "stale preview generation must not replace the raw take");

    auto forged = *preview.preview;
    forged.events.front().value = 0.5;
    require(!talas::project::approveControllerRecordingSimplification(
                 raw, forged).ok(),
            "approval must recompute and reject a forged simplification preview");
}

void testTerminalAndAuthorityFailuresRemainUncommittable()
{
    auto raw = take();
    raw.terminal = talas::project::ControllerRecordingTerminal::SourceLost;
    require(!talas::project::previewControllerRecordingSimplification(
                 raw, 0.0, 1).ok(),
            "source-loss take must remain visibly incomplete and uncommittable");

    raw = take();
    raw.events[1].stageGeneration = 12;
    require(talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "mixed-stage raw events must fail closed before preview");

    raw = take();
    raw.events[1].origin = talas::project::ControllerRecordingOrigin::Osc;
    raw.events[1].routeGeneration = 21;
    raw.events[1].sourceGeneration = 22;
    raw.events[1].sourceTimestampNanoseconds = 1;
    require(!talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "independent exact source clocks may interleave without rewriting timestamps");

    raw.events[1].sourceTimestampNanoseconds = 0;
    require(talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "missing exact source timestamps must fail closed");
}

void testClipLocalAndMusicalApprovalPreserveOwnerAuthority()
{
    auto raw = take();
    raw.sampleRate = 48000;
    raw.startFrame = 12000;
    raw.endFrame = 36000;
    raw.targets.front().scope = talas::project::ScoreAutomationScope::Clip;
    raw.targets.front().ownerId = "clip";
    raw.targets.front().timeDomain =
        talas::project::ScoreTimeDomain::MusicalTicks;
    raw.targets.front().ownerStartFrame = 12000;
    raw.targets.front().ownerStartTick = 480;
    raw.events = {
        {1, 3, 0.25, 12000, 1, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Osc},
        {2, 3, 0.75, 36000, 2, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Osc},
    };
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.0, 23);
    const auto approved = preview.ok()
        ? talas::project::approveControllerRecordingSimplification(
            raw, *preview.preview)
        : talas::project::ControllerRecordingApprovalResult{};
    require(approved.ok()
                && approved.edits->automation.front().points.front()
                       .position.tick == 0
                && approved.edits->automation.front().points.back()
                       .position.tick == 960,
            "clip-local approval should subtract the frozen owner start and preserve musical intent");
}

void testApprovalUsesTheMusicalMapFrozenWithTheRawTake()
{
    auto raw = take();
    raw.sampleRate = 48000;
    raw.musicalTime.tempo = {{0, 1'000'000}};
    raw.targets.front().timeDomain =
        talas::project::ScoreTimeDomain::MusicalTicks;
    raw.events = {
        {1, 3, 0.25, 48000, 1, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
    };
    raw.endFrame = 48000;
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.0, 29);
    const auto approved = preview.ok()
        ? talas::project::approveControllerRecordingSimplification(
            raw, *preview.preview)
        : talas::project::ControllerRecordingApprovalResult{};
    require(approved.ok()
                && approved.edits->automation.front().points.front()
                       .position.tick == 960,
            "approval must use the tempo authority frozen into the raw take rather than later editor state");
}

void testPreviewOrdersIndependentProducersAndCoalescesOneFrame()
{
    auto raw = take();
    raw.events = {
        {1, 3, 0.75, 72000, 3, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
        {2, 3, 0.25, 60000, 1, 11, 19, 23,
         talas::project::ControllerRecordingOrigin::Osc},
        {3, 3, 0.5, 60000, 2, 11, 19, 23,
         talas::project::ControllerRecordingOrigin::Osc},
    };
    const auto rawBefore = raw.events;
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.0, 31);
    const auto approved = preview.ok()
        ? talas::project::approveControllerRecordingSimplification(
            raw, *preview.preview)
        : talas::project::ControllerRecordingApprovalResult{};
    require(preview.ok()
                && preview.preview->events.size() == 2
                && preview.preview->events.front().absoluteFrame == 60000
                && preview.preview->events.front().value == 0.5
                && preview.preview->events.back().absoluteFrame == 72000
                && raw.events == rawBefore,
            "preview should order independent producers, keep the final same-frame value, and leave raw evidence unchanged");
    require(approved.ok()
                && approved.edits->automation.front().points.size() == 2,
            "canonical preview order should always produce a valid increasing automation lane");

    raw.targets.front().timeDomain =
        static_cast<talas::project::ScoreTimeDomain>(255);
    require(talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "unknown target enum authority must fail closed before preview");

    raw = take();
    raw.targets.front().eventName = "invalid-name";
    require(talas::project::controllerRecordingTakeValidationError(raw).has_value(),
            "recording targets must use the exact Score Event Value grammar");
}

void testClipLocalMusicalApprovalUsesAbsoluteTempoMapTicks()
{
    auto raw = take();
    raw.sampleRate = 48000;
    raw.musicalTime.tempo = {{0, 500000}, {960, 1'000'000}};
    raw.startFrame = 24000;
    raw.endFrame = 72000;
    auto& target = raw.targets.front();
    target.scope = talas::project::ScoreAutomationScope::Clip;
    target.ownerId = "clip";
    target.timeDomain = talas::project::ScoreTimeDomain::MusicalTicks;
    target.ownerStartFrame = 24000;
    target.ownerStartTick = 960;
    raw.events = {
        {1, 3, 0.25, 24000, 1, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
        {2, 3, 0.75, 72000, 2, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
    };
    const auto preview = talas::project::previewControllerRecordingSimplification(
        raw, 0.0, 37);
    const auto approved = preview.ok()
        ? talas::project::approveControllerRecordingSimplification(
            raw, *preview.preview)
        : talas::project::ControllerRecordingApprovalResult{};
    require(approved.ok()
                && approved.edits->automation.front().points.front()
                       .position.tick == 0
                && approved.edits->automation.front().points.back()
                       .position.tick == 960,
            "clip-local ticks must subtract absolute ticks under the frozen tempo map");

    raw = take();
    raw.targets.front().timeDomain =
        talas::project::ScoreTimeDomain::MusicalTicks;
    raw.events = {
        {1, 3, 0.25, 60000, 1, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
        {2, 3, 0.75, 60001, 2, 11, 13, 17,
         talas::project::ControllerRecordingOrigin::Midi},
    };
    const auto collapsed =
        talas::project::previewControllerRecordingSimplification(raw, 0.0, 41);
    require(collapsed.ok()
                && collapsed.preview->events.size() == 1
                && collapsed.preview->events.front().value == 0.75,
            "preview must coalesce distinct frames that map to one exact musical tick");
}
}

// The parameters armed for recording each
// take the moves made while the Timeline plays, a MIDI fader's and the
// on-screen fader's alike, and once the take is approved each lane holds
// what was played, the drawn values replaced by the recorded ones.
void testArmedParametersRecordTheirMovesIntoLanes()
{
    using namespace talas::project;
    auto raw = take();
    raw.targets.push_back({4, "recordedRate", ScoreAutomationScope::Track, "voice", "Rate", talas::expr::ValueDomain::Scalar,
        ScoreTimeDomain::Seconds, 0});
    raw.events = {
        {1, 3, 0.25, 48000, 1'000'000'000, 11, 13, 17, ControllerRecordingOrigin::Midi},
        {2, 4, 0.5, 48000, 1'000'000'000, 11, 13, 17, ControllerRecordingOrigin::Ui},
        {3, 3, 0.75, 72000, 1'500'000'000, 11, 13, 17, ControllerRecordingOrigin::Midi},
        {4, 4, 1.0, 96000, 2'000'000'000, 11, 13, 17, ControllerRecordingOrigin::Ui},
    };
    require(!controllerRecordingTakeValidationError(raw).has_value(), "a take of two armed parameters should validate");
    const auto preview = previewControllerRecordingSimplification(raw, 0.0, 19);
    require(preview.ok(), "the take should preview");
    const auto approved = approveControllerRecordingSimplification(raw, *preview.preview);
    require(approved.ok() && approved.edits->automation.size() == 2, "each armed parameter should get its lane");
    for (const auto& lane : approved.edits->automation)
    {
        std::vector<double> values;
        for (const auto& point : lane.points)
            values.push_back(point.value);
        const auto expected = lane.eventName == "Gain" ? std::vector<double>{0.25, 0.75} : std::vector<double>{0.5, 1.0};
        require(values == expected, "the " + lane.eventName + " lane should hold the moves recorded for it");
        require(lane.state == ScoreAutomationState::Automated,
            "a recorded lane is Automated, no longer live, playing what was recorded");
    }
}

int main()
{
    testRawTakeRetainsExactEvidenceAndPreviewIsNonMutating();
    testApprovalRequiresExactPreviewGenerationAndBuildsOneLane();
    testTerminalAndAuthorityFailuresRemainUncommittable();
    testClipLocalAndMusicalApprovalPreserveOwnerAuthority();
    testApprovalUsesTheMusicalMapFrozenWithTheRawTake();
    testPreviewOrdersIndependentProducersAndCoalescesOneFrame();
    testClipLocalMusicalApprovalUsesAbsoluteTempoMapTicks();
    testArmedParametersRecordTheirMovesIntoLanes();
    return 0;
}
