#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The Spectral Analysis tool's steps from a straight spectrum to a harmonic
// one: the sample looped with a silence
// after it; each frame of the straight spectrum judged pitched (a singable
// vowel) or unpitched (consonants, breaths, clicks) by the unpitched
// fader, and the unpitched parts heard alone; the fundamental's frequency
// envelope traced over the pitched frames, guessed, traced again lower or
// higher, on the harmonic named, or drawn by hand; heard as a sine or with
// all its harmonics; and the harmonic spectrum it gives, each track a
// harmonic of the fundamental, with the hints for its pitch.
namespace talas::audio
{
// The sample as the tool plays it, followed by `silenceSeconds`
// of silence (1 second unless another is typed), to loop.
[[nodiscard]] std::vector<double> sampleWithSilence(
    std::span<const double> samples, double sampleRate, double silenceSeconds);

// A straight spectrum frame's pitch: how much of its energy the harmonics
// of one fundamental explain, from 0 (noise) to 1 (a harmonic tone), and
// that fundamental, 0 where the frame has no peak in the range.
struct FramePitch
{
    double pitchedness = 0.0;
    double fundamentalHz = 0.0;
};

// The fundamental of each frame, searched from `lowestHz` (the analysis
// frequency) to `highestHz`, two octaves above it unless given.
[[nodiscard]] std::vector<FramePitch> framePitches(
    const talas::project::PartialTrackAsset& straight, double lowestHz, double highestHz = 0.0);

// The unpitched fader, 0 to 1: raising it takes the most
// pitched frames out first, a frame pitched where its pitchedness is above
// 1 minus the fader. The fader at 0 leaves every frame unpitched.
[[nodiscard]] bool framePitched(const FramePitch& pitch, double fader) noexcept;

// The sample with its pitched frames silenced, so the consonants,
// breaths and clicks the fader leaves are heard alone; each frame lasts to
// the next one's start, its edges ramped over a millisecond.
[[nodiscard]] std::vector<double> unpitchedOnly(
    std::span<const double> samples,
    const talas::project::PartialTrackAsset& straight,
    std::span<const FramePitch> pitches,
    double fader);

// A point of the traced envelope: the traced harmonic's frequency at a
// time. The fundamental is that frequency divided by the traced harmonic.
struct FundamentalPoint
{
    double seconds = 0.0;
    double hz = 0.0;

    bool operator==(const FundamentalPoint&) const = default;
};

// How a trace is guessed. The initial guess takes, in each pitched frame, the
// highest fundamental explaining nearly as much as the best; Auto trace lower
// the lowest, for segments that jumped up to the next partial; Auto trace
// higher the highest, for segments that jumped down.
enum class FundamentalTraceBias
{
    Guess,
    Lower,
    Higher,
};

// The trace over the pitched frames, on the harmonic `tracedHarmonic` (1
// the fundamental itself) at the frequency of the straight spectrum's
// strongest peak near it, the points a line between their neighbours
// already passes within 1% left out.
[[nodiscard]] std::vector<FundamentalPoint> traceFundamental(
    const talas::project::PartialTrackAsset& straight,
    std::span<const FramePitch> pitches,
    double fader,
    double lowestHz,
    double highestHz = 0.0,
    FundamentalTraceBias bias = FundamentalTraceBias::Guess,
    int tracedHarmonic = 1);

// Editing the trace: a point added (Hand trace's
// Shift+Click), moved or deleted, the points kept in time order; nothing
// when the edit does not apply.
[[nodiscard]] std::optional<std::vector<FundamentalPoint>> traceWithPoint(
    std::vector<FundamentalPoint> trace, FundamentalPoint point);
[[nodiscard]] std::optional<std::vector<FundamentalPoint>> traceWithPointMoved(
    std::vector<FundamentalPoint> trace, std::size_t index, FundamentalPoint point);
[[nodiscard]] std::optional<std::vector<FundamentalPoint>> traceWithoutPoint(
    std::vector<FundamentalPoint> trace, std::size_t index);

// The fundamental at a time: the trace's frequency there, linear between
// points and held beyond them, divided by the traced harmonic; 0 for an
// empty trace.
[[nodiscard]] double fundamentalAt(
    std::span<const FundamentalPoint> trace, double seconds, int tracedHarmonic = 1) noexcept;

// Fundamental Only: a sine at the traced fundamental.
[[nodiscard]] std::vector<double> fundamentalSine(
    std::span<const FundamentalPoint> trace,
    int tracedHarmonic,
    double sampleRate,
    std::size_t frames,
    double amplitude = 0.25);

// All Harmonics and the harmonic spectrum file: track k is the k-th harmonic
// of the traced fundamental, in each frame the amplitude and frequency of the
// straight spectrum's strongest peak within half a fundamental of k times it,
// or silent at k times it; harmonics up to half the sample rate, at most
// `maximumHarmonics`.
[[nodiscard]] talas::project::PartialTrackAsset harmonicSpectrum(
    const talas::project::PartialTrackAsset& straight,
    std::span<const FundamentalPoint> trace,
    int tracedHarmonic = 1,
    std::size_t maximumHarmonics = 256);

// The harmonic spectrum as a SumOfSines analysis table, the
// amplitude of each harmonic frame after frame: at most `maximumFrames`
// frames, evenly spaced through the spectrum's, and `maximumPartials`
// harmonics, those a frame lacks silent. Written as numbers, one frame a
// line, a table resource reads.
struct HarmonicAmplitudeTable
{
    std::vector<double> amplitudes;
    std::size_t frames = 0;
    std::size_t partials = 0;
};

[[nodiscard]] HarmonicAmplitudeTable harmonicAmplitudeTable(
    const talas::project::PartialTrackAsset& harmonic,
    std::size_t maximumFrames = 4096,
    std::size_t maximumPartials = 256);
[[nodiscard]] bool writeHarmonicAmplitudeTable(
    const std::filesystem::path& destination, const HarmonicAmplitudeTable& table, std::string& error);

// The hints Create Quasi-Harmonic Spectrum File gives for the
// analysis's pitch, the traced fundamental's starting, ending and average
// frequencies; and middle C, the pitch for speech and sounds of more than
// one pitch.
struct FundamentalSummary
{
    double startHz = 0.0;
    double endHz = 0.0;
    double averageHz = 0.0;
};

inline constexpr double defaultHarmonicAnalysisNoteNumber = 60.0;

[[nodiscard]] std::optional<FundamentalSummary> fundamentalSummary(
    std::span<const FundamentalPoint> trace, int tracedHarmonic = 1);
[[nodiscard]] double noteNumberOfFrequency(double hz) noexcept;
}
