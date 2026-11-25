#include "talas/app/AnalysisToolsModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

// End-to-end scenarios of introductory walkthroughs and tutorials: each
// builds the Sound a walkthrough describes from our prototypes, renders it
// offline and in realtime blocks (which must agree) and checks the result
// the walkthrough states.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;
constexpr double sampleRate = 48000.0;
constexpr auto scalar = talas::expr::ValueDomain::Scalar;

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

Parameter resource(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

Parameter boolean(std::string id, bool value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

Parameter choice(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

Parameter text(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectNode node(std::string id, std::string type, std::vector<Parameter> parameters = {})
{
    return {std::move(id), std::move(type), std::move(parameters)};
}

talas::core::Connection connection(std::string from, std::string fromPort, std::string to, std::string toPort)
{
    return {std::move(from), std::move(fromPort), std::move(to), std::move(toPort)};
}

// A Val document whose given node's port is heard on the output.
talas::project::ProjectDocument heard(std::vector<talas::project::ProjectNode> nodes,
    std::vector<talas::core::Connection> connections, std::string source, std::string port = "audioOut")
{
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.graph.nodes = std::move(nodes);
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    connections.push_back(connection(std::move(source), std::move(port), "out", "audioIn"));
    document.graph.connections = std::move(connections);
    return document;
}

// A control value heard sample for sample: a Level's left gain on a
// constant 1.
talas::project::ProjectDocument heardValue(std::vector<talas::project::ProjectNode> nodes,
    std::vector<talas::core::Connection> connections, std::string field)
{
    nodes.push_back(node("one", "talas.constant", {expression("value", "1")}));
    nodes.push_back(node("gain", "talas.level",
        {expression("left", std::move(field)), expression("right", "1"), boolean("noGain", false)}));
    connections.push_back(connection("one", "value", "gain", "audioIn"));
    return heard(std::move(nodes), std::move(connections), "gain", "leftOut");
}

// Renders offline and in realtime blocks of the same size, which must agree;
// the audio input channels carry the given signals (exact in float).
std::vector<double> render(const talas::project::ProjectDocument& document, std::size_t frames,
    talas::runtime::RenderSettings settings = {sampleRate, 64}, std::vector<std::vector<double>> inputs = {})
{
    for (std::size_t channel = 0; channel < inputs.size(); ++channel)
        settings.audioInputChannels.push_back({channel + 1, inputs[channel]});
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "the scenario should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    require(offline.ok(), "the scenario should render offline: "
        + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "the scenario should prepare for realtime: " + std::string{session.error()});
    std::vector<double> left(frames);
    std::vector<double> right(frames);
    std::vector<std::vector<float>> blockInputs(inputs.size(), std::vector<float>(settings.maxBlockFrames));
    std::vector<const float*> inputChannels;
    for (const auto& channel : blockInputs)
        inputChannels.push_back(channel.data());
    for (std::size_t offset = 0; offset < frames; offset += settings.maxBlockFrames)
    {
        const auto count = std::min<std::size_t>(settings.maxBlockFrames, frames - offset);
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
            for (std::size_t frame = 0; frame < count; ++frame)
                blockInputs[channel][frame] = static_cast<float>(inputs[channel][offset + frame]);
        require(inputs.empty()
                ? session.render(std::span{left}.subspan(offset, count), std::span{right}.subspan(offset, count))
                : session.render({inputChannels.data(), inputChannels.size()}, std::span{left}.subspan(offset, count),
                      std::span{right}.subspan(offset, count)),
            "the scenario should render in realtime");
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(std::abs(left[frame] - offline.mono[frame]) <= 1.0e-9,
            "realtime should match offline at frame " + std::to_string(frame));
    }
    return offline.mono;
}

std::size_t at(double seconds)
{
    return static_cast<std::size_t>(std::llround(seconds * sampleRate));
}

// Mean frequency between rising zero crossings, in Hz.
double measuredFrequency(std::span<const double> samples)
{
    std::vector<double> crossings;
    for (std::size_t index = 1; index < samples.size(); ++index)
    {
        if (samples[index - 1] < 0.0 && samples[index] >= 0.0)
        {
            const auto fraction = samples[index - 1] / (samples[index - 1] - samples[index]);
            crossings.push_back(static_cast<double>(index - 1) + fraction);
        }
    }
    require(crossings.size() >= 2, "a periodic signal should cross zero");
    return sampleRate * static_cast<double>(crossings.size() - 1) / (crossings.back() - crossings.front());
}

// The frequency between two times, in Hz.
double frequencyBetween(std::span<const double> samples, double from, double to)
{
    return measuredFrequency(samples.subspan(at(from), at(to) - at(from)));
}

double noteNumber(double hertz)
{
    return 69.0 + 12.0 * std::log2(hertz / 440.0);
}

// The level of a frequency in samples.
double toneLevel(std::span<const double> samples, double frequency)
{
    auto sine = 0.0;
    auto cosine = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(index) / sampleRate;
        sine += samples[index] * std::sin(phase);
        cosine += samples[index] * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) / static_cast<double>(samples.size());
}

talas::project::ProjectNode oscillator(std::string id, std::string frequency)
{
    return node(std::move(id), "talas.oscillator", {expression("frequency", std::move(frequency))});
}

// A Sound, however complex, can be another's input, a
// Script that plays a score as well as a source: here a Script's score
// through a Level (a modifier), mixed with an Oscillator (a combiner) and
// heard through another Level. A parameter can be a constant, a function
// of time, a hot control, a Sound, or arithmetic of them.
void testSoundsChainAndTheirParametersVary()
{
    const auto frames = at(0.05);
    const auto tone = render(heard({oscillator("tone", "441 hz")}, {}, "tone"), frames);
    const auto other = render(heard({oscillator("other", "882 hz")}, {}, "other"), frames);
    const auto chained = render(heard({oscillator("tone", "441 hz"),
                                          node("score", "talas.script", {text("script", "tone start: 0 s. tone start: 0.01 s.")}),
                                          node("half", "talas.level", {expression("left", "0.5"), expression("right", "0.5")}),
                                          oscillator("other", "882 hz"), node("mix", "talas.mixerV2"),
                                          node("quieter", "talas.level", {expression("left", "0.5"), expression("right", "0.5")})},
                                    {connection("tone", "audioOut", "score", "inputs"),
                                        connection("score", "leftOut", "half", "audioIn"),
                                        connection("half", "leftOut", "mix", "inputs"),
                                        connection("other", "audioOut", "mix", "inputs"),
                                        connection("mix", "leftOut", "quieter", "audioIn")},
                                    "quieter", "leftOut"),
        frames);
    const auto later = at(0.01);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto scored = tone[frame] + (frame >= later ? tone[frame - later] : 0.0);
        require(std::abs(chained[frame] - 0.5 * (0.5 * scored + other[frame])) <= 1.0e-9,
            "a Script's score should serve as the input to a chain of modifiers and combiners, at frame "
                + std::to_string(frame));
    }

    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Volume", 0.25, scalar}};
    const auto value = [&](std::string field, std::vector<talas::project::ProjectNode> nodes = {}) {
        return render(heardValue(std::move(nodes), {}, std::move(field)), at(0.02), settings);
    };
    require(std::ranges::all_of(value("0.5"), [](double sample) { return sample == 0.5; }),
        "a parameter can be a constant");
    require(std::ranges::all_of(value("!Volume"), [](double sample) { return sample == 0.25; }),
        "a parameter can be a hot control");
    require(std::ranges::all_of(value("!Volume * 2 + 0.1"), [](double sample) { return std::abs(sample - 0.6) < 1.0e-12; }),
        "a parameter can be arithmetic of hot controls and constants");
    const auto ramp = value("1 ramp: 0.02 s");
    require(ramp[at(0.005)] > 0.2 && ramp[at(0.005)] < 0.3 && ramp[at(0.015)] > 0.7 && ramp[at(0.015)] < 0.8
            && std::ranges::is_sorted(ramp),
        "a parameter can be a function of time, a ramp rising over 0.02 s");
    const auto pasted = value("sine L * !Volume", {oscillator("sine", "100 hz")});
    // A Sound in a field is read once a millisecond.
    const auto [low, high] = std::ranges::minmax(pasted);
    require(low < -0.2 && high > 0.2 && low >= -0.25 && high <= 0.25,
        "a parameter can be a Sound, an Oscillator times a hot control");
}

// Chorusing using random. randLFO1 is a sine at
// 0.37 hz + 1 s random hz, a subaudio frequency from -0.63 to 1.37 hz that
// changes once a second; the delay's DelayScale 0.7 + (randLFO1 L abs *
// !Delay * 0.3) holds at 0.7 of the 25 ms Delay with !Delay at 0, the
// source unchorused, and with !Delay at 1 moves smoothly within 0.4 to 1
// of it at the LFO's random rate.
void testChorusingVariesTheDelay()
{
    const auto lfoFrequency = render(heardValue({}, {}, "(0.37 hz + 1 s random hz) removeUnits"), at(5.0));
    std::vector<double> distinct;
    for (std::size_t second = 0; second < 5; ++second)
    {
        const auto first = lfoFrequency[at(static_cast<double>(second)) + 10];
        for (auto frame = at(static_cast<double>(second)) + 10; frame < at(static_cast<double>(second) + 1.0) - 10; ++frame)
        {
            require(lfoFrequency[frame] == first,
                "1 s random hz should change only once a second, at frame " + std::to_string(frame));
        }
        require(first >= -0.63 && first <= 1.37,
            "the LFO's frequency should lie from -0.63 to 1.37 hz, not " + std::to_string(first));
        if (std::ranges::find(distinct, first) == distinct.end())
            distinct.push_back(first);
    }
    require(distinct.size() >= 3, "the LFO's frequency should be random from second to second");

    const auto delayScale = [&](double delay) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Delay", delay, scalar}};
        return render(heardValue({oscillator("randLFO1", "0.37 hz + 1 s random hz")}, {},
                          "0.7 + (randLFO1 L abs * !Delay * 0.3)"),
            at(5.0), settings);
    };
    require(std::ranges::all_of(delayScale(0.0), [](double scale) { return std::abs(scale - 0.7) < 1.0e-12; }),
        "with !Delay at 0 the delay should hold at 0.7 of 25 ms, no chorusing");
    const auto chorused = delayScale(1.0);
    const auto [low, high] = std::ranges::minmax(chorused);
    double step = 0.0;
    for (std::size_t frame = 1; frame < chorused.size(); ++frame)
        step = std::max(step, std::abs(chorused[frame] - chorused[frame - 1]));
    // Read once a millisecond, a 1.37 hz LFO moves at most 0.3 * 2 pi *
    // 1.37 / 1000 a step.
    require(low >= 0.4 && high <= 1.0 && high - low > 0.1 && step < 0.005,
        "with !Delay at 1 the delay should move smoothly within 0.4 to 1 of 25 ms, from "
            + std::to_string(low) + " to " + std::to_string(high));

    const auto source = oscillator("source", "441 hz");
    const auto delayed = render(heard({source, node("chorus", "talas.delayWithFeedback",
                                                   {expression("delay", "25 ms"), expression("feedback", "0"),
                                                       expression("mix", "1"),
                                                       expression("delayScale", "0.7 + (randLFO1 L abs * 0 * 0.3)")}),
                                          oscillator("randLFO1", "0.37 hz + 1 s random hz")},
                                    {connection("source", "audioOut", "chorus", "audioIn")}, "chorus"),
        at(0.1));
    const auto plain = render(heard({oscillator("source", "441 hz")}, {}, "source"), at(0.1));
    for (auto frame = at(0.0175); frame < delayed.size(); ++frame)
    {
        require(std::abs(delayed[frame] - plain[frame - at(0.0175)]) < 1.0e-6,
            "a DelayScale of 0.7 should delay the source by 17.5 ms, at frame " + std::to_string(frame));
    }
}

// A controller's range of 0 to 1 becomes 750 to 1000 hz by
// scaling by the difference and offsetting by the minimum, the general
// form being !controller * (maximum - minimum) + minimum.
void testControllerRangeBecomesAFrequencyRange()
{
    for (const auto& [value, expected] : {std::pair{0.0, 750.0}, {0.5, 875.0}, {1.0, 1000.0}})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Frequency", value, scalar}};
        const auto played = render(heard({oscillator("osc", "!Frequency * 250 hz + 750 hz")}, {}, "osc"), at(0.25),
            settings);
        require(std::abs(frequencyBetween(played, 0.05, 0.25) - expected) < 0.05,
            "!Frequency at " + std::to_string(value) + " should give "
                + std::to_string(expected) + " hz");
    }
}

// A GraphicalEnvelope pasted into a Frequency field,
// triggered by a key, bends the pitch along its shape; times 12 nn it
// deviates by an octave above 2 c.
void testEnvelopeBendsThePitchByAnOctave()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyDown", 1.0, scalar}};
    const auto played = render(heard({node("GraphicalEnvelope1", "talas.graphicalEnvelope",
                                          {text("envelope", "{0 s} {0} {0.1 s} {1} {0.4 s} {1}"), expression("gate", "!KeyDown")}),
                                         oscillator("osc", "GraphicalEnvelope1 L * 12 nn + 2 c")},
                                   {}, "osc"),
        at(0.4), settings);
    const auto low = 440.0 * std::exp2((36.0 - 69.0) / 12.0);
    require(std::abs(frequencyBetween(played, 0.15, 0.35) - 2.0 * low) < 0.5,
        "at the envelope's top the pitch should be an octave above 2 c");
    require(frequencyBetween(played, 0.0, 0.08) < 1.6 * low,
        "as the envelope rises the pitch should rise from 2 c");
}

// Filter cutoff L * (!Cutoff * 11050 hz + 200 hz), a
// maximum between 200 hz and 11250 hz as !Cutoff goes from 0 to 1, scaled
// by the envelope a Sound called filter cutoff makes; heard here as an
// Oscillator's frequency.
void testEnvelopeScalesAFrequencyRange()
{
    const auto played = [](double cutoff, std::string level) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Cutoff", cutoff, scalar}};
        return render(heard({node("filterCutoff", "talas.graphicalEnvelope",
                                 {text("envelope", "{0 s} {" + level + "} {1 s} {" + level + "}"), expression("gate", "1")}),
                             oscillator("osc", "filterCutoff L * (!Cutoff * 11050 hz + 200 hz)")},
                          {}, "osc"),
            at(0.2), settings);
    };
    for (const auto& [cutoff, level, expected] : {std::tuple{0.0, std::string{"1"}, 200.0}, {0.1, std::string{"1"}, 1305.0},
             {0.1, std::string{"0.5"}, 652.5}})
    {
        require(std::abs(frequencyBetween(played(cutoff, level), 0.05, 0.2) - expected) < 0.5,
            "!Cutoff at " + std::to_string(cutoff) + " and the envelope at " + level + " should give "
                + std::to_string(expected) + " hz");
    }
}

// Each key down opens the filter, letting more of the highs
// through, and the release closes it again; a 50 ms release stands in for
// the example's 5 s.
void testTheFilterOpensWhileAKeyIsDown()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Cutoff", 0.5, scalar}, {"KeyDown", 0.0, scalar}};
    settings.eventValueChanges = {{at(0.1), "KeyDown", 1.0, scalar}, {at(0.3), "KeyDown", 0.0, scalar}};
    const auto played = render(heard({oscillator("highs", "4000 hz"),
                                         node("cutoff", "talas.adsr",
                                             {expression("gate", "!KeyDown"), expression("attackTime", "10 ms"),
                                                 expression("decayTime", "10 ms"), expression("sustainLevel", "0.75"),
                                                 expression("releaseTime", "50 ms"), expression("scale", "1")}),
                                         node("filter", "talas.filter",
                                             {choice("type", "lowPass"),
                                                 expression("frequency", "cutoff L * (!Cutoff * 11050 hz + 200 hz) + 50 hz")})},
                                   {connection("highs", "audioOut", "filter", "audioIn")}, "filter"),
        at(0.5), settings);
    const auto level = [&](double from, double to) {
        return toneLevel(std::span<const double>{played}.subspan(at(from), at(to) - at(from)), 4000.0);
    };
    require(level(0.15, 0.3) > 10.0 * level(0.02, 0.1) && level(0.15, 0.3) > 10.0 * level(0.45, 0.5),
        "the filter should open while the key is down and close after its release (" + std::to_string(level(0.02, 0.1))
            + ", " + std::to_string(level(0.15, 0.3)) + ", " + std::to_string(level(0.45, 0.5)) + ")");
}

// TimeLeft & Trigger, a SoundToEvent whose Value is a
// Smalltalk program reading the !TimeLeft it generates: a ramp is
// triggered whenever the input's level exceeds the threshold and
// (1 - !TimeLeft asLogicValue) is positive; !TimeLeft, one minus the ramp,
// goes from 1 down to 0 over the capture time, and cannot be triggered
// again until it reaches 0 and the level exceeds the threshold again.
// Here !Level stands for the env follower and 0.06 s for the six seconds.
void testTimeLeftAndTriggerFeedsItselfBack()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Level", 0.0, scalar}, {"Threshold", 5.0, scalar}};
    settings.eventValueChanges = {{at(0.01), "Level", 1.0, scalar}, {at(0.02), "Level", 0.0, scalar},
        {at(0.03), "Level", 1.0, scalar}, {at(0.04), "Level", 0.0, scalar}, {at(0.1), "Level", 1.0, scalar},
        {at(0.11), "Level", 0.0, scalar}};
    const auto timeLeft = render(heardValue({node("timeLeftAndTrigger", "talas.soundToGlobalController",
                                                {text("generatedEvent", "!TimeLeft"),
                                                    text("value",
                                                        "| inputTrigger outputTriggerDuration |\n"
                                                        "\"The trigger to start our trigger and how long it should be on.\"\n"
                                                        "inputTrigger := !Level threshold: (!Threshold * 0.1).\n"
                                                        "outputTriggerDuration := 0.06 s.\n"
                                                        "1 - (inputTrigger * (1 - !TimeLeft asLogicValue)\n"
                                                        "ramp: outputTriggerDuration)")})},
                                     {}, "!TimeLeft"),
        at(0.25), settings);
    const auto near = [&](double seconds, double expected) { return std::abs(timeLeft[at(seconds)] - expected) < 0.05; };
    require(near(0.005, 0.0), "!TimeLeft should be 0 before the level first exceeds the threshold");
    require(near(0.012, 1.0) && near(0.04, 0.5) && near(0.065, 0.083),
        "!TimeLeft should go from 1 down to 0 over the capture time");
    require(near(0.035, 0.583), "the level exceeding the threshold again should not retrigger it");
    require(near(0.09, 0.0), "!TimeLeft should rest at 0 once the ramp ends");
    require(near(0.102, 1.0) && near(0.13, 0.5) && near(0.2, 0.0),
        "a level over the threshold after !TimeLeft reaches 0 should trigger it again");
}

