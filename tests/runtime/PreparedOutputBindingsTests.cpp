#include "runtime/PreparedOutputBindings.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void testActiveOutputSelection()
{
    namespace binding = talas::runtime::prepared_output_bindings;
    using talas::core::PortType;
    const std::array<binding::Port, 4> declared{{{"left", PortType::Audio}, {"right", PortType::Audio},
        {"out1", PortType::Audio}, {"out2", PortType::Audio}}};
    const auto active = std::span{declared}.subspan(2);
    talas::runtime::CompileStep step;
    step.nodeId = "layout";
    step.outputs = {{"left", 0}, {"right", 1}, {"out1", 2}, {"out2", 3}};
    talas::runtime::RuntimePlan plan;
    plan.buffers = {{"layout", "left", PortType::Audio, 8}, {"layout", "right", PortType::Audio, 9},
        {"layout", "out1", PortType::Audio, 10}, {"layout", "out2", PortType::Audio, 11}};
    const std::array<std::size_t, 2> actual{2, 3};
    auto selected = binding::Outputs::prepareActive(plan, step, declared, active, actual, 8);
    require(selected && selected->indices().size() == 2 && selected->contains(2) && selected->contains(3)
        && !selected->contains(0) && !selected->contains(1),
        "declared inactive channels must never be published as active producers");
    for (int mutation = 0; mutation < 13; ++mutation)
    {
        auto changed = plan;
        auto changedStep = step;
        auto changedDeclared = declared;
        std::array<binding::Port, 2> changedActive{active[0], active[1]};
        auto changedActual = actual;
        if (mutation == 0) changed.buffers[0].frames = 7;
        if (mutation == 1) changed.buffers[0].nodeId = "foreign";
        if (mutation == 2) std::swap(changedStep.outputs[0], changedStep.outputs[1]);
        if (mutation == 3) changedActual[1] = changedActual[0];
        if (mutation == 4) changedActual[0] = 0;
        if (mutation == 5) changedDeclared[1] = changedDeclared[0];
        if (mutation == 6) changedStep.outputs.pop_back();
        if (mutation == 7) changed.latencyCompensations.push_back({0, 0, 2, 0, 1});
        if (mutation == 8)
        {
            changed.buffers.push_back(changed.buffers[2]);
            changed.latencyCompensations.push_back({0, 0, 2, 4, 1});
            changedActual[0] = 4;
        }
        if (mutation == 9) changedActive[0].id = "unknown";
        if (mutation == 10) changedActive[0].type = PortType::Control;
        if (mutation == 11) changed.buffers[2].scopeId = 1;
        if (mutation == 12) changedStep.outputs[3].bufferIndex = 999;
        require(!binding::Outputs::prepareActive(changed, changedStep, changedDeclared, changedActive, changedActual, 8),
            "active and inactive compiler declarations must retain order, ownership, type and capacity authority");
    }
    require(!binding::Outputs::prepareActive(plan, step, declared, active, std::span{actual}.first(1), 8),
        "every active port must bind one distinct actual output");
    step.outputs.clear();
    plan.buffers.erase(plan.buffers.begin(), plan.buffers.begin() + 2);
    const std::array<std::size_t, 2> legacyActual{0, 1};
    auto legacy = binding::Outputs::prepareActive(plan, step, declared, active, legacyActual, 8);
    require(legacy && legacy->indices().size() == 2,
        "legacy graphs may omit inactive buffers and retain different larger capacities");
    require(selected->contains(2) && !selected->contains(0),
        "prepared active ownership must survive later public plan changes");
}

