#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

void requireContains(std::string_view haystack, std::string_view needle, const std::string& message)
{
    require(haystack.find(needle) != std::string_view::npos, message);
}

std::filesystem::path temporaryWavPath()
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("talas-wav-writer-test-" + std::to_string(now) + "-" + std::to_string(counter++) + ".wav");
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "test should open written WAV file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint16_t readU16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    require(offset + 2 <= bytes.size(), "test should read u16 within file bounds");
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    require(offset + 4 <= bytes.size(), "test should read u32 within file bounds");
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::int16_t readI16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(readU16(bytes, offset));
}

void requireAscii(const std::vector<unsigned char>& bytes, std::size_t offset, std::string_view expected)
{
    require(offset + expected.size() <= bytes.size(), "test should read ascii field within file bounds");
    require(std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)),
            "ascii WAV field should match " + std::string(expected));
}

void testWritesMonoPcm16WavHeaderAndSamples()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {{0.0, 1.0, -1.0}};
    std::string error;

    const bool ok = talas::audio::writePcm16Wav(path, 48000.0, channels, error);

    require(ok, "mono WAV writer should succeed: " + error);
    const auto bytes = readBytes(path);
    std::filesystem::remove(path);

    require(bytes.size() == 50, "mono WAV should contain 44 byte header plus 6 bytes of data");
    requireAscii(bytes, 0, "RIFF");
    require(readU32(bytes, 4) == 42, "RIFF chunk size should match file size minus 8");
    requireAscii(bytes, 8, "WAVE");
    requireAscii(bytes, 12, "fmt ");
    require(readU32(bytes, 16) == 16, "fmt chunk should be PCM size");
    require(readU16(bytes, 20) == 1, "audio format should be PCM");
    require(readU16(bytes, 22) == 1, "mono WAV should report one channel");
    require(readU32(bytes, 24) == 48000, "sample rate should be serialized");
    require(readU32(bytes, 28) == 96000, "byte rate should be sampleRate * blockAlign");
    require(readU16(bytes, 32) == 2, "block align should be two bytes for mono PCM16");
    require(readU16(bytes, 34) == 16, "bits per sample should be 16");
    requireAscii(bytes, 36, "data");
    require(readU32(bytes, 40) == 6, "data chunk size should match sample bytes");
    require(readI16(bytes, 44) == 0, "zero sample should serialize to 0");
    require(readI16(bytes, 46) == 32767, "positive full-scale sample should serialize to 32767");
    require(readI16(bytes, 48) == -32768, "negative full-scale sample should serialize to -32768");
}

void testWritesStereoSamplesInterleaved()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {
        {0.0, 0.5},
        {1.0, -1.0},
    };
    std::string error;

    const bool ok = talas::audio::writePcm16Wav(path, 44100.0, channels, error);

    require(ok, "stereo WAV writer should succeed: " + error);
    const auto bytes = readBytes(path);
    std::filesystem::remove(path);

    require(readU16(bytes, 22) == 2, "stereo WAV should report two channels");
    require(readU16(bytes, 32) == 4, "stereo block align should be four bytes");
    require(readU32(bytes, 40) == 8, "stereo data chunk should include two frames of two channels");
    require(readI16(bytes, 44) == 0, "left frame zero should be first");
    require(readI16(bytes, 46) == 32767, "right frame zero should be second");
    require(readI16(bytes, 48) == 16384, "left frame one should be third");
    require(readI16(bytes, 50) == -32768, "right frame one should be fourth");
}

void testRejectsEmptyChannels()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels;
    std::string error;

    const bool ok = talas::audio::writePcm16Wav(path, 48000.0, channels, error);

    require(!ok, "empty channel list should fail");
    requireContains(error, "at least one channel", "empty channel failure should explain channel requirement");
}

