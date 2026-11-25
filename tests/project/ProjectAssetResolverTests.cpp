#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetResolver.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-asset-resolver-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeBytes(
    const std::filesystem::path& path,
    std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
        throw std::runtime_error("unable to write resolver fixture");
}

void writeBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
        throw std::runtime_error("unable to write resolver fixture");
}

void writeAscii(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::string_view value)
{
    for (std::size_t index = 0; index < value.size(); ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value[index]);
}

void writeU16(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void writeU32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes[offset + index] = static_cast<std::uint8_t>(
            value >> (index * 8U));
    }
}

std::vector<std::uint8_t> pcm16Wav(
    std::uint16_t channelCount,
    std::uint32_t sampleRate,
    std::uint32_t frameCount)
{
    const auto blockAlign = static_cast<std::uint16_t>(channelCount * 2U);
    const auto dataBytes = static_cast<std::uint32_t>(frameCount * blockAlign);
    std::vector<std::uint8_t> bytes(44U + dataBytes, 0U);
    writeAscii(bytes, 0, "RIFF");
    writeU32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8U));
    writeAscii(bytes, 8, "WAVE");
    writeAscii(bytes, 12, "fmt ");
    writeU32(bytes, 16, 16);
    writeU16(bytes, 20, 1);
    writeU16(bytes, 22, channelCount);
    writeU32(bytes, 24, sampleRate);
    writeU32(bytes, 28, sampleRate * blockAlign);
    writeU16(bytes, 32, blockAlign);
    writeU16(bytes, 34, 16);
    writeAscii(bytes, 36, "data");
    writeU32(bytes, 40, dataBytes);
    return bytes;
}

talas::project::ProjectAsset binaryManifest(
    std::string id,
    std::string path,
    std::string hash = {})
{
    talas::project::ProjectAsset asset;
    asset.id = std::move(id);
    asset.kind = talas::project::ProjectAssetKind::Binary;
    asset.path = std::move(path);
    asset.contentHash = std::move(hash);
    asset.generation = 7;
    asset.provenance = "resolver test";
    return asset;
}

void testClassifiesExactChangedAndUnverifiedCandidates()
{
    TemporaryDirectory directory;
    const auto projectPath = directory.path() / "Project.talas.json";
    const auto exactPath = directory.path() / "moved" / "exact.bin";
    const auto changedPath = directory.path() / "replacement" / "changed.bin";
    writeBytes(exactPath, "accepted bytes");
    writeBytes(changedPath, "different bytes");

    const auto acceptedHash =
        talas::project::calculateProjectAssetSha256(exactPath);
    const auto changedHash =
        talas::project::calculateProjectAssetSha256(changedPath);
    require(acceptedHash.ok() && changedHash.ok(),
            "resolver fixtures should hash");

    const auto manifest = binaryManifest(
        "Samples/Hit",
        "missing/hit.bin",
        acceptedHash.contentHash);
    const auto exact = talas::project::classifyProjectAssetCandidate(
        manifest,
        exactPath,
        projectPath);
    require(exact.ok(), "exact bytes should classify successfully");
    require(
        exact.resolution->mode
            == talas::project::ProjectAssetResolutionMode::Relocation,
        "exact bytes should classify as relocation");
    require(
        exact.resolution->expectedGeneration == 7
            && exact.resolution->expectedContentHash
                == acceptedHash.contentHash,
        "relocation should retain its inspected base identity");
    require(
        exact.resolution->asset.path == "moved/exact.bin",
        "relocation should store a project-relative path, got: "
            + exact.resolution->asset.path);
    require(
        exact.resolution->asset.contentHash
            == acceptedHash.contentHash,
        "relocation should retain its exact accepted hash");

    const auto changed = talas::project::classifyProjectAssetCandidate(
        manifest,
        changedPath,
        projectPath);
    require(
        changed.ok()
            && changed.resolution->mode
                == talas::project::ProjectAssetResolutionMode::Replacement
            && changed.resolution->asset.path
                == "replacement/changed.bin"
            && changed.resolution->asset.contentHash
                == changedHash.contentHash
            && changed.resolution->asset.generation == 7,
        "different valid bytes should remain an inert replacement candidate with the base generation");

    auto unverified = manifest;
    unverified.contentHash.clear();
    const auto initial = talas::project::classifyProjectAssetCandidate(
        unverified,
        exactPath,
        projectPath);
    require(
        initial.ok()
            && initial.resolution->mode
                == talas::project::ProjectAssetResolutionMode::InitialInspection
            && initial.resolution->asset.contentHash
                == acceptedHash.contentHash
            && initial.resolution->asset.generation == 7,
        "an explicitly selected unverified asset should classify as first inspection without generation drift");
}

