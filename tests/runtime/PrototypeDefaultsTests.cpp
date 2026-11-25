#include "../support/PrototypeHarness.h"
#include "talas/dsp/SystemWavetables.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The prototypes are working examples: dragged into a Sound, each one
// plays. Here every catalog node is compiled with only its catalog
// defaults, as the Workbench adds it, with its required inputs fed by an
// oscillator, a constant or a standard spectrum, its sample and file fields
// given a synthesized WAV and any other empty resource field the standard Sine
// wavetable, and it must render offline and, where the node supports it, in
// realtime. The harness's exceptions need material a user has to supply
// or fail on ordinary audio; an exception that starts playing must be
// removed from its list.
namespace
{
using namespace talas::test::prototypes;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Playback
{
    // Empty when the node plays, otherwise why it does not.
    std::string offline;
    std::string realtime;
};

// A realtime session renders the graph in callback-sized blocks.
std::string playInRealtime(const talas::runtime::PreparedRuntimeGraph& graph)
{
    talas::runtime::RealtimeRenderSession session(graph);
    if (!session.ready())
        return "it does not prepare for realtime: " + std::string{session.error()};
    std::vector<double> left(256);
    std::vector<double> right(256);
    const std::vector<float> ones(256, 1.0F);
    const float* const channels[] = {ones.data(), ones.data()};
    for (int block = 0; block < 4; ++block)
    {
        if (!session.render({channels, 2}, left, right))
            return "it does not render in realtime";
        for (const auto sample : left)
        {
            if (!std::isfinite(sample))
                return "it renders non-finite samples in realtime";
        }
    }
    return {};
}

Playback play(const talas::core::NodeTypeDescriptor& type, bool withAudioOutput)
{
    const auto [document, error] = patch(type, {}, withAudioOutput);
    if (!error.empty())
        return {error, {}};
    auto settings = talas::test::prototypes::settings(1024);
    // The Event Values the prototype reads get their controls, as the
    // Virtual Control Surface gives them (the MIDIOutputController's !Amp).
    for (const auto& control : talas::project::discoverProjectEventValueControls(document, registry()).missingControls)
        settings.eventValues.push_back({control.eventName, control.value, control.domain});
    const auto compiled = talas::runtime::compileProject(document, registry(), settings);
    if (!compiled.ok())
    {
        const auto& diagnostic = compiled.diagnostics.front();
        return {"it does not compile: "
                + (diagnostic.parameterId.empty() ? std::string{}
                                                  : diagnostic.parameterId + ": ")
                + diagnostic.message,
            {}};
    }
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 1024);
    if (!rendered.ok())
        return {"it does not render: " + rendered.diagnostics.front().message, {}};
    for (const auto sample : rendered.mono)
    {
        if (!std::isfinite(sample))
            return {"it renders non-finite samples", {}};
    }
    // A graph without the Audio Output ends in the node's own terminal,
    // which realtime callbacks drive through other entry points.
    if (!withAudioOutput
        || type.capabilities.preparedRealtime
            == talas::core::ExecutionSupport::Unsupported)
    {
        return {};
    }
    return {{}, playInRealtime(*compiled.graph)};
}

Playback play(const talas::core::NodeTypeDescriptor& type)
{
    auto playback = play(type, true);
    if (playback.offline.find("multiple audio output nodes") != std::string::npos)
        playback = play(type, false);
    return playback;
}

void testEveryPrototypePlaysWithItsDefaults()
{
    std::size_t playing = 0;
    for (const auto& type : registry().nodeTypes())
    {
        if (type.typeId == "talas.audioOutput")
            continue;
        const auto playback = play(type);
        if (!playable(type))
        {
            require(!playback.offline.empty(),
                type.typeId + " now plays with its defaults; remove it from the exceptions");
            continue;
        }
        require(playback.offline.empty(),
            type.typeId + " should play with its catalog defaults, but "
                + playback.offline);
        if (offlineOnlyDefaults.contains(type.typeId))
        {
            require(!playback.realtime.empty(),
                type.typeId + " now plays in realtime; remove it from the offline-only list");
            continue;
        }
        require(playback.realtime.empty(),
            type.typeId + " should play its catalog defaults in realtime, but "
                + playback.realtime);
        ++playing;
    }
    std::cout << playing << " prototypes play with their defaults\n";
}

