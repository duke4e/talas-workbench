#include "talas/audio/WavReader.h"

#include "talas/audio/Pcm16WaveSpan.h"
#include "talas/audio/SampleFileFormats.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
constexpr std::uint16_t pcmFormat = 1;
constexpr std::uint16_t floatFormat = 3;
constexpr std::uint16_t extensibleFormat = 0xFFFE;

bool hasAscii(const std::vector<unsigned char>& bytes, std::size_t offset, std::string_view expected)
{
    if (offset + expected.size() > bytes.size())
        return false;
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (bytes[offset + index] != static_cast<unsigned char>(expected[index]))
            return false;
    }
    return true;
}

std::uint16_t readU16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t readBigU16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readBigU32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

// The 80-bit IEEE extended sample rate of an AIFF COMM chunk.
std::optional<double> readExtended(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    const auto signAndExponent = readBigU16(bytes, offset);
    std::uint64_t mantissa = 0;
    for (std::size_t index = 0; index < 8; ++index)
        mantissa = (mantissa << 8) | bytes[offset + 2 + index];
    if ((signAndExponent & 0x8000U) != 0U)
        return std::nullopt;
    const auto exponent = static_cast<int>(signAndExponent & 0x7FFFU);
    if (exponent == 0x7FFF)
        return std::nullopt;
    const auto value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    if (!std::isfinite(value))
        return std::nullopt;
    return value;
}

// How a file stores one sample.
struct SampleEncoding
{
    enum class Kind
    {
        UnsignedInteger,
        SignedInteger,
        Float,
    };

    Kind kind = Kind::SignedInteger;
    std::uint16_t bytesPerSample = 2;
    bool bigEndian = false;
};

// One sample, full scale as the WAV writer's: the largest positive code is
// 1 and the extra negative code exactly -1.
double decodeSample(const std::vector<unsigned char>& bytes, std::size_t offset, SampleEncoding encoding)
{
    std::uint64_t code = 0;
    for (std::size_t index = 0; index < encoding.bytesPerSample; ++index)
    {
        const auto byte = encoding.bigEndian
            ? bytes[offset + index]
            : bytes[offset + encoding.bytesPerSample - 1 - index];
        code = (code << 8) | byte;
    }
    const auto bits = 8U * encoding.bytesPerSample;
    if (encoding.kind == SampleEncoding::Kind::Float)
    {
        if (encoding.bytesPerSample == 4)
            return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(code)));
        return std::bit_cast<double>(code);
    }
    const auto half = std::uint64_t{1} << (bits - 1U);
    const auto signedCode = encoding.kind == SampleEncoding::Kind::UnsignedInteger
        ? static_cast<std::int64_t>(code) - static_cast<std::int64_t>(half)
        : (code >= half ? static_cast<std::int64_t>(code) - static_cast<std::int64_t>(half << 1U)
                        : static_cast<std::int64_t>(code));
    const auto fullScale = static_cast<double>(half - 1U);
    if (signedCode == -static_cast<std::int64_t>(half))
        return -1.0;
    return static_cast<double>(signedCode) / fullScale;
}

bool cancelledNow(const WavReadCancellationCheck& shouldCancel, WavReadResult& result)
{
    if (shouldCancel && shouldCancel())
    {
        result.channels.clear();
        result.cancelled = true;
        result.error = "WAV loading cancelled";
        return true;
    }
    return false;
}

// Interleaved frames into channels.
bool decodeFrames(
    const std::vector<unsigned char>& bytes,
    std::size_t dataOffset,
    std::size_t frameCount,
    std::uint16_t channelCount,
    SampleEncoding encoding,
    const WavReadCancellationCheck& shouldCancel,
    WavReadResult& result)
{
    const auto blockAlign = static_cast<std::size_t>(channelCount) * encoding.bytesPerSample;
    result.channels.assign(channelCount, std::vector<double>(frameCount, 0.0));
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        if (frame % 4096U == 0U && cancelledNow(shouldCancel, result))
            return false;
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto sampleOffset = dataOffset + frame * blockAlign
                + channel * encoding.bytesPerSample;
            const auto value = decodeSample(bytes, sampleOffset, encoding);
            if (!std::isfinite(value))
            {
                result.channels.clear();
                result.error = "sample file holds a sample that is not finite";
                return false;
            }
            result.channels[channel][frame] = value;
        }
    }
    return true;
}

