#include "talas/project/PartialTrackAnalysis.h"
#include "talas/audio/PartialTrackAudioAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// The Spectral Analysis Tool's lowest frequency,
// response and level, and its straight and quasi-harmonic spectrum files.
void testSpectralAnalysisToolSettings()
{
    constexpr double sampleRate = 44100.0;
    std::vector<double> audio(22050);
    for (std::size_t frame = 0; frame < audio.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / sampleRate;
        audio[frame] = 0.5 * std::sin(2.0 * std::acos(-1.0) * 440.0 * t)
            + 0.25 * std::sin(2.0 * std::acos(-1.0) * 880.0 * t);
    }
    const auto tracks = [](const talas::project::PartialTrackAsset& asset) {
        std::vector<std::uint64_t> ids;
        for (const auto& frame : asset.frames)
            for (const auto& point : frame.points)
                if (std::find(ids.begin(), ids.end(), point.trackId) == ids.end())
                    ids.push_back(point.trackId);
        return ids.size();
    };
    const auto loudest = [](const talas::project::PartialTrackAsset& asset) {
        talas::project::PartialTrackPoint best;
        for (const auto& frame : asset.frames)
            for (const auto& point : frame.points)
                if (point.amplitude > best.amplitude)
                    best = point;
        return best;
    };
    const auto near = [](const talas::project::PartialTrackAsset& asset, double hz) {
        for (const auto& frame : asset.frames)
            for (const auto& point : frame.points)
                if (point.amplitude > 0.05 && std::abs(point.frequencyHz - hz) < hz * 0.03)
                    return true;
        return false;
    };

    talas::audio::PartialTrackAudioAnalysisSettings straight;
    straight.lowestFrequencyHz = 87.31;
    straight.kind = talas::audio::PartialTrackAnalysisKind::Straight;
    const auto band = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, straight);
    require(band.ok(), band.error.has_value() ? band.error->message : "a straight analysis should succeed");
    const auto peaks = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate);
    require(peaks.ok(), "the strongest-peaks analysis should succeed");
    require(near(band.asset, 440.0) && near(band.asset, 880.0)
            && tracks(band.asset) >= 2 && band.asset != peaks.asset,
        "a straight spectrum should hold the bands the sines sound in, analyzed apart from the strongest peaks");
    require(std::abs(loudest(band.asset).frequencyHz - 440.0) < 15.0,
        "a straight spectrum's loudest track should be the 440 Hz sine");

    auto quieter = straight;
    quieter.level = 0.5;
    const auto half = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, quieter);
    require(half.ok()
            && std::abs(loudest(half.asset).amplitude / loudest(band.asset).amplitude - 0.5) < 1.0e-6,
        "the level should scale the analysis's amplitudes");

    auto timing = straight;
    timing.responseBins = 4;
    const auto bestTime = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, timing);
    require(bestTime.ok() && bestTime.asset != band.asset,
        "BestTime should analyze with wider bands than BestFreq");

    talas::audio::PartialTrackAudioAnalysisSettings harmonic;
    harmonic.lowestFrequencyHz = 349.23;
    harmonic.kind = talas::audio::PartialTrackAnalysisKind::QuasiHarmonic;
    const auto quasi = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, harmonic);
    require(quasi.ok(), quasi.error.has_value() ? quasi.error->message : "a quasi-harmonic analysis should succeed");
    require(near(quasi.asset, 440.0) && near(quasi.asset, 880.0),
        "a quasi-harmonic spectrum should follow the fundamental and its harmonic");
}
}