// The prototypes come with tables: a table field
// defaults to the system wavetable of its kind, Sine for a waveform, Hann
// for an envelope or a window, LinearEnvelope for a FormantBank's impulse
// and FullRamp, the identity, for a shaping
// function. Projects saved before format 86 keep the empty tables they had.
void testTableFieldsDefaultToTalasWavetables()
{
    struct Default
    {
        std::string_view typeId;
        std::string_view field;
        std::string_view table;
    };
    constexpr Default defaults[]{
        {"talas.oscillatorBank", "wavetable", "Sine"},
        {"talas.sosOscillators", "wavetable", "Sine"},
        {"talas.grainCloud", "waveform", "Sine"},
        {"talas.grainCloud", "grainEnv", "Hann"},
        {"talas.cloudBank", "grainWave", "Sine"},
        {"talas.cloudBank", "grainEnvelope", "Hann"},
        {"talas.cloudBankElement", "grainWave", "Sine"},
        {"talas.cloudBankElement", "grainEnvelope", "Hann"},
        {"talas.sampleCloud", "grainEnv", "Hann"},
        {"talas.chopper", "wavetable", "Hann"},
        {"talas.functionGenerator", "wavetable", "Hann"},
        {"talas.spectralShape", "wavetable", "Hann"},
        {"talas.triggeredTableRead", "wavetable", "Sine"},
        {"talas.formantBank", "impulseResponse", "LinearEnvelope"},
        {"talas.formantBankOscillator", "formantImpulse", "LinearEnvelope"},
        {"talas.waveshaper", "shapingFunction", "FullRamp"},
        {"talas.iteratedWaveshaper", "wavetable", "FullRamp"},
        {"talas.multiplyingWaveshaper", "wavetable", "FullRamp"},
    };
    for (const auto& expected : defaults)
    {
        const auto* type = registry().findNodeType(expected.typeId);
        require(type != nullptr, std::string{expected.typeId} + " is not a catalog node");
        const auto field = std::ranges::find(type->parameters, expected.field, &talas::core::ParameterDescriptor::id);
        require(field != type->parameters.end() && field->defaultResource == expected.table,
            std::string{expected.typeId} + "." + std::string{expected.field} + " should default to "
                + std::string{expected.table});
        require(talas::dsp::systemWavetable(expected.table).has_value(),
            std::string{expected.table} + " should be a system wavetable");
    }
    // Fields that take an analysis, an RE table, a terrain or a list of
    // cycles default to computed material of that shape (owner,
    // 2026-10-12); they failed to compile empty, so no project changes.
    constexpr Default computed[]{
        {"talas.spectrumInRam", "analysis", "HarmonicSpectrum"},
        {"talas.spectrumOnDisk", "fileName", "HarmonicSpectrum"},
        {"talas.reResonator", "wavetable", "Resonance"},
        {"talas.waveTerrainOscillator", "terrain", "Terrain"},
        // The analysis a GAOscillators or
        // SumOfSines resynthesizes, the amplitudes of its harmonics.
        {"talas.gaOscillators", "analysis0", "HarmonicAmplitudes"},
        {"talas.sumOfSines", "analysis0", "HarmonicAmplitudes"},
    };
    for (const auto& expected : computed)
    {
        const auto* type = registry().findNodeType(expected.typeId);
        require(type != nullptr, std::string{expected.typeId} + " is not a catalog node");
        const auto field = std::ranges::find(type->parameters, expected.field, &talas::core::ParameterDescriptor::id);
        require(field != type->parameters.end() && field->defaultResource == expected.table,
            std::string{expected.typeId} + "." + std::string{expected.field} + " should default to "
                + std::string{expected.table});
        require(talas::dsp::systemTable(expected.table).has_value(),
            std::string{expected.table} + " should be a system table");
    }
    for (const auto typeId : {"talas.splitSurroundOscillator", "talas.splitSurroundAntialiasedOscillator"})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr && type->parameters.front().id == "fileNames"
                && type->parameters.front().defaultText == "Sine Sine",
            std::string{typeId} + " should default to a Sine cycle for each of its two channels");
    }

    // A GAOscillators added as the Workbench adds it resynthesizes eight
    // harmonics of its 440 hz, the first loudest; a SumOfSines plays too.
    for (const auto* typeId : {"talas.gaOscillators", "talas.sumOfSines"})
    {
        talas::project::ProjectDocument resynthesis;
        resynthesis.graph.nodes.push_back({"n", typeId, {}});
        resynthesis.graph.nodes.push_back({"out", "talas.audioOutput", {}});
        resynthesis.graph.connections.push_back({"n", "audioOut", "out", "audioIn"});
        const auto played = talas::runtime::compileProject(resynthesis, registry(), settings(4800));
        require(played.ok(), std::string{typeId} + " with its defaults should compile: "
                + (played.diagnostics.empty() ? std::string{} : played.diagnostics.front().message));
        const auto heard = talas::runtime::renderOffline(*played.graph, 4800);
        require(heard.ok() && std::ranges::any_of(heard.mono, [](double sample) { return sample != 0.0; }),
            std::string{typeId} + " with its defaults should play its analysis");
        if (std::string_view{typeId} != "talas.gaOscillators")
            continue;
        const auto level = [&](double frequency) {
            double real = 0.0;
            double imaginary = 0.0;
            for (std::size_t frame = 0; frame < heard.mono.size(); ++frame)
            {
                const auto phase = 2.0 * 3.141592653589793 * frequency * static_cast<double>(frame) / 48000.0;
                real += heard.mono[frame] * std::cos(phase);
                imaginary += heard.mono[frame] * std::sin(phase);
            }
            return 2.0 * std::hypot(real, imaginary) / static_cast<double>(heard.mono.size());
        };
        const auto first = level(440.0);
        const auto second = level(880.0);
        require(std::abs(first / second - 2.0) < 0.01,
            "a GAOscillators with its defaults should play harmonics of amplitude 1/n");
    }

    // A FunctionGenerator added as the Workbench adds it plays its Hann.
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"n", "talas.functionGenerator", {}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"n", "value", "out", "audioIn"});
    const auto compiled = talas::runtime::compileProject(document, registry(), settings(30000));
    require(compiled.ok(), "a FunctionGenerator with its defaults should compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 30000);
    require(rendered.ok() && std::ranges::any_of(rendered.mono, [](double sample) { return sample > 0.9; }),
        "a FunctionGenerator with its defaults should play its Hann");

    // An older project that leaves the field out keeps its empty table.
    auto json = talas::project::serializeProjectDocument(document);
    json.replace(json.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\":  85");
    const auto migrated = talas::project::parseProjectDocument(json);
    require(migrated.ok(), "a format 85 FunctionGenerator should open");
    const auto& parameters = migrated.document.graph.nodes.front().parameters;
    const auto wavetable = std::ranges::find(parameters, std::string_view{"wavetable"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(wavetable != parameters.end() && wavetable->resource == std::optional<std::string>{""},
        "a format 85 FunctionGenerator should keep its empty table");
}

void testExceptionsNameCatalogNodes()
{
    for (const auto* list : {&needsFieldValues, &needsUserMaterial,
             &rejectsOrdinaryAudio, &offlineOnlyDefaults})
    {
        for (const auto typeId : *list)
        {
            require(registry().findNodeType(typeId) != nullptr,
                std::string{typeId} + " is not a catalog node");
        }
    }
}
}

int main()
{
    try
    {
        testExceptionsNameCatalogNodes();
        testTableFieldsDefaultToTalasWavetables();
        testEveryPrototypePlaysWithItsDefaults();
        std::cout << "Prototype defaults tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
