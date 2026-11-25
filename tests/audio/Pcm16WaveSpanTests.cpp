#include "talas/audio/Pcm16WaveSpan.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

std::vector<std::uint8_t> pcm16Wave(
    std::uint16_t channels,
    std::uint32_t sampleRate,
    std::initializer_list<std::int16_t> interleaved)
{
    const auto dataBytes = static_cast<std::uint32_t>(
        interleaved.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    appendU32(bytes, 36U + dataBytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    appendU32(bytes, 16U);
    appendU16(bytes, 1U);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * channels * 2U);
    appendU16(bytes, static_cast<std::uint16_t>(channels * 2U));
    appendU16(bytes, 16U);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(bytes, dataBytes);
    for (const auto sample : interleaved)
        appendU16(bytes, static_cast<std::uint16_t>(sample));
    return bytes;
}

void testParsesAndDecodesExactMonoAndStereo()
{
    const auto mono = pcm16Wave(1U, 48'000U, {0, 16'384, -32'768});
    const auto monoLayout = talas::audio::parseStrictPcm16Wave(mono);
    require(monoLayout.ok(), "exact mono PCM16-WAVE should parse");
    require(
        monoLayout.layout.channelCount == 1U
            && monoLayout.layout.sampleRate == 48'000U
            && monoLayout.layout.frameCount == 3U
            && monoLayout.layout.dataOffset == 44U
            && monoLayout.layout.blockAlign == 2U,
        "mono layout should preserve exact media shape");
    std::array<double, 1> monoFrame{};
    require(
        talas::audio::decodePcm16WaveFrame(
            mono, monoLayout.layout, 2U, monoFrame),
        "mono frame should decode within its exact span");
    requireNear(monoFrame[0], -1.0, 1.0e-12, "minimum PCM16 value");

    const auto stereo = pcm16Wave(
        2U, 44'100U, {32'767, -32'768, 16'384, -16'384});
    const auto stereoLayout = talas::audio::parseStrictPcm16Wave(stereo);
    require(stereoLayout.ok(), "exact stereo PCM16-WAVE should parse");
    std::array<double, 2> stereoFrame{};
    require(
        talas::audio::decodePcm16WaveFrame(
            stereo, stereoLayout.layout, 1U, stereoFrame),
        "stereo frame should decode without channel reduction");
    requireNear(
        stereoFrame[0], 16'384.0 / 32'767.0, 1.0e-12,
        "positive stereo sample");
    requireNear(
        stereoFrame[1], -16'384.0 / 32'767.0, 1.0e-12,
        "negative stereo sample uses the writer's full scale");
}

void testRejectsNonExactContainersAndUnsafeDecodeRanges()
{
    auto trailing = pcm16Wave(1U, 48'000U, {0});
    trailing.push_back(0U);
    require(
        !talas::audio::parseStrictPcm16Wave(trailing).ok(),
        "bytes beyond the declared RIFF boundary must reject");

    auto duplicateFormat = pcm16Wave(1U, 48'000U, {0});
    const std::vector<std::uint8_t> formatChunk(
        duplicateFormat.begin() + 12,
        duplicateFormat.begin() + 36);
    duplicateFormat.insert(
        duplicateFormat.begin() + 36,
        formatChunk.begin(),
        formatChunk.end());
    const auto duplicateSize = static_cast<std::uint32_t>(
        duplicateFormat.size() - 8U);
    for (unsigned index = 0; index < 4U; ++index)
    {
        duplicateFormat[4U + index] = static_cast<std::uint8_t>(
            (duplicateSize >> (index * 8U)) & 0xffU);
    }
    require(
        !talas::audio::parseStrictPcm16Wave(duplicateFormat).ok(),
        "duplicate fmt chunks must reject");

    const auto stereo = pcm16Wave(2U, 48'000U, {0, 0});
    const auto layout = talas::audio::parseStrictPcm16Wave(stereo);
    require(layout.ok(), "decode-range fixture should parse");
    std::array<double, 1> wrongChannels{};
    require(
        !talas::audio::decodePcm16WaveFrame(
            stereo, layout.layout, 0U, wrongChannels),
        "decode output must match the exact source channel count");
    std::array<double, 2> output{};
    require(
        !talas::audio::decodePcm16WaveFrame(
            stereo, layout.layout, 1U, output),
        "decode beyond the exact source frame count must reject");
}

void testEnforcesTheClosedSampleRateBound()
{
    const auto wave = pcm16Wave(1U, 192'000U, {0});
    require(
        talas::audio::parseStrictPcm16Wave(wave, 192'000U).ok(),
        "sample rate at the closed bound should parse");
    require(
        !talas::audio::parseStrictPcm16Wave(wave, 191'999U).ok(),
        "sample rate above the caller's closed bound must reject");
    require(
        !talas::audio::parseStrictPcm16Wave(wave, 0U).ok(),
        "zero sample-rate authority must reject");
}
}

int main()
{
    testParsesAndDecodesExactMonoAndStereo();
    testRejectsNonExactContainersAndUnsafeDecodeRanges();
    testEnforcesTheClosedSampleRateBound();
    return 0;
}
