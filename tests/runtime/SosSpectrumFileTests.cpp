#include "talas/audio/PartialTrackAudioAnalysis.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Spectrum files, .spc, as the Sum-of-Sines analysis file format is
// documented: an AIFF, mono and 24 bits a
// sample, a frame's partials in increasing order, each a word of a log
// amplitude byte over a log frequency word, and an APPL 'SOSe' chunk with
// the partials a frame and the frame's duration. Loris (Kelly Fitz's
// SpcFile) writes the same files; its layout is read as well.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void tag(std::vector<std::uint8_t>& bytes, std::string_view text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void u16(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    u16(bytes, value >> 16U);
    u16(bytes, value & 0xFFFFU);
}

void word(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t{bytes[offset]} << 24U) | (std::uint32_t{bytes[offset + 1]} << 16U)
        | (std::uint32_t{bytes[offset + 2]} << 8U) | bytes[offset + 3];
}

std::uint32_t readWord(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t{bytes[offset]} << 16U) | (std::uint32_t{bytes[offset + 1]} << 8U) | bytes[offset + 2];
}

bool tagAt(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view text)
{
    return bytes.size() >= offset + text.size() && std::equal(text.begin(), text.end(), bytes.begin() + offset);
}

// The offset of a chunk's data in an AIFF file.
std::size_t chunkData(std::span<const std::uint8_t> bytes, std::string_view id)
{
    for (std::size_t offset = 12; offset + 8 <= bytes.size();)
    {
        const auto size = readU32(bytes, offset + 4);
        if (tagAt(bytes, offset, id))
            return offset + 8;
        offset += 8 + size + (size & 1U);
    }
    throw std::runtime_error("the file should hold a " + std::string{id} + " chunk");
}

double cents(double frequency, double expected)
{
    return std::abs(1200.0 * std::log2(frequency / expected));
}

double decibels(double amplitude, double expected)
{
    return std::abs(20.0 * std::log10(amplitude / expected));
}

// The AIFF the description lays out, FORM, COMM at 12 and SSND at 38 with
// its samples from 54, then the APPL 'SOSe' chunk: the frames of words, the
// partials a frame and the frame duration in microseconds.
std::vector<std::uint8_t> documentedFile(
    const std::vector<std::uint32_t>& words, std::uint32_t partials, std::uint32_t microseconds)
{
    std::vector<std::uint8_t> bytes;
    tag(bytes, "FORM");
    u32(bytes, 0);
    tag(bytes, "AIFF");
    tag(bytes, "COMM");
    u32(bytes, 18);
    u16(bytes, 1);
    u32(bytes, static_cast<std::uint32_t>(words.size()));
    u16(bytes, 24);
    // 44100 hz as the description gives it.
    for (const std::uint8_t byte : {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0})
        bytes.push_back(byte);
    tag(bytes, "SSND");
    u32(bytes, static_cast<std::uint32_t>(8 + 3 * words.size()));
    u32(bytes, 0);
    u32(bytes, 0);
    for (const auto value : words)
        word(bytes, value);
    if (bytes.size() % 2 != 0)
        bytes.push_back(0);
    tag(bytes, "APPL");
    u32(bytes, partials * 4 + 16);
    tag(bytes, "SOSe");
    u32(bytes, 0);
    u32(bytes, partials);
    for (std::uint32_t index = 0; index < partials; ++index)
        u32(bytes, 0);
    u32(bytes, microseconds);
    const auto form = static_cast<std::uint32_t>(bytes.size() - 8);
    for (std::size_t index = 0; index < 4; ++index)
        bytes[4 + index] = static_cast<std::uint8_t>(form >> (24U - 8U * index));
    return bytes;
}

