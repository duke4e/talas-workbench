#include "talas/audio/WavWriter.h"

#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <string>
#include <utility>

namespace talas::audio
{
namespace
{
constexpr std::uint16_t pcmFormat = 1;
constexpr std::uint32_t fmtChunkSize = 16;
constexpr std::uint32_t riffHeaderPayloadSizeWithoutData = 36;

struct WavLayout
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint32_t dataChunkSize = 0;
};

bool writeBytes(std::FILE* output, const void* bytes, std::size_t size)
{
    return output != nullptr
        && std::fwrite(bytes, 1, size, output) == size;
}

bool writeAscii(std::FILE* output, const char (&text)[5])
{
    return writeBytes(output, text, 4);
}

bool writeU16(std::FILE* output, std::uint16_t value)
{
    const char bytes[2] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
    };
    return writeBytes(output, bytes, 2);
}

bool writeU32(std::FILE* output, std::uint32_t value)
{
    const char bytes[4] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu),
    };
    return writeBytes(output, bytes, 4);
}

// One sample in a word of `bits`, little-endian; the largest positive code
// is 1 and the extra negative code -1, as the readers decode them. 8-bit
// words are unsigned around 128.
bool writeSample(std::FILE* output, double sample, std::uint16_t bits)
{
    const double clipped = std::clamp(sample, -1.0, 1.0);
    const auto half = std::int64_t{1} << (bits - 1);
    auto code = clipped <= -1.0
        ? -half
        : static_cast<std::int64_t>(std::llround(clipped * static_cast<double>(half - 1)));
    if (bits == 8)
        code += half;
    char bytes[3] = {};
    for (std::uint16_t index = 0; index < bits / 8; ++index)
        bytes[index] = static_cast<char>((static_cast<std::uint64_t>(code) >> (8 * index)) & 0xffu);
    return writeBytes(output, bytes, bits / 8);
}

std::int16_t quantizePcm16(double sample)
{
    const double clipped = std::clamp(sample, -1.0, 1.0);
    if (clipped <= -1.0)
        return std::numeric_limits<std::int16_t>::min();
    return static_cast<std::int16_t>(
        std::lround(clipped * static_cast<double>(std::numeric_limits<std::int16_t>::max())));
}

bool describeLayout(
    double sampleRate,
    std::size_t channelCount,
    std::size_t frameCount,
    bool allowEmpty,
    WavLayout& layout,
    std::string& error,
    std::uint16_t bitsPerSample = 16)
{
    const auto bytesPerSample = static_cast<std::size_t>(bitsPerSample / 8);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        error = "sample rate must be finite and greater than zero";
        return false;
    }
    if (sampleRate > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        error = "sample rate is too large for a WAV header";
        return false;
    }
    if (std::floor(sampleRate) != sampleRate)
    {
        error = "WAV sample rate must be a whole number of hertz";
        return false;
    }
    if (channelCount == 0)
    {
        error = "WAV export requires at least one channel";
        return false;
    }
    if (channelCount > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
    {
        error = "channel count is too large for a WAV header";
        return false;
    }
    if (frameCount == 0 && !allowEmpty)
    {
        error = "WAV export requires at least one frame";
        return false;
    }

    const auto blockAlignWide = channelCount * bytesPerSample;
    if (blockAlignWide > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
    {
        error = "channel count is too large for a PCM16 WAV block alignment";
        return false;
    }
    const auto maximumDataChunkSize = static_cast<std::size_t>(
        std::numeric_limits<std::uint32_t>::max() - riffHeaderPayloadSizeWithoutData);
    if (frameCount > maximumDataChunkSize / blockAlignWide)
    {
        error = "rendered audio is too large for a RIFF/WAVE file";
        return false;
    }
    const auto dataChunkSizeWide = frameCount * blockAlignWide;

    layout.sampleRate = static_cast<std::uint32_t>(std::llround(sampleRate));
    if (layout.sampleRate == 0)
    {
        error = "sample rate rounds to zero";
        return false;
    }
    layout.channelCount = static_cast<std::uint16_t>(channelCount);
    layout.blockAlign = static_cast<std::uint16_t>(blockAlignWide);
    layout.dataChunkSize = static_cast<std::uint32_t>(dataChunkSizeWide);

    const auto byteRateWide =
        static_cast<unsigned long long>(layout.sampleRate) * layout.blockAlign;
    if (byteRateWide > std::numeric_limits<std::uint32_t>::max())
    {
        error = "byte rate is too large for a WAV header";
        return false;
    }
    layout.byteRate = static_cast<std::uint32_t>(byteRateWide);
    return true;
}

bool writeHeader(std::FILE* output, const WavLayout& layout, std::uint16_t bitsPerSample = 16)
{
    return writeAscii(output, "RIFF")
        && writeU32(output, riffHeaderPayloadSizeWithoutData + layout.dataChunkSize)
        && writeAscii(output, "WAVE")
        && writeAscii(output, "fmt ")
        && writeU32(output, fmtChunkSize)
        && writeU16(output, pcmFormat)
        && writeU16(output, layout.channelCount)
        && writeU32(output, layout.sampleRate)
        && writeU32(output, layout.byteRate)
        && writeU16(output, layout.blockAlign)
        && writeU16(output, bitsPerSample)
        && writeAscii(output, "data")
        && writeU32(output, layout.dataChunkSize);
}

std::FILE* openBinaryOutput(const std::filesystem::path& path)
{
#if defined(_WIN32)
    return ::_wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}
}

