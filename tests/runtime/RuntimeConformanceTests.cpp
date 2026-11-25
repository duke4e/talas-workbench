#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/RuntimeConformance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <limits>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
thread_local bool trackOfflineAllocations = false;
thread_local std::size_t offlineAllocationCount = 0;
thread_local std::size_t offlineAllocationFailureIndex = std::numeric_limits<std::size_t>::max();
thread_local bool offlineAllocationFaultTriggered = false;

struct ScopedOfflineAllocationFault
{
    explicit ScopedOfflineAllocationFault(std::size_t failure = std::numeric_limits<std::size_t>::max())
    {
        offlineAllocationCount = 0;
        offlineAllocationFailureIndex = failure;
        offlineAllocationFaultTriggered = false;
        trackOfflineAllocations = true;
    }
    ~ScopedOfflineAllocationFault() { trackOfflineAllocations = false; }
};
}

void* operator new(std::size_t size)
{
    if (trackOfflineAllocations && offlineAllocationCount++ == offlineAllocationFailureIndex)
    {
        // Fail once: diagnostics and stack unwinding must be able to allocate.
        trackOfflineAllocations = false;
        offlineAllocationFaultTriggered = true;
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(std::max(size, std::size_t{1}))) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace talas::runtime
{
struct RealtimeRenderSessionTestAccess
{
    static auto beginOfflineTransaction(RealtimeRenderSession& session)
    {
        return session.beginOfflineTransaction();
    }
};
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {
        std::move(fromNode),
        std::move(fromPort),
        std::move(toNode),
        std::move(toPort)};
}

talas::project::ProjectParameterExpression booleanParameter(
    std::string id,
    bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression choiceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

// The track's frequency is in hz in these fixtures, as in projects before
// format 53; spectra now carry a fraction of half the sample rate.
std::vector<talas::project::ProjectParameterExpression> hertzSelector(
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    if (std::ranges::none_of(parameters, [](const auto& parameter) {
            return parameter.parameterId == "freqUnits";
        }))
        parameters.push_back(choiceParameter("freqUnits", "legacyHertz"));
    return parameters;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::runtime::PreparedRuntimeGraph compileOscillatorFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "0.5"}}));
    document.graph.connections.push_back(
        connection("osc", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 32;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "oscillator conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileNativeStftFixture(
    std::string hopSize,
    std::string window,
    bool liveDivisor = false,
    bool captureAnalysis = false)
{
    const auto parameters = [&] {
        return std::vector<talas::project::ProjectParameterExpression>{
            {"frameSize", "64"},
            {"hopSize", hopSize},
            choiceParameter("window", window),
        };
    };
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "inverse", "talas.inverseFftV1", parameters()));
    document.graph.nodes.push_back(node(
        "forward", "talas.forwardFftV1", parameters()));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", liveDivisor ? "1 / !Divisor" : "1.0"}}));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "forward", "audioIn"));
    document.graph.connections.push_back(connection(
        "forward", "spectrumOut", "inverse", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "inverse", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 17;
    if (liveDivisor) settings.eventValues = {{"Divisor", 1.0}};
    if (captureAnalysis)
    {
        settings.enableRealtimeAnalysisCapture = true;
        settings.realtimeAnalysisQueueCapacity = 8;
        settings.realtimeAnalysisTargets.push_back({{}, {}, "forward", "spectrumOut",
            talas::runtime::RealtimeAnalysisKind::Spectrum,
            talas::runtime::RealtimeAnalysisPayloadKind::ComplexFrame, 1, "complex-rollback"});
    }
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
            "native STFT conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileFilterFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "filter",
        "talas.filter",
        {
            {"frequency", "150 hz"},
            {"q", "1.0"},
            {"scale", "1.0"},
            {"feedback", "0.0"},
            {"order", "4"},
            // The baseline is the two-pole sections of projects before
            // format 80.
            choiceParameter("poles", "legacyTwoPerOrder"),
        }));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "100 hz"}, {"amplitude", "0.5"}}));
    document.graph.connections.push_back(connection(
        "osc",
        "audioOut",
        "filter",
        "audioIn"));
    document.graph.connections.push_back(connection(
        "filter",
        "audioOut",
        "out",
        "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "filter conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileAdsrFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node(
        "env",
        "talas.adsr",
        {
            {"attackTime", "4 ms"},
            {"decayTime", "4 ms"},
            {"sustainLevel", "0.25"},
            {"releaseTime", "4 ms"},
            {"scale", "1.0"},
            {"gate", "1.0"},
            {"legato", "0.0"},
        }));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "250 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection(
        "osc",
        "audioOut",
        "vca",
        "audioIn"));
    document.graph.connections.push_back(connection(
        "env",
        "value",
        "vca",
        "gain"));
    document.graph.connections.push_back(connection(
        "vca",
        "audioOut",
        "out",
        "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "ADSR conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileStaticFirFixture(
    bool reResonator)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(reResonator
        ? node(
              "fir",
              "talas.reResonator",
              {resourceParameter("wavetable", "Tables/C18/RE"),
               {"timeIndex", "0"},
               choiceParameter("filterForm", "legacyFir")})
        : node(
              "fir",
              "talas.firFilter",
              {textParameter(
                  "coefficients", "0.5 0.25 -0.125")}));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "0.75"}}));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "fir", "audioIn"));
    document.graph.connections.push_back(connection(
        "fir", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 19;
    if (reResonator)
    {
        settings.tableResources.push_back({
            "Tables/C18/RE", {0.5, 0.25, -0.125}});
    }
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        std::string{reResonator ? "RE Resonator" : "FIR Filter"}
            + " C18 conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileVcfFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vcf", "talas.vcf", {
        {"amplitude", "0.75"}, {"cutoff", "4 hz"},
        {"cutoffModulator", "0.25"},
        {"cutoffModRange", "1 hz"}, {"resonance", "0.25"},
    }));
    document.graph.nodes.push_back(node(
        "osc", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "0.75"},
        }));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "vcf", "audioIn"));
    document.graph.connections.push_back(connection(
        "vcf", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "C19 VCF conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileHarmonicResonatorFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "resonator", "talas.harmonicResonator", {
            {"decayTime", "0.5 s"},
            {"brightness", "2.0"},
            {"frequency", "4 hz"},
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", false),
            {"scale", "0.75"},
        }));
    document.graph.nodes.push_back(node(
        "osc", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "0.75"},
        }));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "resonator", "audioIn"));
    document.graph.connections.push_back(connection(
        "resonator", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 19;
    settings.maxRealtimeHistoryFrames = 16;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C20 Harmonic Resonator conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileQuadOscillatorFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "quad", "talas.quadOscillator", {{"frequency", "-1 hz"}}));
    document.graph.nodes.push_back(node(
        "leftEnvelope", "talas.oscillator", {
            {"frequency", "0.5 hz"}, {"amplitude", "0.75"},
        }));
    document.graph.nodes.push_back(node(
        "rightEnvelope", "talas.oscillator", {
            {"frequency", "0.75 hz"}, {"amplitude", "0.5"},
        }));
    document.graph.connections.push_back(connection(
        "leftEnvelope", "audioOut", "quad", "envelope"));
    document.graph.connections.push_back(connection(
        "rightEnvelope", "audioOut", "quad", "rightEnvelope"));
    document.graph.connections.push_back(connection(
        "quad", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "quad", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C22 Quad Oscillator conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileOverlappingMixerFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "mix",
        "talas.overlappingMixer",
        {
            textParameter("legacyInputs", "in3, in1; in4 in2"),
            {"overlapTime", "0 s"},
            {"left", "0.5"},
            {"right", "-0.25"},
            talas::project::ProjectParameterExpression{"sequence", std::nullopt, "legacyAllAtOnce"},
        }));
    document.graph.nodes.push_back(node(
        "in1", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.75"}}));
    document.graph.nodes.push_back(node(
        "in2", "talas.oscillator", {{"frequency", "2 hz"}, {"amplitude", "0.5"}}));
    document.graph.nodes.push_back(node(
        "in3", "talas.oscillator", {{"frequency", "3 hz"}, {"amplitude", "-0.25"}}));
    document.graph.nodes.push_back(node(
        "in4", "talas.oscillator", {{"frequency", "4 hz"}, {"amplitude", "0.125"}}));
    document.graph.connections.push_back(connection(
        "in1", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(connection(
        "in2", "audioOut", "mix", "in2"));
    document.graph.connections.push_back(connection(
        "in3", "audioOut", "mix", "in3"));
    document.graph.connections.push_back(connection(
        "in4", "audioOut", "mix", "in4"));
    document.graph.connections.push_back(connection(
        "mix", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "mix", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C23 Overlapping Mixer conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileOscillatorBankFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum",
        "talas.syntheticSpectrumFromArray",
        {
            {"nbrPartials", "3"},
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", true),
            {"envelope", "1"},
            textParameter("amplitudes", "0.75 -0.25 0.125"),
            textParameter("frequencies", "1 2 3"),
            textParameter("bandwidths", "0 0 0"),
        }));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.oscillatorBank",
        {
            {"nbrOscillators", "3"},
            {"bankSize", "2"},
            resourceParameter("wavetable", ""),
        }));
    document.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C24 Oscillator Bank conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC61SumOfSinesFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.sumOfSines",
        {
            {"onDuration", "4 s"},
            {"frequency0", "0 hz"},
            {"frequency1", "0 hz"},
            resourceParameter("analysis0", "Analysis/C61A"),
            {"analysisFrequency0", "1 hz"},
            resourceParameter("analysis1", "Analysis/C61B"),
            {"analysisFrequency1", "3 hz"},
            {"dbMorph", "0.25"},
            {"pchMorph", "0.5"},
            {"nbrPartials", "2"},
            {"bankSize", "2"},
            {"timeIndex", "0"},
            {"gate", "1"},
            booleanParameter("loop", false),
            {"loopStart", "0"},
            {"loopEnd", "0"},
            booleanParameter("ctrlTime", false),
            {"envelope", "0.5"},
        }));
    document.graph.connections.push_back(connection(
        "source", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings{32.0, 19};
    settings.tableResources = {
        {"Analysis/C61A", {1.0, 0.25}},
        {"Analysis/C61B", {0.5, -0.25}},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "C61 SumOfSines conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC63SumOfSinesAutomaticLoopFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.sumOfSines",
        {
            {"onDuration", "1 s"},
            {"frequency0", "1 hz"},
            {"frequency1", "1 hz"},
            resourceParameter("analysis0", "Analysis/C63"),
            {"analysisFrequency0", "1 hz"},
            {"analysisFrames0", "3"},
            resourceParameter("analysis1", ""),
            {"analysisFrequency1", "1 hz"},
            {"analysisFrames1", "1"},
            {"dbMorph", "0"},
            {"pchMorph", "0"},
            {"nbrPartials", "1"},
            {"bankSize", "1"},
            {"timeIndex", "0"},
            {"gate", "1"},
            booleanParameter("loop", true),
            {"loopStart", "0.25"},
            {"loopEnd", "0.75"},
            booleanParameter("ctrlTime", false),
            {"envelope", "1"},
        }));
    document.graph.connections.push_back(connection(
        "source", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings{32.0, 19};
    settings.tableResources = {
        {"Analysis/C63", {1.0, 0.5, 0.25}},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
        "C63 automatic SumOfSines conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileFormantBankFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum",
        "talas.syntheticSpectrumFromArray",
        {
            {"nbrPartials", "3"},
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", true),
            {"envelope", "1"},
            textParameter("amplitudes", "0.75 -0.25 0.125"),
            textParameter("frequencies", "1 2 3"),
            textParameter("bandwidths", "0 0 0"),
        }));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.formantBank",
        {
            {"frequency", "2 hz"},
            {"nbrFormants", "3"},
            {"bankSize", "2"},
            resourceParameter("impulseResponse", ""),
        }));
    document.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C25 Formant Bank conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileFormantBankOscillatorFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum",
        "talas.syntheticSpectrumFromArray",
        {
            {"nbrPartials", "4"},
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", true),
            {"envelope", "1"},
            textParameter("amplitudes", "0.5 0.75 -0.25 0.125"),
            textParameter("frequencies", "0.5 1 2 3"),
            textParameter("bandwidths", "0 0 0 0"),
        }));
    document.graph.nodes.push_back(node(
        "cascade",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "0.125"}}));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.formantBankOscillator",
        {
            {"frequency", "2 hz"},
            {"firstFormant", "2"},
            {"nbrFormants", "3"},
            resourceParameter("formantImpulse", ""),
            {"nbrImpulses", "1"},
            booleanParameter("allowFormantAtDC", false),
        }));
    document.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "cascade", "audioOut", "bank", "cascadeInput"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C26 Formant Bank Oscillator conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileSpectrumModifierFixture(
    std::string probability = "TrackNumber / TrackNumber")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum",
        "talas.syntheticSpectrumFromArray",
        {
            {"nbrPartials", "3"},
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", true),
            {"envelope", "1"},
            textParameter("amplitudes", "0.2 0.5 -0.25"),
            textParameter("frequencies", "100 200 300"),
            textParameter("bandwidths", "1 2 3"),
        }));
    document.graph.nodes.push_back(node(
        "modifier",
        "talas.spectrumModifier",
        {
            booleanParameter("select", false),
            {"loTrack", "1"},
            {"hiTrack", "3"},
            {"loFreq", "0 hz"},
            {"hiFreq", "1000 hz"},
            {"freqHysteresis", "0 hz"},
            {"freqScale", "0.5"},
            {"freqOffset", "10 hz"},
            {"loAmp", "-1"},
            {"hiAmp", "1"},
            {"ampHysteresis", "0"},
            {"ampScale", "-2"},
            {"ampOffset", "0.25"},
            {"probability", std::move(probability)},
            {"seed", "1"},
            booleanParameter("hearAll", false),
        }));
    document.graph.nodes.push_back(node(
        "selectControl", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "hearAllControl", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "seedControl", "talas.constant", {{"value", "-1"}}));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.oscillatorBank",
        {
            {"nbrOscillators", "3"},
            {"bankSize", "4096"},
            resourceParameter("wavetable", ""),
        }));
    document.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "modifier", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "selectControl", "value", "modifier", "select"));
    document.graph.connections.push_back(connection(
        "hearAllControl", "value", "modifier", "hearAll"));
    document.graph.connections.push_back(connection(
        "seedControl", "value", "modifier", "seed"));
    document.graph.connections.push_back(connection(
        "modifier", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C54 probability-one TrackNumber-profile Spectrum Modifier conformance fixture should compile: "
            + (compiled.diagnostics.empty()
                    ? std::string{}
                    : compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

void testSpectrumModifierRemainderProfileMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileSpectrumModifierFixture(
        "(TrackNumber - 1) rem: 2");
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C55 VariablesV2 remainder-profile live-selector/seed Spectrum Modifier conformance: "
            + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C55 arbitrary-partition conformance should exercise audible deterministic remainder-profile output");
}

talas::runtime::PreparedRuntimeGraph compileWarpedTimeIndexFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "level",
        "talas.level",
        {
            {"left", "0"},
            {"right", "0"},
            booleanParameter("noGain", false),
        }));
    document.graph.nodes.push_back(node(
        "index",
        "talas.warpedTimeIndex",
        {
            textParameter(
                "idealTimePoints0",
                "{0 s} {0.0625 s} {0.125 s}"),
            textParameter(
                "idealTimePoints1",
                "{0 s} {0.03125 s} {0.0625 s}"),
            textParameter(
                "currentTimePoints",
                "{0 s} {0.125 s} {0.25 s}"),
            {"trigger", "1"},
            {"rate", "1"},
            {"morph", "0"},
        }));
    document.graph.nodes.push_back(node(
        "morph",
        "talas.clock",
        {
            {"frequency", "8 hz"},
            {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }));
    document.graph.nodes.push_back(node(
        "carrier",
        "talas.oscillator",
        {{"frequency", "4 hz"}, {"amplitude", "0.5"}}));
    document.graph.connections.push_back(connection(
        "carrier", "audioOut", "level", "audioIn"));
    document.graph.connections.push_back(connection(
        "morph", "gate", "index", "morph"));
    document.graph.connections.push_back(connection(
        "index", "value", "level", "left"));
    document.graph.connections.push_back(connection(
        "level", "leftOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C30 Warped Time Index conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileSpectrumOnDiskFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.spectrumOnDisk",
        {
            {"frequency", "64 hz"},
            {"level", "0.25"},
            {"rateScale", "1"},
            resourceParameter("fileName", "Analysis/C31"),
            {"nbrPartials", "3"},
            {"firstPartial", "1"},
        }));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.oscillatorBank",
        {
            {"nbrOscillators", "3"},
            {"bankSize", "2"},
            resourceParameter("wavetable", ""),
        }));
    document.graph.connections.push_back(connection(
        "source", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources.push_back({
        "Analysis/C31",
        {1.0, 1.0, 0.0, 0.5, 2.0, 0.0, 0.25, 3.0, 0.0},
    });
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C31 Spectrum On Disk conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC58SpectrumInRamFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.spectrumInRam",
        {
            {"frequency", "64 hz"},
            {"level", "0.25"},
            {"timeIndex", "0"},
            resourceParameter("analysis", "Analysis/C58"),
            {"analysisFrames", "2"},
            {"nbrPartials", "2"},
            {"firstPartial", "1"},
        }));
    document.graph.nodes.push_back(node(
        "selector",
        "talas.spectrumTrackSelector",
        hertzSelector({
            {"track", "1"},
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })));
    document.graph.connections.push_back(connection(
        "source", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "selector", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources.push_back({
        "Analysis/C58",
        {
            1.0, 1.0, 0.0,
            0.5, 2.0, 0.0,
            0.5, 3.0, 0.0,
            0.25, 4.0, 0.0,
        },
    });
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C58 SpectrumInRAM conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC59SpectrumInRamDefaultsFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.spectrumInRam",
        {
            resourceParameter("analysis", "Analysis/C59"),
            {"analysisFrequency", "125 hz"},
        }));
    document.graph.nodes.push_back(node(
        "selector",
        "talas.spectrumTrackSelector",
        hertzSelector({
            {"track", "3"},
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })));
    document.graph.connections.push_back(connection(
        "source", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "selector", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources.push_back({
        "Analysis/C59",
        {
            0.2, 1.0, 0.1,
            0.4, 2.0, 0.2,
            0.6, 3.0, 0.3,
        },
    });
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C59 SpectrumInRAM default metadata conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC60SpectrumOnDiskDefaultsFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.spectrumOnDisk",
        {
            {"level", "0.25"},
            {"rateScale", "0.5"},
            {"trigger", "0"},
            resourceParameter("fileName", "Analysis/C60"),
            {"analysisFrames", "3"},
            {"analysisFrequency", "64 hz"},
            {"firstPartial", "1"},
        }));
    document.graph.nodes.push_back(node(
        "bank",
        "talas.oscillatorBank",
        {
            {"nbrOscillators", "3"},
            {"bankSize", "2"},
            resourceParameter("wavetable", ""),
        }));
    document.graph.connections.push_back(connection(
        "source", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection(
        "bank", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources.push_back({
        "Analysis/C60",
        {
            1.0, 1.0, 0.0,
            0.5, 2.0, 0.0,
            0.25, 3.0, 0.0,
            0.8, 1.5, 0.0,
            0.4, 2.5, 0.0,
            0.2, 3.5, 0.0,
            0.6, 2.0, 0.0,
            0.3, 3.0, 0.0,
            0.15, 4.0, 0.0,
        },
    });
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C60 Spectrum On Disk default-metadata conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileC57VoicedUnvoicedTransitionFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.spectrumOnDisk",
        {
            {"frequency", "100 hz"},
            {"level", "1"},
            {"rateScale", "1"},
            {"trigger", "0"},
            resourceParameter("fileName", "Analysis/C57"),
            {"analysisFrames", "5"},
            {"nbrPartials", "2"},
            {"firstPartial", "1"},
        }));
    document.graph.nodes.push_back(node(
        "voiced", "talas.spectrumVoicedUnvoiced"));
    document.graph.connections.push_back(connection(
        "source", "spectrumOut", "voiced", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "voiced", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "voiced", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 19;
    settings.tableResources.push_back({
        "Analysis/C57",
        {
            1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
            1.0, 1.0, 0.1, 0.5, 2.6, 0.2,
            1.0, 1.0, 0.1, 0.5, 2.6, 0.2,
            1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
            1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
        },
    });
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C57 Spectrum Voiced/Unvoiced conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileGaOscillatorsFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "ga",
        "talas.gaOscillators",
        {
            {"timeIndex", "0"},
            resourceParameter("analysis0", "Analysis/C32A"),
            resourceParameter("analysis1", "Analysis/C32B"),
            {"envelope", "0.25"},
            {"frequency", "32 hz"},
            {"morph", "0"},
        }));
    document.graph.nodes.push_back(node(
        "morph",
        "talas.clock",
        {
            {"frequency", "8 hz"},
            {"dutyCycle", "0.5"},
            booleanParameter("enabled", true),
        }));
    document.graph.connections.push_back(connection(
        "morph", "gate", "ga", "morph"));
    document.graph.connections.push_back(connection(
        "ga", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {
        {"Analysis/C32A", {1.0, 0.5, 0.25}},
        {"Analysis/C32B", {0.25, 0.5, 1.0}},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C32 GA Oscillators conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileGenericSourceRamSampleFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "source",
        "talas.genericSource",
        {
            textParameter("source", "ram sample"),
            booleanParameter("leftChannel", true),
            booleanParameter("rightChannel", true),
            resourceParameter("sample", "Samples/C33"),
            booleanParameter("autoloop", true),
            {"trigger", "1"},
            {"attackTime", "4 ms"},
            {"releaseTime", "0 s"},
            {"scale", "0.25"},
            {"frequency", "220 hz"},
        }));
    document.graph.connections.push_back(connection(
        "source", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "source", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {{
        "Samples/C33", {0.0, 1.0, 0.0, -1.0}}};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C33 Generic Source RAM sample conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileSampleFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "sample",
        "talas.sample",
        {
            {"frequency", "220 hz"},
            {"gate", "1"},
            resourceParameter("sample", "Samples/C34"),
            booleanParameter("setLoop", true),
            {"loopStart", "0"},
            {"loopEnd", "1"},
            booleanParameter("loopFade", false),
            {"start", "0"},
            {"end", "1"},
            booleanParameter("fromMemoryWriter", false),
            {"attackTime", "4 ms"},
            {"releaseTime", "0 s"},
            {"scale", "0.25"},
        }));
    document.graph.connections.push_back(connection(
        "sample", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {{
        "Samples/C34", {0.0, 1.0, 0.0, -1.0}}};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C34 Sample conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMultisampleFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "multisample",
        "talas.multisample",
        {
            {"frequency", "220 hz"},
            {"gate", "1"},
            textParameter("samples", "Samples/C35A, Samples/C35B Samples/C35A"),
            {"index", "1"},
            booleanParameter("loop", true),
            {"loopStart", "0"},
            {"loopEnd", "1"},
            {"attackTime", "4 ms"},
            {"releaseTime", "0 s"},
            {"scale", "0.25"},
        }));
    document.graph.connections.push_back(connection(
        "multisample", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {
        {"Samples/C35A", {0.0, 1.0, 0.0, -1.0}},
        {"Samples/C35B", {0.0, -0.5, 1.0, 0.5, 0.0}},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C35 Multisample conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileKeyMappedMultisampleFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "keymap",
        "talas.keyMappedMultisample",
        {
            {"frequency", "220 hz"},
            {"gate", "1"},
            {"velocity", "0.5"},
            resourceParameter("firstSample", "Samples/C36Root"),
            {"loFreq", "110 hz"},
            {"hiFreq", "880 hz"},
            choiceParameter("mapping", "byPitchRange"),
            {"attackTime", "4 ms"},
            {"releaseTime", "0 s"},
            {"scale", "0.5"},
            booleanParameter("loop", true),
            booleanParameter("fromDisk", false),
            booleanParameter("noTransposition", false),
        }));
    document.graph.connections.push_back(connection(
        "keymap", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {{
        "Samples/C36Root", {0.0, 1.0, 0.0, -1.0, 0.5}}};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C36 Key-Mapped Multisample conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMultifileDiskPlayerFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "multiDisk",
        "talas.multifileDiskPlayer",
        {
            textParameter(
                "fileNames",
                "Samples/C37A, Samples/C37B Samples/C37A"),
            {"rateScale", "0.5"},
            {"trigger", "1"},
            booleanParameter("gated", false),
            {"index", "1"},
        }));
    document.graph.connections.push_back(connection(
        "multiDisk", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    settings.tableResources = {
        {"Samples/C37A", {0.0, 1.0, 0.0, -1.0}},
        {"Samples/C37B", {0.0, -0.5, 1.0, 0.5, 0.0}},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C37 Multifile Disk Player conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMemoryWriterFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "writer",
        "talas.memoryWriter",
        {
            {"captureDuration", "20.5 ms"},
            booleanParameter("global", false),
            booleanParameter("cyclic", false),
            resourceParameter("recordingName", "Memory/C38Conformance"),
            booleanParameter("silent", false),
            {"trigger", "1"},
        }));
    document.graph.nodes.push_back(node(
        "source",
        "talas.oscillator",
        {{"frequency", "64 hz"}, {"amplitude", "0.5"}}));
    document.graph.connections.push_back(connection(
        "source", "audioOut", "writer", "audioIn"));
    document.graph.connections.push_back(connection(
        "writer", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1024.0;
    settings.maxBlockFrames = 19;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C38 Memory Writer conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileTypedGateAdsrFixture(
    bool connectedScalarGate = false,
    double legato = 0.0,
    bool captureAnalysis = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node(
        "env",
        "talas.adsr",
        {
            {"attackTime", "4 ms"},
            {"decayTime", "4 ms"},
            {"sustainLevel", "0.25"},
            {"releaseTime", "4 ms"},
            {"scale", "1.0"},
            {"gate", "0.0"},
            {"legato", std::to_string(legato)},
        }));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "137 hz"}, {"amplitude", "1.0"}}));
    if (connectedScalarGate)
    {
        document.graph.nodes.push_back(node(
            "gate",
            "talas.constant",
            {{"value", "!AdsrGate"}}));
        document.graph.connections.push_back(connection(
            "gate", "value", "env", "gate"));
    }
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection(
        "env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection(
        "vca", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 40;
    if (captureAnalysis)
    {
        settings.enableRealtimeAnalysisCapture = true;
        settings.realtimeAnalysisQueueCapacity = 8;
        settings.maxRealtimeEventTransactions = talas::runtime::maximumRealtimeAnalysisEventCount + 1;
        settings.realtimeAnalysisTargets.push_back({{}, {}, "env",
            std::string{talas::runtime::realtimeAnalysisTypedEventEndpoint},
            talas::runtime::RealtimeAnalysisKind::Oscilloscope,
            talas::runtime::RealtimeAnalysisPayloadKind::EventBatch, 1, "events-rollback"});
        settings.realtimeAnalysisTargets.push_back({{}, {}, "env", "value",
            talas::runtime::RealtimeAnalysisKind::Oscilloscope,
            talas::runtime::RealtimeAnalysisPayloadKind::ControlBlock, 1, "control-rollback"});
    }
    if (connectedScalarGate)
    {
        settings.eventValues.push_back(
            {"AdsrGate", 0.0, talas::expr::ValueDomain::Scalar});
    }
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
        "typed-gate ADSR conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileConnectedKernelFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node(
        "filter",
        "talas.filter",
        {{"frequency", "100 hz"}, {"q", "1.0"}, {"scale", "1.0"},
         {"feedback", "0.0"}, {"order", "2"}}));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "50 hz"}, {"amplitude", "0.5"}}));
    document.graph.nodes.push_back(node(
        "oscFrequency",
        "talas.linearRamp",
        {{"duration", "16 ms"}, {"start", "50"}, {"end", "150"}}));
    document.graph.nodes.push_back(node(
        "oscReset",
        "talas.pulseTrain",
        {{"period", "8 ms"}, booleanParameter("variableDutyCycle", true),
         {"dutyCycle", "0.25"}, {"gate", "1.0"}}));
    document.graph.nodes.push_back(node(
        "filterFrequency",
        "talas.linearRamp",
        {{"duration", "16 ms"}, {"start", "100"}, {"end", "300"}}));
    document.graph.nodes.push_back(node(
        "env",
        "talas.adsr",
        {{"attackTime", "2 ms"}, {"decayTime", "2 ms"},
         {"sustainLevel", "0.25"}, {"releaseTime", "2 ms"},
         {"scale", "1.0"}, {"gate", "0.0"}, {"legato", "0.0"}}));
    document.graph.nodes.push_back(node(
        "gate",
        "talas.pulseTrain",
        {{"period", "6 ms"}, booleanParameter("variableDutyCycle", true),
         {"dutyCycle", "0.5"}, {"gate", "1.0"}}));

    document.graph.connections.push_back(connection(
        "oscFrequency", "value", "osc", "frequency"));
    document.graph.connections.push_back(connection(
        "oscReset", "value", "osc", "reset"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "filter", "audioIn"));
    document.graph.connections.push_back(connection(
        "filterFrequency", "value", "filter", "frequency"));
    document.graph.connections.push_back(connection(
        "filter", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection(
        "gate", "value", "env", "gate"));
    document.graph.connections.push_back(connection(
        "env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection(
        "vca", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "connected-kernel conformance fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileM1UtilityDemonstratorFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node(
        "filter",
        "talas.filter",
        {{"frequency", "300 hz"}, {"q", "1.0"}, {"scale", "1.0"},
         {"feedback", "0.0"}, {"order", "2"}}));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "100 hz"}, {"amplitude", "0.5"}}));
    document.graph.nodes.push_back(node(
        "slew",
        "talas.slew",
        {{"riseTime", "5 ms"}, {"fallTime", "5 ms"}}));
    document.graph.nodes.push_back(node(
        "quantizer",
        "talas.quantizer",
        {{"stepSize", "50"}, {"offset", "100"}}));
    document.graph.nodes.push_back(node(
        "divider",
        "talas.clockDivider",
        {{"division", "2"}, {"steps", "4"}, {"reset", "0.0"}}));
    document.graph.nodes.push_back(node(
        "logic",
        "talas.logic",
        {choiceParameter("operation", "and"), {"left", "0.0"}, {"right", "1.0"}}));
    document.graph.nodes.push_back(node(
        "gate",
        "talas.gate",
        {{"lowThreshold", "0.25"}, {"highThreshold", "0.75"}}));
    document.graph.nodes.push_back(node(
        "clock",
        "talas.clock",
        {{"frequency", "100 hz"}, {"dutyCycle", "0.5"},
         booleanParameter("enabled", true)}));
    document.graph.nodes.push_back(node(
        "env",
        "talas.adsr",
        {{"attackTime", "2 ms"}, {"decayTime", "2 ms"},
         {"sustainLevel", "0.5"}, {"releaseTime", "3 ms"},
         {"scale", "1.0"}, {"gate", "0.0"}, {"legato", "0.0"}}));

    document.graph.connections.push_back(connection("clock", "gate", "gate", "value"));
    document.graph.connections.push_back(connection("gate", "gate", "logic", "left"));
    document.graph.connections.push_back(connection("logic", "gate", "divider", "clock"));
    document.graph.connections.push_back(connection("divider", "step", "quantizer", "value"));
    document.graph.connections.push_back(connection("quantizer", "value", "slew", "value"));
    document.graph.connections.push_back(connection("slew", "value", "osc", "frequency"));
    document.graph.connections.push_back(connection("osc", "audioOut", "filter", "audioIn"));
    document.graph.connections.push_back(connection("filter", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("divider", "gate", "env", "gate"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    const auto serialized = talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(serialized);
    require(loaded.ok(), "M1 utility demonstrator should reload from its project JSON");
    require(
        talas::project::serializeProjectDocument(loaded.document) == serialized,
        "M1 utility demonstrator project JSON should remain canonical after reload");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    auto compiled = talas::runtime::compileProject(
        loaded.document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "M1 utility demonstrator should compile");
    return std::move(*compiled.graph);
}

template <std::size_t Size>
void requireLegacyBaseline(
    const talas::runtime::RuntimeConformanceResult& result,
    const std::array<double, Size>& expected,
    const std::string& name)
{
    require(result.ok(), name + ": " + result.message);
    require(
        result.offlineChannels.size() == 1
            && result.offlineChannels.front().size() == expected.size(),
        name + " should retain one complete mono baseline");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(
                result.offlineChannels.front()[frame]
                - expected[frame])
                <= 1.0e-12,
            name + " legacy output should remain stable at frame "
                + std::to_string(frame));
    }
}