// A continuously variable offset added to the keyboard's
// pitch, !Pitch + !Frequency nn, so that !Frequency at 7 plays a fifth
// above the key; !KeyNumber may be detuned by a fraction of a half step.
void testPitchOffsetByAController()
{
    for (const auto offset : {0.0, 7.0, -12.0})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}, {"Frequency", offset, scalar}};
        const auto played = render(heard({oscillator("osc", "!Pitch + !Frequency nn")}, {}, "osc"), at(0.25), settings);
        require(std::abs(noteNumber(frequencyBetween(played, 0.05, 0.25)) - (60.0 + offset)) < 0.01,
            "!Pitch + !Frequency nn should play " + std::to_string(offset) + " half steps from the key");
    }
    // The key number as a note number plus a fraction of a
    // half step that !Detune1 gives.
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyNumber", 60.0, scalar}, {"Detune1", 0.25, scalar}};
    const auto detuned = render(heard({oscillator("osc", "!KeyNumber smoothed nn + !Detune1 nn")}, {}, "osc"), at(0.25), settings);
    require(std::abs(noteNumber(frequencyBetween(detuned, 0.05, 0.25)) - 60.25) < 0.01,
        "!KeyNumber smoothed nn + !Detune1 nn should play a quarter step above note 60");
}

// For an either-or condition, one Sound's amplitude is
// scaled by !aGate and the other's by (1 - !aGate).
void testEitherOrByAGate()
{
    for (const auto gate : {0.0, 1.0})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"aGate", gate, scalar}};
        const auto played = render(heard({node("one", "talas.oscillator", {expression("frequency", "440 hz"), expression("amplitude", "!aGate")}),
                                              node("other", "talas.oscillator", {expression("frequency", "660 hz"), expression("amplitude", "1 - !aGate")}),
                                              node("mix", "talas.mixerV2")},
                                        {connection("one", "audioOut", "mix", "inputs"), connection("other", "audioOut", "mix", "inputs")},
                                        "mix", "leftOut"),
            at(0.2), settings);
        const auto heard440 = toneLevel(std::span{played}.subspan(at(0.05), at(0.1)), 440.0);
        const auto heard660 = toneLevel(std::span{played}.subspan(at(0.05), at(0.1)), 660.0);
        require(gate == 1.0 ? heard440 > 0.9 && heard660 < 0.01 : heard660 > 0.9 && heard440 < 0.01,
            "!aGate at " + std::to_string(gate) + " should play one Sound or the other");
    }
}

// !Frequency * 12 nn spans an octave of note numbers, and
// adding 60 nn makes the fader run from middle C to the C above.
void testFaderCoversAnOctaveFromMiddleC()
{
    for (const auto& [value, expected] : {std::pair{0.0, 60.0}, {0.5, 66.0}, {1.0, 72.0}})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Frequency", value, scalar}};
        const auto played = render(heard({oscillator("osc", "!Frequency * 12 nn + 60 nn")}, {}, "osc"), at(0.3),
            settings);
        require(std::abs(noteNumber(frequencyBetween(played, 0.05, 0.3)) - expected) < 0.01,
            "the fader at " + std::to_string(value) + " should play note "
                + std::to_string(expected));
    }
}

// Range conversions. !Fader * 2 - 1 takes (0,1) to (-1,1); a
// Sound's L * 0.5 + 0.5 takes its (-1,1) to (0,1); scaling (0,1) by half the
// sample rate in hz spans every frequency; !Fader1 * 6 nn + 4 c spans middle
// C to the F sharp above.
void testRangeConversions()
{
    for (const auto& [value, expected] : {std::pair{0.0, -1.0}, {0.25, -0.5}, {1.0, 1.0}})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Fader", value, scalar}};
        const auto played = render(heardValue({}, {}, "!Fader * 2 - 1"), 128, settings);
        require(std::abs(played.back() - expected) < 1.0e-12,
            "!Fader * 2 - 1 should map " + std::to_string(value) + " to " + std::to_string(expected));
    }
    // A sine's -1..1, shifted into 0..1, read once per millisecond.
    const auto shifted = render(heardValue({oscillator("sine", "10 hz")}, {}, "sine L * 0.5 + 0.5"), at(0.2));
    const auto [low, high] = std::ranges::minmax(shifted);
    require(low >= 0.0 && low < 0.01 && high <= 1.0 && high > 0.99,
        "L * 0.5 + 0.5 should move a Sound's (-1,1) into (0,1)");
    talas::runtime::RenderSettings quarter{sampleRate, 64};
    quarter.eventValues = {{"Fader", 0.25, scalar}};
    const auto fraction = render(heard({oscillator("osc", "!Fader * SignalProcessor sampleRate * 0.5 hz")}, {}, "osc"),
        at(0.05), quarter);
    require(std::abs(frequencyBetween(fraction, 0.01, 0.05) - 0.25 * 0.5 * sampleRate) < 1.0,
        "(0,1) scaled by half the sample rate in hz should span every frequency");
    talas::runtime::RenderSettings top{sampleRate, 64};
    top.eventValues = {{"Fader1", 1.0, scalar}};
    const auto pitched = render(heard({oscillator("osc", "!Fader1 * 6 nn + 4 c")}, {}, "osc"), at(0.3), top);
    require(std::abs(noteNumber(frequencyBetween(pitched, 0.05, 0.3)) - 66.0) < 0.01,
        "!Fader1 * 6 nn + 4 c should reach note 66 at the fader's top");
}

// 441.0 hz inverse samp is 100 samples at 44.1 kHz, and a
// duration's inverse in a Frequency field gives the rate of an Oscillator
// used as a repeating envelope (2 s inverse: once every 2 seconds).
void testInversesConvertPeriodsAndFrequencies()
{
    talas::runtime::RenderSettings cd{44100.0, 64};
    const auto compiled = talas::runtime::compileProject(
        heardValue({}, {}, "441.0 hz inverse samp removeUnits"), talas::core::makeBuiltinNodeRegistry(), cd);
    require(compiled.ok(), "the period should compile");
    const auto period = talas::runtime::renderOffline(*compiled.graph, 64);
    require(period.ok() && std::abs(period.mono.back() - 100.0) < 1.0e-9,
        "the period of 441 hz should be 100 samples at 44.1 kHz");
    const auto envelope = render(heard({oscillator("env", "2 s inverse")}, {}, "env"), at(6.0));
    require(std::abs(frequencyBetween(envelope, 0.0, 6.0) - 0.5) < 1.0e-3,
        "an Oscillator at 2 s inverse should repeat once every 2 seconds");
}

// 4 c + !LocalTime nn raises the pitch a half step a second
// from middle C.
void testPitchRisesWithLocalTime()
{
    const auto played = render(heard({oscillator("osc", "4 c + !LocalTime nn")}, {}, "osc"), at(3.0));
    for (const auto second : {0.5, 1.5, 2.5})
    {
        require(std::abs(noteNumber(frequencyBetween(played, second - 0.05, second + 0.05)) - (60.0 + second)) < 0.02,
            "the pitch should rise one half step per second, at " + std::to_string(second) + " s");
    }
}

// (1 repeatingRamp: 3 s) * 440 hz + 440 hz glides from 440 hz
// up an octave over three seconds and starts again.
void testRepeatingRampGlidesAnOctave()
{
    const auto played = render(heard({oscillator("osc", "(1 repeatingRamp: 3 s) * 440 hz + 440 hz")}, {}, "osc"),
        at(4.0));
    for (const auto& [second, expected] : {std::pair{0.75, 550.0}, {2.25, 770.0}, {3.75, 550.0}})
    {
        require(std::abs(frequencyBetween(played, second - 0.02, second + 0.02) - expected) < 2.0,
            "the glide should be at " + std::to_string(expected) + " hz at "
                + std::to_string(second) + " s");
    }
    require(frequencyBetween(played, 2.9, 2.98) > 860.0 && frequencyBetween(played, 3.02, 3.1) < 460.0,
        "the glide should reach 880 hz and start again at 440 hz");
}

// An Oscillator on Ramp pasted into a Frequency field as
// Oscillator L * (300 - 30) hz + 30 hz glides from 30 hz to 300 hz, once
// each of its periods (here 1 s for the example's 15).
void testAPastedRampSpansThirtyToThreeHundredHertz()
{
    const auto played = render(heard({node("glide", "talas.oscillator",
                                          {expression("frequency", "1 s inverse"), expression("amplitude", "1"), resource("wavetable", "Ramp")}),
                                         oscillator("partial", "glide L * (300 - 30) hz + 30 hz")},
                                   {}, "partial"),
        at(2.0));
    for (const auto start : {0.0, 1.0})
    {
        const auto low = frequencyBetween(played, start + 0.02, start + 0.12);
        const auto high = frequencyBetween(played, start + 0.85, start + 0.95);
        require(low > 30.0 && low < 70.0 && high > 250.0 && high < 300.0,
            "the pasted Ramp should glide from 30 hz up to 300 hz each period (" + std::to_string(low) + " and "
                + std::to_string(high) + ")");
    }
}

// 1 bpm: 60 in a Trigger field triggers a Sound once per
// second, and 1 bpm: (60 / 3.5) loops a 3.5 s recording once every 3.5 s
// (our Sample's trigger is its Gate field).
void testBpmTriggersRestartARecording()
{
    // A recording of 0.25 s: a 1 kHz burst.
    std::vector<double> burst(at(0.25));
    for (std::size_t index = 0; index < burst.size(); ++index)
        burst[index] = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(index) / sampleRate);
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = {{"Samples/Burst", burst}};
    const auto played = [&](std::string trigger, double seconds) {
        return render(heard({node("sample", "talas.sample",
                                {resource("sample", "Samples/Burst"), expression("gate", std::move(trigger))})},
                          {}, "sample"),
            at(seconds), settings);
    };
    const auto sounding = [](std::span<const double> samples, double from, double to) {
        return std::ranges::any_of(samples.subspan(at(from), at(to) - at(from)),
            [](double sample) { return std::abs(sample) > 0.1; });
    };
    const auto everySecond = played("1 bpm: 60", 3.2);
    for (const auto start : {0.0, 1.0, 2.0, 3.0})
    {
        require(sounding(everySecond, start + 0.01, start + 0.2),
            "1 bpm: 60 should trigger the recording at " + std::to_string(start) + " s");
        if (start + 0.6 < 3.2)
            require(!sounding(everySecond, start + 0.3, start + 0.95), "the recording should end between triggers");
    }
    const auto everyThreeAndAHalf = played("1 bpm: (60 / 3.5)", 7.4);
    require(sounding(everyThreeAndAHalf, 3.51, 3.7) && sounding(everyThreeAndAHalf, 7.01, 7.2)
            && !sounding(everyThreeAndAHalf, 1.0, 3.45) && !sounding(everyThreeAndAHalf, 4.0, 6.95),
        "1 bpm: (60 / 3.5) should restart the recording every 3.5 seconds");
}

// !KeyDown nextRandom * 12 nn + 48 nn gives a new pitch,
// between 2 c and 4 c, at each key press; indexing #(53 50 48 45 43) by
// !KeyDown nextRandom abs * 5 chooses each press's pitch from the list,
// the 5 reaching the last entry since 4.999 truncates to 4.
void testKeyPressesChooseRandomPitches()
{
    constexpr int presses = 24;
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyDown", 0.0, scalar}};
    for (int press = 0; press < presses; ++press)
    {
        settings.eventValueChanges.push_back({at(0.1 + 0.3 * press), "KeyDown", 1.0, scalar});
        settings.eventValueChanges.push_back({at(0.3 + 0.3 * press), "KeyDown", 0.0, scalar});
    }
    const auto pitches = [&](std::string frequency) {
        const auto played = render(heard({oscillator("osc", std::move(frequency))}, {}, "osc"),
            at(0.1 + 0.3 * presses), settings);
        std::vector<double> result;
        for (int press = 0; press < presses; ++press)
            result.push_back(noteNumber(frequencyBetween(played, 0.12 + 0.3 * press, 0.38 + 0.3 * press)));
        return result;
    };
    const auto ranged = pitches("!KeyDown nextRandom * 12 nn + 48 nn");
    require(std::ranges::all_of(ranged, [](double pitch) { return pitch >= 35.99 && pitch <= 60.01; }),
        "each press's pitch should lie between 2 c and 4 c");
    std::set<long> distinct;
    for (const auto pitch : ranged)
        distinct.insert(std::lround(pitch * 100.0));
    require(distinct.size() > presses / 2, "each press should give a different pitch");

    const auto listed = pitches("(!KeyDown nextRandom abs * 5 of: #(53 50 48 45 43)) nn");
    std::set<long> chosen;
    for (const auto pitch : listed)
    {
        const auto nearest = std::lround(pitch);
        require(std::abs(pitch - static_cast<double>(nearest)) < 0.02
                && std::set<long>{53, 50, 48, 45, 43}.contains(nearest),
            "each press's pitch should come from the list");
        chosen.insert(nearest);
    }
    require(chosen.size() >= 4, "the presses should reach most of the list's pitches");
}

// !Pitch + ((0.125 s random abs * 5 of: #(10 7 5 2 0)) nn)
// adds an interval chosen at random every eighth of a second to the key's
// pitch.
void testRandomIntervalEveryEighthOfASecond()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    const auto played = render(
        heard({oscillator("osc", "!Pitch + ((0.125 s random abs * 5 of: #(10 7 5 2 0)) nn)")}, {}, "osc"), at(2.0),
        settings);
    std::set<long> intervals;
    for (int step = 0; step < 16; ++step)
    {
        // Within each eighth of a second the pitch holds.
        const auto start = 0.125 * step;
        const auto first = noteNumber(frequencyBetween(played, start + 0.01, start + 0.06));
        const auto second = noteNumber(frequencyBetween(played, start + 0.065, start + 0.115));
        const auto interval = std::lround(first - 60.0);
        require(std::abs(first - second) < 0.02 && std::abs(first - 60.0 - static_cast<double>(interval)) < 0.02
                && std::set<long>{10, 7, 5, 2, 0}.contains(interval),
            "each eighth of a second should hold one of the listed intervals above the key");
        intervals.insert(interval);
    }
    require(intervals.size() >= 3, "the interval should change at random");
}

// In a Sample's Frequency the random interval changes the
// pitch every eighth of a second but does not retrigger the sample, which
// goes on decaying.
void testRandomIntervalsDoNotRetriggerTheSample()
{
    std::vector<double> decay(192000);
    for (std::size_t index = 0; index < decay.size(); ++index)
        decay[index] = 1.0 - static_cast<double>(index) / 192000.0;
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = {{"Samples/Decay", decay}};
    settings.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    const auto played = render(heard({node("sample", "talas.sample",
                                          {resource("sample", "Samples/Decay"),
                                              expression("frequency", "!Pitch + ((0.125 s random abs * 5 of: #(10 7 5 2 0)) nn)")})},
                                   {}, "sample"),
        at(1.0), settings);
    for (std::size_t frame = 1; frame < played.size(); ++frame)
        require(played[frame] <= played[frame - 1] + 1.0e-9, "the sample should not be retriggered");
    std::set<long> steps;
    for (int eighth = 0; eighth < 8; ++eighth)
    {
        const auto from = at(0.125 * eighth + 0.01);
        const auto to = at(0.125 * eighth + 0.115);
        const auto step = (played[from] - played[to]) / static_cast<double>(to - from) * 192000.0;
        const auto interval = std::lround(12.0 * std::log2(step));
        require(std::abs(step - std::pow(2.0, static_cast<double>(interval) / 12.0)) < 1.0e-3
                && std::set<long>{10, 7, 5, 2, 0}.contains(interval),
            "each eighth should play the sample at one of the listed intervals: " + std::to_string(step));
        steps.insert(interval);
    }
    require(steps.size() >= 2, "the pitch should change every eighth of a second");
}