struct Pcm16WavStreamWriter::Impl
{
    std::FILE* output = nullptr;
    std::string error;
    std::size_t channelCount = 0;
    std::size_t totalFrameCount = 0;
    std::size_t framesWritten = 0;
    double sampleRate = 0.0;
    std::uint16_t bitsPerSample = 16;
    bool growing = false;
    bool finished = false;
    std::string diagnosticPath;

    ~Impl()
    {
        if (output != nullptr)
            std::fclose(output);
    }
};

Pcm16WavStreamWriter::Pcm16WavStreamWriter(
    const std::filesystem::path& path,
    double sampleRate,
    std::size_t channelCount,
    std::size_t totalFrameCount)
    : impl_(std::make_unique<Impl>())
{
    WavLayout layout;
    if (!describeLayout(
            sampleRate,
            channelCount,
            totalFrameCount,
            false,
            layout,
            impl_->error))
        return;

    impl_->diagnosticPath = talas::project::pathToUtf8(path);
    impl_->output = openBinaryOutput(path);
    if (impl_->output == nullptr)
    {
        impl_->error = "failed to open WAV file for writing: "
            + talas::project::pathToUtf8(path);
        return;
    }
    impl_->channelCount = channelCount;
    impl_->totalFrameCount = totalFrameCount;
    impl_->sampleRate = sampleRate;
    if (!writeHeader(impl_->output, layout))
        impl_->error = "failed while writing WAV header: "
            + talas::project::pathToUtf8(path);
}

Pcm16WavStreamWriter::Pcm16WavStreamWriter(
    std::FILE* output,
    std::string diagnosticPath,
    double sampleRate,
    std::size_t channelCount,
    std::size_t totalFrameCount)
    : impl_(std::make_unique<Impl>())
{
    impl_->output = output;
    impl_->diagnosticPath = std::move(diagnosticPath);
    WavLayout layout;
    if (output == nullptr
        || !describeLayout(
            sampleRate,
            channelCount,
            totalFrameCount,
            false,
            layout,
            impl_->error))
    {
        if (output == nullptr && impl_->error.empty())
            impl_->error = "secure WAV output stream is unavailable";
        return;
    }
    impl_->channelCount = channelCount;
    impl_->totalFrameCount = totalFrameCount;
    impl_->sampleRate = sampleRate;
    if (!writeHeader(impl_->output, layout))
        impl_->error = "failed while writing WAV header: " + impl_->diagnosticPath;
}

Pcm16WavStreamWriter::Pcm16WavStreamWriter(
    const std::filesystem::path& path,
    double sampleRate,
    std::size_t channelCount)
    : impl_(std::make_unique<Impl>())
{
    WavLayout layout;
    if (!describeLayout(
            sampleRate,
            channelCount,
            0,
            true,
            layout,
            impl_->error))
    {
        return;
    }

    impl_->diagnosticPath = talas::project::pathToUtf8(path);
    impl_->output = openBinaryOutput(path);
    if (impl_->output == nullptr)
    {
        impl_->error = "failed to open WAV file for writing: "
            + talas::project::pathToUtf8(path);
        return;
    }
    impl_->channelCount = channelCount;
    impl_->sampleRate = sampleRate;
    impl_->growing = true;
    if (!writeHeader(impl_->output, layout))
    {
        impl_->error = "failed while writing WAV header: "
            + talas::project::pathToUtf8(path);
    }
}

Pcm16WavStreamWriter::Pcm16WavStreamWriter(
    std::FILE* output,
    std::string diagnosticPath,
    double sampleRate,
    std::size_t channelCount)
    : impl_(std::make_unique<Impl>())
{
    impl_->output = output;
    impl_->diagnosticPath = std::move(diagnosticPath);
    WavLayout layout;
    if (output == nullptr
        || !describeLayout(
            sampleRate,
            channelCount,
            0,
            true,
            layout,
            impl_->error))
    {
        if (output == nullptr && impl_->error.empty())
            impl_->error = "secure WAV output stream is unavailable";
        return;
    }
    impl_->channelCount = channelCount;
    impl_->sampleRate = sampleRate;
    impl_->growing = true;
    if (!writeHeader(impl_->output, layout))
        impl_->error = "failed while writing WAV header: " + impl_->diagnosticPath;
}

