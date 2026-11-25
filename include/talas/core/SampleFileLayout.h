#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Where a WAV or AIFF sample file keeps its frames and how it stores each
// sample, read from its headers alone, so that a player can decode frames
// as it reads them (a DiskPlayer plays a recording imported from another
// program in any of the sample file formats).
namespace talas::core
{
struct SampleWordEncoding
{
    enum class Kind : std::uint8_t
    {
        UnsignedInteger,
        SignedInteger,
        Float,
    };

    Kind kind = Kind::SignedInteger;
    std::uint16_t bytesPerSample = 2;
    bool bigEndian = false;

    bool operator==(const SampleWordEncoding&) const = default;
};

struct SampleFileLayout
{
    std::uint16_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::uint64_t dataOffset = 0;
    std::uint16_t blockAlign = 0;
    SampleWordEncoding encoding;
};

// One sample, full scale as the WAV writer's: the largest positive code is
// 1 and the extra negative code exactly -1. `bytes` holds the word.
[[nodiscard]] inline double decodeSampleWord(
    std::span<const std::uint8_t> bytes,
    SampleWordEncoding encoding) noexcept
{
    std::uint64_t code = 0;
    for (std::size_t index = 0; index < encoding.bytesPerSample; ++index)
    {
        const auto byte = encoding.bigEndian
            ? bytes[index]
            : bytes[encoding.bytesPerSample - 1U - index];
        code = (code << 8U) | byte;
    }
    if (encoding.kind == SampleWordEncoding::Kind::Float)
    {
        return encoding.bytesPerSample == 4U
            ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(code)))
            : std::bit_cast<double>(code);
    }
    const auto half = std::uint64_t{1} << (8U * encoding.bytesPerSample - 1U);
    const auto signedCode = encoding.kind == SampleWordEncoding::Kind::UnsignedInteger
        ? static_cast<std::int64_t>(code) - static_cast<std::int64_t>(half)
        : (code >= half
                ? static_cast<std::int64_t>(code) - static_cast<std::int64_t>(half << 1U)
                : static_cast<std::int64_t>(code));
    if (signedCode == -static_cast<std::int64_t>(half))
        return -1.0;
    return static_cast<double>(signedCode) / static_cast<double>(half - 1U);
}

namespace sample_file_layout_detail
{
[[nodiscard]] inline bool hasTag(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view tag) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < tag.size())
        return false;
    for (std::size_t index = 0; index < tag.size(); ++index)
    {
        if (bytes[offset + index] != static_cast<std::uint8_t>(tag[index]))
            return false;
    }
    return true;
}

[[nodiscard]] inline std::uint32_t readUnsigned(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::size_t count,
    bool bigEndian) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto byte = bigEndian ? bytes[offset + index] : bytes[offset + count - 1U - index];
        value = (value << 8U) | byte;
    }
    return value;
}

// The 80-bit IEEE extended sample rate of an AIFF COMM chunk.
[[nodiscard]] inline std::optional<double> readExtended(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    const auto signAndExponent = readUnsigned(bytes, offset, 2U, true);
    std::uint64_t mantissa = 0;
    for (std::size_t index = 0; index < 8U; ++index)
        mantissa = (mantissa << 8U) | bytes[offset + 2U + index];
    const auto exponent = static_cast<int>(signAndExponent & 0x7FFFU);
    if ((signAndExponent & 0x8000U) != 0U || exponent == 0x7FFF)
        return std::nullopt;
    const auto value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}
}

