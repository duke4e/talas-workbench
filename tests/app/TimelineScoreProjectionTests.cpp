#include "talas/app/TimelineScoreProjection.h"

#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
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

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value};
}

talas::project::ProjectDocument fixture()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voiceDefinition";
    voice.displayName = "Signal Voice";
    document.soundDefinitions.push_back(std::move(voice));
    document.timeline.musicalTime.tempo = {{0, 500000}, {960, 1000000}};
    document.timeline.score.tracks = {
        {"voice", "Voice", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::MusicalTicks, "music", 2,
         talas::project::ScoreOverlapMode::Layer, false, true},
        {"music", "Music", talas::project::ScoreTrackKind::Submix,
         talas::project::ScoreTimeDomain::MusicalTicks, "master", 2,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 2,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clipA", "voice", "voiceDefinition", ticks(480), ticks(960),
         ticks(120), ticks(120), ticks(240), 2},
        {"clipB", "voice", "voiceDefinition", ticks(1440), ticks(480),
         ticks(0), ticks(0), ticks(0), 1},
    };
    document.timeline.score.automation = {
        {"clipGain", talas::project::ScoreAutomationScope::Clip, "clipA",
         "Gain", talas::expr::ValueDomain::Scalar,
         {{ticks(0), 0.0, talas::core::AutomationInterpolation::Linear},
          {ticks(960), 1.0, talas::core::AutomationInterpolation::Step}}},
        {"masterGain", talas::project::ScoreAutomationScope::Master, "master",
         "MasterGain", talas::expr::ValueDomain::Scalar,
         {{seconds(0.75), 0.5, talas::core::AutomationInterpolation::Step}}},
        {"masterFrequency", talas::project::ScoreAutomationScope::Master, "master",
         "Frequency", talas::expr::ValueDomain::Hertz,
         {{seconds(0.25), 440.0, talas::core::AutomationInterpolation::Linear},
          {seconds(0.5), 880.0, talas::core::AutomationInterpolation::Step}}},
    };
    document.timeline.score.markers = {
        {"marker", "Tempo boundary", ticks(960), ticks(1920)},
    };
    return document;
}

talas::app::TimelineScoreRuntimeState runtime()
{
    talas::app::TimelineScoreRuntimeState result;
    result.stageId = 17;
    result.playheadFrame = 24000;
    result.sampleRate = 48000;
    result.playing = true;
    result.publicationValid = true;
    result.syncKind = talas::app::TimelineExternalSyncKind::MidiTimeCode;
    result.externalLocked = true;
    result.smpteRate = talas::app::TimelineSmpteRate::Fps2997Drop;
    result.meters[0] = {"voice", 0.75};
    result.meters[1] = {"master", 0.5};
    result.meterCount = 2;
    result.externalAccessibilityText = "External MIDI Time Code locked.";
    return result;
}

talas::app::TimelineScoreViewport viewport(
    talas::app::TimelineRulerMode mode = talas::app::TimelineRulerMode::Seconds)
{
    return {0, 120000, 0, 8, 720.0, 32.0, mode};
}

