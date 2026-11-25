#include "talas/core/BuiltinNodes.h"

#include "talas/core/BuiltinNodeBindings.h"
#include "talas/core/BuiltinKernelRegistry.h"
#include "talas/project/ProjectSoundVariables.h"
#include <string>
#include <string_view>
#include <utility>

namespace talas::core
{
namespace
{
[[nodiscard]] PortDescriptor input(std::string id, PortType type)
{
    return {std::move(id), type, true};
}

[[nodiscard]] PortDescriptor optionalInput(std::string id, PortType type)
{
    return {std::move(id), type, false};
}

[[nodiscard]] PortDescriptor compilerPopulatedInput(
    std::string id,
    PortType type)
{
    auto result = optionalInput(std::move(id), type);
    result.compilerPopulated = true;
    return result;
}

[[nodiscard]] PortDescriptor output(std::string id, PortType type)
{
    return {std::move(id), type, false};
}

// A CDP multichannel tool's eight outputs, out1 to out8: it plays its
// channels on the first, the others silent.
[[nodiscard]] std::vector<PortDescriptor> cdpChannelOutputs()
{
    std::vector<PortDescriptor> ports;
    for (std::size_t channel = 1; channel <= 8; ++channel)
        ports.push_back(output("out" + std::to_string(channel), PortType::Audio));
    return ports;
}

// A CDP port's left and right outputs, playing one or two channels, and
// out1 to out8, playing every channel.
[[nodiscard]] std::vector<PortDescriptor> cdpStereoAndChannelOutputs()
{
    auto ports = cdpChannelOutputs();
    ports.insert(ports.begin(), {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)});
    return ports;
}

// A CDP multichannel tool's inputs in1 to in8, the first `required` of them
// needed (the tool's fewest channels): its channels are those connected,
// in1 to inN without a gap.
[[nodiscard]] std::vector<PortDescriptor> cdpChannelInputs(std::size_t required)
{
    std::vector<PortDescriptor> ports;
    for (std::size_t channel = 1; channel <= 8; ++channel)
    {
        auto id = "in" + std::to_string(channel);
        ports.push_back(channel <= required ? input(std::move(id), PortType::Audio)
                                            : optionalInput(std::move(id), PortType::Audio));
    }
    return ports;
}

[[nodiscard]] PortDescriptor partialSpectrumPort(
    std::string id,
    bool required,
    SpectralFrameFormat format)
{
    return {
        std::move(id),
        PortType::Spectrum,
        required,
        {
            PortSignalRate::Frame,
            AudioBusLayoutKind::Mono,
            PortEdgeSemantics::Continuous,
            format,
            true,
        },
    };
}

[[nodiscard]] ParameterDescriptor parameter(
    std::string id,
    std::string defaultExpression,
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar)
{
    return {std::move(id), std::move(defaultExpression), domain, ParameterKind::Expression, {}, {}};
}

[[nodiscard]] ParameterDescriptor defaultKeywordParameter(
    ParameterDescriptor descriptor,
    double defaultKeywordValue)
{
    descriptor.defaultKeywordValue = defaultKeywordValue;
    return descriptor;
}

void add(NodeRegistry& registry, NodeTypeDescriptor descriptor)
{
    registry.addNodeType(std::move(descriptor));
}
}

static void registerBuiltinNodeDescriptorsPart1(NodeRegistry& registry)
{
    add(registry, {
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("amplitude", PortType::Control),
            optionalInput("envelope", PortType::Control),
            optionalInput("modulator", PortType::Audio),
            optionalInput("pitchBend", PortType::Control),
            optionalInput("reset", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("envelope", "1.0", talas::expr::ValueDomain::Scalar),
            textParameter("modulation", {}),
            parameter("maxMI", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("pitchBend", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", {}),
            choiceParameter("interpolation", "linear", {"linear", "none"}),
            choiceParameter(
                "tableLength", "first4096", {"first4096", "legacyWholeTable"}),
        },
    });

    add(registry, {
        "talas.antialiasedOscillator",
        "Antialiased Oscillator",
        "Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("brightness", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("brightness", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
            choiceParameter("channel", "mono", {"mono", "left", "right"}),
        },
    });

    add(registry, {
        "talas.noise",
        "Noise",
        "Xtra Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("centerValue", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("initialState", "0.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("type", "white", {"white", "pink", "hotPink"}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("centerValue", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            // HotPink's spectrum peaks at Frequency; projects before
            // format 92 smoothed random steps, whose spectrum dipped there.
            // occasionalJumps plays another waveform instead: small changes
            // about a value, a large jump to a new one once a period of
            // Frequency, then small changes about that.
            choiceParameter("hotPink", "peakAtFrequency", {"peakAtFrequency", "legacySmoothedSteps", "occasionalJumps"}),
        },
    });

    add(registry, {
        "talas.sample",
        "Sample",
        "Sampling",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
            // A Sound pasted into Start, End, LoopStart or LoopEnd.
            optionalInput("start", PortType::Control),
            optionalInput("end", PortType::Control),
            optionalInput("loopStart", PortType::Control),
            optionalInput("loopEnd", PortType::Control),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // A Sample's recorded pitch; a recording without one is taken
            // to be middle C, 4 c.
            defaultKeywordParameter(
                parameter("frequency", "default", talas::expr::ValueDomain::Hertz),
                261.6255653005986),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("sample", {}),
            booleanParameter("setLoop", false),
            parameter("loopStart", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("loopEnd", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("loopFade", false),
            parameter("start", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("end", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("fromMemoryWriter", false),
            parameter("attackTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            // A loop in the sample file's header loops the sample while Gate
            // is positive; projects before format 71 ignored it.
            choiceParameter("headerLoop", "loop", {"loop", "legacyIgnore"}),
            // A stereo file plays its left channel on audioOut and its right
            // on rightOut; projects before format 95 play as before
            // (averaged, left alone, or a stereo table refused).
            choiceParameter("channels", "stereo", {"stereo", "legacyMono"}),
        },
    });

    add(registry, {
        "talas.sampleMapPlayer",
        "Sample Map Player",
        "Sampling",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("velocity", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            textParameter("sampleMapId", {}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("velocity", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("attackTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0 s", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.forcedProcessorAssignment",
        "Forced Processor Assignment",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {parameter("processor", "1", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.diskPlayer",
        "Disk Player",
        "Sampling",
        {
            optionalInput("filePosition", PortType::Control),
            optionalInput("duration", PortType::Control),
            optionalInput("trigger", PortType::Control),
            optionalInput("rateScale", PortType::Control),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            parameter("filePosition", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("duration", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("rateScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            // Duration is the Sound's whole life, within which a
            // Trigger restarts the file; projects before format 56
            // played each trigger for Duration.
            choiceParameter("durationBounds", "sound", {"sound", "legacyPlayback"}),
            // A stereo file plays its left channel on audioOut and its right
            // on rightOut; projects before format 95 play as before
            // (averaged, left alone, or a stereo table refused).
            choiceParameter("channels", "stereo", {"stereo", "legacyMono"}),
        },
    });

    // Segments of one or more sample files, a
    // line each (the file, then optionally its first sample and the sample
    // after its last), played one after another as their files hold them,
    // at no other sample rate.
    add(registry, {
        "talas.diskSplicer",
        "Disk Splicer",
        "Sampling",
        {},
        {output("audioOut", PortType::Audio)},
        {textParameter("segments", {})},
    });

    add(registry, {
        "talas.diskCache",
        "Disk Cache",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            booleanParameter("record", false),
        },
    });

    add(registry, {
        "talas.partialTrackPlayerV1",
        "Partial Track Player",
        "Spectral",
        {},
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("partialTrack", {}),
            parameter("gain", "1.0", talas::expr::ValueDomain::Scalar),
        },
        NodeSupportLevel::Utility,
        "Talas utility.",
        NodeCapabilityDescriptor{
            .limitations = {
                "maximum-4096-tracks-per-segment-for-offline-render",
                "prepared-realtime-maximum-128-tracks-per-segment",
            },
        },
    });

    add(registry, {
        "talas.diskRecorder",
        "Disk Recorder",
        "Sampling",
        {
            input("audioIn", PortType::Audio),
            // A stereo recording's right channel; unconnected, the Input's
            //.
            optionalInput("rightIn", PortType::Audio),
            optionalInput("trigger", PortType::Control),
            optionalInput("captureDuration", PortType::Control),
        },
        // Placed anywhere in a Sound, a
        // DiskRecorder passes its Input on as it records it; the compiler
        // then prepares it as a talas.diskRecorderTap.
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            textParameter("format", {}),
            textParameter("wordSize", {}),
            choiceParameter("channels", "mono", {"mono", "stereo"}),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("captureDuration", "0 s", talas::expr::ValueDomain::Seconds),
            booleanParameter("gated", false),
        },
    });

    // A DiskRecorder whose Input goes on to another Sound: it
    // passes the Input on unchanged and records it as a terminal
    // DiskRecorder does. Only the compiler makes these.
    add(registry, {
        "talas.diskRecorderTap",
        "Disk Recorder Tap",
        "Sampling",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            optionalInput("trigger", PortType::Control),
            optionalInput("captureDuration", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            textParameter("format", {}),
            textParameter("wordSize", {}),
            choiceParameter("channels", "mono", {"mono", "stereo"}),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("captureDuration", "0 s", talas::expr::ValueDomain::Seconds),
            booleanParameter("gated", false),
        },
    });

    add(registry, {
        "talas.memoryWriter",
        "Memory Writer",
        "Sampling",
        {
            input("audioIn", PortType::Audio),
            optionalInput("trigger", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("captureDuration", "0 s", talas::expr::ValueDomain::Seconds),
            booleanParameter("global", false),
            booleanParameter("cyclic", false),
            resourceParameter("recordingName", {}),
            booleanParameter("silent", false),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.crossfade",
        "Crossfade",
        "Level, Compression, Expansion",
        {
            input("snd1", PortType::Audio),
            input("snd2", PortType::Audio),
            {"pan", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"fade", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("pan", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("fade", "0.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("type", "power", {"power", "linear"}),
        },
    });

    add(registry, {
        "talas.dynamicRangeController",
        "Dynamic Range Controller",
        "Level, Compression, Expansion",
        {
            input("sideChain", PortType::Audio),
            input("audioIn", PortType::Audio),
            optionalInput("gain", PortType::Control),
            optionalInput("threshold", PortType::Control),
            optionalInput("ratio", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
            optionalInput("delay", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            // An upwardExpander boosts the
            // Input while the SideChain is below the Threshold; expander,
            // as projects have played it, widens the range below it and
            // gates.
            choiceParameter("type", "compressor", {"compressor", "expander", "upwardExpander"}),
            parameter("ratio", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("threshold", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("attackTime", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("delay", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("gain", "1.0", talas::expr::ValueDomain::Scalar),
            // The Ratio is in decibels; projects
            // before format 100 keep the linear law.
            choiceParameter("ratioLaw", "decibels", {"decibels", "legacyLinear"}),
        },
    });

    add(registry, {
        "talas.level",
        "Level",
        "Level, Compression, Expansion",
        {
            {
                "audioIn",
                PortType::Audio,
                true,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::Mono,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    true,
                },
            },
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {
            {
                "leftOut",
                PortType::Audio,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::Mono,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    true,
                },
            },
            {
                "rightOut",
                PortType::Audio,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::Mono,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    true,
                },
            },
        },
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("noGain", false),
            // A Gain's Levels clip at full scale (format 148).
            choiceParameter("overflow", "unclipped", {"unclipped", "clip"}),
        },
    });

    add(registry, {
        "talas.multiplyingWaveshaper",
        "Multiplying Waveshaper",
        "Level, Compression, Expansion",
        {
            input("nonlinearInput", PortType::Audio),
            input("audioIn", PortType::Audio),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "FullRamp"),
        },
    });

    add(registry, {
        "talas.keyMappedMultisample",
        "Key Mapped Multisample",
        "Sampling",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("loFreq", PortType::Control),
            optionalInput("hiFreq", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("velocity", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("velocity", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("firstSample", {}),
            parameter("loFreq", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("hiFreq", "20000 hz", talas::expr::ValueDomain::Hertz),
            choiceParameter("mapping", "byBasePitch", {"onePerHalfStep", "equallySpaced", "byBasePitch", "byPitchRange"}),
            parameter("attackTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("loop", false),
            booleanParameter("fromDisk", false),
            booleanParameter("noTransposition", false),
            // Loop loops at the loop in the file's header; projects
            // before format 73 looped the whole file.
            choiceParameter("headerLoop", "loop", {"loop", "legacyIgnore"}),
            // Every sample file in FirstSample's folder makes the bank;
            // projects before format 82 played FirstSample alone.
            choiceParameter("samples", "folder", {"folder", "legacyFirstSampleOnly"}),
            // A stereo file plays its left channel on audioOut and its right
            // on rightOut; projects before format 95 play as before
            // (averaged, left alone, or a stereo table refused).
            choiceParameter("channels", "stereo", {"stereo", "legacyMono"}),
            // Where OnePerHalfStep starts counting half steps: at the key
            // LoFreq gives (the default), or with the first sample on MIDI
            // key 1, the next on key 2.
            choiceParameter("halfStepsFrom", "loFreq", {"loFreq", "keyOne"}),
        },
    });

    add(registry, {
        "talas.multifileDiskPlayer",
        "Multifile Disk Player",
        "Sampling",
        {
            optionalInput("index", PortType::Control),
            optionalInput("trigger", PortType::Control),
            optionalInput("rateScale", PortType::Control),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            staticResourceListParameter("fileNames", {}),
            parameter("rateScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("gated", false),
            parameter("index", "0", talas::expr::ValueDomain::Scalar),
            // A stereo file plays its left channel on audioOut and its right
            // on rightOut; projects before format 95 play as before
            // (averaged, left alone, or a stereo table refused).
            choiceParameter("channels", "stereo", {"stereo", "legacyMono"}),
        },
    });

    add(registry, {
        "talas.multisample",
        "Multisample",
        "Sampling",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("index", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("frequency", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            staticResourceListParameter("samples", {}),
            parameter("index", "0", talas::expr::ValueDomain::Scalar),
            booleanParameter("loop", false),
            parameter("loopStart", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("loopEnd", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("attackTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            // Loop loops at the loop in the selected file's header; projects
            // before format 73 used LoopStart and LoopEnd.
            choiceParameter("headerLoop", "loop", {"loop", "legacyIgnore"}),
            // An Event Value Gate that falls releases the file to play on
            // to its end, as a connected Gate does; projects before
            // format 77 stopped it.
            choiceParameter("gateRelease", "playsThrough", {"playsThrough", "legacyStops"}),
            // A stereo file plays its left channel on audioOut and its right
            // on rightOut; projects before format 95 play as before
            // (averaged, left alone, or a stereo table refused).
            choiceParameter("channels", "stereo", {"stereo", "legacyMono"}),
        },
    });

    add(registry, {
        "talas.sampleCloud",
        "Sample Cloud",
        "Sampling",
        {
            optionalInput("amplitude", PortType::Control),
            optionalInput("pan", PortType::Control),
            optionalInput("timeIndex", PortType::Control),
            optionalInput("grainDur", PortType::Control),
            optionalInput("density", PortType::Control),
            optionalInput("maxGrains", PortType::Control),
            optionalInput("panJitter", PortType::Control),
            optionalInput("grainDurJitter", PortType::Control),
            optionalInput("timeIndexJitter", PortType::Control),
        },
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            resourceParameter("sample", {}),
            resourceParameter("grainEnv", "Hann"),
            parameter("maxGrains", "16", talas::expr::ValueDomain::Scalar),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("density", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("grainDur", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("grainDurJitter", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("pan", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("panJitter", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("seed", "0.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("fromMemoryWriter", false),
            parameter("timeIndex", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("timeIndexJitter", "0.0", talas::expr::ValueDomain::Scalar),
            // A grain keeps the start point, duration and pan it starts with
            //; projects before format 74 followed them per
            // frame.
            choiceParameter("grainShapes", "atGrainStart",
                {"atGrainStart", "legacyEveryFrame"}),
            // GrainEnv's own FromMemoryWriter box,
            // the grains shaped by the recording a MemoryWriter of the
            // Sound is making under that name; off, the file's.
            booleanParameter("grainEnvFromMemoryWriter", false),
        },
    });

    add(registry, {
        "talas.sampleAndHold",
        "Sample And Hold",
        "Sampling",
        {
            input("value", PortType::Control),
            optionalInput("holdTime", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("holdTime", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("offsetTime", "0 s", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.samplesFromDiskSingleStep",
        "Samples From Disk Single Step",
        "Sampling",
        {
            optionalInput("filePosition", PortType::Control),
            optionalInput("trigger", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            parameter("filePosition", "0", talas::expr::ValueDomain::Scalar),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.spectralShape",
        "Spectral Shape",
        "Spectral Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("spacing", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {
            output("spectrumOut", PortType::Spectrum),
            // Amplitudes on the left channel, frequencies on the
            // right, a partial on each sample.
            output("amplitudeOut", PortType::Audio),
            output("frequencyOut", PortType::Audio),
        },
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("spacing", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("nbrPartials", "1", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "Hann"),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            // The Wavetable is the shape of the spectrum from 0 hz to half
            // the sample rate; projects before format 55 read one
            // sample per partial.
            choiceParameter("envelope", "frequency", {"frequency", "legacyPartial"}),
        },
    });

    add(registry, {
        "talas.spectrumInRam",
        "Spectrum In RAM",
        "Spectral Sources",
        {
            optionalInput("level", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("timeIndex", PortType::Control),
            optionalInput("nbrPartials", PortType::Control),
            optionalInput("firstPartial", PortType::Control),
        },
        {
            output("spectrumOut", PortType::Spectrum),
            output("amplitudeOut", PortType::Audio),
            output("frequencyOut", PortType::Audio),
        },
        {
            // `default` leaves the analysis unchanged: its own frequency
            // and all of its partials.
            defaultKeywordParameter(
                parameter("frequency", "0 hz", talas::expr::ValueDomain::Hertz), 0.0),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("timeIndex", "0.0", talas::expr::ValueDomain::Scalar),
            // A computed harmonic analysis whose base pitch Frequency sets
            //, until an analysis is chosen.
            resourceParameter("analysis", "HarmonicSpectrum"),
            parameter("analysisFrames", "1", talas::expr::ValueDomain::Scalar),
            parameter("analysisFrequency", "1 hz", talas::expr::ValueDomain::Hertz),
            defaultKeywordParameter(
                parameter("nbrPartials", "0", talas::expr::ValueDomain::Scalar), 0.0),
            parameter("firstPartial", "1", talas::expr::ValueDomain::Scalar),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.spectrumOnDisk",
        "Spectrum On Disk",
        "Spectral Sources",
        {
            optionalInput("level", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("rateScale", PortType::Control),
            optionalInput("trigger", PortType::Control),
            optionalInput("nbrPartials", PortType::Control),
            optionalInput("firstPartial", PortType::Control),
        },
        {output("spectrumOut", PortType::Spectrum)},
        {
            // `default` leaves the analysis unchanged.
            defaultKeywordParameter(
                parameter("frequency", "0 hz", talas::expr::ValueDomain::Hertz), 0.0),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("rateScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("trigger", "0.0", talas::expr::ValueDomain::Scalar),
            // A computed harmonic analysis whose base pitch Frequency sets
            //, until an analysis is chosen.
            resourceParameter("fileName", "HarmonicSpectrum"),
            parameter("analysisFrames", "1", talas::expr::ValueDomain::Scalar),
            parameter("analysisFrequency", "1 hz", talas::expr::ValueDomain::Hertz),
            defaultKeywordParameter(
                parameter("nbrPartials", "0", talas::expr::ValueDomain::Scalar), 0.0),
            parameter("firstPartial", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.syntheticSpectrumFromArray",
        "Synthetic Spectrum From Array",
        "Spectral Sources",
        {optionalInput("envelope", PortType::Control)},
        {
            output("spectrumOut", PortType::Spectrum),
            output("amplitudeOut", PortType::Audio),
            output("frequencyOut", PortType::Audio),
        },
        {
            parameter("nbrPartials", "4", talas::expr::ValueDomain::Scalar),
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", false),
            parameter("envelope", "1.0", talas::expr::ValueDomain::Scalar),
            textParameter("amplitudes", "0.5 0.25 0.125 0.0625"),
            textParameter("frequencies", "220 440 660 880"),
            textParameter("bandwidths", "50 50 50 50"),
            // Live elements update once every NbrPartials samples;
            // projects before format 92 take them each block.
            choiceParameter("updates", "everyNbrPartials", {"everyNbrPartials", "legacyEveryBlock"}),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.harmonicSpectrum",
        "Harmonic Spectrum",
        "Spectral Sources",
        {},
        {output("spectrumOut", PortType::Spectrum)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("nbrPartials", "8", talas::expr::ValueDomain::Scalar),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.spectrumFromSingleCycle",
        "Spectrum From Single Cycle",
        "Spectral Sources",
        {},
        {output("spectrumOut", PortType::Spectrum)},
        {
            resourceParameter("fileName", {}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    add(registry, {
        "talas.syntheticSpectrumFromSounds",
        "Synthetic Spectrum From Sounds",
        "Spectral Sources",
        {
            input("amplitudes", PortType::Audio),
            input("frequenciesAndBandwidths", PortType::Audio),
            optionalInput("nbrPartials", PortType::Control),
        },
        {
            output("spectrumOut", PortType::Spectrum),
            output("amplitudeOut", PortType::Audio),
            output("frequencyOut", PortType::Audio),
        },
        {
            parameter("nbrPartials", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("logScale", false),
            booleanParameter("includesBandwidths", false),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
            // With LogScale its FrequenciesAndBandwidths are pitches, 1 half
            // the sample rate and each 1/15 below it an octave lower;
            // projects before format 107 read frequencies.
            choiceParameter("logFrequencies", "pitch", {"pitch", "legacyLinear"}),
        },
    });

    add(registry, {
        "talas.frameSynchronizer",
        "Frame Synchronizer",
        "Spectral Utilities",
        {
            input("amplitudes", PortType::Audio),
            input("frequencies", PortType::Audio),
            {"syncOutput", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Trigger,
              SpectralFrameFormat::NotApplicable, false}},
            {"syncInput", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Trigger,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("amplitudesOut", PortType::Audio),
            output("frequenciesOut", PortType::Audio),
            {"componentIndexOut", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            partialSpectrumPort(
                "spectrumOut", false,
                SpectralFrameFormat::PartialSpectrumV1),
        },
        {
            parameter("frameLength", "256", talas::expr::ValueDomain::Scalar),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.fft",
        "FFT",
        "Spectral Analysis FFT",
        {input("audioIn", PortType::Audio), optionalInput("rightIn", PortType::Audio)},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("length", "1024", talas::expr::ValueDomain::Scalar),
            booleanParameter("inverse", false),
            // Projects before format 62 keep the packed layout on one channel.
            choiceParameter("packing", "talasStereo", {"talasStereo", "legacyPacked"}),
        },
    });

    add(registry, {
        "talas.forwardFftV1",
        "Forward FFT v1",
        "Spectral Analysis FFT",
        {input("audioIn", PortType::Audio)},
        {output("spectrumOut", PortType::ComplexSpectrum)},
        {
            parameter("frameSize", "1024", talas::expr::ValueDomain::Scalar),
            parameter("hopSize", "1024", talas::expr::ValueDomain::Scalar),
            choiceParameter("window", "rectangular", {"rectangular", "hann"}),
        },
    });

    add(registry, {
        "talas.inverseFftV1",
        "Inverse FFT v1",
        "Spectral Analysis FFT",
        {input("spectrumIn", PortType::ComplexSpectrum)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("frameSize", "1024", talas::expr::ValueDomain::Scalar),
            parameter("hopSize", "1024", talas::expr::ValueDomain::Scalar),
            choiceParameter("window", "rectangular", {"rectangular", "hann"}),
        },
    });

    add(registry, {
        "talas.feedbackLoopInput",
        "Feedback Loop Input",
        "Xtra",
        {input("audioIn", PortType::Audio)},
        {},
        {textParameter("connection", {})},
    });

    add(registry, {
        "talas.feedbackLoopOutput",
        "Feedback Loop Output",
        "Xtra",
        {},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("connection", {}),
            parameter("delay", "0 s", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.matrix4",
        "Matrix 4",
        "Spatializing",
        {
            input("in1", PortType::Audio),
            input("in2", PortType::Audio),
            input("in3", PortType::Audio),
            input("in4", PortType::Audio),
        },
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
        },
        {
            textParameter("insToOut1", "1 0 0 0"),
            textParameter("insToOut2", "0 1 0 0"),
            textParameter("insToOut3", "0 0 1 0"),
            textParameter("insToOut4", "0 0 0 1"),
        },
    });

    add(registry, {
        "talas.matrix8",
        "Matrix 8",
        "Spatializing",
        {
            input("in1", PortType::Audio),
            input("in2", PortType::Audio),
            input("in3", PortType::Audio),
            input("in4", PortType::Audio),
            input("in5", PortType::Audio),
            input("in6", PortType::Audio),
            input("in7", PortType::Audio),
            input("in8", PortType::Audio),
        },
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            textParameter("insToOut1", "1 0 0 0 0 0 0 0"),
            textParameter("insToOut2", "0 1 0 0 0 0 0 0"),
            textParameter("insToOut3", "0 0 1 0 0 0 0 0"),
            textParameter("insToOut4", "0 0 0 1 0 0 0 0"),
            textParameter("insToOut5", "0 0 0 0 1 0 0 0"),
            textParameter("insToOut6", "0 0 0 0 0 1 0 0"),
            textParameter("insToOut7", "0 0 0 0 0 0 1 0"),
            textParameter("insToOut8", "0 0 0 0 0 0 0 1"),
        },
    });

    add(registry, {
        "talas.output4",
        "Output 4",
        "Spatializing",
        {
            input("out1", PortType::Audio),
            input("out2", PortType::Audio),
            input("out3", PortType::Audio),
            input("out4", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.output6",
        "Output 6",
        "Spatializing",
        {
            input("out1", PortType::Audio),
            input("out2", PortType::Audio),
            input("out3", PortType::Audio),
            input("out4", PortType::Audio),
            input("out5", PortType::Audio),
            input("out6", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.output8",
        "Output 8",
        "Spatializing",
        {
            input("out1", PortType::Audio),
            input("out2", PortType::Audio),
            input("out3", PortType::Audio),
            input("out4", PortType::Audio),
            input("out5", PortType::Audio),
            input("out6", PortType::Audio),
            input("out7", PortType::Audio),
            input("out8", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.stereoInOutput4",
        "Stereo In Output 4",
        "Spatializing",
        {
            input("out12", PortType::Audio),
            input("out34", PortType::Audio),
            // The right channels of stereo Sounds; a mono Sound
            // fills its pair alone.
            optionalInput("out12Right", PortType::Audio),
            optionalInput("out34Right", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.stereoInOutput8",
        "Stereo In Output 8",
        "Spatializing",
        {
            input("out12", PortType::Audio),
            input("out34", PortType::Audio),
            input("out56", PortType::Audio),
            input("out78", PortType::Audio),
            optionalInput("out12Right", PortType::Audio),
            optionalInput("out34Right", PortType::Audio),
            optionalInput("out56Right", PortType::Audio),
            optionalInput("out78Right", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.midiFileEcho",
        "MIDI File Echo",
        "MIDI Out",
        {},
        // In a Mixer beside audio Sounds, it copies its file to the MIDI
        // output, prepared as a talas.midiFileEchoTap.
        {output("audioOut", PortType::Audio)},
        {
            parameter("lowChannel", "1", talas::expr::ValueDomain::Scalar),
            parameter("highChannel", "16", talas::expr::ValueDomain::Scalar),
            resourceParameter("fileName", {}),
        },
    });

    // A MIDIFileEcho within a Sound. Only the compiler makes these.
    add(registry, {
        "talas.midiFileEchoTap",
        "MIDI File Echo Tap",
        "MIDI Out",
        {},
        {output("audioOut", PortType::Audio)},
        {
            parameter("lowChannel", "1", talas::expr::ValueDomain::Scalar),
            parameter("highChannel", "16", talas::expr::ValueDomain::Scalar),
            resourceParameter("fileName", {}),
        },
    });

    add(registry, {
        "talas.midiMapper",
        "MIDI Mapper",
        "MIDI In",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            textParameter("map", {}),
            choiceParameter("voiceModel", "polyphonicInput",
                {"legacyPassthrough", "polyphonicInput"}),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("channel", "0", talas::expr::ValueDomain::Scalar),
            textParameter("source", {}),
            resourceParameter("midiFile", {}),
            parameter("polyphony", "4", talas::expr::ValueDomain::Scalar),
            parameter("lowPitch", "0 nn", talas::expr::ValueDomain::NoteNumber),
            parameter("highPitch", "127 nn", talas::expr::ValueDomain::NoteNumber),
            textParameter("script", {}),
            booleanParameter("shared", false),
            // Unshared notes go to the first Sound on the channel and pitch
            // range that takes them; projects before format 81
            // gave every matching Sound each note.
            choiceParameter("unsharedNotes", "toFirstSound", {"toFirstSound", "legacyToEach"}),
            // A constant field of its Sound is read as each voice starts,
            // with that voice's Event Values (owner, 2026-10-12); projects
            // before format 91 read it once, as the Sound compiled.
            choiceParameter("constantFieldsRead", "eachVoice", {"eachVoice", "legacyWhenCompiled"}),
            // A script note without a frequency plays LowPitch; projects
            // before format 101 played note 0.
            choiceParameter("scriptNotePitch", "lowPitch", {"lowPitch", "legacyNoteZero"}),
        },
    });

    add(registry, {
        "talas.midiOutputController",
        "MIDI Output Controller",
        "MIDI Out",
        {
            optionalInput("channel", PortType::Control),
            optionalInput("controllerNumber", PortType::Control),
            optionalInput("value", PortType::Control),
        },
        // In a Mixer beside audio Sounds, prepared
        // as a talas.midiOutputControllerTap.
        {output("audioOut", PortType::Audio)},
        {
            // As the prototype comes, its Amp goes
            // out on MIDI channel 1 as continuous controller 7; projects
            // before format 152 keep controller 1 and an empty Value.
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
            parameter("controllerNumber", "7", talas::expr::ValueDomain::Scalar),
            textParameter("value", "!Amp"),
            // Value is a Sound's amplitude, sent as a 7-bit controller at
            // most once a millisecond; projects
            // before format 67 sent it as the controller value itself.
            choiceParameter("valueMeans", "amplitude",
                {"amplitude", "legacyControllerValue"}),
        },
    });

    // A MIDIOutputController within a Sound. Only the compiler
    // makes these.
    add(registry, {
        "talas.midiOutputControllerTap",
        "MIDI Output Controller Tap",
        "MIDI Out",
        {
            optionalInput("channel", PortType::Control),
            optionalInput("controllerNumber", PortType::Control),
            optionalInput("value", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
            parameter("controllerNumber", "7", talas::expr::ValueDomain::Scalar),
            textParameter("value", "!Amp"),
            // Value is a Sound's amplitude, sent as a 7-bit controller at
            // most once a millisecond; projects
            // before format 67 sent it as the controller value itself.
            choiceParameter("valueMeans", "amplitude",
                {"amplitude", "legacyControllerValue"}),
        },
    });

    add(registry, {
        "talas.midiOutputEvent",
        "MIDI Output Event",
        "MIDI Out",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("amplitude", PortType::Control),
            optionalInput("channel", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        // In a Mixer beside audio Sounds it
        // plays at the same time as they do, adding silence; the compiler
        // then prepares it as a talas.midiOutputEventTap.
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            // A Frequency between keys goes out as the nearest key and a
            // pitch bend; projects before format 67
            // sent the nearest key alone.
            choiceParameter("offKeyPitch", "pitchBend",
                {"pitchBend", "legacyNearestNote"}),
        },
    });

    // A MIDIOutputEvent within a Sound: it sends its notes
    // and adds silence. Only the compiler makes these.
    add(registry, {
        "talas.midiOutputEventTap",
        "MIDI Output Event Tap",
        "MIDI Out",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("amplitude", PortType::Control),
            optionalInput("channel", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            // A Frequency between keys goes out as the nearest key and a
            // pitch bend; projects before format 67
            // sent the nearest key alone.
            choiceParameter("offKeyPitch", "pitchBend",
                {"pitchBend", "legacyNearestNote"}),
        },
    });

    add(registry, {
        "talas.midiOutputEventInBytes",
        "MIDI Output Event In Bytes",
        "MIDI Out",
        {},
        {},
        {textParameter("bytes", {})},
    });

    add(registry, {
        "talas.midiOutputTimingClock",
        "MIDI Output Timing Clock",
        "MIDI Out",
        {},
        {},
        {},
    });

    add(registry, {
        "talas.midiOutputStartStopMasterClock",
        "MIDI Output Start/Stop Master Clock",
        "MIDI Out",
        {
            optionalInput("running", PortType::Control),
            optionalInput("restart", PortType::Control),
        },
        {},
        {
            parameter("running", "1", talas::expr::ValueDomain::Scalar),
            parameter("restart", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.midiVoice",
        "MIDI Voice",
        "MIDI In",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("channel", "0", talas::expr::ValueDomain::Scalar),
            textParameter("source", {}),
            resourceParameter("midiFile", {}),
            // The prototype plays two voices;
            // projects before format 120 played four.
            parameter("polyphony", "2", talas::expr::ValueDomain::Scalar),
            parameter("lowPitch", "0 nn", talas::expr::ValueDomain::NoteNumber),
            parameter("highPitch", "127 nn", talas::expr::ValueDomain::NoteNumber),
            textParameter("script", {}),
            booleanParameter("shared", false),
            // Unshared notes go to the first Sound on the channel and pitch
            // range that takes them; projects before format 81
            // gave every matching Sound each note.
            choiceParameter("unsharedNotes", "toFirstSound", {"toFirstSound", "legacyToEach"}),
            // A constant field of its Sound is read as each voice starts,
            // with that voice's Event Values (owner, 2026-10-12); projects
            // before format 91 read it once, as the Sound compiled.
            choiceParameter("constantFieldsRead", "eachVoice", {"eachVoice", "legacyWhenCompiled"}),
            // A script note without a frequency plays LowPitch; projects
            // before format 101 played note 0.
            choiceParameter("scriptNotePitch", "lowPitch", {"lowPitch", "legacyNoteZero"}),
        },
    });

    add(registry, {
        "talas.cloudBank",
        "Cloud Bank",
        "Aggregate Synthesis",
        {
            input("trigger", PortType::Control),
            input("pan", PortType::Control),
            input("freqMod", PortType::Control),
            input("grainDurMod", PortType::Control),
            // A Spectrum, or its amplitudes and frequencies as a pair of
            // Sounds, amplitudes on the left and frequencies on the right
            //.
            optionalInput("spectrumSource", PortType::Spectrum),
            optionalInput("amplitudeIn", PortType::Audio),
            optionalInput("frequencyIn", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            // The defaults are the project's checked CloudBank example
            // (c111), with BankSize left at "default".
            parameter("nbrTracks", "2", talas::expr::ValueDomain::Scalar),
            parameter("grainsPerTrack", "4", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "default", talas::expr::ValueDomain::Scalar), 8.0),
            resourceParameter("grainWave", "Sine"),
            resourceParameter("grainEnvelope", "Hann"),
            parameter("grainDur", "0.01 s", talas::expr::ValueDomain::Seconds),
            parameter("cyclesPerGrain", "1", talas::expr::ValueDomain::Scalar),
            parameter("seed", "0", talas::expr::ValueDomain::Scalar),
            choiceParameter("interpolation", "linear", {"linear", "none"}),
            // The grains start on the Trigger; projects before
            // format 59 laid them out regularly by GrainsPerTrack.
            choiceParameter("grainStarts", "trigger", {"trigger", "legacyRegular"}),
        },
    });

    add(registry, {
        "talas.cloudBankElement",
        "Cloud Bank Element",
        "Xtra Sources",
        {
            input("trigger", PortType::Control),
            input("pan", PortType::Control),
            input("freqMod", PortType::Control),
            input("grainDurMod", PortType::Control),
            // A Spectrum, or a pair as a CloudBank's.
            optionalInput("spectrumSource", PortType::Spectrum),
            input("cascadeInput", PortType::Audio),
            optionalInput("amplitudeIn", PortType::Audio),
            optionalInput("frequencyIn", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            // The project's CloudBankElement example (c112) from the first
            // partial.
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            parameter("count", "2", talas::expr::ValueDomain::Scalar),
            resourceParameter("grainWave", "Sine"),
            resourceParameter("grainEnvelope", "Hann"),
            parameter("grainDur", "0.01 s", talas::expr::ValueDomain::Seconds),
            parameter("cyclesPerGrain", "1", talas::expr::ValueDomain::Scalar),
            parameter("maxGrains", "4", talas::expr::ValueDomain::Scalar),
            parameter("seed", "0", talas::expr::ValueDomain::Scalar),
            parameter("initialDelay", "0 s", talas::expr::ValueDomain::Seconds),
            choiceParameter("interpolation", "linear", {"linear", "none"}),
            choiceParameter("grainStarts", "trigger", {"trigger", "legacyRegular"}),
        },
    });

}

static void registerBuiltinNodeDescriptorsPart2(NodeRegistry& registry)
{
    add(registry, {
        "talas.filterBank",
        "Filter Bank",
        "Aggregate Synthesis",
        {
            input("spectrum", PortType::Spectrum),
            input("audioIn", PortType::Audio),
            optionalInput("bandwidth", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("nbrFilters", "8", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "8", talas::expr::ValueDomain::Scalar), 8.0),
            parameter("bandwidth", "1.0", talas::expr::ValueDomain::Scalar),
            // Bandwidth is a 0 to 1 control on each filter's spacing
            // from its neighbours; projects before format 58 gave
            // hertz.
            choiceParameter("bandwidthUnits", "spacing", {"spacing", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.filterBankElement",
        "Filter Bank Element",
        "Xtra Sources",
        {
            input("cascadeInput", PortType::Audio),
            input("audioIn", PortType::Audio),
            input("spectrum", PortType::Spectrum),
            optionalInput("bandwidth", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            parameter("count", "8", talas::expr::ValueDomain::Scalar),
            parameter("bandwidth", "1.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("bandwidthUnits", "spacing", {"spacing", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.formantBank",
        "Formant Bank",
        "Aggregate Synthesis",
        {
            optionalInput("spectrum", PortType::Spectrum),
            optionalInput("frequency", PortType::Control),
            optionalInput("amplitudeIn", PortType::Audio),
            optionalInput("frequencyIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("nbrFormants", "4", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "4", talas::expr::ValueDomain::Scalar), 4.0),
            resourceParameter("impulseResponse", "LinearEnvelope"),
            parameter("nbrImpulses", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("allowDC", false),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.formantBankOscillator",
        "Formant Bank Oscillator",
        "Xtra Sources",
        {
            optionalInput("spectrum", PortType::Spectrum),
            input("cascadeInput", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("amplitudeIn", PortType::Audio),
            optionalInput("frequencyIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("firstFormant", "1", talas::expr::ValueDomain::Scalar),
            parameter("nbrFormants", "4", talas::expr::ValueDomain::Scalar),
            resourceParameter("formantImpulse", "LinearEnvelope"),
            parameter("nbrImpulses", "4", talas::expr::ValueDomain::Scalar),
            booleanParameter("allowFormantAtDC", false),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.gaOscillators",
        "GA Oscillators",
        "Xtra Sources",
        {
            optionalInput("envelope", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("morph", PortType::Control),
            // A Sound for TimeIndex, as the field expects, e.g. a
            // FunctionGenerator's FullRamp.
            optionalInput("timeIndex", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            // -1 the analysis's envelopes' beginning, 0 their middle, 1
            // their end.
            parameter("timeIndex", "0.0", talas::expr::ValueDomain::Scalar),
            // A computed analysis of eight harmonics plays when none is
            // named (owner, 2026-10-12).
            resourceParameter("analysis0", "HarmonicAmplitudes"),
            resourceParameter("analysis1", {}),
            parameter("envelope", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("morph", "0.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.genericSource",
        "Generic Source",
        "Xtra Sources",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            textParameter("source", {}),
            booleanParameter("leftChannel", true),
            booleanParameter("rightChannel", true),
            resourceParameter("sample", {}),
            booleanParameter("autoloop", false),
            parameter("trigger", "1", talas::expr::ValueDomain::Scalar),
            parameter("attackTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            // Middle C plays a recording without a recorded pitch at its
            // own rate; Default is the recorded pitch.
            defaultKeywordParameter(
                parameter("frequency", "4 c", talas::expr::ValueDomain::Hertz),
                261.6255653005986),
        },
    });

    add(registry, {
        "talas.grainCloud",
        "Grain Cloud",
        "Xtra Sources",
        {
            optionalInput("amplitude", PortType::Control),
            optionalInput("pan", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("grainDur", PortType::Control),
            optionalInput("density", PortType::Control),
            optionalInput("maxGrains", PortType::Control),
            optionalInput("panJitter", PortType::Control),
            optionalInput("grainDurJitter", PortType::Control),
            optionalInput("freqJitter", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            resourceParameter("waveform", "Sine"),
            resourceParameter("grainEnv", "Hann"),
            parameter("maxGrains", "8", talas::expr::ValueDomain::Scalar),
            parameter("amplitude", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("density", "20", talas::expr::ValueDomain::Scalar),
            parameter("grainDur", "0.1 s", talas::expr::ValueDomain::Seconds),
            parameter("grainDurJitter", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("cyclesPerGrain", "20", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "200 hz", talas::expr::ValueDomain::Hertz),
            parameter("freqJitter", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("pan", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("panJitter", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("seed", "0.5", talas::expr::ValueDomain::Scalar),
            // A grain lasts GrainDur plus CyclesPerGrain cycles of its own
            // frequency; projects before format 50 used GrainDur,
            // or the cycles when GrainDur was 0.
            choiceParameter("grainDuration", "durPlusCycles",
                {"durPlusCycles", "legacyDurOrCycles"}),
            // A grain keeps the frequency, duration and pan it starts with
            //; projects before format 74 followed them per frame.
            choiceParameter("grainShapes", "atGrainStart",
                {"atGrainStart", "legacyEveryFrame"}),
        },
    });

    add(registry, {
        "talas.oscillatorBank",
        "Oscillator Bank",
        "Aggregate Synthesis",
        {
            optionalInput("spectrum", PortType::Spectrum),
            optionalInput("amplitudeIn", PortType::Audio),
            optionalInput("frequencyIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("nbrOscillators", "8", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "8", talas::expr::ValueDomain::Scalar), 8.0),
            resourceParameter("wavetable", "Sine"),
            // An Audio pair's frequencies are fractions of half the sample
            // rate; projects before format 93 carry hertz.
            choiceParameter("pairFrequencies", "halfSampleRate", {"halfSampleRate", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.pulseGenerator",
        "Pulse Generator",
        "Xtra Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("modulator", PortType::Audio),
            optionalInput("envelope", PortType::Control),
            optionalInput("dutyCycle", PortType::Control),
            optionalInput("reset", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            textParameter("modulation", {}),
            parameter("maxMI", "0.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("interpolation", "linear", {"linear", "none"}),
            parameter("envelope", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.pulseTrain",
        "Pulse Train",
        "Xtra Sources",
        {
            optionalInput("period", PortType::Control),
            optionalInput("dutyCycle", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("period", "0.5 s", talas::expr::ValueDomain::Seconds),
            booleanParameter("variableDutyCycle", false),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.quadOscillator",
        "Quad Oscillator",
        "Xtra Sources",
        {
            input("envelope", PortType::Audio),
            optionalInput("rightEnvelope", PortType::Audio),
            optionalInput("frequency", PortType::Control),
        },
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz)},
    });

    add(registry, {
        "talas.quadFunctionGenerator",
        "Quad Function Generator",
        "Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("phaseOffset", PortType::Control),
            optionalInput("skew", PortType::Control),
            optionalInput("curve", PortType::Control),
            optionalInput("smooth", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {
            output("aOut", PortType::Audio),
            output("bOut", PortType::Audio),
            output("cOut", PortType::Audio),
            output("dOut", PortType::Audio),
        },
        {
            parameter("frequency", "1 hz", talas::expr::ValueDomain::Hertz),
            parameter("phaseOffset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("skew", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("curve", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("smooth", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.dualMorphOscillator",
        "Dual Morph Oscillator",
        "Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("ratio", PortType::Control),
            optionalInput("phaseMod", PortType::Control),
            optionalInput("morph", PortType::Control),
            optionalInput("fold", PortType::Control),
            optionalInput("amplitude", PortType::Control),
        },
        {
            output("audioOut", PortType::Audio),
            output("modulatorOut", PortType::Audio),
        },
        {
            parameter("frequency", "220 hz", talas::expr::ValueDomain::Hertz),
            parameter("ratio", "2.0", talas::expr::ValueDomain::Scalar),
            parameter("phaseMod", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("morph", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("fold", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.probabilityChaosGenerator",
        "Probability Chaos Generator",
        "Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("probability", PortType::Control),
            optionalInput("chaos", PortType::Control),
            optionalInput("slew", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {
            output("valueOut", PortType::Audio),
            output("gateOut", PortType::Audio),
        },
        {
            parameter("frequency", "4 hz", talas::expr::ValueDomain::Hertz),
            parameter("probability", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("chaos", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("slew", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.sosOscillators",
        "SOS Oscillators",
        "Xtra Sources",
        {
            input("spectrum", PortType::Spectrum),
            input("cascadeInput", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            // The project's SOSOscillators example (c102), from the
            // fundamental.
            parameter("nbrOscillators", "2", talas::expr::ValueDomain::Scalar),
            parameter("firstOscillator", "1", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "Sine"),
        },
    });

    add(registry, {
        "talas.sumOfSines",
        "Sum Of Sines",
        "Xtra Sources",
        {
            optionalInput("onDuration", PortType::Control),
            optionalInput("frequency0", PortType::Control),
            optionalInput("frequency1", PortType::Control),
            optionalInput("dbMorph", PortType::Control),
            optionalInput("pchMorph", PortType::Control),
            optionalInput("envelope", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("nbrPartials", PortType::Control),
            optionalInput("timeIndex", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("onDuration", "2 s", talas::expr::ValueDomain::Seconds),
            parameter("frequency0", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("frequency1", "0 hz", talas::expr::ValueDomain::Hertz),
            // A computed analysis of eight harmonics plays when none is
            // named (owner, 2026-10-12).
            resourceParameter("analysis0", "HarmonicAmplitudes"),
            parameter("analysisFrequency0", "1 hz", talas::expr::ValueDomain::Hertz),
            parameter("analysisFrames0", "1", talas::expr::ValueDomain::Scalar),
            resourceParameter("analysis1", {}),
            parameter("analysisFrequency1", "1 hz", talas::expr::ValueDomain::Hertz),
            parameter("analysisFrames1", "1", talas::expr::ValueDomain::Scalar),
            parameter("dbMorph", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("pchMorph", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("nbrPartials", "8", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "8", talas::expr::ValueDomain::Scalar), 8.0),
            parameter("timeIndex", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("loop", false),
            parameter("loopStart", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("loopEnd", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("ctrlTime", false),
            parameter("envelope", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });
    registry.addNodeTypeLimitation(
        "talas.sumOfSines",
        "prepared-realtime-bounded-analysis-frame-playback");

    add(registry, {
        "talas.twoFormantVoiceElement",
        "Two Formant Voice Element",
        "Xtra Sources",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("formant1", PortType::Control),
            optionalInput("bandwidth1", PortType::Control),
            optionalInput("scale1", PortType::Control),
            optionalInput("formant2", PortType::Control),
            optionalInput("bandwidth2", PortType::Control),
            optionalInput("scale2", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("formant1", "238 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth1", "70 hz", talas::expr::ValueDomain::Hertz),
            parameter("scale1", "0.3", talas::expr::ValueDomain::Scalar),
            parameter("formant2", "1741 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth2", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("scale2", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
            // A glottal pulse with vibrato; projects before format
            // 60 keep the sine they were heard with.
            choiceParameter("excitation", "glottalPulse", {"glottalPulse", "legacySine"}),
        },
    });

    add(registry, {
        "talas.spectrumFrequencyScale",
        "Spectrum Frequency Scale",
        "Spectral Modifiers",
        {
            input("spectrumIn", PortType::Spectrum),
            optionalInput("scale", PortType::Control),
        },
        {output("spectrumOut", PortType::Spectrum)},
        {
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("formantsOnly", false),
            // The pitch moves and the formants stay; projects before
            // format 92 kept each partial's amplitude, moving them.
            choiceParameter("formants", "stay", {"stay", "legacyMove"}),
        },
    });

    add(registry, {
        "talas.spectrumFundamental",
        "Spectrum Fundamental",
        "Spectral Modifiers",
        {
            optionalInput("spectrumIn", PortType::Spectrum),
            partialSpectrumPort("partialSpectrumIn", false,
                SpectralFrameFormat::PartialSpectrumV1),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.spectrumLinearToLog",
        "Spectrum Linear To Log",
        "Spectral Modifiers",
        {input("spectrumIn", PortType::Spectrum)},
        {output("spectrumOut", PortType::Spectrum)},
        {},
    });

    add(registry, {
        "talas.spectrumLogToLinear",
        "Spectrum Log To Linear",
        "Spectral Modifiers",
        {input("spectrumIn", PortType::Spectrum)},
        {output("spectrumOut", PortType::Spectrum)},
        {},
    });

    add(registry, {
        "talas.spectrumModifier",
        "Spectrum Modifier",
        "Spectral Modifiers",
        {
            input("spectrumIn", PortType::Spectrum),
            optionalInput("select", PortType::Control),
            optionalInput("loTrack", PortType::Control),
            optionalInput("hiTrack", PortType::Control),
            optionalInput("loFreq", PortType::Control),
            optionalInput("hiFreq", PortType::Control),
            optionalInput("freqHysteresis", PortType::Control),
            optionalInput("freqScale", PortType::Control),
            optionalInput("freqOffset", PortType::Control),
            optionalInput("loAmp", PortType::Control),
            optionalInput("hiAmp", PortType::Control),
            optionalInput("ampHysteresis", PortType::Control),
            optionalInput("ampScale", PortType::Control),
            optionalInput("ampOffset", PortType::Control),
            optionalInput("probability", PortType::Control),
            optionalInput("seed", PortType::Control),
            optionalInput("hearAll", PortType::Control),
            optionalInput("freqSmoothing", PortType::Control),
            optionalInput("ampSmoothing", PortType::Control),
        },
        {output("spectrumOut", PortType::Spectrum)},
        {
            booleanParameter("select", true),
            parameter("loTrack", "1", talas::expr::ValueDomain::Scalar),
            parameter("hiTrack", "10000", talas::expr::ValueDomain::Scalar),
            parameter("loFreq", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("hiFreq", "20000 hz", talas::expr::ValueDomain::Hertz),
            parameter("freqHysteresis", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("freqScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("freqOffset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("loAmp", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("hiAmp", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("ampHysteresis", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("ampScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("ampOffset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("probability", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("seed", "0.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("hearAll", false),
            parameter("freqSmoothing", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("ampSmoothing", "0.0", talas::expr::ValueDomain::Scalar),
            // A FreqOffset without units adds a fraction of half the sample
            // rate; projects before format 61 added hertz.
            choiceParameter("freqOffsetUnits", "fractionOfNyquist",
                {"fractionOfNyquist", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.spectrumTrackSelector",
        "Spectrum Track Selector",
        "Spectral Modifiers",
        {
            optionalInput("spectrumIn", PortType::Spectrum),
            partialSpectrumPort("partialSpectrumIn", false,
                SpectralFrameFormat::PartialSpectrumV1),
            optionalInput("track", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("track", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
            // The frequency is a fraction of half the sample rate, as in a
            // Spectrum; projects before format 53 read hz.
            choiceParameter("freqUnits", "fractionOfNyquist",
                {"fractionOfNyquist", "legacyHertz"}),
        },
    });

    add(registry, {
        "talas.partialSpectrumTrackSelectorV1",
        "Partial Spectrum Track Selector v1",
        "Spectral Utilities",
        {
            partialSpectrumPort("spectrumIn", true,
                SpectralFrameFormat::PartialSpectrumV1),
            optionalInput("track", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("track", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        },
        NodeSupportLevel::Utility,
        "Talas typed spectral utility.",
    });

    add(registry, {
        "talas.spectrumVoicedUnvoiced",
        "Spectrum Voiced Unvoiced",
        "Spectral Modifiers",
        {
            optionalInput("spectrumIn", PortType::Spectrum),
            partialSpectrumPort("partialSpectrumIn", false,
                SpectralFrameFormat::PartialSpectrumV1),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.frequencyScale",
        "Frequency Scale",
        "Frequency & Time Scaling",
        {
            input("audioIn", PortType::Audio),
            input("freqTracker", PortType::Control),
            optionalInput("frequencyScale", PortType::Control),
            optionalInput("delay", PortType::Control),
            optionalInput("maxScale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequencyScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("maxScale", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("window", {}),
            parameter("delay", "0", talas::expr::ValueDomain::Scalar),
            // FreqTracker is a FrequencyTracker's fraction of half the
            // sample rate; projects before format 47 gave hz.
            choiceParameter("trackerUnits", "fractionOfNyquist",
                {"fractionOfNyquist", "legacyHertz"}),
            // The grains of the Input keep its formants where they are;
            // projects before format 52 moved them with the pitch.
            choiceParameter("formants", "stay", {"stay", "legacyMove"}),
            // The Input is granulated by the period its FreqTracker gives;
            // projects before format 83 shifted it with a phase vocoder.
            choiceParameter("scaling", "grains", {"grains", "legacyVocoder"}),
            // Delay is a setup value read once, at the start;
            // projects before format 85 followed one that changes offline.
            choiceParameter("delayRead", "atStart", {"atStart", "legacyFollows"}),
        },
    });

    add(registry, {
        "talas.monotonizer",
        "Monotonizer",
        "Frequency & Time Scaling",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("minInputPitch", PortType::Control),
            optionalInput("maxInputPitch", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("minInputPitch", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("maxInputPitch", "440 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    add(registry, {
        "talas.simplePitchShifter",
        "Simple Pitch Shifter",
        "Frequency & Time Scaling",
        {
            input("audioIn", PortType::Audio),
            optionalInput("interval", PortType::Control),
            optionalInput("minInputPitch", PortType::Control),
            optionalInput("maxInputPitch", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("interval", "0", talas::expr::ValueDomain::Scalar),
            parameter("minInputPitch", "20 hz", talas::expr::ValueDomain::Hertz),
            parameter("maxInputPitch", "2000 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    add(registry, {
        "talas.singleSideBandRm",
        "Single Side Band RM",
        "Frequency & Time Scaling",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("freqScale", PortType::Control),
            optionalInput("rightIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("freqScale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.timeFrequencyScale",
        "Time Frequency Scale",
        "Frequency & Time Scaling",
        {
            optionalInput("frequencyScale", PortType::Control),
            optionalInput("rate", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("minInputFreq", PortType::Control),
            optionalInput("maxInputFreq", PortType::Control),
            optionalInput("maxFreqScale", PortType::Control),
            optionalInput("detectors", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequencyScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("minInputFreq", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("maxInputFreq", "2000 hz", talas::expr::ValueDomain::Hertz),
            parameter("maxFreqScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("detectors", "10", talas::expr::ValueDomain::Scalar),
            booleanParameter("fromDisk", false),
            resourceParameter("sample", {}),
            booleanParameter("fromMemoryWriter", false),
            // A Rate above 1 is refused, a live one saturating at 1, since the
            // Sound only stretches time; projects before format 92
            // compress time at such a Rate.
            choiceParameter("rateAboveOne", "refused", {"refused", "legacyCompresses"}),
        },
    });

    add(registry, {
        "talas.diffuseSpectralStretch",
        "Diffuse Spectral Stretch",
        "Frequency & Time Scaling",
        {
            optionalInput("reset", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("level", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("level", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("stretch", "8.0", talas::expr::ValueDomain::Scalar),
            parameter("smear", "0.75", talas::expr::ValueDomain::Scalar),
            parameter("diffusion", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
            resourceParameter("sample", {}),
        },
    });

    add(registry, {
        "talas.contextFreeGrammar",
        "Context Free Grammar",
        "Scripts",
        // The Inputs, s1 to sn in the order
        // they are connected, are the terminals of the grammar, whose
        // result compiling builds from copies of them (ADR-0015 amendment).
        {[] {
            auto inputs = optionalInput("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            textParameter("rewriteRules", {}),
            textParameter("startExpression", {}),
            parameter("seed", "0", talas::expr::ValueDomain::Scalar),
            parameter("maxRewrites", "0", talas::expr::ValueDomain::Scalar),
            parameter("maxSize", "64", talas::expr::ValueDomain::Scalar),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // Prototypes that each become, as compiling begins, what they play
    // (ADR-0015 amendment, 2026-10-19).
    // A Script whose script reads a text file
    // through `file`.
    add(registry, {
        "talas.textFileInterpreter",
        "Text File Interpreter",
        "Scripts",
        {[] {
            auto inputs = optionalInput("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            textParameter("script", {}),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // A Script whose script reads a MIDI file an
    // event at a time through `file`.
    add(registry, {
        "talas.midiFileInterpreter",
        "MIDI File Interpreter",
        "Scripts",
        {[] {
            auto inputs = optionalInput("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            textParameter("script", {}),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // Generations of an elementary cellular automaton,
    // each a chord of the Inputs whose cells are on.
    add(registry, {
        "talas.cellularAutomaton",
        "Cellular Automaton",
        "Scripts",
        {[] {
            auto inputs = optionalInput("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // An 8-bit rule, 0 to 255, as Wolfram numbers them.
            parameter("rule", "30", talas::expr::ValueDomain::Scalar),
            parameter("iterations", "8", talas::expr::ValueDomain::Scalar),
            // A binary number of a digit an Input, the first Input's the
            // leftmost, e.g. 2r1001; leading zeros may be left out.
            textParameter("initialState", "1"),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // An elementary cellular automaton's
    // generations read as rhythms of the Input, a step its duration.
    add(registry, {
        "talas.rhythmicCellularAutomaton",
        "Rhythmic Cellular Automaton",
        "Scripts",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("rule", "30", talas::expr::ValueDomain::Scalar),
            parameter("generations", "4", talas::expr::ValueDomain::Scalar),
            textParameter("initialState", "1"),
            parameter("generationLength", "8", talas::expr::ValueDomain::Scalar),
        },
    });

    // Inputs chosen at random by their weights, at
    // exponentially distributed delays.
    add(registry, {
        "talas.randomSelection",
        "Random Selection",
        "Scripts",
        {[] {
            auto inputs = optionalInput("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // A weight an Input, in order.
            textParameter("weights", "1"),
            parameter("averageDelay", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("iterations", "8", talas::expr::ValueDomain::Scalar),
            parameter("delaySeed", "35409", talas::expr::ValueDomain::Scalar),
            parameter("sampleSeed", "35425", talas::expr::ValueDomain::Scalar),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // The Input, its Event Values starting at saved values.
    add(registry, {
        "talas.preset",
        "Preset",
        "MIDI In",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Entries `!Name channel: n; initialValue: v.`.
            textParameter("eventValues", {}),
        },
    });

    // The Input's left and right channels times Left
    // and Right, from -1 to 1. A stereo Input connects its left channel to
    // audioIn and its right to rightIn; a mono Input plays on both.
    add(registry, {
        "talas.attenuator",
        "Attenuator",
        "Level, Compression, Expansion",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // As an Attenuator, but Left and Right take any value, to
    // boost the Input above 1.
    add(registry, {
        "talas.gain",
        "Gain",
        "Level, Compression, Expansion",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            // Turned up too loud it clips at full scale, as signals do;
            // projects before format 148 boosted past 1 unclipped.
            choiceParameter("overflow", "clip", {"clip", "unclipped"}),
        },
    });

    // A Mixer whose Inputs' duration midpoints line up with
    // the longest Input's. A stereo Input connects its left channel to
    // inputs and its right to rightInputs.
    add(registry, {
        "talas.centeringMixer",
        "CenteringMixer",
        "Mixing & Panning",
        {
            [] {
                auto inputs = input("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            [] {
                auto rightInputs = optionalInput("rightInputs", PortType::Audio);
                rightInputs.acceptsMany = true;
                return rightInputs;
            }(),
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // The Inputs one after another, each starting when the one
    // before it ends. Every Concatenation has the parameters Name, Inputs,
    // Retrograde and Reverse. Retrograde plays the
    // Inputs last first; Reverse plays the whole Concatenation backwards,
    // the Inputs last first and each backwards where it can be (a TimeIndex
    // runs back, a TimeOffset puts its silence after); both together keep
    // the order and play each Input backwards. Both start unchecked, so a
    // saved Concatenation plays as before.
    add(registry, {
        "talas.concatenation",
        "Concatenation",
        "Time & Duration",
        {[] {
            auto inputs = input("inputs", PortType::Audio);
            inputs.acceptsMany = true;
            return inputs;
        }()},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            booleanParameter("retrograde", false),
            booleanParameter("reverse", false),
        },
    });

    // A Mixer whose shorter Inputs start late enough to end
    // with the longest.
    add(registry, {
        "talas.endTogetherMixer",
        "EndTogetherMixer",
        "Mixing & Panning",
        {
            [] {
                auto inputs = input("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            [] {
                auto rightInputs = optionalInput("rightInputs", PortType::Audio);
                rightInputs.acceptsMany = true;
                return rightInputs;
            }(),
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.limeInterpreter",
        "Lime Interpreter",
        "Scripts",
        {
            // The Sounds its Script schedules by name, each a template
            //.
            [] {
                auto inputs = optionalInput("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            resourceParameter("fileName", {}),
            textParameter("script", {}),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.analogSequencer",
        "Analog Sequencer",
        "Sequencers",
        {
            input("audioIn", PortType::Audio),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("step", PortType::Control),
            optionalInput("rate", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            parameter("step", "0", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("loop", false),
            parameter("startIndex", "0", talas::expr::ValueDomain::Scalar),
            parameter("endIndex", "3", talas::expr::ValueDomain::Scalar),
            parameter("polyphony", "1", talas::expr::ValueDomain::Scalar),
            // Four stages of a quarter second, StartIndex 0 to EndIndex 3,
            // each key down for half of it (the first stage of the
            // project's example c157); the one DutyCycle repeats for as
            // long as the longest sequence. Velocities stay empty,
            // as Key arrays need them, and then play at full amplitude.
            textParameter("durations", "{0.25 s} {0.25 s} {0.25 s} {0.25 s}"),
            textParameter("dutyCycles", "{0.5}"),
            textParameter("pitches", {}),
            textParameter("velocities", {}),
            textParameter("extraValues", {}),
            textParameter("keyDowns", {}),
            textParameter("keyPitches", {}),
            textParameter("keyVelocities", {}),
            textParameter("keyTimbres", {}),
            choiceParameter("indexOrigin", "zeroBased",
                {"zeroBased", "legacyOneBased"}),
            textParameter("generatedIndex", {}),
            choiceParameter("stepZeroMode", "waitForTrigger",
                {"waitForTrigger", "legacyAutomatic"}),
            // Without Pitches or KeyDowns, Step steps through the sequence
            // each time it becomes positive, and a constant 1 leaves the
            // Durations to time it; projects before format 75
            // played the stage Step numbered, or followed the Durations at 0.
            choiceParameter("stepMode", "trigger",
                {"trigger", "legacyStageNumber"}),
            // A DutyCycle of 1 keeps the key down into the next stage, whose
            // note takes over without a new attack; projects before
            // format 76 released and retriggered it.
            choiceParameter("dutyCycleOne", "holds",
                {"holds", "legacyRetriggers"}),
            // A constant field of its Sound is read as each voice starts,
            // with that voice's Event Values (owner, 2026-10-12); projects
            // before format 91 read it once, as the Sound compiled.
            choiceParameter("constantFieldsRead", "eachVoice", {"eachVoice", "legacyWhenCompiled"}),
        },
    });

    add(registry, {
        "talas.euSeq",
        "EuSeq",
        "Sequencers",
        {
            optionalInput("stepRate", PortType::Control),
            optionalInput("reset", PortType::Control),
        },
        {
            output("gateOut", PortType::Control),
            output("frequencyOut", PortType::Control),
            output("stepOut", PortType::Control),
        },
        {
            parameter("steps", "8", talas::expr::ValueDomain::Scalar),
            parameter("pulses", "3", talas::expr::ValueDomain::Scalar),
            parameter("rotation", "0", talas::expr::ValueDomain::Scalar),
            parameter("scaleSteps", "5", talas::expr::ValueDomain::Scalar),
            parameter("stepsPerOctave", "12", talas::expr::ValueDomain::Scalar),
            parameter("baseFrequency", "220 hz", talas::expr::ValueDomain::Hertz),
            parameter("stepRate", "4 hz", talas::expr::ValueDomain::Hertz),
            parameter("reset", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.parameterTransformer",
        "Parameter Transformer",
        "Scripts",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {textParameter("transformation", {})},
    });

    add(registry, {
        "talas.script",
        "Script",
        "Scripts",
        {
            optionalInput("audioIn", PortType::Audio),
            // The Sounds the script schedules by name, each a
            // template its events copy.
            [] {
                auto inputs = optionalInput("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            textParameter("script", {}),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            // An event's value follows its keyword's
            // colon after a space, freq: 500 hz; projects before format 155
            // also took one touching the colon, freq:500 hz.
            choiceParameter("keywordSpacing", "spaceAfterColon", {"spaceAfterColon", "legacyTouching"}),
        },
    });

    add(registry, {
        "talas.constant",
        "Constant",
        "Sources",
        {},
        {{
            "value",
            PortType::Control,
            false,
            {
                PortSignalRate::Sample,
                AudioBusLayoutKind::NotApplicable,
                PortEdgeSemantics::Continuous,
                SpectralFrameFormat::NotApplicable,
                true,
            },
        }},
        {
            parameter("value", "1.0", talas::expr::ValueDomain::Scalar),
            // A Value from -1 to 1, the
            // signal range. unitRange refuses a static Value past them and
            // saturates the signal at them; unbounded, the default, keeps
            // any number, a control value in its field's units.
            choiceParameter("valueRange", "unbounded", {"unbounded", "unitRange"}),
        },
    });

    add(registry, {
        "talas.valToSound",
        "Val To Sound",
        "Sources",
        {},
        {output("audioOut", PortType::Audio)},
        {parameter("value", "0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.functionGenerator",
        "Function Generator",
        "Envelopes & Control Signals",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("onDuration", PortType::Control),
            optionalInput("level", PortType::Audio),
        },
        {output("value", PortType::Control)},
        {
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("onDuration", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "Hann"),
            booleanParameter("fromMemoryWriter", false),
        },
    });

    add(registry, {
        "talas.audioInput",
        "Audio Input",
        "Sampling",
        {},
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            choiceParameter("routing", "monoSum", {"monoSum", "stereoPair"}),
            booleanParameter("channel1", true),
            booleanParameter("channel2", false),
            booleanParameter("channel3", false),
            booleanParameter("channel4", false),
            booleanParameter("channel5", false),
            booleanParameter("channel6", false),
            booleanParameter("channel7", false),
            booleanParameter("channel8", false),
        },
    });

    add(registry, {
        "talas.amplitudeFollower",
        "Amplitude Follower",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("timeConstant", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("timeConstant", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.biDiFollower",
        "BiDi Follower",
        "Tracking Live Input",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("attackTime", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "100 ms", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.lossyIntegrator",
        "Lossy Integrator",
        "Tracking Live Input",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("releaseTime", "1 s", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.hysteresis",
        "Hysteresis",
        "Tracking Live Input",
        {
            optionalInput("value", PortType::Control),
            optionalInput("audioIn", PortType::Audio),
        },
        {
            output("value", PortType::Control),
            output("audioOut", PortType::Audio),
        },
        {parameter("width", "0.25", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.analysisFilter",
        "Analysis Filter",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("fundamental", PortType::Control),
            optionalInput("harmonic", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("fundamental", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("harmonic", "1", talas::expr::ValueDomain::Scalar),
            // One period of the Fundamental isolates each harmonic, and the
            // filters at every harmonic add up to the Input; projects
            // before format 54 used a bandpass and an allpass.
            choiceParameter("design", "harmonicWindow",
                {"harmonicWindow", "legacyBandpass"}),
        },
    });

    add(registry, {
        "talas.adsr",
        "ADSR",
        "Envelopes & Control Signals",
        {optionalInput("gate", PortType::Control)},
        {output("value", PortType::Control)},
        {
            parameter("attackTime", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("decayTime", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("sustainLevel", "0.7", talas::expr::ValueDomain::Scalar),
            parameter("releaseTime", "250 ms", talas::expr::ValueDomain::Seconds),
            choiceParameter("type", "linear", {"linear", "exponential"}),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("legato", "0.0", talas::expr::ValueDomain::Scalar),
            // A Gate of -1 makes the envelope decay
            // rapidly to zero; projects before format 88 release over
            // ReleaseTime.
            choiceParameter("negativeGate", "decaysRapidly", {"decaysRapidly", "legacyReleases"}),
            // A Gate of 1 in a Sound of finite
            // duration ends the release with the Sound, its sustain the
            // duration less the attack, decay and release; projects before
            // format 151 hold the Gate to the end.
            choiceParameter("constantGate", "endsWithTheSound", {"endsWithTheSound", "legacyHolds"}),
        },
    });

    add(registry, {
        "talas.ar",
        "AR",
        "Envelopes & Control Signals",
        {optionalInput("gate", PortType::Control)},
        {output("value", PortType::Control)},
        {
            parameter("attackTime", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "250 ms", talas::expr::ValueDomain::Seconds),
            choiceParameter("type", "linear", {"linear", "exponential"}),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            // A Gate of -1 makes the envelope decay
            // rapidly to zero; projects before format 88 release over
            // ReleaseTime.
            choiceParameter("negativeGate", "decaysRapidly", {"decaysRapidly", "legacyReleases"}),
        },
    });

    add(registry, {
        "talas.chopper",
        "Chopper",
        "Envelopes & Control Signals",
        {
            input("audioIn", PortType::Audio),
            optionalInput("grainDuration", PortType::Control),
            optionalInput("interGrainDelay", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("grainDuration", "0.1 s", talas::expr::ValueDomain::Seconds),
            parameter("interGrainDelay", "0.1 s", talas::expr::ValueDomain::Seconds),
            resourceParameter("wavetable", "Hann"),
            // The Chopper there has a DutyCycle,
            // the part of each beat its gate is on. rateAndDutyCycle times
            // the grains by Rate beats a second, each grain lasting
            // DutyCycle of its beat; grainDurationAndDelay, the default,
            // keeps GrainDuration and InterGrainDelay, which projects have
            // played.
            choiceParameter("timing", "grainDurationAndDelay", {"grainDurationAndDelay", "rateAndDutyCycle"}),
            parameter("rate", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.ramp",
        "Ramp",
        "Control",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("duration", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("duration", "1 s", talas::expr::ValueDomain::Seconds),
            choiceParameter("variant", "ramp", {"ramp", "fullRamp", "repeatingRamp", "repeatingFullRamp"}),
        },
    });

    add(registry, {
        "talas.linearRamp",
        "Linear Ramp",
        "Control",
        {
            optionalInput("duration", PortType::Control),
            optionalInput("start", PortType::Control),
            optionalInput("end", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("duration", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("start", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("end", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

}

static void registerBuiltinNodeDescriptorsPart3(NodeRegistry& registry)
{
    add(registry, {
        "talas.lfo",
        "LFO",
        "Control",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("minimum", PortType::Control),
            optionalInput("maximum", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("frequency", "1 hz", talas::expr::ValueDomain::Hertz),
            parameter("minimum", "-1.0", talas::expr::ValueDomain::Scalar),
            parameter("maximum", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.clock",
        "Clock",
        "Control",
        {},
        {output("gate", PortType::Control)},
        {
            parameter("frequency", "2 hz", talas::expr::ValueDomain::Hertz),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
            booleanParameter("enabled", true),
        },
    });

    add(registry, {
        "talas.clockDivider",
        "Clock Divider",
        "Control",
        {
            input("clock", PortType::Control),
            optionalInput("reset", PortType::Control),
        },
        {
            output("step", PortType::Control),
            output("gate", PortType::Control),
        },
        {
            parameter("division", "2", talas::expr::ValueDomain::Scalar),
            parameter("steps", "4", talas::expr::ValueDomain::Scalar),
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.quantizer",
        "Step Quantizer",
        "Control",
        {input("value", PortType::Control)},
        {output("value", PortType::Control)},
        {
            parameter("stepSize", "55", talas::expr::ValueDomain::Scalar),
            parameter("offset", "220", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.gate",
        "Gate",
        "Control",
        {input("value", PortType::Control)},
        {output("gate", PortType::Control)},
        {
            parameter("lowThreshold", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("highThreshold", "0.75", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.logic",
        "Logic",
        "Control",
        {
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {output("gate", PortType::Control)},
        {
            choiceParameter("operation", "and", {"and", "or", "xor", "not"}),
            parameter("left", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.slew",
        "Slew",
        "Control",
        {input("value", PortType::Control)},
        {output("value", PortType::Control)},
        {
            parameter("riseTime", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("fallTime", "10 ms", talas::expr::ValueDomain::Seconds),
        },
    });

    add(registry, {
        "talas.frequencyTracker",
        "Frequency Tracker",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("minFrequency", PortType::Control),
            optionalInput("maxFrequency", PortType::Control),
            optionalInput("confidence", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        // Its left channel the frequency, its right the amplitude envelope of
        // its Input.
        {output("value", PortType::Control), output("rightOut", PortType::Control)},
        {
            parameter("minFrequency", "20 hz", talas::expr::ValueDomain::Hertz),
            parameter("maxFrequency", "2000 hz", talas::expr::ValueDomain::Hertz),
            parameter("confidence", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("detectors", "10", talas::expr::ValueDomain::Scalar),
            parameter("emphasis", "0.0", talas::expr::ValueDomain::Scalar),
            // The estimate is a fraction of half the sample rate, from 0 to 1
            //; projects before format 47 read hz.
            choiceParameter("outputUnits", "fractionOfNyquist",
                {"fractionOfNyquist", "legacyHertz"}),
            // Scale attenuates the Input; projects before format 49
            // scaled the estimate.
            choiceParameter("scaleAttenuates", "input",
                {"input", "legacyEstimate"}),
        },
    });

    add(registry, {
        "talas.graphicalEnvelope",
        "Graphical Envelope",
        "Envelopes & Control Signals",
        {
            optionalInput("level", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("rate", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            textParameter("envelope", "{0 s} {0} {0.1 s} {1} {1 s} {0}"),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
            // A Gate of -1 makes the envelope decay
            // rapidly to zero; projects before format 88 play on.
            choiceParameter("negativeGate", "decaysRapidly", {"decaysRapidly", "legacyReleases"}),
        },
    });

    add(registry, {
        "talas.liveSpectralAnalysis",
        "Live Spectral Analysis",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("ampScale", PortType::Control),
            optionalInput("freqScale", PortType::Control),
        },
        {partialSpectrumPort(
            "spectrumOut",
            false,
            SpectralFrameFormat::PartialSpectrumV1)},
        {
            parameter("lowestAnalyzedFreq", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("ampScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("freqScale", "1.0", talas::expr::ValueDomain::Scalar),
            textParameter("response", {}),
            booleanParameter("harmonic", false),
            parameter("unpitchedOnly", "0", talas::expr::ValueDomain::Scalar),
            parameter("unpitchedThreshold", "0", talas::expr::ValueDomain::Scalar),
            parameter("trackedHarmonic", "1", talas::expr::ValueDomain::Scalar),
            parameter("lowFreq", "50 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFreq", "2000 hz", talas::expr::ValueDomain::Hertz),
            parameter("initFreq", "440 hz", talas::expr::ValueDomain::Hertz),
            booleanParameter("fundamentalOnly", false),
            // The analysis gives one track per band up to half the
            // sample rate; projects before format 57 kept the
            // strongest peaks between LowFreq and HighFreq.
            choiceParameter("tracks", "bands", {"bands", "legacyPeaks"}),
            // LowestAnalyzedFreq 1 F through 5 F gives 512, 256, 128, 64 and
            // 32 bands: the power of two nearest the sample rate over
            // it, halved; projects before format 75 took the next larger
            // power of two and a band fewer.
            choiceParameter("bandWidth", "nearestPower",
                {"nearestPower", "legacyNextPower"}),
        },
    });

    add(registry, {
        "talas.partialSpectrumLegacyHoldV1",
        "Partial Spectrum Legacy Hold v1",
        "Spectral Utilities",
        {
            partialSpectrumPort(
                "spectrumIn",
                true,
                SpectralFrameFormat::PartialSpectrumV1),
        },
        {
            partialSpectrumPort(
                "spectrumOut",
                false,
                SpectralFrameFormat::LegacyPartialTriples),
        },
        {},
        NodeSupportLevel::Utility,
        "Talas utility.",
    });

    // A Sound pasted into another's parameter field (ADR-0020): compiling
    // adds this node, which evaluates the field's expression with the
    // pasted Sounds' channels each millisecond and drives the parameter.
    add(registry, {
        "talas.pastedSoundExpressionV1",
        "Pasted Sound Expression v1",
        "Control Utilities",
        {
            [] {
                auto signals = input("signals", PortType::Audio);
                signals.acceptsMany = true;
                return signals;
            }(),
        },
        {output("value", PortType::Control)},
        {
            textParameter("expression", {}),
            textParameter("references", {}),
            choiceParameter("domain", "scalar", {"scalar", "hertz", "seconds", "noteNumber", "samples"}),
            // The project's choices of how its fields read pasted Sounds
            // (format 156): within -1 and 1 and as
            // each stood just before its tick.
            choiceParameter("range", "unbounded", {"unbounded", "unity"}),
            choiceParameter("reads", "atTheTick", {"atTheTick", "previousMillisecond"}),
            // The field's parts without a Sound that read Event Values or
            // variables or call a function, live like any field; the
            // expression names them __kc_live_1 and on.
            parameter("live1", "0", talas::expr::ValueDomain::Scalar),
            parameter("live2", "0", talas::expr::ValueDomain::Scalar),
            parameter("live3", "0", talas::expr::ValueDomain::Scalar),
            parameter("live4", "0", talas::expr::ValueDomain::Scalar),
            parameter("live5", "0", talas::expr::ValueDomain::Scalar),
            parameter("live6", "0", talas::expr::ValueDomain::Scalar),
            parameter("live7", "0", talas::expr::ValueDomain::Scalar),
            parameter("live8", "0", talas::expr::ValueDomain::Scalar),
        },
        NodeSupportLevel::Utility,
        "Talas utility.",
    });

    add(registry, {
        "talas.multisegmentEnvelope",
        "Multisegment Envelope",
        "Envelopes & Control Signals",
        {
            optionalInput("level", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            textParameter("durations", "{0.1 s} {0.4 s} {0.5 s}"),
            textParameter("breakPoints", "{0} {1} {0.5} {0}"),
            parameter("startLoop", "0", talas::expr::ValueDomain::Scalar),
            parameter("endLoop", "0", talas::expr::ValueDomain::Scalar),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            // The loop repeats while Gate is on; projects
            // before format 73 repeat it until the next trigger.
            choiceParameter("loopRelease", "onGateRelease", {"onGateRelease", "legacyNever"}),
            // A Gate of -1 makes the envelope decay
            // rapidly to zero; projects before format 88 play on.
            choiceParameter("negativeGate", "decaysRapidly", {"decaysRapidly", "legacyReleases"}),
        },
    });

    add(registry, {
        "talas.multislopeFunctionGenerator",
        "Multislope Function Generator",
        "Envelopes & Control Signals",
        {
            optionalInput("level", PortType::Control),
            optionalInput("gate", PortType::Control),
            optionalInput("rate", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            textParameter("timePoints", "{0 s} {0.5 s} {1 s}"),
            textParameter("slopes", "{-1} {1}"),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.peakDetector",
        "Peak Detector",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("attackTime", PortType::Control),
            optionalInput("releaseTime", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("attackTime", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.rmsSquared",
        "RMS Squared",
        "Tracking Live Input",
        {
            input("audioIn", PortType::Audio),
            optionalInput("timeConstant", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("timeConstant", "100 ms", talas::expr::ValueDomain::Seconds),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.soundToGlobalController",
        "Sound To Global Controller",
        "Tracking Live Input",
        {
            optionalInput("value", PortType::Control),
        },
        {},
        {
            textParameter("generatedEvent", {}),
            textParameter("value", {}),
        },
    });

    add(registry, {
        "talas.triggeredSoundToGlobalController",
        "Triggered Sound To Global Controller",
        "Tracking Live Input",
        {
            optionalInput("value", PortType::Control),
            optionalInput("trigger", PortType::Control),
        },
        {},
        {
            textParameter("generatedEvent", {}),
            textParameter("value", {}),
            parameter("trigger", "0", talas::expr::ValueDomain::Scalar),
            booleanParameter("allowLiveOverride", true),
        },
    });

    add(registry, {
        "talas.threshold",
        "Threshold",
        "Tracking Live Input",
        {
            input("value", PortType::Control),
            optionalInput("threshold", PortType::Control),
            optionalInput("hysteresis", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("threshold", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("hysteresis", "0.0", talas::expr::ValueDomain::Scalar),
            // To switch on, the Input exceeds Threshold plus Hysteresis, and
            // to switch off it dips below Threshold minus Hysteresis;
            // projects before format 144 took half the Hysteresis each way.
            choiceParameter("hysteresisBand", "full", {"full", "legacyHalf"}),
        },
    });

    add(registry, {
        "talas.add",
        "Add",
        "Math",
        {
            {
                "left",
                PortType::Control,
                true,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                true,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {{
            "value",
            PortType::Control,
            false,
            {
                PortSignalRate::Sample,
                AudioBusLayoutKind::NotApplicable,
                PortEdgeSemantics::Continuous,
                SpectralFrameFormat::NotApplicable,
                false,
            },
        }},
        {},
    });

    add(registry, {
        "talas.difference",
        "Difference",
        "Math",
        {
            input("left", PortType::Control),
            input("right", PortType::Control),
        },
        {output("value", PortType::Control)},
        {},
    });

    add(registry, {
        "talas.product",
        "Product",
        "Math",
        {
            // All the Inputs multiply together: any number on inputs, and
            // the left and right of projects that gave a Product two.
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
            [] {
                auto inputs = optionalInput("inputs", PortType::Control);
                inputs.acceptsMany = true;
                return inputs;
            }(),
        },
        {output("value", PortType::Control)},
        {},
    });

    add(registry, {
        "talas.arcTan",
        "ArcTan",
        "Math",
        {
            // value and the optional rightValue are the left and right
            // channels of the Input; a mono Input feeds both.
            input("value", PortType::Control),
            optionalInput("rightValue", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            // The four-quadrant arctangent of right over left, as a
            // fraction of a half turn: signals stay within
            // -1 and 1, like a FullRamp's phase. Projects before
            // format 48 took the arctangent of value, in radians.
            choiceParameter("angle", "rightOverLeft",
                {"rightOverLeft", "legacyRadians"}),
        },
    });

    add(registry, {
        "talas.linearToLog",
        "Linear To Log",
        "Math",
        {
            optionalInput("value", PortType::Control),
            optionalInput("audioIn", PortType::Audio),
        },
        {
            output("value", PortType::Control),
            output("audioOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.logToLinear",
        "Log To Linear",
        "Math",
        {
            optionalInput("value", PortType::Control),
            optionalInput("audioIn", PortType::Audio),
        },
        {
            output("value", PortType::Control),
            output("audioOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.equality",
        "Equality",
        "Math",
        {
            input("left", PortType::Control),
            input("right", PortType::Control),
        },
        {output("value", PortType::Control)},
        {parameter("tolerance", "0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.runningMax",
        "Running Max",
        "Math",
        {
            input("value", PortType::Control),
            optionalInput("reset", PortType::Control),
        },
        {output("value", PortType::Control)},
        {parameter("reset", "0.0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.runningMin",
        "Running Min",
        "Math",
        {
            input("value", PortType::Control),
            optionalInput("reset", PortType::Control),
            optionalInput("maximumAmplitude", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("maximumAmplitude", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.phaseShiftBy90",
        "Phase Shift By 90",
        "Math",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("rightIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            // Its filters are very sensitive to Input amplitude:
            // their state saturates at full scale; projects before format
            // 150 leave it unclipped.
            choiceParameter("overflow", "clip", {"clip", "unclipped"}),
        },
    });

    add(registry, {
        "talas.setRange",
        "Set Range",
        "Math",
        {
            input("value", PortType::Control),
            optionalInput("oldMin", PortType::Control),
            optionalInput("oldMax", PortType::Control),
            optionalInput("newMin", PortType::Control),
            optionalInput("newMax", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("oldMin", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("oldMax", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("newMin", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("newMax", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.sqrtMagnitude",
        "Sqrt Magnitude",
        "Math",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {},
    });

    add(registry, {
        "talas.scaleAndOffset",
        "Scale and Offset",
        "Math",
        {
            input("value", PortType::Control),
            optionalInput("rightValue", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("offset", PortType::Control),
            optionalInput("rightScale", PortType::Control),
            optionalInput("rightOffset", PortType::Control),
        },
        {
            output("value", PortType::Control),
            output("rightValue", PortType::Control),
        },
        {
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("offset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("rightScale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("rightOffset", "0.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.absoluteValue",
        "Absolute Value",
        "Math",
        {input("value", PortType::Control)},
        {output("value", PortType::Control)},
        {},
    });

    add(registry, {
        "talas.interpolate",
        "Interpolate",
        "Math",
        {
            input("left", PortType::Control),
            input("right", PortType::Control),
            optionalInput("input1Right", PortType::Control),
            optionalInput("input2Right", PortType::Control),
            optionalInput("amount", PortType::Control),
            optionalInput("rightAmount", PortType::Control),
        },
        {
            output("value", PortType::Control),
            output("rightValue", PortType::Control),
        },
        {
            parameter("amount", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("rightAmount", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.channelCrosser",
        "Channel Crosser",
        "Mixing & Panning",
        {
            {"audioIn", PortType::Audio, true,
             {PortSignalRate::Sample, AudioBusLayoutKind::Mono,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, true}},
            {"rightIn", PortType::Audio, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::Mono,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, true}},
            {"leftInLeft", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"rightInLeft", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"leftInRight", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"rightInRight", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("leftInLeft", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("rightInLeft", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("leftInRight", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("rightInRight", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.channelJoin",
        "Channel Join",
        "Mixing & Panning",
        {
            input("left", PortType::Audio),
            input("right", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.complexProduct",
        "Complex Product",
        "Math",
        {
            input("input0Left", PortType::Audio),
            input("input0Right", PortType::Audio),
            input("input1Left", PortType::Audio),
            input("input1Right", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {},
    });

    add(registry, {
        "talas.clippingDivide",
        "Clipping Divide",
        "Math",
        {
            input("numerator", PortType::Audio),
            input("denominator", PortType::Audio),
            {"scale", PortType::Control, false,
             {PortSignalRate::Sample,
              AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {output("audioOut", PortType::Audio)},
        {parameter("scale", "1.0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.minimum", "Minimum", "Math",
        {
            input("input0Left", PortType::Audio),
            input("input0Right", PortType::Audio),
            input("input1Left", PortType::Audio),
            input("input1Right", PortType::Audio),
            input("input2Left", PortType::Audio),
            input("input2Right", PortType::Audio),
        },
        {output("leftOut", PortType::Audio),
         output("rightOut", PortType::Audio)},
        {},
    });
    add(registry, {
        "talas.maximum", "Maximum", "Math",
        {
            input("input0Left", PortType::Audio),
            input("input0Right", PortType::Audio),
            input("input1Left", PortType::Audio),
            input("input1Right", PortType::Audio),
            input("input2Left", PortType::Audio),
            input("input2Right", PortType::Audio),
        },
        {output("leftOut", PortType::Audio),
         output("rightOut", PortType::Audio)},
        {},
    });

    add(registry, {
        "talas.channeller",
        "Channeller",
        "Mixing & Panning",
        {
            // audioIn and the optional rightIn are the left and right
            // channels of the Input; a mono Input feeds both.
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            {"leftChannel", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"rightChannel", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            booleanParameter("leftChannel", true),
            booleanParameter("rightChannel", true),
        },
    });

    // The Mixer adds all of its Inputs and
    // sets the level of each output channel. A stereo Input connects its
    // left channel to inputs and its right to rightInputs; the right output
    // hears a mono Input, with nothing on rightInputs, too.
    add(registry, {
        "talas.mixerV2",
        "Mixer",
        "Mixing & Panning",
        {
            [] {
                auto inputs = input("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            [] {
                auto rightInputs = optionalInput("rightInputs", PortType::Audio);
                rightInputs.acceptsMany = true;
                return rightInputs;
            }(),
            {
                "left",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "right",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    // The two-input Mixer that projects before format 43 name; opening such
    // a project migrates it to talas.mixerV2.
    add(registry, {
        "talas.mixer",
        "Two-Input Mixer",
        "Mixing",
        {
            {
                "in1",
                PortType::Audio,
                true,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::Mono,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    true,
                },
            },
            {
                "in2",
                PortType::Audio,
                true,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::Mono,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    true,
                },
            },
            {
                "gain1",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
            {
                "gain2",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {{
            "audioOut",
            PortType::Audio,
            false,
            {
                PortSignalRate::Sample,
                AudioBusLayoutKind::Mono,
                PortEdgeSemantics::Continuous,
                SpectralFrameFormat::NotApplicable,
                true,
            },
        }},
        {
            parameter("gain1", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("gain2", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.overlappingMixer",
        "Overlapping Mixer",
        "Mixing & Panning",
        {
            // The Inputs play one after another, in the order they
            // connect to inputs. Projects before format 78 gave at most four
            // on in1 to in4, which play first, in the order legacyInputs
            // lists them.
            optionalInput("in1", PortType::Audio),
            optionalInput("in2", PortType::Audio),
            optionalInput("in3", PortType::Audio),
            optionalInput("in4", PortType::Audio),
            [] {
                auto inputs = optionalInput("inputs", PortType::Audio);
                inputs.acceptsMany = true;
                return inputs;
            }(),
            optionalInput("left", PortType::Control),
            optionalInput("right", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            textParameter("legacyInputs", {}),
            parameter("overlapTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            // The Inputs play one after another; projects before
            // format 46 mixed them at once.
            choiceParameter("sequence", "oneAfterAnother",
                {"oneAfterAnother", "legacyAllAtOnce"}),
        },
    });

    add(registry, {
        "talas.binauralPan",
        "Binaural Pan",
        "Spatializing",
        {
            input("audioIn", PortType::Audio),
            optionalInput("pan", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {parameter("pan", "0.5", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.pan",
        "Pan",
        "Mixing",
        {
            input("audioIn", PortType::Audio),
            {"pan", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            parameter("pan", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("type", "power", {"power", "linear"}),
            choiceParameter(
                "layout", "stereo", {"stereo", "discrete4", "discrete8"}),
        },
    });

    add(registry, {
        "talas.horizontalAmbisonicEncoder",
        "Horizontal Ambisonic Encoder",
        "Spatializing",
        {
            input("audioIn", PortType::Audio),
            optionalInput("azimuth", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {
            output("wOut", PortType::Audio),
            output("xOut", PortType::Audio),
            output("yOut", PortType::Audio),
        },
        {
            parameter("azimuth", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.horizontalAmbisonicDecoder",
        "Horizontal Ambisonic Decoder",
        "Spatializing",
        {
            input("wIn", PortType::Audio),
            input("xIn", PortType::Audio),
            input("yIn", PortType::Audio),
            optionalInput("scale", PortType::Control),
        },
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            choiceParameter(
                "layout", "discrete4", {"discrete4", "discrete8"}),
        },
    });

    add(registry, {
        "talas.stereoMix2",
        "Stereo Mix 2",
        "Mixing",
        {
            input("in1", PortType::Audio),
            input("in2", PortType::Audio),
            {"left", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"right", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan1", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale1", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan2", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale2", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan1", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale1", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan2", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("scale2", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.stereoMix4",
        "Stereo Mix 4",
        "Mixing & Panning",
        {
            input("in1", PortType::Audio),
            input("in2", PortType::Audio),
            input("in3", PortType::Audio),
            input("in4", PortType::Audio),
            {"left", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"right", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan1", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale1", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan2", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale2", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan3", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale3", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"pan4", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
            {"scale4", PortType::Control, false,
             {PortSignalRate::Sample, AudioBusLayoutKind::NotApplicable,
              PortEdgeSemantics::Continuous,
              SpectralFrameFormat::NotApplicable, false}},
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("left", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("right", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan1", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale1", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan2", "0.33", talas::expr::ValueDomain::Scalar),
            parameter("scale2", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan3", "0.67", talas::expr::ValueDomain::Scalar),
            parameter("scale3", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("pan4", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("scale4", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.vca",
        "VCA",
        "Mixing",
        {
            // The Inputs multiply together.
            [] {
                auto audioIn = input("audioIn", PortType::Audio);
                audioIn.acceptsMany = true;
                return audioIn;
            }(),
            optionalInput("gain", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {parameter("gain", "1.0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.filter",
        "Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("q", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("feedback", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("type", "lowPass", {"lowPass", "highPass", "allPass"}),
            parameter("frequency", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("q", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("feedback", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("order", "4", talas::expr::ValueDomain::Scalar),
            choiceParameter("poles", "order", {"order", "legacyTwoPerOrder"}),
            // A negative Feedback is the positive one 180 degrees out of
            // phase; projects before format 92 turned the loop's
            // sign over.
            choiceParameter("negativeFeedback", "invertsPhase", {"invertsPhase", "legacyTurnsSignOver"}),
            // Butterworth makes a LowPass or
            // HighPass's Order poles a Butterworth filter, flat in its
            // passband, so that a high-pass at 150 hz into a low-pass at
            // 1200 hz passes the band between them unattenuated;
            // onePoleSections, the default, keeps Order sections each 3 dB
            // down at Frequency.
            choiceParameter("response", "onePoleSections", {"onePoleSections", "butterworth"}),
        },
    });

}

static void registerBuiltinNodeDescriptorsPart4(NodeRegistry& registry)
{
    add(registry, {
        "talas.averagingLowPassFilter",
        "Averaging Low Pass Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("cutoff", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("cutoff", "1000 hz", talas::expr::ValueDomain::Hertz),
            resourceParameter("wavetable", "Private"),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.lowPassGate",
        "Low-Pass Gate",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("control", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("control", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("closedFrequency", "40 hz", talas::expr::ValueDomain::Hertz),
            parameter("openFrequency", "18000 hz", talas::expr::ValueDomain::Hertz),
            parameter("attackTime", "0.005 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "0.08 s", talas::expr::ValueDomain::Seconds),
            parameter("curve", "2.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.morphFilter",
        "Morph Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("cutoff", PortType::Control),
            optionalInput("resonance", PortType::Control),
            optionalInput("morph", PortType::Control),
            optionalInput("drive", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("cutoff", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("resonance", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("morph", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("drive", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.dualParallelTwoPoleFilter",
        "Dual Parallel Two Pole Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("scale1", PortType::Control),
            optionalInput("scale2", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            textParameter("pole1", "0.99495 % 0.031"),
            parameter("scale1", "0.3", talas::expr::ValueDomain::Scalar),
            textParameter("pole2", "0.96775 % 0.22449"),
            parameter("scale2", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.firFilter",
        "FIR Filter",
        "Filters",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {textParameter("coefficients", "0.25 0.5 0.25")},
    });

    add(registry, {
        "talas.graphicEq",
        "Graphic EQ",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("cf250Hz", PortType::Control),
            optionalInput("cf500Hz", PortType::Control),
            optionalInput("cf1000Hz", PortType::Control),
            optionalInput("cf2000Hz", PortType::Control),
            optionalInput("cf4000Hz", PortType::Control),
            optionalInput("cf8000Hz", PortType::Control),
            optionalInput("cf16000Hz", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("cf250Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf500Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf1000Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf2000Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf4000Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf8000Hz", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cf16000Hz", "1.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("outerBands", "shelving", {"shelving", "legacyBandPass"}),
        },
    });

    add(registry, {
        "talas.harmonicResonator",
        "Harmonic Resonator",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("decayTime", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("decayTime", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("brightness", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "220 hz", talas::expr::ValueDomain::Hertz),
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", false),
            parameter("scale", "0.1", talas::expr::ValueDomain::Scalar),
            // Resonances at the Frequency; projects before format 72
            // rounded the period to whole samples.
            choiceParameter("tuning", "exact", {"exact", "legacyRounded"}),
        },
    });

    add(registry, {
        "talas.modalFilter",
        "Modal Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequencyScale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            textParameter("modeAmps", "#(1)"),
            textParameter("modeFreqs", "#(440)"),
            textParameter("modeBWs", "#(0.1)"),
            parameter("inputLevel", "1", talas::expr::ValueDomain::Scalar),
            parameter("envelope", "1", talas::expr::ValueDomain::Scalar),
            parameter("frequencyScale", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("reset", false),
        },
    });

    add(registry, {
        "talas.highShelvingFilter",
        "High Shelving Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("cutoffFreq", PortType::Control),
            optionalInput("boostOrCut", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("cutoffFreq", "2000 hz", talas::expr::ValueDomain::Hertz),
            parameter("boostOrCut", "6", talas::expr::ValueDomain::Scalar),
            parameter("scale", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.lowShelvingFilter",
        "Low Shelving Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("cutoffFreq", PortType::Control),
            optionalInput("boostOrCut", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("cutoffFreq", "200 hz", talas::expr::ValueDomain::Hertz),
            parameter("boostOrCut", "6", talas::expr::ValueDomain::Scalar),
            parameter("scale", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.presenceFilter",
        "Presence Filter",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("centerFreq", PortType::Control),
            optionalInput("bandwidth", PortType::Control),
            optionalInput("boostOrCut", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("centerFreq", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth", "200 hz", talas::expr::ValueDomain::Hertz),
            parameter("boostOrCut", "6", talas::expr::ValueDomain::Scalar),
            parameter("scale", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.reResonator",
        "RE Resonator",
        "Filters",
        {input("audioIn", PortType::Audio), optionalInput("timeIndex", PortType::Control)},
        {output("audioOut", PortType::Audio)},
        {
            // A computed RE table, a stable resonance, until an RE analysis
            // is chosen (ADR-0022).
            resourceParameter("wavetable", "Resonance"),
            parameter("timeIndex", "0.0", talas::expr::ValueDomain::Scalar),
            // Projects before format 63 read the Wavetable as one static set
            // of FIR coefficients (ADR-0022).
            choiceParameter("filterForm", "allPoleLattice", {"allPoleLattice", "legacyFir"}),
        },
    });

    add(registry, {
        "talas.twoFormantElement",
        "Two Formant Element",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("formant1", PortType::Control),
            optionalInput("bandwidth1", PortType::Control),
            optionalInput("scale1", PortType::Control),
            optionalInput("formant2", PortType::Control),
            optionalInput("bandwidth2", PortType::Control),
            optionalInput("scale2", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("formant1", "238 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth1", "70 hz", talas::expr::ValueDomain::Hertz),
            parameter("scale1", "0.3", talas::expr::ValueDomain::Scalar),
            parameter("formant2", "1741 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth2", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("scale2", "1.0", talas::expr::ValueDomain::Scalar),
            // UnityPeak scales each
            // section to unity gain at its formant, so a narrower band lets
            // less noise through and wants more Gain, and a bandwidth
            // brought down to 0 lets no new energy in while what the filter
            // holds rings on at its formant. none, the default, keeps the
            // unscaled sections projects have played.
            choiceParameter("normalization", "none", {"none", "unityPeak"}),
        },
    });

    add(registry, {
        "talas.vcf",
        "VCF",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            optionalInput("amplitude", PortType::Control),
            optionalInput("cutoff", PortType::Control),
            optionalInput("cutoffModulator", PortType::Control),
            optionalInput("cutoffModRange", PortType::Control),
            optionalInput("resonance", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("amplitude", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("cutoff", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("cutoffModulator", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("cutoffModRange", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("resonance", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.scaleVocoder",
        "Scale Vocoder",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            input("sideChain", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds),
            parameter("attackTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("nbrBands", "7", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "7", talas::expr::ValueDomain::Scalar), 7.0),
            parameter("tonic", "4 c", talas::expr::ValueDomain::NoteNumber),
            textParameter("intervals", "0 2 4 5 7 9 11"),
            parameter("sideLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("inputLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("inBandwidth", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("sideBandwidth", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("tone", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("rolloff", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1.0", talas::expr::ValueDomain::Scalar),
            // Rolloff sets the width of the Tone control, 0
            // weighting the bands alike; projects before format 94 tilted
            // the bands by Tone and again by Rolloff.
            choiceParameter("rolloffShape", "toneWidth", {"toneWidth", "legacyTilt"}),
        },
    });

    add(registry, {
        "talas.tunableVocoder",
        "Tunable Vocoder",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            input("sideChain", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds),
            parameter("attackTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("nbrBands", "16", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "16", talas::expr::ValueDomain::Scalar), 16.0),
            booleanParameter("logSpacing", false),
            parameter("analysisFreq", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("synthesisFreq", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("analysisLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("synthesisLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("analysisSpacing", "100", talas::expr::ValueDomain::Scalar),
            parameter("synthesisSpacing", "100", talas::expr::ValueDomain::Scalar),
            parameter("analysisBW", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("synthesisBW", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("tone", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("rolloff", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1.0", talas::expr::ValueDomain::Scalar),
            // Spacing is in hz, or in nn with LogSpacing; projects
            // before format 44 gave a multiple of the base, or octaves.
            choiceParameter("spacingUnits", "hzOrNn",
                {"hzOrNn", "legacyRelative"}),
            // Rolloff sets how far Tone tilts the filters, 0 weighting them
            // alike; projects before format 79 tilted by Tone and
            // again by Rolloff.
            choiceParameter("rolloffShape", "toneWidth", {"toneWidth", "legacyTilt"}),
        },
    });

    add(registry, {
        "talas.vocoder",
        "Vocoder",
        "Filters",
        {
            input("audioIn", PortType::Audio),
            input("sideChain", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds),
            parameter("attackTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("nbrBands", "16", talas::expr::ValueDomain::Scalar),
            defaultKeywordParameter(
                parameter("bankSize", "16", talas::expr::ValueDomain::Scalar), 16.0),
            parameter("inputLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("sideLevel", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("lowCF", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("highCF", "5000 hz", talas::expr::ValueDomain::Hertz),
            parameter("inFreq", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("sideFreq", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("bw", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("pitch", false),
            parameter("loCutoff", "50 hz", talas::expr::ValueDomain::Hertz),
            parameter("hiCutoff", "10000 hz", talas::expr::ValueDomain::Hertz),
            parameter("rolloff", "0.5", talas::expr::ValueDomain::Scalar),
            // LoCutoff, HiCutoff and Rolloff shape the Input as a weak tone
            // control; projects before format 79 tilted it by
            // Rolloff alone.
            choiceParameter("inputShaping", "cutoffs", {"cutoffs", "legacyTilt"}),
        },
    });

    add(registry, {
        "talas.vocoderChannelBank",
        "Vocoder Channel Bank",
        "Filters",
        {
            input("cascadeInput", PortType::Audio),
            input("audioIn", PortType::Audio),
            input("sideChain", PortType::Audio),
            input("inputParameters", PortType::Spectrum),
            input("sideChainParameters", PortType::Spectrum),
        },
        {output("audioOut", PortType::Audio)},
        {
            // The project's VocoderChannelBank example (c76): a short time
            // constant for intelligibility, the first two filters.
            parameter("timeConstant", "5 ms", talas::expr::ValueDomain::Seconds),
            parameter("attackTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("releaseTime", "-1 s", talas::expr::ValueDomain::Seconds),
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            parameter("count", "2", talas::expr::ValueDomain::Scalar),
            // The filters are scaled by the spectra's NbrPartials, the whole
            // cascade's; projects before format 92 scaled each bank
            // by its own Count.
            choiceParameter("gain", "nbrPartials", {"nbrPartials", "legacyCount"}),
        },
    });

    add(registry, {
        "talas.delayWithFeedback",
        "Delay With Feedback",
        "Effects",
        {
            input("audioIn", PortType::Audio),
            optionalInput("delay", PortType::Control),
            optionalInput("feedback", PortType::Control),
            optionalInput("mix", PortType::Control),
            optionalInput("scale", PortType::Control),
            optionalInput("delayScale", PortType::Control),
            // A stereo Input's right channel: the two are delayed as one
            // mono mix.
            optionalInput("rightIn", PortType::Audio),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("delay", "250 ms", talas::expr::ValueDomain::Seconds),
            parameter("feedback", "0.25", talas::expr::ValueDomain::Scalar),
            // Mix is this project's: 1 plays the delayed signal alone, so a
            // Comb is heard only after the first delay; projects
            // before format 73 mix in half the Input.
            parameter("mix", "1.0", talas::expr::ValueDomain::Scalar),
            // Type, Scale, DelayScale and Interpolation.
            choiceParameter("type", "comb", {"comb", "allpass"}),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("delayScale", "1.0", talas::expr::ValueDomain::Scalar),
            choiceParameter("interpolation", "none", {"linear", "none"}),
            // Wavetable, Prezero and SmoothDelayChanges (ADR-0025): delays
            // naming one Wavetable share a delay line.
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", true),
            booleanParameter("smoothDelayChanges", false),
        },
    });

    add(registry, {
        "talas.stereoDispersiveResonator",
        "Stereo Dispersive Resonator",
        "Effects",
        {
            input("audioIn", PortType::Audio),
            optionalInput("frequency", PortType::Control),
            optionalInput("decayTime", PortType::Control),
            optionalInput("detune", PortType::Control),
            optionalInput("damping", PortType::Control),
            optionalInput("dispersion", PortType::Control),
            optionalInput("width", PortType::Control),
            optionalInput("mix", PortType::Control),
            optionalInput("inputGain", PortType::Control),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            parameter("frequency", "220 hz", talas::expr::ValueDomain::Hertz),
            parameter("decayTime", "1.5 s", talas::expr::ValueDomain::Seconds),
            parameter("detune", "7.0", talas::expr::ValueDomain::Scalar),
            parameter("damping", "0.7", talas::expr::ValueDomain::Scalar),
            parameter("dispersion", "0.2", talas::expr::ValueDomain::Scalar),
            parameter("width", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("mix", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("inputGain", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.pluckedWaveguide",
        "Plucked Waveguide",
        "Generators",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("decayTime", PortType::Control),
            optionalInput("brightness", PortType::Control),
            optionalInput("pickPosition", PortType::Control),
            optionalInput("stiffness", PortType::Control),
            optionalInput("level", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("trigger", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("decayTime", "2 s", talas::expr::ValueDomain::Seconds),
            parameter("brightness", "0.65", talas::expr::ValueDomain::Scalar),
            parameter("pickPosition", "0.23", talas::expr::ValueDomain::Scalar),
            parameter("stiffness", "0.15", talas::expr::ValueDomain::Scalar),
            parameter("level", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("seed", "197", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.dynamicBreakpointOscillator",
        "Dynamic Breakpoint Oscillator",
        "Generators",
        {
            optionalInput("reset", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("volatility", PortType::Control),
            optionalInput("correlation", PortType::Control),
            optionalInput("timingJitter", PortType::Control),
            optionalInput("curve", PortType::Control),
            optionalInput("level", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("volatility", "0.35", talas::expr::ValueDomain::Scalar),
            parameter("correlation", "0.75", talas::expr::ValueDomain::Scalar),
            parameter("timingJitter", "0.2", talas::expr::ValueDomain::Scalar),
            parameter("curve", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("level", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("pointCount", "16", talas::expr::ValueDomain::Scalar),
            parameter("seed", "211", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.waveTerrainOscillator",
        "Wave Terrain Oscillator",
        "Generators",
        {
            optionalInput("reset", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("xRadius", PortType::Control),
            optionalInput("yRadius", PortType::Control),
            optionalInput("yRatio", PortType::Control),
            optionalInput("rotation", PortType::Control),
            optionalInput("phaseOffset", PortType::Control),
            optionalInput("level", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("xRadius", "0.9", talas::expr::ValueDomain::Scalar),
            parameter("yRadius", "0.9", talas::expr::ValueDomain::Scalar),
            parameter("yRatio", "2.0", talas::expr::ValueDomain::Scalar),
            parameter("rotation", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("phaseOffset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("level", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("width", "8", talas::expr::ValueDomain::Scalar),
            parameter("height", "8", talas::expr::ValueDomain::Scalar),
            // A computed 8 by 8 terrain for the default Width and Height.
            resourceParameter("terrain", "Terrain"),
        },
    });

    add(registry, {
        "talas.scannedMassOscillator",
        "Scanned Mass Oscillator",
        "Generators",
        {
            optionalInput("reset", PortType::Control),
            optionalInput("excite", PortType::Control),
            optionalInput("frequency", PortType::Control),
            optionalInput("modelRate", PortType::Control),
            optionalInput("coupling", PortType::Control),
            optionalInput("damping", PortType::Control),
            optionalInput("restoring", PortType::Control),
            optionalInput("excitationPosition", PortType::Control),
            optionalInput("excitationAmount", PortType::Control),
            optionalInput("scanOffset", PortType::Control),
            optionalInput("level", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("reset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("excite", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "110 hz", talas::expr::ValueDomain::Hertz),
            parameter("modelRate", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("coupling", "0.2", talas::expr::ValueDomain::Scalar),
            parameter("damping", "0.01", talas::expr::ValueDomain::Scalar),
            parameter("restoring", "0.01", talas::expr::ValueDomain::Scalar),
            parameter("excitationPosition", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("excitationAmount", "0.75", talas::expr::ValueDomain::Scalar),
            parameter("scanOffset", "0.0", talas::expr::ValueDomain::Scalar),
            parameter("level", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("massCount", "32", talas::expr::ValueDomain::Scalar),
            parameter("initialShape", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.reverbSection",
        "Reverb Section",
        "Reverb, Delay, Feedback",
        {
            input("audioIn", PortType::Audio),
            optionalInput("decayTime", PortType::Control),
            optionalInput("delay", PortType::Control),
            optionalInput("delayScale", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("type", "comb", {"comb", "allpass"}),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("decayTime", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("delay", "250 ms", talas::expr::ValueDomain::Seconds),
            parameter("delayScale", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "Private"),
            booleanParameter("prezero", true),
            choiceParameter("interpolation", "linear", {"linear", "none"}),
            booleanParameter("smoothDelayChanges", true),
        },
    });

    const auto addSoundVariableNode = [&](
        std::string typeId,
        std::string displayName,
        std::string category) {
        NodeTypeDescriptor descriptor{
            std::move(typeId),
            std::move(displayName),
            std::move(category),
            {},
            {output("audioOut", PortType::Audio)},
            {textParameter("variableId", {})},
        };
        descriptor.inputs.reserve(
            talas::project::maximumProjectSoundVariableCandidates);
        for (std::size_t index = 0;
             index < talas::project::maximumProjectSoundVariableCandidates;
             ++index)
        {
            descriptor.inputs.push_back(compilerPopulatedInput(
                "candidate" + std::to_string(index),
                PortType::Audio));
        }
        descriptor.capabilities.state = NodeStateModel::SessionStateful;
        descriptor.capabilities.typedEvents = true;
        descriptor.capabilities.latency.kind = LatencyKind::None;
        descriptor.capabilities.fanInLatencyPolicy =
            FanInLatencyPolicy::Align;
        descriptor.capabilities.outputBusLayouts = {
            AudioBusLayoutKind::Mono};
        // A SoundCollectionVariable in a field that takes more than one
        // Sound stands for all its Sounds; projects before
        // format 90 played the one its declaration selected.
        if (descriptor.typeId == "talas.soundCollectionVariable")
        {
            descriptor.parameters.push_back(
                choiceParameter("members", "all", {"all", "legacySelected"}));
        }
        descriptor.capabilities.limitations = {
            "candidate-inputs-are-compiler-populated-from-variable-id",
            "maximum-32-source-sound-candidates",
            "candidates-require-no-external-inputs-and-one-mono-audio-output",
            "candidate-midi-voice-closures-unsupported",
            "selection-uses-fixed-128-frame-linear-crossfade",
        };
        add(registry, std::move(descriptor));
    };
    addSoundVariableNode(
        "talas.soundCollectionVariable",
        "Sound Collection Variable",
        "Variables");
    addSoundVariableNode(
        "talas.variable",
        "Variable",
        "Variables & Annotation");

    add(registry, {
        "talas.triggeredTableRead",
        "Triggered Table Read",
        "Sampling",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("gate", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "Sine"),
            parameter("gate", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("ignoreLoops", false),
            booleanParameter("fromMemoryWriter", false),
            choiceParameter("headerLoop", "loop", {"loop", "legacyWholeTable"}),
        },
    });

    add(registry, {
        "talas.triggeredSampleAndHold",
        "Triggered Sample And Hold",
        "Sampling",
        {
            input("value", PortType::Control),
            optionalInput("trigger", PortType::Control),
        },
        {output("value", PortType::Control)},
        {parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar)},
    });

    add(registry, {
        "talas.inputOutputCharacteristic",
        "Input Output Characteristic",
        "Distortion & Waveshaping",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("inValues", "-1 0 1"),
            textParameter("outValues", "-1 0 1"),
            parameter("smoothing", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("startSlope", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("endSlope", "1.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.iteratedWaveshaper",
        "Iterated Waveshaper",
        "Distortion & Waveshaping",
        {
            input("audioIn", PortType::Audio),
            optionalInput("iterations", PortType::Control),
            optionalInput("scale", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("iterations", "1", talas::expr::ValueDomain::Scalar),
            parameter("scale", "1.0", talas::expr::ValueDomain::Scalar),
            resourceParameter("wavetable", "FullRamp"),
        },
    });

    add(registry, {
        "talas.saturator",
        "Saturator",
        "Distortion & Waveshaping",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            optionalInput("channel3In", PortType::Audio),
            optionalInput("channel4In", PortType::Audio),
            optionalInput("threshold", PortType::Audio),
        },
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
            output("channel3Out", PortType::Audio),
            output("channel4Out", PortType::Audio),
        },
        {
            parameter("threshold", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("transitionWidth", "0.1", talas::expr::ValueDomain::Scalar),
            choiceParameter("mode", "saturator",
                {"saturator", "repressor"}),
        },
    });

    add(registry, {
        "talas.waveshaper",
        "Waveshaper",
        "Distortion & Waveshaping",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
        },
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            choiceParameter("interpolation", "linear", {"none", "linear"}),
            choiceParameter("shapeFrom", "wavetable", {"wavetable", "polynomial"}),
            resourceParameter("shapingFunction", "FullRamp"),
            textParameter("coefficients", {}),
            booleanParameter("fromMemoryWriter", false),
            // Interpolation none reads the table at the integer part of the
            // index; projects before format 75 read the nearest
            // entry.
            choiceParameter("integerIndex", "truncate", {"truncate", "legacyRound"}),
        },
    });

    add(registry, {
        "talas.setDuration",
        "Set Duration",
        "Time & Duration",
        {
            input("audioIn", PortType::Audio),
            optionalInput("startTime", PortType::Control),
            optionalInput("duration", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("startTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("duration", "1 s", talas::expr::ValueDomain::Seconds),
            // StartTime starts the Input late (ADR-0023), read when the
            // SetDuration starts; projects before format 66 delayed its
            // samples instead, before 87 only a fixed StartTime outside
            // MIDI voices started it late, and before 89 only one in a
            // Sound without nested MIDIVoices, a Sound's value unpaced; before
            // 96 a Duration fed by a Sound was not paced by fixed Rates.
            choiceParameter("startTimeMeans", "startsInputLate",
                {"startsInputLate", "legacyDelay", "legacyFixedStartsLate", "legacyFlatVoicesOnly",
                "legacyDurationUnpaced"}),
        },
    });

    add(registry, {
        "talas.timeControl",
        "Time Control",
        "Time & Duration",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rate", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
            // Rate paces the Input's events, not its sample rate;
            // projects before format 45 replayed the Input at Rate.
            choiceParameter("rateChanges", "eventTimes",
                {"eventTimes", "legacyPlaybackSpeed"}),
        },
    });

    add(registry, {
        "talas.timeStopper",
        "Time Stopper",
        "Time & Duration",
        {
            input("audioIn", PortType::Audio),
            optionalInput("resume", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("resume", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("resumeOnZero", false),
            // The Input plays while time stops; projects before
            // format 45 held its sample.
            choiceParameter("whileStopped", "inputPlays",
                {"inputPlays", "legacyHold"}),
        },
    });

    add(registry, {
        "talas.timeOffset",
        "Time Offset",
        "Time & Duration",
        {
            input("audioIn", PortType::Audio),
            optionalInput("offset", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("offset", "0 s", talas::expr::ValueDomain::Seconds),
            booleanParameter("reverse", false),
            booleanParameter("retrograde", false),
            // Reverse or Retrograde, but not both, puts the silence after
            // the Input; projects before format 51 reversed the
            // Input in windows of the offset when Reverse was set.
            choiceParameter("reverseMeans", "silenceFollows",
                {"silenceFollows", "legacyReverseWindows"}),
            // SilentTime starts the Input late (ADR-0023), read when the
            // TimeOffset starts; projects before format 64 delayed its
            // samples instead, before 87 only a fixed SilentTime outside
            // MIDI voices started it late, and before 89 only one in a
            // Sound without nested MIDIVoices, a Sound's value unpaced.
            choiceParameter("offsetMeans", "startsInputLate",
                {"startsInputLate", "legacyDelay", "legacyFixedStartsLate", "legacyFlatVoicesOnly"}),
        },
    });

    add(registry, {
        "talas.waitUntil",
        "Wait Until",
        "Time & Duration",
        {
            input("audioIn", PortType::Audio),
            optionalInput("resume", PortType::Control),
        },
        {output("audioOut", PortType::Audio)},
        {
            parameter("resume", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("resumeOnZero", false),
        },
    });

    add(registry, {
        "talas.warpedTimeIndex",
        "Warped Time Index",
        "Time & Duration",
        {
            optionalInput("trigger", PortType::Control),
            optionalInput("rate", PortType::Control),
            optionalInput("morph", PortType::Control),
        },
        {output("value", PortType::Control)},
        {
            textParameter("idealTimePoints0", "{0 s} {1 s}"),
            textParameter("idealTimePoints1", "{0 s} {1 s}"),
            textParameter("currentTimePoints", "{0 s} {1 s}"),
            parameter("trigger", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("morph", "0.0", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.audioOutput",
        "Audio Output",
        "IO",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
        },
        {},
        {},
    });

    add(registry, {
        "talas.annotation",
        "Annotation",
        "Annotation",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        // The Text commenting on the Input.
        {textParameter("text", {})},
    });

    add(registry, {
        "talas.mapEventValues",
        "Map Event Values",
        "Control",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("greenVariable", {}),
            choiceParameter("valueKind", "string", {"string", "collection"}),
            textParameter("stringValue", {}),
            textParameter("collectionValues", {}),
        },
    });

    add(registry, {
        "talas.prefixer",
        "Prefixer",
        "Control",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("id", {}),
            textParameter("specialEvents", {}),
            booleanParameter("renameSpecialEvents", false),
        },
    });

    add(registry, {
        "talas.replicator",
        "Replicator",
        "Control",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("number", "1", talas::expr::ValueDomain::Scalar),
            textParameter("prefix", {}),
            booleanParameter("incrementPrefix", true),
            textParameter("suffix", "01"),
            booleanParameter("incrementSuffix", true),
            textParameter("specialEvents", {}),
            booleanParameter("renameSpecialEvents", false),
        },
    });

    add(registry, {
        "talas.stereoizer",
        "Stereoizer",
        "Control",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
        },
        {
            textParameter("prefix", {}),
            booleanParameter("incrementPrefix", false),
            textParameter("suffix", "'L' 'R'"),
            booleanParameter("incrementSuffix", false),
            textParameter("specialEvents", {}),
            booleanParameter("renameSpecialEvents", false),
            textParameter("sharedSounds", {}),
            booleanParameter("leftBypass", false),
            booleanParameter("rightBypass", false),
        },
    });

    add(registry, {
        "talas.surroundifer",
        "Surroundifer",
        "Control",
        {
            input("audioIn", PortType::Audio),
            input("rightIn", PortType::Audio),
        },
        {
            output("leftOut", PortType::Audio),
            output("rightOut", PortType::Audio),
            output("leftSurroundOut", PortType::Audio),
            output("rightSurroundOut", PortType::Audio),
            output("centerOut", PortType::Audio),
            output("lfeOut", PortType::Audio),
        },
        {
            textParameter("prefix", {}),
            booleanParameter("incrementPrefix", false),
            textParameter("suffix", "'L' 'R' 'LS' 'RS' 'C' 'LFE'"),
            booleanParameter("incrementSuffix", false),
            textParameter("specialEvents", {}),
            booleanParameter("renameSpecialEvents", false),
            textParameter("sharedSounds", {}),
            parameter("levelL", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("levelR", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("levelLS", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("levelRS", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("levelC", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("levelLFE", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("bypassL", false),
            booleanParameter("bypassR", false),
            booleanParameter("bypassLS", false),
            booleanParameter("bypassRS", false),
            booleanParameter("bypassC", false),
            booleanParameter("bypassLFE", false),
        },
    });

    add(registry, {
        "talas.sampleWithTimeIndex",
        "Sample With Time Index",
        "Sampling",
        {input("timeIndex", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("sample", {}),
            choiceParameter("interpolation", "linear", {"none", "linear"}),
        },
    });

    add(registry, {
        "talas.multiSampleWithTimeIndex",
        "Multi Sample With Time Index",
        "Sampling",
        {input("timeIndex", PortType::Audio),
         optionalInput("index", PortType::Control)},
        {output("audioOut", PortType::Audio)},
        {
            staticResourceListParameter("fileNames", {}),
            parameter("index", "0", talas::expr::ValueDomain::Scalar),
            choiceParameter("interpolation", "linear", {"none", "linear"}),
        },
    });

    add(registry, {
        "talas.splitSurroundSampleWithTimeIndex",
        "Split Surround Sample With Time Index",
        "Sampling",
        {input("timeIndex", PortType::Audio)},
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            staticResourceListParameter("fileNames", {}),
            choiceParameter("interpolation", "linear", {"none", "linear"}),
        },
    });

    add(registry, {
        "talas.splitSurroundOscillator",
        "Split Surround Oscillator",
        "Oscillators",
        {optionalInput("frequency", PortType::Audio)},
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            // The Sine table as the split cycle of each of the two default
            // Channels until files are chosen.
            staticResourceListParameter("fileNames", "Sine Sine"),
            choiceParameter("channels", "2",
                {"1", "2", "3", "4", "5", "6", "7", "8"}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            choiceParameter("interpolation", "linear", {"none", "linear"}),
        },
    });

    add(registry, {
        "talas.splitSurroundAntialiasedOscillator",
        "Split Surround Antialiased Oscillator",
        "Oscillators",
        {
            optionalInput("frequency", PortType::Control),
            optionalInput("brightness", PortType::Control),
        },
        {
            output("out1", PortType::Audio),
            output("out2", PortType::Audio),
            output("out3", PortType::Audio),
            output("out4", PortType::Audio),
            output("out5", PortType::Audio),
            output("out6", PortType::Audio),
            output("out7", PortType::Audio),
            output("out8", PortType::Audio),
        },
        {
            // The Sine table as the split cycle of each of the two default
            // Channels until files are chosen.
            staticResourceListParameter("fileNames", "Sine Sine"),
            choiceParameter("channels", "2",
                {"1", "2", "3", "4", "5", "6", "7", "8"}),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("brightness", "1.0", talas::expr::ValueDomain::Scalar),
            parameter("dutyCycle", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.oscillatorTimeIndex",
        "Oscillator Time Index",
        "Time & Duration",
        {optionalInput("frequency", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz)},
    });

}

static void registerBuiltinNodeDescriptorsPart5(NodeRegistry& registry)
{
    add(registry, {
        "talas.timeIndex",
        "Time Index",
        "Time & Duration",
        {optionalInput("gate", PortType::Control),
         optionalInput("rate", PortType::Audio),
         optionalInput("start", PortType::Control),
         optionalInput("attackEnd", PortType::Control),
         optionalInput("loopStart", PortType::Control)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("duration", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("rate", "1", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("looping", false),
            booleanParameter("reverse", false),
            parameter("start", "0", talas::expr::ValueDomain::Scalar),
            parameter("attackEnd", "0", talas::expr::ValueDomain::Scalar),
            parameter("loopStart", "0", talas::expr::ValueDomain::Scalar),
            parameter("loopEnd", "1", talas::expr::ValueDomain::Scalar),
            parameter("releaseStart", "1", talas::expr::ValueDomain::Scalar),
            parameter("end", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.timeIndexFromFileName",
        "Time Index From File Name",
        "Time & Duration",
        {optionalInput("gate", PortType::Control),
         optionalInput("rate", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            resourceParameter("fileName", {}),
            parameter("rate", "1", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("looping", true),
            booleanParameter("reverse", false),
        },
    });

    add(registry, {
        "talas.timeIndexForSamples",
        "Time Index For Samples",
        "Time & Duration",
        {optionalInput("index", PortType::Control),
         optionalInput("gate", PortType::Control),
         optionalInput("frequency", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            staticResourceListParameter("fileNames", {}),
            parameter("index", "0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("looping", true),
            booleanParameter("reverse", false),
        },
    });

    add(registry, {
        "talas.timeIndexForFileNames",
        "Time Index For File Names",
        "Time & Duration",
        {optionalInput("index", PortType::Control),
         optionalInput("gate", PortType::Control),
         optionalInput("rate", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            staticResourceListParameter("fileNames", {}),
            parameter("index", "0", talas::expr::ValueDomain::Scalar),
            parameter("rate", "1", talas::expr::ValueDomain::Scalar),
            parameter("gate", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("looping", true),
            booleanParameter("reverse", false),
        },
    });

    add(registry, {
        "talas.sharedFileName",
        "Shared File Name",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("fileVariable", {}),
            resourceParameter("selectedFile", {}),
        },
    });

    add(registry, {
        "talas.sharedFileNames",
        "Shared File Names",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            textParameter("fileVariables", {}),
            staticResourceListParameter("selectedFiles", {}),
        },
    });

    add(registry, {
        "talas.uniqueMemoryWriterRecordingNames",
        "Unique Memory Writer Recording Names",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    add(registry, {
        "talas.cancelUniqueMemoryWriterRecordingNames",
        "Cancel Unique Memory Writer Recording Names",
        "Sampling",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    add(registry, {
        "talas.oscilloscopeDisplay",
        "Oscilloscope Display",
        "Display",
        {
            input("audioIn", PortType::Audio),
            optionalInput("rightIn", PortType::Audio),
            optionalInput("trigger", PortType::Event),
            optionalInput("sweepOrTrig", PortType::Audio),
            optionalInput("level", PortType::Control),
        },
        {
            output("audioOut", PortType::Audio),
            output("rightOut", PortType::Audio),
            output("sweepOrTrigOut", PortType::Audio),
        },
        {
            parameter("sweepFreq", "30 hz", talas::expr::ValueDomain::Hertz),
            parameter("initialSweepFrequency", "30 hz", talas::expr::ValueDomain::Hertz),
            parameter("level", "1.0", talas::expr::ValueDomain::Scalar),
            booleanParameter("trigSwap", false),
            choiceParameter("sweepOrTrig", "sweep", {"sweep", "trigger"}),
            choiceParameter("trigThresh", "rising", {"rising", "falling"}),
        },
    });

    add(registry, {
        "talas.spectrumAnalyzerDisplay",
        "Spectrum Analyzer Display",
        "Display",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    // CDP processes (ADR-0018); their cores are the LGPL talas_cdp ports.
    add(registry, {
        "talas.cdpLowerResolution",
        "CDP Lower Resolution",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("bitResolution", "16", talas::expr::ValueDomain::Scalar),
            parameter("sampleRateDivision", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.cdpRingModulate",
        "CDP Ring Modulate",
        "CDP",
        {
            input("audioIn", PortType::Audio),
            {
                "frequency",
                PortType::Control,
                false,
                {
                    PortSignalRate::Sample,
                    AudioBusLayoutKind::NotApplicable,
                    PortEdgeSemantics::Continuous,
                    SpectralFrameFormat::NotApplicable,
                    false,
                },
            },
        },
        {output("audioOut", PortType::Audio)},
        {parameter("frequency", "60 hz", talas::expr::ValueDomain::Hertz)},
    });

    add(registry, {
        "talas.cdpTremolo",
        "CDP Tremolo",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("frequency", "15 hz", talas::expr::ValueDomain::Hertz),
            parameter("depth", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.cdpPan",
        "CDP Pan",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("position", "0", talas::expr::ValueDomain::Scalar),
            parameter("prescale", "0.7", talas::expr::ValueDomain::Scalar),
        },
    });

    add(registry, {
        "talas.cdpTapDelay",
        "CDP Tap Delay",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // CDP's tap file: one tap a line, a time in seconds, an
            // amplitude from 0 to 1 and, for stereo, a pan.
            textParameter("taps", "0.25 0.8 -0.5"),
            parameter("tapGain", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("feedback", "0", talas::expr::ValueDomain::Scalar),
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify revecho` mode 1 and its defaults.
    add(registry, {
        "talas.cdpEcho",
        "CDP Echo",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("delay", "20 ms", talas::expr::ValueDomain::Seconds),
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("feedback", "0", talas::expr::ValueDomain::Scalar),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("invert", false),
        },
    });

    // CDP `modify revecho` mode 2 and its defaults, but a Seed of 1.
    add(registry, {
        "talas.cdpVaryingDelay",
        "CDP Varying Delay",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("delay", "20 ms", talas::expr::ValueDomain::Seconds),
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("feedback", "0", talas::expr::ValueDomain::Scalar),
            parameter("modulation", "0", talas::expr::ValueDomain::Scalar),
            parameter("lfoFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("lfoPhase", "0", talas::expr::ValueDomain::Scalar),
            parameter("lfoDelay", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify revecho` mode 3 and its defaults.
    add(registry, {
        "talas.cdpStadiumEcho",
        "CDP Stadium Echo",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("gain", "0.645654", talas::expr::ValueDomain::Scalar),
            // Below 1, flattens the echoes' fall in level.
            parameter("rolloff", "1", talas::expr::ValueDomain::Scalar),
            // Scales the echoes' times, 0.1 s apart on average.
            parameter("size", "1", talas::expr::ValueDomain::Scalar),
            parameter("count", "23", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `reverb`, which has no defaults; these give its medium room.
    add(registry, {
        "talas.cdpReverb",
        "CDP Reverb",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            parameter("reverbGain", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("reverbTime", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("damping", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("lowpass", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("predelay", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("lowCut", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("highCut", "0 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    // CDP `filter variable` and its defaults; CDP's mode has none.
    add(registry, {
        "talas.cdpVariableFilter",
        "CDP Variable Filter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("mode", "lowPass", {"notch", "bandPass", "lowPass", "highPass"}),
            parameter("acuity", "0.05", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    // CDP `filter fixed` and its defaults; CDP's mode has none.
    add(registry, {
        "talas.cdpFixedFilter",
        "CDP Fixed Filter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("mode", "belowFrequency", {"belowFrequency", "aboveFrequency", "band"}),
            // Boost or cut in dB.
            parameter("boost", "0", talas::expr::ValueDomain::Scalar),
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("bandwidth", "200 hz", talas::expr::ValueDomain::Hertz),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `filter phasing` and its defaults. CDP's mode has none; a phaser
    // at CDP's default gain of 1 cancels its input.
    add(registry, {
        "talas.cdpPhasing",
        "CDP Phasing",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("mode", "phaseShift", {"phaseShift", "phaser"}),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            parameter("delay", "10 ms", talas::expr::ValueDomain::Seconds),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `filter lohi` mode 1 and its defaults, a high-pass filter as its
    // stop band lies below its pass band.
    add(registry, {
        "talas.cdpLohiFilter",
        "CDP Lohi Filter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Attenuation from the pass band to the stop band, in dB.
            parameter("attenuation", "-60", talas::expr::ValueDomain::Scalar),
            parameter("passBand", "1000 hz", talas::expr::ValueDomain::Hertz),
            parameter("stopBand", "600 hz", talas::expr::ValueDomain::Hertz),
            parameter("prescale", "0.9", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `filter sweeping` and its defaults; CDP's mode has none.
    add(registry, {
        "talas.cdpSweepingFilter",
        "CDP Sweeping Filter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("mode", "lowPass", {"notch", "bandPass", "lowPass", "highPass"}),
            parameter("acuity", "0.05", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            parameter("lowFrequency", "100 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "4000 hz", talas::expr::ValueDomain::Hertz),
            parameter("sweepFrequency", "0.5 hz", talas::expr::ValueDomain::Hertz),
            // 0 starts at the low frequency rising, 0.5 at the high one
            // falling.
            parameter("phase", "0.25", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `filter userbank` and its defaults; its data file has none.
    add(registry, {
        "talas.cdpUserBank",
        "CDP User Bank",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's mode: the bands' pitches in Hz or as MIDI note numbers.
            choiceParameter("pitches", "hertz", {"hertz", "midi"}),
            // CDP's data file: a pitch and an amplitude (or level in dB) a
            // line.
            textParameter("bands", "440 1"),
            parameter("q", "20", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("doubleFiltering", false),
        },
    });

    // CDP `distort overload` and its defaults; CDP's mode has none.
    add(registry, {
        "talas.cdpDistortOverload",
        "CDP Distort Overload",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            choiceParameter("mode", "noise", {"noise", "waveform"}),
            parameter("clipLevel", "0.01", talas::expr::ValueDomain::Scalar),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
            // The waveform's frequency, for the waveform mode.
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort reform`, whose mode has no default, and the default of
    // its exaggeration.
    add(registry, {
        "talas.cdpDistortReform",
        "CDP Distort Reform",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 to 8: each half-cycle becomes a square or a
            // triangle at a fixed level or at its peak, turned over against
            // its peak, clicks, a sine, or its contour exaggerated.
            choiceParameter("mode", "square",
                {"fixedSquare", "square", "fixedTriangle", "triangle", "invert", "click",
                    "sine", "exaggerate"}),
            parameter("exaggeration", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort omit`, whose counts have no defaults: one of every two.
    add(registry, {
        "talas.cdpDistortOmit",
        "CDP Distort Omit",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Of every OutOf wavecycles, the last Omit fall silent.
            parameter("omit", "1", talas::expr::ValueDomain::Scalar),
            parameter("outOf", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort reverse` and its default.
    add(registry, {
        "talas.cdpDistortReverse",
        "CDP Distort Reverse",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // The wavecycles in each group played backwards.
            parameter("cycles", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort envel`, whose mode has no default, and its defaults.
    add(registry, {
        "talas.cdpDistortEnvelope",
        "CDP Distort Envelope",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 to 4: the envelope over each group rises, falls,
            // dips to Trough at its middle, or is the user's.
            choiceParameter("shape", "rising", {"rising", "falling", "troughed", "user"}),
            parameter("cycles", "1", talas::expr::ValueDomain::Scalar),
            parameter("trough", "0", talas::expr::ValueDomain::Scalar),
            // 1 is CDP's linear envelope, without its -e flag.
            parameter("exponent", "1", talas::expr::ValueDomain::Scalar),
            // CDP's envelope file for the user's shape: time and value (0
            // to 1) pairs from time 0, on one line or many, the times
            // stretched over each group.
            textParameter("envelope", "0 0 0.5 1 1 0"),
        },
    });

    // CDP `distort harmonic`, whose file has no default, and its default.
    add(registry, {
        "talas.cdpDistortHarmonic",
        "CDP Distort Harmonic",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's harmonics file: a harmonic number from 2 to 1024 and an
            // amplitude from 0 to 32 a line, the wavecycle's own being 1.
            textParameter("harmonics", "2 0.5"),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort fractal` and its defaults.
    add(registry, {
        "talas.cdpDistortFractal",
        "CDP Distort Fractal",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // The wavecycles in each group, and how small its miniature is.
            parameter("scale", "2", talas::expr::ValueDomain::Scalar),
            parameter("loudness", "1", talas::expr::ValueDomain::Scalar),
            parameter("prescale", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort multiply` and its default.
    add(registry, {
        "talas.cdpDistortMultiply",
        "CDP Distort Multiply",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Each half-wavecycle played this many times faster, as often.
            parameter("factor", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distort divide` and its default.
    add(registry, {
        "talas.cdpDistortDivide",
        "CDP Distort Divide",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Of each group of this many half-wavecycles, the first is
            // stretched over all.
            parameter("factor", "2", talas::expr::ValueDomain::Scalar),
            // CDP's -i: reads the stretched half-cycle between samples.
            booleanParameter("interpolate", false),
        },
    });

    // CDP `distort shuffle`, whose shuffle data has no default, and its
    // default.
    add(registry, {
        "talas.cdpDistortShuffle",
        "CDP Distort Shuffle",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's shuffle data: a domain of letters, a dash, and the same
            // letters in the order each domain's groups play.
            textParameter("domainImage", "ab-ba"),
            // The wavecycles in each group.
            parameter("cycles", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `pvoc anal` then `pvoc synth`, and their defaults.
    add(registry, {
        "talas.cdpPhaseVocoder",
        "CDP Phase Vocoder",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // The analysis points, two to a channel, and the overlap, 1 to
            // 4: a window of four, two, one or a half times the points.
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `spec gain` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpSpectralGain",
        "CDP Spectral Gain",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Scales every channel's amplitude.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `spec gate` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpSpectralGate",
        "CDP Spectral Gate",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Silences a channel whose amplitude is under it.
            parameter("threshold", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `focus exag` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpFocusExaggerate",
        "CDP Focus Exaggerate",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Over 1 sharpens each frame's contour, under 1 flattens it.
            parameter("exaggeration", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `focus fold` between `pvoc anal` and `pvoc synth`; its own band,
    // 5 Hz to half the sample rate, folds nothing.
    add(registry, {
        "talas.cdpFocusFold",
        "CDP Focus Fold",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The band partials move into by octaves, in either order.
            parameter("lowFrequency", "220 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "880 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -x: a folded partial takes a nearby channel when its own
            // holds a louder one.
            booleanParameter("withBody", false),
        },
    });

    // CDP `focus accu` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpFocusAccumulate",
        "CDP Focus Accumulate",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // How much of a held amplitude is left after a second.
            parameter("decay", "1", talas::expr::ValueDomain::Scalar),
            // How far a held partial glides, in octaves a second.
            parameter("glissando", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur avrg` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpBlurAverage",
        "CDP Blur Average",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The channels each mean takes, made odd.
            parameter("n", "13", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur suppress` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpBlurSuppress",
        "CDP Blur Suppress",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The loudest channels that fall silent.
            parameter("n", "16", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `hilite trace` between `pvoc anal` and `pvoc synth`; its band's
    // own defaults are 5 Hz and half the sample rate.
    add(registry, {
        "talas.cdpHiliteTrace",
        "CDP Hilite Trace",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 4: the loudest of every channel, or of those
            // above the low frequency, below the high one, or between.
            choiceParameter("mode", "all", {"all", "above", "below", "between"}),
            // The loudest channels that sound.
            parameter("n", "16", talas::expr::ValueDomain::Scalar),
            parameter("lowFrequency", "500 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "4000 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -r: while the band holds fewer than N channels, the
            // loudest outside it sound too.
            booleanParameter("retain", false),
        },
    });

    // CDP `hilite filter` between `pvoc anal` and `pvoc synth`, whose mode
    // has no default, and its defaults.
    add(registry, {
        "talas.cdpHiliteFilter",
        "CDP Hilite Filter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 12: high-pass, low-pass, band-pass or notch,
            // plain, normalised to each frame's total amplitude, or with a gain.
            choiceParameter("mode", "lowPass",
                {"highPass", "highPassNormalised", "lowPass", "lowPassNormalised", "highPassGain",
                    "lowPassGain", "bandPass", "bandPassNormalised", "notch", "notchNormalised",
                    "bandPassGain", "notchGain"}),
            // The cutoff, or the band's lower edge.
            parameter("frequency", "440 hz", talas::expr::ValueDomain::Hertz),
            // The band's upper edge, for the band-pass and notch modes.
            parameter("highFrequency", "880 hz", talas::expr::ValueDomain::Hertz),
            // How wide the filter's skirts are.
            parameter("q", "20 hz", talas::expr::ValueDomain::Hertz),
            // For the modes with a gain.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `hilite pluck` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpHilitePluck",
        "CDP Hilite Pluck",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Raises a channel as it starts to sound.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `focus step` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpFocusStep",
        "CDP Focus Step",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // How long each held spectrum lasts.
            parameter("step", "0.25 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `strange shift` between `pvoc anal` and `pvoc synth`, whose mode
    // has no default, and its defaults.
    add(registry, {
        "talas.cdpStrangeShift",
        "CDP Strange Shift",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 5: every partial, those above or below the
            // frequency, or those inside or outside the band.
            choiceParameter("mode", "all", {"all", "above", "below", "between", "outside"}),
            // How far the partials move.
            parameter("shift", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("frequency", "500 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "2000 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    // CDP `repitch transpose` between `pvoc anal` and `pvoc synth`, whose
    // mode has no default, and its defaults.
    add(registry, {
        "talas.cdpRepitchTranspose",
        "CDP Repitch Transpose",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 3: the transposition as a ratio, in octaves
            // or in semitones.
            choiceParameter("units", "semitones", {"ratio", "octaves", "semitones"}),
            parameter("transposition", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -l and -h: channels below or above them fall silent; a
            // static zero leaves the flag out.
            parameter("lowFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -x: a moved partial takes a nearby empty channel when its
            // own is taken.
            booleanParameter("fuller", false),
        },
    });

    // CDP `strange waver` between `pvoc anal` and `pvoc synth`, whose mode
    // has no default, and its defaults.
    add(registry, {
        "talas.cdpStrangeWaver",
        "CDP Strange Waver",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: the stretch grows linearly up the spectrum,
            // or by the exponent.
            choiceParameter("mode", "standard", {"standard", "specified"}),
            // How often the spectrum stretches and relaxes.
            parameter("vibrato", "8 hz", talas::expr::ValueDomain::Hertz),
            parameter("stretch", "1", talas::expr::ValueDomain::Scalar),
            // The frequency above which the spectrum stretches.
            parameter("bottomFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("exponent", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `pitch transp` between `pvoc anal` and `pvoc synth`, whose mode
    // has no default, and its defaults.
    add(registry, {
        "talas.cdpPitchTransp",
        "CDP Pitch Transp",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 6: up an octave above the split, down one
            // below it, or both; or by the transposition above, below, or
            // the first above and the second below.
            choiceParameter("mode", "octaveUp",
                {"octaveUp", "octaveDown", "octaveUpAndDown", "shiftUp", "shiftDown", "shiftUpAndDown"}),
            parameter("split", "440 hz", talas::expr::ValueDomain::Hertz),
            // In semitones.
            parameter("transposition", "0", talas::expr::ValueDomain::Scalar),
            parameter("transposition2", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -d: how much of the shift sounds, from none to all.
            booleanParameter("useDepth", false),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur chorus` between `pvoc anal` and `pvoc synth`, whose mode has
    // no default, and its defaults.
    add(registry, {
        "talas.cdpBlurChorus",
        "CDP Blur Chorus",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 7: random amplitudes, random frequencies
            // (either way, up or down), or both.
            choiceParameter("mode", "amplitude",
                {"amplitude", "frequency", "frequencyUp", "frequencyDown", "amplitudeAndFrequency",
                    "amplitudeAndFrequencyUp", "amplitudeAndFrequencyDown"}),
            parameter("amplitudeSpread", "2", talas::expr::ValueDomain::Scalar),
            parameter("frequencySpread", "1.1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur noise` between `pvoc anal` and `pvoc synth`, and their
    // defaults.
    add(registry, {
        "talas.cdpBlurNoise",
        "CDP Blur Noise",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // From the spectrum as it is to an even one.
            parameter("noise", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `stretch spectrum` between `pvoc anal` and `pvoc synth`, whose
    // mode has no default, and its defaults.
    add(registry, {
        "talas.cdpStretchSpectrum",
        "CDP Stretch Spectrum",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: the spectrum above or below the divide.
            choiceParameter("mode", "above", {"above", "below"}),
            parameter("divide", "440 hz", talas::expr::ValueDomain::Hertz),
            // The farthest channel's transposition ratio.
            parameter("maxStretch", "1.4", talas::expr::ValueDomain::Scalar),
            parameter("exponent", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -d: how much of the stretch sounds, from none to all.
            booleanParameter("useDepth", false),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur scatter` between `pvoc anal` and `pvoc synth`; its keep has
    // no default.
    add(registry, {
        "talas.cdpBlurScatter",
        "CDP Blur Scatter",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The blocks kept in each frame.
            parameter("keep", "16", talas::expr::ValueDomain::Scalar),
            // CDP's -b, rounded to whole channels; 0 Hz is CDP's default, one
            // channel.
            parameter("blockSize", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -r: a random count of blocks from 1 to keep.
            booleanParameter("randomise", false),
            // Off is CDP's -n.
            booleanParameter("normalise", true),
        },
    });

    // CDP `hilite arpeg` between `pvoc anal` and `pvoc synth`, whose mode
    // has no default, and its defaults.
    add(registry, {
        "talas.cdpHiliteArpeg",
        "CDP Hilite Arpeg",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 8: the band's channels alone or boosted among
            // the rest, boosted after a start portion that keeps the channels
            // above or below the band, the channels below or above the band's
            // middle, or a start portion only.
            choiceParameter("mode", "on",
                {"on", "boost", "belowBoost", "aboveBoost", "below", "above", "onceBelow", "onceAbove"}),
            choiceParameter("wave", "sine", {"downRamp", "sine", "saw", "upRamp"}),
            // Sweeps a second.
            parameter("rate", "1 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -p; its default in modes aboveBoost and onceAbove is 0.51.
            parameter("startPhase", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -l and -h, in either order; a high frequency of 0 Hz is
            // CDP's default, half the sample rate.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -b; 0 Hz is CDP's default, the channel width.
            parameter("bandwidth", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -a.
            parameter("amplification", "10", talas::expr::ValueDomain::Scalar),
            // CDP's -N, the exponent of each tone's decay; 1 is linear.
            parameter("decay", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -s, in frames.
            parameter("sustain", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -T: a sustained tone follows the source's frequency.
            booleanParameter("track", false),
            // CDP's -K: a tone is attacked again only once it has died.
            booleanParameter("keep", false),
        },
    });

    // CDP `hilite greq` between `pvoc anal` and `pvoc synth`; its mode and
    // data file have no default.
    add(registry, {
        "talas.cdpHiliteGraphicEq",
        "CDP Hilite Graphic EQ",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: one bandwidth for every band, or each its
            // own.
            choiceParameter("mode", "oneBandwidth", {"oneBandwidth", "bandwidths"}),
            // CDP's data file: a bandwidth in octaves, then the bands' centre
            // frequencies; or each band's frequency and bandwidth.
            textParameter("bands", "1 440 1760"),
            // CDP's -r: the bands are notches.
            booleanParameter("notch", false),
        },
    });

    // CDP `hilite band` between `pvoc anal` and `pvoc synth`; its data file
    // has no default.
    add(registry, {
        "talas.cdpHiliteBand",
        "CDP Hilite Band",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's data file: a band's low and high frequencies, a flag of
            // four digits (amplitude, amplitude ramp, transposition, added to
            // the spectrum) and the amplitudes and [+]transposition they take,
            // a line.
            textParameter("bands", "500 2000 1000 2"),
        },
    });

    // CDP `blur spread` between `pvoc anal` and `pvoc synth`; its formant
    // envelope has no default.
    add(registry, {
        "talas.cdpBlurSpread",
        "CDP Blur Spread",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            // CDP's -s: from the spectrum as it is to its formant envelope.
            parameter("spread", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `strange glis` between `pvoc anal` and `pvoc synth`; its mode,
    // formant envelope and glissando rate have no default.
    add(registry, {
        "talas.cdpStrangeGlis",
        "CDP Strange Glis",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 3: Shepard tones, an inharmonic glide, or the
            // spectrum's own partials gliding.
            choiceParameter("mode", "shepard", {"shepard", "inharmonic", "self"}),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            // Semitones a second; below zero glides down.
            parameter("glisRate", "12", talas::expr::ValueDomain::Scalar),
            // CDP's hzstep: the inharmonic glide's partial spacing.
            parameter("spacing", "50 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -t, the top of the spectrum; 0 Hz is CDP's default, none.
            parameter("topFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    // CDP `combine sum` of two files' spectra between `pvoc anal` and
    // `pvoc synth`, and its defaults.
    add(registry, {
        "talas.cdpCombineSum",
        "CDP Combine Sum",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -c: how much of the side chain's amplitude crosses over.
            parameter("crossover", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `combine diff`: the side chain's spectrum taken from the input's.
    add(registry, {
        "talas.cdpCombineDiff",
        "CDP Combine Diff",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -c: how much of the side chain's amplitude is taken away.
            parameter("crossover", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -a: amplitudes taken below zero are kept.
            booleanParameter("keepNegative", false),
        },
    });

    // CDP `combine max`: the louder of the two files' channels.
    add(registry, {
        "talas.cdpCombineMax",
        "CDP Combine Max",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `combine mean`, whose mode has no default, and its defaults.
    add(registry, {
        "talas.cdpCombineMean",
        "CDP Combine Mean",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 8: the mean, the input's, the side chain's or
            // the louder amplitude, with the mean pitch or frequency.
            choiceParameter("mode", "meanAmplitudeMeanPitch",
                {"meanAmplitudeMeanPitch", "meanAmplitudeMeanFrequency", "inputAmplitudeMeanPitch", "inputAmplitudeMeanFrequency", "sideAmplitudeMeanPitch", "sideAmplitudeMeanFrequency", "maximumAmplitudeMeanPitch", "maximumAmplitudeMeanFrequency"}),
            // CDP's -l and -h; a high frequency of 0 Hz is CDP's default, half
            // the sample rate.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -z: outside the band, the input's channels are silenced.
            booleanParameter("zeroOutside", false),
        },
    });

    // CDP `combine cross`: the side chain's amplitudes on the input's
    // frequencies, and its defaults.
    add(registry, {
        "talas.cdpCombineCross",
        "CDP Combine Cross",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -i: from the input's amplitudes to the side chain's.
            parameter("interpolation", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `formants vocode`: the side chain's formant envelope on the
    // input; its formant envelope has no default.
    add(registry, {
        "talas.cdpFormantsVocode",
        "CDP Formants Vocode",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            // CDP's -l and -h, the band vocoded; a high frequency of 0 Hz is
            // CDP's default, half the sample rate.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -g.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `morph morph` between `pvoc anal` and `pvoc synth`: the input's
    // spectrum into the side chain's; its mode and times have no default.
    add(registry, {
        "talas.cdpMorphMorph",
        "CDP Morph Morph",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: along the exponents' curves, or a cosine.
            choiceParameter("mode", "linear", {"linear", "cosine"}),
            // When the amplitudes and the frequencies move from the input's to
            // the side chain's, from the Sound's start.
            parameter("amplitudeStart", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("amplitudeEnd", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("frequencyStart", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("frequencyEnd", "1 s", talas::expr::ValueDomain::Seconds),
            parameter("amplitudeExponent", "1", talas::expr::ValueDomain::Scalar),
            parameter("frequencyExponent", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -s: the side chain enters this late.
            parameter("stagger", "0 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `morph bridge` between `pvoc anal` and `pvoc synth`: a bridge
    // from the input's spectrum towards the side chain's between two times;
    // its mode has no default, and its end, CDP's end of file, is a field.
    add(registry, {
        "talas.cdpMorphBridge",
        "CDP Morph Bridge",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 6: the level as interpolated, the softer of the
            // two, the input's, the side chain's, or moving from one's to the
            // other's.
            choiceParameter("mode", "direct", {"direct", "minimum", "followInput", "followSide", "inputToSide", "sideToInput"}),
            // CDP's -a: the side chain enters this late.
            parameter("offset", "0 s", talas::expr::ValueDomain::Seconds),
            // CDP's -b to -e: how much of the side chain's frequency and
            // amplitude the bridge takes at its start and end.
            parameter("startSideFrequency", "0", talas::expr::ValueDomain::Scalar),
            parameter("startSideAmplitude", "0", talas::expr::ValueDomain::Scalar),
            parameter("endSideFrequency", "1", talas::expr::ValueDomain::Scalar),
            parameter("endSideAmplitude", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -f and -g, from the Sound's start.
            parameter("startTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("endTime", "1 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `formants get` of the side chain then `formants put` on the input,
    // between `pvoc anal` and `pvoc synth`; its formant envelope and mode
    // have no default.
    add(registry, {
        "talas.cdpFormantsPut",
        "CDP Formants Put",
        "CDP",
        // The sound CDP's formants go on and the one `formants get` reads.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // `formants get`'s -f and -p: the formant envelope in bands of
            // equal channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // `formants get`'s -i: a quicker, less accurate search.
            booleanParameter("sideQuickSearch", false),
            // CDP's modes 1 and 2: the side chain's formants replace the
            // input's, or are imposed on them.
            choiceParameter("mode", "replace", {"replace", "impose"}),
            // `formants put`'s -i, for the input's own envelope in mode 1.
            booleanParameter("quickSearch", false),
            // CDP's -l and -h; a high frequency of 0 Hz is CDP's default, half
            // the sample rate.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -g.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `hilite vowels` between `pvoc anal` and `pvoc synth`; its vowel
    // file has no default, and its other defaults are those CDP's usage
    // suggests.
    add(registry, {
        "talas.cdpHiliteVowels",
        "CDP Hilite Vowels",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's vowel file, times from the Sound's start paired with
            // vowels (ee i ai e a ar o or u xx x oo oa aii uu n ui m r th),
            // or a single vowel.
            textParameter("vowels", "a"),
            // The formant peaks' half-width as a fraction of their centre
            // frequencies.
            parameter("halfWidth", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("steepness", "3", talas::expr::ValueDomain::Scalar),
            // The peaks' height as a part of the frame's whole range.
            parameter("range", "0.95", talas::expr::ValueDomain::Scalar),
            // Above this part of the vowel envelope, a channel is forced to
            // it.
            parameter("threshold", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `blur blur` between `pvoc anal` and `pvoc synth`, and its default.
    add(registry, {
        "talas.cdpBlurBlur",
        "CDP Blur Blur",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Frames between the kept frames the spectrum glides between.
            parameter("blur", "16", talas::expr::ValueDomain::Scalar),
            // The largest blur, read when it starts: the frames the Sound
            // waits for, as CDP8 reads ahead to each next kept frame.
            parameter("maximumBlur", "16", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `hilite bltr` between `pvoc anal` and `pvoc synth`, and its
    // defaults.
    add(registry, {
        "talas.cdpHiliteBlurTrace",
        "CDP Hilite Blur Trace",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // Frames between the kept frames the spectrum glides between.
            parameter("blur", "16", talas::expr::ValueDomain::Scalar),
            // The loudest channels that sound.
            parameter("trace", "16", talas::expr::ValueDomain::Scalar),
            // The largest blur, read when it starts: the frames the Sound
            // waits for, as CDP8 reads ahead to each next kept frame.
            parameter("maximumBlur", "16", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `focus focus` between `pvoc anal` and `pvoc synth`; its formant
    // envelope has no default.
    add(registry, {
        "talas.cdpFocusFocus",
        "CDP Focus Focus",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            // The most peaks the spectrum's energy focuses on.
            parameter("peaks", "16", talas::expr::ValueDomain::Scalar),
            // The peak-centred filters' bandwidth, in octaves.
            parameter("bandwidth", "0.333333", talas::expr::ValueDomain::Scalar),
            // CDP's -b and -t, where the peak search runs, in either order; a
            // high frequency of 0 Hz is CDP's default, half the sample rate.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -s: only peaks stable over the stability's frames, which
            // the Sound waits for.
            booleanParameter("stable", false),
            parameter("stability", "9", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `pitch tune` between `pvoc anal` and `pvoc synth`; its mode and
    // template have no default.
    add(registry, {
        "talas.cdpPitchTune",
        "CDP Pitch Tune",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: the template's pitches in Hz or as MIDI
            // note numbers.
            choiceParameter("units", "hertz", {"hertz", "midi"}),
            // CDP's data file: the pitches the spectrum is tuned to.
            textParameter("template", "440 660"),
            // CDP's -f and -c.
            parameter("focus", "1", talas::expr::ValueDomain::Scalar),
            parameter("clarity", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -t, the loudest channels tuned; 0 leaves it out.
            parameter("trace", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -b, below which nothing is tuned; 0 Hz leaves it out.
            parameter("bassCut", "0 hz", talas::expr::ValueDomain::Hertz),
        },
    });

    // CDP `pitch pick` between `pvoc anal` and `pvoc synth`; its mode and
    // fundamental have no default.
    add(registry, {
        "talas.cdpPitchPick",
        "CDP Pitch Pick",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 5: harmonics, octaves, odd harmonics, a linear
            // series or displaced harmonics of the fundamental.
            choiceParameter("mode", "harmonics",
                {"harmonics", "octaves", "oddHarmonics", "linear", "displacedHarmonics"}),
            parameter("fundamental", "440 hz", talas::expr::ValueDomain::Hertz),
            // Modes linear and displacedHarmonics: the series' step.
            parameter("step", "100 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -c.
            parameter("clarity", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `pitch chord` between `pvoc anal` and `pvoc synth`; its data file
    // has no default.
    add(registry, {
        "talas.cdpPitchChord",
        "CDP Pitch Chord",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's data file: transpositions in semitones.
            textParameter("transpositions", "-12 0 7"),
            // CDP's -b and -t, outside which channels fall silent; 5 Hz and
            // a high frequency of 0 Hz, half the sample rate, leave them out.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -x: a moved partial keeps the louder of two in its channel.
            booleanParameter("lessBody", false),
        },
    });

    // CDP `pitch chordf`, a chord that keeps the formant envelope; its
    // formant envelope and data file have no default.
    add(registry, {
        "talas.cdpPitchChordf",
        "CDP Pitch Chordf",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            // CDP's data file: transpositions in semitones.
            textParameter("transpositions", "-12 0 7"),
            // CDP's -b and -t, outside which channels fall silent; 5 Hz and
            // a high frequency of 0 Hz, half the sample rate, leave them out.
            parameter("lowFrequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -x: a moved partial keeps the louder of two in its channel.
            booleanParameter("lessBody", false),
        },
    });

    // CDP `pitch altharms` between `pvoc anal` and `pvoc synth`, reading
    // the pitch getpitch finds in a sample; its mode has no default.
    add(registry, {
        "talas.cdpPitchAltharms",
        "CDP Pitch Altharms",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The sample `repitch getpitch` finds the pitch of, over the whole
            // sample, as CDP8's pitch file is made from its input: the Input
            // should play it from its start.
            resourceParameter("pitchSource", {}),
            // getpitch's -t, -g, -s, -n, -l and -h; a highest pitch of 0 Hz is
            // CDP's default, an eighth of half the sample rate.
            parameter("pitchTuning", "1", talas::expr::ValueDomain::Scalar),
            parameter("pitchedWindows", "2", talas::expr::ValueDomain::Scalar),
            parameter("signalToNoise", "80", talas::expr::ValueDomain::Scalar),
            parameter("harmonics", "5", talas::expr::ValueDomain::Scalar),
            parameter("lowestPitch", "10 hz", talas::expr::ValueDomain::Hertz),
            parameter("highestPitch", "0 hz", talas::expr::ValueDomain::Hertz),
            // getpitch's -a and -z.
            booleanParameter("alternativePitch", false),
            booleanParameter("keepUnpitched", false),
            // CDP's modes 1 and 2: odd or even harmonics deleted.
            choiceParameter("mode", "deleteOdd", {"deleteOdd", "deleteEven"}),
            // CDP's -x.
            booleanParameter("lessBody", false),
        },
    });

    // CDP `pitch octmove`, reading the pitch getpitch finds in a sample;
    // its mode and transposition have no default.
    add(registry, {
        "talas.cdpPitchOctmove",
        "CDP Pitch Octmove",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The sample `repitch getpitch` finds the pitch of, over the whole
            // sample, as CDP8's pitch file is made from its input: the Input
            // should play it from its start.
            resourceParameter("pitchSource", {}),
            // getpitch's -t, -g, -s, -n, -l and -h; a highest pitch of 0 Hz is
            // CDP's default, an eighth of half the sample rate.
            parameter("pitchTuning", "1", talas::expr::ValueDomain::Scalar),
            parameter("pitchedWindows", "2", talas::expr::ValueDomain::Scalar),
            parameter("signalToNoise", "80", talas::expr::ValueDomain::Scalar),
            parameter("harmonics", "5", talas::expr::ValueDomain::Scalar),
            parameter("lowestPitch", "10 hz", talas::expr::ValueDomain::Hertz),
            parameter("highestPitch", "0 hz", talas::expr::ValueDomain::Hertz),
            // getpitch's -a and -z.
            booleanParameter("alternativePitch", false),
            booleanParameter("keepUnpitched", false),
            // CDP's modes 1 to 3: up, down, or down with the bass boosted.
            choiceParameter("mode", "up", {"up", "down", "downBassBoost"}),
            // The transposition, a whole number of times the pitch.
            parameter("transposition", "2", talas::expr::ValueDomain::Scalar),
            // Mode downBassBoost's boost.
            parameter("bassBoost", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `spec bare`, keeping the channels near the harmonics of the pitch
    // getpitch finds in a sample.
    add(registry, {
        "talas.cdpSpecBare",
        "CDP Spec Bare",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // The sample `repitch getpitch` finds the pitch of, over the whole
            // sample, as CDP8's pitch file is made from its input: the Input
            // should play it from its start.
            resourceParameter("pitchSource", {}),
            // getpitch's -t, -g, -s, -n, -l and -h; a highest pitch of 0 Hz is
            // CDP's default, an eighth of half the sample rate.
            parameter("pitchTuning", "1", talas::expr::ValueDomain::Scalar),
            parameter("pitchedWindows", "2", talas::expr::ValueDomain::Scalar),
            parameter("signalToNoise", "80", talas::expr::ValueDomain::Scalar),
            parameter("harmonics", "5", talas::expr::ValueDomain::Scalar),
            parameter("lowestPitch", "10 hz", talas::expr::ValueDomain::Hertz),
            parameter("highestPitch", "0 hz", talas::expr::ValueDomain::Hertz),
            // getpitch's -a and -z.
            booleanParameter("alternativePitch", false),
            booleanParameter("keepUnpitched", false),
            // CDP's -x.
            booleanParameter("lessBody", false),
        },
    });

    // CDP `spec clean` between `pvoc anal` and `pvoc synth`, silencing the
    // channels that fall below the loudest the analysis of a noise sample
    // gives them; its mode has no default.
    add(registry, {
        "talas.cdpSpecClean",
        "CDP Spec Clean",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 4: a channel silenced from the time it first
            // falls below its noise level, anywhere it does, anywhere it
            // does above the frequency, or throughout where the good signal
            // never rises above it.
            choiceParameter("mode", "fromTime", {"fromTime", "anywhere", "filtering", "comparing"}),
            // CDP's nfile and gfile: samples of the noise and of the good
            // signal, analysed whole when the Sound starts.
            resourceParameter("noiseSource", {}),
            resourceParameter("signalSource", {}),
            // Modes fromTime's and anywhere's skip time, mode filtering's
            // frequency, and CDP's -g.
            parameter("skipTime", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("frequency", "5 hz", talas::expr::ValueDomain::Hertz),
            parameter("noiseGain", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `strange invert` between `pvoc anal` and `pvoc synth`, reading
    // the loudest amplitudes of a sample's whole analysis first, as CDP
    // reads its input; its mode has no default.
    add(registry, {
        "talas.cdpStrangeInvert",
        "CDP Strange Invert",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 and 2: amplitudes inverted, or their shares of a
            // frame's total, keeping the total.
            choiceParameter("mode", "normal", {"normal", "keepAmplitude"}),
            // The sample CDP reads first, its input: the Input should play
            // it from its start.
            resourceParameter("normalisingSource", {}),
        },
    });

    // CDP `focus freeze` between `pvoc anal` and `pvoc synth`; its mode and
    // data file have no default.
    add(registry, {
        "talas.cdpFocusFreeze",
        "CDP Focus Freeze",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 3: amplitudes, frequencies or both frozen.
            choiceParameter("mode", "amplitudes", {"amplitudes", "frequencies", "both"}),
            // CDP's data file: times in seconds, `a` freezing the segment
            // after a time on its window, `b` the segment before; the Sound
            // waits for the longest `b` segment, at most 1024 windows.
            textParameter("times", "a0.25 0.75"),
        },
    });

    // CDP `filter varibank2`, a bank of time-varying pitches each with
    // time-varying partials, and its defaults; its data file has none.
    add(registry, {
        "talas.cdpVaryingBank2",
        "CDP Varying Bank 2",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's mode: the pitches in Hz or as MIDI note numbers.
            choiceParameter("pitches", "hertz", {"hertz", "midi"}),
            // CDP's data file, its two sections joined by a # line: lines
            // of a time and pitch and amplitude pairs, then lines of a time
            // and partial number and amplitude pairs.
            textParameter("bands", "0 440 1"),
            textParameter("partials", "0 1 1 2 0.5"),
            parameter("q", "20", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            booleanParameter("doubleFiltering", false),
        },
    });

    // CDP `tremolo tremolo`, the newer tremolo with its fineness, and its
    // defaults; its mode has none.
    add(registry, {
        "talas.cdpTremoloFine",
        "CDP Tremolo Fine",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 and 2: a live frequency read linearly or, as CDP
            // reads its breakpoint file, by its logarithm.
            choiceParameter("frequencyScale", "linear", {"linear", "logarithmic"}),
            parameter("frequency", "15 hz", talas::expr::ValueDomain::Hertz),
            parameter("depth", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            // How many times each sample is multiplied by the gain.
            parameter("fineness", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `distmore double` with its default multiplier of 1: each waveset
    // at twice its frequency in its own length. CDP's multipliers 2 to 4
    // give each waveset a new length, which a stream cannot follow.
    add(registry, {
        "talas.cdpDistmoreDouble",
        "CDP Distmore Double",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    // CDP `clip clip` mode 2 and its default: each half-waveset clipped at
    // a fraction of its own peak, the whole raised by one over the fraction.
    add(registry, {
        "talas.cdpClipWaveset",
        "CDP Clip Waveset",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("fraction", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `quirk quirk` mode 1: each half-waveset over its peak raised to a
    // power, under 1 exaggerating its contour and over 1 smoothing it;
    // CDP's default of 1 leaves the sound as it is.
    add(registry, {
        "talas.cdpQuirkWaveset",
        "CDP Quirk Waveset",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("exponent", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `rmverb`, which has no defaults; these are its port's, a small
    // room with its own predelay and early reflections.
    add(registry, {
        "talas.cdpRoomReverb",
        "CDP Room Reverb",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // CDP's rmsize: Gardner's small, medium and large rooms.
            choiceParameter("room", "small", {"small", "medium", "large"}),
            parameter("reverbGain", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("feedback", "0.5", talas::expr::ValueDomain::Scalar),
            // The lowpass in the diffuser's feedback (air absorption) and
            // the one on the reverberator's input; 0 Hz leaves either out.
            parameter("absorb", "2500 hz", talas::expr::ValueDomain::Hertz),
            parameter("lowpass", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -p: where the first reflection falls; a negative
            // predelay keeps the room's own.
            parameter("predelay", "-1 s", talas::expr::ValueDomain::Seconds),
            // CDP's -L and -H, 12 dB shelves on the input; 0 Hz leaves them
            // out.
            parameter("lowCut", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("highCut", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -d: the absorption lowpass inside each nested allpass.
            booleanParameter("doubleDamping", false),
            // CDP's -c1: one output, written to both.
            booleanParameter("monoOutput", false),
            // CDP's -e file: lines of a time in seconds and an amplitude
            // above 0 up to 1 in place of the room's early reflections,
            // which empty keeps.
            textParameter("reflections", ""),
        },
    });

    // CDP `modify radical` mode 7, QUANTISE: each sample rounded to the
    // middle of a step at a bit resolution. CDP's default of 16 bits leaves
    // little to hear; this default is the port's.
    add(registry, {
        "talas.cdpQuantise",
        "CDP Quantise",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP takes it as fixed; it may change as the Sound plays.
            parameter("bitResolution", "8", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify loudness` modes 1 and 2, GAIN and dBGAIN, and CDP's
    // default factor of 1.
    add(registry, {
        "talas.cdpLoudnessGain",
        "CDP Loudness Gain",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 and 2: the gain as a factor or in decibels.
            choiceParameter("scale", "factor", {"factor", "decibels"}),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            // Off is CDP's CDP_NOCLIP_FLOATS: samples beyond 1 stay as
            // they are.
            booleanParameter("clip", true),
        },
    });

    // CDP's two phase inversions, each sample negated.
    add(registry, {
        "talas.cdpPhaseInvert",
        "CDP Phase Invert",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // `modify loudness 6`, which holds an inverted sample above 1 at
            // 1, or `phase phase 1`, which holds nothing.
            choiceParameter("mode", "phase", {"loudness", "phase"}),
        },
    });

    // CDP `housekeep extract` mode 4, RECTIFY: every sample shifted, to
    // remove a DC offset. CDP's default of 0 it refuses as no change.
    add(registry, {
        "talas.cdpDcShift",
        "CDP DC Shift",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("shift", "0.1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify radical` mode 6, CROSS MODULATE: the input times the side
    // chain.
    add(registry, {
        "talas.cdpCrossModulate",
        "CDP Cross Modulate",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    // CDP `subtract subtract`: the input less the side chain.
    add(registry, {
        "talas.cdpSubtract",
        "CDP Subtract",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {},
    });

    // CDP `submix balance` and its default: the input times the balance plus
    // the side chain times one less the balance.
    add(registry, {
        "talas.cdpMixBalance",
        "CDP Mix Balance",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's -k: 1 plays the input alone, 0 the side chain.
            parameter("balance", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `submix crossfade` and its defaults: from the input to the side
    // chain between two times from the Sound's start; CDP's end, the
    // shorter file's end, is a field.
    add(registry, {
        "talas.cdpMixCrossfade",
        "CDP Mix Crossfade",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 and 2: a linear fade or a raised cosine.
            choiceParameter("contour", "linear", {"linear", "cosine"}),
            // CDP's -b and -e.
            parameter("begin", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("end", "1 s", talas::expr::ValueDomain::Seconds),
            // CDP's -s: the side chain enters this late.
            parameter("stagger", "0 s", talas::expr::ValueDomain::Seconds),
            // CDP's -p: the cosine's position raised to this power first.
            parameter("powerFactor", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `fastconv` with a soundfile impulse response: the input convolved
    // with an impulse sample, scaled by CDP's gain for the sample's length
    // and level, and mixed with the dry input.
    add(registry, {
        "talas.cdpFastConvolution",
        "CDP Fast Convolution",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's impulse soundfile, read whole when the Sound starts, at
            // the Sound's sample rate; the output waits its frames less one.
            resourceParameter("impulse", {}),
            // CDP's dry, 0 to 1: the input's share against the convolution's
            // (1 leaves silence, as in CDP).
            parameter("dry", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -a: the impulse response scaled before its level is
            // measured.
            parameter("scale", "1", talas::expr::ValueDomain::Scalar),
            // The channel of a stereo impulse sample convolved, CDP's output
            // channel; the louder channel sets the gain.
            parameter("impulseChannel", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP's standalone `gate gate` mode 1: a stretch under the gate longer
    // than two 1 ms splices silenced between them. CDP's gate has no
    // default.
    add(registry, {
        "talas.cdpSilenceGate",
        "CDP Silence Gate",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // In decibels, -96 to 0.
            parameter("gate", "-40", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `housekeep deglitch`, which has no defaults: a burst of at most
    // Glitch with Silence on both sides cut out between two splices.
    add(registry, {
        "talas.cdpDeglitch",
        "CDP Deglitch",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("glitch", "2 ms", talas::expr::ValueDomain::Seconds),
            parameter("silence", "10 ms", talas::expr::ValueDomain::Seconds),
            // The level, a mean over the window and each sample, at or under
            // which the input is quiet.
            parameter("threshold", "0.01", talas::expr::ValueDomain::Scalar),
            parameter("splice", "1 ms", talas::expr::ValueDomain::Seconds),
            parameter("window", "1 ms", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `sfedit noisecut`, which has no defaults: noise, wavesets shorter
    // than one over Noise Frequency, replaced by silence.
    add(registry, {
        "talas.cdpNoiseCut",
        "CDP Noise Cut",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("splice", "2 ms", talas::expr::ValueDomain::Seconds),
            parameter("noiseFrequency", "6000 hz", talas::expr::ValueDomain::Hertz),
            // The noise and the tone a switch must follow for CDP to keep it.
            parameter("maxNoise", "5 ms", talas::expr::ValueDomain::Seconds),
            parameter("minTone", "15 ms", talas::expr::ValueDomain::Seconds),
            // CDP's -n: the tone silenced instead.
            booleanParameter("keepNoise", false),
        },
    });

    // CDP `envel attack` modes 1 to 3, which have no defaults: one attack
    // after the Sound's start boosted by Gain, the gain rising over Onset and
    // falling over Decay.
    add(registry, {
        "talas.cdpAttackBoost",
        "CDP Attack Boost",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 1 to 3: the attack where the input first passes the
            // gate (or the loudest sample at its peak), the loudest sample
            // in the 400 ms around a time, or that time itself.
            choiceParameter("mode", "gate", {"gate", "approximateTime", "exactTime"}),
            // The gate, 0 to 1, or the time in seconds from the Sound's
            // start.
            parameter("level", "0.1", talas::expr::ValueDomain::Scalar),
            parameter("gain", "2", talas::expr::ValueDomain::Scalar),
            parameter("onset", "20 ms", talas::expr::ValueDomain::Seconds),
            parameter("decay", "50 ms", talas::expr::ValueDomain::Seconds),
            // CDP's -t1 (its default) and -t0.
            choiceParameter("curve", "exponential", {"exponential", "linear"}),
        },
    });

    // CDP `submix merge` and its defaults: the input times Skew / (Skew + 1)
    // plus the side chain times one less that.
    add(registry, {
        "talas.cdpMixMerge",
        "CDP Mix Merge",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's -s: the side chain enters this late, the input playing
            // alone until then; up to 10 s, rounded down to 256-frame
            // sectors as CDP rounds it.
            parameter("stagger", "0 s", talas::expr::ValueDomain::Seconds),
            // CDP's -k: the input's level against the side chain's, from
            // 1/32767 to 32767.
            parameter("skew", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -j: the side chain starts this far in, at most the
            // stagger, in sectors as the stagger.
            parameter("skip", "0 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `submix faders` with two files: the input and the side chain at
    // the levels of a balance table, ramped from line to line, summed and
    // multiplied by Gain.
    add(registry, {
        "talas.cdpMixFaders",
        "CDP Mix Faders",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's balance file: a line for each time, from 0, in seconds
            // from the Sound's start, then the input's level and the side
            // chain's; each line's levels are scaled to sum to 1, and a line
            // starting with a semicolon is a remark.
            textParameter("balance", "0 1 1"),
            // CDP's gain, 0 to 256, which it may take as a breakpoint file.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `submix interleave` with two mono files, and `repair` with two and
    // 2 channels: the input as the left channel and the side chain as the
    // right.
    add(registry, {
        "talas.cdpMixInterleave",
        "CDP Mix Interleave",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {},
    });

    // CDP `submix inbetween`, one of its output files: the input times one
    // less Ratio plus the side chain times Ratio.
    add(registry, {
        "talas.cdpMixInbetween",
        "CDP Mix Inbetween",
        "CDP",
        // CDP's first file and its second.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 2 and 1: a ratio from its ratios file, or the
            // File-th of Count files between the two.
            choiceParameter("mode", "ratio", {"ratio", "count"}),
            // From 0, the input alone, to 1, the side chain alone.
            parameter("ratio", "0.5", talas::expr::ValueDomain::Scalar),
            // Whole numbers, 1 <= File <= Count <= 999: the ratio is File
            // times 1 / (Count + 1).
            parameter("count", "1", talas::expr::ValueDomain::Scalar),
            parameter("file", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `envel warp` modes 3, 4, 5, 7, 8, 9, 10, 11, 12 and 15: the
    // input's own envelope, the peak of each window, warped and imposed on
    // it, the gain ramping across each window to the next.
    add(registry, {
        "talas.cdpEnvelopeWarp",
        "CDP Envelope Warp",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 3 (exaggerate), 4 (attenuate), 5 (lift), 7
            // (flatten), 8 (gate), 9 (invert), 10 (limit), 11 (corrugate),
            // 12 (expand) and 15 (ducked).
            choiceParameter("mode", "exaggerate",
                {"exaggerate", "attenuate", "lift", "flatten", "gate", "invert", "limit", "corrugate", "expand",
                    "ducked"}),
            // CDP's wsize, 5 to 10000 ms, rounded to multiples of 256 frames
            // (under 256, to a power-of-two fraction of 256); the output
            // waits two windows less a frame, and the windows a mode reads
            // ahead.
            parameter("window", "5 ms", talas::expr::ValueDomain::Seconds),
            // CDP's first parameter, which it may take as a breakpoint file
            // and reads at each window's start: exaggerate 0.01 to 100;
            // attenuate, lift, gate, invert's gate, limit, expand's gate and
            // ducked's gate 0 to 1; flatten 2 to 5000 windows; corrugate's
            // trough delay 1 to 32767 windows (both rounded).
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            // CDP's second parameter, read as the first: invert's mirror
            // (above its gate), limit's threshold (at most its limit),
            // expand's threshold (at least its gate) and ducked's threshold,
            // 0 to 1; corrugate's peak separation, 2 to 32767 windows. The
            // other modes have none.
            parameter("second", "0", talas::expr::ValueDomain::Scalar),
            // CDP's smoothing for gate and expand: the gate follows the mean
            // of this many windows from each, 0 (none) to 32767, which the
            // output waits for less one.
            parameter("smoothing", "0", talas::expr::ValueDomain::Scalar),
            // The largest flatten or trough delay a live first, and peak
            // separation a live second, may take (CDP's flatten reads ahead
            // by its breakpoint file's largest value): the windows the output
            // waits for, read only when the field is live.
            parameter("maximumFirst", "0", talas::expr::ValueDomain::Scalar),
            parameter("maximumSecond", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `envel impose` and `envel replace` mode 1, the second sound's
    // envelope taken from the Side Chain: the input times it, or the input's
    // own envelope replaced by it.
    add(registry, {
        "talas.cdpEnvelopeImpose",
        "CDP Envelope Impose",
        "CDP",
        // CDP's sound and its second sound, at the same rate.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // envel impose 1 or envel replace 1.
            choiceParameter("mode", "impose", {"impose", "replace"}),
            // CDP's wsize, as CDP Envelope Warp's; the output waits two
            // windows less a frame.
            parameter("window", "5 ms", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `envel replace` modes 3 and 4: the input's envelope replaced by a
    // target, as a level or in decibels.
    add(registry, {
        "talas.cdpEnvelopeTarget",
        "CDP Envelope Target",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's modes 3 and 4: the target from 0 to 1, or from -96 to 0
            // dB (-96 silent).
            choiceParameter("scale", "level", {"level", "decibels"}),
            // CDP's wsize, as CDP Envelope Warp's; the output waits two
            // windows less a frame.
            parameter("window", "5 ms", talas::expr::ValueDomain::Seconds),
            // CDP's breakpoint file of the envelope, read at each window's
            // start.
            parameter("target", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `newdelay newdelay`: a comb tuned to a MIDI pitch, its delay one
    // period of the pitch, its line feeding back on itself.
    add(registry, {
        "talas.cdpPitchedDelay",
        "CDP Pitched Delay",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's Midipitch, a MIDI note number from 0.1 Hz's (about
            // -76.24) to the Nyquist frequency's, which CDP may take as a
            // breakpoint file and reads at every frame.
            parameter("pitch", "30", talas::expr::ValueDomain::Scalar),
            // CDP's mix of the line against the input, 0.001 to 1.
            parameter("mix", "0.5", talas::expr::ValueDomain::Scalar),
            // CDP's feedback, -1 to 1.
            parameter("feedback", "0.7", talas::expr::ValueDomain::Scalar),
            // The lowest pitch a live pitch may take, which sizes the line
            // (CDP sizes its line by its breakpoint file's lowest pitch);
            // read only when the pitch is live.
            parameter("lowestPitch", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `bounce bounce ... -s0`: the input followed by copies of itself,
    // closer and closer together and quieter and quieter, like a bouncing
    // ball.
    add(registry, {
        "talas.cdpBounce",
        "CDP Bounce",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // The copies, 1 to 100 (fewer play where the gaps fall under
            // half CDP's 3 ms splice).
            parameter("count", "8", talas::expr::ValueDomain::Scalar),
            // The first copy's delay, 0.04 to 10 s.
            parameter("startGap", "1 s", talas::expr::ValueDomain::Seconds),
            // Each gap after the first is the last times this, 0.1 to 1.
            parameter("shorten", "0.92", talas::expr::ValueDomain::Scalar),
            // The last copy's level, 0 to 1, the levels falling to it from 1
            // along a power of Warp, 0.01 to 100.
            parameter("endLevel", "0.05", talas::expr::ValueDomain::Scalar),
            parameter("warp", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `rejoin rejoin` modes 1 and 2 of two mono files, without -r: their
    // sum, each times Gain.
    add(registry, {
        "talas.cdpRejoin",
        "CDP Rejoin",
        "CDP",
        // CDP's longest file and the other.
        {input("audioIn", PortType::Audio), input("sideChain", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // Above 0 to 1.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `cascade cascade` modes 1 to 5: the input cut into clips, each
    // clip followed by echoes of itself laid over the input from its place.
    add(registry, {
        "talas.cdpCascade",
        "CDP Cascade",
        "CDP",
        {input("audioIn", PortType::Audio)},
        // Left and right play one or two channels (one on both) and are
        // silent with eight; out1 to out8 play CDP's channels, the others
        // silent.
        cdpStereoAndChannelOutputs(),
        {
            // CDP's modes 1 (the echoes in one channel, on both outputs), 2
            // (each echo set panned left to right), 3 (from the centre,
            // right for one clip and left for the next), 4 (eight channels,
            // each echo a channel on) and 5 (as 4, turning back every other
            // clip).
            choiceParameter("mode", "echoes", {"echoes", "panRight", "alternate", "rotate", "rotateBack"}),
            // The clip, 0.005 to 60 s; the output waits for the longest clip
            // and two 5 ms splices, less a frame.
            parameter("clipSize", "0.5 s", talas::expr::ValueDomain::Seconds),
            // The echoes of each clip, 1 to 64, falling to 0.01.
            parameter("echoes", "8", talas::expr::ValueDomain::Scalar),
            // With a clip maximum (not 0), each clip's size is drawn between
            // clipSize and it, and with an echoes maximum (not 0) each echo
            // count between echoes and it.
            parameter("clipMax", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("echoesMax", "0", talas::expr::ValueDomain::Scalar),
            // Each echo but the first and last moved by up to this times the
            // clip, 0 to 1.
            parameter("rand", "0", talas::expr::ValueDomain::Scalar),
            // CDP's draws' seed, 1 to 64 (0 when nothing is drawn).
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -l: the echoes fall by equal steps.
            booleanParameter("linear", false),
        },
    });

    // CDP `phasor phasor`: copies of the input drifting out of phase and
    // back, spread over one to eight output channels.
    add(registry, {
        "talas.cdpPhasor",
        "CDP Phasor",
        "CDP",
        {input("audioIn", PortType::Audio)},
        // Left and right play one or two channels (one on both) and are
        // silent with more; out1 to out8 play CDP's channels, the others
        // silent.
        cdpStereoAndChannelOutputs(),
        {
            // CDP's streams, 2 to 8.
            parameter("streams", "2", talas::expr::ValueDomain::Scalar),
            // CDP's Phasfrq, 0.01 to 100 Hz, and Shift, 0 to 12 semitones,
            // which CDP may take as breakpoint files and reads as each packet
            // of a period starts.
            parameter("frequency", "1 hz", talas::expr::ValueDomain::Hertz),
            parameter("shift", "0.5", talas::expr::ValueDomain::Scalar),
            // CDP's output channels, 1 to 8 and at most the streams.
            parameter("outputChannels", "2", talas::expr::ValueDomain::Scalar),
            // CDP's -o: each stream later by up to this, 0 to 500 ms.
            parameter("offset", "0 ms", talas::expr::ValueDomain::Seconds),
            // The lowest frequency a live frequency may take, above 0 and at
            // most its starting value (CDP sizes its buffer by its breakpoint
            // file's lowest): the packet the output waits for. Read only when
            // the frequency is live.
            parameter("lowestFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -s: the streams around a circle of 3 or more channels.
            booleanParameter("surround", false),
        },
    });

    // CDP `spin stereo` modes 1 to 3 with no doppler: the stereo image spun
    // round the listener, the edge in front raised and the one behind
    // lowered.
    add(registry, {
        "talas.cdpSpin",
        "CDP Spin",
        "CDP",
        // The stereo input's left and right channels.
        {input("audioIn", PortType::Audio), input("rightIn", PortType::Audio)},
        // Left and right play mode 1's two channels and are silent in the
        // wide modes; out1 to out8 play CDP's channels, the others silent.
        cdpStereoAndChannelOutputs(),
        {
            // Cycles a second, -100 to 100 (below 0 the other way), which
            // CDP may take as a breakpoint file and reads at every frame.
            parameter("rate", "1", talas::expr::ValueDomain::Scalar),
            // The edge in front raised by up to Boost + 1 times, 0 to 16, and
            // both lowered by up to Attenuation, 0 to 1.
            parameter("boost", "2", talas::expr::ValueDomain::Scalar),
            parameter("attenuation", "0", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 (two channels), 2 (the edges panned between the
            // channels either side of the centre, which takes the mono mix
            // from the centre minimum to the centre maximum) and 3 (each
            // edge panned across the three channels around the centre, the
            // mix added there at the centre minimum); the wide modes come a
            // frame late.
            choiceParameter("mode", "stereo", {"stereo", "wide", "wideBareCentre"}),
            // The wide modes' channels, 4 to 8, their centre, 1 to the
            // channels, and its levels, 0 to 1.
            parameter("channels", "8", talas::expr::ValueDomain::Scalar),
            parameter("centre", "1", talas::expr::ValueDomain::Scalar),
            parameter("centreMinimum", "0", talas::expr::ValueDomain::Scalar),
            parameter("centreMaximum", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify brassage` modes 1, 3, 4 and 5: the input cut into grains
    // copied back over the output at about their own place, transposed
    // (pitchshift), spread across the outputs at a density, quieter and from
    // a little earlier (reverb), of a size and from earlier (scramble), or at
    // a density (granulate).
    add(registry, {
        "talas.cdpBrassage",
        "CDP Brassage",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("leftOut", PortType::Audio), output("rightOut", PortType::Audio)},
        {
            // CDP's modes 1 (pitchshift: Pitch), 3 (reverb: Density, Pitch,
            // Amp and Range, on two output channels), 4 (scramble: Grainsize
            // and Range) and 5 (granulate: Density); one channel plays on
            // both outputs. A field the mode does not take is ignored.
            choiceParameter("mode", "pitchshift", {"pitchshift", "reverb", "scramble", "granulate"}),
            // Semitones: pitchshift within log2(Nyquist / 10 Hz) octaves,
            // reverb -0.33 to 0.33. CDP may take each field as a breakpoint
            // file, read at each grain's start.
            parameter("pitch", "0", talas::expr::ValueDomain::Scalar),
            // Grains per grain length: reverb 2 to 16384 (CDP's default 8),
            // granulate 0.000002 to 2.
            parameter("density", "2", talas::expr::ValueDomain::Scalar),
            // Reverb's grain level, 0 to 1.
            parameter("amp", "1", talas::expr::ValueDomain::Scalar),
            // Scramble's grain, from 2 ms (CDP's grain is 50 ms in the other
            // modes).
            parameter("grainsize", "50 ms", talas::expr::ValueDomain::Seconds),
            // How far before its place a grain may be read: reverb 0 to
            // 50 ms, scramble from 0.
            parameter("range", "0 ms", talas::expr::ValueDomain::Seconds),
            // The bounds a live pitch, density, grainsize and range may take
            // (CDP sizes its buffers by its breakpoint files' highest pitch,
            // lowest density and longest grain and range), no nearer than
            // their starting values: the pitch and grainsize set the latency.
            // Each is read only when its field is live.
            parameter("highestPitch", "0", talas::expr::ValueDomain::Scalar),
            parameter("lowestDensity", "0", talas::expr::ValueDomain::Scalar),
            parameter("longestGrainsize", "0 ms", talas::expr::ValueDomain::Seconds),
            parameter("longestRange", "0 ms", talas::expr::ValueDomain::Seconds),
            // The draws' seed, a whole number from 0 to 4294967295 (CDP
            // seeds from the time).
            parameter("seed", "1", talas::expr::ValueDomain::Scalar),
            // CDP_MEMORY_BBSIZE, whole kilobytes from 10 to 20480, which
            // sizes CDP's input buffer that its transposed reads count from.
            parameter("memoryKilobytes", "1024", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP's multichannel tools (owner plan 2026-10-12): each plays CDP's
    // channels on out1 to outN of its eight outputs, the others silent; a
    // tool that takes N channels takes them on in1 to inN, connected without
    // a gap, and plays as many. Fields CDP may take as breakpoint files are
    // said so; the others are read once, when it starts.
    //
    // CDP `mchanpan mchanpan 9`: a mono input rotating around a ring of
    // output channels.
    add(registry, {
        "talas.cdpRotatingPan",
        "CDP Rotating Pan",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            // The ring's output channels, 3 to 8.
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // The channel it starts on, 0 to the channels (between 0 and 1,
            // between the last channel and the first).
            parameter("start", "1", talas::expr::ValueDomain::Scalar),
            // Turns a second, 0 to 64, which CDP may take as a breakpoint
            // file and reads every 256 frames.
            parameter("speed", "1", talas::expr::ValueDomain::Scalar),
            // 0 to 1: below 1, (1 - focus) / 2 of the signal goes to the
            // channels beyond the two it sits between.
            parameter("focus", "1", talas::expr::ValueDomain::Scalar),
            // Down the channel numbers.
            booleanParameter("anticlockwise", false),
        },
    });

    // CDP `mchanpan mchanpan 4` (panspread): a mono or stereo input spread
    // from a centre channel over a ring of output channels.
    add(registry, {
        "talas.cdpSpreadPan",
        "CDP Spread Pan",
        "CDP",
        // A connected Right Input makes the input stereo.
        {input("audioIn", PortType::Audio), optionalInput("rightIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // The centre channel, a whole number from 0 (the last channel)
            // to 8 and the channels.
            parameter("centre", "1", talas::expr::ValueDomain::Scalar),
            // The channels spread over, 0 to 16; the channels used each side
            // behind its edges, 0 to 8 (below half the spread a hole opens
            // in the middle); the level's fall as the spread widens, 0 to 1.
            // CDP may take each as a breakpoint file, read every 256 frames.
            parameter("spread", "2", talas::expr::ValueDomain::Scalar),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
            parameter("rolloff", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `mchanpan mchanpan 1`: a mono input moved around a ring of output
    // channels along a table from the stream's start.
    add(registry, {
        "talas.cdpTablePan",
        "CDP Table Pan",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // Rows of time (seconds), position (0 to the channels) and how
            // it moves on to the next row: 0 direct (whole channels), 1
            // clockwise, -1 anticlockwise; rows end at '/' or a line break.
            textParameter("table", "0 1 1 / 1 4 1"),
            parameter("focus", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `mchanpan mchanpan 3` (stepspread): each event of a mono input
    // (sound after a long enough silence) spread from a centre channel and
    // held until the next.
    add(registry, {
        "talas.cdpStepSpread",
        "CDP Step Spread",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // The centre, 0.5 to 8 and the channels; the spread, 1 to 16;
            // the depth, 0 to 8; the rolloff, 0 to 1. CDP may take each as
            // a breakpoint file: each event takes them as it starts.
            parameter("centre", "1", talas::expr::ValueDomain::Scalar),
            parameter("spread", "2", talas::expr::ValueDomain::Scalar),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
            parameter("rolloff", "0", talas::expr::ValueDomain::Scalar),
            // The exact zeros, 2 to 32767 frames long, that end an event.
            parameter("minimumSilence", "10 ms", talas::expr::ValueDomain::Seconds),
            // CDP's -s: the first event on one channel, each later one a
            // channel wider each side up to the spread (static centre,
            // spread and depth).
            booleanParameter("stepped", false),
        },
    });

    // CDP `mchanpan mchanpan 7` (crosspan): a multichannel input moved from
    // one channel map to the next along a table.
    add(registry, {
        "talas.cdpCrossPan",
        "CDP Cross Pan",
        "CDP",
        cdpChannelInputs(2),
        cdpChannelOutputs(),
        {
            // Rows of a time (seconds) and, for each input, its output
            // channel (all 0: the centre); rows end at '/' or a line break.
            textParameter("table", "0 1 2 / 1 2 1"),
            // 0 to 1: how far a spread's covered channels rise from 1/S.
            parameter("rolloff", "1", talas::expr::ValueDomain::Scalar),
            // CDP's buffer, whose ends lose a wrapping spread's spill; 0 is
            // CDP's 131072 / N frames.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
            // Plays what CDP evidently intends (no CDP reference): a held
            // map sends input n to channel map[n], nothing spills, and a
            // table may start after 0.
            booleanParameter("corrected", false),
        },
    });

    // CDP `mchanpan mchanpan 6` (antiphony): the input switched back and
    // forth between two sets of output channels, an event at a time.
    add(registry, {
        "talas.cdpAntiphony",
        "CDP Antiphony",
        "CDP",
        // A connected Right Input makes the input stereo; with a Side Chain
        // (of as many channels), the events alternate between the input and
        // it too.
        {input("audioIn", PortType::Audio), optionalInput("rightIn", PortType::Audio),
            optionalInput("sideChain", PortType::Audio), optionalInput("sideChainRight", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // CDP's two sets of channel letters, a for channel 1, as ab-cd.
            textParameter("sets", "ab-cd"),
            // 0.05 to 60 s and 0 to 60 s, which CDP may take as breakpoint
            // files: each event's splice in ending takes them for the next.
            parameter("eventDuration", "0.25 s", talas::expr::ValueDomain::Seconds),
            parameter("gap", "0 s", talas::expr::ValueDomain::Seconds),
            // Up to 1000 ms.
            parameter("splice", "5 ms", talas::expr::ValueDomain::Seconds),
            // CDP's buffer, whose ends lose channel N's spill; 0 is CDP's
            // 1 MB of samples.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
            // Plays what CDP evidently intends (no CDP reference): every
            // channel of each set, on its own channel.
            booleanParameter("corrected", false),
        },
    });

    // CDP `mchanrev mchanrev`: a mono input heard again through CDP's
    // stadium echoes, spread around a ring of output channels.
    add(registry, {
        "talas.cdpMultichannelStadium",
        "CDP Multichannel Stadium",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            parameter("gain", "0.645654", talas::expr::ValueDomain::Scalar),
            // Below 1, flattens the echoes' fall in level.
            parameter("rolloff", "1", talas::expr::ValueDomain::Scalar),
            // Scales the echoes' times.
            parameter("size", "1", talas::expr::ValueDomain::Scalar),
            // 2 to 1000 echoes.
            parameter("count", "23", talas::expr::ValueDomain::Scalar),
            // The echoes' centre, 0 to the channels, and spread, 2 to the
            // channels.
            parameter("centre", "1", talas::expr::ValueDomain::Scalar),
            parameter("spread", "4", talas::expr::ValueDomain::Scalar),
            // CDP's buffer, which orders the echoes' sums; 0 is CDP's.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `flutter flutter`: a tremolo that moves around the channels of a
    // multichannel input, a channel set a cycle.
    add(registry, {
        "talas.cdpFlutter",
        "CDP Flutter",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            // The channel sets, rows of channel numbers (a row may be
            // empty); none: each channel its own set, in order.
            textParameter("sets", ""),
            // 0 to 120 Hz and 0 to 16, which CDP may take as breakpoint
            // files and reads every frame.
            parameter("frequency", "10 hz", talas::expr::ValueDomain::Hertz),
            parameter("depth", "1", talas::expr::ValueDomain::Scalar),
            // 0.000002 to 10.
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -r: each round of sets in a new random order.
            booleanParameter("random", false),
            // CDP's buffer, over which a depth of 0 freezes its clock; 0 is
            // CDP's 262144 / N frames.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `frame shift` modes 1 and 2: the whole frame of a multichannel
    // input turned around its ring of channels.
    add(registry, {
        "talas.cdpFrameRotate",
        "CDP Frame Rotate",
        "CDP",
        cdpChannelInputs(2),
        cdpChannelOutputs(),
        {
            // CDP's modes 1 (the whole frame) and 2 (the odd channels by
            // the rotation, the even ones by the second; an even count).
            choiceParameter("mode", "oneRotation", {"oneRotation", "twoRotations"}),
            // Turns a second, -500 to 500 (below 0 anticlockwise), which
            // CDP may take as breakpoint files and reads every sr / 500
            // frames.
            parameter("rotation", "1", talas::expr::ValueDomain::Scalar),
            parameter("secondRotation", "-1", talas::expr::ValueDomain::Scalar),
            // 0 to 0.5 of each channel bled onto its neighbours.
            parameter("smear", "0", talas::expr::ValueDomain::Scalar),
            // How a rotation starts, which CDP finds ahead in its breakpoint
            // file: 1 clockwise, -1 anticlockwise, 0 the starting
            // rotation's sign.
            parameter("firstDirection", "0", talas::expr::ValueDomain::Scalar),
            parameter("secondFirstDirection", "0", talas::expr::ValueDomain::Scalar),
            // A route through every channel once, or none for 1, 2, 3, ...
            textParameter("snake", ""),
        },
    });

    // CDP `frame shift` modes 3, 4, 5, 6 and 8: a multichannel input's
    // channels sent, unchanged, to other output channels.
    add(registry, {
        "talas.cdpFrameRemap",
        "CDP Frame Remap",
        "CDP",
        cdpChannelInputs(2),
        cdpChannelOutputs(),
        {
            // CDP's modes 3 (input n to the route's channel n), 4 (mirrored
            // about the plane), 5 (ring numbering to bilateral), 6 (two
            // channels swapped) and 8 (to the BEAST's bilateral numbering).
            choiceParameter("mode", "swap", {"reorient", "mirror", "bilateral", "swap", "beastBilateral"}),
            textParameter("route", ""),
            // A channel, or halfway between two (N.5 between channel N and
            // channel 1), 0 to N + 0.5.
            parameter("plane", "1", talas::expr::ValueDomain::Scalar),
            // Bilateral numbering back to the ring.
            booleanParameter("toRing", false),
            // The channels swapped.
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            parameter("second", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `frame shift 7` (envchans): chosen channels of a multichannel
    // input scaled by a gain, the others passed unchanged.
    add(registry, {
        "talas.cdpChannelEnvelope",
        "CDP Channel Envelope",
        "CDP",
        cdpChannelInputs(2),
        cdpChannelOutputs(),
        {
            // The channels enveloped, each once.
            textParameter("enveloped", "1"),
            // 0 to 1, which CDP may take as a breakpoint file.
            parameter("gain", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `tostereo tostereo`: a stereo input mixed to mono opening out to
    // true stereo between two times (or closing, the end before the start),
    // on two of N output channels.
    add(registry, {
        "talas.cdpToStereo",
        "CDP To Stereo",
        "CDP",
        {input("audioIn", PortType::Audio), input("rightIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "2", talas::expr::ValueDomain::Scalar),
            parameter("start", "0 s", talas::expr::ValueDomain::Seconds),
            parameter("end", "1 s", talas::expr::ValueDomain::Seconds),
            // The output channels of the left and right input (0: CDP's
            // default); with Right 0 and 3 channels or more, the input forks
            // from Left to the channels either side.
            parameter("left", "0", talas::expr::ValueDomain::Scalar),
            parameter("right", "0", talas::expr::ValueDomain::Scalar),
            // 0.5 to 1, or 0 for CDP's default.
            parameter("mixLevel", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `modify space` modes 2 (mirror) and 4 (narrow) on a stereo input.
    add(registry, {
        "talas.cdpStereoSpace",
        "CDP Stereo Space",
        "CDP",
        {input("audioIn", PortType::Audio), input("rightIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            choiceParameter("mode", "mirror", {"mirror", "narrow"}),
            // Mode narrow's Narrowing, -1 (mirrored) to 1 (unchanged, which
            // CDP refuses); 0 is mono.
            parameter("narrowing", "0.5", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `housekeep chans 3`: one channel of 1 to 8 silenced (a mono input
    // played on two channels, its second silent).
    add(registry, {
        "talas.cdpChannelZero",
        "CDP Channel Zero",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
            // CDP's buffer in frames, where a mono input's second channel is
            // also silent on each buffer's first frame; 0 is CDP's 16384.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `housekeep chans 4`: a stereo input mixed to mono.
    add(registry, {
        "talas.cdpChannelMixdown",
        "CDP Channel Mixdown",
        "CDP",
        {input("audioIn", PortType::Audio), input("rightIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            // CDP's -p: the second channel's phase inverted.
            booleanParameter("invertSecond", false),
        },
    });

    // CDP `housekeep chans 5`: a mono input on both channels.
    add(registry, {
        "talas.cdpMonoToStereo",
        "CDP Mono To Stereo",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {},
    });

    // CDP `chanphase`: one channel of 1 to 8 phase-inverted.
    add(registry, {
        "talas.cdpChannelPhase",
        "CDP Channel Phase",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            parameter("channel", "1", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `mton`: a mono input on 2 to 8 channels.
    add(registry, {
        "talas.cdpMonoToMany",
        "CDP Mono To Many",
        "CDP",
        {input("audioIn", PortType::Audio)},
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `pairex`: two channels of a 3- to 8-channel input as a stereo pair.
    add(registry, {
        "talas.cdpChannelPair",
        "CDP Channel Pair",
        "CDP",
        cdpChannelInputs(3),
        cdpChannelOutputs(),
        {
            parameter("first", "1", talas::expr::ValueDomain::Scalar),
            parameter("second", "2", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `mchstereo`: stereo inputs (in1 and in2, in3 and in4, ...) each
    // placed about a centre channel of 2 to 8.
    add(registry, {
        "talas.cdpStereoPlacement",
        "CDP Stereo Placement",
        "CDP",
        cdpChannelInputs(2),
        cdpChannelOutputs(),
        {
            parameter("channels", "4", talas::expr::ValueDomain::Scalar),
            // Each input's centre channel, 1 to the channels, as CDP's data
            // file numbers them.
            textParameter("centres", "1"),
            parameter("pregain", "1", talas::expr::ValueDomain::Scalar),
            // CDP's -s, the stereo image shifted half a channel.
            booleanParameter("shifted", false),
        },
    });

    // CDP `prefix silence`: the input delayed by a silence.
    add(registry, {
        "talas.cdpPrefix",
        "CDP Prefix",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            parameter("duration", "0.1 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `sfedit masks`: silences over start and end times, with splices.
    add(registry, {
        "talas.cdpSilenceMasks",
        "CDP Silence Masks",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            // CDP's modes 1 to 3: the times in seconds, samples or frames.
            choiceParameter("units", "seconds", {"seconds", "samples", "frames"}),
            textParameter("times", "0.5 1"),
            parameter("splice", "15 ms", talas::expr::ValueDomain::Seconds),
            // CDP's buffer in frames, where a splice crossing it restarts;
            // 0 is CDP's.
            parameter("bufferFrames", "0", talas::expr::ValueDomain::Scalar),
        },
    });

    // CDP `sfedit insil -o`: a silence written over the input at a time.
    add(registry, {
        "talas.cdpSilenceOverwrite",
        "CDP Silence Overwrite",
        "CDP",
        cdpChannelInputs(1),
        cdpChannelOutputs(),
        {
            choiceParameter("units", "seconds", {"seconds", "samples", "frames"}),
            // In the units.
            parameter("time", "0.5", talas::expr::ValueDomain::Scalar),
            parameter("duration", "0.25", talas::expr::ValueDomain::Scalar),
            parameter("splice", "15 ms", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `envel warp 6` (timestretch) for a stretch of 1 or more: the
    // input's own envelope slowed down replaces it.
    add(registry, {
        "talas.cdpEnvelopeTimestretch",
        "CDP Envelope Timestretch",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's wsize, 5 to 10000 ms; the output waits two windows less
            // a frame.
            parameter("window", "5 ms", talas::expr::ValueDomain::Seconds),
            // 1 to 1000.
            parameter("stretch", "2", talas::expr::ValueDomain::Scalar),
            // The envelope the stream keeps, up to an hour: exact while the
            // stretch reads back no further.
            parameter("maximumLag", "600 s", talas::expr::ValueDomain::Seconds),
        },
    });

    // CDP `repitch transposef` between `pvoc anal` and `pvoc synth`: a
    // transposition that keeps the spectrum's formant envelope; its mode and
    // formant envelope have no default.
    add(registry, {
        "talas.cdpRepitchTransposef",
        "CDP Repitch Transposef",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            parameter("points", "1024", talas::expr::ValueDomain::Scalar),
            parameter("overlap", "3", talas::expr::ValueDomain::Scalar),
            // CDP's modes 1 to 3: the transposition as a ratio, in octaves
            // or in semitones.
            choiceParameter("units", "semitones", {"ratio", "octaves", "semitones"}),
            // CDP's -f and -p: the formant envelope in bands of equal
            // channels, or of equal pitch, so many an octave.
            choiceParameter("envelope", "frequencyWise", {"frequencyWise", "pitchWise"}),
            // CDP's N: channels a band, or bands an octave.
            parameter("formantBands", "4", talas::expr::ValueDomain::Scalar),
            // CDP's -i: a quicker, less accurate search for formants.
            booleanParameter("quickSearch", false),
            parameter("transposition", "0", talas::expr::ValueDomain::Scalar),
            // CDP's -l and -h: channels below or above them fall silent; a
            // static zero leaves the flag out.
            parameter("lowFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            parameter("highFrequency", "0 hz", talas::expr::ValueDomain::Hertz),
            // CDP's -x: a moved partial takes a nearby empty channel when its
            // own is taken.
            booleanParameter("fuller", false),
        },
    });

    // CDP `filter varibank` and its defaults; its data file has none.
    add(registry, {
        "talas.cdpVaryingBank",
        "CDP Varying Bank",
        "CDP",
        {input("audioIn", PortType::Audio)},
        {output("audioOut", PortType::Audio)},
        {
            // CDP's mode: the bands' pitches in Hz or as MIDI note numbers.
            choiceParameter("pitches", "hertz", {"hertz", "midi"}),
            // CDP's data file: a time in seconds, then a pitch and an
            // amplitude (or level in dB) for each band, a line for each time.
            textParameter("bands", "0 440 1"),
            parameter("q", "20", talas::expr::ValueDomain::Scalar),
            parameter("gain", "1", talas::expr::ValueDomain::Scalar),
            parameter("harmonics", "1", talas::expr::ValueDomain::Scalar),
            // The drop from each harmonic to the next, in dB.
            parameter("rolloff", "-6", talas::expr::ValueDomain::Scalar),
            booleanParameter("doubleFiltering", false),
        },
    });

}

static void registerBuiltinNodeMetadata(NodeRegistry& registry)
{
#define TALAS_NODE_SUPPORT(typeId, level, summary) \
    registry.setNodeTypeSupport(typeId, NodeSupportLevel::level, summary);
#include "BuiltinNodeSupport.generated.inc"
#undef TALAS_NODE_SUPPORT

    for (const auto& descriptor : registry.nodeTypes())
    {
        const auto offlineSupport =
            descriptor.supportLevel == NodeSupportLevel::FullRuntime
                || descriptor.supportLevel == NodeSupportLevel::Utility
            ? ExecutionSupport::Supported
            : ExecutionSupport::Subset;
        registry.setNodeTypeOfflineSupport(
            descriptor.typeId,
            offlineSupport,
            hasBuiltinOfflineKernelBinding(descriptor.typeId)
                ? ExecutionBinding::Kernel
                : ExecutionBinding::LegacyAdapter);
        const auto hasPreparedRealtimeKernel =
            hasBuiltinPreparedRealtimeKernelBinding(
                descriptor.typeId);
        const auto hasPreparedRealtimeBinding =
            hasPreparedRealtimeKernel
            || hasBuiltinPreparedRealtimeLegacyBinding(
                   descriptor.typeId);
        const auto hasDynamicControlUtilityParameters =
            descriptor.typeId == "talas.clock"
            || descriptor.typeId == "talas.clockDivider"
            || descriptor.typeId == "talas.quantizer"
            || descriptor.typeId == "talas.gate"
            || descriptor.typeId == "talas.logic"
            || descriptor.typeId == "talas.slew";
        const auto isStaticParameterRealtimeUtility =
            hasPreparedRealtimeKernel
            && descriptor.supportLevel == NodeSupportLevel::Utility
            && !hasDynamicControlUtilityParameters;
        registry.setNodeTypePreparedRealtimeSupport(
            descriptor.typeId,
            hasPreparedRealtimeBinding
                ? ExecutionSupport::Subset
                : ExecutionSupport::Unsupported,
            hasPreparedRealtimeBinding
                ? (hasPreparedRealtimeKernel
                        ? ExecutionBinding::Kernel
                        : ExecutionBinding::LegacyAdapter)
                : ExecutionBinding::None,
            isStaticParameterRealtimeUtility
                ? "prepared-realtime-static-parameters-only"
                : std::string{});
        registry.setNodeTypeNestedSoundSupport(
            descriptor.typeId,
            NestedSoundSupport::Expanded);
    }

    for (const auto typeId : {
             std::string_view{"talas.logic"},
         })
    {
        registry.addNodeTypeLimitation(
            std::string{typeId},
            "prepared-realtime-bounded-event-value-numeric-parameters");
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.clock",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-long-double-phase-continuous-clock"},
             std::string_view{"static-or-direct-event-value-positive-hertz-frequency-unit-duty-cycle-and-static-enabled"},
             std::string_view{"exact-absolute-frame-and-epoch-rebase-phase-less-than-duty-zero-or-one-gate-and-disabled-zero-output"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-output-buffer-scope-state-reset-transport-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-reset-discontinuity-and-zero-frame-no-op"},
             std::string_view{"external-sync-reset-input-phase-offset-alternate-pulse-shape-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.clock", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.clockDivider",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-positive-uint32-clock-divider"},
             std::string_view{"static-or-direct-event-value-positive-integer-division-and-steps-with-static-scalar-or-connected-reset"},
             std::string_view{"rising-edge-count-modulo-reduction-held-step-divided-gate-and-reset-precedence"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-dual-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-modulus-schedule-state-output-retry-reset-discontinuity-and-zero-frame-no-op"},
             std::string_view{"typed-event-clock-reset-dynamic-reset-fallback-fractional-counter-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.clockDivider", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.filter",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-mono-topology-preserving-order-pole-or-legacy-state-variable-filter"},
             std::string_view{"static-low-high-or-all-pass-static-order-one-through-sixteen-static-poles-and-stateless-event-value-or-connected-controls"},
             std::string_view{"zero-through-open-nyquist-frequency-positive-q-nonnegative-scale-open-unit-feedback-and-connected-precedence"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-cascade-feedback-schedule-output-state-retry-reset-discontinuity-and-zero-frame-no-op"},
             std::string_view{"dynamic-type-order-poles-general-identifier-typed-event-multichannel-oversampled-and-exact-reference-coefficient-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.filter", std::string{limitation});
    }
    registry.addNodeTypeLimitation(
        "talas.logic",
        "prepared-realtime-static-operation-choice");

    registry.setNodeTypePreparedRealtimeSupport(
        "talas.logic",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.quantizer",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.gate",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    for (const auto limitation : {
             std::string_view{
                 "offline-and-prepared-shared-checked-finite-binary64-schmitt-gate"},
             std::string_view{
                 "static-or-direct-event-value-finite-ordered-low-and-high-thresholds"},
             std::string_view{
                 "exact-closed-start-high-inclusive-open-low-inclusive-close-positive-zero-or-one-output"},
             std::string_view{
                 "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-state-reset-zero-latency-and-local-transaction-authority"},
             std::string_view{
                 "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-reset-and-zero-frame-no-op"},
             std::string_view{
                 "debounce-hold-time-edge-events-alternate-polarity-smoothing-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.gate",
            std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.slew",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    for (const auto limitation : {
             std::string_view{
                 "offline-and-prepared-shared-checked-finite-binary64-directional-exponential-slew"},
             std::string_view{
                 "static-or-direct-event-value-finite-nonnegative-rise-and-fall-seconds"},
             std::string_view{
                 "exact-first-sample-initialization-target-greater-or-equal-rise-selection-and-zero-time-direct-tracking"},
             std::string_view{
                 "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-state-reset-zero-latency-and-local-transaction-authority"},
             std::string_view{
                 "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-reset-and-zero-frame-no-op"},
             std::string_view{
                 "alternate-curve-rate-initialization-overshoot-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.slew",
            std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{
                 "offline-and-prepared-shared-checked-finite-clean-room-step-quantizer"},
             std::string_view{
                 "static-or-direct-event-value-positive-step-size-and-finite-offset"},
             std::string_view{
                 "exact-offset-plus-round-value-times-step-size-with-finite-input-and-result"},
             std::string_view{
                 "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{
                 "allocation-free-prepared-callback-failure-atomic-schedule-output-retry-and-zero-frame-no-op"},
             std::string_view{
                 "alternate-tie-breaking-units-clipping-hysteresis-stochastic-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.quantizer",
            std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{
                 "offline-and-prepared-shared-checked-finite-clean-room-static-choice-logic"},
             std::string_view{
                 "static-and-or-xor-not-with-direct-event-value-or-per-frame-connected-left-and-right"},
             std::string_view{
                 "strict-positive-truth-with-exact-zero-or-one-output-and-not-right-operand-ignored"},
             std::string_view{
                 "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{
                 "allocation-free-prepared-callback-failure-atomic-schedule-output-retry-and-zero-frame-no-op"},
             std::string_view{
                 "dynamic-operation-coercion-alternate-threshold-stateful-edge-logic-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.logic",
            std::string{limitation});
    }

    registry.setNodeTypePreparedRealtimeSupport(
        "talas.annotation",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.annotation",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.mapEventValues",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.prefixer",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.prefixer", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.prefixer", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.prefixer", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"compiler-transformed-explicit-or-input-sound-name-prefix-for-exact-event-value-tokens"},
             std::string_view{"selected-or-except-selected-special-events-on-isolated-upstream-mono-sound"},
             std::string_view{"shared-upstream-branch-and-generated-name-collision-rejected"},
             std::string_view{"local-sound-event-value-bindings-rejected-to-preserve-scope"},
             std::string_view{"same-scope-node-id-fallback-and-exact-reference-name-concatenation-collision-semantics-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.prefixer", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.replicator",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.replicator", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.replicator", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.replicator", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"compiler-expanded-static-number-of-independent-upstream-mono-copies"},
             std::string_view{"isolated-root-wrapper-clones-closed-reusable-mono-sounds-with-scoped-local-bindings"},
             std::string_view{"nested-wrapper-clones-same-scope-and-child-sound-trees-with-child-local-bindings"},
             std::string_view{"nested-wrapper-enclosing-event-and-node-parameter-bindings-remain-shared"},
             std::string_view{"child-expression-inherits-nearest-ancestor-event-binding-with-local-shadowing-and-scheduled-parent-updates"},
             std::string_view{"number-must-be-a-static-positive-integer-at-most-64"},
             std::string_view{"selected-or-except-selected-special-events-use-bounded-static-prefix-and-suffix-labels"},
             std::string_view{"default-01-suffix-increments-per-voice-and-custom-quoted-label-lists-repeat-last"},
             std::string_view{"green-variable-renaming-multichannel-and-exact-reference-gain-semantics-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.replicator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.stereoizer", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.stereoizer", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.stereoizer", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.stereoizer", {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOfflineSupport(
        "talas.stereoizer", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.stereoizer", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"compiler-expanded-two-independent-isolated-mono-branches"},
             std::string_view{"selected-or-except-selected-special-events-use-bounded-static-prefix-and-suffix-labels"},
             std::string_view{"static-shared-sounds-keep-one-upstream-source-with-two-processed-branches"},
             std::string_view{"optional-right-input-routes-one-shared-stereo-source-boundary-to-the-right-branch"},
             std::string_view{"static-left-or-right-bypass-routes-one-shared-source-boundary-around-its-processing-branch"},
             std::string_view{"multiple-stereo-boundaries-and-exact-reference-default-labels-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.stereoizer", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.surroundifer", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.surroundifer", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.surroundifer", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.surroundifer", {AudioBusLayoutKind::Discrete6});
    registry.setNodeTypeOfflineSupport(
        "talas.surroundifer", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.surroundifer", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"compiler-expanded-six-independent-processing-branches-from-one-shared-stereo-source"},
             std::string_view{"selected-or-except-selected-special-events-use-bounded-static-prefix-and-suffix-labels"},
             std::string_view{"ordered-left-right-left-surround-right-surround-center-lfe-mono-output-ports-with-independent-levels"},
             std::string_view{"static-per-channel-processing-bypass-preserves-shared-seeds-and-independent-levels"},
             std::string_view{"pseudo-surround-side-and-center-matrix-is-local-and-sub-bass-distortion-and-exact-reference-semantics-remain-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.surroundifer", std::string{limitation});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.mapEventValues", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.mapEventValues", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.mapEventValues", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"compiler-transformed-static-string-or-ordered-string-collection-green-variable-binding"},
             std::string_view{"upstream-mono-audio-closure-including-explicit-nested-sound-instances"},
             std::string_view{"string-resource-and-collection-static-resource-list-consumers-only"},
             std::string_view{"shared-upstream-branch-binding-rejected-to-prevent-scope-leakage"},
             std::string_view{"dynamic-typed-values-general-val-shadowing-and-exact-reference-routing-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.mapEventValues", std::string{limitation});
    }
    for (const auto typeId : {
             std::string_view{"talas.sharedFileName"},
             std::string_view{"talas.sharedFileNames"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::Stateless, false,
            {LatencyKind::None, 0});
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        registry.setNodeTypeResourceAccess(
            typeId, ResourceAccessMode::ImmutableRead);
        registry.setNodeTypeOutputBusLayouts(
            typeId, {AudioBusLayoutKind::Mono});
        for (const auto limitation : {
                 std::string_view{"compiler-transformed-static-file-variable-replacement-after-nested-sound-expansion"},
                 std::string_view{"exact-file-resource-and-static-file-list-tokens-in-upstream-mono-sound-closure"},
                 std::string_view{"shared-upstream-branch-binding-rejected-to-prevent-scope-leakage"},
                 std::string_view{"dynamic-selection-shadowing-filenames-with-whitespace-multichannel-and-exact-reference-authoring-unverified"},
             })
        {
            registry.addNodeTypeLimitation(
                typeId, std::string{limitation});
        }
    }
    for (const auto typeId : {
             std::string_view{"talas.uniqueMemoryWriterRecordingNames"},
             std::string_view{"talas.cancelUniqueMemoryWriterRecordingNames"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::Stateless, false,
            {LatencyKind::None, 0});
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        registry.setNodeTypeResourceAccess(
            typeId, ResourceAccessMode::None);
        registry.setNodeTypeOutputBusLayouts(
            typeId, {AudioBusLayoutKind::Mono});
        for (const auto limitation : {
                 std::string_view{"compiler-transformed-upstream-mono-memory-writer-namespace-after-nested-sound-expansion"},
                 std::string_view{"cancel-boundary-stops-renaming-of-upstream-input-sounds"},
                 std::string_view{"exact-static-recording-name-and-known-from-memory-writer-readers-only"},
                 std::string_view{"shared-upstream-branches-dynamic-names-parameter-pasted-sounds-and-exact-reference-namespace-unverified"},
             })
        {
            registry.addNodeTypeLimitation(
                typeId, std::string{limitation});
        }
    }
    registry.setNodeTypeProcessingContract(
        "talas.sampleWithTimeIndex", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sampleWithTimeIndex", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.sampleWithTimeIndex", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.sampleWithTimeIndex", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"compiler-transformed-static-mono-sample-bipolar-position-reader"},
             std::string_view{"audio-rate-time-index-input-and-nearest-or-linear-immutable-table-lookup"},
             std::string_view{"outside-minus-one-to-one-indices-clamped-and-nonfinite-input-rejected"},
             std::string_view{"multichannel-file-loop-markers-dynamic-sample-switching-and-exact-reference-index-mapping-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sampleWithTimeIndex", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multiSampleWithTimeIndex", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multiSampleWithTimeIndex", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.multiSampleWithTimeIndex", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.multiSampleWithTimeIndex", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"compiler-transformed-bounded-static-mono-sample-list-with-shared-audio-rate-time-index"},
             std::string_view{"zero-based-integer-index-selects-one-file-at-each-frame"},
             std::string_view{"out-of-range-index-silence-and-nonfinite-index-rejected"},
             std::string_view{"reference-time-index-for-file-names-separate-sound-file-types-selection-timing-and-exact-index-rules-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multiSampleWithTimeIndex", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.splitSurroundSampleWithTimeIndex",
        NodeStateModel::Stateless, false, {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.splitSurroundSampleWithTimeIndex",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.splitSurroundSampleWithTimeIndex",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.splitSurroundSampleWithTimeIndex",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.splitSurroundSampleWithTimeIndex",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.splitSurroundSampleWithTimeIndex",
        {AudioBusLayoutKind::Stereo,
         AudioBusLayoutKind::Discrete4,
         AudioBusLayoutKind::Discrete8});
    for (const auto limitation : {
             std::string_view{"compiler-expanded-two-through-eight-static-ordered-mono-file-channels-with-one-shared-audio-rate-bipolar-time-index"},
             std::string_view{"each-output-port-reads-its-own-immutable-file-at-the-same-normalized-position-with-nearest-or-linear-interpolation"},
             std::string_view{"connecting-an-output-beyond-the-file-count-is-rejected"},
             std::string_view{"split-channels-require-equal-length-mono-immutable-table-resources"},
             std::string_view{"interleaved-multichannel-files-file-label-conventions-loop-markers-and-exact-reference-index-mapping-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.splitSurroundSampleWithTimeIndex",
            std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.splitSurroundOscillator",
        NodeStateModel::SessionStateful, true, {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.splitSurroundOscillator",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.splitSurroundOscillator",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.splitSurroundOscillator", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.splitSurroundOscillator", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.splitSurroundOscillator",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo,
         AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete8});
    for (const auto limitation : {
             std::string_view{"compiler-composed-periodic-single-cycle-oscillator-time-index-and-waveshaper"},
             std::string_view{"one-immutable-mono-or-stereo-table-or-two-through-eight-equal-length-mono-split-tables"},
             std::string_view{"finite-cycles-of-at-least-two-samples-with-common-phase-across-channels"},
             std::string_view{"static-or-connected-finite-frequency-from-zero-through-nyquist"},
             std::string_view{"allocation-free-prepared-callbacks-with-phase-continuity-and-reset"},
             std::string_view{"interleaved-files-beyond-table-loader-exact-split-labels-antialiasing-and-reference-phase-interpolation-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.splitSurroundOscillator", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.splitSurroundAntialiasedOscillator",
        NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.splitSurroundAntialiasedOscillator",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.splitSurroundAntialiasedOscillator",
        ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.splitSurroundAntialiasedOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.splitSurroundAntialiasedOscillator",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.splitSurroundAntialiasedOscillator",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo,
         AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete8});
    for (const auto limitation : {
             std::string_view{"compiler-composed-per-channel-antialiased-cycle-readers-with-identical-initial-phase-and-frequency-control"},
             std::string_view{"one-immutable-mono-or-stereo-table-or-two-through-eight-equal-length-mono-split-tables"},
             std::string_view{"complex-fourier-phase-preserving-first-1024-harmonics-strictly-below-nyquist"},
             std::string_view{"static-duty-cycle-and-static-or-connected-frequency-and-brightness"},
             std::string_view{"interleaved-files-beyond-table-loader-exact-split-labels-and-reference-antialias-response-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.splitSurroundAntialiasedOscillator",
            std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.oscillatorTimeIndex", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.oscillatorTimeIndex", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.oscillatorTimeIndex", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.oscillatorTimeIndex", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.oscillatorTimeIndex", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.oscillatorTimeIndex", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"mono-audio-rate-periodic-minus-one-to-one-phase-for-single-cycle-sample-indexing"},
             std::string_view{"static-direct-event-value-or-connected-per-frame-frequency-from-zero-through-nyquist"},
             std::string_view{"phase-continuity-across-offline-and-allocation-free-prepared-blocks-with-transport-reset"},
             std::string_view{"exact-reference-phase-origin-wrap-sample-frequency-modulation-reset-and-phase-modulation-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.oscillatorTimeIndex", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.timeIndex", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeIndex", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.timeIndex", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeIndex", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-normalized-time-index-stage-model"},
             std::string_view{"static-duration-loop-reverse-and-remaining-markers-with-connected-sample-rate-gate-rate-and-start-markers"},
             std::string_view{"positive-gate-start-loop-while-held-and-falling-edge-release-start-jump"},
             std::string_view{"direct-hot-marker-expression-syntax-active-gate-marker-changes-stage-scales-and-exact-loop-boundaries-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeIndex", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeIndexFromFileName", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeIndexFromFileName", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.timeIndexFromFileName", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeIndexFromFileName", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"mono-wav-or-aiff-table-with-first-forward-loop-only"},
             std::string_view{"duration-and-loop-boundaries-derived-from-immutable-file-metadata"},
             std::string_view{"reference-marker-format-precedence-and-exact-timing-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeIndexFromFileName", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeIndexForSamples", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeIndexForSamples", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.timeIndexForSamples", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeIndexForSamples", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"bounded-static-mono-wav-or-aiff-list-with-zero-based-integer-selection"},
             std::string_view{"frequency-to-playback-rate-uses-selected-wav-smpl-midi-unity-note-and-pitch-fraction"},
             std::string_view{"selection-change-restarts-checked-time-index-stage"},
             std::string_view{"reference-frequency-units-selection-timing-marker-precedence-and-exact-phase-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeIndexForSamples", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeIndexForFileNames", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeIndexForFileNames", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.timeIndexForFileNames", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeIndexForFileNames", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"bounded-static-mono-wav-or-aiff-list-with-zero-based-integer-selection"},
             std::string_view{"selected-file-duration-and-forward-smpl-loop-drive-rate-controlled-time-index"},
             std::string_view{"per-frame-selection-change-restarts-checked-time-index-stage"},
             std::string_view{"psi-spectral-sample-cloud-file-types-reference-selection-syntax-and-exact-phase-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeIndexForFileNames", std::string{limitation});
    }
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.annotation", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.annotation", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{
                 "offline-and-prepared-bit-preserving-mono-audio-pass-through"},
             std::string_view{
                 "stateless-resource-free-zero-latency-and-no-analysis-capture"},
             std::string_view{
                 "compiler-owned-exact-descriptor-plan-settings-resource-free-input-output-producer-scope-latency-and-local-transaction-authority"},
             std::string_view{
                 "allocation-free-prepared-callback-failure-atomic-output-retry-and-zero-frame-no-op"},
             std::string_view{
                 "annotation-text-rich-format-layout-interaction-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.annotation", std::string{limitation});
    }

    for (const auto typeId : {
             std::string_view{"talas.variable"},
             std::string_view{"talas.soundCollectionVariable"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId,
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }

    registry.setNodeTypeProcessingContract(
        "talas.midiFileEcho",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.midiFileEcho",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.midiFileEcho",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{
                 "caller-owned-terminal-event-block"},
             std::string_view{
                 "preparation-time-smf-format-0-1-2-parsing"},
             std::string_view{
                 "maximum-4096-prepared-midi-file-events"},
             std::string_view{
                 "no-device-routing-or-midi-endpoint-io"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.midiFileEcho",
            std::string{limitation});
    }

    registry.setNodeTypeProcessingContract(
        "talas.midiOutputController",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.midiOutputController",
        ResourceAccessMode::None);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.midiOutputController",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{
                 "caller-owned-terminal-event-block"},
             std::string_view{
                 "three-byte-midi-control-change-events"},
             std::string_view{
                 "maximum-4096-events-per-callback"},
             std::string_view{
                 "no-device-routing-or-midi-endpoint-io"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.midiOutputController",
            std::string{limitation});
    }

    registry.setNodeTypeProcessingContract(
        "talas.midiOutputEventInBytes",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.midiOutputEventInBytes",
        ResourceAccessMode::None);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.midiOutputEventInBytes",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{
                 "caller-owned-terminal-event-block"},
             std::string_view{
                 "maximum-256-midi-bytes-per-event"},
             std::string_view{
                 "maximum-4096-events-per-callback"},
             std::string_view{
                 "no-device-routing-or-midi-endpoint-io"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.midiOutputEventInBytes",
            std::string{limitation});
    }

    for (const auto typeId : {
             std::string_view{"talas.midiOutputTimingClock"},
             std::string_view{"talas.midiOutputStartStopMasterClock"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::SessionStateful, true,
            {LatencyKind::None, 0});
        registry.setNodeTypeResourceAccess(
            typeId, ResourceAccessMode::None);
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        for (const auto limitation : {
                 std::string_view{"caller-owned-terminal-event-block"},
                 std::string_view{"midi-clock-24-pulses-per-quarter-note"},
                 std::string_view{"project-tempo-20-to-300-bpm"},
                 std::string_view{"maximum-4096-events-per-callback"},
                 std::string_view{"no-device-routing-or-midi-endpoint-io"},
             })
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
    }

    for (const auto typeId : {
             std::string_view{"talas.midiOutputEventTap"},
             std::string_view{"talas.midiOutputControllerTap"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::SessionStateful, true,
            {LatencyKind::None, 0});
        registry.setNodeTypeResourceAccess(typeId, ResourceAccessMode::None);
        registry.setNodeTypeOfflineSupport(typeId, ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
        registry.setNodeTypePreparedRealtimeSupport(typeId, ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter,
            "prepared-realtime-top-level-one-midi-output-within-a-sound");
        for (const auto limitation : {
                 std::string_view{"top-level-only-one-midi-output-within-a-sound"},
                 std::string_view{"maximum-4096-events-per-callback"},
                 std::string_view{"no-device-routing-or-midi-endpoint-io"},
             })
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
    }
    registry.setNodeTypeReplacement("talas.midiOutputEventTap", "talas.midiOutputEvent");
    registry.setNodeTypeReplacement("talas.midiOutputControllerTap", "talas.midiOutputController");

    // A MIDIFileEcho within a Sound.
    registry.setNodeTypeProcessingContract(
        "talas.midiFileEchoTap", NodeStateModel::SessionStateful, true, {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess("talas.midiFileEchoTap", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOfflineSupport("talas.midiFileEchoTap", ExecutionSupport::Subset, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport("talas.midiFileEchoTap", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter, "prepared-realtime-top-level-one-midi-output-within-a-sound");
    for (const auto limitation : {
             std::string_view{"top-level-only-one-midi-output-within-a-sound"},
             std::string_view{"preparation-time-smf-format-0-1-2-parsing"},
             std::string_view{"maximum-4096-prepared-midi-file-events"},
             std::string_view{"no-device-routing-or-midi-endpoint-io"},
         })
        registry.addNodeTypeLimitation("talas.midiFileEchoTap", std::string{limitation});
    registry.setNodeTypeReplacement("talas.midiFileEchoTap", "talas.midiFileEcho");

    registry.setNodeTypeProcessingContract(
        "talas.midiOutputEvent",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.midiOutputEvent",
        ResourceAccessMode::None);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.midiOutputEvent",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{
                 "caller-owned-terminal-event-block"},
             std::string_view{
                 "three-byte-midi-note-on-off-events"},
             std::string_view{
                 "gate-edge-active-note-channel-ownership"},
             std::string_view{
                 "maximum-4096-events-per-callback"},
             std::string_view{
                 "no-device-routing-or-midi-endpoint-io"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.midiOutputEvent",
            std::string{limitation});
    }

    registry.setNodeTypeProcessingContract(
        "talas.soundToGlobalController",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.soundToGlobalController",
        ResourceAccessMode::None);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.soundToGlobalController",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{
                 "caller-owned-terminal-event-and-name-block"},
             std::string_view{
                 "capture-only-no-global-controller-mutation"},
             std::string_view{
                 "bounded-event-value-or-connected-finite-scalar-values"},
             std::string_view{
                 "maximum-4096-events-per-callback"},
             std::string_view{
                 "no-device-endpoint-or-cross-thread-io"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.soundToGlobalController",
            std::string{limitation});
    }

    registry.setNodeTypeProcessingContract(
        "talas.triggeredSoundToGlobalController",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.triggeredSoundToGlobalController",
        ResourceAccessMode::None);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.triggeredSoundToGlobalController",
        FanInLatencyPolicy::NotApplicable);
    for (const auto limitation : {
             std::string_view{"trigger-positive-gated-change-capture"},
             std::string_view{"allow-live-override-requires-explicit-host-route"},
             std::string_view{"capture-only-no-global-controller-mutation"},
             std::string_view{"maximum-4096-events-per-callback"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.triggeredSoundToGlobalController",
            std::string{limitation});
    }

    for (const auto typeId : {
             std::string_view{"talas.contextFreeGrammar"},
             std::string_view{"talas.limeInterpreter"},
             std::string_view{"talas.script"},
             std::string_view{"talas.textFileInterpreter"},
             std::string_view{"talas.midiFileInterpreter"},
         })
    {
        registry.addNodeTypeLimitation(
            typeId,
            "arbitrary-script-runtime-unsupported");
    }
    // These prototypes become, as compiling begins, what they play.
    for (const auto typeId : {
             std::string_view{"talas.textFileInterpreter"},
             std::string_view{"talas.midiFileInterpreter"},
             std::string_view{"talas.cellularAutomaton"},
             std::string_view{"talas.rhythmicCellularAutomaton"},
             std::string_view{"talas.randomSelection"},
             std::string_view{"talas.preset"},
             std::string_view{"talas.attenuator"},
             std::string_view{"talas.gain"},
             std::string_view{"talas.centeringMixer"},
             std::string_view{"talas.endTogetherMixer"},
             std::string_view{"talas.concatenation"},
             std::string_view{"talas.diskSplicer"},
         })
    {
        registry.addNodeTypeLimitation(
            typeId,
            "classic-prototype-outside-the-prototype-catalog-expanded-when-compiling");
    }
    registry.setNodeTypeResourceAccess("talas.textFileInterpreter", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeResourceAccess("talas.midiFileInterpreter", ResourceAccessMode::ImmutableRead);

    for (const auto limitation : {
             std::string_view{"static-logical-processor-only"},
             std::string_view{"portable-serial-host-execution"},
             std::string_view{"hardware-affinity-unsupported"},
             std::string_view{"card-local-memory-unsupported"},
             std::string_view{"parallel-workers-unsupported"},
             std::string_view{"dynamic-load-balancing-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.forcedProcessorAssignment",
            std::string{limitation});
    }

    for (const auto& kernel : builtinKernelRegistry())
    {
        const auto stateless =
            kernel.kind == BuiltinKernelKind::Quantizer
            || kernel.kind == BuiltinKernelKind::Logic;
        registry.setNodeTypeProcessingContract(
            kernel.typeId,
            stateless
                ? NodeStateModel::Stateless
                : NodeStateModel::SessionStateful,
            !stateless,
            {LatencyKind::None, 0});
    }
    for (const auto typeId : {
             std::string_view{"talas.feedbackLoopInput"},
             std::string_view{"talas.feedbackLoopOutput"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId,
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }
    registry.setNodeTypeProcessingContract(
        "talas.forwardFftV1",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "frameSize",
            PreparedLatencyRule::ParameterFramesMinusOne,
        });
    registry.setNodeTypeProcessingContract(
        "talas.inverseFftV1",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.fft",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "length",
            PreparedLatencyRule::ParameterFramesMinusOne,
        });
    registry.setNodeTypeProcessingContract(
        "talas.liveSpectralAnalysis",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "lowestAnalyzedFreq",
            PreparedLatencyRule::
                SampleRateOverParameterNextPowerOfTwoMinusOne,
        });
    registry.setNodeTypeProcessingContract(
        "talas.simplePitchShifter",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "interval",
            PreparedLatencyRule::SimplePitchShifterInterval,
        });
    registry.setNodeTypeProcessingContract(
        "talas.frequencyScale",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "frequencyScale",
            PreparedLatencyRule::FrequencyScaleProfile,
        });
    registry.setNodeTypeProcessingContract(
        "talas.monotonizer",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "frequency",
            PreparedLatencyRule::MonotonizerProfile,
        });
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.monotonizer", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.monotonizer", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.monotonizer", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-known-pitch-retargeting-or-range-pitch-tracking-with-zero-or-1023-frame-latency"},
             std::string_view{"sound-fed-or-live-pitch-controls-select-the-active-profile-a-live-field-holding-each-block"},
             std::string_view{"prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry"},
             std::string_view{"prepared-realtime-failure-atomic-retry-requires-locally-transactional-graph-coexistence"},
             std::string_view{"scale-quantization-stereo-multichannel-and-exact-reference-monotonizer-parity-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.monotonizer", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeFrequencyScale",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "frequencyScale",
            PreparedLatencyRule::TimeFrequencyScaleProfile,
        });
    registry.setNodeTypeProcessingContract(
        "talas.diffuseSpectralStretch",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 4095});
    registry.setNodeTypePreparedRealtimeSupport("talas.diffuseSpectralStretch",
        ExecutionSupport::Supported, ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.diffuseSpectralStretch", FanInLatencyPolicy::Align);
    for (const auto limitation :
        {
            std::string_view{"offline-and-prepared-shared-checked-finite-clean-"
                             "room-monophonic-diffuse-spectral-stretch"},
            std::string_view{"static-direct-event-value-or-per-frame-connected-"
                             "reset-gate-and-level-with-static-stretch-smear-"
                             "diffusion-seed-and-immutable-sample"},
            std::string_view{
                "sample-rate-eight-thousand-through-384000-source-"
                "one-through-8388608-finite-samples-stretch-two-"
                "through-thirty-two-unit-smear-and-diffusion-exact-"
                "uint32-seed-and-level-zero-through-four"},
            std::string_view{"compiler-owned-exact-descriptor-plan-settings-"
                             "table-resource-snapshot-parameter-binding-mono-"
                             "output-buffer-producer-scope-4095-frame-latency-"
                             "private-state-and-local-transaction-authority"},
            std::string_view{"allocation-free-prepared-callback-failure-atomic-"
                             "spectrum-phase-magnitude-overlap-cursor-reset-"
                             "gate-tail-retry-and-transport-reset"},
            std::string_view{
                "disk-streaming-live-input-dynamic-spectral-"
                "configuration-multichannel-alternate-fft-profiles-"
                "transient-formant-pitch-preservation-and-exact-"
                "named-algorithm-or-reference-semantics-unsupported"},
        })
    {
        registry.addNodeTypeLimitation(
            "talas.diffuseSpectralStretch", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.scaleVocoder",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.tunableVocoder",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.vocoder",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.vocoderChannelBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.filterBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.filterBankElement",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.chopper",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.chopper",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.chopper",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.chopper", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.chopper", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.chopper", ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"finite-immutable-plain-table-linear-half-open-cycle-interpolation"},
             std::string_view{"static-seconds-event-or-per-frame-connected-positive-finite-duration-controls-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-pre-sample-cycle-shrink-and-wrap"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-zero-frame-failure-atomic-transport-reset-and-partition-stable-retry"},
             std::string_view{"file-stream-memory-writer-overlap-randomized-density-spatial-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.chopper", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.functionGenerator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.functionGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.functionGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.functionGenerator", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.functionGenerator", ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"finite-immutable-plain-table-or-unique-prior-same-scope-memory-writer-mono-capture-linear-one-shot-interpolation"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-trigger-and-positive-seconds-duration-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-and-duration-retain-resume"},
             std::string_view{"finite-scalar-or-per-frame-mono-sound-level-with-connected-precedence"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"other-memory-writer-routes-file-metadata-multichannel-alternate-interpolation-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.functionGenerator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multisegmentEnvelope",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.multisegmentEnvelope",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.multisegmentEnvelope",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multisegmentEnvelope",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.multisegmentEnvelope",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-static-or-hot-brace-list-checked-piecewise-linear-envelope-with-segments-taking-hot-values-as-they-start-and-slope-preserving-loop-drift"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-unit-level-and-finite-gate-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-falling-edge-continuation-and-same-frame-control-ordering"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-static-loop-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"list-length-changes-dynamic-loops-proprietary-release-lifecycle-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multisegmentEnvelope", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.graphicalEnvelope",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.graphicalEnvelope",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.graphicalEnvelope",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.graphicalEnvelope",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.graphicalEnvelope",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-static-alternating-time-value-checked-piecewise-linear-envelope-with-loop-marks-repeating-while-gate-is-on"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-unit-level-finite-gate-and-positive-rate-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-level-before-output-gate-retrigger-rate-after-output-and-falling-edge-continuation"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"graphical-editor-metadata-hot-shape-expressions-nonlinear-segments-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.graphicalEnvelope", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.noise",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.noise",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.noise", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.noise", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.noise", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-white-pink-and-hot-pink-transition"},
             std::string_view{"static-direct-event-or-connected-finite-frequency-center-and-scale-controls"},
             std::string_view{"compiler-owned-exact-plan-settings-control-bindings-and-buffer-authority"},
             std::string_view{"allocation-free-zero-frame-failure-atomic-transport-reset-copy-replacement-and-partition-stable-retry"},
             std::string_view{"clean-room-generator-algorithm-exact-reference-statistical-seed-phase-and-sound-parity-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.noise", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.constant",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.constant",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.constant", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.constant", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-scalar-broadcast"},
             std::string_view{"exact-signed-zero-subnormal-ordinary-and-maximum-finite-bits-per-scheduler-segment"},
             std::string_view{"compiler-owned-exact-plan-parameter-expression-output-buffer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-callback-failure-atomic-retry-copy-reset-replacement-and-fan-out"},
             std::string_view{"non-finite-unit-polymorphic-audio-vector-matrix-complex-string-resource-script-random-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.constant", std::string{limitation});
    }
    registry.setNodeTypeOfflineSupport(
        "talas.valToSound",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.valToSound",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.valToSound",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.valToSound", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.valToSound", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.valToSound", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-scalar-event-expression-control-to-audio-bridge"},
             std::string_view{"one-millisecond-control-ticks-with-causal-linear-interpolation"},
             std::string_view{"prepared-realtime-allocation-free-checked-state-and-reset"},
             std::string_view{"stateful-timed-expression-exact-reference-tick-phase-and-latency-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.valToSound", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.absoluteValue",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.absoluteValue",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.absoluteValue",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.absoluteValue",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-magnitude"},
             std::string_view{"exact-positive-zero-subnormal-and-maximum-finite-bit-semantics"},
             std::string_view{"compiler-owned-exact-plan-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-complex-vector-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.absoluteValue", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.arcTan",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.arcTan",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.arcTan",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.arcTan",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-platform-arctangent"},
             std::string_view{"exact-signed-zero-subnormal-ordinary-and-maximum-finite-result-bits"},
             std::string_view{"compiler-owned-exact-plan-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-complex-vector-audio-bus-degree-script-alternate-libm-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.arcTan", std::string{limitation});
    }
    for (const auto typeId : {"talas.linearToLog", "talas.logToLinear"})
    {
        registry.setNodeTypeOfflineSupport(
            typeId, ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypePreparedRealtimeSupport(
            typeId, ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::Stateless, false,
            {LatencyKind::None, 0});
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        registry.setNodeTypeResourceAccess(
            typeId, ResourceAccessMode::None);
        for (const auto limitation : {
                 std::string_view{"finite-mono-control-and-audio-natural-logarithm-and-exponential-candidate"},
                 std::string_view{"compiler-owned-exact-plan-input-output-buffer-and-zero-latency-authority"},
                 std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
                 std::string_view{"multichannel-bus-alternate-base-normalization-and-exact-reference-semantics-unsupported"},
             })
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
        registry.addNodeTypeLimitation(
            typeId,
            typeId == std::string_view{"talas.linearToLog"}
                ? "strictly-positive-finite-linear-input"
                : "finite-log-input-and-exponential-output");
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.add",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.add",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.add",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.add",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-addition"},
             std::string_view{"exact-operand-order-signed-zero-cancellation-subnormal-adjacent-and-maximum-finite-result-bits"},
             std::string_view{"compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-saturating-compensated-variadic-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.add", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.mixer",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.mixer",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.mixer", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.mixer", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.mixer", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-unfused-binary64-weighted-sum"},
             std::string_view{"static-fallback-or-connected-finite-gains-with-connected-precedence"},
             std::string_view{"compiler-owned-exact-plan-dual-audio-optional-gain-output-buffer-and-aligned-fan-in-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-fma-saturating-normalized-variadic-stereo-multichannel-complex-vector-matrix-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.mixer", std::string{limitation});
    }
    registry.setNodeTypeReplacement("talas.mixer", "talas.mixerV2");
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.mixerV2",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.mixerV2",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.mixerV2", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.mixerV2", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.mixerV2", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-checked-finite-sums-of-any-number-of-mono-or-stereo-inputs"},
             std::string_view{"stereo-input-right-channels-pair-by-source-mono-inputs-heard-on-both"},
             std::string_view{"static-live-or-connected-finite-bipolar-left-and-right-output-levels"},
             std::string_view{"aligned-fan-in-and-zero-latency"},
             std::string_view{"multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.mixerV2", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.level",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.level",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.level", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.level", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.level", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-mono-to-stereo-gain"},
             std::string_view{"static-fallback-or-connected-finite-independent-gains-with-connected-precedence"},
             std::string_view{"compiler-owned-exact-plan-audio-optional-gain-dual-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"no-gain-boolean-is-an-inert-clean-room-compatibility-parameter"},
             std::string_view{"non-finite-linked-normalized-logarithmic-db-pan-law-stereo-input-multichannel-complex-vector-matrix-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.level", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.channelCrosser",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.channelCrosser",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.channelCrosser", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.channelCrosser", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.channelCrosser", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-separately-rounded-binary64-2x2-matrix"},
             std::string_view{"static-fallback-or-connected-finite-coefficients-with-connected-precedence"},
             std::string_view{"missing-right-input-is-exact-positive-zero"},
             std::string_view{"compiler-owned-exact-plan-optional-input-control-dual-output-buffer-and-aligned-fan-in-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-fused-reassociated-normalized-pan-law-larger-multichannel-complex-vector-matrix-object-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.channelCrosser", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.channelJoin",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.channelJoin",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.channelJoin", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.channelJoin", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.channelJoin", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-bit-exact-binary64-dual-lane-copy"},
             std::string_view{"required-ordered-independent-mono-left-and-right-inputs"},
             std::string_view{"compiler-owned-exact-plan-dual-input-producer-dual-output-buffer-and-aligned-fan-in-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-optional-missing-stereo-bus-channel-conversion-swap-crossfeed-matrix-gain-pan-mix-vector-complex-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.channelJoin", std::string{limitation});
    }
    registry.setNodeTypeOfflineSupport(
        "talas.complexProduct",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.complexProduct",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.complexProduct",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.complexProduct", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.complexProduct", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.complexProduct", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-checked-finite-stereo-complex-multiplication"},
             std::string_view{"required-four-ordered-mono-lane-inputs-and-atomic-dual-output"},
             std::string_view{"allocation-free-prepared-callback-and-whole-block-failure-atomic-retry"},
             std::string_view{"packed-stereo-bus-non-finite-and-exact-reference-rounding-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.complexProduct", std::string{limitation});
    }
    registry.setNodeTypeOfflineSupport(
        "talas.clippingDivide", ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.clippingDivide", ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.clippingDivide", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.clippingDivide", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.clippingDivide", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.clippingDivide", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-checked-finite-numerator-over-nonzero-denominator-times-scale"},
             std::string_view{"static-or-connected-per-frame-scale-with-connected-precedence"},
             std::string_view{"compiler-owned-exact-descriptor-plan-binding-buffer-producer-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-and-whole-block-failure-atomic-retry"},
             std::string_view{"zero-denominator-clipping-threshold-multichannel-special-values-and-exact-reference-semantics-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.clippingDivide", std::string{limitation});
    }
    for (const auto typeId : {"talas.minimum", "talas.maximum"})
    {
        registry.setNodeTypeOfflineSupport(
            typeId, ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypePreparedRealtimeSupport(
            typeId, ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::Stateless, false,
            {LatencyKind::None, 0});
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::Align);
        registry.setNodeTypeResourceAccess(
            typeId, ResourceAccessMode::None);
        registry.setNodeTypeOutputBusLayouts(
            typeId, {AudioBusLayoutKind::Stereo});
        for (const auto limitation : {
                 std::string_view{"offline-and-prepared-finite-per-sample-per-channel-three-stereo-input-extremum"},
                 std::string_view{"six-required-ordered-mono-lanes-and-atomic-dual-output"},
                 std::string_view{"allocation-free-prepared-callback-and-whole-block-failure-atomic-retry"},
                 std::string_view{"arbitrary-input-count-requires-composition-packed-stereo-bus-special-values-and-exact-reference-ties-unverified"},
             })
        {
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
        }
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.channeller",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.channeller",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.channeller", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.channeller", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.channeller", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-bit-exact-binary64-live-channel-selection"},
             std::string_view{"checked-channel-on-both-outputs-both-pass-through-neither-silent"},
             std::string_view{"mono-audio-in-and-optional-right-in-with-static-boolean-or-connected-strictly-positive-selectors"},
             std::string_view{"compiler-owned-exact-plan-audio-selector-fallback-dual-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-threshold-hysteresis-smoothing-edge-state-stereo-bus-crossfeed-matrix-gain-pan-mix-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.channeller", std::string{limitation});
    }
    registry.setNodeTypeOfflineSupport(
        "talas.binauralPan",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.binauralPan",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.binauralPan", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.binauralPan", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.binauralPan", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.binauralPan", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-mono-to-stereo-interaural-delay-and-intensity-candidate"},
             std::string_view{"static-event-or-per-frame-connected-normalized-pan-left-center-right"},
             std::string_view{"local-600-microsecond-maximum-linear-interpolated-far-ear-delay-and-half-intensity"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"reference-delay-expressions-head-geometry-frequency-dependent-shadowing-multichannel-input-and-exact-numeric-output-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.binauralPan", std::string{limitation});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.pan",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.pan",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.pan", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.pan", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.pan",
        {
            AudioBusLayoutKind::Stereo,
            AudioBusLayoutKind::Discrete4,
            AudioBusLayoutKind::Discrete8,
        });
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-stereo-and-discrete-ring-pan"},
             std::string_view{"required-mono-audio-with-static-or-connected-normalized-pan-and-non-negative-scale"},
             std::string_view{"linear-or-equal-power-stereo-and-equal-power-discrete4-or-discrete8-layouts"},
             std::string_view{"compiler-owned-exact-plan-control-fallback-active-layout-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-layout-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-alternate-pan-law-arbitrary-speaker-geometry-elevation-distance-smoothing-multichannel-input-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.pan", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.crossfade",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.crossfade",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.crossfade", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.crossfade", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.crossfade", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-linear-or-equal-power-input-crossfade-with-equal-power-stereo-pan"},
             std::string_view{"required-ordered-dual-mono-audio-with-static-or-connected-normalized-pan-non-negative-scale-and-normalized-fade"},
             std::string_view{"compiler-owned-exact-plan-control-fallback-dual-output-aligned-fan-in-and-zero-latency-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-additional-input-stereo-bus-multichannel-alternate-law-smoothing-limiting-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.crossfade", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.audioOutput",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-exact-binary64-mono-or-ordered-stereo-terminal-copy"},
             std::string_view{"required-mono-audio-and-optional-mono-right-input-with-no-implicit-duplication-or-conversion"},
             std::string_view{"compiler-owned-exact-descriptor-terminal-binding-producer-buffer-scope-latency-and-channel-index-authority"},
             std::string_view{"allocation-free-whole-callback-candidate-publication"},
             std::string_view{"physical-device-selection-channel-mapping-format-conversion-hardware-gain-clocking-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.audioOutput", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.stereoMix2",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.stereoMix2",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.stereoMix2", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.stereoMix2", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.stereoMix2", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-ordered-two-input-equal-power-stereo-mix"},
             std::string_view{"required-ordered-dual-mono-audio-with-static-or-connected-bipolar-output-levels-normalized-pans-and-normalized-scales"},
             std::string_view{"compiler-owned-exact-plan-control-fallback-dual-output-aligned-fan-in-and-zero-latency-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-additional-input-stereo-bus-multichannel-alternate-law-smoothing-limiting-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.stereoMix2", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.stereoMix4",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.stereoMix4",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.stereoMix4", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.stereoMix4", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.stereoMix4", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-ordered-four-input-equal-power-stereo-mix"},
             std::string_view{"required-ordered-four-mono-audio-with-static-or-connected-bipolar-output-levels-normalized-pans-and-normalized-scales"},
             std::string_view{"compiler-owned-exact-plan-control-fallback-dual-output-aligned-fan-in-and-zero-latency-authority"},
             std::string_view{"allocation-free-dual-output-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-additional-input-stereo-bus-multichannel-alternate-law-smoothing-limiting-resource-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.stereoMix4", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.difference",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.difference",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.difference",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.difference",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-subtraction"},
             std::string_view{"exact-operand-order-signed-zero-cancellation-subnormal-adjacent-and-maximum-finite-result-bits"},
             std::string_view{"compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-saturating-epsilon-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.difference", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.product",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.product",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.product",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.product",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-binary64-multiplication"},
             std::string_view{"exact-signed-zero-subnormal-underflow-and-maximum-finite-result-bits"},
             std::string_view{"compiler-owned-exact-plan-any-number-of-inputs-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-saturating-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.product", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.sqrtMagnitude",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.sqrtMagnitude",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sqrtMagnitude", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.sqrtMagnitude", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.sqrtMagnitude", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-native-binary64-hypot"},
             std::string_view{"exact-positive-magnitude-signed-zero-subnormal-boundary-and-finite-overrange-saturation"},
             std::string_view{"compiler-owned-exact-plan-optional-dual-audio-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"complex-vector-matrix-multichannel-control-script-alternate-libm-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sqrtMagnitude", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.vca",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-native-binary64-multiplication"},
             std::string_view{"static-live-expression-or-connected-finite-mono-gain-with-connected-precedence"},
             std::string_view{"compiler-owned-exact-plan-parameter-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-saturating-stereo-multichannel-complex-vector-matrix-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.vca", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.equality",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.equality",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.equality",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.equality",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-exact-binary64-equality"},
             std::string_view{"exact-canonical-boolean-signed-zero-adjacent-subnormal-and-maximum-finite-result-bits"},
             std::string_view{"compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"live-finite-non-negative-tolerance-field"},
             std::string_view{"non-finite-ordered-complex-vector-string-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.equality", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.setRange",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.setRange",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.setRange",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.setRange",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-ordered-binary64-affine-mapping"},
             std::string_view{"static-fallback-or-independent-connected-old-and-new-range-control-authority"},
             std::string_view{"compiler-owned-exact-plan-parameter-optional-binding-buffer-and-zero-latency-authority"},
             std::string_view{"allocation-free-whole-block-failure-atomic-retry"},
             std::string_view{"non-finite-clamping-epsilon-range-sort-vector-complex-audio-bus-script-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.setRange", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multislopeFunctionGenerator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.multislopeFunctionGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.multislopeFunctionGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multislopeFunctionGenerator",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.multislopeFunctionGenerator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-static-or-hot-brace-list-checked-piecewise-linear-one-shot-envelope-with-segments-taking-hot-values-as-they-start"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-level-gate-and-positive-rate-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-falling-edge-continuation-and-same-frame-control-ordering"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"list-length-changes-loops-nonlinear-segments-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multislopeFunctionGenerator",
            std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.grainCloud",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.sampleCloud",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.cloudBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.cloudBankElement",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.partialSpectrumLegacyHoldV1",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.pastedSoundExpressionV1",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.spectrumTrackSelector",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeEventTransportContract(
        "talas.spectrumTrackSelector", false, true);
    registry.setNodeTypeProcessingContract(
        "talas.spectrumFundamental",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeEventTransportContract(
        "talas.spectrumFundamental", false, true);
    registry.setNodeTypeProcessingContract(
        "talas.partialSpectrumTrackSelectorV1",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.addNodeTypeLimitation(
        "talas.partialSpectrumTrackSelectorV1",
        "frame-synchronizer-only-typed-input-updated-on-frame-completion-sample");
    registry.addNodeTypeLimitation(
        "talas.partialSpectrumTrackSelectorV1",
        "local-timing-rule-without-installed-reference-comparison");
    registry.setNodeTypeProcessingContract(
        "talas.partialTrackPlayerV1",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.partialTrackPlayerV1",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter,
        "prepared-realtime-maximum-128-tracks-per-segment");
    registry.setNodeTypeOfflineSupport(
        "talas.partialTrackPlayerV1",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeOfflineSupport(
        "talas.fft",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.fft",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeEventTransportContract(
        "talas.fft",
        false,
        true);
    for (const auto typeId : {
             std::string_view{"talas.liveSpectralAnalysis"},
             std::string_view{"talas.partialSpectrumLegacyHoldV1"},
             std::string_view{"talas.partialSpectrumTrackSelectorV1"},
             std::string_view{"talas.pastedSoundExpressionV1"},
         })
    {
        registry.setNodeTypeOfflineSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeEventTransportContract(
            typeId,
            false,
            true);
    }
    for (const auto typeId : {
             std::string_view{"talas.forwardFftV1"},
             std::string_view{"talas.inverseFftV1"},
         })
    {
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeEventTransportContract(
            typeId,
            false,
            true);
    }
    for (const auto typeId : {
             std::string_view{"talas.audioOutput"},
             std::string_view{"talas.level"},
             std::string_view{"talas.channelCrosser"},
             std::string_view{"talas.channelJoin"},
             std::string_view{"talas.complexProduct"},
             std::string_view{"talas.clippingDivide"},
             std::string_view{"talas.minimum"},
             std::string_view{"talas.maximum"},
             std::string_view{"talas.channeller"},
             std::string_view{"talas.mixer"},
             std::string_view{"talas.sqrtMagnitude"},
             std::string_view{"talas.overlappingMixer"},
             std::string_view{"talas.pan"},
             std::string_view{"talas.horizontalAmbisonicEncoder"},
             std::string_view{"talas.horizontalAmbisonicDecoder"},
             std::string_view{"talas.crossfade"},
             std::string_view{"talas.stereoMix2"},
             std::string_view{"talas.stereoMix4"},
             std::string_view{"talas.matrix4"},
             std::string_view{"talas.matrix8"},
             std::string_view{"talas.output4"},
             std::string_view{"talas.output6"},
             std::string_view{"talas.output8"},
             std::string_view{"talas.stereoInOutput4"},
             std::string_view{"talas.stereoInOutput8"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId,
            NodeStateModel::Stateless,
            false,
            {LatencyKind::None, 0});
    }
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.overlappingMixer",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.overlappingMixer",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.overlappingMixer",
        ResourceAccessMode::None);
    registry.addNodeTypeLimitation(
        "talas.overlappingMixer",
        "prepared-realtime-checked-four-input-stereo-zero-overlap");
    registry.addNodeTypeLimitation(
        "talas.overlappingMixer",
        "prepared-realtime-zero-latency-input-ancestry");
    for (const auto typeId : {
             std::string_view{"talas.highShelvingFilter"},
             std::string_view{"talas.lowShelvingFilter"},
             std::string_view{"talas.presenceFilter"},
         })
    {
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeProcessingContract(
            typeId,
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
        registry.setNodeTypeOutputBusLayouts(
            typeId,
            {AudioBusLayoutKind::Mono});
        for (const auto limitation : {
                 std::string_view{"offline-and-prepared-shared-checked-finite-mono-shelving-presence-biquad"},
                 std::string_view{"stateless-event-value-or-connected-frequency-bandwidth-boost-cut-and-scale-controls"},
                 std::string_view{"open-nyquist-positive-frequency-positive-presence-bandwidth-finite-boost-cut-scale-and-connected-precedence"},
                 std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
                 std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-state-schedule-retry-reset-and-zero-frame-no-op"},
                 std::string_view{"stateful-variable-general-identifier-typed-event-multichannel-alternate-slope-oversampled-and-exact-reference-coefficient-semantics-unsupported"},
             })
        {
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
        }
    }
    registry.setNodeTypeProcessingContract(
        "talas.twoFormantElement",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.twoFormantElement",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeProcessingContract(
        "talas.twoFormantVoiceElement",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.twoFormantVoiceElement",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeProcessingContract(
        "talas.dualParallelTwoPoleFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.dualParallelTwoPoleFilter",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeProcessingContract(
        "talas.graphicEq",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.graphicEq",
        {AudioBusLayoutKind::Mono});
    for (const auto typeId : {
             std::string_view{"talas.twoFormantElement"},
             std::string_view{"talas.twoFormantVoiceElement"},
             std::string_view{"talas.dualParallelTwoPoleFilter"},
             std::string_view{"talas.graphicEq"},
         })
    {
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        for (const auto limitation : {
                 std::string_view{"offline-and-prepared-shared-checked-finite-mono-c17-biquad-family"},
                 std::string_view{"exact-two-formant-two-formant-voice-static-pole-and-seven-band-profiles"},
                 std::string_view{"stateless-event-value-or-connected-formant-bandwidth-scale-frequency-and-band-level-controls"},
                 std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
                 std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-phase-schedule-retry-reset-and-zero-frame-no-op"},
                 std::string_view{"stateful-variable-general-identifier-live-pole-typed-event-multichannel-alternate-coefficient-and-exact-reference-semantics-unsupported"},
             })
        {
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
        }
    }
    registry.setNodeTypeProcessingContract(
        "talas.analysisFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.analysisFilter",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOfflineSupport(
        "talas.analysisFilter",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.analysisFilter",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.analysisFilter", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.analysisFilter", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-one-period-harmonic-window-quadrature-with-format-53-legacy-bandpass"},
             std::string_view{"static-or-direct-event-value-positive-fundamental-and-nonnegative-harmonic-with-connected-input-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-window-resynchronized-on-fundamental-or-harmonic-change"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-stereo-state-retry-and-transport-reset"},
             std::string_view{"exact-reference-multiband-live-tracking-arbitrary-expression-and-polyphonic-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.analysisFilter", std::string{limitation});
    }
    for (const auto typeId : {
             std::string_view{"talas.firFilter"},
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId,
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
        registry.setNodeTypeOutputBusLayouts(
            typeId,
            {AudioBusLayoutKind::Mono});
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        for (const auto limitation : {
                 std::string_view{"offline-and-prepared-shared-checked-finite-mono-static-fir-one-through-4096-taps"},
                 std::string_view{"exact-fir-static-coefficient-text-or-reresonator-immutable-table-and-static-time-index-profile"},
                 std::string_view{"compiler-sealed-expression-language-definitions-and-exact-resource-snapshot-identity"},
                 std::string_view{"compiler-owned-exact-descriptor-plan-settings-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
                 std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-retry-reset-and-zero-frame-no-op"},
                 std::string_view{"dynamic-coefficients-stateful-variable-general-identifier-mutable-table-oversized-taps-typed-event-multichannel-and-exact-reference-semantics-unsupported"},
             })
        {
            registry.addNodeTypeLimitation(typeId, std::string{limitation});
        }
    }
    registry.setNodeTypeResourceAccess(
        "talas.firFilter",
        ResourceAccessMode::None);
    // ADR-0022: the time-varying all-pole lattice of an RE table.
    registry.setNodeTypeProcessingContract(
        "talas.reResonator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.reResonator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.reResonator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-mono-all-pole-lattice-of-order-one-through-128"},
             std::string_view{"re-table-of-our-own-format-with-reflection-coefficients-and-per-sample-time-index"},
             std::string_view{"external-re-files-unsupported-and-legacy-fir-reading-before-format-63"},
         })
    {
        registry.addNodeTypeLimitation("talas.reResonator", std::string{limitation});
    }
    registry.setNodeTypeResourceAccess(
        "talas.reResonator",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeProcessingContract(
        "talas.harmonicResonator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.harmonicResonator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.harmonicResonator",
        ResourceAccessMode::VariableOrUnknown);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.harmonicResonator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-mono-private-memory-harmonic-resonator"},
             std::string_view{"allpass-fractional-period-sixty-db-decay-static-brightness-and-live-decay-frequency-scale-profile"},
             std::string_view{"stateless-event-value-or-connected-decay-frequency-scale-and-bounded-history-authority"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-private-sentinel-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-delay-brightness-schedule-retry-reset-and-zero-frame-no-op"},
             std::string_view{"dynamic-brightness-stateful-variable-general-identifier-named-shared-memory-unbounded-history-typed-event-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.harmonicResonator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.modalFilter", NodeStateModel::SessionStateful,
        true, {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.modalFilter", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.modalFilter", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"bounded-sixteen-parallel-bandpass-modes-static-array-amplitudes-center-frequencies-and-bandwidth-ratios"},
             std::string_view{"live-frequency-scale-and-at-least-octave-downward-jump-reset-clear-all-mode-history"},
             std::string_view{"exact-reference-reset-threshold-filter-coefficients-feedback-shaping-other-mode-types-and-dynamic-mode-arrays-unverified-or-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.modalFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.dynamicRangeController",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::VariableOrUnknown, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.dynamicRangeController",
        FanInLatencyPolicy::PreserveRelative);
    registry.setNodeTypeOutputBusLayouts(
        "talas.dynamicRangeController",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.dynamicRangeController",
        ResourceAccessMode::None);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.dynamicRangeController",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-mono-dynamic-range-controller"},
             std::string_view{"static-compressor-or-downward-expander-and-stateless-event-value-or-connected-ratio-threshold-attack-release-delay-gain-profile"},
             std::string_view{"bounded-integer-delay-variable-latency-preserve-relative-zero-latency-input-ancestry-authority"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-envelope-delay-schedule-retry-reset-and-zero-frame-no-op"},
             std::string_view{"multiband-soft-knee-alternate-detector-stereo-linking-sidechain-filtering-unbounded-lookahead-stateful-variable-general-identifier-typed-event-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.dynamicRangeController", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.vcf",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.vcf",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.vcf",
        ResourceAccessMode::None);
    registry.addNodeTypeLimitation(
        "talas.vcf",
        "prepared-realtime-checked-four-pole-tpt-mono-only");
    registry.setNodeTypeProcessingContract(
        "talas.audioInput",
        NodeStateModel::SessionStateful,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.sampleMapPlayer",
        NodeStateModel::SessionStateful,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.midiMapper",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeEventTransportContract(
        "talas.midiMapper",
        true,
        false);
    registry.setNodeTypeProcessingContract(
        "talas.analogSequencer",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.analogSequencer",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.analogSequencer",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-maximum-256-static-step-mono-in-stereo-out-amplitude-sequencer"},
             std::string_view{"shorter-finite-static-sequences-repeat-final-value-through-longest-sequence"},
             std::string_view{"zero-based-start-end-indices-with-format-38-legacy-one-based-migration"},
             std::string_view{"static-or-event-value-or-per-frame-connected-bipolar-left-right-finite-gate-non-negative-integer-step-and-signed-nonzero-rate-controls-with-nonzero-step-within-selected-range-and-connected-precedence"},
             std::string_view{"prepared-realtime-allocation-free-reset-zero-frame-partition-stable-failure-atomic-retry"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-parameter-binding-buffer-producer-scope-zero-latency-authority"},
             std::string_view{"key-mode-stage-sampled-note-arrays-live-or-connected-nonzero-rate-gate-step-duration-mask-timed-controls-named-extra-values-and-generated-index-no-exact-reference-semantics"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.analogSequencer", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.euSeq", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.euSeq", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.euSeq", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.euSeq", {AudioBusLayoutKind::Mono});
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-maximum-256-step-euclidean-beat-pattern-with-optional-per-frame-step-rate-and-reset"},
             std::string_view{"one-through-96-scale-steps-in-one-through-96-equal-octave-divisions-with-positive-base-frequency-below-half-nyquist"},
             std::string_view{"control-gate-frequency-and-zero-based-step-outputs-with-pitch-advancing-on-active-beats"},
             std::string_view{"bjorklund-rotation-exact-reference-parameter-syntax-key-events-timbre-and-note-duration-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.euSeq", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.warpedTimeIndex",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.warpedTimeIndex",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.warpedTimeIndex",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.warpedTimeIndex",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.warpedTimeIndex",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"equal-2-through-4096-strictly-ascending-finite-seconds-point-arrays-whose-braced-elements-may-follow-event-values"},
             std::string_view{"shared-checked-finite-convex-map-normalization-trigger-rate-and-playback-state-transition"},
             std::string_view{"static-scalar-event-or-per-frame-connected-trigger-rate-and-morph-with-connected-precedence"},
             std::string_view{"rising-edge-restart-closed-live-trigger-continuation-and-immutable-closed-trigger-pristine-reset"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"reverse-playback-pause-on-trigger-close-and-exact-reference-scheduling-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.warpedTimeIndex", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multiplyingWaveshaper",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multiplyingWaveshaper",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.multiplyingWaveshaper",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.multiplyingWaveshaper",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-immutable-normalized-wavetable-product"},
             std::string_view{"prepared-realtime-allocation-free-stateless-connected-positive-scale"},
             std::string_view{"offline-and-prepared-two-input-latency-aligned-ancestry"},
             std::string_view{"editor-detector-delay-file-memory-writer-and-domain-metadata-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multiplyingWaveshaper", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.inputOutputCharacteristic",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.inputOutputCharacteristic",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.inputOutputCharacteristic",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.inputOutputCharacteristic",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-step-linear-hermite-curve"},
             std::string_view{"prepared-realtime-allocation-free-stateless-fixed-length-breakpoints"},
             std::string_view{"non-finite-upstream-input-fails-closed-without-upstream-state-recovery"},
             std::string_view{"bounded-val-array-members-inherit-nearest-scalar-event-values-with-child-shadowing"},
             std::string_view{"general-smalltalk-array-expressions-mutable-length-and-exact-reference-spline-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.inputOutputCharacteristic", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.scaleAndOffset",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.scaleAndOffset",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.scaleAndOffset",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.scaleAndOffset",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-dual-control-lane-affine-transform"},
             std::string_view{"static-or-connected-bounded-independent-left-and-right-scale-and-offset-precedence-profile"},
             std::string_view{"absent-right-value-duplicates-left-input-before-independent-right-affine-transform"},
             std::string_view{"zero-latency-align-stateless-no-reset-two-owned-control-output-authority"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-finite-stage-and-fma-identity"},
             std::string_view{"packed-stereo-sound-object-multichannel-unbounded-control-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.scaleAndOffset", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.interpolate",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.interpolate",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.interpolate",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.interpolate",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-dual-control-lane-convex-interpolation"},
             std::string_view{"static-or-connected-bounded-left-and-right-amount-precedence-profile"},
             std::string_view{"zero-latency-align-stateless-no-reset-two-owned-control-output-authority"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-signed-zero-fma-and-convex-split-identity"},
             std::string_view{"packed-stereo-audio-multichannel-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-non-convex-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.interpolate", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.runningMax",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.runningMax",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.runningMax",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.runningMax",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.runningMax",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-control-session-running-maximum-with-positive-zero-reset-baseline"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-reset-before-current-value"},
             std::string_view{"connected-control-precedence-and-sample-accurate-direct-event-value-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"multichannel-sound-object-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.runningMax", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.runningMin",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.runningMin",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.runningMin",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.runningMin", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.runningMin", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-control-session-running-minimum-with-selected-maximum-amplitude-reset-baseline"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-reset-and-maximum-amplitude-before-current-value"},
             std::string_view{"independent-connected-control-precedence-and-sample-accurate-direct-event-value-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"multichannel-sound-object-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.runningMin", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.amplitudeFollower",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.amplitudeFollower",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.amplitudeFollower",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.amplitudeFollower", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.amplitudeFollower", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"mono-finite-absolute-amplitude-envelope-with-established-zero-point-four-time-constant-calibration"},
             std::string_view{"static-scalar-event-or-per-frame-connected-positive-seconds-time-constant-and-non-negative-scalar-scale"},
             std::string_view{"connected-control-precedence-and-sample-accurate-direct-event-value-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"multichannel-independent-attack-release-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.amplitudeFollower", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.biDiFollower",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.biDiFollower",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.biDiFollower",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.biDiFollower", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.biDiFollower", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.biDiFollower", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"mono-signed-audio-rate-directional-exponential-follower-with-distinct-attack-and-release-seconds"},
             std::string_view{"finite-nonnegative-static-or-direct-event-value-times-and-first-sample-target-initialization"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-state-and-transport-reset"},
             std::string_view{"multichannel-exact-reference-time-constants-initial-state-and-pcm-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.biDiFollower", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.lossyIntegrator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.lossyIntegrator",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.lossyIntegrator",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.lossyIntegrator", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.lossyIntegrator", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.lossyIntegrator", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"mono-audio-rate-exponentially-weighted-integral-with-zero-initial-state"},
             std::string_view{"finite-scale-and-nonnegative-release-seconds-with-direct-event-value-controls"},
             std::string_view{"release-time-one-over-e-decay-after-zero-input-and-sample-accurate-scheduled-parameter-updates"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-state-and-transport-reset"},
             std::string_view{"exact-reference-scale-units-coefficients-initial-state-zero-controls-and-multichannel-behavior-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.lossyIntegrator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.hysteresis", NodeStateModel::SessionStateful, true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.hysteresis", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.hysteresis", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.hysteresis", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.hysteresis", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"bounded-independent-control-and-mono-audio-play-operator-lanes"},
             std::string_view{"finite-nonnegative-width-and-zero-initial-state"},
             std::string_view{"allocation-free-prepared-callback-rollback-and-transport-reset"},
             std::string_view{"reference-hysteresis-fields-loop-curve-initial-state-and-multichannel-semantics-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.hysteresis", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.peakDetector",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.peakDetector",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.peakDetector",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.peakDetector", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.peakDetector", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"mono-finite-asymmetric-peak-envelope-with-strict-scaled-amplitude-branch-selection"},
             std::string_view{"static-scalar-event-or-per-frame-connected-positive-seconds-attack-release-and-non-negative-scalar-scale"},
             std::string_view{"connected-control-precedence-and-sample-accurate-direct-event-value-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"multichannel-arbitrary-window-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.peakDetector", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.frequencyTracker",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::VariableOrUnknown, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.frequencyTracker", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.frequencyTracker", ResourceAccessMode::None);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.frequencyTracker",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-monophonic-yin-callback-window-tracker"},
             std::string_view{"static-or-connected-bounded-min-max-confidence-scale-default-detectors-and-static-emphasis-profile"},
             std::string_view{"bounded-preallocated-history-variable-latency-align-zero-latency-audio-ancestry-authority"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-previous-input-control-retry-reset-and-zero-frame-no-op"},
             std::string_view{"sample-progressive-partition-invariant-polyphonic-non-default-detector-dynamic-emphasis-multichannel-typed-event-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.frequencyTracker", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.ar",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.ar",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.ar",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.ar", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.ar", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-linear-or-exponential-ar-envelope"},
             std::string_view{"static-or-direct-event-value-positive-attack-release-finite-scale-and-gate-with-connected-gate-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-live-times-scale-and-current-value-retrigger"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset"},
             std::string_view{"connected-time-scale-targeted-events-polyphony-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.ar", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.ramp",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.ramp",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.ramp", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.ramp", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-matched-checked-finite-edge-triggered-ramp"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-selected-finite-trigger-and-positive-renderable-edge-captured-duration"},
             std::string_view{"exact-four-variant-positive-edge-rearm-pre-increment-endpoint-hold-and-repeat-boundary-lifecycle"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-and-reset"},
             std::string_view{"proprietary-curve-zero-negative-duration-explicit-reset-sync-phase-tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.ramp", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.sampleAndHold",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.sampleAndHold",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sampleAndHold", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.sampleAndHold", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-matched-checked-finite-periodic-sample-and-hold"},
             std::string_view{"required-control-input-static-direct-event-value-or-per-frame-connected-positive-renderable-hold-time-and-static-non-negative-renderable-offset"},
             std::string_view{"exact-nearest-frame-offset-live-per-frame-modulo-period-capture-hold-and-absolute-clock-lifecycle"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-clock-held-state-schedule-output-retry-and-reset"},
             std::string_view{"proprietary-clock-reset-sync-phase-tempo-typed-event-interpolation-random-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sampleAndHold", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.samplesFromDiskSingleStep",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.samplesFromDiskSingleStep", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.samplesFromDiskSingleStep", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.samplesFromDiskSingleStep",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-immutable-table-single-step-playback"},
             std::string_view{"static-or-connected-integral-position-positive-level-trigger-and-gate-edge-restart"},
             std::string_view{"prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry"},
             std::string_view{"prepared-realtime-failure-atomic-retry-requires-locally-transactional-graph-coexistence"},
             std::string_view{"exact-reference-disk-streaming-file-decoding-multichannel-rate-conversion-looping-and-fractional-playback-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.samplesFromDiskSingleStep", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.singleSideBandRm",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.singleSideBandRm",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.singleSideBandRm",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.singleSideBandRm", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.singleSideBandRm", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.singleSideBandRm", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-stereo-phase-pair-and-summed-quadrature-frequency-shift"},
             std::string_view{"static-or-direct-event-value-positive-frequency-below-nyquist-and-positive-frequency-scale-with-connected-input-precedence"},
             std::string_view{"sample-accurate-direct-event-value-frequency-and-frequency-scale-scheduling-with-signed-wrapped-quadrature-phase"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-two-filter-and-carrier-state-retry-and-transport-reset"},
             std::string_view{"exact-reference-coefficients-gain-transients-nonlinearity-broadband-behavior-frequency-scale-range-aliasing-smoothing-and-polyphony-semantics-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.singleSideBandRm", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.sosOscillators",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.sosOscillators",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.sosOscillators",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sosOscillators", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.sosOscillators", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.sosOscillators", ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-static-spectrum-sos-additive-resynthesis-or-demoted-any-spectrum-phase-continuous-bank"},
             std::string_view{"static-context-free-positive-integral-selected-partial-range-and-cascade-first-ordered-sum"},
             std::string_view{"blank-sine-or-compiler-owned-immutable-finite-cyclic-linear-wavetable-generation"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-phase-resource-retry-and-transport-reset"},
             std::string_view{"exact-reference-sosanalysis-file-streaming-dsp-bank-stereo-multichannel-and-polyphony-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sosOscillators", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.phaseShiftBy90",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.phaseShiftBy90",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.phaseShiftBy90",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.phaseShiftBy90", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.phaseShiftBy90", {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.phaseShiftBy90", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-stereo-two-branch-allpass-phase-shift-at-target-frequency"},
             std::string_view{"static-or-direct-event-value-positive-frequency-strictly-below-nyquist-with-connected-input-precedence"},
             std::string_view{"sample-accurate-direct-event-value-frequency-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset"},
             std::string_view{"exact-reference-coefficients-gain-transients-nonlinearity-broadband-hilbert-arbitrary-expression-smoothing-multichannel-and-polyphony-semantics-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.phaseShiftBy90", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.oscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.oscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.oscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.oscillator", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.oscillator", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.oscillator", ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-sine-or-immutable-cyclic-wavetable-oscillator-kernel"},
             std::string_view{"static-or-direct-event-value-or-connected-selected-finite-frequency-amplitude-envelope-pitch-bend-reset-and-bounded-audio-rate-fm"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-positive-reset-before-current-sample-linear-or-nearest-wavetable-and-phase-continuity"},
             std::string_view{"compiler-owned-exact-descriptor-kernel-plan-settings-immutable-resource-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-state-upstream-schedule-retry-and-transport-reset"},
             std::string_view{"exact-reference-band-limiting-negative-frequency-file-backed-and-metadata-wavetables-additional-interpolation-phase-sync-typed-trigger-multichannel-polyphony-and-proprietary-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.oscillator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.antialiasedOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.antialiasedOscillator", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.antialiasedOscillator", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.antialiasedOscillator", ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"immutable-mono-cycle-of-2-through-65536-finite-samples"},
             std::string_view{"complex-fourier-phase-preserving-first-1024-harmonics"},
             std::string_view{"live-or-per-sample-connected-frequency-and-brightness-with-nyquist-harmonic-exclusion"},
             std::string_view{"static-duty-cycle-bounded-range-and-finite-brightness-unit-interval"},
             std::string_view{"mono-or-explicit-left-right-stereo-table-channel"},
             std::string_view{"exact-reference-controls-interpolation-filter-response-and-live-file-updates-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.antialiasedOscillator", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.lfo",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.lfo",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.lfo", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.lfo", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-matched-checked-finite-sine-lfo"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-selected-non-negative-frequency-and-finite-range-endpoints"},
             std::string_view{"exact-radians-pre-increment-sampling-phase-continuity-positive-zero-transport-reset-and-historical-arithmetic-order"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-and-reset"},
             std::string_view{"negative-frequency-alternate-waveforms-explicit-reset-phase-modulation-sync-tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.lfo", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.pulseTrain",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.pulseTrain",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.pulseTrain", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.pulseTrain", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-matched-checked-finite-absolute-elapsed-time-pulse-train"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-positive-period-selected-bounded-duty-and-finite-gate"},
             std::string_view{"exact-pre-increment-pulse-post-increment-fold-gate-close-positive-zero-reset-and-immediate-reopen"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-elapsed-schedule-output-retry-and-reset"},
             std::string_view{"band-limited-phase-offset-sync-tempo-typed-trigger-multichannel-and-exact-reference-pulse-edge-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.pulseTrain", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.pulseGenerator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.pulseGenerator", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.pulseGenerator", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.pulseGenerator", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-zero-dc-normalized-phase-pulse-generator"},
             std::string_view{"static-or-event-value-or-connected-selected-frequency-envelope-duty-reset-and-bounded-audio-rate-fm"},
             std::string_view{"nonzero-reset-before-current-sample-zero-frequency-phase-hold-and-duty-boundary-path-distinction"},
             std::string_view{"prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry"},
             std::string_view{"prepared-realtime-failure-atomic-retry-requires-locally-transactional-graph-coexistence"},
             std::string_view{"exact-reference-aliasing-additional-interpolation-phase-sync-and-typed-trigger-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.pulseGenerator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.rmsSquared",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.rmsSquared",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.rmsSquared",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.rmsSquared", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.rmsSquared", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"mono-finite-exponential-squared-envelope-with-established-zero-point-four-time-constant-calibration"},
             std::string_view{"static-scalar-event-or-per-frame-connected-positive-seconds-time-constant-and-non-negative-scalar-scale"},
             std::string_view{"connected-control-precedence-and-sample-accurate-direct-event-value-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"true-rms-multichannel-arbitrary-window-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.rmsSquared", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.iteratedWaveshaper",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.iteratedWaveshaper",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeOutputBusLayouts(
        "talas.iteratedWaveshaper",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.iteratedWaveshaper",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-immutable-wavetable-feedback"},
             std::string_view{"prepared-realtime-allocation-free-stateless-maximum-4096-iterations"},
             std::string_view{"file-memory-writer-and-wavetable-domain-metadata-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.iteratedWaveshaper", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.saturator", NodeStateModel::Stateless, false,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.saturator", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.saturator", ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.saturator", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.saturator", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.saturator",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo,
            AudioBusLayoutKind::Discrete4});
    for (const auto limitation : {
             std::string_view{"up-to-four-independent-audio-lanes-with-smoothstep-threshold-curve-reaching-half-maximum-at-threshold"},
             std::string_view{"static-direct-event-value-or-connected-per-frame-threshold-and-static-or-direct-event-value-transition-width"},
             std::string_view{"repressor-complement-and-allocation-free-prepared-render-with-late-input-validation"},
             std::string_view{"transition-width-is-a-local-candidate-control-exact-reference-curve-polarity-gain-and-more-than-four-channels-unverified"},
         })
        registry.addNodeTypeLimitation(
            "talas.saturator", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.waveshaper",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.waveshaper",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.waveshaper",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.waveshaper",
        ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-static-polynomial-immutable-wavetable-or-live-mono-memory-writer"},
             std::string_view{"prepared-realtime-allocation-free-stateless-shaping"},
             std::string_view{"stereo-wav-shaping-function-two-independent-output-lanes-with-optional-right-input"},
             std::string_view{"unique-prior-same-scope-memory-writer-function-with-frame-ordered-read-and-rollback"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.waveshaper", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.vca",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.vca", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.vca", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.vca", {AudioBusLayoutKind::Mono});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.averagingLowPassFilter",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.averagingLowPassFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.averagingLowPassFilter",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.averagingLowPassFilter",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.averagingLowPassFilter",
        ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-fractional-rectangular-moving-average"},
             std::string_view{"static-or-per-frame-connected-positive-cutoff-and-finite-bipolar-scale"},
             std::string_view{"private-node-local-fixed-capacity-history-with-connected-cutoff-bound"},
             std::string_view{"sample-progressive-partition-stable-prehistory-scaling-and-oldest-sample-fraction"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-and-scratch-authority"},
             std::string_view{"prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry"},
             std::string_view{"external-shared-wavetable-memory-alternate-windowing-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.averagingLowPassFilter", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.delayWithFeedback",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.delayWithFeedback",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.delayWithFeedback", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.delayWithFeedback",
        {AudioBusLayoutKind::Mono});
    // A named Wavetable shares a delay line (ADR-0025).
    registry.setNodeTypeResourceAccess(
        "talas.delayWithFeedback", ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-comb-or-schroeder-allpass-feedback-delay"},
             std::string_view{"static-live-or-per-frame-connected-scale-delay-delay-scale-feedback-up-to-0-db-and-finite-affine-mix"},
             std::string_view{"delay-scale-history-sized-for-the-full-delay-and-fixed-capacity-for-a-changing-delay"},
             std::string_view{"nearest-frame-or-linear-fractional-delay"},
             std::string_view{"sample-progressive-partition-stable-zero-delay-history-and-transport-reset"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-and-buffer-authority"},
             std::string_view{"prepared-realtime-allocation-free-and-failure-atomic-retry"},
             std::string_view{"named-wavetable-lines-shared-sample-by-sample-prezero-over-transport-restarts-and-half-frame-delay-glide"},
             std::string_view{"multichannel-event-scheduling-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.delayWithFeedback", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.lowPassGate",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.lowPassGate",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.lowPassGate", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.lowPassGate", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-one-pole-low-pass-gate"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-finite-control-clamped-zero-through-one"},
             std::string_view{"compiler-owned-static-ordered-positive-cutoffs-below-nyquist-non-negative-times-bounded-curve-and-finite-scale"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset"},
             std::string_view{"exact-reference-analog-vactrol-dynamic-configuration-resonance-oversampling-stereo-multichannel-and-polyphony-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.lowPassGate", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.reverbSection",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.reverbSection",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.reverbSection", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.reverbSection", ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-private-or-named-shared-memory-comb-or-allpass-delay-section"},
             std::string_view{"static-live-or-per-frame-connected-finite-scale-positive-decay-time-non-negative-delay-and-zero-through-one-delay-scale"},
             std::string_view{"fixed-capacity-history-for-a-changing-delay-with-linear-or-nearest-interpolation-and-sixty-decibel-decay-feedback"},
             std::string_view{"sample-progressive-partition-stable-zero-delay-history-and-transport-reset"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-and-buffer-authority"},
             std::string_view{"prepared-realtime-allocation-free-and-failure-atomic-retry"},
             std::string_view{"named-wavetable-lines-shared-sample-by-sample-with-delays-prezero-over-transport-restarts-and-half-frame-delay-glide"},
             std::string_view{"stereo-multichannel-event-scheduling-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.reverbSection", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.morphFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.morphFilter",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.morphFilter", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.morphFilter", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-state-variable-morph-filter"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-cutoff-resonance-morph-drive-and-scale"},
             std::string_view{"positive-cutoff-below-zero-point-four-five-sample-rate-unit-resonance-and-morph-drive-zero-through-four-scale-zero-through-eight"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset"},
             std::string_view{"alternate-filter-types-smoothing-oversampling-stereo-multichannel-polyphony-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.morphFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.dualMorphOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.dualMorphOscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.dualMorphOscillator", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.dualMorphOscillator", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-dual-morph-oscillator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-frequency-ratio-phase-mod-morph-fold-and-amplitude"},
             std::string_view{"frequency-zero-through-strict-nyquist-ratio-zero-point-125-through-sixteen-with-strict-nyquist-product-phase-mod-zero-through-two-morph-and-fold-zero-through-one-amplitude-zero-through-eight"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-two-output-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-two-phase-two-output-retry-and-transport-reset"},
             std::string_view{"band-limiting-oversampling-hard-sync-wavetables-unison-polyphony-and-exact-named-hardware-or-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.dualMorphOscillator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.quadOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.quadOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.quadOscillator",
        ResourceAccessMode::None);
    registry.addNodeTypeLimitation(
        "talas.quadOscillator",
        "prepared-realtime-checked-summed-quadrature-strict-nyquist");
    registry.addNodeTypeLimitation(
        "talas.quadOscillator",
        "prepared-realtime-zero-latency-input-ancestry");
    registry.setNodeTypeProcessingContract(
        "talas.gaOscillators",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.gaOscillators",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.gaOscillators",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.gaOscillators",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-immutable-analysis-table-snapshots"},
             std::string_view{"prepared-realtime-maximum-4096-additive-partials"},
             std::string_view{"prepared-realtime-per-frame-envelope-frequency-and-morph-controls"},
             std::string_view{"ga-analysis-tables-of-adr-0027-play-complex-waveforms-their-envelopes-and-the-shared-deviation"},
             std::string_view{"time-index-over-constant-computed-analysis-envelopes-for-harmonic-amplitude-tables"},
             std::string_view{"external-ga-aiff-files-are-not-read-their-header-is-undocumented"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.gaOscillators", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.sumOfSines",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sumOfSines",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.sumOfSines",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.sumOfSines",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeProcessingContract(
        "talas.genericSource",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.genericSource",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.genericSource",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.genericSource",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-live-input-disk-track-and-ram-sample-source-modes"},
             std::string_view{"live-input-channels-1-and-2-at-scale-without-the-sample-envelope"},
             std::string_view{"prepared-realtime-immutable-maximum-1048576-sample-snapshot"},
             std::string_view{"prepared-realtime-per-frame-trigger-envelope-frequency-and-scale-controls"},
             std::string_view{"prepared-realtime-rising-edge-replay-and-optional-autoloop"},
             std::string_view{"disk-stream-and-mutable-sample-sources-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.genericSource", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.sample",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.sample",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.sample",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.sample",
        ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-immutable-maximum-1048576-sample-snapshot"},
             std::string_view{"offline-ram-table-maximum-1048576-frames"},
             std::string_view{"prepared-realtime-static-normalized-playback-and-loop-ranges"},
             std::string_view{"prepared-realtime-per-frame-gate-trigger-frequency-envelope-and-scale-controls"},
             std::string_view{"prepared-realtime-mono-gate-release-trigger-replay-and-optional-loop-fade"},
             std::string_view{"offline-and-prepared-same-sound-named-memory-writer-reader-maximum-1048576-frames"},
             std::string_view{"prepared-realtime-file-stream-sources-unsupported"},
             std::string_view{"cross-sound-memory-writer-and-multichannel-sources-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sample", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multisample",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multisample",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.multisample",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.multisample",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-static-sample-list-maximum-256-positions-4096-bytes"},
             std::string_view{"prepared-realtime-deduplicated-immutable-maximum-256-tables-1048576-samples"},
             std::string_view{"prepared-realtime-per-frame-frequency-gate-index-scale-and-envelope-controls"},
             std::string_view{"prepared-realtime-index-at-gate-start-signed-playback-and-static-loop-window"},
             std::string_view{"disk-dynamic-list-default-pitch-and-multichannel-sources-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multisample", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.multifileDiskPlayer",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.multifileDiskPlayer",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.multifileDiskPlayer",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.multifileDiskPlayer",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-static-file-list-maximum-256-positions-4096-bytes"},
             std::string_view{"prepared-realtime-deduplicated-immutable-maximum-256-tables-1048576-samples"},
             std::string_view{"prepared-realtime-per-frame-index-trigger-and-signed-rate-controls"},
             std::string_view{"prepared-realtime-trigger-selected-interpolated-gated-or-release-through-playback"},
             std::string_view{"offline-and-prepared-shared-selection-cursor-reset-and-failure-atomicity"},
             std::string_view{"connected-index-trigger-and-rate-own-inert-direct-fallbacks"},
             std::string_view{"compiler-owned-exact-prepared-plan-and-immutable-table-authority"},
             std::string_view{"disk-stream-decoding-dynamic-list-sample-rate-metadata-and-multichannel-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.multifileDiskPlayer", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.sampleCloud",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.sampleCloud",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-deterministic-checked-stereo-grain-mixing"},
             std::string_view{"prepared-realtime-all-nine-per-frame-connected-controls"},
             std::string_view{"prepared-realtime-fixed-maximum-64-retained-starts-and-maxgrains-1-through-64"},
             std::string_view{"offline-scalable-maximum-grains-prepared-fixed-bound"},
             std::string_view{"compiler-owned-exact-ordered-plan-and-immutable-sample-envelope-authority"},
             std::string_view{"prepared-realtime-allocation-free-transport-reset-partition-stable-failure-atomic-retry"},
             std::string_view{"disk-stream-mutable-dynamic-resources-connected-seed-and-exact-reference-parity-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.sampleCloud", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cloudBank",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.cloudBank",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-cloud-bank-frame-transition"},
             std::string_view{"exact-absolute-frame-and-analytical-grain-arithmetic-or-reject"},
             std::string_view{"prepared-realtime-fixed-tracks-grains-1-through-64-maximum-4096-candidates"},
             std::string_view{"offline-scalable-maximum-4096-prepared-fixed-bound"},
             std::string_view{"compiler-owned-exact-plan-packed-spectrum-and-immutable-wave-envelope-authority"},
             std::string_view{"zero-frame-failure-atomic-transport-reset-partition-stable-retry"},
             std::string_view{"dynamic-mutable-file-stream-stochastic-multichannel-and-exact-reference-parity-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cloudBank", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cloudBankElement",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeResourceAccess(
        "talas.cloudBankElement",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-cloud-bank-element-frame-transition"},
             std::string_view{"selected-only-packed-spectrum-range-fractional-delay-and-single-cascade-addition"},
             std::string_view{"exact-absolute-frame-and-analytical-grain-arithmetic-or-reject"},
             std::string_view{"prepared-realtime-first-count-maxgrains-1-through-64-maximum-4096-candidates"},
             std::string_view{"offline-scalable-maximum-4096-prepared-fixed-bound"},
             std::string_view{"compiler-owned-exact-plan-spectrum-cascade-and-immutable-wave-envelope-authority"},
             std::string_view{"zero-frame-failure-atomic-transport-reset-partition-stable-retry"},
             std::string_view{"dynamic-mutable-file-stream-stochastic-multichannel-and-exact-reference-parity-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cloudBankElement", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.memoryWriter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.memoryWriter",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.memoryWriter",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.memoryWriter",
        ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-mono-monitor-and-bounded-named-recording"},
             std::string_view{"prepared-realtime-positive-duration-maximum-1048576-frames"},
             std::string_view{"offline-and-prepared-zero-duration-full-capture-for-an-enclosing-set-duration-or-1048576-frames"},
             std::string_view{"prepared-realtime-static-or-per-frame-trigger-cyclic-and-silent-modes"},
             std::string_view{"offline-and-prepared-same-sound-named-capture-and-cyclic-overwrite-maximum-1048576-frames"},
             std::string_view{"live-sample-sample-cloud-function-generator-and-waveshaper-readers-with-complete-rollback"},
             std::string_view{"global-storage-cross-sound-readers-event-scheduling-and-persistence-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.memoryWriter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.keyMappedMultisample",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.keyMappedMultisample",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.keyMappedMultisample",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.keyMappedMultisample",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-immutable-bank-of-up-to-128-tables-of-1048576-samples"},
             std::string_view{"one-per-half-step-equally-spaced-by-base-pitch-and-by-pitch-range-mappings-of-the-first-sample-folder"},
             std::string_view{"prepared-realtime-per-frame-frequency-range-gate-gain-and-envelope-controls"},
             std::string_view{"prepared-realtime-fixed-or-440-hz-relative-playback-and-whole-table-loop"},
             std::string_view{"disk-stream-and-multichannel-sources-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.keyMappedMultisample", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.oscillatorBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.syntheticSpectrumFromArray",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.harmonicSpectrum",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"static-finite-fundamental-and-uniform-amplitude"},
             std::string_view{"one-through-4096-directly-computed-harmonic-frequencies"},
             std::string_view{"zero-bandwidth-spectrum-no-reference-accuracy-profile"},
         })
        registry.addNodeTypeLimitation(
            "talas.harmonicSpectrum", std::string{limitation});
    registry.setNodeTypeProcessingContract(
        "talas.spectrumFromSingleCycle",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.spectrumFromSingleCycle",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"static-mono-single-cycle-table"},
             std::string_view{"arbitrary-length-2-through-65536-samples"},
             std::string_view{"maximum-4096-harmonic-partials"},
             std::string_view{"magnitude-only-zero-bandwidth-spectrum"},
             std::string_view{"no-live-resource-updates"},
         })
        registry.addNodeTypeLimitation(
            "talas.spectrumFromSingleCycle", std::string{limitation});
    for (const auto limitation : {
             std::string_view{"prepared-realtime-static-finite-array-common-prefix"},
             std::string_view{"prepared-realtime-immutable-stateless-brace-array-expressions"},
             std::string_view{"prepared-realtime-maximum-4096-requested-spectrum-partials"},
             std::string_view{"prepared-realtime-static-or-block-constant-frame-zero-envelope"},
             std::string_view{"event-variable-stateful-definition-named-pitch-and-live-array-expressions-unsupported"},
             std::string_view{"serialized-linear-pairs-update-one-partial-per-sample"},
             std::string_view{"serialized-output-requires-full-array-count-no-logscale-no-bandwidths"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.syntheticSpectrumFromArray", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.spectrumInRam",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    // A SpectralShape and the spectrum modifiers that rewrite each partial
    // as it passes keep no state and delay nothing, so an OscillatorBank or
    // FilterBank after them keeps a known timeline.
    for (const std::string_view typeId : {
             "talas.spectralShape",
             "talas.spectrumFrequencyScale",
             "talas.spectrumLinearToLog",
             "talas.spectrumLogToLinear",
         })
    {
        registry.setNodeTypeProcessingContract(
            typeId, NodeStateModel::Stateless, false,
            {LatencyKind::None, 0});
    }
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.spectrumInRam",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.spectrumInRam",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-immutable-fixed-width-analysis-frames"},
             std::string_view{"prepared-realtime-normalized-minus-one-to-one-time-index"},
             std::string_view{"prepared-realtime-project-authored-analysis-frequency-and-default-partial-tail"},
             std::string_view{"prepared-realtime-static-or-block-constant-frame-zero-controls"},
             std::string_view{"legacy-spectrum-out-remains-callback-held"},
             std::string_view{"serialized-amplitude-frequency-pairs-feed-oscillator-bank"},
             std::string_view{"analysis-file-decoding-automatic-metadata-scan-and-resynthesis-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.spectrumInRam", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.spectrumOnDisk",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.spectrumOnDisk",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.spectrumOnDisk",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-immutable-table-snapshot"},
             std::string_view{"prepared-realtime-maximum-4096-spectrum-partials"},
             std::string_view{"prepared-realtime-static-or-block-constant-frame-zero-controls"},
             std::string_view{"prepared-realtime-immutable-fixed-width-frame-scan"},
             std::string_view{"prepared-realtime-logical-public-frame-rate-and-rising-edge-trigger"},
             std::string_view{"prepared-realtime-transactional-scan-state"},
             std::string_view{"prepared-realtime-project-authored-analysis-frequency-and-default-partial-tail"},
             std::string_view{"real-disk-streaming-decoding-automatic-metadata-and-reverse-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.spectrumOnDisk", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.syntheticSpectrumFromSounds",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.syntheticSpectrumFromSounds",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.syntheticSpectrumFromSounds",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"legacy-spectrum-output-uses-callback-local-packed-audio-frame"},
             std::string_view{"serialized-linear-amplitude-frequency-pairs-across-callbacks"},
             std::string_view{"serialized-pairs-require-logscale-and-bandwidths-disabled"},
             std::string_view{"prepared-realtime-maximum-4096-spectrum-partials"},
             std::string_view{"prepared-realtime-static-log-scale-and-bandwidth-mode"},
             std::string_view{"prepared-realtime-block-constant-frame-zero-partial-count"},
             std::string_view{"prepared-realtime-aligned-audio-control-ancestry"},
             std::string_view{"legacy-spectrum-packed-window-before-scheduler-split"},
             std::string_view{"prepared-realtime-no-feedback-pair-coexistence"},
             std::string_view{"prepared-realtime-early-packed-window-failure-consumes-published-event-queues-and-score"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.syntheticSpectrumFromSounds",
            std::string{limitation});
    }
    registry.setNodeTypeOfflineSupport(
        "talas.frameSynchronizer",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.frameSynchronizer",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract(
        "talas.frameSynchronizer",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::VariableOrUnknown, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.frameSynchronizer", FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.frameSynchronizer", ResourceAccessMode::None);
    registry.setNodeTypeOutputBusLayouts(
        "talas.frameSynchronizer", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"paired-planar-amplitude-frequency-sample-stream-and-explicit-component-index"},
             std::string_view{"bounded-timestamped-partial-spectrum-frame-output-across-callbacks"},
             std::string_view{"static-frame-length-1-through-4096-and-optional-connected-sync-controls"},
             std::string_view{"local-positive-edge-alignment-and-startup-silence-inference"},
             std::string_view{"reference-extra-flags-exact-delay-stereo-port-and-installed-reference-unverified"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.frameSynchronizer", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.spectrumModifier",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.spectrumModifier",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.spectrumModifier",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"prepared-realtime-maximum-4096-spectrum-partials"},
             std::string_view{"prepared-realtime-static-or-block-constant-selection-and-transform-controls"},
             std::string_view{"static-frequency-offset-accepts-zero-to-one-scalar-or-hertz-expression-with-scalar-connected-control"},
             std::string_view{"prepared-realtime-deterministic-live-bounded-seed"},
             std::string_view{"prepared-realtime-deterministic-public-frame-probability-stream"},
             std::string_view{"prepared-realtime-static-one-based-source-ordinal-probability-profile"},
             std::string_view{"static-track-number-dependent-frequency-and-amplitude-scale-offset-profiles"},
             std::string_view{"prepared-realtime-variables-v2-remainder-track-profiles"},
             std::string_view{"prepared-realtime-bounded-4096-track-live-hysteresis-state"},
             std::string_view{"independent-zero-to-below-one-frequency-and-amplitude-frame-smoothing-by-source-track-ordinal-with-connected-block-constant-controls"},
             std::string_view{"prepared-realtime-zero-latency-spectrum-control-ancestry"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.spectrumModifier", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.spectrumVoicedUnvoiced",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeEventTransportContract(
        "talas.spectrumVoicedUnvoiced", false, true);
    registry.setNodeTypeOutputBusLayouts(
        "talas.spectrumVoicedUnvoiced",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.spectrumVoicedUnvoiced",
        FanInLatencyPolicy::PreserveRelative);
    registry.setNodeTypeResourceAccess(
        "talas.spectrumVoicedUnvoiced",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{
                 "prepared-realtime-preserved-bounded-harmonicity-classifier"},
             std::string_view{
                 "prepared-realtime-transactional-one-public-frame-voiced-unvoiced-transition"},
             std::string_view{
                 "native-analysis-voicing-metadata-and-longer-smoothing-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.spectrumVoicedUnvoiced", std::string{limitation});
    }
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.oscillatorBank",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.oscillatorBank",
        ResourceAccessMode::ImmutableRead);
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "prepared-realtime-checked-maximum-4096-static-oscillators");
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "prepared-realtime-blank-sine-or-immutable-table-wavetable");
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "prepared-realtime-zero-latency-spectrum-or-paired-audio-ancestry");
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "serialized-direct-source-period-independent-of-selected-oscillator-count");
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "serialized-unmatched-audio-pairs-use-oscillator-count-period");
    registry.addNodeTypeLimitation(
        "talas.oscillatorBank",
        "reference-oscillator-update-timing-unverified");
    registry.setNodeTypeProcessingContract(
        "talas.formantBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.formantBank",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.formantBank",
        ResourceAccessMode::ImmutableRead);
    registry.addNodeTypeLimitation(
        "talas.formantBank",
        "prepared-realtime-checked-maximum-4096-static-formants");
    registry.addNodeTypeLimitation(
        "talas.formantBank",
        "prepared-realtime-immutable-mono-impulse-response-triangular-fallback-and-bounded-overlap");
    registry.addNodeTypeLimitation(
        "talas.formantBank",
        "prepared-realtime-zero-latency-spectrum-frequency-ancestry");
    registry.addNodeTypeLimitation(
        "talas.formantBank",
        "serialized-direct-linear-synthetic-or-inram-source-count-independent-of-selected-formants");
    registry.setNodeTypeProcessingContract(
        "talas.formantBankOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.formantBankOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.formantBankOscillator",
        ResourceAccessMode::ImmutableRead);
    registry.addNodeTypeLimitation(
        "talas.formantBankOscillator",
        "prepared-realtime-checked-selected-maximum-4096-static-formants");
    registry.addNodeTypeLimitation(
        "talas.formantBankOscillator",
        "prepared-realtime-required-mono-cascade-immutable-impulse-and-bounded-overlap");
    registry.addNodeTypeLimitation(
        "talas.formantBankOscillator",
        "prepared-realtime-static-or-connected-positive-pulse-frequency-independent-formant-centers-and-optional-dc");
    registry.addNodeTypeLimitation(
        "talas.formantBankOscillator",
        "prepared-realtime-zero-latency-spectrum-cascade-frequency-ancestry");
    registry.addNodeTypeLimitation(
        "talas.formantBankOscillator",
        "serialized-audio-pair-zero-bandwidth-extension-does-not-fulfill-reference-bandwidth-requirement");
    registry.setNodeTypeProcessingContract(
        "talas.probabilityChaosGenerator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.probabilityChaosGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.probabilityChaosGenerator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.probabilityChaosGenerator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-deterministic-clean-room-probability-chaos-generator"},
             std::string_view{"immutable-integral-seed-one-through-4294967295-and-static-direct-event-value-or-per-frame-connected-frequency-probability-chaos-slew-and-scale"},
             std::string_view{"frequency-zero-through-zero-point-four-five-sample-rate-probability-and-chaos-zero-through-one-slew-zero-through-ten-seconds-scale-zero-through-eight"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-stereo-output-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-rng-logistic-phase-retry-and-transport-reset"},
             std::string_view{"alternate-prngs-distributions-chaos-maps-mutable-seed-gate-width-bipolar-scale-polyphony-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.probabilityChaosGenerator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.quadFunctionGenerator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.quadFunctionGenerator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.quadFunctionGenerator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.quadFunctionGenerator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-four-phase-function-generator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-frequency-phase-offset-skew-curve-smooth-and-scale"},
             std::string_view{"frequency-zero-through-zero-point-four-five-sample-rate-phase-offset-and-smooth-unit-skew-zero-point-zero-five-through-zero-point-nine-five-curve-zero-point-two-five-through-four-scale-zero-through-eight"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-four-output-buffer-producer-scope-zero-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-phase-retry-and-transport-reset"},
             std::string_view{"alternate-functions-independent-output-phases-bipolar-unbounded-stereo-polyphony-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.quadFunctionGenerator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.stereoDispersiveResonator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.stereoDispersiveResonator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.stereoDispersiveResonator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.stereoDispersiveResonator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-stereo-fractional-delay-dispersive-resonator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-frequency-decay-time-detune-damping-dispersion-width-mix-and-input-gain"},
             std::string_view{"sample-rate-eight-thousand-through-384000-frequency-and-derived-branches-twenty-through-zero-point-four-five-sample-rate-decay-zero-point-zero-one-through-twenty-detune-zero-through-one-hundred-unit-controls-and-input-gain-zero-through-eight"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-mono-input-stereo-output-buffer-producer-scope-zero-latency-private-storage-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-two-branch-delay-damping-allpass-retry-and-transport-reset"},
             std::string_view{"alternate-interpolation-damping-dispersion-nonlinearity-modal-waveguide-multichannel-polyphony-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.stereoDispersiveResonator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.pluckedWaveguide",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.pluckedWaveguide",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.pluckedWaveguide",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.pluckedWaveguide",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-monophonic-plucked-waveguide"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-trigger-frequency-decay-time-brightness-pick-position-stiffness-and-level-with-static-seed"},
             std::string_view{"sample-rate-eight-thousand-through-384000-frequency-twenty-through-zero-point-four-sample-rate-decay-zero-point-zero-two-through-twenty-unit-brightness-and-stiffness-pick-position-zero-point-zero-two-through-zero-point-nine-eight-level-zero-through-four-and-integral-seed-one-through-4294967295"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-storage-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-schedule-trigger-excitation-delay-damping-allpass-retry-and-transport-reset"},
             std::string_view{"polyphony-midi-note-allocation-audio-excitation-body-coupling-bow-strike-named-instrument-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.pluckedWaveguide", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.dynamicBreakpointOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.dynamicBreakpointOscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.dynamicBreakpointOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.dynamicBreakpointOscillator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-monophonic-dynamic-breakpoint-oscillator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-reset-frequency-volatility-correlation-timing-jitter-curve-and-level-with-static-point-count-and-seed"},
             std::string_view{"sample-rate-eight-thousand-through-384000-frequency-zero-point-one-through-sample-rate-over-four-point-count-unit-volatility-bipolar-correlation-and-curve-timing-jitter-zero-through-zero-point-five-level-zero-through-four-integral-point-count-four-through-sixty-four-and-integral-seed-one-through-4294967295"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-state-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-segment-generator-target-velocity-duration-reset-edge-retry-and-transport-reset"},
             std::string_view{"live-point-count-breakpoint-table-multichannel-unbounded-jitter-alternate-correlation-reflection-interpolation-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.dynamicBreakpointOscillator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.waveTerrainOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.waveTerrainOscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.waveTerrainOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.waveTerrainOscillator",
        ResourceAccessMode::ImmutableRead);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-monophonic-wave-terrain-oscillator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-reset-frequency-x-radius-y-radius-y-ratio-rotation-phase-offset-and-level-with-static-width-height-and-immutable-terrain"},
             std::string_view{"sample-rate-eight-thousand-through-384000-frequency-zero-through-zero-point-four-sample-rate-unit-radii-y-ratio-zero-point-two-five-through-eight-bipolar-rotation-and-phase-offset-level-zero-through-four-integral-dimensions-two-through-sixty-four-and-finite-terrain-minus-one-through-one"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-table-resource-snapshot-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-state-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-trajectory-coordinate-bilinear-phase-reset-edge-resource-generation-retry-and-transport-reset"},
             std::string_view{"live-dimensions-terrain-replacement-arbitrary-trajectory-higher-order-interpolation-antialiasing-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.waveTerrainOscillator", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.scannedMassOscillator",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.scannedMassOscillator",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.scannedMassOscillator",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.scannedMassOscillator",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-clean-room-monophonic-scanned-mass-oscillator"},
             std::string_view{"static-direct-event-value-or-per-frame-connected-reset-excite-frequency-model-rate-coupling-damping-restoring-excitation-position-excitation-amount-scan-offset-and-level-with-static-mass-count-and-initial-shape"},
             std::string_view{"sample-rate-eight-thousand-through-384000-frequency-zero-through-zero-point-four-sample-rate-model-rate-zero-through-one-thousand-coupling-zero-through-zero-point-four-five-damping-zero-through-zero-point-two-five-restoring-zero-through-zero-point-two-and-four-times-coupling-plus-restoring-at-most-two-unit-excitation-position-bipolar-excitation-amount-and-scan-offset-level-zero-through-four-integral-mass-count-eight-through-sixty-four-and-unit-initial-shape"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-mono-output-buffer-producer-scope-zero-latency-private-state-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-model-tick-force-velocity-position-excitation-reset-edge-scan-interpolation-retry-and-transport-reset"},
             std::string_view{"live-topology-arbitrary-adjacency-external-force-multiple-scan-heads-higher-order-interpolation-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.scannedMassOscillator", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.sampleMapPlayer",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeEventTransportContract(
        "talas.clock",
        false,
        true);
    registry.setNodeTypeEventTransportContract(
        "talas.adsr",
        true,
        false);
    registry.setNodeTypeOfflineSupport(
        "talas.adsr",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.adsr",
        ExecutionSupport::Supported,
        ExecutionBinding::Kernel);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.adsr",
        FanInLatencyPolicy::Align);
    registry.setNodeTypeResourceAccess(
        "talas.adsr",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-shared-checked-finite-linear-or-exponential-adsr-kernel"},
             std::string_view{"static-scalar-event-value-positive-times-unit-sustain-finite-scale-gate-and-legato-with-connected-gate-precedence"},
             std::string_view{"sample-accurate-direct-event-value-and-ordered-targeted-typed-gate-scheduling"},
             std::string_view{"compiler-owned-exact-descriptor-kernel-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-event-state-retry-and-transport-reset"},
             std::string_view{"arbitrary-curve-polyphony-connected-envelope-controls-project-authored-targeted-events-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.adsr",
            std::string{limitation});
    }

    for (const auto typeId : {
             std::string_view{"talas.diskPlayer"},
             std::string_view{"talas.diskCache"},
         })
    {
        registry.setNodeTypeResourceAccess(
            typeId,
            ResourceAccessMode::StreamRead);
    }
    registry.setNodeTypeProcessingContract(
        "talas.diskCache",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeProcessingContract(
        "talas.threshold",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.threshold",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.threshold", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.threshold", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-matched-checked-finite-absolute-amplitude-schmitt-latch"},
             std::string_view{"static-finite-threshold-and-non-negative-hysteresis-or-per-frame-connected-controls-with-connected-precedence"},
             std::string_view{"exact-upper-inclusive-lower-strict-state-persistence-and-transport-reset"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-input-output-producer-scope-latency-and-local-transaction-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-state-output-retry-and-reset"},
             std::string_view{"dynamic-language-direct-scheduled-fallback-edge-event-smoothing-input-analysis-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.threshold", std::string{limitation});
    }
    // A Sound's own timing (Time & Duration: start times, offsets, waits,
    // stops and time rates) is what the Sound means, not processing delay:
    // it declares no latency, so nothing downstream is aligned to it and
    // processors after it keep their known timeline (ADR-0011 amendment).
    registry.setNodeTypeProcessingContract(
        "talas.timeControl",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.timeControl",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.timeControl",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeControl", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeControl", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.timeControl", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-mono-static-or-per-frame-connected-non-negative-rate-with-persistent-fractional-cursor"},
             std::string_view{"fixed-capacity-prepared-push-history-with-whole-block-append-before-linear-interpolation"},
             std::string_view{"shared-checked-finite-offline-and-prepared-append-interpolation-cursor-and-discard-transition"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-and-fixed-input-latency-authority"},
             std::string_view{"prepared-realtime-allocation-free-overwrite-journal-failure-atomic-retry-and-transport-reset"},
             std::string_view{"pull-based-future-sample-ownership-reverse-rate-multichannel-and-exact-reference-scheduling-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeControl", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeStopper",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.timeStopper",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.timeStopper",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeStopper", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeStopper", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.timeStopper", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-mono-static-scalar-event-or-per-frame-connected-resume-with-resume-on-zero-sample-hold"},
             std::string_view{"shared-checked-empty-capture-open-advance-and-closed-hold-transition"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-and-fixed-input-latency-authority"},
             std::string_view{"prepared-realtime-allocation-free-failure-atomic-retry-and-transport-reset"},
             std::string_view{"connected-control-precedence-over-direct-event-value-scalar-fallback"},
             std::string_view{"processor-time-freeze-resume-as-sound-lifecycle-queued-source-events-unbounded-waiting-multichannel-and-exact-reference-scheduling-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeStopper", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.setDuration",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.setDuration",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.setDuration",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.setDuration", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.setDuration", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.setDuration", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-mono-static-scalar-event-or-per-frame-connected-non-negative-start-time-and-positive-duration-half-open-window"},
             std::string_view{"fixed-capacity-static-or-live-delay-history-with-shared-checked-frame-conversion-addressing-window-and-state-transition"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-history-and-fixed-input-latency-authority"},
             std::string_view{"prepared-realtime-allocation-free-failure-atomic-retry-and-transport-reset"},
             std::string_view{"connected-control-precedence-over-direct-event-value-scalar-fallback"},
             std::string_view{"full-sound-event-lifecycle-automatic-source-extension-termination-multichannel-and-exact-reference-scheduling-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.setDuration", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.timeOffset",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.timeOffset",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.timeOffset", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.timeOffset", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.timeOffset", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"offline-and-prepared-finite-mono-forward-static-and-connected-delay-static-reverse-two-window-and-static-retrograde-input-first"},
             std::string_view{"exact-power-of-two-exclusive-frame-bound-and-checked-reverse-capacity-doubling"},
             std::string_view{"fixed-preparation-time-zero-filled-history-with-sample-progressive-partition-stable-read-before-write-transition"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-and-buffer-authority"},
             std::string_view{"prepared-realtime-allocation-free-failure-atomic-retry-and-transport-reset"},
             std::string_view{"connected-dynamic-reverse-retrograde-automatic-timeline-extension-pull-ahead-end-of-stream-invention-multichannel-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.timeOffset", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.waitUntil",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.waitUntil",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.waitUntil",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.waitUntil", FanInLatencyPolicy::Align);
    registry.setNodeTypeOutputBusLayouts(
        "talas.waitUntil", {AudioBusLayoutKind::Mono});
    registry.setNodeTypeResourceAccess(
        "talas.waitUntil", ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-mono-static-or-connected-scalar-resume-with-resume-on-zero-and-latched-open-semantics"},
             std::string_view{"shared-dependency-delayed-input-activation-with-live-resume-and-once-per-sample-producers"},
             std::string_view{"shared-offline-and-prepared-activation-with-failure-atomic-retry-and-reset"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-and-input-latency-authority"},
             std::string_view{"static-resume-requires-an-immutable-finite-expression-or-an-explicit-connected-control-producer"},
             std::string_view{"resume-as-sound-end-detection-complete-midi-and-expression-input-lifecycle-global-timeline-gates-multichannel-and-exact-reference-scheduling-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.waitUntil", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.triggeredSampleAndHold",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.triggeredSampleAndHold",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.triggeredSampleAndHold",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.triggeredSampleAndHold",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.triggeredSampleAndHold",
        ResourceAccessMode::None);
    for (const auto limitation : {
             std::string_view{"finite-control-session-triggered-sample-and-hold-on-inactive-to-positive-edges"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-trigger-with-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-same-frame-finite-capture"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"multichannel-sound-object-event-graph-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.triggeredSampleAndHold", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.triggeredTableRead",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOfflineSupport(
        "talas.triggeredTableRead",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.triggeredTableRead",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.triggeredTableRead",
        FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess(
        "talas.triggeredTableRead",
        ResourceAccessMode::VariableOrUnknown);
    for (const auto limitation : {
             std::string_view{"finite-immutable-or-live-memory-table-session-cursor-with-positive-trigger-read-and-nonpositive-hold"},
             std::string_view{"same-scope-unique-prior-bounded-mono-memory-writer-with-current-sample-visibility"},
             std::string_view{"static-scalar-event-or-per-frame-connected-finite-trigger-and-gate-with-independent-connected-precedence"},
             std::string_view{"sample-accurate-direct-event-value-scheduling-with-gate-edge-reset-before-same-frame-read"},
             std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority"},
             std::string_view{"allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset"},
             std::string_view{"cross-scope-memory-multichannel-event-graph-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.triggeredTableRead", std::string{limitation});
    }
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.diskRecorder",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter,
        "prepared-realtime-explicit-owner-mono-or-stereo-8-16-or-24-bit-pcm-wav-only");
    registry.setNodeTypeProcessingContract(
        "talas.diskRecorder",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.diskRecorder",
        ResourceAccessMode::StreamWrite);
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.diskRecorderTap",
        ExecutionSupport::Subset,
        ExecutionBinding::LegacyAdapter,
        "prepared-realtime-top-level-only-mono-or-stereo-8-16-or-24-bit-pcm-wav-or-aiff");
    // Passing its Input on keeps no state; the session owns what it records.
    registry.setNodeTypeProcessingContract(
        "talas.diskRecorderTap",
        NodeStateModel::Stateless,
        false,
        {LatencyKind::None, 0});
    registry.setNodeTypeResourceAccess(
        "talas.diskRecorderTap",
        ResourceAccessMode::StreamWrite);
    registry.setNodeTypeReplacement("talas.diskRecorderTap", "talas.diskRecorder");
    for (const auto limitation : {
             std::string_view{"explicit-owner-action-only"},
             std::string_view{"mono-or-stereo-8-16-or-24-bit-pcm-wav-only"},
             std::string_view{"resource-name-is-not-path-authority"},
             std::string_view{"no-automatic-trigger-started-filesystem-write"},
             std::string_view{"aiff-32-bit-and-compressed-unsupported"},
             std::string_view{"cloud-publication-unsupported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.diskRecorder", std::string{limitation});
    }
    registry.setNodeTypeResourceAccess(
        "talas.timeFrequencyScale",
        ResourceAccessMode::VariableOrUnknown);
    registry.setNodeTypeResourceAccess(
        "talas.partialTrackPlayerV1",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeResourceAccess(
        "talas.diffuseSpectralStretch",
        ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts(
        "talas.audioOutput",
        {
            AudioBusLayoutKind::Mono,
            AudioBusLayoutKind::Stereo,
        });
    registry.setNodeTypeOutputBusLayouts(
        "talas.oscilloscopeDisplay",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOutputBusLayouts(
        "talas.lowPassGate",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.reverbSection",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.morphFilter",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.dualMorphOscillator",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOutputBusLayouts(
        "talas.quadOscillator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.oscillatorBank",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.formantBank",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.formantBankOscillator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.probabilityChaosGenerator",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOutputBusLayouts(
        "talas.quadFunctionGenerator",
        {AudioBusLayoutKind::Discrete4});
    registry.setNodeTypeOutputBusLayouts(
        "talas.stereoDispersiveResonator",
        {AudioBusLayoutKind::Stereo});
    registry.setNodeTypeOutputBusLayouts(
        "talas.pluckedWaveguide",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.dynamicBreakpointOscillator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.waveTerrainOscillator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.scannedMassOscillator",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.diffuseSpectralStretch",
        {AudioBusLayoutKind::Mono});
    registry.setNodeTypeOutputBusLayouts(
        "talas.pan",
        {
            AudioBusLayoutKind::Stereo,
            AudioBusLayoutKind::Discrete4,
            AudioBusLayoutKind::Discrete8,
        });
    registry.setNodeTypeOutputBusLayouts(
        "talas.horizontalAmbisonicDecoder",
        {
            AudioBusLayoutKind::Discrete4,
            AudioBusLayoutKind::Discrete8,
        });
    for (const auto typeId : {
             std::string_view{"talas.matrix4"},
             std::string_view{"talas.output4"},
             std::string_view{"talas.stereoInOutput4"},
         })
    {
        registry.setNodeTypeOutputBusLayouts(
            typeId,
            {AudioBusLayoutKind::Discrete4});
    }
    for (const auto typeId : {
             std::string_view{"talas.matrix8"},
             std::string_view{"talas.output8"},
             std::string_view{"talas.stereoInOutput8"},
         })
    {
        registry.setNodeTypeOutputBusLayouts(
            typeId,
            {AudioBusLayoutKind::Discrete8});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.output6", {AudioBusLayoutKind::Discrete6});
    for (const auto typeId : {
             std::string_view{"talas.stereoInOutput4"},
             std::string_view{"talas.stereoInOutput8"},
         })
    {
        registry.setNodeTypeFanInLatencyPolicy(
            typeId,
            FanInLatencyPolicy::PreserveRelative);
        registry.addNodeTypeLimitation(
            typeId,
            "prepared-realtime-paired-mono-terminal-routing");
        registry.addNodeTypeLimitation(
            typeId,
            "prepared-realtime-root-terminal-only");
        registry.addNodeTypeLimitation(
            typeId,
            "prepared-realtime-zero-latency-ancestry");
        registry.addNodeTypeLimitation(
            typeId,
            "true-stereo-sound-object-unpacking-unsupported");
    }

    for (const auto typeId : {
             std::string_view{"talas.cdpLowerResolution"},
             std::string_view{"talas.cdpRingModulate"},
             std::string_view{"talas.cdpTremolo"},
             std::string_view{"talas.cdpPan"},
             std::string_view{"talas.cdpTapDelay"},
             std::string_view{"talas.cdpEcho"},
             std::string_view{"talas.cdpVaryingDelay"},
             std::string_view{"talas.cdpStadiumEcho"},
             std::string_view{"talas.cdpReverb"},
             std::string_view{"talas.cdpVariableFilter"},
             std::string_view{"talas.cdpFixedFilter"},
             std::string_view{"talas.cdpPhasing"},
             std::string_view{"talas.cdpLohiFilter"},
             std::string_view{"talas.cdpSweepingFilter"},
             std::string_view{"talas.cdpUserBank"},
             std::string_view{"talas.cdpDistortOverload"},
             std::string_view{"talas.cdpDistortReform"},
             std::string_view{"talas.cdpDistortOmit"},
             std::string_view{"talas.cdpDistortReverse"},
             std::string_view{"talas.cdpDistortEnvelope"},
             std::string_view{"talas.cdpDistortHarmonic"},
             std::string_view{"talas.cdpDistortFractal"},
             std::string_view{"talas.cdpDistortMultiply"},
             std::string_view{"talas.cdpDistortDivide"},
             std::string_view{"talas.cdpDistortShuffle"},
             std::string_view{"talas.cdpPhaseVocoder"},
             std::string_view{"talas.cdpSpectralGain"},
             std::string_view{"talas.cdpSpectralGate"},
             std::string_view{"talas.cdpFocusExaggerate"},
             std::string_view{"talas.cdpFocusFold"},
             std::string_view{"talas.cdpFocusAccumulate"},
             std::string_view{"talas.cdpBlurAverage"},
             std::string_view{"talas.cdpBlurSuppress"},
             std::string_view{"talas.cdpHiliteTrace"},
             std::string_view{"talas.cdpHiliteFilter"},
             std::string_view{"talas.cdpHilitePluck"},
             std::string_view{"talas.cdpFocusStep"},
             std::string_view{"talas.cdpStrangeShift"},
             std::string_view{"talas.cdpRepitchTranspose"},
             std::string_view{"talas.cdpStrangeWaver"},
             std::string_view{"talas.cdpPitchTransp"},
             std::string_view{"talas.cdpBlurChorus"},
             std::string_view{"talas.cdpBlurNoise"},
             std::string_view{"talas.cdpStretchSpectrum"},
             std::string_view{"talas.cdpBlurScatter"},
             std::string_view{"talas.cdpHiliteArpeg"},
             std::string_view{"talas.cdpHiliteGraphicEq"},
             std::string_view{"talas.cdpHiliteBand"},
             std::string_view{"talas.cdpBlurSpread"},
             std::string_view{"talas.cdpStrangeGlis"},
             std::string_view{"talas.cdpRepitchTransposef"},
             std::string_view{"talas.cdpHiliteVowels"},
             std::string_view{"talas.cdpBlurBlur"},
             std::string_view{"talas.cdpHiliteBlurTrace"},
             std::string_view{"talas.cdpFocusFocus"},
             std::string_view{"talas.cdpPitchTune"},
             std::string_view{"talas.cdpPitchPick"},
             std::string_view{"talas.cdpPitchChord"},
             std::string_view{"talas.cdpPitchChordf"},
             std::string_view{"talas.cdpFocusFreeze"},
             std::string_view{"talas.cdpVaryingBank"},
             std::string_view{"talas.cdpVaryingBank2"},
             std::string_view{"talas.cdpTremoloFine"},
             std::string_view{"talas.cdpDistmoreDouble"},
             std::string_view{"talas.cdpClipWaveset"},
             std::string_view{"talas.cdpQuirkWaveset"},
             std::string_view{"talas.cdpRoomReverb"},
             std::string_view{"talas.cdpQuantise"},
             std::string_view{"talas.cdpLoudnessGain"},
             std::string_view{"talas.cdpPhaseInvert"},
             std::string_view{"talas.cdpDcShift"},
             std::string_view{"talas.cdpSilenceGate"},
             std::string_view{"talas.cdpDeglitch"},
             std::string_view{"talas.cdpNoiseCut"},
             std::string_view{"talas.cdpAttackBoost"},
             std::string_view{"talas.cdpEnvelopeWarp"},
             std::string_view{"talas.cdpEnvelopeTimestretch"},
             std::string_view{"talas.cdpEnvelopeTarget"},
             std::string_view{"talas.cdpPitchedDelay"},
             std::string_view{"talas.cdpBounce"},
             std::string_view{"talas.cdpCascade"},
             std::string_view{"talas.cdpPhasor"},
             std::string_view{"talas.cdpBrassage"},
         })
    {
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        registry.setNodeTypeResourceAccess(typeId, ResourceAccessMode::None);
        registry.setNodeTypeOutputBusLayouts(typeId, {AudioBusLayoutKind::Mono});
        registry.addNodeTypeLimitation(
            typeId, "mono-and-cdp-float-sample-buffers");
    }
    for (const auto typeId : {
             std::string_view{"talas.cdpCombineSum"},
             std::string_view{"talas.cdpCombineDiff"},
             std::string_view{"talas.cdpCombineMax"},
             std::string_view{"talas.cdpCombineMean"},
             std::string_view{"talas.cdpCombineCross"},
             std::string_view{"talas.cdpFormantsVocode"},
             std::string_view{"talas.cdpMorphMorph"},
             std::string_view{"talas.cdpMorphBridge"},
             std::string_view{"talas.cdpFormantsPut"},
             std::string_view{"talas.cdpCrossModulate"},
             std::string_view{"talas.cdpSubtract"},
             std::string_view{"talas.cdpMixBalance"},
             std::string_view{"talas.cdpMixCrossfade"},
             std::string_view{"talas.cdpMixMerge"},
             std::string_view{"talas.cdpMixFaders"},
             std::string_view{"talas.cdpMixInterleave"},
             std::string_view{"talas.cdpMixInbetween"},
             std::string_view{"talas.cdpEnvelopeImpose"},
             std::string_view{"talas.cdpRejoin"},
             std::string_view{"talas.cdpSpin"},
         })
    {
        // Two streams, CDP8's first file and its second, aligned as they
        // arrive.
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::Align);
        registry.setNodeTypeResourceAccess(typeId, ResourceAccessMode::None);
        registry.setNodeTypeOutputBusLayouts(typeId, {AudioBusLayoutKind::Mono});
        registry.addNodeTypeLimitation(
            typeId, "mono-and-cdp-float-sample-buffers");
    }
    for (const auto typeId : {
             std::string_view{"talas.cdpPitchAltharms"},
             std::string_view{"talas.cdpPitchOctmove"},
             std::string_view{"talas.cdpSpecBare"},
             std::string_view{"talas.cdpSpecClean"},
             std::string_view{"talas.cdpStrangeInvert"},
         })
    {
        // The pitch source (spec clean's noise and good signal, strange
        // invert's normalising source) is a sample read whole when it
        // starts.
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeFanInLatencyPolicy(
            typeId, FanInLatencyPolicy::NotApplicable);
        registry.setNodeTypeResourceAccess(typeId, ResourceAccessMode::ImmutableRead);
        registry.setNodeTypeOutputBusLayouts(typeId, {AudioBusLayoutKind::Mono});
        registry.addNodeTypeLimitation(
            typeId, "mono-and-cdp-float-sample-buffers");
    }
    // Fast convolution's impulse response is a sample read whole when it
    // starts.
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.cdpFastConvolution",
        ExecutionSupport::Supported,
        ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeFanInLatencyPolicy(
        "talas.cdpFastConvolution", FanInLatencyPolicy::NotApplicable);
    registry.setNodeTypeResourceAccess("talas.cdpFastConvolution", ResourceAccessMode::ImmutableRead);
    registry.setNodeTypeOutputBusLayouts("talas.cdpFastConvolution", {AudioBusLayoutKind::Mono});
    registry.addNodeTypeLimitation(
        "talas.cdpFastConvolution", "mono-and-cdp-float-sample-buffers");
    // A group's mean is held from the frame that completes the group.
    registry.setNodeTypeProcessingContract(
        "talas.cdpLowerResolution",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            "sampleRateDivision",
            PreparedLatencyRule::ParameterFramesMinusOne,
        });
    for (const auto limitation : {
             std::string_view{"cdp8-modify-radical-4-float-file-quantization-bit-exact"},
             std::string_view{"rounded-bit-resolution-1-to-16-following-changes"},
             std::string_view{"sixteen-bits-quantizes-in-sixteen-level-steps-like-twelve-as-in-cdp8"},
             std::string_view{"static-whole-sample-rate-division-1-to-256-with-division-minus-one-frames-latency"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpLowerResolution", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpRingModulate",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-radical-5-table-sine-bit-exact-at-a-steady-frequency"},
             std::string_view{"frequency-read-every-128-frames-and-ramped-one-block-later-than-cdp8"},
             std::string_view{"static-live-or-connected-frequency-from-a-tenth-hertz-to-nyquist"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRingModulate", std::string{limitation});
    }
    for (const auto typeId : {
             std::string_view{"talas.cdpVaryingBank2"},
             std::string_view{"talas.cdpTremoloFine"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-filter-varibank2-bit-exact"},
             std::string_view{"static-data-gain-and-double-filtering-read-when-it-starts"},
             std::string_view{"live-q-read-at-each-128-frame-retuning-as-cdp8-reads-its-breakpoints"},
             std::string_view{"cdp8-retunes-from-frame-128-and-again-from-each-262144-frame-buffer"},
             std::string_view{"cdp8s-first-partial-compounds-with-the-pitch"},
             std::string_view{"cdp8s-normalising-n-reads-the-whole-file-and-is-not-a-field"},
             std::string_view{"no-trailer-time-the-tail-plays-as-the-stream-goes-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpVaryingBank2", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-tremolo-tremolo-bit-exact"},
             std::string_view{"static-mode-and-fineness-read-when-it-starts"},
             std::string_view{"live-frequency-depth-and-gain-held-per-block-where-cdp8-reads-every-frame"},
             std::string_view{"a-live-logarithmic-frequency-is-not-raised-to-one-over-a-files-duration"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpTremoloFine", std::string{limitation});
    }
    // talas::cdp::DistmoreDouble, ClipWaveset and QuirkWaveset's waitFrames:
    // the stream waits for each waveset or half-waveset.
    for (const auto typeId : {
             std::string_view{"talas.cdpDistmoreDouble"},
             std::string_view{"talas.cdpClipWaveset"},
             std::string_view{"talas.cdpQuirkWaveset"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::Fixed, 2047});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-distmore-double-bit-exact"},
             std::string_view{"multiplier-1-only-cdp8s-2-to-4-change-each-wavesets-length"},
             std::string_view{"latency-2047-frames-longer-wavesets-are-cut"},
             std::string_view{"cdp8-drops-its-last-partial-waveset-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistmoreDouble", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-clip-clip-2-bit-exact"},
             std::string_view{"static-fraction-read-when-it-starts"},
             std::string_view{"latency-2047-frames-longer-half-wavesets-are-cut-each-part-clipped-at-its-own-peak"},
             std::string_view{"cdp8-leaves-its-last-half-waveset-unclipped-the-stream-clips-it"},
             std::string_view{"cdp8s-mode-1-clip-at-a-level-is-not-a-field"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpClipWaveset", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-quirk-quirk-1-bit-exact"},
             std::string_view{"static-exponent-read-when-it-starts"},
             std::string_view{"latency-2047-frames-longer-half-wavesets-are-cut-each-part-scaled-to-its-own-peak"},
             std::string_view{"cdp8-drops-its-last-partial-half-waveset-the-stream-plays-on"},
             std::string_view{"cdp8s-mode-2-reads-the-whole-files-peak-and-is-not-a-field"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpQuirkWaveset", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpRoomReverb",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpRoomReverb", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-rmverb-bit-exact-for-a-mono-input-and-two-outputs-or-one"},
             std::string_view{"static-fields-and-reflections-read-when-it-starts"},
             std::string_view{"no-trailer-time-the-tail-plays-as-the-stream-goes-on"},
             std::string_view{"cdp8s-stereo-input-and-more-than-two-outputs-have-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRoomReverb", std::string{limitation});
    }
    // The sample-by-sample ports, which wait for nothing.
    for (const auto typeId : {
             std::string_view{"talas.cdpQuantise"},
             std::string_view{"talas.cdpLoudnessGain"},
             std::string_view{"talas.cdpPhaseInvert"},
             std::string_view{"talas.cdpDcShift"},
             std::string_view{"talas.cdpCrossModulate"},
             std::string_view{"talas.cdpSubtract"},
             std::string_view{"talas.cdpMixBalance"},
             std::string_view{"talas.cdpMixCrossfade"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-modify-radical-7-bit-exact"},
             std::string_view{"live-bit-resolution-1-to-16-rounded-held-per-block-where-cdp8-fixes-it"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpQuantise", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-modify-loudness-1-and-2-bit-exact"},
             std::string_view{"static-scale-and-clipping-read-when-it-starts"},
             std::string_view{"live-gain-held-per-block-where-cdp8-reads-its-breakpoint-file-every-frame"},
             std::string_view{"a-fixed-gain-that-leaves-silence-is-refused-as-in-cdp8"},
             std::string_view{"cdp8s-loudness-modes-that-read-whole-files-are-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpLoudnessGain", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-modify-loudness-6-and-phase-phase-1-bit-exact"},
             std::string_view{"static-mode-read-when-it-starts"},
             std::string_view{"cdp8s-phase-mode-2-stereo-enhancement-is-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPhaseInvert", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-housekeep-extract-4-bit-exact"},
             std::string_view{"static-shift-read-when-it-starts"},
             std::string_view{"every-sample-shifted-unclipped-where-cdp8-refuses-a-shift-past-its-files-peak"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDcShift", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-modify-radical-6-bit-exact-mono-with-mono"},
             std::string_view{"two-streams-of-one-length-where-cdp8-stops-at-the-shorter-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCrossModulate", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-subtract-subtract-bit-exact-mono-less-mono"},
             std::string_view{"two-streams-of-one-length-where-cdp8-pads-the-shorter-file-with-silence"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSubtract", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-submix-balance-bit-exact-mono-with-mono"},
             std::string_view{"live-balance-held-per-block-where-cdp8-reads-its-breakpoint-file-every-frame"},
             std::string_view{"no-begin-or-end-both-play-from-their-start-and-past-the-first-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixBalance", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-submix-crossfade-1-and-2-bit-exact-mono-with-mono"},
             std::string_view{"static-contour-times-stagger-and-power-factor-read-when-it-starts"},
             std::string_view{"end-must-be-given-where-cdp8-defaults-to-the-shorter-files-end"},
             std::string_view{"begin-to-32767-s-and-stagger-to-10-s-where-cdp8-allows-the-first-files-duration"},
             std::string_view{"after-end-the-side-chain-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixCrossfade", std::string{limitation});
    }
    // Fast convolution and CDP's restoration processes wait as long as their
    // ports report (talas::cdp::FastConvolution, SilenceGate, Deglitch,
    // NoiseCut and AttackBoost's latencyFrames), once their static fields,
    // and fast convolution's impulse sample, prepare them.
    for (const auto& [typeId, latencyField] : {
             std::pair{std::string_view{"talas.cdpFastConvolution"}, std::string_view{"impulse"}},
             std::pair{std::string_view{"talas.cdpSilenceGate"}, std::string_view{"gate"}},
             std::pair{std::string_view{"talas.cdpDeglitch"}, std::string_view{"glitch"}},
             std::pair{std::string_view{"talas.cdpNoiseCut"}, std::string_view{"splice"}},
             std::pair{std::string_view{"talas.cdpAttackBoost"}, std::string_view{"onset"}},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {
                LatencyKind::PreparationTime,
                0,
                std::string{latencyField},
                PreparedLatencyRule::CdpPortReported,
            });
    }
    for (const auto limitation : {
             std::string_view{"cdp8-fastconv-bit-exact-with-a-soundfile-impulse-response"},
             std::string_view{"static-dry-scale-and-impulse-channel-read-when-it-starts"},
             std::string_view{"impulse-sample-read-whole-when-it-starts-latency-its-frames-less-one"},
             std::string_view{"realtime-takes-impulses-up-to-4096-frames-offline-up-to-1048576"},
             std::string_view{"one-channel-of-a-stereo-impulse-the-louder-sets-the-gain-as-in-cdp8"},
             std::string_view{"the-tail-plays-on-past-cdp8s-end"},
             std::string_view{"cdp8s-text-impulse-file-is-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFastConvolution", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-gate-gate-1-bit-exact-with-cdp8s-default-buffers"},
             std::string_view{"static-gate-read-when-it-starts"},
             std::string_view{"latency-two-1-ms-splices"},
             std::string_view{"plays-on-where-cdp8-refuses-a-file-gated-nowhere-or-everywhere"},
             std::string_view{"no-gating-at-the-end-of-a-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSilenceGate", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-housekeep-deglitch-bit-exact"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"latency-the-longest-glitch-or-window-the-silence-after-it-and-a-splice"},
             std::string_view{"plays-on-where-cdp8-refuses-a-file-with-no-glitch"},
             std::string_view{"a-glitch-within-a-splice-of-the-start-fades-the-frames-it-has-where-cdp8-stops"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDeglitch", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-sfedit-noisecut-bit-exact-with-cdp8s-default-buffers"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"latency-three-splices-and-the-longer-minimum-length"},
             std::string_view{"cdp8s-second-search-starts-as-if-the-files-last-waveset-were-tone"},
             std::string_view{"plays-on-where-cdp8-refuses-or-stops-at-final-noise"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpNoiseCut", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-envel-attack-1-to-3-bit-exact-with-cdp8s-default-buffers"},
             std::string_view{"static-fields-read-when-it-starts-acting-once-after-the-sounds-start"},
             std::string_view{"latency-the-onset-and-400-ms-for-approximate-time-or-64-frames-for-gate"},
             std::string_view{"onset-up-to-1000-ms-where-cdp8-allows-32767"},
             std::string_view{"plays-on-where-cdp8-refuses"},
             std::string_view{"cdp8s-mode-4-the-whole-files-loudest-sample-is-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpAttackBoost", std::string{limitation});
    }
    // CDP's other two-file submix programs, which wait for nothing.
    for (const auto typeId : {
             std::string_view{"talas.cdpMixMerge"},
             std::string_view{"talas.cdpMixFaders"},
             std::string_view{"talas.cdpMixInterleave"},
             std::string_view{"talas.cdpMixInbetween"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }
    // Interleave's input to the left and its side chain to the right.
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpMixInterleave", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-submix-merge-bit-exact-mono-with-mono"},
             std::string_view{"static-stagger-skew-and-skip-read-when-it-starts"},
             std::string_view{"stagger-to-10-s-where-cdp8-allows-the-first-files-duration"},
             std::string_view{"skip-at-most-the-stagger-so-the-side-chain-is-never-read-ahead"},
             std::string_view{"stagger-and-skip-rounded-down-to-256-frame-sectors-as-in-cdp8"},
             std::string_view{"no-begin-or-end-both-play-on-past-the-shorter-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixMerge", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-submix-faders-bit-exact-with-two-mono-files"},
             std::string_view{"balance-table-read-when-it-starts-its-times-from-the-sounds-start"},
             std::string_view{"live-gain-held-per-block-where-cdp8-reads-its-breakpoint-file-every-frame"},
             std::string_view{"cdp8s-first-frame-at-gain-1-with-a-gain-breakpoint-file-is-not-kept"},
             std::string_view{"two-inputs-where-cdp8-takes-any-number-of-files"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixFaders", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-submix-interleave-and-repair-bit-exact-with-two-mono-files"},
             std::string_view{"input-left-and-side-chain-right"},
             std::string_view{"plays-on-where-cdp8-pads-the-shorter-file-or-refuses-files-of-different-lengths"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixInterleave", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-submix-inbetween-1-and-2-bit-exact-mono-with-mono"},
             std::string_view{"static-mode-ratio-count-and-file-read-when-it-starts"},
             std::string_view{"one-of-cdp8s-output-files"},
             std::string_view{"both-play-on-past-the-shorter-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMixInbetween", std::string{limitation});
    }
    // CDP's envel warp, impose and replace wait as long as their ports
    // report (talas::cdp::EnvelopeWarp, EnvelopeImpose and EnvelopeTarget's
    // latencyFrames): two windows less a frame, and the windows a warp reads
    // ahead, which static fields alone set.
    for (const auto typeId : {
             std::string_view{"talas.cdpEnvelopeWarp"},
             std::string_view{"talas.cdpEnvelopeTimestretch"},
             std::string_view{"talas.cdpEnvelopeImpose"},
             std::string_view{"talas.cdpEnvelopeTarget"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {
                LatencyKind::PreparationTime,
                0,
                std::string{"window"},
                PreparedLatencyRule::CdpPortReported,
            });
    }
    for (const auto limitation : {
             std::string_view{"cdp8-envel-warp-3-4-5-7-8-9-10-11-12-and-15-bit-exact"},
             std::string_view{"live-first-and-second-latched-at-each-window-start-where-cdp8-reads-its-breakpoint-files"},
             std::string_view{"static-mode-window-smoothing-and-maxima-read-when-it-starts"},
             std::string_view{"latency-two-windows-less-a-frame-and-the-windows-a-mode-reads-ahead-at-most-2-to-the-20-frames"},
             std::string_view{"a-live-flatten-or-corrugate-reads-ahead-as-far-as-its-static-maximum"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-rescale-the-whole-file"},
             std::string_view{"flatten-and-smoothing-play-on-past-cdp8s-file-end-corrugates-trough-pass-starts-falling"},
             std::string_view{"cdp8s-modes-1-2-6-13-14-and-16-need-the-whole-file-and-are-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpEnvelopeWarp", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-envel-impose-1-and-replace-1-bit-exact-with-a-mono-side-chain"},
             std::string_view{"static-mode-and-window-read-when-it-starts"},
             std::string_view{"latency-two-windows-less-a-frame"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-rescale-the-whole-file"},
             std::string_view{"the-side-chain-plays-on-where-cdp8-holds-its-last-envelope-value"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpEnvelopeImpose", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-envel-replace-3-and-4-bit-exact"},
             std::string_view{"live-target-latched-at-each-window-start-where-cdp8-samples-its-breakpoint-file"},
             std::string_view{"static-scale-and-window-read-when-it-starts"},
             std::string_view{"latency-two-windows-less-a-frame"},
             std::string_view{"cdp8s-mode-2-binary-envelope-file-is-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpEnvelopeTarget", std::string{limitation});
    }
    // CDP's newdelay, bounce and rejoin wait for nothing.
    for (const auto typeId : {
             std::string_view{"talas.cdpPitchedDelay"},
             std::string_view{"talas.cdpBounce"},
             std::string_view{"talas.cdpRejoin"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
    }
    // Cascade, phasor and spin wait as long as their ports report
    // (talas::cdp::Cascade, Phasor and Spin's latencyFrames): the longest clip
    // and two splices less a frame, the packet of the lowest frequency and a
    // frame, and a frame in spin's wide modes, which static fields alone
    // set.
    for (const auto& [typeId, latencyField] : {
             std::pair{std::string_view{"talas.cdpCascade"}, std::string_view{"clipSize"}},
             std::pair{std::string_view{"talas.cdpPhasor"}, std::string_view{"frequency"}},
             std::pair{std::string_view{"talas.cdpSpin"}, std::string_view{"rate"}},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {
                LatencyKind::PreparationTime,
                0,
                std::string{latencyField},
                PreparedLatencyRule::CdpPortReported,
            });
    }
    // Left and right: one channel on both, or CDP's two; out1 to out8:
    // CDP's channels, eight for cascade's rotations, 1 to 8 for phasor, 4
    // to 8 for spin's wide modes.
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpCascade", {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete8});
    for (const auto typeId : {
             std::string_view{"talas.cdpPhasor"},
             std::string_view{"talas.cdpSpin"},
         })
    {
        registry.setNodeTypeOutputBusLayouts(
            std::string{typeId},
            {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6,
                AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-newdelay-bit-exact"},
             std::string_view{"live-pitch-held-per-block-where-cdp8-reads-its-breakpoint-file-every-frame"},
             std::string_view{"static-mix-feedback-and-lowest-pitch-read-when-it-starts-the-lowest-pitch-for-a-live-pitch"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-prescale-the-input-by-its-peak"},
             std::string_view{"no-tail-the-line-rings-on-as-the-stream-goes-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchedDelay", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-bounce-bounce-with-s0-bit-exact"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"whole-copies-as-a-multi-tap-delay-cdp8s-s-c-and-e-need-the-sources-duration"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-fade-its-output-down"},
             std::string_view{"the-last-copy-within-2-to-the-22-frames"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBounce", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-rejoin-1-and-2-bit-exact-with-two-mono-files-the-input-cdp8s-longest"},
             std::string_view{"static-gain-read-when-it-starts"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-scale-its-output"},
             std::string_view{"cdp8s-r-reads-backwards-from-the-files-ends-and-is-not-ported"},
             std::string_view{"both-play-on-past-the-shorter-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRejoin", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-cascade-1-to-5-bit-exact-before-cdp8s-shortened-last-clip"},
             std::string_view{"static-fields-read-when-it-starts-as-cdp8-reads-its-breakpoint-files-at-time-0"},
             std::string_view{"latency-the-longest-clip-and-two-splices-less-a-frame"},
             std::string_view{"echoes-in-one-channel-on-both-outputs"},
             std::string_view{"rotate-and-rotate-back-on-out1-to-out8-left-and-right-silent"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-normalise-its-output"},
             std::string_view{"cdp8s-modes-6-to-10-shredding-and-n-are-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCascade", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-phasor-bit-exact-on-1-to-8-output-channels-surround-too"},
             std::string_view{"live-frequency-and-shift-latched-as-each-packet-starts"},
             std::string_view{"static-streams-output-channels-surround-offset-and-lowest-frequency-read-when-it-starts"},
             std::string_view{"latency-the-packet-of-the-lowest-frequency-and-a-frame"},
             std::string_view{"one-output-channel-on-both-outputs-more-on-out1-to-outn-left-and-right-silent"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-normalise-its-output"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPhasor", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-spin-stereo-1-to-3-without-doppler-bit-exact-mode-1-where-cdp8s-level-pass-ends-its-half-cycle-clear"},
             std::string_view{"live-rate-held-per-block-where-cdp8-reads-its-breakpoint-file-every-frame"},
             std::string_view{"static-mode-boost-attenuation-channels-centre-and-its-levels-read-when-it-starts"},
             std::string_view{"wide-modes-on-out1-to-outn-a-frame-late-left-and-right-silent"},
             std::string_view{"the-half-cycle-flag-starts-clear-cdp8s-level-pass-end-state-is-a-whole-file-fact"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-normalise-its-output"},
             std::string_view{"cdp8s-doppler-and-quad-are-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpin", std::string{limitation});
    }
    // Brassage waits as long as its port reports (talas::cdp::Brassage's
    // latencyFrames): the longest grain less a frame, or the grain's
    // transposed read at the highest pitch, which static fields alone set.
    registry.setNodeTypeProcessingContract(
        "talas.cdpBrassage",
        NodeStateModel::SessionStateful,
        true,
        {
            LatencyKind::PreparationTime,
            0,
            std::string{"grainsize"},
            PreparedLatencyRule::CdpPortReported,
        });
    // Two outputs: one channel on both, or reverb's two.
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpBrassage", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-brassage-1-3-4-and-5-bit-exact-with-a-mono-input"},
             std::string_view{"live-pitch-density-amp-grainsize-and-range-taken-at-each-grains-start-a-block-at-a-time"},
             std::string_view{"static-mode-bounds-seed-and-memory-read-when-it-starts-a-live-fields-bound-sizes-cdp8s-buffers"},
             std::string_view{"latency-the-longest-grain-less-a-frame-or-its-transposed-read-at-the-highest-pitch"},
             std::string_view{"one-channel-on-both-outputs-reverbs-two-channels"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-renormalise-its-output"},
             std::string_view{"the-seed-is-a-field-where-cdp8-seeds-from-the-time"},
             std::string_view{"a-stereo-input-plays-in-the-port-the-node-takes-mono"},
             std::string_view{"cdp8s-modes-2-6-and-7-and-their-fields-are-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBrassage", std::string{limitation});
    }
    // CDP's multichannel tools: eight outputs, CDP's channels on out1 to
    // outN and the others silent; none waits. Those that take more than one
    // input stream align them as they arrive.
    for (const auto typeId : {
             std::string_view{"talas.cdpRotatingPan"},
             std::string_view{"talas.cdpSpreadPan"},
             std::string_view{"talas.cdpTablePan"},
             std::string_view{"talas.cdpStepSpread"},
             std::string_view{"talas.cdpCrossPan"},
             std::string_view{"talas.cdpAntiphony"},
             std::string_view{"talas.cdpMultichannelStadium"},
             std::string_view{"talas.cdpFlutter"},
             std::string_view{"talas.cdpFrameRotate"},
             std::string_view{"talas.cdpFrameRemap"},
             std::string_view{"talas.cdpChannelEnvelope"},
             std::string_view{"talas.cdpToStereo"},
             std::string_view{"talas.cdpStereoSpace"},
             std::string_view{"talas.cdpChannelZero"},
             std::string_view{"talas.cdpChannelMixdown"},
             std::string_view{"talas.cdpMonoToStereo"},
             std::string_view{"talas.cdpChannelPhase"},
             std::string_view{"talas.cdpMonoToMany"},
             std::string_view{"talas.cdpChannelPair"},
             std::string_view{"talas.cdpStereoPlacement"},
             std::string_view{"talas.cdpPrefix"},
             std::string_view{"talas.cdpSilenceMasks"},
             std::string_view{"talas.cdpSilenceOverwrite"},
         })
    {
        registry.setNodeTypePreparedRealtimeSupport(
            typeId,
            ExecutionSupport::Supported,
            ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeFanInLatencyPolicy(
            typeId,
            typeId == "talas.cdpRotatingPan" || typeId == "talas.cdpTablePan" || typeId == "talas.cdpStepSpread"
                    || typeId == "talas.cdpMultichannelStadium" || typeId == "talas.cdpMonoToStereo"
                    || typeId == "talas.cdpMonoToMany"
                ? FanInLatencyPolicy::NotApplicable
                : FanInLatencyPolicy::Align);
        registry.setNodeTypeResourceAccess(typeId, ResourceAccessMode::None);
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {LatencyKind::None, 0});
        registry.addNodeTypeLimitation(
            typeId, "out1-to-outn-play-cdp8s-channels-the-other-outputs-silent-cdp-float-sample-buffers");
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpRotatingPan",
        {AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-9-rotate-bit-exact-on-3-to-8-channels"},
             std::string_view{"live-speed-taken-where-cdp8-reads-it-every-256-frames-a-block-at-a-time"},
             std::string_view{"static-channels-start-focus-and-direction-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRotatingPan", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpSpreadPan",
        {AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-4-panspread-bit-exact-on-3-to-8-channels-mono-or-stereo-in"},
             std::string_view{"live-spread-depth-and-rolloff-taken-where-cdp8-reads-them-every-256-frames-a-block-at-a-time"},
             std::string_view{"static-channels-and-centre-read-when-it-starts"},
             std::string_view{"cdp8s-stereo-centre-kept-where-the-last-levels-left-it-when-the-spread-fills-the-ring"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpreadPan", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpTablePan",
        {AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-1-table-pan-bit-exact-on-3-to-8-channels"},
             std::string_view{"static-table-and-focus-read-when-it-starts-the-table-from-the-streams-start"},
             std::string_view{"cdp8s-table-quirks-kept-the-first-time-ignored-each-move-a-frame-longer"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpTablePan", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpStepSpread",
        {AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-3-stepspread-bit-exact-on-3-to-8-channels-given-cdp8s-read-times"},
             std::string_view{"live-centre-spread-depth-and-rolloff-taken-as-each-event-starts"},
             std::string_view{"cdp8-reads-centre-depth-and-rolloff-at-a-time-within-its-buffer-the-node-at-the-events-start"},
             std::string_view{"static-channels-minimum-silence-and-stepped-a-stepped-centre-spread-and-depth-static"},
             std::string_view{"a-stream-without-a-long-enough-silence-is-one-event-where-cdp8-refuses-the-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStepSpread", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpCrossPan",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-7-crosspan-bit-exact-on-2-to-8-channels-in-in1-to-inn"},
             std::string_view{"static-table-rolloff-and-buffer-frames-read-when-it-starts"},
             std::string_view{"cdp8s-held-map-wrapping-spill-and-late-table-quirks-by-default-corrected-plays-the-evident-intent"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCrossPan", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpAntiphony",
        {AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanpan-6-antiphony-bit-exact-on-3-to-8-channels-before-cdp8s-last-event"},
             std::string_view{"live-event-duration-and-gap-taken-as-each-events-splice-in-ends"},
             std::string_view{"static-channels-sets-splice-and-buffer-frames-read-when-it-starts"},
             std::string_view{"a-mono-or-stereo-input-and-a-side-chain-of-as-many-channels"},
             std::string_view{"cdp8s-half-sets-a-channel-up-by-default-corrected-plays-whole-sets-on-their-channels"},
             std::string_view{"the-stream-switches-on-where-cdp8s-last-event-plays-to-the-files-end"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpAntiphony", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpMultichannelStadium",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-mchanrev-bit-exact-on-2-to-8-channels-without-the-tail-past-the-input"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"the-direct-sound-as-this-build-of-cdp8-reads-its-unset-tap"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMultichannelStadium", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpFlutter",
        {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-flutter-bit-exact-on-1-to-8-channels-in-in1-to-inn"},
             std::string_view{"live-frequency-and-depth-held-per-block-where-cdp8-reads-them-every-frame"},
             std::string_view{"cdp8s-clock-frozen-for-the-rest-of-its-buffer-after-a-depth-of-0"},
             std::string_view{"static-sets-gain-random-order-and-buffer-frames-read-when-it-starts-no-sets-each-channel-its-own"},
             std::string_view{"random-order-from-the-start-of-cdp8s-unseeded-drand48"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFlutter", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpFrameRotate",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-frame-shift-1-and-2-bit-exact-on-2-to-8-channels-in-in1-to-inn"},
             std::string_view{"live-rotations-taken-where-cdp8-reads-them-every-sr-over-500-frames-a-block-at-a-time"},
             std::string_view{"first-directions-are-static-fields-where-cdp8-looks-ahead-in-its-breakpoint-files"},
             std::string_view{"static-mode-smear-and-snake-read-when-it-starts-cdp8s-later-snakes-are-not-taken"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFrameRotate", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpFrameRemap",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-frame-shift-3-4-5-6-and-8-bit-exact-on-2-to-8-channels-in-in1-to-inn"},
             std::string_view{"static-mode-route-plane-and-channels-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFrameRemap", std::string{limitation});
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpChannelEnvelope",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-frame-shift-7-envchans-bit-exact-on-2-to-8-channels-in-in1-to-inn"},
             std::string_view{"live-gain-held-per-block-where-cdp8-steps-it-frame-by-frame-between-breakpoints"},
             std::string_view{"static-enveloped-channels-read-when-they-start"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpChannelEnvelope", std::string{limitation});
    }
    for (const auto typeId : {
             std::string_view{"talas.cdpStereoSpace"},
             std::string_view{"talas.cdpChannelZero"},
             std::string_view{"talas.cdpChannelMixdown"},
             std::string_view{"talas.cdpMonoToStereo"},
             std::string_view{"talas.cdpChannelPhase"},
             std::string_view{"talas.cdpMonoToMany"},
             std::string_view{"talas.cdpChannelPair"},
             std::string_view{"talas.cdpStereoPlacement"},
             std::string_view{"talas.cdpPrefix"},
             std::string_view{"talas.cdpSilenceMasks"},
             std::string_view{"talas.cdpSilenceOverwrite"},
         })
    {
        registry.setNodeTypeOutputBusLayouts(
            typeId,
            {AudioBusLayoutKind::Mono, AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4,
                AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    }
    for (const auto& [typeId, limitation] : {
             std::pair{std::string_view{"talas.cdpStereoSpace"}, std::string_view{"cdp8-modify-space-2-and-4-bit-exact"}},
             std::pair{std::string_view{"talas.cdpChannelZero"},
                 std::string_view{"cdp8-housekeep-chans-3-bit-exact-a-mono-inputs-second-channel-silent-on-each-buffers-first-frame"}},
             std::pair{std::string_view{"talas.cdpChannelMixdown"},
                 std::string_view{"cdp8-housekeep-chans-4-bit-exact-stereo-only-cdp8s-multichannel-mix-takes-a-whole-file-gain"}},
             std::pair{std::string_view{"talas.cdpMonoToStereo"}, std::string_view{"cdp8-housekeep-chans-5-bit-exact"}},
             std::pair{std::string_view{"talas.cdpChannelPhase"}, std::string_view{"cdp8-chanphase-bit-exact-up-to-8-channels"}},
             std::pair{std::string_view{"talas.cdpMonoToMany"}, std::string_view{"cdp8-mton-bit-exact-up-to-8-channels"}},
             std::pair{std::string_view{"talas.cdpChannelPair"},
                 std::string_view{"cdp8-pairex-bit-exact-a-second-channel-past-the-input-refused"}},
             std::pair{std::string_view{"talas.cdpStereoPlacement"},
                 std::string_view{"cdp8-mchstereo-bit-exact-up-to-four-stereo-inputs-on-in1-to-in8"}},
             std::pair{std::string_view{"talas.cdpPrefix"},
                 std::string_view{"cdp8-prefix-silence-bit-exact-at-the-inputs-length-up-to-10-s"}},
             std::pair{std::string_view{"talas.cdpSilenceMasks"},
                 std::string_view{"cdp8-sfedit-masks-bit-exact-with-its-buffers-times-not-bounded-by-a-files-length"}},
             std::pair{std::string_view{"talas.cdpSilenceOverwrite"},
                 std::string_view{"cdp8-sfedit-insil-overwrite-bit-exact-insertion-changes-length-and-is-not-ported"}},
         })
    {
        registry.addNodeTypeLimitation(std::string{typeId}, std::string{limitation});
        registry.addNodeTypeLimitation(std::string{typeId}, "static-fields-read-when-it-starts");
    }
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpToStereo",
        {AudioBusLayoutKind::Stereo, AudioBusLayoutKind::Discrete4, AudioBusLayoutKind::Discrete6, AudioBusLayoutKind::Discrete8, AudioBusLayoutKind::VariableOrUnknown});
    for (const auto limitation : {
             std::string_view{"cdp8-tostereo-bit-exact-on-2-to-8-channels"},
             std::string_view{"static-fields-read-when-it-starts-cdp8-also-bounds-the-times-by-the-files-duration"},
             std::string_view{"the-channels-cdp8-never-writes-are-silent"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpToStereo", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpTremolo",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-envel-tremolo-raised-table-sine-bit-exact"},
             std::string_view{"static-or-live-frequency-0-to-500-hertz-and-depth-and-gain-0-to-1-held-per-block"},
             std::string_view{"cdp8-logarithmic-breakpoint-frequency-mode-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpTremolo", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpPan",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpPan", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-space-1-distance-law-bit-exact"},
             std::string_view{"position-and-prescale-read-every-128-frames"},
             std::string_view{"position-within-256-and-prescale-from-1-over-32767-to-2"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPan", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpTapDelay",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpTapDelay", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-tapdelay-bit-exact"},
             std::string_view{"static-taps-tap-gain-feedback-and-mix-read-when-it-starts"},
             std::string_view{"up-to-256-taps-within-10-seconds-a-mono-tap-list-on-both-outputs"},
             std::string_view{"no-trailer-time-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpTapDelay", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpEcho",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-revecho-1-bit-exact"},
             std::string_view{"static-delay-prescale-and-invert-read-when-it-starts"},
             std::string_view{"live-mix-and-feedback-read-every-128-frames-as-cdp8-domultiv-reads-breakpoints-without-invert"},
             std::string_view{"delay-from-one-sample-to-10-seconds-feedback-above-minus-1-to-1"},
             std::string_view{"cdp8-trailer-time-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpEcho", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpVaryingDelay",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-revecho-2-bit-exact"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"no-dry-signal-as-cdp8-mode-2-leaves-its-dry-factor-at-zero"},
             std::string_view{"cdp8-random-oscillation-ignores-its-seed-which-seed-1-reproduces"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpVaryingDelay", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpStadiumEcho",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpStadiumEcho", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-modify-revecho-3-bit-exact"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"echoes-added-in-the-order-of-cdp8-default-1-mb-buffers"},
             std::string_view{"last-echo-within-10-seconds-no-trailer-time-the-stream-plays-on"},
             std::string_view{"cdp8-normalising-reads-the-whole-input-and-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStadiumEcho", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpReverb",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    registry.setNodeTypeOutputBusLayouts(
        "talas.cdpReverb", {AudioBusLayoutKind::Stereo});
    for (const auto limitation : {
             std::string_view{"cdp8-reverb-bit-exact-for-a-mono-input-and-two-outputs"},
             std::string_view{"static-fields-read-when-it-starts"},
             std::string_view{"preset-rooms-and-loop-times-without-cdp8-tap-or-times-files"},
             std::string_view{"no-trailer-time-the-tail-plays-as-the-stream-goes-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpReverb", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpVariableFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-variable-bit-exact"},
             std::string_view{"static-mode-and-gain-read-when-it-starts"},
             std::string_view{"live-acuity-and-frequency-read-each-128-frame-block-as-cdp8-reads-breakpoints"},
             std::string_view{"output-clipped-to-plus-or-minus-1-lowering-the-gain-as-in-cdp8"},
             std::string_view{"cdp8-tail-time-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpVariableFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpFixedFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-fixed-bit-exact"},
             std::string_view{"static-mode-boost-frequency-bandwidth-and-prescale-read-when-it-starts"},
             std::string_view{"boost-or-cut-from-minus-96-to-36-db-bandwidth-for-the-band-only"},
             std::string_view{"no-tail-time-the-filter-rings-on-as-the-stream-goes-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFixedFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpPhasing",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-phasing-bit-exact"},
             std::string_view{"static-mode-gain-and-prescale-read-when-it-starts"},
             std::string_view{"live-delay-read-each-frame-in-whole-frames-as-cdp8-reads-breakpoints"},
             std::string_view{"delay-from-one-sample-to-10-seconds-where-cdp8-allows-half-the-input"},
             std::string_view{"cdp8-linear-delay-steps-and-tail-time-have-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPhasing", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpLohiFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-lohi-bit-exact"},
             std::string_view{"static-attenuation-bands-and-prescale-read-when-it-starts"},
             std::string_view{"plays-the-last-section-of-its-cascade-as-cdp8-feeds-each-the-input"},
             std::string_view{"output-clipped-to-plus-or-minus-1-no-tail-time"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpLohiFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpSweepingFilter",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-sweeping-bit-exact"},
             std::string_view{"static-mode-gain-and-phase-read-when-it-starts"},
             std::string_view{"live-acuity-and-low-high-and-sweep-frequencies-read-each-128-frame-block-as-cdp8-reads-breakpoints"},
             std::string_view{"frequency-moves-every-128-frames-output-clipped-lowering-the-gain-as-in-cdp8"},
             std::string_view{"cdp8-tail-time-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSweepingFilter", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpUserBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-userbank-bit-exact"},
             std::string_view{"static-bands-gain-and-double-filtering-read-when-it-starts"},
             std::string_view{"live-q-read-each-128-frame-block-as-cdp8-reads-breakpoints"},
             std::string_view{"at-most-2000-bands-output-clipped-lowering-the-gain-as-in-cdp8"},
             std::string_view{"cdp8-tail-time-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpUserBank", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortOverload",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-overload-bit-exact"},
             std::string_view{"static-clip-level-and-seed-read-when-it-starts"},
             std::string_view{"cdp8-noise-reads-an-unseeded-rand-which-seed-1-reproduces"},
             std::string_view{"live-depth-and-frequency-read-every-10-ms-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-restarts-its-10-ms-reads-at-each-buffer-the-stream-does-not"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortOverload", std::string{limitation});
    }
    // talas::cdp::DistortReform::latencyFrames: the stream waits for each half-cycle.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortReform",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-reform-bit-exact"},
             std::string_view{"static-mode-read-when-it-starts"},
             std::string_view{"live-exaggeration-read-at-each-half-cycle-midpoint-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-reads-a-buffer-behind-where-its-file-ends-the-stream-does-not-end"},
             std::string_view{"latency-2047-frames-longer-half-cycles-with-the-zeros-before-them-are-cut"},
             std::string_view{"cdp8-leaves-its-last-half-cycle-the-stream-plays-on"},
             std::string_view{"reads-zero-past-cdp8-buffers-and-keeps-silence-across-them-in-exaggerate"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortReform", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortOmit",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-omit-bit-exact"},
             std::string_view{"static-out-of-read-when-it-starts"},
             std::string_view{"live-omit-read-rounded-where-each-group-starts-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortOmit", std::string{limitation});
    }
    // talas::cdp::DistortReverse::latencyFrames: the stream waits for each group.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortReverse",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-reverse-bit-exact"},
             std::string_view{"live-cycles-read-rounded-where-each-group-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"latency-2047-frames-longer-groups-are-cut"},
             std::string_view{"cdp8-leaves-its-last-group-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortReverse", std::string{limitation});
    }
    // talas::cdp::DistortEnvelope::latencyFrames: the stream waits for each group.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortEnvelope",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-envel-bit-exact"},
             std::string_view{"static-shape-and-envelope-read-when-it-starts"},
             std::string_view{"live-cycles-trough-and-exponent-read-where-each-group-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"latency-2047-frames-longer-groups-are-cut"},
             std::string_view{"cdp8-envelopes-its-last-group-to-its-end-the-stream-plays-on"},
             std::string_view{"exponent-1-plays-cdp8-linear-envelope-without-its-e-flag"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortEnvelope", std::string{limitation});
    }
    // talas::cdp::DistortHarmonic::latencyFrames: the stream waits for each cycle.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortHarmonic",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-harmonic-bit-exact"},
             std::string_view{"static-harmonics-and-prescale-read-when-it-starts"},
             std::string_view{"latency-2047-frames-longer-cycles-are-cut"},
             std::string_view{"cdp8-leaves-its-last-cycle-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortHarmonic", std::string{limitation});
    }
    // talas::cdp::DistortFractal::latencyFrames: the stream waits for each group.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortFractal",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-fractal-bit-exact"},
             std::string_view{"static-prescale-read-when-it-starts"},
             std::string_view{"live-scale-and-loudness-read-scale-rounded-where-each-group-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"latency-2047-frames-longer-groups-are-cut"},
             std::string_view{"cdp8-leaves-its-last-group-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortFractal", std::string{limitation});
    }
    // talas::cdp::DistortMultiply::latencyFrames: the stream waits for each
    // half-cycle.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortMultiply",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-multiply-bit-exact"},
             std::string_view{"live-factor-read-rounded-where-each-half-cycle-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"latency-2047-frames-longer-half-cycles-with-the-zeros-before-them-are-cut"},
             std::string_view{"cdp8-drops-its-last-half-cycle-the-stream-plays-on"},
             std::string_view{"cdp8-smoothing-flag-changes-nothing-and-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortMultiply", std::string{limitation});
    }
    // talas::cdp::DistortDivide::latencyFrames: the stream waits for each group.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortDivide",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-divide-bit-exact"},
             std::string_view{"static-interpolation-read-when-it-starts"},
             std::string_view{"live-factor-read-rounded-where-each-group-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"latency-2047-frames-longer-groups-are-cut"},
             std::string_view{"cdp8-drops-its-last-group-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortDivide", std::string{limitation});
    }
    // talas::cdp::DistortShuffle::latencyFrames: the stream waits for each domain.
    registry.setNodeTypeProcessingContract(
        "talas.cdpDistortShuffle",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::Fixed, 2047});
    for (const auto limitation : {
             std::string_view{"cdp8-distort-shuffle-bit-exact"},
             std::string_view{"static-shuffle-data-read-when-it-starts"},
             std::string_view{"live-cycles-read-rounded-where-each-domain-starts-as-cdp8-reads-breakpoints"},
             std::string_view{"images-reorder-every-domain-letter-once-as-a-stream-keeps-its-length"},
             std::string_view{"latency-2047-frames-longer-domains-play-as-they-came-in"},
             std::string_view{"cdp8-drops-its-last-domain-the-stream-plays-on"},
             std::string_view{"cdp8-skipped-cycles-have-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpDistortShuffle", std::string{limitation});
    }
    // talas::cdp::PhaseVocoder::latencyFor: the stream waits for the frames
    // each output frame needs.
    for (const auto typeId : {
             std::string_view{"talas.cdpPhaseVocoder"},
             std::string_view{"talas.cdpSpectralGain"},
             std::string_view{"talas.cdpSpectralGate"},
             std::string_view{"talas.cdpFocusExaggerate"},
             std::string_view{"talas.cdpFocusFold"},
             std::string_view{"talas.cdpFocusAccumulate"},
             std::string_view{"talas.cdpBlurAverage"},
             std::string_view{"talas.cdpBlurSuppress"},
             std::string_view{"talas.cdpHiliteTrace"},
             std::string_view{"talas.cdpHiliteFilter"},
             std::string_view{"talas.cdpHilitePluck"},
             std::string_view{"talas.cdpFocusStep"},
             std::string_view{"talas.cdpStrangeShift"},
             std::string_view{"talas.cdpRepitchTranspose"},
             std::string_view{"talas.cdpStrangeWaver"},
             std::string_view{"talas.cdpPitchTransp"},
             std::string_view{"talas.cdpBlurChorus"},
             std::string_view{"talas.cdpBlurNoise"},
             std::string_view{"talas.cdpStretchSpectrum"},
             std::string_view{"talas.cdpBlurScatter"},
             std::string_view{"talas.cdpHiliteArpeg"},
             std::string_view{"talas.cdpHiliteGraphicEq"},
             std::string_view{"talas.cdpHiliteBand"},
             std::string_view{"talas.cdpBlurSpread"},
             std::string_view{"talas.cdpStrangeGlis"},
             std::string_view{"talas.cdpCombineSum"},
             std::string_view{"talas.cdpCombineDiff"},
             std::string_view{"talas.cdpCombineMax"},
             std::string_view{"talas.cdpCombineMean"},
             std::string_view{"talas.cdpCombineCross"},
             std::string_view{"talas.cdpFormantsVocode"},
             std::string_view{"talas.cdpMorphMorph"},
             std::string_view{"talas.cdpMorphBridge"},
             std::string_view{"talas.cdpRepitchTransposef"},
             std::string_view{"talas.cdpFormantsPut"},
             std::string_view{"talas.cdpHiliteVowels"},
             std::string_view{"talas.cdpBlurBlur"},
             std::string_view{"talas.cdpHiliteBlurTrace"},
             std::string_view{"talas.cdpFocusFocus"},
             std::string_view{"talas.cdpPitchTune"},
             std::string_view{"talas.cdpPitchPick"},
             std::string_view{"talas.cdpPitchChord"},
             std::string_view{"talas.cdpPitchChordf"},
             std::string_view{"talas.cdpPitchAltharms"},
             std::string_view{"talas.cdpPitchOctmove"},
             std::string_view{"talas.cdpSpecBare"},
             std::string_view{"talas.cdpSpecClean"},
             std::string_view{"talas.cdpStrangeInvert"},
             std::string_view{"talas.cdpFocusFreeze"},
         })
    {
        registry.setNodeTypeProcessingContract(
            std::string{typeId},
            NodeStateModel::SessionStateful,
            true,
            {
                LatencyKind::PreparationTime,
                0,
                "points",
                PreparedLatencyRule::CdpPhaseVocoderProfile,
            });
        for (const auto limitation : {
                 std::string_view{"static-points-and-overlap-read-when-it-starts"},
                 std::string_view{"latency-of-the-analysis-frames-each-output-frame-needs"},
                 std::string_view{"clips-at-full-scale-as-cdp8-writes-its-output"},
                 std::string_view{"cdp8-misses-a-last-frame-at-the-end-of-its-file-at-some-sizes-the-stream-plays-on"},
             })
        {
            registry.addNodeTypeLimitation(std::string{typeId}, std::string{limitation});
        }
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pvoc-anal-and-synth-bit-exact"},
             std::string_view{"cdp8-pvoc-extract-fails-to-write-its-output-and-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPhaseVocoder", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-spec-gain-bit-exact"},
             std::string_view{"live-gain-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpectralGain", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-exag-bit-exact"},
             std::string_view{"live-exaggeration-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-leaves-its-first-frame"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusExaggerate", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-fold-bit-exact"},
             std::string_view{"live-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"live-frequencies-under-10-hz-read-10-hz-static-ones-zero-as-cdp8"},
             std::string_view{"static-body-flag-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusFold", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-accu-bit-exact"},
             std::string_view{"live-decay-and-glissando-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-leaves-its-first-frame-and-holds-its-amplitudes"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusAccumulate", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-avrg-bit-exact"},
             std::string_view{"live-n-read-rounded-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-leaves-its-first-frame"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurAverage", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-suppress-bit-exact"},
             std::string_view{"live-n-read-rounded-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"n-below-the-channel-count-as-cdp8-keeps-an-empty-ring-at-its-last-size"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurSuppress", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-trace-bit-exact"},
             std::string_view{"live-n-and-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"live-frequencies-under-10-hz-read-10-hz-static-ones-zero-as-cdp8"},
             std::string_view{"static-mode-and-retain-flag-read-when-it-starts"},
             std::string_view{"n-below-the-channel-count-outside-mode-all-as-cdp8-keeps-an-empty-ring-at-its-last-size"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteTrace", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-filter-bit-exact"},
             std::string_view{"live-frequencies-and-q-read-at-each-analysis-frame-as-cdp8-reads-breakpoints-through-their-logarithms"},
             std::string_view{"static-mode-and-gain-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteFilter", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-pluck-bit-exact"},
             std::string_view{"live-gain-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-leaves-its-first-frame-and-takes-the-second-as-the-start"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHilitePluck", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-step-bit-exact"},
             std::string_view{"static-step-read-when-it-starts-rounded-to-whole-frames"},
             std::string_view{"step-at-most-an-hour-where-cdp8-takes-its-whole-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusStep", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-strange-shift-bit-exact"},
             std::string_view{"live-shift-and-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-log-interpolation-flag-has-no-counterpart-as-live-fields-hold-each-frame"},
             std::string_view{"static-mode-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStrangeShift", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-repitch-transpose-bit-exact"},
             std::string_view{"live-transposition-and-band-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"live-semitones-convert-as-cdp8-breakpoint-files-static-ones-as-its-constants"},
             std::string_view{"cdp8-leaves-its-first-frame"},
             std::string_view{"static-units-and-fuller-flag-read-when-it-starts"},
             std::string_view{"cdp8-binary-transposition-file-mode-4-has-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRepitchTranspose", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-strange-waver-bit-exact"},
             std::string_view{"live-vibrato-and-stretch-read-at-each-cycle-start-as-cdp8-reads-breakpoints"},
             std::string_view{"static-mode-bottom-frequency-and-exponent-read-when-it-starts"},
             std::string_view{"slowest-vibrato-an-hour-a-cycle-where-cdp8-takes-its-whole-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStrangeWaver", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-transp-bit-exact"},
             std::string_view{"live-split-transpositions-and-depth-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"shift-modes-need-every-transposition-live-once-any-of-them-or-the-split-is-as-cdp8-converts-static-ones-again-each-frame"},
             std::string_view{"cdp8-depth-leaves-the-second-transposition-of-mode-6-as-its-mode-test-never-matches"},
             std::string_view{"static-mode-and-depth-flag-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchTransp", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-chorus-bit-exact"},
             std::string_view{"live-spreads-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-random-tables-from-its-unseeded-generator-the-same-at-each-start"},
             std::string_view{"both-spreads-live-in-modes-5-to-7-scale-frequencies-by-the-bare-random-number-as-cdp8-does"},
             std::string_view{"static-mode-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurChorus", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-noise-bit-exact"},
             std::string_view{"live-noise-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurNoise", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-stretch-spectrum-bit-exact"},
             std::string_view{"static-mode-divide-stretch-exponent-and-depth-flag-read-when-it-starts"},
             std::string_view{"live-depth-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStretchSpectrum", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-scatter-bit-exact"},
             std::string_view{"live-keep-and-block-size-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-random-draws-from-its-unseeded-generator-the-same-at-each-start"},
             std::string_view{"cdp8-keeps-the-lowest-blocks-as-it-moves-on-only-past-kept-ones"},
             std::string_view{"keep-at-or-over-the-frames-block-count-is-refused-where-cdp8-stops"},
             std::string_view{"static-randomise-and-normalise-flags-read-when-it-starts"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurScatter", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-arpeg-bit-exact"},
             std::string_view{"static-mode-wave-start-phase-decay-and-flags-read-when-it-starts"},
             std::string_view{"live-rate-frequencies-bandwidth-amplification-and-sustain-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-sweeps-each-frame-by-the-mean-of-its-rate-and-the-last-frames"},
             std::string_view{"cdp8-divides-a-fixed-amplification-by-a-live-sustain-again-at-each-frame"},
             std::string_view{"modes-below-to-once-above-take-the-amplification-as-cdp8-takes-a-breakpoint-one-where-a-fixed-one-plays-nan"},
             std::string_view{"sustain-up-to-an-hour-where-cdp8-allows-its-whole-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteArpeg", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-greq-bit-exact"},
             std::string_view{"static-mode-bands-and-notch-read-when-it-starts"},
             std::string_view{"cdp8-sorts-unordered-bands-into-repeated-frequencies-and-bandwidths"},
             std::string_view{"cdp8-shapes-every-band-by-the-last-bands-bandwidth"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteGraphicEq", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-band-bit-exact"},
             std::string_view{"static-bands-read-when-it-starts"},
             std::string_view{"cdp8-ramps-amplitude-across-each-bands-frequencies"},
             std::string_view{"a-partial-moved-below-0-hz-stays-in-its-channel-where-cdp8-stops"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteBand", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-spread-bit-exact"},
             std::string_view{"static-envelope-bands-and-quick-search-read-when-it-starts"},
             std::string_view{"live-spread-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurSpread", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-strange-glis-bit-exact"},
             std::string_view{"static-mode-envelope-bands-quick-search-spacing-and-top-read-when-it-starts"},
             std::string_view{"live-glissando-rate-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-glides-a-zero-rate-folding-up-and-writing-down"},
             std::string_view{"cdp8-self-glissando-keeps-its-shifted-and-stale-channels"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStrangeGlis", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-combine-sum-bit-exact"},
             std::string_view{"live-crossover-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"a-fixed-crossover-of-1-adds-plainly-as-cdp8-does"},
             std::string_view{"two-streams-of-one-length-where-cdp8-copies-the-longer-files-rest"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCombineSum", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-combine-diff-bit-exact"},
             std::string_view{"live-crossover-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"static-keep-negative-flag-read-when-it-starts"},
             std::string_view{"two-streams-of-one-length-where-cdp8-copies-the-longer-inputs-rest"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCombineDiff", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-combine-max-bit-exact"},
             std::string_view{"two-streams-of-one-length-where-cdp8-bases-on-the-longer-file"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCombineMax", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-combine-mean-bit-exact"},
             std::string_view{"static-mode-frequencies-and-zero-flag-read-when-it-starts"},
             std::string_view{"cdp8-loses-and-doubles-loudest-channels-in-its-list-shuffle"},
             std::string_view{"cdp8s-channel-count-which-it-checks-but-never-uses-is-not-a-field"},
             std::string_view{"pairs-cdp8-writes-past-its-window-are-skipped"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCombineMean", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-combine-cross-bit-exact"},
             std::string_view{"live-interpolation-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"a-fixed-interpolation-of-1-copies-the-side-chains-amplitudes-as-cdp8-does"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpCombineCross", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-formants-vocode-bit-exact"},
             std::string_view{"static-envelope-bands-quick-search-frequencies-and-gain-read-when-it-starts"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFormantsVocode", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-morph-morph-bit-exact"},
             std::string_view{"static-mode-times-exponents-and-stagger-read-when-it-starts"},
             std::string_view{"times-from-the-sounds-start-up-to-an-hour-and-a-stagger-up-to-10-s-where-cdp8-checks-its-files-lengths"},
             std::string_view{"cdp8s-last-buffer-written-from-a-stale-count-is-not-reproduced"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMorphMorph", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-morph-bridge-bit-exact"},
             std::string_view{"static-mode-offset-fractions-and-times-read-when-it-starts"},
             std::string_view{"an-end-time-is-required-where-cdp8-reads-to-a-files-end"},
             std::string_view{"an-offset-that-would-start-cdp8s-output-late-is-refused"},
             std::string_view{"times-from-the-sounds-start-up-to-an-hour-and-an-offset-up-to-10-s-where-cdp8-checks-its-files-lengths"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpMorphBridge", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-repitch-transposef-bit-exact"},
             std::string_view{"static-units-envelope-bands-quick-search-and-fuller-flag-read-when-it-starts"},
             std::string_view{"live-transposition-and-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8s-binary-transposition-file-is-a-live-ratio"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpRepitchTransposef", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-formants-get-then-put-bit-exact"},
             std::string_view{"static-envelope-bands-quick-searches-mode-frequencies-and-gain-read-when-it-starts"},
             std::string_view{"cdp8-delays-the-input-by-frames-it-reads-from-an-unread-buffer-as-silence"},
             std::string_view{"the-delay-a-long-side-file-gives-where-cdp8-shortens-it-for-a-short-one"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFormantsPut", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-vowels-bit-exact"},
             std::string_view{"static-vowels-half-width-steepness-range-and-threshold-read-when-it-starts"},
             std::string_view{"vowel-times-from-the-sounds-start"},
             std::string_view{"non-finite-vowel-times-cdp8-takes-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteVowels", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-blur-blur-bit-exact"},
             std::string_view{"live-blur-read-at-each-kept-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"waits-the-maximum-blurs-frames-for-the-next-kept-frame"},
             std::string_view{"cdp8-shortens-its-last-blur-at-its-files-end-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpBlurBlur", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-hilite-bltr-bit-exact"},
             std::string_view{"live-blur-and-trace-read-as-cdp8-reads-breakpoints"},
             std::string_view{"waits-the-maximum-blurs-frames-for-the-next-kept-frame"},
             std::string_view{"cdp8-shortens-its-last-blur-and-leaves-its-last-frame-untraced-at-its-files-end-the-stream-plays-on"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpHiliteBlurTrace", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-focus-bit-exact"},
             std::string_view{"static-envelope-bands-quick-search-peaks-and-stability-read-when-it-starts"},
             std::string_view{"live-bandwidth-and-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"stable-peaks-wait-their-stability-frames"},
             std::string_view{"cdp8-takes-a-stability-of-9-as-no-stability"},
             std::string_view{"cdp8-filters-its-last-frames-with-its-last-filter-at-its-files-end-the-stream-plays-on"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusFocus", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-tune-bit-exact"},
             std::string_view{"static-units-template-and-whether-trace-and-bass-cut-are-on-read-when-it-starts"},
             std::string_view{"live-focus-clarity-trace-and-bass-cut-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8-takes-a-static-trace-of-16-and-bass-cut-of-10-hz-as-none"},
             std::string_view{"cdp8s-trace-tunes-the-even-channels-whose-halves-are-loudest"},
             std::string_view{"pitches-under-half-the-sample-rate-over-2-to-the-24-are-refused-where-cdp8-hangs"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchTune", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-pick-bit-exact"},
             std::string_view{"static-mode-fundamental-and-step-read-when-it-starts"},
             std::string_view{"live-clarity-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchPick", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-chord-bit-exact"},
             std::string_view{"static-transpositions-and-less-body-flag-read-when-it-starts"},
             std::string_view{"live-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8s-bottom-and-top-flags-act-crossed"},
             std::string_view{"cdp8s-x-keeps-less-of-the-body-than-its-usage-says"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchChord", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-chordf-bit-exact"},
             std::string_view{"static-envelope-bands-quick-search-transpositions-and-less-body-flag-read-when-it-starts"},
             std::string_view{"live-frequencies-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"cdp8s-bottom-and-top-flags-act-crossed"},
             std::string_view{"band-counts-where-cdp8-writes-past-its-envelope-arrays-are-refused"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchChordf", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-altharms-bit-exact"},
             std::string_view{"pitch-found-over-the-whole-pitch-source-sample-when-it-starts"},
             std::string_view{"bit-exact-when-the-input-plays-the-pitch-source-from-its-start"},
             std::string_view{"frames-past-the-pitch-source-play-unpitched"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchAltharms", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-pitch-octmove-bit-exact"},
             std::string_view{"pitch-found-over-the-whole-pitch-source-sample-when-it-starts"},
             std::string_view{"bit-exact-when-the-input-plays-the-pitch-source-from-its-start"},
             std::string_view{"live-bass-boost-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
             std::string_view{"frames-past-the-pitch-source-play-unpitched"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpPitchOctmove", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-spec-bare-bit-exact"},
             std::string_view{"pitch-found-over-the-whole-pitch-source-sample-when-it-starts"},
             std::string_view{"bit-exact-when-the-input-plays-the-pitch-source-from-its-start"},
             std::string_view{"cdp8-reads-frame-k-from-the-pitch-files-window-k-minus-1"},
             std::string_view{"frames-past-the-pitch-source-play-unpitched"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpecBare", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-spec-clean-bit-exact"},
             std::string_view{"noise-and-good-signal-samples-analysed-whole-when-it-starts"},
             std::string_view{"skip-time-up-to-an-hour-where-cdp8-takes-up-to-its-files-last-window"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpecClean", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-strange-invert-bit-exact"},
             std::string_view{"loudest-amplitudes-read-over-the-whole-normalising-source-when-it-starts"},
             std::string_view{"bit-exact-when-the-input-plays-the-normalising-source-from-its-start"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpStrangeInvert", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-focus-freeze-bit-exact"},
             std::string_view{"waits-for-the-longest-before-segment-of-its-times-at-most-1024-windows"},
             std::string_view{"a-longer-before-segment-is-cut-its-earlier-windows-pass"},
             std::string_view{"cdp8-misreads-a-freeze-on-a-multiple-of-its-20-window-buffer-the-stream-reads-the-window"},
             std::string_view{"cdp8-drops-some-freezes-between-unfrozen-segments-as-the-stream-does"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpFocusFreeze", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-envel-warp-6-timestretch-bit-exact-for-a-stretch-of-1-or-more"},
             std::string_view{"latency-two-windows-less-a-frame"},
             std::string_view{"exact-while-the-stretch-reads-back-within-its-maximum-lag-then-the-oldest-window-kept"},
             std::string_view{"clips-at-plus-or-minus-1-where-cdp8-would-rescale-its-envelope"},
             std::string_view{"a-stretch-below-1-reads-ahead-without-bound-and-is-not-ported"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpEnvelopeTimestretch", std::string{limitation});
    }
    for (const auto limitation : {
             std::string_view{"cdp8-spec-gate-bit-exact"},
             std::string_view{"live-threshold-read-at-each-analysis-frame-as-cdp8-reads-breakpoints"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpSpectralGate", std::string{limitation});
    }
    registry.setNodeTypeProcessingContract(
        "talas.cdpVaryingBank",
        NodeStateModel::SessionStateful,
        true,
        {LatencyKind::None, 0});
    for (const auto limitation : {
             std::string_view{"cdp8-filter-varibank-bit-exact"},
             std::string_view{"static-bands-gain-harmonics-and-rolloff-read-when-it-starts"},
             std::string_view{"live-q-read-each-128-frame-block-as-cdp8-reads-breakpoints"},
             std::string_view{"bands-glide-from-the-start-of-the-stream-and-hold-after-the-last-line"},
             std::string_view{"cdp8-normalising-and-tail-time-have-no-counterpart"},
         })
    {
        registry.addNodeTypeLimitation(
            "talas.cdpVaryingBank", std::string{limitation});
    }

}

void registerBuiltinNodeTypes(NodeRegistry& registry)
{
    registerBuiltinNodeDescriptorsPart1(registry);
    registerBuiltinNodeDescriptorsPart2(registry);
    registerBuiltinNodeDescriptorsPart3(registry);
    registerBuiltinNodeDescriptorsPart4(registry);
    registerBuiltinNodeDescriptorsPart5(registry);
    registerBuiltinNodeMetadata(registry);
}

NodeRegistry makeBuiltinNodeRegistry()
{
    NodeRegistry registry;
    registerBuiltinNodeTypes(registry);
    return registry;
}
}
