#include "talas/audio/ReAnalysisFiles.h"

#include "talas/audio/WavWriter.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace talas::audio
{
ReAnalysisFilesResult writeReAnalysisFiles(
    std::span<const std::vector<double>> channels,
    double sampleRate,
    const talas::dsp::ReAnalysisSettings& settings,
    const std::filesystem::path& tablePath,
    const std::filesystem::path& excitationPath)
{
    ReAnalysisFilesResult result;
    if (channels.empty())
    {
        result.error = "the sample has no channels";
        return result;
    }
    auto sound = channels.front();
    if (channels.size() == 2)
    {
        for (std::size_t frame = 0; frame < sound.size() && frame < channels[1].size(); ++frame)
            sound[frame] = 0.5 * (sound[frame] + channels[1][frame]);
    }
    const auto analysis = talas::dsp::analyzeRe(
        sound, sampleRate, settings, !excitationPath.empty());
    if (!analysis.ok())
    {
        result.error = analysis.error;
        return result;
    }
    {
        std::ofstream table(tablePath, std::ios::binary | std::ios::trunc);
        table << talas::dsp::formatReTable(analysis.table);
        table.close();
        if (!table)
        {
            result.error = "RE analysis could not write " + tablePath.string();
            return result;
        }
    }
    result.order = static_cast<std::size_t>(analysis.table.front());
    result.coefficientSets = (analysis.table.size() - 1) / result.order;
    if (excitationPath.empty())
        return result;

    auto excitation = analysis.excitation;
    double peak = 0.0;
    for (const auto value : excitation)
        peak = std::max(peak, std::abs(value));
    result.excitationScale = peak > 1.0 ? 1.0 / peak : 1.0;
    for (auto& value : excitation)
        value *= result.excitationScale;
    std::string writeError;
    if (!writePcm16Wav(
            excitationPath,
            sampleRate,
            std::span<const std::vector<double>>{&excitation, 1},
            writeError))
    {
        result.error = "RE analysis could not write " + excitationPath.string() + ": " + writeError;
        return result;
    }
    result.excitationWritten = true;
    return result;
}
}
