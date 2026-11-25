#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectValidation.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
                / ("talas-asset-inspection-"
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
    const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
        throw std::runtime_error("unable to write test fixture");
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

std::vector<std::uint8_t> pcm16WavWithLeadingEmptyChunks(
    std::size_t chunkCount)
{
    const auto base = pcm16Wav(1, 48000, 1);
    std::vector<std::uint8_t> bytes(
        base.size() + chunkCount * 8U,
        0U);
    for (std::size_t index = 0; index < 12U; ++index)
        bytes[index] = base[index];
    auto offset = std::size_t{12};
    for (std::size_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        writeAscii(bytes, offset, "JUNK");
        writeU32(bytes, offset + 4U, 0U);
        offset += 8U;
    }
    for (std::size_t index = 12U; index < base.size(); ++index)
        bytes[offset + index - 12U] = base[index];
    writeU32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8U));
    return bytes;
}

void testSha256UsesCanonicalLowercaseVectors()
{
    TemporaryDirectory directory;
    const auto empty = directory.path() / "empty.bin";
    const auto abc = directory.path() / "abc.bin";
    writeBytes(empty, {});
    writeBytes(abc, {'a', 'b', 'c'});

    const auto emptyHash =
        talas::project::calculateProjectAssetSha256(empty);
    const auto abcHash =
        talas::project::calculateProjectAssetSha256(abc);
    require(
        emptyHash.ok()
            && emptyHash.contentHash
                == "e3b0c44298fc1c149afbf4c8996fb924"
                   "27ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector should match the standard digest");
    require(
        abcHash.ok()
            && abcHash.contentHash
                == "ba7816bf8f01cfea414140de5dae2223"
                   "b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector should match the standard digest");
    const std::vector<std::uint8_t> abcBytes = {'a', 'b', 'c'};
    require(
        talas::project::calculateProjectAssetSha256(abcBytes)
            == abcHash.contentHash,
        "in-memory SHA-256 should use the same canonical byte contract as file hashing");
    require(
        talas::project::isCanonicalProjectAssetContentHash(
            abcHash.contentHash)
            && !talas::project::isCanonicalProjectAssetContentHash(
                "BA7816BF8F01CFEA414140DE5DAE2223"
                "B00361A396177A9CB410FF61F20015AD"),
        "manifest hashes should accept lowercase canonical SHA-256 only");
}

// Sample files of other word sizes, as other programs write them: a RIFF/WAVE
// file of one format, or an AIFF file at 48 kHz.
void appendUnsigned(
    std::vector<std::uint8_t>& bytes,
    std::uint64_t value,
    std::size_t count,
    bool bigEndian = false)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto shift = 8U * (bigEndian ? count - 1U - index : index);
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendTag(std::vector<std::uint8_t>& bytes, std::string_view tag)
{
    bytes.insert(bytes.end(), tag.begin(), tag.end());
}

std::vector<std::uint8_t> sampleWave(
    std::uint16_t format,
    std::uint16_t channels,
    std::uint16_t bits,
    const std::vector<std::uint8_t>& data)
{
    const auto blockAlign = static_cast<std::uint16_t>(channels * bits / 8U);
    std::vector<std::uint8_t> bytes;
    appendTag(bytes, "RIFF");
    appendUnsigned(bytes, 36U + data.size() + (data.size() & 1U), 4);
    appendTag(bytes, "WAVE");
    appendTag(bytes, "fmt ");
    appendUnsigned(bytes, 16, 4);
    appendUnsigned(bytes, format, 2);
    appendUnsigned(bytes, channels, 2);
    appendUnsigned(bytes, 48000, 4);
    appendUnsigned(bytes, 48000U * blockAlign, 4);
    appendUnsigned(bytes, blockAlign, 2);
    appendUnsigned(bytes, bits, 2);
    appendTag(bytes, "data");
    appendUnsigned(bytes, data.size(), 4);
    bytes.insert(bytes.end(), data.begin(), data.end());
    if ((data.size() & 1U) != 0U)
        bytes.push_back(0);
    return bytes;
}

