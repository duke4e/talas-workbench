#include "talas/app/PartialTrackEditorModel.h"
#include "talas/app/PartialTrackFilters.h"
#include "talas/app/PartialTrackSpectrumEditing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

talas::project::PartialTrackAsset sourceFixture()
{
    return {
        48000.0,
        8192,
        {
            {1, 0, {{1, 0.5, 440.0, 10.0},
                    {2, 0.25, 880.0, 20.0}}},
            {1, 1024, {{1, 0.4, 445.0, 11.0},
                       {2, 0.2, 875.0, 19.0}}},
            {1, 4096, {{1, 0.3, 450.0, 12.0},
                       {3, 0.1, 660.0, 8.0}}},
        },
    };
}

talas::project::ProjectAsset manifestFixture(
    const talas::project::PartialTrackAsset& source,
    std::uint64_t generation = 7)
{
    talas::project::ProjectAsset manifest;
    manifest.id = "Analysis/Lead";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "analysis/lead.talaspartials";
    manifest.contentHash.assign(64, 'a');
    manifest.sampleRate = source.sampleRate;
    manifest.frameCount = source.sourceFrameCount;
    manifest.encoding = std::string{talas::project::partialTrackAssetEncoding};
    manifest.generation = generation;
    manifest.provenance = "deterministic-analysis";
    return manifest;
}

talas::project::ProjectPartialTrackEditSet editFixture(
    const talas::project::ProjectAsset& manifest)
{
    talas::project::ProjectPartialTrackEditSet edits;
    edits.editSetId = "lead_editor";
    edits.assetId = manifest.id;
    edits.assetGeneration = manifest.generation;
    edits.sourceContentHash = manifest.contentHash;
    edits.pointOverrides = {
        {{1, 0, 1},
         256,
         {1, 0.8, 455.0, 12.0},
         talas::project::PartialTrackPointInterpolation::Step,
         false},
    };
    edits.insertedPoints = {
        {"inserted_overtone",
         1,
         2048,
         {3, 0.15, 675.0, 9.0},
         talas::project::PartialTrackPointInterpolation::Linear},
    };
    edits.trackStates = {{2, true, false}, {3, false, true}};
    return edits;
}

talas::app::PartialTrackEditorViewport viewport()
{
    return {0, 5000, 400.0, 1000.0, 1000, 600, 0, 8};
}

std::shared_ptr<const talas::project::PartialTrackAsset> sharedSource(
    talas::project::PartialTrackAsset source)
{
    return std::make_shared<const talas::project::PartialTrackAsset>(
        std::move(source));
}

void testProjectionRetainsExactAuthorityAndIdentity()
{
    auto source = sourceFixture();
    const auto manifest = manifestFixture(source);
    const auto edits = editFixture(manifest);
    talas::app::PartialTrackEditorModel model;
    require(model.state() == talas::app::PartialTrackEditorState::Loading,
            "new partial-track editor model should begin in Loading state");
    require(model.setSource({manifest, sharedSource(source), edits}),
            "valid exact partial-track source should prepare");
    require(model.state() == talas::app::PartialTrackEditorState::Ready
                && model.indexedPointCount() == 7
                && model.indexedSegmentCount() == 4
                && model.maximumRenderedSegmentTrackCount() == 1,
            "prepared projection should index every point and same-track segment once");

    const auto initial = model.project(viewport());
    require(initial.state == talas::app::PartialTrackEditorState::Ready
                && initial.rows.size() == 3
                && !initial.points.empty()
                && !initial.segments.empty(),
            "visible exact source should project rows, points, and trajectories");
    require(initial.sourceAuthorityText.find("generation 7")
                    != std::string::npos
                && initial.sourceAuthorityText.find(
                       manifest.contentHash.substr(0, 12))
                    != std::string::npos
                && initial.qualityText.find("linear/step")
                    != std::string::npos
                && initial.qualityText.find("synthesis on")
                    != std::string::npos
                && initial.latencyText.find("0 frames")
                    != std::string::npos,
            "projection should expose exact source generation, quality, and latency truth");
    require(initial.timeTicks.size() >= 2
                && initial.frequencyTicks.size() >= 2
                && initial.timeTicks.size()
                    <= talas::app::maximumPartialTrackEditorRulerTicks,
            "time/frequency rulers should be bounded and nonempty");

    const auto moved = std::ranges::find_if(
        initial.points,
        [](const auto& point) {
            return point.identity.origin
                    == talas::project::MaterializedPartialTrackPointOrigin::Source
                && point.identity.source
                    == talas::project::PartialTrackSourcePointIdentity{1, 0, 1};
        });
    require(moved != initial.points.end()
                && moved->sourceFrame == 256
                && moved->interpolation
                    == talas::project::PartialTrackPointInterpolation::Step,
            "moved source point should retain immutable source identity and edited output geometry");
    const auto inserted = std::ranges::find_if(
        initial.points,
        [](const auto& point) {
            return point.identity.origin
                    == talas::project::MaterializedPartialTrackPointOrigin::Inserted
                && point.identity.insertedEditId == "inserted_overtone";
        });
    require(inserted != initial.points.end(),
            "inserted point should expose its stable edit identity");

    require(model.setSelection({
                talas::app::PartialTrackEditorItemKind::Point,
                inserted->trackId,
                inserted->identity}),
            "exact inserted point identity should be selectable");
    const auto selectedPoint = model.project(viewport());
    require(selectedPoint.selectionRetained
                && std::ranges::count_if(
                       selectedPoint.points,
                       [](const auto& point) { return point.selected; }) == 1
                && (selectedPoint.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::MovePoint)) != 0
                && (selectedPoint.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::MuteTrack)) == 0,
            "point selection should retain exact identity and point-only command authority");
    require((selectedPoint.commandMask
                & talas::app::partialTrackEditorCommandBit(
                    talas::app::PartialTrackEditorCommand::RetrackPoint)) != 0
                && (selectedPoint.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::InsertPoint)) != 0,
            "point selection should expose exact retrack and global insert command authority");
    auto retrackedEdits = edits;
    retrackedEdits.insertedPoints.front().point.trackId = 4;
    require(model.setSource(
                {manifest, sharedSource(source), retrackedEdits}),
            "retracked stable point fixture should prepare");
    const auto retrackedSelection = model.project(viewport());
    require(retrackedSelection.selectionRetained
                && std::ranges::any_of(
                    retrackedSelection.points,
                    [&](const auto& point) {
                        return point.selected
                            && point.identity == inserted->identity
                            && point.trackId == 4;
                    }),
            "stable point focus should follow exact identity across retracking instead of binding to the old row");
    auto offscreenViewport = viewport();
    offscreenViewport.endFrame = 1024;
    const auto offscreenSelection = model.project(offscreenViewport);
    require(offscreenSelection.selectionRetained
                && (offscreenSelection.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::MovePoint)) == 0,
            "offscreen stable selection should remain restorable while edit command authority fails closed");
    require(model.setSelection({
                talas::app::PartialTrackEditorItemKind::Track,
                3,
                {}}),
            "track row should be selectable by stable track identity");
    const auto selectedTrack = model.project(viewport());
    require((selectedTrack.commandMask
                & talas::app::partialTrackEditorCommandBit(
                    talas::app::PartialTrackEditorCommand::MuteTrack)) != 0
                && (selectedTrack.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::DeletePoint)) == 0,
            "track selection should expose track-only command authority");
    require((selectedTrack.commandMask
                & talas::app::partialTrackEditorCommandBit(
                    talas::app::PartialTrackEditorCommand::TransformVisibleRegion)) != 0
                && (selectedTrack.commandMask
                    & talas::app::partialTrackEditorCommandBit(
                        talas::app::PartialTrackEditorCommand::SplitVisibleRegion)) != 0,
            "track selection should expose bounded visible-region transform authority");
}

