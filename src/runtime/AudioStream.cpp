#include "talas/runtime/AudioStream.h"

#include "talas/audio/Pcm16WaveSpan.h"
#include "talas/core/SampleFileLayout.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectDiskCache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace talas::runtime
{
namespace
{
constexpr std::size_t pcm16BytesPerSample = 2;
// The widest frame a stream reads: two channels of 64-bit float.
constexpr std::size_t maximumBytesPerFrame = 16;

[[nodiscard]] std::uint16_t readU16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t readU32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool hasAscii(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view expected) noexcept
{
    return offset <= bytes.size()
        && expected.size() <= bytes.size() - offset
        && std::equal(
            expected.begin(),
            expected.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

template <std::size_t Size>
[[nodiscard]] bool readExact(
    std::ifstream& input,
    std::array<std::uint8_t, Size>& destination)
{
    input.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(destination.size()));
    return input.gcount()
        == static_cast<std::streamsize>(destination.size());
}

struct Pcm16Layout
{
    std::uint16_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::uint64_t dataOffset = 0;
    std::uint16_t blockAlign = 0;
    // A sample file's own words, where the backing is not canonical PCM16
    // WAV.
    std::optional<talas::core::SampleWordEncoding> sampleWords;
    // The channel decoded (both averaged, or one of two).
    AudioStreamChannel channel = AudioStreamChannel::Mix;
};

[[nodiscard]] std::optional<Pcm16Layout> probePcm16Wav(
    const std::filesystem::path& path,
    std::string& error)
{
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        error = "Unable to measure immutable PCM16-WAV backing: "
            + sizeError.message();
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 12> riff{};
    if (!input || !readExact(input, riff))
    {
        error = "Audio stream backing does not contain a complete RIFF/WAVE header";
        return std::nullopt;
    }
    if (!hasAscii(riff, 0, "RIFF")
        || !hasAscii(riff, 8, "WAVE"))
    {
        error = "Audio stream backing is not a RIFF/WAVE container";
        return std::nullopt;
    }
    const auto declaredEnd =
        static_cast<std::uint64_t>(readU32(riff, 4)) + 8U;
    if (declaredEnd < 12U || declaredEnd != fileSize)
    {
        error = "PCM16-WAV RIFF size is inconsistent with the immutable backing";
        return std::nullopt;
    }

    bool foundFormat = false;
    bool foundData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint32_t dataBytes = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t offset = 12;
    while (offset + 8U <= declaredEnd)
    {
        input.seekg(static_cast<std::streamoff>(offset));
        std::array<std::uint8_t, 8> chunkHeader{};
        if (!input || !readExact(input, chunkHeader))
        {
            error = "Unable to read PCM16-WAV chunk header";
            return std::nullopt;
        }
        const auto chunkBytes = readU32(chunkHeader, 4);
        const auto chunkDataOffset = offset + 8U;
        const auto chunkEnd = chunkDataOffset
            + static_cast<std::uint64_t>(chunkBytes);
        if (chunkEnd > declaredEnd)
        {
            error = "PCM16-WAV chunk extends past its RIFF container";
            return std::nullopt;
        }

        if (hasAscii(chunkHeader, 0, "fmt "))
        {
            if (foundFormat || chunkBytes < 16U)
            {
                error = foundFormat
                    ? "PCM16-WAV contains duplicate fmt chunks"
                    : "PCM16-WAV fmt chunk is too short";
                return std::nullopt;
            }
            std::array<std::uint8_t, 16> format{};
            if (!readExact(input, format))
            {
                error = "Unable to read PCM16-WAV format metadata";
                return std::nullopt;
            }
            audioFormat = readU16(format, 0);
            channelCount = readU16(format, 2);
            sampleRate = readU32(format, 4);
            byteRate = readU32(format, 8);
            blockAlign = readU16(format, 12);
            bitsPerSample = readU16(format, 14);
            foundFormat = true;
        }
        else if (hasAscii(chunkHeader, 0, "data"))
        {
            if (foundData)
            {
                error = "PCM16-WAV contains duplicate data chunks";
                return std::nullopt;
            }
            dataBytes = chunkBytes;
            dataOffset = chunkDataOffset;
            foundData = true;
        }

        offset = chunkEnd
            + static_cast<std::uint64_t>(chunkBytes & 1U);
        if (offset > declaredEnd)
        {
            error = "PCM16-WAV chunk padding extends past its RIFF container";
            return std::nullopt;
        }
    }
    if (offset != declaredEnd)
    {
        error = "PCM16-WAV container has an incomplete trailing chunk header";
        return std::nullopt;
    }
    if (!foundFormat || !foundData)
    {
        error = !foundFormat
            ? "PCM16-WAV file is missing a fmt chunk"
            : "PCM16-WAV file is missing a data chunk";
        return std::nullopt;
    }
    if (audioFormat != 1U || bitsPerSample != 16U)
    {
        error = "Audio stream supports uncompressed PCM16-WAV only";
        return std::nullopt;
    }
    if (channelCount == 0U || channelCount > 2U
        || sampleRate == 0U)
    {
        error = "PCM16-WAV stream requires one or two channels and a positive sample rate";
        return std::nullopt;
    }
    const auto expectedBlockAlign =
        static_cast<std::uint32_t>(channelCount)
        * pcm16BytesPerSample;
    const auto expectedByteRate =
        static_cast<std::uint64_t>(sampleRate)
        * expectedBlockAlign;
    if (blockAlign != expectedBlockAlign
        || expectedByteRate
            > std::numeric_limits<std::uint32_t>::max()
        || byteRate != expectedByteRate
        || dataBytes == 0U
        || dataBytes % expectedBlockAlign != 0U)
    {
        error = "PCM16-WAV stream layout is internally inconsistent";
        return std::nullopt;
    }

    return Pcm16Layout{
        channelCount,
        static_cast<double>(sampleRate),
        dataBytes / expectedBlockAlign,
        dataOffset,
        blockAlign,
        std::nullopt,
        AudioStreamChannel::Mix,
    };
}

// The layout of a mono or stereo WAV or AIFF sample file of any word size.
[[nodiscard]] std::optional<Pcm16Layout> probeSampleFile(
    const std::filesystem::path& path,
    std::string& error)
{
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    std::ifstream input(path, std::ios::binary);
    if (sizeError || !input)
    {
        error = "Unable to open immutable sample file backing";
        return std::nullopt;
    }
    const auto layout = talas::core::probeSampleFileLayout(
        fileSize,
        [&](std::uint64_t offset, std::span<std::uint8_t> bytes) {
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            return input.gcount()
                == static_cast<std::streamsize>(bytes.size());
        },
        error);
    if (!layout.has_value())
        return std::nullopt;
    if (layout->channelCount > 2U)
    {
        error = "Audio stream plays sample files of one or two channels";
        return std::nullopt;
    }
    return Pcm16Layout{
        layout->channelCount,
        layout->sampleRate,
        layout->frameCount,
        layout->dataOffset,
        layout->blockAlign,
        layout->encoding,
        AudioStreamChannel::Mix,
    };
}

[[nodiscard]] bool decodePcm16MonoFrames(
    std::span<const std::uint8_t> bytes,
    const Pcm16Layout& source,
    std::size_t frameCount,
    std::span<double> destination) noexcept
{
    const auto channelCount = source.channelCount;
    const auto blockAlign = source.blockAlign;
    if (destination.size() < frameCount)
        return false;
    // A sample file's words, each frame's channels averaged as PCM16 ones
    // are.
    if (source.sampleWords.has_value())
    {
        const auto words = *source.sampleWords;
        if ((channelCount != 1U && channelCount != 2U)
            || blockAlign != channelCount * words.bytesPerSample
            || frameCount > bytes.size() / blockAlign)
        {
            return false;
        }
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            auto sum = 0.0;
            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                const auto value = talas::core::decodeSampleWord(
                    bytes.subspan(frame * blockAlign + channel * words.bytesPerSample,
                        words.bytesPerSample),
                    words);
                if (!std::isfinite(value))
                    return false;
                sum += value;
            }
            if (channelCount == 2U && source.channel != AudioStreamChannel::Mix)
            {
                const auto selected = talas::core::decodeSampleWord(
                    bytes.subspan(frame * blockAlign
                            + (source.channel == AudioStreamChannel::Right ? 1U : 0U)
                                * words.bytesPerSample,
                        words.bytesPerSample),
                    words);
                if (!std::isfinite(selected))
                    return false;
                destination[frame] = selected;
                continue;
            }
            destination[frame] = channelCount == 1U ? sum : sum * 0.5;
        }
        return true;
    }
    const talas::audio::Pcm16WaveLayout layout{
        channelCount,
        1U,
        frameCount,
        0U,
        blockAlign,
    };
    std::array<double, 2> channels{};
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        if (!talas::audio::decodePcm16WaveFrame(
                bytes,
                layout,
                frame,
                std::span<double>{channels.data(), channelCount}))
        {
            return false;
        }
        destination[frame] = channelCount == 1U
            ? channels[0]
            : source.channel == AudioStreamChannel::Left ? channels[0]
            : source.channel == AudioStreamChannel::Right ? channels[1]
            : (channels[0] + channels[1]) * 0.5;
    }
    return true;
}