void testClassificationCancellationAndNonRegularPathsAreTyped()
{
    TemporaryDirectory directory;
    const auto manifest = binaryManifest(
        "Samples/Hit",
        "missing/hit.bin");
    const auto cancelled = talas::project::classifyProjectAssetCandidate(
        manifest,
        directory.path() / "missing.bin",
        std::nullopt,
        [] { return true; });
    require(
        !cancelled.ok()
            && cancelled.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled,
        "cancellation should remain a typed resolver failure");

    const auto directoryCandidate =
        talas::project::classifyProjectAssetCandidate(
            manifest,
            directory.path());
    require(
        !directoryCandidate.ok()
            && directoryCandidate.error->code
                == talas::project::ProjectAssetInspectionErrorCode::NotRegularFile,
        "directories should remain typed non-regular candidates");
}

void testAudioCandidatesPreserveExactShapeAndRejectChangedShape()
{
    TemporaryDirectory directory;
    const auto acceptedPath = directory.path() / "accepted.wav";
    const auto replacementPath = directory.path() / "replacement.wav";
    const auto changedShapePath = directory.path() / "changed-shape.wav";
    const auto wrongShapePath = directory.path() / "wrong-shape.wav";
    writeBytes(acceptedPath, pcm16Wav(1, 48000, 16));
    auto replacementBytes = pcm16Wav(1, 48000, 16);
    replacementBytes.back() = 1;
    writeBytes(replacementPath, replacementBytes);
    writeBytes(changedShapePath, pcm16Wav(1, 48000, 32));
    writeBytes(wrongShapePath, pcm16Wav(3, 48000, 16));

    auto manifest = binaryManifest("Audio/Voice", "missing.wav");
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.contentHash =
        talas::project::calculateProjectAssetSha256(acceptedPath).contentHash;
    manifest.channelCount = 1;
    manifest.sampleRate = 48000;
    manifest.frameCount = 16;
    manifest.encoding = "pcm16-wav";

    const auto exact = talas::project::classifyProjectAssetCandidate(
        manifest,
        acceptedPath);
    require(
        exact.ok()
            && exact.resolution->mode
                == talas::project::ProjectAssetResolutionMode::Relocation
            && exact.resolution->asset.channelCount == 1
            && exact.resolution->asset.frameCount == 16,
        "exact Audio bytes should retain the accepted media shape");

    const auto replacement = talas::project::classifyProjectAssetCandidate(
        manifest,
        replacementPath);
    require(
        replacement.ok()
            && replacement.resolution->mode
                == talas::project::ProjectAssetResolutionMode::Replacement
            && replacement.resolution->asset.channelCount == 1
            && replacement.resolution->asset.frameCount == 16,
        "different Audio bytes with the exact accepted shape should remain an explicit replacement candidate");

    const auto changed = talas::project::classifyProjectAssetCandidate(
        manifest,
        changedShapePath);
    require(
        changed.ok()
            && changed.resolution->mode
                == talas::project::ProjectAssetResolutionMode::Replacement
            && changed.resolution->asset.channelCount == 1
            && changed.resolution->asset.sampleRate == 48000
            && changed.resolution->asset.frameCount == 32
            && changed.resolution->asset.encoding == "pcm16-wav",
        "valid different Audio bytes should carry their newly inspected media shape as an inert replacement candidate");

    const auto wrongShape = talas::project::classifyProjectAssetCandidate(
        manifest,
        wrongShapePath);
    require(
        !wrongShape.ok()
            && wrongShape.error->code
                == talas::project::ProjectAssetInspectionErrorCode::UnsupportedMedia,
        "Audio outside the supported mono/stereo shape should remain a typed failure");

    auto unverified = manifest;
    unverified.contentHash.clear();
    unverified.channelCount = 0;
    unverified.sampleRate = 0;
    unverified.frameCount = 0;
    unverified.encoding.clear();
    const auto initial = talas::project::classifyProjectAssetCandidate(
        unverified,
        changedShapePath);
    require(
        initial.ok()
            && initial.resolution->mode
                == talas::project::ProjectAssetResolutionMode::InitialInspection
            && initial.resolution->asset.channelCount == 1
            && initial.resolution->asset.sampleRate == 48000
            && initial.resolution->asset.frameCount == 32
            && initial.resolution->asset.encoding == "pcm16-wav",
        "unverified Audio should receive one complete first-inspection shape");
}