void testCompiledGraphs()
{
    namespace runtime = talas::runtime;
    for (const auto* consumer : {"talas.absoluteValue", "talas.arcTan", "talas.runningMin", "talas.runningMax"})
    {
        talas::project::ProjectDocument document;
        document.graph.nodes = {{"left", "talas.constant", {{"value", "3"}}},
            {"right", "talas.constant", {{"value", "4"}}},
            {"affine", "talas.scaleAndOffset", {{"scale", "2"}, {"offset", "-1"},
                {"rightScale", "-0.5"}, {"rightOffset", "1"}}},
            {"leftMath", "talas.absoluteValue", {}}, {"rightMath", consumer, {}},
            {"sum", "talas.add", {}}, {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
            {"vca", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"left", "value", "affine", "value"},
            {"right", "value", "affine", "rightValue"},
            {"affine", "value", "leftMath", "value"}, {"affine", "rightValue", "rightMath", "value"},
            {"leftMath", "value", "sum", "left"}, {"rightMath", "value", "sum", "right"},
            {"sum", "value", "vca", "gain"}, {"carrier", "audioOut", "vca", "audioIn"},
            {"vca", "audioOut", "out", "audioIn"}};
        for (bool customSubset : {false, true})
        {
            auto registry = talas::core::makeBuiltinNodeRegistry();
            if (customSubset) registry.setNodeTypePreparedRealtimeSupport("talas.scaleAndOffset",
                talas::core::ExecutionSupport::Subset, talas::core::ExecutionBinding::LegacyAdapter);
            const auto compiled = runtime::compileProject(document, registry, {16.0, 4});
            require(compiled.ok(), "compiler output metadata must preserve builtin and real custom Subset graph admission");
            const auto& plan = compiled.graph->plan();
            for (const auto& step : plan.steps)
            {
                const auto* descriptor = registry.findNodeType(step.typeId);
                require(descriptor && step.outputs.size() == descriptor->outputs.size(),
                    "compiler must enumerate every original registry output, including both affine lanes");
                for (std::size_t index = 0; index < step.outputs.size(); ++index)
                {
                    const auto& output = step.outputs[index];
                    require(output.outputPortId == descriptor->outputs[index].id && output.bufferIndex < plan.buffers.size(),
                        "compiled output records must retain registry port order and valid indices");
                    const auto& buffer = plan.buffers[output.bufferIndex];
                    require(buffer.nodeId == step.nodeId && buffer.scopeId == step.scopeId && buffer.portId == output.outputPortId,
                        "compiled output indices must identify each step's original scoped buffer");
                }
            }
            runtime::OfflineRenderSession offline(*compiled.graph);
            runtime::RealtimeRenderSession realtime(*compiled.graph);
            require(realtime.ready(), "both affine output producers must be recognized after metadata preparation");
            const auto contribution = std::string_view(consumer) == "talas.absoluteValue" ? 1.0
                : std::string_view(consumer) == "talas.arcTan" ? -0.75 : -1.0;
            std::size_t offset = 0;
            for (std::size_t frames : {1, 3, 2, 2})
            {
                const auto rendered = offline.render(frames);
                std::array<double, 4> actual{};
                require(rendered.ok() && realtime.render(std::span(actual).first(frames)),
                    "migrated output layouts must render through both adapters");
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    const auto expected = (5 + contribution) * std::sin(2 * std::numbers::pi_v<double> * (offset + frame) / 16);
                    require(std::abs(rendered.mono[frame] - expected) < 1e-12 && std::abs(actual[frame] - expected) < 1e-12,
                        "both affine lanes and downstream families must match an independent PCM oracle");
                }
                offset += frames;
            }
            for (const auto* nodeId : {"affine", "leftMath", "rightMath"})
            {
                for (int mutation = 0; mutation < 5; ++mutation)
                {
                    auto forged = *compiled.graph;
                    auto& changedPlan = const_cast<runtime::RuntimePlan&>(forged.plan());
                    auto& step = *std::ranges::find(changedPlan.steps, std::string_view{nodeId}, &runtime::CompileStep::nodeId);
                    if (mutation == 0) step.outputs.clear();
                    if (mutation == 1) step.outputs.front().outputPortId = "forged";
                    if (mutation == 2) step.outputs.front().bufferIndex = plan.buffers.size();
                    if (mutation == 3) step.outputs.push_back(step.outputs.front());
                    if (mutation == 4) step.outputs.front().bufferIndex = plan.steps.front().outputs.front().bufferIndex;
                    runtime::RealtimeRenderSession rejected(forged);
                    require(!rejected.ready() && !runtime::renderOffline(forged, 4).ok(),
                        "mutation or omission of compiler-sealed output records must reject on both execution paths");
                }
            }
        }
    }
}

