#pragma once

#include "talas/core/SampleKeyRange.h"
#include "talas/core/SampleLoop.h"
#include "talas/core/SampleMarker.h"
#include "talas/core/SamplePitch.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
using WavReadCancellationCheck = std::function<bool()>;

struct WavReadResult
{
    std::uint32_t sampleRate = 0;
    std::vector<std::vector<double>> channels;
    std::optional<talas::core::SampleLoop> forwardLoop;
    std::optional<talas::core::SamplePitch> samplePitch;
    // The notes and velocities an AIFF INST or a WAVE inst chunk gives.
    std::optional<talas::core::SampleKeyRange> keyRange;
    // The named positions of an AIFF MARK chunk or a WAVE cue chunk, in
    // the file's order, those past the last frame left out.
    std::vector<talas::core::SampleMarker> markers;
    std::string error;
    std::string contentHash;
    bool cancelled = false;

    [[nodiscard]] bool ok() const
    {
        return error.empty() && !cancelled;
    }
};

// Canonical PCM16 RIFF/WAVE only, as the WAV writer makes.
[[nodiscard]] WavReadResult readPcm16Wav(
    const std::filesystem::path& path,
    WavReadCancellationCheck shouldCancel = {});

// A sample file imported from another program: RIFF/WAVE of 8-, 16-, 24- or
// 32-bit PCM or 32- or 64-bit float, extensible or not, or AIFF and AIFF-C
// (NONE, sowt, fl32, fl64), with the base pitch, ranges and loop of its smpl,
// inst or INST chunks; an IRCAM/MTU sound file of 8-, 16-, 24- or 32-bit
// integers or 32-bit floats; a Sound Designer I file; or, on a Macintosh, a
// Sound Designer II file, known by its resource fork.
[[nodiscard]] WavReadResult readSampleFile(
    const std::filesystem::path& path,
    WavReadCancellationCheck shouldCancel = {});
}