[[nodiscard]] bool isConfigurationValid(
    const AudioStreamConfiguration& configuration,
    std::string& error)
{
    if (configuration.blockFrames == 0
        || configuration.requestCapacity < 2
        || configuration.responseCapacity < 2
        || configuration.decodedCacheBlocks == 0)
    {
        error = "Audio stream requires non-zero bounded storage and queue capacities of at least two";
        return false;
    }
    if (configuration.lowWatermark == 0
        || configuration.lowWatermark
            > configuration.highWatermark
        || configuration.highWatermark
            > configuration.responseCapacity)
    {
        error = "Audio stream low/high watermark configuration is invalid";
        return false;
    }
    const auto maximumFrames =
        std::numeric_limits<std::size_t>::max()
        / maximumBytesPerFrame;
    if (configuration.blockFrames > maximumFrames)
    {
        error = "Audio stream block size exceeds supported allocation limits";
        return false;
    }
    return true;
}

std::atomic<std::uint64_t> temporarySequence = 0;

[[nodiscard]] std::optional<std::filesystem::path>
makePrivateBackingDirectory(std::string& error)
{
    std::error_code tempError;
    const auto root = std::filesystem::temp_directory_path(tempError);
    if (tempError)
    {
        error = "Unable to resolve temporary audio stream storage: "
            + tempError.message();
        return std::nullopt;
    }
    for (std::size_t attempt = 0; attempt < 64; ++attempt)
    {
        const auto sequence = temporarySequence.fetch_add(
            1,
            std::memory_order_relaxed);
        const auto candidate = root
            / ("talas-audio-stream-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count())
               + "-" + std::to_string(sequence));
        std::error_code createError;
        if (std::filesystem::create_directory(
                candidate,
                createError))
        {
            return candidate;
        }
        if (createError
            && createError
                != std::errc::file_exists)
        {
            error = "Unable to create private audio stream storage: "
                + createError.message();
            return std::nullopt;
        }
    }
    error = "Unable to allocate a unique private audio stream storage directory";
    return std::nullopt;
}

[[nodiscard]] bool copyToPrivateBacking(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const AudioStreamCancellationCheck& shouldCancel,
    bool& cancelled,
    std::string& error)
{
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(
        destination,
        std::ios::binary | std::ios::trunc);
    if (!input || !output)
    {
        error = "Unable to open audio asset and private immutable backing";
        return false;
    }
    std::array<char, 64U * 1024U> buffer{};
    while (input)
    {
        if (shouldCancel && shouldCancel())
        {
            cancelled = true;
            error = "Audio stream preparation cancelled";
            return false;
        }
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
            output.write(buffer.data(), count);
    }
    if (!input.eof() || !output.good())
    {
        error = "Unable to copy exact audio asset bytes into immutable backing";
        return false;
    }
    output.close();
    if (!output)
    {
        error = "Unable to finalize immutable audio stream backing";
        return false;
    }
    return true;
}

[[nodiscard]] bool writeRetainedBytesToPrivateBacking(
    std::span<const std::uint8_t> bytes,
    const std::filesystem::path& destination,
    const AudioStreamCancellationCheck& shouldCancel,
    bool& cancelled,
    std::string& error)
{
    std::ofstream output(
        destination,
        std::ios::binary | std::ios::trunc);
    if (!output)
    {
        error = "Unable to open private immutable audio backing";
        return false;
    }
    constexpr std::size_t chunkBytes = 64U * 1024U;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (shouldCancel && shouldCancel())
        {
            cancelled = true;
            error = "Audio stream preparation cancelled";
            return false;
        }
        const auto count = std::min(chunkBytes, bytes.size() - offset);
        output.write(
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::streamsize>(count));
        if (!output.good())
        {
            error = "Unable to write retained audio bytes into immutable backing";
            return false;
        }
        offset += count;
    }
    output.close();
    if (!output)
    {
        error = "Unable to finalize immutable audio stream backing";
        return false;
    }
    return true;
}
}

std::optional<double> audioStreamSourceFrameIncrement(
    double rateScale,
    double sourceSampleRate,
    double destinationSampleRate) noexcept
{
    if (!std::isfinite(rateScale)
        || rateScale <= 0.0
        || !std::isfinite(sourceSampleRate)
        || sourceSampleRate <= 0.0
        || !std::isfinite(destinationSampleRate)
        || destinationSampleRate <= 0.0)
    {
        return std::nullopt;
    }

    int rateExponent = 0;
    int sourceExponent = 0;
    int destinationExponent = 0;
    const auto rateMantissa = std::frexp(
        static_cast<long double>(rateScale),
        &rateExponent);
    const auto sourceMantissa = std::frexp(
        static_cast<long double>(sourceSampleRate),
        &sourceExponent);
    const auto destinationMantissa = std::frexp(
        static_cast<long double>(destinationSampleRate),
        &destinationExponent);
    int mantissaExponent = 0;
    const auto mantissa = std::frexp(
        rateMantissa * sourceMantissa
            / destinationMantissa,
        &mantissaExponent);
    const auto combinedExponent =
        static_cast<long long>(rateExponent)
        + static_cast<long long>(sourceExponent)
        - static_cast<long long>(destinationExponent)
        + static_cast<long long>(mantissaExponent);
    constexpr auto minimumRepresentableExponent =
        std::numeric_limits<double>::min_exponent
        - std::numeric_limits<double>::digits + 1;
    if (combinedExponent
            < minimumRepresentableExponent
        || combinedExponent
            > std::numeric_limits<double>::max_exponent)
    {
        return std::nullopt;
    }

    const auto expanded = std::ldexp(
        mantissa,
        static_cast<int>(combinedExponent));
    if (!std::isfinite(expanded)
        || expanded <= 0.0L
        || expanded
            > static_cast<long double>(
                std::numeric_limits<double>::max()))
    {
        return std::nullopt;
    }
    const auto increment = static_cast<double>(expanded);
    if (!std::isfinite(increment) || increment <= 0.0)
        return std::nullopt;
    return increment;
}

// Owned non-atomic state is declared once so a checkpoint cannot silently omit
// a new cursor, queue payload or worker field. Worker access requires owner lock.
struct AudioStreamConsumerValueState
{
    struct Request
    {
        std::uint64_t generation = 0;
        std::uint64_t firstBlockIndex = 0;
        int direction = 1;
    };

    struct Response
    {
        std::uint64_t generation = 0;
        std::uint64_t blockIndex = 0;
        std::vector<double> samples;
    };

    struct PreparedCursor
    {
        double sourceFrame = 0.0;
        int direction = 1;
    };

    AudioStreamConsumerValueState(AudioStreamConfiguration configuration,
        double initialSourceFrame, double initialSourceFrameIncrement)
        : cursor_(initialSourceFrame),
          direction_(initialSourceFrameIncrement < 0.0 ? -1 : 1),
          requests_(configuration.requestCapacity), responses_(configuration.responseCapacity) {}

    static constexpr std::size_t maximumPreparedCursorCount = 5;
    double cursor_ = 0.0;
    std::uint64_t generation_ = 1;
    std::uint64_t lastRequestedGeneration_ = 0;
    std::uint64_t lastRequestedBlockIndex_ = std::numeric_limits<std::uint64_t>::max();
    int lastRequestedDirection_ = 0;
    int direction_ = 1;
    double preparedRestartSourceFrame_ = 0.0;
    int preparedRestartDirection_ = 1;
    bool hasPreparedRestart_ = false;
    bool underrunActive_ = false;
    std::uint64_t workerGeneration_ = 0;
    std::uint64_t workerNextBlock_ = 0;
    int workerDirection_ = 1;
    bool workerHasNextBlock_ = false;
    bool workerInitialized_ = false;
    bool workerFailed_ = false;
    std::uint64_t workerEpochRequestGeneration_ = 0;
    std::vector<Request> requests_;
    std::vector<Response> responses_;
    std::vector<Response> preparedResponses_;
    std::vector<PreparedCursor> preparedCursors_;
};

