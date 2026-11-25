#include "talas/audio/SampleFileFormats.h"

#include "talas/audio/AudioFilePublication.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace talas::audio
{
namespace
{
[[nodiscard]] std::string lowered(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

void putBigEndian(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * (count - 1U - index))) & 0xFFU);
}

void appendBigEndian(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::size_t count)
{
    bytes.resize(bytes.size() + count);
    putBigEndian(bytes, bytes.size() - count, value, count);
}

// A Pascal STRING[capacity] at `offset`: its length, then its characters,
// cut to the capacity.
void putPascalString(std::vector<std::uint8_t>& bytes, std::size_t offset, std::string_view text, std::size_t capacity)
{
    const auto length = std::min(text.size(), capacity);
    bytes[offset] = static_cast<std::uint8_t>(length);
    std::copy_n(text.begin(), length, bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1));
}

// One sample as a two's complement word of `bits`, full scale as the WAV
// and AIFF writers code it: the largest positive code is 1, the extra
// negative code -1.
[[nodiscard]] std::uint64_t sampleCode(double sample, std::uint16_t bits)
{
    const auto clipped = std::isfinite(sample) ? std::clamp(sample, -1.0, 1.0) : 0.0;
    const auto half = std::int64_t{1} << (bits - 1U);
    const auto code = clipped <= -1.0 ? -half : static_cast<std::int64_t>(std::llround(clipped * static_cast<double>(half - 1)));
    return static_cast<std::uint64_t>(code);
}

[[nodiscard]] std::string channelsProblem(const std::vector<std::vector<double>>& channels, double sampleRate)
{
    if (channels.empty() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return "A sample file needs channels and a sample rate";
    const auto frames = channels.front().size();
    if (std::ranges::any_of(channels, [frames](const auto& channel) { return channel.size() != frames; }))
        return "The channels differ in length";
    return {};
}

// A header, then the channels' samples interleaved and big-endian, and on
// a Macintosh a resource fork, published at `destination` in place of
// what is there.
[[nodiscard]] std::string publishSampleFile(const std::filesystem::path& destination,
    const std::vector<std::uint8_t>& header, const std::vector<std::vector<double>>& channels, std::uint16_t bits,
    const std::vector<std::uint8_t>* resourceFork)
{
    std::string error;
    auto publication = AudioFilePublication::prepare(destination, ExistingAudioFilePolicy::Replace, error);
    if (!publication)
        return error;
    auto* output = publication->openTemporaryFile(error);
    if (output == nullptr)
        return error;
    const auto put = [output](const std::vector<std::uint8_t>& bytes) {
        return bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size();
    };
    auto written = put(header);
    const auto bytesPerSample = static_cast<std::size_t>(bits / 8U);
    const auto frames = channels.front().size();
    std::vector<std::uint8_t> buffer;
    buffer.reserve(64U * 1024U + 16U);
    for (std::size_t frame = 0; written && frame < frames; ++frame)
    {
        for (const auto& channel : channels)
            appendBigEndian(buffer, sampleCode(channel[frame], bits), bytesPerSample);
        if (buffer.size() >= 64U * 1024U)
        {
            written = put(buffer);
            buffer.clear();
        }
    }
    written = written && put(buffer);
    const auto closed = std::fclose(output) == 0;
    std::string ignored;
    if (!written || !closed)
    {
        (void) publication->discard(ignored);
        return "Could not write the sample file";
    }
    if (resourceFork != nullptr)
    {
        std::ofstream fork(publication->temporaryPath() / "..namedfork" / "rsrc", std::ios::binary | std::ios::trunc);
        fork.write(reinterpret_cast<const char*>(resourceFork->data()), static_cast<std::streamsize>(resourceFork->size()));
        fork.close();
        if (!fork)
        {
            (void) publication->discard(ignored);
            return "Could not write the Sound Designer II file's resource fork";
        }
    }
    if (!publication->publish(error))
        return error;
    return {};
}

[[nodiscard]] std::optional<std::uint32_t> wholeNumber(std::string_view text)
{
    if (text.empty() || text.size() > 9)
        return std::nullopt;
    std::uint32_t value = 0;
    for (const auto character : text)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    return value;
}

// "44100.0000", or "44100": digits, then a point and more digits.
[[nodiscard]] std::optional<double> decimalNumber(std::string_view text)
{
    const auto point = text.find('.');
    const auto whole = wholeNumber(text.substr(0, point));
    if (!whole.has_value())
        return std::nullopt;
    auto value = static_cast<double>(*whole);
    if (point == std::string_view::npos)
        return value;
    auto scale = 0.1;
    for (const auto character : text.substr(point + 1))
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        value += scale * static_cast<double>(character - '0');
        scale /= 10.0;
    }
    return value;
}
}

