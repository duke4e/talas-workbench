#pragma once

#include "talas/dsp/ReAnalysis.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace talas::audio
{
struct ReAnalysisFilesResult
{
    std::string error;
    std::size_t order = 0;
    std::size_t coefficientSets = 0;
    bool excitationWritten = false;
    // The excitation keeps its level unless it would clip in a 16-bit file.
    double excitationScale = 1.0;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// The RE Analysis Tool's files: the RE table at tablePath and, when
// excitationPath is not empty, the EX file that recreates the sound through
// the REResonator. A stereo sound is analyzed as the mix of its channels.
[[nodiscard]] ReAnalysisFilesResult writeReAnalysisFiles(
    std::span<const std::vector<double>> channels,
    double sampleRate,
    const talas::dsp::ReAnalysisSettings& settings,
    const std::filesystem::path& tablePath,
    const std::filesystem::path& excitationPath);
}
