#pragma once

#include "CheckedBiquadMath.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::checked_modal_filter
{
struct Mode
{
    double amplitude = 0.0;
    double frequency = 0.0;
    double bandwidthRatio = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    std::span<const Mode> modes;
    double inputLevel = 1.0;
    double envelope = 1.0;
    bool resetOnDownwardJump = false;
};

[[nodiscard]] inline std::optional<std::vector<double>> parseArray(
    std::string_view source)
{
    const auto first = source.find_first_not_of(" \t\r\n");
    const auto last = source.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos || last == std::string_view::npos)
        return std::nullopt;
    source = source.substr(first, last - first + 1);
    if (source.size() < 4 || !source.starts_with("#(")
        || source.back() != ')')
        return std::nullopt;
    source.remove_prefix(2);
    source.remove_suffix(1);
    std::vector<double> values;
    std::size_t offset = 0;
    while (offset < source.size())
    {
        while (offset < source.size()
            && (source[offset] == ' ' || source[offset] == '\t'
                || source[offset] == '\r' || source[offset] == '\n'
                || source[offset] == ','))
            ++offset;
        if (offset == source.size())
            break;
        const auto start = offset;
        while (offset < source.size()
            && source[offset] != ' ' && source[offset] != '\t'
            && source[offset] != '\r' && source[offset] != '\n'
            && source[offset] != ',')
            ++offset;
        const std::string token{source.substr(start, offset - start)};
        char* end = nullptr;
        errno = 0;
        const auto value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()
            || errno == ERANGE || !std::isfinite(value))
            return std::nullopt;
        values.push_back(value);
        if (values.size() > 16)
            return std::nullopt;
    }
    if (values.empty())
        return std::nullopt;
    return values;
}

[[nodiscard]] inline std::optional<std::vector<Mode>> parseModes(
    std::string_view amplitudes,
    std::string_view frequencies,
    std::string_view bandwidthRatios,
    double sampleRate)
{
    const auto amps = parseArray(amplitudes);
    const auto freqs = parseArray(frequencies);
    const auto bws = parseArray(bandwidthRatios);
    if (!amps || !freqs || !bws
        || amps->size() != freqs->size()
        || amps->size() != bws->size()
        || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return std::nullopt;
    std::vector<Mode> modes;
    modes.reserve(amps->size());
    for (std::size_t index = 0; index < amps->size(); ++index)
    {
        const Mode mode{(*amps)[index], (*freqs)[index], (*bws)[index]};
        if (std::abs(mode.amplitude) > 1.0
            || mode.frequency <= 0.0
            || mode.frequency >= sampleRate * 0.5
            || mode.bandwidthRatio <= 0.0
            || mode.bandwidthRatio > 1.0)
            return std::nullopt;
        modes.push_back(mode);
    }
    return modes;
}

[[nodiscard]] inline bool valid(const Config& config) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || config.modes.empty() || config.modes.size() > 16
        || !std::isfinite(config.inputLevel)
        || std::abs(config.inputLevel) > 1.0
        || !std::isfinite(config.envelope)
        || std::abs(config.envelope) > 1.0)
        return false;
    for (const auto& mode : config.modes)
    {
        if (!std::isfinite(mode.amplitude)
            || std::abs(mode.amplitude) > 1.0
            || !std::isfinite(mode.frequency)
            || mode.frequency <= 0.0
            || mode.frequency >= config.sampleRate * 0.5
            || !std::isfinite(mode.bandwidthRatio)
            || mode.bandwidthRatio <= 0.0
            || mode.bandwidthRatio > 1.0)
            return false;
    }
    return true;
}

[[nodiscard]] inline bool advance(
    const Config& config,
    double input,
    double frequencyScale,
    std::span<const checked_biquad::State> previous,
    std::span<checked_biquad::State> candidate,
    double previousScale,
    bool initialized,
    double& output) noexcept
{
    if (!valid(config) || !std::isfinite(input)
        || !std::isfinite(frequencyScale) || frequencyScale <= 0.0
        || frequencyScale > 16.0
        || !std::isfinite(previousScale) || previousScale <= 0.0
        || previous.size() != config.modes.size()
        || candidate.size() != config.modes.size())
        return false;
    // The public description does not specify the threshold. Keep this
    // subset's reset tied to a discrete, at-least-octave downward jump.
    const auto reset = config.resetOnDownwardJump && initialized
        && frequencyScale <= previousScale * 0.5;
    double excited = 0.0;
    if (!checked_biquad::checkedMultiply(
            input, config.inputLevel, excited))
        return false;
    double sum = 0.0;
    for (std::size_t index = 0; index < config.modes.size(); ++index)
    {
        const auto& mode = config.modes[index];
        double center = 0.0;
        double bandwidth = 0.0;
        checked_biquad::Coefficients coefficients;
        if (!checked_biquad::checkedMultiply(
                mode.frequency, frequencyScale, center)
            || center >= config.sampleRate * 0.5
            || !checked_biquad::checkedMultiply(
                center, mode.bandwidthRatio, bandwidth)
            || !checked_biquad::makeBandPassCoefficients(
                config.sampleRate, center, bandwidth, coefficients))
            return false;
        auto state = reset ? checked_biquad::State{} : previous[index];
        double filtered = 0.0;
        double weighted = 0.0;
        if (!checked_biquad::processSample(
                coefficients, state, excited, filtered)
            || !checked_biquad::checkedMultiply(
                filtered, mode.amplitude, weighted)
            || !checked_biquad::checkedAdd(sum, weighted, sum))
            return false;
        candidate[index] = state;
    }
    return checked_biquad::checkedMultiply(sum, config.envelope, output);
}
}