// A base note and a detune in cents, as INST chunks give them, in the
// smpl chunk's terms: a unity note and an unsigned fraction of a semitone
// above it.
std::optional<talas::core::SamplePitch> instrumentPitch(int baseNote, int detune)
{
    if (baseNote < 0 || baseNote > 127 || detune < -50 || detune > 50)
        return std::nullopt;
    auto note = static_cast<std::uint32_t>(baseNote);
    auto cents = detune;
    if (cents < 0 && note > 0)
    {
        --note;
        cents += 100;
    }
    else if (cents < 0)
        cents = 0;
    return talas::core::SamplePitch{
        note,
        static_cast<std::uint32_t>(std::llround(
            static_cast<double>(cents) / 100.0 * 4294967296.0) & 0xFFFFFFFFLL),
    };
}

WavReadResult readWaveBytes(
    const std::vector<unsigned char>& bytes,
    const WavReadCancellationCheck& shouldCancel,
    bool pcm16Only)
{
    WavReadResult result;
    bool hasFmt = false;
    bool hasData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t parsedBitsPerSample = 0;
    std::uint32_t dataOffset = 0;
    std::uint32_t dataSize = 0;
    bool hasSamplerChunk = false;
    std::optional<talas::core::SampleLoop> forwardLoop;
    std::optional<talas::core::SamplePitch> samplePitch;
    std::optional<talas::core::SamplePitch> instrumentNote;
    // A cue chunk's points (id and sample offset) and the labels a LIST
    // adtl chunk gives them; a malformed one is passed over.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cuePoints;
    std::vector<std::pair<std::uint32_t, std::string>> cueLabels;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size())
    {
        if (cancelledNow(shouldCancel, result))
            return result;
        const auto chunkSize = readU32(bytes, offset + 4);
        const auto chunkDataOffset = offset + 8;
        const auto chunkEnd = chunkDataOffset + static_cast<std::size_t>(chunkSize);
        if (chunkEnd > bytes.size())
        {
            result.error = "WAV chunk extends past end of file";
            return result;
        }

        if (hasAscii(bytes, offset, "fmt "))
        {
            if (chunkSize < 16)
            {
                result.error = "WAV fmt chunk is too short";
                return result;
            }
            audioFormat = readU16(bytes, chunkDataOffset);
            channelCount = readU16(bytes, chunkDataOffset + 2);
            result.sampleRate = readU32(bytes, chunkDataOffset + 4);
            blockAlign = readU16(bytes, chunkDataOffset + 12);
            parsedBitsPerSample = readU16(bytes, chunkDataOffset + 14);
            // WAVE_FORMAT_EXTENSIBLE names its PCM or float format in the
            // first two bytes of its SubFormat GUID.
            if (audioFormat == extensibleFormat && !pcm16Only)
            {
                if (chunkSize < 40)
                {
                    result.error = "WAV extensible fmt chunk is too short";
                    return result;
                }
                audioFormat = readU16(bytes, chunkDataOffset + 24);
            }
            hasFmt = true;
        }
        else if (hasAscii(bytes, offset, "data"))
        {
            dataOffset = static_cast<std::uint32_t>(chunkDataOffset);
            dataSize = chunkSize;
            hasData = true;
        }
        else if (hasAscii(bytes, offset, "inst") && !pcm16Only)
        {
            // RIFF inst: unshifted note, fine tune in cents, gain, and the
            // low and high notes and velocities it plays.
            if (chunkSize < 7U)
            {
                result.error = "WAV inst chunk is too short";
                return result;
            }
            instrumentNote = instrumentPitch(
                bytes[chunkDataOffset],
                static_cast<std::int8_t>(bytes[chunkDataOffset + 1]));
            result.keyRange = talas::core::SampleKeyRange{
                bytes[chunkDataOffset + 3],
                bytes[chunkDataOffset + 4],
                bytes[chunkDataOffset + 5],
                bytes[chunkDataOffset + 6],
            };
        }
        else if (hasAscii(bytes, offset, "cue ") && !pcm16Only && chunkSize >= 4U)
        {
            const auto count = readU32(bytes, chunkDataOffset);
            if (count <= (chunkSize - 4U) / 24U)
            {
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const auto point = chunkDataOffset + 4U + static_cast<std::size_t>(index) * 24U;
                    cuePoints.emplace_back(readU32(bytes, point), readU32(bytes, point + 20U));
                }
            }
        }
        else if (hasAscii(bytes, offset, "LIST") && !pcm16Only && chunkSize >= 4U
            && hasAscii(bytes, chunkDataOffset, "adtl"))
        {
            auto sub = chunkDataOffset + 4U;
            while (sub + 8U <= chunkEnd)
            {
                const auto subSize = static_cast<std::size_t>(readU32(bytes, sub + 4U));
                if (subSize > chunkEnd - sub - 8U)
                    break;
                if (hasAscii(bytes, sub, "labl") && subSize >= 4U)
                {
                    const auto text = sub + 12U;
                    auto end = text;
                    while (end < sub + 8U + subSize && bytes[end] != 0)
                        ++end;
                    cueLabels.emplace_back(readU32(bytes, sub + 8U),
                        std::string(bytes.begin() + static_cast<std::ptrdiff_t>(text),
                            bytes.begin() + static_cast<std::ptrdiff_t>(end)));
                }
                sub += 8U + subSize + (subSize % 2U);
            }
        }
        else if (hasAscii(bytes, offset, "smpl"))
        {
            if (hasSamplerChunk || chunkSize < 36U)
            {
                result.error = "WAV smpl chunk is duplicate or too short";
                return result;
            }
            hasSamplerChunk = true;
            samplePitch = talas::core::SamplePitch{
                readU32(bytes, chunkDataOffset + 12U),
                readU32(bytes, chunkDataOffset + 16U),
            };
            const auto loopCount = readU32(bytes, chunkDataOffset + 28U);
            if (loopCount > (chunkSize - 36U) / 24U)
            {
                result.error = "WAV smpl loop records extend past the chunk";
                return result;
            }
            for (std::uint32_t index = 0; index < loopCount; ++index)
            {
                if (index % 4096U == 0U && cancelledNow(shouldCancel, result))
                    return result;
                const auto loopOffset = chunkDataOffset + 36U
                    + static_cast<std::size_t>(index) * 24U;
                if (readU32(bytes, loopOffset + 4U) != 0U)
                    continue;
                const auto start = readU32(bytes, loopOffset + 8U);
                const auto inclusiveEnd = readU32(bytes, loopOffset + 12U);
                forwardLoop = talas::core::SampleLoop{
                    start,
                    static_cast<std::uint64_t>(inclusiveEnd) + 1U,
                };
                break;
            }
        }

        offset = chunkEnd + (chunkSize % 2u);
    }

    if (!hasFmt)
    {
        result.error = "WAV file is missing fmt chunk";
        return result;
    }
    if (!hasData)
    {
        result.error = "WAV file is missing data chunk";
        return result;
    }
    if (pcm16Only && (audioFormat != pcmFormat || parsedBitsPerSample != 16))
    {
        result.error = "WAV reader supports PCM16 files only";
        return result;
    }
    // 8-bit WAV samples are unsigned, wider ones signed; float ones are 32-
    // or 64-bit IEEE.
    SampleEncoding encoding;
    if (audioFormat == pcmFormat
        && (parsedBitsPerSample == 8 || parsedBitsPerSample == 16
            || parsedBitsPerSample == 24 || parsedBitsPerSample == 32))
    {
        encoding.kind = parsedBitsPerSample == 8
            ? SampleEncoding::Kind::UnsignedInteger
            : SampleEncoding::Kind::SignedInteger;
    }
    else if (audioFormat == floatFormat
        && (parsedBitsPerSample == 32 || parsedBitsPerSample == 64))
    {
        encoding.kind = SampleEncoding::Kind::Float;
    }
    else
    {
        result.error = "WAV reader supports 8-, 16-, 24- and 32-bit PCM and 32- and 64-bit float files only";
        return result;
    }
    encoding.bytesPerSample = static_cast<std::uint16_t>(parsedBitsPerSample / 8);
    if (channelCount == 0)
    {
        result.error = "WAV file has zero channels";
        return result;
    }
    if (channelCount > std::numeric_limits<std::uint16_t>::max() / encoding.bytesPerSample)
    {
        result.error = "WAV channel count is too large for its block alignment";
        return result;
    }

    const auto expectedBlockAlign = static_cast<std::uint16_t>(channelCount * encoding.bytesPerSample);
    if (blockAlign != expectedBlockAlign)
    {
        result.error = pcm16Only
            ? "WAV block alignment is inconsistent with PCM16 channel count"
            : "WAV block alignment is inconsistent with its channel count";
        return result;
    }
    if (dataSize % blockAlign != 0)
    {
        result.error = "WAV data chunk is not frame-aligned";
        return result;
    }

    const auto frameCount = static_cast<std::size_t>(dataSize / blockAlign);
    if (forwardLoop.has_value()
        && (forwardLoop->startFrame >= forwardLoop->endFrameExclusive
            || forwardLoop->endFrameExclusive > frameCount))
    {
        result.error = "WAV smpl forward loop lies outside the sample frames";
        return result;
    }
    result.forwardLoop = forwardLoop;
    result.samplePitch = samplePitch.has_value() ? samplePitch : instrumentNote;
    // Each cue point by its label, an unlabelled
    // one by its id, those past the last frame left out.
    for (const auto& [id, frame] : cuePoints)
    {
        if (frame > frameCount)
            continue;
        const auto label = std::ranges::find(cueLabels, id, &std::pair<std::uint32_t, std::string>::first);
        result.markers.push_back(
            {label != cueLabels.end() && !label->second.empty() ? label->second : "marker" + std::to_string(id), frame});
    }
    decodeFrames(bytes, dataOffset, frameCount, channelCount, encoding, shouldCancel, result);
    return result;
}

