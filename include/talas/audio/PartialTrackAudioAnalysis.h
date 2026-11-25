#pragma once

#include "talas/project/PartialTrackAnalysis.h"

#include <functional>
#include <filesystem>
#include <span>
#include <string>

namespace talas::audio
{
// The Spectral Analysis Tool's settings: the lowest frequency analyzed, the
// response from BestFreq (one bin) to BestTime (four), the analysis level, and
// the kind of file: a straight spectrum has a track at each equally spaced
// band up to half the sample rate, a quasi-harmonic one the harmonics of the
// fundamental it follows. The default picks the strongest peaks above
// max(20 Hz, rate / 2048).
enum class PartialTrackAnalysisKind
{
    StrongestPeaks,
    Straight,
    QuasiHarmonic,
};

struct PartialTrackAudioAnalysisSettings
{
    double lowestFrequencyHz = 0.0;
    std::size_t responseBins = 1;
    double level = 1.0;
    PartialTrackAnalysisKind kind = PartialTrackAnalysisKind::StrongestPeaks;
};

[[nodiscard]] talas::project::PartialTrackAnalysisResult
analyzeMonoAudioPartialTracks(
    std::span<const double> samples,
    double sampleRate,
    const PartialTrackAudioAnalysisSettings& settings,
    talas::project::PartialTrackAnalysisOptions options = {},
    std::function<bool()> shouldCancel = {});

[[nodiscard]] talas::project::PartialTrackAnalysisResult
analyzeMonoAudioPartialTracks(
    std::span<const double> samples,
    double sampleRate,
    talas::project::PartialTrackAnalysisOptions options = {},
    std::function<bool()> shouldCancel = {});

[[nodiscard]] bool publishPartialTrackAssetNoReplace(
    const std::filesystem::path& destination,
    const talas::project::PartialTrackAsset& asset,
    std::string& error);
}
