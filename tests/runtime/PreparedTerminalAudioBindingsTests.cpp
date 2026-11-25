#include "runtime/PreparedTerminalAudioBindings.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace
{
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void testBindingResolution()
{
    namespace terminal = talas::runtime::prepared_terminal_audio;
    using namespace talas::runtime;
    CompileStep step;
    step.typeId = "talas.audioOutput";
    step.inputs = {{"audioIn", "left", "audioOut", 0}};
    auto channels = terminal::resolve(step);
    require(channels && channels->size() == 1 && channels->front().inputIndex == 0
        && channels->front().bufferIndex == 0, "missing optional right input must produce exactly one channel");
    require(terminal::hasExactInputOrder(step, *channels), "mono physical input order must be exact");
    step.inputs.insert(step.inputs.begin(), {"rightIn", "right", "audioOut", 1});
    channels = terminal::resolve(step);
    require(channels && channels->size() == 2 && (*channels)[0].inputIndex == 1
        && (*channels)[1].inputIndex == 0 && (*channels)[0].portId == "audioIn",
        "channel declaration order must remain independent of input storage order");
    require(!terminal::hasExactInputOrder(step, *channels),
        "resolving canonical channels must not relax Audio Output's separate physical-order authority");
    step.inputs[1].bufferIndex = 9;
    require((*channels)[0].bufferIndex == 0, "resolved indices must own their values without borrowing mutable input bindings");
    step.inputs.erase(step.inputs.begin() + 1);
    require(!terminal::resolve(step), "right-only audio output must reject a missing required left input");
    step.inputs.push_back({"audioIn", "left", "audioOut", 0});
    step.inputs.push_back(step.inputs.back());
    require(!terminal::resolve(step), "duplicate declared audio inputs must not resolve by selecting the first match");

    step.typeId = "talas.stereoInOutput4";
    step.inputs = {{"out34", "right", "audioOut", 1}, {"out12", "left", "audioOut", 0}};
    channels = terminal::resolve(step);
    require(channels && channels->size() == 4 && (*channels)[0].bufferIndex == 0
        && (*channels)[1].bufferIndex == 0 && (*channels)[2].bufferIndex == 1
        && (*channels)[3].bufferIndex == 1 && (*channels)[0].inputIndex == (*channels)[1].inputIndex,
        "paired terminals must repeat logical channels, not duplicate physical producer storage");
    RuntimePlan plan;
    plan.buffers = {{"left", "audioOut", talas::core::PortType::Audio, 5},
        {"right", "audioOut", talas::core::PortType::Audio, 5}};
    plan.outputBufferIndices = {0, 0, 1, 1};
    plan.outputBufferIndex = 0;
    plan.rightOutputBufferIndex = 0;
    require(terminal::agreesWithCompiledRoutes(plan, *channels), "compiled repeated channels and legacy aliases must agree");
    for (int mutation = 0; mutation < 6; ++mutation)
    {
        auto changed = plan;
        if (mutation == 0) changed.outputBufferIndices.pop_back();
        if (mutation == 1) changed.outputBufferIndices[1] = 1;
        if (mutation == 2) changed.outputBufferIndex = 1;
        if (mutation == 3) changed.rightOutputBufferIndex.reset();
        if (mutation == 4) changed.buffers[0].type = talas::core::PortType::Control;
        if (mutation == 5) changed.buffers.clear();
        require(!terminal::agreesWithCompiledRoutes(changed, *channels), "inconsistent compiled channel routes must reject");
    }
    require(!terminal::agreesWithCompiledRoutes(plan, {}), "an empty resolution must never impersonate an audio terminal");
    step.typeId = "talas.diskRecorder";
    step.inputs = {{"captureDuration", "duration", "value", 2}, {"audioIn", "left", "audioOut", 0}};
    channels = terminal::resolve(step);
    require(channels && channels->size() == 1 && channels->front().inputIndex == 1,
        "Disk Recorder controls must not become terminal audio channels");
    // A stereo Disk Recorder records its Right input, or its Input
    // again when Right is unconnected.
    talas::runtime::ParameterValue layout;
    layout.id = "channels";
    layout.choice = "stereo";
    step.parameters = {layout};
    channels = terminal::resolve(step);
    require(channels && channels->size() == 2 && channels->front().inputIndex == 1
            && (*channels)[1].inputIndex == 1 && (*channels)[1].portId == "audioIn",
        "a stereo Disk Recorder should record a mono Input on both channels");
    step.inputs.push_back({"rightIn", "right", "audioOut", 1});
    channels = terminal::resolve(step);
    require(channels && channels->size() == 2 && (*channels)[1].inputIndex == 2
            && (*channels)[1].portId == "rightIn",
        "a stereo Disk Recorder should record its Right input on the right channel");
    step.parameters.clear();
    for (const auto* type : {"talas.midiOutputEvent", "talas.constant", "unknown"})
    {
        step.typeId = type;
        require(!terminal::resolve(step), "unknown and event-only sinks must not gain audio channels");
    }
}

struct Fixture
{
    const char* type;
    std::vector<std::string> ports;
    std::vector<std::size_t> channelSources;
};

void testCompiledTerminalGraphs()
{
    const std::array fixtures{
        Fixture{"talas.audioOutput", {"audioIn"}, {0}},
        Fixture{"talas.audioOutput", {"audioIn", "rightIn"}, {0, 1}},
        Fixture{"talas.output4", {"out1", "out2", "out3", "out4"}, {0, 1, 2, 3}},
        Fixture{"talas.output8", {"out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"}, {0, 1, 2, 3, 4, 5, 6, 7}},
        Fixture{"talas.stereoInOutput4", {"out12", "out34"}, {0, 0, 1, 1}},
        Fixture{"talas.stereoInOutput8", {"out12", "out34", "out56", "out78"}, {0, 0, 1, 1, 2, 2, 3, 3}}};
    for (const auto& fixture : fixtures)
    {
        talas::project::ProjectDocument document;
        document.graph.nodes = {{"out", fixture.type, {}}};
        for (std::size_t index = 0; index < fixture.ports.size(); ++index)
        {
            const auto id = "source" + std::to_string(index);
            document.graph.nodes.push_back({id, "talas.oscillator", {{"frequency", "1 hz"},
                {"amplitude", std::to_string(0.1 * (index + 1))}}});
            document.graph.connections.push_back({id, "audioOut", "out", fixture.ports[index]});
        }
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {32.0, 5});
        require(compiled.ok(), "every independent terminal fixture must compile");
        const auto& plan = compiled.graph->plan();
        const auto& step = *std::ranges::find(plan.steps, std::string_view{"out"}, &talas::runtime::CompileStep::nodeId);
        const auto bound = talas::runtime::prepared_terminal_audio::resolve(step);
        require(bound && talas::runtime::prepared_terminal_audio::agreesWithCompiledRoutes(plan, *bound),
            "compiler routes must agree with the common resolved terminal bindings");
        require(step.outputs.empty(), "terminal routing must never create original producer outputs");
        talas::runtime::OfflineRenderSession offline(*compiled.graph);
        talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
        require(realtime.ready() && realtime.outputChannelCount() == fixture.channelSources.size(),
            "every terminal must prepare the independently specified logical channel count");
        std::size_t position = 0;
        const auto render = [&](std::size_t frames) {
            const auto expected = offline.render(frames);
            std::vector<std::vector<double>> actual(fixture.channelSources.size(), std::vector<double>(frames, -99));
            std::vector<double*> pointers;
            for (auto& channel : actual) pointers.push_back(channel.data());
            require(expected.ok() && realtime.render({}, pointers.data(), pointers.size(), frames),
                "both terminal adapters must render every continuation partition");
            require(actual == expected.channels, "terminal adapters must produce identical channel samples");
            for (std::size_t channel = 0; channel < actual.size(); ++channel)
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    const auto oracle = 0.1 * (fixture.channelSources[channel] + 1)
                        * std::sin(2 * std::numbers::pi_v<double> * (position + frame) / 32);
                    require(std::abs(actual[channel][frame] - oracle) < 1e-12,
                        "terminal channel order and repeated pairs must match an independent sine/amplitude oracle");
                }
            position += frames;
        };
        for (std::size_t frames : {1, 5, 2, 3}) render(frames);
        realtime.resetTransport();
        talas::runtime::OfflineRenderSession fresh(*compiled.graph);
        offline = std::move(fresh);
        position = 0;
        render(5);
        for (int mutation = 0; mutation < 5; ++mutation)
        {
            auto forged = *compiled.graph;
            auto& changed = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
            auto& terminal = *std::ranges::find(changed.steps, std::string_view{"out"}, &talas::runtime::CompileStep::nodeId);
            if (mutation == 0) changed.outputBufferIndices.pop_back();
            if (mutation == 1) changed.outputBufferIndices.front() = changed.buffers.size();
            if (mutation == 2) changed.outputBufferIndex = changed.buffers.size();
            if (mutation == 3) changed.rightOutputBufferIndex = changed.buffers.size();
            if (mutation == 4) terminal.inputs.front().inputPortId = "forged";
            talas::runtime::RealtimeRenderSession rejected(forged);
            require(!rejected.ready() && !talas::runtime::renderOffline(forged, 5).ok(),
                "common channel metadata must not bypass compiler-sealed terminal authority");
        }
    }
}
}

int main()
{
    testBindingResolution();
    testCompiledTerminalGraphs();
}