std::vector<std::uint8_t> sampleAiff(
    std::uint16_t channels,
    std::uint16_t bits,
    const std::vector<std::uint8_t>& data)
{
    const auto frames = data.size() / (channels * ((bits + 7U) / 8U));
    std::vector<std::uint8_t> bytes;
    appendTag(bytes, "FORM");
    appendUnsigned(bytes, 4U + 26U + 16U + data.size(), 4, true);
    appendTag(bytes, "AIFF");
    appendTag(bytes, "COMM");
    appendUnsigned(bytes, 18, 4, true);
    appendUnsigned(bytes, channels, 2, true);
    appendUnsigned(bytes, frames, 4, true);
    appendUnsigned(bytes, bits, 2, true);
    // 48000 as an 80-bit extended float.
    appendUnsigned(bytes, 0x400EU, 2, true);
    appendUnsigned(bytes, 0xBB80000000000000ULL, 8, true);
    appendTag(bytes, "SSND");
    appendUnsigned(bytes, 8U + data.size(), 4, true);
    appendUnsigned(bytes, 0, 8, true);
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

void testAudioInspectionFillsUnknownMetadataWithoutChangingIdentity()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "stereo.wav";
    const auto bytes = pcm16Wav(2, 48000, 2);
    writeBytes(path, bytes);
    talas::project::ProjectAsset asset;
    asset.id = "Audio/Take";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "media/stereo.wav";
    asset.generation = 7;
    asset.provenance = "local capture";

    const auto inspected =
        talas::project::inspectProjectAsset(asset, path);
    require(inspected.ok(), "valid PCM16 WAV should inspect");
    require(
        inspected.asset.id == asset.id
            && inspected.asset.path == asset.path
            && inspected.asset.generation == 7
            && inspected.asset.provenance == "local capture",
        "inspection must preserve project identity, path, generation, and provenance");
    require(
        inspected.asset.contentHash.size() == 64
            && inspected.asset.channelCount == 2
            && inspected.asset.sampleRate == 48000.0
            && inspected.asset.frameCount == 2
            && inspected.asset.encoding
                == talas::project::pcm16WavAssetEncoding,
        "inspection should fill the canonical audio media shape");

    const auto verified =
        talas::project::inspectProjectAsset(inspected.asset, path);
    require(verified.ok() && verified.asset == inspected.asset,
            "reinspection should verify a complete manifest exactly");
    const auto snapshotVerified = talas::project::inspectProjectAsset(
        inspected.asset,
        bytes,
        path);
    require(snapshotVerified.ok()
            && snapshotVerified.asset == inspected.asset,
        "immutable byte-snapshot inspection should preserve the exact file-backed audio contract");
}

void testInspectionFailsExplicitlyForChangedBytesAndShape()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "mono.wav";
    writeBytes(path, pcm16Wav(1, 44100, 3));
    talas::project::ProjectAsset asset;
    asset.id = "Audio/Take";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "mono.wav";
    const auto inspected =
        talas::project::inspectProjectAsset(asset, path);
    require(inspected.ok(), "baseline audio inspection should succeed");

    auto wrongHash = inspected.asset;
    wrongHash.contentHash.assign(64, '0');
    const auto hashMismatch =
        talas::project::inspectProjectAsset(wrongHash, path);
    require(
        !hashMismatch.ok()
            && hashMismatch.error->code
                == talas::project::ProjectAssetInspectionErrorCode::ContentMismatch,
        "an accepted but different content hash should fail explicitly");

    auto wrongShape = inspected.asset;
    wrongShape.channelCount = 2;
    const auto shapeMismatch =
        talas::project::inspectProjectAsset(wrongShape, path);
    require(
        !shapeMismatch.ok()
            && shapeMismatch.error->code
                == talas::project::ProjectAssetInspectionErrorCode::MediaMismatch,
        "accepted media shape drift should fail explicitly");

    const auto missing = talas::project::calculateProjectAssetSha256(
        directory.path() / "missing.bin");
    require(
        !missing.ok()
            && missing.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Unavailable,
        "missing local bytes should have a typed unavailable failure");

    int cancellationChecks = 0;
    const auto cancelled =
        talas::project::calculateProjectAssetSha256(
            path,
            [&] { return ++cancellationChecks >= 2; });
    require(
        !cancelled.ok()
            && cancelled.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled,
        "streaming SHA-256 should expose cooperative cancellation explicitly");
}