Pcm16WavStreamWriter::Pcm16WavStreamWriter(
    std::FILE* output,
    std::string diagnosticPath,
    double sampleRate,
    std::size_t channelCount,
    WavWordSize wordSize)
    : impl_(std::make_unique<Impl>())
{
    impl_->output = output;
    impl_->diagnosticPath = std::move(diagnosticPath);
    if (wordSize.bits != 8 && wordSize.bits != 16 && wordSize.bits != 24)
    {
        impl_->error = "WAV word size must be 8, 16 or 24 bits";
        return;
    }
    WavLayout layout;
    if (output == nullptr
        || !describeLayout(
            sampleRate,
            channelCount,
            0,
            true,
            layout,
            impl_->error,
            wordSize.bits))
    {
        if (output == nullptr && impl_->error.empty())
            impl_->error = "secure WAV output stream is unavailable";
        return;
    }
    impl_->channelCount = channelCount;
    impl_->sampleRate = sampleRate;
    impl_->bitsPerSample = wordSize.bits;
    impl_->growing = true;
    if (!writeHeader(impl_->output, layout, wordSize.bits))
        impl_->error = "failed while writing WAV header: " + impl_->diagnosticPath;
}

Pcm16WavStreamWriter::~Pcm16WavStreamWriter() = default;
Pcm16WavStreamWriter::Pcm16WavStreamWriter(Pcm16WavStreamWriter&&) noexcept = default;
Pcm16WavStreamWriter& Pcm16WavStreamWriter::operator=(Pcm16WavStreamWriter&&) noexcept = default;

bool Pcm16WavStreamWriter::ready() const
{
    return impl_ != nullptr && impl_->error.empty() && !impl_->finished;
}

std::string_view Pcm16WavStreamWriter::error() const
{
    return impl_ != nullptr ? std::string_view{impl_->error} : std::string_view{"WAV writer was moved"};
}

std::size_t Pcm16WavStreamWriter::framesWritten() const
{
    return impl_ != nullptr ? impl_->framesWritten : 0;
}

std::size_t Pcm16WavStreamWriter::remainingFrameCapacity() const
{
    if (impl_ == nullptr || impl_->channelCount == 0)
        return 0;
    const auto blockAlign = impl_->channelCount * (impl_->bitsPerSample / 8U);
    const auto maximumFrames = static_cast<std::size_t>(
        std::numeric_limits<std::uint32_t>::max() - riffHeaderPayloadSizeWithoutData)
        / blockAlign;
    return maximumFrames > impl_->framesWritten ? maximumFrames - impl_->framesWritten : 0;
}

bool Pcm16WavStreamWriter::write(std::span<const std::vector<double>> channels)
{
    if (!ready())
        return false;
    if (channels.size() != impl_->channelCount)
    {
        impl_->error = "WAV block channel count does not match the declared channel count";
        return false;
    }

    const auto blockFrames = channels.front().size();
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        if (channels[channel].size() != blockFrames)
        {
            impl_->error = "all WAV channels must have the same frame count";
            return false;
        }
        for (std::size_t frame = 0; frame < blockFrames; ++frame)
        {
            if (!std::isfinite(channels[channel][frame]))
            {
                impl_->error = "WAV export cannot encode a non-finite sample at channel "
                    + std::to_string(channel)
                    + ", frame "
                    + std::to_string(impl_->framesWritten + frame);
                return false;
            }
        }
    }
    if (!impl_->growing
        && blockFrames > impl_->totalFrameCount - impl_->framesWritten)
    {
        impl_->error = "WAV block exceeds the declared frame count";
        return false;
    }
    if (impl_->growing)
    {
        if (blockFrames
            > std::numeric_limits<std::size_t>::max()
                - impl_->framesWritten)
        {
            impl_->error = "rendered audio is too large for a RIFF/WAVE file";
            return false;
        }
        WavLayout layout;
        std::string layoutError;
        if (!describeLayout(
                impl_->sampleRate,
                impl_->channelCount,
                impl_->framesWritten + blockFrames,
                false,
                layout,
                layoutError,
                impl_->bitsPerSample))
        {
            impl_->error = std::move(layoutError);
            return false;
        }
    }

    for (std::size_t frame = 0; frame < blockFrames; ++frame)
    {
        for (const auto& channel : channels)
            if (impl_->bitsPerSample == 16
                    ? !writeU16(impl_->output, static_cast<std::uint16_t>(quantizePcm16(channel[frame])))
                    : !writeSample(impl_->output, channel[frame], impl_->bitsPerSample))
            {
                impl_->error = "failed while writing WAV sample data";
                return false;
            }
    }
    impl_->framesWritten += blockFrames;
    return true;
}