void testFiltersAreWorkspaceLocalAndDoNotRebuildSource()
{
    auto source = sourceFixture();
    const auto manifest = manifestFixture(source);
    talas::app::PartialTrackEditorModel model;
    require(model.setSource(
                {manifest, sharedSource(source), editFixture(manifest)}),
            "filter fixture should prepare");
    const auto pointCount = model.indexedPointCount();
    const auto segmentCount = model.indexedSegmentCount();
    const auto initialAuthority = model.project(viewport()).sourceAuthorityText;

    require(!model.setFilter({{3, 1}, 0.0, true, false}),
            "unsorted filter identities should fail closed");
    require(model.setFilter({{3}, 0.12, true, true}),
            "valid workspace-local track/amplitude/solo filter should apply");
    const auto filtered = model.project(viewport());
    require(filtered.rows.size() == 1 && filtered.rows.front().trackId == 3
                && filtered.points.size() == 1
                && filtered.points.front().amplitude >= 0.12,
            "filter should project only the selected solo track and amplitude range");
    require(model.indexedPointCount() == pointCount
                && model.indexedSegmentCount() == segmentCount
                && filtered.sourceAuthorityText == initialAuthority,
            "pure filter/view changes must not decode, prepare, or alter source authority");

    require(model.setFilter({{}, 0.0, false, false}),
            "mute exclusion filter should apply");
    const auto withoutMuted = model.project(viewport());
    require(std::ranges::none_of(
                withoutMuted.rows,
                [](const auto& row) { return row.trackId == 2; }),
            "workspace filter should omit muted tracks without changing project state");
}

void testStatesAndBoundsFailClosed()
{
    auto source = sourceFixture();
    auto manifest = manifestFixture(source);
    talas::app::PartialTrackEditorModel model;
    auto stale = editFixture(manifest);
    ++stale.assetGeneration;
    require(!model.setSource({manifest, sharedSource(source), stale})
                && model.state() == talas::app::PartialTrackEditorState::Error,
            "stale edit/source authority should project Error state");

    talas::project::PartialTrackAsset empty{
        48000.0, 1, {{1, 0, {}}}};
    auto emptyManifest = manifestFixture(empty, 8);
    require(model.setSource(
                {emptyManifest, sharedSource(empty), std::nullopt})
                && model.state() == talas::app::PartialTrackEditorState::Empty
                && model.project({0, 1, 20.0, 20000.0, 320, 200, 0, 8})
                       .state == talas::app::PartialTrackEditorState::Empty,
            "valid source with no points should project a truthful Empty state");

    require(model.setSource(
                {manifest, sharedSource(source), editFixture(manifest)}),
            "valid source should recover from prior Error/Empty states");
    auto invalidViewport = viewport();
    invalidViewport.endFrame = invalidViewport.startFrame;
    require(model.project(invalidViewport).state
                == talas::app::PartialTrackEditorState::Error,
            "invalid viewport geometry should fail closed");
    model.setLoading();
    require(model.state() == talas::app::PartialTrackEditorState::Loading
                && model.indexedPointCount() == 0,
            "loading transition should revoke prepared source projection");

    talas::project::PartialTrackAsset discontinuous{
        48000.0,
        2048,
        {{1, 0, {{1, 0.5, 440.0, 1.0}}},
         {2, 1024, {{1, 0.5, 440.0, 1.0}}}}};
    const auto discontinuousManifest = manifestFixture(discontinuous, 10);
    require(model.setSource(
                {discontinuousManifest,
                 sharedSource(discontinuous),
                 std::nullopt})
                && model.indexedSegmentCount() == 0,
            "trajectory geometry must not connect the same track across source segment discontinuities");

    talas::project::PartialTrackAsset capped{
        48000.0, 256, {{1, 0, {}}}};
    for (std::uint64_t trackId = 1; trackId <= 129; ++trackId)
    {
        capped.frames.front().points.push_back(
            {trackId, 0.1, 100.0 + static_cast<double>(trackId), 0.0});
    }
    const auto cappedManifest = manifestFixture(capped, 11);
    talas::project::ProjectPartialTrackEditSet cappedEdits;
    cappedEdits.editSetId = "capped_preview";
    cappedEdits.assetId = cappedManifest.id;
    cappedEdits.assetGeneration = cappedManifest.generation;
    cappedEdits.sourceContentHash = cappedManifest.contentHash;
    cappedEdits.trackStates = {{129, true, false}};
    require(model.setSource(
                {cappedManifest,
                 sharedSource(std::move(capped)),
                 cappedEdits})
                && model.maximumRenderedSegmentTrackCount() == 128,
            "operation authority must count the rendered mute/solo set rather than all materialized tracks");
}

void testLargeViewportQueryIsCandidateBounded()
{
    talas::project::PartialTrackAsset source;
    source.sampleRate = 48000.0;
    source.sourceFrameCount = 200000;
    source.frames.reserve(1000);
    for (std::uint64_t index = 0; index < 1000; ++index)
    {
        source.frames.push_back({
            1,
            index * 128,
            {{index + 1, 0.5, 100.0 + static_cast<double>(index), 1.0}},
        });
    }
    const auto manifest = manifestFixture(source, 9);
    talas::app::PartialTrackEditorModel model;
    require(model.setSource(
                {manifest, sharedSource(std::move(source)), std::nullopt})
                && model.indexedPointCount() == 1000,
            "large exact fixture should prepare every stable track point");
    const talas::app::PartialTrackEditorViewport small{
        0, 4096, 90.0, 180.0, 960, 640, 0, 64};
    const auto projection = model.project(small);
    require(!projection.points.empty()
                && projection.viewportCandidateCount < 160
                && projection.viewportCandidateCount
                    < model.indexedPointCount(),
            "small viewport should use bounded spatial candidates instead of scanning all points");
}
}

// Ten frames 100 samples apart of tracks 1, 2 and 4 at 1 kHz; track 1's
// fourth amplitude stands out, track 2 glides from 100 Hz.
talas::project::PartialTrackAsset filterFixture()
{
    talas::project::PartialTrackAsset source{1000.0, 2000, {}};
    for (std::uint64_t index = 0; index < 10; ++index)
    {
        const auto amplitude = index == 3 ? 0.9 : 0.1 + 0.02 * static_cast<double>(index);
        source.frames.push_back({1, index * 100,
            {{1, amplitude, 50.0 + static_cast<double>(index), 1.0},
             {2, 0.2, 100.0 + 2.0 * static_cast<double>(index), 1.0},
             {4, 0.05, 410.0 + (index == 6 ? 40.0 : 0.0), 1.0}}});
    }
    return source;
}

talas::project::ProjectPartialTrackEditSet emptyEdits(const talas::project::ProjectAsset& manifest)
{
    return {"filters", manifest.id, manifest.generation, manifest.contentHash, {}, {}, {}};
}