void testEmbeddedAssetsAndInvalidRootsRemainFailClosed()
{
    TemporaryDirectory directory;
    const auto candidate = directory.path() / "candidate.bin";
    writeBytes(candidate, "candidate");
    auto embedded = binaryManifest(
        "Samples/Embedded",
        "Assets/candidate.bin",
        talas::project::calculateProjectAssetSha256(candidate).contentHash);
    embedded.disposition = talas::project::ProjectAssetDisposition::Embedded;
    const auto classified = talas::project::classifyProjectAssetCandidate(
        embedded,
        candidate);
    require(
        !classified.ok()
            && classified.error->code
                == talas::project::ProjectAssetInspectionErrorCode::InvalidManifest,
        "embedded package assets should never redirect to an external file");

    const std::vector manifests{embedded};
    const auto invalidRoot = talas::project::searchProjectAssetDirectory(
        manifests,
        candidate);
    require(
        invalidRoot.status == talas::project::ProjectAssetSearchStatus::InvalidRoot
            && invalidRoot.matches.empty()
            && invalidRoot.diagnostics.size() == 1,
        "a file search root should fail closed with one bounded diagnostic");

    talas::project::ProjectAssetSearchLimits noDiagnostics;
    noDiagnostics.maximumDiagnostics = 0;
    const auto truncated = talas::project::searchProjectAssetDirectory(
        manifests,
        directory.path() / "missing-root",
        std::nullopt,
        noDiagnostics);
    require(
        truncated.status == talas::project::ProjectAssetSearchStatus::InvalidRoot
            && truncated.diagnostics.size() == 1
            && truncated.diagnostics.front().code
                == talas::project::ProjectAssetResolverDiagnosticCode::CapacityExceeded,
        "diagnostic capacity zero should retain only one truncation marker");
}

void testDirectorySearchFindsExactSharedAndAmbiguousHashesDeterministically()
{
    TemporaryDirectory directory;
    const auto projectPath = directory.path() / "Project.talas.json";
    const auto searchRoot = directory.path() / "library";
    const auto firstExact = searchRoot / "a" / "hit.bin";
    const auto secondExact = searchRoot / "z" / "copy.bin";
    const auto unrelated = searchRoot / "m" / "other.bin";
    writeBytes(firstExact, "shared exact bytes");
    writeBytes(secondExact, "shared exact bytes");
    writeBytes(unrelated, "unrelated bytes");
    const auto hash = talas::project::calculateProjectAssetSha256(firstExact);
    require(hash.ok(), "directory fixture should hash");

    auto first = binaryManifest(
        "Samples/First",
        "missing/first.bin",
        hash.contentHash);
    auto second = first;
    second.id = "Samples/Second";
    auto unverified = first;
    unverified.id = "Samples/Unverified";
    unverified.contentHash.clear();

    std::vector<talas::project::ProjectAsset> manifests{
        first,
        second,
        unverified,
    };
    std::vector<talas::project::ProjectAssetSearchProgress> progress;
    const auto result = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot,
        projectPath,
        {},
        {},
        [&](const auto& update) { progress.push_back(update); });
    require(result.ok(), "bounded exact directory search should complete");
    require(
        result.matches.size() == 2
            && result.matches[0].assetId == "Samples/First"
            && result.matches[1].assetId == "Samples/Second",
        "search should retain input identity order and skip unverified assets");
    for (const auto& match : result.matches)
    {
        require(
            match.candidates.size() == 2
                && match.candidates[0].asset.path
                    == "library/a/hit.bin"
                && match.candidates[1].asset.path
                    == "library/z/copy.bin",
            "identical canonical matches should remain explicit and lexically ordered");
    }
    require(
        result.hashedFiles == 3
            && !progress.empty()
            && progress.back().hashedFiles == 3,
        "each regular file should be hashed once with observable bounded progress");
}

