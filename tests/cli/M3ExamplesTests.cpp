#include "talas/cli/Cli.h"
#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"
#include "JsonValue.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef TALAS_SOURCE_DIR
#error "TALAS_SOURCE_DIR must identify the repository root"
#endif

namespace
{
struct CliResult
{
    int exitCode = -1;
    std::string output;
    std::string error;
};

struct RenderedChannel
{
    std::string name;
    std::vector<double> samples;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(
    std::string_view haystack,
    std::string_view needle,
    const std::string& message)
{
    require(
        haystack.find(needle) != std::string_view::npos,
        message + " (missing: " + std::string(needle) + ")");
}

// Canonical project text spreads a connection over several lines, so look
// for it in the parsed graph.
void requireConnection(
    std::string_view source,
    const talas::core::Connection& connection,
    const std::string& message)
{
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok()
            && std::ranges::find(parsed.document.graph.connections, connection)
                != parsed.document.graph.connections.end(),
        message);
}

void requireNotContains(
    std::string_view haystack,
    std::string_view needle,
    const std::string& message)
{
    require(
        haystack.find(needle) == std::string_view::npos,
        message + " (unexpected: " + std::string(needle) + ")");
}

std::size_t countOccurrences(std::string_view text, std::string_view needle)
{
    require(!needle.empty(), "occurrence needle should not be empty");
    std::size_t count = 0;
    for (auto offset = text.find(needle);
         offset != std::string_view::npos;
         offset = text.find(needle, offset + needle.size()))
    {
        ++count;
    }
    return count;
}

std::filesystem::path examplePath(std::string_view filename)
{
    return std::filesystem::path{TALAS_SOURCE_DIR} / "examples" / filename;
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.good(), "example should be readable: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

CliResult runCli(std::vector<std::string> arguments)
{
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);

    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = talas::cli::runCli(views, output, error);
    return {exitCode, output.str(), error.str()};
}

void requireCurrentFormat(const std::filesystem::path& path, int expectedVersion = 10)
{
    const auto text = readText(path);
    requireContains(
        text,
        "\"version\": " + std::to_string(expectedVersion),
        "example should be checked in as format " + std::to_string(expectedVersion)
            + ": " + path.string());
    for (const auto requiredCollection : {
             std::string_view{R"json("virtualControls")json"},
             std::string_view{R"json("assets")json"},
             std::string_view{R"json("timeline")json"},
             std::string_view{R"json("deviceSettings")json"},
             std::string_view{R"json("soundDefinitions")json"},
         })
    {
        requireContains(
            text,
            requiredCollection,
            "M3 example should spell out every current-format collection: " + path.string());
    }

    const auto loaded = talas::project::parseProjectDocument(text);
    require(loaded.ok(), "M3 example should parse: " + path.string());
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
        "M3 format-10 source should migrate to current format 36 after parsing: " + path.string());
}

void requireOrdinaryValidation(const std::filesystem::path& path)
{
    const auto result = runCli({"validate", "--project", path.string()});
    require(
        result.exitCode == 0,
        "ordinary validation should succeed for " + path.string() + ": " + result.error);
    require(result.output == "OK\n", "ordinary validation should remain exactly OK for " + path.string());
    require(result.error.empty(), "ordinary validation should not write errors for " + path.string());
}

CliResult statusFor(const std::filesystem::path& path)
{
    auto result = runCli({
        "validate",
        "--project",
        path.string(),
        "--status",
        "--sample-rate",
        "8192",
        "--max-block-frames",
        "128",
    });
    require(
        result.exitCode == 0,
        "prepared status should succeed for " + path.string() + ": " + result.error);
    require(result.error.empty(), "prepared status should not write errors for " + path.string());
    requireContains(result.output, "OK\nPrepared graph status\n", "status should preserve ordinary validation output");
    requireContains(result.output, "sample-rate: 8192 Hz", "status should use the example sample rate");
    requireContains(result.output, "max-block: 128 frames", "status should use the bounded block size");
    return result;
}

std::vector<RenderedChannel> parseRenderedChannels(
    const std::string& json,
    std::size_t expectedFrames,
    double expectedSampleRate = 8192.0)
{
    const auto parsed = talas::project::json::parse(json);
    require(
        parsed.ok(),
        "render output should be valid JSON"
            + (parsed.errors.empty()
                   ? std::string{}
                   : ": " + parsed.errors.front().message));
    require(parsed.value.isObject(), "render output root should be an object");
    require(parsed.value.object().size() == 5, "render output root should have exactly five members");

    const auto* sampleRate = talas::project::json::findObjectMember(parsed.value, "sampleRate");
    const auto* frames = talas::project::json::findObjectMember(parsed.value, "frames");
    const auto* channelsValue = talas::project::json::findObjectMember(parsed.value, "channels");
    const auto* midiEvents = talas::project::json::findObjectMember(parsed.value, "midiEvents");
    const auto* globalControllerEvents =
        talas::project::json::findObjectMember(parsed.value, "globalControllerEvents");
    require(sampleRate != nullptr && sampleRate->isNumber(), "render sampleRate should be numeric");
    require(
        sampleRate->number() == expectedSampleRate,
        "render sampleRate should match the requested rate");
    require(frames != nullptr && frames->isNumber(), "render frames should be numeric");
    require(
        frames->number() == static_cast<double>(expectedFrames),
        "render frames should match the requested frame count");
    require(channelsValue != nullptr && channelsValue->isArray(), "render channels should be an array");
    require(midiEvents != nullptr && midiEvents->isArray(), "render MIDI events should be an array");
    require(
        globalControllerEvents != nullptr && globalControllerEvents->isArray(),
        "render global controller events should be an array");

    std::vector<RenderedChannel> channels;
    channels.reserve(channelsValue->array().size());
    for (const auto& channelValue : channelsValue->array())
    {
        require(channelValue.isObject(), "rendered channel should be an object");
        require(channelValue.object().size() == 2, "rendered channel should have exactly name and samples");
        const auto* name = talas::project::json::findObjectMember(channelValue, "name");
        const auto* samples = talas::project::json::findObjectMember(channelValue, "samples");
        require(name != nullptr && name->isString(), "rendered channel should have a string name");
        require(samples != nullptr && samples->isArray(), "rendered channel should have a sample array");
        require(
            samples->array().size() == expectedFrames,
            "rendered channel should have the requested frame count");

        RenderedChannel channel;
        channel.name = name->string();
        channel.samples.reserve(samples->array().size());
        for (const auto& sample : samples->array())
        {
            require(sample.isNumber(), "every rendered sample should be numeric");
            require(std::isfinite(sample.number()), "every rendered sample should be finite");
            channel.samples.push_back(sample.number());
        }
        channels.push_back(std::move(channel));
    }
    return channels;
}

void testRenderedChannelsRejectMalformedJson()
{
    constexpr std::string_view malformed =
        R"json({"sampleRate": 8192, "frames": 2, "channels": [{"name": "left", "samples": [1 2]}], "midiEvents": [], "globalControllerEvents": []})json";
    bool rejected = false;
    try
    {
        static_cast<void>(parseRenderedChannels(std::string{malformed}, 2));
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    require(rejected, "render parser should reject malformed JSON sample arrays");
}

std::vector<RenderedChannel> render(
    const std::filesystem::path& path,
    std::size_t frames)
{
    const auto result = runCli({
        "render",
        "--project",
        path.string(),
        "--frames",
        std::to_string(frames),
        "--sample-rate",
        "8192",
        "--max-block-frames",
        "128",
        "--format",
        "json",
    });
    require(
        result.exitCode == 0,
        "render should succeed for " + path.string() + ": " + result.error);
    require(result.error.empty(), "render should not write errors for " + path.string());
    auto channels = parseRenderedChannels(result.output, frames);
    require(!channels.empty(), "render should expose at least one channel for " + path.string());
    for (const auto& channel : channels)
    {
        require(channel.samples.size() == frames, "rendered channel should have the requested frame count");
        require(
            std::all_of(channel.samples.begin(), channel.samples.end(), [](double sample) {
                return std::isfinite(sample);
            }),
            "every rendered sample should be finite");
    }
    return channels;
}

void requireAudibleAfter(
    const RenderedChannel& channel,
    std::size_t startupFrames,
    const std::string& message)
{
    require(startupFrames < channel.samples.size(), message + " should have post-startup samples");
    require(
        std::any_of(
            channel.samples.begin() + static_cast<std::ptrdiff_t>(startupFrames),
            channel.samples.end(),
            [](double sample) { return std::abs(sample) > 1.0e-9; }),
        message + " should be non-silent after startup");
}

void requireOscillatorSignature(
    const RenderedChannel& channel,
    double frequency,
    double amplitude)
{
    constexpr double sampleRate = 8192.0;
    for (std::size_t frame = 0; frame < channel.samples.size(); ++frame)
    {
        const auto expected = amplitude
            * std::sin(
                2.0 * std::numbers::pi * frequency
                * static_cast<double>(frame) / sampleRate);
        require(
            std::abs(channel.samples[frame] - expected) <= 1.0e-12,
            channel.name + " should retain its Matrix4 oscillator signature at frame "
                + std::to_string(frame));
    }
}

void testNativeFftRoundTrip()
{
    const auto path = examplePath("m3_native_fft_round_trip.talas.json");
    requireCurrentFormat(path);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: mono (1)", "native FFT example should have one logical output");
    requireContains(status, "terminal-output: 63 frames", "native FFT terminal latency should be exact");
    requireContains(status, "spectral-nodes: 2", "native FFT example should expose both transforms");
    requireContains(
        status,
        "type=talas.forwardFftV1 format=ComplexSpectrumV1 role=forward N=64 H=16 window=hann cadence=512 Hz intrinsic=63 frames cumulative-in=0 frames cumulative-out=63 frames",
        "native forward status should expose exact ComplexSpectrum timing");
    requireContains(
        status,
        "type=talas.inverseFftV1 format=ComplexSpectrumV1 role=inverse N=64 H=16 window=hann cadence=512 Hz intrinsic=0 frames cumulative-in=63 frames cumulative-out=63 frames",
        "native inverse status should preserve the forward latency authority");
    requireContains(status, "limitation=native-stft-radix2-16-4096", "native FFT example should expose its reviewed frame-size boundary");
    requireContains(status, "limitation=native-stft-hann-cola-hops", "native FFT example should expose its Hann COLA boundary");
    requireNotContains(status, "format=PackedRealAudioV1", "native FFT example should never masquerade as packed Audio");

    const auto channels = render(path, 512);
    require(channels.size() == 1 && channels.front().name == "left", "native FFT render should be mono");
    requireAudibleAfter(channels.front(), 63, "native FFT round-trip");
}

void testPackedFftRoundTrip()
{
    const auto path = examplePath("m3_packed_fft_round_trip.talas.json");
    requireCurrentFormat(path);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: mono (1)", "packed FFT example should have one logical output");
    requireContains(status, "terminal-output: 126 frames", "packed FFT terminal latency should include both adapters");
    requireContains(status, "spectral-nodes: 2", "packed FFT example should expose both adapters");
    requireContains(
        status,
        "format=PackedRealAudioV1 role=forward N=64 H=64 window=rectangular cadence=128 Hz intrinsic=63 frames cumulative-in=0 frames cumulative-out=63 frames",
        "packed forward status should expose its historical frame layout");
    requireContains(
        status,
        "format=PackedRealAudioV1 role=inverse N=64 H=64 window=rectangular cadence=128 Hz intrinsic=63 frames cumulative-in=63 frames cumulative-out=126 frames",
        "packed inverse status should expose independent latency");
    requireContains(status, "limitation=packed-real-audio-compatibility-layout", "packed FFT example should be explicitly compatibility-labeled");
    requireNotContains(status, "format=ComplexSpectrumV1", "packed FFT example should never masquerade as native complex frames");

    const auto channels = render(path, 512);
    require(channels.size() == 1 && channels.front().name == "left", "packed FFT render should be mono");
    requireAudibleAfter(channels.front(), 126, "packed FFT round-trip");
}

void testStreamingHealth()
{
    constexpr std::string_view acceptedHash =
        "db335f904b35a742ab63a0fd1cb0a81a79c169d3855ef0623e54c731eff00d89";
    const auto path = examplePath("m3_streaming_health.talas.json");
    requireCurrentFormat(path);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "resources: 1", "streaming example should expose one immutable resource");
    requireContains(
        status,
        std::string{"Audio/M3Stream kind=audio generation=1 hash="} + std::string{acceptedHash}
            + " health=verified channels=1 sample-rate=8192 frames=2048 encoding=pcm16-wav",
        "streaming example should expose the accepted Audio manifest");
    requireContains(status, "stream-contracts: 2", "streaming example should expose player and cache contracts");
    requireContains(status, "type=talas.diskPlayer resource=Audio/M3Stream role=playback live-state=not-running", "DiskPlayer should expose playback status");
    requireContains(status, "type=talas.diskCache resource=Audio/M3Stream role=cache-prewarm live-state=not-running", "DiskCache should expose cache-prewarm status");
    requireContains(status, "limitation=stream-mono-or-stereo-wav-or-aiff-only", "streaming example should expose its encoding boundary");
    requireContains(status, "limitation=disk-cache-read-prewarm-only", "streaming example should expose its read-only cache boundary");

    const auto channels = render(path, 512);
    require(channels.size() == 1 && channels.front().name == "left", "streaming render should be mono");
    requireAudibleAfter(channels.front(), 0, "streamed DiskPlayer/Cache path");
}

void testMatrix4Output4()
{
    const auto path = examplePath("m3_matrix4_output4.talas.json");
    requireCurrentFormat(path);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: discrete (4)", "Matrix4 example should expose a four-channel discrete layout");
    for (std::size_t ordinal = 1; ordinal <= 4; ++ordinal)
    {
        requireContains(
            status,
            "    " + std::to_string(ordinal) + ": matrix.out" + std::to_string(ordinal)
                + " [matrix] scope=0 device=not-attached\n",
            "Matrix4 status should preserve the complete unattached logical output route");
    }
    require(
        countOccurrences(status, "device=not-attached") == 4,
        "Matrix4 CLI status should expose exactly four unattached routes");

    const auto channels = render(path, 128);
    require(channels.size() == 4, "Matrix4 render should expose exactly four channels");
    constexpr std::array frequencies = {64.0, 128.0, 256.0, 512.0};
    constexpr std::array amplitudes = {0.10, 0.15, 0.20, 0.25};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        require(
            channels[channel].name == "ch" + std::to_string(channel + 1),
            "Matrix4 render should use stable ch1-ch4 names");
        requireAudibleAfter(channels[channel], 0, "Matrix4 output channel");
        requireOscillatorSignature(
            channels[channel],
            frequencies[channel],
            amplitudes[channel]);
    }
}

void testLiveSpectralAnalysis()
{
    const auto path = examplePath("m3_live_spectral_analysis.talas.json");
    requireCurrentFormat(path, 43);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: mono (1)", "live spectral example should retain its audible direct branch");
    requireContains(status, "spectral-nodes: 2", "live spectral example should expose native and compatibility records");
    requireContains(
        status,
        "type=talas.liveSpectralAnalysis format=PartialSpectrumV1 role=analysis N=16 H=4 window=hann cadence=2048 Hz intrinsic=15 frames cumulative-in=0 frames cumulative-out=15 frames",
        "LiveSpectralAnalysis should expose exact native partial-frame timing");
    requireContains(
        status,
        "type=talas.partialSpectrumLegacyHoldV1 format=LegacyPartialTriples role=compatibility-hold N=16 H=4 window=hann cadence=2048 Hz intrinsic=0 frames cumulative-in=15 frames cumulative-out=15 frames",
        "legacy hold should remain a distinct compatibility boundary");
    requireContains(status, "limitation=live-spectral-defined-partial-subset", "live spectral example should expose its bounded subset");
    require(status.find("legacy-hold-collapses-process-interval") == std::string::npos,
        "legacy hold should no longer expose a collapse limitation (ADR-0016)");

    const auto channels = render(path, 128);
    require(channels.size() == 1 && channels.front().name == "left", "live spectral example should render mono");
    requireAudibleAfter(channels.front(), 0, "live spectral direct Audio branch");
}

void testBoundedMidiMapperModesStatus()
{
    const auto path = examplePath("c10_midi_mapper_modes.talas.json");
    requireCurrentFormat(path, 43);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "midi-mappers: 2",
        "bounded MIDI Mapper example should expose both prepared mapper contracts");
    requireContains(
        status,
        "gateMapper [gateMapper] scope=0 mode=gate channel=any pitch=0-127 polyphony=4 input=scheduled-midi-only",
        "gate mapper status should expose its exact compiled authority");
    requireContains(
        status,
        "velocityMapper [velocityMapper] scope=0 mode=velocity channel=any pitch=0-127 polyphony=4 input=scheduled-midi-only",
        "velocity mapper status should expose its exact compiled authority");
    requireContains(
        status,
        "deferred=source,midiFile,script,shared",
        "mapped MIDI status should retain the reviewed deferred boundary");
}

void testPolyphonicMidiMapperInputExampleStatus()
{
    const auto path = examplePath("c200_polyphonic_midi_mapper_input.talas.json");
    requireCurrentFormat(path, 38);
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "midi-mappers: 1",
        "polyphonic Mapper example should expose one Mapper owner");
    requireContains(status,
        "mapper [mapper] scope=0 mode=polyphonic-input channel=1 pitch=0-127 polyphony=2 input=scheduled-midi-only",
        "new Mapper example should expose MIDI-triggered Input voice authority");
    requireContains(status, "step osc [osc] scope=1 type=talas.oscillator",
        "first Input copy should have its own prepared step");
    requireContains(status, "step osc [osc] scope=2 type=talas.oscillator",
        "second Input copy should have its own prepared step");
    const auto channels = render(path, 16);
    require(channels.size() == 2
            && std::ranges::all_of(channels,
                [](const auto& channel) {
                    return std::ranges::all_of(channel.samples,
                        [](double sample) { return sample == 0.0; });
                }),
        "polyphonic Mapper example must remain silent before its first note");
}

void testBoundedMidiMapperModesExecuteOfflineAndRealtime()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frameCount = 16;
    const auto parsed = talas::project::parseProjectDocument(
        readText(examplePath("c10_midi_mapper_modes.talas.json")));
    require(parsed.ok(), "bounded MIDI Mapper example should parse for execution");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = frameCount;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto compiled = talas::runtime::compileProject(
        parsed.document,
        registry,
        settings);
    require(
        compiled.ok(),
        "bounded MIDI Mapper example should compile for execution");
    require(
        compiled.graph->plan().midiMappers.size() == 2
            && compiled.graph->plan().midiMappers[0].configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Gate
            && compiled.graph->plan().midiMappers[1].configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Velocity,
        "example execution should bind one gate and one velocity mapper plan");

    const std::array offlineEvents{
        talas::runtime::OfflineMidiVoiceEvent{
            4,
            1,
            1,
            talas::runtime::OfflineMidiVoiceEventKind::NoteOn,
            1,
            60,
            64,
        },
        talas::runtime::OfflineMidiVoiceEvent{
            12,
            2,
            2,
            talas::runtime::OfflineMidiVoiceEventKind::NoteOff,
            1,
            60,
            0,
        },
    };
    talas::runtime::RealtimeRenderSession readinessProbe(*compiled.graph);
    require(
        readinessProbe.ready(),
        "prepared-realtime example should be ready: "
            + std::string{readinessProbe.error()});
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    require(
        offline.scheduleMidiVoiceEvents(offlineEvents),
        "offline example should accept its bounded MIDI stream");
    const auto offlineResult = offline.render(frameCount);
    require(
        offlineResult.ok() && offlineResult.mono.size() == frameCount,
        "offline example should render the complete scheduled stream");

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(), "prepared-realtime example should be ready");
    for (const auto& event : offlineEvents)
    {
        const auto dispatch = realtime.scheduleMidiNote(
            {
                event.kind == talas::runtime::OfflineMidiVoiceEventKind::NoteOn
                    ? talas::runtime::RealtimeMidiNoteKind::NoteOn
                    : talas::runtime::RealtimeMidiNoteKind::NoteOff,
                event.channel,
                event.note,
                event.velocity,
            },
            static_cast<std::size_t>(event.frame));
        require(
            dispatch == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "prepared-realtime example should handle the same MIDI stream");
    }
    std::array<double, frameCount> realtimeOutput{};
    require(
        realtime.render(realtimeOutput),
        "prepared-realtime example should render the complete scheduled stream");

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        require(
            std::abs(offlineResult.mono[frame] - realtimeOutput[frame])
                <= 1.0e-12,
            "offline and prepared-realtime example samples should be exact");
        if (frame < 4 || frame >= 12)
        {
            require(
                std::abs(realtimeOutput[frame]) <= 1.0e-12,
                "example should be closed outside the scheduled note interval");
        }
    }

    constexpr std::size_t evidenceFrame = 7;
    const auto gateSample = 0.25
        * std::sin(
            2.0 * std::numbers::pi * 110.0
            * static_cast<double>(evidenceFrame) / sampleRate);
    const auto velocitySample = 0.25 * (64.0 / 127.0)
        * std::sin(
            2.0 * std::numbers::pi * 220.0
            * static_cast<double>(evidenceFrame) / sampleRate);
    require(
        std::abs(
            realtimeOutput[evidenceFrame]
            - (gateSample + velocitySample)) <= 1.0e-12,
        "example should audibly combine full gate and normalized velocity modes");
}

void testBoundedMidiMapperExampleRejectsFirstOverBoundJsonMap()
{
    auto oversized = readText(
        examplePath("c10_midi_mapper_modes.talas.json"));
    const auto map = oversized.find(R"json("text": "gate")json");
    require(
        map != std::string::npos,
        "bounded MIDI Mapper JSON fixture should expose its gate token");
    oversized.replace(map, std::string_view{R"json("text": "gate")json"}.size(),
        std::string{R"json("text": ")json"}
            + std::string(talas::runtime::maximumPreparedMidiMapperMapBytes + 1, 'g')
            + R"json(")json");

    const auto parsed = talas::project::parseProjectDocument(oversized);
    require(
        parsed.ok(),
        "generic JSON should retain first-over-bound mapper text until compiled authority validates it");
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document,
        registry,
        {});
    require(
        !compiled.ok()
            && std::ranges::any_of(
                compiled.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.message.find(
                               "MIDIMapper map exceeds the "
                               + std::to_string(talas::runtime::maximumPreparedMidiMapperMapBytes)
                               + "-byte prepared limit")
                        != std::string::npos;
                }),
        "first-over-bound JSON map should fail at the shared prepared limit");
}

void testBoundedDiskCacheCaptureExample()
{
    const auto path = examplePath("c11_disk_cache_capture.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok(), "bounded Disk Cache example should parse");
    require(
        parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id == "Audio/C11DiskCache"
            && parsed.document.assets.front().generation == 1
            && parsed.document.assets.front().contentHash
                == "2b0ecfcfa9be0a0376fdc990c429f56e79ad793671c287bb248d52a9fe7caa5b"
            && talas::project::diskCacheAssetAuthority(
                   parsed.document.assets.front())
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
        "bounded Disk Cache example should retain exact current captured authority");
    const auto wavePath = path.parent_path()
        / talas::project::pathFromUtf8(parsed.document.assets.front().path);
    const auto wave = talas::audio::readPcm16Wav(wavePath);
    require(
        wave.ok() && wave.sampleRate == 8000
            && wave.channels.size() == 1
            && wave.channels.front().size() == 32,
        "bounded Disk Cache example should retain the declared 32-frame PCM16 WAV");

    const auto result = runCli({
        "render",
        "--project",
        path.string(),
        "--frames",
        "40",
        "--sample-rate",
        "8000",
        "--max-block-frames",
        "7",
        "--format",
        "json",
    });
    require(
        result.exitCode == 0 && result.error.empty(),
        "bounded Disk Cache example should render: " + result.error);
    const auto channels = parseRenderedChannels(result.output, 40, 8000.0);
    require(
        channels.size() == 1 && channels.front().name == "left",
        "bounded Disk Cache example should render one mono channel");
    for (std::size_t frame = 0; frame < 32; ++frame)
    {
        const auto expected = std::sin(
            2.0 * std::numbers::pi * 1000.0
            * static_cast<double>(frame) / 8000.0);
        require(
            std::abs(channels.front().samples[frame] - expected)
                <= 1.0 / 32767.0 + 1.0e-9,
            "bounded Disk Cache example should replay exact captured PCM16 samples");
    }
    require(
        std::ranges::all_of(
            channels.front().samples.begin() + 32,
            channels.front().samples.end(),
            [](double sample) { return sample == 0.0; }),
        "bounded Disk Cache example should zero-fill after immutable EOF");
}

void testBoundedDiskRecorderExamplePublishesExactPreparedProfile()
{
    const auto path = examplePath(
        "c13_bounded_disk_recorder.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok(), "bounded Disk Recorder example should parse");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = 7;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(compiled.ok(),
        "bounded Disk Recorder example should compile");
    const auto summary = talas::runtime::makePreparedGraphSummary(
        *compiled.graph);
    require(
        summary.ok()
            && summary.summary->diskRecorderCapture.has_value()
            && summary.summary->diskRecorderCapture->nodeId
                == "recorder"
            && summary.summary->diskRecorderCapture->writeSupport
                == talas::runtime::DiskRecorderWriteSupport::Writable
            && summary.summary->diskRecorderCapture
                    ->destinationNameHint
                == "Recordings/C13 Bounded Take"
            && summary.summary->diskRecorderCapture
                    ->captureDurationSeconds
                == 0.02,
        "bounded Disk Recorder example should publish exact writable authority");

    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const auto expected = offline.render(200);
    require(expected.ok() && expected.mono.size() == 200,
        "bounded Disk Recorder example should render offline preview");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(),
        "bounded Disk Recorder example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual;
    actual.reserve(200);
    while (actual.size() < 200)
    {
        const auto frames = std::min<std::size_t>(
            7, 200 - actual.size());
        std::vector<double> block(frames);
        require(realtime.render(block),
            "bounded Disk Recorder partition should render");
        actual.insert(actual.end(), block.begin(), block.end());
    }
    require(actual == expected.mono,
        "bounded Disk Recorder prepared partitions should be sample-identical offline");

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8000", "--max-block-frames", "7",
    });
    require(
        status.exitCode == 0 && status.error.empty()
            && status.output.find(
                   "disk-recorder: writable node=recorder")
                != std::string::npos
            && status.output.find("owner-action=required path-authority=none")
                != std::string::npos,
        "bounded Disk Recorder CLI status should publish explicit local authority truth");
}

void testBoundedAnalogSequencerExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 960;
    const auto path = examplePath(
        "c14_bounded_analog_sequencer.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok(), "bounded Analog Sequencer example should parse");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = 7;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(compiled.ok(),
        "bounded Analog Sequencer example should compile");

    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const auto expected = offline.render(frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount
            && expected.right.size() == frameCount,
        "bounded Analog Sequencer example should render stereo offline");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(),
        "bounded Analog Sequencer example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actualLeft(frameCount);
    std::vector<double> actualRight(frameCount);
    for (std::size_t offset = 0; offset < frameCount;)
    {
        const auto frames = std::min<std::size_t>(
            7, frameCount - offset);
        require(
            realtime.render(
                std::span<double>{actualLeft}.subspan(offset, frames),
                std::span<double>{actualRight}.subspan(offset, frames)),
            "bounded Analog Sequencer prepared partition should render");
        offset += frames;
    }
    require(
        actualLeft == expected.mono && actualRight == expected.right,
        "bounded Analog Sequencer prepared partitions should be sample-identical offline");
    require(
        std::ranges::any_of(actualLeft, [](double sample) {
            return sample != 0.0;
        })
            && actualLeft != actualRight,
        "bounded Analog Sequencer example should produce audible distinct stereo output");

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8000", "--max-block-frames", "7",
    });
    require(
        status.exitCode == 0 && status.error.empty()
            && status.output.find("outputs: stereo (2)")
                != std::string::npos
            && status.output.find("type=talas.analogSequencer")
                != std::string::npos,
        "bounded Analog Sequencer CLI status should publish its stereo prepared graph");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "960",
        "--sample-rate", "8000", "--max-block-frames", "7",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "bounded Analog Sequencer CLI render should succeed: "
            + rendered.error);
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8000.0);
    require(
        channels.size() == 2 && channels[0].name == "left"
            && channels[1].name == "right"
            && channels[0].samples == expected.mono
            && channels[1].samples == expected.right,
        "bounded Analog Sequencer CLI render should match the exact offline stereo oracle");
}

void testBoundedSyntheticSpectrumFromSoundsExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c29_bounded_synthetic_spectrum_from_sounds.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C29 historical project should migrate to current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C29 product example should expose one audible logical output");
    requireContains(
        status,
        "type=talas.syntheticSpectrumFromSounds",
        "C29 product status should expose the prepared packed-Audio Spectrum source");
    requireContains(
        status,
        "type=talas.oscillatorBank",
        "C29 product status should expose the downstream Spectrum resynthesis path");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C29 product status should expose exact zero-latency ancestry");
    requireContains(
        status,
        "terminal-output: 0 frames",
        "C29 product status should expose exact terminal latency");
    requireContains(
        status,
        "latency-warnings: none",
        "C29 product status should contain no unknown or uncompensated ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(compiled.ok(), "C29 product example should compile for prepared execution");

    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const auto expected = offline.render(frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C29 product example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C29 product example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(
                std::span<double>{actual}.subspan(
                    offset, blockFrames)),
            "C29 product prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::any_of(actual, [](double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C29 product prepared partitions should be bit-identical and audible");

    const auto channels = render(path, frameCount);
    require(
        channels.size() == 1
            && channels.front().name == "left",
        "C29 CLI product render should be mono");
    requireAudibleAfter(
        channels.front(), 0,
        "C29 bounded packed-Audio Spectrum resynthesis");

    talas::runtime::ProjectRuntimeBridge bridge(
        parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C29 product bridge should compile and stage the historical project");
    std::array<float, blockFrames> firstBridgeBlock{};
    std::array<float*, 1> bridgeChannels{firstBridgeBlock.data()};
    require(
        bridge.render(
            bridgeChannels.data(),
            bridgeChannels.size(),
            firstBridgeBlock.size()),
        "C29 product bridge should activate and render its first engine");
    require(
        bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(
                firstBridgeBlock,
                [](float sample) { return std::abs(sample) > 1.0e-9F; }),
        "C29 product bridge should publish one audible active output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C29 product bridge should restage an exact replacement");
    std::array<float, blockFrames> replacementBridgeBlock{};
    bridgeChannels[0] = replacementBridgeBlock.data();
    require(
        bridge.render(
            bridgeChannels.data(),
            bridgeChannels.size(),
            replacementBridgeBlock.size())
            && replacementBridgeBlock == firstBridgeBlock
            && bridge.collectRetired(),
        "C29 replacement should activate at the callback boundary, restart exactly, and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c29-bounded-spectrum-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0
            && exported.error.empty()
            && exported.output.empty(),
        "C29 CLI WAV export should succeed without console payload: "
            + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok()
            && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::any_of(
                wav.channels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C29 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedWarpedTimeIndexExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c30_bounded_warped_time_index.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C30 historical project should migrate to current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C30 product example should expose one audible logical output");
    requireContains(
        status,
        "type=talas.warpedTimeIndex",
        "C30 product status should expose the prepared time-map state machine");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C30 product status should expose zero-latency Control ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C30 product status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C30 product example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C30 product example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C30 product example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C30 product prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::any_of(actual, [](double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C30 product prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C30 product bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [](float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C30 product bridge should publish one audible active output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C30 product bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C30 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c30-warped-time-index-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C30 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::any_of(
                wav.channels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C30 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedSpectrumOnDiskExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c31_bounded_spectrum_on_disk.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C31 project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C31 product example should expose one audible logical output");
    requireContains(
        status,
        "type=talas.spectrumOnDisk",
        "C31 product status should expose the prepared immutable Spectrum source");
    requireContains(
        status,
        "Analysis/C31 kind=table generation=1 hash=a219cf0b589a975025d40f529bf460c82540475f48524632ca18ed34104f7691 health=verified",
        "C31 product status should expose the exact verified table generation");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C31 product status should expose zero-latency Spectrum ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C31 product status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources.push_back({
        "Analysis/C31",
        {0.35, 1.0, 0.0, 0.15, 2.0, 0.0, 0.1, 3.0, 0.0},
    });
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C31 product example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C31 product example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C31 product example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C31 product prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::any_of(actual, [](double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C31 product prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C31 product bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [](float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C31 product bridge should publish one audible active output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C31 product bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C31 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c31-spectrum-on-disk-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C31 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::any_of(
                wav.channels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C31 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedGaOscillatorsExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath("c32_bounded_ga_oscillators.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C32 project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C32 product example should expose one audible logical output");
    requireContains(
        status,
        "type=talas.gaOscillators",
        "C32 product status should expose the prepared GA source");
    requireContains(
        status,
        "Analysis/C32A kind=table generation=1 hash=2b16b0eaac7e79df0230adea1f7ccb8dc430a233ceb5b60b62b618d76dc24ae7 health=verified",
        "C32 product status should expose the exact first table generation");
    requireContains(
        status,
        "Analysis/C32B kind=table generation=1 hash=b6e94e9f13c483a94830697f0bc48bc1cffd1f6f048cde75c0bb6d84f5900bf8 health=verified",
        "C32 product status should expose the exact second table generation");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C32 product status should expose zero-latency GA ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C32 product status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {
        {"Analysis/C32A", {0.35, 0.15, 0.10}},
        {"Analysis/C32B", {0.10, 0.20, 0.35}},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C32 product example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C32 product example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C32 product example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C32 product prepared partition should render");
    }
    const auto mismatch = std::ranges::mismatch(actual, expected.mono);
    const auto exactDouble = [](double value) {
        std::ostringstream stream;
        stream << std::hexfloat << value;
        return stream.str();
    };
    require(
        mismatch.in1 == actual.end()
            && std::ranges::any_of(actual, [](double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C32 product prepared partitions should be bit-identical and audible; first mismatch frame "
            + std::to_string(static_cast<std::size_t>(
                std::distance(actual.begin(), mismatch.in1)))
            + " prepared="
            + (mismatch.in1 == actual.end()
                ? std::string{"<none>"}
                : exactDouble(*mismatch.in1))
            + " offline="
            + (mismatch.in2 == expected.mono.end()
                ? std::string{"<none>"}
                : exactDouble(*mismatch.in2)));

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C32 product bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [](float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C32 product bridge should publish one audible active output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C32 product bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C32 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c32-ga-oscillators-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C32 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::any_of(
                wav.channels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-9; }),
        "C32 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedGenericSourceRamSampleExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c33_bounded_generic_source_ram_sample.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C33 project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: stereo (2)",
        "C33 product example should expose two audible logical outputs");
    requireContains(
        status,
        "type=talas.genericSource",
        "C33 product status should expose the prepared Generic Source");
    requireContains(
        status,
        "Samples/C33Loop kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C33 product status should expose the exact sample generation");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C33 product status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C33 product status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {{
        "Samples/C33Loop",
        {
            0.0, 0.3826834323650898, 0.7071067811865475,
            0.9238795325112867, 1.0, 0.9238795325112867,
            0.7071067811865476, 0.3826834323650899,
            0.0000000000000001, -0.3826834323650897,
            -0.7071067811865475, -0.9238795325112865, -1.0,
            -0.9238795325112866, -0.7071067811865477,
            -0.3826834323650904,
        },
    }};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C33 product example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount
            && expected.right.size() == frameCount,
        "C33 product example should render a complete stereo offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C33 product example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actualLeft(frameCount);
    std::vector<double> actualRight(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(
                std::span<double>{actualLeft}.subspan(
                    offset, blockFrames),
                std::span<double>{actualRight}.subspan(
                    offset, blockFrames)),
            "C33 product prepared partition should render");
    }
    require(
        actualLeft == expected.mono
            && actualRight == expected.right
            && std::ranges::any_of(actualLeft, [](double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C33 product prepared partitions should be bit-identical stereo and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C33 product bridge should compile and stage");
    std::array<float, blockFrames> firstLeft{};
    std::array<float, blockFrames> firstRight{};
    std::array<float*, 2> channels{
        firstLeft.data(), firstRight.data()};
    require(
        bridge.render(channels.data(), channels.size(), blockFrames)
            && bridge.activeOutputChannelCount() == 2
            && std::ranges::any_of(firstLeft, [](float sample) {
                return std::abs(sample) > 1.0e-9F;
            })
            && firstLeft == firstRight,
        "C33 product bridge should publish exact audible stereo output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C33 product bridge should restage an exact replacement");
    std::array<float, blockFrames> replacementLeft{};
    std::array<float, blockFrames> replacementRight{};
    channels = {replacementLeft.data(), replacementRight.data()};
    require(
        bridge.render(channels.data(), channels.size(), blockFrames)
            && replacementLeft == firstLeft
            && replacementRight == firstRight
            && bridge.collectRetired(),
        "C33 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c33-generic-source-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C33 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 2
            && wav.channels[0].size() == frameCount
            && wav.channels[1].size() == frameCount
            && std::ranges::any_of(
                wav.channels[0],
                [](double sample) { return std::abs(sample) > 1.0e-9; })
            && wav.channels[0] == wav.channels[1],
        "C33 exported PCM16 WAV should retain exact finite audible stereo");
}

void testTextBackedSampleExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath("sample_text_table.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C34 Sample project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C34 Sample example should expose one audible output");
    requireContains(
        status,
        "type=talas.sample",
        "C34 product status should expose prepared Sample");
    requireContains(
        status,
        "Samples/Hit kind=table generation=1 hash=ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98 health=verified",
        "C34 product status should expose exact table authority");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C34 Sample status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C34 Sample status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {{"Samples/Hit", {1.0, 0.0, -1.0}}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C34 Sample example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C34 Sample example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C34 Sample example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C34 Sample prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::any_of(actual, [] (double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C34 Sample prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C34 Sample bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [] (float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C34 Sample bridge should publish one audible output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C34 Sample bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C34 Sample replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c34-sample-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C34 Sample CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::any_of(
                wav.channels.front(),
                [] (double sample) { return std::abs(sample) > 1.0e-9; }),
        "C34 Sample exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedMultisampleExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath("c35_bounded_multisample.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C35 Multisample project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C35 Multisample example should expose one audible output");
    requireContains(
        status,
        "type=talas.multisample",
        "C35 product status should expose prepared Multisample");
    requireContains(
        status,
        "Samples/C35Hit kind=table generation=1 hash=ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98 health=verified",
        "C35 status should expose exact first-table authority");
    requireContains(
        status,
        "Samples/C35Loop kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C35 status should expose exact second-table authority");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C35 Multisample status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C35 Multisample status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {
        {"Samples/C35Hit", {1.0, 0.0, -1.0}},
        {"Samples/C35Loop", {
            0.0,
            0.3826834323650898,
            0.7071067811865475,
            0.9238795325112867,
            1.0,
            0.9238795325112867,
            0.7071067811865476,
            0.3826834323650899,
            0.0000000000000001,
            -0.3826834323650897,
            -0.7071067811865475,
            -0.9238795325112865,
            -1.0,
            -0.9238795325112866,
            -0.7071067811865477,
            -0.3826834323650904,
        }},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C35 Multisample example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C35 Multisample example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C35 Multisample example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C35 Multisample prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::all_of(actual, [] (double sample) {
                return std::isfinite(sample);
            })
            && std::ranges::any_of(actual, [] (double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C35 Multisample prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C35 Multisample bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [] (float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C35 Multisample bridge should publish one audible output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C35 Multisample bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C35 Multisample replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c35-multisample-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C35 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::all_of(
                wav.channels.front(),
                [] (double sample) { return std::isfinite(sample); })
            && std::ranges::any_of(
                wav.channels.front(),
                [] (double sample) { return std::abs(sample) > 1.0e-9; }),
        "C35 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedKeyMappedMultisampleExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c36_bounded_key_mapped_multisample.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C36 Key-Mapped Multisample project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C36 example should expose one audible output");
    requireContains(
        status,
        "type=talas.keyMappedMultisample",
        "C36 product status should expose prepared Key-Mapped Multisample");
    requireContains(
        status,
        "Samples/C36Root kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C36 status should expose exact table authority");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C36 status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C36 status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {{
        "Samples/C36Root",
        {
            0.0,
            0.3826834323650898,
            0.7071067811865475,
            0.9238795325112867,
            1.0,
            0.9238795325112867,
            0.7071067811865476,
            0.3826834323650899,
            0.0000000000000001,
            -0.3826834323650897,
            -0.7071067811865475,
            -0.9238795325112865,
            -1.0,
            -0.9238795325112866,
            -0.7071067811865477,
            -0.3826834323650904,
        }}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C36 example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C36 example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C36 example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C36 prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::all_of(actual, [] (double sample) {
                return std::isfinite(sample);
            })
            && std::ranges::any_of(actual, [] (double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C36 prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C36 bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [] (float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C36 bridge should publish one audible output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C36 bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C36 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c36-key-mapped-multisample-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C36 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::all_of(
                wav.channels.front(),
                [] (double sample) { return std::isfinite(sample); })
            && std::ranges::any_of(
                wav.channels.front(),
                [] (double sample) { return std::abs(sample) > 1.0e-9; }),
        "C36 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedMultifileDiskPlayerExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath(
        "c37_bounded_multifile_disk_player.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C37 Multifile Disk Player project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C37 example should expose one audible output");
    requireContains(
        status,
        "type=talas.multifileDiskPlayer",
        "C37 product status should expose prepared Multifile Disk Player");
    requireContains(
        status,
        "Samples/C37Hit kind=table generation=1 hash=ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98 health=verified",
        "C37 status should expose exact first-table authority");
    requireContains(
        status,
        "Samples/C37Wave kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C37 status should expose exact selected-table authority");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C37 status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C37 status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = blockFrames;
    settings.tableResources = {
        {"Samples/C37Hit", {1.0, 0.0, -1.0}},
        {"Samples/C37Wave",
         {
             0.0,
             0.3826834323650898,
             0.7071067811865475,
             0.9238795325112867,
             1.0,
             0.9238795325112867,
             0.7071067811865476,
             0.3826834323650899,
             0.0000000000000001,
             -0.3826834323650897,
             -0.7071067811865475,
             -0.9238795325112865,
             -1.0,
             -0.9238795325112866,
             -0.7071067811865477,
             -0.3826834323650904,
         }},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C37 example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C37 example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C37 example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C37 prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::all_of(actual, [] (double sample) {
                return std::isfinite(sample);
            })
            && std::ranges::any_of(actual, [] (double sample) {
                return std::abs(sample) > 1.0e-9;
            }),
        "C37 prepared partitions should be bit-identical and audible");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C37 bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [] (float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C37 bridge should publish one audible output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C37 bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C37 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c37-multifile-disk-player-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C37 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 8192
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::all_of(
                wav.channels.front(),
                [] (double sample) { return std::isfinite(sample); })
            && std::ranges::any_of(
                wav.channels.front(),
                [] (double sample) { return std::abs(sample) > 1.0e-9; }),
        "C37 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedMemoryWriterExampleMatchesPreparedPartitions()
{
    constexpr std::size_t frameCount = 512;
    constexpr std::size_t blockFrames = 128;
    const auto path = examplePath("c38_bounded_memory_writer.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C38 Memory Writer project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: mono (1)",
        "C38 example should expose one audible output");
    requireContains(
        status,
        "type=talas.memoryWriter",
        "C38 product status should expose prepared Memory Writer");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C38 status should expose zero-latency ancestry");
    requireContains(
        status,
        "latency-warnings: none",
        "C38 status should contain no unknown ancestry");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = blockFrames;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C38 example should compile for prepared execution");
    const auto expected = talas::runtime::renderOffline(
        *compiled.graph, frameCount);
    require(
        expected.ok() && expected.mono.size() == frameCount,
        "C38 example should render a complete offline reference");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "C38 example should prepare realtime: "
            + std::string{realtime.error()});
    std::vector<double> actual(frameCount);
    for (std::size_t offset = 0; offset < frameCount;
         offset += blockFrames)
    {
        require(
            realtime.render(std::span<double>{actual}.subspan(
                offset, blockFrames)),
            "C38 prepared partition should render");
    }
    require(
        actual == expected.mono
            && std::ranges::all_of(actual, [] (double sample) {
                return std::isfinite(sample);
            })
            && std::ranges::any_of(actual, [] (double sample) {
                return std::abs(sample) > 1.0e-9;
            })
            && std::ranges::all_of(
                actual | std::views::drop(240),
                [] (double sample) { return sample == 0.0; }),
        "C38 prepared partitions should be bit-identical, audible, and bounded");

    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings).ok(),
        "C38 bridge should compile and stage");
    std::array<float, blockFrames> first{};
    std::array<float*, 1> channels{first.data()};
    require(
        bridge.render(channels.data(), channels.size(), first.size())
            && bridge.activeOutputChannelCount() == 1
            && std::ranges::any_of(first, [] (float sample) {
                return std::abs(sample) > 1.0e-9F;
            }),
        "C38 bridge should publish one audible output");
    require(
        bridge.compileAndStage(settings).ok(),
        "C38 bridge should restage an exact replacement");
    std::array<float, blockFrames> replacement{};
    channels[0] = replacement.data();
    require(
        bridge.render(
            channels.data(), channels.size(), replacement.size())
            && replacement == first
            && bridge.collectRetired(),
        "C38 replacement should restart exactly and retire the prior engine");

    const auto exportPath = std::filesystem::temp_directory_path()
        / "talas-c38-memory-writer-export.wav";
    std::error_code ignored;
    std::filesystem::remove(exportPath, ignored);
    const auto exported = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "48000",
        "--max-block-frames", std::to_string(blockFrames),
        "--output", exportPath.string(),
    });
    require(
        exported.exitCode == 0 && exported.error.empty()
            && exported.output.empty(),
        "C38 CLI WAV export should succeed: " + exported.error);
    const auto wav = talas::audio::readPcm16Wav(exportPath);
    std::filesystem::remove(exportPath, ignored);
    require(
        wav.ok() && wav.sampleRate == 48000
            && wav.channels.size() == 1
            && wav.channels.front().size() == frameCount
            && std::ranges::all_of(
                wav.channels.front(),
                [] (double sample) { return std::isfinite(sample); })
            && std::ranges::any_of(
                wav.channels.front(),
                [] (double sample) { return std::abs(sample) > 1.0e-9; }),
        "C38 exported PCM16 WAV should retain one finite audible channel");
}

void testBoundedRawMidiOutputExamplePublishesTerminalEvents()
{
    constexpr std::size_t frameCount = 64;
    const auto path = examplePath("c39_bounded_raw_midi_output.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C39 Raw MIDI Output project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: none (0)",
        "C39 example should preserve zero-audio terminal topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.midiOutputEventInBytes",
        "C39 product status should expose the exact prepared terminal step");
    requireContains(
        status,
        "resources: none",
        "C39 example should require no external resource");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "64", "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C39 CLI JSON render should succeed: " + rendered.error);
    require(
        countOccurrences(rendered.output, R"json("frame": 0)json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("bytes": [144, 64, 127])json") == 1,
        "C39 CLI render should publish one exact frame-zero raw MIDI packet");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = frameCount;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    const auto firstStage = bridge.compileAndStage(settings);
    require(
        firstStage.ok() && firstStage.outputChannelCount == 0,
        "C39 bridge example should stage with zero audio outputs");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != 0
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 64
            && events[0].bytes[2] == 0x7f,
        "C39 bridge example should publish one exact stage-bound packet");
    const auto firstStageId = output.stageId;
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 0
            && output.stageId == firstStageId,
        "C39 bridge example should deduplicate an unchanged packet");
    require(
        bridge.compileAndStageDocument(parsed.document, settings).ok(),
        "C39 bridge example should prepare an exact restage");
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != firstStageId
            && bridge.collectRetired(),
        "C39 restage should restart publication with exact new-stage identity");
}

void testBoundedMidiControllerOutputExamplePublishesTerminalEvents()
{
    constexpr std::size_t frameCount = 64;
    const auto path = examplePath(
        "c40_bounded_midi_controller_output.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C40 MIDI Controller project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: none (0)",
        "C40 example should preserve zero-audio terminal topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.midiOutputController",
        "C40 product status should expose the exact prepared terminal step");
    requireContains(
        status,
        "resources: none",
        "C40 example should require no external resource");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "64", "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C40 CLI JSON render should succeed: " + rendered.error);
    require(
        countOccurrences(rendered.output, R"json("frame": 0)json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("bytes": [178, 10, 127])json") == 1,
        "C40 CLI render should publish one exact frame-zero CC packet");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = frameCount;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    const auto firstStage = bridge.compileAndStage(settings);
    require(
        firstStage.ok() && firstStage.outputChannelCount == 0,
        "C40 bridge example should stage with zero audio outputs");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != 0
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0xb2
            && events[0].bytes[1] == 10
            && events[0].bytes[2] == 127,
        "C40 bridge example should publish one exact stage-bound CC packet");
    const auto firstStageId = output.stageId;
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 0
            && output.stageId == firstStageId,
        "C40 bridge example should deduplicate an unchanged CC packet");
    require(
        bridge.compileAndStageDocument(parsed.document, settings).ok()
            && bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != firstStageId
            && bridge.collectRetired(),
        "C40 restage should restart CC publication with exact new-stage identity");
}

void testBoundedMidiNoteOutputExamplePublishesTerminalEvents()
{
    constexpr std::size_t frameCount = 64;
    const auto path = examplePath("c41_bounded_midi_note_output.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C41 MIDI Note project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: none (0)", "C41 zero-audio topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.midiOutputEvent",
        "C41 product status should expose the exact prepared note step");
    requireContains(status, "resources: none", "C41 no external resource");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "64", "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty()
            && countOccurrences(rendered.output, R"json("frame": 0)json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("bytes": [145, 69, 127])json") == 1,
        "C41 CLI should publish one exact frame-zero note-on packet");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = frameCount;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
    bridge.setPlaying(true);
    const auto firstStage = bridge.compileAndStage(settings);
    require(
        firstStage.ok() && firstStage.outputChannelCount == 0,
        "C41 bridge example should stage with zero audio outputs");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != 0
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0x91
            && events[0].bytes[1] == 69
            && events[0].bytes[2] == 127,
        "C41 bridge should publish one exact stage-bound note-on packet");
    const auto firstStageId = output.stageId;
    require(
        bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 0
            && output.stageId == firstStageId,
        "C41 held gate should not retrigger across callbacks");
    require(
        bridge.compileAndStageDocument(parsed.document, settings).ok()
            && bridge.renderTerminalEvents(frameCount, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != firstStageId
            && bridge.collectRetired(),
        "C41 restage should restart note publication with new-stage identity");
}

void testBoundedMidiFileEchoExamplePublishesTerminalEvents()
{
    const auto path = examplePath("c42_bounded_midi_file_echo.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C42 MIDI File Echo project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: none (0)", "C42 zero-audio topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.midiFileEcho",
        "C42 status should expose the prepared MIDI file terminal");
    requireContains(
        status,
        "Sequences/C42 kind=binary generation=1",
        "C42 status should expose the immutable SMF resource");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty()
            && countOccurrences(
                   rendered.output,
                   R"json("bytes": [144, 60, 64])json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("bytes": [128, 60, 0])json") == 1,
        "C42 CLI should publish the exact two-event SMF schedule");
}

void testBoundedGlobalControllerOutputExamplePublishesExactCapture()
{
    const auto path = examplePath(
        "c43_bounded_global_controller_output.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C43 global-controller project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status, "outputs: none (0)", "C43 zero-audio topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.soundToGlobalController",
        "C43 status should expose the prepared global-controller terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "128",
        "--sample-rate", "48000", "--max-block-frames", "37",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty()
            && countOccurrences(
                   rendered.output,
                   R"json("globalControllerEvents": [)json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("frame": 0)json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("name": "!Generated")json") == 1
            && countOccurrences(
                   rendered.output,
                   R"json("value": 0.75)json") == 1,
        "C43 CLI should publish exact one-event global-controller JSON");
}

void testDynamicControlUtilitiesExampleMatchesExactTimeline()
{
    const auto path = examplePath(
        "c44_dynamic_control_utilities.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C44 dynamic-control project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    for (const auto typeId : {
             std::string_view{"talas.quantizer"},
             std::string_view{"talas.gate"},
             std::string_view{"talas.logic"}})
    {
        requireContains(
            status, "type=" + std::string{typeId},
            "C44 status should expose every selected prepared utility");
    }

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "6",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C44 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 6, 8.0);
    require(channels.size() == 1 && channels[0].name == "left",
        "C44 CLI render should expose one exact mono channel");
    constexpr std::array<double, 6> expected = {
        0.0,
        0.70710678118654746,
        0.0,
        0.70710678118654757,
        0.0,
        0.0,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C44 CLI should publish the exact dynamic utility timeline at frame "
                + std::to_string(frame));
    }
}

void testDynamicSlewTimesExampleMatchesExactTimeline()
{
    const auto path = examplePath("c45_dynamic_slew_times.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C45 dynamic-Slew project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status, "step slew [slew] scope=0 type=talas.slew",
        "C45 status should expose the prepared Slew step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "12",
        "--sample-rate", "8", "--max-block-frames", "4",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C45 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 12, 8.0);
    require(channels.size() == 1 && channels[0].name == "left",
        "C45 CLI render should expose one exact mono channel");
    constexpr std::array<double, 12> expected = {
        0.0,
        0.38268343236508978,
        0.70710678118654746,
        0.92387953251128674,
        0.77880078307140488,
        0.0,
        0.0,
        0.0,
        2.7089091298550503e-17,
        -0.38268343236508967,
        -0.70710678118654746,
        -0.92387953251128652,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C45 CLI should publish the exact dynamic Slew timeline at frame "
                + std::to_string(frame));
    }
}

void testDynamicClockParametersExampleMatchesExactTimeline()
{
    const auto path = examplePath("c46_dynamic_clock_parameters.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C46 dynamic-Clock project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status, "step clock [clock] scope=0 type=talas.clock",
        "C46 status should expose the prepared Clock step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "4",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C46 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    require(channels.size() == 1 && channels[0].name == "left",
        "C46 CLI render should expose one exact mono channel");
    constexpr std::array<double, 8> expected = {
        0.0,
        0.70710678118654746,
        1.0,
        0.70710678118654757,
        1.2246467991473532e-16,
        -0.70710678118654746,
        -0.0,
        -0.0,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C46 CLI should publish the exact dynamic Clock timeline at frame "
                + std::to_string(frame));
    }
}

void testDynamicClockDividerParametersExampleMatchesExactTimeline()
{
    const auto path = examplePath(
        "c47_dynamic_clock_divider_parameters.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C47 dynamic-Divider project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status, "step divider [divider] scope=0 type=talas.clockDivider",
        "C47 status should expose the prepared Clock Divider step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "20",
        "--sample-rate", "8", "--max-block-frames", "5",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C47 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 20, 8.0);
    require(channels.size() == 1 && channels[0].name == "left",
        "C47 CLI render should expose one exact mono channel");
    constexpr std::array<double, 20> expected = {
        0.0, 0.0, 0.0, 0.0,
        1.2246467991473532e-16,
        -0.70710678118654746,
        -1.0,
        -0.70710678118654768,
        0.0, 0.0, 0.0, 0.0,
        0.0, -0.0, -0.0, -0.0,
        0.0,
        0.70710678118654746,
        1.0,
        0.70710678118654757,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C47 CLI should publish the exact dynamic Clock Divider timeline at frame "
                + std::to_string(frame));
    }
}

void testTimelineDrivenUtilityExamplesPlayInRealtime()
{
    // Each example sets a kernel setting only through a frame-zero Timeline
    // change, so the Event Value default it compiles with is not valid.
    struct Example
    {
        std::string_view file;
        std::size_t frames = 0;
        std::size_t blockFrames = 0;
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto& example : {
             Example{"c44_dynamic_control_utilities.talas.json", 6, 3},
             Example{"c47_dynamic_clock_divider_parameters.talas.json", 20, 5},
             Example{"c158_ar_scheduled_controls.talas.json", 9, 3}})
    {
        const auto path = examplePath(std::string{example.file});
        const auto name = std::string{example.file};
        const auto rendered = runCli({
            "render", "--project", path.string(),
            "--frames", std::to_string(example.frames),
            "--sample-rate", "8",
            "--max-block-frames", std::to_string(example.blockFrames),
            "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            name + " should render offline");
        const auto offline =
            parseRenderedChannels(rendered.output, example.frames, 8.0);
        require(example.frames % example.blockFrames == 0,
            name + " should render whole realtime blocks");
        const auto parsed = talas::project::parseProjectDocument(readText(path));
        require(parsed.ok(), name + " should parse");
        talas::runtime::ProjectRuntimeBridge bridge(parsed.document, registry);
        bridge.setPlaying(true);
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 8.0;
        settings.maxBlockFrames = example.blockFrames;
        const auto staged = bridge.compileAndStage(settings);
        require(staged.ok() && staged.outputChannelCount == 1,
            name + " should stage for realtime playback: " + staged.message);
        std::vector<float> block(example.blockFrames);
        auto* channel = block.data();
        for (std::size_t start = 0; start < example.frames;
             start += example.blockFrames)
        {
            require(bridge.render(&channel, 1, block.size()),
                name + " should render in realtime");
            for (std::size_t frame = 0; frame < block.size(); ++frame)
            {
                require(
                    std::abs(block[frame]
                        - offline[0].samples[start + frame]) <= 1.0e-6,
                    name + " realtime playback should match offline at frame "
                        + std::to_string(start + frame));
            }
        }
    }
}

void testDynamicGlobalControllerFallbackExampleMatchesExactTimeline()
{
    const auto path = examplePath(
        "c48_dynamic_global_controller_fallback.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C48 dynamic Global Controller project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "outputs: none (0)",
        "C48 zero-audio topology");
    requireContains(
        status,
        "step terminal [terminal] scope=0 type=talas.soundToGlobalController",
        "C48 status should expose the prepared Global Controller terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "7",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty()
            && countOccurrences(
                   rendered.output,
                   R"json("globalControllerEvents": [)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("name": "!C48Generated")json")
                == 3
            && countOccurrences(
                   rendered.output,
                   R"json("frame": 0)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("frame": 2)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("frame": 5)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("value": 0.25)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("value": 0.75)json")
                == 1
            && countOccurrences(
                   rendered.output,
                   R"json("value": -0.5)json")
                == 1,
        "C48 CLI should publish exact de-duplicated dynamic fallback JSON");
}

void testLiveChannellerSelectorsExampleMatchesExactTimeline()
{
    const auto path = examplePath(
        "c49_live_channeller_selectors.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C49 live Channeller project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step channeller [channeller] scope=0 type=talas.channeller",
        "C49 status should expose the prepared Channeller step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C49 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    require(
        channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right",
        "C49 CLI render should expose exact stereo channels");
    // Only right is checked, then both, then only left, then only right:
    // a checked channel plays on both outputs.
    constexpr std::array<double, 8> expectedLeft{
        0.0,
        0.5,
        1.0,
        0.70710678118654757,
        0.0,
        0.5,
        0.0,
        -0.5,
    };
    constexpr std::array<double, 8> expectedRight{
        0.0,
        0.5,
        0.0,
        0.70710678118654757,
        0.0,
        0.5,
        0.0,
        -0.5,
    };
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expectedLeft[frame])
                    <= 1.0e-12
                && std::abs(
                    channels[1].samples[frame] - expectedRight[frame])
                    <= 1.0e-12,
            "C49 CLI should publish exact live stereo route at frame "
                + std::to_string(frame));
    }
}

void testLiveSpectrumModifierSelectorsExampleMatchesExactRender()
{
    const auto path = examplePath(
        "c50_live_spectrum_modifier_selectors.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C50 live Spectrum selector project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C50 status should expose the prepared Spectrum Modifier step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C50 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 8, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C50 CLI render should expose one exact mono channel");
    constexpr std::array<double, 8> expected{
        0.0,
        0.15961685422053379,
        0.25197836671182838,
        0.24836089771201578,
        0.17444143730996331,
        0.094147889569584475,
        0.070947440384840493,
        0.12931651300793262,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C50 CLI should publish exact live Spectrum selector output at frame "
                + std::to_string(frame));
    }
}

void testLiveSpectrumModifierSeedExampleMatchesExactRender()
{
    const auto path = examplePath(
        "c51_live_spectrum_modifier_seed.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C51 live Spectrum seed project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C51 status should expose the prepared Spectrum Modifier step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C51 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 8, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C51 CLI render should expose one exact mono channel");
    // A new probability frame each 3-frame callback, as in realtime.
    constexpr std::array<double, 8> expected{
        0.0,
        0.050978526958199433,
        0.11181844771578017,
        0.41858521599069554,
        0.29999999999999999,
        0.13574250351607653,
        -0.050209531894107873,
        0.081377613648395297,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C51 CLI should publish exact live Spectrum seed output at frame "
                + std::to_string(frame));
    }
}

void testLiveSpectrumModifierHysteresisExampleMatchesExactRender()
{
    const auto path = examplePath(
        "c52_live_spectrum_modifier_hysteresis.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C52 live Spectrum hysteresis project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C52 status should expose the prepared Spectrum Modifier step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "12",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C52 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 12, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C52 CLI render should expose one exact mono channel");
    constexpr std::array<double, 12> expected{
        0.0,
        0.35355339059327373,
        0.5,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.35355339059327384,
        0.5,
        0.35355339059327379,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C52 CLI should publish exact hysteretic exit, deadband, and re-entry output at frame "
                + std::to_string(frame));
    }
}

void testSpectrumModifierProbabilityStreamExampleMatchesPublicFrames()
{
    const auto path = examplePath(
        "c53_spectrum_modifier_probability_stream.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C53 probability-stream project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C53 status should expose the prepared Spectrum Modifier step");
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "6",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C53 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 6, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C53 CLI render should expose one exact mono channel");
    // One probability frame per 3-frame callback-sized window, as in
    // realtime: the second deselects the track.
    constexpr std::array<double, 6> expected{
        0.0,
        0.35355339059327373,
        0.5,
        0.0,
        0.0,
        0.0,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C53 CLI should draw one probability frame per window at frame "
                + std::to_string(frame));
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 3;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C53 product example should compile for prepared execution");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    std::array<double, 3> ordinalZero{};
    std::array<double, 3> ordinalOne{};
    require(
        realtime.ready()
            && realtime.render(ordinalZero)
            && realtime.render(ordinalOne)
            && ordinalZero
                == std::array<double, 3>{
                    0.0, 0.35355339059327373, 0.5}
            && ordinalOne == std::array<double, 3>{},
        "C53 prepared product example should sound on ordinal zero and mute on ordinal one");
}

void testSpectrumModifierTrackProbabilityProfileExampleMatchesExactRender()
{
    const auto path = examplePath(
        "c54_spectrum_modifier_track_probability_profile.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C54 TrackNumber-profile project should parse at current format 36");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C54 status should expose the prepared Spectrum Modifier step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "6",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C54 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 6, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C54 CLI render should expose one exact mono channel");
    // One probability frame per 3-frame callback-sized window, as in
    // realtime.
    constexpr std::array<double, 6> expected{
        0.0,
        0.41134011164280987,
        0.65774657842445849,
        0.17677669529663689,
        3.061616997868383e-17,
        -0.17677669529663687,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C54 CLI should publish the exact four-track TrackNumber probability profile at frame "
                + std::to_string(frame));
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 3;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C54 product example should compile for prepared execution");
    const auto modifier = std::ranges::find(
        compiled.graph->plan().steps,
        std::string_view{"talas.spectrumModifier"},
        &talas::runtime::CompileStep::typeId);
    require(
        modifier != compiled.graph->plan().steps.end()
            && modifier->spectrumModifierProbabilityProfile.size()
                == 4096
            && modifier->spectrumModifierProbabilityProfile[0]
                == 0.25
            && modifier->spectrumModifierProbabilityProfile[1]
                == 0.5
            && modifier->spectrumModifierProbabilityProfile[2]
                == 0.75
            && modifier->spectrumModifierProbabilityProfile[3]
                == 1.0
            && modifier->spectrumModifierProbabilityProfile.back()
                == 1.0,
        "C54 product example should retain the exact clipped 4096-entry profile");
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    std::array<double, 3> prepared{};
    require(
        realtime.ready()
            && realtime.render(prepared)
            && prepared == std::array<double, 3>{
                expected[0], expected[1], expected[2]},
        "C54 prepared product example should match exact CLI ordinal-zero output");
}

void testSpectrumModifierRemainderProbabilityProfileExampleMatchesExactRender()
{
    const auto path = examplePath(
        "c55_spectrum_modifier_remainder_probability_profile.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C55 remainder-profile project should parse with exact format-35 VariablesV2 authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step modifier [modifier] scope=0 type=talas.spectrumModifier",
        "C55 status should expose the prepared Spectrum Modifier step");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "6",
        "--sample-rate", "32", "--max-block-frames", "3",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C55 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 6, 32.0);
    require(
        channels.size() == 1 && channels[0].name == "left",
        "C55 CLI render should expose one exact mono channel");
    constexpr std::array<double, 6> expected{
        0.0,
        0.27244755338790932,
        0.42677669529663687,
        0.40774657842445861,
        0.25000000000000006,
        0.054193187831184819,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C55 CLI should publish exact even-track remainder-profile output at frame "
                + std::to_string(frame));
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 32.0;
    settings.maxBlockFrames = 3;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C55 product example should compile for prepared execution");
    const auto modifier = std::ranges::find(
        compiled.graph->plan().steps,
        std::string_view{"talas.spectrumModifier"},
        &talas::runtime::CompileStep::typeId);
    require(
        modifier != compiled.graph->plan().steps.end()
            && modifier->spectrumModifierProbabilityProfile.size()
                == 4096,
        "C55 product example should retain a complete immutable remainder profile");
    for (std::size_t index = 0;
         index < modifier->spectrumModifierProbabilityProfile.size();
         ++index)
    {
        require(
            modifier->spectrumModifierProbabilityProfile[index]
                == (index % 2 == 0 ? 0.0 : 1.0),
            "C55 product example should retain exact alternating one-based source ordinals");
    }
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    std::array<double, 3> prepared{};
    require(
        realtime.ready()
            && realtime.render(prepared)
            && prepared == std::array<double, 3>{
                expected[0], expected[1], expected[2]},
        "C55 prepared product example should match exact CLI output without callback expression work");
}

void testSpectrumOnDiskFrameScanExampleMatchesTransactionSchedule()
{
    constexpr std::size_t blockFrames = 512;
    constexpr std::size_t transactionCount = 8;
    const auto path = examplePath(
        "c56_spectrum_on_disk_frame_scan.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C56 frame-scan project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step source [source] scope=0 type=talas.spectrumOnDisk",
        "C56 status should expose the prepared Spectrum On Disk step");
    requireContains(
        status,
        "Analysis/C56 kind=table generation=1 hash=e5c6e5c0ed78294f2fda94de56775e2dfbabc06cc1eabc26a79d2a4248840e5e health=verified",
        "C56 status should expose the exact verified multi-frame table");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C56 status should preserve zero-latency Spectrum ancestry");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "1024",
        "--sample-rate", "44100", "--max-block-frames", "512",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C56 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(
        rendered.output, 1024, 44100.0);
    require(
        cliChannels.size() == 1
            && cliChannels.front().name == "left"
            && std::ranges::any_of(
                cliChannels.front().samples,
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C56 CLI render should publish one finite audible channel");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 44100.0;
    settings.maxBlockFrames = blockFrames;
    settings.eventValues.push_back({
        "C56Restart", 0.0, talas::expr::ValueDomain::Scalar});
    settings.tableResources.push_back({
        "Analysis/C56",
        {
            0.30, 1.00, 0.0, 0.12, 2.00, 0.0,
            0.08, 3.00, 0.0, 0.04, 4.00, 0.0,
            0.28, 1.00, 0.0, 0.16, 2.03, 0.0,
            0.10, 3.07, 0.0, 0.05, 4.12, 0.0,
            0.24, 1.00, 0.0, 0.20, 2.08, 0.0,
            0.13, 3.16, 0.0, 0.07, 4.25, 0.0,
            0.20, 1.00, 0.0, 0.24, 2.15, 0.0,
            0.16, 3.28, 0.0, 0.09, 4.42, 0.0,
            0.17, 1.00, 0.0, 0.27, 2.24, 0.0,
            0.19, 3.43, 0.0, 0.11, 4.63, 0.0,
            0.15, 1.00, 0.0, 0.25, 2.35, 0.0,
            0.22, 3.61, 0.0, 0.14, 4.88, 0.0,
            0.18, 1.00, 0.0, 0.21, 2.48, 0.0,
            0.24, 3.82, 0.0, 0.17, 5.17, 0.0,
            0.22, 1.00, 0.0, 0.17, 2.63, 0.0,
            0.20, 4.06, 0.0, 0.20, 5.50, 0.0,
        },
    });
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C56 product example should compile for prepared execution");
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
    require(
        prepared.ready(),
        "C56 product example should prepare realtime: "
            + std::string{prepared.error()});
    std::vector<double> offlineOutput;
    std::vector<double> preparedOutput;
    for (std::size_t transaction = 0;
         transaction < transactionCount;
         ++transaction)
    {
        const auto offlineBlock = offline.render(blockFrames);
        require(
            offlineBlock.ok()
                && offlineBlock.mono.size() == blockFrames,
            "C56 offline product transaction should render");
        offlineOutput.insert(
            offlineOutput.end(),
            offlineBlock.mono.begin(),
            offlineBlock.mono.end());
        std::array<double, blockFrames> preparedBlock{};
        require(
            prepared.render(preparedBlock),
            "C56 prepared product transaction should render");
        preparedOutput.insert(
            preparedOutput.end(),
            preparedBlock.begin(),
            preparedBlock.end());
    }
    require(
        offlineOutput == preparedOutput
            && std::ranges::any_of(
                preparedOutput,
                [](double sample) {
                    return std::abs(sample) > 1.0e-9;
                }),
        "C56 offline/prepared product transaction schedule should be bit-identical and audible");
}

void testSpectrumVoicedUnvoicedTransitionExampleMatchesPublicFrames()
{
    constexpr std::array expected{1.0, 0.5, 0.0, 0.5, 1.0};
    const auto path = examplePath(
        "c57_spectrum_voiced_unvoiced_transition.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C57 voiced/unvoiced transition project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step detector [detector] scope=0 type=talas.spectrumVoicedUnvoiced",
        "C57 status should expose the prepared voiced/unvoiced detector step");
    requireContains(
        status,
        "Analysis/C57 kind=table generation=1 hash=0af93d5e6d03bec92bc4b965bb48d7222ec2e5029cdbd4de2073ceca3074aa88 health=verified",
        "C57 status should expose the verified immutable transition table");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C57 status should preserve zero-latency Spectrum ancestry");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "1",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C57 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(
        rendered.output, 8, 8.0);
    // Each one-frame window of the render is a transaction, as each
    // realtime callback is: the CLI render walks the same transitions and
    // then holds the final voiced frame.
    std::vector<double> walked(8, 1.0);
    std::ranges::copy(expected, walked.begin());
    require(
        cliChannels.size() == 2
            && cliChannels[0].name == "left"
            && cliChannels[1].name == "right"
            && cliChannels[0].samples == walked
            && cliChannels[0].samples == cliChannels[1].samples,
        "C57 CLI render should publish each window's endpoint or transition frame on both channels");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 1;
    settings.tableResources.push_back({
        "Analysis/C57",
        {
            0.40, 1.00, 0.0, 0.20, 2.00, 0.0,
            0.40, 1.00, 0.0, 0.20, 2.60, 0.0,
            0.40, 1.00, 0.0, 0.20, 2.60, 0.0,
            0.40, 1.00, 0.0, 0.20, 2.00, 0.0,
            0.40, 1.00, 0.0, 0.20, 2.00, 0.0,
        },
    });
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        parsed.document, registry, settings);
    require(
        compiled.ok(),
        "C57 product example should compile for prepared execution");
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
    require(
        prepared.ready(),
        "C57 product example should prepare realtime: "
            + std::string{prepared.error()});
    for (std::size_t transaction = 0;
         transaction < expected.size();
         ++transaction)
    {
        const auto offlineBlock = offline.render(1);
        std::array<double, 1> preparedBlock{};
        require(
            offlineBlock.ok()
                && offlineBlock.mono
                    == std::vector<double>{expected[transaction]}
                && prepared.render(preparedBlock)
                && preparedBlock.front() == expected[transaction],
            "C57 offline/prepared product transaction should publish the exact endpoint or transition frame at index "
                + std::to_string(transaction));
    }
}

void testSpectrumInRamNormalizedTimeIndexExampleMatchesEndpoints()
{
    const auto path = examplePath(
        "c58_spectrum_in_ram_normalized_time_index.talas.json");
    const auto parsed =
        talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C58 normalized SpectrumInRAM project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step source [source] scope=0 type=talas.spectrumInRam",
        "C58 status should expose the prepared SpectrumInRAM step");
    requireContains(
        status,
        "Analysis/C58 kind=table generation=1 hash=094edff186973a2365770c4ca8975d0ed994804cc4e9a96c61c673c15228a1a5 health=verified",
        "C58 status should expose the verified immutable normalized table");
    requireContains(
        status,
        "critical-path: 0 frames",
        "C58 full-frame TrackSelector should report its local zero-delay processing contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "4",
        "--sample-rate", "1000", "--max-block-frames", "4",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C58 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(
        rendered.output, 4, 1000.0);
    require(
        cliChannels.size() == 2
            && cliChannels[0].name == "left"
            && cliChannels[1].name == "right"
            && std::ranges::all_of(
                cliChannels[0].samples,
                [](double sample) { return sample == 0.4; })
            && std::ranges::all_of(
                cliChannels[1].samples,
                [](double sample) { return sample == 100.0; }),
        "C58 CLI default -1 coordinate should publish the exact first frame");

    struct Endpoint
    {
        double timeIndex;
        double amplitude;
        double frequency;
    };
    constexpr std::array endpoints{
        Endpoint{-1.0, 0.4, 100.0},
        Endpoint{0.0, 0.6, 300.0},
        Endpoint{1.0, 0.8, 500.0},
    };
    for (const auto endpoint : endpoints)
    {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 1000.0;
        settings.maxBlockFrames = 4;
        settings.eventValues.push_back({
            "C58TimeIndex",
            endpoint.timeIndex,
            talas::expr::ValueDomain::Scalar,
        });
        settings.tableResources.push_back({
            "Analysis/C58",
            {
                0.40, 1.00, 0.0, 0.20, 2.00, 0.0,
                0.60, 3.00, 0.0, 0.30, 4.00, 0.0,
                0.80, 5.00, 0.0, 0.40, 6.00, 0.0,
            },
        });
        const auto compiled = talas::runtime::compileProject(
            parsed.document,
            talas::core::makeBuiltinNodeRegistry(),
            settings);
        require(
            compiled.ok(),
            "C58 product endpoint should compile for offline and prepared execution");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
        talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
        std::array<double, 4> left{};
        std::array<double, 4> right{};
        require(
            offline.ok()
                && prepared.ready()
                && prepared.render(left, right)
                && std::ranges::all_of(
                    offline.mono,
                    [&](double sample) {
                        return sample == endpoint.amplitude;
                    })
                && std::ranges::all_of(
                    offline.right,
                    [&](double sample) {
                        return sample == endpoint.frequency;
                    })
                && std::equal(
                    left.begin(), left.end(), offline.mono.begin())
                && std::equal(
                    right.begin(), right.end(), offline.right.begin()),
            "C58 offline/prepared product endpoint should match exactly at TimeIndex "
                + std::to_string(endpoint.timeIndex));
    }
}

void testC59SpectrumInRamDefaultAnalysisMetadataExample()
{
    const auto path = examplePath(
        "c59_spectrum_in_ram_default_analysis_metadata.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C59 default analysis metadata project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step source [source] scope=0 type=talas.spectrumInRam",
        "C59 status should expose the prepared SpectrumInRAM step");
    requireContains(
        status,
        "Analysis/C59 kind=table generation=1 hash=33229df355e62d037191851a5467322e95f22d884f5c25a03de264d004901139 health=verified",
        "C59 status should expose the exact immutable default-metadata table");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "4",
        "--sample-rate", "1000", "--max-block-frames", "4",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C59 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(
        rendered.output, 4, 1000.0);
    require(
        cliChannels.size() == 2
            && std::ranges::all_of(
                cliChannels[0].samples,
                [](double sample) { return sample == 0.6; })
            && std::ranges::all_of(
                cliChannels[1].samples,
                [](double sample) { return sample == 375.0; }),
        "C59 CLI should publish the exact original-pitch final default partial");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 4;
    settings.tableResources.push_back({
        "Analysis/C59",
        {
            0.20, 1.00, 0.10,
            0.40, 2.00, 0.20,
            0.60, 3.00, 0.30,
        },
    });
    const auto compiled = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C59 product example should compile for offline and prepared execution");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession prepared(*compiled.graph);
    std::array<double, 4> left{};
    std::array<double, 4> right{};
    require(
        offline.ok()
            && prepared.ready()
            && prepared.render(left, right)
            && std::ranges::all_of(
                offline.mono,
                [](double sample) { return sample == 0.6; })
            && std::ranges::all_of(
                offline.right,
                [](double sample) { return sample == 375.0; })
            && std::equal(left.begin(), left.end(), offline.mono.begin())
            && std::equal(right.begin(), right.end(), offline.right.begin()),
        "C59 offline and prepared product paths should match exactly");
}

void testC60SpectrumOnDiskDefaultAnalysisMetadataExample()
{
    const auto path = examplePath(
        "c60_spectrum_on_disk_default_analysis_metadata.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C60 default analysis metadata project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step source [source] scope=0 type=talas.spectrumOnDisk",
        "C60 status should expose the prepared SpectrumOnDisk step");
    requireContains(
        status,
        "Analysis/C60 kind=table generation=1 hash=5313322d694f8c1553acdfb1ea0330a88abe562f3d675abc93c6a8526d2608b2 health=verified",
        "C60 status should expose the exact immutable default-metadata table");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "4",
        "--sample-rate", "1000", "--max-block-frames", "2",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C60 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(
        rendered.output, 4, 1000.0);
    // One analysis frame per 2-frame window, as per realtime callback.
    require(
        cliChannels.size() == 2
            && cliChannels[0].samples == std::vector<double>{0.6, 0.6, 1.2, 1.2}
            && cliChannels[1].samples
                == std::vector<double>{375.0, 375.0, 437.5, 437.5},
        "C60 CLI should step the default scan once per window");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 2;
    settings.tableResources.push_back({
        "Analysis/C60",
        {
            0.20, 1.00, 0.10,
            0.40, 2.00, 0.20,
            0.60, 3.00, 0.30,
            0.80, 1.50, 0.15,
            1.00, 2.50, 0.25,
            1.20, 3.50, 0.35,
        },
    });
    const auto compiled = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "C60 product example should compile for offline and prepared execution");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    const auto offlineFirst = offline.render(4);
    const auto offlineSecond = offline.render(4);
    std::array<double, 2> preparedFirstLeft{};
    std::array<double, 2> preparedFirstRight{};
    std::array<double, 2> preparedSecondLeft{};
    std::array<double, 2> preparedSecondRight{};
    require(
        offlineFirst.ok() && offlineSecond.ok()
            && prepared.ready()
            && prepared.render(preparedFirstLeft, preparedFirstRight)
            && prepared.render(preparedSecondLeft, preparedSecondRight)
            && offlineFirst.mono == std::vector<double>{0.6, 0.6, 1.2, 1.2}
            && offlineFirst.right
                == std::vector<double>{375.0, 375.0, 437.5, 437.5}
            && std::ranges::all_of(
                offlineSecond.mono,
                [](double sample) { return sample == 1.2; })
            && std::ranges::all_of(
                offlineSecond.right,
                [](double sample) { return sample == 437.5; })
            && std::equal(
                preparedFirstLeft.begin(), preparedFirstLeft.end(),
                offlineFirst.mono.begin())
            && std::equal(
                preparedFirstRight.begin(), preparedFirstRight.end(),
                offlineFirst.right.begin())
            && std::equal(
                preparedSecondLeft.begin(), preparedSecondLeft.end(),
                offlineSecond.mono.begin())
            && std::equal(
                preparedSecondRight.begin(), preparedSecondRight.end(),
                offlineSecond.right.begin()),
        "C60 offline and prepared product scan transactions should match exactly: offlineFirst="
            + (offlineFirst.mono.empty()
                   ? std::string{"empty"}
                   : std::to_string(offlineFirst.mono.front()))
            + "/"
            + (offlineFirst.right.empty()
                   ? std::string{"empty"}
                   : std::to_string(offlineFirst.right.front()))
            + ", offlineSecond="
            + (offlineSecond.mono.empty()
                   ? std::string{"empty"}
                   : std::to_string(offlineSecond.mono.front()))
            + "/"
            + (offlineSecond.right.empty()
                   ? std::string{"empty"}
                   : std::to_string(offlineSecond.right.front()))
            + ", preparedFirst="
            + std::to_string(preparedFirstLeft.front()) + "/"
            + std::to_string(preparedFirstRight.front())
            + ", preparedSecond="
            + std::to_string(preparedSecondLeft.front()) + "/"
            + std::to_string(preparedSecondRight.front()));
    prepared.resetTransport();
    require(
        prepared.render(preparedFirstLeft, preparedFirstRight)
            && preparedFirstLeft.front() == 0.6
            && preparedFirstRight.front() == 375.0,
        "C60 product reset should restore the exact first default scan transaction");
}

void testC61SumOfSinesDefaultAnalysisFrequenciesExample()
{
    const auto path = examplePath(
        "c61_sum_of_sines_default_analysis_frequencies.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(
        parsed.ok()
            && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C61 default analysis frequencies project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(
        status,
        "step source [source] scope=0 type=talas.sumOfSines",
        "C61 status should expose the prepared SumOfSines step");
    requireContains(
        status,
        "Analysis/C61 kind=table generation=1 hash=4355a46b19d348dc2f57c046f8ef63d4538ebb936000f3c9ee954a27460dd865 health=verified",
        "C61 status should expose the exact immutable amplitude table");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "4",
        "--sample-rate", "16", "--max-block-frames", "4",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
            "C61 CLI render should succeed");
    const auto cliChannels = parseRenderedChannels(rendered.output, 4, 16.0);
    const std::array expected{
        0.0,
        std::numbers::sqrt2_v<double> * 0.5,
        1.0,
        std::numbers::sqrt2_v<double> * 0.5,
    };
    require(cliChannels.size() == 1, "C61 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(cliChannels[0].samples[frame] - expected[frame]) < 1.0e-12,
                "C61 CLI should publish the analytic 2 Hz phase sequence");

    talas::runtime::RenderSettings settings{16.0, 4};
    settings.tableResources.push_back({"Analysis/C61", {1.0}});
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C61 product example should compile offline and prepared");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 4> realtime{};
    require(offline.ok() && prepared.ready() && prepared.render(realtime),
            "C61 product example should render offline and prepared");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(offline.mono[frame] - expected[frame]) < 1.0e-12,
                "C61 offline product output should match the analytic oracle");
        require(std::abs(realtime[frame] - offline.mono[frame]) < 1.0e-12,
                "C61 prepared product output should match offline");
    }
    prepared.resetTransport();
    require(prepared.render(realtime), "C61 product reset should render");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(realtime[frame] - expected[frame]) < 1.0e-12,
                "C61 product reset should restore the analytic phase sequence");
}

void testC62SumOfSinesControlledTimeFramesExample()
{
    const auto path = examplePath(
        "c62_sum_of_sines_controlled_time_frames.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C62 controlled-time project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status,
        "Analysis/C62 kind=table generation=1 hash=ab96cf895eb20ff99ee746aa234b0f0a9f8369963866034a0b9dc95497f4245e health=verified",
        "C62 status should expose the exact immutable amplitude frames");
    requireContains(status,
        "Control/C62TimeIndex kind=table generation=1 hash=21d24ee1bb029545140805f7611a1f3565b381fa9e47ff11daf18399d8e1384c health=verified",
        "C62 status should expose the exact normalized selector sequence");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "4",
        "--sample-rate", "8", "--max-block-frames", "2",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C62 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 4, 8.0);
    const std::array expected{
        0.0,
        std::numbers::sqrt2_v<double> * 0.25,
        0.25,
        std::numbers::sqrt2_v<double> * 0.25,
    };
    require(channels.size() == 1, "C62 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C62 CLI should publish the analytic controlled-time sequence");
}

void testC63SumOfSinesAutomaticGateLoopExample()
{
    const auto path = examplePath(
        "c63_sum_of_sines_automatic_gate_loop.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C63 automatic Gate/Loop project should parse with current format authority");
    requireOrdinaryValidation(path);
    const auto status = statusFor(path).output;
    requireContains(status,
        "Analysis/C63 kind=table generation=1 hash=ab96cf895eb20ff99ee746aa234b0f0a9f8369963866034a0b9dc95497f4245e health=verified",
        "C63 status should expose the exact immutable amplitude frames");
    requireContains(status,
        "Control/C63Gate kind=table generation=1 hash=9dd8ca1c75d36b93cd63c25e85b114ee4e1158b3fbf2e69f5dab1d995e7b1bc2 health=verified",
        "C63 status should expose the exact immutable Gate sequence");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "10",
        "--sample-rate", "4", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C63 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 10, 4.0);
    constexpr std::array expected{
        0.0, 0.75, 0.0, -0.75, 0.0, 0.75, 0.0, 0.0, 0.0, 0.75};
    require(channels.size() == 1, "C63 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C63 CLI should publish the exact loop/release/retrigger sequence");
    }
}

void testC64SyntheticSpectrumArrayPrefixExample()
{
    const auto path = examplePath(
        "c64_synthetic_spectrum_array_prefix.talas.json");
    const auto parsed = talas::project::parseProjectDocument(readText(path));
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C64 array-prefix project should migrate to current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "4", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C64 CLI status should succeed");
    requireContains(status.output,
        "type=talas.syntheticSpectrumFromArray",
        "C64 status should expose the common-prefix Spectrum source");
    requireContains(status.output,
        "type=talas.oscillatorBank",
        "C64 status should expose the audible resynthesis path");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "4", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C64 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 4.0);
    constexpr std::array expected{
        0.0, 0.75, 0.0, -0.75, 0.0, 0.75, 0.0, -0.75};
    require(channels.size() == 1, "C64 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C64 CLI should publish only the shortest array prefix");
    }

    talas::runtime::RenderSettings settings{4.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C64 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C64 product example should prepare offline and realtime");
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C64 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C64 prepared partitions should be bit-identical offline");
}

void testC65SyntheticSpectrumStaticExpressionsExample()
{
    const auto path = examplePath(
        "c65_synthetic_spectrum_static_expressions.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C65 example should be persisted in format 35");
    requireContains(source,
        "\"amplitudes\", \"text\": \"{-6.020599913279624 db} 0.25\"",
        "C65 example should mix a dB expression with a plain amplitude literal");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C65 static-expression project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "4", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C65 CLI status should succeed");
    requireContains(status.output,
        "type=talas.syntheticSpectrumFromArray",
        "C65 status should expose the static-expression Spectrum source");
    requireContains(status.output,
        "type=talas.oscillatorBank",
        "C65 status should expose the audible resynthesis path");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "4", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C65 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 4.0);
    const auto amplitude = std::pow(
        10.0, -6.020599913279624 / 20.0);
    const std::array expected{
        0.0, amplitude, 0.0, -amplitude,
        0.0, amplitude, 0.0, -amplitude};
    require(channels.size() == 1, "C65 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C65 CLI should publish the exact evaluated common-prefix oscillator sequence");
    }

    talas::runtime::RenderSettings settings{4.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C65 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C65 product example should prepare offline and realtime");
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{2, 3, 3})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C65 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C65 prepared partitions should be bit-identical offline");
}

void testC66ThresholdHysteresisGateExample()
{
    const auto path = examplePath(
        "c66_threshold_hysteresis_gate.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C66 example should be persisted in format 35");
    requireContains(source, "\"hysteresis\", \"expression\": \"0.4\"",
        "C66 example should expose non-zero hysteresis");
    requireContains(source, "\"minimum\", \"expression\": \"-0.1\"",
        "C66 example should expose a bipolar control range");
    requireContains(source, "\"maximum\", \"expression\": \"0.7\"",
        "C66 example should sample the exact upper boundary");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C66 Threshold project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C66 CLI status should succeed");
    requireContains(status.output, "type=talas.threshold",
        "C66 status should expose the stateful Threshold gate");
    requireContains(status.output, "type=talas.vca",
        "C66 status should expose the audible VCA path");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C66 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto pi = std::acos(-1.0);
    const std::array expected{
        0.0, 0.0, std::sin(pi / 4.0), std::sin(3.0 * pi / 8.0),
        1.0, 0.0, 0.0, 0.0,
    };
    require(channels.size() == 1, "C66 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C66 CLI should publish the exact hysteresis-gated oscillator sequence");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C66 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C66 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C66 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C66 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 2> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(2)),
        "C66 prepared reset should restore the pristine Threshold lifecycle");
}

void testC67TimeControlHalfSpeedExample()
{
    const auto path = examplePath(
        "c67_time_control_half_speed.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C67 example should be persisted in format 35");
    requireContains(source, "\"rate\", \"expression\": \"0.5\"",
        "C67 example should expose the exact half-speed rate");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C67 Time Control project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C67 CLI status should succeed");
    requireContains(status.output, "type=talas.timeControl",
        "C67 status should expose the stateful Time Control operation");
    requireContains(status.output, "type=talas.audioOutput",
        "C67 status should expose the audible terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C67 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto pi = std::acos(-1.0);
    const auto sinPiOver4 = std::sin(pi / 4.0);
    const std::array expected{
        0.0,
        sinPiOver4 * 0.5,
        sinPiOver4,
        (sinPiOver4 + 1.0) * 0.5,
        1.0,
        (1.0 + sinPiOver4) * 0.5,
        sinPiOver4,
        sinPiOver4 * 0.5,
    };
    require(channels.size() == 1, "C67 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C67 CLI should publish the exact half-speed interpolated sequence");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.maxRealtimeHistoryFrames = 16;
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C67 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C67 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C67 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C67 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C67 prepared reset should restore pristine Time Control history and cursor");
}

void testC68TimeStopperSampleHoldExample()
{
    const auto path = examplePath(
        "c68_time_stopper_sample_hold.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C68 example should be persisted in format 35");
    requireContains(source, "\"eventName\": \"C68Resume\"",
        "C68 example should expose its scheduled scalar resume bridge");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C68 Time Stopper project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C68 CLI status should succeed");
    requireContains(status.output, "type=talas.timeStopper",
        "C68 status should expose the stateful Time Stopper operation");
    requireContains(status.output, "type=talas.audioOutput",
        "C68 status should expose the audible terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C68 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto pi = std::acos(-1.0);
    const auto held = std::sin(3.0 * pi / 4.0);
    const std::array expected{
        0.0,
        std::sin(pi / 4.0),
        1.0,
        held,
        held,
        held,
        held,
        held,
    };
    require(channels.size() == 1, "C68 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C68 CLI should publish the exact stop, progress, and hold sequence");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.eventValues = {
        {"C68Resume", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "C68Resume", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C68Resume", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C68 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C68 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C68 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C68 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C68 prepared reset should restore pristine Time Stopper state and schedule");
}

void testC69TimeOffsetDelayLineExample()
{
    const auto path = examplePath(
        "c69_time_offset_delay_line.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C69 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.timeOffset\"",
        "C69 example should expose its stateful delay-line node");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C69 Time Offset project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C69 CLI status should succeed");
    requireContains(status.output, "type=talas.timeOffset",
        "C69 status should expose the stateful Time Offset operation");
    requireContains(status.output, "type=talas.audioOutput",
        "C69 status should expose the audible terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C69 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto pi = std::acos(-1.0);
    const std::array expected{
        0.0,
        0.0,
        0.0,
        std::sin(pi / 4.0),
        1.0,
        std::sin(3.0 * pi / 4.0),
        std::sin(pi),
        -std::sin(pi / 4.0),
    };
    require(channels.size() == 1, "C69 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(channels[0].samples[frame] - expected[frame]) < 1.0e-12,
            "C69 CLI should publish the exact delayed oscillator sequence");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C69 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C69 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C69 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C69 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C69 prepared reset should restore zero-filled Time Offset history and index");
}

void testC70TriggeredSampleAndHoldEdgesExample()
{
    const auto path = examplePath(
        "c70_triggered_sample_and_hold_edges.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C70 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.triggeredSampleAndHold\"",
        "C70 example should expose its stateful edge-triggered hold node");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion,
        "C70 Triggered Sample And Hold project should use current format authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C70 CLI status should succeed");
    requireContains(status.output, "type=talas.triggeredSampleAndHold",
        "C70 status should expose the stateful Triggered Sample And Hold operation");
    requireContains(status.output, "type=talas.audioOutput",
        "C70 status should expose the audible terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C70 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto rootHalf = std::sqrt(0.5);
    const auto firstHeld = 5.0 + 4.0 * rootHalf;
    const auto secondHeld = 5.0 - 4.0 * rootHalf;
    const std::array expected{
        0.0,
        firstHeld * rootHalf,
        firstHeld,
        firstHeld * rootHalf,
        firstHeld * std::sin(std::acos(-1.0)),
        -secondHeld * rootHalf,
        -secondHeld,
        -secondHeld * rootHalf,
    };
    require(channels.size() == 1,
        "C70 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C70 CLI should publish initial zero, first capture/hold, re-arm, and second capture/hold");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C70 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C70 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C70 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C70 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C70 prepared reset should restore the uncaptured hold and pristine edge state");
}

void testC71TriggeredTableReadGateResetExample()
{
    const auto path = examplePath(
        "c71_triggered_table_read_gate_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C71 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.triggeredTableRead\"",
        "C71 example should expose its session-stateful table reader");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id == "Tables/C71"
            && parsed.document.assets.front().contentHash
                == "aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274",
        "C71 Triggered Table Read project should own exact current-format immutable table authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C71 CLI status should succeed");
    requireContains(status.output, "type=talas.triggeredTableRead",
        "C71 status should expose the stateful Triggered Table Read operation");
    requireContains(
        status.output,
        "Tables/C71 kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C71 status should expose exact verified immutable table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C71 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto rootHalf = std::sqrt(0.5);
    const std::array expected{
        0.0,
        0.5 * rootHalf,
        0.75,
        rootHalf,
        0.25 * std::sin(std::acos(-1.0)),
        -0.5 * rootHalf,
        -0.75,
        -rootHalf,
    };
    require(channels.size() == 1,
        "C71 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C71 CLI should publish table advance, Gate reset, replay, and exact carrier samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.eventValues.push_back(
        {"C71Gate", 1.0, talas::expr::ValueDomain::Scalar});
    settings.eventValueChanges = {
        {2, "C71Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {4, "C71Gate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources.push_back(
        {"Tables/C71", {0.25, 0.5, 0.75, 1.0}});
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C71 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C71 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C71 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C71 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C71 prepared reset should restore cursor, held sample, and Gate polarity");
}

void testC72TunableVocoderBankResetExample()
{
    const auto path = examplePath(
        "c72_tunable_vocoder_bank_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C72 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.tunableVocoder\"",
        "C72 example should expose its stateful filter/envelope bank");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C72 Tunable Vocoder project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C72 CLI status should succeed");
    requireContains(status.output, "type=talas.tunableVocoder",
        "C72 status should expose the stateful Tunable Vocoder operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C72 status should preserve exact zero-latency authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C72 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    constexpr std::array expected{
        0.0,
        0.0020591095517226766,
        0.013201241715327583,
        0.011213326129238891,
        -0.0012308801407124395,
        -0.02850312404608764,
        -0.05911243553744583,
        -0.03749055053845792,
    };
    require(channels.size() == 1,
        "C72 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C72 CLI should publish exact carrier/filter/envelope samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C72 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C72 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C72 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C72 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C72 prepared reset should restore pristine filter and envelope state");
}

void testC73TwoFormantPairResetExample()
{
    const auto path = examplePath(
        "c73_two_formant_pair_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C73 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.twoFormantElement\"",
        "C73 example should expose Two Formant Element");
    requireContains(source, "\"typeId\": \"talas.twoFormantVoiceElement\"",
        "C73 example should expose Two Formant Voice Element");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C73 two-formant project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C73 CLI status should succeed");
    requireContains(status.output, "type=talas.twoFormantElement",
        "C73 status should expose Two Formant Element");
    requireContains(status.output, "type=talas.twoFormantVoiceElement",
        "C73 status should expose Two Formant Voice Element");
    requireContains(status.output, "critical-path: 0 frames",
        "C73 status should preserve exact zero intrinsic latency");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C73 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    // Fixed-pole TFE plus the unchanged sine/RBJ Voice candidate.
    // Independently derived scalar recurrence; not captured reference output.
    constexpr std::array expected{
        0.0,
        0.3461883336772389,
        0.48530180186998495,
        -0.03757722459394741,
        -0.8214422094722416,
        -1.0357405328085585,
        -0.4437152251603097,
        0.48950100934008833,
    };
    require(channels.size() == 1,
        "C73 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C73 CLI should publish exact paired-formant samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C73 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C73 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C73 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C73 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C73 prepared reset should restore both histories and voice phase");
}

void testC74SoundVariableSelectorResetExample()
{
    const auto path = examplePath(
        "c74_sound_variable_selector_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C74 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.variable\"",
        "C74 example should expose the Sound-variable selector");
    requireContains(source, "\"initialSelectedOrdinal\": 1",
        "C74 example should select its second candidate initially");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C74 Sound-variable project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C74 CLI status should succeed");
    requireContains(status.output, "type=talas.variable",
        "C74 status should expose the Sound-variable selector");
    requireContains(status.output, "critical-path: 0 frames",
        "C74 status should preserve exact zero intrinsic latency");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C74 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    constexpr std::array expected{
        0.0,
        0.75,
        9.1848509936051484e-17,
        -0.75,
        0.0,
        0.75,
        9.1848509936051484e-17,
        -0.75,
    };
    require(channels.size() == 1,
        "C74 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C74 CLI should publish exact selected-candidate samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C74 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C74 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C74 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C74 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C74 prepared reset should preserve selection and restore both candidate phases");
}

void testC75FixedVocoderBankResetExample()
{
    const auto path = examplePath(
        "c75_fixed_vocoder_bank_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C75 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.vocoder\"",
        "C75 example should expose its fixed stateful filter/envelope bank");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C75 fixed Vocoder project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C75 CLI status should succeed");
    requireContains(status.output, "type=talas.vocoder",
        "C75 status should expose the stateful fixed Vocoder operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C75 status should preserve exact zero-latency authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C75 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    constexpr std::array expected{
        0.0,
        0.004118219103445353,
        0.026402483430655165,
        0.022426652258477783,
        -0.002461760281424879,
        -0.05700624809217528,
        -0.11822487107489166,
        -0.07498110107691584,
    };
    require(channels.size() == 1,
        "C75 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C75 CLI should publish exact carrier/filter/envelope samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C75 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C75 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C75 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C75 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C75 prepared reset should restore pristine fixed-bank state");
}

void testC76VocoderChannelBankResetExample()
{
    const auto path = examplePath(
        "c76_vocoder_channel_bank_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C76 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.vocoderChannelBank\"",
        "C76 example should expose its mapped stateful bank");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C76 Vocoder Channel Bank project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8000", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C76 CLI status should succeed");
    requireContains(status.output, "type=talas.vocoderChannelBank",
        "C76 status should expose the mapped Vocoder Channel Bank");
    requireContains(status.output, "critical-path: 0 frames",
        "C76 status should preserve exact zero-latency authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "8000", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C76 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 8000.0);
    constexpr std::array expected{
        0.0,
        0.015851446652144333,
        0.031549347097702171,
        0.044187773620297521,
        0.05508822895578476,
        0.065042592744080946,
        0.070692084117980142,
        0.073147620681535963,
        0.079925776317523956,
        0.089479639601713623,
        0.089141775992458511,
        0.070873744063078864,
        0.051876523334977873,
        0.03530523471875148,
        0.010960843271074196,
        -0.0079753375138554572,
    };
    require(channels.size() == 1,
        "C76 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C76 CLI should publish exact mapped-bank and cascade samples");
    }

    talas::runtime::RenderSettings settings{8000.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C76 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 16> realtime{};
    require(offline.ok() && prepared.ready(),
        "C76 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames :
         std::array<std::size_t, 8>{1, 2, 3, 2, 3, 1, 2, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C76 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C76 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C76 prepared reset should restore pristine mapped-bank and producer state");
}

void testC77WaitUntilBufferedResumeExample()
{
    const auto path = examplePath(
        "c77_wait_until_buffered_resume.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C77 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.waitUntil\"",
        "C77 example should expose its stateful buffered gate");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C77 WaitUntil project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C77 CLI status should succeed");
    requireContains(status.output, "type=talas.waitUntil",
        "C77 status should expose the stateful WaitUntil operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C77 status should publish the wait as timing, not latency");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C77 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto pi = std::acos(-1.0);
    const std::array expected{
        0.0,
        0.0,
        0.0,
        std::sin(pi / 4.0),
        1.0,
        std::sin(3.0 * pi / 4.0),
        std::sin(pi),
        -std::sin(pi / 4.0),
    };
    require(channels.size() == 1,
        "C77 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C77 CLI should publish exact closed-prefix and buffered-replay samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.eventValues = {
        {"C77Resume", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C77Resume", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C77 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C77 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C77 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C77 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C77 prepared reset should restore pristine history, indices, resumed flag, and schedule");
}

void testC78WarpedTimeIndexRestartExample()
{
    const auto path = examplePath(
        "c78_warped_time_index_restart.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C78 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.warpedTimeIndex\"",
        "C78 example should expose its stateful time-map index");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C78 WarpedTimeIndex project should be current-format and self-contained");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C78 CLI status should succeed");
    requireContains(status.output, "type=talas.warpedTimeIndex",
        "C78 status should expose the stateful WarpedTimeIndex operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C78 status should derive exact zero latency through the stateless VCA control ancestry");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "12",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C78 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 12, 8.0);
    const auto rootHalf = std::sqrt(0.5);
    const std::array expected{
        -0.0,
        -rootHalf,
        -1.0,
        -0.75 * rootHalf,
        0.0,
        -0.25 * rootHalf,
        1.0,
        0.75 * rootHalf,
        -0.0,
        -0.25 * rootHalf,
        0.0,
        0.25 * rootHalf,
    };
    require(channels.size() == 1,
        "C78 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C78 CLI should publish exact continuation and restart samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.eventValues = {
        {"C78Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {"C78Rate", 1.0, talas::expr::ValueDomain::Scalar},
        {"C78Morph", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C78Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C78Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {4, "C78Rate", 0.5, talas::expr::ValueDomain::Scalar},
        {4, "C78Morph", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "C78Trigger", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C78 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 12);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 12> realtime{};
    require(offline.ok() && prepared.ready(),
        "C78 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames :
         std::array<std::size_t, 6>{1, 2, 3, 2, 3, 1})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C78 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C78 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C78 prepared reset should restore pristine playback, active, trigger-edge, producer, and schedule state");
}

void testC79WaveshaperPolynomialCurveExample()
{
    const auto path = examplePath(
        "c79_waveshaper_polynomial_curve.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C79 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.waveshaper\"",
        "C79 example should expose its finite stateless Waveshaper");
    requireContains(source, "\"text\": \"0 0.5 0 0.5\"",
        "C79 example should freeze the exact polynomial coefficient order");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C79 Waveshaper project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C79 CLI status should succeed");
    requireContains(status.output, "type=talas.waveshaper",
        "C79 status should expose the Waveshaper operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C79 status should expose exact zero-latency ancestry");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C79 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const auto rootHalf = std::sqrt(0.5);
    const std::array expected{
        0.0,
        0.75 * rootHalf,
        1.0,
        0.75 * rootHalf,
        0.0,
        -0.75 * rootHalf,
        -1.0,
        -0.75 * rootHalf,
    };
    require(channels.size() == 1,
        "C79 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C79 CLI should publish exact finite polynomial samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C79 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C79 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C79 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C79 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{reset}.subspan(offset, frames)),
            "C79 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C79 stateless prepared reset should replay the pristine polynomial result");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C79 replacement should prepare from the same immutable graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(
            replacement.render(
                std::span<double>{replaced}.subspan(offset, frames)),
            "C79 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C79 replacement should replay the exact immutable polynomial snapshot");
}

void testC80IteratedWaveshaperFeedbackExample()
{
    const auto path = examplePath(
        "c80_iterated_waveshaper_feedback.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C80 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.iteratedWaveshaper\"",
        "C80 example should expose its stateless finite feedback operation");
    requireContains(source, "\"expression\": \"3\"",
        "C80 example should freeze three shaping passes");
    requireContains(source, "\"expression\": \"0.75\"",
        "C80 example should freeze exact feedback attenuation");
    requireContains(
        source,
        "49a9d0295e3afe4b0a83837e668c54767fc398143841020dd0fa857bcce8ef00",
        "C80 example should bind the original table hash");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.empty(),
        "C80 project should be current-format, single-resource, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C80 CLI status should succeed");
    requireContains(status.output, "type=talas.iteratedWaveshaper",
        "C80 status should expose the IteratedWaveshaper operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C80 status should expose exact zero-latency ancestry");
    requireContains(
        status.output,
        "Tables/C80IteratedFeedback kind=table generation=1 hash=49a9d0295e3afe4b0a83837e668c54767fc398143841020dd0fa857bcce8ef00 health=verified",
        "C80 status should expose exact immutable table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C80 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.078842836656537352,
        0.234375,
        0.078842836656537352,
        0.0,
        -0.078842836656537352,
        -0.234375,
        -0.078842836656537407,
    };
    require(channels.size() == 1,
        "C80 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-15,
            "C80 CLI should publish exact finite feedback samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.tableResources.push_back({
        "Tables/C80IteratedFeedback", {-1.0, -0.25, 0.0, 0.25, 1.0}});
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C80 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C80 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C80 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C80 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(
            prepared.render(std::span<double>{reset}.subspan(offset, frames)),
            "C80 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C80 stateless reset should replay the immutable table result");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C80 replacement should prepare from the same immutable graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(
            replacement.render(
                std::span<double>{replaced}.subspan(offset, frames)),
            "C80 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C80 replacement should replay the exact immutable table snapshot");
}

void testC81MultiplyingWaveshaperProductExample()
{
    const auto path = examplePath(
        "c81_multiplying_waveshaper_product.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C81 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.multiplyingWaveshaper\"",
        "C81 example should expose its stateless finite product operation");
    requireContains(source, "\"expression\": \"0.75\"",
        "C81 example should freeze exact connected product attenuation");
    requireContains(
        source,
        "a8d1cd6f8b7d7d2b6221a7d9d5c68f47f35dd31f84691c2a31d4470b6738909a",
        "C81 example should bind the original table hash");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.empty(),
        "C81 project should be current-format, single-resource, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C81 CLI status should succeed");
    requireContains(status.output, "type=talas.multiplyingWaveshaper",
        "C81 status should expose the MultiplyingWaveshaper operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C81 status should expose exact aligned zero-latency ancestry");
    requireContains(status.output, "latency-warnings: none",
        "C81 status should contain no unknown or uncompensated ancestry");
    requireContains(
        status.output,
        "Tables/C81MultiplyingProduct kind=table generation=1 hash=a8d1cd6f8b7d7d2b6221a7d9d5c68f47f35dd31f84691c2a31d4470b6738909a health=verified",
        "C81 status should expose exact immutable table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C81 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.5303300858899106,
        9.1848509936051484e-17,
        -0.53033008588991071,
        0.0,
        0.1875,
        2.2962127484012871e-17,
        -0.1875,
    };
    require(channels.size() == 1,
        "C81 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-15,
            "C81 CLI should publish exact finite ordered-product samples");
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.tableResources.push_back({
        "Tables/C81MultiplyingProduct", {0.25, 0.5, 1.0}});
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C81 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C81 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(
            prepared.render(
                std::span<double>{realtime}.subspan(offset, frames)),
            "C81 irregular prepared partition should render at offset "
                + std::to_string(offset) + " for "
                + std::to_string(frames) + " frames");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C81 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(
            prepared.render(std::span<double>{reset}.subspan(offset, frames)),
            "C81 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C81 stateless reset should replay the immutable ordered product");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C81 replacement should prepare from the same immutable graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(
            replacement.render(
                std::span<double>{replaced}.subspan(offset, frames)),
            "C81 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C81 replacement should replay the exact immutable table snapshot");
}

void testC82InputOutputCharacteristicCurveExample()
{
    const auto path = examplePath(
        "c82_input_output_characteristic_curve.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C82 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.inputOutputCharacteristic\"",
        "C82 example should expose its stateless finite curve operation");
    requireContains(source, "\"text\": \"-1 -0.5 0 0.5 1\"",
        "C82 example should freeze exact static input breakpoints");
    requireContains(source, "\"expression\": \"0.75\"",
        "C82 example should freeze the linear/Hermite blend");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C82 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C82 CLI status should succeed");
    requireContains(status.output, "type=talas.inputOutputCharacteristic",
        "C82 status should expose the InputOutputCharacteristic operation");
    requireContains(status.output, "critical-path: 0 frames",
        "C82 status should expose exact zero-latency ancestry");
    requireContains(status.output, "latency-warnings: none",
        "C82 status should contain no unknown latency");
    requireContains(status.output, "resources: none",
        "C82 status should expose its no-resource contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C82 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.75,
        0.10526246624763577,
        1.0,
        0.10526246624763627,
        0.74999999999999978,
        -0.6009514805718601,
        -1.0,
        -0.60095148057186054,
    };
    require(channels.size() == 1,
        "C82 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-15,
            "C82 CLI should publish exact finite transfer-curve samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C82 curve example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C82 curve example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C82 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C82 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C82 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C82 stateless reset should replay the immutable curve");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C82 replacement should prepare from the same immutable graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C82 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C82 replacement should replay the exact immutable curve snapshot");
}

void testC83ScaleAndOffsetAffineControlExample()
{
    const auto path = examplePath(
        "c83_scale_and_offset_affine_control.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C83 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.scaleAndOffset\"",
        "C83 example should expose its stateless finite affine operation");
    requireContains(source, "\"toPort\": \"scale\"",
        "C83 example should connect a per-frame scale control");
    requireContains(source, "\"toPort\": \"offset\"",
        "C83 example should connect a per-frame offset control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C83 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C83 CLI status should succeed");
    requireContains(status.output, "type=talas.scaleAndOffset",
        "C83 status should expose the ScaleAndOffset operation");
    requireContains(status.output, "resources: none",
        "C83 status should expose its no-resource product slice");
    requireContains(status.output, "critical-path: 0 frames",
        "C83 status should derive exact zero latency through the stateless VCA terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C83 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.66291260736238822,
        0.5,
        0.13258252147247768,
        0.0,
        0.044194173824159216,
        -0.0,
        -0.13258252147247768,
    };
    require(channels.size() == 1,
        "C83 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C83 CLI should publish exact connected affine samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C83 affine example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C83 affine example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C83 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C83 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C83 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C83 reset should replay the connected affine control graph exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C83 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C83 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C83 replacement should replay the exact connected affine graph");
}

void testC84InterpolateDualLaneControlExample()
{
    const auto path = examplePath(
        "c84_interpolate_dual_lane_control.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C84 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.interpolate\"",
        "C84 example should expose its stateless finite interpolation");
    requireContains(source, "\"toPort\": \"amount\"",
        "C84 example should connect its left amount control");
    requireContains(source, "\"toPort\": \"rightAmount\"",
        "C84 example should connect its right amount control");
    requireContains(source, "\"fromPort\": \"value\"",
        "C84 example should consume the left interpolation lane");
    requireContains(source, "\"fromPort\": \"rightValue\"",
        "C84 example should consume the right interpolation lane");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C84 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C84 CLI status should succeed");
    requireContains(status.output, "type=talas.interpolate",
        "C84 status should expose the Interpolate operation");
    requireContains(status.output, "resources: none",
        "C84 status should expose its no-resource product slice");
    requireContains(status.output, "critical-path: 0 frames",
        "C84 status should derive exact zero latency through the stateless VCA terminal");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C84 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        -0.0,
        -0.2651650429449553,
        0.25,
        0.61871843353822908,
        1.8369701987210297e-16,
        -1.5026019100214134,
        -2.75,
        -2.3864853865045985,
    };
    require(channels.size() == 1,
        "C84 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C84 CLI should publish exact connected dual-lane samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C84 interpolation example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C84 interpolation example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C84 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C84 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C84 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C84 reset should replay the connected interpolation graph exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C84 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C84 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C84 replacement should replay the exact connected interpolation graph");
}

void testC85RunningMaxResetControlExample()
{
    const auto path = examplePath(
        "c85_running_max_reset_control.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C85 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.runningMax\"",
        "C85 example should expose its finite session Running Max");
    requireContains(source, "\"toPort\": \"reset\"",
        "C85 example should connect its one-frame reset control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C85 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C85 CLI status should succeed");
    requireContains(status.output, "type=talas.runningMax",
        "C85 status should expose the Running Max operation");
    requireContains(status.output, "resources: none",
        "C85 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C85 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.12499999999999997,
        0.25,
        0.17677669529663689,
        3.749399456654644e-33,
        -2.1648901405887329e-17,
        -3.061616997868383e-17,
        -2.1648901405887338e-17,
    };
    require(channels.size() == 1,
        "C85 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C85 CLI should publish exact connected-reset samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C85 Running Max example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C85 Running Max example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C85 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C85 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C85 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C85 transport reset should replay Running Max exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C85 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C85 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C85 replacement should replay Running Max exactly");
}

void testC86RunningMinResetBaselineExample()
{
    const auto path = examplePath(
        "c86_running_min_reset_baseline.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C86 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.runningMin\"",
        "C86 example should expose its finite session Running Min");
    requireContains(source, "\"toPort\": \"maximumAmplitude\"",
        "C86 example should connect its changing maximum-amplitude baseline");
    requireContains(source, "\"toPort\": \"reset\"",
        "C86 example should connect its one-frame reset control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C86 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C86 CLI status should succeed");
    requireContains(status.output, "type=talas.runningMin",
        "C86 status should expose the Running Min operation");
    requireContains(status.output, "resources: none",
        "C86 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C86 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.0,
        0.125,
        0.088388347648318447,
        3.749399456654644e-33,
        0.12499999999999997,
        0.25,
        0.17677669529663692,
    };
    require(channels.size() == 1,
        "C86 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C86 CLI should publish exact connected-baseline reset samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C86 Running Min example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C86 Running Min example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C86 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C86 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C86 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C86 transport reset should replay Running Min exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C86 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C86 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C86 replacement should replay Running Min exactly");
}

void testC87RmsSquaredEnvelopeExample()
{
    const auto path = examplePath("c87_rms_squared_envelope.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C87 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.rmsSquared\"",
        "C87 example should expose its finite session RMS Squared node");
    requireContains(source, "\"toPort\": \"timeConstant\"",
        "C87 example should connect its changing smoothing control");
    requireContains(source, "\"toPort\": \"scale\"",
        "C87 example should connect its changing scale control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C87 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C87 CLI status should succeed");
    requireContains(status.output, "type=talas.rmsSquared",
        "C87 status should expose the RMS Squared operation");
    requireContains(status.output, "resources: none",
        "C87 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C87 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.02045143231296143,
        0.077554118106040876,
        0.092540677277517927,
        0.069429066214552762,
        0.061535200197866409,
        0.052491200477206158,
        0.025443551713012543,
    };
    require(channels.size() == 1,
        "C87 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C87 CLI should publish exact connected squared-envelope samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C87 RMS Squared example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C87 RMS Squared example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C87 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C87 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C87 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C87 transport reset should replay RMS Squared exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C87 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C87 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C87 replacement should replay RMS Squared exactly");
}

void testC88AmplitudeFollowerEnvelopeExample()
{
    const auto path = examplePath("c88_amplitude_follower_envelope.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C88 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.amplitudeFollower\"",
        "C88 example should expose its finite session Amplitude Follower node");
    requireContains(source, "\"toPort\": \"timeConstant\"",
        "C88 example should connect its changing smoothing control");
    requireContains(source, "\"toPort\": \"scale\"",
        "C88 example should connect its changing scale control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C88 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C88 CLI status should succeed");
    requireContains(status.output, "type=talas.amplitudeFollower",
        "C88 status should expose the Amplitude Follower operation");
    requireContains(status.output, "resources: none",
        "C88 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C88 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.030850872476741776,
        0.096818402558635108,
        0.12867046332385643,
        0.096535603377732496,
        0.095490338110660114,
        0.082273560591981557,
        0.043080301192806834,
    };
    require(channels.size() == 1,
        "C88 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C88 CLI should publish exact connected absolute-envelope samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C88 Amplitude Follower example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C88 Amplitude Follower example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C88 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C88 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C88 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C88 transport reset should replay Amplitude Follower exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C88 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C88 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C88 replacement should replay Amplitude Follower exactly");
}

void testC89PeakDetectorEnvelopeExample()
{
    const auto path = examplePath("c89_peak_detector_envelope.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C89 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.peakDetector\"",
        "C89 example should expose its finite session Peak Detector node");
    requireContains(source, "\"toPort\": \"attackTime\"",
        "C89 example should connect its changing attack control");
    requireContains(source, "\"toPort\": \"releaseTime\"",
        "C89 example should connect its changing release control");
    requireContains(source, "\"toPort\": \"scale\"",
        "C89 example should connect its changing scale control");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C89 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C89 CLI status should succeed");
    requireContains(status.output, "type=talas.peakDetector",
        "C89 status should expose the Peak Detector operation");
    requireContains(status.output, "resources: none",
        "C89 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "8",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C89 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 8, 8.0);
    const std::array expected{
        0.0,
        0.030850872476741776,
        0.096818402558635108,
        0.12867046332385643,
        0.11954741345453088,
        0.11094829923948629,
        0.091197955659981775,
        0.048056073671322259,
    };
    require(channels.size() == 1,
        "C89 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C89 CLI should publish exact connected attack/release samples");
    }

    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings{8.0, 3});
    require(compiled.ok(), "C89 Peak Detector example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C89 Peak Detector example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 4>{2, 1, 3, 2})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C89 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C89 prepared partitions should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 8> reset{};
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 3, 2})
    {
        require(prepared.render(
            std::span<double>{reset}.subspan(offset, frames)),
            "C89 reset replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(reset, offline.mono),
        "C89 transport reset should replay Peak Detector exactly");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 8> replaced{};
    require(replacement.ready(),
        "C89 replacement should prepare from the same graph");
    offset = 0;
    for (const auto frames : std::array<std::size_t, 3>{3, 2, 3})
    {
        require(replacement.render(
            std::span<double>{replaced}.subspan(offset, frames)),
            "C89 replacement replay should respect the prepared maximum block size");
        offset += frames;
    }
    require(std::ranges::equal(replaced, offline.mono),
        "C89 replacement should replay Peak Detector exactly");
}

void testC90FrequencyTrackerHistoryExample()
{
    const auto path = examplePath(
        "c90_frequency_tracker_history.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C90 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.frequencyTracker\"",
        "C90 example should expose its bounded Frequency Tracker node");
    requireContains(source, "\"toPort\": \"minFrequency\"",
        "C90 example should connect its selected minimum frequency");
    requireContains(source, "\"toPort\": \"maxFrequency\"",
        "C90 example should connect its selected maximum frequency");
    requireContains(source, "\"toPort\": \"confidence\"",
        "C90 example should connect its confidence gate");
    requireContains(source, "\"toPort\": \"scale\"",
        "C90 example should connect its output scale");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C90 project should be current-format, resource-free, and schedule-free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "64", "--max-block-frames", "64",
    });
    require(
        status.exitCode == 0 && status.error.empty(),
        "C90 CLI status should succeed");
    requireContains(status.output, "type=talas.frequencyTracker",
        "C90 status should expose the Frequency Tracker operation");
    requireContains(status.output, "resources: none",
        "C90 status should expose its no-resource product slice");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "256",
        "--sample-rate", "64", "--max-block-frames", "64",
        "--format", "json",
    });
    require(
        rendered.exitCode == 0 && rendered.error.empty(),
        "C90 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 256, 64.0);
    // The first estimate needs two periods of the 1 hz minimum, 128 frames;
    // it stands whatever its confidence and holds after that.
    require(
        channels.size() == 1
            && std::ranges::count_if(
                   channels[0].samples,
                   [](double sample) { return sample != 0.0; }) == 129
            && std::ranges::find_if(
                   channels[0].samples,
                   [](double sample) { return sample != 0.0; })
                == channels[0].samples.begin() + 127,
        "C90 CLI samples should acquire the first estimate at frame 127 and hold it");
    for (const auto [frame, value] : {
             std::pair{std::size_t{127}, -0.022945524795635452},
             std::pair{std::size_t{160}, 0.469359917522355},
             std::pair{std::size_t{255}, -0.023027831316168212},
         })
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C90 CLI should publish exact gated/scaled sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{64.0, 64};
    settings.maxRealtimeHistoryFrames = 128;
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(
        compiled.ok(),
        "C90 Frequency Tracker example should compile");

    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    std::array<double, 256> expected{};
    for (std::size_t block = 0; block < 4; ++block)
    {
        const auto rendered = offline.render(64);
        require(rendered.ok() && rendered.mono.size() == 64,
            "C90 offline product window should render");
        std::ranges::copy(
            rendered.mono, expected.begin()
                + static_cast<std::ptrdiff_t>(block * 64));
    }

    const auto renderWindows = [](talas::runtime::RealtimeRenderSession& session,
                                   std::array<double, 256>& destination) {
        for (std::size_t block = 0; block < 4; ++block)
        {
            if (!session.render(std::span<double>{destination}.subspan(block * 64, 64)))
                return false;
        }
        return true;
    };
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 256> realtime{};
    require(
        prepared.ready() && renderWindows(prepared, realtime),
        "C90 four prepared callback windows should render: "
            + std::string{prepared.error()});
    require(
        std::ranges::equal(realtime, expected),
        "C90 prepared Frequency Tracker windows should be bit-exact offline");
    require(
        std::ranges::all_of(
            std::span<const double>{realtime}.first(127),
            [](double sample) { return sample == 0.0; }),
        "C90 should remain silent before its history fills");
    require(
        std::ranges::none_of(
            std::span<const double>{realtime}.subspan(127),
            [](double sample) { return sample == 0.0; }),
        "C90 should hold its first estimate once acquired");

    prepared.resetTransport();
    std::array<double, 256> reset{};
    require(
        renderWindows(prepared, reset) && reset == realtime,
        "C90 transport reset should replay the exact acquisition windows");
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    std::array<double, 256> replaced{};
    require(
        replacement.ready() && renderWindows(replacement, replaced)
            && replaced == realtime,
        "C90 replacement should begin with exact empty Frequency Tracker history");
}
}

void testC91ArEnvelopeStateExample()
{
    const auto path = examplePath("c91_ar_envelope_state.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C91 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.ar\"",
        "C91 example should expose the checked linear AR node");
    requireContains(source, "\"expression\": \"!C91Gate\"",
        "C91 example should expose its bounded dynamic gate");
    requireContains(source, "\"choice\": \"linear\"",
        "C91 example should freeze the admitted linear subset");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C91 project should be current, resource-free, and four-event bounded");
    requireOrdinaryValidation(path);
    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C91 CLI status should succeed");
    requireContains(status.output, "type=talas.ar",
        "C91 status should expose AR");
    requireContains(status.output, "resources: none",
        "C91 status should expose no resources");
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "32",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C91 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 32, 16.0);
    require(channels.size() == 1, "C91 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{1}, 0.035355339059327376},
             std::pair{std::size_t{9}, 0.11785113019775792},
             std::pair{std::size_t{13}, -0.023570226039551577},
             std::pair{std::size_t{16}, 0.0},
         })
    {
        require(std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C91 exact AR product sample " + std::to_string(frame));
    }
    require(std::ranges::equal(
                std::span<const double>{channels[0].samples}.first(16),
                std::span<const double>{channels[0].samples}.subspan(16)),
        "C91 periodic retrigger should repeat bit-exactly");

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C91Gate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C91Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {16, "C91Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {24, "C91Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C91 AR product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C91 lifecycle fixtures should be ready");
    std::array<double, 32> expected{};
    std::array<double, 32> actual{};
    constexpr std::array<std::size_t, 5> partitions{5, 3, 8, 7, 9};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C91 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C91 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected, "C91 prepared should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C91 reset/replacement should replay exact empty AR state");
}

void testC92AdsrTypedGateStateExample()
{
    const auto path = examplePath("c92_adsr_typed_gate_state.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C92 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.adsr\"",
        "C92 example should expose the checked linear ADSR node");
    requireContains(source, "\"expression\": \"!C92Gate\"",
        "C92 example should expose its bounded dynamic gate");
    requireContains(source, "\"expression\": \"!C92Legato\"",
        "C92 example should expose bounded live legato reattack");
    requireContains(source, "\"choice\": \"linear\"",
        "C92 example should freeze the admitted linear subset");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 12,
        "C92 project should be current, resource-free, and twelve-event bounded");
    requireOrdinaryValidation(path);
    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C92 CLI status should succeed");
    requireContains(status.output, "type=talas.adsr",
        "C92 status should expose ADSR");
    requireContains(status.output, "resources: none",
        "C92 status should expose no resources");
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C92 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 48, 16.0);
    require(channels.size() == 1, "C92 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{1}, 0.0095670858091272456},
             std::pair{std::size_t{11}, -0.13627223104541478},
             std::pair{std::size_t{16}, 0.0},
             std::pair{std::size_t{28}, -0.13125000000000001},
             std::pair{std::size_t{36}, -0.10078125},
             std::pair{std::size_t{40}, 2.5726867491643244e-16},
             std::pair{std::size_t{47}, 0.011958857261408919},
         })
    {
        require(std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C92 exact ADSR product sample " + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C92Sustain", 0.3, talas::expr::ValueDomain::Scalar},
        {"C92Scale", 0.8, talas::expr::ValueDomain::Scalar},
        {"C92Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {"C92Legato", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {12, "C92Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {16, "C92Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {24, "C92Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {24, "C92Legato", 1.0, talas::expr::ValueDomain::Scalar},
        {28, "C92Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {36, "C92Sustain", 0.5, talas::expr::ValueDomain::Scalar},
        {36, "C92Scale", -0.6, talas::expr::ValueDomain::Scalar},
        {40, "C92Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C92 ADSR product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C92 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 7> partitions{5, 7, 4, 8, 3, 9, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C92 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C92 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected, "C92 prepared should be bit-exact offline");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C92 reset/replacement should replay exact empty ADSR and event state");
}

void testC93PulseTrainPhaseStateExample()
{
    const auto path = examplePath("c93_pulse_train_phase_state.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C93 example should be persisted in format 43");
    requireContains(source, "\"id\": \"fixedPulse\"",
        "C93 example should expose its one-sample Pulse Train lane");
    requireContains(source, "\"id\": \"widePulse\"",
        "C93 example should expose its variable-duty Pulse Train lane");
    requireContains(source, "\"expression\": \"!C93FixedPeriod\"",
        "C93 example should expose bounded live period control");
    requireContains(source, "\"expression\": \"!C93IgnoredDuty\"",
        "C93 example should expose ignored connected duty authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 14,
        "C93 project should be current, resource-free, and fourteen-event bounded");
    requireOrdinaryValidation(path);
    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C93 CLI status should succeed");
    requireContains(status.output, "type=talas.pulseTrain",
        "C93 status should expose Pulse Train");
    requireContains(status.output, "resources: none",
        "C93 status should expose no resources");
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C93 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 48, 16.0);
    require(channels.size() == 1, "C93 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{1}, 0.039018064403225652},
             std::pair{std::size_t{7}, 0.1961570560806461},
             std::pair{std::size_t{8}, 2.4492935982947065e-17},
             std::pair{std::size_t{16}, 2.0212861992297212e-16},
             std::pair{std::size_t{24}, -0.20000000000000034},
             std::pair{std::size_t{36}, 0.14142135623731014},
             std::pair{std::size_t{40}, 0.19999999999999932},
             std::pair{std::size_t{44}, -0.058578643762691118},
             std::pair{std::size_t{47}, 0.039018064403224674},
         })
    {
        require(std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C93 exact Pulse Train product sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C93FixedPeriod", 0.5, talas::expr::ValueDomain::Scalar},
        {"C93FixedGate", 1.0, talas::expr::ValueDomain::Scalar},
        {"C93IgnoredDuty", 1.5, talas::expr::ValueDomain::Scalar},
        {"C93WidePeriod", 1.0, talas::expr::ValueDomain::Scalar},
        {"C93WideDuty", 0.5, talas::expr::ValueDomain::Scalar},
        {"C93WideGate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {12, "C93FixedGate", 0.0, talas::expr::ValueDomain::Scalar},
        {12, "C93WidePeriod", 0.5, talas::expr::ValueDomain::Scalar},
        {16, "C93FixedGate", 1.0, talas::expr::ValueDomain::Scalar},
        {20, "C93WideGate", 0.0, talas::expr::ValueDomain::Scalar},
        {24, "C93WideGate", 1.0, talas::expr::ValueDomain::Scalar},
        {32, "C93WideDuty", 0.0, talas::expr::ValueDomain::Scalar},
        {36, "C93WideDuty", 1.0, talas::expr::ValueDomain::Scalar},
        {40, "C93FixedPeriod", 0.25, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C93 Pulse Train product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C93 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 9> partitions{
        5, 7, 4, 4, 4, 8, 4, 4, 8};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C93 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C93 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C93 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C93 reset/replacement should replay exact initial phase and Event Values");
}

void testC94PulseGeneratorPhaseStateExample()
{
    const auto path = examplePath(
        "c94_pulse_generator_phase_state.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C94 example should be persisted in format 43");
    requireContains(source, "\"id\": \"linearPulse\"",
        "C94 example should expose its linear PolyBLEP lane");
    requireContains(source, "\"id\": \"hardPulse\"",
        "C94 example should expose its hard-edge lane");
    requireContains(source, "\"id\": \"fmPulse\"",
        "C94 example should expose its bounded FM lane");
    requireConnection(source, {"fmModulator", "audioOut", "fmPulse", "modulator"},
        "C94 example should connect its audio-rate frequency modulator");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 20,
        "C94 project should be current, resource-free, and twenty-event bounded");
    requireOrdinaryValidation(path);
    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C94 CLI status should succeed");
    requireContains(status.output, "type=talas.pulseGenerator",
        "C94 status should expose Pulse Generator");
    requireContains(status.output, "resources: none",
        "C94 status should expose no resources");
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C94 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 48, 16.0);
    require(channels.size() == 1, "C94 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.25},
             std::pair{std::size_t{1}, 0.54994332859730644},
             std::pair{std::size_t{8}, 0.050000000000000017},
             std::pair{std::size_t{12}, -0.25},
             std::pair{std::size_t{16}, 0.049999999999999989},
             std::pair{std::size_t{18}, 0.14999999999999999},
             std::pair{std::size_t{24}, -0.074999999999999983},
             std::pair{std::size_t{32}, 0.12499999999999997},
             std::pair{std::size_t{36}, 0.12500000000000003},
             std::pair{std::size_t{45}, -0.12291235784563946},
             std::pair{std::size_t{47}, -0.27500000000000002},
         })
    {
        require(std::bit_cast<std::uint64_t>(
                    channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C94 exact Pulse Generator product sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C94LinearFrequency", 2.0,
            talas::expr::ValueDomain::Scalar},
        {"C94LinearEnvelope", 0.2,
            talas::expr::ValueDomain::Scalar},
        {"C94LinearDuty", 0.25,
            talas::expr::ValueDomain::Scalar},
        {"C94LinearReset", 0.0,
            talas::expr::ValueDomain::Scalar},
        {"C94HardFrequency", 1.0,
            talas::expr::ValueDomain::Scalar},
        {"C94HardEnvelope", 0.15,
            talas::expr::ValueDomain::Scalar},
        {"C94HardDuty", 0.5,
            talas::expr::ValueDomain::Scalar},
        {"C94HardReset", 0.0,
            talas::expr::ValueDomain::Scalar},
        {"C94FmFrequency", 0.75,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C94LinearFrequency", 1.0,
            talas::expr::ValueDomain::Scalar},
        {12, "C94LinearEnvelope", 0.0,
            talas::expr::ValueDomain::Scalar},
        {14, "C94LinearDuty", 0.0,
            talas::expr::ValueDomain::Scalar},
        {16, "C94LinearReset", -1.0,
            talas::expr::ValueDomain::Scalar},
        {17, "C94LinearReset", 0.0,
            talas::expr::ValueDomain::Scalar},
        {18, "C94LinearDuty", 0.25,
            talas::expr::ValueDomain::Scalar},
        {18, "C94LinearEnvelope", 0.2,
            talas::expr::ValueDomain::Scalar},
        {24, "C94HardDuty", 0.25,
            talas::expr::ValueDomain::Scalar},
        {32, "C94HardReset", 1.0,
            talas::expr::ValueDomain::Scalar},
        {33, "C94HardReset", 0.0,
            talas::expr::ValueDomain::Scalar},
        {36, "C94FmFrequency", 1.25,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(),
        "C94 Pulse Generator product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C94 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 12> partitions{
        5, 3, 4, 2, 2, 1, 1, 6, 8, 1, 3, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C94 offline irregular partition should render");
        std::ranges::copy(
            block.mono,
            expected.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C94 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C94 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset,
                std::span<const double>{expected}.first(16)),
        "C94 reset/replacement should replay exact initial phase and Event Values");
}

void testC95OscillatorPhaseStateExample()
{
    const auto path = examplePath("c95_oscillator_phase_state.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C95 example should be persisted in format 43");
    requireContains(source, "\"id\": \"sine\"",
        "C95 example should expose its sine fallback lane");
    requireContains(source, "\"id\": \"linearTable\"",
        "C95 example should expose its linear wavetable lane");
    requireContains(source, "\"id\": \"nearestTable\"",
        "C95 example should expose its nearest-sample wavetable lane");
    requireContains(source,
        "0c90321d461578d7b1dc4ccf4b2f33489dca9dc5f7e0cb168ae41be123a7ee3e",
        "C95 example should bind the exact immutable cyclic table hash");
    require(readText(path.parent_path()
                / "resources/c95-oscillator-cyclic-table.txt")
            == "-1\n-0.25\n0.25\n1\n",
        "C95 checked product should retain exact cyclic table bytes");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.size() == 20,
        "C95 project should be current, single-resource, and twenty-event bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C95 CLI status should succeed");
    requireContains(status.output, "type=talas.oscillator",
        "C95 status should expose Oscillator");
    requireContains(status.output,
        "Tables/C95Oscillator kind=table generation=1 hash=0c90321d461578d7b1dc4ccf4b2f33489dca9dc5f7e0cb168ae41be123a7ee3e health=verified",
        "C95 status should expose exact immutable table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C95 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 48, 16.0);
    require(channels.size() == 1, "C95 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, -0.20000000000000001},
             std::pair{std::size_t{8}, -0.016305168369723368},
             std::pair{std::size_t{12}, 0.084778553617477798},
             std::pair{std::size_t{16}, 0.22698410354203308},
             std::pair{std::size_t{24}, 0.1614213562373093},
             std::pair{std::size_t{32}, -0.1780598555606597},
             std::pair{std::size_t{36}, 0.037103982183308404},
             std::pair{std::size_t{47}, 0.13711523540205289},
         })
    {
        require(std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C95 exact Oscillator product sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.tableResources = {
        {"Tables/C95Oscillator", {-1.0, -0.25, 0.25, 1.0}},
    };
    settings.eventValues = {
        {"C95SineFrequency", 1.0, talas::expr::ValueDomain::Scalar},
        {"C95SineAmplitude", 0.2, talas::expr::ValueDomain::Scalar},
        {"C95SinePitchBend", 0.0, talas::expr::ValueDomain::Scalar},
        {"C95SineReset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C95LinearFrequency", 0.75, talas::expr::ValueDomain::Scalar},
        {"C95LinearEnvelope", 0.12, talas::expr::ValueDomain::Scalar},
        {"C95LinearReset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C95NearestFrequency", 0.5, talas::expr::ValueDomain::Scalar},
        {"C95NearestReset", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C95SinePitchBend", 0.5, talas::expr::ValueDomain::Scalar},
        {12, "C95SineReset", 1.0, talas::expr::ValueDomain::Scalar},
        {13, "C95SineReset", 0.0, talas::expr::ValueDomain::Scalar},
        {16, "C95LinearFrequency", 1.25, talas::expr::ValueDomain::Scalar},
        {20, "C95LinearEnvelope", 0.06, talas::expr::ValueDomain::Scalar},
        {24, "C95LinearReset", 1.0, talas::expr::ValueDomain::Scalar},
        {25, "C95LinearReset", 0.0, talas::expr::ValueDomain::Scalar},
        {28, "C95NearestFrequency", 1.25, talas::expr::ValueDomain::Scalar},
        {32, "C95NearestReset", 1.0, talas::expr::ValueDomain::Scalar},
        {33, "C95NearestReset", 0.0, talas::expr::ValueDomain::Scalar},
        {36, "C95SineAmplitude", 0.1, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C95 Oscillator product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C95 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 12> partitions{
        5, 3, 4, 2, 2, 1, 1, 6, 8, 1, 3, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C95 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C95 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C95 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C95 reset/replacement should replay exact initial phase and Event Values");
}

void testC96LfoPhaseTransportExample()
{
    const auto path = examplePath("c96_lfo_phase_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C96 example should be persisted in format 35");
    requireContains(source, "\"id\": \"lfo\"",
        "C96 example should expose its LFO control source");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 10,
        "C96 project should be current, resource-free, and ten-event bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C96 CLI status should succeed");
    requireContains(status.output, "type=talas.lfo",
        "C96 status should expose LFO");
    requireContains(status.output, "resources: none",
        "C96 status should expose its resource-free contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C96 CLI render should succeed");
    const auto channels = parseRenderedChannels(rendered.output, 48, 16.0);
    require(channels.size() == 1, "C96 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{8}, 0.0},
             std::pair{std::size_t{12}, 1.9900510486144492e-17},
             std::pair{std::size_t{16}, 0.0},
             std::pair{std::size_t{24}, 0.0},
             std::pair{std::size_t{32}, 0.0},
             std::pair{std::size_t{47}, -0.095319206108272811},
         })
    {
        require(std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C96 exact LFO product sample " + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C96LfoFrequency", 0.5, talas::expr::ValueDomain::Scalar},
        {"C96LfoMinimum", 0.2, talas::expr::ValueDomain::Scalar},
        {"C96LfoMaximum", 0.8, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C96LfoFrequency", 0.0, talas::expr::ValueDomain::Scalar},
        {12, "C96LfoMinimum", 0.35, talas::expr::ValueDomain::Scalar},
        {12, "C96LfoMaximum", 0.65, talas::expr::ValueDomain::Scalar},
        {16, "C96LfoFrequency", 1.0, talas::expr::ValueDomain::Scalar},
        {24, "C96LfoMinimum", 0.1, talas::expr::ValueDomain::Scalar},
        {24, "C96LfoMaximum", 0.9, talas::expr::ValueDomain::Scalar},
        {32, "C96LfoFrequency", 0.25, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C96 LFO product should compile");
    const auto lfoStep = std::ranges::find(
        compiled.graph->plan().steps,
        std::string{"talas.lfo"},
        &talas::runtime::CompileStep::typeId);
    require(
        lfoStep != compiled.graph->plan().steps.end()
            && lfoStep->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && lfoStep->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && lfoStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && lfoStep->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter,
        "C182 C96 LFO product should compile with exact promoted authority");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C96 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 12> partitions{
        5, 3, 4, 2, 2, 1, 1, 6, 8, 1, 3, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C96 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C96 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C96 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C96 reset/replacement should replay exact initial phase and Event Values");
}

void testC97RampTriggerTransportExample()
{
    const auto path = examplePath(
        "c97_ramp_trigger_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C97 example should be persisted in format 35");
    for (const auto variant : {
             std::string_view{"\"choice\": \"ramp\""},
             std::string_view{"\"choice\": \"fullRamp\""},
             std::string_view{"\"choice\": \"repeatingRamp\""},
             std::string_view{"\"choice\": \"repeatingFullRamp\""},
         })
    {
        requireContains(source, variant,
            "C97 example should expose every admitted Ramp variant");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 15,
        "C97 project should be current, resource-free, and fifteen-event bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C97 CLI status should succeed");
    requireContains(status.output, "type=talas.ramp",
        "C97 status should expose Ramp");
    requireContains(status.output, "resources: none",
        "C97 status should expose its resource-free contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C97 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 48, 16.0);
    require(channels.size() == 1,
        "C97 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, -0.0},
             std::pair{std::size_t{1},
                 -0.11490485194281397},
             std::pair{std::size_t{8}, 0.0},
             std::pair{std::size_t{15},
                 -0.25632620818012353},
             std::pair{std::size_t{20},
                 -2.4492935982947065e-17},
             std::pair{std::size_t{24}, 0.0},
             std::pair{std::size_t{32}, 0.0},
             std::pair{std::size_t{47},
                 -0.17677669529663692},
         })
    {
        require(std::bit_cast<std::uint64_t>(
                    channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C97 exact Ramp product sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C97Duration", 1.0, talas::expr::ValueDomain::Scalar},
        {"C97RampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {"C97FullRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {"C97RepeatingRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {"C97RepeatingFullRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C97Duration", 0.25,
            talas::expr::ValueDomain::Scalar},
        {16, "C97RampTrigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {16, "C97FullRampTrigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {16, "C97RepeatingRampTrigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {16, "C97RepeatingFullRampTrigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {20, "C97RampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {20, "C97FullRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {20, "C97RepeatingRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {20, "C97RepeatingFullRampTrigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {28, "C97Duration", 0.5,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C97 Ramp product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C97 lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 12> partitions{
        5, 3, 4, 2, 2, 1, 1, 6, 8, 1, 3, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C97 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C97 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C97 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C97 reset/replacement should replay exact initial trigger and duration state");
}

void testC98SampleAndHoldClockTransportExample()
{
    const auto path = examplePath(
        "c98_sample_and_hold_clock_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C98 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.sampleAndHold\"",
        "C98 example should expose periodic Sample And Hold");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C98 project should be current, resource-free, and eight-event bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "16",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C98 CLI status should succeed");
    requireContains(status.output, "type=talas.sampleAndHold",
        "C98 status should expose Sample And Hold");
    requireContains(status.output, "resources: none",
        "C98 status should expose its resource-free contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "48",
        "--sample-rate", "16", "--max-block-frames", "16",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C98 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 48, 16.0);
    require(channels.size() == 1,
        "C98 should publish one channel");
    for (const auto [frame, value] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{4},
                 2.4492935982947068e-18},
             std::pair{std::size_t{5},
                 -0.014142135623730951},
             std::pair{std::size_t{8}, 0.0},
             std::pair{std::size_t{15},
                 -0.049497474683058339},
             std::pair{std::size_t{22}, -0.125},
             std::pair{std::size_t{31},
                 -0.11667261889578037},
             std::pair{std::size_t{47},
                 -0.12727922061357858},
         })
    {
        require(std::bit_cast<std::uint64_t>(
                    channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(value),
            "C98 exact Sample And Hold product sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 16};
    settings.eventValues = {
        {"C98DirectHold", 0.0,
            talas::expr::ValueDomain::Scalar},
        {"C98ConnectedHold", 0.25,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {8, "C98DirectHold", -0.25,
            talas::expr::ValueDomain::Scalar},
        {12, "C98ConnectedHold", 0.5,
            talas::expr::ValueDomain::Scalar},
        {16, "C98DirectHold", 0.5,
            talas::expr::ValueDomain::Scalar},
        {20, "C98ConnectedHold", 0.125,
            talas::expr::ValueDomain::Scalar},
        {28, "C98DirectHold", -0.125,
            talas::expr::ValueDomain::Scalar},
        {32, "C98ConnectedHold", 0.75,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "C98 Sample And Hold product should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C98 Sample And Hold lifecycle fixtures should be ready");
    std::array<double, 48> expected{};
    std::array<double, 48> actual{};
    constexpr std::array<std::size_t, 12> partitions{
        5, 3, 4, 2, 2, 1, 1, 6, 8, 1, 3, 12};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C98 offline irregular partition should render");
        std::ranges::copy(block.mono,
            expected.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{actual}.subspan(offset, frames)),
            "C98 prepared irregular partition should render");
        offset += frames;
    }
    require(actual == expected,
        "C98 prepared should be bit-exact offline across partitions");
    prepared.resetTransport();
    std::array<double, 16> reset{};
    std::array<double, 16> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(16)),
        "C98 reset/replacement should replay exact initial clock, held value, and Event Values");
}

void testC99SamplesFromDiskSingleStepTransportExample()
{
    const auto path = examplePath(
        "c99_samples_from_disk_single_step_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C99 example should be persisted in format 35");
    requireContains(source,
        "\"typeId\": \"talas.samplesFromDiskSingleStep\"",
        "C99 example should expose Samples From Disk Single Step");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id
                == "Samples/C99SingleStep"
            && parsed.document.assets.front().contentHash
                == "f936a20cbfdd258de8a2ffdcb77c94565486e82f8131d92ae2ec862cd492f6ee"
            && parsed.document.timeline.eventValues.size() == 8,
        "C99 project should own exact current-format finite table and bounded schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C99 CLI status should succeed");
    requireContains(status.output,
        "type=talas.samplesFromDiskSingleStep",
        "C99 status should expose the stateful single-step operation");
    requireContains(status.output,
        "Samples/C99SingleStep kind=table generation=1 hash=f936a20cbfdd258de8a2ffdcb77c94565486e82f8131d92ae2ec862cd492f6ee health=verified",
        "C99 status should expose exact verified immutable table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "12",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C99 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 12, 8.0);
    constexpr std::array expected{
        0.25, -0.2, -0.2, -0.2, 0.5, -0.4,
        0.75, 0.1, 0.25, -0.2, 0.5, -0.4};
    require(channels.size() == 1,
        "C99 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame])
                < 1.0e-12,
            "C99 CLI should publish exact start, advance, hold, resume, gate restart, and continuation frame "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{8.0, 3};
    settings.eventValues = {
        {"C99Position", 1.0, talas::expr::ValueDomain::Scalar},
        {"C99Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {"C99Gate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C99Trigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {4, "C99Trigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {6, "C99Gate", 0.0,
            talas::expr::ValueDomain::Scalar},
        {7, "C99Position", 0.0,
            talas::expr::ValueDomain::Scalar},
        {7, "C99Gate", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources.push_back({
        "Samples/C99SingleStep",
        {0.1, 0.25, -0.2, 0.5, -0.4, 0.75},
    });
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C99 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C99 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 12> offlineOutput{};
    std::array<double, 12> realtimeOutput{};
    constexpr std::array<std::size_t, 6> partitions{
        1, 2, 1, 3, 2, 3};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C99 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C99 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeOutput == offlineOutput
            && realtimeOutput == expected,
        "C99 prepared and offline irregular partitions should be bit-identical to the product projection");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    std::array<double, 3> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(3)),
        "C99 reset/replacement should replay exact initial cursor, edge, trigger, and schedule state");
}

void testC100PhaseShiftBy90AllpassTransportExample()
{
    const auto path = examplePath(
        "c100_phase_shift_by_90_allpass_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C100 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.phaseShiftBy90\"",
        "C100 example should expose Phase Shift By 90");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C100 project should own exact current-format resource-free bounded schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C100 CLI status should succeed");
    requireContains(status.output, "type=talas.phaseShiftBy90",
        "C100 status should expose the stateful allpass operations");
    requireContains(status.output, "resources: none",
        "C100 status should expose its resource-free contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C100 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 8.0);
    // Independent Python/math model of the candidate coefficients and schedule.
    // Transcendental/constant rounding differs by a few ulps from C++ (including
    // exact zero at Fs/8). Allow only that rounding here; backend, partition and
    // reset comparisons below remain exact against the CLI's actual samples.
    constexpr std::array<double, 16> expectedLeft{
        -0.0,
        0.37499999999999994,
        0.26516504294495524,
        -0.18750000000000008,
        -0.67609875897201577,
        -0.93884483454811685,
        -0.78932114333297032,
        -0.28949214586404687,
        0.36304942001516138,
        0.57831997242285615,
        0.32594368033535298,
        -0.19153323854367377,
        -0.66576453768676291,
        -0.84576661927183683,
        -0.5980473117883367,
        -0.047883309635918359,
    };
    constexpr std::array<double, 16> expectedRight{
        0.0,
        -2.943923360032078e-17,
        0.5303300858899106,
        0.75,
        0.66201257427381743,
        0.23246790165816586,
        -0.31701572614671697,
        -0.67509395813599038,
        -0.45991809961693308,
        0.1079120256804585,
        0.59299777469736981,
        0.73529608369499244,
        0.53033008588991071,
        1.5072697713669306e-16,
        -0.5303300858899106,
        -0.75,
    };
    require(channels.size() == 2,
        "C100 CLI should publish the Phase Shift By 90 stereo pair");
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expectedLeft[frame]) <= 1.0e-14,
            "C100 CLI should match the independent left sample "
                + std::to_string(frame));
        require(
            std::abs(channels[1].samples[frame] - expectedRight[frame]) <= 1.0e-14,
            "C100 CLI should match the independent right sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{8.0, 3};
    settings.eventValues = {
        {"C100Frequency", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {4, "C100Frequency", 0.5,
            talas::expr::ValueDomain::Scalar},
        {8, "C100Frequency", 1.5,
            talas::expr::ValueDomain::Scalar},
        {12, "C100Frequency", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C100 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C100 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 16> offlineLeft{};
    std::array<double, 16> offlineRight{};
    std::array<double, 16> realtimeLeft{};
    std::array<double, 16> realtimeRight{};
    constexpr std::array<std::size_t, 8> partitions{
        1, 2, 3, 2, 1, 3, 2, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames
                && block.right.size() == frames,
            "C100 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineLeft.begin()
                + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(block.right,
            offlineRight.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeLeft}.subspan(offset, frames),
                    std::span<double>{realtimeRight}.subspan(offset, frames)),
            "C100 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeLeft == offlineLeft
            && realtimeRight == offlineRight
            && std::ranges::equal(realtimeLeft, channels[0].samples)
            && std::ranges::equal(realtimeRight, channels[1].samples),
        "C100 prepared and offline irregular partitions should preserve both exact Phase Shift By 90 channels");
    prepared.resetTransport();
    std::array<double, 3> resetLeft{};
    std::array<double, 3> resetRight{};
    std::array<double, 3> replacedLeft{};
    std::array<double, 3> replacedRight{};
    require(prepared.render(resetLeft, resetRight)
            && replacement.render(replacedLeft, replacedRight)
            && resetLeft == replacedLeft
            && resetRight == replacedRight
            && std::ranges::equal(
                resetLeft, std::span<const double>{channels[0].samples}.first(3))
            && std::ranges::equal(
                resetRight, std::span<const double>{channels[1].samples}.first(3)),
        "C100 reset/replacement should replay both exact initial phase branches and Event Value state");
}

void testC101SingleSideBandRmQuadratureTransportExample()
{
    const auto path = examplePath(
        "c101_single_side_band_rm_quadrature_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C101 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.singleSideBandRm\"",
        "C101 example should expose Single Side Band RM");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 7,
        "C101 project should own exact current-format resource-free signed-shift schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C101 CLI status should succeed");
    requireContains(status.output, "type=talas.singleSideBandRm",
        "C101 status should expose the stateful quadrature operations");
    requireContains(status.output, "resources: none",
        "C101 status should expose its resource-free contract");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C101 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 8.0);
    // Independent oscillator -> stereo allpass pair -> carrier sum oracle.
    constexpr std::array<double, 16> expected{
        0.0,
        0.14350628713690863,
        0.23149371286309128,
        -0.2877983188767218,
        -0.692909649383465,
        -0.5293829417532971,
        -0.07671789880834196,
        0.1729502764282856,
        0.06221650539013143,
        -0.37846452368187855,
        -0.3896043188579357,
        -0.036456083304208076,
        0.135634690635787,
        0.2627939296550133,
        0.14484757859093372,
        0.026325807401468615,
    };
    require(channels.size() == 1,
        "C101 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(channels[0].samples[frame] - expected[frame]) <= 1.0e-14,
            "C101 CLI should match the independent phase-pair sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{8.0, 3};
    settings.eventValues = {
        {"C101Frequency", 1.0, talas::expr::ValueDomain::Scalar},
        {"C101FreqScale", 2.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {4, "C101FreqScale", 1.0,
            talas::expr::ValueDomain::Scalar},
        {8, "C101Frequency", 1.5,
            talas::expr::ValueDomain::Scalar},
        {8, "C101FreqScale", 0.5,
            talas::expr::ValueDomain::Scalar},
        {12, "C101Frequency", 0.75,
            talas::expr::ValueDomain::Scalar},
        {12, "C101FreqScale", 2.0,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C101 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C101 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 16> offlineOutput{};
    std::array<double, 16> realtimeOutput{};
    constexpr std::array<std::size_t, 8> partitions{
        1, 2, 3, 2, 1, 3, 2, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C101 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C101 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeOutput == offlineOutput
            && std::ranges::equal(channels[0].samples, realtimeOutput),
        "C101 CLI, prepared and offline irregular partitions should be bit-identical");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(realtimeOutput[frame] - expected[frame]) <= 1.0e-14,
            "C101 prepared/offline output should match the independent stereo phase-pair oracle at frame "
                + std::to_string(frame));
    }
    prepared.resetTransport();
    std::array<double, 3> reset{};
    std::array<double, 3> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(reset,
                std::span{realtimeOutput}.first(reset.size())),
        "C101 reset and replacement should replay exact initial state");
    for (std::size_t frame = 0; frame < reset.size(); ++frame)
    {
        require(
            std::abs(reset[frame] - expected[frame]) <= 1.0e-14,
            "C101 reset output should match the independent phase-pair oracle at frame "
                + std::to_string(frame));
    }
}

void testC102SosOscillatorsSelectedWavetableTransportExample()
{
    const auto path = examplePath(
        "c102_sos_oscillators_selected_wavetable_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C102 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.sosOscillators\"",
        "C102 example should expose SOS Oscillators");
    requireContains(source,
        "dc37ec3d6bff3d1b8ac6bc9344c996473c2a43da7ea1647864359472213ba803",
        "C102 example should bind the exact immutable cyclic wavetable hash");
    require(readText(path.parent_path()
                / "resources/c102-sos-oscillators-wave.txt")
            == "0 1 0 -1\n",
        "C102 checked product should retain exact cyclic wavetable bytes");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.empty(),
        "C102 project should own exact current-format single-resource static authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "5",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C102 CLI status should succeed");
    requireContains(status.output, "type=talas.sosOscillators",
        "C102 status should expose the stateful SOS operations");
    requireContains(status.output,
        "Wavetable/C102 kind=table generation=1 hash=dc37ec3d6bff3d1b8ac6bc9344c996473c2a43da7ea1647864359472213ba803 health=verified",
        "C102 status should expose exact immutable wavetable authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "16", "--max-block-frames", "5",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C102 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 16.0);
    constexpr std::array<double, 16> expected{
        0.0,
        0.31053535184766629,
        0.48182147362502814,
        0.39424262824662071,
        0.22441193554603653,
        0.13363927534614328,
        0.062977076964908368,
        0.12755636317729596,
        0.15303300858899105,
        0.1741559732178288,
        0.22582681793651005,
        0.13415349720244238,
        0.015001643057001329,
        -0.18958670566869995,
        -0.31709411697055834,
        -0.18952714741930909,
    };
    require(channels.size() == 1,
        "C102 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C102 CLI should publish the exact sine/table selected-partial sample "
                + std::to_string(frame));
    }

    auto settings = talas::runtime::RenderSettings{16.0, 5};
    settings.tableResources = {
        {"Wavetable/C102", {0.0, 1.0, 0.0, -1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C102 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C102 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 16> offlineOutput{};
    std::array<double, 16> realtimeOutput{};
    constexpr std::array<std::size_t, 8> partitions{
        1, 2, 3, 2, 1, 3, 2, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C102 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C102 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeOutput == offlineOutput
            && realtimeOutput == expected,
        "C102 prepared and offline irregular partitions should be bit-identical to the sine/table product projection");
    prepared.resetTransport();
    std::array<double, 5> reset{};
    std::array<double, 5> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(5)),
        "C102 reset/replacement should replay exact initial cascade, SOS phase, and resource generation");
}

void testC103LowPassGateControlStateTransportExample()
{
    const auto path = examplePath(
        "c103_low_pass_gate_control_state_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C103 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.lowPassGate\"",
        "C103 example should expose Low-Pass Gate");
    requireContains(source, "\"typeId\": \"talas.pulseTrain\"",
        "C103 example should expose deterministic connected control");
    requireContains(source, "\"expression\": \"1 / 0\"",
        "C103 connected control should own an invalid inert fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C103 project should own exact current-format resource-free authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C103 CLI status should succeed");
    requireContains(status.output, "type=talas.lowPassGate",
        "C103 status should expose the stateful Low-Pass Gate operation");
    requireContains(status.output, "type=talas.pulseTrain",
        "C103 status should expose deterministic connected-control state");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C103 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 8.0);
    constexpr std::array<double, 16> expected{
        0.0,
        0.14927307260097197,
        0.40583875955877907,
        0.41322476094161698,
        0.067463440690483642,
        -0.077491745580020255,
        -0.090586962748168492,
        -0.054301238403083416,
        -0.060033432994835059,
        0.20963389520905151,
        0.49412819759517906,
        0.45452749800341502,
        0.068415398993401599,
        -0.090196307594442804,
        -0.10221465478099429,
        -0.060536392514602258,
    };
    require(channels.size() == 1,
        "C103 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C103 CLI should publish the exact connected-control Low-Pass Gate sample "
                + std::to_string(frame));
    }

    const auto settings = talas::runtime::RenderSettings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C103 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C103 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 16> offlineOutput{};
    std::array<double, 16> realtimeOutput{};
    constexpr std::array<std::size_t, 8> partitions{
        1, 2, 3, 2, 1, 3, 2, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C103 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C103 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeOutput == offlineOutput
            && realtimeOutput == expected,
        "C103 prepared and offline irregular partitions should be bit-identical to the connected-control product projection");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    std::array<double, 3> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(3)),
        "C103 reset/replacement should replay exact oscillator, Pulse Train, and Low-Pass Gate state");
}

void testC104ReverbSectionFractionalDelayTransportExample()
{
    const auto path = examplePath(
        "c104_reverb_section_fractional_delay_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C104 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.reverbSection\"",
        "C141 reused product should expose bounded Reverb Section");
    requireContains(source, "\"typeId\": \"talas.linearRamp\"",
        "C104 example should expose deterministic connected delay modulation");
    requireContains(source, "\"expression\": \"1 / 0\"",
        "C104 connected delayScale should own an invalid inert fallback");
    requireContains(source, "\"resource\": \"Private\"",
        "C104 example should expose the admitted Private delay memory sentinel");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C104 project should own exact current-format resource-free authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C104 CLI status should succeed");
    requireContains(status.output, "type=talas.reverbSection",
        "C104 status should expose the session-stateful Reverb Section operation");
    requireContains(status.output, "type=talas.linearRamp",
        "C104 status should expose deterministic connected delay modulation");
    requireContains(status.output, "resources: none",
        "C104 status should expose the resource-free Private-memory product boundary");
    requireContains(status.output, "latency-warnings: none",
        "C141 Reverb Section artistic delay should publish zero graph-latency authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "16",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C104 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 16, 8.0);
    constexpr std::array<double, 16> expected{
        0.0,
        -0.021576576079135774,
        0.02756831540756338,
        0.36129684932425477,
        0.522554723252007,
        0.4824379690038761,
        0.26524852775726177,
        -0.06143178831946987,
        -0.39065476557518675,
        -0.5619332525557106,
        -0.4013997559208043,
        -0.004433512110312087,
        0.39458758151832596,
        0.5624794622179013,
        0.4008858029008751,
        0.004455627959604103,
    };
    require(channels.size() == 1,
        "C104 CLI should publish one mono channel");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C104 CLI should publish the exact fractional allpass sample "
                + std::to_string(frame));
    }

    const auto settings = talas::runtime::RenderSettings{8.0, 3};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C104 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C104 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 16> offlineOutput{};
    std::array<double, 16> realtimeOutput{};
    constexpr std::array<std::size_t, 8> partitions{
        1, 2, 3, 2, 1, 3, 2, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C104 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C104 prepared irregular partition should render");
        offset += frames;
    }
    require(realtimeOutput == offlineOutput
            && realtimeOutput == expected,
        "C104 prepared and offline irregular partitions should be bit-identical to the fractional-delay product projection");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    std::array<double, 3> replaced{};
    require(prepared.render(reset) && replacement.render(replaced)
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(3)),
        "C104 reset/replacement should replay exact oscillator, Linear Ramp, delay memory, and write index state");
}

void testC105MonotonizerKnownPitchTransportExample()
{
    const auto path = examplePath(
        "c105_monotonizer_known_pitch_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C105 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.monotonizer\"",
        "C105 example should expose historical Monotonizer");
    requireContains(source, "\"typeId\": \"talas.linearRamp\"",
        "C105 example should taper the known-pitch source deterministically");
    requireContains(source, "\"typeId\": \"talas.constant\"",
        "C105 example should expose connected block-constant target authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C105 project should own exact current-format resource-free authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "256",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C105 CLI status should succeed");
    requireContains(status.output, "type=talas.monotonizer",
        "C105 status should expose the session-stateful Monotonizer operation");
    requireContains(status.output, "type=talas.constant",
        "C105 status should expose connected block-constant target authority");
    requireContains(status.output, "critical-path: 1023 frames",
        "C105 status should expose exact active phase-vocoder latency");
    requireContains(status.output, "terminal-output: 1023 frames",
        "C105 status should carry active latency to the terminal output");
    requireContains(status.output, "latency-warnings: none",
        "C105 status should expose exact rather than conservative latency");
    requireContains(status.output, "resources: none",
        "C105 status should preserve the resource-free product boundary");

    constexpr std::size_t renderedFrames = 2048;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "48000",
        "--max-block-frames", "256", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C105 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 48000.0);
    require(channels.size() == 1,
        "C105 CLI should publish one mono channel");
    constexpr std::array<std::pair<std::size_t, double>, 11> probes{{
        {0, 0.0},
        {1022, 0.0},
        {1023, 0.0},
        {1024, -1.2773832976949218e-8},
        {1279, -0.0010957101215589211},
        {1280, -0.0011526690234931847},
        {1535, 0.026374355454268961},
        {1536, 0.02450548622742996},
        {1791, 0.051335053723950412},
        {1792, 0.050543477801212468},
        {2047, -0.011220315704070666},
    }};
    for (const auto [frame, expected] : probes)
    {
        require(
            std::bit_cast<std::uint64_t>(channels[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected),
            "C105 CLI should publish the exact windowed phase-vocoder sample "
                + std::to_string(frame));
    }
    require(std::ranges::all_of(channels[0].samples, [](double sample) {
                return std::isfinite(sample)
                    && std::abs(sample) <= 0.5;
            }),
        "C105 product render should remain finite and bounded through phase-vocoder startup");

    const auto settings = talas::runtime::RenderSettings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C105 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C105 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 16> partitions{
        64, 192, 127, 129, 64, 192, 127, 129,
        64, 192, 127, 129, 64, 192, 127, 129};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C105 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C105 prepared irregular partition should render");
        offset += frames;
    }
    require(offset == renderedFrames
            && realtimeOutput == offlineOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C105 prepared, offline, and CLI renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    for (const auto frames : partitions)
    {
        require(prepared.render(std::span<double>{reset}.subspan(
                    offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(
                        offset, frames)),
            "C105 reset/replacement irregular partition should render");
        offset += frames;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C105 reset/replacement should replay exact oscillator, ramp, phase, overlap, envelope, and peak state");
}

void testC106MultifileDiskPlayerSelectionTransportExample()
{
    const auto path = examplePath(
        "c106_multifile_disk_player_selection_transport.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C106 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.multifileDiskPlayer\"",
        "C106 example should expose Multifile Disk Player");
    requireContains(source, "\"expression\": \"1 / 0\"",
        "C106 product should make unevaluable direct controls visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 2
            && parsed.document.timeline.eventValues.size() == 7,
        "C106 project should own exact current-format table and trigger-selection schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C106 CLI status should succeed");
    requireContains(status.output, "type=talas.multifileDiskPlayer",
        "C106 status should expose the session-stateful Multifile Disk Player operation");
    requireContains(status.output,
        "Samples/C37Hit kind=table generation=1 hash=ce5449356b67e84f906f73a1743e5d8aebed474e3bc3feb60f0e3001f0f42e98 health=verified",
        "C106 status should expose exact reused hit-table authority");
    requireContains(status.output,
        "Samples/C37Wave kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C106 status should expose exact reused waveform-table authority");

    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames", "12",
        "--sample-rate", "8", "--max-block-frames", "3",
        "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C106 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, 12, 8.0);
    constexpr std::array expected{
        -1.0,
        -0.5,
        0.0,
        0.5,
        1.0,
        0.0,
        -0.38268343236509039,
        -0.54489510677581898,
        -0.70710678118654768,
        -0.81549315684891721,
        -0.92387953251128663,
        -0.96193976625564326,
    };
    require(channels.size() == 1
            && std::ranges::equal(channels.front().samples, expected),
        "C106 CLI should publish exact reverse half-rate selection, release, silence, and retrigger samples");

    auto settings = talas::runtime::RenderSettings{8.0, 3};
    settings.eventValues = {
        {"C106Index", 0.0, talas::expr::ValueDomain::Scalar},
        {"C106Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {"C106RateScale", -0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {4, "C106Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {6, "C106Index", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "C106Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {10, "C106Trigger", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Samples/C37Hit", {1.0, 0.0, -1.0}},
        {"Samples/C37Wave", {
            0.0, 0.3826834323650898, 0.7071067811865475,
            0.9238795325112867, 1.0, 0.9238795325112867,
            0.7071067811865476, 0.3826834323650899,
            0.0000000000000001, -0.3826834323650897,
            -0.7071067811865475, -0.9238795325112865,
            -1.0, -0.9238795325112866, -0.7071067811865477,
            -0.3826834323650904,
        }},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C106 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C106 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});
    std::array<double, 12> offlineOutput{};
    std::array<double, 12> realtimeOutput{};
    constexpr std::array<std::size_t, 6> partitions{1, 2, 3, 1, 2, 3};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C106 offline irregular partition should render");
        std::ranges::copy(block.mono,
            offlineOutput.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C106 prepared irregular partition should render");
        offset += frames;
    }
    require(offset == expected.size()
            && offlineOutput == realtimeOutput
            && realtimeOutput == expected,
        "C106 CLI, offline, and prepared irregular partitions should be bit-identical");

    prepared.resetTransport();
    std::array<double, 4> reset{};
    std::array<double, 4> replaced{};
    require(prepared.render(std::span<double>{reset}.first(3))
            && prepared.render(std::span<double>{reset}.subspan(3))
            && replacement.render(std::span<double>{replaced}.first(3))
            && replacement.render(std::span<double>{replaced}.subspan(3))
            && reset == replaced
            && std::ranges::equal(
                reset, std::span<const double>{expected}.first(4)),
        "C106 reset/replacement should replay exact selected index, cursor, trigger edge, and Event Value state");
}

void testC110SampleCloudDynamicControlsExample()
{
    const auto path = examplePath(
        "c110_sample_cloud_dynamic_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C110 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.sampleCloud\"",
        "C110 example should expose Sample Cloud");
    require(countOccurrences(source, "\"expression\": \"1 / 0\"") == 9,
        "C110 product should make all nine connected direct fallbacks visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 2
            && parsed.document.timeline.eventValues.size() == 14,
        "C110 project should own exact current-format table and dynamic-control schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C110 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C110 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.sampleCloud",
        "C110 status should expose the session-stateful Sample Cloud operation");
    requireContains(status.output,
        "Samples/C110Cloud kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C110 status should expose exact reused sample-table authority");
    requireContains(status.output,
        "Envelopes/C110Grain kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C110 status should expose exact reused envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C110 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right",
        "C110 CLI render should expose exact stereo channel order");
    require(std::ranges::any_of(channels[0].samples, [](double sample) {
                return std::abs(sample) > 1.0e-12;
            })
            && channels[0].samples != channels[1].samples,
        "C110 CLI render should be audible and spatially distinct");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C110Amplitude", 0.35, talas::expr::ValueDomain::Scalar},
        {"C110Pan", 0.2, talas::expr::ValueDomain::Scalar},
        {"C110TimeIndex", -1.0, talas::expr::ValueDomain::Scalar},
        {"C110GrainDur", 0.05, talas::expr::ValueDomain::Scalar},
        {"C110Density", 64.0, talas::expr::ValueDomain::Scalar},
        {"C110MaxGrains", 8.0, talas::expr::ValueDomain::Scalar},
        {"C110PanJitter", 0.2, talas::expr::ValueDomain::Scalar},
        {"C110GrainDurJitter", 0.1, talas::expr::ValueDomain::Scalar},
        {"C110TimeIndexJitter", 0.1, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C110Pan", 0.8, talas::expr::ValueDomain::Scalar},
        {256, "C110Density", 96.0, talas::expr::ValueDomain::Scalar},
        {256, "C110MaxGrains", 4.0, talas::expr::ValueDomain::Scalar},
        {512, "C110Pan", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C110MaxGrains", 12.0, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Samples/C110Cloud", {
            0.0, 0.3826834323650898, 0.7071067811865475,
            0.9238795325112867, 1.0, 0.9238795325112867,
            0.7071067811865476, 0.3826834323650899,
            0.0000000000000001, -0.3826834323650897,
            -0.7071067811865475, -0.9238795325112865,
            -1.0, -0.9238795325112866, -0.7071067811865477,
            -0.3826834323650904,
        }},
        {"Envelopes/C110Grain", {0.25, 0.5, 0.75, 1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C110 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C110 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineLeft{};
    std::array<double, renderedFrames> offlineRight{};
    std::array<double, renderedFrames> realtimeLeft{};
    std::array<double, renderedFrames> realtimeRight{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames
                && block.right.size() == frames,
            "C110 offline irregular partition should render stereo");
        std::ranges::copy(block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeLeft}.subspan(offset, frames),
                    std::span<double>{realtimeRight}.subspan(offset, frames)),
            "C110 prepared irregular partition should render stereo");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineLeft == realtimeLeft
            && offlineRight == realtimeRight
            && std::ranges::equal(realtimeLeft, channels[0].samples)
            && std::ranges::equal(realtimeRight, channels[1].samples),
        "C110 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> resetLeft{};
    std::array<double, renderedFrames> resetRight{};
    std::array<double, renderedFrames> replacedLeft{};
    std::array<double, renderedFrames> replacedRight{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{resetLeft}.subspan(offset, frames),
                    std::span<double>{resetRight}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replacedLeft}.subspan(offset, frames),
                    std::span<double>{replacedRight}.subspan(offset, frames)),
            "C110 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(resetLeft == replacedLeft && resetRight == replacedRight
            && resetLeft == realtimeLeft && resetRight == realtimeRight,
        "C110 reset/replacement should replay exact control, scheduler, grain, and stereo state");
}

void testC113GrainCloudDynamicControlsExample()
{
    const auto path = examplePath(
        "c113_grain_cloud_dynamic_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C113 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.grainCloud\"",
        "C113 example should expose Grain Cloud");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 2
            && parsed.document.timeline.eventValues.size() == 16,
        "C113 project should own exact current-format table and dynamic-control schedule authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C113 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C113 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.grainCloud",
        "C113 status should expose the session-stateful Grain Cloud operation");
    requireContains(status.output,
        "Tables/C113Wave kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C113 status should expose exact waveform-table authority");
    requireContains(status.output,
        "Envelopes/C113Grain kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C113 status should expose exact envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C113 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right",
        "C113 CLI render should expose exact stereo channel order");
    require(std::ranges::any_of(channels[0].samples, [](double sample) {
                return std::abs(sample) > 1.0e-12;
            })
            && channels[0].samples != channels[1].samples,
        "C113 CLI render should be audible and spatially distinct");
}

void testC114ChopperDynamicDurationsExample()
{
    const auto path = examplePath(
        "c114_chopper_dynamic_durations.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C114 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.chopper\"",
        "C114 example should expose Chopper");
    require(countOccurrences(
                source,
                "\"expression\": \"1e308 * 1e308 s\"") == 2,
        "C114 product should make both connected duration fallbacks exact inert non-finite numerics");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.size() == 6,
        "C114 project should own exact current-format envelope and dynamic-duration schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C114 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C114 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C114 status should expose the mono terminal");
    requireContains(status.output, "type=talas.chopper",
        "C114 status should expose the session-stateful Chopper operation");
    requireContains(status.output,
        "Envelopes/C114Chopper kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C114 status should expose exact envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C114 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C114 CLI render should expose audible mono Chopper output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C114GrainDuration", 0.02,
            talas::expr::ValueDomain::Scalar},
        {"C114InterGrainDelay", 0.01,
            talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C114GrainDuration", 0.01,
            talas::expr::ValueDomain::Scalar},
        {256, "C114InterGrainDelay", 0.02,
            talas::expr::ValueDomain::Scalar},
        {512, "C114GrainDuration", 0.03,
            talas::expr::ValueDomain::Scalar},
        {512, "C114InterGrainDelay", 0.005,
            talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Envelopes/C114Chopper", {0.25, 0.5, 0.75, 1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C114 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C114 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C114 offline irregular partition should render mono");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C114 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C114 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(
                        offset, frames)),
            "C114 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C114 reset/replacement should replay exact duration schedule, Chopper state, and mono output");
}

void testC115FunctionGeneratorTriggerDurationExample()
{
    const auto path = examplePath(
        "c115_function_generator_trigger_duration.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C115 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.functionGenerator\"",
        "C115 example should expose Function Generator");
    requireContains(source, "\"id\": \"onDuration\", \"expression\": \"0 s\"",
        "C115 product should make the connected duration fallback visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.timeline.eventValues.size() == 8,
        "C115 project should own exact current-format envelope, trigger, and duration schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C115 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C115 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C115 status should expose the mono terminal");
    requireContains(status.output, "type=talas.functionGenerator",
        "C115 status should expose the session-stateful Function Generator operation");
    requireContains(status.output,
        "Envelopes/C115FunctionGenerator kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C115 status should expose exact envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C115 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C115 CLI render should expose audible mono Function Generator output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C115Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {"C115Duration", 0.04, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {128, "C115Trigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {192, "C115Duration", 0.02,
            talas::expr::ValueDomain::Scalar},
        {256, "C115Trigger", 0.0,
            talas::expr::ValueDomain::Scalar},
        {512, "C115Trigger", 1.0,
            talas::expr::ValueDomain::Scalar},
        {576, "C115Duration", 0.04,
            talas::expr::ValueDomain::Scalar},
        {640, "C115Trigger", 0.0,
            talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Envelopes/C115FunctionGenerator", {0.25, 0.5, 0.75, 1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C115 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C115 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C115 offline irregular partition should render mono");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C115 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C115 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(
                        offset, frames)),
            "C115 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C115 reset/replacement should replay exact trigger, duration, Function Generator state, and mono output");

    auto mutatedSettings = settings;
    require(!mutatedSettings.eventValueChanges.empty(),
        "C115 mutation fixture should expose the first trigger edge");
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C115 one-frame trigger mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C115 one-frame trigger schedule mutation should observably change product output");
}

void testC116MultislopeConnectedControlsExample()
{
    const auto path = examplePath(
        "c116_multislope_connected_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C116 example should be persisted in format 35");
    requireContains(source,
        "\"typeId\": \"talas.multislopeFunctionGenerator\"",
        "C116 example should expose Multislope Function Generator");
    require(countOccurrences(
                source, "\"expression\": \"1e200 * 1e200\"") == 3,
        "C116 product should make all three connected fallbacks visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 10,
        "C116 project should own exact current-format envelope and three-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C116 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C116 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C116 status should expose the mono terminal");
    requireContains(status.output,
        "type=talas.multislopeFunctionGenerator",
        "C116 status should expose the session-stateful Multislope operation");
    requireContains(status.output, "resources: none",
        "C116 status should expose the resource-free envelope contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C116 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C116 CLI render should expose audible mono Multislope output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C116Level", 0.8, talas::expr::ValueDomain::Scalar},
        {"C116Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {"C116Rate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {64, "C116Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {256, "C116Level", 0.4, talas::expr::ValueDomain::Scalar},
        {256, "C116Rate", 2.0, talas::expr::ValueDomain::Scalar},
        {384, "C116Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {512, "C116Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {640, "C116Rate", 0.5, talas::expr::ValueDomain::Scalar},
        {768, "C116Level", 0.65, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C116 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C116 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C116 offline irregular partition should render mono");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C116 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C116 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C116 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C116 reset/replacement should replay exact control schedule, Multislope state, and mono output");

    auto mutatedSettings = settings;
    require(!mutatedSettings.eventValueChanges.empty(),
        "C116 mutation fixture should expose the first gate edge");
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C116 one-frame gate mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C116 one-frame gate schedule mutation should observably change product output");
}

void testC117MultisegmentLoopConnectedControlsExample()
{
    const auto path = examplePath(
        "c117_multisegment_loop_connected_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C117 example should be persisted in format 35");
    requireContains(source,
        "\"typeId\": \"talas.multisegmentEnvelope\"",
        "C117 example should expose Multisegment Envelope");
    require(countOccurrences(
                source, "\"expression\": \"1e200 * 1e200\"") == 2,
        "C117 product should make both connected fallbacks visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 7,
        "C117 project should own exact current-format loop and two-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C117 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C117 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C117 status should expose the mono terminal");
    requireContains(status.output,
        "type=talas.multisegmentEnvelope",
        "C117 status should expose the session-stateful Multisegment operation");
    requireContains(status.output, "resources: none",
        "C117 status should expose the resource-free envelope contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C117 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C117 CLI render should expose audible mono Multisegment output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C117Level", 0.8, talas::expr::ValueDomain::Scalar},
        {"C117Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {64, "C117Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {256, "C117Level", 0.4, talas::expr::ValueDomain::Scalar},
        {384, "C117Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {512, "C117Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {768, "C117Level", 0.65, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C117 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C117 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C117 offline irregular partition should render mono");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C117 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C117 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C117 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C117 reset/replacement should replay exact control schedule, loop drift, state, and mono output");

    auto mutatedSettings = settings;
    require(!mutatedSettings.eventValueChanges.empty(),
        "C117 mutation fixture should expose the first gate edge");
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C117 one-frame gate mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C117 one-frame gate schedule mutation should observably change product output");
}

void testC118NoiseModesConnectedControlsExample()
{
    const auto path = examplePath(
        "c118_noise_modes_connected_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C118 example should be persisted in format 43");
    require(countOccurrences(source, "\"typeId\": \"talas.noise\"") == 2,
        "C118 example should expose two distinguishable Noise modes");
    requireContains(source, "\"choice\": \"pink\"",
        "C118 product should expose pink Noise");
    requireContains(source, "\"choice\": \"hotPink\"",
        "C118 product should expose hot-pink Noise");
    require(countOccurrences(
                source, "\"expression\": \"1 / 0\"") == 6,
        "C118 product should make all connected Noise fallbacks visibly inert");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 7,
        "C118 project should own exact current-format two-mode and three-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C118 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C118 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C118 status should expose the mono terminal");
    requireContains(status.output, "type=talas.noise",
        "C118 status should expose the session-stateful Noise operations");
    requireContains(status.output, "resources: none",
        "C118 status should expose the resource-free generator contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C118 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C118 CLI render should expose finite audible mono Noise output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C118Frequency", 64.0, talas::expr::ValueDomain::Scalar},
        {"C118Center", 0.0, talas::expr::ValueDomain::Scalar},
        {"C118Scale", 0.35, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C118Frequency", 512.0,
            talas::expr::ValueDomain::Scalar},
        {512, "C118Center", 0.1,
            talas::expr::ValueDomain::Scalar},
        {768, "C118Scale", 0.2,
            talas::expr::ValueDomain::Scalar},
        {896, "C118Center", -0.05,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C118 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C118 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C118 offline irregular partition should render mono");
        std::ranges::copy(block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C118 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C118 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C118 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C118 reset/replacement should replay exact controls, pink filters, hot-pink phase, PRNG state, and mono output");

    auto mutatedSettings = settings;
    require(!mutatedSettings.eventValueChanges.empty(),
        "C118 mutation fixture should expose the frequency change");
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C118 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C118 one-frame frequency mutation should observably change product output");
}

void testC119AbsoluteValueConnectedMagnitudeExample()
{
    const auto path = examplePath(
        "c119_absolute_value_connected_magnitude.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C119 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.absoluteValue\"",
        "C119 product should expose Absolute Value");
    requireContains(source, "\"expression\": \"!C119Gain\"",
        "C119 product should expose its connected scheduled magnitude source");
    requireContains(source, "\"id\": \"gain\", \"expression\": \"0\"",
        "C119 product should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 2,
        "C119 project should own exact current-format finite negative control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C119 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C119 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C119 status should expose the mono terminal");
    requireContains(status.output, "type=talas.absoluteValue",
        "C119 status should expose the checked magnitude operation");
    requireContains(status.output, "resources: none",
        "C119 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C119 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C119 CLI render should expose finite audible mono magnitude-controlled output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C119Gain", -0.25, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {{
        512, "C119Gain", -0.75,
        talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C119 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C119 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C119 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C119 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C119 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C119 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C119 reset/replacement should replay exact magnitude controls and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "magnitude"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "gain", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C119 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C119 bypassing magnitude should observably preserve the negative gain sign and change output");

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C119 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C119 one-frame magnitude schedule mutation should observably change product output");
}

void testC120ArcTanConnectedCurveExample()
{
    const auto path = examplePath(
        "c120_arctan_connected_curve.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C120 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.arcTan\"",
        "C120 product should expose ArcTan");
    requireContains(source, "\"expression\": \"!C120Gain\"",
        "C120 product should expose its connected scheduled curve source");
    requireContains(source, "\"id\": \"gain\", \"expression\": \"0\"",
        "C120 product should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 2,
        "C120 project should own exact current-format finite negative control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C120 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C120 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C120 status should expose the mono terminal");
    requireContains(status.output, "type=talas.arcTan",
        "C120 status should expose the checked angle operation");
    requireContains(status.output, "resources: none",
        "C120 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C120 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C120 CLI render should expose finite audible mono angle-controlled output");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C120Gain", -0.25, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {{
        512, "C120Gain", -2.0,
        talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C120 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C120 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C120 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C120 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C120 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C120 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C120 reset/replacement should replay exact angle controls and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "angle"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "gain", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C120 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C120 bypassing ArcTan should observably change the connected negative gain curve");

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C120 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C120 one-frame angle schedule mutation should observably change product output");
}

void testC121EqualityConnectedComparisonExample()
{
    const auto path = examplePath(
        "c121_equality_connected_comparison.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C121 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.equality\"",
        "C121 product should expose Equality");
    requireContains(source, "\"expression\": \"!C121Left\"",
        "C121 product should expose its connected left source");
    requireContains(source, "\"expression\": \"!C121Right\"",
        "C121 product should expose its connected right source");
    requireContains(source, "\"id\": \"gain\", \"expression\": \"0\"",
        "C121 product should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C121 project should own exact current-format equal-unequal-equal schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C121 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C121 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C121 status should expose the mono terminal");
    requireContains(status.output, "type=talas.equality",
        "C121 status should expose the checked exact comparator");
    requireContains(status.output, "resources: none",
        "C121 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C121 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                })
            && std::ranges::all_of(
                std::span<const double>{channels[0].samples}.subspan(
                    512, 256),
                [](double sample) {
                    return sample == 0.0;
                }),
        "C121 CLI render should expose finite audible equal phases and exact silent unequal phase");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C121Left", 0.25, talas::expr::ValueDomain::Scalar},
        {"C121Right", 0.25, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {512, "C121Right", 0.75,
            talas::expr::ValueDomain::Scalar},
        {768, "C121Left", 0.75,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C121 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C121 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C121 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C121 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C121 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C121 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C121 reset/replacement should replay exact comparison controls and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "equal"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "left", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C121 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C121 bypassing Equality should observably change the equal-unequal-equal gate");

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.front().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C121 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C121 one-frame comparison schedule mutation should observably change product output");
}

void testC122SetRangeConnectedAffineMappingExample()
{
    const auto path = examplePath(
        "c122_set_range_connected_affine_mapping.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C122 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.setRange\"",
        "C122 product should expose Set Range");
    for (const auto eventName : {
             "C122Value", "C122OldMin", "C122OldMax",
             "C122NewMin", "C122NewMax"})
    {
        requireContains(source, "!" + std::string{eventName},
            "C122 product should expose connected "
                + std::string{eventName} + " authority");
    }
    requireContains(source,
        "\"id\": \"gain\", \"expression\": \"0\"",
        "C122 product should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 18,
        "C122 project should own exact current-format four-phase five-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C122 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C122 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C122 status should expose the mono terminal");
    requireContains(status.output, "type=talas.setRange",
        "C122 status should expose the checked affine mapper");
    requireContains(status.output, "resources: none",
        "C122 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C122 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                }),
        "C122 CLI render should expose finite audible mapped phases");
    std::array<double, 4> phasePeaks{};
    for (std::size_t phase = 0; phase < phasePeaks.size(); ++phase)
    {
        for (const auto sample : std::span<const double>{
                 channels[0].samples}.subspan(phase * 256, 256))
        {
            phasePeaks[phase] = std::max(
                phasePeaks[phase], std::abs(sample));
        }
    }
    require(std::abs(phasePeaks[0] - 0.28) <= 1.0e-12,
        "C122 ordinary mapping phase should expose gain 0.35");
    require(std::abs(phasePeaks[1] - 0.24) <= 1.0e-12,
        "C122 reversed-output phase should expose gain 0.3");
    require(std::abs(phasePeaks[3] - 0.4) <= 1.0e-12,
        "C122 reversed-old-range phase should expose gain 0.5");

    constexpr double orderValue = -1.5759674220269;
    constexpr double orderOldMin = -0.1994278515449457;
    constexpr double orderOldMax = 1.8814662170910252;
    constexpr double orderNewMin = 0.23234058611984;
    constexpr double orderNewMax = -0.48617804865676106;
    const auto orderedGain = orderNewMin
        + ((orderValue - orderOldMin) / (orderOldMax - orderOldMin))
            * (orderNewMax - orderNewMin);
    const auto reassociatedGain = orderNewMin
        + ((orderValue - orderOldMin) * (orderNewMax - orderNewMin))
            / (orderOldMax - orderOldMin);
    require(std::bit_cast<std::uint64_t>(orderedGain)
                != std::bit_cast<std::uint64_t>(reassociatedGain),
        "C122 product tuple should make alternate affine operation order observably different");
    require(std::abs(
                phasePeaks[2] - 0.8 * std::abs(orderedGain))
            <= 1.0e-12,
        "C122 bit-distinguishing phase should expose the locked ordered affine gain");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C122Value", 0.25, talas::expr::ValueDomain::Scalar},
        {"C122OldMin", 0.0, talas::expr::ValueDomain::Scalar},
        {"C122OldMax", 1.0, talas::expr::ValueDomain::Scalar},
        {"C122NewMin", 0.2, talas::expr::ValueDomain::Scalar},
        {"C122NewMax", 0.8, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C122Value", 0.75, talas::expr::ValueDomain::Scalar},
        {256, "C122NewMin", 0.9, talas::expr::ValueDomain::Scalar},
        {256, "C122NewMax", 0.1, talas::expr::ValueDomain::Scalar},
        {512, "C122Value", orderValue, talas::expr::ValueDomain::Scalar},
        {512, "C122OldMin", orderOldMin, talas::expr::ValueDomain::Scalar},
        {512, "C122OldMax", orderOldMax, talas::expr::ValueDomain::Scalar},
        {512, "C122NewMin", orderNewMin, talas::expr::ValueDomain::Scalar},
        {512, "C122NewMax", orderNewMax, talas::expr::ValueDomain::Scalar},
        {768, "C122Value", 0.5, talas::expr::ValueDomain::Scalar},
        {768, "C122OldMin", 0.8, talas::expr::ValueDomain::Scalar},
        {768, "C122OldMax", 0.2, talas::expr::ValueDomain::Scalar},
        {768, "C122NewMin", 0.2, talas::expr::ValueDomain::Scalar},
        {768, "C122NewMax", 0.8, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C122 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C122 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C122 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C122 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C122 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C122 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C122 reset/replacement should replay exact five-control mapping and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "range"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "mappedValue", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C122 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C122 bypassing Set Range should observably change product output");

    for (const auto& [sourceNode, port] : {
             std::pair{std::string_view{"mappedValue"},
                       std::string_view{"value"}},
             std::pair{std::string_view{"oldMin"},
                       std::string_view{"oldMin"}},
             std::pair{std::string_view{"oldMax"},
                       std::string_view{"oldMax"}},
             std::pair{std::string_view{"newMin"},
                       std::string_view{"newMin"}},
             std::pair{std::string_view{"newMax"},
                       std::string_view{"newMax"}},
         })
    {
        auto disconnectedDocument = parsed.document;
        std::erase_if(
            disconnectedDocument.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == sourceNode
                    && edge.toNode == "range"
                    && edge.toPort == port;
            });
        const auto disconnected = talas::runtime::compileProject(
            disconnectedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        if (port == "value")
        {
            require(!disconnected.ok(),
                "C122 disconnecting required value should fail closed");
            continue;
        }
        require(disconnected.ok(),
            "C122 disconnected optional bound should use its finite fallback");
        const auto disconnectedRender = talas::runtime::renderOffline(
            *disconnected.graph, renderedFrames);
        require(disconnectedRender.ok()
                && disconnectedRender.mono.size() == renderedFrames
                && !std::ranges::equal(
                    disconnectedRender.mono, offlineOutput),
            "C122 disconnecting authoritative " + std::string{port}
                + " should observably change mapped output");
    }

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.back().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C122 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C122 one-frame affine schedule mutation should observably change product output");
}

void testC123ProductConnectedFiniteMultiplicationExample()
{
    const auto path = examplePath(
        "c123_product_connected_finite_multiplication.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C123 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.product\"",
        "C123 product should expose Product");
    requireContains(source, "\"expression\": \"!C123Left\"",
        "C123 product should expose its connected left source");
    requireContains(source, "\"expression\": \"!C123Right\"",
        "C123 product should expose its connected right source");
    requireContains(source, "\"id\": \"gain\", \"expression\": \"0\"",
        "C123 product should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 7,
        "C123 project should own exact current-format four-phase dual-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C123 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C123 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C123 status should expose the mono terminal");
    requireContains(status.output, "type=talas.product",
        "C123 status should expose checked finite multiplication");
    requireContains(status.output, "resources: none",
        "C123 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C123 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                })
            && std::ranges::all_of(
                std::span<const double>{channels[0].samples}.subspan(
                    512, 256),
                [](double sample) {
                    return sample == 0.0;
                }),
        "C123 CLI render should expose finite audible positive, negative, and fractional phases plus exact signed-zero silence");
    std::array<double, 4> phasePeaks{};
    for (std::size_t phase = 0; phase < phasePeaks.size(); ++phase)
    {
        for (const auto sample : std::span<const double>{
                 channels[0].samples}.subspan(phase * 256, 256))
        {
            phasePeaks[phase] = std::max(
                phasePeaks[phase], std::abs(sample));
        }
    }
    require(std::abs(phasePeaks[0] - 0.2) <= 1.0e-12
            && std::abs(phasePeaks[1] - 0.3) <= 1.0e-12
            && phasePeaks[2] == 0.0
            && std::abs(phasePeaks[3] - 0.05) <= 1.0e-12,
        "C123 four phases should expose exact products 0.25, -0.375, signed zero, and -0.0625");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C123Left", 0.5, talas::expr::ValueDomain::Scalar},
        {"C123Right", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C123Left", -0.75, talas::expr::ValueDomain::Scalar},
        {512, "C123Left", 0.0, talas::expr::ValueDomain::Scalar},
        {512, "C123Right", -0.75, talas::expr::ValueDomain::Scalar},
        {768, "C123Left", 0.125, talas::expr::ValueDomain::Scalar},
        {768, "C123Right", -0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C123 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C123 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C123 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C123 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C123 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C123 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C123 reset/replacement should replay exact dual-control multiplication and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "product"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "left", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C123 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C123 bypassing Product should observably change output");

    auto addedDocument = parsed.document;
    const auto productNode = std::ranges::find_if(
        addedDocument.graph.nodes,
        [](const auto& graphNode) {
            return graphNode.instanceId == "product";
        });
    require(productNode != addedDocument.graph.nodes.end(),
        "C123 operation mutation should find Product");
    productNode->typeId = "talas.add";
    const auto added = talas::runtime::compileProject(
        addedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(added.ok(),
        "C123 addition mutation should remain structurally valid");
    const auto addedRender = talas::runtime::renderOffline(
        *added.graph, renderedFrames);
    require(addedRender.ok()
            && addedRender.mono.size() == renderedFrames
            && !std::ranges::equal(addedRender.mono, offlineOutput),
        "C123 replacing multiplication with addition should observably change output");

    // A Product of one Input plays it; one
    // without Inputs fails closed.
    for (const auto port : {std::string_view{"left"},
                            std::string_view{"right"}})
    {
        auto disconnectedDocument = parsed.document;
        std::erase_if(
            disconnectedDocument.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "product"
                    && edge.toPort == port;
            });
        const auto disconnected = talas::runtime::compileProject(
            disconnectedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(disconnected.ok(),
            "C123 a Product left with one Input should still compile");
    }
    {
        auto disconnectedDocument = parsed.document;
        std::erase_if(
            disconnectedDocument.graph.connections,
            [&](const auto& edge) { return edge.toNode == "product"; });
        const auto disconnected = talas::runtime::compileProject(
            disconnectedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(!disconnected.ok(),
            "C123 disconnecting every Product input should fail closed");
    }

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.back().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C123 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C123 one-frame multiplication schedule mutation should observably change product output");
}

void testC124DifferenceConnectedFiniteSubtractionExample()
{
    const auto path = examplePath(
        "c124_difference_connected_finite_subtraction.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C124 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.difference\"",
        "C124 example should expose Difference");
    requireContains(source, "\"expression\": \"!C124Left\"",
        "C124 example should expose its connected left source");
    requireContains(source, "\"expression\": \"!C124Right\"",
        "C124 example should expose its connected right source");
    requireContains(source, "\"id\": \"gain\", \"expression\": \"0\"",
        "C124 example should retain a finite inert connected VCA fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C124 project should own exact current-format four-phase dual-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C124 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C124 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C124 status should expose the mono terminal");
    requireContains(status.output, "type=talas.difference",
        "C124 status should expose checked finite subtraction");
    requireContains(status.output, "resources: none",
        "C124 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C124 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 1.0e-12;
                })
            && std::ranges::all_of(
                std::span<const double>{channels[0].samples}.subspan(
                    512, 256),
                [](double sample) {
                    return sample == 0.0;
                }),
        "C124 CLI render should expose finite audible positive, negative, and fractional phases plus exact cancellation silence");
    std::array<double, 4> phasePeaks{};
    for (std::size_t phase = 0; phase < phasePeaks.size(); ++phase)
    {
        for (const auto sample : std::span<const double>{
                 channels[0].samples}.subspan(phase * 256, 256))
        {
            phasePeaks[phase] = std::max(
                phasePeaks[phase], std::abs(sample));
        }
    }
    require(std::abs(phasePeaks[0] - 0.4) <= 1.0e-12
            && std::abs(phasePeaks[1] - 0.6) <= 1.0e-12
            && phasePeaks[2] == 0.0
            && std::abs(phasePeaks[3] - 0.4) <= 1.0e-12,
        "C124 four phases should expose exact differences 0.5, -0.75, zero, and 0.5");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C124Left", 0.75, talas::expr::ValueDomain::Scalar},
        {"C124Right", 0.25, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C124Left", -0.25, talas::expr::ValueDomain::Scalar},
        {256, "C124Right", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C124Left", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C124Right", 0.5, talas::expr::ValueDomain::Scalar},
        {768, "C124Left", 0.25, talas::expr::ValueDomain::Scalar},
        {768, "C124Right", -0.25, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C124 Difference example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C124 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C124 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C124 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C124 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C124 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C124 reset/replacement should replay exact ordered subtraction and mono output");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "difference"
                && edge.toNode == "vca";
        });
    bypassedDocument.graph.connections.push_back({
        "left", "value", "vca", "gain"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C124 bypass mutation should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && bypassedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C124 bypassing Difference should observably change output");

    auto addedDocument = parsed.document;
    const auto differenceNode = std::ranges::find_if(
        addedDocument.graph.nodes,
        [](const auto& graphNode) {
            return graphNode.instanceId == "difference";
        });
    require(differenceNode != addedDocument.graph.nodes.end(),
        "C124 operation mutation should find Difference");
    differenceNode->typeId = "talas.add";
    const auto added = talas::runtime::compileProject(
        addedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(added.ok(),
        "C124 addition mutation should remain structurally valid");
    const auto addedRender = talas::runtime::renderOffline(
        *added.graph, renderedFrames);
    require(addedRender.ok()
            && addedRender.mono.size() == renderedFrames
            && !std::ranges::equal(addedRender.mono, offlineOutput),
        "C124 replacing subtraction with addition should observably change output");

    auto swappedDocument = parsed.document;
    for (auto& edge : swappedDocument.graph.connections)
    {
        if (edge.toNode != "difference")
            continue;
        if (edge.toPort == "left")
            edge.toPort = "right";
        else if (edge.toPort == "right")
            edge.toPort = "left";
    }
    const auto swapped = talas::runtime::compileProject(
        swappedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(swapped.ok(),
        "C124 operand-order mutation should remain structurally valid");
    const auto swappedRender = talas::runtime::renderOffline(
        *swapped.graph, renderedFrames);
    require(swappedRender.ok()
            && swappedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                swappedRender.mono, offlineOutput),
        "C124 swapping Difference operands should observably change output");

    for (const auto port : {std::string_view{"left"},
                            std::string_view{"right"}})
    {
        auto disconnectedDocument = parsed.document;
        std::erase_if(
            disconnectedDocument.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "difference"
                    && edge.toPort == port;
            });
        const auto disconnected = talas::runtime::compileProject(
            disconnectedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(!disconnected.ok(),
            "C124 disconnecting required " + std::string{port}
                + " should fail closed");
    }

    auto mutatedSettings = settings;
    ++mutatedSettings.eventValueChanges.back().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C124 one-frame control mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C124 one-frame subtraction schedule mutation should observably change output");
}

void testC125SqrtMagnitudeConnectedSaturatedNormExample()
{
    const auto path = examplePath(
        "c125_sqrt_magnitude_connected_saturated_norm.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C125 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.sqrtMagnitude\"",
        "C125 example should expose Sqrt Magnitude");
    requireContains(source, "\"expression\": \"!C125LeftAmplitude\"",
        "C125 example should expose its connected left amplitude");
    requireContains(source, "\"expression\": \"!C125RightAmplitude\"",
        "C125 example should expose its connected right amplitude");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C125 project should own exact current-format four-phase dual-amplitude schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C125 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C125 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C125 status should expose the mono terminal");
    requireContains(status.output, "type=talas.sqrtMagnitude",
        "C125 status should expose checked saturated Euclidean magnitude");
    requireContains(status.output, "resources: none",
        "C125 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C125 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample)
                        && sample >= 0.0 && sample <= 1.0;
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return sample > 1.0e-12;
                }),
        "C125 CLI render should expose finite audible positive saturated magnitude");
    std::array<double, 4> phasePeaks{};
    std::array<std::size_t, 4> saturatedCounts{};
    for (std::size_t phase = 0; phase < phasePeaks.size(); ++phase)
    {
        for (const auto sample : std::span<const double>{
                 channels[0].samples}.subspan(phase * 256, 256))
        {
            phasePeaks[phase] = std::max(
                phasePeaks[phase], sample);
            saturatedCounts[phase] += sample == 1.0 ? 1U : 0U;
        }
    }
    require(std::abs(phasePeaks[0] - 0.5) <= 1.0e-12
            && std::abs(phasePeaks[1] - 0.5) <= 1.0e-12
            && phasePeaks[2] == 1.0
            && phasePeaks[3] == 1.0
            && saturatedCounts[3] > saturatedCounts[2],
        "C125 phases should expose mono-like 0.5, ordinary 0.5, exact-boundary 1.0, and observably saturated norms");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C125LeftAmplitude", 0.5, talas::expr::ValueDomain::Scalar},
        {"C125RightAmplitude", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C125LeftAmplitude", 0.3,
            talas::expr::ValueDomain::Scalar},
        {256, "C125RightAmplitude", 0.4,
            talas::expr::ValueDomain::Scalar},
        {512, "C125LeftAmplitude", 0.6,
            talas::expr::ValueDomain::Scalar},
        {512, "C125RightAmplitude", 0.8,
            talas::expr::ValueDomain::Scalar},
        {768, "C125LeftAmplitude", 0.8,
            talas::expr::ValueDomain::Scalar},
        {768, "C125RightAmplitude", 0.8,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C125 Sqrt Magnitude example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C125 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C125 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C125 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C125 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C125 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C125 reset/replacement should replay exact saturated magnitude output");

    for (const auto producer : {
             std::string_view{"left"}, std::string_view{"right"}})
    {
        auto bypassedDocument = parsed.document;
        std::erase_if(
            bypassedDocument.graph.connections,
            [](const auto& edge) {
                return edge.fromNode == "magnitude"
                    && edge.toNode == "out";
            });
        bypassedDocument.graph.connections.push_back({
            std::string{producer}, "audioOut", "out", "audioIn"});
        const auto bypassed = talas::runtime::compileProject(
            bypassedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(bypassed.ok(),
            "C125 raw-lane bypass should remain structurally valid");
        const auto bypassedRender = talas::runtime::renderOffline(
            *bypassed.graph, renderedFrames);
        require(bypassedRender.ok()
                && bypassedRender.mono.size() == renderedFrames
                && !std::ranges::equal(
                    bypassedRender.mono, offlineOutput),
            "C125 replacing Sqrt Magnitude with raw "
                + std::string{producer}
                + " lane should observably change output");
    }

    auto duplicatedDocument = parsed.document;
    const auto rightBinding = std::ranges::find_if(
        duplicatedDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "magnitude"
                && edge.toPort == "rightIn";
        });
    require(rightBinding != duplicatedDocument.graph.connections.end(),
        "C125 duplicate-producer mutation should expose rightIn");
    rightBinding->fromNode = "left";
    const auto duplicated = talas::runtime::compileProject(
        duplicatedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(duplicated.ok(),
        "C125 duplicate-producer mutation should remain structurally valid");
    const auto duplicatedRender = talas::runtime::renderOffline(
        *duplicated.graph, renderedFrames);
    require(duplicatedRender.ok()
            && !std::ranges::equal(
                duplicatedRender.mono, offlineOutput),
        "C125 duplicating the left producer into rightIn should observably change output");

    auto missingRightDocument = parsed.document;
    std::erase_if(
        missingRightDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "magnitude"
                && edge.toPort == "rightIn";
        });
    const auto missingRight = talas::runtime::compileProject(
        missingRightDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(missingRight.ok(),
        "C125 optional-right disconnect should retain mono authority");
    const auto missingRightRender = talas::runtime::renderOffline(
        *missingRight.graph, renderedFrames);
    require(missingRightRender.ok()
            && !std::ranges::equal(
                missingRightRender.mono, offlineOutput),
        "C125 disconnecting optional rightIn should observably select mono fallback");

    auto missingLeftDocument = parsed.document;
    std::erase_if(
        missingLeftDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "magnitude"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                missingLeftDocument,
                talas::core::makeBuiltinNodeRegistry(), settings).ok(),
        "C125 disconnecting required audioIn should fail closed");

    auto mutatedSettings = settings;
    --mutatedSettings.eventValueChanges[
        mutatedSettings.eventValueChanges.size() - 2].frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C125 one-frame amplitude mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C125 one-frame magnitude schedule mutation should observably change output");
}

void testC126VcaConnectedFiniteGainExample()
{
    const auto path = examplePath(
        "c126_vca_connected_finite_gain.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C126 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.vca\"",
        "C126 example should expose VCA");
    requireContains(source, "\"expression\": \"!C126Gain\"",
        "C126 example should expose its connected gain schedule");
    requireContains(source, "\"expression\": \"0.25\"",
        "C126 example should retain a distinct static fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C126 project should own exact current-format four-phase gain schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C126 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C126 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C126 status should expose the mono terminal");
    requireContains(status.output, "type=talas.vca",
        "C126 status should expose checked VCA execution");
    requireContains(status.output, "resources: none",
        "C126 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C126 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                }),
        "C126 CLI render should expose one finite mono VCA stream");
    std::array<double, 4> phasePeaks{};
    for (std::size_t phase = 0; phase < phasePeaks.size(); ++phase)
    {
        for (const auto sample : std::span<const double>{
                 channels[0].samples}.subspan(phase * 256, 256))
        {
            phasePeaks[phase] = std::max(
                phasePeaks[phase], std::abs(sample));
        }
    }
    require(std::abs(phasePeaks[0] - 0.4) <= 1.0e-12
            && std::abs(phasePeaks[1] - 0.6) <= 1.0e-12
            && phasePeaks[2] == 0.0
            && std::abs(phasePeaks[3] - 0.8) <= 1.0e-12
            && channels[0].samples[1] > 0.0
            && channels[0].samples[257] < 0.0,
        "C126 phases should expose positive half, negative three-quarter, exact zero, and unity gain");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C126Gain", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C126Gain", -0.75,
            talas::expr::ValueDomain::Scalar},
        {512, "C126Gain", 0.0,
            talas::expr::ValueDomain::Scalar},
        {768, "C126Gain", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C126 VCA example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C126 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C126 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C126 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C126 CLI, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C126 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C126 reset/replacement should replay exact VCA output");

    auto disconnectedDocument = parsed.document;
    std::erase_if(
        disconnectedDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "vca" && edge.toPort == "gain";
        });
    const auto disconnected = talas::runtime::compileProject(
        disconnectedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(disconnected.ok(),
        "C126 connected-gain disconnect should select static fallback");
    const auto disconnectedRender = talas::runtime::renderOffline(
        *disconnected.graph, renderedFrames);
    require(disconnectedRender.ok()
            && !std::ranges::equal(
                disconnectedRender.mono, offlineOutput),
        "C126 disconnecting gain should observably select the 0.25 fallback");

    auto inertFallbackDocument = parsed.document;
    const auto vca = std::ranges::find(
        inertFallbackDocument.graph.nodes,
        std::string_view{"vca"},
        &talas::project::ProjectNode::instanceId);
    require(vca != inertFallbackDocument.graph.nodes.end()
            && !vca->parameters.empty(),
        "C126 inert-fallback mutation should expose VCA gain");
    vca->parameters.front().expression = "0.125";
    const auto inertFallback = talas::runtime::compileProject(
        inertFallbackDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(inertFallback.ok(),
        "C126 connected VCA should admit a different inert fallback");
    const auto inertFallbackRender = talas::runtime::renderOffline(
        *inertFallback.graph, renderedFrames);
    require(inertFallbackRender.ok()
            && std::ranges::equal(inertFallbackRender.mono, offlineOutput),
        "C126 connected gain should make the static fallback observably inert");

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "vca" && edge.toNode == "out";
        });
    bypassedDocument.graph.connections.push_back({
        "carrier", "audioOut", "out", "audioIn"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C126 VCA bypass should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C126 bypassing VCA should observably change output");

    auto missingAudioDocument = parsed.document;
    std::erase_if(
        missingAudioDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "vca"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                missingAudioDocument,
                talas::core::makeBuiltinNodeRegistry(), settings).ok(),
        "C126 disconnecting required audioIn should fail closed");

    auto mutatedSettings = settings;
    --mutatedSettings.eventValueChanges.back().frame;
    const auto mutated = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mutatedSettings));
    require(mutated.ok(),
        "C126 one-frame gain mutation should remain structurally valid");
    const auto mutatedRender = talas::runtime::renderOffline(
        *mutated.graph, renderedFrames);
    require(mutatedRender.ok()
            && mutatedRender.mono.size() == renderedFrames
            && !std::ranges::equal(
                mutatedRender.mono, offlineOutput),
        "C126 one-frame gain schedule mutation should observably change output");
}

void testC127ConstantFiniteEventValueBroadcastExample()
{
    const auto path = examplePath(
        "c127_constant_finite_event_value_broadcast.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C127 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.constant\"",
        "C127 example should expose Constant");
    requireContains(source, "\"expression\": \"!C127Value\"",
        "C127 example should expose its scheduled scalar expression");
    requireConnection(source, {"constant", "value", "leftVca", "gain"},
        "C127 example should expose the left fan-out edge");
    requireConnection(source, {"constant", "value", "rightVca", "gain"},
        "C127 example should expose the right fan-out edge");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C127 project should own exact current-format four-segment Constant authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C127 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C127 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C127 status should expose the mono terminal");
    requireContains(status.output, "type=talas.constant",
        "C127 status should expose checked Constant execution");
    requireContains(status.output, "resources: none",
        "C127 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C127 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && channels[0].samples[1] > 0.0
            && channels[0].samples[257] < 0.0
            && std::ranges::all_of(
                std::span<const double>{channels[0].samples}.subspan(
                    512, 256),
                [](double sample) { return sample == 0.0; })
            && std::ranges::any_of(
                std::span<const double>{channels[0].samples}.subspan(
                    768, 256),
                [](double sample) {
                    return std::abs(sample) > 0.1;
                }),
        "C127 CLI stream should expose positive, negative, exact-zero, and audible unity Constant phases");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C127Value", 0.5, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {256, "C127Value", -0.75,
            talas::expr::ValueDomain::Scalar},
        {512, "C127Value", 0.0,
            talas::expr::ValueDomain::Scalar},
        {768, "C127Value", 1.0,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C127 Constant example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C127 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C127 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C127 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C127 CLI, Save/reopen, offline, and prepared fan-out renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C127 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C127 reset/replacement should replay exact Constant fan-out output");

    for (const auto consumer : {
             std::string_view{"leftVca"},
             std::string_view{"rightVca"}})
    {
        auto disconnectedDocument = parsed.document;
        std::erase_if(
            disconnectedDocument.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == "constant"
                    && edge.toNode == consumer
                    && edge.toPort == "gain";
            });
        const auto disconnected = talas::runtime::compileProject(
            disconnectedDocument,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(disconnected.ok(),
            "C127 fan-out disconnect should select one static VCA fallback");
        const auto disconnectedRender = talas::runtime::renderOffline(
            *disconnected.graph, renderedFrames);
        require(disconnectedRender.ok()
                && !std::ranges::equal(
                    disconnectedRender.mono, offlineOutput),
            "C127 disconnecting either Constant consumer should observably change output");
    }

    auto bypassedDocument = parsed.document;
    std::erase_if(
        bypassedDocument.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "leftVca"
                && edge.toNode == "mix";
        });
    bypassedDocument.graph.connections.push_back({
        "leftCarrier", "audioOut", "mix", "inputs"});
    const auto bypassed = talas::runtime::compileProject(
        bypassedDocument,
        talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassed.ok(),
        "C127 one-lane VCA bypass should remain structurally valid");
    const auto bypassedRender = talas::runtime::renderOffline(
        *bypassed.graph, renderedFrames);
    require(bypassedRender.ok()
            && !std::ranges::equal(
                bypassedRender.mono, offlineOutput),
        "C127 bypassing one Constant consumer should observably change output");

    auto missingInputDocument = parsed.document;
    std::erase_if(
        missingInputDocument.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "leftVca"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                missingInputDocument,
                talas::core::makeBuiltinNodeRegistry(), settings).ok(),
        "C127 disconnecting a required consumer audio input should fail closed");

    auto signSettings = settings;
    signSettings.eventValues.front().value = -0.5;
    const auto signMutation = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(signSettings));
    require(signMutation.ok(),
        "C127 sign mutation should remain structurally valid");
    const auto signRender = talas::runtime::renderOffline(
        *signMutation.graph, renderedFrames);
    require(signRender.ok()
            && !std::ranges::equal(signRender.mono, offlineOutput),
        "C127 initial Constant sign mutation should observably change output");

    auto scheduleSettings = settings;
    --scheduleSettings.eventValueChanges.back().frame;
    const auto scheduleMutation = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(scheduleSettings));
    require(scheduleMutation.ok(),
        "C127 one-frame schedule mutation should remain structurally valid");
    const auto scheduleRender = talas::runtime::renderOffline(
        *scheduleMutation.graph, renderedFrames);
    require(scheduleRender.ok()
            && !std::ranges::equal(
                scheduleRender.mono, offlineOutput),
        "C127 one-frame Constant schedule mutation should observably change output");
}

void testC128AddFiniteEventValueSumExample()
{
    const auto path = examplePath(
        "c128_add_finite_event_value_sum.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C128 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.add\"",
        "C128 example should expose Add");
    requireContains(source, "\"expression\": \"!C128Left\"",
        "C128 example should expose its scheduled left operand");
    requireConnection(source, {"add", "value", "leftVca", "gain"},
        "C128 example should expose the left Add fan-out edge");
    requireConnection(source, {"add", "value", "rightVca", "gain"},
        "C128 example should expose the right Add fan-out edge");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C128 project should own exact current-format four-segment Add authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C128 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C128 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C128 status should expose the mono terminal");
    requireContains(status.output, "type=talas.add",
        "C128 status should expose checked Add execution");
    requireContains(status.output, "resources: none",
        "C128 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C128 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                std::span<const double>{channels[0].samples}.first(768),
                [](double sample) {
                    return std::abs(sample) > 0.01;
                })
            && std::ranges::all_of(
                std::span<const double>{channels[0].samples}.last(256),
                [](double sample) { return sample == 0.0; }),
        "C128 CLI stream should be finite, audible before the final exact-zero sum, and silent after it");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C128Left", 0.25, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {256, "C128Left", -0.5,
            talas::expr::ValueDomain::Scalar},
        {512, "C128Left", 0.75,
            talas::expr::ValueDomain::Scalar},
        {768, "C128Left", -0.25,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C128 Add example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C128 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C128 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C128 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C128 CLI, Save/reopen, offline, and prepared Add renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C128 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C128 reset/replacement should replay exact Add fan-out output");

    for (const auto inputPort : {
             std::string_view{"left"},
             std::string_view{"right"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(
            disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "add"
                    && edge.toPort == inputPort;
            });
        require(!talas::runtime::compileProject(
                    disconnected,
                    talas::core::makeBuiltinNodeRegistry(), settings).ok(),
            "C128 disconnecting either required Add input should fail closed");
    }

    for (const auto consumer : {
             std::string_view{"leftVca"},
             std::string_view{"rightVca"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(
            disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == "add"
                    && edge.toNode == consumer
                    && edge.toPort == "gain";
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected,
            talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C128 fan-out disconnect should select the VCA fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, renderedFrames);
        require(output.ok()
                && !std::ranges::equal(
                    output.mono, offlineOutput),
            "C128 disconnecting either Add consumer should observably change output");
    }

    auto bypassed = parsed.document;
    std::erase_if(
        bypassed.graph.connections,
        [](const auto& edge) {
            return edge.fromNode == "leftVca"
                && edge.toNode == "mix";
        });
    bypassed.graph.connections.push_back({
        "leftCarrier", "audioOut", "mix", "inputs"});
    const auto bypassMutation = talas::runtime::compileProject(
        bypassed, talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassMutation.ok(),
        "C128 one-lane VCA bypass should remain structurally valid");
    const auto bypassOutput = talas::runtime::renderOffline(
        *bypassMutation.graph, renderedFrames);
    require(bypassOutput.ok()
            && !std::ranges::equal(
                bypassOutput.mono, offlineOutput),
        "C128 bypassing one Add consumer should observably change output");

    auto overflowSettings = settings;
    overflowSettings.eventValues.front().value =
        std::numeric_limits<double>::max();
    auto overflowDocument = parsed.document;
    const auto overflowRight = std::ranges::find(
        overflowDocument.graph.nodes,
        std::string_view{"right"},
        &talas::project::ProjectNode::instanceId);
    require(overflowRight != overflowDocument.graph.nodes.end()
            && overflowRight->parameters.size() == 1,
        "C128 overflow mutation should expose the static right operand");
    overflowRight->parameters.front().expression =
        "1.7976931348623157e308";
    const auto overflow = talas::runtime::compileProject(
        overflowDocument,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(overflowSettings));
    require(overflow.ok(),
        "C128 finite overflow operands should remain structurally valid");
    const auto overflowOutput = talas::runtime::renderOffline(
        *overflow.graph, renderedFrames);
    require(!overflowOutput.ok() && overflowOutput.mono.empty(),
        "C128 overflowing Add example mutation should fail atomically");

    auto scheduleSettings = settings;
    --scheduleSettings.eventValueChanges.back().frame;
    const auto scheduleMutation = talas::runtime::compileProject(
        parsed.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(scheduleSettings));
    require(scheduleMutation.ok(),
        "C128 one-frame schedule mutation should remain structurally valid");
    const auto scheduleOutput = talas::runtime::renderOffline(
        *scheduleMutation.graph, renderedFrames);
    require(scheduleOutput.ok()
            && !std::ranges::equal(
                scheduleOutput.mono, offlineOutput),
        "C128 one-frame Add schedule mutation should observably change output");
}

void testC129MixerCheckedFiniteWeightedSumExample()
{
    const auto path = examplePath(
        "c129_mixer_checked_finite_weighted_sum.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 43",
        "C129 example should be persisted in format 43");
    requireContains(source, "\"typeId\": \"talas.mixerV2\"",
        "C129 example should expose the Mixer of many Inputs");
    requireContains(source, "\"expression\": \"!C129Gain1\"",
        "C129 example should expose its scheduled connected gain");
    const auto parsed = talas::project::parseProjectDocument(source);
    // Format 43 keeps each weight on a VCA feeding the Mixer's Inputs.
    const auto hasEdge = [&](const talas::core::Connection& edge) {
        return std::ranges::find(parsed.document.graph.connections, edge)
            != parsed.document.graph.connections.end();
    };
    require(hasEdge({"mixer", "leftOut", "monitor", "audioIn"}),
        "C129 example should expose the monitored Mixer fan-out edge");
    require(hasEdge({"mixer", "leftOut", "vca3", "audioIn"}),
        "C129 example should expose the weighted direct Mixer fan-out edge");
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C129 project should own exact current-format four-segment Mixer authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C129 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C129 CLI status should succeed");
    requireContains(status.output, "outputs: mono (1)",
        "C129 status should expose the mono terminal");
    requireContains(status.output, "type=talas.mixerV2",
        "C129 status should expose checked Mixer execution");
    requireContains(status.output, "resources: none",
        "C129 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C129 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 1 && channels[0].name == "left"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 0.01;
                }),
        "C129 CLI stream should remain finite and audible across all scheduled gain segments");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C129Gain1", 0.25, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {256, "C129Gain1", -0.5,
            talas::expr::ValueDomain::Scalar},
        {512, "C129Gain1", 0.75,
            talas::expr::ValueDomain::Scalar},
        {768, "C129Gain1", -0.25,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C129 Mixer example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C129 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineOutput{};
    std::array<double, renderedFrames> realtimeOutput{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames,
            "C129 offline irregular partition should render mono");
        std::ranges::copy(
            block.mono,
            offlineOutput.begin()
                + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeOutput}.subspan(
                        offset, frames)),
            "C129 prepared irregular partition should render mono");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineOutput == realtimeOutput
            && std::ranges::equal(
                realtimeOutput, channels[0].samples),
        "C129 CLI, Save/reopen, offline, and prepared Mixer renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> reset{};
    std::array<double, renderedFrames> replaced{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{reset}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replaced}.subspan(offset, frames)),
            "C129 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(reset == replaced && reset == realtimeOutput,
        "C129 reset/replacement should replay exact Mixer output");

    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "mixer"
                    && edge.toPort == "inputs";
            });
        require(!talas::runtime::compileProject(
                    disconnected,
                    talas::core::makeBuiltinNodeRegistry(), settings).ok(),
            "C129 a Mixer without Inputs should fail closed");
    }
    for (const auto weighted : {
             std::string_view{"vca1"}, std::string_view{"vca2"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == weighted
                    && edge.toNode == "mixer";
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C129 a Mixer should play the Inputs that remain");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, renderedFrames);
        require(output.ok()
                && !std::ranges::equal(output.mono, offlineOutput),
            "C129 removing either weighted Input should observably change output");
    }

    for (const auto weighted : {
             std::string_view{"vca1"}, std::string_view{"vca2"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == weighted
                    && edge.toPort == "gain";
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C129 disconnected gain should select its finite expression fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, renderedFrames);
        require(output.ok()
                && !std::ranges::equal(output.mono, offlineOutput),
            "C129 disconnecting either connected gain should observably change weighted output");
    }

    for (const auto consumer : {
             std::string_view{"monitor"},
             std::string_view{"vca3"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == "mixer"
                    && edge.toNode == consumer;
            });
        require(!talas::runtime::compileProject(
                    disconnected,
                    talas::core::makeBuiltinNodeRegistry(), settings).ok(),
            "C129 disconnecting either required Mixer fan-out consumer should fail closed");
    }

    auto bypassed = parsed.document;
    std::erase_if(bypassed.graph.connections, [](const auto& edge) {
        return edge.fromNode == "monitor"
            && edge.toNode == "vca4";
    });
    bypassed.graph.connections.push_back({
        "mixer", "leftOut", "vca4", "audioIn"});
    const auto bypassMutation = talas::runtime::compileProject(
        bypassed, talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassMutation.ok(),
        "C129 downstream VCA bypass should remain structurally valid");
    const auto bypassOutput = talas::runtime::renderOffline(
        *bypassMutation.graph, renderedFrames);
    require(bypassOutput.ok()
            && !std::ranges::equal(bypassOutput.mono, offlineOutput),
        "C129 bypassing the monitored branch should observably change output");

    auto reordered = parsed.document;
    for (auto& edge : reordered.graph.connections)
    {
        if (edge.toPort != "audioIn")
            continue;
        if (edge.toNode == "vca1")
            edge.toNode = "vca2";
        else if (edge.toNode == "vca2")
            edge.toNode = "vca1";
    }
    const auto reorderedMutation = talas::runtime::compileProject(
        reordered, talas::core::makeBuiltinNodeRegistry(), settings);
    require(reorderedMutation.ok(),
        "C129 ordered-branch mutation should remain structurally valid");
    const auto reorderedOutput = talas::runtime::renderOffline(
        *reorderedMutation.graph, renderedFrames);
    require(reorderedOutput.ok()
            && !std::ranges::equal(reorderedOutput.mono, offlineOutput),
        "C129 swapping audio branches under unequal gains should observably change output");

    auto hiddenFallback = parsed.document;
    for (const auto weighted : {
             std::string_view{"vca1"}, std::string_view{"vca2"}})
    {
        const auto hiddenVca = std::ranges::find(
            hiddenFallback.graph.nodes,
            weighted,
            &talas::project::ProjectNode::instanceId);
        require(hiddenVca != hiddenFallback.graph.nodes.end()
                && hiddenVca->parameters.size() == 1,
            "C129 hidden-fallback fixture should expose each weight's gain");
        hiddenVca->parameters[0].expression = "0";
    }
    const auto hiddenFallbackGraph = talas::runtime::compileProject(
        hiddenFallback, talas::core::makeBuiltinNodeRegistry(), settings);
    require(hiddenFallbackGraph.ok(),
        "C129 connected gains should own inert finite fallbacks");
    const auto hiddenFallbackOutput = talas::runtime::renderOffline(
        *hiddenFallbackGraph.graph, renderedFrames);
    require(hiddenFallbackOutput.ok()
            && std::ranges::equal(
                hiddenFallbackOutput.mono, offlineOutput),
        "C129 connected-gain precedence should ignore both fallback values exactly");

    auto overflowSettings = settings;
    overflowSettings.eventValues.front().value =
        std::numeric_limits<double>::max();
    auto overflowDocument = parsed.document;
    const auto overflowLeft = std::ranges::find(
        overflowDocument.graph.nodes,
        std::string_view{"left"},
        &talas::project::ProjectNode::instanceId);
    require(overflowLeft != overflowDocument.graph.nodes.end()
            && overflowLeft->parameters.size() >= 2,
        "C129 overflow mutation should expose the left carrier");
    overflowLeft->parameters[1].expression = "2";
    const auto overflow = talas::runtime::compileProject(
        overflowDocument, talas::core::makeBuiltinNodeRegistry(),
        std::move(overflowSettings));
    require(overflow.ok(),
        "C129 finite overflow operands should remain structurally valid");
    const auto overflowOutput = talas::runtime::renderOffline(
        *overflow.graph, renderedFrames);
    require(!overflowOutput.ok() && overflowOutput.mono.empty(),
        "C129 overflowing Mixer example mutation should fail atomically");

    auto scheduleSettings = settings;
    --scheduleSettings.eventValueChanges.back().frame;
    const auto scheduleMutation = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        std::move(scheduleSettings));
    require(scheduleMutation.ok(),
        "C129 one-frame schedule mutation should remain structurally valid");
    const auto scheduleOutput = talas::runtime::renderOffline(
        *scheduleMutation.graph, renderedFrames);
    require(scheduleOutput.ok()
            && !std::ranges::equal(
                scheduleOutput.mono, offlineOutput),
        "C129 one-frame Mixer schedule mutation should observably change output");
}

void testC130LevelCheckedFiniteStereoGainExample()
{
    const auto path = examplePath(
        "c130_level_checked_finite_stereo_gain.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C130 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.level\"",
        "C130 example should expose Level");
    requireContains(source, "\"expression\": \"!C130LeftGain\"",
        "C130 example should expose its scheduled connected left gain");
    requireContains(source,
        "\"fromNode\": \"level\", \"fromPort\": \"leftOut\", \"toNode\": \"leftMonitor\", \"toPort\": \"audioIn\"",
        "C130 example should expose the monitored Level left branch");
    requireContains(source,
        "\"fromNode\": \"level\", \"fromPort\": \"rightOut\", \"toNode\": \"rightMonitor\", \"toPort\": \"audioIn\"",
        "C130 example should expose the monitored Level right branch");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C130 project should own exact current-format four-segment stereo Level authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C130 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C130 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C130 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.level",
        "C130 status should expose checked Level execution");
    requireContains(status.output, "resources: none",
        "C130 status should expose the resource-free contract");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C130 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right"
            && std::ranges::all_of(
                channels[0].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::all_of(
                channels[1].samples, [](double sample) {
                    return std::isfinite(sample);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double sample) {
                    return std::abs(sample) > 0.01;
                })
            && std::ranges::any_of(
                channels[1].samples, [](double sample) {
                    return std::abs(sample) > 0.01;
                })
            && channels[0].samples != channels[1].samples,
        "C130 CLI stereo streams should remain finite, audible, and independently gained across all schedule segments");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C130LeftGain", 0.25, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {256, "C130LeftGain", -0.5,
            talas::expr::ValueDomain::Scalar},
        {512, "C130LeftGain", 0.75,
            talas::expr::ValueDomain::Scalar},
        {768, "C130LeftGain", -0.25,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C130 Level example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C130 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineLeft{};
    std::array<double, renderedFrames> offlineRight{};
    std::array<double, renderedFrames> realtimeLeft{};
    std::array<double, renderedFrames> realtimeRight{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames
                && block.right.size() == frames,
            "C130 offline irregular partition should render stereo");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeLeft}.subspan(offset, frames),
                    std::span<double>{realtimeRight}.subspan(offset, frames)),
            "C130 prepared irregular partition should render stereo");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineLeft == realtimeLeft
            && offlineRight == realtimeRight
            && std::ranges::equal(
                realtimeLeft, channels[0].samples)
            && std::ranges::equal(
                realtimeRight, channels[1].samples),
        "C130 CLI, Save/reopen, offline, and prepared Level renders should be bit-identical on both channels across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> resetLeft{};
    std::array<double, renderedFrames> resetRight{};
    std::array<double, renderedFrames> replacedLeft{};
    std::array<double, renderedFrames> replacedRight{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{resetLeft}.subspan(offset, frames),
                    std::span<double>{resetRight}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replacedLeft}.subspan(offset, frames),
                    std::span<double>{replacedRight}.subspan(offset, frames)),
            "C130 reset/replacement irregular partition should render stereo");
        offset += frames;
        ++partitionIndex;
    }
    require(resetLeft == replacedLeft
            && resetRight == replacedRight
            && resetLeft == realtimeLeft
            && resetRight == realtimeRight,
        "C130 reset/replacement should replay exact Level stereo output");

    auto disconnectedAudio = parsed.document;
    std::erase_if(disconnectedAudio.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "level"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                disconnectedAudio,
                talas::core::makeBuiltinNodeRegistry(), settings).ok(),
        "C130 disconnecting the required Level audio input should fail closed");

    for (const auto gainPort : {
             std::string_view{"left"}, std::string_view{"right"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "level"
                    && edge.toPort == gainPort;
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C130 disconnected gain should select its finite Level expression fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, renderedFrames);
        require(output.ok()
                && (gainPort == "left"
                    ? !std::ranges::equal(output.mono, offlineLeft)
                    : !std::ranges::equal(output.right, offlineRight)),
            "C130 disconnecting either connected gain should observably change its own Level channel");
    }

    for (const auto consumer : {
             std::string_view{"leftMonitor"},
             std::string_view{"rightMonitor"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == "level"
                    && edge.toNode == consumer;
            });
        require(!talas::runtime::compileProject(
                    disconnected,
                    talas::core::makeBuiltinNodeRegistry(), settings).ok(),
            "C130 disconnecting either required monitored Level branch should fail closed");
    }

    auto bypassed = parsed.document;
    std::erase_if(bypassed.graph.connections, [](const auto& edge) {
        return (edge.fromNode == "level"
                   && edge.toNode == "leftMonitor")
            || (edge.fromNode == "leftMonitor"
                && edge.toNode == "out");
    });
    bypassed.graph.connections.push_back({
        "level", "leftOut", "out", "audioIn"});
    const auto bypassMutation = talas::runtime::compileProject(
        bypassed, talas::core::makeBuiltinNodeRegistry(), settings);
    require(bypassMutation.ok(),
        "C130 downstream left VCA bypass should remain structurally valid");
    const auto bypassOutput = talas::runtime::renderOffline(
        *bypassMutation.graph, renderedFrames);
    require(bypassOutput.ok()
            && !std::ranges::equal(bypassOutput.mono, offlineLeft)
            && std::ranges::equal(bypassOutput.right, offlineRight),
        "C130 bypassing one monitored branch should change only that terminal channel");

    auto swapped = parsed.document;
    for (auto& edge : swapped.graph.connections)
    {
        if (edge.toNode != "out")
            continue;
        if (edge.toPort == "audioIn")
            edge.toPort = "rightIn";
        else if (edge.toPort == "rightIn")
            edge.toPort = "audioIn";
    }
    const auto swappedMutation = talas::runtime::compileProject(
        swapped, talas::core::makeBuiltinNodeRegistry(), settings);
    require(swappedMutation.ok(),
        "C130 channel-swap mutation should remain structurally valid");
    const auto swappedOutput = talas::runtime::renderOffline(
        *swappedMutation.graph, renderedFrames);
    require(swappedOutput.ok()
            && std::ranges::equal(swappedOutput.mono, offlineRight)
            && std::ranges::equal(swappedOutput.right, offlineLeft),
        "C130 swapping terminal branches should exchange exact stereo channels");

    auto hiddenFallback = parsed.document;
    const auto hiddenLevel = std::ranges::find(
        hiddenFallback.graph.nodes,
        std::string_view{"level"},
        &talas::project::ProjectNode::instanceId);
    require(hiddenLevel != hiddenFallback.graph.nodes.end()
            && hiddenLevel->parameters.size() == 3,
        "C130 hidden-fallback fixture should expose both gains and noGain");
    hiddenLevel->parameters[0].expression = "0";
    hiddenLevel->parameters[1].expression = "0";
    hiddenLevel->parameters[2].boolean = true;
    const auto hiddenFallbackGraph = talas::runtime::compileProject(
        hiddenFallback, talas::core::makeBuiltinNodeRegistry(), settings);
    require(hiddenFallbackGraph.ok(),
        "C130 connected gains and inert noGain should retain valid Level authority");
    const auto hiddenFallbackOutput = talas::runtime::renderOffline(
        *hiddenFallbackGraph.graph, renderedFrames);
    require(hiddenFallbackOutput.ok()
            && std::ranges::equal(
                hiddenFallbackOutput.mono, offlineLeft)
            && std::ranges::equal(
                hiddenFallbackOutput.right, offlineRight),
        "C130 connected-gain precedence should ignore both fallbacks and inert noGain exactly");

    auto muted = parsed.document;
    const auto rightGain = std::ranges::find(
        muted.graph.nodes,
        std::string_view{"rightGain"},
        &talas::project::ProjectNode::instanceId);
    require(rightGain != muted.graph.nodes.end()
            && rightGain->parameters.size() == 1,
        "C130 mute fixture should expose the connected right gain");
    rightGain->parameters[0].expression = "0";
    const auto mutedGraph = talas::runtime::compileProject(
        muted, talas::core::makeBuiltinNodeRegistry(), settings);
    require(mutedGraph.ok(),
        "C130 finite right mute should remain structurally valid");
    const auto mutedOutput = talas::runtime::renderOffline(
        *mutedGraph.graph, renderedFrames);
    require(mutedOutput.ok()
            && std::ranges::equal(mutedOutput.mono, offlineLeft)
            && std::ranges::all_of(
                mutedOutput.right,
                [](double sample) { return sample == 0.0; }),
        "C130 independent right mute should preserve left output and silence the right channel");

    auto overflowSettings = settings;
    overflowSettings.eventValues.front().value =
        std::numeric_limits<double>::max();
    auto overflowDocument = parsed.document;
    const auto carrier = std::ranges::find(
        overflowDocument.graph.nodes,
        std::string_view{"carrier"},
        &talas::project::ProjectNode::instanceId);
    require(carrier != overflowDocument.graph.nodes.end()
            && carrier->parameters.size() >= 2,
        "C130 overflow mutation should expose the Level carrier");
    carrier->parameters[1].expression = "2";
    const auto overflow = talas::runtime::compileProject(
        overflowDocument, talas::core::makeBuiltinNodeRegistry(),
        std::move(overflowSettings));
    require(overflow.ok(),
        "C130 finite overflow operands should remain structurally valid");
    const auto overflowOutput = talas::runtime::renderOffline(
        *overflow.graph, renderedFrames);
    require(!overflowOutput.ok()
            && overflowOutput.mono.empty()
            && overflowOutput.right.empty(),
        "C130 overflowing Level example mutation should reject both channels atomically");

    auto scheduleSettings = settings;
    --scheduleSettings.eventValueChanges.back().frame;
    const auto scheduleMutation = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(),
        std::move(scheduleSettings));
    require(scheduleMutation.ok(),
        "C130 one-frame schedule mutation should remain structurally valid");
    const auto scheduleOutput = talas::runtime::renderOffline(
        *scheduleMutation.graph, renderedFrames);
    require(scheduleOutput.ok()
            && !std::ranges::equal(
                scheduleOutput.mono, offlineLeft)
            && std::ranges::equal(
                scheduleOutput.right, offlineRight),
        "C130 one-frame Level schedule mutation should change only the scheduled left channel");
}

void testC111CloudBankCheckedSpectrumExample()
{
    const auto path = examplePath(
        "c111_cloud_bank_checked_spectrum.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C111 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.cloudBank\"",
        "C111 example should expose Cloud Bank");
    requireContains(source, "\"typeId\": \"talas.syntheticSpectrumFromArray\"",
        "C111 example should expose its packed Spectrum producer");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 2
            && parsed.document.timeline.eventValues.size() == 8,
        "C111 project should own exact current-format tables, Spectrum, and dynamic-control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C111 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C111 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C111 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.cloudBank",
        "C111 status should expose the session-stateful Cloud Bank operation");
    requireContains(status.output,
        "Tables/C111Wave kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C111 status should expose exact reused waveform-table authority");
    requireContains(status.output,
        "Tables/C111Envelope kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C111 status should expose exact reused envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C111 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right",
        "C111 CLI render should expose exact stereo channel order");
    require(std::ranges::any_of(channels[0].samples, [](double sample) {
                return std::abs(sample) > 1.0e-12;
            })
            && channels[0].samples != channels[1].samples,
        "C111 CLI render should be audible and spatially distinct");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C111Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {"C111Pan", 0.2, talas::expr::ValueDomain::Scalar},
        {"C111FrequencyMod", 0.0, talas::expr::ValueDomain::Scalar},
        {"C111DurationMod", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C111Pan", 0.8, talas::expr::ValueDomain::Scalar},
        {256, "C111FrequencyMod", 0.25, talas::expr::ValueDomain::Scalar},
        {512, "C111Pan", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C111DurationMod", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Tables/C111Wave", {
            0.0, 0.3826834323650898, 0.7071067811865475,
            0.9238795325112867, 1.0, 0.9238795325112867,
            0.7071067811865476, 0.3826834323650899,
            0.0000000000000001, -0.3826834323650897,
            -0.7071067811865475, -0.9238795325112865,
            -1.0, -0.9238795325112866, -0.7071067811865477,
            -0.3826834323650904,
        }},
        {"Tables/C111Envelope", {0.25, 0.5, 0.75, 1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C111 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C111 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineLeft{};
    std::array<double, renderedFrames> offlineRight{};
    std::array<double, renderedFrames> realtimeLeft{};
    std::array<double, renderedFrames> realtimeRight{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames
                && block.right.size() == frames,
            "C111 offline irregular partition should render stereo");
        std::ranges::copy(block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeLeft}.subspan(offset, frames),
                    std::span<double>{realtimeRight}.subspan(offset, frames)),
            "C111 prepared irregular partition should render stereo");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineLeft == realtimeLeft
            && offlineRight == realtimeRight
            && std::ranges::equal(realtimeLeft, channels[0].samples)
            && std::ranges::equal(realtimeRight, channels[1].samples),
        "C111 CLI, Save/reopen, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> resetLeft{};
    std::array<double, renderedFrames> resetRight{};
    std::array<double, renderedFrames> replacedLeft{};
    std::array<double, renderedFrames> replacedRight{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{resetLeft}.subspan(offset, frames),
                    std::span<double>{resetRight}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replacedLeft}.subspan(offset, frames),
                    std::span<double>{replacedRight}.subspan(offset, frames)),
            "C111 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(resetLeft == replacedLeft && resetRight == replacedRight
            && resetLeft == realtimeLeft && resetRight == realtimeRight,
        "C111 reset/replacement should replay exact Spectrum, control, cursor, and stereo state");
}

void testC112CloudBankElementRangeCascadeDelayExample()
{
    const auto path = examplePath(
        "c112_cloud_bank_element_range_cascade_delay.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C112 example should be persisted in format 35");
    requireContains(source, "\"typeId\": \"talas.cloudBankElement\"",
        "C112 example should expose Cloud Bank Element");
    requireContains(source, "\"typeId\": \"talas.syntheticSpectrumFromArray\"",
        "C112 example should expose its packed Spectrum producer");
    requireContains(source, "\"toPort\": \"cascadeInput\"",
        "C112 example should expose its mono cascade binding");
    requireContains(source, "\"expression\": \"0.00018310546875 s\"",
        "C112 example should preserve its 1.5-frame delay at 8192 Hz");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 2
            && parsed.document.timeline.eventValues.size() == 8,
        "C112 project should own exact current-format tables, Spectrum, cascade, delay, and control schedule authority");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(parsed.document));
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == talas::project::serializeProjectDocument(parsed.document),
        "C112 Save/reopen should preserve exact canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C112 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C112 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.cloudBankElement",
        "C112 status should expose the session-stateful Element operation");
    requireContains(status.output,
        "Tables/C112Wave kind=table generation=1 hash=54e58fa19e33c65f013a9758f497efe8481f684876756f6ab60e79ef6865dcd3 health=verified",
        "C112 status should expose exact reused waveform-table authority");
    requireContains(status.output,
        "Tables/C112Envelope kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C112 status should expose exact reused envelope-table authority");

    constexpr std::size_t renderedFrames = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(renderedFrames), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C112 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, renderedFrames, 8192.0);
    require(channels.size() == 2
            && channels[0].name == "left"
            && channels[1].name == "right",
        "C112 CLI render should expose exact stereo channel order");
    require(std::ranges::any_of(channels[0].samples, [](double sample) {
                return std::abs(sample) > 1.0e-12;
            })
            && channels[0].samples != channels[1].samples,
        "C112 CLI render should expose audible cascade and spatially distinct grains");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C112Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {"C112Pan", 0.2, talas::expr::ValueDomain::Scalar},
        {"C112FrequencyMod", 0.0, talas::expr::ValueDomain::Scalar},
        {"C112DurationMod", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C112Pan", 0.8, talas::expr::ValueDomain::Scalar},
        {256, "C112FrequencyMod", 0.25, talas::expr::ValueDomain::Scalar},
        {512, "C112Pan", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C112DurationMod", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources = {
        {"Tables/C112Wave", {
            0.0, 0.3826834323650898, 0.7071067811865475,
            0.9238795325112867, 1.0, 0.9238795325112867,
            0.7071067811865476, 0.3826834323650899,
            0.0000000000000001, -0.3826834323650897,
            -0.7071067811865475, -0.9238795325112865,
            -1.0, -0.9238795325112866, -0.7071067811865477,
            -0.3826834323650904,
        }},
        {"Tables/C112Envelope", {0.25, 0.5, 0.75, 1.0}},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C112 product example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    talas::runtime::RealtimeRenderSession replacement{*compiled.graph};
    require(prepared.ready() && replacement.ready(),
        "C112 lifecycle fixtures should prepare: "
            + std::string{prepared.error()});

    std::array<double, renderedFrames> offlineLeft{};
    std::array<double, renderedFrames> offlineRight{};
    std::array<double, renderedFrames> realtimeLeft{};
    std::array<double, renderedFrames> realtimeRight{};
    constexpr std::array<std::size_t, 7> partitionPattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.mono.size() == frames
                && block.right.size() == frames,
            "C112 offline irregular partition should render stereo");
        std::ranges::copy(block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        require(prepared.render(
                    std::span<double>{realtimeLeft}.subspan(offset, frames),
                    std::span<double>{realtimeRight}.subspan(offset, frames)),
            "C112 prepared irregular partition should render stereo");
        offset += frames;
        ++partitionIndex;
    }
    require(offlineLeft == realtimeLeft
            && offlineRight == realtimeRight
            && std::ranges::equal(realtimeLeft, channels[0].samples)
            && std::ranges::equal(realtimeRight, channels[1].samples),
        "C112 CLI, Save/reopen, offline, and prepared renders should be bit-identical across irregular partitions");

    prepared.resetTransport();
    std::array<double, renderedFrames> resetLeft{};
    std::array<double, renderedFrames> resetRight{};
    std::array<double, renderedFrames> replacedLeft{};
    std::array<double, renderedFrames> replacedRight{};
    offset = 0;
    partitionIndex = 0;
    while (offset < renderedFrames)
    {
        const auto frames = std::min(
            partitionPattern[partitionIndex % partitionPattern.size()],
            renderedFrames - offset);
        require(prepared.render(
                    std::span<double>{resetLeft}.subspan(offset, frames),
                    std::span<double>{resetRight}.subspan(offset, frames))
                && replacement.render(
                    std::span<double>{replacedLeft}.subspan(offset, frames),
                    std::span<double>{replacedRight}.subspan(offset, frames)),
            "C112 reset/replacement irregular partition should render");
        offset += frames;
        ++partitionIndex;
    }
    require(resetLeft == replacedLeft && resetRight == replacedRight
            && resetLeft == realtimeLeft && resetRight == realtimeRight,
        "C112 reset/replacement should replay exact range, cascade, delay, control, cursor, and stereo state");
}

void testC131ChannelCrosserCheckedFiniteMatrixExample()
{
    const auto path = examplePath(
        "c131_channel_crosser_checked_finite_matrix.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C131 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.channelCrosser\"",
        "C131 example should expose Channel Crosser");
    requireContains(source, "\"expression\": \"!C131LeftInLeft\"",
        "C131 example should expose scheduled connected coefficient authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C131 project should parse with four exact schedule segments");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C131 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C131 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C131 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.channelCrosser",
        "C131 status should expose checked Channel Crosser execution");
    requireContains(status.output, "resources: none",
        "C131 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C131 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2
            && std::ranges::all_of(
                channels[0].samples, [](double value) {
                    return std::isfinite(value);
                })
            && std::ranges::all_of(
                channels[1].samples, [](double value) {
                    return std::isfinite(value);
                })
            && std::ranges::any_of(
                channels[0].samples, [](double value) {
                    return std::abs(value) > 0.01;
                })
            && std::ranges::any_of(
                channels[1].samples, [](double value) {
                    return std::abs(value) > 0.01;
                })
            && channels[0].samples != channels[1].samples,
        "C131 CLI render should produce two finite audible independent channels");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C131LeftInLeft", 0.75,
        talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {256, "C131LeftInLeft", -0.5,
            talas::expr::ValueDomain::Scalar},
        {512, "C131LeftInLeft", 0.25,
            talas::expr::ValueDomain::Scalar},
        {768, "C131LeftInLeft", -0.75,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C131 example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, frameCount> offlineLeft{};
    std::array<double, frameCount> offlineRight{};
    std::array<double, frameCount> preparedLeft{};
    std::array<double, frameCount> preparedRight{};
    constexpr std::array<std::size_t, 7> pattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frameCount)
    {
        const auto frames = std::min(
            pattern[partition++ % pattern.size()], frameCount - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.right.size() == frames
                && prepared.render(
                    std::span<double>{preparedLeft}.subspan(offset, frames),
                    std::span<double>{preparedRight}.subspan(offset, frames)),
            "C131 irregular offline/prepared partition should render");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += frames;
    }
    require(offlineLeft == preparedLeft
            && offlineRight == preparedRight
            && std::ranges::equal(preparedLeft, channels[0].samples)
            && std::ranges::equal(preparedRight, channels[1].samples),
        "C131 CLI, offline, prepared, and irregular partition bits should match exactly");

    auto disconnectedAudio = parsed.document;
    std::erase_if(disconnectedAudio.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "crosser"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                disconnectedAudio, talas::core::makeBuiltinNodeRegistry(),
                settings).ok(),
        "C131 required left input disconnect should fail closed");

    auto missingRight = parsed.document;
    std::erase_if(missingRight.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "crosser"
                && edge.toPort == "rightIn";
        });
    const auto missingRightCompiled = talas::runtime::compileProject(
        missingRight, talas::core::makeBuiltinNodeRegistry(), settings);
    require(missingRightCompiled.ok(),
        "C131 optional right input disconnect should select exact positive zero");
    const auto missingRightOutput = talas::runtime::renderOffline(
        *missingRightCompiled.graph, frameCount);
    require(missingRightOutput.ok()
            && (!std::ranges::equal(missingRightOutput.mono, offlineLeft)
                || !std::ranges::equal(
                    missingRightOutput.right, offlineRight)),
        "C131 missing-right substitution should observably change the matrix render");

    for (const auto coefficient : {
             std::string_view{"leftInLeft"},
             std::string_view{"rightInLeft"},
             std::string_view{"leftInRight"},
             std::string_view{"rightInRight"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "crosser"
                    && edge.toPort == coefficient;
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C131 disconnected coefficient should select finite fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, frameCount);
        require(output.ok()
                && (!std::ranges::equal(output.mono, offlineLeft)
                    || !std::ranges::equal(output.right, offlineRight)),
            "C131 each coefficient disconnect should mutate at least one channel");
    }
}

void testC132ChannelJoinCheckedFiniteDualLaneExample()
{
    const auto path = examplePath(
        "c132_channel_join_checked_finite_dual_lane.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C132 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.channelJoin\"",
        "C132 example should expose Channel Join");
    requireContains(source, "\"expression\": \"!C132LeftAmplitude\"",
        "C132 example should expose scheduled left-lane authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 2,
        "C132 project should parse with two exact schedule segments");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C132 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C132 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C132 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.channelJoin",
        "C132 status should expose checked Channel Join execution");
    requireContains(status.output, "resources: none",
        "C132 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C132 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2
            && std::ranges::all_of(channels[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(channels[1].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(channels[0].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(channels[1].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && channels[0].samples != channels[1].samples,
        "C132 CLI render should produce two finite audible independent lanes");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {{
        "C132LeftAmplitude", 0.35,
        talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {512, "C132LeftAmplitude", -0.2,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C132 example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, frameCount> offlineLeft{};
    std::array<double, frameCount> offlineRight{};
    std::array<double, frameCount> preparedLeft{};
    std::array<double, frameCount> preparedRight{};
    constexpr std::array<std::size_t, 7> pattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frameCount)
    {
        const auto frames = std::min(
            pattern[partition++ % pattern.size()], frameCount - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.right.size() == frames
                && prepared.render(
                    std::span<double>{preparedLeft}.subspan(offset, frames),
                    std::span<double>{preparedRight}.subspan(offset, frames)),
            "C132 irregular offline/prepared partition should render");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += frames;
    }
    require(offlineLeft == preparedLeft
            && offlineRight == preparedRight
            && std::ranges::equal(preparedLeft, channels[0].samples)
            && std::ranges::equal(preparedRight, channels[1].samples),
        "C132 CLI, offline, prepared, and irregular partition bits should match exactly");

    for (const auto input : {
             std::string_view{"left"}, std::string_view{"right"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "join" && edge.toPort == input;
            });
        require(!talas::runtime::compileProject(
                    disconnected, talas::core::makeBuiltinNodeRegistry(),
                    settings).ok(),
            "C132 each required input disconnect should fail closed");
    }

    for (const auto output : {
             std::string_view{"leftOut"}, std::string_view{"rightOut"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.fromNode == "join" && edge.fromPort == output;
            });
        require(!talas::runtime::compileProject(
                    disconnected, talas::core::makeBuiltinNodeRegistry(),
                    settings).ok(),
            "C132 disconnecting either required monitored Join lane should fail closed");
    }
}

void testC133ChannellerCheckedFiniteLiveSelectorsExample()
{
    const auto path = examplePath(
        "c133_channeller_checked_finite_live_selectors.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C133 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.channeller\"",
        "C133 example should expose Channeller");
    requireContains(source, "\"expression\": \"!C133LeftSelector\"",
        "C133 example should expose the scheduled left selector");
    requireContains(source, "\"expression\": \"!C133RightSelector\"",
        "C133 example should expose the scheduled right selector");

    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C133 project should parse with four exact selector masks");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C133 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C133 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C133 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.channeller",
        "C133 status should expose checked Channeller execution");
    requireContains(status.output, "resources: none",
        "C133 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C133 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2,
        "C133 CLI render should expose two output lanes");
    // Only left is checked, then only right, then both, then neither.
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto left = channels[0].samples[frame];
        const auto right = channels[1].samples[frame];
        require(
            (frame >= 768
                ? std::bit_cast<std::uint64_t>(left) == 0ULL
                    && std::bit_cast<std::uint64_t>(right) == 0ULL
                : std::isfinite(left) && std::isfinite(right))
                && (frame >= 512 || left == right),
            "C133 CLI render should preserve all four exact selector masks");
    }
    const auto peak = [&](std::size_t channel, std::size_t firstFrame) {
        auto result = 0.0;
        for (auto frame = firstFrame; frame < firstFrame + 256; ++frame)
            result = std::max(
                result, std::abs(channels[channel].samples[frame]));
        return result;
    };
    require(
        peak(0, 0) > 0.39 && peak(1, 0) > 0.39
            && peak(0, 256) > 0.29 && peak(0, 256) < 0.31
            && peak(0, 512) > 0.39
            && peak(1, 512) > 0.29 && peak(1, 512) < 0.31,
        "C133 CLI render should play the 0.4 left carrier, then the 0.3 right carrier, on both outputs before passing both through");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C133LeftSelector", 1.0, talas::expr::ValueDomain::Scalar},
        {"C133RightSelector", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C133LeftSelector", 0.0,
            talas::expr::ValueDomain::Scalar},
        {256, "C133RightSelector", 1.0,
            talas::expr::ValueDomain::Scalar},
        {512, "C133LeftSelector", 1.0,
            talas::expr::ValueDomain::Scalar},
        {512, "C133RightSelector", 1.0,
            talas::expr::ValueDomain::Scalar},
        {768, "C133LeftSelector", 0.0,
            talas::expr::ValueDomain::Scalar},
        {768, "C133RightSelector", 0.0,
            talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C133 example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, frameCount> offlineLeft{};
    std::array<double, frameCount> offlineRight{};
    std::array<double, frameCount> preparedLeft{};
    std::array<double, frameCount> preparedRight{};
    constexpr std::array<std::size_t, 7> pattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frameCount)
    {
        const auto frames = std::min(
            pattern[partition++ % pattern.size()], frameCount - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.right.size() == frames
                && prepared.render(
                    std::span<double>{preparedLeft}.subspan(offset, frames),
                    std::span<double>{preparedRight}.subspan(offset, frames)),
            "C133 irregular offline/prepared partition should render");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += frames;
    }
    require(offlineLeft == preparedLeft
            && offlineRight == preparedRight
            && std::ranges::equal(preparedLeft, channels[0].samples)
            && std::ranges::equal(preparedRight, channels[1].samples),
        "C133 CLI, offline, prepared, and irregular partition bits should match exactly");

    auto missingAudio = parsed.document;
    std::erase_if(missingAudio.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "channeller"
                && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                missingAudio, talas::core::makeBuiltinNodeRegistry(),
                settings).ok(),
        "C133 required audio disconnect should fail closed");

    for (const auto selector : {
             std::string_view{"leftChannel"},
             std::string_view{"rightChannel"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "channeller"
                    && edge.toPort == selector;
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C133 optional selector disconnect should select its Boolean fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, frameCount);
        require(output.ok()
                && (!std::ranges::equal(output.mono, offlineLeft)
                    || !std::ranges::equal(output.right, offlineRight)),
            "C133 each selector disconnect should observably change routing");
    }
}

void testC134PanCheckedFiniteLiveControlsExample()
{
    const auto path = examplePath(
        "c134_pan_checked_finite_live_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C134 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.pan\"",
        "C134 example should expose Pan");
    requireContains(source, "\"expression\": \"!C134Pan\"",
        "C134 example should expose the scheduled position control");
    requireContains(source, "\"expression\": \"!C134Scale\"",
        "C134 example should expose the scheduled scale control");

    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C134 project should parse with four exact Pan control states");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C134 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C134 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C134 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.pan",
        "C134 status should expose checked Pan execution");
    requireContains(status.output, "resources: none",
        "C134 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C134 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2,
        "C134 CLI render should expose two output lanes");
    require(std::ranges::all_of(
                std::span{channels[1].samples}.first(256),
                [](double value) {
                    return value == 0.0;
                })
            && std::ranges::any_of(
                std::span{channels[0].samples}.first(256),
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(
                std::span{channels[1].samples}.subspan(512, 256),
                [](double value) { return std::abs(value) > 0.01; }),
        "C134 CLI render should expose the exact left and right endpoint states");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C134Pan", 0.0, talas::expr::ValueDomain::Scalar},
        {"C134Scale", 0.25, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C134Pan", 0.5, talas::expr::ValueDomain::Scalar},
        {256, "C134Scale", 0.5, talas::expr::ValueDomain::Scalar},
        {512, "C134Pan", 1.0, talas::expr::ValueDomain::Scalar},
        {512, "C134Scale", 0.75, talas::expr::ValueDomain::Scalar},
        {768, "C134Pan", 0.25, talas::expr::ValueDomain::Scalar},
        {768, "C134Scale", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C134 example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, frameCount> offlineLeft{};
    std::array<double, frameCount> offlineRight{};
    std::array<double, frameCount> preparedLeft{};
    std::array<double, frameCount> preparedRight{};
    constexpr std::array<std::size_t, 7> pattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frameCount)
    {
        const auto frames = std::min(
            pattern[partition++ % pattern.size()], frameCount - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.right.size() == frames
                && prepared.render(
                    std::span<double>{preparedLeft}.subspan(offset, frames),
                    std::span<double>{preparedRight}.subspan(offset, frames)),
            "C134 irregular offline/prepared partition should render");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += frames;
    }
    require(offlineLeft == preparedLeft
            && offlineRight == preparedRight
            && std::ranges::equal(preparedLeft, channels[0].samples)
            && std::ranges::equal(preparedRight, channels[1].samples),
        "C134 CLI, offline, prepared, and irregular partition bits should match exactly");

    auto missingAudio = parsed.document;
    std::erase_if(missingAudio.graph.connections,
        [](const auto& edge) {
            return edge.toNode == "pan" && edge.toPort == "audioIn";
        });
    require(!talas::runtime::compileProject(
                missingAudio, talas::core::makeBuiltinNodeRegistry(),
                settings).ok(),
        "C134 required audio disconnect should fail closed");

    for (const auto control : {
             std::string_view{"pan"}, std::string_view{"scale"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "pan"
                    && edge.toPort == control;
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C134 optional control disconnect should select its scalar fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, frameCount);
        require(output.ok()
                && (!std::ranges::equal(output.mono, offlineLeft)
                    || !std::ranges::equal(output.right, offlineRight)),
            "C134 each control disconnect should observably change the result");
    }
}

void testC135CrossfadeCheckedFiniteLiveControlsExample()
{
    const auto path = examplePath(
        "c135_crossfade_checked_finite_live_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C135 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.crossfade\"",
        "C135 example should expose Crossfade");
    requireContains(source, "\"expression\": \"!C135Pan\"",
        "C135 example should expose the scheduled pan control");
    requireContains(source, "\"expression\": \"!C135Scale\"",
        "C135 example should expose the scheduled scale control");
    requireContains(source, "\"expression\": \"!C135Fade\"",
        "C135 example should expose the scheduled fade control");

    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 12,
        "C135 project should parse with four exact Crossfade control states");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C135 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C135 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C135 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.crossfade",
        "C135 status should expose checked Crossfade execution");
    requireContains(status.output, "resources: none",
        "C135 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C135 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2,
        "C135 CLI render should expose two output lanes");
    require(std::ranges::all_of(
                std::span{channels[1].samples}.first(256),
                [](double value) { return value == 0.0; })
            && std::ranges::any_of(
                std::span{channels[0].samples}.first(256),
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(
                std::span{channels[1].samples}.subspan(512, 256),
                [](double value) { return std::abs(value) > 0.01; }),
        "C135 CLI render should expose exact first-input-left and second-input-right endpoint states");

    auto settings = talas::runtime::RenderSettings{8192.0, 127};
    settings.eventValues = {
        {"C135Pan", 0.0, talas::expr::ValueDomain::Scalar},
        {"C135Scale", 0.25, talas::expr::ValueDomain::Scalar},
        {"C135Fade", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {256, "C135Pan", 0.5, talas::expr::ValueDomain::Scalar},
        {256, "C135Scale", 0.5, talas::expr::ValueDomain::Scalar},
        {256, "C135Fade", 1.0 / 3.0, talas::expr::ValueDomain::Scalar},
        {512, "C135Pan", 1.0, talas::expr::ValueDomain::Scalar},
        {512, "C135Scale", 0.75, talas::expr::ValueDomain::Scalar},
        {512, "C135Fade", 1.0, talas::expr::ValueDomain::Scalar},
        {768, "C135Pan", 0.25, talas::expr::ValueDomain::Scalar},
        {768, "C135Scale", 1.0, talas::expr::ValueDomain::Scalar},
        {768, "C135Fade", 2.0 / 3.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C135 example should compile");
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, frameCount> offlineLeft{};
    std::array<double, frameCount> offlineRight{};
    std::array<double, frameCount> preparedLeft{};
    std::array<double, frameCount> preparedRight{};
    constexpr std::array<std::size_t, 7> pattern{
        127, 1, 64, 17, 93, 2, 111};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frameCount)
    {
        const auto frames = std::min(
            pattern[partition++ % pattern.size()], frameCount - offset);
        const auto block = offline.render(frames);
        require(block.ok() && block.right.size() == frames
                && prepared.render(
                    std::span<double>{preparedLeft}.subspan(offset, frames),
                    std::span<double>{preparedRight}.subspan(offset, frames)),
            "C135 irregular offline/prepared partition should render");
        std::ranges::copy(
            block.mono,
            offlineLeft.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(
            block.right,
            offlineRight.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += frames;
    }
    require(offlineLeft == preparedLeft
            && offlineRight == preparedRight
            && std::ranges::equal(preparedLeft, channels[0].samples)
            && std::ranges::equal(preparedRight, channels[1].samples),
        "C135 CLI, offline, prepared, and irregular partition bits should match exactly");

    for (const auto requiredInput : {
             std::string_view{"snd1"}, std::string_view{"snd2"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "crossfade"
                    && edge.toPort == requiredInput;
            });
        require(!talas::runtime::compileProject(
                    disconnected, talas::core::makeBuiltinNodeRegistry(),
                    settings).ok(),
            "C135 each required audio disconnect should fail closed");
    }

    for (const auto control : {
             std::string_view{"pan"}, std::string_view{"scale"},
             std::string_view{"fade"}})
    {
        auto disconnected = parsed.document;
        std::erase_if(disconnected.graph.connections,
            [&](const auto& edge) {
                return edge.toNode == "crossfade"
                    && edge.toPort == control;
            });
        const auto mutation = talas::runtime::compileProject(
            disconnected, talas::core::makeBuiltinNodeRegistry(), settings);
        require(mutation.ok(),
            "C135 optional control disconnect should select its scalar fallback");
        const auto output = talas::runtime::renderOffline(
            *mutation.graph, frameCount);
        require(output.ok()
                && (!std::ranges::equal(output.mono, offlineLeft)
                    || !std::ranges::equal(output.right, offlineRight)),
            "C135 each control disconnect should observably change the result");
    }
}

void testC136StereoMix2CheckedLiveControlsExample()
{
    const auto path = examplePath(
        "c136_stereo_mix2_checked_live_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C136 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.stereoMix2\"",
        "C136 example should expose Stereo Mix 2");
    for (const auto eventName : {
             "C136Left", "C136Right", "C136Pan1", "C136Scale1",
             "C136Pan2", "C136Scale2"})
    {
        requireContains(source, "!" + std::string{eventName},
            "C136 example should expose every scheduled live control");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 24,
        "C136 project should parse with four exact six-control states");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C136 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C136 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C136 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.stereoMix2",
        "C136 status should expose checked Stereo Mix 2 execution");
    requireContains(status.output, "resources: none",
        "C136 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C136 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2
            && std::ranges::all_of(
                channels[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(
                channels[1].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                channels[0].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(
                channels[1].samples,
                [](double value) { return std::abs(value) > 0.01; }),
        "C136 CLI render should expose finite audible stereo output");
}

void testC137StereoMix4CheckedLiveControlsExample()
{
    const auto path = examplePath(
        "c137_stereo_mix4_checked_live_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C137 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.stereoMix4\"",
        "C137 example should expose Stereo Mix 4");
    for (const auto eventName : {
             "C137Left", "C137Right", "C137Pan1", "C137Scale1",
             "C137Pan2", "C137Scale2", "C137Pan3", "C137Scale3",
             "C137Pan4", "C137Scale4"})
    {
        requireContains(source, "!" + std::string{eventName},
            "C137 example should expose every scheduled live control");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 40,
        "C137 project should parse with four exact ten-control states");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C137 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C137 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C137 status should expose the stereo terminal");
    requireContains(status.output, "type=talas.stereoMix4",
        "C137 status should expose checked Stereo Mix 4 execution");
    requireContains(status.output, "resources: none",
        "C137 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C137 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2
            && std::ranges::all_of(
                channels[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(
                channels[1].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                channels[0].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(
                channels[1].samples,
                [](double value) { return std::abs(value) > 0.01; }),
        "C137 CLI render should expose finite audible stereo output");
}

void testC138AudioOutputExactStereoTerminalExample()
{
    const auto path = examplePath(
        "c138_audio_output_exact_stereo_terminal.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C138 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.audioOutput\"",
        "C138 example should expose Audio Output");
    requireContains(source, "!C138LeftLevel",
        "C138 example should schedule the left producer independently");
    requireContains(source, "!C138RightLevel",
        "C138 example should schedule the right producer independently");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 6,
        "C138 project should parse with three exact stereo states");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C138 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C138 CLI status should succeed");
    requireContains(status.output, "outputs: stereo (2)",
        "C138 status should expose the ordered stereo terminal");
    requireContains(status.output, "type=talas.audioOutput",
        "C138 status should expose Audio Output execution");
    requireContains(status.output, "resources: none",
        "C138 status should expose resource-free execution");

    constexpr std::size_t frameCount = 1024;
    const auto rendered = runCli({
        "render", "--project", path.string(), "--frames",
        std::to_string(frameCount), "--sample-rate", "8192",
        "--max-block-frames", "127", "--format", "json",
    });
    require(rendered.exitCode == 0 && rendered.error.empty(),
        "C138 CLI render should succeed");
    const auto channels = parseRenderedChannels(
        rendered.output, frameCount, 8192.0);
    require(channels.size() == 2
            && std::ranges::all_of(
                channels[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(
                channels[1].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                channels[0].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && std::ranges::any_of(
                channels[1].samples,
                [](double value) { return std::abs(value) > 0.01; })
            && channels[0].samples != channels[1].samples,
        "C138 CLI render should expose finite audible ordered stereo output");
}

void testC139AveragingLowPassFilterDynamicWindowExample()
{
    const auto path = examplePath(
        "c139_averaging_low_pass_filter_dynamic_window.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C139 example should use current project format");
    requireContains(source,
        "\"typeId\": \"talas.averagingLowPassFilter\"",
        "C139 example should expose Averaging Low Pass Filter");
    requireContains(source, "\"resource\": \"Private\"",
        "C139 example should declare private node-local scratch memory");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C139 dynamic-window project should parse without external assets");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C139 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C139 CLI status should succeed");
    requireContains(status.output,
        "type=talas.averagingLowPassFilter",
        "C139 status should expose moving-window execution");

    constexpr std::size_t frameCount = 1024;
    const auto render = [&](std::string_view maxBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames",
            std::to_string(frameCount), "--sample-rate", "8192",
            "--max-block-frames", std::string{maxBlockFrames},
            "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C139 CLI render should succeed");
        return parseRenderedChannels(
            rendered.output, frameCount, 8192.0);
    };
    const auto reference = render("127");
    const auto alternate = render("64");
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && std::ranges::all_of(
                reference[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.01; }),
        "C139 dynamic-window output should be finite, audible, and partition stable");
}

void testC140DelayWithFeedbackDynamicControlsExample()
{
    const auto path = examplePath(
        "c140_delay_with_feedback_dynamic_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C140 example should use current project format");
    requireContains(source,
        "\"typeId\": \"talas.delayWithFeedback\"",
        "C140 example should expose Delay With Feedback");
    requireContains(source, "\"expression\": \"-1 ms\"",
        "C140 example should expose connected ownership over an inert delay fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C140 dynamic-control project should parse without external assets");
    const auto canonical = talas::project::serializeProjectDocument(
        parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "C140 save/reopen should preserve canonical project authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8192", "--max-block-frames", "127",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C140 CLI status should succeed");
    requireContains(status.output, "type=talas.delayWithFeedback",
        "C140 status should expose bounded feedback-delay execution");

    constexpr std::size_t frameCount = 1024;
    const auto render = [&](std::string_view maxBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames",
            std::to_string(frameCount), "--sample-rate", "8192",
            "--max-block-frames", std::string{maxBlockFrames},
            "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C140 CLI render should succeed");
        return parseRenderedChannels(
            rendered.output, frameCount, 8192.0);
    };
    const auto reference = render("127");
    const auto alternate = render("64");
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && std::ranges::all_of(
                reference[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.01; }),
        "C140 dynamic-control output should be finite, audible, and partition stable");
}

void testC141SetDurationBoundedWindowExample()
{
    const auto path = examplePath(
        "c141_set_duration_bounded_window.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C141 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.setDuration\"",
        "C141 example should expose Set Duration");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C141 bounded-window project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C141 CLI status should succeed");
    requireContains(status.output, "type=talas.setDuration",
        "C141 status should expose bounded Set Duration execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C141 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("2");
    const std::array expected{
        0.0, 0.0, 0.0, std::sin(std::numbers::pi / 4.0),
        1.0, std::sin(3.0 * std::numbers::pi / 4.0), 0.0, 0.0,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C141 Set Duration output should be mono and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(reference[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C141 bounded-window frame " + std::to_string(frame));
    }
}

void testC142WarpedTimeIndexScheduledMorphExample()
{
    const auto path = examplePath(
        "c142_warped_time_index_scheduled_morph.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C142 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.warpedTimeIndex\"",
        "C142 example should expose Warped Time Index");
    requireContains(source, "!C147Morph",
        "C142 example should expose direct scheduled morph authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C142 scheduled-morph project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C142 CLI status should succeed");
    requireContains(status.output, "type=talas.warpedTimeIndex",
        "C142 status should expose promoted Warped Time Index execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C142 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        -0.0,
        -0.61871843353822897,
        -0.75,
        0.17677669529663689,
        1.2246467991473532e-16,
        -0.70710678118654746,
        -1.0,
        -0.70710678118654768,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::all_of(
                reference[0].samples,
                [](double value) { return std::isfinite(value); })
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C142 scheduled Warped Time Index output should be finite, audible, and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(reference[0].samples[frame] - expected[frame])
                <= 1.0e-12,
            "C142 scheduled-map frame " + std::to_string(frame));
    }
}

void testC143AmplitudeFollowerScheduledControlsExample()
{
    const auto path = examplePath(
        "c143_amplitude_follower_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C143 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.amplitudeFollower\"",
        "C143 example should expose Amplitude Follower");
    requireContains(source, "!C148TimeDelta",
        "C143 example should expose direct scheduled time-constant authority");
    requireContains(source, "!C148ScaleReduction",
        "C143 example should expose direct scheduled scale authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C143 scheduled-control project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C143 CLI status should succeed");
    requireContains(status.output, "type=talas.amplitudeFollower",
        "C143 status should expose promoted Amplitude Follower execution");
    requireContains(status.output, "resources: none",
        "C143 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C143 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.081179415021929532,
        0.27213203435596423,
        0.34963750251417591,
        0.3009661559152933,
        0.23784840289772269,
        0.16286778850603997,
        0.077022793179043064,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C143 scheduled Amplitude Follower output should be finite, audible, and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(
                reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C143 scheduled-envelope frame "
                + std::to_string(frame));
    }
}

void testC144PeakDetectorScheduledControlsExample()
{
    const auto path = examplePath(
        "c144_peak_detector_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C144 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.peakDetector\"",
        "C144 example should expose Peak Detector");
    requireContains(source, "!C149AttackDelta",
        "C144 example should expose direct scheduled attack authority");
    requireContains(source, "!C149ReleaseDelta",
        "C144 example should expose direct scheduled release authority");
    requireContains(source, "!C149ScaleReduction",
        "C144 example should expose direct scheduled scale authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C144 scheduled-control project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C144 CLI status should succeed");
    requireContains(status.output, "type=talas.peakDetector",
        "C144 status should expose promoted Peak Detector execution");
    requireContains(status.output, "resources: none",
        "C144 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C144 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.081179415021929532,
        0.19167333731315681,
        0.2660351810896151,
        0.22900170952778176,
        0.23512834290048334,
        0.16121215907642478,
        0.076310214549078562,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C144 scheduled Peak Detector output should be finite, audible, and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(
                reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C144 scheduled asymmetric-envelope frame "
                + std::to_string(frame));
    }
}

void testC145RmsSquaredScheduledControlsExample()
{
    const auto path = examplePath(
        "c145_rms_squared_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C145 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.rmsSquared\"",
        "C145 example should expose RMS Squared");
    requireContains(source, "!C150TimeDelta",
        "C145 example should expose direct scheduled time authority");
    requireContains(source, "!C150ScaleReduction",
        "C145 example should expose direct scheduled scale authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C145 scheduled-control project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C145 CLI status should succeed");
    requireContains(status.output, "type=talas.rmsSquared",
        "C145 status should expose promoted RMS Squared execution");
    requireContains(status.output, "resources: none",
        "C145 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C145 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.057402514854763456,
        0.2545584412271571,
        0.31179066153027513,
        0.26838779071546698,
        0.20014912221185679,
        0.12634946015044779,
        0.055604645350153095,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C145 scheduled RMS Squared output should be finite, audible, and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(
                reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C145 scheduled squared-envelope frame "
                + std::to_string(frame));
    }
}

void testC146RunningMaxScheduledResetExample()
{
    const auto path = examplePath(
        "c146_running_max_scheduled_reset.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C146 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.runningMax\"",
        "C146 example should expose Running Max");
    requireContains(source, "!C151Reset",
        "C146 example should expose direct scheduled reset authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C146 scheduled-reset project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C146 CLI status should succeed");
    requireContains(status.output, "type=talas.runningMax",
        "C146 status should expose promoted Running Max execution");
    requireContains(status.output, "resources: none",
        "C146 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C146 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.13529902503654923,
        0.35355339059327373,
        0.46193976625564337,
        6.123233995736766e-17,
        5.6571305614385013e-17,
        4.329780281177467e-17,
        2.3432602026631499e-17,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C146 scheduled Running Max output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C146 scheduled reset frame " + std::to_string(frame));
    }
}

void testC147RunningMinScheduledResetBaselineExample()
{
    const auto path = examplePath(
        "c147_running_min_scheduled_reset_baseline.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C147 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.runningMin\"",
        "C147 example should expose Running Min");
    requireContains(source, "!C152Reset",
        "C147 example should expose direct scheduled reset authority");
    requireContains(source, "!C152MaximumAmplitude",
        "C147 example should expose direct scheduled baseline authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C147 scheduled-control project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C147 CLI status should succeed");
    requireContains(status.output, "type=talas.runningMin",
        "C147 status should expose promoted Running Min execution");
    requireContains(status.output, "resources: none",
        "C147 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C147 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.0,
        0.0,
        0.0,
        -0.25,
        -0.32664074121909409,
        -0.35355339059327379,
        -0.19134171618254495,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C147 scheduled Running Min output should be audible and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(
                reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C147 scheduled reset/baseline frame "
                + std::to_string(frame));
    }
}

void testC148TriggeredSampleAndHoldScheduledEdgesExample()
{
    const auto path = examplePath(
        "c148_triggered_sample_and_hold_scheduled_edges.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C148 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.triggeredSampleAndHold\"",
        "C148 example should expose Triggered Sample And Hold");
    requireContains(source, "!C153Trigger",
        "C148 example should expose direct scheduled trigger authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C148 scheduled-edge project should parse without external assets");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C148 CLI status should succeed");
    requireContains(status.output, "type=talas.triggeredSampleAndHold",
        "C148 status should expose promoted Triggered Sample And Hold execution");
    requireContains(status.output, "resources: none",
        "C148 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C148 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    const std::array expected{
        0.0,
        0.065773714937749805,
        0.12153397801643784,
        0.1587917946503774,
        0.3125,
        0.28871235390977712,
        0.22097086912079611,
        0.11958857261409059,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size()
            && std::ranges::any_of(
                reference[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C148 scheduled Triggered Sample And Hold output should be audible and partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C148 scheduled edge frame " + std::to_string(frame));
    }
}

void testC149TriggeredTableReadPairedSchedulesExample()
{
    const auto path = examplePath(
        "c149_triggered_table_read_paired_schedules.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C149 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.triggeredTableRead\"",
        "C149 example should expose Triggered Table Read");
    requireContains(source, "!C154Trigger",
        "C149 example should expose direct scheduled trigger authority");
    requireContains(source, "!C154Gate",
        "C149 example should expose direct scheduled gate authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id == "Tables/C149"
            && parsed.document.assets.front().contentHash
                == "e9ac4a6422eeaeb687b55bb3eaa5d306f11fb122cd357e4df6b1ad679e50c2d1",
        "C149 paired-schedule project should own exact immutable table authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C149 CLI status should succeed");
    requireContains(status.output, "type=talas.triggeredTableRead",
        "C149 status should expose promoted Triggered Table Read execution");
    requireContains(
        status.output,
        "Tables/C149 kind=table generation=1 hash=e9ac4a6422eeaeb687b55bb3eaa5d306f11fb122cd357e4df6b1ad679e50c2d1 health=verified",
        "C149 status should expose exact verified immutable table authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C149 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    // Independent oscillator -> stereo allpass pair -> carrier sum oracle.
    constexpr std::array expected{
        0.0,
        0.047835429045636223,
        0.088388347648318433,
        0.11548494156391084,
        0.25,
        0.11548494156391084,
        0.088388347648318447,
        0.095670858091272473,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C149 paired Triggered Table Read output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C149 paired trigger/gate schedule frame "
                + std::to_string(frame));
    }

    talas::runtime::RenderSettings settings{8.0, 3};
    settings.eventValues = {
        {"C154Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {"C154Gate", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "C154Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "C154Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C154Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {3, "C154Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C154Gate", 0.0, talas::expr::ValueDomain::Scalar},
        {6, "C154Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {7, "C154Trigger", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.tableResources.push_back(
        {"Tables/C149", {0.25, 0.5}});
    const auto compiled = talas::runtime::compileProject(
        parsed.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "C149 product example should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> realtime{};
    require(offline.ok() && prepared.ready(),
        "C149 product example should prepare offline and realtime: "
            + std::string{prepared.error()});
    std::size_t offset = 0;
    for (const auto frames : std::array<std::size_t, 5>{1, 2, 1, 3, 1})
    {
        require(prepared.render(
            std::span<double>{realtime}.subspan(offset, frames)),
            "C149 irregular prepared partition should render");
        offset += frames;
    }
    require(std::ranges::equal(realtime, offline.mono),
        "C149 prepared partitions should be bit-identical offline");
    prepared.resetTransport();
    std::array<double, 3> reset{};
    require(prepared.render(reset)
            && std::ranges::equal(
                reset, std::span<const double>{offline.mono}.first(3)),
        "C149 prepared reset should restore cursor, held sample, gate polarity, and schedules");
}

void testC150FunctionGeneratorScheduledControlsExample()
{
    const auto path = examplePath(
        "c150_function_generator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C150 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.functionGenerator\"",
        "C150 example should expose Function Generator");
    requireContains(source, "!C155Trigger",
        "C150 example should expose direct scheduled trigger authority");
    requireContains(source, "!C155Duration",
        "C150 example should expose direct scheduled duration authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id == "Envelopes/C150"
            && parsed.document.assets.front().contentHash
                == "e62b808086c7f00a53381d01401c33f6b2702d1ac21033fb3a6605c9d647e91b",
        "C150 scheduled-control project should own exact immutable envelope authority");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C150 CLI status should succeed");
    requireContains(status.output, "type=talas.functionGenerator",
        "C150 status should expose promoted Function Generator execution");
    requireContains(
        status.output,
        "Envelopes/C150 kind=table generation=1 hash=e62b808086c7f00a53381d01401c33f6b2702d1ac21033fb3a6605c9d647e91b health=verified",
        "C150 status should expose exact verified immutable envelope authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C150 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.0,
        0.11785113019775791,
        0.0,
        0.33333333333333331,
        0.0,
        0.17677669529663689,
        0.0,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C150 scheduled Function Generator output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C150 scheduled trigger/duration frame "
                + std::to_string(frame));
    }
}

void testC151MultislopeScheduledControlsExample()
{
    const auto path = examplePath(
        "c151_multislope_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C151 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.multislopeFunctionGenerator\"",
        "C151 example should expose Multislope Function Generator");
    requireContains(source, "!C156Level",
        "C151 example should expose direct scheduled level authority");
    requireContains(source, "!C156Gate",
        "C151 example should expose direct scheduled gate authority");
    requireContains(source, "!C156Rate",
        "C151 example should expose direct scheduled rate authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C151 scheduled-control project should remain resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C151 CLI status should succeed");
    requireContains(status.output, "type=talas.multislopeFunctionGenerator",
        "C151 status should expose promoted Multislope execution");
    requireContains(status.output, "resources: none",
        "C151 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C151 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.095670858091272445,
        0.15467960838455724,
        0.086613706172933125,
        0.0625,
        0.3464548246917325,
        0.19887378220871652,
        0.13154742987549964,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C151 scheduled Multislope output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C151 scheduled level/gate/rate frame "
                + std::to_string(frame));
    }
}

void testC152MultisegmentScheduledLoopControlsExample()
{
    const auto path = examplePath(
        "c152_multisegment_scheduled_loop_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C152 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.multisegmentEnvelope\"",
        "C152 example should expose Multisegment Envelope");
    requireContains(source, "!C157Level",
        "C152 example should expose direct scheduled level authority");
    requireContains(source, "!C157Gate",
        "C152 example should expose direct scheduled gate authority");
    requireContains(source, "\"id\": \"startLoop\", \"expression\": \"2\"",
        "C152 example should expose a static checked loop start");
    requireContains(source, "\"id\": \"endLoop\", \"expression\": \"3\"",
        "C152 example should expose a static checked loop end");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C152 scheduled-loop project should remain resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C152 CLI status should succeed");
    requireContains(status.output, "type=talas.multisegmentEnvelope",
        "C152 status should expose promoted Multisegment execution");
    requireContains(status.output, "resources: none",
        "C152 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C152 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.023917714522818111,
        0.11048543456039804,
        0.11548494156391084,
        0.09375,
        0.086613706172933125,
        0.16572815184059708,
        0.19134171618254495,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C152 scheduled Multisegment output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C152 scheduled level/gate/loop frame "
                + std::to_string(frame));
    }
}

void testC153GraphicalEnvelopeScheduledRateControlsExample()
{
    const auto path = examplePath(
        "c153_graphical_envelope_scheduled_rate_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C153 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.graphicalEnvelope\"",
        "C153 example should expose Graphical Envelope");
    requireContains(source, "!C158Level",
        "C153 example should expose direct scheduled level authority");
    requireContains(source, "!C158Gate",
        "C153 example should expose direct scheduled gate authority");
    requireContains(source, "1 + !C158Rate",
        "C153 example should expose bounded direct scheduled rate authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C153 scheduled Graphical Envelope project should remain resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C153 CLI status should succeed");
    requireContains(status.output, "type=talas.graphicalEnvelope",
        "C153 status should expose promoted Graphical Envelope execution");
    requireContains(status.output, "resources: none",
        "C153 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C153 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.0,
        0.044194173824159216,
        0.086613706172933125,
        0.0625,
        0.0,
        0.033145630368119419,
        0.047835429045636237,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C153 scheduled Graphical Envelope output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C153 scheduled level/gate/rate frame "
                + std::to_string(frame));
    }
}

void testC154AdsrScheduledTypedGateControlsExample()
{
    const auto path = examplePath(
        "c154_adsr_scheduled_typed_gate_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C154 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.adsr\"",
        "C154 example should expose ADSR");
    for (const auto eventName : {
             "!C159Attack", "!C159Decay", "!C159Sustain",
             "!C159Release", "!C159Scale", "!C159Gate",
             "!C159Legato"})
    {
        requireContains(source, eventName,
            "C154 example should expose every scheduled ADSR control");
    }
    requireContains(source, "\"type\", \"choice\": \"linear\"",
        "C154 example should retain exact linear ADSR authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C154 scheduled ADSR project should remain resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C154 CLI status should succeed");
    requireContains(status.output, "type=talas.adsr",
        "C154 status should expose promoted ADSR execution");
    requireContains(status.output, "resources: none",
        "C154 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C154 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.0,
        0.088388347648318433,
        0.23096988312782168,
        0.25,
        0.23096988312782168,
        0.17677669529663689,
        0.0,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C154 scheduled ADSR output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C154 scheduled attack/decay/sustain/release/scale/gate/legato frame "
                + std::to_string(frame));
    }
}

void testC155OscillatorScheduledPhaseControlsExample()
{
    const auto path = examplePath(
        "c155_oscillator_scheduled_phase_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C155 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.oscillator\"",
        "C155 example should expose Oscillator");
    for (const auto eventName : {
             "!C160Frequency", "!C160Amplitude", "!C160Envelope",
             "!C160PitchBend", "!C160Reset"})
    {
        requireContains(source, eventName,
            "C155 example should expose every scheduled Oscillator control");
    }
    requireContains(source, "\"modulation\", \"text\": \"frequency\"",
        "C155 example should expose bounded connected frequency modulation");
    requireContains(source, "\"maxMI\", \"expression\": \"0.25\"",
        "C155 example should expose a static bounded modulation index");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C155 scheduled Oscillator project should remain resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C155 CLI status should succeed");
    requireContains(status.output, "type=talas.oscillator",
        "C155 status should expose promoted Oscillator execution");
    requireContains(status.output, "resources: none",
        "C155 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C155 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.35355339059327373,
        0.49969883381517433,
        0.2248989966249966,
        0.0,
        0.069446279127450272,
        0.45680270473370244,
        0.47499707257695706,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C155 scheduled Oscillator output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C155 scheduled frequency/amplitude/envelope/pitchBend/reset frame "
                + std::to_string(frame));
    }
}

void testC156ChopperScheduledCycleControlsExample()
{
    const auto path = examplePath(
        "c156_chopper_scheduled_cycle_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C156 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.chopper\"",
        "C156 example should expose Chopper");
    requireContains(source, "!C161GrainDuration",
        "C156 example should expose scheduled grain duration");
    requireContains(source, "!C161InterGrainDelay",
        "C156 example should expose scheduled inter-grain delay");
    requireContains(source,
        "aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274",
        "C156 example should pin the immutable finite table hash");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1,
        "C156 scheduled Chopper project should expose one immutable table");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C156 CLI status should succeed");
    requireContains(status.output, "type=talas.chopper",
        "C156 status should expose promoted Chopper execution");
    requireContains(status.output,
        "Envelopes/C156Chopper kind=table generation=1 hash=aa95a3aba2892fae936e255f513790c6f084a2d9a6864be19c5c703ebd204274 health=verified",
        "C156 status should expose exact immutable table authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C156 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.15467960838455724,
        0.0,
        0.088388347648318447,
        2.2962127484012871e-17,
        -0.17677669529663687,
        -0.3125,
        -0.26516504294495535,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C156 scheduled Chopper output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C156 scheduled grain/delay cycle frame "
                + std::to_string(frame));
    }
}

void testC157AnalogSequencerScheduledControlsExample()
{
    const auto path = examplePath(
        "c157_analog_sequencer_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C157 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.analogSequencer\"",
        "C157 example should expose Analog Sequencer");
    for (const auto eventName : {
             std::string_view{"!C162Left"},
             std::string_view{"!C162Right"},
             std::string_view{"!C162Gate"},
             std::string_view{"!C162Step"},
             std::string_view{"!C162Rate"}})
    {
        requireContains(source, eventName,
            "C157 example should expose all five promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty(),
        "C157 scheduled Analog Sequencer should be resource free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C157 CLI status should succeed");
    requireContains(status.output, "type=talas.analogSequencer",
        "C157 status should expose promoted Analog Sequencer execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C157 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    // Frames 3 and 4 advance by 0.25 s and 0.5 s. The integrated phase
    // wraps the 0.75 s cycle at frame 5, so its velocity is the first 0.5.
    constexpr std::array expectedLeft{
        0.0,
        0.0,
        0.0,
        0.35355339059327379,
        6.123233995736766e-17,
        0.13258252147247765,
        0.0,
        0.13258252147247768,
    };
    constexpr std::array expectedRight{
        0.0,
        -0.0,
        0.0,
        -0.17677669529663689,
        -3.061616997868383e-17,
        -0.13258252147247765,
        0.0,
        -0.13258252147247768,
    };
    require(reference.size() == 2 && alternate.size() == 2
            && reference[0].samples == alternate[0].samples
            && reference[1].samples == alternate[1].samples
            && reference[0].samples.size() == expectedLeft.size()
            && reference[1].samples.size() == expectedRight.size(),
        "C157 scheduled Analog Sequencer stereo output should be partition stable");
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                    == std::bit_cast<std::uint64_t>(expectedLeft[frame])
                && std::bit_cast<std::uint64_t>(
                       reference[1].samples[frame])
                    == std::bit_cast<std::uint64_t>(
                       expectedRight[frame]),
            "C157 scheduled Analog Sequencer stereo frame "
                + std::to_string(frame));
    }
}

void testC158ArScheduledControlsExample()
{
    const auto path = examplePath(
        "c158_ar_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C158 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.ar\"",
        "C158 example should expose AR");
    for (const auto eventName : {
             std::string_view{"!C163Attack"},
             std::string_view{"!C163Release"},
             std::string_view{"!C163Scale"},
             std::string_view{"!C163Gate"}})
    {
        requireContains(source, eventName,
            "C158 example should expose all four promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 13,
        "C158 scheduled AR should be resource free and bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C158 CLI status should succeed");
    requireContains(status.output, "type=talas.ar",
        "C158 status should expose promoted AR execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C158 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        0.088388347648318433,
        -0.25,
        -0.17677669529663689,
        -1.5308084989341915e-17,
        -0.0,
        -0.09375,
        -0.066291260736238838,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C158 scheduled AR output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C158 scheduled AR frame "
                + std::to_string(frame));
    }
}

void testC159AnalysisFilterScheduledControlsExample()
{
    const auto path = examplePath(
        "c159_analysis_filter_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C159 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.analysisFilter\"",
        "C159 example should expose Analysis Filter");
    requireContains(source, "!C164Fundamental",
        "C159 example should expose the fundamental schedule");
    requireContains(source, "!C164Harmonic",
        "C159 example should expose the harmonic schedule");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C159 scheduled Analysis Filter should be resource free and bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C159 CLI status should succeed");
    requireContains(status.output, "type=talas.analysisFilter",
        "C159 status should expose promoted Analysis Filter execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C159 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 8.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expectedLeft{
        0.0,
        0.030731444936720149,
        0.084819434993433307,
        0.14657693157232868,
        0.17023199440058043,
        0.13808175213837276,
        0.093795916965219106,
        0.030048179207425404,
    };
    constexpr std::array expectedRight{
        0.0,
        -0.020534095019109605,
        -0.047404058148769586,
        -0.059046671999609314,
        0.024024931145480675,
        0.10926735344873359,
        0.15077883192404584,
        0.19287700305265007,
    };
    require(reference.size() == 2 && alternate.size() == 2
            && reference[0].samples == alternate[0].samples
            && reference[1].samples == alternate[1].samples
            && reference[0].samples.size() == expectedLeft.size()
            && reference[1].samples.size() == expectedRight.size(),
        "C159 scheduled Analysis Filter stereo output should be partition stable");
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(reference[0].samples[frame])
                    == std::bit_cast<std::uint64_t>(expectedLeft[frame])
                && std::bit_cast<std::uint64_t>(reference[1].samples[frame])
                    == std::bit_cast<std::uint64_t>(expectedRight[frame]),
            "C159 scheduled Analysis Filter stereo frame "
                + std::to_string(frame));
    }
}

void testC160PhaseShiftBy90ScheduledFrequencyExample()
{
    const auto path = examplePath(
        "c160_phase_shift_by_90_scheduled_frequency.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C160 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.phaseShiftBy90\"",
        "C160 example should expose Phase Shift By 90");
    requireContains(source, "!C165Frequency",
        "C160 example should expose the direct frequency schedule");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 4,
        "C160 scheduled Phase Shift By 90 should be resource free and bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "1024", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C160 CLI status should succeed");
    requireContains(status.output, "type=talas.phaseShiftBy90",
        "C160 status should expose promoted Phase Shift By 90 execution");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "1024", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C160 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 1024.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    // Independently calculated model values allow only math-library rounding;
    // the two CLI block partitions above still have to agree exactly.
    constexpr std::array expectedLeft{
        -0.0,
        0.16221167441072884,
        0.17335925878090577,
        0.095670858091272348,
        -0.040736863144095095,
        -0.26140772123536754,
        -0.40974343743648328,
        -0.55483356967230812,
    };
    constexpr std::array expectedRight{
        0.0,
        -0.067190275516172668,
        0.19134171618254486,
        0.35355339059327373,
        0.46193976625564337,
        0.5,
        0.42757853914090838,
        0.4990273078923681,
    };
    require(reference.size() == 2 && alternate.size() == 2
            && reference[0].samples == alternate[0].samples
            && reference[1].samples == alternate[1].samples
            && reference[0].samples.size() == expectedLeft.size()
            && reference[1].samples.size() == expectedRight.size(),
        "C160 scheduled Phase Shift By 90 stereo output should be partition stable");
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        require(
            std::abs(reference[0].samples[frame] - expectedLeft[frame]) <= 1.0e-14,
            "C160 scheduled Phase Shift By 90 left frame "
                + std::to_string(frame));
        require(
            std::abs(reference[1].samples[frame] - expectedRight[frame]) <= 1.0e-14,
            "C160 scheduled Phase Shift By 90 right frame "
                + std::to_string(frame));
    }
}

void testC161SingleSideBandRmScheduledQuadratureExample()
{
    const auto path = examplePath(
        "c161_single_side_band_rm_scheduled_quadrature.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C161 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.singleSideBandRm\"",
        "C161 example should expose Single Side Band RM");
    requireContains(source, "!C166Frequency",
        "C161 example should expose the direct frequency schedule");
    requireContains(source, "!C166FreqScale",
        "C161 example should expose the direct frequency-scale schedule");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 8,
        "C161 scheduled Single Side Band RM should be resource free and bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "1024", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C161 CLI status should succeed");
    requireContains(status.output, "type=talas.singleSideBandRm",
        "C161 status should expose promoted Single Side Band RM execution");
    requireContains(status.output, "resources: none",
        "C161 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "1024", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C161 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 1024.0);
    };
    const auto reference = render("3");
    const auto alternate = render("7");
    constexpr std::array expected{
        0.0,
        -0.0975451610080641,
        0.11043497911409202,
        0.2900290888674126,
        0.4423660179084061,
        0.5619761702647303,
        0.012611321358350913,
        -0.7035688619985824,
    };
    require(reference.size() == 1 && alternate.size() == 1
            && reference[0].samples == alternate[0].samples
            && reference[0].samples.size() == expected.size(),
        "C161 scheduled Single Side Band RM output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(reference[0].samples[frame] - expected[frame]) <= 1.0e-14,
            "C161 scheduled Single Side Band RM frame "
                + std::to_string(frame));
    }
}

void testC162SosOscillatorsStaticSpectrumSineBankExample()
{
    const auto path = examplePath(
        "c162_sos_oscillators_static_spectrum_sine_bank.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C162 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.sosOscillators\"",
        "C162 example should expose SOS Oscillators");
    requireContains(source, "\"resource\": \"\"",
        "C162 example should select the compiler-owned sine fallback");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C162 static Spectrum sine bank should be resource free and bounded");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "16", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C162 CLI status should succeed");
    requireContains(status.output, "type=talas.sosOscillators",
        "C162 status should expose promoted SOS Oscillators execution");
    requireContains(status.output, "resources: none",
        "C162 status should expose its sine-fallback product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "8",
            "--sample-rate", "16", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C162 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 8, 16.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto eightFrames = render("8");
    constexpr std::array expected{
        0.0,
        0.32761076771452496,
        0.38045230742999819,
        0.15565976509868124,
        -0.10216457095436374,
        -0.17525461804815071,
        -0.074487703694567628,
        0.041104395100511767,
    };
    require(oneFrame.size() == 1 && threeFrames.size() == 1
            && eightFrames.size() == 1
            && oneFrame[0].samples == threeFrames[0].samples
            && oneFrame[0].samples == eightFrames[0].samples
            && oneFrame[0].samples.size() == expected.size(),
        "C162 static Spectrum sine-bank output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(oneFrame[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C162 static Spectrum sine-bank frame "
                + std::to_string(frame));
    }
}

void testC163LowPassGateScheduledControlExample()
{
    const auto path = examplePath(
        "c163_low_pass_gate_scheduled_control.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C163 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.lowPassGate\"",
        "C163 example should expose Low-Pass Gate");
    requireContains(source, "\"expression\": \"!C168GateControl\"",
        "C163 example should expose direct scheduled control authority");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 3,
        "C163 scheduled Low-Pass Gate should be resource free and own three control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C163 CLI status should succeed");
    requireContains(status.output, "type=talas.lowPassGate",
        "C163 status should expose promoted Low-Pass Gate execution");
    requireContains(status.output, "resources: none",
        "C163 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "16",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C163 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 16, 8.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto sixteenFrames = render("16");
    constexpr std::array expected{
        0.0,
        0.0,
        0.0,
        0.0,
        0.029036986819207626,
        -0.12712194618614481,
        -0.39894548369583305,
        -0.41188050272354415,
        -0.068681022367691441,
        0.12854022689967634,
        0.17552061285365783,
        0.12660945779554236,
        0.043663044121824139,
        -0.026834673761363354,
        -0.060950632146538529,
        -0.05606710874394237,
    };
    require(oneFrame.size() == 1 && threeFrames.size() == 1
            && sixteenFrames.size() == 1
            && oneFrame[0].samples == threeFrames[0].samples
            && oneFrame[0].samples == sixteenFrames[0].samples
            && oneFrame[0].samples.size() == expected.size(),
        "C163 scheduled Low-Pass Gate output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(oneFrame[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C163 scheduled Low-Pass Gate frame "
                + std::to_string(frame));
    }
}

void testC164MorphFilterScheduledControlsExample()
{
    const auto path = examplePath(
        "c164_morph_filter_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C164 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.morphFilter\"",
        "C164 example should expose Morph Filter");
    for (const auto eventName : {
             std::string_view{"!C169Cutoff"},
             std::string_view{"!C169Resonance"},
             std::string_view{"!C169Morph"},
             std::string_view{"!C169Drive"},
             std::string_view{"!C169Scale"}})
    {
        requireContains(source, eventName,
            "C164 example should expose all five promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 15,
        "C164 scheduled Morph Filter should be resource free and own fifteen control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C164 CLI status should succeed");
    requireContains(status.output, "type=talas.morphFilter",
        "C164 status should expose promoted Morph Filter execution");
    requireContains(status.output, "resources: none",
        "C164 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "16",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C164 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 16, 8.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto sixteenFrames = render("16");
    constexpr std::array expected{
        0.0,
        0.011993796127068483,
        0.057410182323496504,
        0.13088012405751237,
        0.030978671353941278,
        -0.37384774610411575,
        -0.53306987795834804,
        -0.095798448329343977,
        0.074158696599445573,
        -0.19080785373957443,
        0.0090904383126407806,
        0.054003818825257899,
        -0.19949678107390975,
        0.16965576679994954,
        0.051937851532007029,
        -0.10438239781057196,
    };
    require(oneFrame.size() == 1 && threeFrames.size() == 1
            && sixteenFrames.size() == 1
            && oneFrame[0].samples == threeFrames[0].samples
            && oneFrame[0].samples == sixteenFrames[0].samples
            && oneFrame[0].samples.size() == expected.size(),
        "C164 scheduled Morph Filter output should be partition stable");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::bit_cast<std::uint64_t>(oneFrame[0].samples[frame])
                == std::bit_cast<std::uint64_t>(expected[frame]),
            "C164 scheduled Morph Filter frame " + std::to_string(frame));
    }
}

void testC165QuadFunctionGeneratorScheduledControlsExample()
{
    const auto path = examplePath(
        "c165_quad_function_generator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C165 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.quadFunctionGenerator\"",
        "C165 example should expose Quad Function Generator");
    for (const auto eventName : {
             std::string_view{"!C170Frequency"},
             std::string_view{"!C170PhaseOffset"},
             std::string_view{"!C170Skew"},
             std::string_view{"!C170Curve"},
             std::string_view{"!C170Smooth"},
             std::string_view{"!C170Scale"}})
    {
        requireContains(source, eventName,
            "C165 example should expose all six promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 18,
        "C165 scheduled generator should be resource free and own eighteen control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C165 CLI status should succeed");
    requireContains(status.output, "type=talas.quadFunctionGenerator",
        "C165 status should expose promoted generator execution");
    requireContains(status.output, "outputs: discrete (4)",
        "C165 status should expose ordered four-channel output");
    requireContains(status.output, "resources: none",
        "C165 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "16",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C165 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 16, 8.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto sixteenFrames = render("16");
    constexpr std::array<std::array<double, 16>, 4> expected{{
        {{0.0, 0.25, 0.5, 0.75, 0.073223304703363107,
          0.29877258050403205, 0.5, 0.0048036798991923924, 0.0,
          0.27146168557940686, 0.00485259984158431, 1.5379985116641821,
          0.091191555289370868, 0.00029996232270080062,
          0.71682659511259972, 0.025732744422370595}},
        {{1.0, 0.91666666666666663, 0.83333333333333337, 0.75,
          0.39339410908776151, 0.29877258050403205, 0.0,
          0.0048036798991923924, 0.71682659511259972,
          0.025732744422370595, 0.0, 0.27146168557940686,
          0.00485259984158431, 1.5379985116641821,
          0.091191555289370868, 0.00029996232270080062}},
        {{0.66666666666666663, 0.58333333333333337, 0.5,
          0.41666666666666669, 0.073223304703363107,
          5.9493230022716892e-05, 0.015076844803522893,
          0.12975280777015311, 0.091191555289370868,
          0.00029996232270080062, 0.71682659511259972,
          0.025732744422370595, 0.0, 0.27146168557940686,
          0.00485259984158431, 1.5379985116641821}},
        {{0.33333333333333331, 0.25, 0.16666666666666666,
          0.083333333333333329, 0.0009513254770636137,
          0.036272032331763349, 0.2065879555832674,
          0.46918168892687689, 0.00485259984158431,
          1.5379985116641821, 0.091191555289370868,
          0.00029996232270080062, 0.71682659511259972,
          0.025732744422370595, 0.0, 0.27146168557940686}},
    }};
    require(oneFrame.size() == 4 && threeFrames.size() == 4
            && sixteenFrames.size() == 4,
        "C165 should render four ordered channels");
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        require(oneFrame[channel].samples == threeFrames[channel].samples
                && oneFrame[channel].samples
                    == sixteenFrames[channel].samples
                && oneFrame[channel].samples.size()
                    == expected[channel].size(),
            "C165 scheduled output should be partition stable");
        for (std::size_t frame = 0; frame < expected[channel].size(); ++frame)
        {
            require(
                std::bit_cast<std::uint64_t>(
                    oneFrame[channel].samples[frame])
                    == std::bit_cast<std::uint64_t>(
                        expected[channel][frame]),
                "C165 scheduled generator channel "
                    + std::to_string(channel) + " frame "
                    + std::to_string(frame));
        }
    }
}

void testC166ProbabilityChaosGeneratorScheduledControlsExample()
{
    const auto path = examplePath(
        "c166_probability_chaos_generator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C166 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.probabilityChaosGenerator\"",
        "C166 example should expose Probability Chaos Generator");
    for (const auto eventName : {
             std::string_view{"!C171Frequency"},
             std::string_view{"!C171Probability"},
             std::string_view{"!C171Chaos"},
             std::string_view{"!C171Slew"},
             std::string_view{"!C171Scale"}})
    {
        requireContains(source, eventName,
            "C166 example should expose all five promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 15,
        "C166 scheduled generator should be resource free and own fifteen control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C166 CLI status should succeed");
    requireContains(status.output, "type=talas.probabilityChaosGenerator",
        "C166 status should expose promoted generator execution");
    requireContains(status.output, "outputs: stereo",
        "C166 status should expose ordered stereo output");
    requireContains(status.output, "resources: none",
        "C166 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "16",
            "--sample-rate", "8", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C166 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 16, 8.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto sixteenFrames = render("16");
    constexpr std::array<std::array<double, 16>, 2> expected{{
        {{0.82523747480885734, 0.82523747480885734,
          0.82523747480885734, 0.82523747480885734,
          0.41261873740442867, -0.011506417336917488,
          -0.13302030863938358, -0.16783462138268784,
          -0.67203312132341586, -0.67271912821167978,
          -0.67339661338551515, -0.67406568270335865,
          -0.67472644070865251, -0.67537899064617934,
          -0.67602343447819446, -0.67665987290035778}},
        {{0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    }};
    require(oneFrame.size() == 2 && threeFrames.size() == 2
            && sixteenFrames.size() == 2
            && oneFrame[0].samples == threeFrames[0].samples
            && oneFrame[0].samples == sixteenFrames[0].samples
            && oneFrame[1].samples == threeFrames[1].samples
            && oneFrame[1].samples == sixteenFrames[1].samples,
        "C166 scheduled stereo output should be partition stable");
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        require(oneFrame[channel].samples.size()
                == expected[channel].size(),
            "C166 scheduled output should own sixteen exact frames");
        for (std::size_t frame = 0; frame < expected[channel].size(); ++frame)
        {
            require(
                std::bit_cast<std::uint64_t>(
                    oneFrame[channel].samples[frame])
                    == std::bit_cast<std::uint64_t>(
                        expected[channel][frame]),
                "C166 scheduled generator channel "
                    + std::to_string(channel) + " frame "
                    + std::to_string(frame));
        }
    }
}

void testC167StereoDispersiveResonatorScheduledControlsExample()
{
    const auto path = examplePath(
        "c167_stereo_dispersive_resonator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C167 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.stereoDispersiveResonator\"",
        "C167 example should expose Stereo Dispersive Resonator");
    for (const auto eventName : {
             std::string_view{"!C172Frequency"},
             std::string_view{"!C172DecayTime"},
             std::string_view{"!C172Detune"},
             std::string_view{"!C172Damping"},
             std::string_view{"!C172Dispersion"},
             std::string_view{"!C172Width"},
             std::string_view{"!C172Mix"},
             std::string_view{"!C172InputGain"}})
    {
        requireContains(source, eventName,
            "C167 example should expose all eight promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 24,
        "C167 scheduled resonator should be resource free and own twenty-four control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "8000", "--max-block-frames", "3",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C167 CLI status should succeed");
    requireContains(status.output, "type=talas.stereoDispersiveResonator",
        "C167 status should expose promoted resonator execution");
    requireContains(status.output, "outputs: stereo",
        "C167 status should expose ordered stereo output");
    requireContains(status.output, "resources: none",
        "C167 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "16",
            "--sample-rate", "8000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C167 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 16, 8000.0);
    };
    const auto oneFrame = render("1");
    const auto threeFrames = render("3");
    const auto sixteenFrames = render("16");
    constexpr std::array<std::array<double, 16>, 2> expected{{
        {{0.0, 0.007821723252011541, 0.015450849718747366,
          0.022699524986977335, 0.0, 4.2476249782336485e-06,
          0.00011235355186847617, 0.0008865162116262692,
          0.16643489035165185, 0.1728454596041491,
          0.17499999999999999, 0.1728454596041491,
          0.16643489035165188, 0.15592614173296437,
          0.14157797401561578, 0.12374368670764582}},
        {{0.0, 0.007821723252011541, 0.015450849718747366,
          0.022699524986977335, 0.0, 7.0793749637227464e-06,
          0.0001419772546385993, 0.00099740881442187887,
          0.16643489035165185, 0.1728454596041491,
          0.17499999999999999, 0.1728454596041491,
          0.16643489035165188, 0.15592614173296437,
          0.14157797401561578, 0.12374368670764582}},
    }};
    require(oneFrame.size() == 2 && threeFrames.size() == 2
            && sixteenFrames.size() == 2
            && oneFrame[0].samples == threeFrames[0].samples
            && oneFrame[0].samples == sixteenFrames[0].samples
            && oneFrame[1].samples == threeFrames[1].samples
            && oneFrame[1].samples == sixteenFrames[1].samples,
        "C167 scheduled stereo output should be partition stable");
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        require(oneFrame[channel].samples.size()
                == expected[channel].size(),
            "C167 scheduled output should own sixteen exact frames");
        for (std::size_t frame = 0; frame < expected[channel].size(); ++frame)
        {
            require(
                std::bit_cast<std::uint64_t>(
                    oneFrame[channel].samples[frame])
                    == std::bit_cast<std::uint64_t>(
                        expected[channel][frame]),
                "C167 scheduled resonator channel "
                    + std::to_string(channel) + " frame "
                    + std::to_string(frame));
        }
    }
}

void testC168PluckedWaveguideScheduledControlsExample()
{
    const auto path = examplePath(
        "c168_plucked_waveguide_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C168 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.pluckedWaveguide\"",
        "C168 example should expose Plucked Waveguide");
    for (const auto eventName : {
             std::string_view{"!C173Trigger"},
             std::string_view{"!C173Frequency"},
             std::string_view{"!C173DecayTime"},
             std::string_view{"!C173Brightness"},
             std::string_view{"!C173PickPosition"},
             std::string_view{"!C173Stiffness"},
             std::string_view{"!C173Level"}})
    {
        requireContains(source, eventName,
            "C168 example should expose all seven promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 22,
        "C168 scheduled waveguide should be resource free and own twenty-two control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C168 CLI status should succeed");
    requireContains(status.output, "type=talas.pluckedWaveguide",
        "C168 status should expose promoted waveguide execution");
    requireContains(status.output, "outputs: mono",
        "C168 status should expose mono output");
    requireContains(status.output, "resources: none",
        "C168 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C168 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C168 scheduled mono output should be partition stable");
    require(oneFrame[0].samples.size() == 64
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[0])
                == std::bit_cast<std::uint64_t>(0.0)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[20])
                == std::bit_cast<std::uint64_t>(
                    0.00036095296175685731)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[24])
                == std::bit_cast<std::uint64_t>(
                    0.72501610808815675)
            && std::ranges::any_of(
                oneFrame[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C168 scheduled waveguide should preserve its exact deterministic onset");
}

void testC169DynamicBreakpointOscillatorScheduledControlsExample()
{
    const auto path = examplePath(
        "c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C169 example should use current project format");
    requireContains(source,
        "\"typeId\": \"talas.dynamicBreakpointOscillator\"",
        "C169 example should expose Dynamic Breakpoint Oscillator");
    for (const auto eventName : {
             std::string_view{"!C174Reset"},
             std::string_view{"!C174Frequency"},
             std::string_view{"!C174Volatility"},
             std::string_view{"!C174Correlation"},
             std::string_view{"!C174TimingJitter"},
             std::string_view{"!C174Curve"},
             std::string_view{"!C174Level"}})
    {
        requireContains(source, eventName,
            "C169 example should expose all seven promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 22,
        "C169 scheduled oscillator should be resource free and own twenty-two control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C169 CLI status should succeed");
    requireContains(status.output, "type=talas.dynamicBreakpointOscillator",
        "C169 status should expose promoted oscillator execution");
    requireContains(status.output, "outputs: mono",
        "C169 status should expose mono output");
    requireContains(status.output, "resources: none",
        "C169 status should expose its resource-free product slice");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C169 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C169 scheduled mono output should be partition stable");
    require(oneFrame[0].samples.size() == 64
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[0])
                == std::bit_cast<std::uint64_t>(0.0)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[8])
                == std::bit_cast<std::uint64_t>(0.58695792795972013)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[24])
                == std::bit_cast<std::uint64_t>(0.0)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[41])
                == std::bit_cast<std::uint64_t>(0.051599919286334242)
            && std::ranges::any_of(
                oneFrame[0].samples,
                [](double value) { return std::abs(value) > 0.5; }),
        "C169 scheduled oscillator should preserve exact deterministic segment and reset oracles");
}

void testC170WaveTerrainOscillatorScheduledControlsExample()
{
    const auto path = examplePath(
        "c170_wave_terrain_oscillator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C170 example should use current project format");
    requireContains(source,
        "\"typeId\": \"talas.waveTerrainOscillator\"",
        "C170 example should expose Wave Terrain Oscillator");
    for (const auto eventName : {
             std::string_view{"!C175Reset"},
             std::string_view{"!C175Frequency"},
             std::string_view{"!C175XRadius"},
             std::string_view{"!C175YRadius"},
             std::string_view{"!C175YRatio"},
             std::string_view{"!C175Rotation"},
             std::string_view{"!C175PhaseOffset"},
             std::string_view{"!C175Level"}})
    {
        requireContains(source, eventName,
            "C170 example should expose all eight promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id
                == "Tables/C170Terrain"
            && parsed.document.timeline.eventValues.size() == 25,
        "C170 scheduled oscillator should own one terrain and twenty-five control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C170 CLI status should succeed");
    requireContains(status.output, "type=talas.waveTerrainOscillator",
        "C170 status should expose promoted oscillator execution");
    requireContains(status.output, "outputs: mono",
        "C170 status should expose mono output");
    requireContains(
        status.output,
        "Tables/C170Terrain kind=table generation=1 hash=4dd526eb63192bf29f962d5785ebabba5b8193f0c5156fa405312557263ebe3d health=verified",
        "C170 status should expose exact immutable terrain authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C170 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C170 scheduled mono terrain output should be partition stable");
    require(oneFrame[0].samples.size() == 64
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[0])
                == std::bit_cast<std::uint64_t>(
                    -0.09312499999999992)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[8])
                == std::bit_cast<std::uint64_t>(
                    0.2472796582143748)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[24])
                == std::bit_cast<std::uint64_t>(
                    0.2124601493348974)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[32])
                == std::bit_cast<std::uint64_t>(
                    0.5626907259276477)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[63])
                == std::bit_cast<std::uint64_t>(
                    0.3893283752611726)
            && std::ranges::any_of(
                oneFrame[0].samples,
                [](double value) { return std::abs(value) > 0.1; }),
        "C170 scheduled terrain should preserve exact deterministic trajectory and reset oracles");
}

void testC171ScannedMassOscillatorScheduledControlsExample()
{
    const auto path = examplePath(
        "c171_scanned_mass_oscillator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C171 example should use current project format");
    requireContains(source,
        "\"typeId\": \"talas.scannedMassOscillator\"",
        "C171 example should expose Scanned Mass Oscillator");
    for (const auto eventName : {
             std::string_view{"!C176Reset"},
             std::string_view{"!C176Excite"},
             std::string_view{"!C176Frequency"},
             std::string_view{"!C176ModelRate"},
             std::string_view{"!C176Coupling"},
             std::string_view{"!C176Damping"},
             std::string_view{"!C176Restoring"},
             std::string_view{"!C176Position"},
             std::string_view{"!C176Amount"},
             std::string_view{"!C176Offset"},
             std::string_view{"!C176Level"}})
    {
        requireContains(source, eventName,
            "C171 example should expose all eleven promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 36,
        "C171 scheduled oscillator should own no resources and thirty-six control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C171 CLI status should succeed");
    requireContains(status.output, "type=talas.scannedMassOscillator",
        "C171 status should expose promoted oscillator execution");
    requireContains(status.output, "outputs: mono",
        "C171 status should expose mono output");
    requireContains(status.output, "resources: none",
        "C171 status should expose exact resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C171 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C171 scheduled mono Scanned Mass output should be partition stable");
    require(oneFrame[0].samples.size() == 64
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[0])
                == std::bit_cast<std::uint64_t>(0.0)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[8])
                == std::bit_cast<std::uint64_t>(0.51266468713280045)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[24])
                == std::bit_cast<std::uint64_t>(0.41085783637895029)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[32])
                == std::bit_cast<std::uint64_t>(-0.24651470182737018)
            && std::bit_cast<std::uint64_t>(oneFrame[0].samples[63])
                == std::bit_cast<std::uint64_t>(0.17709941715490535)
            && std::ranges::any_of(
                oneFrame[0].samples,
                [](double value) { return std::abs(value) > 0.5; }),
        "C171 scheduled oscillator should preserve exact deterministic model, excitation, and reset oracles");
}

void testC172DiffuseSpectralStretchScheduledControlsExample()
{
    const auto path =
        examplePath("c172_diffuse_spectral_stretch_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C172 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.diffuseSpectralStretch\"",
        "C172 example should expose Diffuse Spectral Stretch");
    for (const auto eventName : {std::string_view{"!C177Reset"},
             std::string_view{"!C177Gate"}, std::string_view{"!C177Level"}})
    {
        requireContains(source, eventName,
            "C172 example should expose all three promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.size() == 1
            && parsed.document.assets.front().id == "Tables/C172DiffuseSource"
            && parsed.document.timeline.eventValues.size() == 8,
        "C172 scheduled stretch should own one source table and eight "
        "control values");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate",
        "--project",
        path.string(),
        "--status",
        "--sample-rate",
        "48000",
        "--max-block-frames",
        "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C172 CLI status should succeed");
    requireContains(status.output, "type=talas.diffuseSpectralStretch",
        "C172 status should expose promoted stretch execution");
    requireContains(status.output, "critical-path: 4095 frames",
        "C172 status should expose exact fixed latency");
    requireContains(status.output,
        "Tables/C172DiffuseSource kind=table generation=1 "
        "hash=8ddbee8d7ec174ec4674c9366d38798bad76bf5c6b721796097326d321c11576 "
        "health=verified",
        "C172 status should expose exact immutable source authority");

    const auto render = [&](std::string_view maximumBlockFrames)
    {
        const auto rendered = runCli({
            "render",
            "--project",
            path.string(),
            "--frames",
            "12000",
            "--sample-rate",
            "48000",
            "--max-block-frames",
            std::string{maximumBlockFrames},
            "--format",
            "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C172 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 12000, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
                && sixtyFourFrames.size() == 1
                && oneFrame[0].samples == sevenFrames[0].samples
                && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C172 scheduled mono spectral output should be partition stable");
    require(oneFrame[0].samples.size() == 12000
                && oneFrame[0].samples[4094] == 0.0
                && std::bit_cast<std::uint64_t>(oneFrame[0].samples[4800])
                       == std::bit_cast<std::uint64_t>(1.1476049806271704e-07)
                && oneFrame[0].samples[6720] == 0.0
                && std::bit_cast<std::uint64_t>(oneFrame[0].samples[11056])
                       == std::bit_cast<std::uint64_t>(-6.804965282814979e-14)
                && std::ranges::all_of(oneFrame[0].samples,
                       [](double value) { return std::abs(value) < 2.0e-5; }),
        "C172 scheduled stretch should preserve exact latency, gate, "
        "restart, and spectral oracles");
}

void testC173DualMorphOscillatorScheduledControlsExample()
{
    const auto path =
        examplePath("c173_dual_morph_oscillator_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C173 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.dualMorphOscillator\"",
        "C173 example should expose Dual Morph Oscillator");
    for (const auto eventName : {std::string_view{"!C178Frequency"},
             std::string_view{"!C178Ratio"},
             std::string_view{"!C178PhaseMod"},
             std::string_view{"!C178Morph"}, std::string_view{"!C178Fold"},
             std::string_view{"!C178Amplitude"}})
    {
        requireContains(source, eventName,
            "C173 example should expose all six promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 24,
        "C173 scheduled oscillator should own twenty-four control values and no resources");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C173 CLI status should succeed");
    requireContains(status.output, "type=talas.dualMorphOscillator",
        "C173 status should expose promoted oscillator execution");
    requireContains(status.output, "outputs: stereo (2)",
        "C173 status should expose both oscillator outputs");
    requireContains(status.output, "resources: none",
        "C173 status should expose exact resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C173 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 2 && sevenFrames.size() == 2
            && sixtyFourFrames.size() == 2
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples
            && oneFrame[1].samples == sevenFrames[1].samples
            && oneFrame[1].samples == sixtyFourFrames[1].samples,
        "C173 scheduled dual output should be partition stable");
    require(oneFrame[0].samples.size() == 64
            && oneFrame[1].samples.size() == 64,
        "C173 scheduled oscillator should publish two complete channels");
    for (const auto [channel, frame, expected] : {
             std::tuple{std::size_t{0}, std::size_t{8}, -0.6908165507705654},
             std::tuple{std::size_t{0}, std::size_t{24}, 0.75},
             std::tuple{std::size_t{0}, std::size_t{32}, 0.427130768461672},
             std::tuple{std::size_t{0}, std::size_t{63}, 0.9339335168296231},
             std::tuple{std::size_t{1}, std::size_t{8}, 1.1888206453689418},
             std::tuple{std::size_t{1}, std::size_t{24}, 0.0},
             std::tuple{std::size_t{1}, std::size_t{32}, -0.45399049973954625},
             std::tuple{std::size_t{1}, std::size_t{63}, -0.9969173337331277},
         })
    {
        require(std::abs(oneFrame[channel].samples[frame] - expected)
                    <= 1.0e-12,
            "C173 scheduled oscillator should preserve representative morph, fold, phase-modulation, ratio, and amplitude oracles");
    }
}

void testC174LogicScheduledFallbacksExample()
{
    const auto path = examplePath("c174_logic_scheduled_fallbacks.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C174 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.logic\"",
        "C174 example should expose Logic");
    requireContains(source, "\"choice\": \"xor\"",
        "C174 example should freeze the static XOR operation");
    requireContains(source, "!C179Left",
        "C174 example should expose the left schedule");
    requireContains(source, "!C179Right",
        "C174 example should expose the right schedule");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 7,
        "C174 Logic should own seven scheduled values and no resources");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C174 CLI status should succeed");
    requireContains(status.output, "type=talas.logic",
        "C174 status should expose promoted Logic execution");
    requireContains(status.output, "outputs: mono (1)",
        "C174 status should expose one terminal channel");
    requireContains(status.output, "resources: none",
        "C174 status should expose exact resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C174 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C174 scheduled Logic output should be partition stable");
    for (const auto [frame, expected] : {
             std::pair{std::size_t{1}, 0.03922954786392247},
             std::pair{std::size_t{8}, 0.0},
             std::pair{std::size_t{16}, 0.47552825814757677},
             std::pair{std::size_t{24}, 0.0},
             std::pair{std::size_t{32}, 0.2938926261462366},
             std::pair{std::size_t{48}, 0.0},
         })
    {
        require(std::abs(oneFrame[0].samples[frame] - expected) <= 1.0e-12,
            "C174 Logic should preserve exact binary-gated oscillator oracles");
    }
}

void testC175AnnotationPassThroughExample()
{
    const auto path = examplePath("c175_annotation_passthrough.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C175 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.annotation\"",
        "C175 example should expose Annotation");
    const auto parsed = talas::project::parseProjectDocument(source);
    require(parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.empty(),
        "C175 Annotation should remain resource and schedule free");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C175 CLI status should succeed");
    requireContains(status.output, "type=talas.annotation",
        "C175 status should expose promoted Annotation execution");
    requireContains(status.output, "critical-path: 0 frames",
        "C175 status should expose zero latency");
    requireContains(status.output, "resources: none",
        "C175 status should expose resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C175 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C175 Annotation output should be partition stable");
    for (const auto [frame, expected] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{1}, 0.0490085701647803},
             std::pair{std::size_t{7}, 0.31719664208182274},
             std::pair{std::size_t{16}, 0.5},
             std::pair{std::size_t{32}, 0.0},
             std::pair{std::size_t{47}, -0.4975923633360984},
             std::pair{std::size_t{63}, -0.04900857016478025},
         })
    {
        require(std::abs(oneFrame[0].samples[frame] - expected) <= 1.0e-12,
            "C175 Annotation should preserve exact oscillator samples");
    }
}

void testC191FilterScheduledControlsExample()
{
    const auto path =
        examplePath("c191_filter_scheduled_controls.talas.json");
    const auto source = readText(path);
    requireContains(source, "\"version\": 35",
        "C191 example should use current project format");
    requireContains(source, "\"typeId\": \"talas.filter\"",
        "C191 example should expose Filter");
    for (const auto eventName : {
             std::string_view{"!C191Frequency"},
             std::string_view{"!C191Q"},
             std::string_view{"!C191Scale"},
             std::string_view{"!C191Feedback"},
         })
    {
        requireContains(source, eventName,
            "C191 example should expose all four promoted schedules");
    }
    const auto parsed = talas::project::parseProjectDocument(source);
    require(
        parsed.ok() && parsed.document.formatVersion == talas::project::currentProjectFormatVersion
            && parsed.document.assets.empty()
            && parsed.document.timeline.eventValues.size() == 16,
        "C191 scheduled Filter should own sixteen control values and no "
        "resources");
    requireOrdinaryValidation(path);

    const auto status = runCli({
        "validate", "--project", path.string(), "--status",
        "--sample-rate", "48000", "--max-block-frames", "7",
    });
    require(status.exitCode == 0 && status.error.empty(),
        "C191 CLI status should succeed");
    requireContains(status.output, "type=talas.filter",
        "C191 status should expose promoted Filter execution");
    requireContains(status.output, "critical-path: 0 frames",
        "C191 status should expose zero latency");
    requireContains(status.output, "resources: none",
        "C191 status should expose exact resource-free authority");

    const auto render = [&](std::string_view maximumBlockFrames) {
        const auto rendered = runCli({
            "render", "--project", path.string(), "--frames", "64",
            "--sample-rate", "48000", "--max-block-frames",
            std::string{maximumBlockFrames}, "--format", "json",
        });
        require(rendered.exitCode == 0 && rendered.error.empty(),
            "C191 CLI render should succeed");
        return parseRenderedChannels(rendered.output, 64, 48000.0);
    };
    const auto oneFrame = render("1");
    const auto sevenFrames = render("7");
    const auto sixtyFourFrames = render("64");
    require(oneFrame.size() == 1 && sevenFrames.size() == 1
            && sixtyFourFrames.size() == 1
            && oneFrame[0].samples == sevenFrames[0].samples
            && oneFrame[0].samples == sixtyFourFrames[0].samples,
        "C191 scheduled mono Filter output should be partition stable");
    for (const auto [frame, expected] : {
             std::pair{std::size_t{0}, 0.0},
             std::pair{std::size_t{1}, 8.3597375759216223e-14},
             std::pair{std::size_t{7}, 4.2143783949435917e-09},
             std::pair{std::size_t{8}, 4.9626538431432437e-08},
             std::pair{std::size_t{16}, 0.0006968135233670383},
             std::pair{std::size_t{24}, 0.09271625224762424},
             std::pair{std::size_t{32}, 0.31129728475214152},
             std::pair{std::size_t{63}, 0.31589498629209434},
    })
    {
        require(std::abs(oneFrame[0].samples[frame] - expected) <= 1.0e-12,
            "C191 Filter should preserve the scheduled oracle at frame "
                + std::to_string(frame));
    }
}

int main()
{
    try
    {
        constexpr std::array<std::string_view, 155> requiredExamples = {
            "m3_native_fft_round_trip.talas.json",
            "m3_packed_fft_round_trip.talas.json",
            "m3_streaming_health.talas.json",
            "m3_matrix4_output4.talas.json",
            "m3_live_spectral_analysis.talas.json",
            "c10_midi_mapper_modes.talas.json",
            "c200_polyphonic_midi_mapper_input.talas.json",
            "c11_disk_cache_capture.talas.json",
            "c13_bounded_disk_recorder.talas.json",
            "c14_bounded_analog_sequencer.talas.json",
            "c29_bounded_synthetic_spectrum_from_sounds.talas.json",
            "c30_bounded_warped_time_index.talas.json",
            "c31_bounded_spectrum_on_disk.talas.json",
            "c32_bounded_ga_oscillators.talas.json",
            "c33_bounded_generic_source_ram_sample.talas.json",
            "c35_bounded_multisample.talas.json",
            "c36_bounded_key_mapped_multisample.talas.json",
            "c37_bounded_multifile_disk_player.talas.json",
            "c38_bounded_memory_writer.talas.json",
            "c39_bounded_raw_midi_output.talas.json",
            "c40_bounded_midi_controller_output.talas.json",
            "c41_bounded_midi_note_output.talas.json",
            "c42_bounded_midi_file_echo.talas.json",
            "c43_bounded_global_controller_output.talas.json",
            "c44_dynamic_control_utilities.talas.json",
            "c45_dynamic_slew_times.talas.json",
            "c46_dynamic_clock_parameters.talas.json",
            "c47_dynamic_clock_divider_parameters.talas.json",
            "c48_dynamic_global_controller_fallback.talas.json",
            "c49_live_channeller_selectors.talas.json",
            "c50_live_spectrum_modifier_selectors.talas.json",
            "c51_live_spectrum_modifier_seed.talas.json",
            "c52_live_spectrum_modifier_hysteresis.talas.json",
            "c53_spectrum_modifier_probability_stream.talas.json",
            "c54_spectrum_modifier_track_probability_profile.talas.json",
            "c55_spectrum_modifier_remainder_probability_profile.talas.json",
            "c56_spectrum_on_disk_frame_scan.talas.json",
            "c57_spectrum_voiced_unvoiced_transition.talas.json",
            "c58_spectrum_in_ram_normalized_time_index.talas.json",
            "c59_spectrum_in_ram_default_analysis_metadata.talas.json",
            "c60_spectrum_on_disk_default_analysis_metadata.talas.json",
            "c61_sum_of_sines_default_analysis_frequencies.talas.json",
            "c62_sum_of_sines_controlled_time_frames.talas.json",
            "c63_sum_of_sines_automatic_gate_loop.talas.json",
            "c64_synthetic_spectrum_array_prefix.talas.json",
            "c65_synthetic_spectrum_static_expressions.talas.json",
            "c66_threshold_hysteresis_gate.talas.json",
            "c67_time_control_half_speed.talas.json",
            "c68_time_stopper_sample_hold.talas.json",
            "c69_time_offset_delay_line.talas.json",
            "c70_triggered_sample_and_hold_edges.talas.json",
            "c71_triggered_table_read_gate_reset.talas.json",
            "c72_tunable_vocoder_bank_reset.talas.json",
            "c73_two_formant_pair_reset.talas.json",
            "c74_sound_variable_selector_reset.talas.json",
            "c75_fixed_vocoder_bank_reset.talas.json",
            "c76_vocoder_channel_bank_reset.talas.json",
            "c77_wait_until_buffered_resume.talas.json",
            "c78_warped_time_index_restart.talas.json",
            "c79_waveshaper_polynomial_curve.talas.json",
            "c80_iterated_waveshaper_feedback.talas.json",
            "c81_multiplying_waveshaper_product.talas.json",
            "c82_input_output_characteristic_curve.talas.json",
            "c83_scale_and_offset_affine_control.talas.json",
            "c84_interpolate_dual_lane_control.talas.json",
            "c85_running_max_reset_control.talas.json",
            "c86_running_min_reset_baseline.talas.json",
            "c87_rms_squared_envelope.talas.json",
            "c88_amplitude_follower_envelope.talas.json",
            "c89_peak_detector_envelope.talas.json",
            "c90_frequency_tracker_history.talas.json",
            "c91_ar_envelope_state.talas.json",
            "c92_adsr_typed_gate_state.talas.json",
            "c93_pulse_train_phase_state.talas.json",
            "c94_pulse_generator_phase_state.talas.json",
            "c95_oscillator_phase_state.talas.json",
            "c96_lfo_phase_transport.talas.json",
            "c97_ramp_trigger_transport.talas.json",
            "c98_sample_and_hold_clock_transport.talas.json",
            "c99_samples_from_disk_single_step_transport.talas.json",
            "c100_phase_shift_by_90_allpass_transport.talas.json",
            "c101_single_side_band_rm_quadrature_transport.talas.json",
            "c102_sos_oscillators_selected_wavetable_transport.talas.json",
            "c103_low_pass_gate_control_state_transport.talas.json",
            "c104_reverb_section_fractional_delay_transport.talas.json",
            "c105_monotonizer_known_pitch_transport.talas.json",
            "c106_multifile_disk_player_selection_transport.talas.json",
            "c110_sample_cloud_dynamic_controls.talas.json",
            "c111_cloud_bank_checked_spectrum.talas.json",
            "c112_cloud_bank_element_range_cascade_delay.talas.json",
            "c113_grain_cloud_dynamic_controls.talas.json",
            "c114_chopper_dynamic_durations.talas.json",
            "c115_function_generator_trigger_duration.talas.json",
            "c116_multislope_connected_controls.talas.json",
            "c117_multisegment_loop_connected_controls.talas.json",
            "c118_noise_modes_connected_controls.talas.json",
            "c119_absolute_value_connected_magnitude.talas.json",
            "c120_arctan_connected_curve.talas.json",
            "c121_equality_connected_comparison.talas.json",
            "c122_set_range_connected_affine_mapping.talas.json",
            "c123_product_connected_finite_multiplication.talas.json",
            "c124_difference_connected_finite_subtraction.talas.json",
            "c125_sqrt_magnitude_connected_saturated_norm.talas.json",
            "c126_vca_connected_finite_gain.talas.json",
            "c127_constant_finite_event_value_broadcast.talas.json",
            "c128_add_finite_event_value_sum.talas.json",
            "c129_mixer_checked_finite_weighted_sum.talas.json",
            "c130_level_checked_finite_stereo_gain.talas.json",
            "c131_channel_crosser_checked_finite_matrix.talas.json",
            "c132_channel_join_checked_finite_dual_lane.talas.json",
            "c133_channeller_checked_finite_live_selectors.talas.json",
            "c134_pan_checked_finite_live_controls.talas.json",
            "c135_crossfade_checked_finite_live_controls.talas.json",
            "c136_stereo_mix2_checked_live_controls.talas.json",
            "c137_stereo_mix4_checked_live_controls.talas.json",
            "c138_audio_output_exact_stereo_terminal.talas.json",
            "c139_averaging_low_pass_filter_dynamic_window.talas.json",
            "c140_delay_with_feedback_dynamic_controls.talas.json",
            "c141_set_duration_bounded_window.talas.json",
            "c142_warped_time_index_scheduled_morph.talas.json",
            "c143_amplitude_follower_scheduled_controls.talas.json",
            "c144_peak_detector_scheduled_controls.talas.json",
            "c145_rms_squared_scheduled_controls.talas.json",
            "c146_running_max_scheduled_reset.talas.json",
            "c147_running_min_scheduled_reset_baseline.talas.json",
            "c148_triggered_sample_and_hold_scheduled_edges.talas.json",
            "c149_triggered_table_read_paired_schedules.talas.json",
            "c150_function_generator_scheduled_controls.talas.json",
            "c151_multislope_scheduled_controls.talas.json",
            "c152_multisegment_scheduled_loop_controls.talas.json",
            "c153_graphical_envelope_scheduled_rate_controls.talas.json",
            "c154_adsr_scheduled_typed_gate_controls.talas.json",
            "c155_oscillator_scheduled_phase_controls.talas.json",
            "c156_chopper_scheduled_cycle_controls.talas.json",
            "c157_analog_sequencer_scheduled_controls.talas.json",
            "c158_ar_scheduled_controls.talas.json",
            "c159_analysis_filter_scheduled_controls.talas.json",
            "c160_phase_shift_by_90_scheduled_frequency.talas.json",
            "c161_single_side_band_rm_scheduled_quadrature.talas.json",
            "c162_sos_oscillators_static_spectrum_sine_bank.talas.json",
            "c163_low_pass_gate_scheduled_control.talas.json",
            "c164_morph_filter_scheduled_controls.talas.json",
            "c165_quad_function_generator_scheduled_controls.talas.json",
            "c166_probability_chaos_generator_scheduled_controls.talas.json",
            "c167_stereo_dispersive_resonator_scheduled_controls.talas.json",
            "c168_plucked_waveguide_scheduled_controls.talas.json",
            "c169_dynamic_breakpoint_oscillator_scheduled_controls.talas.json",
            "c170_wave_terrain_oscillator_scheduled_controls.talas.json",
            "c171_scanned_mass_oscillator_scheduled_controls.talas.json",
            "c172_diffuse_spectral_stretch_scheduled_controls.talas.json",
            "c173_dual_morph_oscillator_scheduled_controls.talas.json",
            "c174_logic_scheduled_fallbacks.talas.json",
            "c175_annotation_passthrough.talas.json",
            "c191_filter_scheduled_controls.talas.json",
            "sample_text_table.talas.json",
        };
        for (const auto filename : requiredExamples)
        {
            const auto path = examplePath(filename);
            require(
                std::filesystem::is_regular_file(path),
                "missing checked-in M3 example: " + path.string());
        }

        testRenderedChannelsRejectMalformedJson();
        testNativeFftRoundTrip();
        testPackedFftRoundTrip();
        testStreamingHealth();
        testMatrix4Output4();
        testLiveSpectralAnalysis();
        testBoundedMidiMapperModesStatus();
        testPolyphonicMidiMapperInputExampleStatus();
        testBoundedMidiMapperModesExecuteOfflineAndRealtime();
        testBoundedMidiMapperExampleRejectsFirstOverBoundJsonMap();
        testBoundedDiskCacheCaptureExample();
        testBoundedDiskRecorderExamplePublishesExactPreparedProfile();
        testBoundedAnalogSequencerExampleMatchesPreparedPartitions();
        testBoundedSyntheticSpectrumFromSoundsExampleMatchesPreparedPartitions();
        testBoundedWarpedTimeIndexExampleMatchesPreparedPartitions();
        testBoundedSpectrumOnDiskExampleMatchesPreparedPartitions();
        testBoundedGaOscillatorsExampleMatchesPreparedPartitions();
        testBoundedGenericSourceRamSampleExampleMatchesPreparedPartitions();
        testTextBackedSampleExampleMatchesPreparedPartitions();
        testBoundedMultisampleExampleMatchesPreparedPartitions();
        testBoundedKeyMappedMultisampleExampleMatchesPreparedPartitions();
        testBoundedMultifileDiskPlayerExampleMatchesPreparedPartitions();
        testBoundedMemoryWriterExampleMatchesPreparedPartitions();
        testBoundedRawMidiOutputExamplePublishesTerminalEvents();
        testBoundedMidiControllerOutputExamplePublishesTerminalEvents();
        testBoundedMidiNoteOutputExamplePublishesTerminalEvents();
        testBoundedMidiFileEchoExamplePublishesTerminalEvents();
        testBoundedGlobalControllerOutputExamplePublishesExactCapture();
        testDynamicControlUtilitiesExampleMatchesExactTimeline();
        testDynamicSlewTimesExampleMatchesExactTimeline();
        testDynamicClockParametersExampleMatchesExactTimeline();
        testDynamicClockDividerParametersExampleMatchesExactTimeline();
        testTimelineDrivenUtilityExamplesPlayInRealtime();
        testDynamicGlobalControllerFallbackExampleMatchesExactTimeline();
        testLiveChannellerSelectorsExampleMatchesExactTimeline();
        testLiveSpectrumModifierSelectorsExampleMatchesExactRender();
        testLiveSpectrumModifierSeedExampleMatchesExactRender();
        testLiveSpectrumModifierHysteresisExampleMatchesExactRender();
        testSpectrumModifierProbabilityStreamExampleMatchesPublicFrames();
        testSpectrumModifierTrackProbabilityProfileExampleMatchesExactRender();
        testSpectrumModifierRemainderProbabilityProfileExampleMatchesExactRender();
        testSpectrumOnDiskFrameScanExampleMatchesTransactionSchedule();
        testSpectrumVoicedUnvoicedTransitionExampleMatchesPublicFrames();
        testSpectrumInRamNormalizedTimeIndexExampleMatchesEndpoints();
        testC59SpectrumInRamDefaultAnalysisMetadataExample();
        testC60SpectrumOnDiskDefaultAnalysisMetadataExample();
        testC61SumOfSinesDefaultAnalysisFrequenciesExample();
        testC62SumOfSinesControlledTimeFramesExample();
        testC63SumOfSinesAutomaticGateLoopExample();
        testC64SyntheticSpectrumArrayPrefixExample();
        testC65SyntheticSpectrumStaticExpressionsExample();
        testC66ThresholdHysteresisGateExample();
        testC67TimeControlHalfSpeedExample();
        testC68TimeStopperSampleHoldExample();
        testC69TimeOffsetDelayLineExample();
        testC70TriggeredSampleAndHoldEdgesExample();
        testC71TriggeredTableReadGateResetExample();
        testC72TunableVocoderBankResetExample();
        testC73TwoFormantPairResetExample();
        testC74SoundVariableSelectorResetExample();
        testC75FixedVocoderBankResetExample();
        testC76VocoderChannelBankResetExample();
        testC77WaitUntilBufferedResumeExample();
        testC78WarpedTimeIndexRestartExample();
        testC79WaveshaperPolynomialCurveExample();
        testC80IteratedWaveshaperFeedbackExample();
        testC81MultiplyingWaveshaperProductExample();
        testC82InputOutputCharacteristicCurveExample();
        testC83ScaleAndOffsetAffineControlExample();
        testC84InterpolateDualLaneControlExample();
        testC85RunningMaxResetControlExample();
        testC86RunningMinResetBaselineExample();
        testC87RmsSquaredEnvelopeExample();
        testC88AmplitudeFollowerEnvelopeExample();
        testC89PeakDetectorEnvelopeExample();
        testC90FrequencyTrackerHistoryExample();
        testC91ArEnvelopeStateExample();
        testC92AdsrTypedGateStateExample();
        testC93PulseTrainPhaseStateExample();
        testC94PulseGeneratorPhaseStateExample();
        testC95OscillatorPhaseStateExample();
        testC96LfoPhaseTransportExample();
        testC97RampTriggerTransportExample();
        testC98SampleAndHoldClockTransportExample();
        testC99SamplesFromDiskSingleStepTransportExample();
        testC100PhaseShiftBy90AllpassTransportExample();
        testC101SingleSideBandRmQuadratureTransportExample();
        testC102SosOscillatorsSelectedWavetableTransportExample();
        testC103LowPassGateControlStateTransportExample();
        testC104ReverbSectionFractionalDelayTransportExample();
        testC105MonotonizerKnownPitchTransportExample();
        testC106MultifileDiskPlayerSelectionTransportExample();
        testC110SampleCloudDynamicControlsExample();
        testC111CloudBankCheckedSpectrumExample();
        testC112CloudBankElementRangeCascadeDelayExample();
        testC113GrainCloudDynamicControlsExample();
        testC114ChopperDynamicDurationsExample();
        testC115FunctionGeneratorTriggerDurationExample();
        testC116MultislopeConnectedControlsExample();
        testC117MultisegmentLoopConnectedControlsExample();
        testC118NoiseModesConnectedControlsExample();
        testC119AbsoluteValueConnectedMagnitudeExample();
        testC120ArcTanConnectedCurveExample();
        testC121EqualityConnectedComparisonExample();
        testC122SetRangeConnectedAffineMappingExample();
        testC123ProductConnectedFiniteMultiplicationExample();
        testC124DifferenceConnectedFiniteSubtractionExample();
        testC125SqrtMagnitudeConnectedSaturatedNormExample();
        testC126VcaConnectedFiniteGainExample();
        testC127ConstantFiniteEventValueBroadcastExample();
        testC128AddFiniteEventValueSumExample();
        testC129MixerCheckedFiniteWeightedSumExample();
        testC130LevelCheckedFiniteStereoGainExample();
        testC131ChannelCrosserCheckedFiniteMatrixExample();
        testC132ChannelJoinCheckedFiniteDualLaneExample();
        testC133ChannellerCheckedFiniteLiveSelectorsExample();
        testC134PanCheckedFiniteLiveControlsExample();
        testC135CrossfadeCheckedFiniteLiveControlsExample();
        testC136StereoMix2CheckedLiveControlsExample();
        testC137StereoMix4CheckedLiveControlsExample();
        testC138AudioOutputExactStereoTerminalExample();
        testC139AveragingLowPassFilterDynamicWindowExample();
        testC140DelayWithFeedbackDynamicControlsExample();
        testC141SetDurationBoundedWindowExample();
        testC142WarpedTimeIndexScheduledMorphExample();
        testC143AmplitudeFollowerScheduledControlsExample();
        testC144PeakDetectorScheduledControlsExample();
        testC145RmsSquaredScheduledControlsExample();
        testC146RunningMaxScheduledResetExample();
        testC147RunningMinScheduledResetBaselineExample();
        testC148TriggeredSampleAndHoldScheduledEdgesExample();
        testC149TriggeredTableReadPairedSchedulesExample();
        testC150FunctionGeneratorScheduledControlsExample();
        testC151MultislopeScheduledControlsExample();
        testC152MultisegmentScheduledLoopControlsExample();
        testC153GraphicalEnvelopeScheduledRateControlsExample();
        testC154AdsrScheduledTypedGateControlsExample();
        testC155OscillatorScheduledPhaseControlsExample();
        testC156ChopperScheduledCycleControlsExample();
        testC157AnalogSequencerScheduledControlsExample();
        testC158ArScheduledControlsExample();
        testC159AnalysisFilterScheduledControlsExample();
        testC160PhaseShiftBy90ScheduledFrequencyExample();
        testC161SingleSideBandRmScheduledQuadratureExample();
        testC162SosOscillatorsStaticSpectrumSineBankExample();
        testC163LowPassGateScheduledControlExample();
        testC164MorphFilterScheduledControlsExample();
        testC165QuadFunctionGeneratorScheduledControlsExample();
        testC166ProbabilityChaosGeneratorScheduledControlsExample();
        testC167StereoDispersiveResonatorScheduledControlsExample();
        testC168PluckedWaveguideScheduledControlsExample();
        testC169DynamicBreakpointOscillatorScheduledControlsExample();
        testC170WaveTerrainOscillatorScheduledControlsExample();
        testC171ScannedMassOscillatorScheduledControlsExample();
        testC172DiffuseSpectralStretchScheduledControlsExample();
        testC173DualMorphOscillatorScheduledControlsExample();
        testC174LogicScheduledFallbacksExample();
        testC175AnnotationPassThroughExample();
        testC191FilterScheduledControlsExample();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
