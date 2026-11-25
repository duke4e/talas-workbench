#include "talas/audio/GaAnalysis.h"

#include "talas/audio/CdpProcessToSample.h"
#include "talas/cdp/PhaseVocoder.h"
#include "talas/cdp/PitchTrack.h"
#include "talas/dsp/GaTable.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace talas::audio
{
namespace
{
// getpitch's analysis: 1024 points, overlap 3, so a window every 128
// frames.
constexpr int pitchPoints = 1024;
constexpr int pitchOverlap = 3;

[[nodiscard]] double median(std::vector<double> values)
{
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : 0.5 * (values[middle - 1] + values[middle]);
}
}

// Harmonic amplitude envelopes, each harmonic's in every frame, and the
// shared frequency deviation as a GA table: the harmonics whose envelopes
// are alike grouped into waveforms.
[[nodiscard]] static GaAnalysisResult groupedGaTable(const std::vector<std::vector<double>>& amplitudes,
    const std::vector<double>& deviation, double basePitch, const GaAnalysisSettings& settings)
{
    GaAnalysisResult result;
    const auto harmonics = amplitudes.size();
    const auto frames = deviation.size();
    // Harmonics whose envelopes, each normalised to its peak, are alike are
    // grouped into a waveform: k-means from groups of harmonics in order of
    // when their energy falls.
    const auto groups = settings.waveforms;
    std::vector<std::vector<double>> normalised(harmonics, std::vector<double>(frames, 0.0));
    std::vector<double> centroid(harmonics, 0.0);
    for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
    {
        const auto peak = *std::ranges::max_element(amplitudes[harmonic]);
        double weight = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            normalised[harmonic][frame] = peak > 0.0 ? amplitudes[harmonic][frame] / peak : 0.0;
            weight += normalised[harmonic][frame];
            centroid[harmonic] += normalised[harmonic][frame] * static_cast<double>(frame);
        }
        centroid[harmonic] = weight > 0.0 ? centroid[harmonic] / weight : 0.0;
    }
    std::vector<std::size_t> order(harmonics);
    for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        order[harmonic] = harmonic;
    std::ranges::stable_sort(order, {}, [&](std::size_t harmonic) { return centroid[harmonic]; });
    std::vector<std::size_t> group(harmonics, 0);
    for (std::size_t rank = 0; rank < harmonics; ++rank)
        group[order[rank]] = rank * groups / harmonics;
    std::vector<std::vector<double>> means(groups, std::vector<double>(frames, 0.0));
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        for (auto& mean : means)
            std::ranges::fill(mean, 0.0);
        std::vector<double> counts(groups, 0.0);
        for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        {
            counts[group[harmonic]] += 1.0;
            for (std::size_t frame = 0; frame < frames; ++frame)
                means[group[harmonic]][frame] += normalised[harmonic][frame];
        }
        for (std::size_t index = 0; index < groups; ++index)
        {
            if (counts[index] > 0.0)
            {
                for (auto& value : means[index])
                    value /= counts[index];
            }
        }
        auto moved = false;
        for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        {
            auto best = group[harmonic];
            double bestDistance = -1.0;
            for (std::size_t index = 0; index < groups; ++index)
            {
                if (counts[index] == 0.0 && index != group[harmonic])
                    continue;
                double distance = 0.0;
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    const auto difference = normalised[harmonic][frame] - means[index][frame];
                    distance += difference * difference;
                }
                if (bestDistance < 0.0 || distance < bestDistance)
                {
                    best = index;
                    bestDistance = distance;
                }
            }
            moved = moved || best != group[harmonic];
            group[harmonic] = best;
        }
        if (!moved)
            break;
    }

    // Each group's envelope, the root of its harmonics' summed squares, and
    // each harmonic's share by least squares; the waveform the shares'
    // harmonics, normalised to a peak of 1, its peak moved into the envelope.
    std::vector<double> waveformsTable(groups * talas::dsp::ga::waveformLength, 0.0);
    std::vector<double> envelopes(groups * frames, 0.0);
    for (std::size_t index = 0; index < groups; ++index)
    {
        std::vector<double> envelope(frames, 0.0);
        for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        {
            if (group[harmonic] != index)
                continue;
            for (std::size_t frame = 0; frame < frames; ++frame)
                envelope[frame] += amplitudes[harmonic][frame] * amplitudes[harmonic][frame];
        }
        double energy = 0.0;
        for (auto& value : envelope)
        {
            value = std::sqrt(value);
            energy += value * value;
        }
        if (energy <= 0.0)
            continue;
        auto* waveform = waveformsTable.data() + index * talas::dsp::ga::waveformLength;
        for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        {
            if (group[harmonic] != index)
                continue;
            double fit = 0.0;
            for (std::size_t frame = 0; frame < frames; ++frame)
                fit += amplitudes[harmonic][frame] * envelope[frame];
            const auto share = fit / energy;
            for (std::size_t sample = 0; sample < talas::dsp::ga::waveformLength; ++sample)
            {
                waveform[sample] += share * std::sin(2.0 * std::numbers::pi * static_cast<double>(harmonic + 1)
                    * static_cast<double>(sample) / static_cast<double>(talas::dsp::ga::waveformLength));
            }
        }
        double peak = 0.0;
        for (std::size_t sample = 0; sample < talas::dsp::ga::waveformLength; ++sample)
            peak = std::max(peak, std::abs(waveform[sample]));
        if (peak <= 0.0)
            continue;
        for (std::size_t sample = 0; sample < talas::dsp::ga::waveformLength; ++sample)
            waveform[sample] /= peak;
        for (std::size_t frame = 0; frame < frames; ++frame)
            envelopes[index * frames + frame] = envelope[frame] * peak;
    }

    result.table = {talas::dsp::ga::magic, talas::dsp::ga::version, static_cast<double>(groups), static_cast<double>(frames),
        static_cast<double>(talas::dsp::ga::waveformLength), basePitch};
    result.table.insert(result.table.end(), waveformsTable.begin(), waveformsTable.end());
    result.table.insert(result.table.end(), envelopes.begin(), envelopes.end());
    result.table.insert(result.table.end(), deviation.begin(), deviation.end());
    result.basePitch = basePitch;
    result.envelopeLength = frames;
    result.harmonics = harmonics;
    result.durationSeconds = static_cast<double>(frames - 1) / settings.framesPerSecond;
    return result;
}

