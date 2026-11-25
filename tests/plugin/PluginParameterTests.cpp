#include "talas/core/BuiltinNodes.h"
#include "talas/plugin/PluginParameters.h"
#include "talas/plugin/PluginRuntime.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace
{
using namespace talas::plugin;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] bool near(double left, double right, double tolerance = 1e-9)
{
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] talas::project::ProjectDocument loadDemo(const talas::core::NodeRegistry& registry, std::string_view name)
{
    const auto directory = fs::path{TALAS_SOURCE_DIR} / "examples" / "demos";
    std::ifstream stream(directory / (std::string{name} + ".talas.json"), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    const auto load = parsePluginProject(text, registry, directory / (std::string{name} + ".talas.json"));
    require(load.ok(), "the demo " + std::string{name} + " loads: " + load.error);
    return *load.document;
}

[[nodiscard]] const PluginParameterSpec& find(const PluginParameterMapping& mapping, std::string_view name)
{
    const auto found = std::ranges::find(mapping.parameters, name, &PluginParameterSpec::eventName);
    require(found != mapping.parameters.end(), "the mapping has !" + std::string{name});
    return *found;
}

void testDemoParameters(const talas::core::NodeRegistry& registry)
{
    const auto delay = mapProjectParameters(loadDemo(registry, "stereo_delay"), registry, PluginKind::Effect);
    std::vector<std::string> names;
    for (const auto& spec : delay.parameters)
        names.push_back(spec.eventName);
    require(names == std::vector<std::string>{"Time", "Feedback", "Spread", "Tone", "Mix"},
        "the Virtual Controls become parameters in their order");
    const auto& time = find(delay, "Time");
    require(time.displayName == "Time" && time.unit == "s", "a label's (unit) becomes the parameter's unit");
    require(time.kind == PluginParameterKind::Continuous && time.logarithmic, "a log-tapered fader stays log");
    require(near(time.minimum, 0.05) && near(time.maximum, 1.5) && near(time.defaultValue, 0.375),
        "the control's range and value become the parameter's");
    require(time.declared, "a Virtual Control is a declared parameter");
    const auto& spread = find(delay, "Spread");
    require(spread.displayName == "Spread (R/L ratio)" && spread.unit.empty(),
        "a parenthesis that is not a unit stays in the name");
    require(find(delay, "Tone").unit == "Hz" && !find(delay, "Feedback").logarithmic, "units and linear tapers");
    require(delay.warnings.empty(), "the demo maps without warnings");

    const auto bass = mapProjectParameters(loadDemo(registry, "sub_bass"), registry, PluginKind::Instrument);
    const auto& demo = find(bass, "Demo");
    require(demo.kind == PluginParameterKind::Toggle && near(demo.defaultValue, 1.0), "a toggle is a switch");
    require(std::ranges::none_of(bass.parameters, [](const auto& spec) { return spec.eventName == "KeyPitch"; }),
        "the keyboard's Event Values are not parameters");
}

void testDiscoveredAndMidiParameters(const talas::core::NodeRegistry& registry)
{
    auto instrument = *parsePluginProject(defaultPluginProjectJson(PluginKind::Instrument), registry, {}).document;
    const auto declared = mapProjectParameters(instrument, registry, PluginKind::Instrument);
    require(declared.parameters.size() == 2 && declared.parameters[0].eventName == "Level"
            && declared.parameters[1].eventName == "Release",
        "the raw instrument's parameters are Level and Release");
    instrument.virtualControls.clear();
    const auto discovered = mapProjectParameters(instrument, registry, PluginKind::Instrument);
    require(discovered.parameters.size() == 2, "Event Values without controls are discovered");
    require(std::ranges::none_of(discovered.parameters, [](const auto& spec) { return spec.declared; }),
        "and marked as inferred");

    auto withMidi = *parsePluginProject(defaultPluginProjectJson(PluginKind::Instrument), registry, {}).document;
    auto control = withMidi.virtualControls.front();
    control.eventName = "cc07";
    withMidi.virtualControls.push_back(control);
    control.eventName = "MIDIPitchBend";
    withMidi.virtualControls.push_back(control);
    const auto instrumentMapping = mapProjectParameters(withMidi, registry, PluginKind::Instrument);
    require(instrumentMapping.parameters.size() == 2, "an instrument's host sets MIDI-driven names through MIDI");
    const auto effectMapping = mapProjectParameters(withMidi, registry, PluginKind::Effect);
    require(effectMapping.parameters.size() == 4, "an effect, which receives no MIDI, shows them as parameters");

    auto broken = withMidi;
    broken.virtualControls.front().minimum = 1.0;
    broken.virtualControls.front().maximum = 1.0;
    const auto brokenMapping = mapProjectParameters(broken, registry, PluginKind::Instrument);
    require(brokenMapping.parameters.size() == 1 && brokenMapping.warnings.size() == 1,
        "a fader with no range is left out with a warning");

    auto many = instrument;
    for (std::size_t index = 0; index < maximumPluginParameters + 5; ++index)
    {
        auto extra = withMidi.virtualControls.front();
        extra.eventName = "Extra" + std::to_string(index);
        many.virtualControls.push_back(extra);
    }
    const auto manyMapping = mapProjectParameters(many, registry, PluginKind::Effect);
    require(manyMapping.parameters.size() == maximumPluginParameters, "a plugin shows at most 128 parameters");
    require(!manyMapping.warnings.empty(), "and says which it left out");
}

void testNormalization()
{
    PluginParameterSpec linear;
    linear.minimum = -1.0;
    linear.maximum = 3.0;
    require(near(parameterValueFromNormalized(linear, 0.25), 0.0), "a linear fader maps evenly");
    require(near(parameterValueToNormalized(linear, 2.0), 0.75), "and back");
    require(near(parameterValueFromNormalized(linear, 2.0), 3.0) && near(parameterValueFromNormalized(linear, NAN), -1.0),
        "out-of-range and non-finite positions clamp");
    PluginParameterSpec log = linear;
    log.minimum = 20.0;
    log.maximum = 20000.0;
    log.logarithmic = true;
    require(near(parameterValueFromNormalized(log, 0.5), std::sqrt(20.0 * 20000.0), 1e-6),
        "a log fader's middle is the geometric mean");
    require(near(parameterValueToNormalized(log, 2000.0), 2.0 / 3.0, 1e-9), "and back");
    PluginParameterSpec stepped = linear;
    stepped.minimum = 0.0;
    stepped.maximum = 10.0;
    stepped.step = 2.5;
    require(near(parameterValueFromNormalized(stepped, 0.33), 2.5), "a grid snaps values");
    require(near(clampParameterValue(stepped, 9.0), 10.0), "clamping snaps too");
    PluginParameterSpec toggle = linear;
    toggle.kind = PluginParameterKind::Toggle;
    toggle.minimum = 0.0;
    toggle.maximum = 1.0;
    require(near(parameterValueFromNormalized(toggle, 0.49), 0.0) && near(parameterValueFromNormalized(toggle, 0.5), 1.0),
        "a toggle is off below half way and on from there");
    require(near(parameterValueToNormalized(toggle, 1.0), 1.0) && near(parameterValueToNormalized(toggle, 0.2), 0.0),
        "and normalizes to its ends");
}

void testProjectShape(const talas::core::NodeRegistry& registry)
{
    const auto delayInputs = projectAudioInputChannels(loadDemo(registry, "stereo_delay"));
    require(delayInputs[0] && std::none_of(delayInputs.begin() + 1, delayInputs.end(), [](bool on) { return on; }),
        "the demo effects read input channel 1 alone");
    const auto passThrough =
        projectAudioInputChannels(*parsePluginProject(defaultPluginProjectJson(PluginKind::Effect), registry, {}).document);
    require(passThrough[0] && passThrough[1] && !passThrough[2], "the raw effect reads channels 1 and 2");
    const auto bass = loadDemo(registry, "sub_bass");
    require(std::ranges::none_of(projectAudioInputChannels(bass), [](bool on) { return on; }),
        "an instrument reads no input");
    require(projectPlaysLiveMidi(bass), "the demo instruments play live MIDI");
    require(!projectPlaysLiveMidi(loadDemo(registry, "stereo_delay")), "the demo effects do not");
}

void testState()
{
    PluginState state;
    state.pluginName = "Sub \"Bass\"";
    state.projectHash = "0123456789abcdef";
    state.parameters = {{"Cutoff", 260.5}, {"Demo", 0.0}, {"Tiny", 1.0e-12}, {"Name with space", -3.25}};
    const auto text = serializePluginState(state);
    std::string error;
    const auto parsed = parsePluginState(text, &error);
    require(parsed.has_value() && *parsed == state, "the plugin state round-trips exactly: " + error);
    require(!parsePluginState("{}").has_value() && !parsePluginState("garbage").has_value()
            && !parsePluginState("{\"format\":\"talas.plugin-state\",\"version\":9,\"parameters\":{}}").has_value(),
        "a foreign or newer state is refused");
    const auto partial = parsePluginState(
        R"({"format":"talas.plugin-state","version":1,"parameters":{"A":1,"B":"x","C":true}})");
    require(partial.has_value() && partial->parameters.size() == 1 && partial->parameters.front().first == "A",
        "only numeric values are restored");
}

// The plugin's clock: each render advances it by the part's length, and an
// event `offset` frames into a part arrives at the previous part's time plus
// offset frames, which the bridge maps to exactly that frame. A note's onset
// so moves frame for frame with its offset, whatever the block size (the
// raw instrument's MIDI Voice and envelope add the same few milliseconds to
// every note).
[[nodiscard]] std::size_t firstSoundFrame(
    const talas::core::NodeRegistry& registry,
    std::size_t block,
    std::size_t noteBlock,
    std::size_t offset)
{
    constexpr double sampleRate = 48000.0;
    auto instrument = parsePluginProject(defaultPluginProjectJson(PluginKind::Instrument), registry, {});
    talas::runtime::ProjectRuntimeBridge bridge(*instrument.document, registry);
    PluginStageSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = block;
    const auto staged = stagePluginProject(bridge, registry, std::nullopt, settings);
    require(staged.ok(), "the raw instrument stages: " + staged.error);
    require(staged.outputChannelCount == 2, "in stereo");
    bridge.setPlaying(true);
    std::vector<float> left(block), right(block);
    float* outputs[2] = {left.data(), right.data()};
    double clock = 1.0;
    std::size_t first = 0;
    bool sounded = false;
    for (std::size_t part = 0; part < noteBlock + 16 && !sounded; ++part)
    {
        if (part == noteBlock)
            require(bridge.enqueueMidiNoteOn(1, 60, 100, clock + static_cast<double>(offset) / sampleRate),
                "a note is queued");
        const auto next = clock + static_cast<double>(block) / sampleRate;
        require(bridge.render({}, next, outputs, 2, block), "the bridge renders");
        clock = next;
        require(std::ranges::all_of(left, [](float sample) { return std::isfinite(sample); }), "finitely");
        for (std::size_t frame = 0; frame < block && !sounded; ++frame)
        {
            if (std::abs(left[frame]) > 1.0e-7F)
            {
                require(part >= noteBlock, "nothing sounds before the note");
                require(std::abs(right[frame]) > 0.0F, "the note sounds in both channels");
                first = part * block + frame;
                sounded = true;
            }
        }
    }
    bridge.clearAfterAudioStopped();
    require(sounded, "the note sounds");
    return first;
}

// A Sound starts from the parameters' values, which win over the values its
// persisted controls saved: the demo phrase off from the start, with the
// document left as it is.
void testParameterValuesStartTheSound(const talas::core::NodeRegistry& registry)
{
    const auto document = loadDemo(registry, "sub_bass");
    const auto mapping = mapProjectParameters(document, registry, PluginKind::Instrument);
    PluginStageSettings settings;
    settings.maxBlockFrames = 512;
    const auto render = [&](double demo) {
        settings.eventValues.clear();
        for (const auto& spec : mapping.parameters)
            settings.eventValues.push_back({spec.eventName, spec.eventName == "Demo" ? demo : spec.defaultValue, spec.domain});
        talas::runtime::ProjectRuntimeBridge bridge(document, registry);
        const auto outcome = stagePluginProject(bridge, registry, std::nullopt, settings);
        require(outcome.ok(), "the demo stages: " + outcome.error);
        bridge.setPlaying(true);
        std::vector<float> left(512), right(512);
        float* outputs[2] = {left.data(), right.data()};
        double clock = 1.0;
        float loudest = 0.0F;
        for (int part = 0; part < 24; ++part)
        {
            clock += 512.0 / settings.sampleRate;
            require(bridge.render({}, clock, outputs, 2, 512), "the demo renders");
            for (const auto sample : left)
                loudest = std::max(loudest, std::abs(sample));
        }
        bridge.clearAfterAudioStopped();
        return loudest;
    };
    require(render(1.0) > 0.01F, "the demo phrase plays when its parameter is on");
    require(find(mapping, "Demo").defaultValue == 1.0
            && std::ranges::find(document.virtualControls, "Demo", &talas::project::VirtualControl::eventName)->value == 1.0,
        "the project saved the control on");
    require(render(0.0) == 0.0F, "and the Sound starts from the parameter's value, not the saved one");
}

void testStagingAndMidiTiming(const talas::core::NodeRegistry& registry)
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t block = 256;
    const auto reference = firstSoundFrame(registry, 256, 2, 0);
    const auto delay = reference - 2 * 256;
    require(delay < 1024, "a note sounds within a few milliseconds: " + std::to_string(delay) + " frames");
    for (const auto [size, noteBlock, offset] : {std::tuple{std::size_t{256}, std::size_t{2}, std::size_t{100}},
             std::tuple{std::size_t{128}, std::size_t{3}, std::size_t{37}},
             std::tuple{std::size_t{512}, std::size_t{1}, std::size_t{511}}})
    {
        const auto onset = firstSoundFrame(registry, size, noteBlock, offset);
        require(onset == noteBlock * size + offset + delay,
            "a note " + std::to_string(offset) + " frames into a " + std::to_string(size)
                + "-frame part sounds exactly that much later: " + std::to_string(onset));
    }

    std::vector<float> left(block), right(block);
    float* outputs[2] = {left.data(), right.data()};
    double clock = 1.0;
    PluginStageSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = block;
    auto effect = parsePluginProject(defaultPluginProjectJson(PluginKind::Effect), registry, {});
    talas::runtime::ProjectRuntimeBridge passThrough(*effect.document, registry);
    const auto effectStaged = stagePluginProject(passThrough, registry, std::nullopt, settings);
    require(effectStaged.ok(), "the raw effect stages: " + effectStaged.error);
    passThrough.setPlaying(true);
    std::vector<float> inLeft(block), inRight(block);
    for (std::size_t frame = 0; frame < block; ++frame)
    {
        inLeft[frame] = static_cast<float>(std::sin(0.01 * static_cast<double>(frame)));
        inRight[frame] = static_cast<float>(0.5 * std::cos(0.02 * static_cast<double>(frame)));
    }
    const float* inputs[2] = {inLeft.data(), inRight.data()};
    clock = 1.0;
    for (int part = 0; part < 3; ++part)
    {
        const auto next = clock + static_cast<double>(block) / sampleRate;
        require(passThrough.render({inputs, 2}, next, outputs, 2, block), "the effect renders");
        clock = next;
    }
    for (std::size_t frame = 0; frame < block; ++frame)
    {
        require(near(left[frame], inLeft[frame], 1e-6) && near(right[frame], inRight[frame], 1e-6),
            "the raw effect passes its stereo input through unchanged");
    }
    passThrough.clearAfterAudioStopped();
    require(pluginPreparedBlockFrames(8) == 32 && pluginPreparedBlockFrames(512) == 512
            && pluginPreparedBlockFrames(1 << 20) == 2048,
        "host block sizes map to 32 to 2048 prepared frames");
}
}

int main()
{
    try
    {
        const auto registry = talas::core::makeBuiltinNodeRegistry();
        testDemoParameters(registry);
        testDiscoveredAndMidiParameters(registry);
        testNormalization();
        testProjectShape(registry);
        testState();
        testParameterValuesStartTheSound(registry);
        testStagingAndMidiTiming(registry);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "talas_plugin_parameter_tests passed\n";
    return 0;
}