// The format's worked example: 1000 hz at an amplitude of 0.75, at a
// sample rate of 44100 hz, is 123 (0x7B) over 46038 (0xB3D6): 0x7BB3D6.
void testTheDocumentedWorkedExample()
{
    const auto encoded = talas::project::encodeSosSpectrumPartial(0.75, 1000.0, 44100.0);
    require(encoded == 0x7BB3D6U, "the worked example should encode as 0x7BB3D6, not " + std::to_string(encoded));
    require((encoded >> 16U) == 123U && (encoded & 0xFFFFU) == 46038U,
        "the example's amplitude should be 123 and its frequency 46038");
    // The amplitude byte is clipped to 0 to 127, 0 for silence.
    require((talas::project::encodeSosSpectrumPartial(0.0, 1000.0, 44100.0) >> 16U) == 0
            && (talas::project::encodeSosSpectrumPartial(1.0e-9, 1000.0, 44100.0) >> 16U) == 0
            && (talas::project::encodeSosSpectrumPartial(4.0, 1000.0, 44100.0) >> 16U) == 127,
        "the amplitude byte should be 0 for silence and clipped to 0..127");
    require(talas::project::sosSpectrumPartialAmplitude(0x00B3D6U) == 0.0
            && talas::project::sosSpectrumPartialAmplitude(0x7FB3D6U) == 1.0,
        "a zero byte should be silence and 127 full amplitude");
    // About 90 dB of range in steps of 0.711 dB; up to half the sample rate
    // in steps of 0.275 cents.
    const auto step = 20.0 * std::log10(talas::project::sosSpectrumPartialAmplitude(0x7B0000U)
        / talas::project::sosSpectrumPartialAmplitude(0x7A0000U));
    const auto range = -20.0 * std::log10(talas::project::sosSpectrumPartialAmplitude(0x010000U));
    require(std::abs(step - 0.711) < 0.001 && std::abs(range - 90.0) < 0.5,
        "the amplitude should range about 90 dB in 0.711 dB steps, not " + std::to_string(step) + " and "
            + std::to_string(range));
    const auto frequencyStep = 1200.0 * std::log2(talas::project::sosSpectrumPartialFrequency(46039U, 44100.0)
        / talas::project::sosSpectrumPartialFrequency(46038U, 44100.0));
    require(std::abs(frequencyStep - 0.2747) < 0.0001
            && std::abs(talas::project::sosSpectrumPartialFrequency(65536U - 1, 44100.0) / 22050.0 - 1.0) < 2.0e-4,
        "the frequency should step 0.275 cents up to half the sample rate");
    require(cents(talas::project::sosSpectrumPartialFrequency(encoded, 44100.0), 1000.0) < 0.14
            && decibels(talas::project::sosSpectrumPartialAmplitude(encoded), 0.75) < 0.36,
        "the example should decode within half a step of 1000 hz and 0.75");
}