GaAnalysisResult analyzeGa(
    std::span<const double> samples,
    double sampleRate,
    const GaAnalysisSettings& settings)
{
    GaAnalysisResult result;
    if (samples.empty() || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::ranges::all_of(samples, [](double sample) { return std::isfinite(sample); }))
    {
        result.error = "GA analysis needs finite samples and a positive sample rate";
        return result;
    }
    if (settings.waveforms < 1 || settings.waveforms > talas::dsp::ga::maximumWaveforms || settings.harmonics < 1
        || settings.harmonics > 1024 || !std::isfinite(settings.framesPerSecond) || settings.framesPerSecond < 1.0
        || settings.framesPerSecond > 1000.0)
    {
        result.error = "GA analysis takes 1 to 64 waveforms, 1 to 1024 harmonics and 1 to 1000 frames a second";
        return result;
    }
    if (std::ranges::all_of(samples, [](double sample) { return sample == 0.0; }))
    {
        result.error = "GA analysis needs a pitched sample, not silence";
        return result;
    }
    // The pitch over the whole sound, a value for each analysis window.
    talas::cdp::GetPitchSettings pitchSettings;
    const auto pitches = talas::cdp::getPitch(samples, sampleRate, pitchPoints, pitchOverlap, pitchSettings);
    const auto spacing = talas::cdp::PhaseVocoder::decimationFor(pitchPoints, pitchOverlap);
    std::vector<double> pitched;
    if (pitches.has_value())
    {
        for (const auto pitch : *pitches)
        {
            if (pitch > 0.0F)
                pitched.push_back(pitch);
        }
    }
    if (pitched.empty() || !spacing.has_value())
    {
        result.error = "GA analysis needs a pitched sample: getpitch finds no pitch in it";
        return result;
    }
    const auto basePitch = median(pitched);
    // A window's pitch, or the nearest pitched window's.
    std::vector<double> held(pitches->size(), basePitch);
    double last = -1.0;
    for (std::size_t window = 0; window < held.size(); ++window)
    {
        if ((*pitches)[window] > 0.0F)
            last = (*pitches)[window];
        held[window] = last;
    }
    double next = -1.0;
    for (std::size_t window = held.size(); window-- > 0;)
    {
        if ((*pitches)[window] > 0.0F)
            next = (*pitches)[window];
        if (held[window] <= 0.0)
            held[window] = next > 0.0 ? next : basePitch;
    }
    const auto pitchAt = [&](double seconds) {
        const auto window = static_cast<std::size_t>(
            std::clamp(std::lround(seconds * sampleRate / static_cast<double>(*spacing)), 0L,
                static_cast<long>(held.size() - 1)));
        return held[window];
    };

    const auto nyquist = sampleRate / 2.0;
    const auto harmonics = std::min<std::size_t>(settings.harmonics,
        static_cast<std::size_t>(std::max(1.0, std::floor((nyquist * 0.98) / basePitch))));
    const auto frames = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::floor(static_cast<double>(samples.size() - 1) / sampleRate
            * settings.framesPerSecond)) + 1);
    // Each harmonic's amplitude in each frame, from a Hann window four base
    // periods long centred on the frame.
    const auto windowLength = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::lround(4.0 * sampleRate / basePitch)), 64, 16384);
    std::vector<double> window(windowLength);
    double windowSum = 0.0;
    for (std::size_t n = 0; n < windowLength; ++n)
    {
        window[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(n) + 0.5)
            / static_cast<double>(windowLength));
        windowSum += window[n];
    }
    std::vector<std::vector<double>> amplitudes(harmonics, std::vector<double>(frames, 0.0));
    std::vector<double> deviation(frames, 1.0);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto seconds = static_cast<double>(frame) / settings.framesPerSecond;
        const auto pitch = pitchAt(seconds);
        deviation[frame] = pitch / basePitch;
        const auto centre = static_cast<long>(std::lround(seconds * sampleRate));
        const auto start = centre - static_cast<long>(windowLength / 2);
        for (std::size_t harmonic = 0; harmonic < harmonics; ++harmonic)
        {
            const auto frequency = pitch * static_cast<double>(harmonic + 1);
            if (frequency >= nyquist)
                continue;
            const auto omega = 2.0 * std::numbers::pi * frequency / sampleRate;
            double real = 0.0;
            double imaginary = 0.0;
            for (std::size_t n = 0; n < windowLength; ++n)
            {
                const auto at = start + static_cast<long>(n);
                if (at < 0 || at >= static_cast<long>(samples.size()))
                    continue;
                const auto value = samples[static_cast<std::size_t>(at)] * window[n];
                const auto phase = omega * static_cast<double>(at);
                real += value * std::cos(phase);
                imaginary -= value * std::sin(phase);
            }
            amplitudes[harmonic][frame] = 2.0 * std::hypot(real, imaginary) / windowSum;
        }
    }

    return groupedGaTable(amplitudes, deviation, basePitch, settings);
}