void testOscillatorLegacyBaselineMatchesAcrossContexts()
{
    const auto graph = compileOscillatorFixture();
    constexpr std::array<std::size_t, 5> realtimeBlocks = {
        3,
        5,
        1,
        7,
        16,
    };
    const auto result =
        talas::runtime::compareOfflineAndPreparedRealtime(
            graph,
            32,
            realtimeBlocks,
            {.absolute = 1.0e-12, .relative = 1.0e-12});

    require(result.ok(), result.message);
    require(
        result.offlineChannels.size() == 1
            && result.realtimeChannels.size() == 1,
        "oscillator conformance fixture should remain mono");
    require(
        result.offlineChannels.front().size() == 32,
        "conformance capture should retain every rendered frame");

    constexpr std::array<double, 16> legacyCycle = {
        0.0,
        0.1913417161825449,
        0.3535533905932737,
        0.4619397662556434,
        0.5,
        0.4619397662556434,
        0.3535533905932738,
        0.1913417161825449,
        0.0,
        -0.1913417161825448,
        -0.3535533905932737,
        -0.4619397662556433,
        -0.5,
        -0.4619397662556434,
        -0.3535533905932738,
        -0.1913417161825452,
    };
    for (std::size_t frame = 0; frame < 32; ++frame)
    {
        require(
            std::abs(
                result.offlineChannels.front()[frame]
                - legacyCycle[frame % legacyCycle.size()])
                <= 1.0e-12,
            "oscillator legacy baseline should remain stable");
    }
}

void testC66ThresholdMatchesAcrossIrregularBoundaries()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("vca", "talas.vca"),
        node("carrier", "talas.oscillator", {
            {"frequency", "0.25 hz"}, {"amplitude", "1"},
        }),
        node("control", "talas.lfo", {
            {"frequency", "1 hz"}, {"minimum", "0.2"},
            {"maximum", "0.8"},
        }),
        node("threshold", "talas.threshold", {
            {"threshold", "0.5"}, {"hysteresis", "0.4"},
        }),
    };
    document.graph.connections = {
        connection("control", "value", "threshold", "value"),
        connection("carrier", "audioOut", "vca", "audioIn"),
        connection("threshold", "value", "vca", "gain"),
        connection("vca", "audioOut", "out", "audioIn"),
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 3});
    require(compiled.ok(), "C66 Threshold conformance fixture should compile");
    constexpr std::array<std::size_t, 4> partitions{1, 2, 3, 2};
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        *compiled.graph,
        8,
        partitions,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C66 Threshold should be bit-identical across irregular boundaries: "
            + result.message);
}

void testC67TimeControlMatchesAcrossIrregularBoundaries()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        // The replay that projects before format 45 keep.
        node("time", "talas.timeControl", {{"rate", "0.5"},
            talas::project::ProjectParameterExpression{"rateChanges", std::nullopt, "legacyPlaybackSpeed"}}),
        node("source", "talas.oscillator", {
            {"frequency", "0.5 hz"}, {"amplitude", "1"},
        }),
    };
    document.graph.connections = {
        connection("source", "audioOut", "time", "audioIn"),
        connection("time", "audioOut", "out", "audioIn"),
    };
    auto settings = talas::runtime::RenderSettings{8.0, 3};
    settings.maxRealtimeHistoryFrames = 16;
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C67 Time Control conformance fixture should compile");
    constexpr std::array<std::size_t, 4> partitions{1, 2, 3, 2};
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        *compiled.graph,
        8,
        partitions,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C67 Time Control should be bit-identical across irregular boundaries: "
            + result.message);
}

void testNativeComplexStftMatchesAcrossPreparedContexts()
{
    constexpr std::size_t frameCount = 321;
    constexpr std::size_t latency = 63;
    const std::array<std::size_t, 6> pattern{1, 17, 3, 11, 5, 13};
    std::vector<std::size_t> partitions;
    std::size_t remaining = frameCount;
    std::size_t index = 0;
    while (remaining != 0)
    {
        const auto count = std::min(
            remaining,
            pattern[index % pattern.size()]);
        partitions.push_back(count);
        remaining -= count;
        ++index;
    }

    for (const auto& [window, hop] :
         std::vector<std::pair<std::string, std::string>>{
             {"rectangular", "64"},
             {"hann", "32"},
             {"hann", "16"},
         })
    {
        const auto graph = compileNativeStftFixture(hop, window);
        const auto result =
            talas::runtime::compareOfflineAndPreparedRealtime(
                graph,
                frameCount,
                partitions,
                {.absolute = 1.0e-9, .relative = 1.0e-9});
        require(result.ok(),
                "native STFT offline/prepared conformance should pass: "
                    + result.message);
        require(result.offlineChannels.size() == 1
                    && result.realtimeChannels.size() == 1,
                "native STFT conformance should retain one audio channel");
        for (std::size_t frame = 0; frame < latency; ++frame)
        {
            require(std::abs(result.offlineChannels.front()[frame])
                        <= 1.0e-12,
                    "native STFT conformance should retain exact N-1 startup silence");
        }
    }
}

void testOfflineComplexStftWholeCallRollback()
{
    using namespace talas::runtime;
    for (const auto& [window, hop] : std::array{
             std::pair{"rectangular", "64"}, std::pair{"hann", "32"}, std::pair{"hann", "16"}})
    {
        const auto graph = compileNativeStftFixture(hop, window, true);
        OfflineMidiVoiceEvent invalid;
        invalid.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
        invalid.frame = 251;
        invalid.batchId = invalid.sequence = 1;
        invalid.eventValueName = "Divisor";
        invalid.value = 0.0;
        OfflineRenderSession reference(graph);
        require(reference.scheduleMidiVoiceEvents(std::span{&invalid, 1}) && reference.render(193).ok(),
            "STFT reference must commit the same prefix");
        const auto uninterrupted = reference.render(58);
        require(uninterrupted.ok() && uninterrupted.mono.size() == 58,
            "STFT reference must render the valid continuation");
        for (std::size_t index = 0; index != uninterrupted.mono.size(); ++index)
            require(std::abs(uninterrupted.mono[index] - std::sin(2.0 * std::numbers::pi
                    * static_cast<double>(193 + index - 63) / 64.0)) < 1.0e-9,
                "uninterrupted STFT must first satisfy the independent delayed sine oracle");
        OfflineRenderSession session(graph);
        require(session.scheduleMidiVoiceEvents(std::span{&invalid, 1}) && session.render(193).ok(),
            "STFT rollback fixture must commit an audible prefix");
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = session.render(75);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty(),
                "late STFT failure must publish no partial samples");
            require(session.render(0).ok(), "empty STFT retry must preserve the committed boundary");
        }
        const auto retry = session.render(58);
        require(retry.ok() && retry.mono.size() == 58,
            "STFT retry must restore source sequence, timestamps and synthesis history");
        for (std::size_t index = 0; index != retry.mono.size(); ++index)
        {
            const auto expected = std::sin(2.0 * std::numbers::pi
                * static_cast<double>(193 + index - 63) / 64.0);
            require(std::abs(retry.mono[index] - expected) < 1.0e-9,
                "STFT retry must reproduce the independent N-1 delayed sine oracle");
        }
        require(!session.render(1).ok(), "STFT retry must leave the invalid control queued at frame 251");
    }
}

void testOfflineComplexStftAllocationRollback()
{
    using namespace talas::runtime;
    const auto graph = compileNativeStftFixture("16", "hann", true);
    OfflineMidiVoiceEvent update;
    update.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    update.frame = 210;
    update.batchId = update.sequence = 1;
    update.eventValueName = "Divisor";
    update.value = 2.0;
    const auto prepare = [&] {
        OfflineRenderSession session(graph);
        require(session.scheduleMidiVoiceEvents(std::span{&update, 1}) && session.render(193).ok(),
            "STFT allocation fixture must commit a prefix with a completed spectral frame");
        return session;
    };
    auto reference = prepare();
    OfflineRenderResult expected;
    std::size_t allocationPoints = 0;
    {
        ScopedOfflineAllocationFault counting;
        expected = reference.render(150);
        allocationPoints = offlineAllocationCount;
    }
    require(expected.ok() && expected.mono.size() == 150 && allocationPoints != 0,
        "STFT allocation reference must render across the queued control");
    for (std::size_t index = 0; index != expected.mono.size(); ++index)
    {
        const auto sourceFrame = 193 + index - 63;
        const auto gain = sourceFrame < 210 ? 1.0 : 0.5;
        require(std::abs(expected.mono[index] - gain * std::sin(2.0 * std::numbers::pi
                * static_cast<double>(sourceFrame) / 64.0)) < 1.0e-9,
            "STFT allocation reference must satisfy an independent delayed gain/sine oracle");
    }
    for (std::size_t point = 0; point != allocationPoints; ++point)
    {
        auto session = prepare();
        OfflineRenderResult faulted;
        {
            ScopedOfflineAllocationFault fault(point);
            faulted = session.render(150);
        }
        require(offlineAllocationFaultTriggered && !faulted.ok() && faulted.mono.empty()
            && faulted.right.empty() && faulted.channels.empty() && faulted.midiEvents.empty()
            && faulted.globalControllerEvents.empty(),
            "STFT capture/allocation failure must return no partial output");
        const auto retry = session.render(150);
        require(retry.ok() && retry.mono.size() == expected.mono.size(),
            "STFT allocation failure must preserve a retryable committed boundary");
        for (std::size_t index = 0; index != expected.mono.size(); ++index)
            require(std::abs(retry.mono[index] - expected.mono[index]) < 1.0e-12,
                "STFT allocation retry must preserve the exact spectral history and pending control");
    }
    std::cout << "P2 STFT allocation failure/retry points: " << allocationPoints << '\n';
}

talas::runtime::PreparedRuntimeGraph compilePitchRollbackFixture(std::string_view family)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {node("out", "talas.audioOutput"), node("gain", "talas.vca"),
        node("invalid", "talas.constant", {{"value", "1 / !Divisor"}})};
    talas::runtime::RenderSettings settings{48000.0, 257};
    const auto initialControl = family == "dynamic" ? 0.0 : family == "mono" ? 100.0 : 1.0;
    settings.eventValues = {{"Divisor", 1.0}, {"Control", initialControl}};
    std::string controlPort;
    if (family == "static" || family == "dynamic")
    {
        document.graph.nodes.push_back(node("pitch", "talas.simplePitchShifter", {
            {"interval", family == "static" ? "7" : "0"},
            {"minInputPitch", "20 hz"}, {"maxInputPitch", "2000 hz"}}));
        if (family == "dynamic") controlPort = "interval";
    }
    else if (family == "mono")
    {
        document.graph.nodes.push_back(node("pitch", "talas.monotonizer", {
            {"frequency", "100 hz"}, {"minInputPitch", "100 hz"}, {"maxInputPitch", "100 hz"}}));
        controlPort = "frequency";
    }
    else if (family == "frequency")
    {
        document.graph.nodes.push_back(node("pitch", "talas.frequencyScale", {
            {"frequencyScale", "1"}, {"maxScale", "2"},
            resourceParameter("window", "Private"), {"delay", "0"}}));
        // 100 hz, as a fraction of half the sample rate.
        document.graph.nodes.push_back(node("tracker", "talas.constant", {{"value", "100 / 24000"}}));
        document.graph.connections.push_back(connection("tracker", "value", "pitch", "freqTracker"));
        controlPort = "frequencyScale";
    }
    else
    {
        require(family == "timeFrequency", "pitch rollback family must be explicit");
        document.graph.nodes.push_back(node("pitch", "talas.timeFrequencyScale", {
            {"frequencyScale", "1"}, {"rate", "1"}, {"gate", "1"},
            {"minInputFreq", "20 hz"}, {"maxInputFreq", "2000 hz"},
            {"maxFreqScale", "2"}, {"detectors", "10"}, booleanParameter("fromDisk", false),
            resourceParameter("sample", "Samples/PitchRollback"), booleanParameter("fromMemoryWriter", false)}));
        std::vector<double> samples(32768);
        for (std::size_t frame = 0; frame != samples.size(); ++frame)
            samples[frame] = std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(frame) / 48000.0);
        settings.tableResources = {{"Samples/PitchRollback", std::move(samples)}};
        controlPort = "frequencyScale";
    }
    if (family != "timeFrequency")
    {
        document.graph.nodes.push_back(node("source", "talas.oscillator", {
            {"frequency", "100 hz"}, {"amplitude", "1"}}));
        document.graph.connections.push_back(connection("source", "audioOut", "pitch", "audioIn"));
    }
    if (!controlPort.empty())
    {
        document.graph.nodes.push_back(node("control", "talas.constant", {{"value", "!Control"}}));
        document.graph.connections.push_back(connection("control", "value", "pitch", controlPort));
    }
    document.graph.connections.push_back(connection("pitch", "audioOut", "gain", "audioIn"));
    document.graph.connections.push_back(connection("invalid", "value", "gain", "gain"));
    document.graph.connections.push_back(connection("gain", "audioOut", "out", "audioIn"));
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "pitch rollback fixture must compile: " + std::string(family)
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

void testOfflinePitchFamiliesWholeCallRollback()
{
    using namespace talas::runtime;
    for (const auto family : {"static", "dynamic", "mono", "frequency", "timeFrequency"})
    {
        const auto graph = compilePitchRollbackFixture(family);
        const auto sourceWindow = talas::dsp::timeFrequencyScaleAnalysisFrameSize(48000.0, 20.0);
        // Cross a real synthesis boundary during the failed call, after audible
        // output and while the delayed connected-control transition is active.
        const auto prefixFrames = std::string_view(family) == "timeFrequency"
            ? sourceWindow - 1 + 3 * (sourceWindow / 4) - 113
            : std::size_t{4209};
        std::array<OfflineMidiVoiceEvent, 2> events{};
        events[0].kind = events[1].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
        events[0].frame = 4096;
        events[0].batchId = events[0].sequence = 1;
        events[0].eventValueName = "Control";
        events[0].value = std::string_view(family) == "dynamic" ? 7.0
            : std::string_view(family) == "mono" ? 150.0 : 1.5;
        events[1].frame = prefixFrames + 777;
        events[1].batchId = events[1].sequence = 2;
        events[1].eventValueName = "Divisor";
        events[1].value = 0.0;
        OfflineRenderSession reference(graph), candidate(graph);
        require(reference.scheduleMidiVoiceEvents(events) && candidate.scheduleMidiVoiceEvents(events)
            && reference.render(prefixFrames).ok() && candidate.render(prefixFrames).ok(),
            "pitch rollback must commit a prefix after a live interval change: " + std::string(family));
        const auto expected = reference.render(777);
        require(expected.ok() && expected.mono.size() == 777
            && std::ranges::any_of(expected.mono, [](double sample) { return std::abs(sample) > 1.0e-4; }),
            "pitch rollback reference must exercise audible spectral state: " + std::string(family));
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = candidate.render(1000);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty(),
                "late pitch failure must publish no partial audio: " + std::string(family));
            require(candidate.render(0).ok(), "empty pitch call must preserve pending state");
        }
        const auto retry = candidate.render(777);
        require(retry.ok() && retry.mono.size() == expected.mono.size(),
            "pitch retry must restore timestamps and queued controls: " + std::string(family));
        for (std::size_t frame = 0; frame != expected.mono.size(); ++frame)
            require(std::abs(retry.mono[frame] - expected.mono[frame]) <= 1.0e-12,
                "pitch retry must restore phase, overlap, transition and source cursor: " + std::string(family));
        require(!candidate.render(1).ok(), "invalid pitch control must remain queued at the committed boundary");
    }
}

void testOfflinePitchAllocationRollback()
{
    using namespace talas::runtime;
    const auto graph = compilePitchRollbackFixture("dynamic");
    OfflineMidiVoiceEvent update;
    update.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    update.frame = 1024;
    update.batchId = update.sequence = 1;
    update.eventValueName = "Control";
    update.value = 7.0;
    const auto prepare = [&] {
        OfflineRenderSession session(graph);
        require(session.scheduleMidiVoiceEvents(std::span{&update, 1}) && session.render(1201).ok(),
            "pitch allocation fixture must commit an in-progress interval transition");
        return session;
    };
    auto reference = prepare();
    OfflineRenderResult expected;
    std::size_t allocationPoints = 0;
    {
        ScopedOfflineAllocationFault counting;
        expected = reference.render(600);
        allocationPoints = offlineAllocationCount;
    }
    require(expected.ok() && expected.mono.size() == 600 && allocationPoints != 0
        && std::ranges::any_of(expected.mono, [](double sample) { return std::abs(sample) > 1.0e-4; }),
        "pitch allocation reference must exercise audible transition state");
    for (std::size_t point = 0; point != allocationPoints; ++point)
    {
        auto session = prepare();
        OfflineRenderResult faulted;
        {
            ScopedOfflineAllocationFault fault(point);
            faulted = session.render(600);
        }
        require(offlineAllocationFaultTriggered && !faulted.ok() && faulted.mono.empty()
            && faulted.right.empty() && faulted.channels.empty() && faulted.midiEvents.empty()
            && faulted.globalControllerEvents.empty(),
            "pitch snapshot allocation failure must return no partial output");
        const auto retry = session.render(600);
        require(retry.ok() && retry.mono.size() == expected.mono.size(),
            "pitch snapshot allocation failure must preserve the committed boundary");
        for (std::size_t frame = 0; frame != expected.mono.size(); ++frame)
            require(std::abs(retry.mono[frame] - expected.mono[frame]) <= 1.0e-12,
                "pitch snapshot allocation retry must preserve phase, history and mix transition");
    }
    std::cout << "P2 pitch allocation failure/retry points: " << allocationPoints << '\n';
}

talas::runtime::PreparedRuntimeGraph compileLiveAnalysisRollbackFixture(bool fundamentalOnly, bool captureAnalysis = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"), node("gain", "talas.vca"),
        node("invalid", "talas.constant", {{"value", "1 / !Divisor"}}),
        node("selector", "talas.spectrumTrackSelector", hertzSelector({{"track", "1"},
            booleanParameter("amp", true), booleanParameter("freq", true)})),
        node("bridge", "talas.partialSpectrumLegacyHoldV1"),
        node("spectrum", "talas.liveSpectralAnalysis", {
            {"lowestAnalyzedFreq", "1 hz"}, {"ampScale", "!Scale"}, {"freqScale", "!Frequency"},
            textParameter("response", ""), booleanParameter("harmonic", false),
            {"unpitchedOnly", "0"}, {"unpitchedThreshold", "0"}, {"trackedHarmonic", "1"},
            {"lowFreq", "1 hz"}, {"highFreq", "32 hz"}, {"initFreq", "8 hz"},
            booleanParameter("fundamentalOnly", fundamentalOnly),
            choiceParameter("tracks", "legacyPeaks")}),
        node("source", "talas.oscillator", {{"frequency", "7.3 hz"}, {"amplitude", "!SourceGain"}}),
    };
    document.graph.connections = {
        connection("source", "audioOut", "spectrum", "audioIn"),
        connection("spectrum", "spectrumOut", "bridge", "spectrumIn"),
        connection("bridge", "spectrumOut", "selector", "spectrumIn"),
        connection("selector", "leftOut", "gain", "audioIn"),
        connection("invalid", "value", "gain", "gain"),
        connection("gain", "audioOut", "out", "audioIn"),
        connection("selector", "rightOut", "out", "rightIn"),
    };
    talas::runtime::RenderSettings settings{64.0, 17};
    settings.eventValues = {{"Divisor", 1.0}, {"Scale", 1.0}, {"Frequency", 1.0}, {"SourceGain", 1.0}};
    if (captureAnalysis)
    {
        settings.enableRealtimeAnalysisCapture = true;
        settings.realtimeAnalysisQueueCapacity = 8;
        settings.realtimeAnalysisTargets.push_back({{}, {}, "spectrum", "spectrumOut",
            talas::runtime::RealtimeAnalysisKind::Spectrum,
            talas::runtime::RealtimeAnalysisPayloadKind::PartialSpectrum, 1, "partials-rollback"});
    }
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "live analysis rollback fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

void testOfflineLiveAnalysisWholeCallRollback()
{
    using namespace talas::runtime;
    for (const auto fundamentalOnly : {false, true})
    {
        const auto graph = compileLiveAnalysisRollbackFixture(fundamentalOnly);
        std::array<OfflineMidiVoiceEvent, 4> events{};
        for (std::size_t index = 0; index != events.size(); ++index)
        {
            events[index].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
            events[index].batchId = events[index].sequence = index + 1;
        }
        events[0].frame = 144; events[0].eventValueName = "Scale"; events[0].value = 0.5;
        events[1].frame = 210; events[1].eventValueName = "Frequency"; events[1].value = 1.5;
        events[2].frame = 240; events[2].eventValueName = "SourceGain"; events[2].value = 0.25;
        events[3].frame = 315; events[3].eventValueName = "Divisor"; events[3].value = 0.0;
        OfflineRenderSession reference(graph), candidate(graph);
        require(reference.scheduleMidiVoiceEvents(events) && candidate.scheduleMidiVoiceEvents(events)
            && reference.render(193).ok() && candidate.render(193).ok(),
            "live analysis must commit a prefix containing pending scaled-frame metadata");
        const auto expected = reference.render(122);
        require(expected.ok() && expected.channels.size() == 2
            && std::ranges::any_of(expected.mono, [](double value) { return std::abs(value) > 1.0e-4; }),
            "live analysis continuation reference must contain actual partial output");
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = candidate.render(141);
            require(!failed.ok() && failed.mono.empty() && failed.right.empty() && failed.channels.empty(),
                "late live analysis failure must publish no partial stereo result");
            require(candidate.render(0).ok(), "empty live analysis retry must preserve the committed state");
        }
        const auto retry = candidate.render(122);
        require(retry.ok() && retry.channels.size() == expected.channels.size(),
            "live analysis retry must recover its timestamps and pending control stream");
        for (std::size_t channel = 0; channel != expected.channels.size(); ++channel)
        {
            require(retry.channels[channel].size() == expected.channels[channel].size(),
                "live analysis retry must restore complete ordered channels");
            for (std::size_t frame = 0; frame != expected.channels[channel].size(); ++frame)
                require(std::abs(retry.channels[channel][frame] - expected.channels[channel][frame]) <= 1.0e-12,
                    "live analysis retry must restore frame metadata, payloads, held values and scale history");
        }
        require(!candidate.render(1).ok(), "invalid live analysis input must remain queued at frame 315");
    }
}

