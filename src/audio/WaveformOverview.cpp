#include "talas/audio/WaveformOverview.h"

#include "talas/audio/Pcm16WaveSpan.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace talas::audio
{
namespace
{
[[nodiscard]] WaveformOverviewResult failure(std::string message)
{
    WaveformOverviewResult result;
    result.error = std::move(message);
    return result;
}

[[nodiscard]] WaveformOverviewResult cancellation()
{
    WaveformOverviewResult result;
    result.error = "Waveform overview generation cancelled";
    result.cancelled = true;
    return result;
}

constexpr std::uint16_t pcmFormat = 1;
constexpr std::uint16_t pcmBitsPerSample = 16;
constexpr std::uint16_t pcmBytesPerSample = 2;
constexpr std::size_t streamingBufferBytes = 64U * 1024U;

[[nodiscard]] bool hasAscii(
    const unsigned char* bytes,
    std::string_view expected)
{
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (bytes[index] != static_cast<unsigned char>(expected[index]))
            return false;
    }
    return true;
}

[[nodiscard]] std::uint16_t readU16(const unsigned char* bytes)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

[[nodiscard]] std::uint32_t readU32(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool readExact(
    std::ifstream& input,
    unsigned char* destination,
    std::size_t byteCount)
{
    input.read(
        reinterpret_cast<char*>(destination),
        static_cast<std::streamsize>(byteCount));
    return input.gcount() == static_cast<std::streamsize>(byteCount);
}

[[nodiscard]] double decodePcm16(std::int16_t sample)
{
    return pcm16SampleValue(sample);
}

struct Pcm16WaveformSource
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t blockAlign = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataSize = 0;
};

struct Pcm16WaveformSourceResult
{
    Pcm16WaveformSource source;
    std::string error;
    bool cancelled = false;
};

[[nodiscard]] Pcm16WaveformSourceResult inspectPcm16WaveformSource(
    std::ifstream& input,
    std::uint64_t fileSize,
    const WaveformOverviewCancellationCheck& shouldCancel)
{
    Pcm16WaveformSourceResult result;
    std::array<unsigned char, 12> riff{};
    if (fileSize < riff.size() || !readExact(input, riff.data(), riff.size()))
    {
        result.error = "WAV file is not a RIFF/WAVE container";
        return result;
    }
    if (!hasAscii(riff.data(), "RIFF")
        || !hasAscii(riff.data() + 8, "WAVE"))
    {
        result.error = "WAV file is not a RIFF/WAVE container";
        return result;
    }

    bool hasFmt = false;
    bool hasData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t parsedBitsPerSample = 0;
    std::uint64_t offset = riff.size();
    while (offset + 8U <= fileSize)
    {
        if (shouldCancel && shouldCancel())
        {
            result.cancelled = true;
            result.error = "Waveform overview generation cancelled";
            return result;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        std::array<unsigned char, 8> chunkHeader{};
        if (!input
            || !readExact(
                input,
                chunkHeader.data(),
                chunkHeader.size()))
        {
            result.error = "Failed while reading WAV chunk header";
            return result;
        }
        const auto chunkSize = readU32(chunkHeader.data() + 4);
        const auto chunkDataOffset = offset + chunkHeader.size();
        const auto chunkEnd = chunkDataOffset
            + static_cast<std::uint64_t>(chunkSize);
        if (chunkEnd < chunkDataOffset || chunkEnd > fileSize)
        {
            result.error = "WAV chunk extends past end of file";
            return result;
        }

        if (hasAscii(chunkHeader.data(), "fmt "))
        {
            if (chunkSize < 16U)
            {
                result.error = "WAV fmt chunk is too short";
                return result;
            }
            std::array<unsigned char, 16> format{};
            if (!readExact(input, format.data(), format.size()))
            {
                result.error = "Failed while reading WAV fmt chunk";
                return result;
            }
            audioFormat = readU16(format.data());
            result.source.channelCount = readU16(format.data() + 2);
            result.source.sampleRate = readU32(format.data() + 4);
            result.source.blockAlign = readU16(format.data() + 12);
            parsedBitsPerSample = readU16(format.data() + 14);
            hasFmt = true;
        }
        else if (hasAscii(chunkHeader.data(), "data"))
        {
            result.source.dataOffset = chunkDataOffset;
            result.source.dataSize = chunkSize;
            hasData = true;
        }
        offset = chunkEnd + (chunkSize % 2U);
    }

    if (!hasFmt)
        result.error = "WAV file is missing fmt chunk";
    else if (!hasData)
        result.error = "WAV file is missing data chunk";
    else if (audioFormat != pcmFormat
             || parsedBitsPerSample != pcmBitsPerSample)
        result.error = "WAV reader supports PCM16 files only";
    else if (result.source.sampleRate == 0)
        result.error = "Waveform overview requires a positive sample rate";
    else if (result.source.channelCount == 0
             || result.source.channelCount > 2)
        result.error = "Waveform overview requires mono or stereo samples";
    else if (result.source.blockAlign
             != result.source.channelCount * pcmBytesPerSample)
        result.error = "WAV block alignment is inconsistent with PCM16 channel count";
    else if (result.source.dataSize % result.source.blockAlign != 0)
        result.error = "WAV data chunk is not frame-aligned";
    return result;
}

[[nodiscard]] WaveformOverviewResult streamPcm16WaveformOverview(
    std::ifstream& input,
    const Pcm16WaveformSource& source,
    WaveformOverviewRequest request,
    WaveformOverviewCancellationCheck shouldCancel)
{
    const auto frameCount = source.dataSize / source.blockAlign;
    auto overview = std::make_shared<WaveformOverview>();
    overview->assetId = std::move(request.assetId);
    overview->generation = request.generation;
    overview->contentHash = std::move(request.contentHash);
    overview->sampleRate = source.sampleRate;
    overview->channelCount = source.channelCount;
    overview->frameCount = frameCount;

    const auto bucketCount = std::min<std::uint64_t>(
        request.maximumBuckets,
        frameCount);
    overview->buckets.reserve(static_cast<std::size_t>(bucketCount));
    const auto framesPerBucket = bucketCount == 0
        ? std::uint64_t{0}
        : frameCount / bucketCount;
    const auto remainderFrames = bucketCount == 0
        ? std::uint64_t{0}
        : frameCount % bucketCount;
    const auto boundaryFrame = [&](std::uint64_t boundary) {
        return boundary * framesPerBucket
            + boundary * remainderFrames / bucketCount;
    };
    for (std::uint64_t index = 0; index < bucketCount; ++index)
    {
        WaveformOverviewBucket bucket;
        bucket.firstFrame = boundaryFrame(index);
        bucket.endFrameExclusive = boundaryFrame(index + 1U);
        bucket.channels.reserve(source.channelCount);
        for (std::uint16_t channel = 0;
             channel < source.channelCount;
             ++channel)
        {
            bucket.channels.push_back({
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
            });
        }
        overview->buckets.push_back(std::move(bucket));
    }
    if (frameCount == 0)
    {
        WaveformOverviewResult result;
        result.overview = std::move(overview);
        return result;
    }

    input.clear();
    input.seekg(static_cast<std::streamoff>(source.dataOffset));
    if (!input)
        return failure("Failed to seek to WAV sample data");
    std::array<unsigned char, streamingBufferBytes> buffer{};
    const auto bufferCapacity = buffer.size()
        - buffer.size() % source.blockAlign;
    auto remainingBytes = source.dataSize;
    std::uint64_t frame = 0;
    std::size_t bucketIndex = 0;
    while (remainingBytes > 0)
    {
        if (shouldCancel && shouldCancel())
            return cancellation();
        const auto byteCount = static_cast<std::size_t>(
            std::min<std::uint64_t>(remainingBytes, bufferCapacity));
        if (!readExact(input, buffer.data(), byteCount))
            return failure("Failed while streaming WAV sample data");
        for (std::size_t byteOffset = 0;
             byteOffset < byteCount;
             byteOffset += source.blockAlign, ++frame)
        {
            while (frame
                   >= overview->buckets[bucketIndex].endFrameExclusive)
            {
                ++bucketIndex;
            }
            auto& bucket = overview->buckets[bucketIndex];
            for (std::uint16_t channel = 0;
                 channel < source.channelCount;
                 ++channel)
            {
                const auto sample = static_cast<std::int16_t>(readU16(
                    buffer.data() + byteOffset
                    + channel * pcmBytesPerSample));
                const auto value = decodePcm16(sample);
                auto& range = bucket.channels[channel];
                range.minimum = std::min(range.minimum, value);
                range.maximum = std::max(range.maximum, value);
            }
        }
        remainingBytes -= byteCount;
    }
    WaveformOverviewResult result;
    result.overview = std::move(overview);
    return result;
}
}

WaveformOverviewResult makeWaveformOverview(
    const WaveformOverviewRequest& request,
    std::uint32_t sampleRate,
    const std::vector<std::vector<double>>& channels,
    WaveformOverviewCancellationCheck shouldCancel)
{
    if (request.assetId.empty())
        return failure("Waveform overview requires an asset identity");
    if (request.generation == 0)
        return failure("Waveform overview generation must be positive");
    if (request.maximumBuckets == 0
        || request.maximumBuckets > maximumWaveformOverviewBuckets)
    {
        return failure("Waveform overview bucket limit is unsupported");
    }
    if (sampleRate == 0)
        return failure("Waveform overview requires a positive sample rate");
    if (channels.empty() || channels.size() > 2)
        return failure("Waveform overview requires mono or stereo samples");
    const auto frameCount = channels.front().size();
    for (const auto& channel : channels)
    {
        if (channel.size() != frameCount)
            return failure("Waveform overview channel lengths must match");
    }
    if (shouldCancel && shouldCancel())
        return cancellation();

    auto overview = std::make_shared<WaveformOverview>();
    overview->assetId = request.assetId;
    overview->generation = request.generation;
    overview->contentHash = request.contentHash;
    overview->sampleRate = sampleRate;
    overview->channelCount = static_cast<std::uint32_t>(channels.size());
    overview->frameCount = frameCount;

    const auto bucketCount = std::min(
        request.maximumBuckets,
        frameCount);
    overview->buckets.reserve(bucketCount);
    const auto framesPerBucket = bucketCount == 0
        ? std::size_t{0}
        : frameCount / bucketCount;
    const auto remainderFrames = bucketCount == 0
        ? std::size_t{0}
        : frameCount % bucketCount;
    const auto boundaryFrame = [&](std::size_t bucketBoundary) {
        return bucketBoundary * framesPerBucket
            + bucketBoundary * remainderFrames / bucketCount;
    };
    for (std::size_t bucketIndex = 0;
         bucketIndex < bucketCount;
         ++bucketIndex)
    {
        if (shouldCancel && shouldCancel())
            return cancellation();
        const auto firstFrame = boundaryFrame(bucketIndex);
        const auto endFrame = boundaryFrame(bucketIndex + 1U);
        WaveformOverviewBucket bucket;
        bucket.firstFrame = firstFrame;
        bucket.endFrameExclusive = endFrame;
        bucket.channels.reserve(channels.size());
        for (const auto& channel : channels)
        {
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            for (std::size_t frame = firstFrame; frame < endFrame; ++frame)
            {
                if ((frame - firstFrame) % 4096U == 0U
                    && shouldCancel && shouldCancel())
                {
                    return cancellation();
                }
                const auto sample = channel[frame];
                if (!std::isfinite(sample))
                    return failure("Waveform overview samples must be finite");
                minimum = std::min(minimum, sample);
                maximum = std::max(maximum, sample);
            }
            bucket.channels.push_back({minimum, maximum});
        }
        overview->buckets.push_back(std::move(bucket));
    }

    WaveformOverviewResult result;
    result.overview = std::move(overview);
    return result;
}

WaveformOverviewResult loadPcm16WaveformOverview(
    const std::filesystem::path& path,
    const WaveformOverviewRequest& request,
    WaveformOverviewCancellationCheck shouldCancel)
{
    if (request.assetId.empty())
        return failure("Waveform overview requires an asset identity");
    if (request.generation == 0)
        return failure("Waveform overview generation must be positive");
    if (request.maximumBuckets == 0
        || request.maximumBuckets > maximumWaveformOverviewBuckets)
    {
        return failure("Waveform overview bucket limit is unsupported");
    }
    auto hash = talas::project::calculateProjectAssetSha256(
        path,
        shouldCancel);
    if (!hash.ok())
    {
        auto result = failure(hash.error->message);
        result.cancelled = hash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    if (!request.contentHash.empty()
        && request.contentHash != hash.contentHash)
    {
        return failure(
            "Waveform overview source bytes do not match the requested immutable generation");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return failure(
            "failed to read WAV file: "
            + talas::project::pathToUtf8(path));
    }
    input.seekg(0, std::ios::end);
    const auto endPosition = input.tellg();
    if (endPosition < 0)
        return failure("Unable to determine WAV file size");
    const auto fileSize = static_cast<std::uint64_t>(endPosition);
    input.seekg(0, std::ios::beg);
    auto inspected = inspectPcm16WaveformSource(
        input,
        fileSize,
        shouldCancel);
    if (!inspected.error.empty())
    {
        auto result = failure(std::move(inspected.error));
        result.cancelled = inspected.cancelled;
        return result;
    }
    auto taggedRequest = request;
    if (taggedRequest.contentHash.empty())
        taggedRequest.contentHash = hash.contentHash;
    auto streamed = streamPcm16WaveformOverview(
        input,
        inspected.source,
        std::move(taggedRequest),
        shouldCancel);
    if (!streamed.ok())
        return streamed;
    const auto stableHash = talas::project::calculateProjectAssetSha256(
        path,
        std::move(shouldCancel));
    if (!stableHash.ok())
    {
        auto result = failure(stableHash.error->message);
        result.cancelled = stableHash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    if (stableHash.contentHash != hash.contentHash)
    {
        return failure(
            "Waveform overview source changed during streaming");
    }
    return streamed;
}

bool waveformOverviewMatches(
    const WaveformOverview& overview,
    std::string_view assetId,
    std::uint64_t generation) noexcept
{
    return overview.assetId == assetId
        && overview.generation == generation;
}
}