void testProjectionBuildsStableRowsAndExactClipGeometry()
{
    talas::app::TimelineScoreProjection projection(fixture(), runtime());
    require(projection.valid(), projection.diagnostic());
    require(projection.rowCount() == 6,
            "three tracks and three automation lanes should form six stable rows");
    require(projection.contentEndFrame() == 96000,
            "content extent should use the repeated clip and tempo-integrated marker end");

    const auto result = projection.query(viewport());
    require(result.state == talas::app::TimelineScoreSurfaceState::Content
                && result.rows.size() == 6,
            "a valid Score should project its complete visible row set");
    require(result.rows.front().stableId == "voice",
            "track headers should retain stable document identity");
    require(result.rows.front().meterPeak == 0.75,
            "track headers should retain exact clamped meter truth");
    require(result.rows.front().solo,
            "track headers should retain solo truth");
    require(result.rows.front().audible,
            "track headers should retain resolved audibility truth");
    const auto clip = std::ranges::find_if(
        result.items,
        [](const auto& item) {
            return item.kind == talas::app::TimelineScoreItemKind::Clip
                && item.stableId == "clipA";
        });
    require(clip != result.items.end()
                && clip->startFrame == 12000
                && clip->endFrame == 96000
                && clip->repeatCount == 2,
            "clip projection should retain the runtime compiler's exact tempo-integrated extent");
    require(std::abs(clip->fadeInFraction - (3000.0 / 84000.0)) < 1.0e-12
                && std::abs(clip->fadeOutFraction - (12000.0 / 84000.0)) < 1.0e-12,
            "fade geometry should integrate from the clip's start and exclusive end");
    require(result.playheadX.has_value()
                && std::abs(*result.playheadX - 144.0) < 1.0e-9,
            "playhead geometry should derive from the coherent runtime frame");
    require(result.externalSyncText.starts_with(
                "External MIDI Time Code locked."),
            "external sync text should derive from coherent authority and lock state rather than ruler mode");

    const auto linear = std::ranges::find_if(result.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::AutomationSegment
            && item.stableId == "clipGain";
    });
    require(linear != result.items.end()
                && linear->startFrame == 12000
                && linear->endFrame == 48000
                && linear->value == 0.0
                && linear->endValue == 1.0
                && linear->displayValue == 0.0
                && linear->displayEndValue == 1.0
                && linear->interpolation
                    == talas::core::AutomationInterpolation::Linear,
            "clip-local Linear automation should retain runtime-exact absolute frames and both endpoint values");
    const auto firstPoint = std::ranges::find_if(result.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::AutomationPoint
            && item.stableId == "clipGain"
            && item.sourceIndex == 0;
    });
    require(firstPoint != result.items.end(),
            "the first clip-local automation point should be projected");
    const auto pointHit = projection.hitTest(
        result,
        firstPoint->x + firstPoint->width * 0.5,
        firstPoint->y + firstPoint->height * 0.5);
    require(pointHit.has_value()
                && pointHit->selection.kind
                    == talas::app::TimelineScoreSelectionKind::AutomationPoint
                && pointHit->selection.entityId == "clipGain"
                && pointHit->selection.pointIndex == 0,
            "an automation point should deterministically win the shared segment boundary hit");

    const auto frequency = std::ranges::find_if(result.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::AutomationSegment
            && item.stableId == "masterFrequency";
    });
    require(frequency != result.items.end()
                && frequency->value == 440.0
                && frequency->endValue == 880.0
                && frequency->displayValue == 0.0
                && frequency->displayEndValue == 1.0
                && frequency->domain == talas::expr::ValueDomain::Hertz,
            "non-scalar automation should retain raw domain truth while projecting normalized geometry");

    // A clip longer than the view is not fitted to it; it
    // runs off the view's right edge.
    const auto narrow = projection.query({0, 48000, 0, 8, 720.0, 32.0, talas::app::TimelineRulerMode::Seconds});
    const auto longer = std::ranges::find_if(narrow.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::Clip && item.stableId == "clipA";
    });
    // It is drawn to the view's edge, its end past the view's.
    require(longer != narrow.items.end() && longer->endFrame == 96000
            && std::abs(longer->x + longer->width - 720.0) < 1.0e-9,
        "a clip longer than the view should run off its right edge");

    // The marker strip across the top, the time
    // ruler below it with the cursor's time at its left, then the rows,
    // each track's information at the left.
    const auto layout = talas::app::timelineScoreLayout(800, 400, 148, 24);
    require(layout.markers.y == 0 && layout.markers.bottom() == layout.ruler.y && layout.ruler.bottom() == layout.rows.y
            && layout.markers.x == layout.ruler.x && layout.markers.width == layout.ruler.width,
        "the strip just above the time ruler should be reserved for markers");
    require(layout.timeDisplay.right() == layout.ruler.x && layout.timeDisplay.y == layout.ruler.y
            && layout.ruler.right() == 800 && layout.ruler.bottom() <= layout.trackInformation.y,
        "the time ruler should run to the right of the time display, just above the tracks");
    require(layout.trackInformation.x == 0 && layout.trackInformation.right() == layout.rows.x
            && layout.trackInformation.y == layout.rows.y && layout.rows.bottom() == 400,
        "track information should be shown at the left of the window");
    const auto scrolling = talas::app::timelineScoreScrolling(800, 400, 16);
    require(scrolling.zoomButtons.x == scrolling.vertical.x && scrolling.zoomButtons.y == scrolling.vertical.bottom()
            && scrolling.zoomButtons.bottom() == 400 && scrolling.zoomButtons.right() == 800
            && scrolling.zoomButtons.height == 3 * 16 && scrolling.horizontal.right() <= scrolling.zoomButtons.x,
        "the three zoom buttons should sit in the lower right corner, below the vertical scroll bar");
}