// (1 bpm: 240) nextRandom draws a new random number
// four times a second; !KeyDown nextRandom abs lies between 0 and 1 and,
// times 5, between 0 and 5; smoothed glides from one value to the next in
// 100 ms instead of jumping, as in random intervals above !Pitch.
void testRandomValuesAndSmoothing()
{
    const auto valueOf = [](std::string field, double seconds, talas::runtime::RenderSettings settings = {sampleRate, 64}) {
        return render(heardValue({}, {}, std::move(field)), at(seconds), settings);
    };
    const auto quarters = valueOf("(1 bpm: 240) nextRandom", 2.0);
    std::set<long> drawn;
    for (int quarter = 0; quarter < 8; ++quarter)
    {
        const auto held = std::span{quarters}.subspan(at(0.25 * quarter + 0.01), at(0.23));
        require(std::ranges::all_of(held, [&](double value) { return value == held.front(); })
                && held.front() >= -1.0 && held.front() <= 1.0,
            "(1 bpm: 240) nextRandom should hold one random number for each quarter second");
        drawn.insert(std::lround(held.front() * 1.0e6));
    }
    require(drawn.size() >= 7, "each beat at 240 bpm should draw a new random number");

    talas::runtime::RenderSettings keys{sampleRate, 64};
    keys.eventValues = {{"KeyDown", 0.0, scalar}};
    for (int press = 0; press < 24; ++press)
    {
        keys.eventValueChanges.push_back({at(0.1 + 0.1 * press), "KeyDown", 1.0, scalar});
        keys.eventValueChanges.push_back({at(0.15 + 0.1 * press), "KeyDown", 0.0, scalar});
    }
    const auto positive = valueOf("!KeyDown nextRandom abs", 2.5, keys);
    const auto scaled = valueOf("!KeyDown nextRandom abs * 5", 2.5, keys);
    require(std::ranges::all_of(positive, [](double value) { return value >= 0.0 && value <= 1.0; })
            && std::ranges::any_of(positive, [](double value) { return value > 0.5; }),
        "nextRandom abs should lie between 0 and 1");
    require(std::ranges::all_of(scaled, [](double value) { return value >= 0.0 && value <= 5.0; })
            && std::ranges::any_of(scaled, [](double value) { return value > 2.5; }),
        "nextRandom abs * 5 should lie between 0 and 5");

    talas::runtime::RenderSettings pitch{sampleRate, 64};
    pitch.eventValues = {{"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    const auto jumping = valueOf("(!Pitch + (0.125 s random abs * 5 of: #(10 7 5 2 0)) nn) removeUnits - 60", 2.0, pitch);
    const auto gliding = valueOf("(!Pitch + (0.125 s random abs * 5 of: #(10 7 5 2 0)) nn) smoothed removeUnits - 60", 2.0, pitch);
    auto largestJump = 0.0;
    auto largestGlide = 0.0;
    for (std::size_t frame = 1; frame < jumping.size(); ++frame)
    {
        largestJump = std::max(largestJump, std::abs(jumping[frame] - jumping[frame - 1]));
        largestGlide = std::max(largestGlide, std::abs(gliding[frame] - gliding[frame - 1]));
    }
    // The glide moves in control-rate steps, each a small part of a jump.
    require(largestJump >= 2.0 && largestGlide <= largestJump / 20.0,
        "smoothed should glide from one interval to the next rather than jump");
    for (int step = 1; step < 16; ++step)
    {
        const auto settled = at(0.125 * step + 0.11);
        require(std::abs(gliding[settled] - jumping[settled]) < 1.0e-6,
            "smoothed should reach each new value within its 100 ms");
    }
}

// An Oscillator's default wavetable is the Sine, so a 1 hz
// Oscillator pasted as Oscillator1 L nn + 2 c moves the frequency in a sine
// between a half step below 2 c and a half step above.
void testOscillatorPastedAsAPitchDeviation()
{
    const auto played = render(heard({oscillator("Oscillator1", "1 hz"), oscillator("osc", "Oscillator1 L nn + 2 c")},
                                   {}, "osc"),
        at(1.0));
    // 2 c is note 36; its frequency is low, so measure over 80 ms windows.
    require(std::abs(noteNumber(frequencyBetween(played, 0.21, 0.29)) - 37.0) < 0.03
            && std::abs(noteNumber(frequencyBetween(played, 0.71, 0.79)) - 35.0) < 0.03
            && std::abs(noteNumber(frequencyBetween(played, 0.46, 0.54)) - 36.0) < 0.08,
        "a 1 hz sine should move the pitch a half step either side of 2 c");
}

// An LFO pasted into a field, its own amplitude the
// hot value !Modulation, brings a new Modulation fader to the surface, which
// sets how far the field swings either side of its centre value. (The LFO is
// an Oscillator at 1 hz here; our LFO prototype has Minimum and Maximum
// fields instead of !Modulation.)
void testPastedLfoModulationSetsTheDeviation()
{
    auto lfo = oscillator("LFO", "1 hz");
    lfo.parameters.push_back(expression("amplitude", "!Modulation"));
    const auto document = heardValue({lfo}, {}, "0.5 + (LFO L * 0.5)");
    const auto discovered = talas::project::discoverProjectEventValueControls(document, talas::core::makeBuiltinNodeRegistry());
    require(discovered.ok() && discovered.missingControls.size() == 1
            && discovered.missingControls.front().eventName == "Modulation"
            && discovered.missingControls.front().kind == talas::project::VirtualControlKind::Fader,
        "Modulation should show as a new fader in the surface");
    for (const auto modulation : {0.0, 0.5, 1.0})
    {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"Modulation", modulation, scalar}};
        const auto played = render(document, at(1.0), settings);
        const auto [low, high] = std::ranges::minmax(played);
        require(std::abs(high - (0.5 + 0.5 * modulation)) < 1.0e-3 && std::abs(low - (0.5 - 0.5 * modulation)) < 1.0e-3,
            "Modulation " + std::to_string(modulation) + " should swing the field from "
                + std::to_string(0.5 - 0.5 * modulation) + " to " + std::to_string(0.5 + 0.5 * modulation) + ", not "
                + std::to_string(low) + " to " + std::to_string(high));
    }
}

// !KeyNumber / 127 in a Constant becomes a signal a chain
// of Sounds can process (here a 100 ms delay) before it is pasted back into
// a Frequency field as Chain L * 127 nn.
void testKeyNumberProcessedAsASignal()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyNumber", 60.0, scalar}};
    settings.eventValueChanges = {{at(0.3), "KeyNumber", 67.0, scalar}};
    const auto played = render(heard({node("key", "talas.constant", {expression("value", "!KeyNumber / 127")}),
                                         node("Chain", "talas.delayWithFeedback",
                                             {expression("delay", "100 ms"), expression("feedback", "0"),
                                                 expression("scale", "1")}),
                                         oscillator("osc", "Chain L * 127 nn")},
                                   {connection("key", "value", "Chain", "audioIn")}, "osc"),
        at(0.6), settings);
    require(std::abs(noteNumber(frequencyBetween(played, 0.15, 0.28)) - 60.0) < 0.02
            && std::abs(noteNumber(frequencyBetween(played, 0.32, 0.39)) - 60.0) < 0.02
            && std::abs(noteNumber(frequencyBetween(played, 0.42, 0.58)) - 67.0) < 0.02,
        "the new key should reach the pitch only after the chain's delay");
}

// An AveragingLowPassFilter with a Cutoff of 10 hz between
// the Constant holding !KeyNumber / 127 and the Oscillator low-pass filters
// the note numbers: a new key glides to its pitch over a tenth of a second
// instead of jumping to it.
void testLowPassFilteredNoteNumbersGlide()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyNumber", 60.0, scalar}};
    settings.eventValueChanges = {{at(0.3), "KeyNumber", 67.0, scalar}};
    const auto played = render(heard({node("key", "talas.constant", {expression("value", "!KeyNumber / 127")}),
                                         node("Smoothing", "talas.averagingLowPassFilter", {expression("cutoff", "10 hz")}),
                                         oscillator("osc", "Smoothing L * 127 nn")},
                                   {connection("key", "value", "Smoothing", "audioIn")}, "osc"),
        at(0.6), settings);
    const auto midway = noteNumber(frequencyBetween(played, 0.34, 0.36));
    require(std::abs(noteNumber(frequencyBetween(played, 0.15, 0.28)) - 60.0) < 0.02
            && midway > 62.5 && midway < 64.5
            && std::abs(noteNumber(frequencyBetween(played, 0.45, 0.58)) - 67.0) < 0.02,
        "low-pass filtered note numbers should glide from 60 to 67, not jump (midway "
            + std::to_string(midway) + ")");
}

// One Constant holding an Event Value expression, pasted into
// the fields of two Oscillators, moves both alike.
void testOneConstantSharedByTwoFields()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Bend", 0.0, scalar}};
    settings.eventValueChanges = {{at(0.2), "Bend", 1.0, scalar}};
    const auto shared = node("Shared", "talas.constant", {expression("value", "(!Bend * 0.5) + 0.25")});
    const auto first = render(heard({shared, oscillator("a", "Shared L * 400 hz + 200 hz")}, {}, "a"), at(0.4), settings);
    const auto second = render(heard({shared, oscillator("b", "Shared L * 400 hz + 400 hz")}, {}, "b"), at(0.4), settings);
    require(std::abs(frequencyBetween(first, 0.05, 0.19) - 300.0) < 0.1
            && std::abs(frequencyBetween(first, 0.21, 0.39) - 500.0) < 0.1
            && std::abs(frequencyBetween(second, 0.05, 0.19) - 500.0) < 0.1
            && std::abs(frequencyBetween(second, 0.21, 0.39) - 700.0) < 0.1,
        "a Constant pasted into two fields should move both with its expression");
}

// (!KeyDown ramp: 0.1 s) * 3 nn + !Pitch bends the key's
// pitch from zero to three half steps up over the tenth of a second after
// the key goes down.
void testKeyDownRampBendsThePitch()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyDown", 0.0, scalar}, {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    settings.eventValueChanges = {{at(0.2), "KeyDown", 1.0, scalar}};
    const auto played = render(heard({oscillator("osc", "(!KeyDown ramp: 0.1 s) * 3 nn + !Pitch")}, {}, "osc"),
        at(0.6), settings);
    const auto early = noteNumber(frequencyBetween(played, 0.2, 0.23));
    const auto middle = noteNumber(frequencyBetween(played, 0.24, 0.26));
    const auto bent = noteNumber(frequencyBetween(played, 0.35, 0.6));
    require(early < 61.2 && std::abs(middle - 61.5) < 0.2 && std::abs(bent - 63.0) < 0.01,
        "the key's pitch should bend up three half steps over 0.1 s after the key goes down");
}

struct ScriptNote
{
    double start;
    double duration;
    double frequency;
};

// A MIDIVoice playing a Script, its voice sounding a level of the note's
// frequency / 10000 while the key is down, so that the output at any time
// is the sum of the frequencies sounding.
std::vector<double> scriptFrequencies(std::string script, double seconds)
{
    auto document = heard({node("level", "talas.oscillator",
                               {expression("frequency", "1 hz"), resource("wavetable", "Tables/Level"),
                                   expression("amplitude", "(!KeyDown > 0) * (!KeyNumber nn hz removeUnits) / 10000")}),
                              node("voice", "talas.midiVoice",
                                  {text("source", "Script"), text("script", std::move(script)), expression("channel", "1"),
                                      expression("polyphony", "16")})},
        {connection("level", "audioOut", "voice", "audioIn")}, "voice", "leftOut");
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = {{"Tables/Level", {1.0, 1.0}}};
    settings.eventValues = {{"KeyDown", 0.0, scalar}, {"KeyNumber", 0.0, scalar}};
    return render(document, at(seconds), settings);
}

// The sum of the frequencies of the notes sounding at a time, away from
// their edges and the 5 ms dip of !KeyDown at each note-on.
std::optional<double> expectedFrequencies(const std::vector<ScriptNote>& notes, double time)
{
    auto sum = 0.0;
    for (const auto& note : notes)
    {
        const auto end = note.start + note.duration;
        if (std::abs(time - note.start) < 0.007 || std::abs(time - end) < 0.002)
            return std::nullopt;
        if (time > note.start && time < end)
            sum += note.frequency / 10000.0;
    }
    return sum;
}

void requireScriptPlays(const std::vector<double>& played, const std::vector<ScriptNote>& notes, double seconds,
    const std::string& message)
{
    auto checked = 0;
    for (auto time = 0.0035; time < seconds - 0.01; time += 0.0103)
    {
        const auto expected = expectedFrequencies(notes, time);
        if (!expected)
            continue;
        ++checked;
        require(std::abs(played[at(time)] - *expected) < 1.0e-4,
            message + " at " + std::to_string(time) + " s: heard " + std::to_string(played[at(time)] * 10000.0)
                + " hz of notes, expected " + std::to_string(*expected * 10000.0));
    }
    require(checked > 100, message + ": too few times checked");
}

// A Script transcribing notes by hand, its first line
// setting the metronome that defines its beats (MM := 90: a beat is 2/3 s).
void testScriptTranscribesNotesByHand()
{
    const auto played = scriptFrequencies(
        "MM := 90.\n"
        "\"------------------------------------\"\n"
        "self keyDownAt: 0 beats duration: 3 beats frequency: 4 a.\n"
        "self keyDownAt: 3 beats duration: 1 beat frequency: 4 a.\n"
        "self keyDownAt: 4 beats duration: 1 beat frequency: 4 b.\n"
        "self keyDownAt: 5 beats duration: 1 beat frequency: 4 g.\n"
        "self keyDownAt: 6 beats duration: 2 beats frequency: 4 a.\n"
        "self keyDownAt: 8 beats duration: 0.5 beats frequency: 4 f.\n"
        "self keyDownAt: 8.5 beats duration: 0.5 beats frequency: 4 e.",
        6.2);
    const auto beat = 60.0 / 90.0;
    const auto a = 440.0;
    const auto semitone = [](double steps) { return 440.0 * std::pow(2.0, steps / 12.0); };
    requireScriptPlays(played,
        {{0.0, 3 * beat, a}, {3 * beat, beat, a}, {4 * beat, beat, semitone(2.0)}, {5 * beat, beat, semitone(-2.0)},
            {6 * beat, 2 * beat, a}, {8 * beat, 0.5 * beat, semitone(-4.0)}, {8.5 * beat, 0.5 * beat, semitone(-5.0)}},
        6.2, "the transcribed notes should play at MM 90");
}

// The self-similar MIDI script: a measure of three notes, a
// group of the measure, the measure at half its durations a fifth up and
// the measure a fourth down, a phrase of groups made alike, and a section
// of the phrase, the phrase inverted about 3 d and the phrase a fourth
// down backwards, played on the voice at MM 160.
void testSelfSimilarEventSequencePlays()
{
    using Melody = std::vector<std::pair<double, double>>; // beats, note number
    const auto dim = [](Melody melody, double factor) {
        for (auto& note : melody)
            note.first *= factor;
        return melody;
    };
    const auto trsp = [](Melody melody, double steps) {
        for (auto& note : melody)
            note.second += steps;
        return melody;
    };
    const auto join = [](std::initializer_list<Melody> parts) {
        Melody joined;
        for (const auto& part : parts)
            joined.insert(joined.end(), part.begin(), part.end());
        return joined;
    };
    const Melody measure{{2.0, 43.0}, {1.0, 50.0}, {2.0, 38.0}};
    const auto group = join({measure, trsp(dim(measure, 0.5), 7.0), trsp(measure, -5.0)});
    const auto phrase = join({group, trsp(dim(group, 0.5), 7.0), trsp(group, -5.0)});
    auto inverted = phrase;
    for (auto& note : inverted)
        note.second = 2.0 * 50.0 - note.second;
    auto backwards = trsp(phrase, -5.0);
    std::reverse(backwards.begin(), backwards.end());
    const auto section = join({phrase, inverted, backwards});
    std::vector<ScriptNote> notes;
    auto beat = 0.0;
    for (const auto& [beats, pitch] : section)
    {
        notes.push_back({beat * 60.0 / 160.0, beats * 60.0 / 160.0, 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0)});
        beat += beats;
    }
    const auto seconds = beat * 60.0 / 160.0 + 0.2;
    const auto played = scriptFrequencies(
        "| measure group phrase section |\n"
        "measure := EventSequence events: #( {Note durationInBeats: 2 frequency: 2 g} "
        "{Note durationInBeats: 1 frequency: 3 d} {Note durationInBeats: 2 frequency: 2 d}).\n"
        "group := EventSequence events: (Array with: measure with: ((measure dim: 0.5) trsp: 7) with: (measure trsp: -5)).\n"
        "phrase := EventSequence events: (Array with: group with: ((group dim: 0.5) trsp: 7) with: (group trsp: -5)).\n"
        "section := EventSequence events: (Array with: phrase with: (phrase inv: 3 d) with: (phrase trsp: -5) retrograde).\n"
        "section playOnVoice: self bpm: 160.",
        seconds);
    require(notes.size() == 81, "the section should hold 81 notes");
    requireScriptPlays(played, notes, seconds, "the self-similar section should play its 81 notes");
}

// Kurt's new rhythm method, a loop over 201 beats at MM
// 300 whose modulo tests schedule seven kinds of event: every fourth beat
// 100 hz, every second 250 hz, every third 150 hz, every fifth before beat
// 50 400 hz, every thirteenth 700 hz, every seventh 50 hz, and after beat 50
// 1000 hz on beats no multiple of 7, 5 or 3, each lasting its number of
// cycles.
void testScriptDrumMachineByModuloArithmetic()
{
    const auto played = scriptFrequencies(
        "MM := 300.\n"
        "0 to: 200 do: [ :i |\n"
        "  i \\\\ 4 = 0 ifTrue: [ self keyDownAt: i beats duration: 25 / 100 hz frequency: 100 hz].\n"
        "  i \\\\ 2 = 0 ifTrue: [ self keyDownAt: i beats duration: 25 / 250 hz frequency: 250 hz].\n"
        "  i \\\\ 3 = 0 ifTrue: [ self keyDownAt: i beats duration: 25 / 150 hz frequency: 150 hz].\n"
        "  (i < 50 and: [i \\\\ 5 = 0]) ifTrue: [ self keyDownAt: i beats duration: 25 / 400 hz frequency: 400 hz].\n"
        "  i \\\\ 13 = 0 ifTrue: [ self keyDownAt: i beats duration: 25 / 700 hz frequency: 700 hz].\n"
        "  i \\\\ 7 = 0 ifTrue: [ self keyDownAt: i beats duration: 50 / 50 hz frequency: 50 hz].\n"
        "  (i > 50 and: [i \\\\ 7 ~= 0 and: [i \\\\ 5 ~= 0 and: [i \\\\ 3 ~= 0]]]) ifTrue: [\n"
        "    self keyDownAt: i beats duration: 100 / 1000 hz frequency: 1000 hz]. ].",
        13.0);
    std::vector<ScriptNote> notes;
    for (int i = 0; i <= 200; ++i)
    {
        const auto start = 0.2 * i;
        const auto add = [&](double cycles, double frequency) { notes.push_back({start, cycles / frequency, frequency}); };
        if (i % 4 == 0)
            add(25, 100);
        if (i % 2 == 0)
            add(25, 250);
        if (i % 3 == 0)
            add(25, 150);
        if (i < 50 && i % 5 == 0)
            add(25, 400);
        if (i % 13 == 0)
            add(25, 700);
        if (i % 7 == 0)
            add(50, 50);
        if (i > 50 && i % 7 != 0 && i % 5 != 0 && i % 3 != 0)
            add(100, 1000);
    }
    requireScriptPlays(played, notes, 13.0, "the drum machine's events should play on their beats");
}

// Amplitude modulating a complex carrier gives each of its
// sine components sum and difference frequencies with the modulator, more
// frequencies out than in.
void testAmplitudeModulationGivesSumsAndDifferences()
{
    const auto played = render(heard({node("a", "talas.oscillator", {expression("frequency", "500 hz"), expression("amplitude", "0.5")}),
                                         node("b", "talas.oscillator", {expression("frequency", "800 hz"), expression("amplitude", "0.5")}),
                                         node("carrier", "talas.mixer"), oscillator("modulator", "100 hz"),
                                         node("ring", "talas.product")},
                                   {connection("a", "audioOut", "carrier", "in1"), connection("b", "audioOut", "carrier", "in2"),
                                       connection("carrier", "audioOut", "ring", "left"),
                                       connection("modulator", "audioOut", "ring", "right")},
                                   "ring", "value"),
        at(0.5));
    const auto window = std::span{played}.subspan(at(0.1), at(0.4));
    for (const auto frequency : {400.0, 600.0, 700.0, 900.0})
    {
        require(std::abs(toneLevel(window, frequency) - 0.25) < 0.005,
            "the product should hold a component at " + std::to_string(frequency) + " hz");
    }
    for (const auto frequency : {100.0, 500.0, 800.0})
    {
        require(toneLevel(window, frequency) < 0.005,
            "the carrier's and modulator's own frequencies should be gone, " + std::to_string(frequency));
    }
}