// An AIFF or AIFF-C file (FORM AIFF or AIFC), big-endian, with the base
// pitch, the key and velocity ranges and the sustain loop of its INST chunk
// (the base pitch in the header of an AIFF sample).
WavReadResult readAiffBytes(
    const std::vector<unsigned char>& bytes,
    const WavReadCancellationCheck& shouldCancel)
{
    WavReadResult result;
    const auto compressed = hasAscii(bytes, 8, "AIFC");
    bool hasComm = false;
    bool hasSound = false;
    std::uint16_t channelCount = 0;
    std::uint32_t frameCount = 0;
    std::uint16_t sampleSize = 0;
    double sampleRate = 0.0;
    SampleEncoding encoding;
    encoding.bigEndian = true;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    std::optional<std::array<std::uint8_t, 6>> instrument;
    std::array<std::int16_t, 3> sustainLoop{};
    std::vector<std::pair<std::int16_t, std::uint32_t>> markers;
    std::vector<std::string> markerNames;

    std::size_t offset = 12;
    while (offset + 8 <= bytes.size())
    {
        if (cancelledNow(shouldCancel, result))
            return result;
        const auto chunkSize = readBigU32(bytes, offset + 4);
        const auto chunkDataOffset = offset + 8;
        const auto chunkEnd = chunkDataOffset + static_cast<std::size_t>(chunkSize);
        if (chunkEnd > bytes.size())
        {
            result.error = "AIFF chunk extends past end of file";
            return result;
        }
        if (hasAscii(bytes, offset, "COMM"))
        {
            if (chunkSize < (compressed ? 22U : 18U))
            {
                result.error = "AIFF COMM chunk is too short";
                return result;
            }
            channelCount = readBigU16(bytes, chunkDataOffset);
            frameCount = readBigU32(bytes, chunkDataOffset + 2);
            sampleSize = readBigU16(bytes, chunkDataOffset + 6);
            const auto rate = readExtended(bytes, chunkDataOffset + 8);
            if (!rate.has_value())
            {
                result.error = "AIFF sample rate is invalid";
                return result;
            }
            sampleRate = *rate;
            encoding.kind = SampleEncoding::Kind::SignedInteger;
            if (compressed)
            {
                if (hasAscii(bytes, chunkDataOffset + 18, "sowt"))
                    encoding.bigEndian = false;
                else if (hasAscii(bytes, chunkDataOffset + 18, "fl32")
                    || hasAscii(bytes, chunkDataOffset + 18, "FL32"))
                {
                    encoding.kind = SampleEncoding::Kind::Float;
                    sampleSize = 32;
                }
                else if (hasAscii(bytes, chunkDataOffset + 18, "fl64")
                    || hasAscii(bytes, chunkDataOffset + 18, "FL64"))
                {
                    encoding.kind = SampleEncoding::Kind::Float;
                    sampleSize = 64;
                }
                else if (!hasAscii(bytes, chunkDataOffset + 18, "NONE"))
                {
                    result.error = "AIFF-C compression is not supported";
                    return result;
                }
            }
            hasComm = true;
        }
        else if (hasAscii(bytes, offset, "SSND"))
        {
            if (chunkSize < 8U)
            {
                result.error = "AIFF SSND chunk is too short";
                return result;
            }
            const auto soundOffset = readBigU32(bytes, chunkDataOffset);
            if (soundOffset > chunkSize - 8U)
            {
                result.error = "AIFF SSND offset lies past its chunk";
                return result;
            }
            dataOffset = chunkDataOffset + 8U + soundOffset;
            dataSize = chunkSize - 8U - soundOffset;
            hasSound = true;
        }
        else if (hasAscii(bytes, offset, "INST"))
        {
            if (chunkSize < 20U)
            {
                result.error = "AIFF INST chunk is too short";
                return result;
            }
            std::array<std::uint8_t, 6> fields{};
            for (std::size_t index = 0; index < fields.size(); ++index)
                fields[index] = bytes[chunkDataOffset + index];
            instrument = fields;
            for (std::size_t index = 0; index < sustainLoop.size(); ++index)
            {
                sustainLoop[index] = static_cast<std::int16_t>(
                    readBigU16(bytes, chunkDataOffset + 8 + 2 * index));
            }
        }
        else if (hasAscii(bytes, offset, "MARK"))
        {
            if (chunkSize < 2U)
            {
                result.error = "AIFF MARK chunk is too short";
                return result;
            }
            const auto count = readBigU16(bytes, chunkDataOffset);
            auto position = chunkDataOffset + 2;
            for (std::uint16_t index = 0; index < count; ++index)
            {
                if (position + 7 > chunkEnd)
                {
                    result.error = "AIFF markers extend past their chunk";
                    return result;
                }
                const auto id = static_cast<std::int16_t>(readBigU16(bytes, position));
                const auto frame = readBigU32(bytes, position + 2);
                const auto nameLength = static_cast<std::size_t>(bytes[position + 6]);
                const auto nameStart = position + 7;
                // A Pascal string, padded to an even length.
                position += 7 + nameLength + ((nameLength + 1) % 2);
                if (position > chunkEnd)
                {
                    result.error = "AIFF markers extend past their chunk";
                    return result;
                }
                markers.emplace_back(id, frame);
                markerNames.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(nameStart),
                    bytes.begin() + static_cast<std::ptrdiff_t>(nameStart + nameLength));
            }
        }
        offset = chunkEnd + (chunkSize % 2u);
    }

    if (!hasComm)
    {
        result.error = "AIFF file is missing its COMM chunk";
        return result;
    }
    if (!hasSound && frameCount != 0)
    {
        result.error = "AIFF file is missing its SSND chunk";
        return result;
    }
    if (channelCount == 0)
    {
        result.error = "AIFF file has zero channels";
        return result;
    }
    if (encoding.kind != SampleEncoding::Kind::Float
        && (sampleSize == 0 || sampleSize > 32))
    {
        result.error = "AIFF reader supports samples of 1 to 32 bits only";
        return result;
    }
    // Narrower samples are left-justified in whole bytes.
    encoding.bytesPerSample = static_cast<std::uint16_t>((sampleSize + 7) / 8);
    if (!encoding.bigEndian && encoding.bytesPerSample == 1)
        encoding.bigEndian = true;
    if (!(sampleRate >= 1.0) || sampleRate > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        result.error = "AIFF sample rate is out of range";
        return result;
    }
    result.sampleRate = static_cast<std::uint32_t>(std::lround(sampleRate));
    const auto blockAlign = static_cast<std::size_t>(channelCount) * encoding.bytesPerSample;
    if (static_cast<std::size_t>(frameCount) > dataSize / blockAlign)
    {
        result.error = "AIFF SSND chunk holds fewer frames than COMM says";
        return result;
    }

    // The markers by name, an unnamed one by its
    // id, those past the last frame left out.
    for (std::size_t index = 0; index < markers.size(); ++index)
    {
        if (markers[index].second > frameCount)
            continue;
        result.markers.push_back({markerNames[index].empty() ? "marker" + std::to_string(markers[index].first)
                                                             : markerNames[index],
            markers[index].second});
    }

    if (instrument.has_value())
    {
        // INST: base note, detune in cents, low and high notes and
        // velocities.
        result.samplePitch = instrumentPitch(
            (*instrument)[0], static_cast<std::int8_t>((*instrument)[1]));
        result.keyRange = talas::core::SampleKeyRange{
            (*instrument)[2],
            (*instrument)[3],
            (*instrument)[4],
            (*instrument)[5],
        };
        // A forward sustain loop between two markers.
        if (sustainLoop[0] == 1)
        {
            const auto find = [&](std::int16_t id) -> std::optional<std::uint32_t> {
                for (const auto& [markerId, frame] : markers)
                    if (markerId == id)
                        return frame;
                return std::nullopt;
            };
            const auto begin = find(sustainLoop[1]);
            const auto end = find(sustainLoop[2]);
            if (begin.has_value() && end.has_value())
            {
                if (*begin >= *end || *end > frameCount)
                {
                    result.error = "AIFF sustain loop lies outside the sample frames";
                    return result;
                }
                result.forwardLoop = talas::core::SampleLoop{*begin, *end};
            }
        }
    }
    decodeFrames(bytes, dataOffset, frameCount, channelCount, encoding, shouldCancel, result);
    return result;
}

