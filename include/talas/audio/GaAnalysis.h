#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::audio
{
// The GA Analysis tool (ADR-0027): a sample becomes a GA analysis, the complex
// waveforms, amplitude envelopes and frequency deviation envelope a
// GAOscillators plays, as our GA table (talas/dsp/GaTable.h). The tool
// analyses the sample's harmonics itself: its pitch over the whole sound
// (CDP's getpitch), the amplitude of each harmonic of that pitch in each
// frame, then the harmonics whose amplitude envelopes are alike grouped into
// waveforms (the grouping clusters the envelopes, each normalised to its peak,
// by k-means, and fits each harmonic's share of its group by least squares).
struct GaAnalysisSettings
{
    // The defaults: 3 waveforms, 124 harmonics.
    std::size_t waveforms = 3;
    std::size_t harmonics = 124;
    // Envelope frames a second.
    double framesPerSecond = 100.0;
};

struct GaAnalysisResult
{
    // The GA table: header, waveforms, amplitude envelopes, deviation.
    std::vector<double> table;
    double basePitch = 0.0;
    std::size_t envelopeLength = 0;
    std::size_t harmonics = 0;
    // The sound's duration the envelopes cover, from the first frame to the
    // last.
    double durationSeconds = 0.0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Refused: no samples or a non-finite one, settings outside 1 to 64
// waveforms, 1 to 1024 harmonics or 1 to 1000 frames a second, or a sample
// in which getpitch finds no pitch.
[[nodiscard]] GaAnalysisResult analyzeGa(
    std::span<const double> samples,
    double sampleRate,
    const GaAnalysisSettings& settings);

// The analysis of a
// quasi-harmonic spectrum file, track k its k-th harmonic: each harmonic's
// amplitude read from the spectrum at the frames a second asked for, linear
// between its frames; the fundamental in each frame the harmonics' frequencies
// over their numbers, weighted by their amplitudes; the base pitch the median
// fundamental and the deviation the fundamental over it. Refused: a spectrum
// without sounding harmonics.
// The analysis of a spectrum goes in two stages, reading the
// envelopes from the SOS analysis, then determining the waveforms and their
// envelopes; `stage` is told as each begins.
enum class GaAnalysisStage : std::uint8_t
{
    ReadingEnvelopes,
    DeterminingWaveforms,
};
[[nodiscard]] std::string gaAnalysisStageText(GaAnalysisStage stage);
[[nodiscard]] GaAnalysisResult analyzeGaFromSpectrum(
    const talas::project::PartialTrackAsset& harmonic,
    const GaAnalysisSettings& settings,
    const std::function<void(GaAnalysisStage)>& stage = {});
[[nodiscard]] GaAnalysisResult writeGaAnalysisFileFromSpectrum(
    const talas::project::PartialTrackAsset& harmonic,
    const GaAnalysisSettings& settings,
    const std::filesystem::path& path,
    const std::function<void(GaAnalysisStage)>& stage = {});

// The analysis of a sample's channels (a stereo one as their mix) written as
// a 32-bit float WAV, the GA table's numbers as its samples.
[[nodiscard]] GaAnalysisResult writeGaAnalysisFile(
    std::span<const std::vector<double>> channels,
    double sampleRate,
    const GaAnalysisSettings& settings,
    const std::filesystem::path& path);
}