// A DelayWithFeedback's Delay is the longest delay and its
// DelayScale the share of it delaying the signal now, so two delays of one
// source differ in time only, not in amplitude.
void testDelayScaleTakesAShareOfTheDelay()
{
    std::vector<double> source;
    const auto delayed = [&](std::string scale) {
        return render(heard({oscillator("source", "300 hz"),
                              node("late", "talas.delayWithFeedback",
                                  {expression("delay", "2000 usec"), expression("delayScale", std::move(scale)),
                                      expression("feedback", "0"), expression("scale", "1")})},
                          {connection("source", "audioOut", "late", "audioIn")}, "late"),
            at(0.1));
    };
    const auto direct = render(heard({oscillator("source", "300 hz")}, {}, "source"), at(0.1));
    const auto left = delayed("0.25");
    const auto right = delayed("0.75");
    for (std::size_t frame = at(0.01); frame < at(0.1); ++frame)
    {
        require(std::abs(left[frame] - direct[frame - 24]) < 1.0e-9 && std::abs(right[frame] - direct[frame - 72]) < 1.0e-9,
            "DelayScale should delay by its share of the 2 ms Delay");
    }
    require(std::abs(toneLevel(std::span{left}.subspan(at(0.01), at(0.09)), 300.0)
                - toneLevel(std::span{right}.subspan(at(0.01), at(0.09)), 300.0))
            < 1.0e-6,
        "the two delays should not differ in amplitude");
}

// Time is the start of the Sound being transformed and
// totalDuration the duration of the ParameterTransformer's input, both in
// microseconds, so that a Transformation lowering each note by 3 * (time /
// totalDuration) half steps leaves the first of three one-second notes
// alone and lowers the next by one and two half steps. The Mixer lasts
// its longest Input.
void testTransformationFollowsEachNotesStart()
{
    // Three one-second notes, the second and third offset by one and two
    // seconds, mixed.
    const auto transformed = [](std::string transformation, double seconds) {
        std::vector<talas::project::ProjectNode> nodes;
        std::vector<talas::core::Connection> connections;
        for (int note = 0; note < 3; ++note)
        {
            const auto id = std::to_string(note);
            nodes.push_back(node("tone" + id, "talas.oscillator", {expression("frequency", "440 hz"), expression("amplitude", "0.25")}));
            nodes.push_back(node("note" + id, "talas.setDuration", {expression("duration", "1 s")}));
            nodes.push_back(node("start" + id, "talas.timeOffset", {expression("offset", id + " s")}));
            connections.push_back(connection("tone" + id, "audioOut", "note" + id, "audioIn"));
            connections.push_back(connection("note" + id, "audioOut", "start" + id, "audioIn"));
        }
        nodes.push_back(node("firstTwo", "talas.mixer"));
        nodes.push_back(node("all", "talas.mixer"));
        nodes.push_back(node("transform", "talas.parameterTransformer", {text("transformation", std::move(transformation))}));
        for (const auto& wire : {connection("start0", "audioOut", "firstTwo", "in1"), connection("start1", "audioOut", "firstTwo", "in2"),
                 connection("firstTwo", "audioOut", "all", "in1"), connection("start2", "audioOut", "all", "in2"),
                 connection("all", "audioOut", "transform", "audioIn")})
            connections.push_back(wire);
        return render(heard(std::move(nodes), std::move(connections), "transform"), at(seconds));
    };
    const auto lowered = transformed(
        "snd frequency isNil ifFalse: [snd frequency: (snd frequency nn - (3 * (time / totalDuration)) nn)].", 3.0);
    for (int note = 0; note < 3; ++note)
    {
        const auto expected = 440.0 * std::exp2(-note / 12.0);
        require(std::abs(frequencyBetween(lowered, note + 0.1, note + 0.9) - expected) < 0.5,
            "note " + std::to_string(note) + " should be lowered by " + std::to_string(note) + " half steps");
    }
}

// A Transformation names parameters, and sets every
// parameter of that name throughout its Input: snd left: 0.25 sets each
// Left, whatever its field held (Left is the parameter's name, not an
// Event Value's). Arithmetic on a parameter the Input lacks, snd
// frequency: snd cmRatio * 0.5, is an error, which snd cmRatio isNil
// ifFalse: [...] guards against.
void testTransformingParametersByName()
{
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Left", 1.0, scalar}};
    const auto document = [](std::string transformation) {
        return heard({oscillator("tone", "441 hz"), node("inner", "talas.level", {expression("left", "!Left"), expression("right", "1")}),
                         node("mix", "talas.mixerV2", {expression("left", "1"), expression("right", "1")}),
                         node("transform", "talas.parameterTransformer", {text("transformation", std::move(transformation))})},
            {connection("tone", "audioOut", "inner", "audioIn"), connection("inner", "leftOut", "mix", "inputs"),
                connection("mix", "leftOut", "transform", "audioIn")},
            "transform");
    };
    const auto tone = render(heard({oscillator("tone", "441 hz")}, {}, "tone"), at(0.1));
    const auto set = render(document("snd left: 0.25."), at(0.1), settings);
    for (std::size_t frame = 0; frame < set.size(); ++frame)
    {
        require(std::abs(set[frame] - 0.0625 * tone[frame]) < 1.0e-9,
            "snd left: 0.25 should set every Left, the Level's and the Mixer's, at frame " + std::to_string(frame));
    }
    const auto compiled = [&](std::string transformation) {
        return talas::runtime::compileProject(document(std::move(transformation)), talas::core::makeBuiltinNodeRegistry(), settings).ok();
    };
    require(!compiled("snd frequency: snd cmRatio * 0.5."),
        "arithmetic on a parameter the Input lacks should be an error");
    const auto guarded = render(document("snd cmRatio isNil ifFalse: [snd frequency: snd cmRatio * 0.5]."), at(0.1), settings);
    require(guarded == tone, "isNil ifFalse: should guard against calculating with nil");
}

// Independent partials, Oscillators at ?frequency,
// ?frequency * 2 and ?frequency * 3, played three times one after another
// and transformed by adding 110 hz * time / totalDuration to every
// Frequency. Each repetition is higher, and since a fixed amount is added
// to each partial, the harmonic tone of 100, 200 and 300 hz becomes
// inharmonic: the Sound expands before it is transformed.
void testTransformingIndependentPartialsMakesThemInharmonic()
{
    std::vector<talas::project::ProjectNode> nodes;
    std::vector<talas::core::Connection> connections;
    for (int partial = 1; partial <= 3; ++partial)
    {
        const auto id = "partial" + std::to_string(partial);
        nodes.push_back(node(id, "talas.oscillator",
            {expression("frequency", partial == 1 ? "?frequency" : "?frequency * " + std::to_string(partial)),
                expression("amplitude", "0.2")}));
        connections.push_back(connection(id, "audioOut", "independentPartials", "inputs"));
    }
    nodes.push_back(node("independentPartials", "talas.mixerV2"));
    nodes.push_back(node("once", "talas.setDuration", {expression("duration", "0.5 s")}));
    nodes.push_back(node("repetitions", "talas.concatenation"));
    nodes.push_back(node("transformIndependentPartials", "talas.parameterTransformer",
        {text("transformation",
            "snd frequency isNil ifFalse: [\n"
            "snd frequency:\n"
            "snd frequency nn + (110.0 hz * time / totalDuration)].")}));
    connections.push_back(connection("independentPartials", "leftOut", "once", "audioIn"));
    for (int repetition = 0; repetition < 3; ++repetition)
        connections.push_back(connection("once", "audioOut", "repetitions", "inputs"));
    connections.push_back(connection("repetitions", "leftOut", "transformIndependentPartials", "audioIn"));
    auto document = heard(std::move(nodes), std::move(connections), "transformIndependentPartials");
    document.variableLibrary.variables = {{"frequency", "frequency", talas::expr::ValueDomain::Hertz, 100.0}};
    const auto played = render(document, at(1.5));
    for (int repetition = 0; repetition < 3; ++repetition)
    {
        const auto added = 110.0 * repetition / 3.0;
        const auto heard = std::span{played}.subspan(at(0.5 * repetition + 0.05), at(0.4));
        for (int partial = 1; partial <= 3; ++partial)
        {
            const auto frequency = 100.0 * partial + added;
            require(toneLevel(heard, frequency) > 0.15,
                "repetition " + std::to_string(repetition) + " should play "
                    + std::to_string(frequency) + " hz, its partial raised by a fixed amount");
            if (repetition > 0 && partial > 1)
            {
                require(toneLevel(heard, partial * (100.0 + added)) < 0.05,
                    "repetition " + std::to_string(repetition) + " should not stay harmonic");
            }
        }
    }
}

// HarmonicPartials, a class made from independent
// partials with its one parameter Frequency, and Expand unchecked: the
// Transformation adding 110 hz * time / totalDuration applies to each
// instance's Frequency before it expands, and the class computes its
// partials from it, so each repetition is higher and stays harmonic. With
// Expand checked the instance expands first, and its partials are raised
// alike, inharmonic.
void testUnexpandedClassStaysHarmonic()
{
    const auto played = [](bool expands) {
        talas::project::ProjectSoundDefinition harmonic;
        harmonic.definitionId = "harmonicPartials";
        harmonic.displayName = "HarmonicPartials";
        harmonic.expandsBeforeTransformation = expands;
        harmonic.outputs.push_back({"audioOut", talas::core::PortType::Audio, "once", "audioOut"});
        for (int partial = 1; partial <= 3; ++partial)
        {
            const auto id = "partial" + std::to_string(partial);
            harmonic.graph.nodes.push_back(node(id, "talas.oscillator",
                {expression("frequency", partial == 1 ? "!Freq" : "!Freq * " + std::to_string(partial)),
                    expression("amplitude", "0.2")}));
            harmonic.graph.connections.push_back(connection(id, "audioOut", "partials", "inputs"));
        }
        harmonic.graph.nodes.push_back(node("partials", "talas.mixerV2"));
        harmonic.graph.nodes.push_back(node("once", "talas.setDuration", {expression("duration", "0.5 s")}));
        harmonic.graph.connections.push_back(connection("partials", "leftOut", "once", "audioIn"));
        talas::project::ProjectSoundExposedParameter frequency;
        frequency.parameterId = "frequency";
        frequency.displayName = "Frequency";
        frequency.kind = talas::core::ParameterKind::Expression;
        frequency.domain = talas::expr::ValueDomain::Hertz;
        frequency.defaultValue = expression("frequency", "100 hz");
        frequency.bindings.push_back(
            {talas::project::ProjectSoundParameterTarget::EventValue, {}, {}, "Freq", talas::expr::ValueDomain::Hertz});
        harmonic.exposedParameters.push_back(std::move(frequency));

        std::vector<talas::project::ProjectNode> nodes;
        std::vector<talas::core::Connection> connections;
        for (int repetition = 1; repetition <= 3; ++repetition)
        {
            auto instance = node("harmonic" + std::to_string(repetition), "", {expression("frequency", "100 hz")});
            instance.soundDefinitionId = "harmonicPartials";
            nodes.push_back(std::move(instance));
            connections.push_back(connection("harmonic" + std::to_string(repetition), "audioOut", "repetitions", "inputs"));
        }
        nodes.push_back(node("repetitions", "talas.concatenation"));
        nodes.push_back(node("transformIndependentPartials", "talas.parameterTransformer",
            {text("transformation", "snd frequency isNil ifFalse: [snd frequency: snd frequency nn + (110.0 hz * time / totalDuration)].")}));
        connections.push_back(connection("repetitions", "leftOut", "transformIndependentPartials", "audioIn"));
        auto document = heard(std::move(nodes), std::move(connections), "transformIndependentPartials");
        document.soundDefinitions.push_back(std::move(harmonic));
        // The box unchecked is kept in the project (format 105).
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok() && reopened.document.soundDefinitions.front().expandsBeforeTransformation == expands,
            "a class's Expand box should be kept in the project");
        return render(document, at(1.5));
    };
    const auto expanded = played(true);
    const auto harmonicTone = played(false);
    for (int repetition = 1; repetition < 3; ++repetition)
    {
        const auto fundamental = 100.0 + 110.0 * repetition / 3.0;
        const auto window = [&](const std::vector<double>& samples) {
            return std::span{samples}.subspan(at(0.5 * repetition + 0.05), at(0.4));
        };
        for (int partial = 1; partial <= 3; ++partial)
        {
            require(toneLevel(window(harmonicTone), partial * fundamental) > 0.15,
                "with Expand unchecked repetition " + std::to_string(repetition + 1)
                    + " should keep its partials harmonic, " + std::to_string(partial * fundamental) + " hz");
        }
        require(toneLevel(window(expanded), 2.0 * fundamental) < 0.05 && toneLevel(window(expanded), 200.0 + fundamental - 100.0) > 0.15,
            "with Expand checked the partials should each be raised alike");
    }
}

// The Design Alternate Tunings Tool's Create Example
// plays each key at its frequency in the tuning: a 19-tone equal
// temperament, a just ratio scale below and above its tonic, a
// two-interval scale and a file's arbitrary frequencies.
void testTuningExamplePlaysEachKeysFrequency()
{
    namespace tunings = talas::project::tunings;
    const auto played = [](const tunings::Tuning& tuning, int key) {
        std::string error;
        const auto frequencies = tunings::keyFrequencies(tuning, error);
        require(frequencies.has_value(), "the tuning should give its frequencies: " + error);
        talas::project::ProjectDocument owner;
        owner.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
        const auto example = talas::app::tuningExampleSound(
            owner, tuning, *frequencies, talas::app::TuningExampleInstrument::Sine, {}, error);
        require(example.has_value(), "the tuning should make an example: " + error);
        // The example's graph, its voice playing one key from a script
        // rather than the MIDI keyboard.
        auto nodes = example->definition.graph.nodes;
        for (auto& node : nodes)
        {
            if (node.typeId == "talas.midiVoice")
            {
                node.parameters = {text("source", "Script"), expression("polyphony", "8"),
                    text("script", "self keyDownAt: 0 s duration: 1 s frequency: " + std::to_string(key) + " nn.")};
            }
        }
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"KeyDown", 0.0, scalar}, {"KeyNumber", 60.0, scalar}, {"Volume", 1.0, scalar}};
        require(example->controls.size() == 1 && example->controls.front().eventName == "Volume"
                && example->controls.front().value == 1.0,
            "the example's Volume fader starts at its top");
        const auto rendered =
            render(heard(std::move(nodes), example->definition.graph.connections, "voice", "leftOut"), at(0.5), settings);
        return std::pair{frequencyBetween(rendered, 0.1, 0.45), (*frequencies)[static_cast<std::size_t>(key)]};
    };
    tunings::Tuning tuning;
    tuning.degreesPerOctave = 19;
    for (const auto key : {67, 41})
    {
        const auto [heardFrequency, expected] = played(tuning, key);
        require(std::abs(heardFrequency - expected) < 0.05,
            "the example should play key " + std::to_string(key) + " of 19-tone equal temperament");
    }
    tuning.method = tunings::Method::RatioScale;
    for (const auto key : {55, 76})
    {
        const auto [heardFrequency, expected] = played(tuning, key);
        require(std::abs(heardFrequency - expected) < 0.05,
            "the example should play key " + std::to_string(key) + " of the ratio scale");
    }
    tuning.method = tunings::Method::TwoInterval;
    tuning.degreesPerOctave = 7;
    const auto [twoHeard, twoExpected] = played(tuning, 64);
    require(std::abs(twoHeard - twoExpected) < 0.05, "the example should play the two-interval scale");
    // A file's tuning that repeats each octave, here
    // after five keys.
    tuning.method = tunings::Method::FromTextFile;
    const std::array<double, 5> pattern{1.0, 1.1, 1.3, 1.5, 1.8};
    for (std::size_t key = 0; key < tunings::keyCount; ++key)
        tuning.fileFrequencies[key] = 8.0 * pattern[key % 5] * std::exp2(static_cast<double>(key / 5));
    for (const auto key : {37, 43})
    {
        const auto [heardFrequency, expected] = played(tuning, key);
        require(std::abs(heardFrequency - expected) < 0.05,
            "the example should play key " + std::to_string(key) + " at the file's frequency");
    }
    tuning.fileFrequencies[100] *= 1.01;
    std::string error;
    talas::project::ProjectDocument owner;
    owner.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    require(!talas::app::tuningExampleSound(owner, tuning, tuning.fileFrequencies, talas::app::TuningExampleInstrument::Sine, {},
                error)
                 .has_value()
            && !talas::app::tuningExampleSound({}, tunings::Tuning{}, *tunings::keyFrequencies(tunings::Tuning{}, error),
                talas::app::TuningExampleInstrument::Sine, {}, error)
                    .has_value(),
        "a file's tuning that does not repeat by octaves, or a project not in Val, should make no example");
    // The 21-harmonic sawtooth instrument.
    const auto sawtooth = talas::app::tuningExampleSound(owner, tunings::Tuning{},
        *tunings::keyFrequencies(tunings::Tuning{}, error), talas::app::TuningExampleInstrument::Sawtooth, {}, error);
    require(sawtooth.has_value()
            && std::ranges::any_of(sawtooth->definition.graph.nodes, [](const talas::project::ProjectNode& node) {
                   return node.typeId == "talas.oscillator"
                       && std::ranges::any_of(node.parameters, [](const auto& parameter) {
                              return parameter.parameterId == "wavetable"
                                  && parameter.resource == std::optional<std::string>{"Saw21"};
                          });
               }),
        "the sawtooth example's oscillator should read the 21-harmonic sawtooth");
}