void testAudioInspectionRejectsFormatsOutsideTheInitialContract()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "surround.wav";
    writeBytes(path, pcm16Wav(3, 48000, 1));
    talas::project::ProjectAsset asset;
    asset.id = "Audio/Surround";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "surround.wav";
    const auto inspected =
        talas::project::inspectProjectAsset(asset, path);
    require(
        !inspected.ok()
            && inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::UnsupportedMedia,
        "format-10 inspection should fail closed outside mono/stereo PCM16 WAV");
}

// Audio assets may be any WAV or AIFF sample file a DiskPlayer streams
//; canonical PCM16 WAV keeps its own encoding.
void testAudioInspectionAcceptsWavAndAiffSamplesOfOtherWordSizes()
{
    TemporaryDirectory directory;
    const auto inspect = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
        const auto path = directory.path() / name;
        writeBytes(path, bytes);
        talas::project::ProjectAsset asset;
        asset.id = "Audio/" + name;
        asset.kind = talas::project::ProjectAssetKind::Audio;
        asset.path = name;
        auto inspected = talas::project::inspectProjectAsset(asset, path);
        if (inspected.ok())
        {
            const auto again = talas::project::inspectProjectAsset(inspected.asset, bytes, path);
            require(again.ok() && again.asset == inspected.asset,
                name + " should inspect alike from its bytes");
        }
        return inspected;
    };
    const auto twentyFour = inspect("stereo24.wav",
        sampleWave(1, 2, 24, {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40}));
    require(twentyFour.ok() && twentyFour.asset.channelCount == 2
            && twentyFour.asset.sampleRate == 48000.0 && twentyFour.asset.frameCount == 2
            && twentyFour.asset.encoding == "sample-file",
        "a 24-bit stereo WAV should inspect as a sample file");
    const auto aiff = inspect("mono16.aiff", sampleAiff(1, 16, {0x40, 0x00, 0x80, 0x00}));
    require(aiff.ok() && aiff.asset.channelCount == 1 && aiff.asset.frameCount == 2
            && aiff.asset.encoding == "sample-file",
        "a 16-bit AIFF should inspect as a sample file");
    const auto canonical = inspect("mono16.wav", sampleWave(1, 1, 16, {0x00, 0x40}));
    require(canonical.ok() && canonical.asset.encoding == talas::project::pcm16WavAssetEncoding,
        "canonical PCM16 WAV should keep its encoding");
    auto claimed = twentyFour.asset;
    claimed.encoding = std::string{talas::project::pcm16WavAssetEncoding};
    require(!talas::project::inspectProjectAsset(claimed, directory.path() / "stereo24.wav").ok(),
        "a sample file claimed as PCM16 WAV should not verify");
    const auto surround = inspect("surround.wav", sampleWave(1, 3, 24, std::vector<std::uint8_t>(9, 0)));
    require(!surround.ok()
            && surround.error->code == talas::project::ProjectAssetInspectionErrorCode::UnsupportedMedia,
        "a sample file of more than two channels should stay unsupported");
}

void testAudioInspectionRejectsBytesPastTheRiffContainer()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "trailing.wav";
    auto bytes = pcm16Wav(1, 48000, 1);
    bytes.push_back(0x7f);
    writeBytes(path, bytes);

    talas::project::ProjectAsset asset;
    asset.id = "Audio/Trailing";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "trailing.wav";
    const auto inspected =
        talas::project::inspectProjectAsset(asset, path);
    require(
        !inspected.ok()
            && inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::InvalidMedia
            && inspected.error->message.find("size")
                != std::string::npos,
        "strict WAV inspection should reject bytes beyond the declared RIFF container");
}

void testAudioProbeChecksCooperativeCancellationBetweenChunks()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "many-chunks.wav";
    writeBytes(path, pcm16WavWithLeadingEmptyChunks(64));
    talas::project::ProjectAsset asset;
    asset.id = "Audio/ManyChunks";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "many-chunks.wav";

    int cancellationChecks = 0;
    const auto inspected = talas::project::inspectProjectAsset(
        asset,
        path,
        [&] { return ++cancellationChecks >= 8; });
    require(
        !inspected.ok()
            && inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled
            && cancellationChecks == 8,
        "WAV metadata probing should cooperatively cancel between bounded chunk reads");
}

