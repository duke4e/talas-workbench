#include "talas/audio/WavReader.h"
#include "talas/audio/Pcm16WaveSpan.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectPathEncoding.h"
#include "WavSmplFixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <span>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    const auto difference = std::abs(actual - expected);
    if (difference > tolerance)
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

std::filesystem::path temporaryWavPath()
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("talas-wav-reader-test-" + std::to_string(now) + "-" + std::to_string(counter++) + ".wav");
}

void writeBytes(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary);
    require(output.good(), "test should create malformed WAV file");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "test should write malformed WAV file");
}

void writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    require(output.good(), "test should create malformed WAV file");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "test should write malformed WAV file");
}

void testReadsMonoPcm16Wav()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {{0.0, 1.0, -1.0}};
    std::string error;
    require(talas::audio::writePcm16Wav(path, 48000.0, channels, error), "test should write mono WAV fixture: " + error);

    const auto read = talas::audio::readPcm16Wav(path);

    std::filesystem::remove(path);
    require(read.ok(), "reader should parse mono WAV: " + read.error);
    require(read.sampleRate == 48000, "reader should preserve sample rate");
    require(read.channels.size() == 1, "reader should return one channel");
    require(read.channels[0].size() == 3, "reader should return three frames");
    requireNear(read.channels[0][0], 0.0, 1.0e-12, "mono frame zero");
    requireNear(read.channels[0][1], 1.0, 1.0e-12, "mono frame one");
    requireNear(read.channels[0][2], -1.0, 1.0e-12, "mono frame two");
}

void testReadsStereoPcm16WavAsSeparateChannels()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {
        {0.0, 0.5},
        {1.0, -1.0},
    };
    std::string error;
    require(talas::audio::writePcm16Wav(path, 44100.0, channels, error), "test should write stereo WAV fixture: " + error);

    const auto read = talas::audio::readPcm16Wav(path);

    std::filesystem::remove(path);
    require(read.ok(), "reader should parse stereo WAV: " + read.error);
    require(read.sampleRate == 44100, "reader should preserve stereo sample rate");
    require(read.channels.size() == 2, "reader should return two channels");
    require(read.channels[0].size() == 2, "reader should return left frames");
    require(read.channels[1].size() == 2, "reader should return right frames");
    requireNear(read.channels[0][0], 0.0, 1.0e-12, "left frame zero");
    requireNear(read.channels[1][0], 1.0, 1.0e-12, "right frame zero");
    requireNear(read.channels[0][1], 16384.0 / 32767.0, 1.0e-12, "left frame one");
    requireNear(read.channels[1][1], -1.0, 1.0e-12, "right frame one");
}

void testLargeWavReadIsCooperativelyCancellable()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels = {
        std::vector<double>(40'000U, 0.25),
    };
    std::string error;
    require(
        talas::audio::writePcm16Wav(
            path,
            48000.0,
            channels,
            error),
        "test should write cancellable WAV fixture: " + error);

    int cancellationChecks = 0;
    const auto read = talas::audio::readPcm16Wav(
        path,
        [&] { return ++cancellationChecks == 2; });

    std::filesystem::remove(path);
    require(
        read.cancelled
            && !read.ok()
            && read.channels.empty(),
        "WAV payload read should stop between bounded file chunks");
}

void testRejectsMissingFile()
{
    const auto read = talas::audio::readPcm16Wav("/path/that/does/not/exist.wav");

    require(!read.ok(), "missing WAV file should fail");
    requireContains(read.error, "failed to read WAV file", "missing WAV failure should explain file read");
}

void testMissingUnicodePathIsReportedAsUtf8()
{
    const auto path = std::filesystem::temp_directory_path()
        / std::filesystem::path(std::u8string{u8"nema κ.wav"});
    const auto read = talas::audio::readPcm16Wav(path);

    require(!read.ok(), "missing Unicode WAV file should fail");
    requireContains(
        read.error,
        talas::project::pathToUtf8(path),
        "missing Unicode WAV failure should preserve the UTF-8 path");
}

void testRejectsNonWaveFile()
{
    const auto path = temporaryWavPath();
    writeBytes(path, "not a wave");

    const auto read = talas::audio::readPcm16Wav(path);

    std::filesystem::remove(path);
    require(!read.ok(), "non-WAVE file should fail");
    requireContains(read.error, "RIFF/WAVE", "non-WAVE failure should identify expected container");
}

