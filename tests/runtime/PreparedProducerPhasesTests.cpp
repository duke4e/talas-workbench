#include "runtime/PreparedOutputBindings.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace
{
std::atomic<bool> countAllocations{false};
std::atomic<std::size_t> allocations{0};
}
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

bool renderWithoutAllocation(talas::runtime::RealtimeRenderSession& session, std::span<double> output)
{
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render(output);
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocations.load(std::memory_order_relaxed) == 0,
        "phase processing and callback rollback must not allocate C++ storage");
    return rendered;
}

void testPhaseOwnership()
{
    using namespace talas::runtime;
    using Outputs = prepared_output_bindings::Outputs;
    RuntimePlan plan;
    plan.buffers = {{"source", "audioOut", talas::core::PortType::Audio, 5},
        {"source", "audioOut", talas::core::PortType::Audio, 5},
        {"target", "audioOut", talas::core::PortType::Audio, 5}};
    CompileStep source;
    source.nodeId = "source";
    source.outputs = {{"audioOut", 0}};
    CompileStep target;
    target.nodeId = "target";
    target.inputs = {{"audioIn", "source", "audioOut", 1}};
    target.outputs = {{"audioOut", 2}};
    plan.steps = {source, target};
    plan.latencyCompensations = {{1, 0, 0, 1, 3}};
    require(prepared_output_bindings::countOriginalOutputs(plan, source, 5) == 1,
        "validated compensation must not inflate original source output count");
    auto untracked = plan;
    untracked.buffers.push_back(plan.buffers[0]);
    require(prepared_output_bindings::countOriginalOutputs(untracked, source, 5) == 2,
        "an untracked original output must still be counted");
    auto scoped = plan;
    scoped.buffers.push_back(plan.buffers[0]);
    scoped.buffers.back().scopeId = 1;
    require(prepared_output_bindings::countOriginalOutputs(scoped, source, 5) == 1,
        "output counts must respect scope identity");
    auto compensation = Outputs::prepareCompensation(plan, 0, 1, 5);
    require(compensation && compensation->contains(1) && !compensation->contains(0)
        && !compensation->contains(2), "compensation must own only its synthetic slot, not source or target outputs");
    const std::array<prepared_output_bindings::Port, 1> ports{{{"audioOut", talas::core::PortType::Audio}}};
    const std::array<std::size_t, 1> original{0};
    auto producer = Outputs::prepare(plan, source, ports, original, 5);
    require(producer && producer->contains(0) && !producer->contains(1),
        "identical source and compensation descriptions must retain different producer ownership");
    require(!Outputs::prepareCompensation(plan, 1, 1, 5)
        && !Outputs::prepareCompensation(plan, 0, 0, 5), "missing record and wrong actual output slot must reject");
    for (int mutation = 0; mutation < 12; ++mutation)
    {
        auto changed = plan;
        auto& record = changed.latencyCompensations[0];
        if (mutation == 0) record.sourceBufferIndex = 1;
        if (mutation == 1) record.targetStepIndex = 9;
        if (mutation == 2) record.inputBindingIndex = 9;
        if (mutation == 3) changed.steps[1].inputs[0].bufferIndex = 0;
        if (mutation == 4) record.sourceBufferIndex = 9;
        if (mutation == 5) record.outputBufferIndex = 9;
        if (mutation == 6) changed.buffers[0].frames = 4;
        if (mutation == 7) changed.buffers[1].frames = 4;
        if (mutation == 8) changed.buffers[1].type = talas::core::PortType::Control;
        if (mutation == 9) changed.latencyCompensations.push_back(record);
        if (mutation == 10) changed.steps[0].outputs.push_back({"forged", 1});
        if (mutation == 11) record.delayFrames = 0;
        require(!Outputs::prepareCompensation(changed, 0, 1, 5),
            "inconsistent or ambiguous synthetic output ownership must reject");
        const auto count = prepared_output_bindings::countOriginalOutputs(changed, source, 5);
        require(mutation == 5 ? count == 2 : !count,
            "malformed compensation cannot hide an otherwise owned output");
    }
    auto largerCapacity = plan;
    largerCapacity.buffers[0].frames = 8;
    largerCapacity.buffers[1].frames = 9;
    require(prepared_output_bindings::countOriginalOutputs(largerCapacity, source, 5) == 1,
        "larger legacy capacities must preserve original ownership count");
    require(Outputs::prepareCompensation(largerCapacity, 0, 1, 5).has_value(),
        "legacy compensation buffers may have capacity larger than the callback limit");
    plan.latencyCompensations[0].outputBufferIndex = 2;
    require(compensation->contains(1) && !compensation->contains(2),
        "prepared synthetic ownership must be independent of later public plan mutations");
    CompileStep sink;
    sink.nodeId = "sink";
    const auto noOutputs = Outputs::prepare(plan, sink, {}, {}, 5);
    require(noOutputs && noOutputs->indices().empty() && !noOutputs->contains(0),
        "terminal or commit operations must never impersonate producer buffer zero");
    sink.outputs = {{"forged", 0}};
    require(!Outputs::prepare(plan, sink, {}, {}, 5), "non-producing phases must reject unexpected compiler outputs");
    plan.latencyCompensations[0].outputBufferIndex = 1;
    for (auto& step : plan.steps) step.outputs.clear();
    require(prepared_output_bindings::countOriginalOutputs(plan, source, 5) == 1,
        "legacy empty output records must preserve validated compensation exclusion");
    require(Outputs::prepareCompensation(plan, 0, 1, 5).has_value(),
        "legacy plans without original output records must retain valid synthetic bindings");
}