std::vector<SampleFileFormat> sampleFileFormats()
{
    return {SampleFileFormat::SoundDesignerI, SampleFileFormat::SoundDesignerII, SampleFileFormat::Aiff,
        SampleFileFormat::Ircam, SampleFileFormat::Wave};
}

std::optional<SampleFileFormat> sampleFileFormatNamed(std::string_view name) noexcept
{
    if (name.empty() || name == "WAVE" || name == "WAV")
        return SampleFileFormat::Wave;
    if (name == "AIFF")
        return SampleFileFormat::Aiff;
    if (name == "IRCAM")
        return SampleFileFormat::Ircam;
    if (name == "SDI")
        return SampleFileFormat::SoundDesignerI;
    if (name == "SDII")
        return SampleFileFormat::SoundDesignerII;
    return std::nullopt;
}

std::string_view sampleFileFormatName(SampleFileFormat format) noexcept
{
    switch (format)
    {
    case SampleFileFormat::Wave: return "WAVE";
    case SampleFileFormat::Aiff: return "AIFF";
    case SampleFileFormat::Ircam: return "IRCAM";
    case SampleFileFormat::SoundDesignerI: return "SDI";
    case SampleFileFormat::SoundDesignerII: return "SDII";
    }
    return "WAVE";
}

std::string_view sampleFileFormatExtension(SampleFileFormat format) noexcept
{
    switch (format)
    {
    case SampleFileFormat::Wave: return ".wav";
    case SampleFileFormat::Aiff: return ".aif";
    case SampleFileFormat::Ircam: return ".sf";
    case SampleFileFormat::SoundDesignerI: return ".sd1";
    case SampleFileFormat::SoundDesignerII: return ".sd2";
    }
    return ".wav";
}

bool sampleFileHasFormatExtension(const std::filesystem::path& path, SampleFileFormat format)
{
    const auto extension = lowered(talas::project::pathToUtf8(path.extension()));
    switch (format)
    {
    case SampleFileFormat::Wave: return extension == ".wav" || extension == ".wave";
    case SampleFileFormat::Aiff: return extension == ".aif" || extension == ".aiff" || extension == ".aifc";
    case SampleFileFormat::Ircam: return extension == ".sf";
    case SampleFileFormat::SoundDesignerI: return extension == ".sd1";
    case SampleFileFormat::SoundDesignerII: return extension == ".sd2";
    }
    return false;
}

bool soundDesignerIIAvailable() noexcept
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::string sampleFileFormatRefusal(SampleFileFormat format, std::size_t channels, std::uint16_t bits)
{
    if (bits != 8 && bits != 16 && bits != 24)
        return "A sample file holds 8-, 16- or 24-bit samples";
    if (channels == 0 || channels > 64)
        return "A sample file holds 1 to 64 channels";
    if (format == SampleFileFormat::SoundDesignerI && channels != 1)
        return "A Sound Designer I file holds one channel";
    if (format == SampleFileFormat::SoundDesignerII && !soundDesignerIIAvailable())
        return "Sound Designer II files are written on a Macintosh only";
    return {};
}

std::string writeIrcamFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits)
{
    if (auto problem = channelsProblem(channels, sampleRate); !problem.empty())
        return problem;
    if (auto refusal = sampleFileFormatRefusal(SampleFileFormat::Ircam, channels.size(), bits); !refusal.empty())
        return refusal;
    std::vector<std::uint8_t> header(1024, 0);
    // The Sun's identifier, big-endian; the codes that follow end at once
    // with SF_END (a zero code of zero bytes).
    header[0] = 0x64;
    header[1] = 0xA3;
    header[2] = 0x02;
    header[3] = 0x00;
    putBigEndian(header, 4, std::bit_cast<std::uint32_t>(static_cast<float>(sampleRate)), 4);
    putBigEndian(header, 8, channels.size(), 4);
    // SF_CHAR, SF_SHORT or SF_24INT: the bytes in a sample.
    putBigEndian(header, 12, bits / 8U, 4);
    return publishSampleFile(destination, header, channels, bits, nullptr);
}

