#include "talas/app/PartialTrackEditorModel.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectPartialTrackEdits.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

constexpr std::size_t largeTrackCount = 128;
constexpr std::size_t largeFrameCount = 2048;
constexpr std::size_t pointsPerFrame = 2;
constexpr std::size_t largePointCount =
    largeFrameCount * pointsPerFrame;
constexpr std::size_t largeOverrideCount = 1024;
constexpr std::uint64_t frameStride = 16;
constexpr std::uint64_t sourceFrameCount =
    largeFrameCount * frameStride;

constexpr std::uint64_t codecBudgetNs = 2'000'000'000;
constexpr std::uint64_t materializationBudgetNs = 2'000'000'000;
constexpr std::uint64_t projectionPreparationBudgetNs = 2'000'000'000;
constexpr std::uint64_t viewportP95BudgetNs = 5'000'000;
constexpr std::uint64_t editBudgetNs = 2'000'000'000;
constexpr std::uint64_t resynthesisPreparationBudgetNs = 2'000'000'000;
constexpr std::uint64_t callbackP99BudgetNs = 5'333'334;
constexpr std::uint64_t previewBudgetNs = 2'000'000'000;
constexpr std::uint64_t offlineBudgetNs = 10'000'000'000;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
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

talas::project::PartialTrackAsset largeSource()
{
    talas::project::PartialTrackAsset source;
    source.sampleRate = 48000.0;
    source.sourceFrameCount = sourceFrameCount;
    source.frames.reserve(largeFrameCount);
    for (std::size_t frameIndex = 0;
         frameIndex < largeFrameCount;
         ++frameIndex)
    {
        const auto firstTrack = frameIndex % largeTrackCount + 1U;
        const auto secondTrack =
            (frameIndex + largeTrackCount / 2U)
                % largeTrackCount + 1U;
        const auto point = [](std::size_t track, double amplitude) {
            return talas::project::PartialTrackPoint{
                static_cast<std::uint64_t>(track),
                amplitude,
                110.0 + static_cast<double>(track) * 100.0,
                5.0 + static_cast<double>(track % 7U),
            };
        };
        std::vector<talas::project::PartialTrackPoint> points{
            point(firstTrack, 0.015),
            point(secondTrack, 0.01),
        };
        std::ranges::sort(points, {}, &talas::project::PartialTrackPoint::trackId);
        source.frames.push_back({
            1,
            static_cast<std::uint64_t>(frameIndex) * frameStride,
            std::move(points),
        });
    }
    return source;
}

talas::project::ProjectPartialTrackEditSet largeEdits(
    const talas::project::PartialTrackAsset& source,
    std::string contentHash)
{
    talas::project::ProjectPartialTrackEditSet edits;
    edits.editSetId = "large_partial_edits";
    edits.assetId = "large_partial_source";
    edits.assetGeneration = 17;
    edits.sourceContentHash = std::move(contentHash);
    edits.pointOverrides.reserve(largeOverrideCount);
    for (std::size_t frameIndex = 0;
         frameIndex < largeOverrideCount;
         ++frameIndex)
    {
        const auto& frame = source.frames[frameIndex];
        auto output = frame.points.front();
        output.amplitude += 0.005;
        edits.pointOverrides.push_back({
            {frame.segmentId,
             frame.sourceFrame,
             frame.points.front().trackId},
            frame.sourceFrame,
            output,
            frameIndex % 2U == 0
                ? talas::project::PartialTrackPointInterpolation::Step
                : talas::project::PartialTrackPointInterpolation::Linear,
            false,
        });
    }
    return edits;
}

talas::project::ProjectAsset manifestFor(
    const talas::project::PartialTrackAsset& source,
    std::string contentHash)
{
    talas::project::ProjectAsset manifest;
    manifest.id = "large_partial_source";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "large/fixture.talaspartials";
    manifest.contentHash = std::move(contentHash);
    manifest.sampleRate = source.sampleRate;
    manifest.frameCount = source.sourceFrameCount;
    manifest.encoding = std::string{talas::project::partialTrackAssetEncoding};
    manifest.generation = 17;
    manifest.provenance = "E10 deterministic large fixture";
    return manifest;
}