talas::project::ProjectParameterExpression connectionName()
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = "connection";
    result.text = "loop";
    return result;
}

void testCompiledPhases(bool feedback, bool rollback = false, bool cascade = false)
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"source", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
        {"out", "talas.audioOutput", {}}};
    if (feedback)
    {
        document.graph.nodes.push_back({"feedback", "talas.feedbackLoopOutput", {connectionName(), {"delay", "1 samples"}}});
        document.graph.nodes.push_back({"sink", "talas.feedbackLoopInput", {connectionName()}});
        document.graph.nodes.push_back({"mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "0.5"}}});
        document.graph.nodes.push_back({"post", "talas.vca", {{"gain", "1"}}});
        document.graph.connections = {{"source", "audioOut", "mix", "in1"},
            {"feedback", "audioOut", "mix", "in2"}, {"mix", "audioOut", "sink", "audioIn"},
            {"feedback", "audioOut", "post", "audioIn"}, {"post", "audioOut", "out", "audioIn"}};
    }
    else
    {
        registry.setNodeTypeProcessingContract("talas.delayWithFeedback", talas::core::NodeStateModel::SessionStateful,
            true, {talas::core::LatencyKind::Fixed, 3});
        registry.setNodeTypePreparedRealtimeSupport("talas.delayWithFeedback", talas::core::ExecutionSupport::Subset,
            talas::core::ExecutionBinding::LegacyAdapter);
        registry.setNodeTypeFanInLatencyPolicy("talas.mixer", talas::core::FanInLatencyPolicy::Align);
        document.graph.nodes.push_back({"delay", "talas.delayWithFeedback", {{"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"}}});
        document.graph.nodes.push_back({"mix", "talas.mixer", {{"gain1", "1"}, {"gain2", "1"}}});
        document.graph.connections = {{"source", "audioOut", "delay", "audioIn"},
            {"source", "audioOut", "mix", "in1"}, {"delay", "audioOut", "mix", "in2"},
            {"mix", "audioOut", "out", "audioIn"}};
    }
    if (cascade)
    {
        document.graph.nodes.push_back({"second", "talas.delayWithFeedback", {{"delay", "3 samples"}, {"feedback", "0"}, {"mix", "1"}}});
        document.graph.connections = {{"source", "audioOut", "delay", "audioIn"},
            {"delay", "audioOut", "mix", "in1"}, {"delay", "audioOut", "second", "audioIn"},
            {"second", "audioOut", "mix", "in2"}, {"mix", "audioOut", "out", "audioIn"}};
    }
    talas::runtime::RenderSettings settings{32.0, 5};
    if (rollback)
    {
        settings.eventValues = {{"Gain", 1.0, talas::expr::ValueDomain::Scalar}};
        document.graph.nodes.push_back({"post", "talas.vca", {}});
        document.graph.nodes.push_back({"gain", "talas.constant", {{"value", "!Gain"}}});
        document.graph.connections.back() = {"mix", "audioOut", "post", "audioIn"};
        document.graph.connections.push_back({"gain", "value", "post", "gain"});
        document.graph.connections.push_back({"post", "audioOut", "out", "audioIn"});
    }
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "phase ownership fixtures must compile");
    const auto& plan = compiled.graph->plan();
    require(feedback ? plan.feedbackPairs.size() == 1 : plan.latencyCompensations.size() == 1,
        "fixture must contain the actual compiler-created ownership phase");
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    if (!realtime.ready()) throw std::runtime_error(std::string(feedback ? "feedback: " : "compensation: ")
        + std::string(realtime.error()));
    std::size_t position = 0;
    double history = 0;
    const auto render = [&](std::size_t frames) {
        const auto expected = offline.render(frames);
        std::array<double, 5> actual{};
        require(expected.ok() && renderWithoutAllocation(realtime, std::span(actual).first(frames)), "both adapters must render phase continuation");
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto absolute = position + frame;
            const auto latency = cascade ? 6u : 3u;
            const auto oracle = feedback ? history : absolute < latency ? 0.0
                : 2 * std::sin(2 * std::numbers::pi_v<double> * (absolute - latency) / 32);
            if (feedback) history = std::sin(2 * std::numbers::pi_v<double> * absolute / 32) + 0.5 * history;
            require(std::abs(actual[frame] - oracle) < 1e-12 && std::abs(expected.mono[frame] - oracle) < 1e-12,
                "synthetic delay or feedback read-before-commit must match an independent PCM recurrence");
        }
        position += frames;
    };
    if (rollback)
    {
        render(5);
        if (cascade) render(3);
        require(realtime.updateEventValue("Gain", 1e308, talas::expr::ValueDomain::Scalar),
            "finite high gain must be admitted before a later sample overflows");
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            std::array<double, 5> failed;
            failed.fill(-99);
            require(!renderWithoutAllocation(realtime, failed)
                && std::ranges::all_of(failed, [](double value) { return value == 0; }),
                "late overflow must silence the callback and allow repeated rollback");
        }
        require(realtime.updateEventValue("Gain", 1.0, talas::expr::ValueDomain::Scalar),
            "corrected gain must be publishable after failure");
        render(5);
        render(2);
        return;
    }
    for (std::size_t frames : {1, 5, 2, 4}) render(frames);
    realtime.resetTransport();
    offline = talas::runtime::OfflineRenderSession(*compiled.graph);
    position = 0;
    history = 0;
    render(5);
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto forged = *compiled.graph;
        auto& changed = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        if (feedback)
        {
            auto& pair = changed.feedbackPairs[0];
            auto& reader = changed.steps[pair.outputStepIndex];
            if (mutation == 0) reader.outputs[0].bufferIndex = changed.buffers.size();
            if (mutation == 1) changed.steps[pair.inputStepIndex].outputs = {{"forged", pair.outputBufferIndex}};
            if (mutation == 2) pair.outputBufferIndex = changed.buffers.size();
        }
        else
        {
            auto& compensation = changed.latencyCompensations[0];
            if (mutation == 0) compensation.outputBufferIndex = compensation.sourceBufferIndex;
            if (mutation == 1) changed.latencyCompensations.push_back(compensation);
            if (mutation == 2) changed.steps[compensation.targetStepIndex].outputs.push_back({"forged", compensation.outputBufferIndex});
        }
        talas::runtime::RealtimeRenderSession rejected(forged);
        require(!rejected.ready() && !talas::runtime::renderOffline(forged, 5).ok(),
            "prepared ownership metadata must not authorize forged phase plans");
    }
}

