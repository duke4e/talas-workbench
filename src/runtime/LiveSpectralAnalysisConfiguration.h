#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <ranges>
#include <string_view>

namespace talas::runtime::detail
{
// Response: BestFreq's one bin, or blank, to BestTime's four.
[[nodiscard]] inline std::optional<std::size_t>
liveSpectralAnalysisResponseBins(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return 1;
    const auto last = text.find_last_not_of(" \t\r\n");
    const auto word = text.substr(first, last + 1 - first);
    const auto equals = [&](std::string_view name) {
        return word.size() == name.size()
            && std::equal(word.begin(), word.end(), name.begin(), [](char one, char other) {
                   return std::tolower(static_cast<unsigned char>(one))
                       == std::tolower(static_cast<unsigned char>(other));
               });
    };
    if (equals("BestFreq"))
        return 1;
    if (equals("BetterFreq"))
        return 2;
    if (equals("BetterTime"))
        return 3;
    if (equals("BestTime"))
        return 4;
    return std::nullopt;
}

[[nodiscard]] inline const ParameterValue*
findLiveSpectralAnalysisParameter(
    const CompileStep& step,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find_if(
        step.parameters,
        [&](const ParameterValue& parameter) {
            return parameter.id == parameterId;
        });
    return found == step.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] inline std::optional<
    talas::dsp::LiveSpectralAnalysisConfiguration>
liveSpectralAnalysisConfiguration(
    const CompileStep& step,
    const RenderSettings& settings) noexcept
{
    const auto* lowest = findLiveSpectralAnalysisParameter(
        step, "lowestAnalyzedFreq");
    const auto* ampScale = findLiveSpectralAnalysisParameter(
        step, "ampScale");
    const auto* freqScale = findLiveSpectralAnalysisParameter(
        step, "freqScale");
    const auto* response = findLiveSpectralAnalysisParameter(
        step, "response");
    const auto* harmonic = findLiveSpectralAnalysisParameter(
        step, "harmonic");
    const auto* unpitchedOnly = findLiveSpectralAnalysisParameter(
        step, "unpitchedOnly");
    const auto* unpitchedThreshold =
        findLiveSpectralAnalysisParameter(
            step, "unpitchedThreshold");
    const auto* trackedHarmonic =
        findLiveSpectralAnalysisParameter(
            step, "trackedHarmonic");
    const auto* lowFreq = findLiveSpectralAnalysisParameter(
        step, "lowFreq");
    const auto* highFreq = findLiveSpectralAnalysisParameter(
        step, "highFreq");
    const auto* initFreq = findLiveSpectralAnalysisParameter(
        step, "initFreq");
    const auto* fundamentalOnly =
        findLiveSpectralAnalysisParameter(
            step, "fundamentalOnly");
    // The tracks are the bands; projects before format 57 kept
    // the strongest peaks between LowFreq and HighFreq.
    const auto* tracks = findLiveSpectralAnalysisParameter(
        step, "tracks");
    const auto bandTracks = !(tracks != nullptr
        && tracks->choice == "legacyPeaks");
    // The bands give 512 to 32 partials; projects before format
    // 75 took the next larger power of two.
    const auto* bandWidth = findLiveSpectralAnalysisParameter(
        step, "bandWidth");
    const auto legacyNextPower = bandWidth != nullptr
        && bandWidth->choice == "legacyNextPower";
    if (lowest == nullptr || ampScale == nullptr
        || freqScale == nullptr || response == nullptr
        || harmonic == nullptr || unpitchedOnly == nullptr
        || unpitchedThreshold == nullptr
        || trackedHarmonic == nullptr || lowFreq == nullptr
        || highFreq == nullptr || initFreq == nullptr
        || fundamentalOnly == nullptr
        || !response->text.has_value()
        || !harmonic->boolean.has_value()
        || !fundamentalOnly->boolean.has_value()
        || !liveSpectralAnalysisResponseBins(*response->text).has_value()
        || (*harmonic->boolean && !bandTracks)
        || !std::isfinite(unpitchedOnly->value)
        || (bandTracks
                ? unpitchedOnly->value != 0.0 && unpitchedOnly->value != 1.0
                : std::abs(unpitchedOnly->value) > 1.0e-12)
        || !std::isfinite(unpitchedThreshold->value)
        || (bandTracks
                ? unpitchedThreshold->value < 0.0 || unpitchedThreshold->value > 1.0
                : std::abs(unpitchedThreshold->value) > 1.0e-12)
        || !std::isfinite(trackedHarmonic->value)
        || (bandTracks
                ? trackedHarmonic->value < 1.0 || trackedHarmonic->value > 64.0
                    || std::floor(trackedHarmonic->value) != trackedHarmonic->value
                : std::abs(trackedHarmonic->value - 1.0) > 1.0e-12))
    {
        return std::nullopt;
    }

    const auto derived =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            settings.sampleRate,
            settings.maxBlockFrames,
            lowest->value,
            ampScale->value,
            freqScale->value,
            lowFreq->value,
            highFreq->value,
            initFreq->value,
            *fundamentalOnly->boolean,
            bandTracks,
            *harmonic->boolean,
            static_cast<std::size_t>(trackedHarmonic->value),
            unpitchedOnly->value == 1.0,
            unpitchedThreshold->value,
            legacyNextPower,
            *liveSpectralAnalysisResponseBins(*response->text),
        });
    return derived
        ? std::optional{derived.configuration}
        : std::nullopt;
}
}