void testPointerTimePublishesExactAndExplicitSnapCandidates()
{
    const auto secondsPointer = talas::app::timelineScorePointerTime(
        12500,
        talas::project::ScoreTimeDomain::Seconds,
        fixture().timeline.musicalTime,
        48000);
    require(secondsPointer.has_value()
                && secondsPointer->exact.seconds == 12500.0 / 48000.0
                && secondsPointer->snapCandidate.has_value()
                && secondsPointer->snapCandidate->seconds == 0.25
                && secondsPointer->snapFrame == 12000,
            "seconds pointer mapping should retain exact frame time and publish a separate snap candidate");

    const auto tickFrame = talas::project::musicalTickToSampleFrame(
        fixture().timeline.musicalTime, 130, 48000);
    require(tickFrame.has_value(),
            "musical pointer fixture should map its exact tick to a frame");
    const auto musicalPointer = talas::app::timelineScorePointerTime(
        *tickFrame,
        talas::project::ScoreTimeDomain::MusicalTicks,
        fixture().timeline.musicalTime,
        48000);
    require(musicalPointer.has_value()
                && musicalPointer->snapCandidate.has_value()
                && musicalPointer->snapCandidate->tick == 120
                && musicalPointer->exact.tick >= 129
                && musicalPointer->exact.tick <= 130,
            "musical pointer mapping should preserve exact conversion separately from the 120-tick snap candidate");

    const auto offGridMusicalPointer = talas::app::timelineScorePointerTime(
        *tickFrame + 1U,
        talas::project::ScoreTimeDomain::MusicalTicks,
        fixture().timeline.musicalTime,
        48000);
    require(offGridMusicalPointer.has_value()
                && offGridMusicalPointer->exact.tick == musicalPointer->exact.tick
                && offGridMusicalPointer->exactFrame == *tickFrame,
            "musical pointer mapping should canonicalize an off-grid sample frame to its exact representable tick instead of rejecting it");

    const auto maximumGridPointer = talas::app::timelineScorePointerTime(
        *tickFrame,
        talas::project::ScoreTimeDomain::MusicalTicks,
        fixture().timeline.musicalTime,
        48000,
        0.25,
        std::numeric_limits<std::uint64_t>::max());
    require(maximumGridPointer.has_value()
                && maximumGridPointer->snapCandidate.has_value()
                && maximumGridPointer->snapCandidate->tick == 0,
            "the largest musical snap grid should round without overflowing its midpoint threshold");

    require(!talas::app::timelineScorePointerTime(
                 std::numeric_limits<std::uint64_t>::max() - 1U,
                 talas::project::ScoreTimeDomain::Seconds,
                 fixture().timeline.musicalTime,
                 3205)
                 .has_value(),
            "pointer time should fail closed when binary64 round-trip reaches the unprojectable UINT64 tail");
    require(!talas::app::timelineScorePointerTime(
                 (std::uint64_t{1} << 53U) + 1U,
                 talas::project::ScoreTimeDomain::Seconds,
                 fixture().timeline.musicalTime,
                 1)
                 .has_value(),
            "pointer time should not claim exactness when binary64 cannot preserve the source frame");
    const auto exactWithoutFiniteGrid = talas::app::timelineScorePointerTime(
        1,
        talas::project::ScoreTimeDomain::Seconds,
        fixture().timeline.musicalTime,
        48000,
        std::numeric_limits<double>::denorm_min());
    require(exactWithoutFiniteGrid.has_value()
                && !exactWithoutFiniteGrid->snapCandidate.has_value(),
            "an overflowing seconds-grid quotient should preserve exact pointer truth without a snap candidate");
}

