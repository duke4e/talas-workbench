#include "talas/app/TimelineScoreProjection.h"
#include "talas/audio/ProjectWavExporter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectScore.h"
#include "talas/runtime/PreparedScore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr std::size_t largeTrackCount = 128;
constexpr std::size_t largeClipCount = 2048;
constexpr std::size_t largeLaneCount = 128;
constexpr std::size_t pointsPerLane = 32;
constexpr std::size_t largeMarkerCount = 1024;
// Offline export carries an exact render horizon, so the complete accepted
// 2,048-clip authority is retained while only clips that can reach the output
// interval are materialized and projected.
constexpr std::size_t offlineClipCount = largeClipCount;
constexpr std::uint64_t projectionBudgetNs = 500'000'000;
constexpr std::uint64_t viewportP95BudgetNs = 5'000'000;
constexpr std::uint64_t editBudgetNs = 750'000'000;
constexpr std::uint64_t preparationBudgetNs = 500'000'000;
constexpr std::uint64_t callbackP99BudgetNs = 5'333'334;
constexpr std::uint64_t offlineRenderBudgetNs = 10'000'000'000;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

std::uint64_t elapsedNs(Clock::time_point started)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started)
            .count());
}

std::uint64_t nearestRank(
    std::vector<std::uint64_t> values,
    std::size_t numerator,
    std::size_t denominator)
{
    std::ranges::sort(values);
    const auto rank = (values.size() * numerator + denominator - 1U)
        / denominator;
    return values[std::max<std::size_t>(1, rank) - 1U];
}

talas::project::ProjectSoundDefinition voiceDefinition()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Large Score Voice";
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    oscillator.parameters.push_back({"frequency", "220 hz"});
    oscillator.parameters.push_back({"amplitude", "!Gain"});
    definition.graph.nodes.push_back(std::move(oscillator));
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = {"gain", "0.1"};
    gain.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "Gain",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(gain));
    return definition;
}

talas::project::ProjectDocument largeScoreDocument(
    std::size_t trackCount,
    std::size_t clipCount,
    std::size_t laneCount,
    std::size_t pointCount,
    std::size_t markerCount)
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(voiceDefinition());
    auto& score = document.timeline.score;
    score.tracks.reserve(trackCount + 1U);
    score.clips.reserve(clipCount);
    score.automation.reserve(laneCount);
    score.markers.reserve(markerCount);
    for (std::size_t index = 0; index < trackCount; ++index)
    {
        score.tracks.push_back({
            "track" + std::to_string(index),
            "Track " + std::to_string(index),
            talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::Seconds,
            "master",
            1,
            talas::project::ScoreOverlapMode::Layer,
        });
    }
    score.tracks.push_back({
        "master",
        "Master",
        talas::project::ScoreTrackKind::Master,
        talas::project::ScoreTimeDomain::Seconds,
        {},
        1,
        talas::project::ScoreOverlapMode::Layer,
    });
    for (std::size_t index = 0; index < clipCount; ++index)
    {
        score.clips.push_back({
            "clip" + std::to_string(index),
            "track" + std::to_string(index % trackCount),
            "voice",
            seconds(static_cast<double>(index) * 0.125),
            seconds(0.1),
            seconds(0.0),
            seconds(0.005),
            seconds(0.005),
            1,
        });
    }
    for (std::size_t laneIndex = 0; laneIndex < laneCount; ++laneIndex)
    {
        talas::project::ScoreAutomationLane lane;
        lane.laneId = "lane" + std::to_string(laneIndex);
        lane.scope = talas::project::ScoreAutomationScope::Track;
        lane.ownerId = "track" + std::to_string(laneIndex % trackCount);
        lane.eventName = "Gain";
        lane.domain = talas::expr::ValueDomain::Scalar;
        lane.points.reserve(pointCount);
        for (std::size_t point = 0; point < pointCount; ++point)
        {
            lane.points.push_back({
                seconds(static_cast<double>(point) * 8.0),
                static_cast<double>(point % 8U) / 7.0,
                point % 2U == 0
                    ? talas::core::AutomationInterpolation::Step
                    : talas::core::AutomationInterpolation::Linear,
            });
        }
        score.automation.push_back(std::move(lane));
    }
    for (std::size_t index = 0; index < markerCount; ++index)
    {
        score.markers.push_back({
            "marker" + std::to_string(index),
            "Marker " + std::to_string(index),
            seconds(static_cast<double>(index) * 0.25),
            {},
        });
    }
    return document;
}