// Throughout the Design Alternate Tunings
// Tool the MIDI keyboard tries out the tuning in real time: the Tool plays
// its example on its own from the live keyboard, and a key played sounds at
// its frequency in the tuning.
void testTheKeyboardTriesOutTheTuning()
{
    namespace tunings = talas::project::tunings;
    tunings::Tuning tuning;
    tuning.degreesPerOctave = 19;
    std::string error;
    const auto frequencies = tunings::keyFrequencies(tuning, error);
    talas::project::ProjectDocument owner;
    owner.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    const auto tryOut =
        talas::app::tuningTryOut(owner, tuning, *frequencies, talas::app::TuningExampleInstrument::Sine, {}, error);
    require(tryOut.has_value() && tryOut->document.virtualControls.size() == 1
            && tryOut->document.virtualControls.front().eventName == "Volume",
        "the Tool should play its example with its Volume fader: " + error);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(tryOut->document, registry);
    bridge.setPlaying(true);
    talas::runtime::RenderSettings settings{sampleRate, 64};
    const auto staged = bridge.compileAndStageDocument(tryOut->document, settings);
    require(staged.ok(), "the try-out should compile: " + staged.message);
    std::vector<double> played;
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    const auto blocks = [&](std::size_t count) {
        for (std::size_t block = 0; block < count; ++block)
        {
            left.fill(0.0F);
            right.fill(0.0F);
            std::array<float*, 2> channels{left.data(), right.data()};
            require(bridge.render(channels.data(), channels.size(), left.size()), "the try-out should render");
            played.insert(played.end(), left.begin(), left.end());
        }
    };
    blocks(8);
    require(bridge.enqueueMidiNoteOn(1, 67, 100, 0.0), "a key played should reach the Tool's Sound");
    blocks(static_cast<std::size_t>(sampleRate * 0.5) / 64);
    require(std::abs(frequencyBetween(played, 0.2, 0.45) - (*frequencies)[67]) < 0.1,
        "key 67 should sound at its frequency in 19-tone equal temperament, "
            + std::to_string((*frequencies)[67]) + " hz");
}

// MIDI Timing clock ctrl seq & del: an AnalogSequencer
// stepped by !TimingClock, 24 triggers a beat, with Durations of 1 masking
// all but one a second, its Rate (!TimingClockDuration * 24) inverse the
// beats in a second, moves to its next stage each beat: every half second
// at 120 bpm, every second at 60.
void testTimingClockStepsASequencerEachBeat()
{
    const auto stages = [](double tempo) {
        auto document = heard({node("pitch", "talas.valToSound", {expression("value", "!KeyPitch removeUnits / 100")}),
                                  node("seq", "talas.analogSequencer",
                                      {expression("left", "1"), expression("right", "1"), expression("gate", "1"),
                                          expression("step", "!TimingClock"),
                                          expression("rate", "(!TimingClockDuration * 24) inverse"),
                                          text("durations", "#(1 1 1 1)"), text("dutyCycles", "#(1)"),
                                          text("keyDowns", "#(1 1 1 1)"), text("keyPitches", "#(60 62 64 65)"),
                                          text("keyVelocities", "#(1)"), text("keyTimbres", "#(0)")})},
            {connection("pitch", "audioOut", "seq", "audioIn")}, "seq", "leftOut");
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.tempoBpm = tempo;
        settings.eventValues = {{"KeyPitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
        return render(document, at(2.2), settings);
    };
    for (const auto [tempo, beat] : {std::pair{120.0, 0.5}, std::pair{60.0, 1.0}})
    {
        const auto played = stages(tempo);
        // Each beat, from a little after it starts, plays the next stage's
        // pitch.
        for (int stage = 0; stage < static_cast<int>(2.0 / beat); ++stage)
        {
            const auto expected = std::array{0.60, 0.62, 0.64, 0.65}[static_cast<std::size_t>(stage) % 4];
            const auto value = played[at(stage * beat + 0.75 * beat)];
            require(std::abs(value - expected) < 1.0e-6,
                "at " + std::to_string(tempo) + " bpm, beat " + std::to_string(stage)
                    + " should play stage " + std::to_string(stage % 4) + ", not " + std::to_string(value));
        }
    }
}

// Random notes overlap on one key: a note inside another
// of the same key ends when it ends, and the outer one plays on.
void testOverlappingNotesOnOneKeyEndOnTheirOwn()
{
    auto document = heard({node("envelope", "talas.adsr", {expression("gate", "!KeyDown"), expression("attackTime", "1 ms"),
                               expression("releaseTime", "1 ms")}),
                              node("tone", "talas.oscillator", {expression("frequency", "!KeyPitch"), expression("amplitude", "0.25")}),
                              node("voice", "talas.midiVoice", {text("source", "Script"), expression("polyphony", "4"),
                                  text("script", "self keyDownAt: 0 s duration: 0.6 s frequency: 440 hz. "
                                                 "self keyDownAt: 0.2 s duration: 0.2 s frequency: 446 hz.")})},
        {connection("envelope", "value", "tone", "envelope"), connection("tone", "audioOut", "voice", "audioIn")}, "voice",
        "leftOut");
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyDown", 0.0, scalar}, {"KeyPitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    const auto played = render(document, at(0.7), settings);
    require(std::abs(frequencyBetween(played, 0.45, 0.58) - 440.0) < 0.5,
        "after the inner note ends, the outer note of the same key should play on");
    require(toneLevel(std::span{played}.subspan(at(0.25), at(0.1)), 446.0) > 0.1,
        "the inner note should sound while it lasts");
}

// A MIDIVoice's script, a score, sets the Interval of a
// SimplePitchShifter on the live input, each note shifting it from the
// score's first pitch.
void testAScriptShiftsTheLiveInput()
{
    std::vector<double> voice(at(0.7));
    for (std::size_t frame = 0; frame < voice.size(); ++frame)
        voice[frame] = static_cast<double>(static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(frame) / sampleRate)));
    auto document = heard({node("live", "talas.audioInput"),
                              node("shift", "talas.simplePitchShifter",
                                  {expression("interval", "!KeyPitch removeUnits - 60"), expression("minInputPitch", "150 hz"),
                                      expression("maxInputPitch", "300 hz")}),
                              node("score", "talas.midiVoice",
                                  {text("source", "Script"), expression("polyphony", "1"),
                                      text("script", "self keyDownAt: 0 s duration: 0.2 s frequency: 4 c. "
                                                     "self keyDownAt: 0.2 s duration: 0.2 s frequency: 4 e. "
                                                     "self keyDownAt: 0.4 s duration: 0.2 s frequency: 4 g.")})},
        {connection("live", "audioOut", "shift", "audioIn"), connection("shift", "audioOut", "score", "audioIn")}, "score", "leftOut");
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"KeyDown", 0.0, scalar}, {"KeyPitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    const auto played = render(document, at(0.7), settings, {voice});
    for (const auto& [middle, steps] : {std::pair{0.1, 0.0}, std::pair{0.3, 4.0}, std::pair{0.5, 7.0}})
    {
        const auto note = std::span<const double>{played}.subspan(at(middle - 0.05), at(0.1));
        const auto shifted = toneLevel(note, 220.0 * std::exp2(steps / 12.0));
        require(shifted > 0.05 && (steps == 0.0 || shifted > 4.0 * toneLevel(note, 220.0)),
            "each note of the script should shift the live input by " + std::to_string(steps) + " half steps");
    }
}

// A variable typed into a field as ?freq plays the
// value the project gives it, as @freq does.
void testQuestionMarkVariablesInFields()
{
    for (const std::string field : {"?freq", "@freq"})
    {
        auto document = heard({node("tone", "talas.oscillator", {expression("frequency", field), expression("amplitude", "0.5")})},
            {}, "tone");
        document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 220.0}};
        const auto played = render(document, at(0.2));
        require(std::abs(frequencyBetween(played, 0.05, 0.2) - 220.0) < 0.5,
            "" + field + " should play the variable's 220 hz");
    }
}

// Freq first, then amp: an Interpolate between two
// spectral analyses whose left channel, the amplitudes, follows
// 2 * !Morph - 1 and whose right channel, the frequencies, 2 * !Morph:
// both are limited to 0 to 1, so the frequencies reach the second
// analysis's when !Morph is 0.5 while the amplitudes stay the first's, and
// only then do the amplitudes cross over.
void testFrequencyMorphsBeforeAmplitude()
{
    const auto morphed = [](double morph) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.tableResources = {{"Analysis/A", {0.5, 1.0, 0.0, 0.25, 2.0, 0.0}}, {"Analysis/B", {0.25, 1.5, 0.0, 0.5, 3.0, 0.0}}};
        settings.eventValues = {{"Morph", morph, scalar}};
        const auto source = [](std::string id, std::string analysis) {
            return node(std::move(id), "talas.spectrumInRam",
                {expression("frequency", "220 hz"), resource("analysis", std::move(analysis)), expression("analysisFrames", "1"),
                    expression("analysisFrequency", "220 hz"), expression("nbrPartials", "2"), expression("timeIndex", "-1")});
        };
        const auto played = render(heard({source("a", "Analysis/A"), source("b", "Analysis/B"),
                                             node("morph", "talas.interpolate",
                                                 {expression("amount", "2 * !Morph - 1"), expression("rightAmount", "2 * !Morph")}),
                                             node("rebuilt", "talas.syntheticSpectrumFromSounds", {expression("nbrPartials", "2")}),
                                             node("bank", "talas.oscillatorBank", {expression("nbrOscillators", "2"), expression("bankSize", "2")})},
                                       {connection("a", "amplitudeOut", "morph", "left"), connection("b", "amplitudeOut", "morph", "right"),
                                           connection("a", "frequencyOut", "morph", "input1Right"),
                                           connection("b", "frequencyOut", "morph", "input2Right"),
                                           connection("morph", "value", "rebuilt", "amplitudes"),
                                           connection("morph", "rightValue", "rebuilt", "frequenciesAndBandwidths"),
                                           connection("rebuilt", "spectrumOut", "bank", "spectrum")},
                                       "bank"),
            9600, settings);
        return std::vector<double>(played.begin() + 4800, played.end());
    };
    const auto near = [](const std::vector<double>& played, double frequency, double level) {
        return std::abs(toneLevel(played, frequency) - level) < 0.01;
    };
    const auto quarter = morphed(0.25);
    require(near(quarter, 275.0, 0.5) && near(quarter, 550.0, 0.25),
        "at !Morph 0.25 the frequencies should be half way and the amplitudes the first analysis's");
    const auto half = morphed(0.5);
    require(near(half, 330.0, 0.5) && near(half, 660.0, 0.25),
        "2 * !Morph should reach 1 at 0.5, all the second's frequencies with the first's amplitudes");
    const auto threeQuarters = morphed(0.75);
    require(near(threeQuarters, 330.0, 0.375) && near(threeQuarters, 660.0, 0.375),
        "past 0.5 the frequencies should stay at 1 while the amplitudes climb");
    const auto whole = morphed(1.0);
    require(near(whole, 330.0, 0.25) && near(whole, 660.0, 0.5),
        "at !Morph 1 both should be the second analysis's");
}

// A delay whose length follows an inverted triangle, getting
// shorter to zero and then longer again, models the Doppler shift of a
// passing source: higher while the delay shrinks, lower while it grows. The
// delay line interpolates and smooths its changes, as a moving delay needs.
void testShrinkingAndGrowingDelayShiftsThePitch()
{
    // Over 2 s the delay falls from 50 ms to zero and grows back.
    const auto played = render(heard({oscillator("source", "1000 hz"),
                                         node("path", "talas.delayWithFeedback",
                                             {expression("delay", "50 ms"), expression("feedback", "0"),
                                                 expression("scale", "1"), choice("interpolation", "linear"),
                                                 boolean("smoothDelayChanges", true),
                                                 expression("delayScale", "((1 repeatingRamp: 2 s) * 2 - 1) abs")})},
                                   {connection("source", "audioOut", "path", "audioIn")}, "path"),
        at(2.0));
    // The delay changes by 50 ms a second: the frequency moves by 5%.
    require(std::abs(frequencyBetween(played, 0.3, 0.7) - 1050.0) < 2.0,
        "the pitch should rise while the delay shortens");
    require(std::abs(frequencyBetween(played, 1.3, 1.7) - 950.0) < 2.0,
        "the pitch should fall while the delay grows");
}

// A Script whose instruments last @dur (or 1 s) and play @freq (or their
// own frequency) at a quarter amplitude, rendered for the given seconds.
std::vector<double> scriptOfSounds(std::string script, double seconds)
{
    auto document = heard({node("tone", "talas.oscillator", {expression("frequency", "@freq"), expression("amplitude", "0.25")}),
                              node("aSound", "talas.setDuration", {expression("duration", "@dur")}),
                              node("evenTone", "talas.oscillator", {expression("frequency", "300 hz"), expression("amplitude", "0.25")}),
                              node("evenSound", "talas.setDuration", {expression("duration", "1 s")}),
                              node("oddTone", "talas.oscillator", {expression("frequency", "500 hz"), expression("amplitude", "0.25")}),
                              node("oddSound", "talas.setDuration", {expression("duration", "1 s")}),
                              node("score", "talas.script", {text("script", std::move(script))})},
        {connection("tone", "audioOut", "aSound", "audioIn"), connection("evenTone", "audioOut", "evenSound", "audioIn"),
            connection("oddTone", "audioOut", "oddSound", "audioIn"), connection("aSound", "audioOut", "score", "inputs"),
            connection("evenSound", "audioOut", "score", "inputs"), connection("oddSound", "audioOut", "score", "inputs")},
        "score", "leftOut");
    document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 100.0},
        {"dur", "dur", talas::expr::ValueDomain::Seconds, 1.0}};
    return render(document, at(seconds));
}

// 4 timesRepeat: starts four copies of aSound at once,
// sounding like aSound four times louder; 0 to: 3 do: starts a copy each
// second, a hundred hertz higher each time, and the whileTrue: loop that
// counts i up to 3 does the same; 0 to: 5 do: with ifTrue:ifFalse: on
// i \\ 2 = 0 alternates evenSound and oddSound.
void testScriptLoopsAndConditionalsStartTheirSounds()
{
    const auto level = [](const std::vector<double>& played, double from, double frequency) {
        return toneLevel(std::span{played}.subspan(at(from), at(0.5)), frequency);
    };
    const auto four = scriptOfSounds("4 timesRepeat: [ aSound start: 0 s freq: 100 hz dur: 1 s].", 1.5);
    require(std::abs(level(four, 0.25, 100.0) - 1.0) < 0.01
            && std::ranges::all_of(std::span{four}.subspan(at(1.01)), [](double sample) { return sample == 0.0; }),
        "four copies started together should sound four times louder for their second");
    const auto counted = scriptOfSounds("0 to: 3 do: [ :i | aSound start: i s freq: ((i + 1) * 100) hz dur: 1 s].", 4.0);
    for (int i = 0; i < 4; ++i)
    {
        const auto frequency = 100.0 * (i + 1);
        require(std::abs(level(counted, i + 0.25, frequency) - 0.25) < 0.01
                && (i == 0 || level(counted, i + 0.25, frequency - 100.0) < 0.01),
            "to:do: should start the copy of second " + std::to_string(i) + " at "
                + std::to_string(frequency) + " hz alone");
    }
    const auto whileLoop = scriptOfSounds(
        "| i | i := 0. [i <= 3] whileTrue: [ aSound start: i s freq: ((i + 1) * 100) hz dur: 1 s. i := i + 1].", 4.0);
    require(whileLoop == counted, "the whileTrue: loop should play as the to:do: loop does");
    const auto alternating = scriptOfSounds(
        "0 to: 5 do: [ :i | (i \\\\ 2 = 0) ifTrue: [evenSound start: i s] ifFalse: [oddSound start: i s]].", 6.0);
    for (int i = 0; i < 6; ++i)
    {
        const auto even = i % 2 == 0;
        require(level(alternating, i + 0.25, even ? 300.0 : 500.0) > 0.24
                && level(alternating, i + 0.25, even ? 500.0 : 300.0) < 0.01,
            "second " + std::to_string(i) + " should play " + (even ? "evenSound" : "oddSound"));
    }
}

// A file interpreter whose script starts a 5 ms beep at each check's time
// (i * 10 ms) only when the check holds: the beeps say which checks held.
std::vector<bool> heldChecks(std::string typeId, std::vector<std::uint8_t> file, std::string script, int checks)
{
    talas::project::ProjectParameterExpression fileName;
    fileName.parameterId = "fileName";
    fileName.resource = "Files/Data";
    auto document = heard({node("beep", "talas.oscillator", {expression("frequency", "1000 hz"), expression("amplitude", "0.5")}),
                              node("anInstr", "talas.setDuration", {expression("duration", "5 ms")}),
                              node("score", std::move(typeId), {fileName, text("script", std::move(script))})},
        {connection("beep", "audioOut", "anInstr", "audioIn"), connection("anInstr", "audioOut", "score", "inputs")}, "score");
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.binaryResources.push_back({"Files/Data", std::move(file)});
    const auto played = render(document, at(0.01 * (checks + 2)), settings);
    std::vector<bool> held;
    for (int check = 1; check <= checks; ++check)
        held.push_back(std::ranges::any_of(std::span{played}.subspan(at(0.01 * check), at(0.005)),
            [](double sample) { return std::abs(sample) > 0.1; }));
    return held;
}

void requireAllHeld(const std::vector<bool>& held, const std::vector<std::string>& names, const std::string& page)
{
    for (std::size_t index = 0; index < held.size(); ++index)
        require(held[index], page + ": " + names[index]);
}

