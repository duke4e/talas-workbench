#include "talas/cdp/AttackBoost.h"
#include "talas/cdp/Bounce.h"
#include "talas/cdp/Brassage.h"
#include "talas/cdp/Cascade.h"
#include "talas/cdp/DcShift.h"
#include "talas/cdp/Deglitch.h"
#include "talas/cdp/DistortEnvelope.h"
#include "talas/cdp/DistortHarmonic.h"
#include "talas/cdp/DistortMultiply.h"
#include "talas/cdp/DistortOmit.h"
#include "talas/cdp/DistortOverload.h"
#include "talas/cdp/DistortReform.h"
#include "talas/cdp/DistortReverse.h"
#include "talas/cdp/DistortShuffle.h"
#include "talas/cdp/Echo.h"
#include "talas/cdp/EnvelopeWarp.h"
#include "talas/cdp/FastConvolution.h"
#include "talas/cdp/FixedFilter.h"
#include "talas/cdp/LohiFilter.h"
#include "talas/cdp/LoudnessGain.h"
#include "talas/cdp/LowerResolution.h"
#include "talas/cdp/NoiseCut.h"
#include "talas/cdp/Pan.h"
#include "talas/cdp/PhaseInvert.h"
#include "talas/cdp/Phasor.h"
#include "talas/cdp/Phasing.h"
#include "talas/cdp/PitchedDelay.h"
#include "talas/cdp/Quantise.h"
#include "talas/cdp/Rejoin.h"
#include "talas/cdp/Reverb.h"
#include "talas/cdp/RingModulate.h"
#include "talas/cdp/RoomReverb.h"
#include "talas/cdp/SilenceGate.h"
#include "talas/cdp/Spin.h"
#include "talas/cdp/StadiumEcho.h"
#include "talas/cdp/SubmixPairs.h"
#include "talas/cdp/SweepingFilter.h"
#include "talas/cdp/TapDelay.h"
#include "talas/cdp/Tremolo.h"
#include "talas/cdp/TwoInput.h"
#include "talas/cdp/UserBank.h"
#include "talas/cdp/VariableFilter.h"
#include "talas/cdp/VaryingBank.h"
#include "talas/cdp/VaryingDelay.h"
#include "talas/cdp/WavesetLocal.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

// The CDP nodes (ADR-0018) play their talas_cdp ports offline and in realtime
// blocks alike, and the Lower Resolution latency aligns with other branches.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;
constexpr double sampleRate = 48000.0;
constexpr std::size_t frames = 2048;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Parameter expression(std::string id, std::string text)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

talas::project::ProjectNode node(
    std::string id, std::string type, std::vector<Parameter> parameters = {})
{
    return {std::move(id), std::move(type), std::move(parameters)};
}

talas::project::ProjectDocument graph(
    std::vector<talas::project::ProjectNode> nodes,
    std::vector<talas::core::Connection> connections)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = std::move(nodes);
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.connections = std::move(connections);
    return document;
}

talas::project::ProjectNode tone(std::string id, std::string frequency)
{
    return node(std::move(id), "talas.oscillator",
        {expression("frequency", std::move(frequency)), expression("amplitude", "0.9")});
}

// The right channel of the last render, empty when it copied the left.
std::vector<double> lastRight;

// The left channel rendered offline, after requiring realtime blocks of 64
// frames to match both channels exactly.
std::vector<double> render(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings = {sampleRate, 64},
    std::size_t frameCount = frames)
{
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "CDP graph should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(offline.ok(), "CDP graph should render offline: "
        + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "CDP graph should prepare for realtime: " + std::string{session.error()});
    std::vector<double> left(frameCount);
    std::vector<double> right(frameCount);
    const std::vector<float> silence(settings.maxBlockFrames, 0.0F);
    const float* const channels[] = {silence.data()};
    for (std::size_t offset = 0; offset < frameCount; offset += settings.maxBlockFrames)
    {
        const auto count = std::min<std::size_t>(settings.maxBlockFrames, frameCount - offset);
        require(session.render({channels, 1},
                    std::span{left}.subspan(offset, count),
                    std::span{right}.subspan(offset, count)),
            "CDP graph should render in realtime: " + std::string{session.error()});
    }
    require(left == offline.mono
            && (offline.right.empty() ? right == left : right == offline.right),
        "CDP graph should play alike offline and in realtime");
    lastRight = offline.right;
    return offline.mono;
}

bool fails(const talas::project::ProjectDocument& document)
{
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {sampleRate, 64});
    if (!compiled.ok())
        return true;
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    return !talas::runtime::renderOffline(*compiled.graph, frames).ok() && !session.ready();
}

// A CDP8 reference in tests/cdp/fixtures: its input and its first output
// channel.
struct CdpReference
{
    std::vector<double> input;
    std::vector<double> output;
};

CdpReference cdpReference(const std::string& name)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    CdpReference result;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        char* end = nullptr;
        result.input.push_back(std::strtod(line.c_str(), &end));
        result.output.push_back(std::strtod(end, &end));
    }
    require(!result.input.empty(), "CDP reference " + name + " should hold samples");
    return result;
}

// A CDP node's field following an Event Value that changes at a frame.
talas::runtime::EventValueChange eventValueChange(std::uint64_t frame, std::string name, double value)
{
    talas::runtime::EventValueChange change;
    change.frame = frame;
    change.name = std::move(name);
    change.value = value;
    return change;
}

// The node's Input played as CDP8's input, offline.
std::vector<double> renderCdpInput(
    const talas::project::ProjectDocument& document, talas::runtime::RenderSettings settings, const CdpReference& cdp)
{
    settings.audioInputChannels.push_back({1, cdp.input});
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "CDP graph should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, cdp.input.size());
    require(offline.ok(), "CDP graph should render offline: "
        + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
    return offline.mono;
}

// The node's Input played as CDP8's input, offline, then silence for its
// latency; what follows the latency, to `compared` frames.
std::vector<double> renderCdpInputLagging(const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings, CdpReference cdp, std::size_t latency, std::size_t compared)
{
    cdp.input.resize(cdp.input.size() + latency, 0.0);
    const auto played = renderCdpInput(document, std::move(settings), cdp);
    require(std::all_of(played.begin(), played.begin() + static_cast<std::ptrdiff_t>(latency),
                [](double sample) { return sample == 0.0; }),
        "a CDP distortion should start with its latency's silence");
    return std::vector<double>(played.begin() + static_cast<std::ptrdiff_t>(latency),
        played.begin() + static_cast<std::ptrdiff_t>(latency + compared));
}

std::vector<double> dryTone(std::size_t frameCount = frames)
{
    return render(graph({tone("osc", "441 hz")}, {{"osc", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, frameCount);
}

void testLowerResolutionPlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("lowBit", "talas.cdpLowerResolution",
                {expression("bitResolution", "5"), expression("sampleRateDivision", "4")})},
        {{"osc", "audioOut", "lowBit", "audioIn"}, {"lowBit", "audioOut", "out", "audioIn"}}));
    talas::cdp::LowerResolution port;
    require(port.prepare(5, 4), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Lower Resolution should play its talas_cdp port");
}

// Mixed with its dry input, the held group means stay aligned: the dry branch
// is delayed by the node's latency of one group less a frame.
void testLowerResolutionLatencyAlignsTheDryBranch()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("lowBit", "talas.cdpLowerResolution",
                {expression("bitResolution", "4"), expression("sampleRateDivision", "8")}),
            node("mix", "talas.mixerV2")},
        {{"osc", "audioOut", "lowBit", "audioIn"},
            {"osc", "audioOut", "mix", "inputs"},
            {"lowBit", "audioOut", "mix", "inputs"},
            {"mix", "leftOut", "out", "audioIn"}}));
    talas::cdp::LowerResolution port;
    require(port.prepare(4, 8) && port.latencyFrames() == 7, "the port should lag 7 frames");
    std::vector<double> reduced(frames);
    require(port.process(dry, reduced), "the port should process");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto delayed = frame >= 7 ? dry[frame - 7] : 0.0;
        require(std::abs(played[frame] - (delayed + reduced[frame])) <= 1.0e-12,
            "frame " + std::to_string(frame) + " should add the aligned dry and reduced signals");
    }
}

void testLowerResolutionFollowsItsBitResolution()
{
    auto document = graph(
        {tone("osc", "441 hz"),
            node("lowBit", "talas.cdpLowerResolution",
                {expression("bitResolution", "!Bits"), expression("sampleRateDivision", "1")})},
        {{"osc", "audioOut", "lowBit", "audioIn"}, {"lowBit", "audioOut", "out", "audioIn"}});
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues.push_back({"Bits", 12.0});
    talas::runtime::EventValueChange change;
    change.frame = 1024;
    change.name = "Bits";
    change.value = 3.0;
    settings.eventValueChanges.push_back(change);
    // Offline applies the scheduled change at its frame.
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "a live bit resolution should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    require(offline.ok(), "a live bit resolution should render offline");
    const auto dry = dryTone();
    talas::cdp::LowerResolution port;
    require(port.prepare(12, 1), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(std::span{dry}.first(1024), std::span{expected}.first(1024))
            && port.setBitResolution(3)
            && port.process(std::span{dry}.subspan(1024), std::span{expected}.subspan(1024)),
        "the port should follow the change");
    require(offline.mono == expected, "CDP Lower Resolution should follow its bit resolution");
}

void testRingModulatePlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("ring", "talas.cdpRingModulate", {expression("frequency", "250 hz")})},
        {{"osc", "audioOut", "ring", "audioIn"}, {"ring", "audioOut", "out", "audioIn"}}));
    talas::cdp::RingModulate port;
    require(port.prepare(sampleRate), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, std::nullopt, 250.0, expected), "the port should process");
    require(played == expected, "CDP Ring Modulate should play its talas_cdp port");
}

void testRingModulateTakesAConnectedFrequency()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("frequency", "talas.constant", {expression("value", "730")}),
            node("ring", "talas.cdpRingModulate")},
        {{"osc", "audioOut", "ring", "audioIn"},
            {"frequency", "value", "ring", "frequency"},
            {"ring", "audioOut", "out", "audioIn"}}));
    talas::cdp::RingModulate port;
    require(port.prepare(sampleRate), "the port should prepare");
    std::vector<double> expected(frames);
    const std::vector<double> frequency(frames, 730.0);
    require(port.process(dry, frequency, 0.0, expected), "the port should process");
    require(played == expected, "a connected frequency should drive CDP Ring Modulate");
}

void testTremoloPlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("tremolo", "talas.cdpTremolo",
                {expression("frequency", "6 hz"), expression("depth", "0.7"),
                    expression("gain", "0.8")})},
        {{"osc", "audioOut", "tremolo", "audioIn"}, {"tremolo", "audioOut", "out", "audioIn"}}));
    talas::cdp::Tremolo port;
    require(port.prepare(sampleRate), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, 6.0, 0.7, 0.8, expected), "the port should process");
    require(played == expected, "CDP Tremolo should play its talas_cdp port");
}

void testPanPlaysItsPortInStereo()
{
    const auto dry = dryTone();
    const auto left = render(graph(
        {tone("osc", "441 hz"),
            node("pan", "talas.cdpPan",
                {expression("position", "-0.4"), expression("prescale", "0.9")})},
        {{"osc", "audioOut", "pan", "audioIn"},
            {"pan", "leftOut", "out", "audioIn"},
            {"pan", "rightOut", "out", "rightIn"}}));
    talas::cdp::Pan port;
    std::vector<double> expectedLeft(frames);
    std::vector<double> expectedRight(frames);
    require(port.process(dry, -0.4, 0.9, expectedLeft, expectedRight), "the port should process");
    require(left == expectedLeft && lastRight == expectedRight,
        "CDP Pan should play its talas_cdp port on both channels");
}

void testTapDelayPlaysItsPortInStereo()
{
    const auto dry = dryTone();
    const auto taps = std::string{"0.001 0.8 -0.6\n0.0023 0.6 0.7\n0.004 0.4 0"};
    Parameter tapText;
    tapText.parameterId = "taps";
    tapText.text = taps;
    const auto left = render(graph(
        {tone("osc", "441 hz"),
            node("delay", "talas.cdpTapDelay",
                {tapText, expression("tapGain", "0.3"), expression("feedback", "0.4"),
                    expression("mix", "0.25")})},
        {{"osc", "audioOut", "delay", "audioIn"},
            {"delay", "leftOut", "out", "audioIn"},
            {"delay", "rightOut", "out", "rightIn"}}));
    const auto list = talas::cdp::readDelayTaps(taps, sampleRate);
    talas::cdp::TapDelay port;
    require(list.has_value() && port.prepare(sampleRate, *list, 0.3, 0.4, 0.25),
        "the port should prepare");
    std::vector<double> expectedLeft(frames);
    std::vector<double> expectedRight(frames);
    require(port.process(dry, expectedLeft, expectedRight), "the port should process");
    require(left == expectedLeft && lastRight == expectedRight && left != expectedRight,
        "CDP Tap Delay should play its talas_cdp port on both channels");
}

void testEchoPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter invert;
    invert.parameterId = "invert";
    invert.boolean = true;
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("echo", "talas.cdpEcho",
                {expression("delay", "3 ms"), expression("mix", "0.6"), expression("feedback", "0.7"),
                    expression("prescale", "0.8"), invert})},
        {{"osc", "audioOut", "echo", "audioIn"}, {"echo", "audioOut", "out", "audioIn"}}));
    talas::cdp::Echo port;
    require(port.prepare(sampleRate, 3.0, 0.6, 0.7, 0.8, true), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Echo should play its talas_cdp port");
}

void testVaryingDelayPlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("delay", "talas.cdpVaryingDelay",
                {expression("delay", "4 ms"), expression("mix", "0.6"), expression("feedback", "0.5"),
                    expression("modulation", "0.7"), expression("lfoFrequency", "-20 hz"),
                    expression("lfoPhase", "0.3"), expression("seed", "9")})},
        {{"osc", "audioOut", "delay", "audioIn"}, {"delay", "audioOut", "out", "audioIn"}}));
    talas::cdp::VaryingDelaySettings settings;
    settings.delayMilliseconds = 4.0;
    settings.mix = 0.6;
    settings.feedback = 0.5;
    settings.modulation = 0.7;
    settings.lfoFrequency = -20.0;
    settings.lfoPhase = 0.3;
    settings.seed = 9;
    talas::cdp::VaryingDelay port;
    require(port.prepare(sampleRate, settings), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Varying Delay should play its talas_cdp port");
}

void testReverbPlaysItsPortInStereo()
{
    const auto dry = dryTone();
    const auto left = render(graph(
        {tone("osc", "441 hz"),
            node("reverb", "talas.cdpReverb",
                {expression("reverbGain", "0.7"), expression("mix", "0.4"), expression("reverbTime", "0.4 s"),
                    expression("damping", "0.3"), expression("predelay", "5 ms")})},
        {{"osc", "audioOut", "reverb", "audioIn"},
            {"reverb", "leftOut", "out", "audioIn"},
            {"reverb", "rightOut", "out", "rightIn"}}));
    talas::cdp::ReverbSettings settings;
    settings.reverbGain = 0.7;
    settings.mix = 0.4;
    settings.reverbTime = 0.4;
    settings.damping = 0.3;
    settings.predelayMilliseconds = 5.0;
    talas::cdp::Reverb port;
    require(port.prepare(sampleRate, settings), "the port should prepare");
    std::vector<double> expectedLeft(frames);
    std::vector<double> expectedRight(frames);
    require(port.process(dry, expectedLeft, expectedRight), "the port should process");
    require(left == expectedLeft && lastRight == expectedRight && left != expectedRight,
        "CDP Reverb should play its talas_cdp port on both channels");
}

void testVariableFilterPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "highPass";
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("filter", "talas.cdpVariableFilter",
                {mode, expression("acuity", "0.3"), expression("gain", "0.9"), expression("frequency", "700 hz")})},
        {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}}));
    talas::cdp::VariableFilter port;
    require(port.prepare(sampleRate, talas::cdp::VariableFilter::Mode::HighPass, 0.3, 0.9, 700.0),
        "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Variable Filter should play its talas_cdp port");
}