// A file laid out as described: three partials a frame, four frames of
// 10 ms; the second partial silent in the first frame and the third always.
void testReadingTheDocumentedLayout()
{
    const auto partial = [](double amplitude, double frequency) {
        return talas::project::encodeSosSpectrumPartial(amplitude, frequency, 44100.0);
    };
    std::vector<std::uint32_t> words;
    for (int frame = 0; frame < 4; ++frame)
    {
        words.push_back(partial(0.5, 220.0 + frame));
        words.push_back(frame == 0 ? 0x00A000U : partial(0.25, 440.0));
        words.push_back(0x00C000U);
    }
    const auto file = documentedFile(words, 3, 10000);
    require(tagAt(file, chunkData(file, "COMM") - 8, "COMM") && chunkData(file, "COMM") == 20
            && chunkData(file, "SSND") == 46,
        "the fixture should have COMM at 12 and SSND at 38");
    require(talas::project::isSosSpectrumFile(file), "an AIFF with an APPL 'SOSe' chunk should be a spectrum file");
    const auto decoded = talas::project::decodePartialTrackAsset(file);
    require(decoded.ok(), "the documented file should read: " + (decoded.error ? decoded.error->message : std::string{}));
    const auto& asset = decoded.asset;
    require(asset.sampleRate == 44100.0 && asset.frames.size() == 4 && asset.sourceFrameCount == 1764,
        "the file's rate, frames and duration should read");
    for (std::size_t frame = 0; frame < 4; ++frame)
    {
        const auto& points = asset.frames[frame].points;
        require(asset.frames[frame].sourceFrame == 441 * frame, "frame " + std::to_string(frame) + " should be 10 ms on");
        require(points.size() == (frame == 0 ? 1U : 2U) && points.front().trackId == 1
                && (frame == 0 || points.back().trackId == 2),
            "a silent partial should be no point, the partial its track");
        require(cents(points.front().frequencyHz, 220.0 + static_cast<double>(frame)) < 0.14
                && decibels(points.front().amplitude, 0.5) < 0.36 && points.front().bandwidthHz == 0.0,
            "the first partial should read 220 hz and more at 0.5");
        require(frame == 0 || (cents(points.back().frequencyHz, 440.0) < 0.14 && decibels(points.back().amplitude, 0.25) < 0.36),
            "the second partial should read 440 hz at 0.25");
    }
    // Talas's own file is still read as before; an AIFF without the
    // chunk is no spectrum.
    const talas::project::PartialTrackAsset own{48000.0, 512, {{1, 0, {{4, 0.5, 300.0, 2.0}}}}};
    const auto ownBytes = talas::project::encodePartialTrackAsset(own);
    require(talas::project::decodePartialTrackAsset(ownBytes.bytes).asset == own && !talas::project::isSosSpectrumFile(ownBytes.bytes),
        "a .talaspartial should read as before");
    auto sample = file;
    const auto appl = chunkData(sample, "APPL");
    sample[appl] = 'X';
    require(!talas::project::isSosSpectrumFile(sample) && !talas::project::decodePartialTrackAsset(sample).ok(),
        "an AIFF without the SOSe chunk should be no spectrum");
    auto stereo = file;
    stereo[21] = 2;
    require(!talas::project::decodeSosSpectrumFile(stereo).ok(), "a spectrum file must be mono");
}