bool Pcm16WavStreamWriter::writeInterleaved(
    std::span<const float> samples)
{
    if (!ready())
        return false;
    if (samples.empty()
        || samples.size() % impl_->channelCount != 0)
    {
        impl_->error =
            "interleaved WAV block does not contain complete frames";
        return false;
    }
    const auto blockFrames = samples.size() / impl_->channelCount;
    if (!impl_->growing
        && blockFrames > impl_->totalFrameCount - impl_->framesWritten)
    {
        impl_->error = "WAV block exceeds the declared frame count";
        return false;
    }
    if (blockFrames
        > std::numeric_limits<std::size_t>::max()
            - impl_->framesWritten)
    {
        impl_->error = "rendered audio is too large for a RIFF/WAVE file";
        return false;
    }
    WavLayout layout;
    std::string layoutError;
    if (!describeLayout(
            impl_->sampleRate,
            impl_->channelCount,
            impl_->framesWritten + blockFrames,
            false,
            layout,
            layoutError,
            impl_->bitsPerSample))
    {
        impl_->error = std::move(layoutError);
        return false;
    }
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        if (!std::isfinite(samples[index]))
        {
            impl_->error =
                "WAV export cannot encode a non-finite interleaved sample at index "
                + std::to_string(index);
            return false;
        }
    }
    for (const auto sample : samples)
    {
        if (impl_->bitsPerSample == 16
                ? !writeU16(impl_->output, static_cast<std::uint16_t>(quantizePcm16(sample)))
                : !writeSample(impl_->output, sample, impl_->bitsPerSample))
        {
            impl_->error = "failed while writing WAV sample data";
            return false;
        }
    }
    impl_->framesWritten += blockFrames;
    return true;
}

bool Pcm16WavStreamWriter::finish()
{
    if (!ready())
        return false;
    if (impl_->growing && impl_->framesWritten == 0)
    {
        impl_->error = "WAV writer did not receive any frames";
        return false;
    }
    if (!impl_->growing
        && impl_->framesWritten != impl_->totalFrameCount)
    {
        impl_->error = "WAV writer did not receive the declared frame count";
        return false;
    }
    if (impl_->growing)
    {
        WavLayout layout;
        if (!describeLayout(
                impl_->sampleRate,
                impl_->channelCount,
                impl_->framesWritten,
                false,
                layout,
                impl_->error,
                impl_->bitsPerSample))
        {
            return false;
        }
        if (std::fseek(impl_->output, 0, SEEK_SET) != 0
            || !writeHeader(impl_->output, layout, impl_->bitsPerSample))
        {
            impl_->error = "failed while finalizing WAV header";
            return false;
        }
    }
    if (std::fflush(impl_->output) != 0)
    {
        impl_->error = "failed while flushing WAV sample data";
        return false;
    }
    if (std::fclose(impl_->output) != 0)
    {
        impl_->output = nullptr;
        impl_->error = "failed while closing WAV file";
        return false;
    }
    impl_->output = nullptr;
    impl_->finished = true;
    return true;
}

void Pcm16WavStreamWriter::abandon() noexcept
{
    if (impl_ == nullptr || impl_->finished)
        return;
    if (impl_->output != nullptr)
    {
        std::fclose(impl_->output);
        impl_->output = nullptr;
    }
    impl_->finished = true;
}

bool writePcm16Wav(
    const std::filesystem::path& path,
    double sampleRate,
    std::span<const std::vector<double>> channels,
    std::string& error)
{
    error.clear();
    if (channels.empty())
    {
        error = "WAV export requires at least one channel";
        return false;
    }
    const auto frameCount = channels.front().size();
    Pcm16WavStreamWriter writer(path, sampleRate, channels.size(), frameCount);
    if (!writer.ready() || !writer.write(channels) || !writer.finish())
    {
        error = std::string(writer.error());
        return false;
    }
    return true;
}

bool writePcmWav(
    const std::filesystem::path& path,
    double sampleRate,
    std::span<const std::vector<double>> channels,
    WavWordSize wordSize,
    std::string& error)
{
    if (wordSize.bits == 16)
        return writePcm16Wav(path, sampleRate, channels, error);
    error.clear();
    if (channels.empty())
    {
        error = "WAV export requires at least one channel";
        return false;
    }
    auto* output = openBinaryOutput(path);
    if (output == nullptr)
    {
        error = "failed to open WAV file for writing: " + talas::project::pathToUtf8(path);
        return false;
    }
    Pcm16WavStreamWriter writer(output, talas::project::pathToUtf8(path), sampleRate, channels.size(), wordSize);
    if (!writer.ready() || !writer.write(channels) || !writer.finish())
    {
        error = std::string(writer.error());
        return false;
    }
    return true;
}
}