// CDP's filter variable takes its acuity and frequency as breakpoint files
// ("ACUITY and FRQ can vary over time") and reads them at the start of each
// 128-frame block: fields following Event Values play as CDP8 plays
// breakpoints stepping at those blocks, the acuity a block before its
// breakpoint, as CDP steps its Q.
void testVariableFilterFollowsItsAcuityAndFrequency()
{
    const auto cdp = cdpReference("filter-variable-2-band-pass-live-acuity-and-frequency");
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "bandPass";
    const auto document = graph({node("in", "talas.audioInput"),
                                    node("filter", "talas.cdpVariableFilter",
                                        {mode, expression("acuity", "!Acuity"), expression("gain", "0.8"),
                                            expression("frequency", "!Frequency * (1 hz)")})},
        {{"in", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Acuity", 0.2}, {"Frequency", 600.0}};
    settings.eventValueChanges = {eventValueChange(512, "Acuity", 0.05), eventValueChange(768, "Frequency", 2400.0)};
    require(renderCdpInput(document, settings, cdp) == cdp.output,
        "CDP Variable Filter should follow its acuity and frequency as CDP8 follows breakpoints");
}

// CDP's filter sweeping takes its acuity and low, high and sweep
// frequencies as breakpoint files ("ACUITY, LOFRQ, HIFRQ and SWEEPFRQ may
// all vary over time"), read at the start of each 128-frame block.
void testSweepingFilterFollowsItsAcuityAndFrequencies()
{
    const auto cdp = cdpReference("filter-sweeping-2-band-pass-live-acuity-and-frequencies");
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "bandPass";
    const auto document = graph({node("in", "talas.audioInput"),
                                    node("filter", "talas.cdpSweepingFilter",
                                        {mode, expression("acuity", "!Acuity"), expression("gain", "1"),
                                            expression("lowFrequency", "!Low * (1 hz)"),
                                            expression("highFrequency", "!High * (1 hz)"),
                                            expression("sweepFrequency", "!Sweep * (1 hz)"), expression("phase", "0.25")})},
        {{"in", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Acuity", 0.1}, {"Low", 300.0}, {"High", 3000.0}, {"Sweep", 20.0}};
    settings.eventValueChanges = {eventValueChange(512, "Acuity", 0.03), eventValueChange(768, "Low", 500.0),
        eventValueChange(1024, "High", 2000.0), eventValueChange(1280, "Sweep", 60.0)};
    require(renderCdpInput(document, settings, cdp) == cdp.output,
        "CDP Sweeping Filter should follow its acuity and frequencies as CDP8 follows breakpoints");
}

// CDP's userbank and varibank take their Q as a breakpoint file ("Q may vary
// over time"), read at the start of each 128-frame block.
void testFilterBanksFollowTheirQ()
{
    const auto played = [](std::string type, std::string bands, std::string fixture, double startQ, double endQ,
                            std::uint64_t frame) {
        const auto cdp = cdpReference(fixture);
        Parameter pitches;
        pitches.parameterId = "pitches";
        pitches.choice = "hertz";
        Parameter text;
        text.parameterId = "bands";
        text.text = std::move(bands);
        const auto document = graph({node("in", "talas.audioInput"),
                                        node("bank", std::move(type),
                                            {pitches, text, expression("q", "!Q"), expression("gain", "1")})},
            {{"in", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Q", startQ}};
        settings.eventValueChanges = {eventValueChange(frame, "Q", endQ)};
        return renderCdpInput(document, settings, cdp) == cdp.output;
    };
    require(played("talas.cdpUserBank", "440 1\n1200 0.5\n2600 0.8", "filter-userbank-1-live-q", 20.0, 5.0, 768),
        "CDP User Bank should follow its Q as CDP8 follows a breakpoint file");
    require(played("talas.cdpVaryingBank", "0 300 1 1500 0.5\n0.03 500 1 1200 0.7", "filter-varibank-1-live-q", 30.0, 8.0,
                1024),
        "CDP Varying Bank should follow its Q as CDP8 follows a breakpoint file");
}

// CDP's filter phasing takes its delay as a breakpoint file ("DELAY may vary
// over time"), read at each frame and rounded to whole frames.
void testPhasingFollowsItsDelay()
{
    for (const auto& [fixture, choice, first, second, frame] :
        {std::tuple{std::string{"filter-phasing-1-live-delay"}, std::string{"phaseShift"}, 0.002, 0.004, 800},
            std::tuple{std::string{"filter-phasing-2-phaser-live-delay"}, std::string{"phaser"}, 0.003, 0.0015, 1024}})
    {
        const auto cdp = cdpReference(fixture);
        Parameter mode;
        mode.parameterId = "mode";
        mode.choice = choice;
        const auto document = graph({node("in", "talas.audioInput"),
                                        node("phasing", "talas.cdpPhasing",
                                            {mode, expression("gain", "0.6"), expression("delay", "!Delay * (1 s)"),
                                                expression("prescale", "0.9")})},
            {{"in", "audioOut", "phasing", "audioIn"}, {"phasing", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Delay", first}};
        settings.eventValueChanges = {eventValueChange(static_cast<std::uint64_t>(frame), "Delay", second)};
        require(renderCdpInput(document, settings, cdp) == cdp.output,
            "CDP Phasing should follow its delay as CDP8 follows breakpoints: " + fixture);
    }
}

// CDP's revecho takes its Mix and Feedback as breakpoint files and then
// plays them as its domultiv, read every 128 frames.
void testEchoFollowsItsMixAndFeedback()
{
    const auto cdp = cdpReference("modify-revecho-1-live-mix-and-feedback");
    Parameter invert;
    invert.parameterId = "invert";
    invert.boolean = false;
    const auto document = graph({node("in", "talas.audioInput"),
                                    node("echo", "talas.cdpEcho",
                                        {expression("delay", "7 ms"), expression("mix", "!Mix"),
                                            expression("feedback", "!Feedback"), expression("prescale", "0.8"), invert})},
        {{"in", "audioOut", "echo", "audioIn"}, {"echo", "audioOut", "out", "audioIn"}});
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Mix", 0.4}, {"Feedback", 0.3}};
    settings.eventValueChanges = {eventValueChange(640, "Mix", 0.8), eventValueChange(1152, "Feedback", -0.5)};
    require(renderCdpInput(document, settings, cdp) == cdp.output,
        "CDP Echo should follow its Mix and Feedback as CDP8 follows breakpoints");
}

// CDP's distortions take their counts, and distort envel its trough and
// exponent, as breakpoint files ("may vary over time"), read, counts
// rounded, where each half-cycle or group starts.
void testDistortionsFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode distort,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(distort)},
            {{"in", "audioOut", "distort", "audioIn"}, {"distort", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        // The last group CDP leaves or envelopes as its file ends.
        const std::size_t compared = 1984;
        return renderCdpInputLagging(document, settings, cdp, latency, compared)
            == std::vector<double>(cdp.output.begin(), cdp.output.begin() + static_cast<std::ptrdiff_t>(compared));
    };
    require(played("distort-multiply-live-factor", node("distort", "talas.cdpDistortMultiply", {expression("factor", "!Factor")}),
                {{"Factor", 2.0}}, {eventValueChange(1024, "Factor", 5.0)}, talas::cdp::DistortMultiply::latencyFrames),
        "CDP Distort Multiply should follow its factor as CDP8 follows a breakpoint file");
    Parameter interpolate;
    interpolate.parameterId = "interpolate";
    interpolate.boolean = true;
    require(played("distort-divide-live-factor-interpolated",
                node("distort", "talas.cdpDistortDivide", {expression("factor", "!Factor"), interpolate}), {{"Factor", 3.0}},
                {eventValueChange(1280, "Factor", 2.0)}, talas::cdp::DistortDivide::latencyFrames),
        "CDP Distort Divide should follow its factor as CDP8 follows a breakpoint file");
    require(played("distort-omit-live-a-of-5",
                node("distort", "talas.cdpDistortOmit", {expression("omit", "!Omit"), expression("outOf", "5")}),
                {{"Omit", 1.0}}, {eventValueChange(896, "Omit", 3.0)}, 0),
        "CDP Distort Omit should follow its Omit as CDP8 follows a breakpoint file");
    require(played("distort-reverse-live-cycles", node("distort", "talas.cdpDistortReverse", {expression("cycles", "!Cycles")}),
                {{"Cycles", 1.0}}, {eventValueChange(768, "Cycles", 4.0)}, talas::cdp::DistortReverse::latencyFrames),
        "CDP Distort Reverse should follow its cycles as CDP8 follows a breakpoint file");
    Parameter rising;
    rising.parameterId = "shape";
    rising.choice = "rising";
    require(played("distort-envel-1-live-cycles-trough-exponent",
                node("distort", "talas.cdpDistortEnvelope",
                    {rising, expression("cycles", "!Cycles"), expression("trough", "!Trough"),
                        expression("exponent", "!Exponent")}),
                {{"Cycles", 2.0}, {"Trough", 0.2}, {"Exponent", 1.5}},
                {eventValueChange(704, "Cycles", 3.0), eventValueChange(1088, "Trough", 0.6),
                    eventValueChange(1280, "Exponent", 3.0)},
                talas::cdp::DistortEnvelope::latencyFrames),
        "CDP Distort Envelope should follow its fields as CDP8 follows breakpoint files");
}

// CDP's distort overload takes its depth and waveform frequency, shuffle its
// cycles, reform its exaggeration and fractal its scale and loudness as
// breakpoint files: overload reads them every 0.01 s, shuffle, rounded,
// where each domain starts, reform at each half-cycle's midpoint and
// fractal, the scale rounded, where each group starts.
void testOverloadShuffleReformAndFractalFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode distort,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency,
                            std::size_t compared) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(distort)},
            {{"in", "audioOut", "distort", "audioIn"}, {"distort", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, compared)
            == std::vector<double>(cdp.output.begin(), cdp.output.begin() + static_cast<std::ptrdiff_t>(compared));
    };
    Parameter noise;
    noise.parameterId = "mode";
    noise.choice = "noise";
    require(played("distort-overload-1-noise-live-depth",
                node("distort", "talas.cdpDistortOverload",
                    {noise, expression("clipLevel", "0.3"), expression("depth", "!Depth")}),
                {{"Depth", 0.7}}, {eventValueChange(704, "Depth", 0.2)}, 0, 2048),
        "CDP Distort Overload's noise should follow its depth as CDP8 follows a breakpoint file");
    Parameter waveform;
    waveform.parameterId = "mode";
    waveform.choice = "waveform";
    require(played("distort-overload-2-waveform-live-depth-and-frequency",
                node("distort", "talas.cdpDistortOverload",
                    {waveform, expression("clipLevel", "0.25"), expression("depth", "!Depth"),
                        expression("frequency", "!Frequency * (1 hz)")}),
                {{"Depth", 0.5}, {"Frequency", 1000.0}},
                {eventValueChange(704, "Depth", 1.0), eventValueChange(1152, "Frequency", 300.0)}, 0, 2048),
        "CDP Distort Overload's waveform should follow its depth and frequency as CDP8 follows breakpoint files");
    Parameter shuffle;
    shuffle.parameterId = "domainImage";
    shuffle.text = "abc-bca";
    // The recorded input ends where CDP's output does, so the domain from
    // frame 1635 never ends here.
    require(played("distort-shuffle-live-cycles",
                node("distort", "talas.cdpDistortShuffle", {shuffle, expression("cycles", "!Cycles")}), {{"Cycles", 1.0}},
                {eventValueChange(832, "Cycles", 3.0)}, talas::cdp::DistortShuffle::latencyFrames, 1635),
        "CDP Distort Shuffle should follow its cycles as CDP8 follows a breakpoint file");
    Parameter exaggerate;
    exaggerate.parameterId = "mode";
    exaggerate.choice = "exaggerate";
    // CDP leaves its last half-cycle, the frames from 12237, as they are.
    require(played("distort-reform-8-live-exaggeration-5376-frame-buffers",
                node("distort", "talas.cdpDistortReform", {exaggerate, expression("exaggeration", "!Exaggeration")}),
                {{"Exaggeration", 0.5}}, {eventValueChange(1024, "Exaggeration", 2.0)},
                talas::cdp::DistortReform::latencyFrames, 12237),
        "CDP Distort Reform should follow its exaggeration as CDP8 follows a breakpoint file");
    // CDP finishes its last group as its file ends.
    require(played("distort-fractal-live-scale-and-loudness",
                node("distort", "talas.cdpDistortFractal",
                    {expression("scale", "!Scale"), expression("loudness", "!Loudness"), expression("prescale", "0.8")}),
                {{"Scale", 2.0}, {"Loudness", 0.5}},
                {eventValueChange(768, "Scale", 3.0), eventValueChange(1216, "Loudness", 1.5)},
                talas::cdp::DistortFractal::latencyFrames, 1952),
        "CDP Distort Fractal should follow its scale and loudness as CDP8 follows breakpoint files");
}

// CDP's phase vocoder, `pvoc anal` then `pvoc synth`, and spec gain and gate
// between them, whose gain and threshold CDP takes as breakpoint files and
// reads at each analysis frame.
void testPhaseVocoderNodesPlayTheirPorts()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    require(played("pvoc-anal-synth-512-points-overlap-4",
                node("spectral", "talas.cdpPhaseVocoder", {expression("points", "512"), expression("overlap", "4")}), {},
                {}, 288),
        "CDP Phase Vocoder should play CDP8's pvoc anal and synth");
    require(played("spec-gain-live-gain", node("spectral", "talas.cdpSpectralGain", {expression("gain", "!Gain")}),
                {{"Gain", 0.5}}, {eventValueChange(1024, "Gain", 2.0)}, 1152),
        "CDP Spectral Gain should follow its gain as CDP8 follows a breakpoint file");
    require(played("spec-gate-live-threshold",
                node("spectral", "talas.cdpSpectralGate",
                    {expression("points", "256"), expression("overlap", "2"), expression("threshold", "!Threshold")}),
                {{"Threshold", 0.002}}, {eventValueChange(1344, "Threshold", 0.01)}, 576),
        "CDP Spectral Gate should follow its threshold as CDP8 follows a breakpoint file");
}

// CDP's focus, blur and hilite processes, whose fields CDP takes as
// breakpoint files and reads at each analysis frame.
void testSpectralProcessNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        // The default 1024 points and overlap 3 wait 1152 frames.
        return renderCdpInputLagging(document, settings, cdp, 1152, cdp.output.size()) == cdp.output;
    };
    require(played("focus-exag-live-exaggeration",
                node("spectral", "talas.cdpFocusExaggerate", {expression("exaggeration", "!Exaggeration")}),
                {{"Exaggeration", 2.0}}, {eventValueChange(1024, "Exaggeration", 0.5)}),
        "CDP Focus Exaggerate should follow its exaggeration as CDP8 follows a breakpoint file");
    require(played("focus-fold-live-frequencies",
                node("spectral", "talas.cdpFocusFold",
                    {expression("lowFrequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)")}),
                {{"Low", 500.0}, {"High", 1500.0}},
                {eventValueChange(1024, "Low", 350.0), eventValueChange(1344, "High", 1100.0)}),
        "CDP Focus Fold should follow its frequencies as CDP8 follows breakpoint files");
    require(played("focus-accu-live-decay-and-glis",
                node("spectral", "talas.cdpFocusAccumulate",
                    {expression("decay", "!Decay"), expression("glissando", "!Glissando")}),
                {{"Decay", 0.5}, {"Glissando", 0.0}},
                {eventValueChange(1024, "Decay", 0.95), eventValueChange(1344, "Glissando", 1.5)}),
        "CDP Focus Accumulate should follow its decay and glissando as CDP8 follows breakpoint files");
    require(played("blur-avrg-live-n", node("spectral", "talas.cdpBlurAverage", {expression("n", "!N")}), {{"N", 3.0}},
                {eventValueChange(1024, "N", 10.0)}),
        "CDP Blur Average should follow its N as CDP8 follows a breakpoint file");
    require(played("blur-suppress-live-n", node("spectral", "talas.cdpBlurSuppress", {expression("n", "!N")}),
                {{"N", 500.0}}, {eventValueChange(1024, "N", 8.0)}),
        "CDP Blur Suppress should follow its N as CDP8 follows a breakpoint file");
    Parameter between;
    between.parameterId = "mode";
    between.choice = "between";
    require(played("hilite-trace-4-live-n-and-band",
                node("spectral", "talas.cdpHiliteTrace",
                    {between, expression("n", "!N"), expression("lowFrequency", "!Low * (1 hz)"),
                        expression("highFrequency", "2000 hz")}),
                {{"N", 6.0}, {"Low", 400.0}}, {eventValueChange(1024, "N", 40.0), eventValueChange(1344, "Low", 900.0)}),
        "CDP Hilite Trace should follow its N and band as CDP8 follows breakpoint files");
}

// CDP's hilite filter, whose frequencies and Q CDP reads at each analysis
// frame through their logarithms, hilite pluck and focus step.
void testHiliteFilterPluckAndStepNodesPlayTheirPorts()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    Parameter band;
    band.parameterId = "mode";
    band.choice = "bandPassNormalised";
    require(played("hilite-filter-8-live-band",
                node("spectral", "talas.cdpHiliteFilter",
                    {band, expression("frequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)"),
                        expression("q", "150 hz")}),
                {{"Low", 400.0}, {"High", 1500.0}},
                {eventValueChange(1024, "Low", 700.0), eventValueChange(1344, "High", 2500.0)}, 1152),
        "CDP Hilite Filter should follow its band as CDP8 follows breakpoint files");
    Parameter notch;
    notch.parameterId = "mode";
    notch.choice = "notchGain";
    require(played("hilite-filter-12-notch-gain-live-q",
                node("spectral", "talas.cdpHiliteFilter",
                    {notch, expression("frequency", "300 hz"), expression("highFrequency", "2000 hz"),
                        expression("q", "!Q * (1 hz)"), expression("gain", "0.5")}),
                {{"Q", 80.0}}, {eventValueChange(1024, "Q", 400.0)}, 1152),
        "CDP Hilite Filter should follow its Q as CDP8 follows a breakpoint file");
    require(played("hilite-pluck-live-gain", node("spectral", "talas.cdpHilitePluck", {expression("gain", "!Gain")}),
                {{"Gain", 2.0}}, {eventValueChange(1024, "Gain", 6.0)}, 1152),
        "CDP Hilite Pluck should follow its gain as CDP8 follows a breakpoint file");
    require(played("focus-step-0.005-256-points",
                node("spectral", "talas.cdpFocusStep",
                    {expression("points", "256"), expression("overlap", "2"), expression("step", "0.005 s")}),
                {}, {}, 576),
        "CDP Focus Step should play CDP8's focus step");
}

// CDP's strange shift and repitch transpose, whose shift, frequencies and
// transposition CDP takes as breakpoint files and reads at each analysis
// frame.
void testStrangeShiftAndRepitchTransposeNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, 1152, cdp.output.size()) == cdp.output;
    };
    require(played("strange-shift-1-live-shift",
                node("spectral", "talas.cdpStrangeShift", {expression("shift", "!Shift * (1 hz)")}), {{"Shift", 100.0}},
                {eventValueChange(1024, "Shift", -150.0)}),
        "CDP Strange Shift should follow its shift as CDP8 follows a breakpoint file");
    Parameter below;
    below.parameterId = "mode";
    below.choice = "below";
    require(played("strange-shift-3-live-divide",
                node("spectral", "talas.cdpStrangeShift",
                    {below, expression("shift", "200 hz"), expression("frequency", "!Divide * (1 hz)")}),
                {{"Divide", 800.0}}, {eventValueChange(1344, "Divide", 1500.0)}),
        "CDP Strange Shift should follow its frequency as CDP8 follows a breakpoint file");
    Parameter ratio;
    ratio.parameterId = "units";
    ratio.choice = "ratio";
    require(played("repitch-transpose-1-live-ratio",
                node("spectral", "talas.cdpRepitchTranspose", {ratio, expression("transposition", "!Ratio")}),
                {{"Ratio", 1.5}}, {eventValueChange(1024, "Ratio", 0.75)}),
        "CDP Repitch Transpose should follow its transposition as CDP8 follows a breakpoint file");
    require(played("repitch-transpose-1-live-low-frequency",
                node("spectral", "talas.cdpRepitchTranspose",
                    {ratio, expression("transposition", "1.25"), expression("lowFrequency", "!Low * (1 hz)")}),
                {{"Low", 300.0}}, {eventValueChange(1024, "Low", 900.0)}),
        "CDP Repitch Transpose should follow its low frequency as CDP8 follows a breakpoint file");
    require(played("repitch-transpose-3-semitones-fuller",
                node("spectral", "talas.cdpRepitchTranspose", {expression("transposition", "5"), [] {
                    Parameter fuller;
                    fuller.parameterId = "fuller";
                    fuller.boolean = true;
                    return fuller;
                }()}),
                {}, {}),
        "CDP Repitch Transpose should play CDP8's fuller spectrum");
}

// CDP's strange waver, pitch transp, blur chorus and blur noise, whose
// fields CDP takes as breakpoint files.
void testWaverTranspChorusAndNoiseNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, 1152, cdp.output.size()) == cdp.output;
    };
    require(played("strange-waver-1-live-vibrato-and-stretch",
                node("spectral", "talas.cdpStrangeWaver",
                    {expression("vibrato", "!Vibrato * (1 hz)"), expression("stretch", "!Stretch"),
                        expression("bottomFrequency", "300 hz")}),
                {{"Vibrato", 40.0}, {"Stretch", 1.5}},
                {eventValueChange(1024, "Vibrato", 20.0), eventValueChange(1344, "Stretch", 2.5)}),
        "CDP Strange Waver should follow its vibrato and stretch as CDP8 follows breakpoint files");
    Parameter shiftUp;
    shiftUp.parameterId = "mode";
    shiftUp.choice = "shiftUp";
    require(played("pitch-transp-4-live-semitones",
                node("spectral", "talas.cdpPitchTransp",
                    {shiftUp, expression("split", "700 hz"), expression("transposition", "!Semitones")}),
                {{"Semitones", 3.0}}, {eventValueChange(1024, "Semitones", -4.0)}),
        "CDP Pitch Transp should follow its transposition as CDP8 follows a breakpoint file");
    Parameter upAndDown;
    upAndDown.parameterId = "mode";
    upAndDown.choice = "shiftUpAndDown";
    Parameter useDepth;
    useDepth.parameterId = "useDepth";
    useDepth.boolean = true;
    require(played("pitch-transp-6-live-depth",
                node("spectral", "talas.cdpPitchTransp",
                    {upAndDown, expression("split", "800 hz"), expression("transposition", "4"),
                        expression("transposition2", "-3"), useDepth, expression("depth", "!Depth")}),
                {{"Depth", 1.0}}, {eventValueChange(1024, "Depth", 0.4)}),
        "CDP Pitch Transp should follow its depth as CDP8 follows a breakpoint file");
    require(played("blur-chorus-1-live-aspread",
                node("spectral", "talas.cdpBlurChorus", {expression("amplitudeSpread", "!Spread")}), {{"Spread", 1.5}},
                {eventValueChange(1024, "Spread", 6.0)}),
        "CDP Blur Chorus should follow its amplitude spread as CDP8 follows a breakpoint file");
    require(played("blur-noise-live-noise", node("spectral", "talas.cdpBlurNoise", {expression("noise", "!Noise")}),
                {{"Noise", 0.3}}, {eventValueChange(1024, "Noise", 0.9)}),
        "CDP Blur Noise should follow its noise as CDP8 follows a breakpoint file");
}

// CDP's stretch spectrum, whose depth CDP takes as a breakpoint file, and
// blur scatter, whose keep and block size it does.
void testStretchSpectrumAndScatterNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    Parameter below;
    below.parameterId = "mode";
    below.choice = "below";
    Parameter useDepth;
    useDepth.parameterId = "useDepth";
    useDepth.boolean = true;
    require(played("stretch-spectrum-2-live-depth",
                node("spectral", "talas.cdpStretchSpectrum",
                    {expression("points", "512"), below, expression("divide", "2000 hz"), expression("maxStretch", "0.6"),
                        expression("exponent", "0.7"), useDepth, expression("depth", "!Depth")}),
                {{"Depth", 1.0}}, {eventValueChange(1024, "Depth", 0.3)}, 576),
        "CDP Stretch Spectrum should follow its depth as CDP8 follows a breakpoint file");
    require(played("blur-scatter-1-live-keep", node("spectral", "talas.cdpBlurScatter", {expression("keep", "!Keep")}),
                {{"Keep", 100.0}}, {eventValueChange(1024, "Keep", 30.0)}, 1152),
        "CDP Blur Scatter should follow its keep as CDP8 follows a breakpoint file");
    require(played("blur-scatter-2-live-blocksize",
                node("spectral", "talas.cdpBlurScatter",
                    {expression("keep", "10"), expression("blockSize", "!Block * (1 hz)")}),
                {{"Block", 117.1875}}, {eventValueChange(1024, "Block", 900.0)}, 1152),
        "CDP Blur Scatter should follow its block size as CDP8 follows a breakpoint file");
}

// CDP's hilite arpeg, whose rate, frequencies, bandwidth, amplification and
// sustain CDP takes as breakpoint files, and hilite greq and band, which read
// a data file.
void testHiliteArpegGreqAndBandNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto text = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto flag = [](std::string id) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = true;
        return parameter;
    };
    require(played("hilite-arpeg-1-sine-live-rate",
                node("spectral", "talas.cdpHiliteArpeg",
                    {expression("rate", "!Rate * (1 hz)"), expression("lowFrequency", "200 hz"),
                        expression("highFrequency", "2500 hz"), expression("bandwidth", "300 hz"),
                        expression("amplification", "2")}),
                {{"Rate", 20.0}}, {eventValueChange(1024, "Rate", 40.0)}, 1152),
        "CDP Hilite Arpeg should follow its rate as CDP8 follows a breakpoint file");
    require(played("hilite-arpeg-1-live-band-512-points",
                node("spectral", "talas.cdpHiliteArpeg",
                    {expression("points", "512"), choice("wave", "saw"), expression("rate", "25 hz"),
                        expression("lowFrequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)"),
                        expression("bandwidth", "!Band * (1 hz)"), expression("amplification", "3"),
                        expression("decay", "3")}),
                {{"Low", 5.0}, {"High", 2500.0}, {"Band", 400.0}},
                {eventValueChange(1024, "High", 1500.0), eventValueChange(1344, "Band", 200.0)}, 576),
        "CDP Hilite Arpeg should follow its frequencies and bandwidth as CDP8 follows breakpoint files");
    require(played("hilite-arpeg-2-live-sustain-fixed-amplification",
                node("spectral", "talas.cdpHiliteArpeg",
                    {choice("mode", "boost"), expression("rate", "30 hz"), expression("lowFrequency", "200 hz"),
                        expression("highFrequency", "2500 hz"), expression("bandwidth", "300 hz"),
                        expression("sustain", "!Sustain")}),
                {{"Sustain", 2.0}}, {eventValueChange(1024, "Sustain", 4.0)}, 1152),
        "CDP Hilite Arpeg should follow its sustain as CDP8 follows a breakpoint file, dividing a fixed amplification again");
    require(played("hilite-greq-2-notch",
                node("spectral", "talas.cdpHiliteGraphicEq",
                    {choice("mode", "bandwidths"), text("bands", "441 0.5\n1000 2\n1733 0.25\n6000 1\n"),
                        flag("notch")}),
                {}, {}, 1152),
        "CDP Hilite Graphic EQ should play its bands as CDP8 reads its data file");
    require(played("hilite-band-bits",
                node("spectral", "talas.cdpHiliteBand",
                    {text("bands",
                        "100 400 1000 0.5\n500 1200 1100 0.2 3\n1500 2500 0010 1.5\n3000 4000 0011 +700\n"
                        "4500 6000 1111 2 0.5 0.75\n9000 12000 0010 +-300\n")}),
                {}, {}, 1152),
        "CDP Hilite Band should play its bands as CDP8 reads its data file");
}

// CDP's blur spread, whose spread CDP takes as a breakpoint file, and strange
// glis, whose glissando rate it does.
void testBlurSpreadAndGlisNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("blur-spread-2-pitchwise-live-spread",
                node("spectral", "talas.cdpBlurSpread",
                    {choice("envelope", "pitchWise"), expression("formantBands", "2"),
                        expression("spread", "!Spread")}),
                {{"Spread", 0.2}}, {eventValueChange(1024, "Spread", 0.9)}, 1152),
        "CDP Blur Spread should follow its spread as CDP8 follows a breakpoint file");
    require(played("strange-glis-1-live-rate",
                node("spectral", "talas.cdpStrangeGlis", {expression("glisRate", "!Rate")}),
                {{"Rate", 800.0}}, {eventValueChange(1024, "Rate", -1200.0)}, 1152),
        "CDP Strange Glis should follow its rate as CDP8 follows a breakpoint file");
    require(played("strange-glis-2-live-rate",
                node("spectral", "talas.cdpStrangeGlis",
                    {choice("mode", "inharmonic"), choice("envelope", "pitchWise"), expression("glisRate", "!Rate"),
                        expression("spacing", "46.875 hz"), expression("topFrequency", "20000 hz")}),
                {{"Rate", -300.0}}, {eventValueChange(1024, "Rate", 2500.0)}, 1152),
        "CDP Strange Glis should follow its rate in an inharmonic glide under a top");
}

// CDP's two-file processes with the Input as CDP's first file and the Side
// Chain as its second: combine sum, whose crossover CDP takes as a breakpoint
// file, combine cross, whose interpolation it does, combine mean and formants
// vocode.
void testCombineAndVocodeNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> output;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            output.push_back(std::strtod(end, &end));
        }
        const auto length = input.size();
        input.resize(length + latency, 0.0);
        side.resize(length + latency, 0.0);
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"side", "audioOut", "spectral", "sideChain"},
                {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), "CDP graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return std::vector<double>(offline.mono.begin() + static_cast<std::ptrdiff_t>(latency), offline.mono.end())
            == output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("combine-sum-3-live-crossover",
                node("spectral", "talas.cdpCombineSum", {expression("crossover", "!Crossover")}),
                {{"Crossover", 0.3}}, {eventValueChange(1024, "Crossover", 0.8)}, 1152),
        "CDP Combine Sum should follow its crossover as CDP8 follows a breakpoint file");
    require(played("combine-cross-4-live-interp-512-points",
                node("spectral", "talas.cdpCombineCross",
                    {expression("points", "512"), expression("interpolation", "!Interpolation")}),
                {{"Interpolation", 0.9}}, {eventValueChange(1344, "Interpolation", 0.4)}, 576),
        "CDP Combine Cross should follow its interpolation as CDP8 follows a breakpoint file");
    require(played("combine-mean-2-zero-outside",
                node("spectral", "talas.cdpCombineMean",
                    {choice("mode", "meanAmplitudeMeanFrequency"), expression("lowFrequency", "500 hz"),
                        expression("highFrequency", "4000 hz"), [] {
                            Parameter zero;
                            zero.parameterId = "zeroOutside";
                            zero.boolean = true;
                            return zero;
                        }()}),
                {}, {}, 1152),
        "CDP Combine Mean should play its two files as CDP8 does");
    require(played("formants-vocode-3-quicksearch-range-gain",
                node("spectral", "talas.cdpFormantsVocode",
                    {expression("formantBands", "2"), [] {
                         Parameter quick;
                         quick.parameterId = "quickSearch";
                         quick.boolean = true;
                         return quick;
                     }(),
                        expression("lowFrequency", "200 hz"), expression("highFrequency", "9000 hz"),
                        expression("gain", "0.5")}),
                {}, {}, 1152),
        "CDP Formants Vocode should put the side chain's formants on the input as CDP8 does");
}

// CDP's morph morph and bridge with the Input as CDP's first file, the Side
// Chain as its second and the times from the Sound's start.
void testMorphNodesPlayTheirFiles()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral, std::size_t latency) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> output;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            output.push_back(std::strtod(end, &end));
        }
        const auto length = input.size();
        input.resize(length + latency, 0.0);
        side.resize(length + latency, 0.0);
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"side", "audioOut", "spectral", "sideChain"},
                {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), "CDP graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return std::vector<double>(offline.mono.begin() + static_cast<std::ptrdiff_t>(latency), offline.mono.end())
            == output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("morph-morph-2-stagger",
                node("spectral", "talas.cdpMorphMorph",
                    {choice("mode", "cosine"), expression("amplitudeStart", "0.012 s"),
                        expression("amplitudeEnd", "0.025 s"), expression("frequencyStart", "0.015 s"),
                        expression("frequencyEnd", "0.04 s"), expression("amplitudeExponent", "0.6"),
                        expression("frequencyExponent", "2"), expression("stagger", "0.01 s")}),
                1152),
        "CDP Morph Morph should morph the input into the side chain as CDP8 does");
    require(played("morph-bridge-3-offset",
                node("spectral", "talas.cdpMorphBridge",
                    {choice("mode", "followInput"), expression("offset", "0.005 s"),
                        expression("endSideFrequency", "0.6"), expression("endSideAmplitude", "0.8"),
                        expression("startTime", "0.01 s"), expression("endTime", "0.03 s")}),
                1152),
        "CDP Morph Bridge should bridge the input towards the side chain as CDP8 does");
}

// CDP's repitch transposef, whose transposition and frequencies CDP takes as
// breakpoint files.
void testRepitchTransposefNodeFollowsItsFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("repitch-transposef-3-live-semitones-pitchwise",
                node("spectral", "talas.cdpRepitchTransposef",
                    {choice("envelope", "pitchWise"), expression("transposition", "!Semitones")}),
                {{"Semitones", 5.0}}, {eventValueChange(1024, "Semitones", -7.0)}, 1152),
        "CDP Repitch Transposef should follow its transposition as CDP8 follows a breakpoint file");
    require(played("repitch-transposef-1-live-low-and-high",
                node("spectral", "talas.cdpRepitchTransposef",
                    {choice("units", "ratio"), expression("transposition", "1.25"),
                        expression("lowFrequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)")}),
                {{"Low", 5.0}, {"High", 4000.0}},
                {eventValueChange(1024, "Low", 900.0), eventValueChange(1344, "High", 2000.0)}, 1152),
        "CDP Repitch Transposef should follow its band frequencies as CDP8 follows breakpoint files");
}

// CDP's formants put, with the side chain's formants as `formants get` reads
// them, and hilite vowels, whose vowel file is a text field.
void testFormantsPutAndVowelsNodesPlayTheirFields()
{
    const auto pair = [](std::string fixture, talas::project::ProjectNode spectral, std::size_t latency) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> output;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            output.push_back(std::strtod(end, &end));
        }
        const auto length = input.size();
        input.resize(length + latency, 0.0);
        side.resize(length + latency, 0.0);
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"side", "audioOut", "spectral", "sideChain"},
                {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), "CDP graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return std::vector<double>(offline.mono.begin() + static_cast<std::ptrdiff_t>(latency), offline.mono.end())
            == output;
    };
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        return renderCdpInputLagging(document, {sampleRate, 64}, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto text = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    require(pair("formants-put-8-impose-range-gain-p4",
                node("spectral", "talas.cdpFormantsPut",
                    {choice("envelope", "pitchWise"), choice("mode", "impose"), expression("lowFrequency", "150 hz"),
                        expression("highFrequency", "9000 hz"), expression("gain", "3")}),
                1152),
        "CDP Formants Put should put the side chain's formants on the input as CDP8 does");
    require(played("hilite-vowels-2-late-start-narrow",
                node("spectral", "talas.cdpHiliteVowels",
                    {text("vowels", "; a late start, pairs across lines\n0.008 i 0.02\nar 0.035 u\n"),
                        expression("halfWidth", "0.1")}),
                1152),
        "CDP Hilite Vowels should shape the spectrum by its vowels as CDP8 does");
}

// CDP's blur blur and hilite bltr, whose blur and trace CDP takes as
// breakpoint files, a maximum blur's frames late.
void testBlurBlurAndBltrNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency,
                            std::size_t compared) {
        auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        const auto expected = std::vector<double>(cdp.output.begin(),
            cdp.output.begin() + static_cast<std::ptrdiff_t>(compared));
        return renderCdpInputLagging(document, settings, std::move(cdp), latency, compared) == expected;
    };
    require(played("blur-blur-live-blur",
                node("spectral", "talas.cdpBlurBlur", {expression("blur", "!Blur"), expression("maximumBlur", "9")}),
                {{"Blur", 3.0}}, {eventValueChange(640, "Blur", 7.0), eventValueChange(1344, "Blur", 2.0)},
                1152 + 9 * 128, 2048),
        "CDP Blur Blur should follow its blur as CDP8 follows a breakpoint file, its maximum blur's frames late");
    require(played("hilite-bltr-live-blur-and-trace",
                node("spectral", "talas.cdpHiliteBlurTrace",
                    {expression("blur", "!Blur"), expression("trace", "!Trace"), expression("maximumBlur", "8")}),
                {{"Blur", 2.0}, {"Trace", 5.0}},
                {eventValueChange(704, "Blur", 6.0), eventValueChange(1024, "Trace", 30.0),
                    eventValueChange(1536, "Blur", 3.0), eventValueChange(1728, "Trace", 2.0)},
                1152 + 8 * 128, 1792),
        "CDP Hilite Blur Trace should follow its blur and trace as CDP8 follows breakpoint files");
}