void testCompiledCompensationOwnership()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypeProcessingContract("talas.delayWithFeedback", talas::core::NodeStateModel::SessionStateful,
        true, {talas::core::LatencyKind::Fixed, 3});
    registry.setNodeTypePreparedRealtimeSupport("talas.delayWithFeedback", talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeProcessingContract("talas.mixer", talas::core::NodeStateModel::Stateless,
        false, {talas::core::LatencyKind::None, 0});
    registry.setNodeTypeFanInLatencyPolicy("talas.mixer", talas::core::FanInLatencyPolicy::Align);
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"source", "talas.oscillator", {{"frequency", "100 hz"}, {"amplitude", "1"}}},
        {"delay", "talas.delayWithFeedback", {{"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"}}},
        {"mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "delay", "audioIn"},
        {"source", "audioOut", "mix", "in1"}, {"delay", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"}};
    const auto compiled = talas::runtime::compileProject(document, registry, {1000.0, 16});
    require(compiled.ok() && compiled.graph->plan().latencyCompensations.size() == 1,
        "unequal compiled audio paths must create a real compensation buffer");
    const auto& plan = compiled.graph->plan();
    const auto& compensation = plan.latencyCompensations.front();
    require(plan.buffers[compensation.sourceBufferIndex] == plan.buffers[compensation.outputBufferIndex],
        "compensation fixture must distinguish producers with otherwise identical buffer descriptions");
    std::size_t sourceOwners = 0;
    for (const auto& step : plan.steps)
        for (const auto& output : step.outputs)
        {
            sourceOwners += output.bufferIndex == compensation.sourceBufferIndex;
            require(output.bufferIndex != compensation.outputBufferIndex,
                "compiler output enumeration must exclude synthetic compensation producers");
        }
    require(sourceOwners == 1, "only the original producer owns the source's compiled output binding");
}

void testSecondaryOutputConsumers()
{
    using Parameter = talas::project::ProjectParameterExpression;
    const auto text = [](std::string id, std::string value) {
        Parameter p; p.parameterId = std::move(id); p.text = std::move(value); return p;
    };
    const auto boolean = [](std::string id, bool value) {
        Parameter p; p.parameterId = std::move(id); p.boolean = value; return p;
    };
    for (std::string_view type : {"talas.channelCrosser", "talas.highShelvingFilter", "talas.twoFormantElement", "talas.analogSequencer"})
    {
        std::vector<Parameter> parameters;
        std::string control;
        std::string output = "audioOut";
        if (type == "talas.channelCrosser") { control = "leftInLeft"; output = "leftOut"; }
        if (type == "talas.highShelvingFilter")
        {
            control = "scale";
            parameters = {{"cutoffFreq", "1000 hz"}, {"boostOrCut", "1"}, {"scale", "1"}};
        }
        if (type == "talas.twoFormantElement")
        {
            control = "scale1";
            parameters = {{"formant1", "1000 hz"}, {"bandwidth1", "200 hz"}, {"scale1", "1"},
                {"formant2", "2000 hz"}, {"bandwidth2", "300 hz"}, {"scale2", "1"}};
        }
        if (type == "talas.analogSequencer")
        {
            control = "left"; output = "leftOut";
            parameters = {{"left", "0.5"}, {"right", "0.25"}, {"gate", "1"}, {"step", "1"}, {"rate", "1"},
                boolean("loop", true), {"startIndex", "0"}, {"endIndex", "1"}, {"polyphony", "1"},
                text("durations", "{1 s} {1 s}"), text("dutyCycles", "{0.5} {1}"), text("pitches", ""),
                text("velocities", "{0.25} {1}"), text("extraValues", "")};
        }
        std::array<double, 32> expected{};
        for (bool right : {false, true})
        {
            talas::project::ProjectDocument document;
            document.graph.nodes = {{"source", "talas.constant", {{"value", "0.5"}}},
                {"affine", "talas.scaleAndOffset", {{"scale", "1"}, {"offset", "0"}, {"rightScale", "1"}, {"rightOffset", "0"}}},
                {"carrier", "talas.oscillator", {{"frequency", "100 hz"}, {"amplitude", "1"}}},
                {"consumer", std::string(type), parameters}, {"out", "talas.audioOutput", {}}};
            document.graph.connections = {{"source", "value", "affine", "value"},
                {"affine", right ? "rightValue" : "value", "consumer", control},
                {"carrier", "audioOut", "consumer", "audioIn"}, {"consumer", output, "out", "audioIn"}};
            const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {48000.0, 32});
            require(compiled.ok(), "secondary-output consumer graph must compile");
            const auto offline = talas::runtime::renderOffline(*compiled.graph, 32);
            talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
            if (!offline.ok() || !realtime.ready())
                throw std::runtime_error(std::string(type) + " must admit either affine lane: " + std::string(realtime.error()));
            std::array<double, 32> actual{};
            require(realtime.render(actual), "a recognized secondary control output must render");
            if (!right) expected = actual;
            require(std::ranges::any_of(actual, [](double value) { return std::abs(value) > 1e-8; }),
                "secondary-output regression must exercise audible processing");
            for (std::size_t frame = 0; frame < actual.size(); ++frame)
            {
                require(std::abs(actual[frame] - offline.mono[frame]) < 1e-12 && std::abs(actual[frame] - expected[frame]) < 1e-12,
                    "identical affine left/right controls must retain the existing audible DSP result");
                if (type == "talas.channelCrosser")
                    require(std::abs(actual[frame] - 0.5 * std::sin(2 * std::numbers::pi_v<double> * 100 * frame / 48000)) < 1e-12,
                        "channel crossing must match the independent gain/carrier oracle on either control port");
            }
        }
    }
}
}