// An IRCAM (BICSF) or MTU sound file, .sf:
// a 1024-byte header and its samples after it. The header's identifier
// names the machine that wrote it in its third byte, as a machine writes
// it (64 A3 m 00): 1 (VAX) or 3 (MIPS) little-endian, 2 (Sun) or 4 (NeXT)
// big-endian; the same bytes reversed (00 m A3 64) mean the other order
// (the McGill AFsp description). Then the sample rate as a float, the
// channels and the packing: 1 for 8-bit, 2 for 16-bit, 3 for 24-bit and
// 0x40004 for 32-bit integers, 4 for 32-bit floats. A header that makes no
// sense in the order its identifier gives is read in the other, as some
// programs wrote the reversed identifier in their own order.
constexpr std::size_t ircamHeaderSize = 1024;

struct IrcamIdentifier
{
    std::uint8_t machine = 0;
    bool reversed = false;
};

[[nodiscard]] std::optional<IrcamIdentifier> ircamIdentifier(const std::vector<unsigned char>& bytes)
{
    if (bytes.size() < 16)
        return std::nullopt;
    if (bytes[0] == 0x64 && bytes[1] == 0xA3 && bytes[3] == 0 && bytes[2] >= 1 && bytes[2] <= 4)
        return IrcamIdentifier{bytes[2], false};
    if (bytes[0] == 0 && bytes[2] == 0xA3 && bytes[3] == 0x64 && bytes[1] >= 1 && bytes[1] <= 4)
        return IrcamIdentifier{bytes[1], true};
    return std::nullopt;
}