// CDP's focus focus, whose bandwidth and frequencies CDP takes as breakpoint
// files, and with -s its stability's frames late.
void testFocusFocusNodeFollowsItsFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency,
                            std::size_t compared) {
        auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        const auto expected = std::vector<double>(cdp.output.begin(),
            cdp.output.begin() + static_cast<std::ptrdiff_t>(compared));
        return renderCdpInputLagging(document, settings, std::move(cdp), latency, compared) == expected;
    };
    Parameter pitchwise;
    pitchwise.parameterId = "envelope";
    pitchwise.choice = "pitchWise";
    Parameter stable;
    stable.parameterId = "stable";
    stable.boolean = true;
    require(played("focus-focus-10-pitchwise-live-bandwidth",
                node("spectral", "talas.cdpFocusFocus",
                    {pitchwise, expression("formantBands", "3"), expression("peaks", "4"),
                        expression("bandwidth", "!Bandwidth")}),
                {{"Bandwidth", 0.3}},
                {eventValueChange(704, "Bandwidth", 2.0), eventValueChange(1408, "Bandwidth", 0.12)}, 1152, 2048),
        "CDP Focus Focus should follow its bandwidth as CDP8 follows a breakpoint file");
    require(played("focus-focus-20-stability-5-live-bandwidth-bottom-top",
                node("spectral", "talas.cdpFocusFocus",
                    {expression("peaks", "5"), expression("bandwidth", "!Bandwidth"),
                        expression("lowFrequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)"),
                        stable, expression("stability", "5")}),
                {{"Bandwidth", 0.25}, {"Low", 200.0}, {"High", 3000.0}},
                {eventValueChange(512, "Low", 1000.0), eventValueChange(960, "Bandwidth", 1.2),
                    eventValueChange(1216, "High", 6000.0)},
                1152 + 3 * 128, 1792),
        "CDP Focus Focus should keep stable peaks, its stability's frames late, following its fields");
}

// CDP's pitch tune, whose focus, clarity, trace and bass cut CDP takes as
// breakpoint files, pick, whose clarity it does, and chord, whose band it
// does, with their data files as text fields.
void testPitchTunePickAndChordNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto text = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("pitch-tune-1-live-focus-clarity-trace-bass-cut-256-points",
                node("spectral", "talas.cdpPitchTune",
                    {expression("points", "256"), text("template", "650\n"), expression("focus", "!Focus"),
                        expression("clarity", "!Clarity"), expression("trace", "!Trace"),
                        expression("bassCut", "!BassCut * (1 hz)")}),
                {{"Focus", 1.0}, {"Clarity", 0.2}, {"Trace", 3.0}, {"BassCut", 9.5}},
                {eventValueChange(512, "Focus", 0.3), eventValueChange(768, "Clarity", 0.9),
                    eventValueChange(1024, "Trace", 12.0), eventValueChange(1280, "BassCut", 2000.0)},
                288),
        "CDP Pitch Tune should follow its fields as CDP8 follows breakpoint files");
    require(played("pitch-pick-4-linear-live-clarity",
                node("spectral", "talas.cdpPitchPick",
                    {choice("mode", "linear"), expression("fundamental", "400 hz"), expression("step", "650 hz"),
                        expression("clarity", "!Clarity")}),
                {{"Clarity", 0.4}}, {eventValueChange(1024, "Clarity", 1.0)}, 1152),
        "CDP Pitch Pick should follow its clarity as CDP8 follows a breakpoint file");
    require(played("pitch-chord-live-bottom-and-top-256-points",
                node("spectral", "talas.cdpPitchChord",
                    {expression("points", "256"), text("transpositions", "-12\n0\n7.5\n"),
                        expression("lowFrequency", "!Low * (1 hz)"), expression("highFrequency", "!High * (1 hz)")}),
                {{"Low", 800.0}, {"High", 6000.0}},
                {eventValueChange(768, "Low", 3000.0), eventValueChange(1280, "High", 2000.0)}, 288),
        "CDP Pitch Chord should follow its band as CDP8 follows breakpoint files");
    require(played("pitch-chordf",
                node("spectral", "talas.cdpPitchChordf", {text("transpositions", "-12 0 7\n")}), {}, {}, 1152),
        "CDP Pitch Chordf should keep the formant envelope as CDP8 does");
}

// CDP's pitch altharms, octmove and spec bare, reading the pitch getpitch
// finds over a sample resource the Input plays from its start.
void testPitchFileNodesReadTheirPitchSource()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode spectral,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        settings.tableResources.push_back({"Samples/Pitched", cdp.input});
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto source = [] {
        Parameter parameter;
        parameter.parameterId = "pitchSource";
        parameter.resource = "Samples/Pitched";
        return parameter;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto flag = [](std::string id) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = true;
        return parameter;
    };
    require(played("pitch-altharms-2-256-points-less-body",
                node("spectral", "talas.cdpPitchAltharms",
                    {expression("points", "256"), source(), choice("mode", "deleteEven"), flag("lessBody")}),
                {}, {}, 288),
        "CDP Pitch Altharms should delete the harmonics of the pitch getpitch finds in its source");
    require(played("pitch-octmove-3-256-points-live-bass-boost-keep-unpitched",
                node("spectral", "talas.cdpPitchOctmove",
                    {expression("points", "256"), source(), flag("keepUnpitched"), choice("mode", "downBassBoost"),
                        expression("bassBoost", "!Boost")}),
                {{"Boost", 0.0}}, {eventValueChange(4096, "Boost", 50.0)}, 288),
        "CDP Pitch Octmove should move the spectrum by the pitch getpitch finds, following its bass boost");
    require(played("spec-bare-128-points",
                node("spectral", "talas.cdpSpecBare", {expression("points", "128"), source()}), {}, {}, 144),
        "CDP Spec Bare should keep the harmonics of the pitch getpitch finds in its source");
    // In realtime as offline, the pitch found when the Sound prepares.
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources.push_back({"Samples/Pitched", cdpReference("pitch-altharms-1-256-points").input});
    for (const auto* type : {"talas.cdpPitchAltharms", "talas.cdpPitchOctmove", "talas.cdpSpecBare"})
    {
        render(graph({tone("osc", "441 hz"), node("spectral", type, {expression("points", "256"), source()})},
                   {{"osc", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}}),
            settings);
    }
}

// CDP's spec clean and strange invert, which analyse a whole sample when
// the Sound starts (spec clean's noise and good signal, strange invert's
// normalising source, which the Input plays from its start), and focus
// freeze, which waits for the windows its times read ahead.
void testWholeSoundAndFreezeNodesPlayTheirPorts()
{
    // A fixture's columns: the input, the samples it analyses, CDP's output.
    const auto columns = [](const std::string& name) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
        require(file.good(), "CDP reference " + name + " should be readable");
        std::vector<std::vector<double>> result;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            const char* at = line.c_str();
            for (std::size_t column = 0;; ++column)
            {
                char* end = nullptr;
                const auto value = std::strtod(at, &end);
                if (end == at)
                    break;
                if (result.size() <= column)
                    result.emplace_back();
                result[column].push_back(value);
                at = end;
            }
        }
        return result;
    };
    const auto resource = [](std::string id, std::string name) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.resource = std::move(name);
        return parameter;
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto text = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto played = [&](const std::string& fixture, talas::project::ProjectNode spectral,
                            const std::vector<std::string>& samples, std::size_t latency) {
        const auto values = columns(fixture);
        CdpReference cdp;
        cdp.input = values.front();
        cdp.output = values.back();
        const auto document = graph({node("in", "talas.audioInput"), std::move(spectral)},
            {{"in", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        for (std::size_t index = 0; index < samples.size(); ++index)
            settings.tableResources.push_back({samples[index], values[index]});
        return renderCdpInputLagging(document, settings, cdp, latency, cdp.output.size()) == cdp.output;
    };
    const auto latency = [](int points) { return *talas::cdp::PhaseVocoder::latencyFor(points, 3); };
    require(played("spec-clean-1-from-time-256-points",
                node("spectral", "talas.cdpSpecClean",
                    {expression("points", "256"), choice("mode", "fromTime"),
                        resource("noiseSource", "Samples/Noise"), expression("skipTime", "0.005 s")}),
                {"Samples/Input", "Samples/Noise"}, latency(256)),
        "CDP Spec Clean should silence channels below the noise sample's levels from the skip time");
    require(played("spec-clean-4-comparing-256-points",
                node("spectral", "talas.cdpSpecClean",
                    {expression("points", "256"), choice("mode", "comparing"),
                        resource("noiseSource", "Samples/Noise"), resource("signalSource", "Samples/Good")}),
                {"Samples/Input", "Samples/Noise", "Samples/Good"}, latency(256)),
        "CDP Spec Clean should silence the channels the good signal never takes above the noise");
    require(played("strange-invert-2-keep-amplitude-256-points",
                node("spectral", "talas.cdpStrangeInvert",
                    {expression("points", "256"), choice("mode", "keepAmplitude"),
                        resource("normalisingSource", "Samples/Input")}),
                {"Samples/Input"}, latency(256)),
        "CDP Strange Invert should invert against the loudest shares of its normalising source");
    require(played("focus-freeze-1-amplitudes-256-points",
                node("spectral", "talas.cdpFocusFreeze",
                    {expression("points", "256"), choice("mode", "amplitudes"),
                        text("times", "a0.004 0.02 b0.03 a0.05")}),
                {}, latency(256) + 15 * 32),
        "CDP Focus Freeze should freeze its segments, waiting for its b time's window");
    // In realtime as offline.
    talas::runtime::RenderSettings settings{sampleRate, 64};
    const auto noise = columns("spec-clean-4-comparing-256-points");
    settings.tableResources.push_back({"Samples/Noise", noise[1]});
    settings.tableResources.push_back({"Samples/Good", noise[2]});
    for (auto spectral : {
             node("spectral", "talas.cdpSpecClean",
                 {expression("points", "256"), choice("mode", "comparing"), resource("noiseSource", "Samples/Noise"),
                     resource("signalSource", "Samples/Good")}),
             node("spectral", "talas.cdpSpecClean",
                 {choice("mode", "filtering"), resource("noiseSource", "Samples/Noise"),
                     expression("frequency", "2000 hz"), expression("noiseGain", "4")}),
             node("spectral", "talas.cdpStrangeInvert", {resource("normalisingSource", "Samples/Good")}),
             node("spectral", "talas.cdpFocusFreeze",
                 {expression("points", "256"), choice("mode", "both"), text("times", "a0.002 0.01 b0.02")}),
         })
    {
        render(graph({tone("osc", "441 hz"), std::move(spectral)},
                   {{"osc", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}}),
            settings);
    }
    const auto refused = [&](talas::project::ProjectNode spectral) {
        const auto document = graph({tone("osc", "441 hz"), std::move(spectral)},
            {{"osc", "audioOut", "spectral", "audioIn"}, {"spectral", "audioOut", "out", "audioIn"}});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        if (!compiled.ok())
            return true;
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        return !talas::runtime::renderOffline(*compiled.graph, frames).ok() && !session.ready();
    };
    const auto clean = [&](std::vector<Parameter> parameters) {
        parameters.push_back(resource("noiseSource", "Samples/Noise"));
        return node("spectral", "talas.cdpSpecClean", std::move(parameters));
    };
    require(!refused(clean({})) && refused(clean({expression("noiseGain", "41")}))
            && refused(clean({choice("mode", "filtering"), expression("frequency", "4 hz")}))
            && refused(clean({choice("mode", "comparing")}))
            && refused(node("spectral", "talas.cdpSpecClean", {}))
            && refused(node("spectral", "talas.cdpStrangeInvert", {}))
            && refused(node("spectral", "talas.cdpFocusFreeze", {text("times", "0.1 0.2")}))
            && refused(node("spectral", "talas.cdpFocusFreeze", {text("times", "a0.2 b0.4")})),
        "CDP Spec Clean should refuse a noise gain over 40, a frequency under 5 Hz, comparing without a good "
        "signal and no noise sample, Strange Invert no normalising source, and Focus Freeze times without a flag "
        "or with an a before a b, as CDP does");
}

// CDP's envel warp 6 (timestretch) for a stretch of 1 or more, two windows
// less a frame late.
void testEnvelopeTimestretchPlaysItsPort()
{
    for (const auto& [fixture, stretch] : {
             std::pair{std::string{"envel-warp-6-timestretch-1.37-ripple"}, std::string{"1.37"}},
             std::pair{std::string{"envel-warp-6-timestretch-3.3-ripple"}, std::string{"3.3"}},
             std::pair{std::string{"envel-warp-6-timestretch-1"}, std::string{"1"}},
         })
    {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"),
                                        node("stretch", "talas.cdpEnvelopeTimestretch",
                                            {expression("window", "5.333333 ms"), expression("stretch", stretch)})},
            {{"in", "audioOut", "stretch", "audioIn"}, {"stretch", "audioOut", "out", "audioIn"}});
        require(renderCdpInputLagging(document, {sampleRate, 64}, cdp, 511, cdp.output.size()) == cdp.output,
            "CDP Envelope Timestretch should match CDP8's envel warp 6 at stretch " + stretch);
    }
    render(graph({tone("osc", "441 hz"), node("stretch", "talas.cdpEnvelopeTimestretch", {})},
        {{"osc", "audioOut", "stretch", "audioIn"}, {"stretch", "audioOut", "out", "audioIn"}}));
    require(fails(graph({tone("osc", "441 hz"), node("stretch", "talas.cdpEnvelopeTimestretch", {expression("stretch", "0.5")})},
                {{"osc", "audioOut", "stretch", "audioIn"}, {"stretch", "audioOut", "out", "audioIn"}})),
        "CDP Envelope Timestretch should refuse a stretch below 1, which reads ahead without bound");
}

// CDP's varibank2, whose Q CDP takes as a breakpoint file, and its newer
// tremolo, whose frequency, depth and gain it does.
void testVaryingBank2AndTremoloFineNodesFollowTheirFields()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode process,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return renderCdpInputLagging(document, settings, cdp, 0, cdp.output.size()) == cdp.output;
    };
    // CDP8's data file, its partials after its # line.
    Parameter bands;
    bands.parameterId = "bands";
    bands.text = "; two pitches gliding\n0 300 1 1500 0.5\n\n0.015 420 0.8 1300 -6dB\n0.03 380 1 1650 0.3";
    Parameter partials;
    partials.parameterId = "partials";
    partials.text = "0 1 1 2 0.5 3 0.25\n0.02 1 1 2.5 0.3 3.5 0.4\n0.04 1 0.6 2 0.7 4 0.2";
    require(played("filter-varibank2-1-live-q",
                node("process", "talas.cdpVaryingBank2", {bands, partials, expression("q", "!Q")}),
                {{"Q", 30.0}}, {eventValueChange(1024, "Q", 8.0), eventValueChange(1664, "Q", 50.0)}),
        "CDP Varying Bank 2 should follow its Q as CDP8 follows a breakpoint file");
    require(played("tremolo-new-1-live-frequency-depth-gain-fineness-3",
                node("process", "talas.cdpTremoloFine",
                    {expression("frequency", "!Frequency * (1 hz)"), expression("depth", "!Depth"),
                        expression("gain", "!Gain"), expression("fineness", "3")}),
                {{"Frequency", 60.0}, {"Depth", 1.0}, {"Gain", 0.8}},
                {eventValueChange(768, "Frequency", 0.0), eventValueChange(1024, "Depth", 0.25),
                    eventValueChange(1280, "Frequency", 210.5), eventValueChange(1536, "Gain", 1.0),
                    eventValueChange(2560, "Frequency", 500.0), eventValueChange(3072, "Depth", 0.0),
                    eventValueChange(3584, "Gain", 0.3)}),
        "CDP Tremolo Fine should follow its frequency, depth and gain as CDP8 follows breakpoint files");
}

// CDP's distmore double, clip clip 2 and quirk quirk 1 wait for each
// waveset or half-waveset, so they lag 2047 frames: offline on CDP8's runs,
// as far as CDP writes (distmore and quirk drop their last partial waveset
// or half-waveset, clip leaves its last unclipped), and alike in realtime.
void testWavesetDistortionNodesPlayTheirPorts()
{
    const auto lagging = [](std::string fixture, talas::project::ProjectNode process, std::size_t compared) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        const auto played = renderCdpInputLagging(document, {sampleRate, 64}, cdp, 2047, compared);
        return std::equal(played.begin(), played.end(), cdp.output.begin());
    };
    require(lagging("distmore-double-1-zero-runs", node("process", "talas.cdpDistmoreDouble"), 4071),
        "CDP Distmore Double should match CDP8's distmore double, 2047 frames late");
    require(lagging("clip-2-fraction-0.8-negative-zero-runs",
                node("process", "talas.cdpClipWaveset", {expression("fraction", "0.8")}), 4085),
        "CDP Clip Waveset should match CDP8's clip clip 2, 2047 frames late");
    require(lagging("quirk-1-exponent-3-zero-runs",
                node("process", "talas.cdpQuirkWaveset", {expression("exponent", "3")}), 4085),
        "CDP Quirk Waveset should match CDP8's quirk quirk 1, 2047 frames late");

    constexpr std::size_t longer = 3 * frames;
    constexpr auto latency = talas::cdp::DistmoreDouble::waitFrames;
    const auto dry = dryTone(longer);
    const auto played = [&](talas::project::ProjectNode process) {
        return render(graph({tone("osc", "441 hz"), std::move(process)},
                          {{"osc", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}}),
            {sampleRate, 64}, longer);
    };
    std::vector<double> expected(longer);
    talas::cdp::DistmoreDouble distmoreDouble;
    require(distmoreDouble.prepare(1) && distmoreDouble.process(dry, expected), "the distmore port should play");
    const auto doubled = played(node("process", "talas.cdpDistmoreDouble"));
    require(doubled == expected && doubled[latency + 30] != 0.0,
        "CDP Distmore Double should play its talas_cdp port");
    talas::cdp::ClipWaveset clipWaveset;
    require(clipWaveset.prepare(0.3) && clipWaveset.process(dry, expected), "the clip port should play");
    const auto clipped = played(node("process", "talas.cdpClipWaveset", {expression("fraction", "0.3")}));
    require(clipped == expected && clipped[latency + 30] != 0.0,
        "CDP Clip Waveset should play its talas_cdp port");
    talas::cdp::QuirkWaveset quirkWaveset;
    require(quirkWaveset.prepare(0.5) && quirkWaveset.process(dry, expected), "the quirk port should play");
    const auto quirked = played(node("process", "talas.cdpQuirkWaveset"));
    require(quirked == expected && quirked[latency + 30] != 0.0,
        "CDP Quirk Waveset should play its default exponent of 0.5 through its talas_cdp port");
}

// CDP's rmverb plays a mono input in stereo: offline on CDP8's runs at
// 16 kHz, from its defaults (the room's own predelay and reflections) to a
// reflections file and a mono output, and alike in realtime.
void testRoomReverbNodePlaysItsPortInStereo()
{
    // CDP8's input and its two outputs; a mono output writes both.
    const auto stereoReference = [](const std::string& name) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
        require(file.good(), "CDP reference " + name + " should be readable");
        std::vector<double> input;
        std::vector<double> left;
        std::vector<double> right;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            left.push_back(std::strtod(end, &end));
            char* after = nullptr;
            const auto second = std::strtod(end, &after);
            right.push_back(after == end ? left.back() : second);
        }
        require(!input.empty(), "CDP reference " + name + " should hold samples");
        return std::tuple{input, left, right};
    };
    Parameter medium;
    medium.parameterId = "room";
    medium.choice = "medium";
    Parameter large;
    large.parameterId = "room";
    large.choice = "large";
    Parameter doubleDamping;
    doubleDamping.parameterId = "doubleDamping";
    doubleDamping.boolean = true;
    Parameter monoOutput;
    monoOutput.parameterId = "monoOutput";
    monoOutput.boolean = true;
    Parameter reflections;
    reflections.parameterId = "reflections";
    reflections.text = "; hand-written early reflections for rmverb -e\n0.00001 0.9\n0.0123 0.6\n   0.0179  0.45\n\n0.0179 0.3\n0.0241 0.5\n0.0241 5.0\n0.0302 0.35 and a remark\n+0.0377 0.2\n0.0466 0.25";
    struct Room
    {
        std::string fixture;
        std::vector<Parameter> fields;
    };
    for (const auto& c : {
             // rmverb in out 1 0.5 0.5 0.5 4200 0 0.25: the defaults but the
             // absorption.
             Room{"rmverb-small-room-16k", {expression("absorb", "4200 hz")}},
             Room{"rmverb-small-room-double-damping-no-absorb-cuts-all-wet-16k",
                 {doubleDamping, expression("reverbGain", "1"), expression("mix", "0"), expression("feedback", "0.9"),
                     expression("absorb", "0 hz"), expression("lowCut", "200 hz"), expression("highCut", "3000 hz")}},
             Room{"rmverb-medium-room-mono-out-lowpass-16k",
                 {medium, monoOutput, expression("lowpass", "2000 hz")}},
             Room{"rmverb-medium-room-reflections-file-predelay-15-16k",
                 {medium, reflections, expression("reverbGain", "0.9"), expression("mix", "0.3"),
                     expression("feedback", "0.8"), expression("absorb", "3000 hz"), expression("lowpass", "4000 hz"),
                     expression("predelay", "15 ms")}},
         })
    {
        const auto [input, left, right] = stereoReference(c.fixture);
        const auto document = graph({node("in", "talas.audioInput"), node("reverb", "talas.cdpRoomReverb", c.fields)},
            {{"in", "audioOut", "reverb", "audioIn"},
                {"reverb", "leftOut", "out", "audioIn"},
                {"reverb", "rightOut", "out", "rightIn"}});
        talas::runtime::RenderSettings settings{16000.0, 64};
        settings.audioInputChannels.push_back({1, input});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), c.fixture + " should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), c.fixture + " should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        require(offline.mono == left && offline.right == right,
            "CDP Room Reverb should match CDP8's rmverb on both channels: " + c.fixture);
    }

    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("reverb", "talas.cdpRoomReverb",
                {large, doubleDamping, expression("reverbGain", "0.7"), expression("mix", "0.4"),
                    expression("feedback", "0.6"), expression("absorb", "3000 hz"), expression("predelay", "5 ms"),
                    expression("lowCut", "100 hz"), expression("highCut", "9000 hz")})},
        {{"osc", "audioOut", "reverb", "audioIn"},
            {"reverb", "leftOut", "out", "audioIn"},
            {"reverb", "rightOut", "out", "rightIn"}}));
    talas::cdp::RoomReverbSettings settings;
    settings.room = talas::cdp::RoomSize::large;
    settings.reverbGain = 0.7;
    settings.mix = 0.4;
    settings.feedback = 0.6;
    settings.absorbHz = 3000.0;
    settings.predelayMilliseconds = 5.0;
    settings.lowCutHz = 100.0;
    settings.highCutHz = 9000.0;
    settings.doubleDamping = true;
    talas::cdp::RoomReverb port;
    require(port.prepare(sampleRate, settings), "the room reverb port should prepare");
    std::vector<double> expectedLeft(frames);
    std::vector<double> expectedRight(frames);
    require(port.process(dry, expectedLeft, expectedRight), "the room reverb port should process");
    require(played == expectedLeft && lastRight == expectedRight && played != expectedRight,
        "CDP Room Reverb should play its talas_cdp port on both channels");
}

// The same samples, but for a zero's sign: the graph around a node plays
// CDP8's -0 as +0, so the sign is left to the processor tests.
bool sameSamples(const std::vector<double>& played, const std::vector<double>& cdp)
{
    return played == cdp;
}

