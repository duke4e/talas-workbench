#include "talas/audio/PartialTrackDerivative.h"

#include "talas/audio/WavReader.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectAssetInspection.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

talas::project::ProjectAsset manifestFor(
    const talas::project::PartialTrackAsset& source,
    const std::filesystem::path& path)
{
    const auto encoded = talas::project::encodePartialTrackAsset(source);
    require(encoded.ok(), "fixture should encode");
    talas::project::ProjectAsset manifest;
    manifest.id = "partials";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.disposition =
        talas::project::ProjectAssetDisposition::Referenced;
    manifest.path = path.string();
    manifest.generation = 7;
    manifest.contentHash =
        talas::project::calculateProjectAssetSha256(encoded.bytes);
    manifest.sampleRate = source.sampleRate;
    manifest.frameCount = source.sourceFrameCount;
    manifest.channelCount = 0;
    manifest.encoding = std::string{
        talas::project::partialTrackAssetEncoding};
    return manifest;
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("talas-partial-derivative-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const talas::project::PartialTrackAsset source{
        48000.0,
        128,
        {
            {1, 0, {{1, 0.5, 440.0, 0.0}, {2, 0.25, 880.0, 0.0}}},
            {1, 64, {{1, 0.25, 660.0, 0.0}, {2, 0.1, 990.0, 0.0}}},
        }};
    const auto sourcePath = root / "source.talaspartial";
    auto manifest = manifestFor(source, sourcePath);
    talas::project::ProjectPartialTrackEditSet edits;
    edits.editSetId = "edited";
    edits.assetId = manifest.id;
    edits.assetGeneration = manifest.generation;
    edits.sourceContentHash = manifest.contentHash;
    edits.trackStates.push_back({2, true, false});

    std::uint16_t lastProgress = 0;
    const auto partialPath = root / "edited.talaspartial";
    const talas::audio::PartialTrackDerivativeRequest partialRequest{
        partialPath,
        sourcePath,
        manifest,
        std::make_shared<const talas::project::PartialTrackAsset>(source),
        edits,
        talas::audio::PartialTrackDerivativeKind::EditedPartial,
        17,
    };
    const auto partial = talas::audio::renderPartialTrackDerivative(
        partialRequest,
        {},
        [] { return true; },
        [&](std::uint16_t value) {
            require(value >= lastProgress, "progress should be monotonic");
            lastProgress = value;
        });
    require(partial.ok()
                && std::filesystem::is_regular_file(partialPath)
                && partial.encoding
                    == talas::project::partialTrackAssetEncoding
                && lastProgress == 1000,
            "edited partial derivative should publish one verified no-replace artifact");
    std::ifstream partialInput(partialPath, std::ios::binary);
    const std::vector<std::uint8_t> partialBytes(
        std::istreambuf_iterator<char>{partialInput}, {});
    const auto decoded = talas::project::decodePartialTrackAsset(partialBytes);
    require(decoded.ok()
                && decoded.asset.frames.front().points.size() == 1
                && decoded.asset.frames.front().points.front().trackId == 1
                && partial.contentHash
                    == talas::project::calculateProjectAssetSha256(
                        partialBytes),
            "edited partial derivative should materialize mute/solo state without mutating source bytes");

    auto soloRequest = partialRequest;
    soloRequest.destination = root / "solo.talaspartial";
    soloRequest.edits->trackStates = {{2, false, true}};
    const auto solo = talas::audio::renderPartialTrackDerivative(
        soloRequest, {}, [] { return true; });
    std::ifstream soloInput(soloRequest.destination, std::ios::binary);
    const std::vector<std::uint8_t> soloBytes(
        std::istreambuf_iterator<char>{soloInput}, {});
    const auto soloDecoded =
        talas::project::decodePartialTrackAsset(soloBytes);
    require(solo.ok()
                && soloDecoded.ok()
                && soloDecoded.asset.frames.front().points.size() == 1
                && soloDecoded.asset.frames.front().points.front().trackId
                    == 2,
            "solo should exclude tracks without an explicit sparse state");

    const auto audioPath = root / "edited.wav";
    auto audioRequest = partialRequest;
    audioRequest.destination = audioPath;
    audioRequest.kind = talas::audio::PartialTrackDerivativeKind::Audio;
    const auto audio = talas::audio::renderPartialTrackDerivative(
        audioRequest, {}, [] { return true; });
    const auto wav = talas::audio::readPcm16Wav(audioPath);
    require(audio.ok()
                && wav.ok()
                && wav.sampleRate == 48000.0
                && wav.channels.size() == 1
                && wav.channels.front().size() == 128
                && audio.contentHash.size() == 64,
            "audio derivative should stream the exact edited source extent through immutable staging");

    const auto retained = root / "retained.wav";
    {
        std::ofstream output(retained, std::ios::binary);
        output << "prior";
    }
    auto retainedRequest = audioRequest;
    retainedRequest.destination = retained;
    const auto rejected = talas::audio::renderPartialTrackDerivative(
        retainedRequest);
    std::ifstream retainedInput(retained, std::ios::binary);
    const std::string retainedBytes(
        std::istreambuf_iterator<char>{retainedInput}, {});
    require(!rejected.ok() && retainedBytes == "prior",
            "no-replace rejection must retain the prior usable artifact");

    const auto cancelledPath = root / "cancelled.wav";
    std::atomic<int> cancellationChecks = 0;
    auto cancelledRequest = audioRequest;
    cancelledRequest.destination = cancelledPath;
    const auto cancelled = talas::audio::renderPartialTrackDerivative(
        cancelledRequest,
        [&] { return cancellationChecks.fetch_add(1) >= 1; },
        [] { return true; });
    require(cancelled.status
                == talas::audio::PartialTrackDerivativeStatus::Cancelled
                && !std::filesystem::exists(cancelledPath),
            "cancelled derivative must not publish a destination");

    const auto stalePath = root / "stale.wav";
    auto staleRequest = audioRequest;
    staleRequest.destination = stalePath;
    const auto stale = talas::audio::renderPartialTrackDerivative(
        staleRequest, {}, [] { return false; });
    require(stale.status
                == talas::audio::PartialTrackDerivativeStatus::AuthorityChanged
                && !std::filesystem::exists(stalePath),
            "terminal authority failure must discard private staging without publication");

    const auto mismatchedPath = root / "mismatched.wav";
    auto mismatchedRequest = audioRequest;
    mismatchedRequest.destination = mismatchedPath;
    mismatchedRequest.sourceManifest.contentHash.assign(64, '0');
    const auto mismatched = talas::audio::renderPartialTrackDerivative(
        mismatchedRequest);
    require(mismatched.status
                == talas::audio::PartialTrackDerivativeStatus::InvalidRequest
                && !std::filesystem::exists(mismatchedPath),
            "decoded source bytes must match the exact manifest hash before derivative work begins");

    auto aliasRequest = audioRequest;
    aliasRequest.destination = sourcePath;
    const auto alias = talas::audio::renderPartialTrackDerivative(aliasRequest);
    require(alias.status
                == talas::audio::PartialTrackDerivativeStatus::DestinationRejected
                && !std::filesystem::exists(sourcePath),
            "derivative destination must never alias the immutable source path");

    std::filesystem::remove_all(root);
    std::cout << "Partial-track derivative tests passed\n";
    return 0;
}