struct IrcamHeader
{
    double sampleRate = 0.0;
    std::uint32_t channelCount = 0;
    SampleEncoding encoding;
};

[[nodiscard]] std::optional<IrcamHeader> ircamHeader(
    const std::vector<unsigned char>& bytes, bool bigEndian, std::string& error)
{
    const auto word = [&](std::size_t offset) { return bigEndian ? readBigU32(bytes, offset) : readU32(bytes, offset); };
    IrcamHeader header;
    header.sampleRate = static_cast<double>(std::bit_cast<float>(word(4)));
    header.channelCount = word(8);
    header.encoding.bigEndian = bigEndian;
    switch (word(12))
    {
    case 0x00001: header.encoding.bytesPerSample = 1; break;
    case 0x00002: header.encoding.bytesPerSample = 2; break;
    case 0x00003: header.encoding.bytesPerSample = 3; break;
    case 0x40004: header.encoding.bytesPerSample = 4; break;
    case 0x00004:
        header.encoding.kind = SampleEncoding::Kind::Float;
        header.encoding.bytesPerSample = 4;
        break;
    default:
        error = "IRCAM file packs its samples in a way Talas does not read";
        return std::nullopt;
    }
    if (!std::isfinite(header.sampleRate) || header.sampleRate < 1.0 || header.sampleRate > 4294967295.0)
    {
        error = "IRCAM file names no sample rate";
        return std::nullopt;
    }
    if (header.channelCount == 0 || header.channelCount > 64)
    {
        error = "IRCAM file names no channels it could hold";
        return std::nullopt;
    }
    return header;
}

