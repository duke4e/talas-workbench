#include "talas/audio/Pcm16WaveSpan.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

namespace talas::audio
{
namespace
{
constexpr std::uint16_t pcm16BytesPerSample = 2U;

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

[[nodiscard]] std::uint16_t readU16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t readU32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] Pcm16WaveParseResult failed(
    Pcm16WaveDiagnostic diagnostic) noexcept
{
    return {{}, diagnostic};
}

[[nodiscard]] double decodePcm16(std::uint16_t raw) noexcept
{
    const auto signedValue = raw < 0x8000U
        ? static_cast<std::int32_t>(raw)
        : static_cast<std::int32_t>(raw) - 0x1'0000;
    return pcm16SampleValue(static_cast<std::int16_t>(signedValue));
}
}

Pcm16WaveParseResult parseStrictPcm16Wave(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maximumSampleRate) noexcept
{
    if (maximumSampleRate == 0U || bytes.size() < 44U
        || !hasAscii(bytes, 0U, "RIFF")
        || !hasAscii(bytes, 8U, "WAVE"))
    {
        return failed(Pcm16WaveDiagnostic::ContainerInvalid);
    }
    const auto declaredBytes =
        static_cast<std::uint64_t>(readU32(bytes, 4U)) + 8U;
    if (declaredBytes != bytes.size())
        return failed(Pcm16WaveDiagnostic::ContainerInvalid);

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
    std::size_t offset = 12U;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 8U)
            return failed(Pcm16WaveDiagnostic::ChunkInvalid);
        const auto chunkBytes = readU32(bytes, offset + 4U);
        const auto chunkDataOffset = offset + 8U;
        if (chunkBytes > bytes.size() - chunkDataOffset)
            return failed(Pcm16WaveDiagnostic::ChunkInvalid);
        const auto chunkEnd =
            chunkDataOffset + static_cast<std::size_t>(chunkBytes);

        if (hasAscii(bytes, offset, "fmt "))
        {
            if (foundFormat || chunkBytes != 16U)
                return failed(Pcm16WaveDiagnostic::FormatInvalid);
            audioFormat = readU16(bytes, chunkDataOffset);
            channelCount = readU16(bytes, chunkDataOffset + 2U);
            sampleRate = readU32(bytes, chunkDataOffset + 4U);
            byteRate = readU32(bytes, chunkDataOffset + 8U);
            blockAlign = readU16(bytes, chunkDataOffset + 12U);
            bitsPerSample = readU16(bytes, chunkDataOffset + 14U);
            foundFormat = true;
        }
        else if (hasAscii(bytes, offset, "data"))
        {
            if (foundData)
                return failed(Pcm16WaveDiagnostic::FormatInvalid);
            dataBytes = chunkBytes;
            dataOffset = chunkDataOffset;
            foundData = true;
        }

        offset = chunkEnd;
        if ((chunkBytes & 1U) != 0U)
        {
            if (offset >= bytes.size() || bytes[offset] != 0U)
                return failed(Pcm16WaveDiagnostic::ChunkInvalid);
            ++offset;
        }
    }
    if (offset != bytes.size() || !foundFormat || !foundData)
        return failed(Pcm16WaveDiagnostic::FormatInvalid);
    if (audioFormat != 1U || bitsPerSample != 16U
        || (channelCount != 1U && channelCount != 2U)
        || sampleRate == 0U || sampleRate > maximumSampleRate)
    {
        return failed(Pcm16WaveDiagnostic::FormatInvalid);
    }

    const auto expectedBlockAlign =
        static_cast<std::uint32_t>(channelCount) * pcm16BytesPerSample;
    const auto expectedByteRate =
        static_cast<std::uint64_t>(sampleRate) * expectedBlockAlign;
    if (blockAlign != expectedBlockAlign
        || expectedByteRate > std::numeric_limits<std::uint32_t>::max()
        || byteRate != expectedByteRate || dataBytes == 0U
        || dataBytes % expectedBlockAlign != 0U)
    {
        return failed(Pcm16WaveDiagnostic::LayoutInvalid);
    }

    return {
        {
            channelCount,
            sampleRate,
            dataBytes / expectedBlockAlign,
            dataOffset,
            blockAlign,
        },
        Pcm16WaveDiagnostic::Valid,
    };
}

bool decodePcm16WaveFrame(
    std::span<const std::uint8_t> bytes,
    const Pcm16WaveLayout& layout,
    std::uint64_t frameIndex,
    std::span<double> channels) noexcept
{
    if ((layout.channelCount != 1U && layout.channelCount != 2U)
        || channels.size() != layout.channelCount
        || layout.blockAlign
            != static_cast<std::uint16_t>(
                layout.channelCount * pcm16BytesPerSample)
        || frameIndex >= layout.frameCount
        || layout.dataOffset > bytes.size()
        || frameIndex
            > (std::numeric_limits<std::uint64_t>::max()
                - layout.dataOffset)
                / layout.blockAlign)
    {
        return false;
    }
    const auto frameOffset = layout.dataOffset
        + frameIndex * layout.blockAlign;
    const auto frameBytes = static_cast<std::uint64_t>(layout.channelCount)
        * pcm16BytesPerSample;
    if (frameOffset > bytes.size()
        || frameBytes > bytes.size() - frameOffset)
    {
        return false;
    }
    const auto byteOffset = static_cast<std::size_t>(frameOffset);
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        channels[channel] = decodePcm16(
            readU16(bytes, byteOffset + channel * pcm16BytesPerSample));
    }
    return true;
}
}