std::string gaAnalysisStageText(GaAnalysisStage stage)
{
    return stage == GaAnalysisStage::ReadingEnvelopes ? "Reading envelopes from the SOS analysis"
                                                      : "Determining waveforms and corresponding envelopes...";
}

GaAnalysisResult analyzeGaFromSpectrum(
    const talas::project::PartialTrackAsset& harmonic, const GaAnalysisSettings& settings, const std::function<void(GaAnalysisStage)>& stage)
{
    GaAnalysisResult result;
    if (settings.waveforms < 1 || settings.waveforms > talas::dsp::ga::maximumWaveforms || settings.harmonics < 1
        || settings.harmonics > 1024 || !std::isfinite(settings.framesPerSecond) || settings.framesPerSecond < 1.0
        || settings.framesPerSecond > 1000.0)
    {
        result.error = "GA analysis takes 1 to 64 waveforms, 1 to 1024 harmonics and 1 to 1000 frames a second";
        return result;
    }
    if (harmonic.frames.empty() || !(harmonic.sampleRate > 0.0))
    {
        result.error = "GA analysis needs a quasi-harmonic spectrum file";
        return result;
    }
    if (stage)
        stage(GaAnalysisStage::ReadingEnvelopes);
    // Each source frame's time and fundamental.
    std::size_t tracks = 0;
    std::vector<double> times;
    std::vector<double> fundamentals;
    for (const auto& frame : harmonic.frames)
    {
        times.push_back(static_cast<double>(frame.sourceFrame) / harmonic.sampleRate);
        double weighted = 0.0;
        double weight = 0.0;
        for (const auto& point : frame.points)
        {
            tracks = std::max<std::size_t>(tracks, point.trackId);
            if (point.amplitude > 0.0 && point.frequencyHz > 0.0)
            {
                weighted += point.amplitude * point.frequencyHz / static_cast<double>(point.trackId);
                weight += point.amplitude;
            }
        }
        fundamentals.push_back(weight > 0.0 ? weighted / weight : 0.0);
    }
    std::vector<double> sounding;
    for (const auto fundamental : fundamentals)
    {
        if (fundamental > 0.0)
            sounding.push_back(fundamental);
    }
    if (sounding.empty() || tracks == 0)
    {
        result.error = "GA analysis needs a quasi-harmonic spectrum with sounding harmonics";
        return result;
    }
    const auto basePitch = median(sounding);
    const auto harmonics = std::min<std::size_t>({settings.harmonics, tracks,
        static_cast<std::size_t>(std::max(1.0, std::floor(harmonic.sampleRate * 0.49 / basePitch)))});
    const auto frames = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(times.back() * settings.framesPerSecond)) + 1);
    // A source frame's value of a harmonic, and the frames either side of a
    // time.
    const auto amplitudeIn = [&](std::size_t source, std::size_t number) {
        for (const auto& point : harmonic.frames[source].points)
        {
            if (point.trackId == number)
                return point.amplitude;
        }
        return 0.0;
    };
    std::vector<std::vector<double>> amplitudes(harmonics, std::vector<double>(frames, 0.0));
    std::vector<double> deviation(frames, 1.0);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto seconds = static_cast<double>(frame) / settings.framesPerSecond;
        const auto after = static_cast<std::size_t>(std::ranges::upper_bound(times, seconds) - times.begin());
        const auto right = std::min(after, times.size() - 1);
        const auto left = after == 0 ? 0 : after - 1;
        const auto span = times[right] - times[left];
        const auto amount = span > 0.0 ? std::clamp((seconds - times[left]) / span, 0.0, 1.0) : 0.0;
        for (std::size_t index = 0; index < harmonics; ++index)
            amplitudes[index][frame] = std::lerp(amplitudeIn(left, index + 1), amplitudeIn(right, index + 1), amount);
        const auto leftPitch = fundamentals[left] > 0.0 ? fundamentals[left] : fundamentals[right];
        const auto rightPitch = fundamentals[right] > 0.0 ? fundamentals[right] : leftPitch;
        const auto pitch = leftPitch > 0.0 ? std::lerp(leftPitch, rightPitch, amount) : basePitch;
        deviation[frame] = pitch / basePitch;
    }
    if (stage)
        stage(GaAnalysisStage::DeterminingWaveforms);
    return groupedGaTable(amplitudes, deviation, basePitch, settings);
}