class AudioStreamConsumer::Impl : public AudioStreamConsumerValueState
{
public:
    // Only the audio thread accesses this candidate. The published response
    // read index remains pinned while the worker can append to other slots.
    struct CallbackCandidate
    {
        bool active = false;
        double cursor = 0;
        std::uint64_t generation = 0, lastGeneration = 0, lastBlock = 0;
        int direction = 1, lastDirection = 0;
        bool underrun = false;
        std::uint64_t read = 0, writeLimit = 0;
        std::uint64_t missing = 0, underruns = 0, stale = 0, publishedSeek = 0;
        bool hasRequest = false, hasEpoch = false;
        std::uint64_t requestBlock = 0, epochBlock = 0;
        int requestDirection = 1, epochDirection = 1;
    } callback_;
    // Borrowed only while an exclusive offline checkpoint owns decoded data.
    const DecodedAudioData* offlineReadData_ = nullptr;
    std::uint64_t consumerReadIndex() const noexcept
    { return callback_.active ? callback_.read : responseRead_.load(std::memory_order_relaxed); }
    std::uint64_t consumerWriteLimit() const noexcept
    { return callback_.active ? callback_.writeLimit : responseWrite_.load(std::memory_order_acquire); }
    bool beginCallback() noexcept
    {
        if (callback_.active) return false;
        callback_ = {};
        callback_.cursor = cursor_; callback_.generation = generation_;
        callback_.lastGeneration = lastRequestedGeneration_;
        callback_.lastBlock = lastRequestedBlockIndex_;
        callback_.lastDirection = lastRequestedDirection_;
        callback_.direction = direction_; callback_.underrun = underrunActive_;
        callback_.read = responseRead_.load(std::memory_order_relaxed);
        callback_.writeLimit = responseWrite_.load(std::memory_order_acquire);
        callback_.active = true;
        return true;
    }
    void rollbackCallback() noexcept
    {
        if (!callback_.active) return;
        cursor_ = callback_.cursor; generation_ = callback_.generation;
        lastRequestedGeneration_ = callback_.lastGeneration;
        lastRequestedBlockIndex_ = callback_.lastBlock;
        lastRequestedDirection_ = callback_.lastDirection;
        direction_ = callback_.direction; underrunActive_ = callback_.underrun;
        callback_.active = false;
    }
    void commitCallback() noexcept
    {
        if (!callback_.active) return;
        callback_.active = false;
        responseRead_.store(callback_.read, std::memory_order_release);
        missingFrames_.fetch_add(callback_.missing, std::memory_order_relaxed);
        underrunEvents_.fetch_add(callback_.underruns, std::memory_order_relaxed);
        staleResponseBlocks_.fetch_add(callback_.stale, std::memory_order_relaxed);
        if (callback_.publishedSeek)
            publishedGeneration_.store(callback_.publishedSeek, std::memory_order_release);
        if (callback_.hasEpoch)
        {
            workerState_.store(AudioStreamWorkerState::Waiting, std::memory_order_release);
            publishEpochRequest(callback_.epochBlock, callback_.epochDirection);
        }
        if (callback_.hasRequest)
        {
            // A completed callback needs refill at its latest cursor. Coalesce
            // provisional requests rather than publishing work for aborted reads.
            lastRequestedGeneration_ = 0;
            publishRequest(callback_.requestBlock, callback_.requestDirection);
        }
    }
    // Ordered inspection fields: underrun, missing, discontinuity, request
    // rejection, queue full, stale, decoded, decode failure, EOF, buffered,
    // capacity, low/high watermarks, pending requests, worker state, generation.
    using InspectionValues = std::array<std::uint64_t, 16>;
    [[nodiscard]] InspectionValues liveInspection() const noexcept
    {
        return {underrunEvents_.load(std::memory_order_relaxed), missingFrames_.load(std::memory_order_relaxed), 0,
            requestRejections_.load(std::memory_order_relaxed), responseQueueFull_.load(std::memory_order_relaxed),
            staleResponseBlocks_.load(std::memory_order_relaxed), decodedBlocks_.load(std::memory_order_relaxed),
            decodeFailures_.load(std::memory_order_relaxed), 0, responseCount(), configuration_.responseCapacity,
            configuration_.lowWatermark, configuration_.highWatermark, requestCount(),
            static_cast<std::uint64_t>(workerState_.load(std::memory_order_acquire)),
            publishedGeneration_.load(std::memory_order_acquire)};
    }
    [[nodiscard]] InspectionValues inspection() const noexcept
    {
        // Inspection is control-thread only. This mutex never participates in
        // decoding or the audio callback; it serializes readers with publication
        // transitions so no reader can sample a provisional live generation.
        std::lock_guard lock(inspectionMutex_);
        return inspectionDeferred_ ? inspectionBaseline_ : liveInspection();
    }
    void deferInspection() noexcept
    {
        std::lock_guard lock(inspectionMutex_);
        inspectionBaseline_ = liveInspection();
        inspectionDeferred_ = true;
    }
    void publishInspection() noexcept
    {
        std::lock_guard lock(inspectionMutex_);
        inspectionDeferred_ = false;
    }
    Impl(
        const Pcm16Layout& layout,
        AudioStreamConfiguration configuration,
        double initialSourceFrame,
        double initialSourceFrameIncrement)
        : AudioStreamConsumerValueState(configuration, initialSourceFrame, initialSourceFrameIncrement)
        , layout_(layout), configuration_(configuration)
    {
        for (auto& response : responses_)
            response.samples.resize(
                configuration_.blockFrames + 1,
                0.0);
        preparedResponses_.reserve(
            configuration_.highWatermark);
        preparedCursors_.reserve(maximumPreparedCursorCount);
        publishRequest(blockIndexFor(cursor_), direction_);
    }

    [[nodiscard]] std::uint64_t blockIndexFor(
        double sourceFrame) const noexcept
    {
        return static_cast<std::uint64_t>(
            sourceFrame
            / static_cast<double>(
                configuration_.blockFrames));
    }