void testRejectsUnsupportedFormat()
{
    const auto path = temporaryWavPath();
    const std::vector<unsigned char> bytes = {
        'R', 'I', 'F', 'F', 0x28, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 0x10, 0x00, 0x00, 0x00,
        0x03, 0x00,
        0x01, 0x00,
        0x80, 0xbb, 0x00, 0x00,
        0x00, 0x77, 0x01, 0x00,
        0x02, 0x00,
        0x10, 0x00,
        'd', 'a', 't', 'a', 0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    writeBytes(path, bytes);

    const auto read = talas::audio::readPcm16Wav(path);

    std::filesystem::remove(path);
    require(!read.ok(), "non-PCM WAV should fail");
    requireContains(read.error, "PCM16", "unsupported format failure should identify PCM16");
}

void testReadsForwardSmplLoopAndRejectsInvalidFrameBounds()
{
    const auto path = temporaryWavPath();
    std::string error;
    require(talas::test::writeMonoWavWithSmpl(
                path, 8.0, std::vector<double>(8U, 0.25),
                2U, 4U, error),
        "test should write smpl WAV: " + error);
    const auto read = talas::audio::readPcm16Wav(path);
    require(read.ok() && read.sampleRate == 8
                && read.forwardLoop
                    == talas::core::SampleLoop{2U, 5U},
        "WAV reader should convert inclusive smpl end to an exclusive frame boundary");

    require(talas::test::writeMonoWavWithSmpl(
                path, 8.0, std::vector<double>(8U, 0.25),
                2U, 8U, error),
        "test should write out-of-range smpl WAV: " + error);
    const auto invalid = talas::audio::readPcm16Wav(path);
    require(!invalid.ok()
                && invalid.error.find("outside") != std::string::npos,
        "WAV reader should reject a smpl endpoint past decoded frames");

    require(talas::test::writeMonoWavWithSmpl(
                path, 8.0, std::vector<double>(8U, 0.25),
                2U, 4U, error, 1U),
        "test should write unsupported loop-type WAV: " + error);
    const auto otherType = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(otherType.ok() && !otherType.forwardLoop.has_value(),
        "WAV reader should keep non-forward smpl loops distinct from forward loops");
}
}

void testTableAndStreamingDecodeEveryPcm16CodeAlike()
{
    std::vector<unsigned char> bytes;
    const auto appendU16 = [&](std::uint16_t value) {
        bytes.push_back(static_cast<unsigned char>(value & 0xffU));
        bytes.push_back(static_cast<unsigned char>(value >> 8U));
    };
    const auto appendU32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U)
            bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    };
    constexpr std::uint32_t frames = 65'536U;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    appendU32(36U + frames * 2U);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    appendU32(16U);
    appendU16(1U);
    appendU16(1U);
    appendU32(48'000U);
    appendU32(96'000U);
    appendU16(2U);
    appendU16(16U);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(frames * 2U);
    for (std::uint32_t code = 0; code < frames; ++code)
        appendU16(static_cast<std::uint16_t>(code));

    const auto path = temporaryWavPath();
    writeBytes(path, bytes);
    const auto table = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    const auto stream = talas::audio::parseStrictPcm16Wave(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()});
    require(table.ok() && stream.ok()
            && table.channels.front().size() == frames,
        "every PCM16 code should load through both readers");
    for (std::uint32_t frame = 0; frame < frames; ++frame)
    {
        std::array<double, 1> streamed{};
        require(talas::audio::decodePcm16WaveFrame(
                    std::span<const std::uint8_t>{bytes.data(), bytes.size()},
                    stream.layout, frame, streamed)
                && streamed[0] == table.channels.front()[frame],
            "Table and streamed Audio must decode PCM16 code "
                + std::to_string(frame) + " identically");
    }
}

// Sample file bytes, little-endian RIFF/WAVE or big-endian AIFF chunks.
struct SampleFileBytes
{
    std::vector<unsigned char> bytes;

    void ascii(std::string_view text) { bytes.insert(bytes.end(), text.begin(), text.end()); }
    void little(std::uint64_t value, int count)
    {
        for (int index = 0; index < count; ++index)
            bytes.push_back(static_cast<unsigned char>(value >> (8 * index)));
    }
    void big(std::uint64_t value, int count)
    {
        for (int index = count - 1; index >= 0; --index)
            bytes.push_back(static_cast<unsigned char>(value >> (8 * index)));
    }
};