// CDP's modify radical 7, modify loudness 1, 2 and 6, phase phase 1 and
// housekeep extract 4: offline on CDP8's runs bit for bit, with a bit
// resolution that changes and gains that change as CDP8's breakpoint files
// step, and alike in realtime.
void testSampleProcessNodesPlayTheirPorts()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode process,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return sameSamples(renderCdpInput(document, settings, cdp), cdp.output);
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto flag = [](std::string id, bool value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = value;
        return parameter;
    };
    require(played("modify-radical-7-quantise-bits-3",
                node("process", "talas.cdpQuantise", {expression("bitResolution", "3")}), {}, {}),
        "CDP Quantise should match CDP8's modify radical 7");
    {
        // 16 bits, then 3 from frame 1024: each plays as CDP8 plays it.
        const auto sixteen = cdpReference("modify-radical-7-quantise-bits-16");
        const auto three = cdpReference("modify-radical-7-quantise-bits-3");
        const auto document = graph(
            {node("in", "talas.audioInput"), node("process", "talas.cdpQuantise", {expression("bitResolution", "!Bits")})},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Bits", 16.0}};
        settings.eventValueChanges = {eventValueChange(1024, "Bits", 3.0)};
        std::vector<double> expected(sixteen.output.begin(), sixteen.output.begin() + 1024);
        expected.insert(expected.end(), three.output.begin() + 1024, three.output.end());
        require(sameSamples(renderCdpInput(document, settings, sixteen), expected),
            "CDP Quantise should follow its bit resolution as it changes");
    }
    require(played("modify-loudness-1-live-gain", node("process", "talas.cdpLoudnessGain", {expression("gain", "!Gain")}),
                {{"Gain", 0.5}},
                {eventValueChange(576, "Gain", 3.0), eventValueChange(1024, "Gain", 0.0),
                    eventValueChange(1280, "Gain", 1.25), eventValueChange(1664, "Gain", 32767.0)}),
        "CDP Loudness Gain should follow its gain as CDP8 follows a breakpoint file");
    require(played("modify-loudness-2-live-db-gain-no-clip",
                node("process", "talas.cdpLoudnessGain",
                    {choice("scale", "decibels"), expression("gain", "!Gain"), flag("clip", false)}),
                {{"Gain", -6.0}},
                {eventValueChange(512, "Gain", 33.3), eventValueChange(768, "Gain", -96.0),
                    eventValueChange(1024, "Gain", 0.000001), eventValueChange(1280, "Gain", -53.714),
                    eventValueChange(1408, "Gain", -33.3), eventValueChange(1536, "Gain", 96.0),
                    eventValueChange(1792, "Gain", 10.292), eventValueChange(1920, "Gain", -95.5)}),
        "CDP Loudness Gain should follow its gain in decibels, unclipped, as CDP8 follows a breakpoint file");
    require(played("modify-loudness-2-db-gain-7.5",
                node("process", "talas.cdpLoudnessGain", {choice("scale", "decibels"), expression("gain", "7.5")}), {},
                {}),
        "CDP Loudness Gain should match CDP8's fixed gain in decibels, clipped");
    require(played("modify-loudness-6-invert-phase", node("process", "talas.cdpPhaseInvert", {choice("mode", "loudness")}),
                {}, {})
            && played("phase-phase-1-invert", node("process", "talas.cdpPhaseInvert"), {}, {}),
        "CDP Phase Invert should match CDP8's modify loudness 6 and, by default, phase phase 1");
    require(played("housekeep-extract-4-rectify-shift-0.3",
                node("process", "talas.cdpDcShift", {expression("shift", "0.3")}), {}, {})
            && played("housekeep-extract-4-rectify-shift-minus-0.25",
                node("process", "talas.cdpDcShift", {expression("shift", "-0.25")}), {}, {}),
        "CDP DC Shift should match CDP8's housekeep extract 4");

    const auto dry = dryTone();
    const auto through = [](talas::project::ProjectNode process, talas::runtime::RenderSettings settings) {
        return render(graph({tone("osc", "441 hz"), std::move(process)},
                          {{"osc", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}}),
            settings);
    };
    std::vector<double> expected(frames);
    talas::cdp::Quantise quantise;
    require(quantise.prepare(5) && quantise.process(dry, expected), "the quantise port should play");
    require(through(node("process", "talas.cdpQuantise", {expression("bitResolution", "5")}), {sampleRate, 64})
            == expected,
        "CDP Quantise should play its talas_cdp port");
    talas::cdp::LoudnessGain loudness;
    require(loudness.prepare(talas::cdp::LoudnessGain::Mode::Decibels, -3.0, false, true)
            && loudness.setGain(-3.0) && loudness.process(dry, expected),
        "the loudness port should play");
    talas::runtime::RenderSettings live{sampleRate, 64};
    live.eventValues = {{"Gain", -3.0}};
    require(through(node("process", "talas.cdpLoudnessGain",
                        {choice("scale", "decibels"), expression("gain", "!Gain"), flag("clip", false)}),
                live)
            == expected,
        "CDP Loudness Gain should play a live gain in decibels through its talas_cdp port");
    talas::cdp::PhaseInvert invert;
    require(invert.prepare(talas::cdp::PhaseInvert::Mode::Loudness) && invert.process(dry, expected),
        "the phase invert port should play");
    require(through(node("process", "talas.cdpPhaseInvert", {choice("mode", "loudness")}), {sampleRate, 64}) == expected,
        "CDP Phase Invert should play its talas_cdp port");
    talas::cdp::DcShift dcShift;
    require(dcShift.prepare(-0.05) && dcShift.process(dry, expected), "the DC shift port should play");
    require(through(node("process", "talas.cdpDcShift", {expression("shift", "-0.05")}), {sampleRate, 64}) == expected,
        "CDP DC Shift should play its talas_cdp port");
}

// CDP's modify radical 6, subtract subtract, submix balance and submix
// crossfade with the Input as CDP's first file and the Side Chain as its
// second: offline on CDP8's runs bit for bit, with a balance that changes as
// CDP8's breakpoint file steps, and alike in realtime.
void testTwoInputProcessNodesPlayTheirPorts()
{
    const auto played = [](std::string fixture, talas::project::ProjectNode process,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> output;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            output.push_back(std::strtod(end, &end));
        }
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"},
                {"process", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), "CDP graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return sameSamples(offline.mono, output);
    };
    Parameter cosine;
    cosine.parameterId = "contour";
    cosine.choice = "cosine";
    require(played("modify-radical-6-cross-modulation", node("process", "talas.cdpCrossModulate"), {}, {}),
        "CDP Cross Modulate should match CDP8's modify radical 6");
    require(played("subtract-channel-1", node("process", "talas.cdpSubtract"), {}, {}),
        "CDP Subtract should match CDP8's subtract subtract");
    require(played("submix-balance-default", node("process", "talas.cdpMixBalance"), {}, {}),
        "CDP Mix Balance should match CDP8's submix balance at its default");
    require(played("submix-balance-live-balance", node("process", "talas.cdpMixBalance", {expression("balance", "!Balance")}),
                {{"Balance", 0.25}},
                {eventValueChange(1024, "Balance", 0.9), eventValueChange(2048, "Balance", 0.0),
                    eventValueChange(3072, "Balance", 1.0)}),
        "CDP Mix Balance should follow its balance as CDP8 follows a breakpoint file");
    require(played("submix-crossfade-2-stagger-0.00502-powfac-0.4",
                node("process", "talas.cdpMixCrossfade",
                    {cosine, expression("begin", "0.02 s"), expression("end", "0.06 s"),
                        expression("stagger", "0.00502 s"), expression("powerFactor", "0.4")}),
                {}, {})
            && played("submix-crossfade-1-end-0.04", node("process", "talas.cdpMixCrossfade", {expression("end", "0.04 s")}),
                {}, {}),
        "CDP Mix Crossfade should match CDP8's submix crossfade, staggered and skewed, and linear");

    // In realtime as offline, through each port, with a quieter side chain.
    const auto quiet = [] {
        return node("side", "talas.oscillator", {expression("frequency", "300 hz"), expression("amplitude", "0.05")});
    };
    const auto dry = dryTone();
    const auto drySide = render(graph({quiet()}, {{"side", "audioOut", "out", "audioIn"}}));
    const auto through = [&](talas::project::ProjectNode process, talas::runtime::RenderSettings settings) {
        return render(graph({tone("osc", "441 hz"), quiet(), std::move(process)},
                          {{"osc", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"},
                              {"process", "audioOut", "out", "audioIn"}}),
            settings);
    };
    std::vector<double> expected(frames);
    talas::cdp::CrossModulate crossModulate;
    require(crossModulate.prepare() && crossModulate.process(dry, drySide, expected),
        "the cross modulation port should play");
    require(through(node("process", "talas.cdpCrossModulate"), {sampleRate, 64}) == expected,
        "CDP Cross Modulate should play its talas_cdp port");
    talas::cdp::Subtract subtract;
    require(subtract.prepare() && subtract.process(dry, drySide, expected), "the subtract port should play");
    require(through(node("process", "talas.cdpSubtract"), {sampleRate, 64}) == expected,
        "CDP Subtract should play its talas_cdp port");
    talas::cdp::MixBalance balance;
    require(balance.prepare(0.3) && balance.process(dry, drySide, expected), "the balance port should play");
    talas::runtime::RenderSettings live{sampleRate, 64};
    live.eventValues = {{"Balance", 0.3}};
    require(through(node("process", "talas.cdpMixBalance", {expression("balance", "!Balance")}), live) == expected,
        "CDP Mix Balance should play a live balance through its talas_cdp port");
    talas::cdp::MixCrossfade crossfade;
    require(crossfade.prepare(sampleRate, talas::cdp::MixCrossfade::Mode::Cosine, 0.005, 0.03, 0.002, 2.0)
            && crossfade.process(dry, drySide, expected),
        "the crossfade port should play");
    const auto crossfaded = through(node("process", "talas.cdpMixCrossfade",
                                        {cosine, expression("begin", "0.005 s"), expression("end", "0.03 s"),
                                            expression("stagger", "0.002 s"), expression("powerFactor", "2")}),
        {sampleRate, 64});
    require(crossfaded == expected && crossfaded.back() == static_cast<double>(static_cast<float>(drySide[frames - 97])),
        "CDP Mix Crossfade should play its talas_cdp port, the side chain staggered after its end");
}

// CDP's fastconv with its impulse response a sample resource read whole
// when the Sound starts: offline on CDP8's runs bit for bit, its impulse's
// frames less one late, each channel of a stereo impulse sample; alike in
// realtime through its port; and only offline past 4096 frames.
void testFastConvolutionNodePlaysItsImpulseSample()
{
    // An impulse response in tests/cdp/fixtures as a sample resource; a
    // stereo one's second column is its right channel.
    const auto impulseSample = [](const std::string& name) {
        const auto ir = cdpReference("fastconv-ir-" + name);
        talas::runtime::TableResource table;
        table.id = "Samples/Impulse";
        table.samples = ir.input;
        if (name.find("stereo") != std::string::npos)
            table.rightSamples = ir.output;
        return table;
    };
    Parameter impulse;
    impulse.parameterId = "impulse";
    impulse.resource = "Samples/Impulse";
    const auto played = [&](const std::string& fixture, const std::string& ir, std::vector<Parameter> fields,
                            std::size_t column) {
        // CDP8's input and the output channel in the given column.
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        CdpReference cdp;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            cdp.input.push_back(std::strtod(line.c_str(), &end));
            double sample = 0.0;
            for (std::size_t index = 0; index <= column; ++index)
                sample = std::strtod(end, &end);
            cdp.output.push_back(sample);
        }
        fields.push_back(impulse);
        const auto document = graph(
            {node("in", "talas.audioInput"), node("conv", "talas.cdpFastConvolution", std::move(fields))},
            {{"in", "audioOut", "conv", "audioIn"}, {"conv", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        const auto table = impulseSample(ir);
        settings.tableResources.push_back(table);
        return sameSamples(
            renderCdpInputLagging(document, settings, cdp, table.samples.size() - 1, cdp.output.size()), cdp.output);
    };
    require(played("fastconv-37-frame-lowpass", "37-frame-lowpass", {}, 0),
        "CDP Fast Convolution should match CDP8's fastconv with its defaults");
    require(played("fastconv-1000-frame-reverb-scale-0.5-dry-0.3", "1000-frame-reverb",
                {expression("dry", "0.3"), expression("scale", "0.5")}, 0)
            && played("fastconv-100-frame-noise-scale-2-dry-0.25", "100-frame-noise",
                {expression("dry", "0.25"), expression("scale", "2")}, 0)
            && played("fastconv-256-frame-noise-dry-0.5", "256-frame-noise", {expression("dry", "0.5")}, 0),
        "CDP Fast Convolution should match CDP8's fastconv with a dry part and a scale");
    require(played("fastconv-5-frame-dry-0.8", "5-frame", {expression("dry", "0.8")}, 0)
            && played("fastconv-2-frame", "2-frame", {}, 0)
            && played("fastconv-1-frame-dry-0.2", "1-frame", {expression("dry", "0.2")}, 0),
        "CDP Fast Convolution should match CDP8's fastconv with the shortest impulse responses");
    require(played("fastconv-37-frame-lowpass-dry-1-silent", "37-frame-lowpass", {expression("dry", "1")}, 0)
            && played("fastconv-silent-impulse-dry-0.5", "16-frame-silent", {expression("dry", "0.5")}, 0),
        "CDP Fast Convolution should match CDP8's silence at a dry of 1 and the dry part alone of a silent impulse");
    require(played("fastconv-300-frame-stereo-impulse-dry-0.4", "300-frame-stereo", {expression("dry", "0.4")}, 0)
            && played("fastconv-300-frame-stereo-impulse-dry-0.4", "300-frame-stereo",
                {expression("dry", "0.4"), expression("impulseChannel", "2")}, 1),
        "CDP Fast Convolution should match each output channel of CDP8's fastconv with a stereo impulse sample");

    // In realtime as offline, through its port.
    const auto dry = dryTone();
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources.push_back(impulseSample("300-frame-stereo"));
    const auto ir = cdpReference("fastconv-ir-300-frame-stereo");
    std::vector<double> interleaved;
    for (std::size_t frame = 0; frame < ir.input.size(); ++frame)
    {
        interleaved.push_back(ir.input[frame]);
        interleaved.push_back(ir.output[frame]);
    }
    talas::cdp::FastConvolutionSettings fields;
    fields.dry = 0.25;
    fields.scale = 0.5;
    fields.impulseChannels = 2;
    fields.channel = 1;
    talas::cdp::FastConvolution port;
    std::vector<double> expected(frames);
    require(port.prepare(sampleRate, interleaved, sampleRate, fields) && port.latencyFrames() == 299
            && port.process(dry, expected),
        "the fast convolution port should play");
    const auto convolved = render(
        graph({tone("osc", "441 hz"),
                  node("conv", "talas.cdpFastConvolution",
                      {impulse, expression("dry", "0.25"), expression("scale", "0.5"),
                          expression("impulseChannel", "2")})},
            {{"osc", "audioOut", "conv", "audioIn"}, {"conv", "audioOut", "out", "audioIn"}}),
        settings);
    require(convolved == expected && convolved[400] != 0.0,
        "CDP Fast Convolution should play a stereo impulse sample's right channel through its talas_cdp port");

    // Past 4096 frames the impulse sample renders offline only, the port's
    // FFTs of a block running in the callback that completes it.
    talas::runtime::RenderSettings longSettings{sampleRate, 64};
    talas::runtime::TableResource longImpulse;
    longImpulse.id = "Samples/Long";
    longImpulse.samples.assign(5000, 0.0);
    longImpulse.samples.front() = 0.5;
    longImpulse.samples.back() = -0.25;
    longSettings.tableResources.push_back(longImpulse);
    Parameter longField;
    longField.parameterId = "impulse";
    longField.resource = "Samples/Long";
    const auto longGraph = graph({tone("osc", "441 hz"), node("conv", "talas.cdpFastConvolution", {longField})},
        {{"osc", "audioOut", "conv", "audioIn"}, {"conv", "audioOut", "out", "audioIn"}});
    const auto compiled = talas::runtime::compileProject(longGraph, talas::core::makeBuiltinNodeRegistry(), longSettings);
    require(compiled.ok(), "a 5000-frame impulse sample should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 6000);
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(offline.ok()
            && std::all_of(offline.mono.begin(), offline.mono.begin() + 4999, [](double sample) { return sample == 0.0; })
            && offline.mono[5500] != 0.0,
        "CDP Fast Convolution should render a 5000-frame impulse sample offline, 4999 frames late");
    require(!session.ready() && std::string{session.error()}.find("4096") != std::string::npos,
        "CDP Fast Convolution should refuse a 5000-frame impulse sample in realtime");

    // An impulse sample at another sample rate, or a channel it has not, is
    // refused as CDP refuses them.
    const auto refuses = [&](talas::runtime::TableResource table, std::vector<Parameter> parameters) {
        talas::runtime::RenderSettings refusing{sampleRate, 64};
        refusing.tableResources.push_back(std::move(table));
        parameters.push_back(impulse);
        return !talas::runtime::compileProject(
            graph({tone("osc", "441 hz"), node("conv", "talas.cdpFastConvolution", std::move(parameters))},
                {{"osc", "audioOut", "conv", "audioIn"}, {"conv", "audioOut", "out", "audioIn"}}),
            talas::core::makeBuiltinNodeRegistry(), refusing)
                    .ok();
    };
    auto otherRate = impulseSample("37-frame-lowpass");
    otherRate.sourceSampleRate = 44100;
    require(refuses(otherRate, {}) && refuses(impulseSample("37-frame-lowpass"), {expression("impulseChannel", "2")}),
        "CDP Fast Convolution should refuse an impulse sample at another sample rate and a channel it has not");
}

// CDP's gate gate 1, housekeep deglitch, sfedit noisecut and envel attack 1
// to 3 with CDP's default buffers: offline on CDP8's runs bit for bit, each
// its latency late, over all but CDP's last latency frames (where CDP ends
// its file as a stream cannot); alike in realtime through their ports; and
// the gate's latency aligns a dry branch.
void testRestorationNodesPlayTheirPorts()
{
    const auto played = [](const std::string& fixture, talas::project::ProjectNode process, std::size_t latency) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        const auto compared = cdp.output.size() - latency;
        return sameSamples(renderCdpInputLagging(document, {sampleRate, 64}, cdp, latency, compared),
            std::vector<double>(cdp.output.begin(), cdp.output.begin() + static_cast<std::ptrdiff_t>(compared)));
    };
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    require(played("gate-1-gate-minus-40-db", node("process", "talas.cdpSilenceGate"), 96)
            && played("gate-1-gate-0-db-unit-samples", node("process", "talas.cdpSilenceGate", {expression("gate", "0")}),
                96),
        "CDP Silence Gate should match CDP8's gate gate 1, two splices late");
    const auto fineDeglitch = [] {
        return std::vector<Parameter>{expression("glitch", "1.5 ms"), expression("silence", "0.5 ms"),
            expression("threshold", "0.005"), expression("splice", "0.25 ms"), expression("window", "3 ms")};
    };
    require(played("housekeep-deglitch-glitch-2-sil-10-thresh-0.01-splice-1-window-1", node("process", "talas.cdpDeglitch"),
                96 + 480 - 1 + 48)
            && played("housekeep-deglitch-glitch-2-sil-10-thresh-0.01-splice-1-window-1-start",
                node("process", "talas.cdpDeglitch"), 96 + 480 - 1 + 48)
            && played("housekeep-deglitch-glitch-1.5-sil-0.5-thresh-0.005-splice-0.25-window-3",
                node("process", "talas.cdpDeglitch", fineDeglitch()), 144 + 24 - 1 + 12)
            && played("housekeep-deglitch-glitch-1.5-sil-0.5-thresh-0.005-splice-0.25-window-3-start",
                node("process", "talas.cdpDeglitch", fineDeglitch()), 144 + 24 - 1 + 12)
            && played("housekeep-deglitch-glitch-2-sil-10-thresh-0-splice-0-window-1-digital-silence",
                node("process", "talas.cdpDeglitch", {expression("threshold", "0"), expression("splice", "0 ms")}),
                96 + 480 - 1 + 1),
        "CDP Deglitch should match CDP8's housekeep deglitch, its longest glitch, silence and splice late");
    const std::string noisecut = "sfedit-noisecut-splice-2-noisefrq-6000-maxnoise-5-mintone-15";
    Parameter keepNoise;
    keepNoise.parameterId = "keepNoise";
    keepNoise.boolean = true;
    require(played(noisecut + "-noise-first", node("process", "talas.cdpNoiseCut"), 1008)
            && played(noisecut + "-tone-first", node("process", "talas.cdpNoiseCut"), 1008)
            && played(noisecut + "-keep-noise", node("process", "talas.cdpNoiseCut", {keepNoise}), 1008)
            && played("sfedit-noisecut-splice-1-noisefrq-2000-maxnoise-2-mintone-5-noise-first",
                node("process", "talas.cdpNoiseCut",
                    {expression("splice", "1 ms"), expression("noiseFrequency", "2000 hz"),
                        expression("maxNoise", "2 ms"), expression("minTone", "5 ms")}),
                144 + 240)
            && played("sfedit-noisecut-splice-1-noisefrq-6000-maxnoise-0-mintone-0.5-pairs",
                node("process", "talas.cdpNoiseCut",
                    {expression("splice", "1 ms"), expression("maxNoise", "0 ms"), expression("minTone", "0.5 ms")}),
                144 + 24),
        "CDP Noise Cut should match CDP8's sfedit noisecut, three splices and the longer minimum late");
    const auto linear = choice("curve", "linear");
    const auto approximate = [&](std::vector<Parameter> more) {
        more.push_back(choice("mode", "approximateTime"));
        more.push_back(expression("level", "0.3"));
        return node("process", "talas.cdpAttackBoost", std::move(more));
    };
    const auto exact = [&](std::vector<Parameter> more) {
        for (auto field : {choice("mode", "exactTime"), expression("level", "0.1"), expression("gain", "2.5"),
                 expression("onset", "10 ms"), expression("decay", "40 ms")})
            more.push_back(std::move(field));
        return node("process", "talas.cdpAttackBoost", std::move(more));
    };
    require(played("envel-attack-1-gate-0.1-gain-2-onset-20-decay-50", node("process", "talas.cdpAttackBoost"), 961 + 64)
            && played("envel-attack-1-gate-0.1-gain-2-onset-20-decay-50-linear",
                node("process", "talas.cdpAttackBoost", {linear}), 961 + 64)
            && played("envel-attack-2-time-0.3-gain-2-onset-20-decay-50", approximate({}), 961 + 19200)
            && played("envel-attack-2-time-0.3-gain-2-onset-20-decay-50-linear", approximate({linear}), 961 + 19200)
            && played("envel-attack-3-time-0.1-gain-2.5-onset-10-decay-40", exact({}), 0)
            && played("envel-attack-3-time-0.1-gain-2.5-onset-10-decay-40-linear", exact({linear}), 0),
        "CDP Attack Boost should match CDP8's envel attack 1 to 3, the onset (and its search) late");

    // In realtime as offline, through each port.
    const auto dry = dryTone();
    const auto through = [](talas::project::ProjectNode process) {
        return render(graph({tone("osc", "441 hz"), std::move(process)},
            {{"osc", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}}));
    };
    std::vector<double> expected(frames);
    talas::cdp::SilenceGate gate;
    require(gate.prepare(sampleRate, -20.0) && gate.process(dry, expected), "the gate port should play");
    require(through(node("process", "talas.cdpSilenceGate", {expression("gate", "-20")})) == expected,
        "CDP Silence Gate should play its talas_cdp port");
    talas::cdp::Deglitch deglitch;
    require(deglitch.prepare(sampleRate, 2.0, 10.0, 0.01, 1.0, 1.0) && deglitch.process(dry, expected),
        "the deglitch port should play");
    require(through(node("process", "talas.cdpDeglitch")) == expected, "CDP Deglitch should play its talas_cdp port");
    talas::cdp::NoiseCut noiseCut;
    require(noiseCut.prepare(sampleRate, 2.0, 6000.0, 5.0, 15.0) && noiseCut.process(dry, expected),
        "the noisecut port should play");
    require(through(node("process", "talas.cdpNoiseCut")) == expected, "CDP Noise Cut should play its talas_cdp port");
    talas::cdp::AttackBoost attack;
    require(attack.prepare(sampleRate, talas::cdp::AttackBoost::Mode::Gate, 0.5, 3.0, 5.0, 10.0,
                talas::cdp::AttackBoost::Envelope::Linear)
            && attack.process(dry, expected),
        "the envel attack port should play");
    require(through(node("process", "talas.cdpAttackBoost",
                {expression("level", "0.5"), expression("gain", "3"), expression("onset", "5 ms"),
                    expression("decay", "10 ms"), linear}))
            == expected,
        "CDP Attack Boost should play its talas_cdp port");

    // Mixed with its dry input, the gated branch stays aligned: the dry
    // branch is delayed by the gate's two splices.
    const auto mixed = render(graph(
        {tone("osc", "441 hz"), node("process", "talas.cdpSilenceGate", {expression("gate", "-20")}),
            node("mix", "talas.mixerV2")},
        {{"osc", "audioOut", "process", "audioIn"},
            {"osc", "audioOut", "mix", "inputs"},
            {"process", "audioOut", "mix", "inputs"},
            {"mix", "leftOut", "out", "audioIn"}}));
    talas::cdp::SilenceGate aligned;
    std::vector<double> gated(frames);
    require(aligned.prepare(sampleRate, -20.0) && aligned.latencyFrames() == 96 && aligned.process(dry, gated),
        "the gate port should lag two 48-frame splices");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto delayed = frame >= 96 ? dry[frame - 96] : 0.0;
        require(std::abs(mixed[frame] - (delayed + gated[frame])) <= 1.0e-12,
            "frame " + std::to_string(frame) + " should add the aligned dry and gated signals");
    }
}

// CDP's submix merge, faders, interleave and inbetween and standnew repair
// with the Input as CDP's first file and the Side Chain as its second:
// offline on CDP8's runs bit for bit, interleave on both channels and faders
// with a gain that changes as CDP8's breakpoint file steps, and alike in
// realtime through each port.
void testSubmixPairNodesPlayTheirPorts()
{
    // CDP8's input, side chain and output, played offline; a stereo
    // reference's two output channels from the left and right outputs.
    const auto played = [](std::string fixture, talas::project::ProjectNode process, bool stereo,
                            std::vector<talas::runtime::EventValueBinding> values,
                            std::vector<talas::runtime::EventValueChange> changes) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> left;
        std::vector<double> right;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            left.push_back(std::strtod(end, &end));
            if (stereo)
                right.push_back(std::strtod(end, &end));
        }
        require(input.size() == 4096, "CDP reference " + fixture + " should hold 4096 frames");
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        std::vector<talas::core::Connection> connections{
            {"in", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"}};
        if (stereo)
        {
            connections.push_back({"process", "leftOut", "out", "audioIn"});
            connections.push_back({"process", "rightOut", "out", "rightIn"});
        }
        else
        {
            connections.push_back({"process", "audioOut", "out", "audioIn"});
        }
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(process)},
            std::move(connections));
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), fixture + " should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), fixture + " should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return sameSamples(offline.mono, left) && (!stereo || sameSamples(offline.right, right));
    };
    const auto textField = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    Parameter countMode;
    countMode.parameterId = "mode";
    countMode.choice = "count";
    require(played("submix-merge-default", node("process", "talas.cdpMixMerge"), false, {}, {})
            && played("submix-merge-skew-0.2", node("process", "talas.cdpMixMerge", {expression("skew", "0.2")}), false,
                {}, {})
            && played("submix-merge-stagger-0.0123-skew-0.5",
                node("process", "talas.cdpMixMerge", {expression("stagger", "0.0123 s"), expression("skew", "0.5")}),
                false, {}, {})
            && played("submix-merge-stagger-0.03-skip-0.011-skew-1.4",
                node("process", "talas.cdpMixMerge",
                    {expression("stagger", "0.03 s"), expression("skip", "0.011 s"), expression("skew", "1.4")}),
                false, {}, {})
            && played("submix-merge-stagger-0.016-skip-0.016-skew-4",
                node("process", "talas.cdpMixMerge",
                    {expression("stagger", "0.016 s"), expression("skip", "0.016 s"), expression("skew", "4")}),
                false, {}, {}),
        "CDP Mix Merge should match CDP8's submix merge, its side chain skewed, staggered and skipped");
    require(played("submix-faders-ramps-gain-0.8",
                node("process", "talas.cdpMixFaders",
                    {textField("balance", "0 1 0\n0.0123 1 0\n0.03331 0.2 0.8\n0.06 3 1\n0.07 0 2"),
                        expression("gain", "0.8")}),
                false, {}, {})
            && played("submix-faders-unordered-times",
                node("process", "talas.cdpMixFaders",
                    {textField("balance", "; the times need not rise\n0 1 0\n0.05 0 1\n\n  0.02 1 1\n0.07 1 3\n"),
                        expression("gain", "1.3")}),
                false, {}, {})
            && played("submix-faders-one-line", node("process", "talas.cdpMixFaders", {textField("balance", "0 2 3")}),
                false, {}, {}),
        "CDP Mix Faders should match CDP8's submix faders, following its balance table line by line as CDP8 does");
    require(played("submix-faders-live-gain",
                node("process", "talas.cdpMixFaders",
                    {textField("balance", "0 1 1\n0.05 0.3 0.7"), expression("gain", "!Gain")}),
                false, {{"Gain", 1.0}},
                {eventValueChange(1024, "Gain", 0.5), eventValueChange(2048, "Gain", 2.5),
                    eventValueChange(3072, "Gain", 0.0)}),
        "CDP Mix Faders should follow its gain as CDP8 follows a breakpoint file");
    require(played("submix-interleave", node("process", "talas.cdpMixInterleave"), true, {}, {})
            && played("repair-2-channels", node("process", "talas.cdpMixInterleave"), true, {}, {}),
        "CDP Mix Interleave should match CDP8's submix interleave and repair on both channels");
    require(played("submix-inbetween-1-count-11-file-5",
                node("process", "talas.cdpMixInbetween", {countMode, expression("count", "11"), expression("file", "5")}),
                false, {}, {})
            && played("submix-inbetween-2-ratio-0.3", node("process", "talas.cdpMixInbetween", {expression("ratio", "0.3")}),
                false, {}, {}),
        "CDP Mix Inbetween should match CDP8's submix inbetween 1 and 2");

    // In realtime as offline, through each port, with a quieter side chain.
    const auto quiet = [] {
        return node("side", "talas.oscillator", {expression("frequency", "300 hz"), expression("amplitude", "0.05")});
    };
    const auto dry = dryTone();
    const auto drySide = render(graph({quiet()}, {{"side", "audioOut", "out", "audioIn"}}));
    const auto through = [&](talas::project::ProjectNode process, talas::runtime::RenderSettings settings, bool stereo) {
        std::vector<talas::core::Connection> connections{
            {"osc", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"}};
        if (stereo)
        {
            connections.push_back({"process", "leftOut", "out", "audioIn"});
            connections.push_back({"process", "rightOut", "out", "rightIn"});
        }
        else
        {
            connections.push_back({"process", "audioOut", "out", "audioIn"});
        }
        return render(graph({tone("osc", "441 hz"), quiet(), std::move(process)}, std::move(connections)), settings);
    };
    std::vector<double> expected(frames);
    talas::cdp::MixMerge merge;
    // 0.012 s is 576 frames, 512 in sectors, and 0.0054 s 259, 256: the
    // input alone until frame 512, then the side chain 256 frames late.
    require(merge.prepare(sampleRate, 0.012, 2.0, 0.0054) && merge.process(dry, drySide, expected),
        "the merge port should play");
    const auto merged = through(node("process", "talas.cdpMixMerge",
                                    {expression("stagger", "0.012 s"), expression("skew", "2"),
                                        expression("skip", "0.0054 s")}),
        {sampleRate, 64}, false);
    require(merged == expected
            && merged[100] == static_cast<double>(static_cast<float>(
                   static_cast<double>(static_cast<float>(dry[100])) * (2.0 / (2.0 + 1.0)))),
        "CDP Mix Merge should play its talas_cdp port, the input alone until the stagger");
    talas::cdp::MixFaders faders;
    const std::vector<talas::cdp::MixFaders::Line> balance{{0.0, 1.0, 0.0}, {0.01, 0.2, 0.8}, {0.03, 1.0, 1.0}};
    require(faders.prepare(sampleRate, balance, 0.7) && faders.process(dry, drySide, expected),
        "the faders port should play");
    talas::runtime::RenderSettings live{sampleRate, 64};
    live.eventValues = {{"Gain", 0.7}};
    require(through(node("process", "talas.cdpMixFaders",
                        {textField("balance", "0 1 0\n0.01 0.2 0.8\n0.03 1 1"), expression("gain", "!Gain")}),
                live, false)
            == expected,
        "CDP Mix Faders should play a live gain through its talas_cdp port");
    talas::cdp::MixInterleave interleave;
    std::vector<double> expectedRight(frames);
    require(interleave.prepare() && interleave.process(dry, drySide, expected, expectedRight),
        "the interleave port should play");
    const auto interleaved = through(node("process", "talas.cdpMixInterleave"), {sampleRate, 64}, true);
    require(interleaved == expected && lastRight == expectedRight && interleaved != expectedRight,
        "CDP Mix Interleave should play its talas_cdp port, the input left and the side chain right");
    talas::cdp::MixInbetween inbetween;
    require(inbetween.prepareCount(4, 3) && inbetween.process(dry, drySide, expected),
        "the inbetween port should play");
    require(through(node("process", "talas.cdpMixInbetween",
                        {countMode, expression("count", "4"), expression("file", "3")}),
                {sampleRate, 64}, false)
            == expected,
        "CDP Mix Inbetween should play its talas_cdp port");
}

// CDP's envel warp, impose and replace with 5 ms windows (256 frames at 48
// kHz, as CDP8's 5.333333 ms are): offline on CDP8's runs bit for bit, two
// windows less a frame late and the windows a mode reads ahead, in every warp
// mode, with live fields as CDP8's breakpoint files step and the side chain's
// envelope imposed or put in place of the input's; alike in realtime through
// each port.
void testEnvelopeNodesPlayTheirPorts()
{
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto played = [](const std::string& fixture, talas::project::ProjectNode process, std::size_t latency,
                            talas::runtime::RenderSettings settings) {
        const auto cdp = cdpReference(fixture);
        const auto document = graph({node("in", "talas.audioInput"), std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}});
        return sameSamples(renderCdpInputLagging(document, std::move(settings), cdp, latency, cdp.output.size()),
            cdp.output);
    };
    const auto warp = [&](std::string mode, std::vector<Parameter> fields) {
        fields.push_back(choice("mode", std::move(mode)));
        return node("process", "talas.cdpEnvelopeWarp", std::move(fields));
    };
    const talas::runtime::RenderSettings plain{sampleRate, 64};
    require(played("envel-warp-3-exaggerate-2", warp("exaggerate", {expression("first", "2")}), 511, plain)
            && played("envel-warp-4-attenuate-0.5", warp("attenuate", {expression("first", "0.5")}), 511, plain)
            && played("envel-warp-5-lift-0.2", warp("lift", {expression("first", "0.2")}), 511, plain)
            && played("envel-warp-7-flatten-5", warp("flatten", {expression("first", "5")}), 2 * 256 + 511, plain)
            && played("envel-warp-8-gate-0.05-smoothing-4",
                warp("gate", {expression("first", "0.05"), expression("smoothing", "4")}), 3 * 256 + 511, plain)
            && played("envel-warp-9-invert-0.25-0.5",
                warp("invert", {expression("first", "0.25"), expression("second", "0.5")}), 511, plain)
            && played("envel-warp-10-limit-0.3-threshold-0.1",
                warp("limit", {expression("first", "0.3"), expression("second", "0.1")}), 511, plain)
            && played("envel-warp-11-corrugate-3-5",
                warp("corrugate", {expression("first", "3"), expression("second", "5")}), 4 * 256 + 511, plain)
            && played("envel-warp-12-expand-0.01-0.2",
                warp("expand", {expression("first", "0.01"), expression("second", "0.2")}), 511, plain)
            && played("envel-warp-15-ducked-0.1-0.3",
                warp("ducked", {expression("first", "0.1"), expression("second", "0.3")}), 511, plain),
        "CDP Envelope Warp should match CDP8's envel warp in each of its modes, its windows' latency late");
    require(played("envel-warp-5-lift-0.6-overload", warp("lift", {expression("first", "0.6")}), 511, plain)
            && played("envel-warp-5-lift-0.2-rate-22050", warp("lift", {expression("first", "0.2")}), 255,
                {22050.0, 64}),
        "CDP Envelope Warp should clip where CDP8 rescales its file, and take 128-frame windows at 22.05 kHz");

    // CDP8's breakpoint files: the first parameter's steps at frames 1280,
    // 3072, 5376 and 6656, the second's at 768, 2304, 4608 and 6144.
    const auto steps = [](std::string name, std::array<double, 5> values, bool second) {
        const std::array<std::uint64_t, 4> frames = second ? std::array<std::uint64_t, 4>{768, 2304, 4608, 6144}
                                                            : std::array<std::uint64_t, 4>{1280, 3072, 5376, 6656};
        std::vector<talas::runtime::EventValueChange> changes;
        for (std::size_t step = 0; step < 4; ++step)
            changes.push_back(eventValueChange(frames[step], name, values[step + 1]));
        return changes;
    };
    const auto live = [&](std::vector<talas::runtime::EventValueBinding> values,
                          std::vector<talas::runtime::EventValueChange> changes) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return settings;
    };
    auto limitChanges = steps("First", {0.5, 0.2, 0.35, 0.15, 0.4}, false);
    for (auto change : steps("Second", {0.1, 0.05, 0.12, 0.15, 0.02}, true))
        limitChanges.push_back(std::move(change));
    std::ranges::sort(limitChanges, {}, &talas::runtime::EventValueChange::frame);
    require(played("envel-warp-10-live-limit",
                warp("limit", {expression("first", "!First"), expression("second", "!Second")}), 511,
                live({{"First", 0.5}, {"Second", 0.1}}, limitChanges))
            && played("envel-warp-7-live-flatten",
                warp("flatten", {expression("first", "!First"), expression("maximumFirst", "7")}), 3 * 256 + 511,
                live({{"First", 2.0}}, steps("First", {2.0, 7.0, 3.0, 6.0, 4.0}, false))),
        "CDP Envelope Warp should follow a live first and second as CDP8 follows its breakpoint files, a live flatten reading ahead as far as its maximum");

    // The input and CDP's second sound, the side chain, played offline,
    // then silence for the latency.
    const auto imposed = [](const std::string& fixture, talas::project::ProjectNode process, std::size_t latency) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<double> input;
        std::vector<double> side;
        std::vector<double> output;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            char* end = nullptr;
            input.push_back(std::strtod(line.c_str(), &end));
            side.push_back(std::strtod(end, &end));
            output.push_back(std::strtod(end, &end));
        }
        require(input.size() == 8192, "CDP reference " + fixture + " should hold 8192 frames");
        input.resize(input.size() + latency, 0.0);
        side.resize(side.size() + latency, 0.0);
        Parameter second;
        second.parameterId = "channel1";
        second.boolean = false;
        Parameter secondOn;
        secondOn.parameterId = "channel2";
        secondOn.boolean = true;
        const auto document = graph({node("in", "talas.audioInput"), node("side", "talas.audioInput", {second, secondOn}),
                                        std::move(process)},
            {{"in", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"},
                {"process", "audioOut", "out", "audioIn"}});
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.audioInputChannels.push_back({1, input});
        settings.audioInputChannels.push_back({2, side});
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), fixture + " should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), fixture + " should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        return std::all_of(offline.mono.begin(), offline.mono.begin() + static_cast<std::ptrdiff_t>(latency),
                   [](double sample) { return sample == 0.0; })
            && sameSamples(std::vector<double>(offline.mono.begin() + static_cast<std::ptrdiff_t>(latency),
                               offline.mono.end()),
                output);
    };
    require(imposed("envel-impose-1", node("process", "talas.cdpEnvelopeImpose"), 511)
            && imposed("envel-impose-1-window-10ms",
                node("process", "talas.cdpEnvelopeImpose", {expression("window", "10 ms")}), 1023)
            && imposed("envel-replace-1", node("process", "talas.cdpEnvelopeImpose", {choice("mode", "replace")}), 511),
        "CDP Envelope Impose should match CDP8's envel impose 1 and replace 1 with the side chain as its second sound");

    require(played("envel-replace-3-target-0.3", node("process", "talas.cdpEnvelopeTarget", {expression("target", "0.3")}),
                511, plain)
            && played("envel-replace-4-live-decibels",
                node("process", "talas.cdpEnvelopeTarget", {choice("scale", "decibels"), expression("target", "!Target")}),
                511, live({{"Target", -14.0}}, steps("Target", {-14.0, -10.0, -96.0, -12.0, -20.0}, false))),
        "CDP Envelope Target should match CDP8's envel replace 3 and 4, following a live target as CDP8 samples its breakpoint file");

    // In realtime as offline, through each port, a live flatten waiting as
    // long as its maximum reads ahead.
    const auto dry = dryTone();
    const auto quiet = [] {
        return node("side", "talas.oscillator", {expression("frequency", "300 hz"), expression("amplitude", "0.05")});
    };
    const auto drySide = render(graph({quiet()}, {{"side", "audioOut", "out", "audioIn"}}));
    const auto through = [](talas::project::ProjectNode process, talas::runtime::RenderSettings settings) {
        return render(graph({tone("osc", "441 hz"), std::move(process)},
                          {{"osc", "audioOut", "process", "audioIn"}, {"process", "audioOut", "out", "audioIn"}}),
            settings);
    };
    using Mode = talas::cdp::EnvelopeWarp::Mode;
    std::vector<double> expected(frames);
    talas::cdp::EnvelopeWarp flatten;
    require(flatten.prepare(sampleRate, Mode::Flatten, 5.0, 3.0, 0.0, 0, 6) && flatten.latencyFrames() == 4 * 256 - 1
            && flatten.process(dry, expected),
        "the envel warp port should play a flatten reading ahead to 6 windows");
    require(through(warp("flatten", {expression("first", "!First"), expression("maximumFirst", "6")}),
                live({{"First", 3.0}}, {}))
            == expected,
        "CDP Envelope Warp should play a live flatten through its talas_cdp port, as late as its maximum reads ahead");
    talas::cdp::EnvelopeWarp lift;
    require(lift.prepare(sampleRate, Mode::Lift, 8.0, 0.3) && lift.process(dry, expected), "the lift port should play");
    require(through(warp("lift", {expression("first", "0.3"), expression("window", "8 ms")}), plain) == expected,
        "CDP Envelope Warp should play its talas_cdp port with 512-frame windows");
    talas::cdp::EnvelopeImpose replace;
    require(replace.prepare(sampleRate, talas::cdp::EnvelopeImpose::Mode::Replace, 10.0)
            && replace.process(dry, drySide, expected),
        "the envel replace 1 port should play");
    require(render(graph({tone("osc", "441 hz"), quiet(),
                             node("process", "talas.cdpEnvelopeImpose",
                                 {choice("mode", "replace"), expression("window", "10 ms")})},
                       {{"osc", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"},
                           {"process", "audioOut", "out", "audioIn"}}))
            == expected,
        "CDP Envelope Impose should play its talas_cdp port, the side chain's envelope in place of the input's");
    talas::cdp::EnvelopeTarget target;
    require(target.prepare(sampleRate, talas::cdp::EnvelopeTarget::Mode::Decibels, 5.0, -6.0)
            && target.process(dry, expected),
        "the envel replace 4 port should play");
    require(through(node("process", "talas.cdpEnvelopeTarget", {choice("scale", "decibels"), expression("target", "!Target")}),
                live({{"Target", -6.0}}, {}))
            == expected,
        "CDP Envelope Target should play a live target through its talas_cdp port");
}