void testPartialTrackInspectionUsesOneCanonicalByteSnapshot()
{
    talas::project::PartialTrackAsset partials;
    partials.sampleRate = 48000.0;
    partials.sourceFrameCount = 4096;
    partials.frames = {
        {1, 0, {{1, 0.5, 440.0, 10.0}}},
        {1, 1024, {{1, 0.4, 445.0, 11.0},
                   {2, 0.2, 880.0, 20.0}}},
    };
    const auto encoded =
        talas::project::encodePartialTrackAsset(partials);
    require(encoded.ok(), "partial-track inspection fixture should encode");

    TemporaryDirectory directory;
    const auto path = directory.path() / "analysis.talaspartials";
    writeBytes(path, encoded.bytes);
    talas::project::ProjectAsset manifest;
    manifest.id = "Analysis/Voice";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "analysis.talaspartials";

    const auto inspected =
        talas::project::inspectProjectAsset(manifest, path);
    require(inspected.ok()
            && inspected.asset.channelCount == 0
            && inspected.asset.sampleRate == partials.sampleRate
            && inspected.asset.frameCount == partials.sourceFrameCount
            && inspected.asset.encoding
                == talas::project::partialTrackAssetEncoding
            && !inspected.asset.contentHash.empty(),
        "partial-track inspection should fill exact canonical shape and hash");
    const auto snapshot = talas::project::inspectProjectAsset(
        inspected.asset,
        encoded.bytes,
        path);
    require(snapshot.ok() && snapshot.asset == inspected.asset,
            "file and immutable-snapshot partial inspection should agree exactly");

    const auto symlinkPath =
        directory.path() / "analysis-link.talaspartials";
    std::error_code symlinkError;
    std::filesystem::create_symlink(path, symlinkPath, symlinkError);
    require(!symlinkError, "partial inspection symlink fixture should create");
    const auto symlinked = talas::project::inspectProjectAsset(
        manifest,
        symlinkPath);
    require(!symlinked.ok()
            && symlinked.error->code
                == talas::project::ProjectAssetInspectionErrorCode::NotRegularFile,
        "partial inspection should fail closed on a symlinked source path");

    auto wrongRate = inspected.asset;
    wrongRate.sampleRate = 44100.0;
    const auto mismatch = talas::project::inspectProjectAsset(
        wrongRate,
        encoded.bytes,
        path);
    require(!mismatch.ok()
            && mismatch.error->code
                == talas::project::ProjectAssetInspectionErrorCode::MediaMismatch,
        "accepted partial-track shape drift should fail explicitly");

    auto malformed = encoded.bytes;
    malformed.push_back(0);
    const auto invalid = talas::project::inspectProjectAsset(
        manifest,
        malformed,
        path);
    require(!invalid.ok()
            && invalid.error->code
                == talas::project::ProjectAssetInspectionErrorCode::InvalidMedia,
        "malformed partial-track bytes should fail media inspection");

    auto invalidManifest = manifest;
    invalidManifest.channelCount = 1;
    require(talas::project::validateProjectAssetManifest(invalidManifest)
                .has_value(),
        "partial-track manifests should reject audio channel metadata");

    talas::project::ProjectDocument currentDocument;
    currentDocument.formatVersion = 21;
    currentDocument.assets.push_back(inspected.asset);
    const auto persistence =
        talas::project::validateProjectDocumentForPersistence(
            currentDocument);
    require(!persistence.ok(),
            "format 21 persistence must reject premature partial-track assets until format 22 owns their edits");
}
}

int main()
{
    testSha256UsesCanonicalLowercaseVectors();
    testAudioInspectionFillsUnknownMetadataWithoutChangingIdentity();
    testInspectionFailsExplicitlyForChangedBytesAndShape();
    testAudioInspectionRejectsFormatsOutsideTheInitialContract();
    testAudioInspectionAcceptsWavAndAiffSamplesOfOtherWordSizes();
    testAudioInspectionRejectsBytesPastTheRiffContainer();
    testAudioProbeChecksCooperativeCancellationBetweenChunks();
    testPartialTrackInspectionUsesOneCanonicalByteSnapshot();
    return 0;
}