void testOfflinePartialSequenceStorageIdentity()
{
    using namespace talas::runtime;
    const auto graph = compileLiveAnalysisRollbackFixture(false);
    RealtimeRenderSession session(graph);
    require(session.ready(), "partial checkpoint session must prepare");
    std::array<double, 17> left{}, right{};
    const auto render = [&](std::size_t count) {
        require(session.render(std::span(left).first(count), std::span(right).first(count)),
            "partial checkpoint fixture must render");
    };
    {
        auto transaction = RealtimeRenderSessionTestAccess::beginOfflineTransaction(session);
        require(bool(transaction), "live partial buffers must admit an outer checkpoint");
        for (int block = 0; block != 5; ++block) render(17);
    }
    require(session.inspectPartialSpectrum("spectrum", "spectrumOut").empty(),
        "fresh-boundary rollback must restore the empty partial sequence");
    for (int block = 0; block != 11; ++block) render(17);
    render(6); // Frame 191 is emitted into the final callback's sequence.
    const auto before = session.inspectPartialSpectrum("spectrum", "spectrumOut");
    require(!before.empty() && !before.front().partials.empty(),
        "partial checkpoint must observe a populated baseline sequence");
    std::vector<talas::dsp::PartialSpectrumFrame> metadata(before.begin(), before.end());
    std::vector<std::vector<talas::dsp::PartialSpectrumPoint>> payloads;
    for (const auto& frame : before) payloads.emplace_back(frame.partials.begin(), frame.partials.end());
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        {
            auto transaction = RealtimeRenderSessionTestAccess::beginOfflineTransaction(session);
            require(bool(transaction), "populated partial sequence must be checkpointable");
            for (int block = 0; block != 6; ++block) render(17);
        }
        const auto restored = session.inspectPartialSpectrum("spectrum", "spectrumOut");
        require(restored.data() == before.data() && restored.size() == metadata.size(),
            "partial rollback must retain the original frame storage and frame count");
        for (std::size_t index = 0; index != restored.size(); ++index)
        {
            const auto& frame = restored[index];
            require(frame.formatVersion == metadata[index].formatVersion
                && frame.sourceTimestamp == metadata[index].sourceTimestamp
                && frame.sourceSequence == metadata[index].sourceSequence
                && frame.partials.data() == metadata[index].partials.data()
                && frame.partials.size() == payloads[index].size(),
                "partial rollback must restore exact metadata into original payload storage");
            for (std::size_t partial = 0; partial != frame.partials.size(); ++partial)
            {
                const auto& a = frame.partials[partial]; const auto& b = payloads[index][partial];
                require(a.hasTrackId == b.hasTrackId && a.trackId == b.trackId && a.amplitude == b.amplitude
                    && a.frequencyHz == b.frequencyHz && a.bandwidthHz == b.bandwidthHz,
                    "partial checkpoint payload must not alias overwritten live storage");
            }
        }
    }
}

void testOfflineLiveAnalysisAllocationRollback()
{
    using namespace talas::runtime;
    const auto graph = compileLiveAnalysisRollbackFixture(false);
    std::array<OfflineMidiVoiceEvent, 3> events{};
    for (std::size_t index = 0; index != events.size(); ++index)
    {
        events[index].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
        events[index].batchId = events[index].sequence = index + 1;
    }
    events[0].frame = 144; events[0].eventValueName = "Scale"; events[0].value = 0.5;
    events[1].frame = 210; events[1].eventValueName = "Frequency"; events[1].value = 1.5;
    events[2].frame = 240; events[2].eventValueName = "SourceGain"; events[2].value = 0.25;
    const auto prepare = [&] {
        OfflineRenderSession session(graph);
        require(session.scheduleMidiVoiceEvents(events) && session.render(193).ok(),
            "analysis allocation fixture must commit a populated partial sequence and pending metadata");
        return session;
    };
    auto reference = prepare();
    OfflineRenderResult expected;
    std::size_t allocationPoints = 0;
    {
        ScopedOfflineAllocationFault counting;
        expected = reference.render(122);
        allocationPoints = offlineAllocationCount;
    }
    require(expected.ok() && expected.channels.size() == 2 && allocationPoints != 0,
        "analysis allocation reference must retain both ordered outputs");
    for (std::size_t point = 0; point != allocationPoints; ++point)
    {
        auto session = prepare();
        OfflineRenderResult faulted;
        {
            ScopedOfflineAllocationFault fault(point);
            faulted = session.render(122);
        }
        require(offlineAllocationFaultTriggered && !faulted.ok() && faulted.mono.empty()
            && faulted.right.empty() && faulted.channels.empty() && faulted.midiEvents.empty()
            && faulted.globalControllerEvents.empty(),
            "analysis snapshot allocation failure must publish no partial result");
        const auto retry = session.render(122);
        require(retry.ok() && retry.channels.size() == expected.channels.size(),
            "analysis snapshot failure must preserve a retryable committed boundary");
        for (std::size_t channel = 0; channel != expected.channels.size(); ++channel)
        {
            require(retry.channels[channel].size() == expected.channels[channel].size(),
                "analysis allocation retry must preserve each channel length");
            for (std::size_t frame = 0; frame != expected.channels[channel].size(); ++frame)
                require(std::abs(retry.channels[channel][frame] - expected.channels[channel][frame]) <= 1.0e-12,
                    "analysis allocation retry must retain metadata, partial payloads and pending input");
        }
    }
    std::cout << "P2 live analysis allocation failure/retry points: " << allocationPoints << '\n';
}

talas::runtime::PreparedRuntimeGraph compilePartialTrackRollbackFixture(std::size_t analysisCapacity = 0)
{
    const talas::project::PartialTrackAsset asset{64.0, 160, {
        {1, 0, {{3, 0.5, 7.3, 0.0}}},
        {1, 32, {{3, 0.25, 11.1, 1.0}, {9, 0.1, 13.7, 0.2}}},
        {1, 64, {{3, 0.8, 5.4, 2.0}, {9, 0.2, 17.0, 0.0}}},
        {2, 80, {{5, 0.6, 9.2, 1.0}}},
        {2, 112, {{5, 0.2, 13.1, 0.0}}},
        {3, 136, {}},
    }};
    const auto encoded = talas::project::encodePartialTrackAsset(asset);
    require(encoded.ok(), "partial-track rollback asset must encode: "
        + (encoded.error ? encoded.error->message : std::string{}));
    const std::string hash(64, 'd');
    talas::project::ProjectDocument document;
    document.assets.push_back({"Analysis/Rollback", talas::project::ProjectAssetKind::PartialSpectrum,
        "analysis/rollback.talaspartial", talas::project::ProjectAssetDisposition::Referenced,
        hash, 0, asset.sampleRate, asset.sourceFrameCount,
        std::string{talas::project::partialTrackAssetEncoding}, 7, {}});
    document.graph.nodes = {node("out", "talas.audioOutput"), node("gain", "talas.vca"),
        node("control", "talas.constant", {{"value", "!Gain / !Divisor"}}),
        node("partials", "talas.partialTrackPlayerV1", {
            resourceParameter("partialTrack", "Analysis/Rollback"), {"gain", "0.75"}})};
    document.graph.connections = {
        connection("partials", "audioOut", "gain", "audioIn"),
        connection("control", "value", "gain", "gain"),
        connection("gain", "audioOut", "out", "audioIn"),
    };
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Analysis/Rollback";
    snapshot->kind = talas::core::ResourceSnapshotKind::PartialSpectrum;
    snapshot->generation = 7;
    snapshot->contentHash = hash;
    snapshot->sampleRate = asset.sampleRate;
    snapshot->frameCount = asset.sourceFrameCount;
    snapshot->encoding = std::string{talas::project::partialTrackAssetEncoding};
    snapshot->partialTrackBytes = encoded.bytes;
    talas::runtime::RenderSettings settings{64.0, 17};
    settings.resourceSnapshots = {std::move(snapshot)};
    settings.eventValues = {{"Gain", 1.0}, {"Divisor", 1.0}};
    if (analysisCapacity != 0)
    {
        settings.enableRealtimeAnalysisCapture = true;
        settings.realtimeAnalysisQueueCapacity = analysisCapacity;
        settings.realtimeAnalysisTargets.push_back({{}, {}, "gain", "audioOut",
            talas::runtime::RealtimeAnalysisKind::Oscilloscope,
            talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock, 1, "rollback-audio"});
    }
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "partial-track rollback fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

void testOfflineAnalysisPublicationWholeCallRollback()
{
    using namespace talas::runtime;
    for (const std::size_t capacity : {1, 8})
    {
        const auto graph = compilePartialTrackRollbackFixture(capacity);
        const auto referenceGraph = compilePartialTrackRollbackFixture(capacity);
        const auto capture = graph.analysisCapture();
        const auto referenceCapture = referenceGraph.analysisCapture();
        require(capture && referenceCapture && capture->bindStage(701)
            && referenceCapture->bindStage(701), "offline analysis must bind independent captures");
        std::array<OfflineMidiVoiceEvent, 2> events{};
        events[0].kind = events[1].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
        events[0].frame = 57; events[0].batchId = events[0].sequence = 1;
        events[0].eventValueName = "Gain"; events[0].value = 0.5;
        events[1].frame = 121; events[1].batchId = events[1].sequence = 2;
        events[1].eventValueName = "Divisor"; events[1].value = 0.0;
        OfflineRenderSession reference(referenceGraph), candidate(graph);
        require(reference.scheduleMidiVoiceEvents(events) && candidate.scheduleMidiVoiceEvents(events)
            && reference.render(41).ok() && candidate.render(41).ok(),
            "offline analysis must retain committed prefix evidence");
        const auto dropped = capture->droppedBlockCount(0);
        const auto rejected = capture->rejectedRecordCount(0);
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = candidate.render(90);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty(),
                "late analysis failure must return no partial audio");
            require(capture->droppedBlockCount(0) == dropped
                && capture->rejectedRecordCount(0) == rejected,
                "failed offline call must publish no analysis drop/rejection effects");
        }
        std::array<double, 17> actual{}, expected{};
        const auto previous = capture->copyLatestRecordResult(0, actual);
        const auto referencePrevious = referenceCapture->copyLatestRecordResult(0, expected);
        require(previous.status == RealtimeAnalysisCopyStatus::Accepted
            && previous.status == referencePrevious.status && previous.header == referencePrevious.header
            && actual == expected, "failed offline call must preserve previously queued analysis evidence");
        const auto retry = candidate.render(80);
        const auto uninterrupted = reference.render(80);
        require(retry.ok() && uninterrupted.ok() && retry.mono == uninterrupted.mono,
            "analysis publication rollback must also restore audible state and input");
        actual.fill(0); expected.fill(0);
        const auto record = capture->copyLatestRecordResult(0, actual);
        const auto referenceRecord = referenceCapture->copyLatestRecordResult(0, expected);
        require(record.status == RealtimeAnalysisCopyStatus::Accepted
            && record.status == referenceRecord.status && record.header == referenceRecord.header
            && actual == expected && capture->droppedBlockCount(0) == referenceCapture->droppedBlockCount(0),
            "retry must publish the exact successful callback sequence and queue capacity effects");
        require(candidate.render(0).ok() && !candidate.render(1).ok()
            && capture->copyLatestRecordResult(0, actual).status == RealtimeAnalysisCopyStatus::NoRecord,
            "empty or immediately failed analysis calls must publish no new record");
    }
}

void testOfflineDeferredAnalysisMatchesDirectPublication()
{
    using namespace talas::runtime;
    for (int family = 0; family != 4; ++family)
    {
        const auto prepareGraph = [&] {
            if (family == 0) return compileNativeStftFixture("16", "hann", false, true);
            if (family == 1) return compileLiveAnalysisRollbackFixture(false, true);
            return compileTypedGateAdsrFixture(false, 0.0, true);
        };
        const auto graph = prepareGraph();
        const auto referenceGraph = prepareGraph();
        const auto capture = graph.analysisCapture();
        const auto referenceCapture = referenceGraph.analysisCapture();
        RealtimeRenderSession candidate(graph), reference(referenceGraph);
        require(candidate.ready() && reference.ready() && candidate.bindAnalysisStage(702)
            && reference.bindAnalysisStage(702), "deferred typed analysis fixtures must prepare");
        const auto render = [&](RealtimeRenderSession& session, std::size_t frames) {
            std::array<double, 17> left{}, right{};
            std::array<double*, 2> channels{left.data(), right.data()};
            bool ok = false;
            std::size_t allocations = 0;
            {
                ScopedOfflineAllocationFault count;
                ok = session.render({}, channels.data(), session.outputChannelCount(), frames);
                allocations = offlineAllocationCount;
            }
            require(ok && allocations == 0, "deferred analysis callback must remain allocation-free");
            return std::pair{left, right};
        };
        for (int warm = 0; warm != 5; ++warm)
            require(render(candidate, 17) == render(reference, 17), "analysis prefix audio must agree");
        std::array<double, maximumRealtimeAnalysisPayloadValues> values{}, expected{};
        std::array<talas::dsp::PartialSpectrumPoint, maximumRealtimeAnalysisPartialCount> points{}, expectedPoints{};
        for (std::size_t probe = 0; probe < capture->probeCount(); ++probe)
        {
            static_cast<void>(capture->copyLatestRecordResult(probe, values));
            static_cast<void>(referenceCapture->copyLatestRecordResult(probe, expected));
            if (capture->probeSpec(probe)->payloadKind == RealtimeAnalysisPayloadKind::PartialSpectrum)
            {
                static_cast<void>(capture->copyLatestPartialSpectrum(probe, points));
                static_cast<void>(referenceCapture->copyLatestPartialSpectrum(probe, expectedPoints));
            }
        }
        if (family >= 2)
        {
            const auto schedule = [&](RealtimeRenderSession& session) {
                const auto target = session.gateTargetIndex("env");
                require(target.has_value(), "typed analysis target must exist");
                const std::size_t count = family == 2 ? 3 : maximumRealtimeAnalysisEventCount + 1;
                std::vector<PreparedTypedEvent> events;
                for (std::size_t index = 0; index != count; ++index)
                {
                    TypedEventPayload payload{};
                    payload.gateEdge = {static_cast<std::uint8_t>(index % 2 == 0)};
                    events.push_back({TypedEventKind::GateEdge,
                        {TypedEventTargetKind::Gate, *target, 1, 1},
                        index * 17 / count, index + 1, payload});
                }
                std::vector<RealtimeTypedEventDispatchResult> results(count);
                require(session.scheduleTypedEvents(events, results)
                    && std::ranges::all_of(results, [](auto value) { return value == RealtimeTypedEventDispatchResult::Handled; }),
                    "typed analysis events must schedule before the transaction boundary");
            };
            schedule(candidate); schedule(reference);
        }
        constexpr std::array<std::size_t, 8> blocks{17, 9, 17, 16, 17, 11, 17, 17};
        const auto dropped = capture->droppedBlockCount(0);
        const auto rejected = capture->rejectedRecordCount(0);
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            auto transaction = RealtimeRenderSessionTestAccess::beginOfflineTransaction(candidate);
            require(bool(transaction), "analysis participant must have an offline checkpoint");
            for (const auto frames : blocks)
            {
                const auto actualAudio = render(candidate, frames);
                transaction->collectAnalysis(frames);
                if (attempt == 1)
                    require(actualAudio == render(reference, frames), "deferred retry must restore exact DSP and typed input");
                for (std::size_t probe = 0; probe < capture->probeCount(); ++probe)
                {
                    require(capture->copyLatestRecordResult(probe, values).status == RealtimeAnalysisCopyStatus::NoRecord,
                        "consumer must see no provisional callback records");
                    if (capture->probeSpec(probe)->payloadKind == RealtimeAnalysisPayloadKind::PartialSpectrum)
                        require(capture->copyLatestPartialSpectrum(probe, points).status == RealtimeAnalysisCopyStatus::NoRecord,
                            "consumer must see no provisional typed partial points");
                }
            }
            require(capture->droppedBlockCount(0) == dropped && capture->rejectedRecordCount(0) == rejected,
                "provisional analysis must not change public counters");
            if (attempt == 1)
            {
                std::size_t allocations = 0;
                bool committed = false;
                { ScopedOfflineAllocationFault count; committed = transaction->commit(); allocations = offlineAllocationCount; }
                require(committed && allocations == 0, "analysis commit must succeed without allocating memory");
            }
        }
        for (std::size_t probe = 0; probe < capture->probeCount(); ++probe)
        {
            values.fill(0); expected.fill(0);
            const auto actual = capture->copyLatestRecordResult(probe, values);
            const auto direct = referenceCapture->copyLatestRecordResult(probe, expected);
            const auto shouldReject = family == 3 && probe == 0;
            require(actual.status == (shouldReject ? RealtimeAnalysisCopyStatus::NoRecord : RealtimeAnalysisCopyStatus::Accepted)
                && actual.status == direct.status && actual.header == direct.header && values == expected
                && capture->droppedBlockCount(probe) == referenceCapture->droppedBlockCount(probe)
                && capture->rejectedRecordCount(probe) == referenceCapture->rejectedRecordCount(probe),
                "deferred commit must match direct complex/control/event/partial records and counters");
            if (shouldReject) require(capture->rejectedRecordCount(probe) == rejected + 1,
                "successful oversized event capture must publish exactly one rejection");
            if (family == 2 && probe == 0)
                require(actual.header.absoluteSamplePosition == 85 && actual.header.valueCount == 9
                    && values[1] == 1.0 && values[2] == 0.0 && values[4] == 0.0 && values[5] == 5.0
                    && values[7] == 1.0 && values[8] == 11.0,
                    "typed event record must retain independently specified edges and offsets");
            if (family == 1)
            {
                const auto typed = capture->copyLatestPartialSpectrum(probe, points);
                const auto directTyped = referenceCapture->copyLatestPartialSpectrum(probe, expectedPoints);
                require(typed.status == RealtimeAnalysisCopyStatus::Accepted && typed.status == directTyped.status
                    && typed.header == directTyped.header && typed.pointCount == directTyped.pointCount && typed.pointCount != 0,
                    "typed partial publication must retain its separate sequence and header");
                for (std::size_t index = 0; index != typed.pointCount; ++index)
                    require(points[index].trackId == expectedPoints[index].trackId
                        && points[index].hasTrackId == expectedPoints[index].hasTrackId
                        && points[index].amplitude == expectedPoints[index].amplitude
                        && points[index].frequencyHz == expectedPoints[index].frequencyHz
                        && points[index].bandwidthHz == expectedPoints[index].bandwidthHz,
                        "typed partial publication must preserve every point field");
            }
        }
    }
}

void testOfflineAnalysisPublicationAllocationRollback()
{
    using namespace talas::runtime;
    for (const bool partial : {false, true})
    {
        const auto makeGraph = [&] { return partial ? compileLiveAnalysisRollbackFixture(false, true)
                                                  : compilePartialTrackRollbackFixture(8); };
        const auto graph = makeGraph();
        const auto capture = graph.analysisCapture();
        OfflineMidiVoiceEvent event;
        event.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
        event.frame = partial ? 210 : 57; event.batchId = event.sequence = 1;
        event.eventValueName = partial ? "Scale" : "Gain"; event.value = 0.5;
        const auto prepare = [&](const PreparedRuntimeGraph& input) {
            require(input.analysisCapture()->bindStage(703), "analysis allocation capture must bind");
            OfflineRenderSession session(input);
            require(session.scheduleMidiVoiceEvents(std::span{&event, 1}) && session.render(partial ? 193 : 41).ok(),
                "analysis allocation prefix must commit");
            std::array<double, maximumRealtimeAnalysisPayloadValues> discard{};
            static_cast<void>(input.analysisCapture()->copyLatestRecordResult(0, discard));
            if (partial)
            {
                std::array<talas::dsp::PartialSpectrumPoint, maximumRealtimeAnalysisPartialCount> points{};
                static_cast<void>(input.analysisCapture()->copyLatestPartialSpectrum(0, points));
            }
            return session;
        };
        auto reference = prepare(graph);
        OfflineRenderResult expected;
        std::size_t allocationPoints = 0;
        {
            ScopedOfflineAllocationFault count;
            expected = reference.render(100);
            allocationPoints = offlineAllocationCount;
        }
        std::array<double, maximumRealtimeAnalysisPayloadValues> expectedValues{};
        const auto expectedRecord = capture->copyLatestRecordResult(0, expectedValues);
        require(expected.ok() && expectedRecord.status == RealtimeAnalysisCopyStatus::Accepted && allocationPoints != 0,
            "analysis allocation reference must publish successful continuation");
        for (std::size_t point = 0; point != allocationPoints; ++point)
        {
            const auto candidateGraph = makeGraph();
            const auto candidateCapture = candidateGraph.analysisCapture();
            auto candidate = prepare(candidateGraph);
            const auto dropped = candidateCapture->droppedBlockCount(0);
            const auto rejected = candidateCapture->rejectedRecordCount(0);
            OfflineRenderResult failed;
            { ScopedOfflineAllocationFault fault(point); failed = candidate.render(100); }
            std::array<double, maximumRealtimeAnalysisPayloadValues> values{};
            require(offlineAllocationFaultTriggered && !failed.ok() && failed.mono.empty() && failed.channels.empty()
                && candidateCapture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord
                && candidateCapture->droppedBlockCount(0) == dropped && candidateCapture->rejectedRecordCount(0) == rejected,
                "allocation failure must discard all provisional analysis and audio");
            if (partial)
            {
                std::array<talas::dsp::PartialSpectrumPoint, maximumRealtimeAnalysisPartialCount> points{};
                require(candidateCapture->copyLatestPartialSpectrum(0, points).status == RealtimeAnalysisCopyStatus::NoRecord,
                    "allocation failure must discard the separate typed partial queue");
            }
            const auto retry = candidate.render(100);
            const auto actual = candidateCapture->copyLatestRecordResult(0, values);
            require(retry.ok() && retry.mono == expected.mono && retry.right == expected.right
                && actual.status == expectedRecord.status && actual.header == expectedRecord.header && values == expectedValues
                && candidateCapture->droppedBlockCount(0) == capture->droppedBlockCount(0),
                "analysis allocation retry must restore exact audible state, sequence, metadata and values");
        }
        std::cout << "P2 " << (partial ? "partial" : "audio") << " publication allocation failure/retry points: " << allocationPoints << '\n';
    }
}

void testOfflinePartialTrackWholeCallRollback()
{
    using namespace talas::runtime;
    const auto graph = compilePartialTrackRollbackFixture();
    std::array<OfflineMidiVoiceEvent, 2> events{};
    events[0].kind = events[1].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    events[0].frame = 57; events[0].batchId = events[0].sequence = 1;
    events[0].eventValueName = "Gain"; events[0].value = 0.5;
    events[1].frame = 121; events[1].batchId = events[1].sequence = 2;
    events[1].eventValueName = "Divisor"; events[1].value = 0.0;
    OfflineRenderSession reference(graph), candidate(graph);
    require(reference.scheduleMidiVoiceEvents(events) && candidate.scheduleMidiVoiceEvents(events)
        && reference.render(41).ok() && candidate.render(41).ok(),
        "partial resynthesis must commit nonzero phases and interpolation cursors");
    const auto expected = reference.render(80);
    require(expected.ok() && expected.mono.size() == 80
        && std::ranges::any_of(expected.mono, [](double sample) { return std::abs(sample) > 0.01; }),
        "partial resynthesis reference must contain audible multi-track and multi-segment output");
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        const auto failed = candidate.render(90);
        require(!failed.ok() && failed.mono.empty() && failed.channels.empty(),
            "late partial resynthesis failure must publish no partial result");
        require(candidate.render(0).ok(), "empty partial resynthesis retry must preserve state");
    }
    const auto first = candidate.render(19);
    const auto second = candidate.render(61);
    require(first.ok() && second.ok() && first.mono.size() == 19 && second.mono.size() == 61,
        "partial resynthesis retry must preserve its committed source position");
    for (std::size_t frame = 0; frame != expected.mono.size(); ++frame)
    {
        const auto actual = frame < 19 ? first.mono[frame] : second.mono[frame - 19];
        require(actual == expected.mono[frame],
            "partial resynthesis retry must restore phases, track/segment cursors and pending gain");
    }
    require(!candidate.render(1).ok(), "invalid partial resynthesis control must remain queued at frame 121");
}

void testOfflinePartialTrackAllocationRollback()
{
    using namespace talas::runtime;
    const auto graph = compilePartialTrackRollbackFixture();
    OfflineMidiVoiceEvent gain;
    gain.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    gain.frame = 57; gain.batchId = gain.sequence = 1;
    gain.eventValueName = "Gain"; gain.value = 0.5;
    const auto prepare = [&] {
        OfflineRenderSession session(graph);
        require(session.scheduleMidiVoiceEvents(std::span{&gain, 1}) && session.render(41).ok(),
            "partial allocation fixture must commit its initial phases");
        return session;
    };
    auto reference = prepare();
    OfflineRenderResult expected;
    std::size_t allocationPoints = 0;
    {
        ScopedOfflineAllocationFault counting;
        expected = reference.render(100);
        allocationPoints = offlineAllocationCount;
    }
    require(expected.ok() && expected.mono.size() == 100 && allocationPoints != 0
        && std::ranges::all_of(std::span(expected.mono).last(5), [](double sample) { return sample == 0.0; }),
        "partial allocation reference must cross tracks, segments and an empty segment boundary");
    for (std::size_t point = 0; point != allocationPoints; ++point)
    {
        auto session = prepare();
        OfflineRenderResult faulted;
        {
            ScopedOfflineAllocationFault fault(point);
            faulted = session.render(100);
        }
        require(offlineAllocationFaultTriggered && !faulted.ok() && faulted.mono.empty()
            && faulted.right.empty() && faulted.channels.empty() && faulted.midiEvents.empty()
            && faulted.globalControllerEvents.empty(),
            "partial snapshot allocation failure must return no partial output");
        const auto retry = session.render(100);
        require(retry.ok() && retry.mono == expected.mono,
            "partial snapshot allocation retry must restore exact phase/interpolation/segment continuation");
    }
    std::cout << "P2 partial resynthesis allocation failure/retry points: " << allocationPoints << '\n';
}

void testConformanceRejectsAnIncompleteRealtimePartition()
{
    const auto graph = compileOscillatorFixture();
    constexpr std::array<std::size_t, 2> incompleteBlocks = {8, 7};
    const auto result =
        talas::runtime::compareOfflineAndPreparedRealtime(
            graph,
            16,
            incompleteBlocks,
            {});

    require(!result.ok(), "incomplete realtime partition should fail");
    require(
        result.message.find("partition") != std::string::npos,
        "partition failure should be diagnostic");
}