// Each track's (frame, amplitude, frequency) after the edits.
std::vector<std::tuple<std::uint64_t, double, double>> trackValues(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    std::uint64_t trackId)
{
    std::vector<std::tuple<std::uint64_t, double, double>> values;
    for (const auto& frame : materialized.asset.frames)
    {
        for (const auto& point : frame.points)
        {
            if (point.trackId == trackId)
                values.emplace_back(frame.sourceFrame, point.amplitude, point.frequencyHz);
        }
    }
    return values;
}

talas::project::MaterializedPartialTrackEdits filtered(
    talas::app::PartialTrackFilter filter,
    talas::app::PartialTrackFilterRegion region,
    talas::app::PartialTrackFilterSettings settings = {})
{
    const auto source = filterFixture();
    const auto manifest = manifestFixture(source);
    const auto edits = emptyEdits(manifest);
    const auto before = talas::project::materializeProjectPartialTrackEdits(source, edits);
    require(before.ok(), "the fixture should materialize");
    auto result = talas::app::filterPartialTracks(before.materialized, region, filter, settings);
    require(result.ok(), "the filter should apply: " + result.error);
    talas::project::ProjectPartialTrackEditBatch batch{edits.editSetId, edits.assetId, edits.assetGeneration,
        edits.sourceContentHash, std::move(result.mutations), {}};
    const auto applied = talas::project::applyProjectPartialTrackEditBatch(source, &edits, batch);
    require(applied.ok(), "the filtered points should apply");
    const auto after = talas::project::materializeProjectPartialTrackEdits(source, applied.edits);
    require(after.ok(), "the filtered edits should materialize");
    return after.materialized;
}

bool near(double left, double right)
{
    return std::abs(left - right) <= 1.0e-9;
}

void testTrackFiltersAsTheManualDescribesThem()
{
    using Filter = talas::app::PartialTrackFilter;
    const talas::app::PartialTrackFilterRegion trackOne{{1}};
    const auto original = trackValues(filtered(Filter::ScaleAmplitudes, trackOne), 1);

    // Scale amplitudes scales the selected tracks by the amount.
    talas::app::PartialTrackFilterSettings half;
    half.scale = 0.5;
    const auto scaled = filtered(Filter::ScaleAmplitudes, trackOne, half);
    const auto source = filterFixture();
    for (std::size_t index = 0; index < 10; ++index)
    {
        require(near(std::get<1>(trackValues(scaled, 1)[index]), source.frames[index].points[0].amplitude * 0.5),
            "Scale amplitudes should scale each amplitude by the amount");
    }
    require(trackValues(scaled, 2) == trackValues(filtered(Filter::ScaleAmplitudes, trackOne), 2),
        "a filter should leave the tracks not selected alone");

    // Replace amplitude with average: one constant, the average.
    double sum = 0.0;
    for (const auto& frame : source.frames)
        sum += frame.points[0].amplitude;
    for (const auto& [frame, amplitude, frequency] : trackValues(filtered(Filter::AverageAmplitude, trackOne), 1))
        require(near(amplitude, sum / 10.0), "Replace amplitude with average should hold the average");

    // Smooth amplitude: the mean of the neighborhood; Remove outliers: its
    // median, so the fourth value's spike goes.
    talas::app::PartialTrackFilterSettings three;
    three.neighborhood = 3;
    const auto smooth = trackValues(filtered(Filter::SmoothAmplitude, trackOne, three), 1);
    const auto amplitudeAt = [&](std::size_t index) { return source.frames[index].points[0].amplitude; };
    require(near(std::get<1>(smooth[4]), (amplitudeAt(3) + amplitudeAt(4) + amplitudeAt(5)) / 3.0)
            && near(std::get<1>(smooth[0]), (amplitudeAt(0) + amplitudeAt(1)) / 2.0),
        "Smooth amplitude should take each value's neighborhood mean");
    const auto median = trackValues(filtered(Filter::RemoveAmplitudeOutliers, trackOne, three), 1);
    require(near(std::get<1>(median[3]), amplitudeAt(4)) && near(std::get<1>(median[5]), amplitudeAt(5)),
        "Remove outliers in amplitude should take each value's neighborhood median");

    // Force to harmonic frequency: track 4 a copy of guide track 2's
    // frequency envelope moved an octave higher.
    talas::app::PartialTrackFilterSettings guide;
    guide.guideTrack = 2;
    const auto harmonic = filtered(Filter::ForceHarmonicFrequency, {{4}}, guide);
    for (std::size_t index = 0; index < 10; ++index)
    {
        require(near(std::get<2>(trackValues(harmonic, 4)[index]), 2.0 * std::get<2>(trackValues(harmonic, 2)[index])),
            "Force to harmonic frequency should set track 4 to twice guide track 2");
    }

    // The frequency filters: average, mean and median over the neighborhood.
    const talas::app::PartialTrackFilterRegion trackFour{{4}};
    for (const auto& [frame, amplitude, frequency] : trackValues(filtered(Filter::AverageFrequency, trackFour), 4))
        require(near(frequency, 414.0), "Replace frequency with average should hold the average");
    const auto smoothFrequency = trackValues(filtered(Filter::SmoothFrequency, trackFour, three), 4);
    require(near(std::get<2>(smoothFrequency[6]), 410.0 + 40.0 / 3.0) && near(std::get<2>(smoothFrequency[2]), 410.0),
        "Smooth frequency should take each value's neighborhood mean");
    const auto medianFrequency = trackValues(filtered(Filter::RemoveFrequencyOutliers, trackFour, three), 4);
    require(std::ranges::all_of(medianFrequency, [](const auto& value) { return near(std::get<2>(value), 410.0); }),
        "Remove outliers in frequency should drop the spike");

    // Add noise to frequency: a random deviation within the amount asked
    // for, the same for the same seed.
    talas::app::PartialTrackFilterSettings noise;
    noise.deviationHz = 5.0;
    noise.changesPerSecond = 4.0;
    const auto noisy = trackValues(filtered(Filter::AddFrequencyNoise, {{2}}, noise), 2);
    bool moved = false;
    for (std::size_t index = 0; index < noisy.size(); ++index)
    {
        const auto deviation = std::get<2>(noisy[index]) - (100.0 + 2.0 * static_cast<double>(index));
        require(std::abs(deviation) <= 5.0 + 1.0e-9, "Add noise to frequency should stay within the deviation");
        moved = moved || std::abs(deviation) > 1.0e-6;
    }
    require(moved && noisy == trackValues(filtered(Filter::AddFrequencyNoise, {{2}}, noise), 2),
        "Add noise to frequency should deviate the track, the same way for one seed");

    // Arpeggiate: each higher numbered track waits the delay after the one
    // before it.
    talas::app::PartialTrackFilterSettings delay;
    delay.delaySeconds = 0.25;
    const auto arpeggio = filtered(Filter::Arpeggiate, {{1, 2, 4}}, delay);
    require(std::get<0>(trackValues(arpeggio, 1).front()) == 0 && std::get<0>(trackValues(arpeggio, 2).front()) == 250
            && std::get<0>(trackValues(arpeggio, 4).front()) == 500 && std::get<0>(trackValues(arpeggio, 4).back()) == 1400,
        "Arpeggiate should delay each track the amount after the previous one");

    // The region's times bound a filter.
    const auto bounded = trackValues(filtered(Filter::ScaleAmplitudes, {{1}, 300, 500}, half), 1);
    require(near(std::get<1>(bounded[2]), amplitudeAt(2)) && near(std::get<1>(bounded[4]), amplitudeAt(4) * 0.5)
            && near(std::get<1>(bounded[6]), amplitudeAt(6)),
        "a filter should change only the points within the region");
    require(original == trackValues(filtered(Filter::ScaleAmplitudes, trackOne), 1), "a scale of one should change nothing");

    const auto materialized = talas::project::materializeProjectPartialTrackEdits(source, emptyEdits(manifestFixture(source)));
    talas::app::PartialTrackFilterSettings none;
    none.neighborhood = 0;
    talas::app::PartialTrackFilterSettings missingGuide;
    missingGuide.guideTrack = 3;
    require(!talas::app::filterPartialTracks(materialized.materialized, trackOne, Filter::SmoothAmplitude, none).ok()
            && !talas::app::filterPartialTracks(materialized.materialized, trackFour, Filter::ForceHarmonicFrequency, missingGuide).ok(),
        "an empty neighborhood or a guide track without points should be refused");
}