// The layout of a sample file of `size` bytes, which `readAt(offset, bytes)`
// fills from, answering whether it could: an uncompressed RIFF/WAVE file of
// 8-, 16-, 24- or 32-bit PCM or 32- or 64-bit float, extensible or not, or an
// AIFF or AIFF-C file (NONE, twos, sowt, fl32, fl64) of samples of 1 to 32
// bits, with at least one channel and one frame. Nothing, with `error` set,
// for anything else.
template <typename ReadAt>
[[nodiscard]] std::optional<SampleFileLayout> probeSampleFileLayout(
    std::uint64_t size,
    ReadAt&& readAt,
    std::string& error)
{
    using namespace sample_file_layout_detail;
    std::array<std::uint8_t, 12> container{};
    if (size < container.size() || !readAt(0U, std::span<std::uint8_t>{container}))
    {
        error = "Sample file is shorter than its container header";
        return std::nullopt;
    }
    const auto wave = hasTag(container, 0U, "RIFF") && hasTag(container, 8U, "WAVE");
    const auto aiff = hasTag(container, 0U, "FORM")
        && (hasTag(container, 8U, "AIFF") || hasTag(container, 8U, "AIFC"));
    if (!wave && !aiff)
    {
        error = "Sample file is neither a RIFF/WAVE nor an AIFF container";
        return std::nullopt;
    }
    const auto compressed = aiff && hasTag(container, 8U, "AIFC");

    SampleFileLayout layout;
    layout.encoding.bigEndian = aiff;
    bool hasFormat = false;
    bool hasData = false;
    std::uint16_t formatTag = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t declaredBlockAlign = 0;
    std::uint64_t dataBytes = 0;
    std::uint64_t offset = container.size();
    while (offset <= size && size - offset >= 8U)
    {
        std::array<std::uint8_t, 8> header{};
        if (!readAt(offset, std::span<std::uint8_t>{header}))
        {
            error = "Sample file chunk header could not be read";
            return std::nullopt;
        }
        const auto chunkBytes = static_cast<std::uint64_t>(readUnsigned(header, 4U, 4U, aiff));
        const auto chunkData = offset + 8U;
        if (chunkBytes > size - chunkData)
        {
            error = "Sample file chunk extends past the end of the file";
            return std::nullopt;
        }
        if ((wave && hasTag(header, 0U, "fmt ")) || (aiff && hasTag(header, 0U, "COMM")))
        {
            std::array<std::uint8_t, 40> format{};
            const auto minimum = wave ? 16U : (compressed ? 22U : 18U);
            if (hasFormat || chunkBytes < minimum)
            {
                error = hasFormat ? "Sample file has two format chunks" : "Sample file format chunk is too short";
                return std::nullopt;
            }
            const auto readable = static_cast<std::size_t>(std::min<std::uint64_t>(chunkBytes, format.size()));
            if (!readAt(chunkData, std::span<std::uint8_t>{format.data(), readable}))
            {
                error = "Sample file format chunk could not be read";
                return std::nullopt;
            }
            if (wave)
            {
                formatTag = static_cast<std::uint16_t>(readUnsigned(format, 0U, 2U, false));
                layout.channelCount = static_cast<std::uint16_t>(readUnsigned(format, 2U, 2U, false));
                layout.sampleRate = static_cast<double>(readUnsigned(format, 4U, 4U, false));
                declaredBlockAlign = static_cast<std::uint16_t>(readUnsigned(format, 12U, 2U, false));
                bitsPerSample = static_cast<std::uint16_t>(readUnsigned(format, 14U, 2U, false));
                // WAVE_FORMAT_EXTENSIBLE names its PCM or float format in the
                // first two bytes of its SubFormat GUID.
                if (formatTag == 0xFFFEU)
                {
                    if (chunkBytes < 40U)
                    {
                        error = "Sample file extensible format chunk is too short";
                        return std::nullopt;
                    }
                    formatTag = static_cast<std::uint16_t>(readUnsigned(format, 24U, 2U, false));
                }
            }
            else
            {
                layout.channelCount = static_cast<std::uint16_t>(readUnsigned(format, 0U, 2U, true));
                layout.frameCount = readUnsigned(format, 2U, 4U, true);
                bitsPerSample = static_cast<std::uint16_t>(readUnsigned(format, 6U, 2U, true));
                const auto rate = readExtended(format, 8U);
                if (!rate.has_value())
                {
                    error = "AIFF sample rate is invalid";
                    return std::nullopt;
                }
                layout.sampleRate = *rate;
                layout.encoding.kind = SampleWordEncoding::Kind::SignedInteger;
                if (compressed)
                {
                    if (hasTag(format, 18U, "sowt"))
                    {
                        layout.encoding.bigEndian = false;
                    }
                    else if (hasTag(format, 18U, "fl32") || hasTag(format, 18U, "FL32"))
                    {
                        layout.encoding.kind = SampleWordEncoding::Kind::Float;
                        bitsPerSample = 32;
                    }
                    else if (hasTag(format, 18U, "fl64") || hasTag(format, 18U, "FL64"))
                    {
                        layout.encoding.kind = SampleWordEncoding::Kind::Float;
                        bitsPerSample = 64;
                    }
                    else if (!hasTag(format, 18U, "NONE") && !hasTag(format, 18U, "twos"))
                    {
                        error = "AIFF-C compression is not supported";
                        return std::nullopt;
                    }
                }
            }
            hasFormat = true;
        }
        else if (wave && hasTag(header, 0U, "data"))
        {
            if (hasData)
            {
                error = "Sample file has two data chunks";
                return std::nullopt;
            }
            layout.dataOffset = chunkData;
            dataBytes = chunkBytes;
            hasData = true;
        }
        else if (aiff && hasTag(header, 0U, "SSND"))
        {
            std::array<std::uint8_t, 4> soundOffset{};
            if (hasData || chunkBytes < 8U || !readAt(chunkData, std::span<std::uint8_t>{soundOffset}))
            {
                error = "AIFF SSND chunk is repeated or too short";
                return std::nullopt;
            }
            const auto skip = static_cast<std::uint64_t>(readUnsigned(soundOffset, 0U, 4U, true));
            if (skip > chunkBytes - 8U)
            {
                error = "AIFF SSND offset lies past its chunk";
                return std::nullopt;
            }
            layout.dataOffset = chunkData + 8U + skip;
            dataBytes = chunkBytes - 8U - skip;
            hasData = true;
        }
        offset = chunkData + chunkBytes + (chunkBytes & 1U);
    }

    if (!hasFormat || !hasData)
    {
        error = hasFormat ? "Sample file has no sample data" : "Sample file has no format chunk";
        return std::nullopt;
    }
    if (wave)
    {
        // 8-bit WAV samples are unsigned, wider ones signed; float ones are
        // 32- or 64-bit IEEE.
        if (formatTag == 1U
            && (bitsPerSample == 8U || bitsPerSample == 16U || bitsPerSample == 24U || bitsPerSample == 32U))
        {
            layout.encoding.kind = bitsPerSample == 8U
                ? SampleWordEncoding::Kind::UnsignedInteger
                : SampleWordEncoding::Kind::SignedInteger;
        }
        else if (formatTag == 3U && (bitsPerSample == 32U || bitsPerSample == 64U))
        {
            layout.encoding.kind = SampleWordEncoding::Kind::Float;
        }
        else
        {
            error = "WAV sample files hold 8-, 16-, 24- or 32-bit PCM or 32- or 64-bit float samples";
            return std::nullopt;
        }
    }
    else if (layout.encoding.kind != SampleWordEncoding::Kind::Float
        && (bitsPerSample == 0U || bitsPerSample > 32U))
    {
        error = "AIFF sample files hold samples of 1 to 32 bits";
        return std::nullopt;
    }
    // Narrower AIFF samples are left-justified in whole bytes.
    layout.encoding.bytesPerSample = static_cast<std::uint16_t>((bitsPerSample + 7U) / 8U);
    if (layout.encoding.bytesPerSample == 1U)
        layout.encoding.bigEndian = true;
    if (layout.channelCount == 0U
        || layout.channelCount > std::numeric_limits<std::uint16_t>::max() / layout.encoding.bytesPerSample)
    {
        error = "Sample file channel count is invalid";
        return std::nullopt;
    }
    layout.blockAlign = static_cast<std::uint16_t>(layout.channelCount * layout.encoding.bytesPerSample);
    if (!(layout.sampleRate >= 1.0) || layout.sampleRate > 1.0e7)
    {
        error = "Sample file sample rate is out of range";
        return std::nullopt;
    }
    if (wave)
    {
        if (declaredBlockAlign != layout.blockAlign || dataBytes % layout.blockAlign != 0U)
        {
            error = "WAV block alignment is inconsistent with its channels and data";
            return std::nullopt;
        }
        layout.frameCount = dataBytes / layout.blockAlign;
    }
    else if (layout.frameCount > dataBytes / layout.blockAlign)
    {
        error = "AIFF SSND chunk holds fewer frames than COMM says";
        return std::nullopt;
    }
    if (layout.frameCount == 0U)
    {
        error = "Sample file holds no frames";
        return std::nullopt;
    }
    return layout;
}
}
