#include "talas/audio/QuasiHarmonicAnalysis.h"

#include "talas/audio/AudioFilePublication.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <utility>

namespace talas::audio
{
namespace
{
// The peaks that judge a frame's pitch lie below this, where a voice's
// harmonics are, and above nothing: energy below the fundamental counts
// against it.
constexpr double pitchMeasureCeilingHz = 5000.0;
// A peak belongs to a harmonic within this fraction of the fundamental.
constexpr double harmonicTolerance = 0.15;
// The strongest peaks a frame's candidate fundamentals come from, each
// divided by 1 to this.
constexpr std::size_t candidatePeaks = 8;
constexpr int candidateDivisors = 6;
// The initial guess takes the highest fundamental explaining this share of
// what the best one does.
constexpr double guessShare = 0.95;
// Candidates this close in score are tied.
constexpr double scoreTie = 0.02;

struct Candidate
{
    double hz = 0.0;
    double score = 0.0;
};

[[nodiscard]] double highestFundamental(double lowestHz, double highestHz) noexcept
{
    return highestHz > 0.0 ? highestHz : lowestHz * 4.0;
}

[[nodiscard]] std::vector<talas::project::PartialTrackPoint> measuredPeaks(
    const talas::project::PartialTrackFrame& frame, double sampleRate)
{
    const auto ceiling = std::min(sampleRate * 0.5, pitchMeasureCeilingHz);
    std::vector<talas::project::PartialTrackPoint> peaks;
    for (const auto& point : frame.points)
    {
        if (point.amplitude > 0.0 && point.frequencyHz > 0.0 && point.frequencyHz <= ceiling)
            peaks.push_back(point);
    }
    return peaks;
}

[[nodiscard]] double score(std::span<const talas::project::PartialTrackPoint> peaks, double energy, double hz) noexcept
{
    double explained = 0.0;
    for (const auto& peak : peaks)
    {
        const auto harmonic = std::round(peak.frequencyHz / hz);
        if (harmonic >= 1.0 && std::abs(peak.frequencyHz - harmonic * hz) <= harmonicTolerance * hz)
            explained += peak.amplitude * peak.amplitude;
    }
    return energy > 0.0 ? explained / energy : 0.0;
}

// Each fundamental in the range that one of the strongest peaks is a
// harmonic of, with the share of the frame's energy its harmonics hold.
[[nodiscard]] std::vector<Candidate> candidates(
    const talas::project::PartialTrackFrame& frame, double sampleRate, double lowestHz, double highestHz)
{
    auto peaks = measuredPeaks(frame, sampleRate);
    double energy = 0.0;
    for (const auto& peak : peaks)
        energy += peak.amplitude * peak.amplitude;
    std::vector<Candidate> found;
    if (!(energy > 0.0))
        return found;
    auto strongest = peaks;
    std::ranges::sort(strongest, [](const auto& a, const auto& b) { return a.amplitude > b.amplitude; });
    strongest.resize(std::min(strongest.size(), candidatePeaks));
    for (const auto& peak : strongest)
    {
        for (int divisor = 1; divisor <= candidateDivisors; ++divisor)
        {
            const auto hz = peak.frequencyHz / divisor;
            if (hz < lowestHz || hz > highestHz)
                continue;
            if (std::ranges::any_of(found, [hz](const Candidate& seen) { return std::abs(seen.hz - hz) < hz * 0.01; }))
                continue;
            found.push_back({hz, score(peaks, energy, hz)});
        }
    }
    return found;
}

[[nodiscard]] std::optional<Candidate> guess(std::span<const Candidate> found)
{
    if (found.empty())
        return std::nullopt;
    const auto best = std::ranges::max(found, {}, &Candidate::score).score;
    std::optional<Candidate> chosen;
    for (const auto& candidate : found)
    {
        if (candidate.score >= best * guessShare && (!chosen || candidate.hz > chosen->hz))
            chosen = candidate;
    }
    return chosen;
}

// Below the guess, the best explanation (ties to the highest); above it,
// the best (ties to the lowest).
[[nodiscard]] std::optional<Candidate> biased(std::span<const Candidate> found, FundamentalTraceBias bias)
{
    const auto initial = guess(found);
    if (!initial || bias == FundamentalTraceBias::Guess)
        return initial;
    const auto lower = bias == FundamentalTraceBias::Lower;
    std::vector<Candidate> side;
    for (const auto& candidate : found)
    {
        if (lower ? candidate.hz < initial->hz * 0.97 : candidate.hz > initial->hz * 1.03)
            side.push_back(candidate);
    }
    if (side.empty())
        return initial;
    const auto best = std::ranges::max(side, {}, &Candidate::score).score;
    std::optional<Candidate> chosen;
    for (const auto& candidate : side)
    {
        if (candidate.score < best - scoreTie)
            continue;
        if (!chosen || (lower ? candidate.hz > chosen->hz : candidate.hz < chosen->hz))
            chosen = candidate;
    }
    return chosen;
}

[[nodiscard]] const talas::project::PartialTrackPoint* strongestNear(
    const talas::project::PartialTrackFrame& frame, double centreHz, double halfWidthHz) noexcept
{
    const talas::project::PartialTrackPoint* strongest = nullptr;
    for (const auto& point : frame.points)
    {
        if (point.amplitude > 0.0 && std::abs(point.frequencyHz - centreHz) < halfWidthHz
            && (strongest == nullptr || point.amplitude > strongest->amplitude))
            strongest = &point;
    }
    return strongest;
}

[[nodiscard]] double frameSeconds(const talas::project::PartialTrackAsset& asset, std::size_t index) noexcept
{
    return asset.sampleRate > 0.0 ? static_cast<double>(asset.frames[index].sourceFrame) / asset.sampleRate : 0.0;
}

// A run of points thinned: the first and last kept, and each one a line
// from the last kept to the next misses by more than 1%.
void appendThinned(std::vector<FundamentalPoint>& trace, const std::vector<FundamentalPoint>& run)
{
    if (run.empty())
        return;
    trace.push_back(run.front());
    for (std::size_t index = 1; index + 1 < run.size(); ++index)
    {
        const auto& kept = trace.back();
        const auto& next = run[index + 1];
        const auto& point = run[index];
        const auto span = next.seconds - kept.seconds;
        const auto line = span > 0.0 ? kept.hz + (next.hz - kept.hz) * (point.seconds - kept.seconds) / span : kept.hz;
        if (std::abs(line - point.hz) > point.hz * 0.01)
            trace.push_back(point);
    }
    if (run.size() > 1)
        trace.push_back(run.back());
}

[[nodiscard]] bool validPoint(const FundamentalPoint& point) noexcept
{
    return std::isfinite(point.seconds) && point.seconds >= 0.0 && std::isfinite(point.hz) && point.hz > 0.0;
}

[[nodiscard]] bool hasTime(const std::vector<FundamentalPoint>& trace, double seconds, std::size_t except)
{
    for (std::size_t index = 0; index < trace.size(); ++index)
    {
        if (index != except && trace[index].seconds == seconds)
            return true;
    }
    return false;
}
}

std::vector<double> sampleWithSilence(std::span<const double> samples, double sampleRate, double silenceSeconds)
{
    std::vector<double> looped(samples.begin(), samples.end());
    if (std::isfinite(sampleRate) && sampleRate > 0.0 && std::isfinite(silenceSeconds) && silenceSeconds > 0.0)
        looped.resize(looped.size() + static_cast<std::size_t>(std::llround(silenceSeconds * sampleRate)), 0.0);
    return looped;
}

std::vector<FramePitch> framePitches(
    const talas::project::PartialTrackAsset& straight, double lowestHz, double highestHz)
{
    const auto highest = highestFundamental(lowestHz, highestHz);
    std::vector<FramePitch> pitches;
    pitches.reserve(straight.frames.size());
    for (const auto& frame : straight.frames)
    {
        const auto found = candidates(frame, straight.sampleRate, lowestHz, highest);
        const auto chosen = guess(found);
        if (!chosen)
        {
            pitches.push_back({});
            continue;
        }
        const auto best = std::ranges::max(found, {}, &Candidate::score).score;
        pitches.push_back({std::clamp(best, 0.0, 1.0), chosen->hz});
    }
    return pitches;
}

bool framePitched(const FramePitch& pitch, double fader) noexcept
{
    return fader > 0.0 && pitch.fundamentalHz > 0.0 && pitch.pitchedness > 1.0 - std::min(fader, 1.0);
}

std::vector<double> unpitchedOnly(
    std::span<const double> samples,
    const talas::project::PartialTrackAsset& straight,
    std::span<const FramePitch> pitches,
    double fader)
{
    std::vector<double> gains(samples.size(), 1.0);
    for (std::size_t index = 0; index < straight.frames.size() && index < pitches.size(); ++index)
    {
        if (!framePitched(pitches[index], fader))
            continue;
        const auto begin = std::min<std::size_t>(straight.frames[index].sourceFrame, samples.size());
        const auto end = index + 1 < straight.frames.size()
            ? std::min<std::size_t>(straight.frames[index + 1].sourceFrame, samples.size())
            : samples.size();
        std::fill(gains.begin() + static_cast<std::ptrdiff_t>(begin),
            gains.begin() + static_cast<std::ptrdiff_t>(std::max(begin, end)), 0.0);
    }
    const auto ramp = straight.sampleRate > 0.0 ? std::max(1.0, straight.sampleRate * 0.001) : 1.0;
    std::vector<double> heard(samples.size(), 0.0);
    auto gain = gains.empty() ? 1.0 : gains.front();
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
    {
        gain += std::clamp(gains[frame] - gain, -1.0 / ramp, 1.0 / ramp);
        heard[frame] = samples[frame] * gain;
    }
    return heard;
}

std::vector<FundamentalPoint> traceFundamental(
    const talas::project::PartialTrackAsset& straight,
    std::span<const FramePitch> pitches,
    double fader,
    double lowestHz,
    double highestHz,
    FundamentalTraceBias bias,
    int tracedHarmonic)
{
    const auto highest = highestFundamental(lowestHz, highestHz);
    const auto harmonic = static_cast<double>(std::max(1, tracedHarmonic));
    std::vector<FundamentalPoint> trace;
    std::vector<FundamentalPoint> run;
    for (std::size_t index = 0; index < straight.frames.size(); ++index)
    {
        const auto& frame = straight.frames[index];
        std::optional<Candidate> chosen;
        if (index < pitches.size() && framePitched(pitches[index], fader))
            chosen = biased(candidates(frame, straight.sampleRate, lowestHz, highest), bias);
        if (!chosen)
        {
            appendThinned(trace, run);
            run.clear();
            continue;
        }
        const auto centre = chosen->hz * harmonic;
        const auto* peak = strongestNear(frame, centre, chosen->hz * 0.5);
        run.push_back({frameSeconds(straight, index), peak != nullptr ? peak->frequencyHz : centre});
    }
    appendThinned(trace, run);
    return trace;
}

std::optional<std::vector<FundamentalPoint>> traceWithPoint(std::vector<FundamentalPoint> trace, FundamentalPoint point)
{
    if (!validPoint(point) || hasTime(trace, point.seconds, trace.size()))
        return std::nullopt;
    const auto at = std::ranges::upper_bound(trace, point.seconds, {}, &FundamentalPoint::seconds);
    trace.insert(at, point);
    return trace;
}

std::optional<std::vector<FundamentalPoint>> traceWithPointMoved(
    std::vector<FundamentalPoint> trace, std::size_t index, FundamentalPoint point)
{
    if (index >= trace.size() || !validPoint(point) || hasTime(trace, point.seconds, index))
        return std::nullopt;
    trace.erase(trace.begin() + static_cast<std::ptrdiff_t>(index));
    return traceWithPoint(std::move(trace), point);
}

std::optional<std::vector<FundamentalPoint>> traceWithoutPoint(std::vector<FundamentalPoint> trace, std::size_t index)
{
    if (index >= trace.size())
        return std::nullopt;
    trace.erase(trace.begin() + static_cast<std::ptrdiff_t>(index));
    return trace;
}

double fundamentalAt(std::span<const FundamentalPoint> trace, double seconds, int tracedHarmonic) noexcept
{
    if (trace.empty())
        return 0.0;
    const auto harmonic = static_cast<double>(std::max(1, tracedHarmonic));
    if (seconds <= trace.front().seconds)
        return trace.front().hz / harmonic;
    if (seconds >= trace.back().seconds)
        return trace.back().hz / harmonic;
    const auto after = std::ranges::upper_bound(trace, seconds, {}, &FundamentalPoint::seconds);
    const auto& next = *after;
    const auto& previous = *(after - 1);
    const auto span = next.seconds - previous.seconds;
    const auto hz = span > 0.0 ? previous.hz + (next.hz - previous.hz) * (seconds - previous.seconds) / span : next.hz;
    return hz / harmonic;
}

std::vector<double> fundamentalSine(
    std::span<const FundamentalPoint> trace, int tracedHarmonic, double sampleRate, std::size_t frames, double amplitude)
{
    std::vector<double> sine(frames, 0.0);
    if (trace.empty() || !(sampleRate > 0.0))
        return sine;
    double phase = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        sine[frame] = amplitude * std::sin(phase);
        const auto hz = fundamentalAt(trace, static_cast<double>(frame) / sampleRate, tracedHarmonic);
        phase = std::fmod(phase + 2.0 * std::numbers::pi * hz / sampleRate, 2.0 * std::numbers::pi);
    }
    return sine;
}