    [[nodiscard]] bool tryPushRequest(
        Request request) noexcept
    {
        const auto write = requestWrite_.load(
            std::memory_order_relaxed);
        const auto read = requestRead_.load(
            std::memory_order_acquire);
        if (write - read >= requests_.size())
        {
            requestRejections_.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }
        requests_[static_cast<std::size_t>(
            write % requests_.size())] = request;
        requestWrite_.store(
            write + 1,
            std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPopRequest(
        Request& request) noexcept
    {
        const auto read = requestRead_.load(
            std::memory_order_relaxed);
        const auto write = requestWrite_.load(
            std::memory_order_acquire);
        if (read == write)
            return false;
        request = requests_[static_cast<std::size_t>(
            read % requests_.size())];
        requestRead_.store(
            read + 1,
            std::memory_order_release);
        return true;
    }

    void publishRequest(
        std::uint64_t blockIndex,
        int direction) noexcept
    {
        if (lastRequestedGeneration_ == generation_
            && lastRequestedBlockIndex_ == blockIndex
            && lastRequestedDirection_ == direction)
        {
            return;
        }
        if (callback_.active)
        {
            callback_.hasRequest = true; callback_.requestBlock = blockIndex;
            callback_.requestDirection = direction;
            lastRequestedGeneration_ = generation_; lastRequestedBlockIndex_ = blockIndex;
            lastRequestedDirection_ = direction;
            return;
        }
        if (tryPushRequest({generation_, blockIndex, direction}))
        {
            lastRequestedGeneration_ = generation_;
            lastRequestedBlockIndex_ = blockIndex;
            lastRequestedDirection_ = direction;
        }
    }

    // Latest-wins single-producer/single-consumer mailbox for epoch-changing
    // operations. Unlike the ordinary bounded request queue, a restart or
    // prepared relocation cannot be rejected behind stale cursor requests.
    void publishEpochRequest(
        std::uint64_t blockIndex,
        int direction) noexcept
    {
        if (callback_.active)
        {
            callback_.hasEpoch = true; callback_.epochBlock = blockIndex;
            callback_.epochDirection = direction;
            // The new epoch supersedes every request for the previous one.
            callback_.hasRequest = false;
            return;
        }
        epochRequestSequence_.fetch_add(
            1,
            std::memory_order_acq_rel);
        epochRequestBlockIndex_.store(
            blockIndex,
            std::memory_order_relaxed);
        epochRequestDirection_.store(
            direction,
            std::memory_order_relaxed);
        epochRequestGeneration_.store(
            generation_,
            std::memory_order_relaxed);
        epochRequestSequence_.fetch_add(
            1,
            std::memory_order_release);
    }

    [[nodiscard]] bool tryReadEpochRequest(
        Request& request) noexcept
    {
        const auto sequence = epochRequestSequence_.load(
            std::memory_order_acquire);
        if ((sequence & 1U) != 0U)
        {
            return false;
        }
        const auto generation = epochRequestGeneration_.load(
            std::memory_order_relaxed);
        if (generation == 0
            || generation == workerEpochRequestGeneration_)
        {
            return false;
        }
        const auto blockIndex = epochRequestBlockIndex_.load(
            std::memory_order_relaxed);
        const auto direction = epochRequestDirection_.load(
            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto confirmedSequence =
            epochRequestSequence_.load(
                std::memory_order_relaxed);
        if (confirmedSequence != sequence
            || (confirmedSequence & 1U) != 0U)
        {
            return false;
        }
        request = {
            generation,
            blockIndex,
            direction,
        };
        return true;
    }

    void acknowledgeEpochRequestThrough(
        std::uint64_t processedGeneration) noexcept
    {
        const auto sequence = epochRequestSequence_.load(
            std::memory_order_acquire);
        if ((sequence & 1U) != 0U)
            return;
        const auto generation = epochRequestGeneration_.load(
            std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto confirmedSequence =
            epochRequestSequence_.load(
                std::memory_order_relaxed);
        if (confirmedSequence == sequence
            && (confirmedSequence & 1U) == 0U
            && generation != 0
            && generation <= processedGeneration)
        {
            workerEpochRequestGeneration_ = generation;
        }
    }

    [[nodiscard]] const Response* responseForBlock(
        std::uint64_t blockIndex) const noexcept
    {
        const auto read = consumerReadIndex();
        const auto write = consumerWriteLimit();
        for (auto index = read; index < write; ++index)
        {
            const auto& response = responses_[
                static_cast<std::size_t>(
                    index % responses_.size())];
            if (response.generation == generation_
                && response.blockIndex == blockIndex)
            {
                return &response;
            }
        }
        const auto prepared = std::ranges::lower_bound(
            preparedResponses_,
            blockIndex,
            {},
            &Response::blockIndex);
        if (prepared != preparedResponses_.end()
            && prepared->blockIndex == blockIndex)
            return &*prepared;
        return nullptr;
    }

    [[nodiscard]] bool hasPreparedResponseForBlock(
        std::uint64_t blockIndex) const noexcept
    {
        const auto prepared = std::ranges::lower_bound(
            preparedResponses_,
            blockIndex,
            {},
            &Response::blockIndex);
        return prepared != preparedResponses_.end()
            && prepared->blockIndex == blockIndex;
    }

    [[nodiscard]] bool hasPreparedCursor(
        double sourceFrame,
        int direction) const noexcept
    {
        return std::ranges::any_of(
            preparedCursors_,
            [&](const auto& prepared) {
                return prepared.sourceFrame == sourceFrame
                    && prepared.direction == direction;
            });
    }

    void discardConsumedResponses(
        std::uint64_t desiredBlock,
        int direction) noexcept
    {
        const auto writeSnapshot = consumerWriteLimit();
        while (consumerReadIndex()
               < writeSnapshot)
        {
            const auto* response = frontResponse();
            if (response == nullptr)
                return;
            if (response->generation != generation_)
            {
                popResponse(true);
                continue;
            }
            const auto consumed = direction >= 0
                ? response->blockIndex < desiredBlock
                : response->blockIndex > desiredBlock;
            if (!consumed)
                return;
            popResponse(false);
        }
    }

    [[nodiscard]] std::size_t responseCount() const noexcept
    {
        return static_cast<std::size_t>(
            responseWrite_.load(std::memory_order_acquire)
            - responseRead_.load(std::memory_order_acquire));
    }

    [[nodiscard]] std::size_t requestCount() const noexcept
    {
        return static_cast<std::size_t>(
            requestWrite_.load(std::memory_order_acquire)
            - requestRead_.load(std::memory_order_acquire));
    }

    [[nodiscard]] Response* beginResponseWrite() noexcept
    {
        const auto write = responseWrite_.load(
            std::memory_order_relaxed);
        const auto read = responseRead_.load(
            std::memory_order_acquire);
        if (write - read >= responses_.size())
        {
            responseQueueFull_.fetch_add(
                1,
                std::memory_order_relaxed);
            return nullptr;
        }
        return &responses_[static_cast<std::size_t>(
            write % responses_.size())];
    }

    void publishResponseWrite() noexcept
    {
        responseWrite_.fetch_add(
            1,
            std::memory_order_release);
        decodedBlocks_.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    [[nodiscard]] const Response* frontResponse() const noexcept
    {
        const auto read = consumerReadIndex();
        const auto write = consumerWriteLimit();
        if (read == write)
            return nullptr;
        return &responses_[static_cast<std::size_t>(
            read % responses_.size())];
    }

    void popResponse(bool stale) noexcept
    {
        if (callback_.active)
        {
            ++callback_.read;
            if (stale) ++callback_.stale;
            return;
        }
        if (stale)
        {
            staleResponseBlocks_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        responseRead_.fetch_add(
            1,
            std::memory_order_release);
    }

    void drainPublishedResponsesAsStale() noexcept
    {
        const auto writeSnapshot = consumerWriteLimit();
        while (consumerReadIndex()
               < writeSnapshot)
        {
            popResponse(true);
        }
    }

    void beginRefill(
        double sourceFrame,
        double sourceFrameIncrement,
        bool publishSeekGeneration) noexcept
    {
        drainPublishedResponsesAsStale();
        cursor_ = sourceFrame;
        ++generation_;
        if (publishSeekGeneration && callback_.active)
            callback_.publishedSeek = generation_;
        else if (publishSeekGeneration)
        {
            publishedGeneration_.store(
                generation_,
                std::memory_order_release);
        }
        lastRequestedGeneration_ = 0;
        lastRequestedBlockIndex_ =
            std::numeric_limits<std::uint64_t>::max();
        lastRequestedDirection_ = 0;
        direction_ = sourceFrameIncrement < 0.0 ? -1 : 1;
        underrunActive_ = false;
        if (!callback_.active)
            workerState_.store(AudioStreamWorkerState::Waiting, std::memory_order_release);
        publishEpochRequest(
            blockIndexFor(sourceFrame),
            direction_);
    }

    void markMissing() noexcept
    {
        if (callback_.active)
        {
            ++callback_.missing;
            if (!underrunActive_) ++callback_.underruns;
            underrunActive_ = true;
            return;
        }
        missingFrames_.fetch_add(
            1,
            std::memory_order_relaxed);
        if (!underrunActive_)
        {
            underrunEvents_.fetch_add(
                1,
                std::memory_order_relaxed);
            underrunActive_ = true;
        }
    }

    // Immutable preparation remains on the live object; health readers may
    // inspect configuration while an exclusive offline checkpoint is restored.
    Pcm16Layout layout_;
    AudioStreamConfiguration configuration_;
    std::atomic<std::uint64_t> publishedGeneration_ = 1;
    std::atomic<AudioStreamWorkerState> workerState_ =
        AudioStreamWorkerState::Waiting;

    std::atomic<std::uint64_t> requestWrite_ = 0;
    std::atomic<std::uint64_t> requestRead_ = 0;
    std::atomic<std::uint64_t> epochRequestSequence_ = 0;
    std::atomic<std::uint64_t> epochRequestGeneration_ = 0;
    std::atomic<std::uint64_t> epochRequestBlockIndex_ = 0;
    std::atomic<int> epochRequestDirection_ = 1;
    std::atomic<std::uint64_t> responseWrite_ = 0;
    std::atomic<std::uint64_t> responseRead_ = 0;

    std::atomic<std::uint64_t> underrunEvents_ = 0;
    std::atomic<std::uint64_t> missingFrames_ = 0;
    std::atomic<std::uint64_t> requestRejections_ = 0;
    std::atomic<std::uint64_t> responseQueueFull_ = 0;
    std::atomic<std::uint64_t> staleResponseBlocks_ = 0;
    std::atomic<std::uint64_t> decodedBlocks_ = 0;
    std::atomic<std::uint64_t> decodeFailures_ = 0;
    bool offlineCheckpointActive_ = false;
    // Publication state remains outside processing rollback at a stable address.
    mutable std::mutex inspectionMutex_;
    bool inspectionDeferred_ = false;
    InspectionValues inspectionBaseline_{};
};

class DecodedBlockCacheOwner::Impl
{
public:
    struct CacheBlock
    {
        std::uint64_t blockIndex = 0;
        std::uint64_t useStamp = 0;
        bool valid = false;
        std::vector<double> samples;
    };

    Impl(
        talas::core::ResourceSnapshotPtr snapshot,
        AudioStreamConfiguration configuration,
        Pcm16Layout layout,
        std::filesystem::path backingDirectory,
        std::filesystem::path backingPath)
        : snapshot_(std::move(snapshot))
        , configuration_(configuration)
        , layout_(layout)
        , backingDirectory_(std::move(backingDirectory))
        , backingPath_(std::move(backingPath))
        , input_(backingPath_, std::ios::binary)
        , cache_(configuration_.decodedCacheBlocks)
        , decodeBytes_(
              (configuration_.blockFrames + 1)
                  * static_cast<std::size_t>(layout_.blockAlign),
              0)
    {
        for (auto& block : cache_)
            block.samples.resize(
                configuration_.blockFrames + 1,
                0.0);
    }

    ~Impl()
    {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        input_.close();
        std::error_code cleanupError;
        std::filesystem::remove_all(
            backingDirectory_,
            cleanupError);
    }

    void start()
    {
        if (configuration_.workerMode
            == AudioStreamWorkerMode::Background)
        {
            worker_ = std::thread([this] { workerLoop(); });
        }
    }

    [[nodiscard]] bool decodeBlock(
        std::uint64_t blockIndex,
        std::span<double> destination)
    {
        ++useStamp_;
        auto found = std::find_if(
            cache_.begin(),
            cache_.end(),
            [&](const auto& candidate) {
                return candidate.valid
                    && candidate.blockIndex == blockIndex;
            });
        if (found != cache_.end())
        {
            found->useStamp = useStamp_;
            std::copy(
                found->samples.begin(),
                found->samples.end(),
                destination.begin());
            return true;
        }

        auto selected = std::find_if(
            cache_.begin(),
            cache_.end(),
            [](const auto& candidate) {
                return !candidate.valid;
            });
        if (selected == cache_.end())
        {
            selected = std::min_element(
                cache_.begin(),
                cache_.end(),
                [](const auto& left, const auto& right) {
                    return left.useStamp < right.useStamp;
                });
        }

        // The entry holds no block until the read below succeeds; a failed
        // read must not leave its zeroed samples cached as the old block.
        selected->valid = false;
        std::fill(
            selected->samples.begin(),
            selected->samples.end(),
            0.0);
        const auto firstFrame = blockIndex
            * static_cast<std::uint64_t>(
                configuration_.blockFrames);
        if (firstFrame >= layout_.frameCount)
            return false;
        const auto availableFrames = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                configuration_.blockFrames + 1,
                layout_.frameCount - firstFrame));
        const auto byteCount = availableFrames
            * static_cast<std::size_t>(layout_.blockAlign);
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(
            layout_.dataOffset
            + firstFrame
                * static_cast<std::uint64_t>(
                    layout_.blockAlign)));
        input_.read(
            reinterpret_cast<char*>(decodeBytes_.data()),
            static_cast<std::streamsize>(byteCount));
        if (!input_
            || input_.gcount()
                != static_cast<std::streamsize>(byteCount))
        {
            return false;
        }

        if (!decodePcm16MonoFrames(
                std::span<const std::uint8_t>{decodeBytes_.data(), byteCount},
                layout_,
                availableFrames,
                selected->samples))
        {
            return false;
        }
        selected->blockIndex = blockIndex;
        selected->useStamp = useStamp_;
        selected->valid = true;
        std::copy(
            selected->samples.begin(),
            selected->samples.end(),
            destination.begin());
        return true;
    }

    [[nodiscard]] std::size_t processConsumer(
        AudioStreamConsumer::Impl& consumer,
        std::size_t maximumBlocks)
    {
        AudioStreamConsumer::Impl::Request request;
        bool hasRequest = consumer.tryReadEpochRequest(request);
        AudioStreamConsumer::Impl::Request queuedRequest;
        while (consumer.tryPopRequest(queuedRequest))
        {
            if (!hasRequest
                || queuedRequest.generation
                    >= request.generation)
            {
                request = queuedRequest;
                hasRequest = true;
            }
        }
        if (hasRequest)
        {
            consumer.acknowledgeEpochRequestThrough(
                request.generation);
            consumer.workerGeneration_ = request.generation;
            consumer.workerNextBlock_ = request.firstBlockIndex;
            consumer.workerDirection_ = request.direction < 0
                ? -1
                : 1;
            consumer.workerHasNextBlock_ = true;
            consumer.workerInitialized_ = true;
            consumer.workerFailed_ = false;
            consumer.workerState_.store(
                AudioStreamWorkerState::Filling,
                std::memory_order_release);
        }
        if (!consumer.workerInitialized_
            || consumer.workerFailed_)
        {
            return 0;
        }

        std::size_t published = 0;
        const auto blockCount =
            (layout_.frameCount
             + configuration_.blockFrames - 1)
            / configuration_.blockFrames;
        while (published < maximumBlocks
               && consumer.responseCount()
                   < configuration_.highWatermark
               && consumer.workerHasNextBlock_
               && consumer.workerNextBlock_ < blockCount)
        {
            auto* response = consumer.beginResponseWrite();
            if (response == nullptr)
                break;
            response->generation =
                consumer.workerGeneration_;
            response->blockIndex =
                consumer.workerNextBlock_;
            if (!decodeBlock(
                    response->blockIndex,
                    response->samples))
            {
                consumer.decodeFailures_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                consumer.workerFailed_ = true;
                consumer.workerState_.store(
                    AudioStreamWorkerState::Failed,
                    std::memory_order_release);
                break;
            }
            consumer.publishResponseWrite();
            if (consumer.workerDirection_ >= 0)
            {
                if (consumer.workerNextBlock_
                    == blockCount - 1U)
                {
                    consumer.workerHasNextBlock_ = false;
                }
                else
                {
                    ++consumer.workerNextBlock_;
                }
            }
            else if (consumer.workerNextBlock_ == 0)
            {
                consumer.workerHasNextBlock_ = false;
            }
            else
            {
                --consumer.workerNextBlock_;
            }
            ++published;
        }
        if (!consumer.workerFailed_)
        {
            consumer.workerState_.store(
                !consumer.workerHasNextBlock_
                    ? AudioStreamWorkerState::EndOfStream
                    : (consumer.responseCount()
                               >= configuration_.highWatermark
                           ? AudioStreamWorkerState::Ready
                           : AudioStreamWorkerState::Filling),
                std::memory_order_release);
        }
        return published;
    }

    [[nodiscard]] std::size_t pump(
        std::size_t maximumBlocks)
    {
        std::lock_guard lock(consumersMutex_);
        std::size_t published = 0;
        for (auto* consumer : consumers_)
        {
            if (published < maximumBlocks)
            {
                published += processConsumer(
                    *consumer->impl_,
                    maximumBlocks - published);
            }
        }
        return published;
    }

    void workerLoop()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            const auto published = pump(
                std::numeric_limits<std::size_t>::max());
            if (published == 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds{1});
        }
    }

    talas::core::ResourceSnapshotPtr snapshot_;
    AudioStreamConfiguration configuration_;
    Pcm16Layout layout_;
    std::filesystem::path backingDirectory_;
    std::filesystem::path backingPath_;
    std::ifstream input_;
    std::vector<CacheBlock> cache_;
    std::vector<std::uint8_t> decodeBytes_;
    std::uint64_t useStamp_ = 0;
    std::mutex consumersMutex_;
    std::vector<AudioStreamConsumer*> consumers_;
    std::atomic<bool> stop_ = false;
    std::thread worker_;
};

class AudioStreamConsumer::OfflineCheckpoint::State
{
    struct Undo
    {
        virtual ~Undo() = default;
        virtual void restore() noexcept = 0;
    };
    struct ValueUndo final : Undo
    {
        explicit ValueUndo(AudioStreamConsumerValueState& value) : target(value), baseline(value) {}
        void restore() noexcept override { using std::swap; swap(target, baseline); }
        AudioStreamConsumerValueState& target;
        AudioStreamConsumerValueState baseline;
    };
    template <class T> struct AtomicUndo final : Undo
    {
        explicit AtomicUndo(std::atomic<T>& value) : target(value), baseline(value.load(std::memory_order_relaxed)) {}
        void restore() noexcept override { target.store(baseline, std::memory_order_relaxed); }
        std::atomic<T>& target;
        T baseline;
    };
public:
    State(AudioStreamConsumer::Impl& consumer, std::shared_ptr<DecodedBlockCacheOwner> owner,
        bool deferInspection, std::shared_ptr<const DecodedAudioData> offlineData)
        : target(&consumer), owner_(std::move(owner)), offlineData_(std::move(offlineData)), deferredInspection_(deferInspection)
    {
        static_assert(std::is_nothrow_swappable_v<AudioStreamConsumerValueState>);
        undo_.push_back(std::make_unique<ValueUndo>(consumer));
        capture(consumer.publishedGeneration_);
        capture(consumer.workerState_);
        capture(consumer.requestWrite_); capture(consumer.requestRead_);
        capture(consumer.epochRequestSequence_); capture(consumer.epochRequestGeneration_);
        capture(consumer.epochRequestBlockIndex_); capture(consumer.epochRequestDirection_);
        capture(consumer.responseWrite_); capture(consumer.responseRead_);
        capture(consumer.underrunEvents_); capture(consumer.missingFrames_);
        capture(consumer.requestRejections_); capture(consumer.responseQueueFull_);
        capture(consumer.staleResponseBlocks_); capture(consumer.decodedBlocks_); capture(consumer.decodeFailures_);
        consumer.offlineReadData_ = offlineData_.get();
        consumer.offlineCheckpointActive_ = true;
        if (deferredInspection_) consumer.deferInspection();
    }
    ~State()
    {
        if (!target) return;
        // No callback is active for this consumer. Exclude the only worker
        // writer while restoring its queues and owned response payloads.
        std::lock_guard lock(owner_->impl_->consumersMutex_);
        for (auto index = undo_.size(); index != 0; --index) undo_[index - 1]->restore();
        target->offlineReadData_ = nullptr;
        target->offlineCheckpointActive_ = false;
        if (deferredInspection_) target->publishInspection();
    }
    void commit() noexcept
    {
        if (!target) return;
        target->offlineReadData_ = nullptr;
        target->offlineCheckpointActive_ = false;
        if (deferredInspection_) target->publishInspection();
        target = nullptr;
    }
private:
    template <class T> void capture(std::atomic<T>& value) { undo_.push_back(std::make_unique<AtomicUndo<T>>(value)); }
    AudioStreamConsumer::Impl* target;
    std::shared_ptr<DecodedBlockCacheOwner> owner_;
    std::shared_ptr<const DecodedAudioData> offlineData_;
    std::vector<std::unique_ptr<Undo>> undo_;
    bool deferredInspection_ = false;
};

AudioStreamConsumer::OfflineCheckpoint::OfflineCheckpoint(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
AudioStreamConsumer::OfflineCheckpoint::~OfflineCheckpoint() = default;
void AudioStreamConsumer::OfflineCheckpoint::commit() noexcept { state_->commit(); }

std::unique_ptr<AudioStreamConsumer::OfflineCheckpoint> AudioStreamConsumer::beginOfflineCheckpoint(bool deferInspection, bool deterministicReads)
{
    if (impl_ && impl_->callback_.active) throw std::logic_error("Pending audio stream callback checkpoint");
    if (!impl_ || !owner_ || !owner_->impl_) return {};
    std::shared_ptr<const DecodedAudioData> offlineData;
    if (deterministicReads)
    {
        auto loaded = owner_->loadAllForOffline();
        if (loaded.allocationFailed) throw std::bad_alloc{};
        if (!loaded.ok()) throw std::logic_error(loaded.error);
        offlineData = std::move(loaded.data);
    }
    std::unique_ptr<OfflineCheckpoint::State> state;
    {
        std::lock_guard lock(owner_->impl_->consumersMutex_);
        if (impl_->offlineCheckpointActive_) throw std::logic_error("Nested offline stream checkpoint");
        state = std::make_unique<OfflineCheckpoint::State>(*impl_, owner_, deferInspection, std::move(offlineData));
    }
    // Wrapper allocation may fail after capture; State then restores under the
    // owner lock. Allocate it outside that lock to avoid recursive locking.
    return std::make_unique<OfflineCheckpoint>(std::move(state));
}

bool AudioStreamConsumer::canRestorePreparedSourceBoundary(double sourceFrame, double increment,
    std::uint64_t refillCount) const noexcept
{
    if (!impl_ || impl_->callback_.active || !std::isfinite(sourceFrame) || sourceFrame < 0
        || !std::isfinite(increment) || increment <= 0) return false;
    if (refillCount > std::numeric_limits<std::uint64_t>::max() - impl_->generation_) return false;
    return sourceFrame >= static_cast<double>(impl_->layout_.frameCount)
        || (impl_->hasPreparedCursor(sourceFrame, 1)
            && impl_->hasPreparedResponseForBlock(impl_->blockIndexFor(sourceFrame)));
}

bool AudioStreamConsumer::restorePreparedSourceBoundary(double sourceFrame, double increment) noexcept
{
    if (!canRestorePreparedSourceBoundary(sourceFrame, increment, 1)) return false;
    if (sourceFrame < static_cast<double>(impl_->layout_.frameCount))
        return relocatePrepared(sourceFrame, increment);
    // EOF has no readable block and can exceed the integral block index range.
    // A later trigger performs its ordinary seek; never cast the EOF cursor.
    impl_->cursor_ = sourceFrame; impl_->direction_ = 1; impl_->underrunActive_ = false;
    return true;
}

bool AudioStreamConsumer::beginCallbackCheckpoint() noexcept
{ return impl_ && impl_->beginCallback(); }
void AudioStreamConsumer::commitCallbackCheckpoint() noexcept
{ if (impl_) impl_->commitCallback(); }
void AudioStreamConsumer::rollbackCallbackCheckpoint() noexcept
{ if (impl_) impl_->rollbackCallback(); }

AudioStreamConsumer::AudioStreamConsumer(
    std::shared_ptr<DecodedBlockCacheOwner> owner,
    std::unique_ptr<Impl> impl)
    : owner_(std::move(owner))
    , impl_(std::move(impl))
{
}

AudioStreamConsumer::~AudioStreamConsumer()
{
    if (owner_ != nullptr)
        owner_->unregisterConsumer(this);
}

bool AudioStreamConsumer::seek(double sourceFrame) noexcept
{
    return seek(sourceFrame, 1.0);
}

bool AudioStreamConsumer::seek(
    double sourceFrame,
    double sourceFrameIncrement) noexcept
{
    if (impl_ == nullptr
        || !std::isfinite(sourceFrame)
        || sourceFrame < 0.0
        || sourceFrame
            >= static_cast<double>(
                impl_->layout_.frameCount)
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0
        || impl_->generation_
            == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    impl_->beginRefill(
        sourceFrame,
        sourceFrameIncrement,
        true);
    return true;
}

bool AudioStreamConsumer::restartPrepared(
    double sourceFrame,
    double sourceFrameIncrement) noexcept
{
    if (impl_ == nullptr
        || !impl_->hasPreparedRestart_
        || !std::isfinite(sourceFrame)
        || sourceFrame != impl_->preparedRestartSourceFrame_
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0
        || (sourceFrameIncrement < 0.0 ? -1 : 1)
            != impl_->preparedRestartDirection_
        || impl_->generation_
            == std::numeric_limits<std::uint64_t>::max()
        || !impl_->hasPreparedResponseForBlock(
            impl_->blockIndexFor(sourceFrame)))
    {
        return false;
    }
    impl_->beginRefill(
        sourceFrame,
        sourceFrameIncrement,
        false);
    return true;
}

bool AudioStreamConsumer::relocatePrepared(
    double sourceFrame,
    double sourceFrameIncrement) noexcept
{
    const auto direction = sourceFrameIncrement < 0.0 ? -1 : 1;
    if (impl_ == nullptr
        || !std::isfinite(sourceFrame)
        || sourceFrame < 0.0
        || sourceFrame
            >= static_cast<double>(impl_->layout_.frameCount)
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0
        || !impl_->hasPreparedCursor(sourceFrame, direction)
        || impl_->generation_
            == std::numeric_limits<std::uint64_t>::max()
        || !impl_->hasPreparedResponseForBlock(
            impl_->blockIndexFor(sourceFrame)))
    {
        return false;
    }
    impl_->beginRefill(
        sourceFrame,
        sourceFrameIncrement,
        false);
    return true;
}

AudioStreamReadResult AudioStreamConsumer::readNext(
    double sourceFrameIncrement) noexcept
{
    if (impl_ == nullptr
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0)
    {
        return {AudioStreamReadStatus::InvalidIncrement, 0.0};
    }
    if (!std::isfinite(impl_->cursor_)
        || impl_->cursor_ < 0.0
        || impl_->cursor_
            >= static_cast<double>(
                impl_->layout_.frameCount))
    {
        impl_->underrunActive_ = false;
        return {AudioStreamReadStatus::EndOfStream, 0.0};
    }

    const auto floorFrame = std::floor(impl_->cursor_);
    const auto rightFrame = std::min(
        floorFrame + 1.0,
        static_cast<double>(impl_->layout_.frameCount - 1U));
    return readInterpolated(
        impl_->cursor_,
        rightFrame,
        impl_->cursor_ + sourceFrameIncrement,
        sourceFrameIncrement);
}

AudioStreamReadResult AudioStreamConsumer::readInterpolated(
    double sourceFrame,
    double interpolationRightSourceFrame,
    double nextSourceFrame,
    double sourceFrameIncrement) noexcept
{
    if (impl_ == nullptr
        || !std::isfinite(sourceFrame)
        || !std::isfinite(interpolationRightSourceFrame)
        || !std::isfinite(nextSourceFrame)
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0)
    {
        return {AudioStreamReadStatus::InvalidIncrement, 0.0};
    }
    const auto frameCount = static_cast<double>(
        impl_->layout_.frameCount);
    if (sourceFrame < 0.0 || sourceFrame >= frameCount)
    {
        impl_->underrunActive_ = false;
        return {AudioStreamReadStatus::EndOfStream, 0.0};
    }
    if (interpolationRightSourceFrame < 0.0
        || interpolationRightSourceFrame >= frameCount
        || std::floor(interpolationRightSourceFrame)
            != interpolationRightSourceFrame)
    {
        return {AudioStreamReadStatus::InvalidIncrement, 0.0};
    }

    if (impl_->offlineReadData_)
    {
        const auto leftFrame = static_cast<std::size_t>(std::floor(sourceFrame));
        const auto rightFrame = static_cast<std::size_t>(interpolationRightSourceFrame);
        const auto& samples = impl_->offlineReadData_->monoSamples;
        if (leftFrame >= samples.size() || rightFrame >= samples.size())
            return {AudioStreamReadStatus::InvalidIncrement, 0.0};
        const auto fraction = sourceFrame - static_cast<double>(leftFrame);
        impl_->cursor_ = nextSourceFrame;
        impl_->direction_ = sourceFrameIncrement < 0.0 ? -1 : 1;
        impl_->underrunActive_ = false;
        return {AudioStreamReadStatus::Available,
            samples[leftFrame] + (samples[rightFrame] - samples[leftFrame]) * fraction};
    }

    const auto direction = sourceFrameIncrement < 0.0 ? -1 : 1;
    impl_->direction_ = direction;
    const auto leftFrame = static_cast<std::uint64_t>(
        std::floor(sourceFrame));
    const auto rightFrame = static_cast<std::uint64_t>(
        interpolationRightSourceFrame);
    const auto leftBlock = leftFrame
        / impl_->configuration_.blockFrames;
    const auto rightBlock = rightFrame
        / impl_->configuration_.blockFrames;
    const auto* leftResponse = impl_->responseForBlock(leftBlock);
    const auto* rightResponse = rightBlock == leftBlock
        ? leftResponse
        : impl_->responseForBlock(rightBlock);
    const auto leftOffset = static_cast<std::size_t>(
        leftFrame % impl_->configuration_.blockFrames);
    const auto rightOffset = static_cast<std::size_t>(
        rightFrame % impl_->configuration_.blockFrames);
    const auto fraction = sourceFrame
        - static_cast<double>(leftFrame);
    const auto needsRightSample = fraction > 0.0;

    AudioStreamReadResult result;
    if (leftResponse != nullptr
        && leftOffset < leftResponse->samples.size()
        && (!needsRightSample
            || (rightResponse != nullptr
                && rightOffset < rightResponse->samples.size())))
    {
        const auto left = leftResponse->samples[leftOffset];
        const auto right = needsRightSample
            ? rightResponse->samples[rightOffset]
            : left;
        result.status = AudioStreamReadStatus::Available;
        result.sample = left + (right - left) * fraction;
        impl_->underrunActive_ = false;
    }
    else
    {
        result.status = AudioStreamReadStatus::Missing;
        result.sample = 0.0;
        impl_->markMissing();
        impl_->publishRequest(leftBlock, direction);
        if (needsRightSample && rightBlock != leftBlock)
            impl_->publishRequest(rightBlock, direction);
    }

    const auto previousBlock = impl_->blockIndexFor(sourceFrame);
    impl_->cursor_ = nextSourceFrame;
    if (nextSourceFrame >= 0.0 && nextSourceFrame < frameCount)
    {
        const auto nextBlock = impl_->blockIndexFor(nextSourceFrame);
        const auto blockDiscontinuity = direction >= 0
            ? nextBlock < previousBlock
            : nextBlock > previousBlock;
        if (blockDiscontinuity)
        {
            impl_->drainPublishedResponsesAsStale();
            impl_->lastRequestedGeneration_ = 0;
            impl_->lastRequestedBlockIndex_ =
                std::numeric_limits<std::uint64_t>::max();
            impl_->lastRequestedDirection_ = 0;
        }
        else
        {
            impl_->discardConsumedResponses(nextBlock, direction);
        }
        impl_->publishRequest(nextBlock, direction);
    }
    return result;
}

double AudioStreamConsumer::sourceFrameCursor() const noexcept
{
    return impl_ != nullptr ? impl_->cursor_ : 0.0;
}

std::uint64_t AudioStreamConsumer::seekGeneration() const noexcept
{
    return impl_ != nullptr
        ? impl_->inspection()[15]
        : 0;
}

AudioStreamHealthSnapshot AudioStreamConsumer::health() const noexcept
{
    if (impl_ == nullptr)
        return {};
    const auto values = impl_->inspection();
    return {
        .underrunEvents = values[0], .missingFrames = values[1], .discontinuities = values[2],
        .requestRejections = values[3], .responseQueueFull = values[4], .staleResponseBlocks = values[5],
        .decodedBlocks = values[6], .decodeFailures = values[7], .endOfStreamFrames = values[8],
        .bufferedBlocks = static_cast<std::size_t>(values[9]), .bufferCapacity = static_cast<std::size_t>(values[10]),
        .lowWatermark = static_cast<std::size_t>(values[11]), .highWatermark = static_cast<std::size_t>(values[12]),
        .pendingRequests = static_cast<std::size_t>(values[13]), .workerState = static_cast<AudioStreamWorkerState>(values[14]),
    };
}

std::string_view audioStreamWorkerStateName(
    AudioStreamWorkerState state) noexcept
{
    switch (state)
    {
    case AudioStreamWorkerState::Waiting:
        return "waiting";
    case AudioStreamWorkerState::Filling:
        return "filling";
    case AudioStreamWorkerState::Ready:
        return "ready";
    case AudioStreamWorkerState::EndOfStream:
        return "end-of-stream";
    case AudioStreamWorkerState::Failed:
        return "failed";
    }
    return "unknown";
}

DecodedBlockCacheOwner::DecodedBlockCacheOwner(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

DecodedBlockCacheOwner::~DecodedBlockCacheOwner() = default;

void DecodedBlockCacheOwner::unregisterConsumer(
    AudioStreamConsumer* consumer) noexcept
{
    if (impl_ == nullptr || consumer == nullptr)
        return;
    std::lock_guard lock(impl_->consumersMutex_);
    std::erase(impl_->consumers_, consumer);
}

std::shared_ptr<AudioStreamConsumer>
DecodedBlockCacheOwner::createConsumer(
    double initialSourceFrame,
    double initialSourceFrameIncrement)
{
    if (impl_ == nullptr
        || !std::isfinite(initialSourceFrame)
        || initialSourceFrame < 0.0
        || initialSourceFrame
            >= static_cast<double>(
                impl_->layout_.frameCount)
        || !std::isfinite(initialSourceFrameIncrement)
        || initialSourceFrameIncrement == 0.0)
    {
        return {};
    }
    auto consumer = std::shared_ptr<AudioStreamConsumer>(
        new AudioStreamConsumer(
            shared_from_this(),
            std::make_unique<AudioStreamConsumer::Impl>(
                impl_->layout_,
                impl_->configuration_,
                initialSourceFrame,
                initialSourceFrameIncrement)));
    {
        std::lock_guard lock(impl_->consumersMutex_);
        impl_->consumers_.push_back(consumer.get());
    }
    return consumer;
}

bool DecodedBlockCacheOwner::prewarm(
    const std::shared_ptr<AudioStreamConsumer>& consumer,
    std::chrono::milliseconds timeout)
{
    if (impl_ == nullptr
        || consumer == nullptr
        || consumer->owner_.get() != this
        || impl_->configuration_.workerMode
            != AudioStreamWorkerMode::Background
        || timeout < std::chrono::milliseconds::zero())
    {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    const auto firstBlock = consumer->impl_->blockIndexFor(
        consumer->impl_->cursor_);
    const auto blockCount =
        (impl_->layout_.frameCount
         + impl_->configuration_.blockFrames - 1)
        / impl_->configuration_.blockFrames;
    const auto targetBlocks = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            impl_->configuration_.highWatermark,
            consumer->impl_->direction_ >= 0
                ? (blockCount > firstBlock
                       ? blockCount - firstBlock
                       : 0)
                : firstBlock + 1U));
    if (targetBlocks == 0)
        return false;
    while (std::chrono::steady_clock::now() <= deadline)
    {
        if (consumer->impl_->responseCount()
            >= targetBlocks)
        {
            return true;
        }
        if (consumer->impl_->decodeFailures_.load(
                std::memory_order_acquire)
            != 0)
        {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{1});
    }
    return false;
}

bool DecodedBlockCacheOwner::prepareReadWindow(
    const std::shared_ptr<AudioStreamConsumer>& consumer,
    double sourceFrame,
    double sourceFrameIncrement,
    std::size_t maximumOutputFrames,
    AudioStreamPreparedWindowBudget& budget)
{
    if (impl_ == nullptr
        || consumer == nullptr
        || consumer->owner_.get() != this
        || !std::isfinite(sourceFrame)
        || sourceFrame < 0.0
        || sourceFrame
            >= static_cast<double>(impl_->layout_.frameCount)
        || !std::isfinite(sourceFrameIncrement)
        || sourceFrameIncrement == 0.0
        || maximumOutputFrames == 0
        || budget.usedBlocks > budget.maximumBlocks
        || budget.usedBytes > budget.maximumBytes)
    {
        return false;
    }

    const auto firstBlock = consumer->impl_->blockIndexFor(
        sourceFrame);
    const auto blockCount =
        (impl_->layout_.frameCount
         + impl_->configuration_.blockFrames - 1U)
        / impl_->configuration_.blockFrames;
    const auto direction = sourceFrameIncrement < 0.0 ? -1 : 1;
    const auto projectedSourceFrame = std::clamp(
        static_cast<long double>(sourceFrame)
            + static_cast<long double>(direction)
                * std::abs(static_cast<long double>(
                    sourceFrameIncrement))
                * static_cast<long double>(maximumOutputFrames),
        0.0L,
        static_cast<long double>(impl_->layout_.frameCount - 1U));
    const auto projectedBlock = static_cast<std::uint64_t>(
        projectedSourceFrame
        / static_cast<long double>(
            impl_->configuration_.blockFrames));
    const auto watermarkLastBlock = direction >= 0
        ? std::min<std::uint64_t>(
              blockCount - 1U,
              firstBlock
                  + std::min<std::uint64_t>(
                      impl_->configuration_.highWatermark - 1U,
                      blockCount - 1U - firstBlock))
        : firstBlock
            - std::min<std::uint64_t>(
                impl_->configuration_.highWatermark - 1U,
                firstBlock);
    const auto lastBlock = direction >= 0
        ? std::max(projectedBlock, watermarkLastBlock)
        : std::min(projectedBlock, watermarkLastBlock);
    const auto targetBlocks = static_cast<std::size_t>(
        direction >= 0
            ? lastBlock - firstBlock + 1U
            : firstBlock - lastBlock + 1U);
    if (targetBlocks == 0)
        return false;

    const auto preparedDirection = direction;
    const auto alreadyBound = consumer->impl_->hasPreparedCursor(
        sourceFrame,
        preparedDirection);
    if (!alreadyBound
        && consumer->impl_->preparedCursors_.size()
            >= AudioStreamConsumer::Impl::maximumPreparedCursorCount)
    {
        return false;
    }

    constexpr auto responseOverhead =
        sizeof(AudioStreamConsumer::Impl::Response);
    if (impl_->configuration_.blockFrames
            >= (std::numeric_limits<std::size_t>::max()
                - responseOverhead)
                / sizeof(double))
    {
        return false;
    }
    const auto bytesPerPreparedBlock = responseOverhead
        + (impl_->configuration_.blockFrames + 1U)
            * sizeof(double);
    const auto remainingPreparedBlocks = std::min(
        budget.maximumBlocks - budget.usedBlocks,
        (budget.maximumBytes - budget.usedBytes)
            / bytesPerPreparedBlock);
    if (consumer->impl_->preparedResponses_.size()
            > std::numeric_limits<std::size_t>::max()
                - remainingPreparedBlocks
        || targetBlocks
            > consumer->impl_->preparedResponses_.size()
                + remainingPreparedBlocks)
    {
        return false;
    }

    std::vector<std::uint64_t> missingBlocks;
    missingBlocks.reserve(targetBlocks);
    for (std::size_t offset = 0; offset < targetBlocks; ++offset)
    {
        const auto blockIndex = direction >= 0
            ? firstBlock + offset
            : firstBlock - offset;
        if (!consumer->impl_->hasPreparedResponseForBlock(blockIndex))
            missingBlocks.push_back(blockIndex);
    }
    if (consumer->impl_->preparedResponses_.size() > blockCount
        || missingBlocks.size()
            > blockCount
                - consumer->impl_->preparedResponses_.size())
    {
        return false;
    }

    if (missingBlocks.empty())
    {
        if (!alreadyBound)
        {
            consumer->impl_->preparedCursors_.push_back({
                sourceFrame,
                preparedDirection,
            });
        }
        return true;
    }

    if (missingBlocks.size()
            > budget.maximumBlocks - budget.usedBlocks
        || missingBlocks.size()
            > (budget.maximumBytes - budget.usedBytes)
                / bytesPerPreparedBlock)
    {
        return false;
    }
    const auto additionalBytes =
        missingBlocks.size() * bytesPerPreparedBlock;

    std::ifstream input(impl_->backingPath_, std::ios::binary);
    if (!input)
        return false;
    std::vector<std::uint8_t> decodeBytes(
        (impl_->configuration_.blockFrames + 1U)
            * static_cast<std::size_t>(impl_->layout_.blockAlign),
        0);
    std::vector<AudioStreamConsumer::Impl::Response> prepared;
    prepared.reserve(missingBlocks.size());
    for (const auto blockIndex : missingBlocks)
    {
        AudioStreamConsumer::Impl::Response response;
        response.blockIndex = blockIndex;
        response.samples.resize(
            impl_->configuration_.blockFrames + 1U,
            0.0);
        const auto firstFrame = blockIndex
            * static_cast<std::uint64_t>(
                impl_->configuration_.blockFrames);
        const auto availableFrames = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                impl_->configuration_.blockFrames + 1U,
                impl_->layout_.frameCount - firstFrame));
        const auto byteCount = availableFrames
            * static_cast<std::size_t>(impl_->layout_.blockAlign);
        input.clear();
        input.seekg(static_cast<std::streamoff>(
            impl_->layout_.dataOffset
            + firstFrame
                * static_cast<std::uint64_t>(
                    impl_->layout_.blockAlign)));
        input.read(
            reinterpret_cast<char*>(decodeBytes.data()),
            static_cast<std::streamsize>(byteCount));
        if (input.gcount()
            != static_cast<std::streamsize>(byteCount))
        {
            return false;
        }
        if (!decodePcm16MonoFrames(
                std::span<const std::uint8_t>{decodeBytes.data(), byteCount},
                impl_->layout_,
                availableFrames,
                response.samples))
        {
            return false;
        }
        prepared.push_back(std::move(response));
    }
    consumer->impl_->preparedResponses_.reserve(
        consumer->impl_->preparedResponses_.size()
        + prepared.size());
    for (auto& response : prepared)
    {
        consumer->impl_->preparedResponses_.push_back(
            std::move(response));
    }
    std::ranges::sort(
        consumer->impl_->preparedResponses_,
        {},
        &AudioStreamConsumer::Impl::Response::blockIndex);
    budget.usedBlocks += missingBlocks.size();
    budget.usedBytes += additionalBytes;
    if (!alreadyBound)
    {
        consumer->impl_->preparedCursors_.push_back({
            sourceFrame,
            preparedDirection,
        });
    }
    return true;
}

bool DecodedBlockCacheOwner::prepareRestartWindow(
    const std::shared_ptr<AudioStreamConsumer>& consumer,
    double sourceFrame,
    double sourceFrameIncrement,
    std::size_t maximumOutputFrames,
    AudioStreamPreparedWindowBudget& budget)
{
    if (!prepareReadWindow(
            consumer,
            sourceFrame,
            sourceFrameIncrement,
            maximumOutputFrames,
            budget))
    {
        return false;
    }
    consumer->impl_->preparedRestartSourceFrame_ = sourceFrame;
    consumer->impl_->preparedRestartDirection_ =
        sourceFrameIncrement < 0.0 ? -1 : 1;
    consumer->impl_->hasPreparedRestart_ = true;
    return true;
}

std::size_t DecodedBlockCacheOwner::pumpForTest(
    std::size_t maximumBlocks)
{
    if (impl_ == nullptr
        || impl_->configuration_.workerMode
            != AudioStreamWorkerMode::ManualTest)
    {
        return 0;
    }
    return impl_->pump(maximumBlocks);
}

AudioStreamChannel DecodedBlockCacheOwner::channel() const noexcept
{
    return impl_ != nullptr ? impl_->layout_.channel : AudioStreamChannel::Mix;
}

std::string_view DecodedBlockCacheOwner::resourceId() const noexcept
{
    return impl_ != nullptr && impl_->snapshot_ != nullptr
        ? std::string_view{impl_->snapshot_->id}
        : std::string_view{};
}

AudioStreamFullLoadResult
DecodedBlockCacheOwner::loadAllForOffline() const
{
    AudioStreamFullLoadResult result;
    if (impl_ == nullptr || impl_->snapshot_ == nullptr)
    {
        result.error = "Audio stream owner is not prepared";
        return result;
    }
    if (impl_->layout_.frameCount
        > std::numeric_limits<std::size_t>::max())
    {
        result.error = "PCM16-WAV frame count exceeds offline address space";
        return result;
    }

    try
    {
        auto decoded = std::make_shared<DecodedAudioData>();
        decoded->sampleRate = impl_->layout_.sampleRate;
        decoded->monoSamples.resize(
            static_cast<std::size_t>(
                impl_->layout_.frameCount),
            0.0);

        std::ifstream input(
            impl_->backingPath_,
            std::ios::binary);
        if (!input)
        {
            result.error = "Unable to open immutable PCM16-WAV backing for offline decode";
            return result;
        }
        input.seekg(static_cast<std::streamoff>(
            impl_->layout_.dataOffset));
        constexpr std::size_t chunkFrames = 4096;
        std::vector<std::uint8_t> bytes(
            chunkFrames
                * static_cast<std::size_t>(
                    impl_->layout_.blockAlign),
            0);
        std::size_t firstFrame = 0;
        while (firstFrame < decoded->monoSamples.size())
        {
            const auto frames = std::min(
                chunkFrames,
                decoded->monoSamples.size() - firstFrame);
            const auto byteCount = frames
                * static_cast<std::size_t>(
                    impl_->layout_.blockAlign);
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(byteCount));
            if (!input
                || input.gcount()
                    != static_cast<std::streamsize>(
                        byteCount))
            {
                result.error = "Unable to decode complete immutable PCM16-WAV backing offline";
                return result;
            }
            if (!decodePcm16MonoFrames(
                    std::span<const std::uint8_t>{bytes.data(), byteCount},
                    impl_->layout_,
                    frames,
                    std::span<double>{
                        decoded->monoSamples.data() + firstFrame,
                        frames}))
            {
                result.error = "Unable to decode strict immutable PCM16-WAV backing offline";
                return result;
            }
            firstFrame += frames;
        }
        result.data = std::move(decoded);
    }
    catch (const std::bad_alloc&)
    {
        result.allocationFailed = true;
        result.error = "Unable to allocate offline PCM16-WAV decode";
    }
    catch (const std::length_error&)
    {
        result.allocationFailed = true;
        result.error = "Offline PCM16-WAV decode exceeds vector limits";
    }
    catch (const std::exception& exception)
    {
        result.error = "Unable to allocate offline PCM16-WAV decode: ";
        result.error += exception.what();
    }
    return result;
}