void testRejectsMismatchedChannelLengths()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {
        {0.0, 1.0},
        {0.0},
    };
    std::string error;

    const bool ok = talas::audio::writePcm16Wav(path, 48000.0, channels, error);

    require(!ok, "mismatched channel lengths should fail");
    requireContains(error, "same frame count", "mismatched length failure should explain frame count requirement");
}

void testRejectsNonFiniteSamples()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {{0.0, std::numeric_limits<double>::quiet_NaN()}};
    std::string error;

    const bool ok = talas::audio::writePcm16Wav(path, 48000.0, channels, error);

    require(!ok, "non-finite sample should fail");
    requireContains(error, "non-finite sample", "non-finite sample failure should identify sample validation");
}

void testUnwritableUnicodePathIsReportedAsUtf8()
{
    const auto path = std::filesystem::temp_directory_path()
        / std::filesystem::path(std::u8string{u8"nedostaje κ"})
        / "output.wav";
    talas::audio::Pcm16WavStreamWriter writer(path, 48000.0, 1, 1);

    require(!writer.ready(), "writer should reject a path under a missing directory");
    requireContains(
        writer.error(),
        talas::project::pathToUtf8(path),
        "unwritable Unicode WAV failure should preserve the UTF-8 path");
}

void testStreamingWriterAppendsBlocksAndFinalizesExactFrameCount()
{
    const auto path = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter writer(path, 8000.0, 2, 3);
    require(writer.ready(), "streaming writer should initialize: " + std::string(writer.error()));

    const std::vector<std::vector<double>> first = {
        {0.0, 0.5},
        {1.0, -1.0},
    };
    const std::vector<std::vector<double>> second = {
        {-0.5},
        {0.25},
    };
    require(writer.write(first), "streaming writer should accept first block");
    require(writer.write(second), "streaming writer should accept final block");
    require(writer.framesWritten() == 3, "streaming writer should track appended frames");
    require(writer.finish(), "streaming writer should finalize exact declared frame count");

    const auto bytes = readBytes(path);
    std::filesystem::remove(path);
    require(readU16(bytes, 22) == 2, "streaming WAV should report declared channels");
    require(readU32(bytes, 40) == 12, "streaming WAV should report all appended PCM bytes");
    require(readI16(bytes, 44) == 0 && readI16(bytes, 46) == 32767,
            "streaming first frame should be interleaved");
    require(readI16(bytes, 52) == -16384 && readI16(bytes, 54) == 8192,
            "streaming final frame should follow earlier blocks");
}

void testStreamingWriterRejectsOverflowAndIncompleteFinish()
{
    const auto overflowPath = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter overflow(overflowPath, 48000.0, 1, 1);
    require(overflow.ready(), "overflow fixture should initialize");
    const std::vector<std::vector<double>> tooMany = {{0.0, 1.0}};
    require(!overflow.write(tooMany), "streaming writer should reject more than declared frames");
    requireContains(overflow.error(), "declared frame count",
                    "streaming overflow should explain the declared size");
    std::filesystem::remove(overflowPath);

    const auto incompletePath = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter incomplete(incompletePath, 48000.0, 1, 2);
    require(incomplete.ready(), "incomplete fixture should initialize");
    const std::vector<std::vector<double>> one = {{0.0}};
    require(incomplete.write(one), "incomplete fixture should accept a partial block");
    require(!incomplete.finish(), "streaming writer should reject premature finish");
    requireContains(incomplete.error(), "declared frame count",
                    "premature finish should explain missing frames");
    std::filesystem::remove(incompletePath);

    const auto riffOverflowPath = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter riffOverflow(
        riffOverflowPath,
        48000.0,
        2,
        std::numeric_limits<std::size_t>::max());
    require(!riffOverflow.ready(), "RIFF size overflow should reject before opening a file");
    requireContains(riffOverflow.error(), "too large",
                    "RIFF size overflow should explain the format limit");
    require(!std::filesystem::exists(riffOverflowPath),
            "RIFF size overflow should not create a destination");
}