// A RIFF/WAVE file of one fmt chunk, then `extra` chunks, then data.
std::vector<unsigned char> waveFile(std::uint16_t format, std::uint16_t channels, std::uint16_t bits,
    const std::vector<unsigned char>& data, const std::vector<unsigned char>& extra = {},
    std::uint16_t extensibleFormat = 0)
{
    SampleFileBytes chunks;
    chunks.ascii("fmt ");
    chunks.little(extensibleFormat != 0 ? 40 : 16, 4);
    chunks.little(extensibleFormat != 0 ? 0xFFFE : format, 2);
    chunks.little(channels, 2);
    chunks.little(48000, 4);
    chunks.little(48000U * channels * (bits / 8U), 4);
    chunks.little(channels * (bits / 8U), 2);
    chunks.little(bits, 2);
    if (extensibleFormat != 0)
    {
        chunks.little(22, 2);
        chunks.little(bits, 2);
        chunks.little(0, 4);
        chunks.little(extensibleFormat, 2);
        for (const auto byte : {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71})
            chunks.bytes.push_back(static_cast<unsigned char>(byte));
    }
    chunks.bytes.insert(chunks.bytes.end(), extra.begin(), extra.end());
    chunks.ascii("data");
    chunks.little(data.size(), 4);
    chunks.bytes.insert(chunks.bytes.end(), data.begin(), data.end());
    if (data.size() % 2 != 0)
        chunks.bytes.push_back(0);
    SampleFileBytes file;
    file.ascii("RIFF");
    file.little(4 + chunks.bytes.size(), 4);
    file.ascii("WAVE");
    file.bytes.insert(file.bytes.end(), chunks.bytes.begin(), chunks.bytes.end());
    return file.bytes;
}

// An AIFF file, or an AIFF-C one with `compression`, at 48 kHz.
std::vector<unsigned char> aiffFile(std::string_view compression, std::uint16_t channels, std::uint16_t bits,
    std::uint32_t frames, const std::vector<unsigned char>& data, const std::vector<unsigned char>& extra = {})
{
    SampleFileBytes chunks;
    chunks.ascii("COMM");
    chunks.big(compression.empty() ? 18 : 24, 4);
    chunks.big(channels, 2);
    chunks.big(frames, 4);
    chunks.big(bits, 2);
    // 48000 as an 80-bit IEEE extended number.
    chunks.big(0x400E, 2);
    chunks.big(0xBB80000000000000ULL, 8);
    if (!compression.empty())
    {
        chunks.ascii(compression);
        chunks.big(0, 2);
    }
    chunks.bytes.insert(chunks.bytes.end(), extra.begin(), extra.end());
    chunks.ascii("SSND");
    chunks.big(8 + data.size(), 4);
    chunks.big(0, 4);
    chunks.big(0, 4);
    chunks.bytes.insert(chunks.bytes.end(), data.begin(), data.end());
    if (data.size() % 2 != 0)
        chunks.bytes.push_back(0);
    SampleFileBytes file;
    file.ascii("FORM");
    file.big(4 + chunks.bytes.size(), 4);
    file.ascii(compression.empty() ? "AIFF" : "AIFC");
    file.bytes.insert(file.bytes.end(), chunks.bytes.begin(), chunks.bytes.end());
    return file.bytes;
}

talas::audio::WavReadResult readSampleBytes(const std::vector<unsigned char>& bytes)
{
    const auto path = temporaryWavPath();
    writeBytes(path, bytes);
    auto read = talas::audio::readSampleFile(path);
    std::filesystem::remove(path);
    return read;
}

// Samples imported from other programs read in their formats, AIFF and
// WAV among them, of 8, 16 or 24 bits.
void testReadsEightTwentyFourAndThirtyTwoBitWav()
{
    const auto eight = readSampleBytes(waveFile(1, 1, 8, {255, 0, 128}));
    require(eight.ok() && eight.channels.front() == std::vector<double>{1.0, -1.0, 0.0},
        "8-bit WAV samples are unsigned around 128: " + eight.error);
    const auto twentyFour = readSampleBytes(waveFile(1, 1, 24,
        {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00}));
    require(twentyFour.ok()
            && twentyFour.channels.front() == std::vector<double>{1.0, -1.0, 1.0 / 8388607.0},
        "24-bit WAV samples should decode with the WAV writer's full scale: " + twentyFour.error);
    const auto thirtyTwo = readSampleBytes(waveFile(1, 2, 32,
        {0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80}));
    require(thirtyTwo.ok() && thirtyTwo.channels.size() == 2
            && thirtyTwo.channels[0] == std::vector<double>{1.0}
            && thirtyTwo.channels[1] == std::vector<double>{-1.0},
        "32-bit stereo WAV samples should decode into their channels: " + thirtyTwo.error);
    const auto strictPath = temporaryWavPath();
    writeBytes(strictPath, waveFile(1, 1, 24, {0, 0, 0}));
    const auto strict = talas::audio::readPcm16Wav(strictPath);
    std::filesystem::remove(strictPath);
    require(!strict.ok() && strict.error.find("PCM16") != std::string::npos,
        "the PCM16 reader should keep refusing other word sizes");
}