talas::app::TimelineScoreRuntimeState runtimeState()
{
    talas::app::TimelineScoreRuntimeState runtime;
    runtime.stageId = 7;
    runtime.sampleRate = 48000;
    runtime.publicationValid = true;
    return runtime;
}

void testLargeScoreBudgets()
{
    auto document = largeScoreDocument(
        largeTrackCount,
        largeClipCount,
        largeLaneCount,
        pointsPerLane,
        largeMarkerCount);
    const std::array<std::string_view, 1> definitions{"voice"};
    require(
        !talas::project::projectScoreValidationError(
            document.timeline.score, definitions)
             .has_value(),
        "large Score fixture must satisfy the product authority");

    const auto projectionStarted = Clock::now();
    talas::app::TimelineScoreProjection projection(document, runtimeState());
    const auto projectionNs = elapsedNs(projectionStarted);
    require(projection.valid(), std::string(projection.diagnostic()));
    require(projectionNs <= projectionBudgetNs,
        "large Score projection exceeded its 500 ms Debug budget");

    const talas::app::TimelineScoreViewport viewport{
        0,
        48000ULL,
        0,
        24,
        1200.0,
        32.0,
        talas::app::TimelineRulerMode::Seconds,
    };
    std::vector<std::uint64_t> viewportSamples;
    viewportSamples.reserve(64);
    std::size_t maximumCandidates = 0;
    bool observedClip = false;
    bool observedAutomation = false;
    bool observedMarker = false;
    for (std::size_t sample = 0; sample < 68; ++sample)
    {
        const auto started = Clock::now();
        const auto result = projection.query(viewport);
        const auto duration = elapsedNs(started);
        require(result.state == talas::app::TimelineScoreSurfaceState::Content
                && !result.truncated,
            "large Score viewport must publish complete content");
        maximumCandidates = std::max(maximumCandidates, result.candidateCount);
        for (const auto& item : result.items)
        {
            observedClip = observedClip
                || item.kind == talas::app::TimelineScoreItemKind::Clip;
            observedAutomation = observedAutomation
                || item.kind
                    == talas::app::TimelineScoreItemKind::AutomationPoint
                || item.kind
                    == talas::app::TimelineScoreItemKind::AutomationSegment;
            observedMarker = observedMarker
                || item.kind == talas::app::TimelineScoreItemKind::Marker;
        }
        if (sample >= 4)
            viewportSamples.push_back(duration);
    }
    const auto viewportP95 = nearestRank(viewportSamples, 95, 100);
    require(maximumCandidates > 0
            && observedClip
            && observedAutomation
            && observedMarker,
        "large Score viewport did not exercise visible clip, automation, and marker candidates");
    require(maximumCandidates <= 128,
        "large Score viewport touched too many offscreen candidates");
    require(viewportP95 <= viewportP95BudgetNs,
        "large Score viewport exceeded its 5 ms p95 Debug budget");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(document, registry);
    auto replacement = document.timeline.score.clips[largeClipCount / 2U];
    replacement.start.seconds += 0.001;
    const auto editStarted = Clock::now();
    const auto edited = editor.updateScoreClip(std::move(replacement));
    const auto editNs = elapsedNs(editStarted);
    require(edited.ok() && editor.canUndo(),
        "one large Score edit must remain one atomic undo transaction");
    require(editNs <= editBudgetNs,
        "large Score edit exceeded its 750 ms Debug budget");
    require(editor.undo().ok()
            && editor.document().timeline.score == document.timeline.score
            && !editor.canUndo(),
        "large Score edit must restore the exact prior authority in one undo");

    const auto preparationStarted = Clock::now();
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 11, 13, 48000, 17));
    const auto preparationNs = elapsedNs(preparationStarted);
    require(prepared.ok(), prepared.error);
    require(preparationNs <= preparationBudgetNs,
        "large Score preparation exceeded its 500 ms Debug budget");

    talas::runtime::PreparedScorePlaybackCursor cursor(prepared.schedule);
    require(cursor.ready() && cursor.relocate(0),
        "large Score callback cursor must start from exact frame zero");
    std::vector<std::uint64_t> callbackSamples;
    callbackSamples.reserve(4096);
    double checksum = 0.0;
    for (std::uint64_t block = 1; block <= 4096; ++block)
    {
        const auto frame = block * 256U;
        const auto started = Clock::now();
        require(cursor.advanceTo(frame),
            "large Score cursor advance must remain valid");
        const auto value = cursor.automationValue(0, frame);
        callbackSamples.push_back(elapsedNs(started));
        checksum += value.value_or(0.0)
            + static_cast<double>(cursor.eventCursor());
    }
    const auto callbackP99 = nearestRank(callbackSamples, 99, 100);
    require(callbackP99 <= callbackP99BudgetNs,
        "large Score callback exceeded one 256-frame realtime deadline");
    require(checksum > 0.0,
        "large Score callback measurement did not consume fresh schedule state");

    const auto offlineDocument = largeScoreDocument(
        largeTrackCount,
        offlineClipCount,
        largeLaneCount,
        pointsPerLane,
        largeMarkerCount);
    require(offlineDocument.timeline.score.tracks.size()
                == largeTrackCount + 1U
            && offlineDocument.timeline.score.clips.size()
                == offlineClipCount
            && offlineDocument.timeline.score.automation.size()
                == largeLaneCount
            && offlineDocument.timeline.score.markers.size()
                == largeMarkerCount,
        "offline budget fixture lost its large Score cardinalities");
    const auto directory = std::filesystem::temp_directory_path()
        / ("talas-e9-score-budget-"
            + std::to_string(Clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(directory),
        "offline budget fixture could not create a private directory");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = directory / "large-score.wav";
    options.sampleRate = 48000.0;
    options.durationSeconds = 0.02;
    options.maxBlockFrames = 256;
    const auto offlineStarted = Clock::now();
    const auto rendered = talas::audio::exportProjectToPcm16Wav(
        offlineDocument,
        registry,
        std::move(options));
    const auto offlineNs = elapsedNs(offlineStarted);
    for (const auto& diagnostic : rendered.diagnostics)
        std::cerr << "offline diagnostic: " << diagnostic.message << '\n';
    require(rendered.ok(),
        "large Score offline render failed: " + rendered.error);
    require(rendered.frameCount == 960
            && std::filesystem::file_size(directory / "large-score.wav") > 44,
        "large Score offline render did not publish fresh audio bytes");
    require(offlineNs <= offlineRenderBudgetNs,
        "large Score offline render exceeded its 10 s Debug budget: "
            + std::to_string(offlineNs) + " ns");
    std::filesystem::remove(directory / "large-score.wav");
    std::filesystem::remove(directory);

    std::cout
        << "E9 large Score budgets (ns): projection=" << projectionNs
        << " viewport-p95=" << viewportP95
        << " edit=" << editNs
        << " prepare=" << preparationNs
        << " callback-p99=" << callbackP99
        << " offline=" << offlineNs
        << " offline-clips=" << offlineClipCount
        << " candidates=" << maximumCandidates << '\n';
}
}

int main()
{
    testLargeScoreBudgets();
    return 0;
}