std::string writeSoundDesignerIFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits)
{
    if (auto problem = channelsProblem(channels, sampleRate); !problem.empty())
        return problem;
    if (auto refusal = sampleFileFormatRefusal(SampleFileFormat::SoundDesignerI, channels.size(), bits); !refusal.empty())
        return refusal;
    const auto dataBytes = static_cast<std::uint64_t>(channels.front().size()) * (bits / 8U);
    if (dataBytes > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
        || sampleRate > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
    {
        return "The sound is too long for a Sound Designer I file";
    }
    // The HeaderType record (Digidesign Developer Documentation, April 1990,
    // pp. 29-32) as MPW Pascal lays it out: Booleans, signed bytes and
    // enumerations in a byte, every larger field and every string at an
    // even offset, records rounded to an even size. That puts SampRate at
    // 1020 and ends the record at 1336, the header's documented length.
    std::vector<std::uint8_t> header(1336, 0);
    putBigEndian(header, 0, 1336, 2); // HeadrSize
    putBigEndian(header, 2, 32, 2); // Version (32)
    // Zoom: v (-256), h (1).
    putBigEndian(header, 56, static_cast<std::uint16_t>(-256), 2);
    putBigEndian(header, 58, 1, 4);
    // Scale: VFactor (327), VType Percent (4), VString, HFactor (1), HType
    // Time (0), HString ("sec").
    putBigEndian(header, 62, 327, 4);
    header[66] = 4;
    putPascalString(header, 68, "%", 32);
    putBigEndian(header, 102, 1, 4);
    header[106] = 0;
    putPascalString(header, 108, "sec", 32);
    putBigEndian(header, 184, dataBytes, 4); // FileSize: the bytes of sound
    putPascalString(header, 254, talas::project::pathToUtf8(destination.filename()), 64); // FileName
    // MarkerData: ten markers, each free (Free TRUE, Position 0, no name).
    for (std::size_t marker = 0; marker < 10; ++marker)
        header[348 + 40 * marker] = 1;
    putBigEndian(header, 752, 0xFFFFFFFFU, 4); // LoopStart (-1)
    putBigEndian(header, 756, 0xFFFFFFFFU, 4); // LoopEnd (-1)
    putBigEndian(header, 1020, static_cast<std::uint64_t>(std::llround(sampleRate)), 4); // SampRate
    putBigEndian(header, 1024, static_cast<std::uint64_t>(std::llround(1.0e6 / sampleRate)), 4); // SampPeriod
    putBigEndian(header, 1028, bits, 2); // SampSize
    putBigEndian(header, 1320, dataBytes, 4); // BufSize: the sound
    putBigEndian(header, 1324, 0xFFFFFFFFU, 4); // Loop2Start (-1)
    putBigEndian(header, 1328, 0xFFFFFFFFU, 4); // Loop2End (-1)
    header[1332] = 1; // Loop1Type forward
    header[1333] = 1; // Loop2Type forward
    return publishSampleFile(destination, header, channels, bits, nullptr);
}

std::string writeSoundDesignerIIFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits)
{
    if (auto problem = channelsProblem(channels, sampleRate); !problem.empty())
        return problem;
    if (auto refusal = sampleFileFormatRefusal(SampleFileFormat::SoundDesignerII, channels.size(), bits); !refusal.empty())
        return refusal;
    const auto fork = soundDesignerIIResourceFork(sampleRate, channels.size(), static_cast<std::uint16_t>(bits / 8U));
    return publishSampleFile(destination, {}, channels, bits, &fork);
}

std::vector<std::uint8_t> soundDesignerIIResourceFork(double sampleRate, std::size_t channels,
    std::uint16_t bytesPerSample)
{
    struct Resource
    {
        std::uint16_t id;
        std::string_view name;
        std::string value;
    };
    std::array<char, 64> rate{};
    std::snprintf(rate.data(), rate.size(), "%.4f", sampleRate);
    const std::array<Resource, 3> resources{{
        {1000, "sample-size", std::to_string(bytesPerSample)},
        {1001, "sample-rate", rate.data()},
        {1002, "channels", std::to_string(channels)},
    }};
    // The resource data: each a length, then the 'STR ' resource, a Pascal
    // string.
    constexpr std::uint32_t dataOffset = 256;
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> dataOffsets;
    std::vector<std::uint8_t> names;
    std::vector<std::uint16_t> nameOffsets;
    for (const auto& resource : resources)
    {
        dataOffsets.push_back(static_cast<std::uint32_t>(data.size()));
        appendBigEndian(data, resource.value.size() + 1U, 4);
        data.push_back(static_cast<std::uint8_t>(resource.value.size()));
        data.insert(data.end(), resource.value.begin(), resource.value.end());
        nameOffsets.push_back(static_cast<std::uint16_t>(names.size()));
        names.push_back(static_cast<std::uint8_t>(resource.name.size()));
        names.insert(names.end(), resource.name.begin(), resource.name.end());
    }
    // The map: a copy of the header, a handle, a file reference and the
    // attributes; the offsets of the type list and the name list; one type,
    // 'STR ', of three resources, each its ID, name and data.
    constexpr std::uint16_t typeListOffset = 28;
    constexpr std::uint16_t nameListOffset = typeListOffset + 2 + 8 + 3 * 12;
    std::vector<std::uint8_t> map(typeListOffset, 0);
    putBigEndian(map, 24, typeListOffset, 2);
    putBigEndian(map, 26, nameListOffset, 2);
    appendBigEndian(map, 0, 2);
    map.insert(map.end(), {'S', 'T', 'R', ' '});
    appendBigEndian(map, resources.size() - 1U, 2);
    appendBigEndian(map, 2 + 8, 2);
    for (std::size_t index = 0; index < resources.size(); ++index)
    {
        appendBigEndian(map, resources[index].id, 2);
        appendBigEndian(map, nameOffsets[index], 2);
        appendBigEndian(map, dataOffsets[index], 4); // no attributes
        appendBigEndian(map, 0, 4);
    }
    map.insert(map.end(), names.begin(), names.end());
    const auto mapOffset = dataOffset + static_cast<std::uint32_t>(data.size());
    std::vector<std::uint8_t> fork(dataOffset, 0);
    putBigEndian(fork, 0, dataOffset, 4);
    putBigEndian(fork, 4, mapOffset, 4);
    putBigEndian(fork, 8, data.size(), 4);
    putBigEndian(fork, 12, map.size(), 4);
    std::copy_n(fork.begin(), 16, map.begin());
    fork.insert(fork.end(), data.begin(), data.end());
    fork.insert(fork.end(), map.begin(), map.end());
    return fork;
}