void testClearAndTrimZeroTracksWithoutDeletingThem()
{
    const auto source = filterFixture();
    const auto manifest = manifestFixture(source);
    const auto edits = emptyEdits(manifest);
    const auto materialized = talas::project::materializeProjectPartialTrackEdits(source, edits);
    const auto stateOf = [&](const talas::project::ProjectPartialTrackEditSet& set, std::uint64_t trackId) {
        const auto found = std::ranges::find(set.trackStates, trackId, &talas::project::ProjectPartialTrackState::trackId);
        return found != set.trackStates.end() && found->muted;
    };
    const auto apply = [&](const talas::project::ProjectPartialTrackEditSet& current,
                           std::vector<talas::project::ProjectPartialTrackStateMutation> states) {
        const auto result = talas::project::applyProjectPartialTrackEditBatch(source, &current,
            {current.editSetId, current.assetId, current.assetGeneration, current.sourceContentHash, {}, std::move(states)});
        require(result.ok(), "the track states should apply");
        return result.edits;
    };

    // Clear zeroes the selected tracks; they stay, grayed out, and
    // no longer sound.
    const std::array cleared{std::uint64_t{2}};
    const auto afterClear = apply(edits, talas::app::clearPartialTracks(materialized.materialized, cleared));
    require(stateOf(afterClear, 2) && !stateOf(afterClear, 1) && !stateOf(afterClear, 4),
        "Clear should zero the selected track alone");
    const auto stillThere = talas::project::materializeProjectPartialTrackEdits(source, afterClear);
    require(stillThere.ok() && trackValues(stillThere.materialized, 2).size() == 10,
        "a cleared track should keep its points");
    talas::app::PartialTrackEditorModel model;
    require(model.setSource({manifest, sharedSource(source), afterClear}), "the editor should take the cleared edits");
    const auto projection = model.project({0, 2000, 20.0, 500.0, 1000, 600, 0, 8});
    const auto row = std::ranges::find(projection.rows, std::uint64_t{2}, &talas::app::PartialTrackEditorTrackRow::trackId);
    require(row != projection.rows.end() && row->muted, "the editor should show the cleared track as zeroed");

    // Restoring it brings it back; Trim zeroes all but the selection.
    const auto restored = apply(afterClear, {{2, false, false}});
    require(!stateOf(restored, 2), "a cleared track should be restorable");
    const std::array kept{std::uint64_t{1}};
    const auto afterTrim = apply(restored, talas::app::trimPartialTracks(materialized.materialized, kept));
    require(!stateOf(afterTrim, 1) && stateOf(afterTrim, 2) && stateOf(afterTrim, 4),
        "Trim should zero everything but the selected track");
    // Selecting a cleared track restores it.
    const auto reselected = talas::app::partialTrackRestoredBySelection(3, true, false, std::uint64_t{1});
    require(reselected.has_value() && reselected->trackId == 3 && !reselected->muted
            && !talas::app::partialTrackRestoredBySelection(3, false, false, std::nullopt).has_value()
            && !talas::app::partialTrackRestoredBySelection(3, true, false, std::uint64_t{3}).has_value(),
        "a greyed track should come back once selected, and only then");

    // Clear zeroes all or part of a track's duration: a region
    // of track 2's first three points is cleared, the rest still sounding,
    // and no point is deleted.
    const auto before = trackValues(materialized.materialized, 2);
    std::set<std::pair<std::uint64_t, std::uint64_t>> region;
    for (std::size_t index = 0; index < 3 && index < before.size(); ++index)
        region.insert({2, std::get<0>(before[index])});
    const auto partly = talas::project::applyProjectPartialTrackEditBatch(source, &edits,
        {edits.editSetId, edits.assetId, edits.assetGeneration, edits.sourceContentHash,
            talas::app::clearPartialTrackPoints(materialized.materialized, region), {}});
    const auto partlyCleared = partly.ok() ? talas::project::materializeProjectPartialTrackEdits(source, partly.edits)
                                           : talas::project::MaterializedPartialTrackEditsResult{};
    const auto after = trackValues(partlyCleared.materialized, 2);
    require(partly.ok() && partlyCleared.ok() && after.size() == before.size() && std::get<1>(after[0]) == 0.0
            && std::get<1>(after[2]) == 0.0 && std::get<1>(after[3]) == std::get<1>(before[3]) && std::get<1>(before[0]) > 0.0,
        "Clear of a region should zero part of a track's duration, deleting nothing");
}

talas::project::MaterializedPartialTrackEdits materializedFixture()
{
    const auto source = filterFixture();
    const auto materialized = talas::project::materializeProjectPartialTrackEdits(source, emptyEdits(manifestFixture(source)));
    require(materialized.ok(), "the fixture should materialize");
    return materialized.materialized;
}

// Amplitude is shown
// as colour, dark blue for the quietest through green to yellow for the
// loudest, and a selected track's colour is brighter.
void testAmplitudeIsShownAsColour()
{
    const auto parts = [](std::uint32_t colour) {
        return std::array<int, 3>{static_cast<int>((colour >> 16) & 0xffu), static_cast<int>((colour >> 8) & 0xffu),
            static_cast<int>(colour & 0xffu)};
    };
    const auto brightness = [&](std::uint32_t colour) {
        const auto c = parts(colour);
        return c[0] + c[1] + c[2];
    };
    const auto loud = parts(talas::app::partialTrackAmplitudeColour(1.0, false));
    const auto quiet = parts(talas::app::partialTrackAmplitudeColour(0.0001, false));
    const auto middle = parts(talas::app::partialTrackAmplitudeColour(0.03, false));
    require(loud[0] > 200 && loud[1] > 200 && loud[2] < 100, "the loudest should be yellow");
    require(quiet[2] > quiet[0] && quiet[2] > quiet[1] && brightness(talas::app::partialTrackAmplitudeColour(0.0001, false)) < 200,
        "the quietest should be dark blue");
    require(middle[1] > middle[0] && middle[1] > middle[2], "the middle of the range should be green");
    require(brightness(talas::app::partialTrackAmplitudeColour(0.5, false)) > brightness(talas::app::partialTrackAmplitudeColour(0.05, false)),
        "louder should be brighter");
    require(brightness(talas::app::partialTrackAmplitudeColour(0.05, true)) > brightness(talas::app::partialTrackAmplitudeColour(0.05, false)),
        "a selected track should be brighter");
}