void testEveryRulerModeIsBoundedAndTruthful()
{
    talas::app::TimelineScoreProjection projection(fixture(), runtime());
    for (const auto mode : {
             talas::app::TimelineRulerMode::Seconds,
             talas::app::TimelineRulerMode::BarsBeatsTicks,
             talas::app::TimelineRulerMode::Smpte,
         })
    {
        const auto result = projection.query(viewport(mode));
        require(!result.rulerTicks.empty()
                    && result.rulerTicks.size()
                        <= talas::app::maximumTimelineRulerTicks,
                "each ruler mode should emit a nonempty fixed-bounded tick set");
        for (std::size_t index = 1; index < result.rulerTicks.size(); ++index)
            require(result.rulerTicks[index - 1].frame
                        < result.rulerTicks[index].frame,
                    "ruler ticks should remain strictly frame ordered");
    }
    const auto musical = projection.query(
        viewport(talas::app::TimelineRulerMode::BarsBeatsTicks));
    require(std::ranges::any_of(musical.rulerTicks, [](const auto& tick) {
                return tick.label.starts_with("1.2.");
            }),
            "musical ruler should project canonical bar-beat-tick labels across meter time");
    const auto smpte = projection.query(
        viewport(talas::app::TimelineRulerMode::Smpte));
    require(std::ranges::all_of(smpte.rulerTicks, [](const auto& tick) {
                return tick.label.size() == 11 && tick.label[8] == ';';
            }),
            "29.97 drop-frame ruler should use an explicit drop-frame separator");
}