void testLargePartialTrackBudgets()
{
    const auto source = largeSource();
    require(!talas::project::validatePartialTrackAsset(source).has_value()
                && source.frames.size() == largeFrameCount,
        "large partial source must satisfy product authority");

    const auto encodeStarted = Clock::now();
    const auto encoded = talas::project::encodePartialTrackAsset(source);
    const auto encodeNs = elapsedNs(encodeStarted);
    require(encoded.ok() && !encoded.bytes.empty(),
        "large partial source must encode canonically");
    const auto decodeStarted = Clock::now();
    const auto decoded = talas::project::decodePartialTrackAsset(encoded.bytes);
    const auto decodeNs = elapsedNs(decodeStarted);
    require(decoded.ok() && decoded.asset == source,
        "large partial codec must preserve exact source identity and values");
    require(sanitizerInstrumented
            || (encodeNs <= codecBudgetNs && decodeNs <= codecBudgetNs),
        "large partial codec exceeded its 2 s Debug budget");

    const auto contentHash =
        talas::project::calculateProjectAssetSha256(encoded.bytes);
    const auto manifest = manifestFor(source, contentHash);
    const auto edits = largeEdits(source, contentHash);
    require(edits.pointOverrides.size() == largeOverrideCount
                && !talas::project::validateProjectPartialTrackEditSet(edits)
                        .has_value()
                && talas::project::partialTrackEditSetMatchesAsset(
                    edits, manifest),
        "large edit recipe must retain exact immutable asset authority");

    const auto materializeStarted = Clock::now();
    const auto materialized =
        talas::project::materializeProjectPartialTrackEdits(source, edits);
    const auto materializeNs = elapsedNs(materializeStarted);
    require(materialized.ok()
                && materialized.materialized.asset.frames.size()
                    == largeFrameCount
                && materialized.materialized.pointIdentities.size()
                    == largePointCount,
        "large edit recipe must materialize every stable point exactly once");
    require(sanitizerInstrumented
            || materializeNs <= materializationBudgetNs,
        "large edit materialization exceeded its 2 s Debug budget");

    talas::app::PartialTrackEditorModel model;
    const auto projectionPreparationStarted = Clock::now();
    require(model.setSource({
                manifest,
                std::make_shared<const talas::project::PartialTrackAsset>(
                    source),
                edits}),
        "large editor projection must accept exact source and edits");
    const auto projectionPreparationNs =
        elapsedNs(projectionPreparationStarted);
    require(model.indexedPointCount() == largePointCount
                && (sanitizerInstrumented
                    || projectionPreparationNs
                        <= projectionPreparationBudgetNs),
        "large projection preparation exceeded its 2 s Debug budget");

    const talas::app::PartialTrackEditorViewport viewport{
        0,
        512,
        20.0,
        20000.0,
        1200,
        640,
        0,
        24,
    };
    std::vector<std::uint64_t> viewportSamples;
    viewportSamples.reserve(64);
    std::size_t maximumCandidates = 0;
    std::size_t maximumPoints = 0;
    std::size_t maximumSegments = 0;
    for (std::size_t sample = 0; sample < 68; ++sample)
    {
        const auto started = Clock::now();
        const auto projected = model.project(viewport);
        const auto duration = elapsedNs(started);
        require(projected.state == talas::app::PartialTrackEditorState::Ready,
            "large viewport must remain ready");
        maximumCandidates = std::max(
            maximumCandidates, projected.viewportCandidateCount);
        maximumPoints = std::max(maximumPoints, projected.points.size());
        maximumSegments = std::max(
            maximumSegments, projected.segments.size());
        if (sample >= 4)
            viewportSamples.push_back(duration);
    }
    const auto viewportP95 = nearestRank(viewportSamples, 95, 100);
    require(maximumCandidates > 0
                && maximumCandidates < largePointCount
                && maximumPoints > 0
                && maximumSegments > 0,
        "large viewport must exercise positive bounded point and segment candidates");
    require(sanitizerInstrumented
            || viewportP95 <= viewportP95BudgetNs,
        "large viewport exceeded its 5 ms p95 Debug budget");

    auto batch = talas::project::ProjectPartialTrackEditBatch{};
    batch.editSetId = edits.editSetId;
    batch.assetId = edits.assetId;
    batch.assetGeneration = edits.assetGeneration;
    batch.sourceContentHash = edits.sourceContentHash;
    auto changedPoint = source.frames.front().points.front();
    changedPoint.amplitude = 0.025;
    batch.pointMutations.push_back({
        talas::project::ProjectPartialTrackPointMutationKind::Update,
        {talas::project::MaterializedPartialTrackPointOrigin::Source,
         {1, 0, changedPoint.trackId},
         {}},
        1,
        0,
        changedPoint,
        talas::project::PartialTrackPointInterpolation::Linear,
    });
    const auto editStarted = Clock::now();
    const auto edited = talas::project::applyProjectPartialTrackEditBatch(
        source, &edits, batch);
    const auto editNs = elapsedNs(editStarted);
    require(edited.ok()
                && edited.edits.pointOverrides.size()
                    == largeOverrideCount
                && (sanitizerInstrumented
                    || editNs <= editBudgetNs),
        "one large-recipe edit exceeded its 2 s atomic-edit budget");

    const auto prepareStarted = Clock::now();
    talas::audio::PreparedPartialTrackResynthesizer synthesizer{
        materialized.materialized.asset,
        materialized.materialized.pointInterpolations,
        {talas::audio::maximumRealtimePartialTrackVoices, 1.0, 0}};
    const auto prepareNs = elapsedNs(prepareStarted);
    require(synthesizer.ready()
                && synthesizer.trackCount() == largeTrackCount
                && (sanitizerInstrumented
                    || prepareNs <= resynthesisPreparationBudgetNs),
        "large partial resynthesis preparation exceeded its 2 s Debug budget");

    std::vector<std::uint64_t> callbackSamples;
    callbackSamples.reserve(sourceFrameCount / 256U);
    std::vector<double> callbackOutput(256);
    double callbackChecksum = 0.0;
    for (std::uint64_t frame = 0;
         frame < sourceFrameCount;
         frame += callbackOutput.size())
    {
        const auto started = Clock::now();
        require(synthesizer.render(frame, callbackOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok,
            "large callback render must remain valid");
        callbackSamples.push_back(elapsedNs(started));
        for (const auto sample : callbackOutput)
            callbackChecksum += std::abs(sample);
    }
    const auto callbackP99 = nearestRank(callbackSamples, 99, 100);
    require(callbackChecksum > 0.0,
        "large callback budget must consume fresh rendered audio");
    require(sanitizerInstrumented
            || callbackP99 <= callbackP99BudgetNs,
        "large callback exceeded one 48 kHz / 256-frame deadline");

    const auto previewStarted = Clock::now();
    const auto previewMaterialized =
        talas::project::materializeProjectPartialTrackEdits(source, edits);
    require(previewMaterialized.ok(),
        "large preview generation must materialize exact edits");
    talas::audio::PreparedPartialTrackResynthesizer preview{
        previewMaterialized.materialized.asset,
        previewMaterialized.materialized.pointInterpolations,
        {talas::audio::maximumRealtimePartialTrackVoices, 1.0, 0}};
    std::vector<double> previewOutput(256);
    require(preview.ready()
                && preview.render(0, previewOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok,
        "large preview generation must prepare and render its first block");
    const auto previewNs = elapsedNs(previewStarted);
    require(sanitizerInstrumented
            || previewNs <= previewBudgetNs,
        "large preview preparation exceeded its 2 s Debug budget");

    const auto offlineStarted = Clock::now();
    const auto offline = talas::audio::renderPartialTrackAssetOffline(
        materialized.materialized.asset,
        materialized.materialized.pointInterpolations,
        {},
        4096);
    const auto offlineNs = elapsedNs(offlineStarted);
    double offlineChecksum = 0.0;
    for (const auto sample : offline.samples)
        offlineChecksum += std::abs(sample);
    require(offline.ok()
                && offline.samples.size() == sourceFrameCount
                && offlineChecksum > 0.0,
        "large offline resynthesis must publish fresh complete audio");
    require(sanitizerInstrumented
            || offlineNs <= offlineBudgetNs,
        "large offline resynthesis exceeded its 10 s Debug budget");

    std::cout
        << "E10 large partial budgets (ns): encode=" << encodeNs
        << " decode=" << decodeNs
        << " materialize=" << materializeNs
        << " projection-prepare=" << projectionPreparationNs
        << " viewport-p95=" << viewportP95
        << " edit=" << editNs
        << " resynthesis-prepare=" << prepareNs
        << " callback-p99=" << callbackP99
        << " preview=" << previewNs
        << " offline=" << offlineNs
        << " tracks=" << largeTrackCount
        << " frames=" << largeFrameCount
        << " points=" << largePointCount
        << " overrides=" << largeOverrideCount
        << " candidates=" << maximumCandidates << '\n';
}
}

int main()
{
    testLargePartialTrackBudgets();
    return 0;
}