// In drawing mode the pen
// sets the selected envelope by its height: an amplitude from 1 at the top
// to 0 at the bottom, or a frequency between the display's top and bottom.
void testDrawingRedrawsTheEnvelope()
{
    using Axis = talas::app::PartialTrackVerticalAxis;
    require(talas::app::partialTrackDrawnValue(Axis::Amplitude, 0.0, 100.0, 5000.0) == 1.0
            && talas::app::partialTrackDrawnValue(Axis::Amplitude, 0.75, 100.0, 5000.0) == 0.25,
        "on the amplitude axis the pen's height is the amplitude");
    require(talas::app::partialTrackDrawnValue(Axis::Frequency, 0.0, 100.0, 5000.0) == 5000.0
            && talas::app::partialTrackDrawnValue(Axis::Frequency, 1.0, 100.0, 5000.0) == 100.0
            && talas::app::partialTrackDrawnValue(Axis::Frequency, 0.5, 100.0, 5000.0) == 2550.0,
        "on the frequency axis the pen's height is a frequency of the display");
    require(talas::app::partialTrackDrawnValue(Axis::Amplitude, 1.5, 100.0, 5000.0) == 0.0,
        "the pen below the display draws the lowest value");
}

// Copy makes a new spectrum of the selected region
// alone, its tracks and times, starting at its first frame.
void testCopyMakesASpectrumOfTheRegion()
{
    const auto materialized = materializedFixture();
    talas::app::PartialTrackSelection selection;
    selection.selectBox(materialized, {1, 2, 100, 300}, false, 1, 100);
    const auto region = talas::app::partialTrackRegionAsset(materialized.asset, selection);
    require(region.has_value() && region->sampleRate == materialized.asset.sampleRate
            && !region->frames.empty() && region->frames.front().sourceFrame == 0
            && region->sourceFrameCount == 300 - 100 + 1,
        "the copy should span the selected times, from the start");
    for (const auto& frame : region->frames)
        for (const auto& point : frame.points)
            require((point.trackId == 1 || point.trackId == 2) && selection.contains(point.trackId, frame.sourceFrame + 100),
                "the copy should hold only the selected tracks' selected points");
    require(!talas::app::partialTrackRegionAsset(materialized.asset, talas::app::PartialTrackSelection{}).has_value(),
        "an empty selection copies nothing");
}

// A box selects the tracks and frames
// within it; Shift extends the selection, or begun inside it cuts a hole; a
// number key selects tracks 1 to 10, and Shift adds or removes a track.
void testBoxesExtendAndCutHoles()
{
    const auto materialized = materializedFixture();
    talas::app::PartialTrackSelection selection;
    selection.selectBox(materialized, {1, 2, 100, 500}, false, 1, 100);
    require(selection.points().size() == 10 && selection.contains(2, 300) && !selection.contains(4, 300),
        "a box should select the tracks and frames within it");
    selection.selectBox(materialized, {4, 4, 0, 200}, true, 4, 0);
    require(selection.points().size() == 13 && selection.contains(1, 100) && selection.contains(4, 0),
        "Shift should extend the selection");
    selection.selectBox(materialized, {1, 2, 200, 300}, true, 1, 250);
    require(selection.points().size() == 9 && !selection.contains(1, 200) && !selection.contains(2, 300)
            && selection.contains(1, 100) && selection.contains(1, 400),
        "a Shift box begun inside the selection should leave a hole");
    selection.selectBox(materialized, {1, 1, 500, 700}, true, 1, 600);
    require(selection.contains(1, 700) && selection.contains(1, 500),
        "a Shift box begun outside should extend the selection even where it meets it");
    selection.selectBox(materialized, {4, 4, 800, 900}, false, 4, 800);
    require(selection.points().size() == 2 && selection.trackIds() == std::vector<std::uint64_t>{4},
        "without Shift a new selection should replace the old");
    require(talas::app::partialTrackForNumberKey('1') == std::optional<std::uint64_t>{1}
            && talas::app::partialTrackForNumberKey('0') == std::optional<std::uint64_t>{10}
            && !talas::app::partialTrackForNumberKey('a').has_value(),
        "the number keys should select tracks 1 to 10");
    selection.selectTrack(materialized, 2, false);
    require(selection.trackIds() == std::vector<std::uint64_t>{2} && selection.points().size() == 10,
        "a track should select all of it");
    selection.selectTrack(materialized, 4, true);
    require(selection.trackIds() == std::vector<std::uint64_t>{2, 4}, "Shift should add a track");
    selection.selectTrack(materialized, 4, true);
    require(selection.trackIds() == std::vector<std::uint64_t>{2}, "Shift should remove a selected track");
    selection.selectComplement(materialized);
    require(selection.trackIds() == std::vector<std::uint64_t>{1, 4}, "the complement should select the rest");
    // Select All (Ctrl+A), then a Shift+click on the
    // fundamental takes it out.
    selection.selectAll(materialized);
    require(selection.trackIds() == std::vector<std::uint64_t>{1, 2, 4}, "Ctrl+A should select all the tracks");
    selection.selectTrack(materialized, 1, true);
    require(selection.trackIds() == std::vector<std::uint64_t>{2, 4},
        "a Shift+click should take the fundamental out of the selection");
}

// Save as... makes a clear
// permanent, the cleared track gone from the spectrum saved; with a track
// soloed, the others go.
void testSavingMakesClearsPermanent()
{
    auto materialized = materializedFixture();
    const auto tracksOf = [](const talas::project::PartialTrackAsset& asset) {
        std::set<std::uint64_t> tracks;
        for (const auto& frame : asset.frames)
            for (const auto& point : frame.points)
                tracks.insert(point.trackId);
        return tracks;
    };
    require(tracksOf(talas::app::partialTrackAssetSaved(materialized)) == std::set<std::uint64_t>{1, 2, 4},
        "nothing cleared, the spectrum is saved whole");
    materialized.trackStates = {{2, true, false}};
    const auto cleared = talas::app::partialTrackAssetSaved(materialized);
    require(tracksOf(cleared) == std::set<std::uint64_t>{1, 4} && cleared.frames.size() == materialized.asset.frames.size(),
        "a cleared track should be gone for good from the spectrum saved");
    // A cleared track shows dark gray until saved; then it
    // is gone, and the display's black shows where it was.
    const auto gray = talas::app::partialTrackClearedColour();
    const auto channel = [](std::uint32_t argb, int shift) { return (argb >> shift) & 0xffu; };
    require(channel(gray, 16) == channel(gray, 8) && channel(gray, 8) == channel(gray, 0) && channel(gray, 0) > 0
            && channel(gray, 0) < 0x80 && (talas::app::spectrumEditorBackgroundColour() & 0xffffffu) == 0
            && !tracksOf(cleared).contains(2),
        "a cleared track should show dark gray, and black once saved away");
    const auto loop = talas::app::spectrumEditorLoopMarkerColour();
    require(channel(loop, 16) > 0xc0 && channel(loop, 8) < 0x60 && channel(loop, 0) < 0x60,
        "a start or end marker should show red");
    materialized.trackStates = {{4, false, true}};
    require(tracksOf(talas::app::partialTrackAssetSaved(materialized)) == std::set<std::uint64_t>{4},
        "with a track soloed, the others should go from the spectrum saved");
}