void testActiveMultichannelConsumers(bool rollback = false)
{
    const auto choice = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    for (std::string_view family : {"matrix", "pan", "decoder", "quad"})
    for (std::size_t count : {2, 4, 8})
    {
        if (rollback && family != "quad") continue;
        if ((family != "pan" && count == 2) || (family == "quad" && count != 4)) continue;
        const bool hasInactive = family == "pan" || (family == "decoder" && count == 4);
        for (std::size_t channel = 0; channel < count + (hasInactive ? 1 : 0); ++channel)
        {
            if (rollback && channel != 0) continue;
            const bool inactive = channel == count;
            talas::project::ProjectDocument document;
            document.graph.nodes = {{"delay", "talas.delayWithFeedback", {{"delay", "1 samples"}, {"feedback", "0"}, {"mix", "1"}}},
                {"out", "talas.audioOutput", {}}};
            if (family != "quad") document.graph.nodes.push_back(
                {"source", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}});
            std::string port = "out" + std::to_string(channel + 1);
            double coefficient = 1;
            if (family == "matrix")
            {
                std::vector<talas::project::ProjectParameterExpression> parameters;
                for (std::size_t row = 0; row < count; ++row)
                {
                    talas::project::ProjectParameterExpression parameter;
                    parameter.parameterId = "insToOut" + std::to_string(row + 1);
                    std::string gains;
                    for (std::size_t column = 0; column < count; ++column)
                        gains += (column == 0 ? "" : " ") + std::to_string(row == column ? row + 1 : 0);
                    parameter.text = gains;
                    parameters.push_back(std::move(parameter));
                    document.graph.connections.push_back({"source", "audioOut", "producer", "in" + std::to_string(row + 1)});
                }
                document.graph.nodes.push_back({"producer", count == 4 ? "talas.matrix4" : "talas.matrix8", parameters});
                coefficient = static_cast<double>(channel + 1);
            }
            else if (family == "pan")
            {
                const auto position = count == 2 ? 0.2 : (static_cast<double>(channel % count) + 0.2) / count;
                document.graph.nodes.push_back({"producer", "talas.pan", {{"pan", std::to_string(position)}, {"scale", "1"},
                    choice("type", "power"), choice("layout", count == 2 ? "stereo" : count == 4 ? "discrete4" : "discrete8")}});
                document.graph.connections.push_back({"source", "audioOut", "producer", "audioIn"});
                if (count == 2) port = channel == 0 ? "leftOut" : "rightOut";
                if (inactive) port = count == 2 ? "out1" : "leftOut";
                coefficient = count == 2 && channel == 1 ? std::sin(0.1 * std::numbers::pi_v<double>)
                    : std::cos(0.1 * std::numbers::pi_v<double>);
            }
            else if (family == "decoder")
            {
                document.graph.nodes.push_back({"encoder", "talas.horizontalAmbisonicEncoder", {{"azimuth", "0.2"}, {"scale", "1"}}});
                document.graph.nodes.push_back({"producer", "talas.horizontalAmbisonicDecoder", {{"scale", "1"},
                    choice("layout", count == 4 ? "discrete4" : "discrete8")}});
                document.graph.connections.push_back({"source", "audioOut", "encoder", "audioIn"});
                for (std::string_view axis : {"w", "x", "y"})
                    document.graph.connections.push_back({"encoder", std::string(axis) + "Out", "producer", std::string(axis) + "In"});
                coefficient = std::sqrt(4.0 / (3 * count)) * (0.5 + std::cos(2 * std::numbers::pi_v<double>
                    * (0.2 - static_cast<double>(channel) / count)));
            }
            else
            {
                document.graph.nodes.push_back({"producer", "talas.quadFunctionGenerator", {{"frequency", "1 hz"}, {"phaseOffset", "0"},
                    {"skew", "0.5"}, {"curve", "1"}, {"smooth", "0"}, {"scale", rollback ? "4" : "1"}}});
                port = std::string(1, static_cast<char>('a' + channel)) + "Out";
            }
            document.graph.connections.push_back({"producer", port, "delay", "audioIn"});
            document.graph.connections.push_back({"delay", "audioOut", "out", "audioIn"});
            talas::runtime::RenderSettings settings{32.0, 5};
            if (rollback)
            {
                settings.eventValues = {{"Gain", 1.0, talas::expr::ValueDomain::Scalar}};
                document.graph.nodes.push_back({"post", "talas.vca", {}});
                document.graph.nodes.push_back({"gain", "talas.constant", {{"value", "!Gain"}}});
                document.graph.connections.back() = {"delay", "audioOut", "post", "audioIn"};
                document.graph.connections.push_back({"gain", "value", "post", "gain"});
                document.graph.connections.push_back({"post", "audioOut", "out", "audioIn"});
            }
            const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
            if (!compiled.ok())
            {
                require(inactive, "every active multichannel port must compile through a checked consumer");
                continue;
            }
            talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
            if (inactive)
            {
                require(!realtime.ready(), "declared but unwritten layout outputs must not acquire producer authority");
                continue;
            }
            if (!realtime.ready()) throw std::runtime_error(std::string(family) + "/" + port + ": " + std::string(realtime.error()));
            talas::runtime::OfflineRenderSession offline(*compiled.graph);
            std::size_t position = 0;
            const auto render = [&](std::size_t frames) {
                const auto expected = offline.render(frames);
                std::array<double, 5> actual{};
                require(expected.ok() && renderWithoutAllocation(realtime, std::span(actual).first(frames)),
                    "active multichannel outputs must render through checked consumers without callback allocation");
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    const auto absolute = position + frame;
                    double oracle = 0;
                    if (absolute != 0)
                    {
                        if (family == "quad")
                        {
                            const auto phase = std::fmod((absolute - 1) / 32.0 + channel / 4.0, 1.0);
                            oracle = (1 - std::abs(2 * phase - 1)) * (rollback ? 4 : 1);
                        }
                        else oracle = coefficient * std::sin(2 * std::numbers::pi_v<double> * (absolute - 1) / 32);
                    }
                    require(std::abs(actual[frame] - oracle) < 1e-12 && std::abs(expected.mono[frame] - oracle) < 1e-12,
                        "selected Matrix/spatial/Quad channels must match independent delayed signal oracles");
                }
                position += frames;
            };
            if (rollback)
            {
                render(5);
                require(realtime.updateEventValue("Gain", 1e308, talas::expr::ValueDomain::Scalar),
                    "Quad/Delay late-overflow gain must be accepted");
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    std::array<double, 5> failed;
                    failed.fill(-99);
                    require(!renderWithoutAllocation(realtime, failed)
                        && std::ranges::all_of(failed, [](double value) { return value == 0; }),
                        "general Delay checkpoint must permit repeated silent failure and retry");
                }
                require(realtime.updateEventValue("Gain", 1.0, talas::expr::ValueDomain::Scalar),
                    "corrected Quad/Delay gain must be accepted after failure");
                render(5);
                render(2);
                continue;
            }
            for (std::size_t frames : {1, 5, 2, 4}) render(frames);
            realtime.resetTransport();
            offline = talas::runtime::OfflineRenderSession(*compiled.graph);
            position = 0;
            render(5);
        }
    }
}