void testReadsFloatAndExtensibleWav()
{
    std::vector<unsigned char> floats;
    for (const auto value : {0.5F, -0.25F})
    {
        const auto code = std::bit_cast<std::uint32_t>(value);
        for (int index = 0; index < 4; ++index)
            floats.push_back(static_cast<unsigned char>(code >> (8 * index)));
    }
    const auto single = readSampleBytes(waveFile(3, 1, 32, floats));
    require(single.ok() && single.channels.front() == std::vector<double>{0.5, -0.25},
        "32-bit float WAV samples should read as they are: " + single.error);
    std::vector<unsigned char> doubleBytes;
    const auto doubleCode = std::bit_cast<std::uint64_t>(0.125);
    for (int index = 0; index < 8; ++index)
        doubleBytes.push_back(static_cast<unsigned char>(doubleCode >> (8 * index)));
    const auto twice = readSampleBytes(waveFile(3, 1, 64, doubleBytes));
    require(twice.ok() && twice.channels.front() == std::vector<double>{0.125},
        "64-bit float WAV samples should read as they are: " + twice.error);
    const auto extensible = readSampleBytes(waveFile(0, 1, 24, {0x00, 0x00, 0x40}, {}, 1));
    require(extensible.ok() && extensible.channels.front() == std::vector<double>{4194304.0 / 8388607.0},
        "WAVE_FORMAT_EXTENSIBLE PCM should read as plain PCM: " + extensible.error);
}

void testReadsAiffWithItsInstrument()
{
    // INST: base note 60 detuned -10 cents, notes 48 to 72, velocities 1 to
    // 100, and a forward sustain loop between markers 1 and 2.
    SampleFileBytes extra;
    extra.ascii("INST");
    extra.big(20, 4);
    for (const auto byte : {60, 0xF6, 48, 72, 1, 100})
        extra.big(static_cast<std::uint64_t>(byte), 1);
    extra.big(0, 2);
    extra.big(1, 2);
    extra.big(1, 2);
    extra.big(2, 2);
    extra.big(0, 2);
    extra.big(0, 2);
    extra.big(0, 2);
    extra.ascii("MARK");
    extra.big(2 + 2 * 8, 4);
    extra.big(2, 2);
    for (const auto& [id, position] : {std::pair{1, 1}, std::pair{2, 2}})
    {
        extra.big(static_cast<std::uint64_t>(id), 2);
        extra.big(static_cast<std::uint64_t>(position), 4);
        extra.big(0, 2);
    }
    const auto read = readSampleBytes(aiffFile({}, 1, 16, 3, {0x40, 0x00, 0xC0, 0x00, 0x7F, 0xFF}, extra.bytes));
    require(read.ok() && read.sampleRate == 48000
            && read.channels.front() == std::vector<double>{16384.0 / 32767.0, -16384.0 / 32767.0, 1.0},
        "AIFF samples should decode big-endian: " + read.error);
    require(read.samplePitch
                == talas::core::SamplePitch{59, static_cast<std::uint32_t>(0.9 * 4294967296.0 + 0.5)}
            && read.keyRange == talas::core::SampleKeyRange{48, 72, 1, 100}
            && read.forwardLoop == talas::core::SampleLoop{1, 2},
        "an AIFF INST chunk should give the base pitch, the key and velocity ranges and the sustain loop");
}