// Loris's SpcFile packs a frequency as 65535 / ln 32768 times
// ln(32768 f / 22050 + 1) and an amplitude as the top seven bits of the
// same of the amplitude; it pads a frame to 32 partials, writes an APPL
// chunk with an obsolete table after the frame duration (two samples a
// partial) and INST and MARK chunks, its marker names padded with a zero
// it counts. Read with the documented formulas, as they are played, its
// frequencies are within six cents above 200 hz (its "+ 1" raises the low
// ones) and its amplitudes within a step, its markers at their frames. Its enhanced files keep a noise and phase word after
// each partial's and say so in the ignored word; the partials are read.
std::vector<std::uint8_t> lorisFile(bool enhanced)
{
    const auto envLog = [](double value) {
        return static_cast<std::uint32_t>(65535.0 / std::log(32768.0) * std::log(32768.0 * value + 1.0));
    };
    const auto pack = [&](double amplitude, double frequency) {
        return ((envLog(amplitude) & 0xFE00U) << 7U) | (envLog(frequency / 22050.0) & 0xFFFFU);
    };
    constexpr std::uint32_t partials = 3;
    constexpr std::uint32_t padded = 32;
    constexpr std::uint32_t frames = 3;
    const std::uint32_t values = frames * padded * (enhanced ? 2 : 1);
    std::vector<std::uint8_t> bytes;
    tag(bytes, "FORM");
    u32(bytes, 0);
    tag(bytes, "AIFF");
    tag(bytes, "COMM");
    u32(bytes, 18);
    u16(bytes, 1);
    u32(bytes, values);
    u16(bytes, 24);
    for (const std::uint8_t byte : {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0})
        bytes.push_back(byte);
    tag(bytes, "MARK");
    u32(bytes, 2 + 2 + 4 + 8);
    u16(bytes, 1);
    u16(bytes, 1);
    u32(bytes, 1 * padded * (enhanced ? 2 : 1));
    bytes.push_back(7);
    tag(bytes, "attack");
    bytes.push_back(0);
    tag(bytes, "INST");
    u32(bytes, 20);
    for (const std::uint8_t byte : {60, 0, 0, 127, 1, 127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})
        bytes.push_back(byte);
    tag(bytes, "APPL");
    u32(bytes, 12 + 4 * (256 + 8));
    tag(bytes, "SOSe");
    u32(bytes, enhanced ? 1 : 0);
    const auto valid = partials * (enhanced ? 2 : 1);
    u32(bytes, valid);
    std::vector<std::uint32_t> table(256 + 8, 0);
    table[valid] = static_cast<std::uint32_t>(1.0e6 * 2.0 * padded / 44100.0);
    table[valid + 1] = valid;
    for (const auto value : table)
        u32(bytes, value);
    tag(bytes, "SSND");
    u32(bytes, 8 + 3 * values);
    u32(bytes, 0);
    u32(bytes, 0);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
    {
        for (std::uint32_t slot = 0; slot < padded; ++slot)
        {
            // Loris's padding partials are the first's multiples, silent.
            const auto frequency = 220.0 * (slot + 1);
            const auto amplitude = slot < partials ? 0.8 / (slot + 1) : 0.0;
            word(bytes, slot < partials ? pack(amplitude, frequency) : envLog(frequency / 22050.0) & 0xFFFFU);
            if (enhanced)
                word(bytes, (0x20U << 16U) | 0x4000U);
        }
    }
    const auto form = static_cast<std::uint32_t>(bytes.size() - 8);
    for (std::size_t index = 0; index < 4; ++index)
        bytes[4 + index] = static_cast<std::uint8_t>(form >> (24U - 8U * index));
    return bytes;
}

void testReadingLorisFiles()
{
    for (const auto enhanced : {false, true})
    {
        const auto decoded = talas::project::decodePartialTrackAsset(lorisFile(enhanced));
        const auto which = enhanced ? std::string{"an enhanced"} : std::string{"a sinusoidal"};
        require(decoded.ok(), which + " Loris file should read: " + (decoded.error ? decoded.error->message : std::string{}));
        const auto& asset = decoded.asset;
        require(asset.sampleRate == 44100.0 && asset.frames.size() == 3,
            which + " Loris file's 32 partials a frame should give its three frames");
        for (const auto& frame : asset.frames)
        {
            require(frame.points.size() == 3, which + " Loris file's padding partials should be silent");
            for (std::size_t index = 0; index < 3; ++index)
            {
                const auto& point = frame.points[index];
                const auto number = static_cast<double>(index + 1);
                require(point.trackId == index + 1 && cents(point.frequencyHz, 220.0 * number) < 6.0
                        && decibels(point.amplitude, 0.8 / number) < 0.8,
                    which + " Loris file's partial " + std::to_string(index + 1) + " should read near "
                        + std::to_string(220.0 * number) + " hz, not " + std::to_string(point.frequencyHz));
            }
        }
        // Frames two samples a partial apart (1451 microseconds).
        require(asset.frames[1].sourceFrame == 64 && asset.frames[2].sourceFrame == 128,
            which + " Loris file's frames should be 1451 microseconds apart");
        require(asset.markers.size() == 1 && asset.markers.front().label == "attack"
                && asset.markers.front().sourceFrame == 64 && !asset.loopStartFrame.has_value(),
            which + " Loris file's marker should read at its frame, without the zero it counts");
    }
}