void testFilterLegacyBaselineMatchesAcrossContexts()
{
    const auto graph = compileFilterFixture();
    constexpr std::array<std::size_t, 5> realtimeBlocks = {
        1,
        3,
        2,
        5,
        5,
    };
    const auto result =
        talas::runtime::compareOfflineAndPreparedRealtime(
            graph,
            16,
            realtimeBlocks,
            {.absolute = 1.0e-12, .relative = 1.0e-12});
    constexpr std::array<double, 16> legacyOutput = {
        0.0,
        0.00013629041829737188,
        0.0017671449897811128,
        0.010913197489603918,
        0.042719734247254559,
        0.11876975938895362,
        0.24810022799147724,
        0.39879684622343353,
        0.48865581308566136,
        0.42058645004937423,
        0.15197025854476423,
        -0.25113679412416007,
        -0.62374495668390484,
        -0.78088794817105311,
        -0.6202316254767356,
        -0.18509703587020512,
    };
    requireLegacyBaseline(result, legacyOutput, "Filter");
    const std::vector<std::vector<double>> oracle{{legacyOutput.begin(), legacyOutput.end()}};
    const auto replay = talas::runtime::runRuntimeConformance(graph, 16, realtimeBlocks,
        {.referenceChannels = oracle, .verifyResetReplay = true});
    require(replay.ok() && replay.resetReplayVerified,
        "Filter reset must restore its delay state and legacy samples: " + replay.message);
}

void testAdsrLegacyBaselineMatchesAcrossContexts()
{
    const auto graph = compileAdsrFixture();
    constexpr std::array<std::size_t, 5> realtimeBlocks = {
        1,
        3,
        2,
        5,
        5,
    };
    const auto result =
        talas::runtime::compareOfflineAndPreparedRealtime(
            graph,
            16,
            realtimeBlocks,
            {.absolute = 1.0e-12, .relative = 1.0e-12});
    constexpr std::array<double, 16> legacyOutput = {
        0.0,
        0.25,
        6.123233995736766e-17,
        -0.75,
        0.0,
        0.8125,
        7.6540424946709579e-17,
        -0.4375,
        0.0,
        0.25,
        3.061616997868383e-17,
        -0.25,
        0.0,
        0.25,
        3.061616997868383e-17,
        -0.25,
    };
    requireLegacyBaseline(result, legacyOutput, "ADSR");
    const std::vector<std::vector<double>> oracle{{legacyOutput.begin(), legacyOutput.end()}};
    const auto replay = talas::runtime::runRuntimeConformance(graph, 16, realtimeBlocks,
        {.referenceChannels = oracle, .verifyResetReplay = true});
    require(replay.ok() && replay.resetReplayVerified,
        "ADSR reset must restore envelope state and legacy samples: " + replay.message);
}

void testDirectTypedAdsrGateConformance()
{
    const auto directGraph = compileTypedGateAdsrFixture();
    const auto directWholeGraph = compileTypedGateAdsrFixture();
    const auto scalarGraph = compileTypedGateAdsrFixture(true);
    talas::runtime::RealtimeRenderSession direct(directGraph);
    talas::runtime::RealtimeRenderSession directWhole(directWholeGraph);
    talas::runtime::RealtimeRenderSession scalar(scalarGraph);
    require(direct.ready() && directWhole.ready() && scalar.ready(),
        "typed ADSR conformance sessions should prepare");

    const auto gateEvent = [](
        std::uint32_t target,
        bool high,
        std::size_t frameOffset,
        std::uint64_t sequence) {
        talas::runtime::TypedEventPayload payload{};
        payload.gateEdge = {
            static_cast<std::uint8_t>(high ? 1 : 0)};
        return talas::runtime::PreparedTypedEvent{
            talas::runtime::TypedEventKind::GateEdge,
            {
                talas::runtime::TypedEventTargetKind::Gate,
                target,
                1,
                1,
            },
            frameOffset,
            sequence,
            payload,
        };
    };
    const auto scheduleEdges = [&](
        talas::runtime::RealtimeRenderSession& session,
        std::uint64_t sequenceBase = 0) {
        const auto target = session.gateTargetIndex("env");
        if (!target.has_value())
            return false;
        const std::array events = {
            gateEvent(*target, true, 0, sequenceBase + 1),
            gateEvent(*target, false, 8, sequenceBase + 2),
            gateEvent(*target, true, 12, sequenceBase + 3),
            gateEvent(*target, false, 24, sequenceBase + 4),
            gateEvent(*target, true, 32, sequenceBase + 5),
        };
        std::array<talas::runtime::RealtimeTypedEventDispatchResult, 5>
            results{};
        return session.scheduleTypedEvents(events, results)
            && std::ranges::all_of(
                results,
                [](const auto result) {
                    return result
                        == talas::runtime::RealtimeTypedEventDispatchResult::Handled;
                });
    };
    require(scheduleEdges(direct) && scheduleEdges(directWhole),
        "direct ADSR conformance edges should schedule");
    require(scalar.scheduleEventValue(
                "AdsrGate", 1.0,
                talas::expr::ValueDomain::Scalar, 0)
            && scalar.scheduleEventValue(
                "AdsrGate", 0.0,
                talas::expr::ValueDomain::Scalar, 8)
            && scalar.scheduleEventValue(
                "AdsrGate", 1.0,
                talas::expr::ValueDomain::Scalar, 12)
            && scalar.scheduleEventValue(
                "AdsrGate", 0.0,
                talas::expr::ValueDomain::Scalar, 24)
            && scalar.scheduleEventValue(
                "AdsrGate", 1.0,
                talas::expr::ValueDomain::Scalar, 32),
        "scalar ADSR conformance edges should schedule");

    constexpr std::array<std::size_t, 6> partitions = {
        3, 7, 1, 9, 4, 16};
    const auto renderPartitions = [](
        talas::runtime::RealtimeRenderSession& session,
        std::span<const std::size_t> blocks,
        std::span<double> output) {
        std::size_t offset = 0;
        for (const auto frames : blocks)
        {
            if (offset + frames > output.size()
                || !session.render(output.subspan(offset, frames)))
            {
                return false;
            }
            offset += frames;
        }
        return offset == output.size();
    };
    std::array<double, 40> directOutput{};
    std::array<double, 40> directWholeOutput{};
    std::array<double, 40> scalarOutput{};
    constexpr std::array<std::size_t, 1> wholeBlock = {40};
    require(renderPartitions(direct, partitions, directOutput)
            && renderPartitions(
                directWhole,
                wholeBlock,
                directWholeOutput)
            && renderPartitions(scalar, partitions, scalarOutput),
        "typed ADSR conformance partitions should render");
    for (std::size_t frame = 0; frame < directOutput.size(); ++frame)
    {
        require(directOutput[frame] == directWholeOutput[frame],
            "typed ADSR partition divergence at frame "
                + std::to_string(frame) + ": "
                + std::to_string(directOutput[frame]) + " vs "
                + std::to_string(directWholeOutput[frame]));
        require(directOutput[frame] == scalarOutput[frame],
            "typed/scalar ADSR divergence at frame "
                + std::to_string(frame) + ": "
                + std::to_string(directOutput[frame]) + " vs "
                + std::to_string(scalarOutput[frame]));
    }

    const auto callbackEndGraph = compileTypedGateAdsrFixture();
    const auto callbackStartGraph = compileTypedGateAdsrFixture();
    talas::runtime::RealtimeRenderSession callbackEnd(callbackEndGraph);
    talas::runtime::RealtimeRenderSession callbackStart(callbackStartGraph);
    const auto callbackEndTarget = callbackEnd.gateTargetIndex("env");
    const auto callbackStartTarget = callbackStart.gateTargetIndex("env");
    require(callbackEndTarget.has_value()
            && callbackStartTarget.has_value(),
        "callback-boundary ADSR targets should resolve");
    const std::array deferred = {
        gateEvent(*callbackEndTarget, true, 3, 20),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        deferredResult{};
    require(callbackEnd.scheduleTypedEvents(deferred, deferredResult),
        "callback-end ADSR event should schedule");
    std::array<double, 3> beforeDeferred{};
    std::array<double, 3> beforeReference{};
    require(callbackEnd.render(beforeDeferred)
            && callbackStart.render(beforeReference)
            && beforeDeferred == beforeReference,
        "callback-end ADSR event should remain deferred");
    const std::array nextCallback = {
        gateEvent(*callbackStartTarget, true, 0, 20),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        nextCallbackResult{};
    require(callbackStart.scheduleTypedEvents(
                nextCallback,
                nextCallbackResult),
        "next-callback ADSR reference should schedule");
    std::array<double, 5> afterDeferred{};
    std::array<double, 5> afterReference{};
    require(callbackEnd.render(afterDeferred)
            && callbackStart.render(afterReference)
            && afterDeferred == afterReference,
        "callback-end ADSR event should arrive at next callback frame zero");

    const auto resetGraph = compileTypedGateAdsrFixture();
    const auto freshGraph = compileTypedGateAdsrFixture();
    talas::runtime::RealtimeRenderSession reset(resetGraph);
    talas::runtime::RealtimeRenderSession fresh(freshGraph);
    const auto resetTarget = reset.gateTargetIndex("env");
    const auto freshTarget = fresh.gateTargetIndex("env");
    require(resetTarget.has_value() && freshTarget.has_value(),
        "reset ADSR targets should resolve");
    const std::array seed = {
        gateEvent(*resetTarget, true, 0, 30),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        seedResult{};
    std::array<double, 6> seedOutput{};
    require(reset.scheduleTypedEvents(seed, seedResult)
            && reset.render(seedOutput),
        "reset ADSR fixture should enter attack/decay");
    reset.resetTransport();
    const std::array resetEdge = {
        gateEvent(*resetTarget, true, 0, 31),
    };
    const std::array freshEdge = {
        gateEvent(*freshTarget, true, 0, 31),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        resetResult{};
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        freshResult{};
    std::array<double, 12> resetOutput{};
    std::array<double, 12> freshOutput{};
    require(reset.scheduleTypedEvents(resetEdge, resetResult)
            && fresh.scheduleTypedEvents(freshEdge, freshResult)
            && reset.render(resetOutput)
            && fresh.render(freshOutput)
            && resetOutput == freshOutput,
        "typed ADSR reset should match fresh session state");

    const auto repeatedGraph = compileTypedGateAdsrFixture();
    const auto continuedGraph = compileTypedGateAdsrFixture();
    talas::runtime::RealtimeRenderSession repeated(repeatedGraph);
    talas::runtime::RealtimeRenderSession continued(continuedGraph);
    const auto repeatedTarget = repeated.gateTargetIndex("env");
    const auto continuedTarget = continued.gateTargetIndex("env");
    require(repeatedTarget.has_value()
            && continuedTarget.has_value(),
        "retrigger ADSR targets should resolve");
    const std::array repeatedSeed = {
        gateEvent(*repeatedTarget, true, 0, 40),
    };
    const std::array continuedSeed = {
        gateEvent(*continuedTarget, true, 0, 40),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        repeatedSeedResult{};
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        continuedSeedResult{};
    std::array<double, 6> repeatedSeedOutput{};
    std::array<double, 6> continuedSeedOutput{};
    require(repeated.scheduleTypedEvents(
                repeatedSeed,
                repeatedSeedResult)
            && continued.scheduleTypedEvents(
                continuedSeed,
                continuedSeedResult)
            && repeated.render(repeatedSeedOutput)
            && continued.render(continuedSeedOutput),
        "retrigger ADSR fixtures should seed equally");
    const std::array repeatedEdges = {
        gateEvent(*repeatedTarget, false, 0, 41),
        gateEvent(*repeatedTarget, true, 0, 42),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 2>
        repeatedResults{};
    std::array<double, 4> repeatedOutput{};
    std::array<double, 4> continuedOutput{};
    require(repeated.scheduleTypedEvents(
                repeatedEdges,
                repeatedResults)
            && repeated.render(repeatedOutput)
            && continued.render(continuedOutput)
            && repeatedOutput != continuedOutput,
        "same-frame falling/rising ADSR edges should remain observable as a retrigger");

    const auto legatoGraph = compileTypedGateAdsrFixture(false, 1.0);
    const auto nonLegatoGraph = compileTypedGateAdsrFixture();
    talas::runtime::RealtimeRenderSession legato(legatoGraph);
    talas::runtime::RealtimeRenderSession nonLegato(nonLegatoGraph);
    const auto legatoTarget = legato.gateTargetIndex("env");
    const auto nonLegatoTarget = nonLegato.gateTargetIndex("env");
    require(legatoTarget.has_value()
            && nonLegatoTarget.has_value(),
        "legato ADSR targets should resolve");
    const std::array legatoSeed = {
        gateEvent(*legatoTarget, true, 0, 50),
    };
    const std::array nonLegatoSeed = {
        gateEvent(*nonLegatoTarget, true, 0, 50),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        legatoSeedResult{};
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1>
        nonLegatoSeedResult{};
    std::array<double, 2> legatoSeedOutput{};
    std::array<double, 2> nonLegatoSeedOutput{};
    require(legato.scheduleTypedEvents(
                legatoSeed,
                legatoSeedResult)
            && nonLegato.scheduleTypedEvents(
                nonLegatoSeed,
                nonLegatoSeedResult)
            && legato.render(legatoSeedOutput)
            && nonLegato.render(nonLegatoSeedOutput),
        "legato ADSR fixtures should seed equally");
    const std::array legatoEdges = {
        gateEvent(*legatoTarget, false, 0, 51),
        gateEvent(*legatoTarget, true, 0, 52),
    };
    const std::array nonLegatoEdges = {
        gateEvent(*nonLegatoTarget, false, 0, 51),
        gateEvent(*nonLegatoTarget, true, 0, 52),
    };
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 2>
        legatoResults{};
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 2>
        nonLegatoResults{};
    std::array<double, 2> legatoOutput{};
    std::array<double, 2> nonLegatoOutput{};
    require(legato.scheduleTypedEvents(legatoEdges, legatoResults)
            && nonLegato.scheduleTypedEvents(
                nonLegatoEdges,
                nonLegatoResults)
            && legato.render(legatoOutput)
            && nonLegato.render(nonLegatoOutput)
            && legatoOutput != nonLegatoOutput,
        "typed ADSR legato should preserve current level on retrigger");
}

void testConnectedKernelAdaptersMatchAcrossIrregularBoundaries()
{
    const auto graph = compileConnectedKernelFixture();
    constexpr std::array<std::size_t, 5> realtimeBlocks = {2, 5, 1, 7, 9};
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        24,
        realtimeBlocks,
        {.absolute = 1.0e-12, .relative = 1.0e-12});
    require(result.ok(), result.message);
    require(
        std::ranges::any_of(
            result.offlineChannels.front(),
            [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "connected-kernel fixture should exercise audible attack/release output");
}

void testM1UtilityDemonstratorMatchesAcrossIrregularBoundaries()
{
    const auto graph = compileM1UtilityDemonstratorFixture();
    const auto firstOffline = talas::runtime::renderOffline(graph, 40);
    const auto secondOffline = talas::runtime::renderOffline(graph, 40);
    require(firstOffline.ok() && secondOffline.ok()
            && firstOffline.channels == secondOffline.channels,
        "M1 utility demonstrator should render deterministically from fresh sessions");
    constexpr std::array<std::size_t, 6> realtimeBlocks = {3, 7, 1, 9, 4, 16};
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        40,
        realtimeBlocks,
        {.absolute = 1.0e-12, .relative = 1.0e-12});
    require(result.ok(), result.message);
    require(
        std::ranges::any_of(
            result.offlineChannels.front(),
            [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "M1 utility demonstrator should produce gated audible output");
}

void testCompileDiagnosticsUseDescriptorOfflineCapability()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.addNodeType({
        "talas.testUnsupported",
        "Unsupported Test Node",
        "Testing",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {},
    });

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(
        node("unsupported", "talas.testUnsupported"));
    document.graph.connections.push_back(connection(
        "unsupported",
        "audioOut",
        "out",
        "audioIn"));

    const auto compiled = talas::runtime::compileProject(
        document,
        registry,
        {});
    require(!compiled.ok(), "offline-unsupported descriptor should reject compilation");
    const auto diagnostic = std::ranges::find_if(
        compiled.diagnostics,
        [](const auto& candidate) {
            return candidate.code
                    == talas::runtime::DiagnosticCode::UnsupportedNodeType
                && candidate.nodeId == "unsupported";
        });
    require(
        diagnostic != compiled.diagnostics.end()
            && diagnostic->message.find("offline capability")
                != std::string::npos,
        "compile diagnostics should explain descriptor-derived offline support");
}

void testStaticFirFamilyMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    for (const auto reResonator : {false, true})
    {
        const auto graph = compileStaticFirFixture(reResonator);
        const auto result =
            talas::runtime::compareOfflineAndPreparedRealtime(
                graph,
                48,
                realtimeBlocks,
                {.absolute = 1.0e-12, .relative = 1.0e-12});
        const auto name = std::string{
            reResonator ? "RE Resonator" : "FIR Filter"};
        require(result.ok(), name + " C18 conformance: " + result.message);
        require(
            result.offlineChannels.size() == 1
                && std::ranges::any_of(
                    result.offlineChannels.front(),
                    [](double sample) {
                        return std::abs(sample) > 1.0e-9;
                    }),
            name + " C18 conformance should exercise audible FIR state");
    }
}

void testVcfMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileVcfFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(result.ok(), "C19 VCF conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C19 VCF conformance should exercise audible four-stage state");
}

void testHarmonicResonatorMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileHarmonicResonatorFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C20 Harmonic Resonator conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C20 Harmonic Resonator conformance should exercise audible delay and brightness state");
}

void testQuadOscillatorMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileQuadOscillatorFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C22 Quad Oscillator conformance: " + result.message);
    require(
        result.offlineChannels.size() == 2
            && std::ranges::any_of(
                result.offlineChannels[0],
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                })
            && std::ranges::any_of(
                result.offlineChannels[1],
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                })
            && std::ranges::equal(
                result.offlineChannels[0], result.offlineChannels[1]),
        "C22 conformance should exercise audible summed mono output mirrored on compatibility rightOut");
}

void testOverlappingMixerMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileOverlappingMixerFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C23 Overlapping Mixer conformance: " + result.message);
    require(
        result.offlineChannels.size() == 2
            && std::ranges::any_of(
                result.offlineChannels[0],
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                })
            && std::ranges::any_of(
                result.offlineChannels[1],
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                })
            && result.offlineChannels[0]
                != result.offlineChannels[1],
        "C23 conformance should exercise distinct audible stereo products");
}

void testOscillatorBankMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileOscillatorBankFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C24 Oscillator Bank conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C24 conformance should exercise audible ordered partial resynthesis");
}

void testC61SumOfSinesDefaultsMatchAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileC61SumOfSinesFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 1.0e-12, .relative = 1.0e-12});
    require(result.ok(), "C61 SumOfSines conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C61 conformance should exercise audible default-frequency morph resynthesis");
}

void testC63SumOfSinesAutomaticLoopMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileC63SumOfSinesAutomaticLoopFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 1.0e-12, .relative = 1.0e-12});
    require(result.ok(),
        "C63 automatic SumOfSines conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C63 conformance should exercise audible automatic loop traversal");
}

void testFormantBankMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileFormantBankFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C25 Formant Bank conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C25 conformance should exercise audible phase-accumulated formants");
}

void testFormantBankOscillatorMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileFormantBankOscillatorFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C26 Formant Bank Oscillator conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C26 conformance should exercise audible selected cascade resynthesis");
}

void testSpectrumModifierMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileSpectrumModifierFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C54 probability-one TrackNumber-profile live-selector/seed Spectrum Modifier conformance: "
            + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C54 arbitrary-partition conformance should exercise audible deterministic TrackNumber-profile/live-selector/seed modified partials where probability cannot change output");
}

void testWarpedTimeIndexMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileWarpedTimeIndexFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C30 Warped Time Index conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C30 conformance should exercise the audible connected-morph time-map path");
}

void testSpectrumOnDiskMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileSpectrumOnDiskFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C31 Spectrum On Disk conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C31 conformance should exercise audible immutable-table resynthesis");
}

void testC58SpectrumInRamMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileC58SpectrumInRamFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C58 normalized SpectrumInRAM conformance: " + result.message);
    require(
        result.offlineChannels.size() == 2
            && std::ranges::all_of(
                result.offlineChannels.front(),
                [](double sample) {
                    return std::abs(sample - 0.1875) <= 1.0e-12;
                })
            && std::ranges::all_of(
                result.offlineChannels.back(),
                [](double sample) {
                    return std::abs(sample - 128.0) <= 1.0e-12;
                }),
        "C58 conformance should prove exact even-frame midpoint interpolation");
}

void testC59SpectrumInRamDefaultsMatchAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileC59SpectrumInRamDefaultsFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C59 default SpectrumInRAM conformance: " + result.message);
    require(
        result.offlineChannels.size() == 2
            && std::ranges::all_of(
                result.offlineChannels.front(),
                [](double sample) { return sample == 0.6; })
            && std::ranges::all_of(
                result.offlineChannels.back(),
                [](double sample) { return sample == 375.0; }),
        "C59 conformance should prove exact original-pitch full-frame defaults");
}

void testC60SpectrumOnDiskDefaultsMatchOfflinePreparedTransactionSchedule()
{
    constexpr std::array<std::size_t, 7> transactionFrames{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileC60SpectrumOnDiskDefaultsFixture();
    talas::runtime::OfflineRenderSession offline(graph);
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(
        prepared.ready(),
        "C60 prepared default transaction-schedule session should prepare");

    std::vector<double> offlineOutput;
    std::vector<double> preparedOutput;
    for (const auto frames : transactionFrames)
    {
        const auto offlineBlock = offline.render(frames);
        require(
            offlineBlock.ok()
                && offlineBlock.mono.size() == frames,
            "C60 offline default transaction-schedule block should render");
        offlineOutput.insert(
            offlineOutput.end(),
            offlineBlock.mono.begin(),
            offlineBlock.mono.end());

        std::vector<double> preparedBlock(frames, 0.0);
        require(
            prepared.render(preparedBlock),
            "C60 prepared default transaction-schedule block should render");
        preparedOutput.insert(
            preparedOutput.end(),
            preparedBlock.begin(),
            preparedBlock.end());
    }
    require(
        offlineOutput.size() == preparedOutput.size()
            && std::ranges::any_of(
                offlineOutput,
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C60 default transaction schedule should produce exact audible evidence");
    for (std::size_t frame = 0;
         frame < offlineOutput.size();
         ++frame)
    {
        const auto difference =
            std::abs(offlineOutput[frame] - preparedOutput[frame]);
        require(
            difference <= 1.0e-12,
            "C60 default offline/prepared transaction schedule mismatch at frame "
                + std::to_string(frame));
    }
}

void testC57VoicedUnvoicedMatchesOfflinePreparedTransactionSchedule()
{
    constexpr std::array<std::size_t, 7> transactionFrames{
        1, 5, 2, 7, 3, 11, 19};
    constexpr std::array<double, 7> expectedTransactionValues{
        1.0, 0.5, 0.0, 0.5, 1.0, 1.0, 1.0};
    const auto graph = compileC57VoicedUnvoicedTransitionFixture();
    talas::runtime::OfflineRenderSession offline(graph);
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(
        prepared.ready(),
        "C57 prepared transaction-schedule session should prepare");

    for (std::size_t transaction = 0;
         transaction < transactionFrames.size(); ++transaction)
    {
        const auto frames = transactionFrames[transaction];
        const auto offlineBlock = offline.render(frames);
        require(
            offlineBlock.ok() && offlineBlock.mono.size() == frames,
            "C57 offline transaction-schedule block should render");
        std::vector<double> preparedBlock(frames, 0.0);
        require(
            prepared.render(preparedBlock),
            "C57 prepared transaction-schedule block should render");
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            require(
                offlineBlock.mono[frame] == preparedBlock[frame]
                    && offlineBlock.mono[frame]
                        == expectedTransactionValues[transaction],
                "C57 offline/prepared transition schedule should match exactly at transaction "
                    + std::to_string(transaction)
                    + " frame " + std::to_string(frame));
        }
    }
}

void testGaOscillatorsMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileGaOscillatorsFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C32 GA Oscillators conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C32 conformance should exercise audible phase-continuous analysis morphing");
}

void testGenericSourceRamSampleMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileGenericSourceRamSampleFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C33 Generic Source RAM sample conformance: " + result.message);
    require(
        result.offlineChannels.size() == 2
            && result.offlineChannels[0] == result.offlineChannels[1]
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C33 conformance should exercise exact audible stereo autoloop playback");
}

void testSampleMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileSampleFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C34 Sample conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C34 conformance should exercise exact audible looped mono playback");
}

void testMultisampleMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileMultisampleFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C35 Multisample conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C35 conformance should exercise exact audible selected looped playback");
}

void testKeyMappedMultisampleMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileKeyMappedMultisampleFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C36 Key-Mapped Multisample conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C36 conformance should exercise exact audible mapped loop playback");
}

void testMultifileDiskPlayerMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileMultifileDiskPlayerFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C37 Multifile Disk Player conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C37 conformance should exercise exact audible interpolated playback");
}

void testMemoryWriterMatchesAcrossIrregularBoundaries()
{
    constexpr std::array<std::size_t, 7> realtimeBlocks{
        1, 5, 2, 7, 3, 11, 19};
    const auto graph = compileMemoryWriterFixture();
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(
        graph,
        48,
        realtimeBlocks,
        {.absolute = 0.0, .relative = 0.0});
    require(
        result.ok(),
        "C38 Memory Writer conformance: " + result.message);
    require(
        result.offlineChannels.size() == 1
            && std::ranges::any_of(
                result.offlineChannels.front(),
                [] (double sample) {
                    return std::abs(sample) > 1.0e-9;
                })
            && std::ranges::all_of(
                result.offlineChannels.front() | std::views::drop(21),
                [] (double sample) { return sample == 0.0; }),
        "C38 conformance should exercise audible bounded monitor preview");
}