void testSelectionCommandsAndHitIdentityAreExact()
{
    talas::app::TimelineScoreProjection projection(fixture(), runtime());
    const talas::app::TimelineScoreSelection selection{
        talas::app::TimelineScoreSelectionKind::Clip, "clipA", 0};
    const auto result = projection.query(viewport(), selection);
    require(!talas::app::timelineScoreHasCommand(
                result.commands,
                talas::app::TimelineScoreCommand::DeleteSelection)
                && talas::app::timelineScoreHasCommand(
                    result.commands,
                    talas::app::TimelineScoreCommand::DuplicateSelection),
            "a clip that owns automation should not advertise a deletion the edit authority rejects");

    const auto deletableClip = projection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Clip, "clipB", 0});
    require(talas::app::timelineScoreHasCommand(
                deletableClip.commands,
                talas::app::TimelineScoreCommand::DeleteSelection),
            "a dependency-free clip should expose its valid deletion command");

    const auto routedTrack = projection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Track, "music", 0});
    require(!talas::app::timelineScoreHasCommand(
                routedTrack.commands,
                talas::app::TimelineScoreCommand::DeleteSelection),
            "a routed-to submix should not advertise a deletion that would orphan its input route");
    require(!talas::app::timelineScoreHasCommand(
                routedTrack.commands,
                talas::app::TimelineScoreCommand::DuplicateSelection),
            "a track should not advertise the clip-only duplicate command");
    require(!talas::app::timelineScoreHasCommand(
                routedTrack.commands,
                talas::app::TimelineScoreCommand::AddClip),
            "a submix should not advertise the Track-only clip creation command");

    const auto voiceTrack = projection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Track, "voice", 0});
    require(talas::app::timelineScoreHasCommand(
                voiceTrack.commands,
                talas::app::TimelineScoreCommand::AddClip),
            "a selected Score Track with a Sound authority should advertise clip creation");

    auto occupiedGainDocument = fixture();
    occupiedGainDocument.timeline.score.automation.push_back({
        "voiceGain",
        talas::project::ScoreAutomationScope::Track,
        "voice",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {},
    });
    talas::app::TimelineScoreProjection occupiedGainProjection(
        occupiedGainDocument, runtime());
    const auto occupiedGain = occupiedGainProjection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Track, "voice", 0});
    require(!talas::app::timelineScoreHasCommand(
                occupiedGain.commands,
                talas::app::TimelineScoreCommand::AddAutomation),
            "a selected track that already owns exact Gain automation should not advertise another Gain lane");

    auto singleRouteDocument = fixture();
    singleRouteDocument.timeline.score.tracks.erase(
        singleRouteDocument.timeline.score.tracks.begin() + 1);
    singleRouteDocument.timeline.score.tracks.front().outputTrackId = "master";
    talas::app::TimelineScoreProjection singleRouteProjection(
        singleRouteDocument, runtime());
    const auto singleRoute = singleRouteProjection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Track, "voice", 0});
    require(talas::app::timelineScoreHasCommand(
                singleRoute.commands,
                talas::app::TimelineScoreCommand::ToggleSolo)
                && !talas::app::timelineScoreHasCommand(
                    singleRoute.commands,
                    talas::app::TimelineScoreCommand::ChangeOutput),
            "a track with no alternative valid output should retain Solo without advertising a no-op route change");
    const auto clip = std::ranges::find_if(result.items, [](const auto& item) {
        return item.stableId == "clipA"
            && item.kind == talas::app::TimelineScoreItemKind::Clip;
    });
    require(clip != result.items.end() && clip->selected,
            "selection should bind to the stable clip ID rather than viewport position");
    const auto hit = projection.hitTest(
        result,
        clip->x + 2.0,
        clip->y + clip->height * 0.5);
    require(hit.has_value()
                && hit->selection == selection
                && hit->focusIdentity == "timeline.clip:clipA",
            "hit testing should return the exact stable selection and focus identity");

    const auto firstPoint = std::ranges::find_if(result.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::AutomationPoint
            && item.stableId == "clipGain" && item.sourceIndex == 1;
    });
    require(firstPoint != result.items.end()
                && firstPoint->focusIdentity
                    == "timeline.automation-point:clipGain:tick:960",
            "automation point focus should bind to exact time rather than array index");
    auto withEarlierPoint = fixture();
    withEarlierPoint.timeline.score.automation.front().points.insert(
        withEarlierPoint.timeline.score.automation.front().points.begin() + 1,
        {{talas::project::ScoreTimeDomain::MusicalTicks, 0.0, 1},
         0.125,
         talas::core::AutomationInterpolation::Step});
    talas::app::TimelineScoreProjection shiftedProjection(
        withEarlierPoint, runtime());
    const auto shifted = shiftedProjection.query(viewport());
    require(std::ranges::any_of(shifted.items, [](const auto& item) {
                return item.kind == talas::app::TimelineScoreItemKind::AutomationPoint
                    && item.stableId == "clipGain"
                    && item.focusIdentity
                        == "timeline.automation-point:clipGain:tick:960";
            }),
            "inserting an earlier point should not rebind an existing point focus identity");

    const auto marker = std::ranges::find_if(result.items, [](const auto& item) {
        return item.kind == talas::app::TimelineScoreItemKind::Marker;
    });
    require(marker != result.items.end()
                && marker->height == result.rows.size() * 32.0,
            "a marker should retain one exact hit target across all visible Score rows");
    const auto markerHit = projection.hitTest(
        result,
        marker->x + 0.5,
        marker->height - 2.0);
    require(markerHit.has_value()
                && markerHit->selection.kind
                    == talas::app::TimelineScoreSelectionKind::Marker
                && markerHit->selection.entityId == "marker",
            "the visible marker line should remain selectable on the final visible row");

    const auto master = projection.query(
        viewport(),
        {talas::app::TimelineScoreSelectionKind::Track, "master", 0});
    require(talas::app::timelineScoreHasCommand(
                master.commands,
                talas::app::TimelineScoreCommand::ToggleMute)
                && !talas::app::timelineScoreHasCommand(
                    master.commands,
                    talas::app::TimelineScoreCommand::ChangeOutput)
                && !talas::app::timelineScoreHasCommand(
                    master.commands,
                    talas::app::TimelineScoreCommand::DeleteSelection),
            "master selection should expose only commands valid for the required master authority");

    require(projection.containsSelection({
                talas::app::TimelineScoreSelectionKind::AutomationPoint,
                "clipGain",
                1})
                && !projection.containsSelection({
                    talas::app::TimelineScoreSelectionKind::AutomationPoint,
                    "clipGain",
                    2}),
            "automation point focus authority should include the exact bounded point index");
}