talas::project::PartialTrackAsset writtenFixture()
{
    // Track 7 glides from 440 to 485 hz; track 3 sounds 220 hz for four
    // frames and track 9 880 hz for the last four, a silent frame between.
    talas::project::PartialTrackAsset asset;
    asset.sampleRate = 48000.0;
    asset.sourceFrameCount = 2560;
    for (std::uint64_t frame = 0; frame < 10; ++frame)
    {
        talas::project::PartialTrackFrame written{1, frame * 256, {}};
        if (frame < 4)
            written.points.push_back({3, 0.25, 220.0, 0.0});
        written.points.push_back({7, 0.5, 440.0 + 5.0 * static_cast<double>(frame), 12.0});
        if (frame >= 6)
            written.points.push_back({9, 0.125, 880.0, 0.0});
        asset.frames.push_back(std::move(written));
    }
    asset.markers = {{"onset", 512}};
    asset.loopStartFrame = 256;
    asset.loopEndFrame = 1792;
    return asset;
}

// Written, the frames are a frame duration apart and the tracks laid into
// as few partials as never sound at once, a power of two of at least 32 of
// them, ordered by frequency; the markers go in a MARK chunk and the loop in
// an INST chunk. A file read and written again is the same file.
void testWritingASpectrumFile()
{
    const auto asset = writtenFixture();
    const auto written = talas::project::encodeSpectrumFileFor("voice s512.spc", asset);
    require(written.ok(), "the spectrum should write as an .spc file: " + (written.error ? written.error->message : std::string{}));
    const std::span<const std::uint8_t> bytes{written.bytes};
    require(tagAt(bytes, 0, "FORM") && readU32(bytes, 4) == bytes.size() - 8 && tagAt(bytes, 8, "AIFF")
            && tagAt(bytes, 12, "COMM") && tagAt(bytes, 38, "SSND"),
        "the file should be an AIFF of the documented layout");
    const auto common = chunkData(bytes, "COMM");
    require(bytes[common + 1] == 1 && bytes[common + 7] == 24 && readU32(bytes, common + 2) == 10 * 32
            && bytes[common + 8] == 0x40 && bytes[common + 9] == 0x0E && bytes[common + 10] == 0xBB && bytes[common + 11] == 0x80,
        "the file should be mono, 24 bits, 320 words at 48 kHz");
    const auto appl = chunkData(bytes, "APPL");
    require(readU32(bytes, appl - 4) == 32 * 4 + 16 && tagAt(bytes, appl, "SOSe") && readU32(bytes, appl + 4) == 0
            && readU32(bytes, appl + 8) == 32 && readU32(bytes, appl + 12 + 4 * 32) == 5333,
        "the APPL chunk should give 32 partials and 5333 microseconds");
    const auto samples = chunkData(bytes, "SSND") + 8;
    require(readWord(bytes, samples) == talas::project::encodeSosSpectrumPartial(0.5, 440.0, 48000.0)
            && readWord(bytes, samples + 3) == talas::project::encodeSosSpectrumPartial(0.25, 220.0, 48000.0)
            && readWord(bytes, samples + 6) == 0,
        "the first frame should hold the lower mean frequency first, then silence");
    const auto sixth = samples + 5 * 32 * 3;
    require(readWord(bytes, sixth + 3) == (talas::project::encodeSosSpectrumPartial(0.25, 220.0, 48000.0) & 0xFFFFU),
        "a silent partial should keep its last frequency");

    const auto decoded = talas::project::decodePartialTrackAsset(written.bytes);
    require(decoded.ok(), "the written file should read back");
    const auto& read = decoded.asset;
    require(read.sampleRate == 48000.0 && read.sourceFrameCount == 2560 && read.frames.size() == 10,
        "the written file should keep its rate, extent and frames");
    for (std::size_t frame = 0; frame < 10; ++frame)
    {
        const auto& points = read.frames[frame].points;
        require(read.frames[frame].sourceFrame == 256 * frame, "frame " + std::to_string(frame) + " should keep its time");
        require(points.size() == (frame == 4 || frame == 5 ? 1U : 2U) && points.front().trackId == 1
                && cents(points.front().frequencyHz, 440.0 + 5.0 * static_cast<double>(frame)) < 0.14
                && decibels(points.front().amplitude, 0.5) < 0.36 && points.front().bandwidthHz == 0.0,
            "track 7 should be the first partial, its bandwidth not kept, frame " + std::to_string(frame));
        if (points.size() == 2)
        {
            const auto expected = frame < 4 ? 220.0 : 880.0;
            require(points.back().trackId == 2 && cents(points.back().frequencyHz, expected) < 0.14,
                "tracks 3 and 9 should share the second partial, frame " + std::to_string(frame));
        }
    }
    require(read.markers == std::vector<talas::project::PartialTrackAssetMarker>{{"onset", 512}}
            && read.loopStartFrame == std::optional<std::uint64_t>{256} && read.loopEndFrame == std::optional<std::uint64_t>{1792},
        "the markers and the loop should read back");
    const auto again = talas::project::encodeSosSpectrumFile(read);
    require(again.ok() && again.bytes == written.bytes, "a spectrum file read and written again should be the same file");

    // A .talaspartial name keeps Talas's own file.
    require(talas::project::encodeSpectrumFileFor("voice s512.talaspartial", asset).bytes
                == talas::project::encodePartialTrackAsset(asset).bytes
            && talas::project::isSpectrumFilePath("A.SPC") && talas::project::isSpectrumFilePath("b.talaspartial")
            && talas::project::isSosSpectrumFilePath("c.Spc") && !talas::project::isSosSpectrumFilePath("c.talaspartial")
            && !talas::project::isSpectrumFilePath("voice.aif"),
        "the file's name should choose its format");

    // Frames apart unevenly: the most common spacing, each track's values
    // taken between its frames.
    const talas::project::PartialTrackAsset uneven{48000.0, 1200,
        {{1, 0, {{1, 0.5, 400.0, 0.0}}}, {1, 400, {{1, 0.5, 600.0, 0.0}}}, {1, 800, {{1, 0.5, 600.0, 0.0}}},
            {1, 1000, {{1, 0.5, 600.0, 0.0}}}}};
    const auto resampled = talas::project::decodePartialTrackAsset(talas::project::encodeSosSpectrumFile(uneven).bytes);
    require(resampled.ok() && resampled.asset.frames.size() == 3 && resampled.asset.frames[1].sourceFrame == 400
            && cents(resampled.asset.frames[1].points.front().frequencyHz, 600.0) < 0.14,
        "an unevenly spaced spectrum should be written at its most common spacing");
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Wherever a spectrum is taken it may be an .spc file: the resource loader and
// the asset inspection read an .spc as they read a .talaspartial, the Spectral
// Analysis tool's publisher writes one for an .spc name, and a Partial
// Track Player plays it.
void testSpectraTakenFromSpcFiles()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-sos-spectrum-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    talas::project::PartialTrackAsset tone{48000.0, 9600, {}};
    for (std::uint64_t frame = 0; frame < 20; ++frame)
        tone.frames.push_back({1, frame * 480, {{1, 0.5, 1000.0, 0.0}}});
    const auto published = folder / "tone s512.spc";
    std::string error;
    require(talas::audio::publishPartialTrackAssetNoReplace(published, tone, error), "the analysis should publish: " + error);
    const auto bytes = readFile(published);
    require(talas::project::isSosSpectrumFile(bytes) && bytes == talas::project::encodeSosSpectrumFile(tone).bytes,
        "an .spc name should publish a Sum-of-Sines spectrum file");

    const auto loaded = talas::audio::loadPartialTrackResource(published);
    require(loaded.ok() && loaded.bytes == bytes, "the resource loader should take an .spc: " + loaded.error);
    talas::project::ProjectAsset manifest;
    manifest.id = "tone";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = published.string();
    const auto inspected = talas::project::inspectProjectAsset(manifest, published);
    require(inspected.ok() && inspected.asset.sampleRate == 48000.0 && inspected.asset.frameCount == 9600
            && inspected.asset.contentHash == loaded.contentHash,
        "an .spc asset should inspect as a spectrum: " + (inspected.error ? inspected.error->message : std::string{}));

    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Analysis/Tone";
    snapshot->kind = talas::core::ResourceSnapshotKind::PartialSpectrum;
    snapshot->generation = 1;
    snapshot->contentHash = loaded.contentHash;
    snapshot->sampleRate = inspected.asset.sampleRate;
    snapshot->frameCount = inspected.asset.frameCount;
    snapshot->encoding = inspected.asset.encoding;
    snapshot->partialTrackBytes = bytes;
    talas::runtime::RenderSettings settings{48000.0, 256};
    settings.resourceSnapshots.push_back(snapshot);
    talas::project::ProjectDocument document;
    document.assets.push_back(inspected.asset);
    document.assets.back().id = snapshot->id;
    Parameter partialTrack;
    partialTrack.parameterId = "partialTrack";
    partialTrack.resource = snapshot->id;
    Parameter gain;
    gain.parameterId = "gain";
    gain.expression = "1";
    document.graph.nodes = {{"source", "talas.partialTrackPlayerV1", {partialTrack, gain}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "out", "audioIn"}};
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "a Partial Track Player of an .spc should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto played = talas::runtime::renderOffline(*compiled.graph, 4800);
    require(played.ok(), "a Partial Track Player of an .spc should play");
    const std::span<const double> middle{played.mono.data() + 1200, 2400};
    std::size_t crossings = 0;
    for (std::size_t index = 1; index < middle.size(); ++index)
        crossings += middle[index - 1] < 0.0 && middle[index] >= 0.0 ? 1 : 0;
    const auto loudest = std::ranges::max(middle, {}, [](double sample) { return std::abs(sample); });
    const auto level = talas::project::sosSpectrumPartialAmplitude(talas::project::encodeSosSpectrumPartial(0.5, 1000.0, 48000.0));
    require(crossings >= 49 && crossings <= 51 && std::abs(std::abs(loudest) - level) < 0.01,
        "the .spc's 1000 hz partial should play at its level: " + std::to_string(crossings) + " cycles, "
            + std::to_string(loudest));
    std::filesystem::remove_all(folder);
}