void testRawMidiTerminalMatchesAcrossAllPartitionShapes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 {!Byte} {63 + 64}")}));

    talas::runtime::RenderSettings settings{8.0, 7};
    settings.eventValues = {
        {"Byte", 64.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {0, "Byte", 64.0, talas::expr::ValueDomain::Scalar,
         talas::core::AutomationInterpolation::Linear},
        {6, "Byte", 70.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C39 raw MIDI conformance fixture should compile");
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, 7);
    require(
        offline.ok() && offline.midiEvents.size() == 7,
        "C39 offline oracle should expose initial plus linear events");

    const auto requirePartition = [&] (
        std::span<const std::size_t> partitions,
        std::string_view context) {
        std::size_t totalFrames = 0;
        for (const auto frames : partitions) totalFrames += frames;
        const auto common = talas::runtime::runRuntimeConformance(graph, totalFrames, partitions,
            {.offlineBlockPartition = partitions, .verifyResetReplay = true});
        require(common.ok(), "common terminal partition: " + std::string{context}
            + ": " + common.message);
        talas::runtime::RealtimeRenderSession session(graph);
        require(
            session.ready(),
            std::string{"C39 conformance session should prepare: "}
                + std::string{context});
        std::vector<talas::runtime::MidiEvent> actual;
        std::uint64_t callbackBase = 0;
        for (const auto frames : partitions)
        {
            std::array<talas::runtime::RealtimeMidiOutputEvent, 7> events{};
            talas::runtime::RealtimeTerminalOutputBlock output{events};
            require(
                session.renderTerminalEvents(frames, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                std::string{"C39 conformance callback should render: "}
                    + std::string{context});
            for (std::size_t index = 0;
                 index < output.midiEventCount;
                 ++index)
            {
                actual.push_back({
                    callbackBase + events[index].frameOffset,
                    std::vector<std::uint8_t>(
                        events[index].bytes.begin(),
                        events[index].bytes.begin()
                            + static_cast<std::ptrdiff_t>(
                                events[index].byteCount)),
                });
            }
            callbackBase += frames;
        }
        require(
            callbackBase == 7
                && actual.size() == offline.midiEvents.size(),
            std::string{"C39 conformance event count: "}
                + std::string{context});
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            require(
                actual[index].frame == offline.midiEvents[index].frame
                    && actual[index].bytes
                        == offline.midiEvents[index].bytes,
                std::string{"C39 conformance exact event: "}
                    + std::string{context});
        }
    };

    constexpr std::array<std::size_t, 1> oneShot{7};
    constexpr std::array<std::size_t, 7> oneFrame{
        1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::size_t, 4> irregular{2, 1, 3, 1};
    requirePartition(oneShot, "one-shot");
    requirePartition(oneFrame, "one-frame callbacks");
    requirePartition(irregular, "irregular callbacks");
}

void testMidiControllerTerminalMatchesAcrossAllPartitionShapes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("channel", "talas.constant", {{"value", "!Channel"}}),
        node("controller", "talas.constant", {{"value", "!Controller"}}),
        node("value", "talas.constant", {{"value", "!Value"}}),
        node(
            "terminal",
            "talas.midiOutputController",
            {
                {"channel", "1"},
                {"controllerNumber", "0"},
                textParameter("value", ""),
                // Raw controller values, as in projects before format 67.
                choiceParameter("valueMeans", "legacyControllerValue"),
            }),
    };
    document.graph.connections = {
        {"channel", "value", "terminal", "channel"},
        {"controller", "value", "terminal", "controllerNumber"},
        {"value", "value", "terminal", "value"},
    };

    talas::runtime::RenderSettings settings{8.0, 7};
    settings.eventValues = {
        {"Channel", 1.0, talas::expr::ValueDomain::Scalar},
        {"Controller", 0.0, talas::expr::ValueDomain::Scalar},
        {"Value", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "Value", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "Controller", 10.0, talas::expr::ValueDomain::Scalar},
        {3, "Channel", 3.0, talas::expr::ValueDomain::Scalar},
        {5, "Value", 127.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C40 MIDI controller conformance fixture should compile");
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, 7);
    require(
        offline.ok() && offline.midiEvents.size() == 5,
        "C40 offline oracle should expose initial plus changed CC tuples");

    const auto requirePartition = [&] (
        std::span<const std::size_t> partitions,
        std::string_view context) {
        std::size_t totalFrames = 0;
        for (const auto frames : partitions) totalFrames += frames;
        const auto common = talas::runtime::runRuntimeConformance(graph, totalFrames, partitions,
            {.offlineBlockPartition = partitions, .verifyResetReplay = true});
        require(common.ok(), "common terminal partition: " + std::string{context}
            + ": " + common.message);
        talas::runtime::RealtimeRenderSession session(graph);
        require(
            session.ready(),
            std::string{"C40 conformance session should prepare: "}
                + std::string{context});
        std::vector<talas::runtime::MidiEvent> actual;
        std::uint64_t callbackBase = 0;
        for (const auto frames : partitions)
        {
            std::array<talas::runtime::RealtimeMidiOutputEvent, 7> events{};
            talas::runtime::RealtimeTerminalOutputBlock output{events};
            require(
                session.renderTerminalEvents(frames, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                std::string{"C40 conformance callback should render: "}
                    + std::string{context});
            for (std::size_t index = 0;
                 index < output.midiEventCount;
                 ++index)
            {
                actual.push_back({
                    callbackBase + events[index].frameOffset,
                    std::vector<std::uint8_t>(
                        events[index].bytes.begin(),
                        events[index].bytes.begin()
                            + static_cast<std::ptrdiff_t>(
                                events[index].byteCount)),
                });
            }
            callbackBase += frames;
        }
        require(
            callbackBase == 7
                && actual.size() == offline.midiEvents.size(),
            std::string{"C40 conformance event count: "}
                + std::string{context});
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            require(
                actual[index].frame == offline.midiEvents[index].frame
                    && actual[index].bytes
                        == offline.midiEvents[index].bytes,
                std::string{"C40 conformance exact event: "}
                    + std::string{context});
        }
    };

    constexpr std::array<std::size_t, 1> oneShot{7};
    constexpr std::array<std::size_t, 7> oneFrame{
        1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::size_t, 4> irregular{2, 1, 3, 1};
    requirePartition(oneShot, "one-shot");
    requirePartition(oneFrame, "one-frame callbacks");
    requirePartition(irregular, "irregular callbacks");
}

void testMidiNoteTerminalMatchesAcrossAllPartitionShapes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("frequency", "talas.constant", {{"value", "!Frequency"}}),
        node("amplitude", "talas.constant", {{"value", "!Amplitude"}}),
        node("channel", "talas.constant", {{"value", "!Channel"}}),
        node("gate", "talas.constant", {{"value", "!Gate"}}),
        node(
            "terminal",
            "talas.midiOutputEvent",
            {
                {"frequency", "440 hz"},
                {"amplitude", "1"},
                {"channel", "1"},
                {"gate", "0"},
            }),
    };
    document.graph.connections = {
        {"frequency", "value", "terminal", "frequency"},
        {"amplitude", "value", "terminal", "amplitude"},
        {"channel", "value", "terminal", "channel"},
        {"gate", "value", "terminal", "gate"},
    };
    talas::runtime::RenderSettings settings{8.0, 7};
    settings.eventValues = {
        {"Frequency", 440.0, talas::expr::ValueDomain::Scalar},
        {"Amplitude", 1.0, talas::expr::ValueDomain::Scalar},
        {"Channel", 2.0, talas::expr::ValueDomain::Scalar},
        {"Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "Frequency", 880.0, talas::expr::ValueDomain::Scalar},
        {2, "Amplitude", 0.5, talas::expr::ValueDomain::Scalar},
        {2, "Channel", 3.0, talas::expr::ValueDomain::Scalar},
        {3, "Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {5, "Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "C41 MIDI note conformance fixture compile");
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, 7);
    require(
        offline.ok() && offline.midiEvents.size() == 4,
        "C41 offline oracle should expose two note pairs");

    const auto requirePartition = [&](
        std::span<const std::size_t> partitions,
        std::string_view context) {
        std::size_t totalFrames = 0;
        for (const auto frames : partitions) totalFrames += frames;
        const auto common = talas::runtime::runRuntimeConformance(graph, totalFrames, partitions,
            {.offlineBlockPartition = partitions, .verifyResetReplay = true});
        require(common.ok(), "common terminal partition: " + std::string{context}
            + ": " + common.message);
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(), std::string{"C41 prepare: "} + std::string{context});
        std::vector<talas::runtime::MidiEvent> actual;
        std::uint64_t callbackBase = 0;
        for (const auto frames : partitions)
        {
            std::array<talas::runtime::RealtimeMidiOutputEvent, 7> events{};
            talas::runtime::RealtimeTerminalOutputBlock output{events};
            require(
                session.renderTerminalEvents(frames, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                std::string{"C41 render: "} + std::string{context});
            for (std::size_t index = 0; index < output.midiEventCount; ++index)
            {
                actual.push_back({
                    callbackBase + events[index].frameOffset,
                    std::vector<std::uint8_t>(
                        events[index].bytes.begin(),
                        events[index].bytes.begin()
                            + static_cast<std::ptrdiff_t>(events[index].byteCount)),
                });
            }
            callbackBase += frames;
        }
        require(
            callbackBase == 7 && actual.size() == offline.midiEvents.size(),
            std::string{"C41 event count: "} + std::string{context});
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            require(
                actual[index].frame == offline.midiEvents[index].frame
                    && actual[index].bytes == offline.midiEvents[index].bytes,
                std::string{"C41 exact event: "} + std::string{context});
        }
    };
    constexpr std::array<std::size_t, 1> oneShot{7};
    constexpr std::array<std::size_t, 7> oneFrame{1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::size_t, 4> irregular{2, 1, 3, 1};
    requirePartition(oneShot, "one-shot");
    requirePartition(oneFrame, "one-frame");
    requirePartition(irregular, "irregular");
}

void testMidiFileTerminalMatchesAcrossAllPartitionShapes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {node(
        "terminal",
        "talas.midiFileEcho",
        {
            {"lowChannel", "1"},
            {"highChannel", "16"},
            resourceParameter("fileName", "Sequences/C42.mid"),
        })};
    talas::runtime::RenderSettings settings{8.0, 8};
    settings.binaryResources.push_back({
        "Sequences/C42.mid",
        {
            'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
            'M', 'T', 'r', 'k', 0, 0, 0, 12,
            0, 0x90, 60, 64, 96, 0x80, 60, 0, 0, 0xff, 0x2f, 0,
        }});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C42 MIDI file conformance fixture compile");
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, 8);
    require(
        offline.ok() && offline.midiEvents.size() == 2,
        "C42 offline oracle should expose exact note pair");

    const auto requirePartition = [&] (
        std::span<const std::size_t> partitions,
        std::string_view context) {
        std::size_t totalFrames = 0;
        for (const auto frames : partitions) totalFrames += frames;
        const auto common = talas::runtime::runRuntimeConformance(graph, totalFrames, partitions,
            {.offlineBlockPartition = partitions, .verifyResetReplay = true});
        require(common.ok(), "common terminal partition: " + std::string{context}
            + ": " + common.message);
        talas::runtime::RealtimeRenderSession session(graph);
        require(
            session.ready(),
            std::string{"C42 prepare: "} + std::string{context});
        std::vector<talas::runtime::MidiEvent> actual;
        std::size_t callbackBase = 0;
        for (const auto frames : partitions)
        {
            std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
            talas::runtime::RealtimeTerminalOutputBlock output{events};
            require(
                session.renderTerminalEvents(frames, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                std::string{"C42 render: "} + std::string{context});
            for (std::size_t index = 0;
                 index < output.midiEventCount;
                 ++index)
            {
                actual.push_back({
                    callbackBase + events[index].frameOffset,
                    std::vector<std::uint8_t>(
                        events[index].bytes.begin(),
                        events[index].bytes.begin()
                            + static_cast<std::ptrdiff_t>(
                                events[index].byteCount)),
                });
            }
            callbackBase += frames;
        }
        require(
            callbackBase == 8
                && actual.size() == offline.midiEvents.size(),
            std::string{"C42 event count: "} + std::string{context});
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            require(
                actual[index].frame == offline.midiEvents[index].frame
                    && actual[index].bytes
                        == offline.midiEvents[index].bytes,
                std::string{"C42 exact event: "}
                    + std::string{context});
        }
    };
    constexpr std::array<std::size_t, 1> oneShot{8};
    constexpr std::array<std::size_t, 8> oneFrame{
        1, 1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::size_t, 4> irregular{3, 2, 1, 2};
    requirePartition(oneShot, "one-shot");
    requirePartition(oneFrame, "one-frame");
    requirePartition(irregular, "irregular");
}

void testGlobalControllerTerminalMatchesAcrossAllPartitionShapes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node(
            "value",
            "talas.linearRamp",
            {{"duration", "1 s"}, {"start", "0"}, {"end", "1"}}),
        node(
            "terminal",
            "talas.soundToGlobalController",
            {
                textParameter("generatedEvent", "!Generated"),
                textParameter("value", ""),
            }),
    };
    document.graph.connections = {
        {"value", "value", "terminal", "value"},
    };
    talas::runtime::RenderSettings settings{4.0, 7};
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(
        compiled.ok(),
        "C43 global-controller conformance fixture compile");
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, 7);
    require(
        offline.ok() && offline.globalControllerEvents.size() == 5,
        "C43 offline oracle should expose initial plus ramp changes");

    const auto requirePartition = [&] (
        std::span<const std::size_t> partitions,
        std::string_view context) {
        std::size_t totalFrames = 0;
        for (const auto frames : partitions) totalFrames += frames;
        const auto common = talas::runtime::runRuntimeConformance(graph, totalFrames, partitions,
            {.offlineBlockPartition = partitions, .verifyResetReplay = true});
        require(common.ok(), "common terminal partition: " + std::string{context}
            + ": " + common.message);
        talas::runtime::RealtimeRenderSession session(graph);
        require(
            session.ready(),
            std::string{"C43 prepare: "} + std::string{context});
        std::vector<talas::runtime::GlobalControllerEvent> actual;
        std::size_t callbackBase = 0;
        for (const auto frames : partitions)
        {
            std::array<
                talas::runtime::RealtimeGlobalControllerOutputEvent,
                7> events{};
            std::array<char, 10> name{};
            talas::runtime::RealtimeTerminalOutputBlock output{};
            output.globalControllerEvents = events;
            output.globalControllerName = name;
            require(
                session.renderTerminalEvents(frames, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
                std::string{"C43 render: "} + std::string{context});
            require(
                output.midiEventCount == 0,
                std::string{"C43 MIDI isolation: "} + std::string{context});
            if (output.globalControllerEventCount != 0)
            {
                require(
                    output.globalControllerNameLength == 10
                        && std::string_view{
                               name.data(),
                               output.globalControllerNameLength}
                            == "!Generated",
                    std::string{"C43 exact common name: "}
                        + std::string{context});
            }
            for (std::size_t index = 0;
                 index < output.globalControllerEventCount;
                 ++index)
            {
                actual.push_back({
                    callbackBase + events[index].frameOffset,
                    std::string{
                        name.data(), output.globalControllerNameLength},
                    events[index].value,
                });
            }
            callbackBase += frames;
        }
        require(
            callbackBase == 7
                && actual.size() == offline.globalControllerEvents.size(),
            std::string{"C43 event count: "} + std::string{context});
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            require(
                actual[index].frame
                        == offline.globalControllerEvents[index].frame
                    && actual[index].name
                        == offline.globalControllerEvents[index].name
                    && actual[index].value
                        == offline.globalControllerEvents[index].value,
                std::string{"C43 exact global event: "}
                    + std::string{context});
        }
    };
    constexpr std::array<std::size_t, 1> oneShot{7};
    constexpr std::array<std::size_t, 7> oneFrame{
        1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<std::size_t, 4> regular{2, 2, 2, 1};
    constexpr std::array<std::size_t, 4> irregular{3, 1, 2, 1};
    requirePartition(oneShot, "one-shot");
    requirePartition(oneFrame, "one-frame");
    requirePartition(regular, "regular");
    requirePartition(irregular, "irregular");
}

std::vector<std::uint8_t> midiFileFormatZeroFeatureFixture()
{
    return {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 36,
        0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20,
        0, 0x90, 60, 64,
        0, 61, 65,
        0, 0xc0, 5,
        0, 6,
        0, 0x91, 62, 66,
        96, 0x80, 60, 0,
        0, 0xf0, 2, 1, 2,
        0, 0xff, 0x2f, 0,
    };
}

std::vector<std::uint8_t> midiFileFormatOneTempoFixture()
{
    return {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 11,
        0, 0xff, 0x51, 3, 0x0f, 0x42, 0x40,
        0, 0xff, 0x2f, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 12,
        0, 0x90, 60, 64,
        96, 0x80, 60, 0,
        0, 0xff, 0x2f, 0,
    };
}

std::vector<std::uint8_t> midiFileFormatTwoTempoFixture()
{
    return {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 2, 0, 2, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 19,
        0, 0xff, 0x51, 3, 0x0f, 0x42, 0x40,
        0, 0x90, 60, 64,
        96, 0x80, 60, 0,
        0, 0xff, 0x2f, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 12,
        0, 0x90, 64, 64,
        96, 0x80, 64, 0,
        0, 0xff, 0x2f, 0,
    };
}

std::vector<std::uint8_t> midiFileSmpteFixture(
    std::uint8_t rateCode,
    std::uint8_t deltaTicks)
{
    return {
        'M', 'T', 'h', 'd', 0, 0, 0, 6,
        0, 0, 0, 1, rateCode, 10,
        'M', 'T', 'r', 'k', 0, 0, 0, 12,
        0, 0x90, 60, 64,
        deltaTicks, 0x80, 60, 0,
        0, 0xff, 0x2f, 0,
    };
}

void requireMidiFilePreparedMatchesExactOffline(
    std::vector<std::uint8_t> bytes,
    double sampleRate,
    std::size_t frameCount,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::vector<talas::runtime::MidiEvent> expected,
    std::string_view context)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {node(
        "terminal",
        "talas.midiFileEcho",
        {
            {"lowChannel", std::to_string(lowChannel)},
            {"highChannel", std::to_string(highChannel)},
            resourceParameter("fileName", "Sequences/C42-matrix.mid"),
        })};
    talas::runtime::RenderSettings settings{sampleRate, frameCount};
    settings.maxRealtimeTerminalEvents = 16;
    settings.binaryResources.push_back({
        "Sequences/C42-matrix.mid", std::move(bytes)});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(
        compiled.ok(),
        "C42 matrix fixture should compile: " + std::string{context});
    auto graph = std::move(*compiled.graph);
    const auto offline = talas::runtime::renderOffline(graph, frameCount);
    const std::array<std::size_t, 5> commonPartitions{0, 1, 0, frameCount - 1, 0};
    const std::array<std::size_t, 4> offlinePartitions{0, frameCount - 1, 0, 1};
    const auto common = talas::runtime::runRuntimeConformance(graph, frameCount, commonPartitions,
        {.offlineBlockPartition = offlinePartitions,
         .realtimeZeroFrames = talas::runtime::ConformanceZeroFramePolicy::RequireRejection,
         .referenceMidiEvents = expected,
         .referenceGlobalControllerEvents = std::span<const talas::runtime::GlobalControllerEvent>{},
         .verifyResetReplay = true},
        {.absolute = 0.0, .relative = 0.0});
    require(common.ok(), "C42 common independent MIDI file oracle: " + std::string{context}
        + ": " + common.message);
    require(
        offline.ok() && offline.midiEvents.size() == expected.size(),
        "C42 matrix offline event count: " + std::string{context});
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(
            offline.midiEvents[index].frame == expected[index].frame
                && offline.midiEvents[index].bytes == expected[index].bytes,
            "C42 matrix exact offline event: " + std::string{context});
    }

    talas::runtime::RealtimeRenderSession session(graph);
    require(
        session.ready(),
        "C42 matrix should prepare: " + std::string{context}
            + ": " + std::string{session.error()});
    std::array<talas::runtime::RealtimeMidiOutputEvent, 16> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        session.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == expected.size(),
        "C42 matrix prepared event count: " + std::string{context});
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(
            events[index].frameOffset == expected[index].frame
                && events[index].byteCount == expected[index].bytes.size()
                && std::equal(
                    expected[index].bytes.begin(),
                    expected[index].bytes.end(),
                    events[index].bytes.begin()),
            "C42 matrix exact prepared event: " + std::string{context});
    }
}

void testMidiFileTerminalMatchesCompleteSharedParserMatrix()
{
    requireMidiFilePreparedMatchesExactOffline(
        midiFileFormatZeroFeatureFixture(),
        10.0,
        12,
        1,
        1,
        {
            {0, {0x90, 60, 64}},
            {0, {0x90, 61, 65}},
            {0, {0xc0, 5}},
            {0, {0xc0, 6}},
            {5, {0x80, 60, 0}},
        },
        "format-0 PPQ tempo/running-status/channel-filter/one-two-byte/stable-order");
    requireMidiFilePreparedMatchesExactOffline(
        midiFileFormatOneTempoFixture(),
        10.0,
        12,
        1,
        16,
        {{0, {0x90, 60, 64}}, {10, {0x80, 60, 0}}},
        "format-1 global tempo map");
    requireMidiFilePreparedMatchesExactOffline(
        midiFileFormatTwoTempoFixture(),
        10.0,
        12,
        1,
        16,
        {
            {0, {0x90, 60, 64}},
            {0, {0x90, 64, 64}},
            {5, {0x80, 64, 0}},
            {10, {0x80, 60, 0}},
        },
        "format-2 independent tempo maps");

    const std::array smpteCases{
        std::pair{std::uint8_t{0xe8}, std::uint8_t{24}},
        std::pair{std::uint8_t{0xe7}, std::uint8_t{25}},
        std::pair{std::uint8_t{0xe3}, std::uint8_t{30}},
        std::pair{std::uint8_t{0xe2}, std::uint8_t{30}},
    };
    for (const auto [rateCode, deltaTicks] : smpteCases)
    {
        requireMidiFilePreparedMatchesExactOffline(
            midiFileSmpteFixture(rateCode, deltaTicks),
            100.0,
            16,
            1,
            16,
            {{0, {0x90, 60, 64}}, {10, {0x80, 60, 0}}},
            "SMPTE 24/25/29.97/30 matrix");
    }
}

talas::runtime::PreparedRuntimeGraph compileOrderedOutputFixture(std::size_t channels)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output" + std::to_string(channels)));
    for (std::size_t index = 0; index < channels; ++index)
    {
        const auto id = "osc" + std::to_string(index);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {
            {"frequency", std::to_string(index + 1) + " hz"},
            {"amplitude", std::to_string(static_cast<double>(index + 1) / 8.0)},
        }));
        document.graph.connections.push_back(connection(
            id, "audioOut", "out", "out" + std::to_string(index + 1)));
    }
    auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), {64.0, 8});
    require(compiled.ok(), "ordered multichannel fixture should compile");
    return std::move(*compiled.graph);
}

void testSharedHarnessRetainsFourAndEightOrderedChannels()
{
    constexpr std::array<std::size_t, 4> partition{3, 1, 8, 5};
    for (const auto channels : {4U, 8U})
    {
        const auto graph = compileOrderedOutputFixture(channels);
        const auto result = talas::runtime::compareOfflineAndPreparedRealtime(graph, 17, partition);
        require(result.ok(), "shared N-channel harness should compare all outputs: " + result.message);
        require(result.realtimeChannels.size() == channels,
            "shared harness must retain every ordered output channel");
        for (std::size_t channel = 0; channel < channels; ++channel)
            for (std::size_t frame = 0; frame < 17; ++frame)
            {
                const auto expected = static_cast<double>(channel + 1) / 8.0
                    * std::sin(2.0 * std::acos(-1.0) * (channel + 1) * frame / 64.0);
                require(std::abs(result.realtimeChannels[channel][frame] - expected) < 1.0e-12,
                    "each output must match its independent oscillator oracle");
            }
    }
}

void testIndependentOracleRejectsSharedErrorsAndSupportsEmptyTransactions()
{
    const auto graph = compileOrderedOutputFixture(8);
    const std::array<std::size_t, 7> realtime{0, 3, 1, 0, 8, 5, 0};
    const std::array<std::size_t, 4> offline{0, 9, 0, 8};
    std::vector<std::vector<double>> expected(8, std::vector<double>(17));
    for (std::size_t channel = 0; channel < 8; ++channel)
        for (std::size_t frame = 0; frame < 17; ++frame)
            expected[channel][frame] = static_cast<double>(channel + 1) / 8.0
                * std::sin(2.0 * std::acos(-1.0) * (channel + 1) * frame / 64.0);
    const auto run = [&] {
        return talas::runtime::runRuntimeConformance(graph, 17, realtime,
            {.offlineBlockPartition = offline, .referenceChannels = expected,
             .realtimeZeroFrames = talas::runtime::ConformanceZeroFramePolicy::RequireRejection});
    };
    const auto initial = run();
    require(initial.ok(), "independent N-channel oracle must pass with zero-frame transactions: " + initial.message);
    require(!talas::runtime::runRuntimeConformance(graph, 17, realtime, {}).ok(),
        "a rejected empty transaction must not count as the default successful no-op");
    const auto filter = compileFilterFixture();
    const std::array<std::size_t, 5> filterPartition{0, 3, 0, 13, 0};
    require(talas::runtime::compareOfflineAndPreparedRealtime(filter, 16, filterPartition).ok(),
        "a profile accepting empty calls must preserve state around positive blocks");
    std::swap(expected[3], expected[6]);
    auto swapped = run();
    require(!swapped.ok() && swapped.mismatch && swapped.mismatch->channel == 3
            && swapped.mismatch->comparison == "reference/offline",
        "an independent oracle must detect channel swaps even when both runtimes agree");
    std::swap(expected[3], expected[6]);
    expected[7][16] += 0.1;
    const auto tail = run();
    require(!tail.ok() && tail.mismatch && tail.mismatch->channel == 7
            && tail.mismatch->frame == 16,
        "the oracle must inspect the last frame of the last channel");
    expected[7].pop_back();
    require(!run().ok(), "an incomplete oracle must be rejected");
    expected[7].push_back(std::numeric_limits<double>::quiet_NaN());
    require(!run().ok(), "a nonfinite oracle must be rejected");
    const std::array<std::size_t, 1> invalidOffline{16};
    require(!talas::runtime::runRuntimeConformance(graph, 17, realtime,
                {.offlineBlockPartition = invalidOffline}).ok(),
        "offline partitions must account for every requested frame");
}

void testSharedMidiScheduleMatchesIndependentGateOracle()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.75"}}),
        node("mapper", "talas.midiMapper", {
            textParameter("map", "gate"), {"channel", "1"}, {"polyphony", "1"},
            choiceParameter("voiceModel", "legacyPassthrough"),
            {"left", "1"}, {"right", "-0.5"},
            textParameter("source", ""), resourceParameter("midiFile", ""),
            {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
            textParameter("script", ""), booleanParameter("shared", false),
        }),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {
        connection("osc", "audioOut", "mapper", "audioIn"),
        connection("mapper", "leftOut", "out", "audioIn"),
        connection("mapper", "rightOut", "out", "rightIn"),
    };
    auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), {64.0, 8});
    require(compiled.ok(), "scheduled MIDI mapper fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    using Kind = talas::runtime::ConformanceMidiNoteKind;
    std::array<talas::runtime::ConformanceMidiNote, 4> notes{{
        {2, Kind::NoteOn, 1, 60, 127}, {9, Kind::NoteOff, 1, 60, 0},
        {9, Kind::NoteOn, 1, 60, 127}, {15, Kind::NoteOff, 1, 60, 0},
    }};
    const std::array<std::size_t, 8> realtime{0, 2, 0, 7, 0, 6, 2, 0};
    const std::array<std::size_t, 5> offline{3, 0, 8, 6, 0};
    std::vector<std::vector<double>> expected(2, std::vector<double>(17));
    for (std::size_t frame = 2; frame < 15; ++frame)
    {
        expected[0][frame] = 0.75 * std::sin(2.0 * std::acos(-1.0) * frame / 64.0);
        expected[1][frame] = -0.5 * expected[0][frame];
    }
    const auto run = [&] {
        return talas::runtime::runRuntimeConformance(*compiled.graph, 17, realtime,
            {.offlineBlockPartition = offline, .midiNotes = notes, .referenceChannels = expected,
             .realtimeZeroFrames = talas::runtime::ConformanceZeroFramePolicy::RequireRejection,
             .verifyResetReplay = true});
    };
    const auto result = run();
    require(result.ok() && result.resetReplayVerified,
        "shared note schedule and reset replay must match analytical gate/oscillator output: " + result.message);
    const std::array<std::size_t, 3> midBlockPartition{8, 8, 1};
    const auto midBlock = talas::runtime::runRuntimeConformance(*compiled.graph, 17, midBlockPartition,
        {.midiNotes = notes, .referenceChannels = expected});
    require(midBlock.ok() && midBlock.realtimeChannels == result.realtimeChannels,
        "events inside callbacks must produce the same stream as events on callback boundaries: " + midBlock.message);
    const std::vector<talas::runtime::ConformanceInputEvent> orderedNotes(notes.begin(), notes.end());
    const auto typedNotes = talas::runtime::runRuntimeConformance(*compiled.graph, 17, midBlockPartition,
        {.referenceChannels = expected, .verifyResetReplay = true, .inputEvents = orderedNotes});
    require(typedNotes.ok() && typedNotes.resetReplayVerified,
        "an explicit note-only stream must match the same independent gate oracle: " + typedNotes.message);
    require(run().realtimeChannels == result.realtimeChannels,
        "a fresh harness run must not inherit prior voice state");
    std::swap(notes[1], notes[2]);
    const auto reordered = run();
    require(!reordered.ok() && reordered.mismatch
            && reordered.mismatch->comparison == "reference/offline",
        "same-frame note order mutation must fail the independent gate oracle");
    std::swap(notes[1], notes[2]);
    notes.back().frame = 17;
    require(!run().ok(), "an event outside the requested range must not be silently omitted");
    notes.back().frame = 15;
    notes[0].frame = 10;
    require(!run().ok(), "unsorted event schedules must be rejected");
    notes[0].frame = 2;
    notes[0].channel = 0;
    require(!run().ok(), "invalid MIDI channels must be rejected");
}