// Selections by amplitude, by threshold and by the
// numbers of the tracks.
void testSelectionCriteria()
{
    const auto materialized = materializedFixture();
    using Criterion = talas::app::PartialTrackSelectionCriterion;
    const auto tracks = [&](Criterion criterion) {
        return talas::app::selectPartialTracksBy(materialized, criterion).trackIds();
    };
    require(tracks(Criterion::EvenTracks) == std::vector<std::uint64_t>{2, 4}
            && tracks(Criterion::OddTracks) == std::vector<std::uint64_t>{1},
        "even and odd tracks");
    require(tracks(Criterion::OctaveHarmonics) == std::vector<std::uint64_t>{1, 2, 4}
            && tracks(Criterion::FibonacciTracks) == std::vector<std::uint64_t>{1, 2}
            && tracks(Criterion::PrimeTracks) == std::vector<std::uint64_t>{1, 2},
        "octave harmonics, Fibonacci and prime numbered tracks");
    const auto within = talas::app::selectPartialTracksBy(materialized, Criterion::AmplitudesWithinRange, 20, 30);
    require(within.contains(2, 0) && within.contains(1, 400) && within.contains(1, 700) && !within.contains(1, 800)
            && !within.contains(1, 300) && !within.contains(4, 0),
        "amplitudes within a range should select those tracks and frames");
    const auto above = talas::app::selectPartialTracksBy(materialized, Criterion::AboveAmplitudeThreshold, 100);
    require(above.points().size() == 1 && above.contains(1, 300), "above a threshold");
}

// The readout of the point under the
// mouse, and a marker named m and its frame number.
void testReadoutAndMarkerNames()
{
    require(talas::app::partialTrackReadout({3, 0.5, 440.0, 0.0}, 1250, 1000.0) == "Track 3 · 1.250 s · -6.0 dB (64) · 440.0 Hz",
        "the readout should give the track, time, dB with the raw value, and frequency");
    require(talas::app::defaultPartialTrackMarkerName(filterFixture(), 450) == "m4",
        "a marker should be named m and its frame number");
}

// Playing all, the selection, the frames
// between the loop markers with their pre- and post-roll, or those outside
// them; the scissors cut the frames between the markers.
void testPlayingPartsAndCuttingLoops()
{
    const auto materialized = materializedFixture();
    talas::app::PartialTrackMarkers markers;
    markers.loopStart = 300;
    markers.loopEnd = 600;
    markers.preRollSeconds = 0.1;
    markers.postRollSeconds = 0.1;
    // The frames that hold points; an edited spectrum keeps its source's
    // frames, empty where every point moved away.
    const auto frames = [](const talas::project::MaterializedPartialTrackEdits& played) {
        std::vector<std::uint64_t> result;
        for (const auto& frame : played.asset.frames)
        {
            if (!frame.points.empty())
                result.push_back(frame.sourceFrame);
        }
        return result;
    };
    talas::app::PartialTrackSelection none;
    const auto all = talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::All, none, markers);
    require(all.has_value() && *all == materialized, "the leftmost button should play all tracks");
    const auto between = talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::BetweenLoopMarkers, none, markers);
    require(between.has_value() && frames(*between) == std::vector<std::uint64_t>{0, 100, 200, 300, 400, 500},
        "between the loop markers should play them with the pre- and post-roll");
    const auto outside = talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::OutsideLoopMarkers, none, markers);
    require(outside.has_value() && frames(*outside) == std::vector<std::uint64_t>{0, 100, 200, 300, 400, 500, 600},
        "outside the loop markers should play the rest, closing the gap");
    talas::app::PartialTrackSelection selection;
    selection.selectBox(materialized, {2, 2, 300, 500}, false, 2, 300);
    const auto selected = talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::Selection, selection, markers);
    require(selected.has_value() && frames(*selected) == std::vector<std::uint64_t>{0, 100, 200}
            && std::ranges::all_of(selected->asset.frames, [](const auto& frame) {
                   return frame.points.size() == 1 && frame.points.front().trackId == 2;
               }),
        "the selection button should play the selected regions only");
    require(!talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::BetweenLoopMarkers, none, {}).has_value(),
        "a loop play without loop markers should play nothing");
    // The scrub bar plays the frames
    // from it at their natural rate, all the tracks or the selected only as
    // the play button used last chose; drawing plays the track drawn.
    const auto scrubbedAll = talas::app::partialTrackScrubbed(materialized, talas::app::PartialTrackPlay::All, selection, 300, 200);
    const auto scrubbedSelected =
        talas::app::partialTrackScrubbed(materialized, talas::app::PartialTrackPlay::Selection, selection, 300, 200);
    require(scrubbedAll.has_value() && frames(*scrubbedAll) == std::vector<std::uint64_t>{0, 100}
            && std::ranges::any_of(scrubbedAll->asset.frames.front().points, [](const auto& point) { return point.trackId != 2; })
            && scrubbedSelected.has_value() && frames(*scrubbedSelected) == std::vector<std::uint64_t>{0, 100}
            && std::ranges::all_of(scrubbedSelected->asset.frames, [](const auto& frame) {
                   return std::ranges::all_of(frame.points, [](const auto& point) { return point.trackId == 2; });
               })
            && !talas::app::partialTrackScrubbed(materialized, talas::app::PartialTrackPlay::All, selection, 100000, 200).has_value(),
        "the scrub bar should play all tracks, or the selected only after the selection's button, from where it is");

    const auto source = filterFixture();
    const auto edits = emptyEdits(manifestFixture(source));
    talas::project::ProjectPartialTrackEditBatch batch{edits.editSetId, edits.assetId, edits.assetGeneration, edits.sourceContentHash,
        talas::app::cutPartialTrackFrames(materialized, 300, 600), {}};
    const auto applied = talas::project::applyProjectPartialTrackEditBatch(source, &edits, batch);
    require(applied.ok(), "the cut should apply");
    const auto cut = talas::project::materializeProjectPartialTrackEdits(source, applied.edits);
    require(cut.ok() && frames(cut.materialized) == std::vector<std::uint64_t>{0, 100, 200, 300, 400, 500, 600}
            && near(std::get<1>(trackValues(cut.materialized, 1)[3]), source.frames[6].points[0].amplitude),
        "the scissors should remove the frames between the loop markers");
    require(talas::app::cutPartialTrackFramesQuestion(1654, 1668) == "Delete the frames from 1654 to 1667?",
        "the scissors should first ask to verify the frames they delete");
}

// Frequency or amplitude on the vertical axis,
// zooming in and out, a box filling the display and the full display; the
// scrub bar stepped, dragged, jumped with Shift and set by pitch bend; the
// arrows selecting the track above or below.
// Join-the-dots joins only
// frames far enough apart for a gap between their dots.
void testJoinTheDotsNeedsAGap()
{
    require(talas::app::partialTrackDotsJoined(true, 20.0, 6.0) && !talas::app::partialTrackDotsJoined(true, 4.0, 6.0)
            && !talas::app::partialTrackDotsJoined(false, 20.0, 6.0) && talas::app::partialTrackDotsJoined(true, -20.0, 6.0),
        "the dots should be joined only where there is a gap between them, while the mode is on");
}

