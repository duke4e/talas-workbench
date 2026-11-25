#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// A spectrum file as SpectrumInRAM's analysis: frames of (amplitude,
// frequency, bandwidth) triples, one after another, a slot for each track in
// the order of their numbers, the frequencies and bandwidths in hertz for an
// AnalysisFrequency of 1 hz. At most `maximumFrames` frames, evenly spaced
// through the spectrum's, and `maximumTriples` triples in all; a track silent
// in a frame keeps its frequency there. Written as numbers, a frame a line,
// the way a table resource reads them.
namespace talas::audio
{
struct SpectrumInRamTable
{
    std::vector<double> triples;
    std::size_t frames = 0;
    std::size_t partials = 0;
    double durationSeconds = 0.0;
};

[[nodiscard]] SpectrumInRamTable spectrumInRamTable(
    const talas::project::PartialTrackAsset& spectrum,
    std::size_t maximumFrames = 4096,
    std::size_t maximumTriples = std::size_t{1} << 18U);
[[nodiscard]] bool writeSpectrumInRamTable(
    const std::filesystem::path& destination, const SpectrumInRamTable& table, std::string& error);
}