void testDirectorySearchCancellationAndCapacityPublishNoPartialMatches()
{
    TemporaryDirectory directory;
    const auto searchRoot = directory.path() / "library";
    writeBytes(searchRoot / "a.bin", "match");
    writeBytes(searchRoot / "b.bin", "other");
    const auto hash = talas::project::calculateProjectAssetSha256(
        searchRoot / "a.bin");
    require(hash.ok(), "cancellation fixture should hash");
    const std::vector manifests{
        binaryManifest("Samples/Hit", "missing.bin", hash.contentHash),
    };

    std::size_t checks = 0;
    const auto cancelled = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot,
        std::nullopt,
        {},
        [&] { return ++checks >= 4; });
    require(
        cancelled.status == talas::project::ProjectAssetSearchStatus::Cancelled
            && cancelled.matches.empty(),
        "cancelled search should publish no partial match set");

    talas::project::ProjectAssetSearchLimits limits;
    limits.maximumHashedFiles = 1;
    const auto capped = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot,
        std::nullopt,
        limits);
    require(
        capped.status
                == talas::project::ProjectAssetSearchStatus::CapacityExceeded
            && capped.matches.empty()
            && !capped.diagnostics.empty()
            && capped.diagnostics.back().code
                == talas::project::ProjectAssetResolverDiagnosticCode::CapacityExceeded,
        "over-capacity search should fail completely with an actionable typed diagnostic");

    limits = {};
    limits.maximumVisitedEntries = 1;
    const auto visitCapped = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot,
        std::nullopt,
        limits);
    require(
        visitCapped.status
                == talas::project::ProjectAssetSearchStatus::CapacityExceeded
            && visitCapped.visitedEntries == 1
            && visitCapped.matches.empty(),
        "visited-entry capacity should bound enumeration before a wide directory is accumulated");
}

void testDirectorySearchDoesNotFollowDirectorySymlinks()
{
    TemporaryDirectory directory;
    const auto searchRoot = directory.path() / "library";
    const auto outside = directory.path() / "outside";
    writeBytes(outside / "hit.bin", "outside exact bytes");
    std::filesystem::create_directories(searchRoot);
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(
        outside,
        searchRoot / "linked-outside",
        symlinkError);
    if (symlinkError)
        return;

    const auto hash = talas::project::calculateProjectAssetSha256(
        outside / "hit.bin");
    require(hash.ok(), "symlink fixture should hash");
    const std::vector manifests{
        binaryManifest("Samples/Hit", "missing.bin", hash.contentHash),
    };
    const auto result = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot);
    require(
        result.ok()
            && result.hashedFiles == 0
            && result.matches.size() == 1
            && result.matches.front().candidates.empty(),
        "directory search should not follow a symlink outside the selected root");
}