// A MIDIFileInterpreter's events answer what they
// are (isControl, isProgramChange, isChannelPressure, isPitchWheel,
// isPolyphonicKeyPressure, isSwitch, isSwitchOn, isSwitchOff, isNoteOn,
// isNoteOff and status as a Symbol), their channel (1-16) and values
// (control number and value, program number, pressure values, the pitch
// wheel's change within +-2048, velocity), the last control and pitch
// wheel values seen on a channel, and the file can be reset.
void testMidiFileInterpreterEventMessages()
{
    // At 120 bpm and 96 ticks a quarter: on channel 2 control 7 at 100, on
    // 3 program 5, on 4 pressure 64, on 1 the wheel up a quarter (+2048
    // of +-8192), key pressure 45 on 60, the sustain switch on and off, a
    // note 60 at velocity 80 for a quarter and a note off of 62 at 32.
    const std::vector<std::uint8_t> track{0x00, 0xB1, 7, 100, 0x00, 0xC2, 5, 0x00, 0xD3, 64, 0x00, 0xE0, 0x00, 0x50,
        0x00, 0xA0, 60, 45, 0x00, 0xB0, 64, 127, 0x00, 0xB0, 64, 0, 0x00, 0x90, 60, 80, 0x60, 0x80, 60, 0, 0x00, 0x80, 62,
        32, 0x00, 0xFF, 0x2F, 0x00};
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x00, 0x60, 'M', 'T', 'r', 'k', 0, 0, 0,
        static_cast<std::uint8_t>(track.size())};
    file.insert(file.end(), track.begin(), track.end());
    const std::vector<std::string> names{"a control's channel, number, value and status", "a program change",
        "a channel pressure", "the pitch wheel's change within +-2048", "a polyphonic key pressure",
        "a switch turned on", "a switch turned off", "a note on's velocity and status",
        "the last control value seen on a channel", "the last pitch wheel value in half steps",
        "ifNone: when no such value was seen", "a note off's velocity and status", "reset moves to the first event"};
    const auto held = heldChecks("talas.midiFileInterpreter", file,
        "| e c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 c12 c13 |\n"
        "[file atEnd] whileFalse: [e := file nextEvent.\n"
        " (e isControl and: [e controlNumber = 7]) ifTrue: [c1 := (e channel = 2) & (e controlValue = 100) & (e status == #control)].\n"
        " e isProgramChange ifTrue: [c2 := (e programNumber = 5) & (e channel = 3) & (e status == #programChange)].\n"
        " e isChannelPressure ifTrue: [c3 := (e channelPressureValue = 64) & (e channel = 4) & (e status == #channelPressure)].\n"
        " e isPitchWheel ifTrue: [c4 := (e pitchWheelChange = 512) & (e status == #pitchWheel)].\n"
        " e isPolyphonicKeyPressure ifTrue: [c5 := (e polyphonicKeyPressureValue = 45) & (e status == #polyphonicKeyPressure)].\n"
        " (e isSwitch and: [e isSwitchOn]) ifTrue: [c6 := e controlValue = 127].\n"
        " (e isSwitch and: [e isSwitchOff]) ifTrue: [c7 := e controlValue = 0].\n"
        " e isNoteOn ifTrue: [c8 := (e velocity = 80) & (e status == #noteOn) & (e channel = 1).\n"
        "   c9 := (e lastControlValueForChannel: 1 controlNumber: 64 ifNone: [99]) = 0.\n"
        "   c10 := (e lastPitchWheelValueForChannel: 1 sensitivityInHalfSteps: 2 ifNone: [99]) = 0.5.\n"
        "   c11 := (e lastControlValueForChannel: 5 controlNumber: 64 ifNone: [99]) = 99].\n"
        " e isNoteOff ifTrue: [c12 := (e velocity = 32) & (e status == #noteOff)]].\n"
        "file reset. e := file nextEvent. c13 := e isControl & (e controlNumber = 7).\n"
        "{c1. c2. c3. c4. c5. c6. c7. c8. c9. c10. c11. c12. c13} doWithIndex: [:c :i | c == true ifTrue: [anInstr start: (i * 10) ms]].",
        13);
    requireAllHeld(held, names, "MIDIFileInterpreter event messages");
}

// A TextFileInterpreter's maxValue and minValue are the
// file's largest and smallest numbers, -infinity and +infinity in a file
// without numbers; nextNumber answers the next number, 0 at a word and at
// the end; linesInFile counts its lines and reset starts it again.
void testTextFileInterpreterMessages()
{
    const std::string numbers = "3 7 x\n-2 9\n";
    const auto held = heldChecks("talas.textFileInterpreter", {numbers.begin(), numbers.end()},
        "| a b c d e f g |\n"
        "a := (file maxValue = 9) & (file minValue = -2) & (file linesInFile = 2).\n"
        "b := (file nextNumber = 3) & (file nextNumber = 7).\n"
        "c := file nextNumber = 0.\n"
        "d := (file nextNumber = -2) & (file nextNumber = 9).\n"
        "e := file nextNumber = 0.\n"
        "file reset. f := file nextNumber = 3.\n"
        "{a. b. c. d. e. f} doWithIndex: [:check :i | check == true ifTrue: [anInstr start: (i * 10) ms]].",
        6);
    requireAllHeld(held, {"maxValue, minValue and linesInFile", "nextNumber reading numbers", "nextNumber at a word",
                             "nextNumber reading on", "nextNumber at the end", "reset"},
        "TextFileInterpreter messages");
    const std::string words = "no numbers here\n";
    const auto empty = heldChecks("talas.textFileInterpreter", {words.begin(), words.end()},
        "{file maxValue < -1.0e300. file minValue > 1.0e300} doWithIndex: [:check :i | check == true ifTrue: [anInstr start: (i * 10) ms]].",
        2);
    requireAllHeld(empty, {"maxValue of no numbers", "minValue of no numbers"}, "TextFileInterpreter messages");
}

// A FrequencyTracker's output scaled by half the
// sample rate (22050 at 44.1 kHz) compared with gt:, le: and lt: gives 1
// or 0, so ((22050 * freq L) gt: !Freq nn hz removeUnits) and its le:
// twin pass one input or the other by pitch, and multiplying a gt: and an
// lt: is a logical AND, one only while the pitch lies between 4 f and 4 g.
void testPitchComparisonsChooseByThePitch()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t frames = 22050;
    const auto compared = [&](double inputFrequency, std::string field, double freq) {
        std::vector<double> input(frames);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            input[frame] = static_cast<double>(static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * inputFrequency * static_cast<double>(frame) / rate)));
        }
        talas::runtime::RenderSettings settings{rate, 64};
        settings.eventValues = {{"Freq", freq, talas::expr::ValueDomain::NoteNumber}};
        const auto played = render(heardValue({node("voice", "talas.audioInput"),
                                                  node("freq", "talas.frequencyTracker", {expression("minFrequency", "100 hz"),
                                                                                         expression("maxFrequency", "1000 hz")})},
                                       {connection("voice", "audioOut", "freq", "audioIn")}, std::move(field)),
            frames, settings, {std::move(input)});
        return played.back();
    };
    const auto above = "(22050 * freq L) gt: !Freq nn hz removeUnits";
    const auto atOrBelow = "(22050 * freq L) le: !Freq nn hz removeUnits";
    require(compared(370.0, above, 60.0) == 1.0 && compared(370.0, atOrBelow, 60.0) == 0.0,
        "a pitch above !Freq should pass the gt: input and not the le: one");
    require(compared(370.0, above, 72.0) == 0.0 && compared(370.0, atOrBelow, 72.0) == 1.0,
        "a pitch at or below !Freq should pass the le: input and not the gt: one");
    const auto window = "((22050 * freq L) gt: 4 f hz removeUnits) * ((22050 * freq L) lt: 4 g hz removeUnits)";
    require(compared(370.0, window, 0.0) == 1.0 && compared(300.0, window, 0.0) == 0.0
            && compared(440.0, window, 0.0) == 0.0,
        "the product of gt: and lt: should be one only between 4 f and 4 g");
}

// A Script's events start their instrument at their
// start times (a 5 s instrument at 0 s and 6 s leaves a second of
// silence), times in beats follow the metronome MM, which a script may
// redefine as it goes and which is 60 in a script that sets none; the
// parameter:value pairs may come in any order, separated by any spaces,
// tabs or returns, and any may be omitted, but an event without a start
// time is not scheduled; and a script of three events plays three notes.
// (An event names its Sound and its start time first, then its pairs.)
void testScriptEventsScheduleTheirInstruments()
{
    const auto level = [](const std::vector<double>& played, double from, double frequency, double length = 0.2) {
        return toneLevel(std::span{played}.subspan(at(from), at(length)), frequency);
    };
    const auto silent = [](const std::vector<double>& played, double from, double to) {
        return std::ranges::all_of(std::span{played}.subspan(at(from), at(to) - at(from)),
            [](double sample) { return sample == 0.0; });
    };
    const auto twice = scriptOfSounds("aSound start: 0 s dur: 5 s. aSound start: 6 s.", 11.5);
    require(level(twice, 2.0, 100.0) > 0.24 && silent(twice, 5.01, 5.99) && level(twice, 8.0, 100.0) > 0.24
            && silent(twice, 11.01, 11.5),
        "two 5 s copies at 0 s and 6 s should leave a second of silence between them");
    const auto metronome = scriptOfSounds("MM := 120. aSound start: 1 beats dur: 0.25 s. MM := 60. aSound start: 2 beats dur: 0.25 s freq: 200 hz.", 2.5);
    require(silent(metronome, 0.0, 0.49) && level(metronome, 0.5, 100.0) > 0.24 && silent(metronome, 0.76, 1.99)
            && level(metronome, 2.0, 200.0) > 0.24,
        "MM should time the beats and may be redefined within the script");
    const auto defaulted = scriptOfSounds("aSound start: 1 beat dur: 0.25 s.", 1.5);
    require(silent(defaulted, 0.0, 0.99) && level(defaulted, 1.0, 100.0) > 0.24,
        "a script without MM should count 60 beats a minute");
    const auto ordered = scriptOfSounds(
        "aSound start: 0 s dur: 0.5 s\n\tfreq:   300 hz.\naSound freq: 500 hz dur: 0.5 s.\naSound start: 1 s.", 1.6);
    require(level(ordered, 0.1, 300.0) > 0.24 && level(ordered, 0.1, 500.0) < 0.01 && level(ordered, 1.1, 300.0) > 0.24
            && silent(ordered, 0.51, 0.99) && silent(ordered, 1.51, 1.6),
        "pairs in any order and spacing should set the event, one without a start time should not play, "
            "and an event that omits its pairs should take the previous event's values");
    const auto three = scriptOfSounds(
        "aSound start: 0 s freq: 100 hz dur: 1 s. aSound start: 1 s freq: 200 hz dur: 1 s. aSound start: 2 s freq: 300 hz dur: 1 s.",
        3.0);
    require(level(three, 0.4, 100.0) > 0.24 && level(three, 1.4, 200.0) > 0.24 && level(three, 2.4, 300.0) > 0.24
            && level(three, 1.4, 100.0) < 0.01,
        "the script's three events should play three notes one after another");
}

// A Sound reading a recording takes Default in its
// Frequency field as the recording's own frequency, and arithmetic on it:
// Default * 0.5 an octave lower, Default nn + 7 nn a fifth higher.
void testDefaultFrequencyArithmetic()
{
    std::vector<double> ramp(400);
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<double>(index + 1) / 400.0;
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = {{"Samples/Ramp", ramp}};
    const auto played = [&](std::string frequency) {
        return render(heard({node("source", "talas.genericSource",
                                {text("source", "sample in RAM"), resource("sample", "Samples/Ramp"),
                                    expression("frequency", std::move(frequency))})},
                          {}, "source", "leftOut"),
            128, settings);
    };
    const auto natural = played("Default");
    const auto lower = played("Default * 0.5");
    const auto fifth = played("Default nn + 7 nn");
    const auto ratio = std::pow(2.0, 7.0 / 12.0);
    require(std::abs(natural[60] - ramp[60]) < 1.0e-9, "Default should play the recording's own frequency");
    require(std::abs(lower[60] - ramp[30]) < 1.0e-9 && std::abs(lower[61] - 0.5 * (ramp[30] + ramp[31])) < 1.0e-9,
        "Default * 0.5 should play the recording an octave lower");
    const auto position = 60.0 * ratio;
    const auto below = static_cast<std::size_t>(std::floor(position));
    const auto expected = ramp[below] + (position - static_cast<double>(below)) * (ramp[below + 1] - ramp[below]);
    require(std::abs(fifth[60] - expected) < 1.0e-6, "Default nn + 7 nn should play a fifth higher");
}

// A Script starts copies of its inputs at the times its
// script gives: s1 at 0 s, s2 at 1 s and s1 again at 3 s.
void testScriptPlacesItsInputsOnATimeline()
{
    auto document = heard({oscillator("s1", "440 hz"), oscillator("s2", "660 hz"),
                              node("score", "talas.script", {text("script", "s1 start: 0 s. s2 start: 1 s. s1 start: 3 s.")})},
        {connection("s1", "audioOut", "score", "inputs"), connection("s2", "audioOut", "score", "inputs")}, "score",
        "leftOut");
    const auto played = render(document, at(4.0));
    const auto level = [&](double from, double frequency) {
        return toneLevel(std::span{played}.subspan(at(from), at(0.5)), frequency);
    };
    require(level(0.25, 440.0) > 0.9 && level(0.25, 660.0) < 0.01,
        "only s1 should sound before 1 s");
    require(level(1.5, 440.0) > 0.9 && level(1.5, 660.0) > 0.9, "s2 should join s1 at 1 s");
    // The second copy of s1 starts a whole number of its cycles later, so
    // the two copies add.
    require(level(3.25, 440.0) > 1.9 && level(3.25, 660.0) > 0.9,
        "a second copy of s1 should start at 3 s");
}

// An event of a script sets its instrument's variables, so
// aSound start: 3.2 s freq: 443 hz amp: 0.5 plays a copy at 443 hz and half
// amplitude from 3.2 s.
void testScriptEventSetsFrequencyAndAmplitude()
{
    auto document = heard({node("aSound", "talas.oscillator",
                               {expression("frequency", "@freq"), expression("amplitude", "@amp")}),
                              node("score", "talas.script", {text("script", "aSound start: 3.2 s freq: 443 hz amp: 0.5.")})},
        {connection("aSound", "audioOut", "score", "inputs")}, "score", "leftOut");
    document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 100.0},
        {"amp", "amp", scalar, 1.0}};
    const auto played = render(document, at(3.6));
    require(std::ranges::all_of(std::span{played}.first(at(3.19)), [](double sample) { return sample == 0.0; }),
        "the copy should be silent before its start time");
    const auto tail = std::span{played}.subspan(at(3.25), at(0.3));
    require(std::abs(measuredFrequency(tail) - 443.0) < 0.05 && std::abs(toneLevel(tail, 443.0) - 0.5) < 0.01,
        "the copy should play at 443 hz and half amplitude");
}

// A Script's events give their instrument's
// variables values by name, in any order; a value an event leaves out is
// the one the event before gave, and an event with no start time is not
// scheduled.
void testScriptEventsKeepEarlierValues()
{
    auto document = heard({node("anInstr", "talas.oscillator", {expression("frequency", "?freq"), expression("amplitude", "?amp")}),
                              node("score", "talas.script",
                                  {text("script", "anInstr start: 0 s amp: 0.5 freq: 300 hz.\n"
                                                  "anInstr start: 0.5 s.\n"
                                                  "anInstr start: 1 s freq: 4 a.\n"
                                                  "anInstr amp: 0.25.")})},
        {connection("anInstr", "audioOut", "score", "inputs")}, "score", "leftOut");
    document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 100.0},
        {"amp", "amp", scalar, 1.0}};
    const auto played = render(document, at(1.5));
    const auto level = [&](double from, double frequency) {
        return toneLevel(std::span{played}.subspan(at(from), at(0.2)), frequency);
    };
    require(std::abs(level(0.2, 300.0) - 0.5) < 0.01, "the first event should play 300 hz at 0.5");
    require(std::abs(level(0.7, 300.0) - 1.0) < 0.01 && level(0.7, 100.0) < 0.01,
        "an event that sets nothing should take the earlier event's values");
    require(std::abs(level(1.2, 440.0) - 0.5) < 0.01 && std::abs(level(1.2, 300.0) - 1.0) < 0.01,
        "an event setting only freq: should keep the earlier amp:, and no event without a start time should play");
}

// A whole-tone scale as a Smalltalk loop, p declared
// between bars and set to 3 c, each of five events a second long at p and
// p then raised two half steps, Left rising by fifths; then starts half a
// second apart in minor thirds, overlapping.
void testWholeToneScaleScript()
{
    const auto played = [](std::string script, double seconds) {
        auto document = heard({node("tone", "talas.oscillator", {expression("frequency", "?freq"), expression("amplitude", "0.25")}),
                                  node("pan", "talas.level", {expression("left", "?left"), expression("right", "1 - ?left")}),
                                  node("instr", "talas.setDuration", {expression("duration", "?dur")}),
                                  node("score", "talas.script", {text("script", std::move(script))})},
            {connection("tone", "audioOut", "pan", "audioIn"), connection("pan", "leftOut", "instr", "audioIn"),
                connection("instr", "audioOut", "score", "inputs")},
            "score", "leftOut");
        document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 440.0},
            {"dur", "dur", talas::expr::ValueDomain::Seconds, 1.0}, {"left", "left", scalar, 0.5}};
        return render(document, at(seconds));
    };
    const auto whole = played("| p | p := 3 c.\n"
                              "1 to: 5 do: [ :i |\n"
                              "instr\n"
                              "start: (i - 1) s\n"
                              "freq: p\n"
                              "dur: 1 s\n"
                              "left: i / 5.\n"
                              "p := p + 2 nn].",
        5.0);
    for (int note = 0; note < 5; ++note)
    {
        const auto heard = std::span{whole}.subspan(at(note + 0.1), at(0.8));
        const auto expected = 48.0 + 2.0 * note;
        require(std::abs(noteNumber(measuredFrequency(heard)) - expected) < 0.02,
            "note " + std::to_string(note + 1) + " should be " + std::to_string(expected) + " nn, two half steps up, not " + std::to_string(noteNumber(measuredFrequency(heard))));
        require(std::abs(toneLevel(heard, 440.0 * std::exp2((expected - 69.0) / 12.0)) - 0.05 * (note + 1)) < 0.005,
            "note " + std::to_string(note + 1) + " should be heard at Left " + std::to_string(note + 1) + "/5");
    }
    const auto thirds = played("| p | p := 3 c.\n"
                               "1 to: 5 do: [ :i |\n"
                               "instr start: (i - 1 * 0.5) s freq: p dur: 1 s left: i / 5.\n"
                               "p := p + 3 nn].",
        3.0);
    const auto overlap = std::span{thirds}.subspan(at(0.6), at(0.3));
    const auto hertz = [](double note) { return 440.0 * std::exp2((note - 69.0) / 12.0); };
    require(toneLevel(overlap, hertz(48.0)) > 0.04 && toneLevel(overlap, hertz(51.0)) > 0.09
            && toneLevel(std::span{thirds}.subspan(at(1.1), at(0.3)), hertz(48.0)) < 0.01,
        "notes half a second apart and a minor third higher should overlap for half a second");
}