void testGrowingWriterFinalizesExactInterleavedFrames()
{
    const auto path = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter writer(path, 8000.0, 2);
    require(writer.ready(),
        "growing writer should initialize: " + std::string(writer.error()));
    const std::vector<float> first{0.0F, 1.0F, 0.5F, -1.0F};
    const std::vector<float> second{-0.5F, 0.25F};
    require(writer.writeInterleaved(first),
        "growing writer should accept first interleaved block");
    require(writer.writeInterleaved(second),
        "growing writer should accept second interleaved block");
    require(writer.framesWritten() == 3,
        "growing writer should track exact appended frames");
    require(writer.finish(),
        "growing writer should finalize the observed frame count");

    const auto bytes = readBytes(path);
    std::filesystem::remove(path);
    require(readU16(bytes, 22) == 2,
        "growing writer should retain exact channel metadata");
    require(readU32(bytes, 24) == 8000,
        "growing writer should retain exact sample-rate metadata");
    require(readU32(bytes, 40) == 12,
        "growing writer should rewrite the exact final data size");
    require(readI16(bytes, 44) == 0 && readI16(bytes, 46) == 32767
            && readI16(bytes, 52) == -16384
            && readI16(bytes, 54) == 8192,
        "growing writer should preserve exact interleaved samples");
}

// A DiskRecorder writes 8-, 16- or 24-bit words: the largest positive
// code is 1 and the extra negative code -1, and
// 8-bit words are unsigned around 128.
void testGrowingWriterWritesEightAndTwentyFourBitWords()
{
    for (const auto bits : {std::uint16_t{8}, std::uint16_t{24}})
    {
        const auto path = temporaryWavPath();
        auto* output = std::fopen(path.c_str(), "wb");
        require(output != nullptr, "test should open a WAV file");
        talas::audio::Pcm16WavStreamWriter writer(output, talas::project::pathToUtf8(path), 8000.0, 1,
            talas::audio::WavWordSize{bits});
        require(writer.ready(), "word-size writer should initialize: " + std::string(writer.error()));
        require(writer.writeInterleaved(std::vector<float>{1.0F, -1.0F, 0.0F, 0.5F}) && writer.finish(),
            "word-size writer should take and finish a block: " + std::string(writer.error()));
        const auto bytes = readBytes(path);
        std::filesystem::remove(path);
        const auto bytesPerSample = bits / 8U;
        require(readU16(bytes, 32) == bytesPerSample && readU16(bytes, 34) == bits
                && readU32(bytes, 28) == 8000U * bytesPerSample && readU32(bytes, 40) == 4U * bytesPerSample,
            "word-size writer should describe its words in the header");
        if (bits == 8)
        {
            require(bytes[44] == 255 && bytes[45] == 0 && bytes[46] == 128 && bytes[47] == 128 + 64,
                "8-bit words should be unsigned around 128");
        }
        else
        {
            const auto word = [&](std::size_t index) {
                const auto offset = 44 + 3 * index;
                const auto code = static_cast<std::uint32_t>(bytes[offset])
                    | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
                    | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
                return code;
            };
            require(word(0) == 0x7FFFFF && word(1) == 0x800000 && word(2) == 0 && word(3) == 0x400000,
                "24-bit words should take the full scale of 8388607");
        }
    }
    const auto refusedPath = temporaryWavPath();
    {
        talas::audio::Pcm16WavStreamWriter refused(std::fopen(refusedPath.c_str(), "wb"), "refused", 8000.0, 1,
            talas::audio::WavWordSize{32});
        require(!refused.ready(), "a 32-bit word size should be refused");
    }
    std::filesystem::remove(refusedPath);
}

