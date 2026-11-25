#pragma once

#include "talas/audio/ResourceLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Plays a catalog node as the Workbench adds it: with its catalog defaults,
// its required inputs fed by an oscillator, a constant or a standard
// spectrum source, a sample or file field given a synthesized WAV, a MIDI
// file field a synthesized MIDI file and any other empty resource field
// the Sine wavetable. A spectrum output is heard through an
// OscillatorBank. The exceptions below need material a user has to supply
// or fail on ordinary audio.
namespace talas::test::prototypes
{
using Parameter = talas::project::ProjectParameterExpression;
using talas::core::PortType;

// Nodes whose catalog leaves empty a field their prototype fills.
inline const std::set<std::string_view> needsFieldValues{};

// Nodes that need files, lists, variables or names chosen by the user.
inline const std::set<std::string_view> needsUserMaterial{
    "talas.mapEventValues",
    // A PartialSpectrum analysis (ADR-0013).
    "talas.partialTrackPlayerV1",
    // Compiling adds it for a field with a pasted Sound (ADR-0020).
    "talas.pastedSoundExpressionV1",
    "talas.sampleMapPlayer",
    "talas.sharedFileName",
    "talas.sharedFileNames",
    "talas.surroundifer",
};

// Nodes that reject ordinary audio at an input. ClippingDivide, which failed
// on the zero denominator audio reaches at every zero crossing, now clips.
inline const std::set<std::string_view> rejectsOrdinaryAudio{};

// Nodes that play offline with their defaults but need more for realtime.
inline const std::set<std::string_view> offlineOnlyDefaults{};

// Inputs a node needs one of, though none is marked required.
inline const std::map<std::string_view, std::string_view> alternativeInputs{
    {"talas.hysteresis", "value"},
    {"talas.product", "inputs"},
    {"talas.linearToLog", "value"},
    {"talas.logToLinear", "value"},
    // A bank or a spectrum reader plays the spectrum it is given.
    {"talas.formantBank", "spectrum"},
    {"talas.formantBankOscillator", "spectrum"},
    {"talas.oscillatorBank", "spectrum"},
    {"talas.spectrumFundamental", "spectrumIn"},
    {"talas.spectrumTrackSelector", "spectrumIn"},
    {"talas.spectrumVoicedUnvoiced", "spectrumIn"},
    // A CloudBank's SpectrumSource, or its pair.
    {"talas.cloudBank", "spectrumSource"},
    {"talas.cloudBankElement", "spectrumSource"},
};

// The material a resource field is given in place of the Sine wavetable:
// a sample read into a table, a file a disk player streams, a MIDI file,
// the computed harmonic analysis, or nothing, for a field whose file this
// harness cannot synthesize.
enum class Material
{
    Sample,
    File,
    Midi,
    Harmonics,
    None,
};

inline const std::map<std::string_view, Material> materials{
    {"talas.antialiasedOscillator.fileName", Material::Sample},
    {"talas.cdpFastConvolution.impulse", Material::Sample},
    {"talas.cdpPitchAltharms.pitchSource", Material::Sample},
    {"talas.cdpPitchOctmove.pitchSource", Material::Sample},
    {"talas.cdpSpecBare.pitchSource", Material::Sample},
    {"talas.cdpSpecClean.noiseSource", Material::Sample},
    {"talas.cdpSpecClean.signalSource", Material::Sample},
    {"talas.cdpStrangeInvert.normalisingSource", Material::Sample},
    {"talas.diffuseSpectralStretch.sample", Material::Sample},
    // A DiskCache with nothing recorded passes its Input through.
    {"talas.diskCache.fileName", Material::None},
    {"talas.diskPlayer.fileName", Material::File},
    // A second analysis to morph toward.
    {"talas.gaOscillators.analysis1", Material::Harmonics},
    {"talas.genericSource.sample", Material::Sample},
    {"talas.keyMappedMultisample.firstSample", Material::Sample},
    // A Lime binary file, whose format our
    // references do not give.
    {"talas.limeInterpreter.fileName", Material::None},
    {"talas.midiFileEcho.fileName", Material::Midi},
    {"talas.midiFileEchoTap.fileName", Material::Midi},
    {"talas.sample.sample", Material::Sample},
    {"talas.sampleCloud.sample", Material::Sample},
    {"talas.sampleWithTimeIndex.sample", Material::Sample},
    {"talas.samplesFromDiskSingleStep.fileName", Material::Sample},
    {"talas.spectrumFromSingleCycle.fileName", Material::Sample},
    {"talas.sumOfSines.analysis1", Material::Harmonics},
    {"talas.timeFrequencyScale.sample", Material::Sample},
    {"talas.timeIndexFromFileName.fileName", Material::Sample},
};

// File lists given the harness's two tones, the second an octave lower
//.
inline const std::map<std::string_view, std::string_view> fileLists{
    {"talas.multiSampleWithTimeIndex.fileNames", "Samples/Tone Samples/Low"},
    {"talas.multifileDiskPlayer.fileNames", "Samples/Tone Samples/Low"},
    {"talas.multisample.samples", "Samples/Tone Samples/Low"},
    {"talas.splitSurroundSampleWithTimeIndex.fileNames", "Samples/Tone Samples/Low"},
    {"talas.timeIndexForFileNames.fileNames", "Samples/Tone Samples/Low"},
    {"talas.timeIndexForSamples.fileNames", "Samples/Tone Samples/Low"},
};

inline constexpr std::string_view toneSample = "Samples/Tone";
inline constexpr std::string_view lowSample = "Samples/Low";
inline constexpr std::string_view toneFile = "Audio/Tone";
inline constexpr std::string_view midiFile = "MIDI/Notes";

inline const talas::core::NodeRegistry& registry()
{
    static const auto builtins = talas::core::makeBuiltinNodeRegistry();
    return builtins;
}

// Whether the harness plays the node at all.
inline bool playable(const talas::core::NodeTypeDescriptor& type)
{
    return type.typeId != "talas.audioOutput"
        && !needsFieldValues.contains(type.typeId)
        && !needsUserMaterial.contains(type.typeId)
        && !rejectsOrdinaryAudio.contains(type.typeId);
}

// Whether the harness feeds the input from a source node.
inline bool fed(
    const talas::core::NodeTypeDescriptor& type,
    const talas::core::PortDescriptor& input)
{
    const auto alternative = alternativeInputs.find(type.typeId);
    return input.required
        || (alternative != alternativeInputs.end()
            && alternative->second == input.id);
}

inline talas::project::ProjectNode source(const std::string& id, PortType type)
{
    Parameter parameter;
    if (type == PortType::Audio)
    {
        parameter.parameterId = "frequency";
        parameter.expression = "440 hz";
        return {id, "talas.oscillator", {parameter}};
    }
    parameter.parameterId = "value";
    parameter.expression = "0.5";
    return {id, "talas.constant", {parameter}};
}

struct Endpoint
{
    std::string node;
    std::string port;
};

inline void addOnce(talas::project::ProjectDocument& document, talas::project::ProjectNode node)
{
    for (const auto& existing : document.graph.nodes)
    {
        if (existing.instanceId == node.instanceId)
            return;
    }
    document.graph.nodes.push_back(std::move(node));
}

// The partials of a SyntheticSpectrumFromArray with its defaults, the
// serialized spectra the spectrum Sounds read.
inline Endpoint partials(talas::project::ProjectDocument& document, std::string port)
{
    addOnce(document, {"partials", "talas.syntheticSpectrumFromArray", {}});
    return {"partials", std::move(port)};
}

// A standard source for a spectrum input: a HarmonicSpectrum for the
// amplitude and frequency triples, a LiveSpectralAnalysis of a tone for a
// partial spectrum and a forward FFT of a tone for a complex one.
inline std::optional<Endpoint> spectrumSource(
    talas::project::ProjectDocument& document,
    const talas::core::NodeTypeDescriptor& type,
    const talas::core::PortDescriptor& input,
    const std::string& id)
{
    using talas::core::SpectralFrameFormat;
    const auto tone = [&] {
        document.graph.nodes.push_back(source(id + "Tone", PortType::Audio));
        return id + "Tone";
    };
    switch (input.signal.spectralFormat)
    {
    case SpectralFrameFormat::LegacyPartialTriples:
        if (type.typeId == "talas.spectrumLogToLinear")
        {
            // It undoes a SpectrumLinearToLog.
            document.graph.nodes.push_back({id + "Harmonics", "talas.harmonicSpectrum", {}});
            document.graph.nodes.push_back({id, "talas.spectrumLinearToLog", {}});
            document.graph.connections.push_back({id + "Harmonics", "spectrumOut", id, "spectrumIn"});
            return Endpoint{id, "spectrumOut"};
        }
        if (type.typeId == "talas.vocoderChannelBank")
        {
            // A SyntheticSpectrumFromArray that sends the
            // filters' bandwidths.
            Parameter bandwidths;
            bandwidths.parameterId = "sendBandwidths";
            bandwidths.boolean = true;
            document.graph.nodes.push_back({id, "talas.syntheticSpectrumFromArray", {bandwidths}});
            return Endpoint{id, "spectrumOut"};
        }
        document.graph.nodes.push_back({id, "talas.harmonicSpectrum", {}});
        return Endpoint{id, "spectrumOut"};
    case SpectralFrameFormat::PartialSpectrumV1:
        if (type.typeId == "talas.partialSpectrumTrackSelectorV1")
        {
            // A FrameSynchronizer times the partials' frames.
            document.graph.nodes.push_back({id, "talas.frameSynchronizer", {}});
            const auto amplitudes = partials(document, "amplitudeOut");
            const auto frequencies = partials(document, "frequencyOut");
            document.graph.connections.push_back({amplitudes.node, amplitudes.port, id, "amplitudes"});
            document.graph.connections.push_back({frequencies.node, frequencies.port, id, "frequencies"});
            return Endpoint{id, "spectrumOut"};
        }
        {
            const auto analysed = tone();
            document.graph.nodes.push_back({id, "talas.liveSpectralAnalysis", {}});
            document.graph.connections.push_back({analysed, "audioOut", id, "audioIn"});
            return Endpoint{id, "spectrumOut"};
        }
    case SpectralFrameFormat::ComplexSpectrumV1:
    {
        const auto analysed = tone();
        document.graph.nodes.push_back({id, "talas.forwardFftV1", {}});
        document.graph.connections.push_back({analysed, "audioOut", id, "audioIn"});
        return Endpoint{id, "spectrumOut"};
    }
    case SpectralFrameFormat::NotApplicable:
    case SpectralFrameFormat::VariableOrUnknown:
        break;
    }
    return std::nullopt;
}

// Where an input is fed from: Audio by an oscillator, Control by a
// constant, a spectrum by a standard source. A FrameSynchronizer and a
// SyntheticSpectrumFromSounds take the serialized partials of a
// SyntheticSpectrumFromArray, where an oscillator's negative half
// cycles would be negative frequencies.
inline std::optional<Endpoint> feed(
    talas::project::ProjectDocument& document,
    const talas::core::NodeTypeDescriptor& type,
    const talas::core::PortDescriptor& input,
    const std::string& id)
{
    if (input.type == PortType::Audio
        && (type.typeId == "talas.frameSynchronizer" || type.typeId == "talas.syntheticSpectrumFromSounds"))
    {
        return partials(document, input.id == "amplitudes" ? "amplitudeOut" : "frequencyOut");
    }
    if (input.type == PortType::Audio || input.type == PortType::Control)
    {
        document.graph.nodes.push_back(source(id, input.type));
        return Endpoint{id, input.type == PortType::Audio ? "audioOut" : "value"};
    }
    if (input.type == PortType::Spectrum || input.type == PortType::ComplexSpectrum)
        return spectrumSource(document, type, input, id);
    return std::nullopt;
}

// Where the node's output is heard: an Audio output directly, a Control
// output through a VCA over the constant audio input, a spectrum through
// an OscillatorBank or an inverse FFT, a SoundCollectionVariable through a
// Mixer, and a node without any of these beside an oscillator. A node that is itself a terminal replaces the
// Audio Output.
inline void hear(
    talas::project::ProjectDocument& document,
    const talas::core::NodeTypeDescriptor& type)
{
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    // A SoundCollectionVariable stands only in a field that takes more than
    // one Sound.
    if (type.typeId == "talas.soundCollectionVariable")
    {
        document.graph.nodes.push_back({"mix", "talas.mixerV2", {}});
        document.graph.connections.push_back({"n", "audioOut", "mix", "inputs"});
        document.graph.connections.push_back({"mix", "leftOut", "out", "audioIn"});
        return;
    }
    for (const auto& output : type.outputs)
    {
        if (output.type == PortType::Audio)
        {
            document.graph.connections.push_back({"n", output.id, "out", "audioIn"});
            return;
        }
    }
    for (const auto& output : type.outputs)
    {
        if (output.type == PortType::Control)
        {
            document.graph.nodes.push_back({"one", "talas.audioInput", {}});
            document.graph.nodes.push_back({"vca", "talas.vca", {}});
            document.graph.connections.push_back({"one", "audioOut", "vca", "audioIn"});
            document.graph.connections.push_back({"n", output.id, "vca", "gain"});
            document.graph.connections.push_back({"vca", "audioOut", "out", "audioIn"});
            return;
        }
    }
    for (const auto& output : type.outputs)
    {
        if (output.type == PortType::Spectrum)
        {
            document.graph.nodes.push_back({"bank", "talas.oscillatorBank", {}});
            document.graph.connections.push_back({"n", output.id, "bank", "spectrum"});
            document.graph.connections.push_back({"bank", "audioOut", "out", "audioIn"});
            return;
        }
        if (output.type == PortType::ComplexSpectrum)
        {
            document.graph.nodes.push_back({"inverse", "talas.inverseFftV1", {}});
            document.graph.connections.push_back({"n", output.id, "inverse", "spectrumIn"});
            document.graph.connections.push_back({"inverse", "audioOut", "out", "audioIn"});
            return;
        }
    }
    document.graph.nodes.push_back(source("tone", PortType::Audio));
    document.graph.connections.push_back({"tone", "audioOut", "out", "audioIn"});
}

struct Patch
{
    talas::project::ProjectDocument document;
    // Empty when the harness can feed every input the node needs.
    std::string error;
};

// The node as "n" with the given parameters over its catalog defaults.
inline Patch patch(
    const talas::core::NodeTypeDescriptor& type,
    std::vector<Parameter> parameters,
    bool withAudioOutput)
{
    for (const auto& parameter : type.parameters)
    {
        if (parameter.kind == talas::core::ParameterKind::Resource
            && parameter.defaultResource.empty())
        {
            const auto material = materials.find(type.typeId + "." + parameter.id);
            std::string_view resource = "Sine";
            if (material != materials.end())
            {
                switch (material->second)
                {
                case Material::Sample:
                    resource = toneSample;
                    break;
                case Material::File:
                    resource = toneFile;
                    break;
                case Material::Midi:
                    resource = midiFile;
                    break;
                case Material::Harmonics:
                    resource = "HarmonicAmplitudes";
                    break;
                case Material::None:
                    continue;
                }
            }
            Parameter table;
            table.parameterId = parameter.id;
            table.resource = std::string{resource};
            parameters.push_back(table);
        }
        const auto list = fileLists.find(type.typeId + "." + parameter.id);
        if (parameter.kind == talas::core::ParameterKind::Text && list != fileLists.end())
        {
            Parameter files;
            files.parameterId = parameter.id;
            files.text = std::string{list->second};
            parameters.push_back(files);
        }
    }
    Patch result;
    auto& document = result.document;
    // A FeedbackLoopOutput hears what a FeedbackLoopInput of the same name
    // writes; named, the pair leaves the
    // Sound one output.
    Parameter loopName;
    loopName.parameterId = "connection";
    loopName.text = "Loop";
    if (type.typeId == "talas.feedbackLoopOutput"
        && std::ranges::none_of(parameters, [](const Parameter& given) { return given.parameterId == "connection"; }))
    {
        parameters.push_back(loopName);
    }
    document.graph.nodes.push_back({"n", type.typeId, std::move(parameters)});
    if (type.typeId == "talas.feedbackLoopOutput")
    {
        document.graph.nodes.push_back(source("looped", PortType::Audio));
        document.graph.nodes.push_back({"loopInput", "talas.feedbackLoopInput", {loopName}});
        document.graph.connections.push_back({"looped", "audioOut", "loopInput", "audioIn"});
    }
    std::size_t sources = 0;
    for (const auto& input : type.inputs)
    {
        if (!fed(type, input))
            continue;
        const auto id = "source" + std::to_string(sources++);
        const auto from = feed(document, type, input, id);
        if (!from.has_value())
        {
            result.error = "it needs a " + input.id + " input this harness cannot feed";
            return result;
        }
        document.graph.connections.push_back({from->node, from->port, "n", input.id});
    }
    if (withAudioOutput)
        hear(document, type);
    return result;
}

// Short tones of 437.5 Hz and an octave lower, whole cycles in 1536
// frames, written as mono 16-bit WAVs and read back as a sample is read,
// each with its loop over the whole file and its base pitch at the nearest
// key. They stay within the blocks a realtime disk stream decodes before it
// starts.
struct Tone
{
    std::filesystem::path path;
    std::filesystem::path lowPath;
    talas::runtime::TableResource table;
    talas::runtime::TableResource low;
    talas::core::ResourceSnapshotPtr file;