void testTheDisplayZoomAndScrub()
{
    require(talas::app::partialTrackAxisLabel(talas::app::PartialTrackVerticalAxis::Frequency) == "FREQ"
            && talas::app::partialTrackAxisLabel(talas::app::PartialTrackVerticalAxis::Amplitude) == "AMP"
            && talas::app::partialTrackAxisColour(talas::app::PartialTrackVerticalAxis::Amplitude)
                != talas::app::partialTrackAxisColour(talas::app::PartialTrackVerticalAxis::Frequency),
        "the vertical axis is labelled AMP in red in amplitude mode, FREQ in green otherwise");
    talas::app::PartialTrackEditorViewport view;
    view.startFrame = 0;
    view.endFrame = 400;
    view.minimumFrequencyHz = 20.0;
    view.maximumFrequencyHz = 20000.0;
    const auto closer = talas::app::partialTrackZoomedIn(view);
    require(closer.startFrame == 100 && closer.endFrame == 300 && closer.maximumFrequencyHz < 20000.0
            && closer.minimumFrequencyHz > 20.0,
        "Zoom in looks in more detail at a smaller region");
    const auto back = talas::app::partialTrackZoomedOut(closer, 400, 20.0, 20000.0);
    require(back.endFrame - back.startFrame == 400 && back.maximumFrequencyHz <= 20000.0,
        "Zoom out goes back out in steps");
    const auto boxed = talas::app::partialTrackZoomedToBox(view, 50, 90, 200.0, 800.0);
    require(boxed.startFrame == 50 && boxed.endFrame == 90 && boxed.minimumFrequencyHz == 200.0 && boxed.maximumFrequencyHz == 800.0,
        "a box drawn with Control fills the entire display");
    const auto full = talas::app::partialTrackFullDisplay(boxed, 400, 20.0, 20000.0);
    require(full.startFrame == 0 && full.endFrame == 400 && full.maximumFrequencyHz == 20000.0,
        "the full display button zooms all the way out");
    require(talas::app::partialTrackScrubStepped(10, 400, true) == 11 && talas::app::partialTrackScrubStepped(0, 400, false) == 0,
        "the arrow keys step the scrub bar one frame");
    require(talas::app::partialTrackScrubDragged(100, 25, 300, false, 400) == 125
            && talas::app::partialTrackScrubDragged(100, 25, 300, true, 400) == 300,
        "dragged the scrub bar moves with the mouse, and with Shift jumps to it");
    require(talas::app::partialTrackScrubForPitchBend(0, 401) == 0 && talas::app::partialTrackScrubForPitchBend(16383, 401) == 400
            && talas::app::partialTrackScrubForPitchBend(8192, 401) == 200,
        "MIDI pitch bend moves the scrub bar across the file");
    const auto bar = talas::app::partialTrackScrubBarColour();
    require((bar >> 16 & 0xffu) > 0xc0 && (bar >> 8 & 0xffu) > 0xb0 && (bar & 0xffu) < 0x80
            && talas::app::partialTrackScrubTimeColour() == 0xffffffffu
            && talas::app::partialTrackScrubTimeText(230400, 48000.0) == "4.800 s",
        "the yellow time cursor's time should show in white, 4.8 s");
    require(talas::app::partialTrackAdjacent({1, 2, 5}, 2, true) == std::optional<std::uint64_t>{5}
            && talas::app::partialTrackAdjacent({1, 2, 5}, 2, false) == std::optional<std::uint64_t>{1}
            && talas::app::partialTrackAdjacent({1, 2, 5}, 5, true) == std::optional<std::uint64_t>{5},
        "the up arrow selects the track above, the down arrow the one below");
}

// The spectrum editor
// shows the spectrum file opened, else the project's first spectrum.
void testTheEditorShowsTheSpectrumOpened()
{
    talas::project::ProjectDocument document;
    require(talas::app::editedSpectrumAsset(document, std::nullopt) == nullptr, "a project without a spectrum shows none");
    for (const auto* id : {"sample", "first", "opened"})
    {
        talas::project::ProjectAsset asset;
        asset.id = id;
        asset.kind = std::string_view{id} == "sample" ? talas::project::ProjectAssetKind::Audio : talas::project::ProjectAssetKind::PartialSpectrum;
        document.assets.push_back(asset);
    }
    require(talas::app::editedSpectrumAsset(document, std::nullopt)->id == "first"
            && talas::app::editedSpectrumAsset(document, std::string{"opened"})->id == "opened"
            && talas::app::editedSpectrumAsset(document, std::string{"sample"})->id == "first",
        "the editor should show the spectrum opened, else the project's first");
}

// The spectrum editor uses colour throughout, and
// its track selection works on raw amplitudes, 0 to 127, the value the
// readout shows in parentheses: a criterion compares the rounded raw value,
// not the amplitude itself.
void testTheEditorUsesColourAndRawAmplitudes()
{
    const auto channels = [](std::uint32_t colour) {
        return std::array<int, 3>{static_cast<int>((colour >> 16) & 0xffu), static_cast<int>((colour >> 8) & 0xffu),
            static_cast<int>(colour & 0xffu)};
    };
    const auto chroma = [&](std::uint32_t colour) {
        const auto c = channels(colour);
        return std::ranges::max(c) - std::ranges::min(c);
    };
    const auto dominant = [&](std::uint32_t colour) {
        const auto c = channels(colour);
        return static_cast<int>(std::ranges::max_element(c) - c.begin());
    };
    for (const auto amplitude : {0.0005, 0.01, 0.03, 0.3, 1.0})
    {
        require(chroma(talas::app::partialTrackAmplitudeColour(amplitude, false)) >= 0x40
                && chroma(talas::app::partialTrackAmplitudeColour(amplitude, true)) >= 0x20,
            "each amplitude should show in a colour, not a gray, selected or not");
    }
    require(dominant(talas::app::partialTrackAmplitudeColour(0.0005, false)) == 2
            && dominant(talas::app::partialTrackAmplitudeColour(0.03, false)) == 1
            && channels(talas::app::partialTrackAmplitudeColour(1.0, false))[0] > 0xc0,
        "the amplitudes should run through blue and green to yellow");
    require(chroma(talas::app::partialTrackClearedColour()) == 0
            && chroma(talas::app::partialTrackAxisColour(talas::app::PartialTrackVerticalAxis::Frequency)) >= 0x40
            && chroma(talas::app::partialTrackAxisColour(talas::app::PartialTrackVerticalAxis::Amplitude)) >= 0x40
            && chroma(talas::app::spectrumEditorLoopMarkerColour()) >= 0x40 && chroma(talas::app::partialTrackScrubBarColour()) >= 0x40,
        "the axis labels, loop markers and scrub bar should be coloured, a cleared track alone gray");

    require(talas::app::partialTrackRawAmplitude(1.0) == 127 && talas::app::partialTrackRawAmplitude(0.5) == 64
            && talas::app::partialTrackRawAmplitude(0.2) == 25 && talas::app::partialTrackRawAmplitude(0.0) == 0,
        "raw amplitudes should run from 0 to 127");
    require(talas::app::partialTrackReadout({2, 0.2, 100.0, 0.0}, 0, 1000.0).find("(25)") != std::string::npos,
        "the readout should show the raw amplitude in parentheses");
    const auto materialized = materializedFixture();
    using Criterion = talas::app::PartialTrackSelectionCriterion;
    // 0.2 lies above 25 / 127 yet its raw value is 25, so a raw range of 25
    // to 25 takes it.
    const auto raw25 = talas::app::selectPartialTracksBy(materialized, Criterion::AmplitudesWithinRange, 25, 25);
    const auto above24 = talas::app::selectPartialTracksBy(materialized, Criterion::AboveAmplitudeThreshold, 24);
    std::size_t takenAt25 = 0;
    for (const auto& frame : materialized.asset.frames)
    {
        for (const auto& point : frame.points)
        {
            const auto raw = talas::app::partialTrackRawAmplitude(point.amplitude);
            require(raw25.contains(point.trackId, frame.sourceFrame) == (raw == 25)
                    && above24.contains(point.trackId, frame.sourceFrame) == (raw > 24),
                "the selection criteria should compare each point's raw amplitude");
            takenAt25 += raw == 25 ? 1 : 0;
        }
    }
    require(takenAt25 >= 10 && raw25.contains(2, 0) && !talas::app::selectPartialTracksBy(materialized, Criterion::AboveAmplitudeThreshold, 25).contains(2, 0),
        "track 2's amplitude of 0.2 should be selected by its raw value of 25");
}