void testGrowingWriterRejectsEmptyAndMalformedBlocks()
{
    const auto emptyPath = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter empty(emptyPath, 48000.0, 2);
    require(empty.ready(), "empty growing fixture should initialize");
    require(!empty.finish(), "empty growing take should not publish");
    requireContains(empty.error(), "any frames",
        "empty growing failure should explain missing audio");
    std::filesystem::remove(emptyPath);

    const auto malformedPath = temporaryWavPath();
    talas::audio::Pcm16WavStreamWriter malformed(malformedPath, 48000.0, 2);
    const std::vector<float> partialFrame{0.0F};
    require(!malformed.writeInterleaved(partialFrame),
        "partial interleaved frame should reject");
    requireContains(malformed.error(), "complete frames",
        "partial-frame rejection should be explicit");
    std::filesystem::remove(malformedPath);
}
}

// A DiskRecorder's 24-bit words take the most
// space and give the most dynamic range, 8-bit ones the least of each, and
// a mono take of identical channels half the space of a stereo one.
void testWordSizesTradeSpaceForDynamicRange()
{
    constexpr std::size_t frames = 4096;
    std::vector<float> quiet(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        quiet[frame] = 0.01F * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 1000.0 * static_cast<double>(frame) / 48000.0));
    const auto written = [&](std::uint16_t bits, std::uint16_t channels) {
        const auto path = temporaryWavPath();
        auto* output = std::fopen(path.c_str(), "wb");
        require(output != nullptr, "test should open a WAV file");
        talas::audio::Pcm16WavStreamWriter writer(output, talas::project::pathToUtf8(path), 48000.0, channels,
            talas::audio::WavWordSize{bits});
        std::vector<float> interleaved;
        for (const auto sample : quiet)
            interleaved.insert(interleaved.end(), channels, sample);
        require(writer.ready() && writer.writeInterleaved(interleaved) && writer.finish(),
            "the take should be written: " + std::string(writer.error()));
        const auto bytes = readBytes(path);
        std::filesystem::remove(path);
        return bytes;
    };
    std::vector<double> errors;
    for (const auto bits : {std::uint16_t{8}, std::uint16_t{16}, std::uint16_t{24}})
    {
        const auto bytes = written(bits, 1);
        const auto width = bits / 8U;
        require(readU32(bytes, 40) == frames * width, "a word of " + std::to_string(bits) + " bits should take "
                + std::to_string(width) + " bytes a frame");
        double squared = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto offset = 44 + width * frame;
            double decoded = 0.0;
            if (bits == 8)
                decoded = (static_cast<double>(bytes[offset]) - 128.0) / 128.0;
            else if (bits == 16)
                decoded = static_cast<double>(static_cast<std::int16_t>(readU16(bytes, offset))) / 32768.0;
            else
            {
                auto code = static_cast<std::int32_t>(bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16));
                if ((code & 0x800000) != 0)
                    code -= 0x1000000;
                decoded = static_cast<double>(code) / 8388608.0;
            }
            squared += (decoded - quiet[frame]) * (decoded - quiet[frame]);
        }
        errors.push_back(std::sqrt(squared / static_cast<double>(frames)));
    }
    require(errors[0] > errors[1] && errors[1] > errors[2],
        "the wider the word, the smaller the error of a quiet take");
    require(readU32(written(16, 1), 40) * 2 == readU32(written(16, 2), 40),
        "a mono take should take half the space of a stereo one");
}

int main()
{
    testWritesMonoPcm16WavHeaderAndSamples();
    testWritesStereoSamplesInterleaved();
    testRejectsEmptyChannels();
    testRejectsMismatchedChannelLengths();
    testRejectsNonFiniteSamples();
    testUnwritableUnicodePathIsReportedAsUtf8();
    testStreamingWriterAppendsBlocksAndFinalizesExactFrameCount();
    testStreamingWriterRejectsOverflowAndIncompleteFinish();
    testGrowingWriterFinalizesExactInterleavedFrames();
    testGrowingWriterRejectsEmptyAndMalformedBlocks();
    testGrowingWriterWritesEightAndTwentyFourBitWords();
    testWordSizesTradeSpaceForDynamicRange();
    return 0;
}