    static talas::audio::TableResourceLoadResult write(const std::filesystem::path& path, double frequency)
    {
        constexpr double sampleRate = 48000.0;
        std::vector<double> samples(1536);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
        {
            samples[frame] = 0.5
                * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / sampleRate);
        }
        std::string error;
        const std::vector<std::vector<double>> channels{samples};
        if (!talas::audio::writePcm16Wav(path, sampleRate, channels, error))
            throw std::runtime_error("the harness tone should be written: " + error);
        auto loaded = talas::audio::loadTableResource(path);
        if (!loaded.ok())
            throw std::runtime_error("the harness tone should read back: " + loaded.error);
        return loaded;
    }

    static talas::runtime::TableResource sample(
        std::string_view id, const talas::audio::TableResourceLoadResult& loaded, std::uint32_t key)
    {
        talas::runtime::TableResource table;
        table.id = std::string{id};
        table.samples = loaded.samples;
        table.sourceSampleRate = loaded.sourceSampleRate;
        table.forwardLoop = talas::core::SampleLoop{0, loaded.samples.size()};
        table.samplePitch = talas::core::SamplePitch{key, 0};
        return table;
    }

    Tone()
    {
        constexpr double sampleRate = 48000.0;
        const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() / ("talas-prototype-tone-" + stamp + ".wav");
        lowPath = std::filesystem::temp_directory_path() / ("talas-prototype-low-" + stamp + ".wav");
        const auto loaded = write(path, 437.5);
        table = sample(toneSample, loaded, 69);
        low = sample(lowSample, write(lowPath, 218.75), 57);
        const auto& samples = loaded.samples;

        talas::core::ResourceSnapshot snapshot;
        snapshot.id = std::string{toneFile};
        snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
        snapshot.generation = 1;
        snapshot.contentHash = loaded.contentHash;
        snapshot.resolvedPath = path;
        snapshot.channelCount = 1;
        snapshot.sampleRate = sampleRate;
        snapshot.frameCount = samples.size();
        snapshot.encoding = "pcm16-wav";
        snapshot.projectAssetPath = "media/tone.wav";
        snapshot.provenance = "repository-original";
        file = std::make_shared<const talas::core::ResourceSnapshot>(std::move(snapshot));
    }

    Tone(const Tone&) = delete;
    Tone& operator=(const Tone&) = delete;

    ~Tone()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(lowPath, ignored);
    }
};

inline const Tone& tone()
{
    static const Tone written;
    return written;
}

// A Standard MIDI File of one track: middle C held for a beat at 120 bpm.
inline talas::runtime::BinaryResource notes()
{
    return {std::string{midiFile},
        {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
            'M', 'T', 'r', 'k', 0, 0, 0, 12,
            0x00, 0x90, 60, 100,
            0x60, 0x80, 60, 0,
            0x00, 0xFF, 0x2F, 0x00}};
}

// Two channels of constant live input, and the harness's tones and MIDI
// file.
inline talas::runtime::RenderSettings settings(std::size_t frames)
{
    talas::runtime::RenderSettings settings{48000.0, 256};
    settings.audioInputChannels.push_back({1, std::vector<double>(frames, 1.0)});
    settings.audioInputChannels.push_back({2, std::vector<double>(frames, 1.0)});
    settings.tableResources.push_back(tone().table);
    settings.tableResources.push_back(tone().low);
    settings.resourceSnapshots.push_back(tone().file);
    settings.binaryResources.push_back(notes());
    return settings;
}
}
