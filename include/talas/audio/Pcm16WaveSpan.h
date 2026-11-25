#pragma once

#include <cstdint>
#include <limits>
#include <span>

namespace talas::audio
{
enum class Pcm16WaveDiagnostic : std::uint8_t
{
    Valid,
    ContainerInvalid,
    ChunkInvalid,
    FormatInvalid,
    LayoutInvalid,
};

struct Pcm16WaveLayout
{
    std::uint16_t channelCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t dataOffset = 0;
    std::uint16_t blockAlign = 0;
};

struct Pcm16WaveParseResult
{
    Pcm16WaveLayout layout;
    Pcm16WaveDiagnostic diagnostic =
        Pcm16WaveDiagnostic::ContainerInvalid;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostic == Pcm16WaveDiagnostic::Valid;
    }
};

// Parses one exact RIFF/WAVE container from an immutable byte span. The
// accepted profile is deliberately closed: canonical PCM16, one or two
// channels, one 16-byte fmt chunk, one non-empty data chunk, exact RIFF size,
// and zero-valued odd-chunk padding.
[[nodiscard]] Pcm16WaveParseResult parseStrictPcm16Wave(
    std::span<const std::uint8_t> bytes,
    std::uint32_t maximumSampleRate =
        std::numeric_limits<std::uint32_t>::max()) noexcept;

// The one PCM16 sample convention, the inverse of the WAV writer: full scale
// is 32767 and the extra negative code -32768 decodes to exactly -1.
[[nodiscard]] constexpr double pcm16SampleValue(std::int16_t sample) noexcept
{
    return sample == std::numeric_limits<std::int16_t>::min()
        ? -1.0
        : static_cast<double>(sample)
            / static_cast<double>(std::numeric_limits<std::int16_t>::max());
}

// Decodes one exact interleaved source frame into a caller-owned channel span.
// No allocation, I/O, synchronization, logging, or hidden format work occurs.
[[nodiscard]] bool decodePcm16WaveFrame(
    std::span<const std::uint8_t> bytes,
    const Pcm16WaveLayout& layout,
    std::uint64_t frameIndex,
    std::span<double> channels) noexcept;
}
