#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::test
{
// Pure source mathematics: no production DSP or renderer supplies the oracle.
struct TimeFrequencyScaleVariableSourceReference
{
    static constexpr double sampleRate = 48000, tau = 2 * std::numbers::pi;
    static constexpr std::size_t window = 8192, latency = window + window / 4 - 1;
    static constexpr std::size_t recordingFrames = 8 * 48000, timelineFrames = 16 * 48000;
    static constexpr std::size_t outputFrames = latency + timelineFrames;
    static constexpr std::size_t plateauFrames = 96000, measurementFrames = 24000;
    static constexpr std::array frequencies{53.0, 173.0, 389.0, 701.0};
    static constexpr std::array<std::pair<std::size_t, double>, 6> changes{{
        {0, 0.25}, {48000 + 113, 0.75}, {4 * 48000 + 37, 0.5},
        {7 * 48000 + 211, 1.0}, {10 * 48000 + 59, 0.25}, {13 * 48000 + 173, 0.5}}};
    static constexpr std::array centers{1.25, 2.25, 4.0, 5.5, 8.0, 9.75, 11.25, 12.0};
    static constexpr double chirpStart = 80, chirpSlope = 16;
    std::vector<double> clock, expectedPhase, rates;

    TimeFrequencyScaleVariableSourceReference()
        : clock(timelineFrames + 1), expectedPhase(timelineFrames + 1), rates(timelineFrames)
    {
        std::size_t change = 0;
        for (std::size_t frame = 0; frame < timelineFrames; ++frame)
        {
            if (change + 1 < changes.size() && frame == changes[change + 1].first) ++change;
            rates[frame] = changes[change].second;
            clock[frame + 1] = clock[frame] + rates[frame];
            // Exact trapezoidal integral of f0+k*sourceTime over a constant-rate
            // sample interval, independent of the implementation's phase estimator.
            const auto meanSourceTime = (clock[frame] + clock[frame + 1]) / (2 * sampleRate);
            expectedPhase[frame + 1] = expectedPhase[frame]
                + tau * (chirpStart + chirpSlope * meanSourceTime) / sampleRate;
        }
    }

    std::vector<double> recording(bool chirp) const
    {
        std::vector<double> result(recordingFrames);
        for (std::size_t frame = 0; frame < result.size(); ++frame)
        {
            const auto time = static_cast<double>(frame) / sampleRate;
            const auto phase = chirp ? tau * (chirpStart * time + 0.5 * chirpSlope * time * time)
                : tau * frequencies[frame / plateauFrames] * time;
            result[frame] = 0.08 + 0.25 * std::sin(phase);
        }
        return result;
    }

    void verify(std::span<const double> output, bool chirp, std::string_view path,
        std::span<const std::pair<std::size_t, double>> scaleChanges = {},
        std::span<const double> measurementCenters = centers) const
    {
        const auto require = [path](bool condition, const char* message) {
            if (!condition) throw std::runtime_error(std::string(path) + ": " + message);
        };
        require(output.size() == outputFrames, "changing-rate output shape");
        require(std::ranges::all_of(output, [](double value) { return std::isfinite(value); }),
            "changing-rate output must be finite including transitions");
        require(std::ranges::all_of(output.first(latency), [](double value) { return value == 0; }),
            "changing-rate source must retain its exact silent native delay");
        require(clock[timelineFrames - 48000 - window] - window >= recordingFrames,
            "final silence measurement must follow independently integrated EOF and support");
        require(std::ranges::all_of(output.last(48000), [](double value) { return std::abs(value) < 1e-12; }),
            "changing-rate source must drain after independently integrated EOF and support");
        std::size_t failedMeasurements = 0;
        const auto valueAt = [](const auto& timeline, std::size_t frame, double fallback) {
            for (const auto& [at, value] : timeline)
            {
                if (at > frame) break;
                fallback = value;
            }
            return fallback;
        };
        const auto neutralAt = [&](std::size_t frame) {
            return valueAt(changes, frame, changes.front().second)
                == valueAt(scaleChanges, frame, 1);
        };
        for (const auto centerSeconds : measurementCenters)
        {
            const auto center = static_cast<std::size_t>(centerSeconds * sampleRate);
            const auto start = center - measurementFrames / 2;
            const auto measured = output.subspan(latency + start, measurementFrames);
            const auto identity = static_cast<std::size_t>(clock[center]) / plateauFrames;
            double frequencyScale = 1;
            for (const auto& [frame, scale] : scaleChanges)
            {
                if (frame <= center) frequencyScale = scale;
                // The declared native four-hop ramp occupies N samples;
                // another N excludes synthesis support around that ramp.
                require(frame == 0 || frame + 2 * window <= start
                    || frame >= start + measurementFrames + window,
                    "pitch measurement must stay outside scale ramps and synthesis support");
            }
            const auto checkMixSupport = [&](std::size_t frame) {
                if (frame != 0 && neutralAt(frame) != neutralAt(frame - 1))
                    require(start + measurementFrames <= frame || start >= frame + window,
                        "steady pitch measurement must exclude the N-frame spectral/raw transition");
            };
            for (const auto& change : changes) checkMixSupport(change.first);
            for (const auto& change : scaleChanges) checkMixSupport(change.first);
            // Conservative source and synthesis supports of one whole N each
            // keep tone measurements clear of an unspecified transient shape.
            const auto lower = clock[start - window] - window;
            const auto upper = clock[start + measurementFrames + window] + window;
            require(identity < frequencies.size() && lower >= identity * plateauFrames
                && upper < (identity + 1) * plateauFrames,
                "measurement must stay inside independently predicted source support");
            const auto project = [&](double frequencyOffset, bool resampled = false) {
                std::complex<double> sum{};
                for (std::size_t i = 0; i < measurementFrames; ++i)
                {
                    const auto sourceTime = clock[start + i] / sampleRate;
                    const auto phase = !chirp ? 0 : resampled
                        ? tau * (chirpStart * sourceTime + 0.5 * chirpSlope * sourceTime * sourceTime)
                        : frequencyScale * expectedPhase[start + i];
                    const auto weight = 0.5 - 0.5 * std::cos(tau * i / measurementFrames);
                    sum += measured[i] * weight
                        * std::polar(1.0, -phase - tau * frequencyOffset * i / sampleRate);
                }
                return std::abs(sum) / measurementFrames;
            };
            const auto frequency = chirp ? 0 : frequencyScale * frequencies[identity];
            const auto wanted = project(frequency);
            double wrong = 0, bestOffset = 0, best = 0;
            if (!chirp)
            {
                for (const auto other : frequencies)
                    if (other != frequencies[identity])
                        wrong = std::max(wrong, project(frequencyScale * other));
                if (frequencyScale != 1) wrong = std::max(wrong, project(frequencies[identity]));
                for (const auto rate : {0.25, 0.5, 0.75})
                {
                    wrong = std::max(wrong, project(frequency * rate));
                    if (rate != frequencyScale)
                        wrong = std::max(wrong, project(frequencies[identity] * rate));
                }
            }
            else
            {
                const auto neutral = std::ranges::all_of(std::span(rates).subspan(start, measurementFrames),
                    [frequencyScale](double rate) { return rate == frequencyScale; });
                // At Rate=Scale the resampled and independently pitched trajectories
                // coincide; their ratio cannot distinguish the two behaviors.
                if (!neutral) wrong = project(0, true);
            }
            for (double offset = -2; offset <= 2; offset += 0.25)
            {
                const auto value = project(frequency + offset);
                if (value > best) { best = value; bestOffset = offset; }
            }
            const auto valid = wanted > 0.045 && wanted > 10 * wrong && std::abs(bestOffset) <= 1;
            std::cout << "varying_source path=" << path << " chirp=" << chirp << " center_s=" << centerSeconds
                      << " source_s=" << clock[center] / sampleRate << " scale=" << frequencyScale << " wanted=" << wanted
                      << " wrong=" << wrong << " peak_offset_hz=" << bestOffset
                      << " pass=" << valid << '\n';
            if (!valid) ++failedMeasurements;
        }
        require(failedMeasurements == 0, "changing-rate source must preserve independent source time, pitch and gain");
    }
};
}