// The fourth button from the left plays up to the
// start marker and then stops, once the Spectrum menu chooses it; the
// default, playing everything outside the loop markers, is the one at
// first.
void testTheFourthButtonPlaysUpToTheStartMarker()
{
    const auto materialized = materializedFixture();
    const auto buttons = talas::app::spectrumEditorButtons();
    require(buttons[3].button == talas::app::SpectrumEditorButton::PlayOutsideLoopMarkers
            && talas::app::spectrumEditorFourthButtonPlay(true) == talas::app::PartialTrackPlay::UpToStartMarker
            && talas::app::spectrumEditorFourthButtonLabel(true) == "Play to start",
        "the fourth button from the left, chosen so, should play up to the start marker");
    require(talas::app::spectrumEditorFourthButtonPlay(false) == talas::app::PartialTrackPlay::OutsideLoopMarkers
            && talas::app::spectrumEditorFourthButtonLabel(false) == buttons[3].label,
        "at first the fourth button should play outside the loop markers");
    const auto frames = [](const talas::project::MaterializedPartialTrackEdits& played) {
        std::vector<std::uint64_t> result;
        for (const auto& frame : played.asset.frames)
        {
            if (!frame.points.empty())
                result.push_back(frame.sourceFrame);
        }
        return result;
    };
    const auto upTo = talas::app::spectrumEditorFourthButtonPlay(true);
    talas::app::PartialTrackSelection none;
    talas::app::PartialTrackMarkers markers;
    markers.loopStart = 300;
    const auto startOnly = talas::app::partialTrackPlayable(materialized, upTo, none, markers);
    require(startOnly.has_value() && frames(*startOnly) == std::vector<std::uint64_t>{0, 100, 200}
            && startOnly->asset.frames.front().points.size() == materialized.asset.frames.front().points.size(),
        "every track should play up to the start marker, then stop");
    markers.loopEnd = 600;
    const auto withEnd = talas::app::partialTrackPlayable(materialized, upTo, none, markers);
    const auto outside = talas::app::partialTrackPlayable(materialized, talas::app::PartialTrackPlay::OutsideLoopMarkers, none, markers);
    require(withEnd.has_value() && frames(*withEnd) == std::vector<std::uint64_t>{0, 100, 200} && outside.has_value()
            && frames(*outside).size() > 3,
        "with an end marker too it should stop at the start marker, where playing outside the loop markers plays on after the end");
    require(!talas::app::partialTrackPlayable(materialized, upTo, none, {}).has_value(),
        "without a start marker there is nothing to play up to");
}

// The
// buttons across the bottom, grouped in fours as the function keys are.
void testTheButtonsAcrossTheBottom()
{
    using Button = talas::app::SpectrumEditorButton;
    const auto buttons = talas::app::spectrumEditorButtons();
    require(buttons.size() == 12, "twelve buttons across the bottom");
    for (std::size_t index = 0; index < buttons.size(); ++index)
    {
        require(buttons[index].functionKey == static_cast<int>(index) + 1
                && talas::app::spectrumEditorButtonForFunctionKey(buttons[index].functionKey) == buttons[index].button,
            "each button should answer its function key, F1 to F12 in order");
    }
    require(buttons[0].button == Button::PlayAll && buttons[1].button == Button::PlaySelection
            && buttons[2].button == Button::PlayBetweenLoopMarkers && buttons[3].button == Button::PlayOutsideLoopMarkers,
        "the first group plays all, the selection, between and outside the loop markers");
    require(buttons[buttons.size() - 3].button == Button::TrackFilters && buttons[buttons.size() - 4].button == Button::SelectionCriteria
            && buttons[buttons.size() - 2].button == Button::Cut,
        "the filters third from the right, the selection criteria fourth, the scissors second");
    require(buttons[5].button == Button::LoopStart, "the start marker's button near the centre");
    require(!talas::app::spectrumEditorButtonForFunctionKey(13).has_value(), "only F1 to F12 press a button");
}

// The markers, with their labels, and the loop
// markers go into the spectrum file's header the next time it is saved, and
// the file opened again brings them back; those outside the file, and loop
// markers out of order, are left out.
void testMarkersAreKeptInTheSpectrumFile()
{
    const auto source = sourceFixture();
    talas::app::PartialTrackMarkers markers;
    markers.markers = {{"attack", source.frames.front().sourceFrame}, {"past", source.sourceFrameCount + 1}};
    markers.loopStart = source.frames.front().sourceFrame;
    markers.loopEnd = source.frames.back().sourceFrame;
    const auto saved = talas::app::partialTrackAssetWithMarkers(source, markers);
    const auto encoded = talas::project::encodePartialTrackAsset(saved);
    const auto decoded = encoded.ok() ? talas::project::decodePartialTrackAsset(encoded.bytes) : talas::project::PartialTrackAssetDecodeResult{};
    require(decoded.ok() && decoded.asset.markers.size() == 1 && decoded.asset.markers.front().label == "attack",
        "a marker's label should be kept in the saved spectrum file, one past its end left out");
    const auto reopened = talas::app::partialTrackMarkersOf(decoded.asset);
    require(reopened.markers.size() == 1 && reopened.markers.front().name == "attack" && reopened.loopStart == markers.loopStart
            && reopened.loopEnd == markers.loopEnd,
        "the spectrum file opened again should bring its markers and loop markers");
    markers.loopStart = markers.loopEnd;
    const auto unordered = talas::app::partialTrackAssetWithMarkers(source, markers);
    require(!unordered.loopStartFrame.has_value() && !unordered.loopEndFrame.has_value(),
        "loop markers out of order should not be kept");
}

int main()
{
    testAmplitudeIsShownAsColour();
    testMarkersAreKeptInTheSpectrumFile();
    testTheButtonsAcrossTheBottom();
    testTheEditorUsesColourAndRawAmplitudes();
    testTheFourthButtonPlaysUpToTheStartMarker();
        testDrawingRedrawsTheEnvelope();
        testCopyMakesASpectrumOfTheRegion();
        testBoxesExtendAndCutHoles();
        testSavingMakesClearsPermanent();
    testSelectionCriteria();
    testReadoutAndMarkerNames();
    testPlayingPartsAndCuttingLoops();
    testTheDisplayZoomAndScrub();
    testTrackFiltersAsTheManualDescribesThem();
    testClearAndTrimZeroTracksWithoutDeletingThem();
    testProjectionRetainsExactAuthorityAndIdentity();
    testFiltersAreWorkspaceLocalAndDoNotRebuildSource();
    testStatesAndBoundsFailClosed();
    testLargeViewportQueryIsCandidateBounded();
    testTheEditorShowsTheSpectrumOpened();
    testJoinTheDotsNeedsAGap();
}