// A spectrum from a spectrum file has
// logarithmic frequencies, one generated in real time linear frequencies.
// A spectrum file's frequency words step alike for each octave, so 220,
// 440, 660 and 880 hz, equally spaced in hertz, are not equally spaced
// there, and reading turns them back into hertz; a LiveSpectralAnalysis's
// frequencies are fractions of half the sample rate, an octave up twice as
// large.
void testSpectrumFilesAreLogarithmicLiveSpectraLinear()
{
    talas::project::PartialTrackAsset harmonics{44100.0, 4410, {}};
    for (std::uint64_t frame = 0; frame < 4; ++frame)
        harmonics.frames.push_back({1, frame * 1024, {{1, 0.5, 220.0, 0.0}, {2, 0.25, 440.0, 0.0}, {3, 0.2, 660.0, 0.0},
                                                         {4, 0.125, 880.0, 0.0}}});
    const auto file = talas::project::encodeSosSpectrumFile(harmonics);
    require(file.ok(), "the harmonics should write");
    const auto samples = chunkData(file.bytes, "SSND") + 8;
    std::vector<double> stored;
    for (std::size_t partial = 0; partial < 4; ++partial)
        stored.push_back(static_cast<double>(readWord(file.bytes, samples + 3 * partial) & 0xFFFFU));
    const auto octave = 65536.0 / 15.0;
    require(std::abs(stored[1] - stored[0] - octave) <= 1.0 && std::abs(stored[3] - stored[1] - octave) <= 1.0
            && std::abs(stored[2] - stored[1] - std::log2(1.5) * octave) <= 1.0
            && std::abs((stored[2] - stored[1]) - (stored[1] - stored[0])) > 1000.0,
        "a spectrum file's frequencies should be logarithmic, an octave a step of 4369");
    const auto read = talas::project::decodePartialTrackAsset(file.bytes);
    require(read.ok(), "the harmonics should read back");
    for (std::size_t partial = 0; partial < 4; ++partial)
    {
        const auto hertz = read.asset.frames.front().points[partial].frequencyHz;
        require(cents(hertz, 220.0 * static_cast<double>(partial + 1)) < 0.14,
            "reading a spectrum file should give its frequencies in hertz, not " + std::to_string(hertz));
    }

    // A LiveSpectralAnalysis following the fundamental of a voice and its
    // second and third harmonics, at 220 and an octave up at 440.
    constexpr double rate = 48000.0;
    const auto fundamental = [&](double pitch) {
        std::vector<double> voice(24000);
        for (std::size_t frame = 0; frame < voice.size(); ++frame)
        {
            const auto time = static_cast<double>(frame) / rate;
            voice[frame] = static_cast<double>(static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * pitch * time)
                + 0.2 * std::sin(4.0 * std::numbers::pi * pitch * time) + 0.1 * std::sin(6.0 * std::numbers::pi * pitch * time)));
        }
        const auto field = [](std::string id, std::string text) {
            Parameter parameter;
            parameter.parameterId = std::move(id);
            parameter.expression = std::move(text);
            return parameter;
        };
        const auto flag = [](std::string id, bool value) {
            Parameter parameter;
            parameter.parameterId = std::move(id);
            parameter.boolean = value;
            return parameter;
        };
        const auto hz = [](double value) { return std::to_string(static_cast<int>(value)) + " hz"; };
        talas::project::ProjectDocument document;
        document.graph.nodes = {{"voice", "talas.audioInput", {}},
            {"analysis", "talas.liveSpectralAnalysis",
                {field("lowestAnalyzedFreq", "25 hz"), flag("harmonic", true), field("trackedHarmonic", "1"),
                    field("lowFreq", hz(pitch * 0.7)), field("highFreq", hz(pitch * 1.4)), field("initFreq", hz(pitch))}},
            {"hold", "talas.partialSpectrumLegacyHoldV1", {}},
            {"selector", "talas.spectrumTrackSelector", {field("track", "1"), flag("amp", false), flag("freq", true)}},
            {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"voice", "audioOut", "analysis", "audioIn"},
            {"analysis", "spectrumOut", "hold", "spectrumIn"}, {"hold", "spectrumOut", "selector", "spectrumIn"},
            {"selector", "leftOut", "out", "audioIn"}};
        talas::runtime::RenderSettings settings{rate, 2048};
        settings.audioInputChannels.push_back({1, std::move(voice), {}});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "the live analysis should compile: "
                + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto rendered = talas::runtime::renderOffline(*compiled.graph, 24000);
        require(rendered.ok(), "the live analysis should render");
        return rendered.mono.back();
    };
    const auto low = fundamental(220.0);
    const auto high = fundamental(440.0);
    require(std::abs(low * rate * 0.5 - 220.0) < 0.5 && std::abs(high * rate * 0.5 - 440.0) < 1.0,
        "a live spectrum's frequency should be linear, a fraction of half the sample rate: "
            + std::to_string(low * rate * 0.5) + " and " + std::to_string(high * rate * 0.5));
    require(std::abs(high / low - 2.0) < 0.005,
        "an octave up should double a live spectrum's frequency, not add to it: " + std::to_string(high / low));
}
}

int main()
{
    try
    {
        testTheDocumentedWorkedExample();
        testReadingTheDocumentedLayout();
        testReadingLorisFiles();
        testWritingASpectrumFile();
        testSpectraTakenFromSpcFiles();
        testSpectrumFilesAreLogarithmicLiveSpectraLinear();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Spectrum file tests passed\n";
    return 0;
}