AudioStreamPrepareResult prepareAudioStream(
    talas::core::ResourceSnapshotPtr snapshot,
    AudioStreamConfiguration configuration,
    AudioStreamCancellationCheck shouldCancel)
{
    AudioStreamPrepareResult result;
    if (shouldCancel && shouldCancel())
    {
        result.cancelled = true;
        result.error = "Audio stream preparation cancelled";
        return result;
    }
    if (snapshot == nullptr
        || snapshot->kind
            != talas::core::ResourceSnapshotKind::Audio
        || snapshot->generation == 0
        || !talas::project::isCanonicalProjectAssetContentHash(
            snapshot->contentHash)
        || snapshot->resolvedPath.empty()
        || (snapshot->channelCount != 1
            && snapshot->channelCount != 2)
        || !std::isfinite(snapshot->sampleRate)
        || snapshot->sampleRate <= 0.0
        || snapshot->frameCount == 0
        || (snapshot->encoding != talas::project::pcm16WavAssetEncoding
            && (snapshot->encoding != talas::project::sampleFileAssetEncoding
                || !snapshot->audioBytes.empty()))
        || !snapshot->tableSamples.empty()
        || !snapshot->tableRightSamples.empty()
        || !snapshot->binaryBytes.empty()
        || (snapshot->provenance.starts_with(
                talas::project::diskCacheCaptureProvenanceNamespace)
            != !snapshot->audioBytes.empty())
        || (!snapshot->audioBytes.empty()
            && snapshot->audioBytes.size()
                > 44U + 2U * static_cast<std::size_t>(
                    talas::project::maximumDiskCacheCaptureFrames)))
    {
        result.error = "Audio stream requires a canonical immutable Audio resource snapshot";
        return result;
    }
    if (!isConfigurationValid(configuration, result.error))
        return result;

    const auto backingDirectory =
        makePrivateBackingDirectory(result.error);
    if (!backingDirectory.has_value())
        return result;
    const auto backingPath =
        *backingDirectory / "generation.wav";
    const auto cleanup = [&] {
        std::error_code cleanupError;
        std::filesystem::remove_all(
            *backingDirectory,
            cleanupError);
    };
    const auto copied = snapshot->audioBytes.empty()
        ? copyToPrivateBacking(
              snapshot->resolvedPath,
              backingPath,
              shouldCancel,
              result.cancelled,
              result.error)
        : writeRetainedBytesToPrivateBacking(
              snapshot->audioBytes,
              backingPath,
              shouldCancel,
              result.cancelled,
              result.error);
    if (!copied)
    {
        cleanup();
        return result;
    }

    const auto copiedHash =
        talas::project::calculateProjectAssetSha256(
            backingPath,
            shouldCancel);
    if (!copiedHash.ok())
    {
        result.cancelled = copiedHash.error.has_value()
            && copiedHash.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        result.error = copiedHash.error.has_value()
            ? copiedHash.error->message
            : "Unable to verify immutable audio stream backing";
        cleanup();
        return result;
    }
    if (copiedHash.contentHash
        != snapshot->contentHash)
    {
        result.error = "Audio asset changed while preparing its immutable stream backing";
        cleanup();
        return result;
    }

    auto layout = snapshot->encoding == talas::project::sampleFileAssetEncoding
        ? probeSampleFile(backingPath, result.error)
        : probePcm16Wav(backingPath, result.error);
    if (!layout.has_value())
    {
        cleanup();
        return result;
    }
    layout->channel = configuration.channel;
    if (layout->channelCount != snapshot->channelCount
        || layout->sampleRate != snapshot->sampleRate
        || layout->frameCount != snapshot->frameCount)
    {
        result.error = "PCM16-WAV backing media shape does not match its immutable snapshot";
        cleanup();
        return result;
    }

    try
    {
        auto owner = std::shared_ptr<DecodedBlockCacheOwner>(
            new DecodedBlockCacheOwner(
                std::make_unique<
                    DecodedBlockCacheOwner::Impl>(
                        snapshot,
                        configuration,
                        *layout,
                        *backingDirectory,
                        backingPath)));
        if (!owner->impl_->input_)
        {
            result.error = "Unable to open immutable PCM16-WAV worker backing";
            return result;
        }
        owner->impl_->start();
        result.owner = std::move(owner);
    }
    catch (const std::exception& exception)
    {
        result.error = "Unable to allocate bounded audio stream state: ";
        result.error += exception.what();
        cleanup();
    }
    return result;
}
}