// A Frequency field takes a constant in hertz or note
// number, or an expression mapping Event Values to one: !Pitch plays the
// equal-tempered scale; (!Pitch * 0.5) + 30 nn turns half steps into
// quarter steps and keeps middle C; (!Pitch * 0.25) + 45 nn gives eighth
// steps, four keyboard octaves to one octave of pitch; (!KeyNumber / 12)
// twoExp * 8.176 hz computes the equal-tempered scale in hertz.
void testKeyboardsInOtherSteps()
{
    const auto pitch = [](std::string frequency, std::string name, double value, talas::expr::ValueDomain domain) {
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{std::move(name), value, domain}};
        const auto played = render(heard({oscillator("osc", std::move(frequency))}, {}, "osc"), at(0.3), settings);
        return noteNumber(frequencyBetween(played, 0.05, 0.3));
    };
    constexpr auto nn = talas::expr::ValueDomain::NoteNumber;
    require(std::abs(pitch("440 hz", "Unused", 0.0, scalar) - 69.0) < 0.01
            && std::abs(pitch("61 nn", "Unused", 0.0, scalar) - 61.0) < 0.01,
        "a Frequency field should take a constant in hertz or note number");
    require(std::abs(pitch("!Pitch", "Pitch", 61.0, nn) - 61.0) < 0.01,
        "!Pitch should play the equal-tempered scale");
    for (const auto& [key, expected] : {std::pair{60.0, 60.0}, {61.0, 60.5}, {72.0, 66.0}})
    {
        require(std::abs(pitch("(!Pitch * 0.5) + 30 nn", "Pitch", key, nn) - expected) < 0.01,
            "(!Pitch * 0.5) + 30 nn should play key " + std::to_string(key) + " at "
                + std::to_string(expected));
    }
    for (const auto& [key, expected] : {std::pair{60.0, 60.0}, {61.0, 60.25}, {108.0, 72.0}})
    {
        require(std::abs(pitch("(!Pitch * 0.25) + 45 nn", "Pitch", key, nn) - expected) < 0.01,
            "(!Pitch * 0.25) + 45 nn should play key " + std::to_string(key) + " at "
                + std::to_string(expected));
    }
    for (const auto key : {57.0, 69.0, 72.0})
    {
        require(std::abs(pitch("(!KeyNumber / 12) twoExp * 8.176 hz", "KeyNumber", key, scalar) - key) < 0.01,
            "(!KeyNumber / 12) twoExp * 8.176 hz should be the equal-tempered key "
                + std::to_string(key));
    }
}

// A sine added to a copy of itself delayed by one cycle is
// reinforced, and delayed by half a cycle cancelled.
void testDelayedCopyReinforcesOrCancels()
{
    const auto summed = [](std::string delay) {
        const auto played = render(heard({oscillator("sine", "500 hz"),
                                             node("late", "talas.delayWithFeedback",
                                                 {expression("delay", std::move(delay)), expression("feedback", "0"),
                                                     expression("scale", "1")}),
                                             node("sum", "talas.mixer")},
                                       {connection("sine", "audioOut", "late", "audioIn"),
                                           connection("sine", "audioOut", "sum", "in1"),
                                           connection("late", "audioOut", "sum", "in2")},
                                       "sum"),
            at(0.2));
        return toneLevel(std::span{played}.subspan(at(0.1), at(0.1)), 500.0);
    };
    require(std::abs(summed("2 ms") - 2.0) < 0.01, "a delay of one cycle should reinforce the sine");
    require(summed("1 ms") < 0.01, "a delay of half a cycle should cancel the sine");
}

// A negative frequency makes the Oscillator read its
// wavetable backwards, so a sine at -440 hz sounds the same as at 440 hz
// but 180 degrees out of phase, constant or from an Event Value.
void testNegativeFrequencyReadsTheWavetableBackwards()
{
    const auto forwards = render(heard({oscillator("osc", "440 hz")}, {}, "osc"), at(0.05));
    const auto backwards = render(heard({oscillator("osc", "-440 hz")}, {}, "osc"), at(0.05));
    talas::runtime::RenderSettings live{sampleRate, 64};
    live.eventValues = {{"Freq", -440.0, talas::expr::ValueDomain::Hertz}};
    const auto fromEventValue = render(heard({oscillator("osc", "!Freq")}, {}, "osc"), at(0.05), live);
    for (std::size_t frame = 0; frame < forwards.size(); ++frame)
    {
        require(std::abs(backwards[frame] + forwards[frame]) < 1.0e-9
                && std::abs(fromEventValue[frame] + forwards[frame]) < 1.0e-9,
            "a negative frequency should play the sine 180 degrees out of phase at frame "
                + std::to_string(frame));
    }
    // A ramp table of eight steps read an eighth of a cycle per sample, in
    // reverse.
    talas::runtime::RenderSettings table{sampleRate, 64};
    table.tableResources = {{"Tables/Steps", {0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875}}};
    const auto steps = render(heard({node("osc", "talas.oscillator",
                                        {expression("frequency", "-6000 hz"), resource("wavetable", "Tables/Steps")})},
                                  {}, "osc"),
        64, table);
    for (std::size_t frame = 0; frame < steps.size(); ++frame)
    {
        require(std::abs(steps[frame] - 0.125 * static_cast<double>((8 - frame % 8) % 8)) < 1.0e-9,
            "a negative frequency should read the table backwards at frame " + std::to_string(frame));
    }
}

// Continuous shepard's: a ramp repeating once a frame gives a
// SyntheticSpectrumFromSounds with LogScale pitches evenly spaced, static
// from frame to frame. A little faster, the ramp rolls against the frames:
// every pitch rises a little each frame until it reaches the top and comes
// back in at the bottom; a little slower, every pitch falls. Here eight
// partials and an Oscillator on Ramp scaled to the two octaves below half
// the sample rate, the log frequencies 13/15 to 1.
void testRollingRampMakesCircularPitches()
{
    // Each frame's frequency of a track, in hz.
    const auto pitches = [](std::string rate, int track) {
        auto document = heard({node("ramp", "talas.oscillator",
                                   {expression("frequency", std::move(rate)), expression("amplitude", "2 / 15"),
                                       resource("wavetable", "Ramp")}),
                                  node("top", "talas.constant", {expression("value", "13 / 15")}),
                                  node("pitches", "talas.mixerV2"),
                                  node("level", "talas.constant", {expression("value", "0.5")}),
                                  node("spectrum", "talas.syntheticSpectrumFromSounds",
                                      {expression("nbrPartials", "8"), boolean("logScale", true)}),
                                  node("linear", "talas.spectrumLogToLinear"),
                                  node("selector", "talas.spectrumTrackSelector",
                                      {expression("track", std::to_string(track))})},
            {connection("ramp", "audioOut", "pitches", "inputs"), connection("top", "value", "pitches", "inputs"),
                connection("level", "value", "spectrum", "amplitudes"),
                connection("pitches", "leftOut", "spectrum", "frequenciesAndBandwidths"),
                connection("spectrum", "spectrumOut", "linear", "spectrumIn"),
                connection("linear", "spectrumOut", "selector", "spectrumIn")},
            "selector", "rightOut");
        const auto played = render(document, 8 * 800);
        std::vector<double> frames;
        for (std::size_t frame = 0; frame < 800; ++frame)
            frames.push_back(played[frame * 8 + 7] * 0.5 * sampleRate);
        return frames;
    };
    std::vector<std::vector<double>> still;
    for (int track = 1; track <= 8; ++track)
        still.push_back(pitches("6000 hz", track));
    for (int track = 1; track < 8; ++track)
    {
        const auto step = std::log2(still[track][100] / still[track - 1][100]);
        require(std::abs(step - 0.25) < 1.0e-3 && std::abs(still[track][700] / still[track][100] - 1.0) < 1.0e-9,
            "a ramp once a frame should give pitches a quarter octave apart, the same each frame, not "
                + std::to_string(step) + " octaves at track " + std::to_string(track + 1));
    }
    require(std::abs(still[0][100] / (0.125 * sampleRate) - 1.0) < 1.0e-9,
        "the lowest pitch should be 13/15, two octaves below half the sample rate");
    // 1.25 hz faster, each pitch climbs an eighth of the ramp in 600 frames.
    const auto rising = pitches("6001.25 hz", 1);
    require(rising[10] < rising[300] && rising[300] < rising[590]
            && std::abs(std::log2(rising[600] / still[1][100])) < 0.01,
        "a faster ramp should raise every pitch frame by frame, to the next partial's");
    const auto top = pitches("6001.25 hz", 8);
    require(top[590] > 0.45 * sampleRate && top[700] < 0.15 * sampleRate && top[700] > still[0][100],
        "the highest pitch should wrap around to the bottom and climb again");
    const auto falling = pitches("5998.75 hz", 2);
    require(falling[300] < falling[10] && falling[590] < falling[300],
        "a slower ramp should lower every pitch frame by frame");
}

// The document saved and opened again as a project of an older format, so
// that its migrations give it that format's legacy choices.
talas::project::ProjectDocument reopenedAs(const talas::project::ProjectDocument& document, int format)
{
    auto json = talas::project::serializeProjectDocument(document);
    const auto current = "\"version\": " + std::to_string(talas::project::currentProjectFormatVersion);
    const auto found = json.find(current);
    require(found != std::string::npos, "the fixture should serialize in the current format");
    json.replace(found, current.size(), "\"version\": " + std::to_string(format));
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok(), "the fixture should open as format " + std::to_string(format)
        + (reopened.errors.empty() ? std::string{} : ": " + reopened.errors.front().message));
    return reopened.document;
}

// A compressor's Ratio is in decibels, a Ratio of 4/1 letting
// the Input rise 1 dB for every 4 dB the SideChain's envelope rises past
// the Threshold. Projects before format 100 keep dividing the level past
// the Threshold by the Ratio.
void testCompressionRatioIsInDecibels()
{
    constexpr std::size_t frames = 4800;
    const auto compressed = [](double side, int format, double level = 0.5) {
        auto document = heard({node("in", "talas.audioInput"),
                                  node("side", "talas.audioInput", {boolean("channel1", false), boolean("channel2", true)}),
                                  node("drc", "talas.dynamicRangeController",
                                      {choice("type", "compressor"), expression("ratio", "4"),
                                          expression("threshold", "0.2"), expression("attackTime", "1 ms"),
                                          expression("releaseTime", "10 ms")})},
            {connection("in", "audioOut", "drc", "audioIn"), connection("side", "audioOut", "drc", "sideChain")}, "drc");
        if (format != 0)
            document = reopenedAs(document, format);
        const auto exact = [](double value) { return static_cast<double>(static_cast<float>(value)); };
        return render(document, frames, {sampleRate, 64},
            {std::vector<double>(frames, exact(level)), std::vector<double>(frames, exact(side))})
            .back();
    };
    // Past the Threshold the gain is (envelope / Threshold)^(1/Ratio - 1).
    const auto side = static_cast<double>(static_cast<float>(0.8));
    require(std::abs(compressed(0.8, 0) - 0.5 * std::pow(side / 0.2, 0.25 - 1.0)) < 1.0e-6,
        "the compressor should follow its Ratio in decibels");
    // The Input as its own SideChain: 4 dB more of it past the Threshold
    // gives 1 dB more output.
    const auto exact = [](double value) { return static_cast<double>(static_cast<float>(value)); };
    const auto quiet = exact(0.4);
    const auto loud = exact(0.4 * std::pow(10.0, 4.0 / 20.0));
    const auto quieter = compressed(quiet, 0, quiet);
    const auto louder = compressed(loud, 0, loud);
    require(std::abs(20.0 * std::log10(louder / quieter) - 20.0 * std::log10(loud / quiet) / 4.0) < 1.0e-6,
        "4 dB more Input past the Threshold should give 1 dB more output at 4/1");
    require(std::abs(compressed(0.1, 0) - 0.5) < 1.0e-9, "below the Threshold the Input should pass");
    require(std::abs(compressed(0.8, 99) - 0.5 * (0.2 + (side - 0.2) / 4.0) / side) < 1.0e-6,
        "a project before format 100 should keep the linear Ratio");
}

// Once a compressor has attenuated the loudest parts, its
// Gain raises the whole signal without clipping where the same Gain alone
// would clip, and the quiet parts come up with it.
void testCompressorGainRaisesWithoutClipping()
{
    constexpr std::size_t frames = 4800;
    const auto played = [](double level, std::string ratio) {
        const auto exact = static_cast<double>(static_cast<float>(level));
        auto document = heard({node("in", "talas.audioInput"),
                                  node("drc", "talas.dynamicRangeController",
                                      {choice("type", "compressor"), expression("ratio", std::move(ratio)),
                                          expression("threshold", "0.2"), expression("attackTime", "1 ms"),
                                          expression("releaseTime", "10 ms"), expression("gain", "2")})},
            {connection("in", "audioOut", "drc", "audioIn"), connection("in", "audioOut", "drc", "sideChain")}, "drc");
        return render(document, frames, {sampleRate, 64}, {std::vector<double>(frames, exact)}).back();
    };
    require(played(0.9, "4") < 1.0 && played(0.9, "1") > 1.0,
        "a Gain of 2 should stay below clipping only after the loud part is compressed");
    require(std::abs(played(0.1, "4") - 2.0 * static_cast<double>(static_cast<float>(0.1))) < 1.0e-9,
        "the Gain should raise the quiet part below the Threshold");
}

// Timbre: filters split a signal into frequency
// bands, an AmplitudeFollower follows each, the ratio of the high band's
// envelope to the low one's, smoothed by a PeakDetector, is the amplitude
// envelope of pink noise, so the noise bursts wherever the input has much
// high-frequency energy. Here 200 hz, then 6000 hz.
void testHighToLowRatioShapesNoise()
{
    constexpr std::size_t frames = 9600;
    std::vector<double> input(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto frequency = frame < frames / 2 ? 200.0 : 6000.0;
        input[frame] = static_cast<double>(static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / sampleRate)));
    }
    const auto document = heard({node("in", "talas.audioInput"),
                                    node("low", "talas.filter", {choice("type", "lowPass"), expression("frequency", "1000 hz")}),
                                    node("high", "talas.filter", {choice("type", "highPass"), expression("frequency", "3000 hz")}),
                                    node("lowLevel", "talas.amplitudeFollower", {expression("timeConstant", "10 ms")}),
                                    node("highLevel", "talas.amplitudeFollower", {expression("timeConstant", "10 ms")}),
                                    node("ratio", "talas.clippingDivide"),
                                    node("smooth", "talas.peakDetector",
                                        {expression("attackTime", "5 ms"), expression("releaseTime", "50 ms")}),
                                    node("noise", "talas.noise", {choice("type", "pink")}),
                                    node("burst", "talas.product")},
        {connection("in", "audioOut", "low", "audioIn"), connection("in", "audioOut", "high", "audioIn"),
            connection("low", "audioOut", "lowLevel", "audioIn"), connection("high", "audioOut", "highLevel", "audioIn"),
            connection("highLevel", "value", "ratio", "numerator"), connection("lowLevel", "value", "ratio", "denominator"),
            connection("ratio", "audioOut", "smooth", "audioIn"), connection("noise", "audioOut", "burst", "inputs"),
            connection("smooth", "value", "burst", "inputs")},
        "burst", "value");
    const auto played = render(document, frames, {sampleRate, 64}, {input});
    const auto level = [&](std::size_t from, std::size_t to) {
        double sum = 0.0;
        for (auto frame = from; frame < to; ++frame)
            sum += played[frame] * played[frame];
        return std::sqrt(sum / static_cast<double>(to - from));
    };
    const auto low = level(3600, 4800);
    const auto high = level(8400, 9600);
    require(high > 0.01 && high > 10.0 * low,
        "the noise should burst on the high frequencies, not " + std::to_string(high) + " against "
            + std::to_string(low));
}

// Frequency: a PeakDetector's AttackTime and ReleaseTime
// react to increases and decreases of its input, so it makes an envelope
// that jumps up at an onset and decays slowly when the input goes to zero.
void testFastAttackSlowReleaseEnvelope()
{
    constexpr std::size_t frames = 24000;
    std::vector<double> beat(frames, 0.0);
    for (std::size_t frame = 4800; frame < 9600; ++frame)
        beat[frame] = static_cast<double>(static_cast<float>(
            0.8 * std::sin(2.0 * std::numbers::pi * 300.0 * static_cast<double>(frame) / sampleRate)));
    const auto followed = render(heard({node("in", "talas.audioInput"),
                                           node("peak", "talas.peakDetector",
                                               {expression("attackTime", "1 ms"), expression("releaseTime", "500 ms")})},
                                     {connection("in", "audioOut", "peak", "audioIn")}, "peak", "value"),
        frames, {sampleRate, 64}, {beat});
    require(followed[4800 + at(0.005)] > 0.6, "the envelope should jump up at the onset");
    require(followed[9600 + at(0.1)] > 0.5 && followed[9600 + at(0.1)] < followed[9599]
            && followed[frames - 1] < followed[9600 + at(0.1)],
        "the envelope should decay slowly once the input stops");
}

// Harmonics given symbolically, !Frequency, !Frequency * 2
// and so on, keep their harmonic relationship whenever !Frequency changes.
void testSymbolicHarmonicsFollowTheirFundamental()
{
    constexpr std::size_t frames = 19200;
    std::vector<talas::project::ProjectNode> nodes{node("partials", "talas.mixerV2")};
    std::vector<talas::core::Connection> connections;
    for (int harmonic = 1; harmonic <= 4; ++harmonic)
    {
        const auto id = "h" + std::to_string(harmonic);
        nodes.push_back(node(id, "talas.oscillator",
            {expression("frequency", harmonic == 1 ? "!Frequency" : "!Frequency * " + std::to_string(harmonic)),
                expression("amplitude", "0.2")}));
        connections.push_back(connection(id, "audioOut", "partials", "inputs"));
    }
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.eventValues = {{"Frequency", 200.0, talas::expr::ValueDomain::Hertz}};
    settings.eventValueChanges = {{9600, "Frequency", 330.0, talas::expr::ValueDomain::Hertz}};
    const auto played = render(heard(std::move(nodes), std::move(connections), "partials", "leftOut"), frames, settings);
    const auto before = std::span{played}.subspan(2400, 7200);
    const auto after = std::span{played}.subspan(12000, 7200);
    for (int harmonic = 1; harmonic <= 4; ++harmonic)
    {
        require(toneLevel(before, 200.0 * harmonic) > 0.05 && toneLevel(after, 330.0 * harmonic) > 0.05
                && toneLevel(after, 200.0 * harmonic) < 0.01,
            "harmonic " + std::to_string(harmonic) + " should follow !Frequency from 200 to 330 hz");
    }
}