GaAnalysisResult writeGaAnalysisFileFromSpectrum(const talas::project::PartialTrackAsset& harmonic, const GaAnalysisSettings& settings,
    const std::filesystem::path& path, const std::function<void(GaAnalysisStage)>& stage)
{
    auto result = analyzeGaFromSpectrum(harmonic, settings, stage);
    if (!result.ok())
        return result;
    const auto rate = static_cast<std::uint32_t>(std::lround(harmonic.sampleRate));
    if (!writeFloat32Wav(path, rate, result.table, result.error))
        return result;
    return result;
}

GaAnalysisResult writeGaAnalysisFile(
    std::span<const std::vector<double>> channels,
    double sampleRate,
    const GaAnalysisSettings& settings,
    const std::filesystem::path& path)
{
    if (channels.empty())
    {
        GaAnalysisResult refused;
        refused.error = "GA analysis needs a sample";
        return refused;
    }
    auto mono = channels.front();
    for (std::size_t channel = 1; channel < channels.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < mono.size() && frame < channels[channel].size(); ++frame)
            mono[frame] += channels[channel][frame];
    }
    if (channels.size() > 1)
    {
        for (auto& sample : mono)
            sample /= static_cast<double>(channels.size());
    }
    auto result = analyzeGa(mono, sampleRate, settings);
    if (!result.ok())
        return result;
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));
    if (!writeFloat32Wav(path, rate, result.table, result.error))
        return result;
    return result;
}
}