int main()
{
    using namespace talas::project;
    const std::vector<UntrackedPartialFrame> frames{
        {1, 0, {{0, 0.5, 440.0, 8.0}, {0, 0.2, 880.0, 12.0}}},
        {1, 256, {{0, 0.45, 445.0, 8.0}, {0, 0.18, 870.0, 12.0}}},
        {1, 512, {{0, 0.4, 450.0, 8.0}, {0, 0.16, 860.0, 12.0}}},
    };
    const auto tracked = trackPartialSpectrumFrames(
        48000.0, 1024, frames);
    require(tracked.ok(),
            tracked.error.has_value()
                ? tracked.error->message
                : "deterministic partial tracker should succeed");
    require(tracked.asset.frames.size() == 3
                && tracked.asset.frames[0].points[0].trackId == 1
                && tracked.asset.frames[1].points[0].trackId == 1
                && tracked.asset.frames[2].points[1].trackId == 2,
            "nearest-frequency tracking should preserve stable integer identities");

    const std::vector<UntrackedPartialFrame> amplitudeCrossing{
        {1, 0, {{0, 0.9, 440.0, 0.0}, {0, 0.1, 444.0, 0.0}}},
        {1, 1, {{0, 0.1, 441.0, 0.0}, {0, 0.9, 443.0, 0.0}}},
    };
    const auto amplitudeTracked = trackPartialSpectrumFrames(
        48000.0,
        16,
        amplitudeCrossing,
        {10.0, 0.0, 1.0, 4});
    require(amplitudeTracked.ok(),
            "amplitude-aware tracker fixture should be accepted");
    const auto lowAmplitudePoint = std::ranges::find(
        amplitudeTracked.asset.frames[1].points,
        441.0,
        &PartialTrackPoint::frequencyHz);
    const auto highAmplitudePoint = std::ranges::find(
        amplitudeTracked.asset.frames[1].points,
        443.0,
        &PartialTrackPoint::frequencyHz);
    require(lowAmplitudePoint
                    != amplitudeTracked.asset.frames[1].points.end()
                && highAmplitudePoint
                    != amplitudeTracked.asset.frames[1].points.end()
                && lowAmplitudePoint->trackId == 2
                && highAmplitudePoint->trackId == 1,
            "tracker matching must combine bounded frequency and relative-amplitude cost");

    std::vector<UntrackedPartialFrame> denseCandidates{
        {1, 0, {}},
        {1, 1, {{0, 0.5, 440.0, 0.0}}},
    };
    for (std::size_t index = 0;
         index <= maximumPartialTrackTrackingCandidatesPerPeak;
         ++index)
    {
        denseCandidates.front().peaks.push_back(
            {0, 0.5, 430.0 + static_cast<double>(index) * 0.25, 0.0});
    }
    const auto denseRejected = trackPartialSpectrumFrames(
        48000.0,
        16,
        denseCandidates,
        {40.0, 0.0, 1.0, 4});
    require(!denseRejected.ok()
                && denseRejected.error->code
                    == PartialTrackAnalysisErrorCode::LimitExceeded,
            "tracker must fail closed when one peak exceeds the fixed candidate budget");

    const std::vector<LegacyPartialTriplesFrame> legacy{
        {1, 0, {2, 0.5, 440.0, 8.0, 0.2, 880.0, 12.0}},
        {1, 256, {2, 0.45, 445.0, 8.0, 0.18, 870.0, 12.0}},
        {1, 512, {2, 0.4, 450.0, 8.0, 0.16, 860.0, 12.0}},
    };
    const auto converted = convertLegacyPartialTriples(
        48000.0, 1024, legacy);
    require(converted.ok() && converted.asset == tracked.asset,
            "explicit legacy-triples conversion should share canonical tracking/timing authority");

    std::vector<CapturedPartialFrame> captured;
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        CapturedPartialFrame frame{
            9, 4, index + 1, frames[index].segmentId,
            frames[index].sourceFrame, {}};
        for (const auto& point : frames[index].peaks)
        {
            frame.points.push_back({
                std::nullopt,
                point.amplitude,
                point.frequencyHz,
                point.bandwidthHz});
        }
        captured.push_back(std::move(frame));
    }
    const auto assembled = assembleCapturedPartialFrames(
        9, 4, 48000.0, 1024, captured);
    require(assembled.ok()
                && assembled.asset == tracked.asset
                && assembled.acceptedFrameCount == 3
                && assembled.acceptedPointCount == 6
                && assembled.progressPermille == 1000,
            "typed anonymous capture should assemble through the same deterministic tracker and exact telemetry");
    captured.front().stageId = 10;
    require(!assembleCapturedPartialFrames(
                 9, 4, 48000.0, 1024, captured)
                 .ok(),
            "capture assembly should reject stale stage evidence");
    captured.front().stageId = 9;
    auto identifiedCapture = captured;
    for (auto& frame : identifiedCapture)
    {
        frame.points.resize(1);
        frame.points.front().trackId = 9007199254740993ULL;
    }
    const auto preservedIdentity = assembleCapturedPartialFrames(
        9, 4, 48000.0, 1024, identifiedCapture);
    require(preservedIdentity.ok()
                && preservedIdentity.asset.frames.front()
                       .points.front().trackId
                    == 9007199254740993ULL,
            "typed capture should preserve uint64 identities above exact-double range without a floating payload");

    auto malformed = legacy;
    malformed.front().encodedTriples.pop_back();
    require(!convertLegacyPartialTriples(48000.0, 1024, malformed).ok(),
            "malformed legacy triples should fail closed");
    require(!trackPartialSpectrumFrames(
                 48000.0,
                 1024,
                 frames,
                 {},
                 [] { return true; })
                 .ok(),
            "offline tracking should honor cooperative cancellation");

    std::vector<double> audio(4096);
    for (std::size_t frame = 0; frame < audio.size(); ++frame)
    {
        audio[frame] = std::sin(
            2.0 * std::acos(-1.0) * 440.0
            * static_cast<double>(frame) / 48000.0);
    }
    const auto analyzed = talas::audio::analyzeMonoAudioPartialTracks(
        audio, 48000.0);
    require(analyzed.ok()
                && !analyzed.asset.frames.empty()
                && !analyzed.asset.frames.front().points.empty()
                && analyzed.asset.frames.front().points.front().trackId != 0,
            analyzed.error.has_value()
                ? analyzed.error->message
                : "offline audio analysis should publish typed tracked partials");

    auto destination = std::filesystem::temp_directory_path()
        / ("talas-partial-analysis-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count())
            + ".talaspartials");
    std::string publicationError;
    require(talas::audio::publishPartialTrackAssetNoReplace(
                destination,
                tracked.asset,
                publicationError),
            "typed partial publication should install one new destination: "
                + publicationError);
    require(!talas::audio::publishPartialTrackAssetNoReplace(
                 destination,
                 tracked.asset,
                 publicationError),
            "typed partial publication must never replace an existing destination");
    std::error_code cleanupError;
    std::filesystem::remove(destination, cleanupError);

    testSpectralAnalysisToolSettings();

    std::cout << "Partial track analysis tests passed\n";
    return 0;
}
