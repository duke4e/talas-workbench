#pragma once

#include "talas/audio/WavWriter.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace talas::test
{
inline bool writeMonoWavWithSmpl(
    const std::filesystem::path& path,
    double sampleRate,
    const std::vector<double>& samples,
    std::uint32_t startFrame,
    std::uint32_t inclusiveEndFrame,
    std::string& error,
    std::uint32_t loopType = 0,
    std::uint32_t midiUnityNote = 0,
    std::uint32_t midiPitchFraction = 0)
{
    const std::array<std::vector<double>, 1> channels{samples};
    if (!talas::audio::writePcm16Wav(
            path, sampleRate, channels, error))
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "failed to reopen WAV fixture";
        return false;
    }
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    input.close();
    if (bytes.size() < 12
        || bytes.size() > std::numeric_limits<std::uint32_t>::max() - 68U)
    {
        error = "invalid WAV fixture size";
        return false;
    }
    const auto appendU32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    bytes.insert(bytes.end(), {'s', 'm', 'p', 'l'});
    appendU32(60U);
    // Nine DWORD sampler fields, followed by one 24-byte loop record.
    for (int index = 0; index < 7; ++index)
        appendU32(index == 3 ? midiUnityNote
            : index == 4 ? midiPitchFraction : 0U);
    appendU32(1U);
    appendU32(0U);
    appendU32(0U);
    appendU32(loopType);
    appendU32(startFrame);
    appendU32(inclusiveEndFrame);
    appendU32(0U);
    appendU32(0U);
    const auto riffSize = static_cast<std::uint32_t>(bytes.size() - 8U);
    for (int shift = 0; shift < 32; shift += 8)
        bytes[4U + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>(riffSize >> shift);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
    {
        error = "failed to write WAV smpl fixture";
        return false;
    }
    return true;
}
}
