#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace talas::runtime::checked_serialized_spectrum_source
{
// An Audio pair carries its frequencies as fractions of half the sample
// rate, as audio values do (a Ramp's cycle gives low to high partials;
// format 93). Projects before it carry
// hertz: `pairFrequencies` `legacyHertz`. A pair sample times this factor is
// hertz; hertz divided by it is a pair sample.
[[nodiscard]] inline double pairFrequencyHertz(
    const CompileStep& step, double sampleRate) noexcept
{
    const auto found = std::ranges::find(step.parameters,
        std::string_view{"pairFrequencies"}, &ParameterValue::id);
    if (found != step.parameters.end()
        && found->choice == std::optional<std::string>{"legacyHertz"})
        return 1.0;
    return 0.5 * sampleRate;
}

// With LogScale, a SyntheticSpectrumFromSounds reads its
// FrequenciesAndBandwidths as log frequencies, pitches (format 107):
// 1 is half the sample rate and each 1/15
// below it an octave lower. Projects before it read frequencies, as a
// linear SyntheticSpectrumFromSounds does, and take their pitch:
// `logFrequencies` `legacyLinear`.
[[nodiscard]] inline bool readsLogPitches(const CompileStep& step) noexcept
{
    const auto found = std::ranges::find(step.parameters,
        std::string_view{"logFrequencies"}, &ParameterValue::id);
    return found == step.parameters.end()
        || found->choice != std::optional<std::string>{"legacyLinear"};
}

// Whether a step's spectrum input is a log spectrum, carrying note numbers:
// a LogScale spectral source's or a SpectrumLinearToLog's, through Sounds
// that pass a spectrum on, not through a SpectrumLogToLinear.
[[nodiscard]] inline bool readsLogSpectrum(
    const RuntimePlan& plan, const CompileStep& step, std::string_view port)
{
    const auto* reading = &step;
    for (std::size_t depth = 0; depth < plan.steps.size(); ++depth)
    {
        const auto binding = std::ranges::find(reading->inputs, port, &InputBinding::inputPortId);
        if (binding == reading->inputs.end())
            return false;
        const auto source = std::ranges::find(plan.steps, binding->sourceNodeId, &CompileStep::nodeId);
        if (source == plan.steps.end() || source->typeId == "talas.spectrumLogToLinear")
            return false;
        if (source->typeId == "talas.spectrumLinearToLog")
            return true;
        if (source->typeId == "talas.syntheticSpectrumFromSounds" || source->typeId == "talas.syntheticSpectrumFromArray")
        {
            const auto logScale = std::ranges::find(source->parameters, std::string_view{"logScale"}, &ParameterValue::id);
            return logScale != source->parameters.end() && logScale->boolean == std::optional<bool>{true};
        }
        // A Sound passing a spectrum on keeps its kind.
        if (std::ranges::find(source->inputs, std::string_view{"spectrumIn"}, &InputBinding::inputPortId)
            == source->inputs.end())
            return false;
        reading = &*source;
        port = "spectrumIn";
    }
    return false;
}

// A log spectrum's note number as its frequency in hertz.
[[nodiscard]] inline double noteNumberHertz(double noteNumber) noexcept
{
    return 440.0 * std::pow(2.0, (noteNumber - 69.0) / 12.0);
}

// A log frequency as the note number of its pitch, as a log spectrum
// carries it.
[[nodiscard]] inline double logPitchNoteNumber(double value, double sampleRate) noexcept
{
    return 69.0 + 12.0 * std::log2(0.5 * sampleRate / 440.0) + 180.0 * (value - 1.0);
}

struct Source
{
    // Ordinary or transformed Audio pairs preserve the implicit bank-size period.
    std::optional<std::size_t> countBuffer;
};

// The two direct pair edges already order this producer before the bank. Its
// sibling Spectrum header supplies the active count, including inferred InRAM
// selections and connected Sounds counts. Never infer metadata from one lane.
[[nodiscard]] inline std::optional<Source> resolve(
    const RuntimePlan& plan, const InputBinding& amplitude, const InputBinding& frequency)
{
    if (amplitude.inputPortId != "amplitudeIn" || frequency.inputPortId != "frequencyIn")
        return std::nullopt;
    for (const auto* binding : {&amplitude, &frequency})
    {
        if (binding->bufferIndex >= plan.buffers.size())
            return std::nullopt;
        const auto& buffer = plan.buffers[binding->bufferIndex];
        if (buffer.nodeId != binding->sourceNodeId
            || buffer.scopeId != binding->sourceScopeId
            || buffer.portId != binding->sourcePortId
            || buffer.type != talas::core::PortType::Audio)
            return std::nullopt;
    }
    if (amplitude.sourceNodeId != frequency.sourceNodeId
        || amplitude.sourceScopeId != frequency.sourceScopeId
        || amplitude.sourcePortId != "amplitudeOut"
        || frequency.sourcePortId != "frequencyOut")
        return Source{};

    const CompileStep* producer = nullptr;
    for (const auto& step : plan.steps)
    {
        if (step.nodeId == amplitude.sourceNodeId
            && step.scopeId == amplitude.sourceScopeId)
        {
            if (producer != nullptr)
                return std::nullopt;
            producer = &step;
        }
    }
    if (producer == nullptr)
        return std::nullopt;
    if (producer->typeId != "talas.syntheticSpectrumFromArray"
        && producer->typeId != "talas.syntheticSpectrumFromSounds"
        && producer->typeId != "talas.spectrumInRam")
        return Source{};

    Source source;
    for (std::size_t index = 0; index < plan.buffers.size(); ++index)
    {
        const auto& buffer = plan.buffers[index];
        if (buffer.nodeId == producer->nodeId
            && buffer.scopeId == producer->scopeId
            && buffer.portId == "spectrumOut")
        {
            if (source.countBuffer.has_value()
                || buffer.type != talas::core::PortType::Spectrum)
                return std::nullopt;
            source.countBuffer = index;
        }
    }
    if (!source.countBuffer.has_value())
        return std::nullopt;
    if (!producer->outputs.empty())
    {
        for (const auto index : {amplitude.bufferIndex, frequency.bufferIndex,
                 *source.countBuffer})
        {
            const auto& port = plan.buffers[index].portId;
            if (std::ranges::count(producer->outputs, port,
                    &OutputBinding::outputPortId) != 1
                || std::ranges::find(producer->outputs, port,
                    &OutputBinding::outputPortId)->bufferIndex != index)
                return std::nullopt;
        }
    }
    return source;
}

// OscillatorBank has exactly these two inputs in serialized mode. Other
// consumers validate their own complete input shape before resolving the pair.
[[nodiscard]] inline std::optional<Source> resolve(
    const RuntimePlan& plan, const CompileStep& consumer)
{
    if (consumer.inputs.size() != 2)
        return std::nullopt;
    return resolve(plan, consumer.inputs[0], consumer.inputs[1]);
}

[[nodiscard]] inline std::optional<std::size_t> partialCount(
    std::span<const double> spectrum, std::size_t selectedCount) noexcept
{
    if (spectrum.empty() || !std::isfinite(spectrum[0])
        || spectrum[0] < 1.0 || spectrum[0] > 4096.0
        || std::floor(spectrum[0]) != spectrum[0])
        return std::nullopt;
    const auto count = static_cast<std::size_t>(spectrum[0]);
    if (count < selectedCount || spectrum.size() < 1 + count * 3)
        return std::nullopt;
    return count;
}
}