void testReadsLittleEndianAndFloatAifc()
{
    const auto swapped = readSampleBytes(aiffFile("sowt", 1, 16, 2, {0x00, 0x40, 0x00, 0xC0}));
    require(swapped.ok() && swapped.channels.front() == std::vector<double>{16384.0 / 32767.0, -16384.0 / 32767.0},
        "AIFF-C sowt samples are little-endian: " + swapped.error);
    std::vector<unsigned char> floats;
    for (const auto value : {0.75F, -0.5F})
    {
        const auto code = std::bit_cast<std::uint32_t>(value);
        for (int index = 3; index >= 0; --index)
            floats.push_back(static_cast<unsigned char>(code >> (8 * index)));
    }
    const auto floating = readSampleBytes(aiffFile("fl32", 1, 32, 2, floats));
    require(floating.ok() && floating.channels.front() == std::vector<double>{0.75, -0.5},
        "AIFF-C fl32 samples are big-endian floats: " + floating.error);
}

void testReadsRiffInstrumentChunk()
{
    SampleFileBytes extra;
    extra.ascii("inst");
    extra.little(7, 4);
    for (const auto byte : {64, 0, 0, 60, 70, 1, 127})
        extra.little(static_cast<std::uint64_t>(byte), 1);
    extra.little(0, 1);
    const auto read = readSampleBytes(waveFile(1, 1, 16, {0x00, 0x00}, extra.bytes));
    require(read.ok() && read.samplePitch == talas::core::SamplePitch{64, 0}
            && read.keyRange == talas::core::SampleKeyRange{60, 70, 1, 127},
        "a RIFF inst chunk should give the base pitch and the key and velocity ranges: " + read.error);
}

// A sample's markers by name, from an AIFF MARK
// chunk or a WAVE cue chunk labelled in a LIST adtl chunk; an unnamed one is
// named by its id, and one past the last frame is left out.
void testReadsMarkerNames()
{
    SampleFileBytes mark;
    mark.ascii("MARK");
    mark.big(2 + 14 + 8 + 12, 4);
    mark.big(3, 2);
    for (const auto& [id, frame, name] : {std::tuple{1, 1, std::string_view{"attack"}}, std::tuple{2, 2, std::string_view{}},
             std::tuple{3, 9, std::string_view{"late"}}})
    {
        mark.big(static_cast<std::uint64_t>(id), 2);
        mark.big(static_cast<std::uint64_t>(frame), 4);
        mark.big(name.size(), 1);
        mark.ascii(name);
        if (name.size() % 2 == 0)
            mark.big(0, 1);
    }
    const auto aiff = readSampleBytes(aiffFile({}, 1, 16, 3, {0, 0, 0, 0, 0, 0}, mark.bytes));
    require(aiff.ok()
            && aiff.markers == std::vector<talas::core::SampleMarker>{{"attack", 1}, {"marker2", 2}},
        "AIFF markers should keep their names and frames: " + aiff.error);

    SampleFileBytes cue;
    cue.ascii("cue ");
    cue.little(4 + 2 * 24, 4);
    cue.little(2, 4);
    for (const auto& [id, offset] : {std::pair{1, 1}, std::pair{7, 0}})
    {
        cue.little(static_cast<std::uint64_t>(id), 4);
        cue.little(0, 4);
        cue.ascii("data");
        cue.little(0, 4);
        cue.little(0, 4);
        cue.little(static_cast<std::uint64_t>(offset), 4);
    }
    cue.ascii("LIST");
    cue.little(4 + 8 + 9 + 1, 4);
    cue.ascii("adtl");
    cue.ascii("labl");
    cue.little(9, 4);
    cue.little(1, 4);
    cue.ascii("loop");
    cue.little(0, 1);
    cue.little(0, 1);
    const auto wave = readSampleBytes(waveFile(1, 1, 16, {0, 0, 0, 0}, cue.bytes));
    require(wave.ok() && wave.markers == std::vector<talas::core::SampleMarker>{{"loop", 1}, {"marker7", 0}},
        "WAVE cue points should take their labels: " + wave.error);
}

int main()
{
    testReadsMonoPcm16Wav();
    testReadsStereoPcm16WavAsSeparateChannels();
    testLargeWavReadIsCooperativelyCancellable();
    testRejectsMissingFile();
    testMissingUnicodePathIsReportedAsUtf8();
    testRejectsNonWaveFile();
    testRejectsUnsupportedFormat();
    testReadsForwardSmplLoopAndRejectsInvalidFrameBounds();
    testTableAndStreamingDecodeEveryPcm16CodeAlike();
    testReadsEightTwentyFourAndThirtyTwoBitWav();
    testReadsFloatAndExtensibleWav();
    testReadsAiffWithItsInstrument();
    testReadsLittleEndianAndFloatAifc();
    testReadsRiffInstrumentChunk();
    testReadsMarkerNames();
    return 0;
}