void testAmbisonicEncoderConsumers()
{
    constexpr std::array ports{"wOut", "xOut", "yOut"};
    const std::array coefficients{1 / std::sqrt(2.0),
        std::cos(0.4 * std::numbers::pi_v<double>),
        std::sin(0.4 * std::numbers::pi_v<double>)};
    for (std::size_t channel = 0; channel < ports.size(); ++channel)
    {
        talas::project::ProjectDocument document;
        document.graph.nodes = {{"source", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
            {"encode", "talas.horizontalAmbisonicEncoder", {{"azimuth", "0.2"}, {"scale", "1"}}},
            {"delay", "talas.delayWithFeedback", {{"delay", "1 samples"}, {"feedback", "0"}, {"mix", "1"}}},
            {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"source", "audioOut", "encode", "audioIn"},
            {"encode", ports[channel], "delay", "audioIn"}, {"delay", "audioOut", "out", "audioIn"}};
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {32.0, 5});
        require(compiled.ok(), "all ambisonic encoder outputs must compile with a checked downstream consumer");
        talas::runtime::OfflineRenderSession offline(*compiled.graph);
        talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
        if (!realtime.ready()) throw std::runtime_error(std::string(ports[channel]) + ": " + std::string(realtime.error()));
        std::size_t position = 0;
        const auto render = [&](std::size_t frames) {
            const auto expected = offline.render(frames);
            std::array<double, 5> actual{};
            require(expected.ok() && renderWithoutAllocation(realtime, std::span(actual).first(frames)),
                "ambisonic outputs must remain recognized by downstream delay consumers");
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                const auto absolute = position + frame;
                const auto oracle = absolute == 0 ? 0.0 : coefficients[channel]
                    * std::sin(2 * std::numbers::pi_v<double> * (absolute - 1) / 32);
                require(std::abs(actual[frame] - oracle) < 1e-12
                    && std::abs(expected.mono[frame] - oracle) < 1e-12,
                    "each ambisonic basis output must match its distinct analytical coefficient and delay");
            }
            position += frames;
        };
        for (std::size_t frames : {1, 5, 2, 4}) render(frames);
        realtime.resetTransport();
        offline = talas::runtime::OfflineRenderSession(*compiled.graph);
        position = 0;
        render(5);
    }
}

}

int main()
{
    testActiveMultichannelConsumers();
    testActiveMultichannelConsumers(true);
    testAmbisonicEncoderConsumers();
    testPhaseOwnership();
    testCompiledPhases(false);
    testCompiledPhases(true);
    testCompiledPhases(false, true);
    testCompiledPhases(false, false, true);
    testCompiledPhases(false, true, true);
}