// CDP's newdelay, bounce, rejoin, cascade 1 to 3, phasor on one or two
// output channels and spin stereo 1: offline on CDP8's runs bit for bit, each
// output channel, a live pitch and a live frequency and shift as CDP8's
// breakpoint files step, cascade before the clip CDP8 shortens, and clipped
// where CDP8 rescales its file; alike in realtime through each port.
void testRescaleNodesPlayTheirPorts()
{
    const auto choice = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    // A reference's columns: its inputs, then each CDP output channel.
    const auto columnsOf = [](const std::string& fixture) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<std::vector<double>> columns;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line.front() == '#')
                continue;
            const char* position = line.c_str();
            for (std::size_t column = 0;; ++column)
            {
                char* end = nullptr;
                const auto value = std::strtod(position, &end);
                if (end == position)
                    break;
                if (columns.size() <= column)
                    columns.emplace_back();
                columns[column].push_back(value);
                position = end;
            }
        }
        require(!columns.empty() && !columns[0].empty(), "CDP reference " + fixture + " should hold samples");
        return columns;
    };
    const auto clipped = [](std::vector<double> samples) {
        for (auto& sample : samples)
            sample = std::clamp(sample, -1.0, 1.0);
        return samples;
    };
    // The process played offline on one input, or two (on audio input
    // channels 1 and 2, the second its sideChain or rightIn), then silence
    // for its latency: its output channels (audioOut, or leftOut and
    // rightOut) from the latency on, to the given frames.
    const auto played = [](talas::project::ProjectNode process, std::vector<std::vector<double>> inputs,
                            std::string secondInput, bool stereo, std::size_t latency, std::size_t compared,
                            talas::runtime::RenderSettings settings) {
        std::vector<talas::project::ProjectNode> nodes{node("in", "talas.audioInput")};
        std::vector<talas::core::Connection> connections{{"in", "audioOut", "process", "audioIn"}};
        if (inputs.size() == 2)
        {
            Parameter first;
            first.parameterId = "channel1";
            first.boolean = false;
            Parameter second;
            second.parameterId = "channel2";
            second.boolean = true;
            nodes.push_back(node("second", "talas.audioInput", {first, second}));
            connections.push_back({"second", "audioOut", "process", secondInput});
        }
        nodes.push_back(std::move(process));
        if (stereo)
        {
            connections.push_back({"process", "leftOut", "out", "audioIn"});
            connections.push_back({"process", "rightOut", "out", "rightIn"});
        }
        else
        {
            connections.push_back({"process", "audioOut", "out", "audioIn"});
        }
        const auto length = inputs[0].size() + latency;
        for (std::size_t index = 0; index < inputs.size(); ++index)
        {
            inputs[index].resize(length, 0.0);
            settings.audioInputChannels.push_back({index + 1, inputs[index]});
        }
        const auto compiled = talas::runtime::compileProject(
            graph(std::move(nodes), std::move(connections)), talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, length);
        require(offline.ok(), "CDP graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        require(std::all_of(offline.mono.begin(), offline.mono.begin() + static_cast<std::ptrdiff_t>(latency),
                    [](double sample) { return sample == 0.0; }),
            "a CDP process should start with its latency's silence");
        const auto from = [&](const std::vector<double>& channel) {
            return std::vector<double>(channel.begin() + static_cast<std::ptrdiff_t>(latency),
                channel.begin() + static_cast<std::ptrdiff_t>(latency + compared));
        };
        std::vector<std::vector<double>> channels{from(offline.mono)};
        if (stereo)
            channels.push_back(from(offline.right.empty() ? offline.mono : offline.right));
        return channels;
    };
    const auto first = [](const std::vector<double>& samples, std::size_t frames) {
        return std::vector<double>(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(frames));
    };
    const talas::runtime::RenderSettings plain{sampleRate, 64};
    const auto withValues = [](std::vector<talas::runtime::EventValueBinding> values,
                                std::vector<talas::runtime::EventValueChange> changes) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return settings;
    };

    // newdelay: a mono input and output.
    const auto delay = [&](std::string fixture, std::vector<Parameter> fields, bool overload,
                           talas::runtime::RenderSettings settings) {
        const auto cdp = columnsOf(fixture);
        const auto output = played(node("process", "talas.cdpPitchedDelay", std::move(fields)), {cdp[0]}, {}, false, 0,
            cdp[0].size(), std::move(settings));
        return sameSamples(output[0], overload ? clipped(cdp[1]) : cdp[1]);
    };
    require(delay("newdelay-60-mix-0.5-feedback-0.7", {expression("pitch", "60")}, false, plain)
            && delay("newdelay-45.5-mix-0.8-feedback-minus-0.6",
                {expression("pitch", "45.5"), expression("mix", "0.8"), expression("feedback", "-0.6")}, false, plain)
            && delay("newdelay-70-mix-0.6-feedback-0.9-rate-44100",
                {expression("pitch", "70"), expression("mix", "0.6"), expression("feedback", "0.9")}, false,
                {44100.0, 64})
            && delay("newdelay-64-mix-0.5-feedback-0.8-overload",
                {expression("pitch", "64"), expression("feedback", "0.8")}, true, plain),
        "CDP Pitched Delay should match CDP8's newdelay, clipped where CDP8 prescales its input");
    // CDP8's pitch steps just before the 64-frame blocks at 1024, 2240,
    // 3200, 4480, 5760 and 6400.
    std::vector<talas::runtime::EventValueChange> pitches;
    for (const auto& [frame, pitch] : {std::pair{1024, 72.0}, std::pair{2240, 40.0}, std::pair{3200, 81.0},
             std::pair{4480, 55.3}, std::pair{5760, 96.0}, std::pair{6400, 66.0}})
        pitches.push_back(eventValueChange(static_cast<std::uint64_t>(frame), "Pitch", pitch));
    require(delay("newdelay-live-pitch",
                {expression("pitch", "!Pitch"), expression("mix", "0.5"), expression("feedback", "0.75")}, false,
                withValues({{"Pitch", 60.0}}, pitches)),
        "CDP Pitched Delay should follow a live pitch as CDP8 follows its breakpoint file");

    // bounce: a mono input and output.
    const auto bounce = [&](std::string fixture, std::vector<Parameter> fields, bool overload) {
        const auto cdp = columnsOf(fixture);
        const auto output = played(node("process", "talas.cdpBounce", std::move(fields)), {cdp[0]}, {}, false, 0,
            cdp[0].size(), plain);
        return sameSamples(output[0], overload ? clipped(cdp[1]) : cdp[1]);
    };
    const auto bounceFields = [](std::string count, std::string gap, std::string shorten, std::string end,
                                  std::string warp) {
        return std::vector<Parameter>{expression("count", std::move(count)), expression("startGap", std::move(gap)),
            expression("shorten", std::move(shorten)), expression("endLevel", std::move(end)),
            expression("warp", std::move(warp))};
    };
    require(bounce("bounce-5-gap-0.05-shorten-0.8-end-0.1-warp-1", bounceFields("5", "0.05 s", "0.8", "0.1", "1"), false)
            && bounce("bounce-12-gap-0.04-shorten-0.9-end-0.05-warp-2-clicks",
                bounceFields("12", "0.04 s", "0.9", "0.05", "2"), false)
            && bounce("bounce-100-gap-0.04-shorten-0.95-end-0-warp-0.5-curtailed",
                bounceFields("100", "0.04 s", "0.95", "0", "0.5"), false)
            && bounce("bounce-6-gap-0.04-shorten-0.85-end-0.3-warp-1-overload",
                bounceFields("6", "0.04 s", "0.85", "0.3", "1"), true),
        "CDP Bounce should match CDP8's bounce -s0, clipped where CDP8 fades its output down");

    // rejoin: CDP's longest file the input and the other the side chain.
    const auto rejoin = [&](std::string fixture, std::string gain, bool overload) {
        const auto cdp = columnsOf(fixture);
        const auto output = played(node("process", "talas.cdpRejoin", {expression("gain", std::move(gain))}),
            {cdp[0], cdp[1]}, "sideChain", false, 0, cdp[0].size(), plain);
        return sameSamples(output[0], overload ? clipped(cdp[2]) : cdp[2]);
    };
    require(rejoin("rejoin-1-gain-1", "1", false) && rejoin("rejoin-1-gain-0.6", "0.6", false)
            && rejoin("rejoin-2-gain-0.35", "0.35", false) && rejoin("rejoin-1-gain-1-overload", "1", true),
        "CDP Rejoin should match CDP8's rejoin of two mono files, clipped where CDP8 scales its output");

    // cascade: echoes' one channel on both outputs; before the clip CDP8
    // shortens where its file ends.
    using CascadeMode = talas::cdp::Cascade::Mode;
    const auto cascade = [&](std::string fixture, std::vector<Parameter> fields, talas::cdp::Cascade::Settings settings,
                             bool overload) {
        const auto cdp = columnsOf(fixture);
        talas::cdp::Cascade port;
        require(port.prepare(sampleRate, settings), fixture + "'s port should prepare");
        auto upTo = cdp[0].size();
        std::vector<double> out(port.outputChannels());
        for (std::size_t frame = 0; frame < cdp[0].size(); ++frame)
        {
            if (port.readStart() + port.clipFrames() > cdp[0].size())
            {
                upTo = port.clipStart();
                break;
            }
            require(port.process(std::span<const double>{cdp[0]}.subspan(frame, 1), out), "cascade should process");
        }
        const auto output = played(node("process", "talas.cdpCascade", std::move(fields)), {cdp[0]}, {}, true,
            port.latencyFrames(), upTo, plain);
        const auto left = overload ? clipped(cdp[1]) : cdp[1];
        const auto& right = cdp.size() == 3 ? cdp[2] : left;
        return sameSamples(output[0], first(left, upTo)) && sameSamples(output[1], first(right, upTo));
    };
    const auto cascadeSettings = [](CascadeMode mode, double clip, int echoes, double clipMax = 0.0,
                                     int echoesMax = 0, double rand = 0.0, int seed = 1, bool linear = false) {
        talas::cdp::Cascade::Settings settings;
        settings.mode = mode;
        settings.clipSize = clip;
        settings.echoes = echoes;
        settings.clipMax = clipMax;
        settings.echoesMax = echoesMax;
        settings.rand = rand;
        settings.seed = seed;
        settings.linear = linear;
        return settings;
    };
    Parameter linear;
    linear.parameterId = "linear";
    linear.boolean = true;
    require(cascade("cascade-1-clip-0.0213333-echoes-3",
                {expression("clipSize", "0.021333333 s"), expression("echoes", "3")},
                cascadeSettings(CascadeMode::Echoes, 0.021333333, 3), false)
            && cascade("cascade-1-clip-0.0106667-echoes-8-linear",
                {expression("clipSize", "0.010666667 s"), linear},
                cascadeSettings(CascadeMode::Echoes, 0.010666667, 8, 0.0, 0, 0.0, 1, true), false)
            && cascade("cascade-1-clip-0.0213333-echoes-6-rand-0.5-seed-3",
                {expression("clipSize", "0.021333333 s"), expression("echoes", "6"), expression("rand", "0.5"),
                    expression("seed", "3")},
                cascadeSettings(CascadeMode::Echoes, 0.021333333, 6, 0.0, 0, 0.5, 3), false)
            && cascade("cascade-1-clip-0.0213333-echoes-12-overload",
                {expression("clipSize", "0.021333333 s"), expression("echoes", "12")},
                cascadeSettings(CascadeMode::Echoes, 0.021333333, 12), true),
        "CDP Cascade should match CDP8's cascade 1 on both outputs, clipped where CDP8 normalises its output");
    require(cascade("cascade-2-clip-0.0426667-echoes-4",
                {choice("mode", "panRight"), expression("clipSize", "0.042666667 s"), expression("echoes", "4")},
                cascadeSettings(CascadeMode::PanRight, 0.042666667, 4), false)
            && cascade("cascade-2-clip-0.0125-to-0.03125-echoes-2-to-6",
                {choice("mode", "panRight"), expression("clipSize", "0.0125 s"), expression("echoes", "2"),
                    expression("clipMax", "0.03125 s"), expression("echoesMax", "6")},
                cascadeSettings(CascadeMode::PanRight, 0.0125, 2, 0.03125, 6), false)
            && cascade("cascade-3-clip-0.0213333-echoes-5",
                {choice("mode", "alternate"), expression("clipSize", "0.021333333 s"), expression("echoes", "5")},
                cascadeSettings(CascadeMode::Alternate, 0.021333333, 5), false),
        "CDP Cascade should match CDP8's cascade 2 and 3 on its two outputs");

    // phasor: one output channel on both outputs, or two; its packet of the
    // lowest frequency and a frame late.
    const auto phasor = [&](std::string fixture, std::vector<Parameter> fields, std::size_t latency, bool overload,
                            talas::runtime::RenderSettings settings) {
        const auto cdp = columnsOf(fixture);
        const auto output = played(node("process", "talas.cdpPhasor", std::move(fields)), {cdp[0]}, {}, true, latency,
            cdp[0].size(), std::move(settings));
        const auto left = overload ? clipped(cdp[1]) : cdp[1];
        const auto& right = cdp.size() == 3 ? cdp[2] : left;
        return sameSamples(output[0], left) && sameSamples(output[1], right);
    };
    require(phasor("phasor-2-streams-10hz-shift-1-2-channels",
                {expression("frequency", "10 hz"), expression("shift", "1")}, 4801, false, plain)
            && phasor("phasor-4-streams-20hz-shift-3-2-channels-offset-10",
                {expression("streams", "4"), expression("frequency", "20 hz"), expression("shift", "3"),
                    expression("offset", "10 ms")},
                2401, false, plain)
            && phasor("phasor-2-streams-100hz-shift-12-1-channel",
                {expression("frequency", "100 hz"), expression("shift", "12"), expression("outputChannels", "1")}, 481,
                false, plain)
            && phasor("phasor-8-streams-10hz-shift-1-1-channel-overload",
                {expression("streams", "8"), expression("frequency", "10 hz"), expression("shift", "1"),
                    expression("outputChannels", "1")},
                4801, true, plain),
        "CDP Phasor should match CDP8's phasor on one output channel, on both outputs, and on two, clipped where CDP8 normalises its output");
    // CDP8 reads Phasfrq and Shift as each packet starts, at a time that
    // advances by 1 / Phasfrq a packet: each packet's values set at the
    // 64-frame block it starts in.
    {
        const auto fixture = std::string{"phasor-4-streams-live-phasfrq-and-shift-2-channels"};
        std::vector<std::pair<double, double>> frequencySteps;
        std::vector<std::pair<double, double>> shiftSteps;
        {
            std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
            std::string line;
            while (std::getline(file, line))
            {
                const auto read = [&](const std::string& label, std::vector<std::pair<double, double>>& steps) {
                    if (line.rfind(label, 0) != 0)
                        return;
                    std::istringstream items(line.substr(label.size()));
                    std::string item;
                    while (std::getline(items, item, '/'))
                    {
                        char* end = nullptr;
                        const auto value = std::strtod(item.c_str(), &end);
                        const auto at = item.find("frame");
                        require(at != std::string::npos, fixture + "'s breakpoints should be readable");
                        steps.emplace_back(std::strtod(item.c_str() + at + 5, nullptr) / sampleRate, value);
                    }
                };
                read("# phasfrq.brk: ", frequencySteps);
                read("# shift.brk: ", shiftSteps);
            }
        }
        require(!frequencySteps.empty() && !shiftSteps.empty(), fixture + " should record its breakpoints");
        // read_value_from_brktable.
        const auto at = [](const std::vector<std::pair<double, double>>& steps, double time) {
            if (time <= steps.front().first)
                return steps.front().second;
            if (time >= steps.back().first)
                return steps.back().second;
            std::size_t point = 0;
            while (steps[point].first < time)
                ++point;
            auto value = (time - steps[point - 1].first) / (steps[point].first - steps[point - 1].first);
            value *= (steps[point].second - steps[point - 1].second);
            value += steps[point - 1].second;
            return value;
        };
        std::vector<talas::runtime::EventValueChange> changes;
        double time = 0.0;
        double lastFrequency = at(frequencySteps, 0.0);
        double lastShift = at(shiftSteps, 0.0);
        for (std::size_t start = 0; start < 32768;)
        {
            const auto frequency = at(frequencySteps, time);
            const auto shift = at(shiftSteps, time);
            const auto block = static_cast<std::uint64_t>(start / 64 * 64);
            if (frequency != lastFrequency)
                changes.push_back(eventValueChange(block, "Frequency", frequency));
            if (shift != lastShift)
                changes.push_back(eventValueChange(block, "Shift", shift));
            lastFrequency = frequency;
            lastShift = shift;
            start += talas::cdp::Phasor::packetFrames(sampleRate, frequency);
            time += 1.0 / frequency;
        }
        require(phasor(fixture,
                    {expression("streams", "4"), expression("frequency", "!Frequency * (1 hz)"),
                        expression("shift", "!Shift"), expression("lowestFrequency", "10 hz")},
                    4801, false, withValues({{"Frequency", 10.0}, {"Shift", 2.0}}, changes)),
            "CDP Phasor should latch a live frequency and shift as each packet starts, as CDP8 reads its breakpoint files, waiting for its lowest frequency's packet");
    }

    // spin: the left and right inputs, whose half-cycle flag CDP8's level
    // pass leaves clear.
    const auto spin = [&](std::string fixture, std::vector<Parameter> fields, talas::runtime::RenderSettings settings) {
        const auto cdp = columnsOf(fixture);
        const auto output = played(node("process", "talas.cdpSpin", std::move(fields)), {cdp[0], cdp[1]}, "rightIn", true,
            0, cdp[0].size(), std::move(settings));
        return sameSamples(output[0], cdp[2]) && sameSamples(output[1], cdp[3]);
    };
    require(spin("spin-stereo-1-rate-2-boost-0", {expression("rate", "2"), expression("boost", "0")}, plain)
            && spin("spin-stereo-1-rate-11-boost-1.5-rate-44100", {expression("rate", "11"), expression("boost", "1.5")},
                {44100.0, 64}),
        "CDP Spin should match CDP8's spin stereo 1 without doppler where its level pass ends its half-cycle clear");

    // In realtime as offline, through each port, with live fields.
    const auto dry = dryTone();
    const auto quiet = [] {
        return node("side", "talas.oscillator", {expression("frequency", "300 hz"), expression("amplitude", "0.05")});
    };
    const auto drySide = render(graph({quiet()}, {{"side", "audioOut", "out", "audioIn"}}));
    const auto through = [&](talas::project::ProjectNode process, std::string secondInput, bool stereo,
                             talas::runtime::RenderSettings settings) {
        std::vector<talas::project::ProjectNode> nodes{tone("osc", "441 hz")};
        std::vector<talas::core::Connection> connections{{"osc", "audioOut", "process", "audioIn"}};
        if (!secondInput.empty())
        {
            nodes.push_back(quiet());
            connections.push_back({"side", "audioOut", "process", secondInput});
        }
        nodes.push_back(std::move(process));
        if (stereo)
        {
            connections.push_back({"process", "leftOut", "out", "audioIn"});
            connections.push_back({"process", "rightOut", "out", "rightIn"});
        }
        else
        {
            connections.push_back({"process", "audioOut", "out", "audioIn"});
        }
        return render(graph(std::move(nodes), std::move(connections)), settings);
    };
    std::vector<double> expected(frames);
    std::vector<double> expectedRight(frames);
    talas::cdp::PitchedDelay pitchedDelay;
    require(pitchedDelay.prepare(sampleRate, 57.0, 0.5, 0.7, 0.0) && pitchedDelay.process(dry, expected),
        "the newdelay port should play");
    require(through(node("process", "talas.cdpPitchedDelay", {expression("pitch", "!Pitch")}), {}, false,
                withValues({{"Pitch", 57.0}}, {}))
            == expected,
        "CDP Pitched Delay should play a live pitch through its talas_cdp port");
    talas::cdp::Bounce bounces;
    require(bounces.prepare(sampleRate, 4, 0.04, 0.8, 0.2, 1.0) && bounces.process(dry, expected),
        "the bounce port should play");
    require(through(node("process", "talas.cdpBounce",
                        {expression("count", "4"), expression("startGap", "0.04 s"), expression("shorten", "0.8"),
                            expression("endLevel", "0.2")}),
                {}, false, plain)
            == expected,
        "CDP Bounce should play its talas_cdp port");
    talas::cdp::Rejoin rejoined;
    require(rejoined.prepare(0.7) && rejoined.process(dry, drySide, expected), "the rejoin port should play");
    require(through(node("process", "talas.cdpRejoin", {expression("gain", "0.7")}), "sideChain", false, plain) == expected,
        "CDP Rejoin should play its talas_cdp port");
    // The interleaving ports' channels.
    const auto deinterleave = [&](const std::vector<double>& interleaved, std::size_t channels) {
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            expected[frame] = interleaved[frame * channels];
            expectedRight[frame] = interleaved[frame * channels + channels - 1];
        }
    };
    talas::cdp::Cascade cascaded;
    std::vector<double> interleaved(2 * frames);
    require(cascaded.prepare(sampleRate, cascadeSettings(CascadeMode::PanRight, 0.01, 3))
            && cascaded.process(dry, interleaved),
        "the cascade port should play");
    deinterleave(interleaved, 2);
    require(through(node("process", "talas.cdpCascade",
                        {choice("mode", "panRight"), expression("clipSize", "0.01 s"), expression("echoes", "3")}),
                {}, true, plain)
                == expected
            && lastRight == expectedRight && expected != expectedRight,
        "CDP Cascade should play its talas_cdp port in stereo");
    talas::cdp::Phasor phased;
    require(phased.prepare(sampleRate, 3, 100.0, 2.0, 1, 0.0, false, 50.0) && phased.latencyFrames() == 961
            && phased.process(dry, std::span{interleaved}.first(frames)),
        "the phasor port should play one channel, its lowest frequency's packet late");
    deinterleave(interleaved, 1);
    require(through(node("process", "talas.cdpPhasor",
                        {expression("streams", "3"), expression("frequency", "!Frequency * (1 hz)"),
                            expression("shift", "2"), expression("outputChannels", "1"),
                            expression("lowestFrequency", "50 hz")}),
                {}, true, withValues({{"Frequency", 100.0}}, {}))
                == expected
            && lastRight == expected,
        "CDP Phasor should play a live frequency through its talas_cdp port, one channel on both outputs");
    talas::cdp::Spin spun;
    require(spun.prepare(sampleRate, talas::cdp::Spin::Mode::Stereo, 3.0, 1.0, 0.2) && spun.process(dry, drySide, interleaved),
        "the spin port should play");
    deinterleave(interleaved, 2);
    require(through(node("process", "talas.cdpSpin",
                        {expression("rate", "!Rate"), expression("boost", "1"), expression("attenuation", "0.2")}),
                "rightIn", true, withValues({{"Rate", 3.0}}, {}))
                == expected
            && lastRight == expectedRight,
        "CDP Spin should play a live rate through its talas_cdp port");
}