void testMarkerViewportMeterRefreshAndWideRulersRemainTruthful()
{
    auto document = fixture();
    document.timeline.score.markers.insert(
        document.timeline.score.markers.begin(),
        {"long", "Long range", ticks(0), ticks(4000)});
    document.timeline.score.markers.push_back(
        {"short", "Finished before view", ticks(100), ticks(200)});
    talas::app::TimelineScoreProjection projection(document, runtime());

    auto scrolled = viewport();
    scrolled.startFrame = 60000;
    scrolled.endFrame = 90000;
    const auto result = projection.query(scrolled);
    require(std::ranges::any_of(result.items, [](const auto& item) {
                return item.kind == talas::app::TimelineScoreItemKind::Marker
                    && item.stableId == "long";
            })
                && std::ranges::none_of(result.items, [](const auto& item) {
                    return item.kind == talas::app::TimelineScoreItemKind::Marker
                        && item.stableId == "short";
                }),
            "marker viewport projection should retain only exact intersecting ranges");

    auto nextRuntime = runtime();
    nextRuntime.meters[0] = {"voice", 0.125};
    nextRuntime.meterCount = 1;
    projection.updateRuntimeState(nextRuntime);
    const auto refreshed = projection.query(viewport());
    require(refreshed.rows.front().meterPeak == 0.125,
            "runtime meter publication should refresh existing row truth");

    auto veryWide = viewport(talas::app::TimelineRulerMode::Seconds);
    veryWide.endFrame = std::uint64_t{48000} * 60U * 60U * 24U * 365U * 10U;
    const auto secondsResult = projection.query(veryWide);
    require(secondsResult.rulerTicks.size()
                <= talas::app::maximumTimelineRulerTicks
                && !secondsResult.rulerTicks.empty()
                && secondsResult.rulerTicks.back().x > 0.5 * veryWide.pixelWidth,
            "wide seconds rulers should choose a coarse bounded interval spanning the viewport");
    veryWide.rulerMode = talas::app::TimelineRulerMode::BarsBeatsTicks;
    const auto musicalResult = projection.query(veryWide);
    require(musicalResult.rulerTicks.size()
                <= talas::app::maximumTimelineRulerTicks
                && !musicalResult.rulerTicks.empty()
                && musicalResult.rulerTicks.back().x > 0.5 * veryWide.pixelWidth,
            "wide musical rulers should choose a coarse bounded interval spanning the viewport");

    talas::project::ProjectDocument markerOnly;
    markerOnly.timeline.score.markers = {
        {"only", "Marker only", seconds(9.0), std::nullopt},
    };
    talas::app::TimelineScoreProjection markerProjection(markerOnly, runtime());
    auto tailViewport = viewport();
    tailViewport.endFrame = markerProjection.contentEndFrame();
    tailViewport.startFrame = tailViewport.endFrame - 8U * 48000U;
    const auto markerResult = markerProjection.query(tailViewport);
    require(markerResult.state == talas::app::TimelineScoreSurfaceState::Content
                && markerProjection.contentEndFrame() == 9U * 48000U + 1U
                && std::ranges::any_of(markerResult.items, [](const auto& item) {
                    return item.kind == talas::app::TimelineScoreItemKind::Marker
                        && item.stableId == "only";
                }),
            "a marker-only valid Score should remain visible content");
}