void testOverflowingDifferenceDoesNotHideAnOracleMismatch()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator", {
            {"frequency", "16 hz"}, {"amplitude", "-1.7976931348623157e308"}}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("osc", "audioOut", "out", "audioIn")};
    auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), {64.0, 2});
    require(compiled.ok(), "extreme finite oscillator fixture must compile");
    const auto maximum = std::numeric_limits<double>::max();
    const std::vector<std::vector<double>> expected{{0.0, maximum}};
    const std::array<std::size_t, 1> partition{2};
    const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 2, partition,
        {.referenceChannels = expected}, {.absolute = maximum, .relative = 0.5});
    require(!result.ok() && result.mismatch
            && result.mismatch->comparison == "reference/offline" && result.mismatch->frame == 1,
        "overflowing difference and tolerance must not turn infinity > infinity into a false pass: " + result.message);
}

void testOfflineDriverDoesNotInvokeKernelForEmptyRange()
{
    const auto graph = compileOscillatorFixture();
    const auto result = talas::runtime::renderOffline(graph, 0);
    require(result.ok(), "zero-frame offline render should remain a valid empty range");
    require(result.mono.empty(), "zero-frame offline render should not produce samples");
}

void testCommonHarnessAdmitsEventOnlyTerminals()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {node("terminal", "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 60 127")})};
    auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), {8.0, 7});
    require(compiled.ok(), "common raw MIDI fixture must compile");
    const std::array<std::size_t, 3> blocks{3, 1, 3};
    const auto result = talas::runtime::compareOfflineAndPreparedRealtime(*compiled.graph, 7, blocks);
    require(result.ok(), "common harness must admit an event-only terminal: " + result.message);
    require(result.offlineChannels.empty() && result.realtimeChannels.empty()
        && result.offlineMidiEvents.size() == 1 && result.realtimeMidiEvents.size() == 1,
        "event-only conformance must retain its trace without manufacturing audio channels");
}

void testCommonTerminalOraclesDistinguishPayloadOrderAndSilence()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.graph.nodes = {node("terminal", "talas.midiFileEcho", {
        {"lowChannel", "1"}, {"highChannel", "1"},
        resourceParameter("fileName", "Sequences/common.mid")})};
    RenderSettings settings{10.0, 12};
    settings.binaryResources.push_back({"Sequences/common.mid", midiFileFormatZeroFeatureFixture()});
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "common ordered MIDI file fixture must compile");
    std::vector<MidiEvent> expected{
        {0, {0x90, 60, 64}}, {0, {0x90, 61, 65}}, {0, {0xc0, 5}},
        {0, {0xc0, 6}}, {5, {0x80, 60, 0}},
    };
    const std::array<std::size_t, 7> blocks{0, 3, 2, 0, 1, 6, 0};
    const std::array<std::size_t, 4> offlineBlocks{5, 0, 1, 6};
    const auto run = [&] {
        return runRuntimeConformance(*compiled.graph, 12, blocks,
            {.offlineBlockPartition = offlineBlocks,
             .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
             .referenceMidiEvents = expected,
             .referenceGlobalControllerEvents = std::span<const GlobalControllerEvent>{}});
    };
    const auto good = run();
    require(good.ok(), "independent ordered MIDI file oracle: " + good.message);
    const auto requireMismatch = [&](std::string_view field, std::size_t index) {
        const auto result = run();
        require(!result.ok() && result.eventMismatch
            && result.eventMismatch->comparison == "reference/offline"
            && result.eventMismatch->stream == "MIDI"
            && result.eventMismatch->index == index && result.eventMismatch->field == field,
            "independent MIDI oracle must distinguish " + std::string(field) + ": " + result.message);
    };
    std::swap(expected[0], expected[1]);
    requireMismatch("bytes/order", 0);
    std::swap(expected[0], expected[1]);
    expected.back().bytes.back() = 3;
    requireMismatch("bytes/order", 4);
    expected.back().bytes.back() = 0;
    expected.back().frame = 6;
    requireMismatch("frame/order", 4);
    expected.back().frame = 5;
    expected.pop_back();
    requireMismatch("count", 4);
    expected.clear();
    requireMismatch("count", 0);
    expected = {{12, {0x90, 60, 1}}};
    require(!run().ok(), "oracle at the exclusive render end must be rejected");
    expected = {{2, {0x90, 60, 1}}, {1, {0x80, 60, 0}}};
    require(!run().ok(), "an unsorted oracle must be rejected, never sorted into a pass");

    // An explicitly empty oracle must also succeed for a truly silent source.
    document.graph.nodes = {node("terminal", "talas.midiFileEcho", {
        {"lowChannel", "1"}, {"highChannel", "1"}, resourceParameter("fileName", "")})};
    auto silent = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {10.0, 12});
    require(silent.ok(), "silent MIDI file terminal must compile");
    const auto silence = runRuntimeConformance(*silent.graph, 12, blocks,
        {.realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
         .referenceMidiEvents = std::span<const MidiEvent>{},
         .referenceGlobalControllerEvents = std::span<const GlobalControllerEvent>{}});
    require(silence.ok(), "explicit empty terminal oracles must verify silence: " + silence.message);
}

void testCommonControllerOracleIncludesNamesValuesAndContinuation()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("value", "talas.linearRamp", {{"duration", "1 s"}, {"start", "0"}, {"end", "1"}}),
        node("terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "!Generated"), textParameter("value", "")}),
    };
    document.graph.connections = {{"value", "value", "terminal", "value"}};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 7});
    require(compiled.ok(), "common controller fixture must compile");
    std::vector<GlobalControllerEvent> expected;
    for (std::size_t frame = 0; frame <= 4; ++frame)
        expected.push_back({frame, "!Generated", static_cast<double>(frame) / 4.0});
    const std::array<std::size_t, 7> blocks{0, 3, 0, 1, 2, 1, 0};
    const std::array<std::size_t, 4> offlineBlocks{1, 0, 5, 1};
    const auto run = [&](ConformanceTolerance tolerance = {}) {
        return runRuntimeConformance(*compiled.graph, 7, blocks,
            {.offlineBlockPartition = offlineBlocks,
             .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
             .referenceMidiEvents = std::span<const MidiEvent>{},
             .referenceGlobalControllerEvents = expected, .verifyResetReplay = true}, tolerance);
    };
    const auto good = run();
    require(good.ok() && good.offlineGlobalControllerEvents.size() == 5
        && good.realtimeGlobalControllerEvents.size() == 5,
        "independent controller oracle across continuation: " + good.message);
    expected[3].name = "!Other";
    const auto wrongName = run();
    require(wrongName.eventMismatch && wrongName.eventMismatch->field == "name/order"
        && wrongName.eventMismatch->index == 3,
        "a wrong generated controller name must fail its independent oracle");
    expected[3].name = "!Generated";
    expected.back().value += 0.01;
    const auto wrongValue = run();
    require(wrongValue.eventMismatch && wrongValue.eventMismatch->field == "value"
        && wrongValue.eventMismatch->index == 4,
        "a wrong final controller value must fail its independent oracle");
    require(run({.absolute = 0.02, .relative = 0.0}).ok(),
        "controller values must use the declared numeric tolerance");
    expected.back().value = std::numeric_limits<double>::quiet_NaN();
    require(!run().ok(), "nonfinite controller references must never pass");
}

void testCommonTerminalCapacityFailureIsNotAnEmptyTracePass()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {node("terminal", "talas.midiFileEcho", {
        {"lowChannel", "1"}, {"highChannel", "1"},
        resourceParameter("fileName", "Sequences/capacity.mid")})};
    talas::runtime::RenderSettings settings{10.0, 12};
    settings.maxRealtimeTerminalEvents = 1;
    settings.binaryResources.push_back({"Sequences/capacity.mid", midiFileFormatZeroFeatureFixture()});
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "limited caller event capacity fixture must compile");
    const std::array<std::size_t, 1> blocks{12};
    const auto preparationFailure = talas::runtime::runRuntimeConformance(*compiled.graph, 12, blocks, {});
    require(!preparationFailure.ok()
        && preparationFailure.message.find("session failed") != std::string::npos,
        "an oversized MIDI file must fail prepared conformance admission");

    // A dynamic raw stream prepares successfully, then exceeds the capacity
    // inside a callback. Exercise that distinct publication failure as well.
    document.graph.nodes = {node("terminal", "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 {!Byte} 127")})};
    settings = {8.0, 7};
    settings.maxRealtimeTerminalEvents = 1;
    settings.eventValues = {{"Byte", 64.0, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {0, "Byte", 64.0, talas::expr::ValueDomain::Scalar, talas::core::AutomationInterpolation::Linear},
        {6, "Byte", 70.0, talas::expr::ValueDomain::Scalar},
    };
    compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "dynamic limited event capacity fixture must compile");
    const std::array<std::size_t, 1> dynamicBlocks{7};
    const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 7, dynamicBlocks, {});
    require(!result.ok() && result.message.find("render outcome differs") != std::string::npos,
        "a capacity-exceeded terminal render must fail conformance: " + result.message);
}

void testCommonResetReplayChecksActualState()
{
    const auto graph = compileOrderedOutputFixture(8);
    const std::array<std::size_t, 4> blocks{3, 1, 8, 5};
    const std::array<std::size_t, 3> offlineBlocks{9, 0, 8};
    std::vector<std::vector<double>> expected(8, std::vector<double>(17));
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
        for (std::size_t frame = 0; frame < 17; ++frame)
            expected[channel][frame] = static_cast<double>(channel + 1) / 8.0
                * std::sin(2.0 * std::numbers::pi * static_cast<double>((channel + 1) * frame) / 64.0);
    const auto result = talas::runtime::runRuntimeConformance(graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected, .verifyResetReplay = true});
    require(result.ok() && result.resetReplayVerified,
        "common reset replay must exercise real sessions and the independent 8-channel oracle: " + result.message);
}

void testCommonVariableScheduleMatchesIndependentOracle()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"gain", "Gain", talas::expr::ValueDomain::Scalar, 0.25}};
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "@gain"}}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("osc", "audioOut", "out", "audioIn")};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {64.0, 8});
    require(compiled.ok(), "common variable schedule fixture must compile");
    const auto identity = compiled.graph->preparedVariables().cells.front().identity;
    const std::array assignments{
        ConformanceVariableAssignment{0, identity, 0.25},
        ConformanceVariableAssignment{2, identity, 0.5},
        ConformanceVariableAssignment{9, identity, 0.75},
        ConformanceVariableAssignment{9, identity, 1.0},
        ConformanceVariableAssignment{15, identity, -0.5},
    };
    const std::array<std::size_t, 4> blocks{2, 7, 6, 2};
    const std::array<std::size_t, 4> offlineBlocks{3, 0, 8, 6};
    std::vector<std::vector<double>> expected(1, std::vector<double>(17));
    for (std::size_t frame = 0; frame < 17; ++frame)
    {
        const auto gain = frame < 2 ? 0.25 : frame < 9 ? 0.5 : frame < 15 ? 1.0 : -0.5;
        expected[0][frame] = gain * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    }
    const auto result = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected,
         .verifyResetReplay = true, .variableAssignments = assignments});
    require(result.ok() && result.resetReplayVerified,
        "timed variables and last-wins order must match independent gain oracle: " + result.message);

    document.graph.nodes.push_back(node("mapper", "talas.midiMapper", {
        textParameter("map", "gate"), {"channel", "1"}, {"polyphony", "1"},
        choiceParameter("voiceModel", "legacyPassthrough"),
        {"left", "1"}, {"right", "1"},
        textParameter("source", ""), resourceParameter("midiFile", ""),
        textParameter("script", ""), {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
        booleanParameter("shared", false),
    }));
    document.graph.connections = {
        connection("osc", "audioOut", "mapper", "audioIn"),
        connection("mapper", "leftOut", "out", "audioIn"),
    };
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {64.0, 8});
    require(compiled.ok(), "mixed variable and MIDI gate fixture must compile");
    const std::array notes{
        ConformanceMidiNote{2, ConformanceMidiNoteKind::NoteOn, 1, 60, 127},
        ConformanceMidiNote{9, ConformanceMidiNoteKind::NoteOff, 1, 60, 0},
        ConformanceMidiNote{9, ConformanceMidiNoteKind::NoteOn, 1, 60, 127},
        ConformanceMidiNote{15, ConformanceMidiNoteKind::NoteOff, 1, 60, 0},
    };
    expected[0][1] = 0.0;
    expected[0][15] = expected[0][16] = 0.0;
    const auto mixed = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .midiNotes = notes, .referenceChannels = expected,
         .verifyResetReplay = true, .variableAssignments = assignments});
    require(mixed.ok() && mixed.resetReplayVerified,
        "same-frame project controls and MIDI notes must compose atomically: " + mixed.message);

    const std::array<std::size_t, 6> irregular{0, 8, 0, 8, 1, 0};
    const auto insideBlocks = runRuntimeConformance(*compiled.graph, 17, irregular,
        {.midiNotes = notes, .referenceChannels = expected,
         .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
         .verifyResetReplay = true, .variableAssignments = assignments});
    require(insideBlocks.ok(), "mixed controls inside callbacks and across empty calls: " + insideBlocks.message);

    auto changed = assignments;
    const auto runChanged = [&] {
        return runRuntimeConformance(*compiled.graph, 17, blocks,
            {.midiNotes = notes, .referenceChannels = expected, .variableAssignments = changed});
    };
    std::swap(changed[2], changed[3]);
    const auto reordered = runChanged();
    require(reordered.mismatch && reordered.mismatch->comparison == "reference/offline"
        && reordered.mismatch->frame == 9,
        "changing equal-frame variable order must fail the independent mixed oracle: " + reordered.message);
    changed = assignments;
    changed.back().frame = 17;
    require(!runChanged().ok(), "out-of-range variable inputs must not be silently dropped");
    changed = assignments;
    changed[1].frame = 10;
    require(!runChanged().ok(), "unsorted variable inputs must be rejected");
    changed = assignments;
    changed[1].target.instancePath = {"missing-instance"};
    require(!runChanged().ok(), "a stale variable owner path must be rejected");
    changed = assignments;
    changed[1].domain = talas::expr::ValueDomain::Hertz;
    require(!runChanged().ok(), "a variable domain mismatch must be rejected");
    changed = assignments;
    changed[1].value = std::numeric_limits<double>::quiet_NaN();
    require(!runChanged().ok(), "nonfinite variable inputs must be rejected");

    // Admission of non-voice cells must not weaken voice lifetime checks.
    OfflineMidiVoiceEvent assignment;
    assignment.frame = 0;
    assignment.batchId = assignment.sequence = 1;
    assignment.kind = OfflineMidiVoiceEventKind::VariableAssignment;
    assignment.target = identity;
    assignment.value = 0.5;
    assignment.voiceGeneration = 1;
    const auto accepts = [&] {
        OfflineRenderSession session(*compiled.graph);
        return session.scheduleMidiVoiceEvents(std::span{&assignment, 1});
    };
    require(!accepts(), "a non-voice cell must reject a forged voice generation");
    assignment.voiceGeneration = 0;
    require(accepts(), "an exact non-voice assignment must enter the mixed backend");
    assignment.domain = talas::expr::ValueDomain::Hertz;
    require(!accepts(), "the mixed backend must independently validate variable domain");
    assignment.domain = talas::expr::ValueDomain::Scalar;
    assignment.target.variableId = "missing-variable";
    require(!accepts(), "the mixed backend must independently validate variable identity");
    assignment.target = identity;
    assignment.value = std::numeric_limits<double>::infinity();
    require(!accepts(), "the mixed backend must independently reject nonfinite assignments");

    document.graph.nodes.front() = node("osc", "talas.oscillator", {
        {"frequency", "1 hz"}, {"amplitude", "!KeyDown * @gain"}});
    document.graph.nodes.back() = node("mapper", "talas.midiVoice", {
        {"channel", "1"}, {"polyphony", "1"}, {"left", "1"}, {"right", "1"},
        textParameter("source", ""), resourceParameter("midiFile", ""),
        textParameter("script", ""), {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
        booleanParameter("shared", false),
    });
    RenderSettings voiceSettings{64.0, 8};
    voiceSettings.eventValues = {{"KeyDown", 0.0, talas::expr::ValueDomain::Scalar}};
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), voiceSettings);
    require(compiled.ok(), "project control inside a MIDI Voice fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const std::array voiceNotes{
        ConformanceMidiNote{2, ConformanceMidiNoteKind::NoteOn, 1, 60, 127},
        ConformanceMidiNote{15, ConformanceMidiNoteKind::NoteOff, 1, 60, 0},
    };
    // This native fixed-frequency source advances while silent. Note-on
    // resets voice expression state, not the oscillator's running phase.
    for (std::size_t frame = 0; frame < 17; ++frame)
        expected[0][frame] = frame < 2 || frame >= 15 ? 0.0
            : (frame < 9 ? 0.5 : 1.0)
                * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    const auto voice = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .midiNotes = voiceNotes, .referenceChannels = expected,
         .verifyResetReplay = true, .variableAssignments = assignments});
    require(voice.ok() && voice.resetReplayVerified,
        "project controls must also reach the active MIDI Voice source: " + voice.message
        + (voice.mismatch ? " expected=" + std::to_string(voice.mismatch->offline)
            + " actual=" + std::to_string(voice.mismatch->realtime) : ""));
}

void testCommonVariableTransactionsAreAtomic()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {
        {"a", "A", talas::expr::ValueDomain::Scalar, 1.0},
        {"b", "B", talas::expr::ValueDomain::Scalar, 0.0},
    };
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1 / (@a - @b)"}}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("osc", "audioOut", "out", "audioIn")};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {64.0, 8});
    require(compiled.ok(), "atomic variable expression fixture must compile");
    const auto& cells = compiled.graph->preparedVariables().cells;
    const auto a = std::ranges::find_if(cells, [](const auto& cell) { return cell.identity.variableId == "a"; });
    const auto b = std::ranges::find_if(cells, [](const auto& cell) { return cell.identity.variableId == "b"; });
    require(a != cells.end() && b != cells.end(), "atomic variables must retain exact identities");
    // Sequential evaluation would divide by zero after B; one frame's batch
    // publishes A and B together and therefore keeps the gain exactly one.
    const std::array assignments{
        ConformanceVariableAssignment{3, b->identity, 1.0},
        ConformanceVariableAssignment{3, a->identity, 2.0},
    };
    std::vector<std::vector<double>> expected(1, std::vector<double>(9));
    for (std::size_t frame = 0; frame < 9; ++frame)
        expected[0][frame] = std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    const std::array<std::size_t, 2> blocks{8, 1};
    const auto result = runRuntimeConformance(*compiled.graph, 9, blocks,
        {.referenceChannels = expected, .variableAssignments = assignments});
    require(result.ok(), "same-frame variable batch must avoid transient invalid expressions: " + result.message);
    const std::vector<ConformanceInputEvent> orderedAssignments(assignments.begin(), assignments.end());
    const auto explicitResult = runRuntimeConformance(*compiled.graph, 9, blocks,
        {.referenceChannels = expected, .inputEvents = orderedAssignments});
    require(explicitResult.ok(), "ordered pure variables must retain the independent offline driver: " + explicitResult.message);
}

void testCommonTerminalRetryPreservesQueuedControlsAndState()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"value", "Value", talas::expr::ValueDomain::Scalar, 0.25}};
    document.graph.nodes = {
        node("value", "talas.constant", {{"value", "@value"}}),
        node("terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "!Generated"), textParameter("value", "")}),
    };
    document.graph.connections = {{"value", "value", "terminal", "value"}};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {8.0, 5});
    require(compiled.ok(), "retryable controller fixture must compile");
    const auto identity = compiled.graph->preparedVariables().cells.front().identity;
    const std::array assignments{
        ConformanceVariableAssignment{0, identity, 0.25},
        ConformanceVariableAssignment{2, identity, 0.5},
        ConformanceVariableAssignment{4, identity, 0.75},
        ConformanceVariableAssignment{6, identity, 1.0},
        ConformanceVariableAssignment{8, identity, 0.125},
    };
    const std::array<std::size_t, 2> blocks{5, 4};
    const std::array<std::size_t, 3> offlineBlocks{3, 0, 6};
    const std::array probes{
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .controllerEvents = 1},
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .controllerNameBytes = 0},
        ConformanceTerminalCapacityProbe{.blockIndex = 1, .controllerEvents = 1},
    };
    const std::vector<GlobalControllerEvent> expected{
        {0, "!Generated", 0.25}, {2, "!Generated", 0.5}, {4, "!Generated", 0.75},
        {6, "!Generated", 1.0}, {8, "!Generated", 0.125},
    };
    const auto result = runRuntimeConformance(*compiled.graph, 9, blocks,
        {.offlineBlockPartition = offlineBlocks,
         .referenceMidiEvents = std::span<const MidiEvent>{}, .referenceGlobalControllerEvents = expected,
         .verifyResetReplay = true, .variableAssignments = assignments, .terminalCapacityProbes = probes});
    require(result.ok() && result.resetReplayVerified && result.terminalFailureProbesVerified == 6,
        "failed terminal attempts must preserve controls/state for exact retry and replay: " + result.message);

    const auto invalidProbe = [&](ConformanceTerminalCapacityProbe probe,
                                  std::span<const std::size_t> partition = {}) {
        const std::array invalid{probe};
        return runRuntimeConformance(*compiled.graph, 9, partition.empty() ? std::span{blocks} : partition,
            {.variableAssignments = assignments, .terminalCapacityProbes = invalid});
    };
    require(!invalidProbe({.blockIndex = 2, .controllerEvents = 1}).ok(),
        "an out-of-range failure probe must not disappear");
    require(!invalidProbe({.blockIndex = 0}).ok(), "a capacity probe must specify a capacity");
    require(!invalidProbe({.blockIndex = 0, .controllerNameBytes = 100}).ok(),
        "a probe cannot exceed its backing storage");
    const auto noFailure = invalidProbe({.blockIndex = 0, .controllerEvents = 256});
    require(!noFailure.ok() && noFailure.message.find("did not return CapacityExceeded") != std::string::npos,
        "a successful callback must not count as an expected capacity failure");
    const std::array<std::size_t, 3> emptyFirst{0, 5, 4};
    require(!invalidProbe({.blockIndex = 0, .controllerEvents = 0}, emptyFirst).ok(),
        "empty-call rejection cannot substitute for a capacity failure");
    const std::array unordered{
        ConformanceTerminalCapacityProbe{.blockIndex = 1, .controllerEvents = 1},
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .controllerEvents = 1},
    };
    require(!runRuntimeConformance(*compiled.graph, 9, blocks,
        {.variableAssignments = assignments, .terminalCapacityProbes = unordered}).ok(),
        "unsorted failure probes must be rejected, never silently skipped");
}

void testCommonMidiCapacityRetryPreservesOrderAndChangeState()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.graph.nodes = {node("terminal", "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 {!Byte} 127")})};
    RenderSettings settings{8.0, 4};
    settings.eventValues = {{"Byte", 64.0, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {0, "Byte", 64.0, talas::expr::ValueDomain::Scalar, talas::core::AutomationInterpolation::Linear},
        {6, "Byte", 70.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "raw MIDI capacity retry fixture must compile");
    const std::array<std::size_t, 2> blocks{3, 4};
    const std::array<std::size_t, 3> offlineBlocks{1, 0, 6};
    const std::array probes{
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .midiEvents = 1},
        ConformanceTerminalCapacityProbe{.blockIndex = 1, .midiEvents = 2},
    };
    std::vector<MidiEvent> expected;
    for (std::size_t frame = 0; frame < 7; ++frame)
        expected.push_back({frame, {0x90, static_cast<std::uint8_t>(64 + frame), 127}});
    const auto raw = runRuntimeConformance(*compiled.graph, 7, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceMidiEvents = expected,
         .verifyResetReplay = true, .terminalCapacityProbes = probes});
    require(raw.ok() && raw.terminalFailureProbesVerified == 4,
        "raw MIDI retry must preserve exact change-suppression state: " + raw.message);

    document.graph.nodes = {node("terminal", "talas.midiFileEcho", {
        {"lowChannel", "1"}, {"highChannel", "1"}, resourceParameter("fileName", "Sequences/retry.mid")})};
    settings = {10.0, 6};
    settings.binaryResources.push_back({"Sequences/retry.mid", midiFileFormatZeroFeatureFixture()});
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "MIDI file cursor retry fixture must compile");
    const std::array<std::size_t, 2> fileBlocks{6, 6};
    const std::array fileProbes{
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .midiEvents = 1},
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .midiEvents = 3},
    };
    expected = {{0, {0x90, 60, 64}}, {0, {0x90, 61, 65}}, {0, {0xc0, 5}},
        {0, {0xc0, 6}}, {5, {0x80, 60, 0}}};
    const auto file = runRuntimeConformance(*compiled.graph, 12, fileBlocks,
        {.referenceMidiEvents = expected, .verifyResetReplay = true, .terminalCapacityProbes = fileProbes});
    require(file.ok() && file.terminalFailureProbesVerified == 4,
        "MIDI file retry must preserve cursor and equal-time order: " + file.message);
}

