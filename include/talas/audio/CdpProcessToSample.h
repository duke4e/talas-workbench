#pragma once

#include "talas/cdp/SpectralWholeFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace talas::audio
{
// CDP8's spectral processes that change a sound's length or order, as an
// offline "process to sample" (ADR-0018 amendment, 2026-10-19): samples go
// through `pvoc anal`, the CDP program over their whole analyses and `pvoc
// synth` (talas::cdp::renderOfflineSpectralProcess), and the result is a new
// sample a project can play.
struct CdpProcessToSampleResult
{
    std::string error;
    std::uint32_t sampleRate = 0;
    std::size_t frames = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Reads each input (any sample file readSampleFile reads; a stereo one as
// the mix of its channels, CDP's spectral programs taking mono files, so
// that only a mono input matches CDP8 sample for sample), runs the request
// and writes CDP's output as a mono 32-bit float WAV at the inputs' sample
// rate, the floats CDP8 writes. Refused, with nothing written: an input
// that cannot be read, inputs at different sample rates, what the CDP
// process refuses, an output of no samples (pvoc synth writes none for a
// file of 9 windows or fewer, e.g. spec grab's one window), or a file that
// cannot be written.
[[nodiscard]] CdpProcessToSampleResult processSamplesThroughCdp(
    const talas::cdp::OfflineSpectralRequest& request,
    std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output);

// The samples as a mono 32-bit IEEE float RIFF/WAVE file.
[[nodiscard]] bool writeFloat32Wav(
    const std::filesystem::path& path,
    std::uint32_t sampleRate,
    std::span<const double> samples,
    std::string& error);
}