void testInvalidScoreAndViewportFailClosed()
{
    auto invalid = fixture();
    invalid.timeline.score.tracks[0].outputTrackId = "missing";
    talas::app::TimelineScoreProjection projection(invalid, runtime());
    require(!projection.valid(),
            "invalid routing should fail before any Score surface is published");
    const auto invalidScore = projection.query(viewport());
    require(invalidScore.state == talas::app::TimelineScoreSurfaceState::Error
                && invalidScore.rows.empty()
                && !invalidScore.diagnostic.empty(),
            "invalid Score state should be explicit and contain no plausible content");

    talas::app::TimelineScoreProjection valid(fixture(), runtime());
    auto oversized = viewport();
    oversized.rowCount = talas::app::maximumTimelineViewportRows + 1;
    const auto invalidViewport = valid.query(oversized);
    require(invalidViewport.state == talas::app::TimelineScoreSurfaceState::Error
                && invalidViewport.items.empty(),
            "oversized viewport requests should fail closed before projection work");
    auto extremePixels = viewport();
    extremePixels.pixelWidth = std::numeric_limits<double>::max();
    const auto invalidPixels = valid.query(extremePixels);
    require(invalidPixels.state == talas::app::TimelineScoreSurfaceState::Error,
            "an unrepresentable viewport width should fail before integer projection casts");
    auto extremeRows = viewport();
    extremeRows.rowHeight = std::numeric_limits<double>::max();
    const auto invalidRows = valid.query(extremeRows);
    require(invalidRows.state == talas::app::TimelineScoreSurfaceState::Error,
            "an unrepresentable row height should fail before paint geometry projection");

    auto invalidMeters = runtime();
    invalidMeters.meterCount = invalidMeters.meters.size() + 1;
    talas::app::TimelineScoreProjection invalidMeterProjection(
        fixture(), invalidMeters);
    require(!invalidMeterProjection.valid(),
            "meter telemetry count should fail before exceeding its fixed storage authority");

    constexpr auto maximumFrameSecondsBits = std::uint64_t{0x433472b36651f3b3};
    const auto maximumFrameSeconds = std::bit_cast<double>(
        maximumFrameSecondsBits);
    auto maximumFrameRuntime = runtime();
    maximumFrameRuntime.sampleRate = 3205;
    talas::project::ProjectDocument maximumMarker;
    maximumMarker.timeline.score.markers = {
        {"maximum", "Maximum frame marker", seconds(maximumFrameSeconds),
         std::nullopt},
    };
    talas::app::TimelineScoreProjection maximumMarkerProjection(
        maximumMarker, maximumFrameRuntime);
    require(!maximumMarkerProjection.valid(),
            "an instant marker at UINT64_MAX should fail closed instead of publishing invisible zero-width content");

    auto maximumPoint = fixture();
    maximumPoint.timeline.score.automation[1].points[0].position =
        seconds(maximumFrameSeconds);
    talas::app::TimelineScoreProjection maximumPointProjection(
        maximumPoint, maximumFrameRuntime);
    require(!maximumPointProjection.valid(),
            "an automation point at UINT64_MAX should fail closed instead of publishing invisible zero-width content");
}