talas::project::PartialTrackAsset harmonicSpectrum(
    const talas::project::PartialTrackAsset& straight,
    std::span<const FundamentalPoint> trace,
    int tracedHarmonic,
    std::size_t maximumHarmonics)
{
    talas::project::PartialTrackAsset harmonic;
    harmonic.sampleRate = straight.sampleRate;
    harmonic.sourceFrameCount = straight.sourceFrameCount;
    const auto nyquist = straight.sampleRate * 0.5;
    for (std::size_t index = 0; index < straight.frames.size(); ++index)
    {
        const auto& frame = straight.frames[index];
        talas::project::PartialTrackFrame out{frame.segmentId, frame.sourceFrame, {}};
        const auto fundamental = fundamentalAt(trace, frameSeconds(straight, index), tracedHarmonic);
        if (fundamental > 0.0)
        {
            const auto count = std::min<std::size_t>(
                maximumHarmonics, static_cast<std::size_t>(std::floor(nyquist / fundamental)));
            for (std::size_t k = 1; k <= count; ++k)
            {
                const auto centre = fundamental * static_cast<double>(k);
                const auto* peak = strongestNear(frame, centre, fundamental * 0.5);
                if (peak != nullptr && peak->frequencyHz <= nyquist)
                    out.points.push_back({k, peak->amplitude, peak->frequencyHz, peak->bandwidthHz});
                else
                    out.points.push_back({k, 0.0, centre, 0.0});
            }
        }
        harmonic.frames.push_back(std::move(out));
    }
    return harmonic;
}