// Format 109: a class made from an example whose
// Oscillators play ?freq, ?freq * 2, ?freq * 3 and ?freq * 4 takes ?freq as
// its one parameter, Frequency; each instance's Frequency goes in for
// ?freq, so every instance keeps its partials harmonic. The class keeps the
// variable through a save; a format 108 project cannot hold it.
void testClassFromExampleVariables()
{
    talas::project::ProjectSoundDefinition harmonics;
    harmonics.definitionId = "harmonics";
    harmonics.displayName = "Harmonics";
    harmonics.outputs = {{"audioOut", talas::core::PortType::Audio, "partials", "leftOut"}};
    harmonics.graph.nodes.push_back(node("partials", "talas.mixerV2"));
    for (int harmonic = 1; harmonic <= 4; ++harmonic)
    {
        const auto id = "h" + std::to_string(harmonic);
        harmonics.graph.nodes.push_back(node(id, "talas.oscillator",
            {expression("frequency", harmonic == 1 ? "?freq" : "?freq * " + std::to_string(harmonic)),
                expression("amplitude", "0.2")}));
        harmonics.graph.connections.push_back(connection(id, "audioOut", "partials", "inputs"));
    }
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("frequency", "100 hz");
    frequency.bindings.push_back(
        {talas::project::ProjectSoundParameterTarget::Variable, {}, {}, {}, talas::expr::ValueDomain::Scalar, "freq"});
    harmonics.exposedParameters.push_back(std::move(frequency));
    const auto played = [&](std::string fundamental) {
        auto instance = node("voice", "", {expression("frequency", std::move(fundamental))});
        instance.soundDefinitionId = "harmonics";
        auto document = heard({std::move(instance)}, {}, "voice");
        document.soundDefinitions.push_back(harmonics);
        return render(document, 9600);
    };
    for (const auto fundamental : {150.0, 200.0})
    {
        const auto tone = played(std::to_string(static_cast<int>(fundamental)) + " hz");
        const auto steady = std::span{tone}.subspan(2400);
        for (int harmonic = 1; harmonic <= 4; ++harmonic)
        {
            require(toneLevel(steady, fundamental * harmonic) > 0.05,
                "an instance at " + std::to_string(fundamental) + " hz should play harmonic "
                    + std::to_string(harmonic));
        }
        require(toneLevel(steady, fundamental * 1.5) < 0.01, "the partials should stay harmonic");
    }

    auto instance = node("voice", "", {expression("frequency", "150 hz")});
    instance.soundDefinitionId = "harmonics";
    auto document = heard({std::move(instance)}, {}, "voice");
    document.soundDefinitions.push_back(harmonics);
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok() && reopened.document.soundDefinitions.front().exposedParameters.front().bindings.front()
                == harmonics.exposedParameters.front().bindings.front(),
        "the class should keep its variable through a save");
    auto json = talas::project::serializeProjectDocument(document);
    const auto current = "\"version\": " + std::to_string(talas::project::currentProjectFormatVersion);
    json.replace(json.find(current), current.size(), "\"version\": 108");
    require(!talas::project::parseProjectDocument(json).ok(), "a format 108 project should not hold a variable binding");
}

// Making a class from an example turns the variables
// its fields read into the class's parameters, so Extract to Sound on two
// Oscillators at ?freq and ?freq * 2 gives a Sound whose Frequency
// parameter goes in for ?freq. Its default is the project's ?freq itself,
// so the Sound plays as before; another Frequency moves both partials.
void testExtractedSoundTakesTheExampleVariables()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.variableLibrary.variables = {{"freq", "freq", talas::expr::ValueDomain::Hertz, 220.0}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{document, registry};
    const auto ok = [](const talas::project::EditResult& result, const std::string& what) {
        require(result.ok(), what + ": " + result.message);
    };
    ok(editor.addNode(node("low", "talas.oscillator", {expression("frequency", "?freq"), expression("amplitude", "0.2")}),
           {0, 0}),
        "the example's first Oscillator");
    ok(editor.addNode(node("high", "talas.oscillator", {expression("frequency", "?freq * 2"), expression("amplitude", "0.2")}),
           {0, 100}),
        "the example's second Oscillator");
    ok(editor.addNode(node("partials", "talas.mixerV2"), {200, 50}), "the example's Mixer");
    ok(editor.addNode(node("out", "talas.audioOutput"), {400, 50}), "the output");
    ok(editor.connect(connection("low", "audioOut", "partials", "inputs")), "a partial");
    ok(editor.connect(connection("high", "audioOut", "partials", "inputs")), "another partial");
    ok(editor.connect(connection("partials", "leftOut", "out", "audioIn")), "the output connection");
    // Encapsulating a Sound makes it no more efficient; it
    // compiles to the same processing and plays the same samples.
    const auto processing = [&](const talas::project::ProjectDocument& played) {
        const auto compiled = talas::runtime::compileProject(played, registry, {sampleRate, 64});
        std::vector<std::string> types;
        if (compiled.ok())
        {
            for (const auto& step : compiled.graph->plan().steps)
                types.push_back(step.typeId);
            std::ranges::sort(types);
        }
        return types;
    };
    const auto example = editor.document();
    ok(editor.extractNodesToSound({"low", "high", "partials"}, "harmonics", "Harmonics", "voice"), "extracting the Sound");
    require(!processing(example).empty() && processing(example) == processing(editor.document())
            && render(example, 9600) == render(editor.document(), 9600),
        "the encapsulated Sound should compile to the same processing and play alike");
    const auto& definition = editor.document().soundDefinitions.front();
    require(definition.exposedParameters.size() == 1 && definition.exposedParameters.front().parameterId == "freq"
            && definition.exposedParameters.front().domain == talas::expr::ValueDomain::Hertz
            && definition.exposedParameters.front().defaultValue.expression == std::optional<std::string>{"?freq"}
            && definition.exposedParameters.front().bindings.front().target
                == talas::project::ProjectSoundParameterTarget::Variable,
        "the example's ?freq should become the Sound's one parameter");
    const auto steady = [&] {
        const auto played = render(editor.document(), 9600);
        return std::vector<double>(played.begin() + 2400, played.end());
    };
    const auto before = steady();
    require(toneLevel(before, 220.0) > 0.05 && toneLevel(before, 440.0) > 0.05,
        "the extracted Sound should play as the example did");
    ok(editor.setParameter("voice", expression("freq", "330 hz")), "setting the instance's Frequency");
    const auto after = steady();
    require(toneLevel(after, 330.0) > 0.05 && toneLevel(after, 660.0) > 0.05 && toneLevel(after, 220.0) < 0.01,
        "the instance's Frequency should move both partials");
}

// A ramp rests at 1 until its receiver first becomes
// positive, so !KeyDown ramp: 0.1 s is 1 before the first key and starts
// from 0 at it; projects before format 100 start it at 0.
void testRampRestsAtOneUntilItsTrigger()
{
    const auto ramped = [](int format) {
        auto document = heardValue({}, {}, "!KeyDown ramp: 0.1 s");
        if (format != 0)
            document = reopenedAs(document, format);
        talas::runtime::RenderSettings settings{sampleRate, 64};
        settings.eventValues = {{"KeyDown", 0.0, scalar}};
        settings.eventValueChanges = {{at(0.1), "KeyDown", 1.0, scalar}};
        return render(document, at(0.3), settings);
    };
    const auto resting = ramped(0);
    require(resting[at(0.05)] == 1.0 && resting[at(0.101)] < 0.05 && std::abs(resting[at(0.15)] - 0.5) < 0.02
            && resting[at(0.25)] == 1.0,
        "the ramp should rest at 1, start from 0 at the key and stick at 1");
    const auto legacy = ramped(99);
    require(legacy[at(0.05)] == 0.0 && std::abs(legacy[at(0.15)] - 0.5) < 0.02,
        "a project before format 100 should start the ramp at 0");
}

// A high-pass filter at 150 hz feeding a low-pass filter at
// 1200 hz pass the band between them and attenuate either side.
void testCascadedFiltersPassABand()
{
    const auto passed = [](double frequency) {
        const auto played = render(heard({oscillator("tone", std::to_string(frequency) + " hz"),
                                             node("above150", "talas.filter",
                                                 {choice("type", "highPass"), expression("frequency", "150 hz")}),
                                             node("below1200", "talas.filter",
                                                 {choice("type", "lowPass"), expression("frequency", "1200 hz")})},
                                       {connection("tone", "audioOut", "above150", "audioIn"),
                                           connection("above150", "audioOut", "below1200", "audioIn")},
                                       "below1200"),
            at(0.5));
        return toneLevel(std::span{played}.subspan(at(0.3), at(0.2)), frequency);
    };
    // Each of a Filter's four poles is down 3 dB at its cutoff, so the band
    // passes at about -4 dB between the two (the tutorial's "unattenuated"
    // is not checked), and either side falls away.
    require(passed(450.0) > 0.6 && passed(600.0) > 0.55,
        "frequencies between 150 and 1200 hz should pass through");
    require(passed(40.0) < 0.01 && passed(6000.0) < 0.01,
        "the high-pass should block below 150 hz and the low-pass above 1200 hz");
}

// Together the two filters act as a bandpass, letting 150 to
// 1200 hz through unattenuated. A Filter's Order one-pole sections, each
// 3 dB down at its cutoff, cannot: the
// band's middle passes about 4 dB down and its edges 12 dB. The claim
// needs the Filters' Response butterworth, Order poles flat in their
// passband. Taking unattenuated within a stated tolerance: no gain
// anywhere, within 3 dB at the band's edges (each a cutoff, the half-power
// point), within 1 dB from 180 to 1000 hz and within 0.05 dB from 300 to
// 600 hz, while an octave outside the band is 20 dB down and 40 hz and
// 6 khz 40 dB.
void testButterworthFiltersPassTheBandUnattenuated()
{
    const auto decibels = [](double frequency, const std::string& response) {
        const auto tone = oscillator("tone", std::to_string(frequency) + " hz");
        const auto window = [&](const std::vector<double>& played) {
            return toneLevel(std::span{played}.subspan(at(0.3), at(0.2)), frequency);
        };
        const auto alone = window(render(heard({tone}, {}, "tone"), at(0.5)));
        const auto filtered = window(render(heard({tone,
                                                      node("above150", "talas.filter",
                                                          {choice("type", "highPass"), expression("frequency", "150 hz"),
                                                              choice("response", response)}),
                                                      node("below1200", "talas.filter",
                                                          {choice("type", "lowPass"), expression("frequency", "1200 hz"),
                                                              choice("response", response)})},
                                                {connection("tone", "audioOut", "above150", "audioIn"),
                                                    connection("above150", "audioOut", "below1200", "audioIn")},
                                                "below1200"),
            at(0.5)));
        return 20.0 * std::log10(filtered / alone);
    };
    // Every frequency here fills the 0.2 s window with whole cycles.
    for (const auto frequency : {150.0, 180.0, 200.0, 250.0, 300.0, 425.0, 600.0, 800.0, 900.0, 1000.0, 1200.0})
    {
        const auto passed = decibels(frequency, "butterworth");
        const auto tolerance = frequency >= 300.0 && frequency <= 600.0 ? 0.05
            : frequency >= 180.0 && frequency <= 1000.0                 ? 1.0
                                                                        : 3.02;
        require(passed <= 0.01 && passed >= -tolerance,
            "butterworth filters should pass " + std::to_string(frequency) + " hz within "
                + std::to_string(tolerance) + " dB, not " + std::to_string(passed) + " dB");
    }
    require(decibels(75.0, "butterworth") < -20.0 && decibels(2400.0, "butterworth") < -20.0
            && decibels(40.0, "butterworth") < -40.0 && decibels(6000.0, "butterworth") < -40.0,
        "butterworth filters should block below 150 hz and above 1200 hz");
    // The Reference's sections, the default, lose the band's middle.
    require(decibels(425.0, "onePoleSections") < -3.5,
        "Order one-pole sections should pass the band's middle about 4 dB down");
}

int failures = 0;

void run(void (*test)(), const char* name)
{
    try
    {
        test();
    }
    catch (const std::exception& error)
    {
        ++failures;
        std::cerr << name << " failed: " << error.what() << '\n';
    }
}
} // namespace

int main()
{
    run(testSoundsChainAndTheirParametersVary, "testSoundsChainAndTheirParametersVary");
    run(testChorusingVariesTheDelay, "testChorusingVariesTheDelay");
    run(testControllerRangeBecomesAFrequencyRange, "testControllerRangeBecomesAFrequencyRange");
    run(testFaderCoversAnOctaveFromMiddleC, "testFaderCoversAnOctaveFromMiddleC");
    run(testPitchOffsetByAController, "testPitchOffsetByAController");
    run(testEnvelopeBendsThePitchByAnOctave, "testEnvelopeBendsThePitchByAnOctave");
    run(testEnvelopeScalesAFrequencyRange, "testEnvelopeScalesAFrequencyRange");
    run(testTheFilterOpensWhileAKeyIsDown, "testTheFilterOpensWhileAKeyIsDown");
    run(testAScriptShiftsTheLiveInput, "testAScriptShiftsTheLiveInput");
    run(testAPastedRampSpansThirtyToThreeHundredHertz, "testAPastedRampSpansThirtyToThreeHundredHertz");
    run(testTimeLeftAndTriggerFeedsItselfBack, "testTimeLeftAndTriggerFeedsItselfBack");
    run(testEitherOrByAGate, "testEitherOrByAGate");
    run(testRangeConversions, "testRangeConversions");
    run(testInversesConvertPeriodsAndFrequencies, "testInversesConvertPeriodsAndFrequencies");
    run(testPitchRisesWithLocalTime, "testPitchRisesWithLocalTime");
    run(testRepeatingRampGlidesAnOctave, "testRepeatingRampGlidesAnOctave");
    run(testBpmTriggersRestartARecording, "testBpmTriggersRestartARecording");
    run(testKeyPressesChooseRandomPitches, "testKeyPressesChooseRandomPitches");
    run(testRandomIntervalEveryEighthOfASecond, "testRandomIntervalEveryEighthOfASecond");
    run(testRandomIntervalsDoNotRetriggerTheSample, "testRandomIntervalsDoNotRetriggerTheSample");
    run(testRandomValuesAndSmoothing, "testRandomValuesAndSmoothing");
    run(testOscillatorPastedAsAPitchDeviation, "testOscillatorPastedAsAPitchDeviation");
    run(testPastedLfoModulationSetsTheDeviation, "testPastedLfoModulationSetsTheDeviation");
    run(testKeyNumberProcessedAsASignal, "testKeyNumberProcessedAsASignal");
    run(testLowPassFilteredNoteNumbersGlide, "testLowPassFilteredNoteNumbersGlide");
    run(testOneConstantSharedByTwoFields, "testOneConstantSharedByTwoFields");
    run(testKeyDownRampBendsThePitch, "testKeyDownRampBendsThePitch");
    run(testScriptTranscribesNotesByHand, "testScriptTranscribesNotesByHand");
    run(testSelfSimilarEventSequencePlays, "testSelfSimilarEventSequencePlays");
    run(testScriptDrumMachineByModuloArithmetic, "testScriptDrumMachineByModuloArithmetic");
    run(testAmplitudeModulationGivesSumsAndDifferences, "testAmplitudeModulationGivesSumsAndDifferences");
    run(testDelayScaleTakesAShareOfTheDelay, "testDelayScaleTakesAShareOfTheDelay");
    run(testShrinkingAndGrowingDelayShiftsThePitch, "testShrinkingAndGrowingDelayShiftsThePitch");
    run(testFrequencyMorphsBeforeAmplitude, "testFrequencyMorphsBeforeAmplitude");
    run(testQuestionMarkVariablesInFields, "testQuestionMarkVariablesInFields");
    run(testOverlappingNotesOnOneKeyEndOnTheirOwn, "testOverlappingNotesOnOneKeyEndOnTheirOwn");
    run(testTimingClockStepsASequencerEachBeat, "testTimingClockStepsASequencerEachBeat");
    run(testTuningExamplePlaysEachKeysFrequency, "testTuningExamplePlaysEachKeysFrequency");
    run(testTheKeyboardTriesOutTheTuning, "testTheKeyboardTriesOutTheTuning");
    run(testTransformationFollowsEachNotesStart, "testTransformationFollowsEachNotesStart");
    run(testTransformingIndependentPartialsMakesThemInharmonic, "testTransformingIndependentPartialsMakesThemInharmonic");
    run(testTransformingParametersByName, "testTransformingParametersByName");
    run(testUnexpandedClassStaysHarmonic, "testUnexpandedClassStaysHarmonic");
    run(testScriptLoopsAndConditionalsStartTheirSounds, "testScriptLoopsAndConditionalsStartTheirSounds");
    run(testMidiFileInterpreterEventMessages, "testMidiFileInterpreterEventMessages");
    run(testTextFileInterpreterMessages, "testTextFileInterpreterMessages");
    run(testPitchComparisonsChooseByThePitch, "testPitchComparisonsChooseByThePitch");
    run(testScriptEventsScheduleTheirInstruments, "testScriptEventsScheduleTheirInstruments");
    run(testDefaultFrequencyArithmetic, "testDefaultFrequencyArithmetic");
    run(testScriptPlacesItsInputsOnATimeline, "testScriptPlacesItsInputsOnATimeline");
    run(testScriptEventSetsFrequencyAndAmplitude, "testScriptEventSetsFrequencyAndAmplitude");
    run(testWholeToneScaleScript, "testWholeToneScaleScript");
    run(testScriptEventsKeepEarlierValues, "testScriptEventsKeepEarlierValues");
    run(testKeyboardsInOtherSteps, "testKeyboardsInOtherSteps");
    run(testDelayedCopyReinforcesOrCancels, "testDelayedCopyReinforcesOrCancels");
    run(testCascadedFiltersPassABand, "testCascadedFiltersPassABand");
    run(testButterworthFiltersPassTheBandUnattenuated, "testButterworthFiltersPassTheBandUnattenuated");
    run(testCompressionRatioIsInDecibels, "testCompressionRatioIsInDecibels");
    run(testCompressorGainRaisesWithoutClipping, "testCompressorGainRaisesWithoutClipping");
    run(testHighToLowRatioShapesNoise, "testHighToLowRatioShapesNoise");
    run(testFastAttackSlowReleaseEnvelope, "testFastAttackSlowReleaseEnvelope");
    run(testSymbolicHarmonicsFollowTheirFundamental, "testSymbolicHarmonicsFollowTheirFundamental");
    run(testClassFromExampleVariables, "testClassFromExampleVariables");
    run(testExtractedSoundTakesTheExampleVariables, "testExtractedSoundTakesTheExampleVariables");
    run(testRampRestsAtOneUntilItsTrigger, "testRampRestsAtOneUntilItsTrigger");
    run(testNegativeFrequencyReadsTheWavetableBackwards, "testNegativeFrequencyReadsTheWavetableBackwards");
    run(testRollingRampMakesCircularPitches, "testRollingRampMakesCircularPitches");
    if (failures != 0)
        return 1;
    std::cout << "Manual scenario tests passed\n";
    return 0;
}