void testLargeScoreViewportQueryTouchesOnlyOverlappingCandidates()
{
    auto document = fixture();
    document.timeline.score.clips.clear();
    document.timeline.score.automation.clear();
    document.timeline.score.markers.clear();
    for (std::size_t index = 0;
         index < talas::project::maximumScoreClips;
         ++index)
    {
        const auto start = static_cast<double>(index) * 0.25;
        document.timeline.score.clips.push_back({
            "clip" + std::to_string(index),
            "voice",
            "voiceDefinition",
            seconds(start),
            seconds(0.1),
            seconds(0.0),
            seconds(0.0),
            seconds(0.0),
            1,
        });
    }
    document.timeline.score.tracks[0].timeDomain =
        talas::project::ScoreTimeDomain::Seconds;
    talas::app::TimelineScoreProjection projection(document, runtime());
    require(projection.valid(), projection.diagnostic());
    constexpr std::uint64_t start = 48000ULL * 900ULL;
    const auto result = projection.query({
        start,
        start + 48000,
        0,
        1,
        1000.0,
        32.0,
        talas::app::TimelineRulerMode::Seconds,
    });
    require(result.items.size() <= 5 && result.candidateCount <= 5,
            "a one-second viewport near the end of a 4096-clip score should not scan total Score content");
    require(!result.truncated,
            "ordinary large-score viewport density should fit the fixed output bound");
}
}

// A Live lane shows a green dot beside its
// name, a Record-enabled lane a red one; an Automated lane neither.
void testLaneRowsShowTheirState()
{
    auto document = fixture();
    auto& score = document.timeline.score;
    score.automation.front().state = talas::project::ScoreAutomationState::Live;
    score.automation.back().state = talas::project::ScoreAutomationState::Record;
    talas::app::TimelineScoreProjection projection(document, runtime());
    require(projection.valid(), projection.diagnostic());
    const auto result = projection.query(viewport());
    const auto row = [&result](const std::string& id) {
        const auto found = std::ranges::find(result.rows, id, &talas::app::TimelineScoreRowProjection::stableId);
        require(found != result.rows.end(), "the lane " + id + " should have a row");
        return *found;
    };
    const auto live = row(score.automation.front().laneId);
    const auto record = row(score.automation.back().laneId);
    require(live.automationState == talas::project::ScoreAutomationState::Live && live.accessibilityText.ends_with(", live"),
        "a Live lane's row should show it expects live control");
    require(record.automationState == talas::project::ScoreAutomationState::Record,
        "a Record-enabled lane's row should show it is armed");
    require(row(score.automation[1].laneId).automationState == talas::project::ScoreAutomationState::Automated,
        "an Automated lane shows no dot");
}

// Dragging a bar's right edge goes no further by
// hand than the view's edge, and a bar dragged as far left as it goes
// starts at time 0.
void testDragsStopAtTheViewsEdgeAndTimeZero()
{
    require(talas::app::timelineScoreFrameAtPointer(500.0, 400.0, 48000, 96000) == std::optional<std::uint64_t>{96000}
            && talas::app::timelineScoreFrameAtPointer(-30.0, 400.0, 48000, 96000) == std::optional<std::uint64_t>{48000}
            && talas::app::timelineScoreFrameAtPointer(200.0, 400.0, 48000, 96000) == std::optional<std::uint64_t>{72000},
        "the pointer's time should stay within the view, so an edge goes no further than the view's edge");
    require(!talas::app::timelineScoreFrameAtPointer(10.0, 0.0, 0, 100).has_value()
            && !talas::app::timelineScoreFrameAtPointer(10.0, 100.0, 100, 100).has_value(),
        "no time under a view of no width or no frames");
    require(talas::app::timelineScoreDraggedFrame(1000, 5000, 1000) == 0 && talas::app::timelineScoreDraggedFrame(1000, 5000, 4500) == 500
            && talas::app::timelineScoreDraggedFrame(1000, 5000, 6000) == 2000,
        "a bar dragged as far left as it goes should start at time 0");
}

int main()
{
    testProjectionBuildsStableRowsAndExactClipGeometry();
    testPointerTimePublishesExactAndExplicitSnapCandidates();
    testEveryRulerModeIsBoundedAndTruthful();
    testSelectionCommandsAndHitIdentityAreExact();
    testMarkerViewportMeterRefreshAndWideRulersRemainTruthful();
    testInvalidScoreAndViewportFailClosed();
    testLaneRowsShowTheirState();
    testLargeScoreViewportQueryTouchesOnlyOverlappingCandidates();
    testDragsStopAtTheViewsEdgeAndTimeZero();
    return 0;
}