void testCandidateClassificationRejectsRetargetedSymlink()
{
    TemporaryDirectory directory;
    const auto first = directory.path() / "first.bin";
    const auto second = directory.path() / "second.bin";
    const auto selected = directory.path() / "selected.bin";
    writeBytes(first, std::vector<std::uint8_t>(128U * 1024U, 'a'));
    writeBytes(second, std::vector<std::uint8_t>(128U * 1024U, 'b'));

    std::error_code symlinkError;
    std::filesystem::create_symlink(first, selected, symlinkError);
    if (symlinkError)
        return;

    const auto hash = talas::project::calculateProjectAssetSha256(first);
    require(hash.ok(), "retarget fixture should hash");
    const auto manifest = binaryManifest(
        "Samples/Retarget",
        "missing.bin",
        hash.contentHash);
    int cancellationChecks = 0;
    const auto classified = talas::project::classifyProjectAssetCandidate(
        manifest,
        selected,
        std::nullopt,
        [&] {
            ++cancellationChecks;
            if (cancellationChecks != 3)
                return false;
            std::error_code retargetError;
            std::filesystem::remove(selected, retargetError);
            if (!retargetError)
            {
                std::filesystem::create_symlink(
                    second,
                    selected,
                    retargetError);
            }
            require(!retargetError, "symlink retarget fixture should update");
            return false;
        });
    require(
        !classified.ok()
            && classified.error->code
                == talas::project::ProjectAssetInspectionErrorCode::ContentMismatch,
        "candidate classification must reject a selected path retargeted between inspection and stored-path publication");
}

void testDirectorySearchRejectsCandidateRetargetedOutsideRoot()
{
    TemporaryDirectory directory;
    const auto searchRoot = directory.path() / "library";
    const auto candidate = searchRoot / "hit.bin";
    const auto outside = directory.path() / "outside.bin";
    writeBytes(candidate, std::vector<std::uint8_t>(128U * 1024U, 'a'));
    writeBytes(outside, std::vector<std::uint8_t>(128U * 1024U, 'b'));
    const auto hash = talas::project::calculateProjectAssetSha256(candidate);
    require(hash.ok(), "directory retarget fixture should hash");
    const std::vector manifests{
        binaryManifest("Samples/Hit", "missing.bin", hash.contentHash),
    };

    int cancellationChecks = 0;
    const auto result = talas::project::searchProjectAssetDirectory(
        manifests,
        searchRoot,
        std::nullopt,
        {},
        [&] {
            ++cancellationChecks;
            if (cancellationChecks != 7)
                return false;
            std::error_code retargetError;
            std::filesystem::remove(candidate, retargetError);
            if (!retargetError)
            {
                std::filesystem::create_symlink(
                    outside,
                    candidate,
                    retargetError);
            }
            require(!retargetError, "directory candidate should retarget");
            return false;
        });
    require(
        result.ok()
            && result.matches.size() == 1
            && result.matches.front().candidates.empty()
            && !result.diagnostics.empty(),
        "directory search must discard a candidate that retargets outside its canonical root during hashing");
}

void testDirectorySearchRequiresExactAudioMediaShape()
{
    TemporaryDirectory directory;
    const auto searchRoot = directory.path() / "audio-library";
    const auto candidate = searchRoot / "voice.wav";
    writeBytes(candidate, pcm16Wav(1, 48000, 16));
    const auto hash = talas::project::calculateProjectAssetSha256(candidate);
    require(hash.ok(), "Audio directory-search fixture should hash");

    auto manifest = binaryManifest(
        "Audio/Voice",
        "missing.wav",
        hash.contentHash);
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.channelCount = 1;
    manifest.sampleRate = 48000;
    manifest.frameCount = 32;
    manifest.encoding = "pcm16-wav";
    const std::vector mismatched{manifest};
    const auto rejected = talas::project::searchProjectAssetDirectory(
        mismatched,
        searchRoot);
    require(
        rejected.ok()
            && rejected.matches.size() == 1
            && rejected.matches.front().candidates.empty()
            && !rejected.diagnostics.empty(),
        "an exact Audio hash with inconsistent accepted media shape must not publish a relocation candidate");

    manifest.frameCount = 16;
    const std::vector exact{manifest};
    const auto accepted = talas::project::searchProjectAssetDirectory(
        exact,
        searchRoot);
    require(
        accepted.ok()
            && accepted.matches.size() == 1
            && accepted.matches.front().candidates.size() == 1
            && accepted.matches.front().candidates.front().mode
                == talas::project::ProjectAssetResolutionMode::Relocation,
        "an exact Audio hash and media shape should remain a relocation candidate");
}