void testCommonSoundStateCheckpointsAndOfflineFailureEvidence()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    for (const auto& [id, amplitude] : std::array{
             std::pair{"toneA", "0"}, std::pair{"toneB", "1 / !Divisor"}})
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = id;
        definition.displayName = id;
        definition.graph.nodes = {node("osc", "talas.oscillator",
            {{"frequency", "1 hz"}, {"amplitude", amplitude}})};
        definition.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
        document.soundDefinitions.push_back(std::move(definition));
    }
    talas::project::ProjectSoundVariableDeclaration selected;
    selected.variableId = "selected";
    selected.displayName = "Selected";
    selected.candidateDefinitionIds = {"toneA", "toneB"};
    document.soundVariableLibrary.variables = {selected};
    document.graph.nodes = {
        node("selector", "talas.variable", {textParameter("variableId", "selected")}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("selector", "audioOut", "out", "audioIn")};
    RenderSettings settings{16.0, 4};
    settings.eventValues = {{"Divisor", 1.0}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Sound state oracle fixture must compile");
    const auto identity = compiled.graph->preparedSoundVariables().cells.front().identity;
    const std::vector<OfflineSoundVariableValue> stateA{{identity, "toneA"}};
    const std::vector<OfflineSoundVariableValue> stateB{{identity, "toneB"}};
    std::vector<ConformanceSoundStateCheckpoint> checkpoints{
        {0, stateA, stateB}, {2, stateA}, {4, stateB}, {8, stateB},
    };
    const std::array<ConformanceInputEvent, 2> inputs{
        ConformanceSoundVariableAssignment{0, identity, "toneA"},
        ConformanceSoundVariableAssignment{2, identity, "toneB"},
    };
    const std::array<std::size_t, 5> blocks{0, 2, 2, 0, 4};
    const std::array<std::size_t, 5> offlineBlocks{1, 1, 0, 2, 4};
    const auto run = [&] {
        return runRuntimeConformance(*compiled.graph, 8, blocks,
            {.offlineBlockPartition = offlineBlocks,
             .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
             .verifyResetReplay = true, .inputEvents = inputs, .soundStateCheckpoints = checkpoints});
    };
    const auto good = run();
    require(good.ok() && good.offlineSoundStates.size() == 8 && good.realtimeSoundStates.size() == 8,
        "shared state checkpoints must retain first and reset publications: " + good.message);
    require(!good.offlineSoundStates[0].afterReset && good.offlineSoundStates[4].afterReset
        && good.offlineSoundStates[4].frame == 0
        && good.offlineSoundStates[4].values.front().selectedDefinitionId == "toneB",
        "reset checkpoints must observe retained state before frame-zero inputs restore the audio baseline");
    checkpoints[2].expected = stateA;
    const auto wrongValue = run();
    require(!wrongValue.ok() && wrongValue.stateMismatch && wrongValue.stateMismatch->frame == 4
        && wrongValue.stateMismatch->comparison == "reference/offline"
        && wrongValue.stateMismatch->field == "selectedDefinitionId",
        "a wrong independent Sound selection must fail at its exact boundary");
    checkpoints[2].expected = stateB;
    checkpoints[0].expectedAfterReset.reset();
    const auto wrongReset = run();
    require(!wrongReset.ok() && wrongReset.stateMismatch && wrongReset.stateMismatch->afterReset
        && wrongReset.stateMismatch->frame == 0,
        "state oracles must expose retained reset state even when the audio alone repeats");
    checkpoints[0].expectedAfterReset = stateB;
    checkpoints[2].expected.front().identity = {};
    const auto wrongIdentity = run();
    require(wrongIdentity.stateMismatch && wrongIdentity.stateMismatch->field == "identity",
        "state comparison must require complete exact identities");
    checkpoints[2].expected.clear();
    const auto wrongCount = run();
    require(wrongCount.stateMismatch && wrongCount.stateMismatch->field == "count",
        "an empty oracle must not ignore published Sound cells");
    checkpoints[2].expected = stateB;
    const auto validCheckpoints = checkpoints;
    for (const auto frame : std::array<std::size_t, 4>{0, 1, 9, 8})
    {
        checkpoints[1].frame = frame;
        const auto invalid = run();
        require(!invalid.ok() && !invalid.stateMismatch && invalid.offlineSoundStates.empty(),
            "duplicate, unshared, out-of-range and descending checkpoints must fail validation");
    }
    checkpoints = validCheckpoints;
    const auto unusedReset = runRuntimeConformance(*compiled.graph, 8, blocks,
        {.offlineBlockPartition = offlineBlocks, .inputEvents = inputs, .soundStateCheckpoints = checkpoints});
    require(!unusedReset.ok() && unusedReset.offlineSoundStates.empty(),
        "reset-specific expectations require an enabled reset pass");
    const auto emptyGraph = compileOscillatorFixture();
    const std::array emptyCheckpoint{ConformanceSoundStateCheckpoint{0, {}}};
    const std::array<std::size_t, 1> emptyBlocks{4};
    const auto noCells = runRuntimeConformance(emptyGraph, 4, emptyBlocks,
        {.soundStateCheckpoints = emptyCheckpoint});
    require(noCells.ok() && noCells.realtimeSoundStates.front().publicationValid,
        "a valid empty Sound publication must satisfy an explicitly empty oracle");

    const std::array<ConformanceInputEvent, 2> failingInputs{
        ConformanceSoundVariableAssignment{2, identity, "toneB"},
        ConformanceEventValueAssignment{6, "Divisor", 0.0},
    };
    const std::array failureCheckpoint{ConformanceSoundStateCheckpoint{0, stateA}};
    const std::array<std::size_t, 3> failingBlocks{4, 4, 1};
    const auto failed = runRuntimeConformance(*compiled.graph, 9, failingBlocks,
        {.inputEvents = failingInputs, .soundStateCheckpoints = failureCheckpoint});
    require(!failed.ok() && failed.offlineFailureSoundState,
        "late offline failure must retain its before/after Sound publication evidence");
    const auto& evidence = *failed.offlineFailureSoundState;
    require(evidence.startFrame == 0 && evidence.requestedFrames == 9 && !evidence.afterReset
        && evidence.before.size() == 1 && evidence.before.front().identity == identity
        && evidence.before.front().selectedDefinitionId == "toneA",
        "failure evidence must identify the attempted public call and its original publication");
    OfflineRenderSession direct(*compiled.graph);
    std::array<OfflineMidiVoiceEvent, 2> directInputs{};
    directInputs[0].frame = 2;
    directInputs[0].kind = OfflineMidiVoiceEventKind::SoundVariableAssignment;
    directInputs[0].soundTarget = identity;
    directInputs[0].selectedDefinitionId = "toneB";
    directInputs[0].batchId = 3;
    directInputs[0].sequence = 1;
    directInputs[1].frame = 6;
    directInputs[1].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    directInputs[1].eventValueName = "Divisor";
    directInputs[1].value = 0.0;
    directInputs[1].batchId = 7;
    directInputs[1].sequence = 2;
    require(direct.scheduleMidiVoiceEvents(directInputs) && !direct.render(9).ok(),
        "the direct session must reproduce the late combined-input failure");
    const auto directAfter = direct.soundVariableValues();
    require(evidence.after.size() == directAfter.size(), "failure diagnostic must preserve every published cell");
    for (std::size_t index = 0; index < directAfter.size(); ++index)
        require(evidence.after[index].identity == directAfter[index].identity
            && evidence.after[index].selectedDefinitionId == directAfter[index].selectedDefinitionId,
            "failure diagnostic must match direct getter evidence without prescribing rollback behavior");
    require(!evidence.after.empty(), "failure evidence must contain the declared Sound cell");
    require(evidence.after.front().selectedDefinitionId == "toneA",
        "a failed multi-callback offline call must restore its entry Sound selection");
    const auto retryPrefix = direct.render(2);
    require(retryPrefix.ok() && retryPrefix.mono.size() == 2
        && std::ranges::all_of(retryPrefix.mono, [](double sample) { return sample == 0.0; }),
        "retrying the valid prefix after failure must preserve the original queue and DSP boundary");
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        require(!direct.render(7).ok(), "the same late failure must remain reproducible after a committed prefix");
        require(direct.soundVariableValues().front().selectedDefinitionId == "toneA",
            "failure after a successful prefix must preserve that prefix's entry selection");
        require(direct.render(0).ok(), "empty calls between retries must not consume pending input");
    }
    const auto continuation = direct.render(4);
    require(continuation.ok() && continuation.mono.size() == 4,
        "a shorter retry must resume from the last committed public boundary");
    for (std::size_t index = 0; index != 4; ++index)
    {
        const auto frame = index + 2;
        const auto expected = static_cast<double>(frame - 1) / 128.0
            * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 16.0);
        require(std::abs(continuation.mono[index] - expected) <= 1.0e-12,
            "late-failure retry must preserve oscillator phase and the independent 128-sample crossfade");
    }
    require(!direct.render(1).ok() && direct.soundVariableValues().front().selectedDefinitionId == "toneB",
        "failure at the next boundary must retain the already committed Sound selection");


    std::cout << "P1 offline failure Sound publication: " << evidence.before.front().selectedDefinitionId
              << " -> " << evidence.after.front().selectedDefinitionId << '\n';
}

void testCommonEventValueInputsMatchIndependentGain()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "!Gain"}}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("osc", "audioOut", "out", "audioIn")};
    RenderSettings settings{64.0, 8};
    settings.eventValues = {{"Gain", 1.0, talas::expr::ValueDomain::Scalar}};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Event Value input fixture must compile");
    std::vector<ConformanceInputEvent> inputs{
        ConformanceEventValueAssignment{0, "Gain", 0.25},
        ConformanceEventValueAssignment{3, "Gain", 0.5},
        ConformanceEventValueAssignment{3, "Gain", 0.75},
        ConformanceEventValueAssignment{9, "Gain", 0.125},
        ConformanceEventValueAssignment{13, "Gain", 0.5},
    };
    const std::array<std::size_t, 4> blocks{3, 8, 2, 4};
    const std::array<std::size_t, 4> offlineBlocks{1, 0, 7, 9};
    std::vector<std::vector<double>> expected(1, std::vector<double>(17));
    for (std::size_t frame = 0; frame < 17; ++frame)
        expected[0][frame] = (frame < 3 ? 0.25 : frame < 9 ? 0.75 : frame < 13 ? 0.125 : 0.5)
            * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    const auto result = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected,
         .verifyResetReplay = true, .inputEvents = inputs});
    require(result.ok() && result.resetReplayVerified,
        "timed Event Values must match the independent gain oracle and reset replay: " + result.message);
    // Also retain the native offline automation driver as an independent
    // execution check; live combined inputs use the realtime-backed adapter.
    auto timelineSettings = settings;
    // Compiled automation admits one point per binding/frame. Its independently
    // authored last values must equal the live batch, which admits duplicates.
    timelineSettings.eventValueChanges = {
        {0, "Gain", 0.25}, {3, "Gain", 0.75}, {9, "Gain", 0.125}, {13, "Gain", 0.5},
    };
    const auto timeline = compileProject(document, talas::core::makeBuiltinNodeRegistry(), timelineSettings);
    require(timeline.ok(), "independent offline Event Value timeline must compile");
    const auto timelineOutput = renderOffline(*timeline.graph, 17);
    require(timelineOutput.ok() && timelineOutput.mono.size() == 17,
        "independent offline Event Value timeline must render");
    for (std::size_t frame = 0; frame < 17; ++frame)
        require(std::abs(timelineOutput.mono[frame] - expected[0][frame]) < 1.0e-12,
            "compiled automation must also match the independent gain oracle");

    const auto original = inputs;
    const auto run = [&] {
        return runRuntimeConformance(*compiled.graph, 17, blocks,
            {.referenceChannels = expected, .inputEvents = inputs});
    };
    std::swap(inputs[1], inputs[2]);
    const auto reversed = run();
    require(!reversed.ok() && reversed.mismatch && reversed.mismatch->frame == 3,
        "same-frame Event Value assignment order must affect the independent oracle");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs[1]).name = "gain";
    require(!run().ok(), "Event Value names must match exactly");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs[1]).scopeId = 999;
    require(!run().ok(), "Event Value scopes must match exactly");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs[1]).domain = talas::expr::ValueDomain::Hertz;
    require(!run().ok(), "Event Value domains must match exactly");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs[1]).value = std::numeric_limits<double>::quiet_NaN();
    require(!run().ok(), "nonfinite Event Value assignments must reject");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs.back()).frame = 17;
    require(!run().ok(), "Event Value assignments at the exclusive horizon must reject");
    inputs = original;
    std::get<ConformanceEventValueAssignment>(inputs[1]).frame = 4;
    require(!run().ok(), "Event Value frames must not be silently sorted");
    inputs = original;
    const std::array<std::size_t, 6> emptyCalls{0, 8, 0, 8, 1, 0};
    const auto empty = runRuntimeConformance(*compiled.graph, 17, emptyCalls,
        {.referenceChannels = expected, .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
         .verifyResetReplay = true, .inputEvents = inputs});
    require(empty.ok(), "empty callbacks must not consume Event Values: " + empty.message);

    OfflineMidiVoiceEvent direct;
    direct.batchId = direct.sequence = 1;
    direct.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    direct.eventValueName = "Gain";
    direct.value = 0.5;
    for (int mutation = 0; mutation < 4; ++mutation)
    {
        auto invalid = direct;
        if (mutation == 0) invalid.voiceGeneration = 1;
        if (mutation == 1) invalid.eventValueScopeId = 999;
        if (mutation == 2) invalid.domain = talas::expr::ValueDomain::Hertz;
        if (mutation == 3) invalid.value = std::numeric_limits<double>::infinity();
        OfflineRenderSession session(*compiled.graph);
        require(!session.scheduleMidiVoiceEvents(std::span{&invalid, 1}),
            "direct offline Event Value admission must validate payload and identity");
        require(session.scheduleMidiVoiceEvents(std::span{&direct, 1}) && session.render(2).ok(),
            "invalid Event Value admission must leave the offline session usable");
    }

    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"trim", "Trim", talas::expr::ValueDomain::Scalar, 1.0}};
    document.graph.nodes.front().parameters.back().expression = "!Gain * @trim";
    const auto ungatedExpected = expected;
    for (const bool voice : {false, true})
    {
        auto mixedDocument = document;
        if (voice)
            mixedDocument.graph.nodes.front().parameters.back().expression = "!KeyDown * !Gain * @trim";
        mixedDocument.graph.nodes.push_back(node("midi", voice ? "talas.midiVoice" : "talas.midiMapper", {
            {"channel", "1"}, {"polyphony", "1"}, {"left", "1"}, {"right", "1"},
            textParameter("source", ""), resourceParameter("midiFile", ""), textParameter("script", ""),
            {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, booleanParameter("shared", false),
        }));
        if (!voice)
        {
            mixedDocument.graph.nodes.back().parameters.push_back(textParameter("map", "gate"));
            mixedDocument.graph.nodes.back().parameters.push_back(
                choiceParameter("voiceModel", "legacyPassthrough"));
        }
        mixedDocument.graph.connections = {
            connection("osc", "audioOut", "midi", "audioIn"),
            connection("midi", "leftOut", "out", "audioIn"),
        };
        auto mixedSettings = settings;
        mixedSettings.eventValues.push_back({"KeyDown", 0.0, talas::expr::ValueDomain::Scalar});
        compiled = compileProject(mixedDocument, talas::core::makeBuiltinNodeRegistry(), mixedSettings);
        require(compiled.ok(), "mixed Event Value/numeric/MIDI fixture must compile");
        const auto trim = compiled.graph->preparedVariables().cells.front().identity;
        inputs = original;
        inputs.insert(inputs.begin() + 1, ConformanceVariableAssignment{0, trim, 0.5});
        inputs.insert(inputs.begin() + 2, ConformanceMidiNote{2, ConformanceMidiNoteKind::NoteOn, 1, 60, 127});
        inputs.emplace_back(ConformanceMidiNote{15, ConformanceMidiNoteKind::NoteOff, 1, 60, 0});
        expected = ungatedExpected;
        for (std::size_t frame = 0; frame < 17; ++frame)
            expected[0][frame] *= frame >= 2 && frame < 15 ? 0.5 : 0.0;
        const auto mixed = runRuntimeConformance(*compiled.graph, 17, blocks,
            {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected,
             .verifyResetReplay = true, .inputEvents = inputs});
        require(mixed.ok(), "mixed Event Value/numeric/MIDI inputs must match the independent oracle: " + mixed.message);
    }
}

void testCommonEventValueBatchesEvaluateFinalSnapshot()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "!Numerator / !Denominator"}}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("osc", "audioOut", "out", "audioIn")};
    RenderSettings settings{64.0, 8};
    settings.eventValues = {{"Numerator", 1.0}, {"Denominator", 1.0}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "atomic Event Value fixture must compile");
    std::vector<ConformanceInputEvent> inputs{
        ConformanceEventValueAssignment{0, "Numerator", 1.0},
        ConformanceEventValueAssignment{0, "Denominator", 1.0},
        ConformanceEventValueAssignment{3, "Denominator", 0.0},
        ConformanceEventValueAssignment{3, "Numerator", 2.0},
        ConformanceEventValueAssignment{3, "Denominator", 4.0},
    };
    const std::array<std::size_t, 2> blocks{5, 3};
    std::vector<std::vector<double>> expected(1, std::vector<double>(8));
    for (std::size_t frame = 0; frame < 8; ++frame)
        expected[0][frame] = (frame < 3 ? 1.0 : 0.5)
            * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    const auto good = runRuntimeConformance(*compiled.graph, 8, blocks,
        {.referenceChannels = expected, .verifyResetReplay = true, .inputEvents = inputs});
    require(good.ok(), "Event Value batches must evaluate the final valid snapshot: " + good.message);
    std::get<ConformanceEventValueAssignment>(inputs[3]).frame = 4;
    std::get<ConformanceEventValueAssignment>(inputs[4]).frame = 4;
    const auto invalid = runRuntimeConformance(*compiled.graph, 8, blocks, {.inputEvents = inputs});
    require(!invalid.ok(), "a zero denominator in a separate transaction must still reject");
}

void testCommonEventValueScopesAndProjections()
{
    using namespace talas::runtime;
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "tone";
    tone.displayName = "Tone";
    tone.graph.nodes = {node("osc", "talas.oscillator",
        {{"frequency", "!LocalPitch"}, {"amplitude", "!LocalGain"}})};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = {"frequency", "1 hz"};
    frequency.bindings = {{ProjectSoundParameterTarget::EventValue, {}, {}, "LocalPitch", talas::expr::ValueDomain::Hertz}};
    ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = {"gain", "0.25"};
    gain.bindings = {{ProjectSoundParameterTarget::EventValue, {}, {}, "LocalGain", talas::expr::ValueDomain::Scalar}};
    tone.exposedParameters = {frequency, gain};
    document.soundDefinitions = {tone};
    ProjectNode first;
    first.instanceId = "first";
    first.soundDefinitionId = "tone";
    ProjectNode second = first;
    second.instanceId = "second";
    second.parameters = {{"frequency", "2 hz"}, {"gain", "0.5"}};
    document.graph.nodes = {first, second, node("mix", "talas.mixer"), node("out", "talas.audioOutput")};
    document.graph.connections = {
        connection("first", "audioOut", "mix", "in1"),
        connection("second", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    RenderSettings settings{64.0, 8};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "scoped Event Value fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto scope = [&](std::string_view path) {
        const auto step = std::ranges::find(compiled.graph->plan().steps, path, &CompileStep::structuralSourcePath);
        require(step != compiled.graph->plan().steps.end(), "scoped fixture must preserve structural identity");
        return step->eventValueScopeId;
    };
    const auto firstScope = scope("first/tone::osc");
    const auto secondScope = scope("second/tone::osc");
    require(firstScope != secondScope && firstScope != globalRuntimeScope && secondScope != globalRuntimeScope,
        "same-named local Event Values must have distinct prepared scopes");
    std::vector<ConformanceInputEvent> inputs{
        ConformanceEventValueAssignment{0, "LocalGain", 0.25, talas::expr::ValueDomain::Scalar, firstScope},
        ConformanceEventValueAssignment{3, "LocalGain", 0.75, talas::expr::ValueDomain::Scalar, firstScope},
    };
    const std::array<std::size_t, 3> blocks{3, 8, 6};
    const std::array<std::size_t, 3> offlineBlocks{6, 0, 11};
    std::vector<std::vector<double>> expected(1, std::vector<double>(17));
    for (std::size_t frame = 0; frame < 17; ++frame)
        expected[0][frame] = (frame < 3 ? 0.25 : 0.75)
                * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0)
            + 0.5 * std::sin(4.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    auto result = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected,
         .verifyResetReplay = true, .inputEvents = inputs});
    require(result.ok(), "local Event Value updates must leave the sibling scope untouched: " + result.message);
    std::get<ConformanceEventValueAssignment>(inputs.back()).scopeId = secondScope;
    result = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.referenceChannels = expected, .inputEvents = inputs});
    require(!result.ok() && result.mismatch && result.mismatch->frame == 3,
        "a valid but wrong sibling scope must fail the independent waveform oracle");

    document.soundDefinitions.front().exposedParameters.back().defaultValue.expression = "!GlobalGain";
    settings.eventValues = {{"GlobalGain", 0.25, talas::expr::ValueDomain::Scalar}};
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "projected Event Value fixture must compile");
    inputs = {
        ConformanceEventValueAssignment{0, "GlobalGain", 0.25},
        ConformanceEventValueAssignment{3, "GlobalGain", 0.75},
    };
    result = runRuntimeConformance(*compiled.graph, 17, blocks,
        {.referenceChannels = expected, .verifyResetReplay = true, .inputEvents = inputs});
    require(result.ok(), "global source updates must refresh projections while preserving sparse overrides: " + result.message);
    inputs.back() = ConformanceEventValueAssignment{3, "LocalGain", 0.75,
        talas::expr::ValueDomain::Scalar, scope("first/tone::osc")};
    result = runRuntimeConformance(*compiled.graph, 17, blocks, {.inputEvents = inputs});
    require(!result.ok(), "derived Event Value projections must not admit direct assignment");
}

void testCommonEventValueTerminalInputsAndRetry()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"scale", "Scale", talas::expr::ValueDomain::Scalar, 1.0}};
    document.graph.nodes = {
        node("value", "talas.constant", {{"value", "!Control * @scale"}}),
        node("terminal", "talas.soundToGlobalController", {
            textParameter("generatedEvent", "  !Controlled  "), textParameter("value", "")}),
    };
    document.graph.connections = {{"value", "value", "terminal", "value"}};
    RenderSettings settings{8.0, 5};
    settings.eventValues = {{"Control", 0.0, talas::expr::ValueDomain::Scalar}};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Event Value terminal fixture must compile");
    const auto scale = compiled.graph->preparedVariables().cells.front().identity;
    const std::vector<ConformanceInputEvent> inputs{
        ConformanceEventValueAssignment{0, "Control", 0.25},
        ConformanceVariableAssignment{0, scale, 2.0},
        ConformanceEventValueAssignment{3, "Control", 0.5},
        ConformanceVariableAssignment{3, scale, 1.0},
        ConformanceEventValueAssignment{5, "Control", 0.75},
        ConformanceEventValueAssignment{8, "Control", 0.125},
    };
    const std::array<std::size_t, 2> blocks{5, 4};
    const std::array<std::size_t, 3> offlineBlocks{2, 0, 7};
    const std::vector<GlobalControllerEvent> expected{
        {0, "!Controlled", 0.5}, {5, "!Controlled", 0.75}, {8, "!Controlled", 0.125},
    };
    const std::array probes{
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .controllerEvents = 0},
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .controllerNameBytes = 0},
        ConformanceTerminalCapacityProbe{.blockIndex = 1, .controllerEvents = 1},
    };
    const auto result = runRuntimeConformance(*compiled.graph, 9, blocks,
        {.offlineBlockPartition = offlineBlocks,
         .referenceMidiEvents = std::span<const MidiEvent>{}, .referenceGlobalControllerEvents = expected,
         .verifyResetReplay = true, .terminalCapacityProbes = probes, .inputEvents = inputs});
    require(result.ok() && result.terminalFailureProbesVerified == 6,
        "Event Value terminal inputs must preserve trace and state across retry/reset: " + result.message);

    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::LegacyV1;
    document.variableLibrary.variables.clear();
    document.graph.nodes = {node("terminal", "talas.midiOutputEventInBytes", {
        textParameter("bytes", "16r90 {!Byte} 127")})};
    document.graph.connections.clear();
    settings.eventValues = {{"Byte", 64.0, talas::expr::ValueDomain::Scalar}};
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "live Event Value MIDI terminal must compile");
    const std::vector<ConformanceInputEvent> byteInputs{
        ConformanceEventValueAssignment{0, "Byte", 60.0},
        ConformanceEventValueAssignment{3, "Byte", 61.0},
        ConformanceEventValueAssignment{3, "Byte", 62.0},
        ConformanceEventValueAssignment{5, "Byte", 62.0},
        ConformanceEventValueAssignment{8, "Byte", 63.0},
    };
    const std::vector<MidiEvent> midiExpected{
        {0, {0x90, 60, 127}}, {3, {0x90, 62, 127}}, {8, {0x90, 63, 127}},
    };
    const std::array midiProbes{
        ConformanceTerminalCapacityProbe{.blockIndex = 0, .midiEvents = 1},
        ConformanceTerminalCapacityProbe{.blockIndex = 1, .midiEvents = 0},
    };
    const auto midi = runRuntimeConformance(*compiled.graph, 9, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceMidiEvents = midiExpected,
         .referenceGlobalControllerEvents = std::span<const GlobalControllerEvent>{},
         .verifyResetReplay = true, .terminalCapacityProbes = midiProbes, .inputEvents = byteInputs});
    require(midi.ok() && midi.terminalFailureProbesVerified == 4,
        "live MIDI bytes must preserve last-wins order, change suppression, retry and reset: " + midi.message);
    const std::array<std::size_t, 5> zeros{0, 5, 0, 4, 0};
    const auto zeroCalls = runRuntimeConformance(*compiled.graph, 9, zeros,
        {.realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
         .referenceMidiEvents = midiExpected, .inputEvents = byteInputs});
    require(zeroCalls.ok(), "empty terminal callbacks must preserve scheduled Event Values: " + zeroCalls.message);

    OfflineMidiVoiceEvent valid;
    valid.batchId = valid.sequence = 1;
    valid.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    valid.eventValueName = "Byte";
    valid.value = 60;
    auto invalid = valid;
    invalid.frame = 3;
    invalid.batchId = invalid.sequence = 2;
    invalid.value = 300;
    const std::array lateFailure{valid, invalid};
    OfflineRenderSession failed(*compiled.graph);
    require(failed.scheduleMidiVoiceEvents(lateFailure), "invalid evaluated MIDI bytes should reach callback validation");
    const auto failedResult = failed.render(5);
    require(!failedResult.ok() && failedResult.mono.empty() && failedResult.channels.empty()
        && failedResult.midiEvents.empty() && failedResult.globalControllerEvents.empty(),
        "a late offline terminal failure must not expose any earlier partial output");
    require(!failed.render(5).ok(), "repeated offline failure must not consume the invalid terminal transaction");
    const auto midiRetry = failed.render(3);
    require(midiRetry.ok() && midiRetry.midiEvents.size() == 1
        && midiRetry.midiEvents.front().frame == 0
        && midiRetry.midiEvents.front().bytes == std::vector<std::uint8_t>{0x90, 60, 127},
        "valid terminal retry must restore the initial emission and byte schedule exactly once");
    require(!failed.render(1).ok(), "the queued invalid byte must remain at the new committed boundary");


    // Sweep allocation failures at an already committed boundary, including
    // journal construction and event-result assembly after callbacks commit.
    auto allocationInputs = lateFailure;
    allocationInputs[1].value = 61;
    const auto prepareAllocationSession = [&] {
        OfflineRenderSession session(*compiled.graph);
        require(session.scheduleMidiVoiceEvents(allocationInputs) && session.render(1).ok(),
            "allocation fixture must commit its first MIDI emission before capture");
        return session;
    };
    auto measuredSession = prepareAllocationSession();
    OfflineRenderResult measured;
    std::size_t allocationPoints = 0;
    {
        ScopedOfflineAllocationFault counting;
        measured = measuredSession.render(4);
        allocationPoints = offlineAllocationCount;
    }
    require(measured.ok() && allocationPoints != 0, "the offline transaction must expose allocation points to sweep");
    for (std::size_t point = 0; point < allocationPoints; ++point)
    {
        auto allocationSession = prepareAllocationSession();
        OfflineRenderResult faulted;
        {
            ScopedOfflineAllocationFault failure(point);
            faulted = allocationSession.render(4);
        }
        require(offlineAllocationFaultTriggered && !faulted.ok() && faulted.mono.empty()
            && faulted.channels.empty() && faulted.midiEvents.empty() && faulted.globalControllerEvents.empty(),
            "every allocation fault must return an empty failed offline transaction");
        const auto retry = allocationSession.render(4);
        require(retry.ok() && retry.midiEvents.size() == 1 && retry.midiEvents.front().frame == 2
            && retry.midiEvents.front().bytes == std::vector<std::uint8_t>{0x90, 61, 127},
            "allocation failure retry must preserve the committed prefix, future input and MIDI suppression");
    }
    std::cout << "P2 offline allocation failure/retry points: " << allocationPoints << '\n';

    document.graph.nodes = {node("terminal", "talas.midiFileEcho", {
        {"lowChannel", "1"}, {"highChannel", "1"}, resourceParameter("fileName", "Sequences/live.mid")})};
    settings.sampleRate = 10.0;
    settings.binaryResources = {{"Sequences/live.mid", midiFileFormatZeroFeatureFixture()}};
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "combined MIDI-file terminal must compile");
    const std::array<ConformanceInputEvent, 1> unusedControl{ConformanceEventValueAssignment{2, "Byte", 65.0}};
    const std::vector<MidiEvent> fileExpected{
        {0, {0x90, 60, 64}}, {0, {0x90, 61, 65}}, {0, {0xc0, 5}}, {0, {0xc0, 6}}, {5, {0x80, 60, 0}},
    };
    const std::array<std::size_t, 3> fileBlocks{3, 5, 4};
    const std::array<std::size_t, 3> fileOffline{3, 0, 9};
    const auto file = runRuntimeConformance(*compiled.graph, 12, fileBlocks,
        {.offlineBlockPartition = fileOffline, .referenceMidiEvents = fileExpected,
         .referenceGlobalControllerEvents = std::span<const GlobalControllerEvent>{},
         .verifyResetReplay = true, .inputEvents = unusedControl});
    require(file.ok(), "an unrelated live input must not disturb MIDI-file cursor or equal-time order: " + file.message);
}