WavReadResult readIrcamBytes(
    const std::vector<unsigned char>& bytes,
    IrcamIdentifier identifier,
    const WavReadCancellationCheck& shouldCancel)
{
    WavReadResult result;
    const auto machineBigEndian = identifier.machine == 2 || identifier.machine == 4;
    const auto bigEndian = identifier.reversed ? !machineBigEndian : machineBigEndian;
    std::string ignored;
    auto header = ircamHeader(bytes, bigEndian, result.error);
    if (!header.has_value())
        header = ircamHeader(bytes, !bigEndian, ignored);
    if (!header.has_value())
        return result;
    result.error.clear();
    if (bytes.size() < ircamHeaderSize)
    {
        result.error = "IRCAM file ends inside its header";
        return result;
    }
    result.sampleRate = static_cast<std::uint32_t>(std::lround(header->sampleRate));
    const auto blockAlign = static_cast<std::size_t>(header->channelCount) * header->encoding.bytesPerSample;
    const auto frameCount = (bytes.size() - ircamHeaderSize) / blockAlign;
    decodeFrames(bytes, ircamHeaderSize, frameCount, static_cast<std::uint16_t>(header->channelCount),
        header->encoding, shouldCancel, result);
    return result;
}

// A Sound Designer I file: a 1336-byte header,
// big-endian, whose first word is its length, then one channel of
// big-endian samples. Its HeaderType record gives the bytes of sound at
// offset 184, the sample rate at 1020 and the bits in a sample at 1028.
constexpr std::size_t soundDesignerIHeaderSize = 1336;

