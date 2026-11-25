#include "talas/audio/CdpProcessToSample.h"

#include "talas/audio/WavReader.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>

namespace talas::audio
{
namespace
{
void put16(std::string& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<char>(value & 0xffu));
    bytes.push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void put32(std::string& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}
}

bool writeFloat32Wav(
    const std::filesystem::path& path,
    std::uint32_t sampleRate,
    std::span<const double> samples,
    std::string& error)
{
    error.clear();
    if (sampleRate == 0)
    {
        error = "a float WAV needs a positive sample rate";
        return false;
    }
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 64) / 4)
    {
        error = "the samples are too many for a RIFF/WAVE file";
        return false;
    }
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 4);
    std::string bytes;
    bytes.reserve(44 + samples.size() * 4);
    bytes += "RIFF";
    put32(bytes, 36 + dataBytes);
    bytes += "WAVEfmt ";
    put32(bytes, 16);
    put16(bytes, 3);
    put16(bytes, 1);
    put32(bytes, sampleRate);
    put32(bytes, sampleRate * 4);
    put16(bytes, 4);
    put16(bytes, 32);
    bytes += "data";
    put32(bytes, dataBytes);
    for (const auto sample : samples)
        put32(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(sample)));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !file.flush())
    {
        error = "the WAV file could not be written: " + talas::project::pathToUtf8(path);
        return false;
    }
    return true;
}

CdpProcessToSampleResult processSamplesThroughCdp(
    const talas::cdp::OfflineSpectralRequest& request,
    std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output)
{
    CdpProcessToSampleResult result;
    std::vector<std::vector<double>> sounds;
    for (const auto& input : inputs)
    {
        auto read = readSampleFile(input);
        if (!read.ok() || read.channels.empty() || read.channels.front().empty())
        {
            result.error = "the sample " + talas::project::pathToUtf8(input.filename()) + " could not be read"
                + (read.ok() ? std::string{": it has no frames"} : ": " + read.error);
            return result;
        }
        if (result.sampleRate != 0 && read.sampleRate != result.sampleRate)
        {
            result.error = "CDP's inputs must share a sample rate";
            return result;
        }
        result.sampleRate = read.sampleRate;
        auto mono = std::move(read.channels.front());
        if (read.channels.size() > 1)
        {
            // CDP's spectral programs take mono files: a stereo sample as
            // the mix of its channels.
            const auto count = static_cast<double>(read.channels.size());
            for (std::size_t frame = 0; frame < mono.size(); ++frame)
            {
                auto sum = mono[frame];
                for (std::size_t channel = 1; channel < read.channels.size(); ++channel)
                    sum += frame < read.channels[channel].size() ? read.channels[channel][frame] : 0.0;
                mono[frame] = sum / count;
            }
        }
        sounds.push_back(std::move(mono));
    }
    if (sounds.empty())
    {
        result.error = "CDP's process needs an input sample";
        return result;
    }
    const auto rendered = talas::cdp::renderOfflineSpectralProcess(
        request, sounds, static_cast<double>(result.sampleRate));
    if (!rendered.ok())
    {
        result.error = rendered.error;
        return result;
    }
    if (rendered.samples.empty())
    {
        result.error = "CDP8's pvoc synth writes no samples for this result (an analysis of 9 windows or fewer)";
        return result;
    }
    if (!writeFloat32Wav(output, result.sampleRate, rendered.samples, result.error))
        return result;
    result.frames = rendered.samples.size();
    return result;
}
}