void testCommonSoundSelectionMatchesIndependentCrossfade()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    for (const auto& [id, amplitude] : std::array{
             std::pair{"toneA", "0"}, std::pair{"toneB", "1"}})
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = id;
        definition.displayName = id;
        definition.graph.nodes = {node("osc", "talas.oscillator",
            {{"frequency", "1 hz"}, {"amplitude", amplitude}})};
        definition.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
        document.soundDefinitions.push_back(std::move(definition));
    }
    talas::project::ProjectSoundVariableDeclaration selected;
    selected.variableId = "selected";
    selected.displayName = "Selected Sound";
    selected.candidateDefinitionIds = {"toneA", "toneB"};
    document.soundVariableLibrary.variables = {selected};
    document.graph.nodes = {
        node("selector", "talas.variable", {textParameter("variableId", "selected")}),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {connection("selector", "audioOut", "out", "audioIn")};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), {256.0, 32});
    require(compiled.ok(), "Sound selector conformance fixture must compile");
    const auto identity = compiled.graph->preparedSoundVariables().cells.front().identity;
    std::vector<ConformanceInputEvent> inputs{
        ConformanceSoundVariableAssignment{0, identity, "toneA"},
        ConformanceSoundVariableAssignment{5, identity, "toneA"},
        ConformanceSoundVariableAssignment{5, identity, "toneB"},
        ConformanceSoundVariableAssignment{145, identity, "toneA"},
    };
    const std::array<std::size_t, 12> blocks{2, 3, 32, 31, 32, 32, 13, 32, 32, 32, 32, 27};
    const std::array<std::size_t, 5> offlineBlocks{1, 0, 66, 80, 153};
    std::vector<std::vector<double>> expected(1, std::vector<double>(300));
    // Native contract: every candidate runs continuously; a change blends
    // over 128 samples, with 1/128 of the new branch in the first sample.
    // This is an analytical native oracle, not an observed reference response.
    for (std::size_t frame = 5; frame < 273; ++frame)
    {
        const double blend = frame < 145
            ? std::min(1.0, static_cast<double>(frame - 4) / 128.0)
            : std::max(0.0, 1.0 - static_cast<double>(frame - 144) / 128.0);
        expected[0][frame] = blend * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 256.0);
    }
    const auto result = runRuntimeConformance(*compiled.graph, 300, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected,
         .verifyResetReplay = true, .inputEvents = inputs});
    require(result.ok() && result.resetReplayVerified,
        "Sound selection and reset must match an independent crossfade oracle: " + result.message);
    const auto original = inputs;
    const auto run = [&] {
        return runRuntimeConformance(*compiled.graph, 300, blocks,
            {.referenceChannels = expected, .inputEvents = inputs});
    };
    std::swap(inputs[1], inputs[2]);
    const auto reversed = run();
    require(!reversed.ok() && reversed.mismatch && reversed.mismatch->frame == 5,
        "reversed same-frame selections must fail the independent oracle");
    inputs = original;
    std::get<ConformanceSoundVariableAssignment>(inputs[2]).selectedDefinitionId = "unknown";
    require(!run().ok(), "an unprepared Sound candidate must be rejected");
    inputs = original;
    std::get<ConformanceSoundVariableAssignment>(inputs[2]).target.instancePath = {"wrong-instance"};
    require(!run().ok(), "a forged Sound instance identity must be rejected");
    inputs = original;
    std::get<ConformanceSoundVariableAssignment>(inputs[2]).voiceGeneration = 1;
    require(!run().ok(), "a project Sound cell must reject a voice generation");
    inputs = original;

    // Mixed Sound/numeric transactions also work without any MIDI owner.
    // When an owner is present, the same stream may additionally contain notes.
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {{"gain", "Gain", talas::expr::ValueDomain::Scalar, 1.0}};
    document.soundDefinitions[1].graph.nodes.front().parameters.back().expression = "!EventGain * @gain";
    for (const bool withMidi : {false, true})
    {
        if (withMidi)
        {
            document.graph.nodes.push_back(node("mapper", "talas.midiMapper", {
                textParameter("map", "gate"), {"channel", "1"}, {"polyphony", "1"},
                choiceParameter("voiceModel", "legacyPassthrough"),
                {"left", "1"}, {"right", "1"}, textParameter("source", ""),
                resourceParameter("midiFile", ""), textParameter("script", ""),
                {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, booleanParameter("shared", false),
            }));
            document.graph.connections = {
                connection("selector", "audioOut", "mapper", "audioIn"),
                connection("mapper", "leftOut", "out", "audioIn"),
            };
        }
        RenderSettings combinedSettings{256.0, 32};
        combinedSettings.eventValues = {{"EventGain", 1.0, talas::expr::ValueDomain::Scalar}};
        compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), combinedSettings);
        require(compiled.ok(), "combined Sound/numeric/MIDI fixture must compile");
        const auto gainIdentity = compiled.graph->preparedVariables().cells.front().identity;
        inputs = original;
        inputs.insert(inputs.begin() + 3, ConformanceVariableAssignment{
            5, gainIdentity, 0.5, talas::expr::ValueDomain::Scalar});
        if (withMidi)
            inputs.insert(inputs.begin() + 1, ConformanceMidiNote{0, ConformanceMidiNoteKind::NoteOn, 1, 60, 127});
        auto mixedExpected = expected;
        for (auto& sample : mixedExpected[0]) sample *= 0.5;
        const auto mixed = runRuntimeConformance(*compiled.graph, 300, blocks,
            {.offlineBlockPartition = offlineBlocks, .referenceChannels = mixedExpected,
             .verifyResetReplay = true, .inputEvents = inputs});
        require(mixed.ok() && mixed.resetReplayVerified,
            "combined Sound controls and optional MIDI must match the analytical oracle: " + mixed.message);
        auto allKinds = inputs;
        for (auto& input : allKinds)
            if (auto* numeric = std::get_if<ConformanceVariableAssignment>(&input)) numeric->value = 0.25;
        allKinds.insert(allKinds.end() - 1, ConformanceEventValueAssignment{5, "EventGain", 2.0});
        const auto composed = runRuntimeConformance(*compiled.graph, 300, blocks,
            {.referenceChannels = mixedExpected, .verifyResetReplay = true, .inputEvents = allKinds});
        require(composed.ok(), "Sound, numeric, Event Value and optional note inputs must compose: " + composed.message);

        OfflineMidiVoiceEvent selection;
        selection.frame = 5;
        selection.batchId = selection.sequence = 1;
        selection.kind = OfflineMidiVoiceEventKind::SoundVariableAssignment;
        selection.soundTarget = identity;
        selection.selectedDefinitionId = "toneB";
        OfflineRenderSession direct(*compiled.graph);
        require(direct.scheduleMidiVoiceEvents(std::span{&selection, 1}) && direct.render(8).ok(),
            "combined offline adapter must admit a project Sound selection");
        require(direct.soundVariableValues().front().selectedDefinitionId == "toneB",
            "combined offline Sound publication must report the rendered selection");
        direct.reset();
        require(direct.soundVariableValues().front().selectedDefinitionId == "toneB",
            "offline reset must retain the committed non-voice Sound selection");
        OfflineMidiVoiceEvent postReset;
        postReset.batchId = postReset.sequence = 1;
        postReset.kind = OfflineMidiVoiceEventKind::VariableAssignment;
        postReset.target = gainIdentity;
        postReset.value = 0.5;
        if (withMidi)
        {
            postReset.kind = OfflineMidiVoiceEventKind::NoteOn;
            postReset.channel = 1;
            postReset.note = 60;
            postReset.velocity = 127;
        }
        require(direct.scheduleMidiVoiceEvents(std::span{&postReset, 1}),
            "post-reset combined input should preserve a retained Sound selection");
        const auto retained = direct.render(2);
        require(retained.ok() && retained.mono.size() == 2
            && std::abs(retained.mono[1] - (withMidi ? 1.0 : 0.5)
                * std::sin(2.0 * std::numbers::pi / 256.0)) < 1.0e-12,
            "new combined backend must start from the retained Sound with reset oscillator phase");
        OfflineRenderSession invalidGeneration(*compiled.graph);
        selection.voiceGeneration = 1;
        require(!invalidGeneration.scheduleMidiVoiceEvents(std::span{&selection, 1}),
            "direct offline project Sound admission must reject a voice generation");
        selection.voiceGeneration = 0;
        require(invalidGeneration.scheduleMidiVoiceEvents(std::span{&selection, 1})
                && invalidGeneration.render(8).ok(),
            "rejected Sound admission must leave the offline session usable");
        selection.kind = static_cast<OfflineMidiVoiceEventKind>(255);
        OfflineRenderSession forged(*compiled.graph);
        require(!forged.scheduleMidiVoiceEvents(std::span{&selection, 1}),
            "an unknown offline event kind must not be interpreted as Sound selection");
    }

    // The same prepared selector inside a voice requires the exact generation
    // created by the preceding NoteOn, even when both inputs share frame zero.
    document.graph.nodes.back().typeId = "talas.midiVoice";
    std::erase_if(document.graph.nodes.back().parameters,
        [](const auto& parameter) {
            return parameter.parameterId == "map"
                || parameter.parameterId == "voiceModel";
        });
    document.soundVariableLibrary.variables.front().owner = {
        talas::project::ProjectVariableOwnerKind::MidiVoice, {}, "mapper"};
    document.soundVariableLibrary.variables.front().snapshotEligible = false;
    document.soundDefinitions[1].graph.nodes.front().parameters.back().expression = "!KeyDown";
    RenderSettings voiceSettings{256.0, 32};
    voiceSettings.eventValues = {{"KeyDown", 0.0, talas::expr::ValueDomain::Scalar}};
    compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), voiceSettings);
    require(compiled.ok(), "voice Sound selector fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto voiceIdentity = compiled.graph->preparedSoundVariables().cells.front().identity;
    inputs = original;
    for (auto& input : inputs)
    {
        auto& assignment = std::get<ConformanceSoundVariableAssignment>(input);
        assignment.target = voiceIdentity;
        assignment.voiceGeneration = 1;
    }
    inputs.insert(inputs.begin(), ConformanceMidiNote{0, ConformanceMidiNoteKind::NoteOn, 1, 60, 127});
    const auto voiceResult = runRuntimeConformance(*compiled.graph, 300, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected, .inputEvents = inputs});
    require(voiceResult.ok(), "voice-owned Sound selection must match the analytical oracle: " + voiceResult.message);
    const auto voiceOriginal = inputs;
    std::get<ConformanceSoundVariableAssignment>(inputs[1]).voiceGeneration = 2;
    require(!run().ok(), "Sound selection must reject an unborn voice generation");
    inputs = voiceOriginal;
    std::get<ConformanceSoundVariableAssignment>(inputs[1]).voiceGeneration = 0;
    require(!run().ok(), "voice Sound selection requires a generation");
    inputs = voiceOriginal;
    std::swap(inputs[0], inputs[1]);
    require(run().ok(),
        "native atomic batches validate Sound assignments against the final voice generation");
    inputs = voiceOriginal;
    const auto staleReplay = runRuntimeConformance(*compiled.graph, 300, blocks,
        {.referenceChannels = expected, .verifyResetReplay = true, .inputEvents = inputs});
    require(!staleReplay.ok() && staleReplay.message.starts_with("reset replay:"),
        "Sound reset replay must expose retained voice generations");

    OfflineMidiVoiceEvent invalidKind;
    invalidKind.batchId = invalidKind.sequence = 1;
    invalidKind.kind = static_cast<OfflineMidiVoiceEventKind>(255);
    invalidKind.soundTarget = voiceIdentity;
    invalidKind.selectedDefinitionId = "toneB";
    invalidKind.voiceGeneration = 1;
    OfflineRenderSession voiceAdapter(*compiled.graph);
    require(!voiceAdapter.scheduleMidiVoiceEvents(std::span{&invalidKind, 1}),
        "unknown offline event kinds must reject even with a valid voice Sound payload");
}

void testCommonOrderedInputsPreserveVoiceGenerationAndInterleaving()
{
    using namespace talas::runtime;
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    talas::project::ProjectVariableDeclaration gain;
    gain.variableId = "gain";
    gain.displayName = "Voice gain";
    gain.initialValue = 0.125;
    gain.snapshotEligible = false;
    gain.owner = {talas::project::ProjectVariableOwnerKind::MidiVoice, {}, "voice"};
    document.variableLibrary.variables = {gain};
    document.graph.nodes = {
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "!KeyDown * @gain"}}),
        node("voice", "talas.midiVoice", {
            {"channel", "1"}, {"polyphony", "1"}, {"left", "1"}, {"right", "1"},
            textParameter("source", ""), resourceParameter("midiFile", ""),
            textParameter("script", ""), {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
            booleanParameter("shared", false),
        }),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {
        connection("osc", "audioOut", "voice", "audioIn"),
        connection("voice", "leftOut", "out", "audioIn"),
    };
    RenderSettings settings{64.0, 8};
    settings.eventValues = {{"KeyDown", 0.0, talas::expr::ValueDomain::Scalar}};
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "ordered voice input fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto identity = compiled.graph->preparedVariables().cells.front().identity;
    std::vector<ConformanceInputEvent> inputs{
        ConformanceMidiNote{0, ConformanceMidiNoteKind::NoteOn, 1, 60, 127},
        ConformanceVariableAssignment{0, identity, 0.5, talas::expr::ValueDomain::Scalar, 1},
        ConformanceVariableAssignment{3, identity, 0.75, talas::expr::ValueDomain::Scalar, 1},
        ConformanceMidiNote{6, ConformanceMidiNoteKind::NoteOff, 1, 60, 0},
        ConformanceMidiNote{6, ConformanceMidiNoteKind::NoteOn, 1, 60, 127},
        ConformanceVariableAssignment{6, identity, 0.25, talas::expr::ValueDomain::Scalar, 2},
        ConformanceMidiNote{9, ConformanceMidiNoteKind::NoteOff, 1, 60, 0},
    };
    const std::array<std::size_t, 3> blocks{3, 5, 3};
    const std::array<std::size_t, 4> offlineBlocks{2, 0, 5, 4};
    std::vector<std::vector<double>> expected(1, std::vector<double>(11));
    for (std::size_t frame = 0; frame < 9; ++frame)
        expected[0][frame] = (frame < 3 ? 0.5 : frame < 6 ? 0.75 : 0.25)
            * std::sin(2.0 * std::numbers::pi * static_cast<double>(frame) / 64.0);
    const auto result = runRuntimeConformance(*compiled.graph, 11, blocks,
        {.offlineBlockPartition = offlineBlocks, .referenceChannels = expected, .inputEvents = inputs});
    require(result.ok(), "interleaved notes and generation-aware assignments must match the voice oracle: " + result.message);
    // A late stale generation fails after earlier voice lifetimes and gain
    // transactions have executed. Rollback must restore both producer projection
    // and consumer allocator state, not just the published waveform controls.
    std::vector<OfflineMidiVoiceEvent> directInputs;
    for (const auto& input : inputs)
    {
        OfflineMidiVoiceEvent event;
        if (const auto* note = std::get_if<ConformanceMidiNote>(&input))
        {
            event.frame = note->frame;
            event.kind = note->kind == ConformanceMidiNoteKind::NoteOn
                ? OfflineMidiVoiceEventKind::NoteOn : OfflineMidiVoiceEventKind::NoteOff;
            event.channel = note->channel;
            event.note = note->note;
            event.velocity = note->velocity;
        }
        else
        {
            const auto& assignment = std::get<ConformanceVariableAssignment>(input);
            event.frame = assignment.frame;
            event.kind = OfflineMidiVoiceEventKind::VariableAssignment;
            event.target = assignment.target;
            event.value = assignment.value;
            event.domain = assignment.domain;
            event.voiceGeneration = assignment.voiceGeneration;
        }
        event.batchId = event.frame + 1;
        event.sequence = directInputs.size() + 1;
        directInputs.push_back(event);
    }
    auto stale = directInputs[1];
    stale.frame = 10;
    stale.batchId = 11;
    stale.sequence = directInputs.size() + 1;
    stale.voiceGeneration = 99;
    directInputs.push_back(stale);
    OfflineRenderSession retryVoice(*compiled.graph);
    require(retryVoice.scheduleMidiVoiceEvents(directInputs), "stale voice input must reach runtime lifetime validation");
    require(!retryVoice.render(11).ok(), "a late stale generation must fail the public voice transaction");
    const auto voicePrefix = retryVoice.render(2);
    require(voicePrefix.ok() && voicePrefix.mono.size() == 2,
        "voice creation and generation-one assignment must be retryable after rollback");
    require(!retryVoice.render(9).ok(), "late voice failure after a committed prefix must preserve the queue");
    const auto voiceContinuation = retryVoice.render(8);
    require(voiceContinuation.ok() && voiceContinuation.mono.size() == 8,
        "restored voice projection must admit the original generation-one and generation-two assignments");
    for (std::size_t frame = 0; frame != 10; ++frame)
    {
        const auto actual = frame < 2 ? voicePrefix.mono[frame] : voiceContinuation.mono[frame - 2];
        require(std::abs(actual - expected[0][frame]) <= 1.0e-12,
            "voice rollback must preserve the independent gate/gain/phase trace across lifetime replacement");
    }
    require(!retryVoice.render(1).ok(), "the stale generation must remain queued at its exact boundary");

    const auto original = inputs;
    const auto run = [&] {
        return runRuntimeConformance(*compiled.graph, 11, blocks,
            {.referenceChannels = expected, .inputEvents = inputs});
    };
    const std::array<std::size_t, 6> irregular{0, 2, 0, 5, 4, 0};
    const auto interior = runRuntimeConformance(*compiled.graph, 11, irregular,
        {.referenceChannels = expected, .realtimeZeroFrames = ConformanceZeroFramePolicy::RequireRejection,
         .inputEvents = inputs});
    require(interior.ok(), "ordered events must retain their time inside/around empty callbacks: " + interior.message);

    std::swap(inputs[3], inputs[4]);
    const auto reversed = run();
    require(!reversed.ok() && reversed.mismatch && reversed.mismatch->comparison == "reference/offline"
        && reversed.mismatch->frame == 6,
        "reversing equal-frame on/off order must fail the independent oracle: " + reversed.message);
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[5]).voiceGeneration = 1;
    require(!run().ok(), "a stale assignment after voice replacement must reject the entire transaction");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[1]).voiceGeneration = 2;
    require(!run().ok(), "an assignment to an unborn generation must fail at execution");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[1]).voiceGeneration = 0;
    require(!run().ok(), "voice-owned assignments require an explicit generation");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[1]).target.ownerNodeId = "missing-owner";
    require(!run().ok(), "ordered inputs must preserve exact voice owner identity");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[1]).domain = talas::expr::ValueDomain::Hertz;
    require(!run().ok(), "ordered inputs must preserve exact variable domain");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[1]).value = std::numeric_limits<double>::infinity();
    require(!run().ok(), "ordered inputs must reject nonfinite assignment values");
    inputs = original;
    std::get<ConformanceVariableAssignment>(inputs[2]).frame = 7;
    require(!run().ok(), "the ordered stream must not silently sort backwards input frames");
    inputs = original;
    std::get<ConformanceMidiNote>(inputs.back()).frame = 11;
    require(!run().ok(), "events at the exclusive horizon must not be silently omitted");
    inputs = original;
    const std::array redundant{std::get<ConformanceMidiNote>(inputs.front())};
    const auto ambiguous = runRuntimeConformance(*compiled.graph, 11, blocks,
        {.midiNotes = redundant, .inputEvents = inputs});
    require(!ambiguous.ok() && ambiguous.message.find("cannot be combined") != std::string::npos,
        "ordered and convenience schedules must not have ambiguous precedence");

    // Transport reset preserves slot generations. Reusing stale generation
    // numbers must fail, rather than making the replay appear equivalent.
    const auto staleReplay = runRuntimeConformance(*compiled.graph, 11, blocks,
        {.referenceChannels = expected, .verifyResetReplay = true, .inputEvents = inputs});
    require(!staleReplay.ok() && staleReplay.message.starts_with("reset replay:"),
        "reset replay must expose retained voice generations: " + staleReplay.message);
}
}

int main()
{
    testOfflineAnalysisPublicationWholeCallRollback();
    testOfflineDeferredAnalysisMatchesDirectPublication();
    testOfflineAnalysisPublicationAllocationRollback();
    testOfflinePartialTrackWholeCallRollback();
    testOfflinePartialTrackAllocationRollback();
    testOfflineLiveAnalysisWholeCallRollback();
    testOfflinePartialSequenceStorageIdentity();
    testOfflineLiveAnalysisAllocationRollback();
    testOfflinePitchFamiliesWholeCallRollback();
    testOfflinePitchAllocationRollback();
    testOfflineComplexStftWholeCallRollback();
    testOfflineComplexStftAllocationRollback();
    testCommonSoundStateCheckpointsAndOfflineFailureEvidence();
    testCommonEventValueInputsMatchIndependentGain();
    testCommonEventValueBatchesEvaluateFinalSnapshot();
    testCommonEventValueScopesAndProjections();
    testCommonEventValueTerminalInputsAndRetry();
    testCommonSoundSelectionMatchesIndependentCrossfade();
    testCommonOrderedInputsPreserveVoiceGenerationAndInterleaving();
    testCommonTerminalRetryPreservesQueuedControlsAndState();
    testCommonMidiCapacityRetryPreservesOrderAndChangeState();
    testCommonVariableScheduleMatchesIndependentOracle();
    testCommonVariableTransactionsAreAtomic();
    testCommonHarnessAdmitsEventOnlyTerminals();
    testCommonTerminalOraclesDistinguishPayloadOrderAndSilence();
    testCommonControllerOracleIncludesNamesValuesAndContinuation();
    testCommonTerminalCapacityFailureIsNotAnEmptyTracePass();
    testCommonResetReplayChecksActualState();
    testSharedHarnessRetainsFourAndEightOrderedChannels();
    testIndependentOracleRejectsSharedErrorsAndSupportsEmptyTransactions();
    testSharedMidiScheduleMatchesIndependentGateOracle();
    testOverflowingDifferenceDoesNotHideAnOracleMismatch();
    testOscillatorLegacyBaselineMatchesAcrossContexts();
    testC66ThresholdMatchesAcrossIrregularBoundaries();
    testC67TimeControlMatchesAcrossIrregularBoundaries();
    testNativeComplexStftMatchesAcrossPreparedContexts();
    testConformanceRejectsAnIncompleteRealtimePartition();
    testFilterLegacyBaselineMatchesAcrossContexts();
    testAdsrLegacyBaselineMatchesAcrossContexts();
    testDirectTypedAdsrGateConformance();
    testConnectedKernelAdaptersMatchAcrossIrregularBoundaries();
    testM1UtilityDemonstratorMatchesAcrossIrregularBoundaries();
    testCompileDiagnosticsUseDescriptorOfflineCapability();
    testStaticFirFamilyMatchesAcrossIrregularBoundaries();
    testVcfMatchesAcrossIrregularBoundaries();
    testHarmonicResonatorMatchesAcrossIrregularBoundaries();
    testQuadOscillatorMatchesAcrossIrregularBoundaries();
    testOverlappingMixerMatchesAcrossIrregularBoundaries();
    testOscillatorBankMatchesAcrossIrregularBoundaries();
    testC61SumOfSinesDefaultsMatchAcrossIrregularBoundaries();
    testC63SumOfSinesAutomaticLoopMatchesAcrossIrregularBoundaries();
    testFormantBankMatchesAcrossIrregularBoundaries();
    testFormantBankOscillatorMatchesAcrossIrregularBoundaries();
    testSpectrumModifierMatchesAcrossIrregularBoundaries();
    testSpectrumModifierRemainderProfileMatchesAcrossIrregularBoundaries();
    testWarpedTimeIndexMatchesAcrossIrregularBoundaries();
    testSpectrumOnDiskMatchesAcrossIrregularBoundaries();
    testC58SpectrumInRamMatchesAcrossIrregularBoundaries();
    testC59SpectrumInRamDefaultsMatchAcrossIrregularBoundaries();
    testC60SpectrumOnDiskDefaultsMatchOfflinePreparedTransactionSchedule();
    testC57VoicedUnvoicedMatchesOfflinePreparedTransactionSchedule();
    testGaOscillatorsMatchesAcrossIrregularBoundaries();
    testGenericSourceRamSampleMatchesAcrossIrregularBoundaries();
    testSampleMatchesAcrossIrregularBoundaries();
    testMultisampleMatchesAcrossIrregularBoundaries();
    testKeyMappedMultisampleMatchesAcrossIrregularBoundaries();
    testMultifileDiskPlayerMatchesAcrossIrregularBoundaries();
    testMemoryWriterMatchesAcrossIrregularBoundaries();
    testRawMidiTerminalMatchesAcrossAllPartitionShapes();
    testMidiControllerTerminalMatchesAcrossAllPartitionShapes();
    testMidiNoteTerminalMatchesAcrossAllPartitionShapes();
    testMidiFileTerminalMatchesAcrossAllPartitionShapes();
    testGlobalControllerTerminalMatchesAcrossAllPartitionShapes();
    testMidiFileTerminalMatchesCompleteSharedParserMatrix();
    testOfflineDriverDoesNotInvokeKernelForEmptyRange();
    return 0;
}