[[nodiscard]] bool isSoundDesignerI(const std::vector<unsigned char>& bytes)
{
    return bytes.size() >= soundDesignerIHeaderSize && readBigU16(bytes, 0) == soundDesignerIHeaderSize;
}

WavReadResult readSoundDesignerIBytes(
    const std::vector<unsigned char>& bytes,
    const WavReadCancellationCheck& shouldCancel)
{
    WavReadResult result;
    const auto sampleRate = static_cast<std::int32_t>(readBigU32(bytes, 1020));
    const auto bits = readBigU16(bytes, 1028);
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
    {
        result.error = "Sound Designer I file holds samples of a size Talas does not read";
        return result;
    }
    if (sampleRate < 1)
    {
        result.error = "Sound Designer I file names no sample rate";
        return result;
    }
    SampleEncoding encoding;
    encoding.bigEndian = true;
    encoding.bytesPerSample = static_cast<std::uint16_t>(bits / 8U);
    const auto available = bytes.size() - soundDesignerIHeaderSize;
    const auto fileSize = static_cast<std::size_t>(readBigU32(bytes, 184));
    const auto dataBytes = fileSize > 0 && fileSize <= available ? fileSize : available;
    result.sampleRate = static_cast<std::uint32_t>(sampleRate);
    decodeFrames(bytes, soundDesignerIHeaderSize, dataBytes / encoding.bytesPerSample, 1, encoding, shouldCancel,
        result);
    return result;
}

// A Sound Designer II file (on a Macintosh; Digidesign Developer
// Documentation p. 33): the samples, interleaved and big-endian,
// are all of the data fork; its resource fork gives their size, rate and
// channels.
WavReadResult readSoundDesignerIIBytes(
    const std::vector<unsigned char>& bytes,
    const SoundDesignerIIParameters& parameters,
    const WavReadCancellationCheck& shouldCancel)
{
    WavReadResult result;
    SampleEncoding encoding;
    encoding.bigEndian = true;
    encoding.bytesPerSample = parameters.bytesPerSample;
    result.sampleRate = static_cast<std::uint32_t>(std::lround(parameters.sampleRate));
    const auto blockAlign = static_cast<std::size_t>(parameters.channels) * parameters.bytesPerSample;
    decodeFrames(bytes, 0, bytes.size() / blockAlign, parameters.channels, encoding, shouldCancel, result);
    return result;
}