// CDP's modify brassage 1, 3, 4 and 5 on a mono input: offline on CDP8's
// runs bit for bit, each output channel (one on both outputs, reverb's two),
// live fields following CDP8's breakpoint files where they step, and clipped
// where CDP8 renormalises its file; alike in realtime through the port.
void testBrassageNodePlaysItsPort()
{
    const auto choice = [](std::string value) {
        Parameter parameter;
        parameter.parameterId = "mode";
        parameter.choice = std::move(value);
        return parameter;
    };
    // A reference's columns (its input, then each CDP output channel) and
    // the seed CDP drew from.
    const auto columnsOf = [](const std::string& fixture, std::string& seed) {
        std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + fixture + ".txt");
        require(file.good(), "CDP reference " + fixture + " should be readable");
        std::vector<std::vector<double>> columns;
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            if (line.front() == '#')
            {
                const auto at = line.find("drand48 seeded with ");
                if (at != std::string::npos)
                    seed = std::to_string(std::strtoul(line.c_str() + at + 20, nullptr, 10));
                continue;
            }
            const char* position = line.c_str();
            for (std::size_t column = 0;; ++column)
            {
                char* end = nullptr;
                const auto value = std::strtod(position, &end);
                if (end == position)
                    break;
                if (columns.size() <= column)
                    columns.emplace_back();
                columns[column].push_back(value);
                position = end;
            }
        }
        require(columns.size() >= 2 && !columns[0].empty() && !seed.empty(),
            "CDP reference " + fixture + " should hold samples and its seed");
        return columns;
    };
    const auto clipped = [](std::vector<double> samples) {
        for (auto& sample : samples)
            sample = std::clamp(sample, -1.0, 1.0);
        return samples;
    };
    const auto withValues = [](double rate, std::vector<talas::runtime::EventValueBinding> values,
                                std::vector<talas::runtime::EventValueChange> changes) {
        talas::runtime::RenderSettings settings{rate, 64};
        settings.eventValues = std::move(values);
        settings.eventValueChanges = std::move(changes);
        return settings;
    };
    // CDP8's breakpoint files step just before frames 1024, 2560, 4096, 5632
    // and 7168 (from the first value at frame 0).
    const auto steps = [](std::string name, std::array<double, 6> values) {
        std::vector<talas::runtime::EventValueChange> changes;
        const std::array<std::uint64_t, 5> frames{1024, 2560, 4096, 5632, 7168};
        for (std::size_t step = 0; step < frames.size(); ++step)
            changes.push_back(eventValueChange(frames[step], name, values[step + 1]));
        return changes;
    };
    // The node on CDP8's input, then silence for its latency, offline: both
    // outputs from the latency on match CDP8's channel (one on both, or
    // reverb's two), its seed CDP8's.
    const auto brassage = [&](std::string fixture, std::vector<Parameter> fields, std::size_t latency, bool overload,
                              talas::runtime::RenderSettings settings) {
        std::string seed;
        const auto cdp = columnsOf(fixture, seed);
        fields.push_back(expression("seed", seed));
        std::vector<talas::project::ProjectNode> nodes{node("in", "talas.audioInput"),
            node("process", "talas.cdpBrassage", std::move(fields))};
        std::vector<talas::core::Connection> connections{{"in", "audioOut", "process", "audioIn"},
            {"process", "leftOut", "out", "audioIn"}, {"process", "rightOut", "out", "rightIn"}};
        auto input = cdp[0];
        const auto compared = input.size();
        input.resize(compared + latency, 0.0);
        settings.audioInputChannels.push_back({1, input});
        const auto compiled = talas::runtime::compileProject(
            graph(std::move(nodes), std::move(connections)), talas::core::makeBuiltinNodeRegistry(), settings);
        require(compiled.ok(), "CDP Brassage should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, input.size());
        require(offline.ok(), "CDP Brassage should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        require(std::all_of(offline.mono.begin(), offline.mono.begin() + static_cast<std::ptrdiff_t>(latency),
                    [](double sample) { return sample == 0.0; }),
            "CDP Brassage should start with its latency's silence");
        const auto from = [&](const std::vector<double>& channel) {
            return std::vector<double>(channel.begin() + static_cast<std::ptrdiff_t>(latency),
                channel.begin() + static_cast<std::ptrdiff_t>(latency + compared));
        };
        const auto left = overload ? clipped(cdp[1]) : cdp[1];
        const auto right = cdp.size() == 3 ? (overload ? clipped(cdp[2]) : cdp[2]) : left;
        return sameSamples(from(offline.mono), left)
            && sameSamples(from(offline.right.empty() ? offline.mono : offline.right), right);
    };
    const talas::runtime::RenderSettings plain{sampleRate, 64};
    const talas::runtime::RenderSettings plain44100{44100.0, 64};

    require(brassage("modify-brassage-1-pitch-3", {expression("pitch", "3")}, 2854, false, plain)
            && brassage("modify-brassage-1-pitch-12-rate-44100", {expression("pitch", "12")}, 4409, false, plain44100)
            && brassage("modify-brassage-1-pitch-7-memory-10",
                {expression("pitch", "7"), expression("memoryKilobytes", "10")}, 3596, false, plain)
            && brassage("modify-brassage-1-pitch-2-overload", {expression("pitch", "2")}, 2694, true, plain),
        "CDP Brassage should match CDP8's brassage 1 on both outputs, clipped where CDP8 renormalises its file");
    require(brassage("modify-brassage-1-live-pitch-memory-10",
                {expression("pitch", "!Pitch"), expression("highestPitch", "12"), expression("memoryKilobytes", "10")},
                4799, false, withValues(sampleRate, {{"Pitch", 3.0}}, steps("Pitch", {3, -5, 9, 0, 12, -12}))),
        "CDP Brassage should follow a live pitch at each grain's start, as CDP8 follows its breakpoint file, waiting for its highest pitch");

    require(brassage("modify-brassage-3-density-8-pitch-0.2-amp-0.3",
                {choice("reverb"), expression("density", "8"), expression("pitch", "0.2"), expression("amp", "0.3")},
                2428, false, plain)
            && brassage("modify-brassage-3-density-12-pitch-minus-0.33-amp-0.2-range-50-memory-10",
                {choice("reverb"), expression("density", "12"), expression("pitch", "-0.33"), expression("amp", "0.2"),
                    expression("range", "50 ms"), expression("memoryKilobytes", "10")},
                2355, false, plain)
            && brassage("modify-brassage-3-density-16-pitch-0.1-amp-1-overload",
                {choice("reverb"), expression("density", "16"), expression("pitch", "0.1")}, 2414, true, plain),
        "CDP Brassage should match CDP8's brassage 3 on its two outputs, clipped where CDP8 renormalises its file");
    {
        auto changes = steps("Density", {8, 4, 12, 3, 6, 16});
        for (const auto& [name, values] : {
                 std::pair{std::string{"Pitch"}, std::array<double, 6>{0.2, -0.3, 0.33, 0, -0.1, 0.25}},
                 std::pair{std::string{"Amp"}, std::array<double, 6>{0.3, 0.2, 0.15, 0.35, 0.25, 0.12}},
                 std::pair{std::string{"Range"}, std::array<double, 6>{0, 20, 50, 10, 35, 5}},
             })
        {
            const auto more = steps(name, values);
            changes.insert(changes.end(), more.begin(), more.end());
        }
        std::ranges::stable_sort(changes, {}, &talas::runtime::EventValueChange::frame);
        require(brassage("modify-brassage-3-live-memory-10",
                    {choice("reverb"), expression("density", "!Density"), expression("pitch", "!Pitch"),
                        expression("amp", "!Amp"), expression("range", "!Range / 1000 * (1 s)"),
                        expression("lowestDensity", "3"), expression("highestPitch", "0.33"),
                        expression("longestRange", "50 ms"), expression("memoryKilobytes", "10")},
                    2447, false,
                    withValues(sampleRate, {{"Density", 8.0}, {"Pitch", 0.2}, {"Amp", 0.3}, {"Range", 0.0}},
                        std::move(changes))),
            "CDP Brassage should follow a live reverb density, pitch, amp and range at each grain's start, as CDP8 follows its breakpoint files");
    }

    require(brassage("modify-brassage-4-grainsize-50", {choice("scramble")}, 2399, false, plain)
            && brassage("modify-brassage-4-grainsize-20-range-100",
                {choice("scramble"), expression("grainsize", "20 ms"), expression("range", "100 ms")}, 959, false,
                plain),
        "CDP Brassage should match CDP8's brassage 4 on both outputs");
    {
        auto changes = steps("Grainsize", {50, 10, 80, 5, 30, 2});
        const auto ranges = steps("Range", {0, 100, 30, 200, 0, 60});
        changes.insert(changes.end(), ranges.begin(), ranges.end());
        std::ranges::stable_sort(changes, {}, &talas::runtime::EventValueChange::frame);
        require(brassage("modify-brassage-4-live-grainsize-range",
                    {choice("scramble"), expression("grainsize", "!Grainsize / 1000 * (1 s)"),
                        expression("range", "!Range / 1000 * (1 s)"), expression("longestGrainsize", "80 ms"),
                        expression("longestRange", "200 ms")},
                    3839, false, withValues(sampleRate, {{"Grainsize", 50.0}, {"Range", 0.0}}, std::move(changes))),
            "CDP Brassage should follow a live scramble grainsize and range, waiting for its longest grain");
    }

    require(brassage("modify-brassage-5-density-1", {choice("granulate"), expression("density", "1")}, 2399, false,
                plain)
            && brassage("modify-brassage-5-density-0.7-rate-44100", {choice("granulate"), expression("density", "0.7")},
                2204, false, plain44100)
            && brassage("modify-brassage-5-live-density",
                {choice("granulate"), expression("density", "!Density"), expression("lowestDensity", "0.05")}, 2399,
                false, withValues(sampleRate, {{"Density", 1.0}}, steps("Density", {1, 0.3, 2, 0.7, 1.5, 0.05}))),
        "CDP Brassage should match CDP8's brassage 5 and follow a live density");

    // In realtime as offline, through the port, with live fields that step.
    constexpr std::size_t length = 8192;
    const auto dry = dryTone(length);
    const auto through = [&](std::vector<Parameter> fields, talas::runtime::RenderSettings settings) {
        return render(graph({tone("osc", "441 hz"), node("process", "talas.cdpBrassage", std::move(fields))},
                          {{"osc", "audioOut", "process", "audioIn"}, {"process", "leftOut", "out", "audioIn"},
                              {"process", "rightOut", "out", "rightIn"}}),
            std::move(settings), length);
    };
    // The port on the dry tone, the setter called as the block at frame
    // `at` starts.
    const auto ported = [&](talas::cdp::Brassage::Settings fields, std::size_t at,
                            const std::function<bool(talas::cdp::Brassage&)>& set) {
        talas::cdp::Brassage port;
        require(port.prepare(sampleRate, fields), "the brassage port should prepare");
        std::vector<double> interleaved(length * port.outputChannels());
        const auto channels = port.outputChannels();
        require((at == 0 || port.process(std::span{dry}.first(at), std::span{interleaved}.first(at * channels)))
                && set(port)
                && port.process(std::span{dry}.subspan(at), std::span{interleaved}.subspan(at * channels)),
            "the brassage port should play");
        std::vector<double> left(length);
        std::vector<double> right(length);
        for (std::size_t frame = 0; frame < length; ++frame)
        {
            left[frame] = interleaved[frame * channels];
            right[frame] = interleaved[frame * channels + channels - 1];
        }
        return std::pair{left, right};
    };
    using Mode = talas::cdp::Brassage::Mode;
    talas::cdp::Brassage::Settings fields;
    fields.mode = Mode::Pitchshift;
    fields.pitch = 4.0;
    fields.highestPitch = 7.0;
    fields.seed = 11;
    auto expected = ported(fields, 2048, [](talas::cdp::Brassage& port) { return port.setPitch(6.0); });
    require(through({expression("pitch", "!Pitch"), expression("highestPitch", "7"), expression("seed", "11")},
                withValues(sampleRate, {{"Pitch", 4.0}}, {eventValueChange(2048, "Pitch", 6.0)}))
                == expected.first
            && lastRight == expected.second && expected.first == expected.second,
        "CDP Brassage should play a live pitch through its talas_cdp port, one channel on both outputs");
    fields = {};
    fields.mode = Mode::Reverb;
    fields.density = 6.0;
    fields.pitch = 0.1;
    fields.amp = 0.5;
    fields.rangeMs = 10.0;
    fields.lowestDensity = 3.0;
    fields.seed = 3;
    expected = ported(fields, 3072, [](talas::cdp::Brassage& port) { return port.setDensity(3.0); });
    require(through({choice("reverb"), expression("density", "!Density"), expression("pitch", "0.1"),
                        expression("amp", "0.5"), expression("range", "10 ms"), expression("lowestDensity", "3"),
                        expression("seed", "3")},
                withValues(sampleRate, {{"Density", 6.0}}, {eventValueChange(3072, "Density", 3.0)}))
                == expected.first
            && lastRight == expected.second && expected.first != expected.second,
        "CDP Brassage should play a live reverb density through its talas_cdp port in stereo");
    fields = {};
    fields.mode = Mode::Scramble;
    fields.grainsizeMs = 20.0;
    fields.rangeMs = 5.0;
    fields.longestGrainsizeMs = 30.0;
    fields.seed = 5;
    expected = ported(fields, 1024, [](talas::cdp::Brassage& port) { return port.setGrainsize(30.0); });
    require(through({choice("scramble"), expression("grainsize", "!Grainsize / 1000 * (1 s)"),
                        expression("range", "5 ms"), expression("longestGrainsize", "30 ms"), expression("seed", "5")},
                withValues(sampleRate, {{"Grainsize", 20.0}}, {eventValueChange(1024, "Grainsize", 30.0)}))
                == expected.first,
        "CDP Brassage should play a live scramble grainsize through its talas_cdp port");
    fields = {};
    fields.mode = Mode::Granulate;
    fields.density = 1.5;
    fields.seed = 0;
    expected = ported(fields, 0, [](talas::cdp::Brassage&) { return true; });
    require(through({choice("granulate"), expression("density", "1.5"), expression("seed", "0")}, plain)
                == expected.first,
        "CDP Brassage should play its granulate through its talas_cdp port");
}

void testFixedFilterPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "band";
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("filter", "talas.cdpFixedFilter",
                {mode, expression("boost", "-8"), expression("frequency", "450 hz"),
                    expression("bandwidth", "120 hz"), expression("prescale", "0.9")})},
        {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}}));
    talas::cdp::FixedFilter port;
    require(port.prepare(sampleRate, talas::cdp::FixedFilter::Mode::Band, -8.0, 450.0, 120.0, 0.9),
        "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Fixed Filter should play its talas_cdp port");
}

void testPhasingPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "phaser";
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("phasing", "talas.cdpPhasing",
                {mode, expression("gain", "0.7"), expression("delay", "0.5 ms"),
                    expression("prescale", "0.9")})},
        {{"osc", "audioOut", "phasing", "audioIn"}, {"phasing", "audioOut", "out", "audioIn"}}));
    talas::cdp::Phasing port;
    require(port.prepare(sampleRate, talas::cdp::Phasing::Mode::Phaser, 0.7, 0.5, 0.9),
        "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Phasing should play its talas_cdp port");
}

void testLohiFilterPlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("filter", "talas.cdpLohiFilter",
                {expression("attenuation", "-50"), expression("passBand", "800 hz"),
                    expression("stopBand", "1400 hz"), expression("prescale", "0.8")})},
        {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}}));
    talas::cdp::LohiFilter port;
    require(port.prepare(sampleRate, -50.0, 800.0, 1400.0, 0.8), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Lohi Filter should play its talas_cdp port");
}

void testSweepingFilterPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "bandPass";
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("filter", "talas.cdpSweepingFilter",
                {mode, expression("acuity", "0.2"), expression("gain", "0.9"),
                    expression("lowFrequency", "200 hz"), expression("highFrequency", "2000 hz"),
                    expression("sweepFrequency", "30 hz"), expression("phase", "0.1")})},
        {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}}));
    talas::cdp::SweepingFilter port;
    require(port.prepare(sampleRate, talas::cdp::SweepingFilter::Mode::BandPass, 0.2, 0.9, 200.0, 2000.0, 30.0, 0.1),
        "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Sweeping Filter should play its talas_cdp port");
}

void testUserBankPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter pitches;
    pitches.parameterId = "pitches";
    pitches.choice = "midi";
    Parameter bands;
    bands.parameterId = "bands";
    bands.text = "57 1\n64 -6dB\n69 0.5";
    Parameter doubled;
    doubled.parameterId = "doubleFiltering";
    doubled.boolean = true;
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("bank", "talas.cdpUserBank",
                {pitches, bands, expression("q", "12"), expression("gain", "0.7"), doubled})},
        {{"osc", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}}));
    const auto list = talas::cdp::readFilterBands("57 1\n64 -6dB\n69 0.5", talas::cdp::FilterPitches::Midi, sampleRate);
    talas::cdp::UserBank port;
    require(list.has_value() && port.prepare(sampleRate, *list, 12.0, 0.7, true), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP User Bank should play its talas_cdp port");
}

void testDistortOverloadPlaysItsPort()
{
    const auto dry = dryTone();
    for (const auto& mode : {std::string{"noise"}, std::string{"waveform"}})
    {
        Parameter choice;
        choice.parameterId = "mode";
        choice.choice = mode;
        const auto played = render(graph(
            {tone("osc", "441 hz"),
                node("overload", "talas.cdpDistortOverload",
                    {choice, expression("clipLevel", "0.2"), expression("depth", "0.6"),
                        expression("frequency", "700 hz"), expression("seed", "3")})},
            {{"osc", "audioOut", "overload", "audioIn"}, {"overload", "audioOut", "out", "audioIn"}}));
        talas::cdp::DistortOverload port;
        require(port.prepare(sampleRate,
                    mode == "noise" ? talas::cdp::DistortOverload::Mode::Noise : talas::cdp::DistortOverload::Mode::Waveform,
                    0.2, 0.6, 700.0, 3),
            "the port should prepare");
        std::vector<double> expected(frames);
        require(port.process(dry, expected), "the port should process");
        require(played == expected, "CDP Distort Overload should play its talas_cdp port in its " + mode + " mode");
    }
}

void testVaryingBankPlaysItsPort()
{
    const auto dry = dryTone();
    Parameter bands;
    bands.parameterId = "bands";
    bands.text = "0 300 1 900 -6dB\n0.005 450 0.5 700 1";
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("bank", "talas.cdpVaryingBank",
                {bands, expression("q", "15"), expression("gain", "0.8"), expression("harmonics", "2"),
                    expression("rolloff", "-3")})},
        {{"osc", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}}));
    const auto data = talas::cdp::readVaryingFilterData(
        "0 300 1 900 -6dB\n0.005 450 0.5 700 1", talas::cdp::FilterPitches::Hertz, sampleRate);
    talas::cdp::VaryingBank port;
    require(data.has_value() && port.prepare(sampleRate, *data, 15.0, 0.8, 2, -3.0, false), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected, "CDP Varying Bank should play its talas_cdp port");
}

void testStadiumEchoPlaysItsPortInStereo()
{
    const auto dry = dryTone();
    const auto left = render(graph(
        {tone("osc", "441 hz"),
            node("echo", "talas.cdpStadiumEcho",
                {expression("gain", "0.5"), expression("rolloff", "0.7"), expression("size", "0.01"),
                    expression("count", "30")})},
        {{"osc", "audioOut", "echo", "audioIn"},
            {"echo", "leftOut", "out", "audioIn"},
            {"echo", "rightOut", "out", "rightIn"}}));
    talas::cdp::StadiumEcho port;
    require(port.prepare(sampleRate, 0.5, 0.7, 0.01, 30), "the port should prepare");
    std::vector<double> expectedLeft(frames);
    std::vector<double> expectedRight(frames);
    require(port.process(dry, expectedLeft, expectedRight), "the port should process");
    require(left == expectedLeft && lastRight == expectedRight && left != expectedRight,
        "CDP Stadium Echo should play its talas_cdp port on both channels");
}