HarmonicAmplitudeTable harmonicAmplitudeTable(
    const talas::project::PartialTrackAsset& harmonic, std::size_t maximumFrames, std::size_t maximumPartials)
{
    HarmonicAmplitudeTable table;
    if (harmonic.frames.empty() || maximumFrames == 0 || maximumPartials == 0)
        return table;
    for (const auto& frame : harmonic.frames)
    {
        for (const auto& point : frame.points)
            table.partials = std::max<std::size_t>(table.partials, std::min<std::uint64_t>(point.trackId, maximumPartials));
    }
    if (table.partials == 0)
        return table;
    table.frames = std::min(maximumFrames, harmonic.frames.size());
    table.amplitudes.assign(table.frames * table.partials, 0.0);
    for (std::size_t row = 0; row < table.frames; ++row)
    {
        const auto source = table.frames == 1
            ? 0U
            : row * (harmonic.frames.size() - 1) / (table.frames - 1);
        for (const auto& point : harmonic.frames[source].points)
        {
            if (point.trackId >= 1 && point.trackId <= table.partials)
                table.amplitudes[row * table.partials + point.trackId - 1] = point.amplitude;
        }
    }
    return table;
}

bool writeHarmonicAmplitudeTable(
    const std::filesystem::path& destination, const HarmonicAmplitudeTable& table, std::string& error)
{
    if (table.frames == 0 || table.partials == 0 || table.amplitudes.size() != table.frames * table.partials)
    {
        error = "The harmonic spectrum has no amplitudes to write";
        return false;
    }
    auto publication = AudioFilePublication::prepare(destination, ExistingAudioFilePolicy::Replace, error);
    if (publication == nullptr)
        return false;
    {
        std::ofstream output(publication->temporaryPath(), std::ios::binary | std::ios::trunc);
        output << std::setprecision(9);
        for (std::size_t row = 0; row < table.frames; ++row)
        {
            for (std::size_t partial = 0; partial < table.partials; ++partial)
                output << (partial == 0 ? "" : " ") << table.amplitudes[row * table.partials + partial];
            output << '\n';
        }
        output.flush();
        if (!output.good())
        {
            error = "The harmonic spectrum table could not be written";
            std::string ignored;
            (void) publication->discard(ignored);
            return false;
        }
    }
    return publication->publish(error);
}

std::optional<FundamentalSummary> fundamentalSummary(std::span<const FundamentalPoint> trace, int tracedHarmonic)
{
    if (trace.empty())
        return std::nullopt;
    const auto harmonic = static_cast<double>(std::max(1, tracedHarmonic));
    FundamentalSummary summary{trace.front().hz / harmonic, trace.back().hz / harmonic, 0.0};
    const auto duration = trace.back().seconds - trace.front().seconds;
    if (!(duration > 0.0))
    {
        double sum = 0.0;
        for (const auto& point : trace)
            sum += point.hz;
        summary.averageHz = sum / static_cast<double>(trace.size()) / harmonic;
        return summary;
    }
    double area = 0.0;
    for (std::size_t index = 1; index < trace.size(); ++index)
        area += (trace[index].seconds - trace[index - 1].seconds) * (trace[index].hz + trace[index - 1].hz) * 0.5;
    summary.averageHz = area / duration / harmonic;
    return summary;
}

double noteNumberOfFrequency(double hz) noexcept
{
    return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0;
}
}