[[nodiscard]] bool hasSampleFileMagic(const std::vector<unsigned char>& bytes)
{
    return (bytes.size() >= 12 && hasAscii(bytes, 0, "RIFF") && hasAscii(bytes, 8, "WAVE"))
        || (bytes.size() >= 12 && hasAscii(bytes, 0, "FORM")
            && (hasAscii(bytes, 8, "AIFF") || hasAscii(bytes, 8, "AIFC")))
        || ircamIdentifier(bytes).has_value();
}

WavReadResult readSampleBytes(
    const std::vector<unsigned char>& bytes,
    const WavReadCancellationCheck& shouldCancel,
    bool pcm16Only)
{
    WavReadResult result;
    if (cancelledNow(shouldCancel, result))
        return result;
    if (bytes.size() >= 12 && hasAscii(bytes, 0, "RIFF") && hasAscii(bytes, 8, "WAVE"))
        return readWaveBytes(bytes, shouldCancel, pcm16Only);
    if (!pcm16Only && bytes.size() >= 12 && hasAscii(bytes, 0, "FORM")
        && (hasAscii(bytes, 8, "AIFF") || hasAscii(bytes, 8, "AIFC")))
    {
        return readAiffBytes(bytes, shouldCancel);
    }
    if (const auto identifier = pcm16Only ? std::nullopt : ircamIdentifier(bytes))
        return readIrcamBytes(bytes, *identifier, shouldCancel);
    if (!pcm16Only && isSoundDesignerI(bytes))
        return readSoundDesignerIBytes(bytes, shouldCancel);
    result.error = pcm16Only
        ? "WAV file is not a RIFF/WAVE container"
        : "sample file is not a RIFF/WAVE, AIFF, IRCAM or Sound Designer file";
    return result;
}

WavReadResult readSampleFileAt(
    const std::filesystem::path& path,
    const WavReadCancellationCheck& shouldCancel,
    bool pcm16Only)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        WavReadResult result;
        result.error = (pcm16Only ? "failed to read WAV file: " : "failed to read sample file: ")
            + talas::project::pathToUtf8(path);
        return result;
    }

    std::vector<unsigned char> bytes;
    std::array<char, 64U * 1024U> buffer{};
    while (input)
    {
        if (shouldCancel && shouldCancel())
        {
            WavReadResult result;
            result.cancelled = true;
            result.error = "WAV loading cancelled";
            return result;
        }
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
        {
            const auto begin = reinterpret_cast<
                const unsigned char*>(buffer.data());
            bytes.insert(
                bytes.end(),
                begin,
                begin + static_cast<std::size_t>(count));
        }
    }
    if (input.bad())
    {
        WavReadResult result;
        result.error = (pcm16Only ? "failed while reading WAV file: " : "failed while reading sample file: ")
            + talas::project::pathToUtf8(path);
        return result;
    }

    // A Sound Designer II file is known by its resource fork, its data fork
    // holding nothing but samples.
    std::optional<SoundDesignerIIParameters> soundDesignerII;
    if (!pcm16Only && !hasSampleFileMagic(bytes))
        soundDesignerII = soundDesignerIIParametersFromResourceFork(resourceForkOf(path));
    auto result = soundDesignerII.has_value() ? readSoundDesignerIIBytes(bytes, *soundDesignerII, shouldCancel)
                                              : readSampleBytes(bytes, shouldCancel, pcm16Only);
    if (!result.ok())
        return result;
    auto contentHash =
        talas::project::calculateProjectAssetSha256(
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(
                    bytes.data()),
                bytes.size(),
            },
            shouldCancel);
    if (!contentHash.ok())
    {
        result.channels.clear();
        result.error = contentHash.error->message;
        result.cancelled =
            contentHash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    result.contentHash =
        std::move(contentHash.contentHash);
    return result;
}
}

WavReadResult readPcm16Wav(
    const std::filesystem::path& path,
    WavReadCancellationCheck shouldCancel)
{
    return readSampleFileAt(path, shouldCancel, true);
}

WavReadResult readSampleFile(
    const std::filesystem::path& path,
    WavReadCancellationCheck shouldCancel)
{
    return readSampleFileAt(path, shouldCancel, false);
}
}