int main()
{
    namespace binding = talas::runtime::prepared_output_bindings;
    using talas::core::PortType;
    talas::runtime::RuntimePlan plan;
    talas::runtime::CompileStep step;
    step.nodeId = "source";
    step.scopeId = 3;
    plan.buffers = {{"source", "left", PortType::Audio, 8, 3},
        {"source", "right", PortType::Control, 8, 3},
        {"source", "left", PortType::Audio, 8, 3}};
    plan.latencyCompensations.push_back({0, 0, 0, 2, 1});
    step.outputs = {{"left", 0}, {"right", 1}};
    const std::array<binding::Port, 2> ports{{{"left", PortType::Audio}, {"right", PortType::Control}}};
    const std::array<std::size_t, 2> actual{0, 1};
    auto outputs = binding::Outputs::prepare(plan, step, ports, actual, 8);
    require(outputs && outputs->contains(0) && outputs->contains(1) && !outputs->contains(2),
        "only original admitted output indices may become producers");
    step.outputs[0].bufferIndex = 2;
    const std::array<std::size_t, 2> delayed{2, 1};
    require(!binding::Outputs::prepare(plan, step, ports, delayed, 8),
        "same-named compensation buffer must never impersonate its source producer");
    step.outputs = {{"left", 0}, {"right", 1}};
    const auto validStep = step;
    for (int mutation = 0; mutation < 7; ++mutation)
    {
        step = validStep;
        if (mutation == 0) step.outputs.pop_back();
        if (mutation == 1) step.outputs.push_back(step.outputs.front());
        if (mutation == 2) step.outputs[1].bufferIndex = 0;
        if (mutation == 3) step.outputs[0].outputPortId = "other";
        if (mutation == 4) step.outputs[0].bufferIndex = 99;
        if (mutation == 5) std::swap(step.outputs[0], step.outputs[1]);
        if (mutation == 6) step.scopeId = 4;
        require(!binding::Outputs::prepare(plan, step, ports, actual, 8), "forged ordered output records must reject");
    }
    step = validStep;
    for (int mutation = 0; mutation < 4; ++mutation)
    {
        auto changed = plan;
        if (mutation == 0) changed.buffers[0].nodeId = "other";
        if (mutation == 1) changed.buffers[0].portId = "other";
        if (mutation == 2) changed.buffers[0].type = PortType::Control;
        if (mutation == 3) changed.buffers[0].frames = 7;
        require(!binding::Outputs::prepare(changed, step, ports, actual, 8), "buffer ownership/type/capacity must match actual binding");
    }
    step.outputs.clear();
    require(binding::Outputs::prepare(plan, step, ports, actual, 8).has_value(),
        "legacy adapters without compiler output records retain validation of actual bound buffers");
    step = validStep;
    step.outputs[0].bufferIndex = 2;
    require(outputs->contains(0) && !outputs->contains(2), "prepared metadata must own immutable indices independent of mutable public plans");
    require(!binding::Outputs::prepare(plan, step, ports, std::span(actual).first(1), 8), "binding count must be exact");
    testActiveOutputSelection();
    testCompiledGraphs();
    testCompiledCompensationOwnership();
    testSecondaryOutputConsumers();
    return 0;
}