void testPartialTrackCandidatesRequireCanonicalBytesAndExactShape()
{
    TemporaryDirectory directory;
    const auto searchRoot = directory.path() / "partial-library";
    const auto candidate = searchRoot / "voice.talaspartials";
    talas::project::PartialTrackAsset partials;
    partials.sampleRate = 48000.0;
    partials.sourceFrameCount = 2048;
    partials.frames = {
        {1, 0, {{3, 0.5, 440.0, 8.0}}},
        {1, 1024, {{3, 0.4, 445.0, 9.0}}},
    };
    const auto encoded =
        talas::project::encodePartialTrackAsset(partials);
    require(encoded.ok(), "partial resolver fixture should encode");
    writeBytes(candidate, encoded.bytes);
    const auto inspected = talas::project::inspectProjectAsset(
        {"Analysis/Voice",
         talas::project::ProjectAssetKind::PartialSpectrum,
         "missing.talaspartials"},
        candidate);
    require(inspected.ok(), "partial resolver fixture should inspect");

    auto mismatched = inspected.asset;
    mismatched.path = "missing.talaspartials";
    mismatched.frameCount += 1;
    const auto rejected = talas::project::searchProjectAssetDirectory(
        std::vector{mismatched},
        searchRoot);
    require(rejected.ok()
            && rejected.matches.size() == 1
            && rejected.matches.front().candidates.empty()
            && !rejected.diagnostics.empty(),
        "partial relocation must reject an exact hash with mismatched media shape");

    auto exact = inspected.asset;
    exact.path = "missing.talaspartials";
    const auto accepted = talas::project::searchProjectAssetDirectory(
        std::vector{exact},
        searchRoot);
    require(accepted.ok()
            && accepted.matches.size() == 1
            && accepted.matches.front().candidates.size() == 1
            && accepted.matches.front().candidates.front().asset.kind
                == talas::project::ProjectAssetKind::PartialSpectrum
            && accepted.matches.front().candidates.front().asset.sampleRate
                == 48000.0
            && accepted.matches.front().candidates.front().asset.frameCount
                == 2048,
        "partial relocation should retain exact kind and inspected shape");

    auto malformed = encoded.bytes;
    malformed.push_back(0);
    const auto malformedPath = searchRoot / "malformed.talaspartials";
    writeBytes(malformedPath, malformed);
    auto unverified = exact;
    unverified.contentHash.clear();
    unverified.sampleRate = 0.0;
    unverified.frameCount = 0;
    unverified.encoding.clear();
    const auto classified =
        talas::project::classifyProjectAssetCandidate(
            unverified,
            malformedPath,
            std::nullopt);
    require(!classified.ok()
            && classified.error->code
                == talas::project::ProjectAssetInspectionErrorCode::InvalidMedia,
        "partial candidate classification should reject malformed bytes before publication");
}
}

int main()
{
    testClassifiesExactChangedAndUnverifiedCandidates();
    testClassificationCancellationAndNonRegularPathsAreTyped();
    testCandidateClassificationRejectsRetargetedSymlink();
    testAudioCandidatesPreserveExactShapeAndRejectChangedShape();
    testEmbeddedAssetsAndInvalidRootsRemainFailClosed();
    testDirectorySearchFindsExactSharedAndAmbiguousHashesDeterministically();
    testDirectorySearchCancellationAndCapacityPublishNoPartialMatches();
    testDirectorySearchDoesNotFollowDirectorySymlinks();
    testDirectorySearchRejectsCandidateRetargetedOutsideRoot();
    testDirectorySearchRequiresExactAudioMediaShape();
    testPartialTrackCandidatesRequireCanonicalBytesAndExactShape();
    return 0;
}