std::optional<SoundDesignerIIParameters> soundDesignerIIParametersFromResourceFork(
    std::span<const std::uint8_t> fork)
{
    const auto big = [&](std::size_t offset, std::size_t count) -> std::optional<std::uint32_t> {
        if (offset > fork.size() || count > fork.size() - offset)
            return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < count; ++index)
            value = (value << 8U) | fork[offset + index];
        return value;
    };
    const auto dataOffset = big(0, 4);
    const auto mapOffset = big(4, 4);
    if (!dataOffset || !mapOffset)
        return std::nullopt;
    const auto typeListOffset = big(std::size_t{*mapOffset} + 24U, 2);
    if (!typeListOffset)
        return std::nullopt;
    const auto typeList = std::size_t{*mapOffset} + *typeListOffset;
    const auto typeCount = big(typeList, 2);
    if (!typeCount)
        return std::nullopt;
    std::map<std::uint32_t, std::string> strings;
    for (std::size_t type = 0; type < ((*typeCount + 1U) & 0xFFFFU); ++type)
    {
        const auto entry = typeList + 2U + 8U * type;
        const auto name = big(entry, 4);
        const auto count = big(entry + 4U, 2);
        const auto references = big(entry + 6U, 2);
        if (!name || !count || !references)
            return std::nullopt;
        if (*name != 0x53545220U) // 'STR '
            continue;
        for (std::size_t index = 0; index < ((*count + 1U) & 0xFFFFU); ++index)
        {
            const auto reference = typeList + *references + 12U * index;
            const auto id = big(reference, 2);
            const auto offset = big(reference + 5U, 3);
            if (!id || !offset)
                return std::nullopt;
            const auto at = std::size_t{*dataOffset} + *offset;
            const auto length = big(at, 4);
            const auto text = big(at + 4U, 1);
            if (!length || !text || *length == 0 || *text + 1U > *length || at + 5U + *text > fork.size())
                continue;
            strings[*id] = std::string(fork.begin() + static_cast<std::ptrdiff_t>(at + 5U),
                fork.begin() + static_cast<std::ptrdiff_t>(at + 5U + *text));
        }
    }
    if (!strings.contains(1000) || !strings.contains(1001) || !strings.contains(1002))
        return std::nullopt;
    const auto size = wholeNumber(strings[1000]);
    const auto rate = decimalNumber(strings[1001]);
    const auto channels = wholeNumber(strings[1002]);
    if (!size || *size < 1 || *size > 4 || !rate || !(*rate >= 1.0) || *rate > 4294967295.0 || !channels
        || *channels < 1 || *channels > 64)
    {
        return std::nullopt;
    }
    return SoundDesignerIIParameters{static_cast<std::uint16_t>(*size), *rate, static_cast<std::uint16_t>(*channels)};
}

std::vector<std::uint8_t> resourceForkOf(const std::filesystem::path& path)
{
    if (!soundDesignerIIAvailable())
        return {};
    std::ifstream input(path / "..namedfork" / "rsrc", std::ios::binary);
    if (!input)
        return {};
    // A resource fork holds at most 16 MB.
    std::vector<std::uint8_t> bytes;
    std::array<char, 64U * 1024U> buffer{};
    while (input && bytes.size() <= 16U * 1024U * 1024U)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    return bytes.size() > 16U * 1024U * 1024U ? std::vector<std::uint8_t>{} : bytes;
}
}