// Distort Reform waits for each half-cycle to end, so it lags 2047 frames; mixed
// with its dry input, the dry branch is delayed to match.
void testDistortReformPlaysItsPortAndAlignsTheDryBranch()
{
    constexpr std::size_t longer = 3 * frames;
    const auto latency = talas::cdp::DistortReform::latencyFrames;
    const auto dry = dryTone(longer);
    Parameter mode;
    mode.parameterId = "mode";
    mode.choice = "sine";
    const auto reformed = render(graph(
        {tone("osc", "441 hz"), node("reform", "talas.cdpDistortReform", {mode})},
        {{"osc", "audioOut", "reform", "audioIn"}, {"reform", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    talas::cdp::DistortReform port;
    require(port.prepare(sampleRate, talas::cdp::DistortReform::Mode::Sine, 2.0), "the port should prepare");
    std::vector<double> expected(longer);
    require(port.process(dry, expected), "the port should process");
    require(reformed == expected && reformed[latency + 40] != 0.0,
        "CDP Distort Reform should play its talas_cdp port");
    const auto mixed = render(graph(
        {tone("osc", "441 hz"), node("reform", "talas.cdpDistortReform", {mode}), node("mix", "talas.mixerV2")},
        {{"osc", "audioOut", "reform", "audioIn"},
            {"osc", "audioOut", "mix", "inputs"},
            {"reform", "audioOut", "mix", "inputs"},
            {"mix", "leftOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    for (std::size_t frame = 0; frame < longer; ++frame)
    {
        const auto delayed = frame >= latency ? dry[frame - latency] : 0.0;
        require(std::abs(mixed[frame] - (delayed + expected[frame])) <= 1.0e-12,
            "frame " + std::to_string(frame) + " should add the aligned dry and reformed signals");
    }
}

void testDistortOmitPlaysItsPort()
{
    const auto dry = dryTone();
    const auto played = render(graph(
        {tone("osc", "441 hz"),
            node("omit", "talas.cdpDistortOmit", {expression("omit", "2"), expression("outOf", "3")})},
        {{"osc", "audioOut", "omit", "audioIn"}, {"omit", "audioOut", "out", "audioIn"}}));
    talas::cdp::DistortOmit port;
    require(port.prepare(2, 3), "the port should prepare");
    std::vector<double> expected(frames);
    require(port.process(dry, expected), "the port should process");
    require(played == expected && std::ranges::count(played, 0.0) > 1000,
        "CDP Distort Omit should play its talas_cdp port with no latency");
}

// Distort Reverse and Envelope wait for each group, so they lag 2047 frames.
void testDistortReverseAndEnvelopePlayTheirPorts()
{
    constexpr std::size_t longer = 3 * frames;
    const auto dry = dryTone(longer);
    const auto reversed = render(graph(
        {tone("osc", "441 hz"), node("reverse", "talas.cdpDistortReverse", {expression("cycles", "2")})},
        {{"osc", "audioOut", "reverse", "audioIn"}, {"reverse", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    talas::cdp::DistortReverse reverse;
    require(reverse.prepare(2), "the reverse port should prepare");
    std::vector<double> expected(longer);
    require(reverse.process(dry, expected), "the reverse port should process");
    require(reversed == expected && reversed[talas::cdp::DistortReverse::latencyFrames + 30] != 0.0,
        "CDP Distort Reverse should play its talas_cdp port");
    Parameter shape;
    shape.parameterId = "shape";
    shape.choice = "user";
    Parameter envelope;
    envelope.parameterId = "envelope";
    envelope.text = "0 0\n0.2 1\n1 0";
    const auto enveloped = render(graph(
        {tone("osc", "441 hz"),
            node("envelope", "talas.cdpDistortEnvelope", {shape, expression("cycles", "2"), envelope})},
        {{"osc", "audioOut", "envelope", "audioIn"}, {"envelope", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    const auto points = talas::cdp::readDistortEnvelope("0 0\n0.2 1\n1 0");
    talas::cdp::DistortEnvelope distortEnvelope;
    require(points.has_value()
            && distortEnvelope.prepare(talas::cdp::DistortEnvelope::Shape::User, 2, 0.0, 1.0, *points),
        "the envelope port should prepare");
    require(distortEnvelope.process(dry, expected), "the envelope port should process");
    require(enveloped == expected && enveloped[talas::cdp::DistortEnvelope::latencyFrames + 30] != 0.0,
        "CDP Distort Envelope should play its talas_cdp port");
}

// Distort Harmonic and Fractal wait for each cycle or group, so they lag
// 2047 frames.
void testDistortHarmonicAndFractalPlayTheirPorts()
{
    constexpr std::size_t longer = 3 * frames;
    const auto latency = talas::cdp::DistortHarmonic::latencyFrames;
    const auto dry = dryTone(longer);
    Parameter harmonics;
    harmonics.parameterId = "harmonics";
    harmonics.text = "2 0.4\n3 0.2";
    const auto harmonized = render(graph(
        {tone("osc", "441 hz"),
            node("harmonic", "talas.cdpDistortHarmonic", {harmonics, expression("prescale", "0.8")})},
        {{"osc", "audioOut", "harmonic", "audioIn"}, {"harmonic", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    const auto partials = talas::cdp::readDistortHarmonics("2 0.4\n3 0.2");
    talas::cdp::DistortHarmonic harmonic;
    require(partials.has_value() && harmonic.prepare(*partials, 0.8), "the harmonic port should prepare");
    std::vector<double> expected(longer);
    require(harmonic.process(dry, expected), "the harmonic port should process");
    require(harmonized == expected && harmonized[latency + 30] != 0.0,
        "CDP Distort Harmonic should play its talas_cdp port");
    const auto fractal = render(graph(
        {tone("osc", "441 hz"),
            node("fractal", "talas.cdpDistortFractal",
                {expression("scale", "3"), expression("loudness", "0.6")})},
        {{"osc", "audioOut", "fractal", "audioIn"}, {"fractal", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    talas::cdp::DistortFractal distortFractal;
    require(distortFractal.prepare(sampleRate, 3, 0.6, 1.0), "the fractal port should prepare");
    require(distortFractal.process(dry, expected), "the fractal port should process");
    require(fractal == expected && fractal[latency + 30] != 0.0,
        "CDP Distort Fractal should play its talas_cdp port");
}

// Distort Multiply and Divide wait for each half-cycle or group, so they
// lag 2047 frames.
void testDistortMultiplyAndDividePlayTheirPorts()
{
    constexpr std::size_t longer = 3 * frames;
    const auto latency = talas::cdp::DistortMultiply::latencyFrames;
    const auto dry = dryTone(longer);
    const auto multiplied = render(graph(
        {tone("osc", "441 hz"), node("multiply", "talas.cdpDistortMultiply", {expression("factor", "3")})},
        {{"osc", "audioOut", "multiply", "audioIn"}, {"multiply", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    talas::cdp::DistortMultiply distortMultiply;
    require(distortMultiply.prepare(3), "the multiply port should prepare");
    std::vector<double> expected(longer);
    require(distortMultiply.process(dry, expected), "the multiply port should process");
    require(multiplied == expected && multiplied[latency + 30] != 0.0,
        "CDP Distort Multiply should play its talas_cdp port");
    Parameter interpolate;
    interpolate.parameterId = "interpolate";
    interpolate.boolean = true;
    const auto divided = render(graph(
        {tone("osc", "441 hz"),
            node("divide", "talas.cdpDistortDivide", {expression("factor", "2"), interpolate})},
        {{"osc", "audioOut", "divide", "audioIn"}, {"divide", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    talas::cdp::DistortDivide distortDivide;
    require(distortDivide.prepare(2, true), "the divide port should prepare");
    require(distortDivide.process(dry, expected), "the divide port should process");
    require(divided == expected && divided[latency + 30] != 0.0,
        "CDP Distort Divide should play its talas_cdp port");
}

// Distort Shuffle waits for each domain, so it lags 2047 frames.
void testDistortShufflePlaysItsPort()
{
    constexpr std::size_t longer = 3 * frames;
    const auto dry = dryTone(longer);
    Parameter domainImage;
    domainImage.parameterId = "domainImage";
    domainImage.text = "abc-cab";
    const auto shuffled = render(graph(
        {tone("osc", "441 hz"), node("shuffle", "talas.cdpDistortShuffle", {domainImage})},
        {{"osc", "audioOut", "shuffle", "audioIn"}, {"shuffle", "audioOut", "out", "audioIn"}}),
        {sampleRate, 64}, longer);
    const auto map = talas::cdp::readDistortShuffle("abc-cab");
    talas::cdp::DistortShuffle port;
    require(map.has_value() && port.prepare(*map, 1), "the port should prepare");
    std::vector<double> expected(longer);
    require(port.process(dry, expected), "the port should process");
    require(shuffled == expected && shuffled[talas::cdp::DistortShuffle::latencyFrames + 30] != 0.0,
        "CDP Distort Shuffle should play its talas_cdp port");
}

void testCdpNodesRejectValuesOutsideCdpRanges()
{
    const auto lowBit = [](std::string bits, std::string division) {
        return graph(
            {tone("osc", "441 hz"),
                node("lowBit", "talas.cdpLowerResolution",
                    {expression("bitResolution", std::move(bits)),
                        expression("sampleRateDivision", std::move(division))})},
            {{"osc", "audioOut", "lowBit", "audioIn"}, {"lowBit", "audioOut", "out", "audioIn"}});
    };
    require(fails(lowBit("17", "1")) && fails(lowBit("0", "1")) && fails(lowBit("8", "257")),
        "CDP Lower Resolution should reject values outside CDP's ranges");
    const auto ring = [](std::string frequency) {
        return graph(
            {tone("osc", "441 hz"),
                node("ring", "talas.cdpRingModulate", {expression("frequency", std::move(frequency))})},
            {{"osc", "audioOut", "ring", "audioIn"}, {"ring", "audioOut", "out", "audioIn"}});
    };
    require(fails(ring("0.05 hz")) && fails(ring("30000 hz")),
        "CDP Ring Modulate should reject frequencies outside 0.1 Hz to Nyquist");
    const auto tremolo = [](std::string depth) {
        return graph(
            {tone("osc", "441 hz"),
                node("tremolo", "talas.cdpTremolo", {expression("depth", std::move(depth))})},
            {{"osc", "audioOut", "tremolo", "audioIn"}, {"tremolo", "audioOut", "out", "audioIn"}});
    };
    require(fails(tremolo("1.5")) && fails(tremolo("-0.1")),
        "CDP Tremolo should reject a depth outside 0 to 1");
    const auto pan = [](std::string prescale) {
        return graph(
            {tone("osc", "441 hz"),
                node("pan", "talas.cdpPan", {expression("prescale", std::move(prescale))})},
            {{"osc", "audioOut", "pan", "audioIn"}, {"pan", "leftOut", "out", "audioIn"}});
    };
    require(fails(pan("3")) && fails(pan("0")),
        "CDP Pan should reject a prescale outside 1/32767 to 2");
    const auto tapDelay = [](std::string taps, std::string feedback) {
        Parameter tapText;
        tapText.parameterId = "taps";
        tapText.text = std::move(taps);
        return graph(
            {tone("osc", "441 hz"),
                node("delay", "talas.cdpTapDelay", {tapText, expression("feedback", std::move(feedback))})},
            {{"osc", "audioOut", "delay", "audioIn"}, {"delay", "leftOut", "out", "audioIn"}});
    };
    require(fails(tapDelay("0.01 0.5", "1.5")) && fails(tapDelay("0.02 0.5\n0.01 0.5", "0"))
            && fails(tapDelay("0.01 2", "0")) && fails(tapDelay("0.01 0.5", "!Feedback")),
        "CDP Tap Delay should reject feedback beyond 1, unordered taps, loud taps and a live field");
    const auto echo = [](std::string delay, std::string feedback) {
        return graph(
            {tone("osc", "441 hz"),
                node("echo", "talas.cdpEcho",
                    {expression("delay", std::move(delay)), expression("feedback", std::move(feedback))})},
            {{"osc", "audioOut", "echo", "audioIn"}, {"echo", "audioOut", "out", "audioIn"}});
    };
    require(fails(echo("20 ms", "-1")) && fails(echo("11 s", "0")) && fails(echo("0.001 ms", "0"))
            && fails(echo("!Delay * (1 ms)", "0")),
        "CDP Echo should reject Feedback of -1, delays outside a sample to 10 s and a live delay");
    const auto varying = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("delay", "talas.cdpVaryingDelay", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "delay", "audioIn"}, {"delay", "audioOut", "out", "audioIn"}});
    };
    require(fails(varying("seed", "0")) && fails(varying("seed", "2.5")) && fails(varying("lfoFrequency", "60 hz"))
            && fails(varying("modulation", "!Depth")),
        "CDP Varying Delay should reject a Seed outside 1 to 256, a sweep beyond 50 Hz and a live field");
    const auto reverb = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("reverb", "talas.cdpReverb", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "reverb", "audioIn"}, {"reverb", "leftOut", "out", "audioIn"}});
    };
    require(fails(reverb("mix", "1")) && fails(reverb("reverbTime", "0 s")) && fails(reverb("lowpass", "20000 hz"))
            && fails(reverb("reverbGain", "!Gain")),
        "CDP Reverb should reject a Mix of 1, no reverb time, a lowpass beyond a third of the rate and a live field");
    const auto filter = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("filter", "talas.cdpVariableFilter", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    };
    require(fails(filter("acuity", "2")) && fails(filter("gain", "0")) && fails(filter("frequency", "30000 hz"))
            && fails(filter("gain", "!Gain")),
        "CDP Variable Filter should reject an acuity above 1, no gain, a frequency beyond Nyquist and a live gain");
    const auto fixedFilter = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("filter", "talas.cdpFixedFilter", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    };
    require(fails(fixedFilter("boost", "40")) && fails(fixedFilter("prescale", "300"))
            && fails(fixedFilter("frequency", "30000 hz")) && fails(fixedFilter("boost", "!Boost")),
        "CDP Fixed Filter should reject a boost over 36 dB, a prescale over 200, a frequency beyond Nyquist and a live field");
    const auto phasing = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("phasing", "talas.cdpPhasing", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "phasing", "audioIn"}, {"phasing", "audioOut", "out", "audioIn"}});
    };
    require(fails(phasing("gain", "1.5")) && fails(phasing("prescale", "-2"))
            && fails(phasing("delay", "20 s")) && fails(phasing("gain", "!Gain")),
        "CDP Phasing should reject a gain or prescale beyond -1 to 1, a delay over 10 s and a live gain");
    const auto lohi = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("filter", "talas.cdpLohiFilter", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    };
    require(fails(lohi("attenuation", "6")) && fails(lohi("stopBand", "1000 hz"))
            && fails(lohi("passBand", "30000 hz")) && fails(lohi("prescale", "!Prescale")),
        "CDP Lohi Filter should reject a gain for an attenuation, equal bands, a band beyond Nyquist and a live field");
    const auto sweeping = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("filter", "talas.cdpSweepingFilter", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "filter", "audioIn"}, {"filter", "audioOut", "out", "audioIn"}});
    };
    require(fails(sweeping("acuity", "2")) && fails(sweeping("sweepFrequency", "300 hz"))
            && fails(sweeping("highFrequency", "30000 hz")) && fails(sweeping("phase", "!Phase")),
        "CDP Sweeping Filter should reject an acuity above 1, a sweep over 200 Hz, a frequency beyond Nyquist and a live phase");
    const auto userBank = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("bank", "talas.cdpUserBank", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}});
    };
    Parameter badBands;
    badBands.parameterId = "bands";
    badBands.text = "440 1 2";
    require(fails(userBank("q", "0")) && fails(userBank("gain", "20000")) && fails(userBank("gain", "!Gain"))
            && fails(graph(
                {tone("osc", "441 hz"), node("bank", "talas.cdpUserBank", {badBands})},
                {{"osc", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}})),
        "CDP User Bank should reject a Q of 0, a gain over 10000, a live gain and a line of three values");
    const auto overload = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("overload", "talas.cdpDistortOverload", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "overload", "audioIn"}, {"overload", "audioOut", "out", "audioIn"}});
    };
    require(fails(overload("clipLevel", "1.5")) && fails(overload("seed", "2.5"))
            && fails(overload("seed", "!Seed + 1")) && fails(overload("depth", "!Depth + 2")),
        "CDP Distort Overload should reject a clip level over 1, a fractional seed, a live seed and a live depth over 1");
    const auto varyingBank = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("bank", "talas.cdpVaryingBank", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "bank", "audioIn"}, {"bank", "audioOut", "out", "audioIn"}});
    };
    require(fails(varyingBank("harmonics", "1.5")) && fails(varyingBank("rolloff", "3"))
            && fails(varyingBank("harmonics", "100")) && fails(varyingBank("gain", "!Gain")),
        "CDP Varying Bank should reject fractional harmonics, a positive rolloff, harmonics past Nyquist and a live gain");
    const auto stadiumEcho = [](std::string field, std::string value) {
        return graph(
            {tone("osc", "441 hz"),
                node("echo", "talas.cdpStadiumEcho", {expression(std::move(field), std::move(value))})},
            {{"osc", "audioOut", "echo", "audioIn"}, {"echo", "leftOut", "out", "audioIn"}});
    };
    require(fails(stadiumEcho("gain", "1.5")) && fails(stadiumEcho("count", "23.5"))
            && fails(stadiumEcho("count", "1000")) && fails(stadiumEcho("size", "!Size")),
        "CDP Stadium Echo should reject a gain over 1, a fractional count, a last echo past 10 s and a live field");
    const auto reform = [](std::string exaggeration) {
        return graph(
            {tone("osc", "441 hz"),
                node("reform", "talas.cdpDistortReform", {expression("exaggeration", std::move(exaggeration))})},
            {{"osc", "audioOut", "reform", "audioIn"}, {"reform", "audioOut", "out", "audioIn"}});
    };
    require(fails(reform("0")) && fails(reform("41")) && fails(reform("!Exaggeration + 41")),
        "CDP Distort Reform should reject an exaggeration outside 0.000002 to 40, live or not");
    const auto distortion = [](std::string type, std::vector<Parameter> parameters) {
        return graph(
            {tone("osc", "441 hz"), node("distort", std::move(type), std::move(parameters))},
            {{"osc", "audioOut", "distort", "audioIn"}, {"distort", "audioOut", "out", "audioIn"}});
    };
    Parameter userShape;
    userShape.parameterId = "shape";
    userShape.choice = "user";
    Parameter lateEnvelope;
    lateEnvelope.parameterId = "envelope";
    lateEnvelope.text = "0.5 0\n1 1";
    require(fails(distortion("talas.cdpDistortOmit", {expression("omit", "2"), expression("outOf", "2")}))
            && fails(distortion("talas.cdpDistortReverse", {expression("cycles", "1.5")}))
            && fails(distortion("talas.cdpDistortEnvelope", {expression("exponent", "60")}))
            && fails(distortion("talas.cdpDistortEnvelope", {userShape, lateEnvelope}))
            && fails(distortion("talas.cdpDistortOmit", {expression("omit", "1"), expression("outOf", "!OutOf")})),
        "CDP Distort Omit, Reverse and Envelope should reject Omit not under OutOf, fractional cycles, an exponent over 50, an envelope starting late and a live OutOf");
    Parameter badHarmonics;
    badHarmonics.parameterId = "harmonics";
    badHarmonics.text = "1 0.5";
    require(fails(distortion("talas.cdpDistortHarmonic", {badHarmonics}))
            && fails(distortion("talas.cdpDistortHarmonic", {expression("prescale", "300")}))
            && fails(distortion("talas.cdpDistortFractal", {expression("scale", "2.5")}))
            && fails(distortion("talas.cdpDistortFractal", {expression("prescale", "!Prescale + 1")}))
            && fails(distortion("talas.cdpDistortFractal", {expression("loudness", "!Loudness + 40000")})),
        "CDP Distort Harmonic and Fractal should reject a harmonic under 2, a prescale over 200, a fractional scale, a live prescale and a live loudness over 32767");
    require(fails(distortion("talas.cdpDistortMultiply", {expression("factor", "17")}))
            && fails(distortion("talas.cdpDistortDivide", {expression("factor", "2.5")}))
            && fails(distortion("talas.cdpDistortMultiply", {expression("factor", "!Factor * 20")})),
        "CDP Distort Multiply and Divide should reject a factor over 16, a fractional factor and a live one over 16");
    Parameter lengthening;
    lengthening.parameterId = "domainImage";
    lengthening.text = "ab-aab";
    require(fails(distortion("talas.cdpDistortShuffle", {lengthening}))
            && fails(distortion("talas.cdpDistortShuffle", {expression("cycles", "0")}))
            && fails(distortion("talas.cdpDistortShuffle", {expression("cycles", "!Cycles")})),
        "CDP Distort Shuffle should reject an image that lengthens its domain and no cycles, live or not");
    require(fails(distortion("talas.cdpPhaseVocoder", {expression("points", "1")}))
            && fails(distortion("talas.cdpPhaseVocoder", {expression("points", "8194")}))
            && fails(distortion("talas.cdpPhaseVocoder", {expression("points", "100.5")}))
            && fails(distortion("talas.cdpPhaseVocoder", {expression("overlap", "5")}))
            && fails(distortion("talas.cdpPhaseVocoder", {expression("points", "!Points + 512")}))
            && fails(distortion("talas.cdpSpectralGain", {expression("gain", "-1")}))
            && fails(distortion("talas.cdpSpectralGain", {expression("gain", "!Gain - 1")}))
            && fails(distortion("talas.cdpSpectralGate", {expression("threshold", "2")})),
        "CDP Phase Vocoder, Spectral Gain and Gate should reject points outside 2 to 8192 or not whole, an overlap over 4, live points, a negative gain, live or not, and a threshold over 1");
    require(fails(distortion("talas.cdpFocusExaggerate", {expression("exaggeration", "2000")}))
            && fails(distortion("talas.cdpFocusFold", {expression("highFrequency", "30000 hz")}))
            && fails(distortion("talas.cdpFocusAccumulate", {expression("decay", "1.5")}))
            && fails(distortion("talas.cdpFocusAccumulate", {expression("glissando", "100")}))
            && fails(distortion("talas.cdpBlurAverage", {expression("n", "1")}))
            && fails(distortion("talas.cdpBlurSuppress", {expression("n", "!N + 600")}))
            && fails(distortion("talas.cdpHiliteTrace", {expression("n", "2.5")}))
            && fails(distortion("talas.cdpHiliteTrace", {expression("overlap", "!Overlap + 1")})),
        "CDP's focus, blur and hilite nodes should reject fields outside CDP's ranges, live or not, a fractional N and a live overlap");
    require(fails(distortion("talas.cdpHiliteFilter", {expression("frequency", "5 hz")}))
            && fails(distortion("talas.cdpHiliteFilter", {expression("q", "0.5 hz")}))
            && fails(distortion("talas.cdpHiliteFilter", {expression("gain", "!Gain + 1")}))
            && fails(distortion("talas.cdpHilitePluck", {expression("gain", "2000")}))
            && fails(distortion("talas.cdpFocusStep", {expression("step", "0.001 s")}))
            && fails(distortion("talas.cdpFocusStep", {expression("step", "!Step + 1")})),
        "CDP Hilite Filter, Pluck and Focus Step should reject fields outside CDP's ranges, a live filter gain and a live step");
    require(fails(distortion("talas.cdpStrangeShift", {expression("shift", "30000 hz")}))
            && fails(distortion("talas.cdpStrangeShift", {expression("frequency", "0 hz")}))
            && fails(distortion("talas.cdpRepitchTranspose", {expression("transposition", "100")}))
            && fails(distortion("talas.cdpRepitchTranspose", {expression("lowFrequency", "-1 hz")}))
            && fails(distortion("talas.cdpRepitchTranspose", {expression("transposition", "!Semitones + 120")})),
        "CDP Strange Shift and Repitch Transpose should reject a shift beyond half the sample rate, no frequency, a transposition beyond eight octaves, live or not, and a negative band frequency");
    Parameter transpShiftUp;
    transpShiftUp.parameterId = "mode";
    transpShiftUp.choice = "shiftUp";
    require(fails(distortion("talas.cdpStrangeWaver", {expression("stretch", "0.5")}))
            && fails(distortion("talas.cdpStrangeWaver", {expression("exponent", "!Exponent + 1")}))
            && fails(distortion("talas.cdpPitchTransp", {expression("depth", "2")}))
            && fails(distortion("talas.cdpPitchTransp",
                {transpShiftUp, expression("split", "!Split * (1 hz) + 700 hz"), expression("transposition", "3")}))
            && fails(distortion("talas.cdpBlurChorus", {expression("frequencySpread", "5")}))
            && fails(distortion("talas.cdpBlurNoise", {expression("noise", "!Noise + 2")})),
        "CDP Strange Waver, Pitch Transp, Blur Chorus and Noise should reject fields outside CDP's ranges, a live exponent and a live split with a static transposition");
    require(fails(distortion("talas.cdpStretchSpectrum", {expression("exponent", "60")}))
            && fails(distortion("talas.cdpStretchSpectrum", {expression("maxStretch", "!Stretch + 1")}))
            && fails(distortion("talas.cdpBlurScatter", {expression("keep", "0")}))
            && fails(distortion("talas.cdpBlurScatter", {expression("keep", "!Keep + 600")})),
        "CDP Stretch Spectrum and Blur Scatter should reject an exponent over 50, a live stretch and a keep outside the frame's blocks, live or not");
    Parameter arpegBelowBoost;
    arpegBelowBoost.parameterId = "mode";
    arpegBelowBoost.choice = "belowBoost";
    Parameter arpegDownRamp;
    arpegDownRamp.parameterId = "wave";
    arpegDownRamp.choice = "downRamp";
    Parameter unpaired;
    unpaired.parameterId = "bands";
    unpaired.text = "441 0.5\n1000";
    Parameter eqModeTwo;
    eqModeTwo.parameterId = "mode";
    eqModeTwo.choice = "bandwidths";
    Parameter overlapping;
    overlapping.parameterId = "bands";
    overlapping.text = "100 400 1000 2\n300 600 1000 2";
    require(fails(distortion("talas.cdpHiliteArpeg", {expression("decay", "60")}))
            && fails(distortion("talas.cdpHiliteArpeg", {expression("startPhase", "!Phase")}))
            && fails(distortion("talas.cdpHiliteArpeg", {expression("rate", "!Rate * (1 hz) + 1000 hz")}))
            && fails(distortion("talas.cdpHiliteArpeg", {arpegBelowBoost, arpegDownRamp}))
            && fails(distortion("talas.cdpHiliteGraphicEq", {eqModeTwo, unpaired}))
            && fails(distortion("talas.cdpHiliteBand", {overlapping})),
        "CDP Hilite Arpeg, Graphic EQ and Band should reject a decay over 50, a live start phase, a live rate over the frame rate, a wave the mode refuses, an unpaired band and overlapping bands");
    require(fails(distortion("talas.cdpBlurSpread", {expression("spread", "2")}))
            && fails(distortion("talas.cdpBlurSpread", {expression("formantBands", "!Bands")}))
            && fails(distortion("talas.cdpBlurSpread", {expression("formantBands", "2.5")}))
            && fails(distortion("talas.cdpStrangeGlis", {expression("glisRate", "!Rate + 4000")}))
            && fails(distortion("talas.cdpStrangeGlis", {expression("topFrequency", "3 hz")})),
        "CDP Blur Spread and Strange Glis should reject a spread over 1, live or fractional bands, a live rate over 8 semitones a frame and a top under 5 Hz");
    require(fails(distortion("talas.cdpSpecBare", {expression("harmonics", "9")}))
            && fails(distortion("talas.cdpPitchOctmove", {expression("bassBoost", "60")})),
        "CDP Spec Bare and Pitch Octmove should refuse getpitch's options and a bass boost outside CDP's ranges");
    Parameter hashInBands;
    hashInBands.parameterId = "bands";
    hashInBands.text = "0 440 1\n#\n0 1 1";
    require(fails(distortion("talas.cdpVaryingBank2", {expression("gain", "!Gain")}))
            && fails(distortion("talas.cdpTremoloFine", {expression("fineness", "101")}))
            && fails(distortion("talas.cdpTremoloFine", {expression("fineness", "!Fine")}))
            && fails(distortion("talas.cdpVaryingBank2", {hashInBands})),
        "CDP Varying Bank 2 and Tremolo Fine should refuse a live gain, a # line of its own and a fineness over 100 or live");
    require(fails(distortion("talas.cdpClipWaveset", {expression("fraction", "0")}))
            && fails(distortion("talas.cdpClipWaveset", {expression("fraction", "1.5")}))
            && fails(distortion("talas.cdpClipWaveset", {expression("fraction", "!Fraction")}))
            && fails(distortion("talas.cdpQuirkWaveset", {expression("exponent", "0.005")}))
            && fails(distortion("talas.cdpQuirkWaveset", {expression("exponent", "101")}))
            && fails(distortion("talas.cdpQuirkWaveset", {expression("exponent", "!Exponent")})),
        "CDP Clip Waveset and Quirk Waveset should refuse a fraction outside 0 (exclusive) to 1, an exponent outside 0.01 to 100 and a live field");
    const auto roomReverb = [](std::vector<Parameter> parameters) {
        return graph({tone("osc", "441 hz"), node("reverb", "talas.cdpRoomReverb", std::move(parameters))},
            {{"osc", "audioOut", "reverb", "audioIn"},
                {"reverb", "leftOut", "out", "audioIn"},
                {"reverb", "rightOut", "out", "rightIn"}});
    };
    Parameter unorderedReflections;
    unorderedReflections.parameterId = "reflections";
    unorderedReflections.text = "0.02 0.5\n0.01 0.5\n";
    require(fails(roomReverb({expression("reverbGain", "0")}))
            && fails(roomReverb({expression("mix", "1.5")}))
            && fails(roomReverb({expression("absorb", "20000 hz")}))
            && fails(roomReverb({expression("lowCut", "500 hz"), expression("highCut", "400 hz")}))
            && fails(roomReverb({expression("predelay", "11 s")}))
            && fails(roomReverb({expression("feedback", "!Feedback")}))
            && fails(roomReverb({unorderedReflections})),
        "CDP Room Reverb should refuse no reverb gain, a mix over 1, an absorption beyond 0.35 of the rate, a high cut under its low cut, a predelay over 10 s, a live field and reflections out of order");
    Parameter decibels;
    decibels.parameterId = "scale";
    decibels.choice = "decibels";
    require(fails(distortion("talas.cdpQuantise", {expression("bitResolution", "17")}))
            && fails(distortion("talas.cdpQuantise", {expression("bitResolution", "0.5")}))
            && fails(distortion("talas.cdpLoudnessGain", {expression("gain", "0")}))
            && fails(distortion("talas.cdpLoudnessGain", {expression("gain", "40000")}))
            && fails(distortion("talas.cdpLoudnessGain", {decibels, expression("gain", "-96")}))
            && fails(distortion("talas.cdpDcShift", {expression("shift", "0")}))
            && fails(distortion("talas.cdpDcShift", {expression("shift", "1.5")}))
            && fails(distortion("talas.cdpDcShift", {expression("shift", "!Shift")})),
        "CDP Quantise, Loudness Gain and DC Shift should refuse a bit resolution outside 1 to 16, a fixed gain of silence or over 32767, a shift of 0 or beyond 1 and a live shift");
    const auto twoInput = [](std::string type, std::vector<Parameter> parameters) {
        return graph({tone("osc", "441 hz"), tone("side", "300 hz"), node("process", std::move(type), std::move(parameters))},
            {{"osc", "audioOut", "process", "audioIn"}, {"side", "audioOut", "process", "sideChain"},
                {"process", "audioOut", "out", "audioIn"}});
    };
    Parameter cosineContour;
    cosineContour.parameterId = "contour";
    cosineContour.choice = "cosine";
    require(fails(twoInput("talas.cdpMixBalance", {expression("balance", "1.5")}))
            && fails(twoInput("talas.cdpMixCrossfade", {expression("begin", "0.5 s"), expression("end", "0.5 s")}))
            && fails(twoInput("talas.cdpMixCrossfade", {expression("begin", "0.001 s"), expression("stagger", "0.002 s")}))
            && fails(twoInput("talas.cdpMixCrossfade",
                {expression("begin", "12 s"), expression("end", "13 s"), expression("stagger", "11 s")}))
            && fails(twoInput("talas.cdpMixCrossfade", {cosineContour, expression("powerFactor", "9")}))
            && fails(twoInput("talas.cdpMixCrossfade", {expression("end", "!End * (1 s)")})),
        "CDP Mix Balance and Mix Crossfade should refuse a balance over 1, a crossfade of no length or starting before its stagger, a stagger over 10 s, a power factor over 8 and a live end");
    // The standard Sine wavetable, which resolves by name, as an impulse sample.
    Parameter sineImpulse;
    sineImpulse.parameterId = "impulse";
    sineImpulse.resource = "Sine";
    Parameter approximateTime;
    approximateTime.parameterId = "mode";
    approximateTime.choice = "approximateTime";
    require(fails(distortion("talas.cdpFastConvolution", {}))
            && fails(distortion("talas.cdpFastConvolution", {sineImpulse, expression("dry", "1.5")}))
            && fails(distortion("talas.cdpFastConvolution", {sineImpulse, expression("scale", "0")}))
            && fails(distortion("talas.cdpFastConvolution", {sineImpulse, expression("impulseChannel", "2")}))
            && fails(distortion("talas.cdpFastConvolution", {sineImpulse, expression("dry", "!Dry")})),
        "CDP Fast Convolution should refuse no impulse sample, a dry over 1, no scale, a channel the sample has not and a live dry");
    require(fails(distortion("talas.cdpSilenceGate", {expression("gate", "0.5")}))
            && fails(distortion("talas.cdpSilenceGate", {expression("gate", "-97")}))
            && fails(distortion("talas.cdpSilenceGate", {expression("gate", "-95")}))
            && fails(distortion("talas.cdpSilenceGate", {expression("gate", "!Gate")}))
            && fails(distortion("talas.cdpDeglitch", {expression("glitch", "0 ms")}))
            && fails(distortion("talas.cdpDeglitch", {expression("window", "5 ms")}))
            && fails(distortion("talas.cdpDeglitch", {expression("splice", "6 ms")}))
            && fails(distortion("talas.cdpDeglitch", {expression("threshold", "!Threshold")})),
        "CDP Silence Gate and Deglitch should refuse a gate outside -96 to 0 dB or under one over the sample rate, no glitch, a window over 4 ms, a splice over half the silence and a live field");
    require(fails(distortion("talas.cdpNoiseCut", {expression("noiseFrequency", "500 hz")}))
            && fails(distortion("talas.cdpNoiseCut", {expression("noiseFrequency", "30000 hz")}))
            && fails(distortion("talas.cdpNoiseCut", {expression("minTone", "1100 ms")}))
            && fails(distortion("talas.cdpNoiseCut", {expression("splice", "!Splice * (1 ms)")}))
            && fails(distortion("talas.cdpAttackBoost", {expression("level", "1.5")}))
            && fails(distortion("talas.cdpAttackBoost", {approximateTime, expression("level", "3700")}))
            && fails(distortion("talas.cdpAttackBoost", {expression("gain", "0.5")}))
            && fails(distortion("talas.cdpAttackBoost", {expression("onset", "2 ms")}))
            && fails(distortion("talas.cdpAttackBoost", {expression("onset", "1100 ms")}))
            && fails(distortion("talas.cdpAttackBoost", {expression("decay", "!Decay * (1 ms)")})),
        "CDP Noise Cut and Attack Boost should refuse a noise frequency outside 1000 Hz to half the sample rate, a min tone over 1000 ms, a gate over 1, a time over an hour, a gain under 1, an onset outside 5 to 1000 ms and a live field");
    const auto balance = [](std::string text) {
        Parameter parameter;
        parameter.parameterId = "balance";
        parameter.text = std::move(text);
        return parameter;
    };
    Parameter countMode;
    countMode.parameterId = "mode";
    countMode.choice = "count";
    require(fails(twoInput("talas.cdpMixMerge", {expression("stagger", "11 s")}))
            && fails(twoInput("talas.cdpMixMerge", {expression("skew", "0")}))
            && fails(twoInput("talas.cdpMixMerge", {expression("skew", "40000")}))
            && fails(twoInput("talas.cdpMixMerge", {expression("stagger", "0.01 s"), expression("skip", "0.02 s")}))
            && fails(twoInput("talas.cdpMixMerge", {expression("stagger", "!Stagger * (1 s)")})),
        "CDP Mix Merge should refuse a stagger over 10 s, a skew outside 1/32767 to 32767, a skip past the stagger and a live stagger");
    require(fails(twoInput("talas.cdpMixFaders", {expression("gain", "300")}))
            && fails(twoInput("talas.cdpMixFaders", {expression("gain", "-1")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0.5 1 1")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0 1 1\n0.1 0 0")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0 1 -1")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0 1")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0 1 1\n0.00001 1 0")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("0 1 1e0")}))
            && fails(twoInput("talas.cdpMixFaders", {balance("")})),
        "CDP Mix Faders should refuse a gain outside 0 to 256, a balance not starting at 0, levels that sum to 0 or are negative, a line without both levels, a time within a frame of the first, a number CDP cannot read and no balance");
    require(fails(twoInput("talas.cdpMixInbetween", {expression("ratio", "1.5")}))
            && fails(twoInput("talas.cdpMixInbetween", {countMode, expression("count", "0")}))
            && fails(twoInput("talas.cdpMixInbetween", {countMode, expression("count", "1000")}))
            && fails(twoInput("talas.cdpMixInbetween", {countMode, expression("count", "2.5")}))
            && fails(twoInput("talas.cdpMixInbetween", {countMode, expression("count", "2"), expression("file", "3")}))
            && fails(twoInput("talas.cdpMixInbetween", {expression("ratio", "!Ratio")})),
        "CDP Mix Inbetween should refuse a ratio over 1, a count outside 1 to 999 or not whole, a file past the count and a live ratio");
    const auto envelopeMode = [](std::string id, std::string value) {
        Parameter parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto flattenMode = envelopeMode("mode", "flatten");
    require(fails(distortion("talas.cdpEnvelopeWarp", {expression("window", "3 ms")}))
            && fails(distortion("talas.cdpEnvelopeWarp", {expression("first", "200")}))
            && fails(distortion("talas.cdpEnvelopeWarp", {flattenMode, expression("first", "1")}))
            && fails(distortion("talas.cdpEnvelopeWarp",
                {envelopeMode("mode", "invert"), expression("first", "0.5"), expression("second", "0.5")}))
            && fails(distortion("talas.cdpEnvelopeWarp",
                {envelopeMode("mode", "gate"), expression("first", "0.1"), expression("smoothing", "2.5")}))
            && fails(distortion("talas.cdpEnvelopeWarp", {expression("window", "!Window * (1 ms)")}))
            && fails(distortion("talas.cdpEnvelopeWarp", {expression("smoothing", "!Smoothing")}))
            && fails(distortion("talas.cdpEnvelopeWarp", {flattenMode, expression("first", "!Flatten + 4")}))
            && fails(distortion("talas.cdpEnvelopeWarp",
                {flattenMode, expression("first", "!Flatten + 4"), expression("maximumFirst", "3")})),
        "CDP Envelope Warp should refuse a window under 5 ms, values outside its mode's ranges, an invert gate at its mirror, a fractional smoothing, a live window or smoothing, and a live flatten without a maximum at least its value");
    require(fails(twoInput("talas.cdpEnvelopeImpose", {expression("window", "4 ms")}))
            && fails(twoInput("talas.cdpEnvelopeImpose", {expression("window", "!Window * (1 ms)")}))
            && fails(distortion("talas.cdpEnvelopeTarget", {expression("target", "1.5")}))
            && fails(distortion("talas.cdpEnvelopeTarget", {envelopeMode("scale", "decibels"), expression("target", "0.5")}))
            && fails(distortion("talas.cdpEnvelopeTarget", {expression("window", "!Window * (1 ms)")})),
        "CDP Envelope Impose and Target should refuse a window under 5 ms or live, a level over 1 and decibels over 0");
    require(fails(distortion("talas.cdpPitchedDelay", {expression("mix", "0")}))
            && fails(distortion("talas.cdpPitchedDelay", {expression("feedback", "1.5")}))
            && fails(distortion("talas.cdpPitchedDelay", {expression("pitch", "200")}))
            && fails(distortion("talas.cdpPitchedDelay", {expression("mix", "!Mix")}))
            && fails(distortion("talas.cdpBounce", {expression("count", "0")}))
            && fails(distortion("talas.cdpBounce", {expression("count", "2.5")}))
            && fails(distortion("talas.cdpBounce", {expression("startGap", "0.01 s")}))
            && fails(distortion("talas.cdpBounce", {expression("warp", "200")}))
            && fails(distortion("talas.cdpBounce",
                {expression("count", "100"), expression("startGap", "10 s"), expression("shorten", "1")}))
            && fails(distortion("talas.cdpBounce", {expression("count", "!Count")}))
            && fails(twoInput("talas.cdpRejoin", {expression("gain", "0")}))
            && fails(twoInput("talas.cdpRejoin", {expression("gain", "1.5")}))
            && fails(twoInput("talas.cdpRejoin", {expression("gain", "!Gain")})),
        "CDP Pitched Delay, Bounce and Rejoin should refuse values outside CDP's ranges, a fractional count, copies past 2^22 frames and a live field CDP reads once");
    const auto stereoOut = [](std::string type, std::vector<Parameter> parameters, std::string secondInput = {}) {
        std::vector<talas::project::ProjectNode> nodes{tone("osc", "441 hz")};
        std::vector<talas::core::Connection> connections{{"osc", "audioOut", "process", "audioIn"},
            {"process", "leftOut", "out", "audioIn"}, {"process", "rightOut", "out", "rightIn"}};
        if (!secondInput.empty())
        {
            nodes.push_back(tone("side", "300 hz"));
            connections.push_back({"side", "audioOut", "process", secondInput});
        }
        nodes.push_back(node("process", std::move(type), std::move(parameters)));
        return graph(std::move(nodes), std::move(connections));
    };
    Parameter alternate;
    alternate.parameterId = "mode";
    alternate.choice = "alternate";
    require(fails(stereoOut("talas.cdpCascade", {expression("clipSize", "0.001 s")}))
            && fails(stereoOut("talas.cdpCascade", {expression("echoes", "65")}))
            && fails(stereoOut("talas.cdpCascade", {alternate, expression("rand", "0.5"), expression("seed", "0")}))
            && fails(stereoOut("talas.cdpCascade", {expression("clipSize", "!Clip * (1 s)")}))
            && fails(stereoOut("talas.cdpPhasor", {expression("streams", "9")}))
            && fails(stereoOut("talas.cdpPhasor", {expression("outputChannels", "3")}))
            && fails(stereoOut("talas.cdpPhasor", {expression("frequency", "200 hz")}))
            && fails(stereoOut("talas.cdpPhasor", {expression("frequency", "(!Frequency + 10) * (1 hz)")}))
            && fails(stereoOut("talas.cdpPhasor",
                {expression("frequency", "(!Frequency + 10) * (1 hz)"), expression("lowestFrequency", "20 hz")}))
            && fails(stereoOut("talas.cdpPhasor", {expression("streams", "!Streams")}))
            && fails(stereoOut("talas.cdpSpin", {expression("rate", "101")}, "rightIn"))
            && fails(stereoOut("talas.cdpSpin", {expression("boost", "17")}, "rightIn"))
            && fails(stereoOut("talas.cdpSpin", {expression("boost", "!Boost")}, "rightIn")),
        "CDP Cascade, Phasor and Spin should refuse values outside CDP's ranges, seed 0 with draws, more than two output channels, a live frequency without a lowest frequency at most its value and a live field CDP reads once");
    const auto brassageMode = [](std::string value) {
        Parameter parameter;
        parameter.parameterId = "mode";
        parameter.choice = std::move(value);
        return parameter;
    };
    require(fails(stereoOut("talas.cdpBrassage", {expression("pitch", "135")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("pitch", "130")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("reverb"), expression("density", "1.5")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("reverb"), expression("pitch", "0.5")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("reverb"), expression("range", "60 ms")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("reverb"), expression("amp", "1.5")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("scramble"), expression("grainsize", "1 ms")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("granulate"), expression("density", "3")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("seed", "1.5")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("seed", "-1")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("memoryKilobytes", "9")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("seed", "!Seed")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("memoryKilobytes", "!Memory")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("highestPitch", "!High")}))
            && fails(stereoOut("talas.cdpBrassage", {expression("pitch", "!Pitch + 1")}))
            && fails(stereoOut("talas.cdpBrassage", {brassageMode("granulate"), expression("density", "!Density + 1")}))
            && fails(stereoOut("talas.cdpBrassage",
                {brassageMode("scramble"), expression("grainsize", "(!Grain + 20) / 1000 * (1 s)")})),
        "CDP Brassage should refuse values outside its mode's ranges, a latency past 2^22 frames, a seed or memory that is not whole within its range or is live, and a live field beyond its bound");
}
}

int main()
{
    try
    {
        testLowerResolutionPlaysItsPort();
        testLowerResolutionLatencyAlignsTheDryBranch();
        testLowerResolutionFollowsItsBitResolution();
        testRingModulatePlaysItsPort();
        testRingModulateTakesAConnectedFrequency();
        testTremoloPlaysItsPort();
        testPanPlaysItsPortInStereo();
        testTapDelayPlaysItsPortInStereo();
        testEchoPlaysItsPort();
        testVaryingDelayPlaysItsPort();
        testReverbPlaysItsPortInStereo();
        testVariableFilterPlaysItsPort();
        testVariableFilterFollowsItsAcuityAndFrequency();
        testSweepingFilterFollowsItsAcuityAndFrequencies();
        testFilterBanksFollowTheirQ();
        testPhasingFollowsItsDelay();
        testEchoFollowsItsMixAndFeedback();
        testDistortionsFollowTheirFields();
        testOverloadShuffleReformAndFractalFollowTheirFields();
        testPhaseVocoderNodesPlayTheirPorts();
        testSpectralProcessNodesFollowTheirFields();
        testHiliteFilterPluckAndStepNodesPlayTheirPorts();
        testStrangeShiftAndRepitchTransposeNodesFollowTheirFields();
        testWaverTranspChorusAndNoiseNodesFollowTheirFields();
        testStretchSpectrumAndScatterNodesFollowTheirFields();
        testHiliteArpegGreqAndBandNodesFollowTheirFields();
        testBlurSpreadAndGlisNodesFollowTheirFields();
        testCombineAndVocodeNodesFollowTheirFields();
        testMorphNodesPlayTheirFiles();
        testRepitchTransposefNodeFollowsItsFields();
        testFormantsPutAndVowelsNodesPlayTheirFields();
        testBlurBlurAndBltrNodesFollowTheirFields();
        testFocusFocusNodeFollowsItsFields();
        testPitchTunePickAndChordNodesFollowTheirFields();
        testPitchFileNodesReadTheirPitchSource();
        testWholeSoundAndFreezeNodesPlayTheirPorts();
        testEnvelopeTimestretchPlaysItsPort();
        testVaryingBank2AndTremoloFineNodesFollowTheirFields();
        testWavesetDistortionNodesPlayTheirPorts();
        testRoomReverbNodePlaysItsPortInStereo();
        testSampleProcessNodesPlayTheirPorts();
        testTwoInputProcessNodesPlayTheirPorts();
        testFastConvolutionNodePlaysItsImpulseSample();
        testRestorationNodesPlayTheirPorts();
        testSubmixPairNodesPlayTheirPorts();
        testEnvelopeNodesPlayTheirPorts();
        testRescaleNodesPlayTheirPorts();
        testBrassageNodePlaysItsPort();
        testFixedFilterPlaysItsPort();
        testPhasingPlaysItsPort();
        testLohiFilterPlaysItsPort();
        testSweepingFilterPlaysItsPort();
        testUserBankPlaysItsPort();
        testDistortOverloadPlaysItsPort();
        testVaryingBankPlaysItsPort();
        testStadiumEchoPlaysItsPortInStereo();
        testDistortReformPlaysItsPortAndAlignsTheDryBranch();
        testDistortOmitPlaysItsPort();
        testDistortReverseAndEnvelopePlayTheirPorts();
        testDistortHarmonicAndFractalPlayTheirPorts();
        testDistortMultiplyAndDividePlayTheirPorts();
        testDistortShufflePlaysItsPort();
        testCdpNodesRejectValuesOutsideCdpRanges();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
