#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace talas::runtime
{
class RealtimeGraphHostTestAccess
{
public:
    static void setTransportPublicationBusy(
        RealtimeGraphHost& host,
        bool busy) noexcept
    {
        host.transportSnapshotGeneration_.store(
            busy ? 1 : 0,
            std::memory_order_release);
    }
};
}

namespace { std::atomic<bool> countAllocations = false; std::atomic<std::size_t> allocationCount = 0; }
void* operator new(std::size_t size) { if (countAllocations.load()) allocationCount.fetch_add(1); if (auto* p = std::malloc(size)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t size) { if (countAllocations.load()) allocationCount.fetch_add(1); if (auto* p = std::malloc(size)) return p; throw std::bad_alloc{}; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
static_assert(noexcept(std::declval<talas::runtime::RealtimeRenderSession&>().scheduleEventValue(
    {}, 0.0, talas::expr::ValueDomain::Scalar, 0)));
static_assert(noexcept(std::declval<talas::runtime::RealtimeAudioEngine&>().scheduleEventValue(
    {}, 0.0, talas::expr::ValueDomain::Scalar, 0)));
static_assert(noexcept(std::declval<talas::runtime::RealtimeGraphHost&>().scheduleEventValue(
    {}, 0.0, talas::expr::ValueDomain::Scalar, 0)));
static_assert(noexcept(std::declval<const talas::runtime::RealtimeGraphHost&>().transportFramePosition()));
static_assert(noexcept(std::declval<const talas::runtime::RealtimeGraphHost&>().transportSnapshot()));
static_assert(noexcept(std::declval<talas::runtime::RealtimeGraphHost&>().requestTransportReset()));
static_assert(noexcept(std::declval<talas::runtime::RealtimeGraphHost&>()
                           .relocateTransportAtNextRender(0)));
static_assert(noexcept(std::declval<const talas::runtime::RealtimeGraphHost&>().realtimeMetricsRaw()));
static_assert(std::is_trivially_copyable_v<talas::runtime::RealtimeTransportSnapshot>);
static_assert(std::is_trivially_copyable_v<talas::runtime::RealtimeMetricsSnapshotV1>);

void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void requireNear(float actual, float expected, const std::string& message) { if (std::abs(actual - expected) > 1.0e-6F) throw std::runtime_error(message); }

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::runtime::PreparedRuntimeGraph graphFor(
    double frequency,
    std::optional<talas::runtime::TransportLoopRegion> transportLoop = std::nullopt,
    double sampleRate = 4.0)
{
    talas::project::ProjectDocument doc;
    doc.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"osc", "talas.oscillator", {{"frequency", std::to_string(frequency) + " hz"}, {"amplitude", "1"}}},
        {"gain", "talas.constant", {{"value", "1"}}},
    };
    doc.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    auto settings = talas::runtime::RenderSettings{sampleRate, 2};
    settings.transportLoop = transportLoop;
    auto compiled = talas::runtime::compileProject(
        doc,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(), "host fixture should compile");
    return std::move(*compiled.graph);
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> engineFor(
    double frequency,
    std::uint64_t stageId = 0)
{
    auto graph = graphFor(frequency);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    engine->setStageId(stageId);
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> terminalEngineFor(
    std::uint64_t stageId,
    bool midiController = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = midiController
        ? std::vector<talas::project::ProjectNode>{{
              "terminal",
              "talas.midiOutputController",
              {
                  talas::project::ProjectParameterExpression{
                      "channel", "3"},
                  talas::project::ProjectParameterExpression{
                      "controllerNumber", "10"},
                  textParameter("value", "127"),
              },
          }}
        : std::vector<talas::project::ProjectNode>{{
              "terminal",
              "talas.midiOutputEventInBytes",
              {textParameter("bytes", "16r90 64 16r7f")},
          }};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "terminal MIDI host fixture should compile");
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        *compiled.graph);
    require(engine->ready(), std::string{engine->error()});
    engine->setStageId(stageId);
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> noteTerminalEngineFor(
    std::uint64_t stageId)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiOutputEvent",
        {
            talas::project::ProjectParameterExpression{"frequency", "440 hz"},
            talas::project::ProjectParameterExpression{"amplitude", "1"},
            talas::project::ProjectParameterExpression{"channel", "2"},
            talas::project::ProjectParameterExpression{"gate", "1"},
        },
    }};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "terminal MIDI note host fixture should compile");
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        std::move(*compiled.graph));
    require(engine->ready(), std::string{engine->error()});
    engine->setStageId(stageId);
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> midiFileTerminalEngineFor(
    std::uint64_t stageId)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiFileEcho",
        {
            talas::project::ProjectParameterExpression{"lowChannel", "1"},
            talas::project::ProjectParameterExpression{"highChannel", "16"},
            resourceParameter("fileName", "Sequences/C42.mid"),
        },
    }};
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.binaryResources.push_back({
        "Sequences/C42.mid",
        {
            'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
            'M', 'T', 'r', 'k', 0, 0, 0, 12,
            0, 0x90, 60, 64, 96, 0x80, 60, 0, 0, 0xff, 0x2f, 0,
        }});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "terminal MIDI file host fixture should compile");
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        std::move(*compiled.graph));
    require(engine->ready(), std::string{engine->error()});
    engine->setStageId(stageId);
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine>
globalControllerTerminalEngineFor(
    std::uint64_t stageId,
    std::string generatedEvent,
    std::string value)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.soundToGlobalController",
        {
            textParameter(
                "generatedEvent", std::move(generatedEvent)),
            textParameter("value", std::move(value)),
        },
    }};
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(
        compiled.ok(),
        "global-controller host fixture should compile");
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        std::move(*compiled.graph));
    require(engine->ready(), std::string{engine->error()});
    engine->setStageId(stageId);
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> loopEngineFor(
    double frequency,
    talas::runtime::TransportLoopRegion loop)
{
    auto graph = graphFor(frequency, loop);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> engineForRate(
    double frequency,
    double sampleRate)
{
    auto graph = graphFor(frequency, std::nullopt, sampleRate);
    auto engine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    return engine;
}

talas::runtime::PreparedRuntimeGraph feedbackGraph(
    std::optional<talas::runtime::TransportLoopRegion> transportLoop =
        std::nullopt)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"sink", "talas.feedbackLoopInput", {
            textParameter("connection", "hostLoop")}},
        {"mix", "talas.mixer", {
            {"gain1", "1"}, {"gain2", "0.5"}}},
        {"feedback", "talas.feedbackLoopOutput", {
            textParameter("connection", "hostLoop"),
            {"delay", "0 s"}}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "mix", "in1"},
        {"feedback", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "sink", "audioIn"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    auto settings = talas::runtime::RenderSettings{4.0, 2};
    settings.transportLoop = transportLoop;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "feedback host fixture should compile");
    return std::move(*compiled.graph);
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> feedbackEngine(
    std::optional<talas::runtime::TransportLoopRegion> transportLoop =
        std::nullopt)
{
    auto graph = feedbackGraph(transportLoop);
    auto engine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    return engine;
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine> mixingEngine(
    std::string_view typeId)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "input1", "talas.audioInput",
        {booleanParameter("channel1", true),
         booleanParameter("channel2", false)}});
    if (typeId == "talas.pan")
    {
        document.graph.nodes.push_back({
            "mix", "talas.pan",
            {{"pan", "0"}, {"scale", "1"},
             choiceParameter("type", "linear")}});
        document.graph.connections = {
            {"input1", "audioOut", "mix", "audioIn"},
        };
    }
    else
    {
        document.graph.nodes.push_back({
            "input2", "talas.audioInput",
            {booleanParameter("channel1", false),
             booleanParameter("channel2", true)}});
        if (typeId == "talas.crossfade")
        {
            document.graph.nodes.push_back({
                "mix", "talas.crossfade",
                {{"pan", "0"}, {"scale", "1"}, {"fade", "1"},
                 choiceParameter("type", "linear")}});
            document.graph.connections = {
                {"input1", "audioOut", "mix", "snd1"},
                {"input2", "audioOut", "mix", "snd2"},
            };
        }
        else
        {
            document.graph.nodes.push_back({
                "mix", "talas.stereoMix2", {}});
            document.graph.connections = {
                {"input1", "audioOut", "mix", "in1"},
                {"input2", "audioOut", "mix", "in2"},
            };
        }
    }
    document.graph.connections.push_back(
        {"mix", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {"mix", "rightOut", "out", "rightIn"});
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 1});
    require(compiled.ok(), "mixing replacement fixture should compile");
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        *compiled.graph);
    require(engine->ready(), std::string(engine->error()));
    return engine;
}

talas::runtime::PreparedRuntimeGraph m1UtilityGraph(
    std::optional<talas::runtime::TransportLoopRegion> transportLoop = std::nullopt)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.5"}}},
        {"filter", "talas.filter", {{"frequency", "4 hz"}, {"q", "1"}, {"scale", "1"}, {"feedback", "0"}, {"order", "2"}}},
        {"env", "talas.adsr", {{"attackTime", "125 ms"}, {"decayTime", "125 ms"}, {"sustainLevel", "0.5"}, {"releaseTime", "125 ms"}, {"scale", "1"}, {"gate", "0"}, {"legato", "0"}}},
        {"slew", "talas.slew", {{"riseTime", "5 ms"}, {"fallTime", "5 ms"}}},
        {"quantizer", "talas.quantizer", {{"stepSize", "1"}, {"offset", "1"}}},
        {"divider", "talas.clockDivider", {{"division", "1"}, {"steps", "4"}, {"reset", "0"}}},
        {"logic", "talas.logic", {choiceParameter("operation", "and"), {"left", "0"}, {"right", "1"}}},
        {"gate", "talas.gate", {{"lowThreshold", "0.25"}, {"highThreshold", "0.75"}}},
        {"clock", "talas.clock", {{"frequency", "4 hz"}, {"dutyCycle", "0.5"}, booleanParameter("enabled", true)}},
    };
    document.graph.connections = {
        {"clock", "gate", "gate", "value"},
        {"gate", "gate", "logic", "left"},
        {"logic", "gate", "divider", "clock"},
        {"divider", "step", "quantizer", "value"},
        {"quantizer", "value", "slew", "value"},
        {"slew", "value", "osc", "frequency"},
        {"osc", "audioOut", "filter", "audioIn"},
        {"filter", "audioOut", "vca", "audioIn"},
        {"divider", "gate", "env", "gate"},
        {"env", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{16.0, 8};
    settings.transportLoop = transportLoop;
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(), "M1 utility host fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph liveGraphFor(double initialGain = 1.0, std::size_t queueCapacity = 256)
{
    talas::project::ProjectDocument doc;
    doc.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"source", "talas.noise", {
            {"initialState", "1"},
            {"frequency", "0 hz"},
            {"centerValue", "0"},
            {"scale", "1"},
            choiceParameter("type", "hotPink"),
        }},
        {"gain", "talas.constant", {{"value", "!Gain"}}},
    };
    doc.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    auto settings = talas::runtime::RenderSettings{4.0, 2};
    settings.eventValues.push_back({"Gain", initialGain, talas::expr::ValueDomain::Scalar});
    settings.maxRealtimeEventTransactions = queueCapacity;
    auto compiled = talas::runtime::compileProject(doc, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(compiled.ok(), "live host fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph liveTimelineGraphFor()
{
    auto graphSettings = talas::runtime::RenderSettings{4.0, 2};
    graphSettings.eventValues.push_back(
        {"Gain", 1.0, talas::expr::ValueDomain::Scalar});
    graphSettings.eventValueChanges.push_back(
        {0, "Gain", 0.5, talas::expr::ValueDomain::Scalar});
    graphSettings.transportLoop =
        talas::runtime::TransportLoopRegion{0, 100};

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"source", "talas.noise", {
            {"initialState", "1"},
            {"frequency", "0 hz"},
            {"centerValue", "0"},
            {"scale", "1"},
            choiceParameter("type", "hotPink"),
        }},
        {"gain", "talas.constant", {{"value", "!Gain"}}},
    };
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(graphSettings));
    require(compiled.ok(), "live Timeline host fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph eventGainGraph(
    bool latencySensitive)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {{"gain", "!Gain"}}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    auto settings = talas::runtime::RenderSettings{4.0, 2};
    settings.eventValues.push_back({
        "Gain",
        1.0,
        talas::expr::ValueDomain::Scalar,
    });
    auto registry = talas::core::makeBuiltinNodeRegistry();
    if (latencySensitive)
    {
        registry.setNodeTypeProcessingContract(
            "talas.vca",
            talas::core::NodeStateModel::Stateless,
            false,
            {
                talas::core::LatencyKind::PreparationTime,
                0,
                "gain",
                talas::core::PreparedLatencyRule::ParameterFrames,
            });
    }
    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        std::move(settings));
    require(compiled.ok(),
        "Event Value reprepare host fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph fixedInputGainGraph(double gain)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {{"gain", std::to_string(gain)}}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, 2});
    require(compiled.ok(), "fixed input-gain host fixture should compile");
    return std::move(*compiled.graph);
}

std::unique_ptr<talas::runtime::RealtimeAudioEngine>
fixedInputGainEngine(double gain)
{
    auto graph = fixedInputGainGraph(gain);
    auto engine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    return engine;
}

talas::runtime::PreparedRuntimeGraph audioInputGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "out", "audioIn"},
    };
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 2});
    require(compiled.ok(), "host AudioInput fixture should compile");
    return std::move(*compiled.graph);
}

bool renderAndCount(talas::runtime::RealtimeGraphHost& host, float* const* channels, std::size_t count, std::size_t frames, std::size_t& allocations)
{
    allocationCount.store(0); countAllocations.store(true);
    const auto rendered = host.render(channels, count, frames);
    countAllocations.store(false); allocations = allocationCount.load();
    return rendered;
}

bool renderInputAndCount(
    talas::runtime::RealtimeGraphHost& host,
    talas::runtime::RealtimeAudioInputBlock input,
    float* const* channels,
    std::size_t count,
    std::size_t frames,
    std::size_t& allocations)
{
    allocationCount.store(0);
    countAllocations.store(true);
    const auto rendered = host.render(
        input, channels, count, frames);
    countAllocations.store(false);
    allocations = allocationCount.load();
    return rendered;
}

struct PreparedRenderContext
{
    talas::runtime::RealtimeGraphHost* host = nullptr;
    std::unique_ptr<talas::runtime::RealtimeAudioEngine> replacement;
    bool stagedReplacement = false;
    bool scheduled = false;
};

void scheduleAfterActivation(void* opaque, talas::runtime::RealtimeAudioEngine& renderTarget) noexcept
{
    auto& context = *static_cast<PreparedRenderContext*>(opaque);
    if (context.replacement != nullptr)
    {
        context.stagedReplacement = context.host->tryStage(std::move(context.replacement))
            == talas::runtime::RealtimeGraphStageResult::Accepted;
    }
    context.scheduled = renderTarget.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1);
}

void testHostRunsPreparationAfterActivationBeforeRendering()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor(1.0));
    require(pending->ready(), std::string(pending->error()));
    require(host.tryStage(std::move(pending)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "prepared-render fixture should stage");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor(1.0, 1));
    require(replacement->ready(), std::string(replacement->error()));
    require(replacement->scheduleEventValue("Gain", 0.75, talas::expr::ValueDomain::Scalar, 0),
            "replacement fixture should saturate its Event Value queue");
    PreparedRenderContext context{&host, std::move(replacement)};
    require(host.renderPrepared(
                {},
                channels.data(),
                channels.size(),
                output.size(),
                &context,
                scheduleAfterActivation),
            "prepared host callback should render");
    require(context.scheduled, "prepared host callback should observe the newly active graph");
    require(context.stagedReplacement, "prepared host callback should stage a replacement after target pinning");
    requireNear(output[0], 1.0F, "prepared host callback should preserve frame zero");
    requireNear(output[1], 0.25F, "prepared host callback should schedule before the same render");
}

void testHostActivatesPreparedGraphsAtBlockBoundaries()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(engineFor(1.0)) == talas::runtime::RealtimeGraphStageResult::Accepted, "first graph should stage");
    require(host.tryStage(engineFor(2.0)) == talas::runtime::RealtimeGraphStageResult::PendingEngineAwaitingActivation, "second graph should wait for pending activation");

    std::array<float, 2> left{}; std::array<float, 2> right{}; std::array<float*, 2> channels{left.data(), right.data()}; std::size_t allocations = 0;
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations), "first block should render");
    require(allocations == 0, "first host render should not allocate");
    requireNear(left[0], 0.0F, "first graph frame zero"); requireNear(left[1], 1.0F, "first graph frame one");

    require(host.tryStage(engineFor(0.0)) == talas::runtime::RealtimeGraphStageResult::Accepted, "replacement graph should stage after activation");
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations), "replacement block should render");
    require(allocations == 0, "replacement host render should not allocate");
    requireNear(left[0], 0.0F, "replacement graph frame zero"); requireNear(left[1], 0.0F, "replacement graph frame one");
    require(host.tryStage(engineFor(1.0)) == talas::runtime::RealtimeGraphStageResult::RetiredEngineAwaitingCollection, "retired engine must be collected before another stage");
    require(host.collectRetired() != nullptr, "control thread should collect retired engine");
    require(host.tryStage(engineFor(1.0)) == talas::runtime::RealtimeGraphStageResult::Accepted, "stage should resume after collection");
}

void testEmptyHostRendersSilenceWithoutAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    std::array<float, 2> left{1.0F, 1.0F}; std::array<float*, 1> channels{left.data()}; std::size_t allocations = 0;
    require(!renderAndCount(host, channels.data(), channels.size(), left.size(), allocations), "empty host should not render");
    require(allocations == 0, "empty host render should not allocate"); requireNear(left[0], 0.0F, "empty host clears output"); requireNear(left[1], 0.0F, "empty host clears output");
}

void testGraphMetricsCountCallbacksFailuresAndStageRejections()
{
    talas::runtime::RealtimeGraphHost host;
    require(
        host.realtimeMetricsRaw().renderCallbacks == 0
            && host.realtimeMetricsRaw().renderFailures == 0
            && host.realtimeMetricsRaw().stageRejections == 0,
        "new graph metrics should start at zero");
    require(
        host.tryStage({})
            == talas::runtime::RealtimeGraphStageResult::NullEngine,
        "null graph stage should reject");

    std::array<float, 2> output{1.0F, 1.0F};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    require(
        !renderAndCount(
            host,
            channels.data(),
            channels.size(),
            output.size(),
            allocations),
        "empty graph callback should fail closed");
    require(allocations == 0, "metrics must not allocate in the callback");

    require(
        host.tryStage(engineFor(1.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "valid metrics fixture should stage");
    host.setPlaying(true);
    require(
        renderAndCount(
            host,
            channels.data(),
            channels.size(),
            output.size(),
            allocations),
        "active graph callback should render");
    require(allocations == 0, "successful metrics callback must not allocate");
    host.setPlaying(false);
    require(
        !renderAndCount(
            host,
            channels.data(),
            channels.size(),
            output.size(),
            allocations),
        "paused graph callback should render silence");
    require(
        allocations == 0,
        "paused metrics callback must not allocate");

    const auto metrics = host.realtimeMetricsRaw();
    require(
        metrics.renderCallbacks == 3
            && metrics.renderFailures == 1
            && metrics.stageRejections == 1,
        "graph metrics should count callbacks without classifying healthy pause as failure");
    require(
        talas::runtime::realtimeMetricDelta(7, 3) == 4
            && talas::runtime::realtimeMetricDelta(3, 7) == 0
            && talas::runtime::realtimeMetricAddSaturated(
                std::numeric_limits<std::uint64_t>::max(),
                1)
                == std::numeric_limits<std::uint64_t>::max(),
        "metrics helpers should fail closed on regression and saturation");
    auto baseline = talas::runtime::RealtimeMetricsSnapshotV1{};
    baseline.streamDecodedBlocks = 8;
    baseline.streamHealthAvailable = 1;
    auto regressed = baseline;
    regressed.streamDecodedBlocks = 2;
    auto unavailable = regressed;
    unavailable.streamHealthAvailable = 0;
    unavailable.streamConsumerCount = 4;
    require(
        talas::runtime::realtimeMetricsCountersRegressed(
            regressed, baseline)
            && !talas::runtime::realtimeMetricsCountersRegressed(
                unavailable, baseline)
            && !talas::runtime::realtimeMetricsCountersRegressed(
                baseline, regressed),
        "counter regression should open a new presentation epoch");
    regressed.renderCallbacks = 12;
    baseline.renderCallbacks = 10;
    const auto regressionBaseline =
        talas::runtime::realtimeMetricsRegressionBaseline(
            regressed, baseline);
    const auto recovered = talas::runtime::realtimeMetricsSince(
        regressed, regressionBaseline, 2);
    require(
        recovered.renderCallbacks == 2
            && recovered.streamDecodedBlocks == 0,
        "regression recovery should preserve unrelated deltas and rebase only the regressed counter");
    const auto unavailableProjection =
        talas::runtime::realtimeMetricsSince(
            unavailable, baseline, 2);
    require(
        unavailableProjection.streamConsumerCount == 0
            && unavailableProjection.streamDecodedBlocks == 0,
        "an unavailable stream-health source should not project stale totals");
}

void testHostForwardsScheduledEventValuesToActiveAndPendingEngines()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor(1.0));
    auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor(1.0));
    require(active->ready(), std::string(active->error()));
    require(pending->ready(), std::string(pending->error()));
    require(host.tryStage(std::move(active)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "active engine should stage before scheduled forwarding");
    std::array<float, 2> left{};
    std::array<float, 2> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    std::size_t allocations = 0;
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "active engine should activate before scheduled forwarding");
    require(host.tryStage(std::move(pending)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "pending engine should stage before scheduled forwarding");
    require(host.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "host should forward a scheduled Event Value to active and pending engines");
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "pending engine with scheduled Event Value should render");
    require(allocations == 0, "scheduled host render should not allocate");
    requireNear(left[0], 1.0F, "scheduled Event Value should preserve pending frame zero");
    requireNear(left[1], 0.25F, "scheduled Event Value should reach the pending active engine");

    auto retired = host.collectRetired();
    require(retired != nullptr, "replaced active engine should be collectible");
    retired->setPlaying(true);
    std::array<float*, 1> retiredChannels{left.data()};
    require(retired->render(retiredChannels.data(), retiredChannels.size(), left.size()),
            "retired active engine should retain its forwarded schedule");
    requireNear(left[0], 1.0F, "scheduled Event Value should preserve retired active frame zero");
    requireNear(left[1], 0.25F, "scheduled Event Value should reach the retired active engine");
}

void testHostAcceptsScheduleWhenOnlyPendingEngineHasBinding()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(engineFor(1.0)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "static active fixture should stage");

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "static active fixture should activate");

    auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor());
    require(pending->ready(), std::string(pending->error()));
    require(host.tryStage(std::move(pending)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "live pending fixture should stage");
    require(host.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "host should report success when pending accepts and active rejects");
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "partially accepted pending schedule should render");
    requireNear(left[0], 1.0F, "partially accepted schedule frame zero");
    requireNear(left[1], 0.25F, "partially accepted schedule frame one");
}

void testHostRejectsScheduleTransactionWhenOneTargetCannotAccept()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor(1.0, 1));
    require(active->ready(), std::string(active->error()));
    require(host.tryStage(std::move(active)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "bounded active fixture should stage");

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "bounded active fixture should activate");
    require(host.scheduleEventValue("Gain", 0.5, talas::expr::ValueDomain::Scalar, 0),
            "active queue fixture should accept its first transaction");

    auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(liveGraphFor());
    require(pending->ready(), std::string(pending->error()));
    require(host.tryStage(std::move(pending)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "pending transaction fixture should stage");
    require(!host.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "host should reject atomically when one target queue is full");
    require(renderAndCount(host, channels.data(), channels.size(), left.size(), allocations),
            "pending engine should activate after rejected host transaction");
    requireNear(left[0], 1.0F, "rejected host transaction should preserve pending frame zero");
    requireNear(left[1], 1.0F, "rejected host transaction should not partially update pending engine");
}

void testHostRejectsReprepareUpdateBeforeMutatingAnyGeneration()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        eventGainGraph(false));
    require(active->ready(), std::string(active->error()));
    require(host.tryStage(std::move(active))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "ordinary active Event Value graph should stage");

    std::array<float, 2> input{1.0F, 1.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
        "ordinary active Event Value graph should activate");

    auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        eventGainGraph(true));
    require(pending->ready(), std::string(pending->error()));
    require(host.tryStage(std::move(pending))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "latency-sensitive pending Event Value graph should stage");
    require(!host.updateEventValue(
                "Gain",
                5.0,
                talas::expr::ValueDomain::Scalar),
        "host should reject a reprepare-only update before forwarding it");

    require(host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
        "pending graph should activate after the rejected update");
    requireNear(output.front(), 1.0F,
        "pending generation should retain its prepared Event Value");

    auto retired = host.collectRetired();
    require(retired != nullptr,
        "ordinary active generation should become collectible");
    retired->setPlaying(true);
    std::ranges::fill(output, 0.0F);
    require(retired->render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
        "retired generation should remain renderable for mutation verification");
    requireNear(output.front(), 1.0F,
        "rejected host update must not partially mutate the retired active generation");
}

void testHostDoesNotLoseSchedulesDuringPendingActivation()
{
    for (std::size_t iteration = 0; iteration < 200; ++iteration)
    {
        talas::runtime::RealtimeGraphHost host;
        host.setPlaying(true);
        auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(
            liveTimelineGraphFor());
        require(active->ready(), std::string(active->error()));
        require(host.tryStage(std::move(active)) == talas::runtime::RealtimeGraphStageResult::Accepted,
                "activation race active fixture should stage");

        std::array<float, 2> output{};
        std::array<float*, 1> channels{output.data()};
        require(host.render(channels.data(), channels.size(), output.size()),
                "activation race active fixture should activate");

        auto pending = std::make_unique<talas::runtime::RealtimeAudioEngine>(
            liveTimelineGraphFor());
        require(pending->ready(), std::string(pending->error()));
        require(host.tryStage(std::move(pending)) == talas::runtime::RealtimeGraphStageResult::Accepted,
                "activation race pending fixture should stage");

        std::barrier start{2};
        std::atomic<bool> scheduled = false;
        std::thread producer([&] {
            start.arrive_and_wait();
            scheduled.store(
                host.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 0),
                std::memory_order_release);
        });
        start.arrive_and_wait();
        const auto renderedDuringActivation = host.render(channels.data(), channels.size(), output.size());
        producer.join();
        require(renderedDuringActivation, "activation race callback should render");
        require(scheduled.load(std::memory_order_acquire),
                "activation race schedule should reach an observable engine");
        require(host.render(channels.data(), channels.size(), output.size()),
                "activation race verification callback should render");
        requireNear(output[0], 0.25F, "activation race should preserve the scheduled value");
        requireNear(output[1], 0.25F, "activation race should preserve the scheduled value across the block");
    }
}

void testHostForwardsCurrentAudioInputAcrossGraphReplacement()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto first = std::make_unique<talas::runtime::RealtimeAudioEngine>(audioInputGraph());
    require(first->ready(), std::string(first->error()));
    require(host.tryStage(std::move(first)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "first AudioInput engine should stage");

    std::array<float, 2> firstInput{1.0F, 2.0F};
    std::array<const float*, 1> inputChannels{firstInput.data()};
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(host.render({inputChannels.data(), inputChannels.size()}, outputChannels.data(), outputChannels.size(), output.size()),
            "host should forward input to the first active engine");
    requireNear(output[1], 2.0F, "first host AudioInput block");

    auto replacement = std::make_unique<talas::runtime::RealtimeAudioEngine>(audioInputGraph());
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "replacement AudioInput engine should stage");
    std::array<float, 2> secondInput{7.0F, 8.0F};
    inputChannels[0] = secondInput.data();
    require(host.render({inputChannels.data(), inputChannels.size()}, outputChannels.data(), outputChannels.size(), output.size()),
            "host replacement should borrow the current input block");
    requireNear(output[0], 7.0F, "replacement host should not retain the previous input pointer");
}

void testHostTransportAdvancesOnlyWhilePlayingAndSurvivesReplacement()
{
    talas::runtime::RealtimeGraphHost host;
    require(host.transportFramePosition() == 0, "transport should start at frame zero");
    const auto initialSnapshot = host.transportSnapshot();
    require(initialSnapshot.epoch == 0
            && initialSnapshot.absoluteFrame == 0
            && initialSnapshot.playheadFrame == 0
            && initialSnapshot.sampleRate == 0.0
            && !initialSnapshot.playing
            && !initialSnapshot.loopEnabled
            && initialSnapshot.discontinuitySequence == 0,
        "unstaged host should publish one coherent empty transport snapshot");
    require(host.tryStage(engineFor(0.0)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "transport fixture should stage");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(!host.render(channels.data(), channels.size(), output.size()),
            "stopped transport should silence the active graph");
    require(host.transportFramePosition() == 0, "stopped callback must not advance transport");
    const auto stoppedSnapshot = host.transportSnapshot();
    require(stoppedSnapshot.epoch == 1
            && stoppedSnapshot.absoluteFrame == 0
            && stoppedSnapshot.playheadFrame == 0
            && stoppedSnapshot.sampleRate == 4.0
            && !stoppedSnapshot.playing
            && !stoppedSnapshot.loopEnabled,
        "first activation should start epoch one without advancing while stopped");

    host.setPlaying(true);
    require(host.render(channels.data(), channels.size(), output.size()),
            "playing transport should render");
    require(host.transportFramePosition() == 2, "first callback should advance by its exact frame count");
    const auto firstPlayingSnapshot = host.transportSnapshot();
    require(firstPlayingSnapshot.epoch == stoppedSnapshot.epoch
            && firstPlayingSnapshot.absoluteFrame == 2
            && firstPlayingSnapshot.playheadFrame == 2
            && firstPlayingSnapshot.playing,
        "playing snapshot should expose the exact absolute and public playhead frame");
    require(host.render(channels.data(), channels.size(), 1),
            "playing transport should accept a shorter callback");
    require(host.transportFramePosition() == 3, "variable callback size should advance the same host clock");

    host.setPlaying(false);
    require(!host.render(channels.data(), channels.size(), output.size()),
            "paused transport should silence the graph");
    require(host.transportFramePosition() == 3, "pause should preserve transport position");
    const auto pausedSnapshot = host.transportSnapshot();
    require(pausedSnapshot.absoluteFrame == 3
            && pausedSnapshot.playheadFrame == 3
            && !pausedSnapshot.playing,
        "pause should change playing state without advancing either frame domain");

    require(host.tryStage(engineFor(0.0)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "transport replacement should stage");
    host.setPlaying(true);
    require(host.render(channels.data(), channels.size(), output.size()),
            "replacement should render at the shared transport position");
    require(host.transportFramePosition() == 5, "graph activation must not reset the host transport");
    const auto replacementSnapshot = host.transportSnapshot();
    require(replacementSnapshot.epoch == pausedSnapshot.epoch
            && replacementSnapshot.discontinuitySequence
                == pausedSnapshot.discontinuitySequence
            && replacementSnapshot.absoluteFrame == 5,
        "same-rate graph replacement should preserve the host epoch and discontinuity");
}

void testHostPublishesLoopedTransportFromMonotonicClockWithoutAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    const talas::runtime::TransportLoopRegion loop{2, 6};
    require(
        host.tryStage(loopEngineFor(0.0, loop))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "loop transport fixture should stage");
    host.setPlaying(true);

    std::array<float, 3> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    require(
        renderAndCount(host, channels.data(), channels.size(), 2, allocations),
        "loop transport first callback should render");
    require(allocations == 0, "loop transport first callback must not allocate");
    require(host.transportFramePosition() == 2,
            "loop transport should reach its start normally");

    require(
        renderAndCount(host, channels.data(), channels.size(), 2, allocations),
        "loop transport first interior callback should render");
    require(host.transportFramePosition() == 4,
            "loop transport should advance inside the region");
    require(
        renderAndCount(host, channels.data(), channels.size(), 1, allocations),
        "loop transport shorter callback should render");
    require(host.transportFramePosition() == 5,
            "loop transport should preserve the exact variable callback size");
    require(
        renderAndCount(host, channels.data(), channels.size(), 2, allocations),
        "loop transport wrapping callback should render");
    require(allocations == 0, "loop transport wrapping callback must not allocate");
    require(host.transportFramePosition() == 3,
            "loop transport should wrap the public position across a variable callback");
    const auto wrappedSnapshot = host.transportSnapshot();
    require(wrappedSnapshot.absoluteFrame == 7
            && wrappedSnapshot.playheadFrame == 3
            && wrappedSnapshot.loopEnabled
            && wrappedSnapshot.loopStartFrame == loop.startFrame
            && wrappedSnapshot.loopEndFrame == loop.endFrame,
        "loop snapshot should keep monotonic absolute time and half-open loop mapping distinct");

    host.setPlaying(false);
    require(
        !renderAndCount(host, channels.data(), channels.size(), 2, allocations),
        "paused loop transport should silence the graph");
    require(host.transportFramePosition() == 3,
            "pause should preserve the wrapped public position");

    require(
        host.tryStage(loopEngineFor(0.0, loop))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "loop replacement should stage");
    host.setPlaying(true);
    require(
        renderAndCount(host, channels.data(), channels.size(), 2, allocations),
        "loop replacement should render");
    require(allocations == 0, "loop replacement callback must not allocate");
    require(host.transportFramePosition() == 5,
            "replacement should retain the shared monotonic loop phase");

    host.setPlaying(false);
    const auto beforeReset = host.transportSnapshot();
    host.requestTransportReset();
    require(
        !renderAndCount(host, channels.data(), channels.size(), 1, allocations),
        "loop reset should adopt while stopped");
    require(host.transportFramePosition() == 0,
            "loop reset should return the transport position to zero");
    const auto afterReset = host.transportSnapshot();
    require(afterReset.epoch == beforeReset.epoch
            && afterReset.discontinuitySequence
                == beforeReset.discontinuitySequence + 1
            && afterReset.absoluteFrame == 0
            && afterReset.playheadFrame == 0,
        "stop/reset should publish a discontinuity without changing sample-rate epoch");
}

void testHostAdoptsTransportResetAtCallbackBoundaryWithoutAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(engineFor(0.0, 31)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "transport reset fixture should stage");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    require(renderAndCount(host, channels.data(), channels.size(), output.size(), allocations),
            "transport reset fixture should render");
    require(host.transportFramePosition() == 2, "transport reset fixture should advance");
    const auto beforeReset = host.transportSnapshot();

    host.setPlaying(false);
    host.requestTransportReset();
    require(host.transportFramePosition() == 2,
            "control-thread reset should wait for the audio callback boundary");
    require(!renderAndCount(host, channels.data(), channels.size(), output.size(), allocations),
            "reset adoption callback should remain stopped");
    require(allocations == 0, "transport reset adoption must not allocate");
    require(host.transportFramePosition() == 0, "audio callback should adopt the reset request");
    const auto resetSnapshot = host.transportSnapshot();
    require(resetSnapshot.epoch == beforeReset.epoch
            && resetSnapshot.discontinuitySequence
                == beforeReset.discontinuitySequence + 1,
        "callback-boundary reset adoption should increment only the discontinuity sequence");

    host.setPlaying(true);
    require(renderAndCount(host, channels.data(), channels.size(), 1, allocations),
            "transport should resume after reset");
    require(allocations == 0, "resumed transport callback must not allocate");
    require(host.transportFramePosition() == 1, "resumed transport should advance from frame zero");

    talas::runtime::MtcTransportFollowerSnapshot mtc;
    mtc.identity = {
        .authorityGeneration = 41,
        .sourceGeneration = 51,
        .stageId = host.activeStageId(),
        .sourceSlot = 1,
    };
    require(talas::runtime::makeSmpteTimeCode(
                talas::runtime::SmpteRate::Fps24,
                0, 0, 1, 0,
                talas::runtime::SmpteDirection::Forward,
                mtc.timeCode),
        "device-stop fixture should create valid MTC evidence");
    mtc.state = talas::runtime::MtcTransportLockState::Locked;
    mtc.hasTimeCode = true;
    mtc.runningEvidence = true;
    host.setExternalMtc(mtc, 24);
    require(renderAndCount(
                host, channels.data(), channels.size(), 1, allocations),
        "device-stop fixture should publish external MTC");
    require(host.musicalTransportSnapshot().mtcAuthority,
        "device-stop fixture should establish active MTC publication");

    const auto beforeDeviceStop = host.transportSnapshot();
    host.clearAfterAudioStopped();
    require(host.transportFramePosition() == 0, "device-stop clear should start a new transport epoch");
    const auto deviceStopSnapshot = host.transportSnapshot();
    require(deviceStopSnapshot.epoch == beforeDeviceStop.epoch + 1
            && deviceStopSnapshot.discontinuitySequence
                == beforeDeviceStop.discontinuitySequence + 1
            && deviceStopSnapshot.sampleRate == 0.0
            && !deviceStopSnapshot.loopEnabled,
        "device stop should publish a fresh empty epoch and discontinuity");
    const auto musicalAfterDeviceStop = host.musicalTransportSnapshot();
    require(musicalAfterDeviceStop.publicationValid
            && !musicalAfterDeviceStop.mtcAuthority
            && musicalAfterDeviceStop.mtcAuthorityGeneration == 0
            && musicalAfterDeviceStop.mtcStageId == 0,
        "device stop should clear all external MTC evidence before publishing the empty stage");
}

void testInternalContinuationDefersAConcurrentControlReset()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(engineFor(0.0, 31))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "continuation reset fixture should stage");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(host.prepareNextRenderTransportBoundary().absoluteFrame == 0
                && host.render(channels.data(), channels.size(), 1),
            "first internal segment should adopt the outer callback boundary");
    host.requestTransportReset();
    const auto continuation = host.prepareContinuationRenderTransportBoundary();
    require(continuation.absoluteFrame == 1
                && continuation.discontinuitySequence == 0
                && host.render(channels.data(), channels.size(), 1),
            "an internal segment boundary must not adopt a control reset that arrived mid-callback");

    const auto nextCallback = host.prepareNextRenderTransportBoundary();
    require(nextCallback.absoluteFrame == 0
                && nextCallback.discontinuitySequence == 1,
            "the deferred control reset must adopt at the next outer callback boundary");
}

void testHostRelocatesAtCallbackBoundaryWithoutAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    require(host.tryStage(engineFor(0.0))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "transport relocation fixture should stage");
    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    (void)renderAndCount(
        host, channels.data(), channels.size(), output.size(), allocations);
    const auto before = host.transportSnapshot();

    host.relocateTransportAtNextRender(7);
    require(!renderAndCount(
                host,
                channels.data(),
                channels.size(),
                output.size(),
                allocations),
            "paused relocation should be adopted without rendering audio");
    require(allocations == 0,
            "transport relocation callback must not allocate");
    const auto relocated = host.transportSnapshot();
    require(relocated.absoluteFrame == 7
                && relocated.playheadFrame == 7
                && relocated.epoch == before.epoch
                && relocated.discontinuitySequence
                    == before.discontinuitySequence + 1,
            "relocation should preserve epoch and publish one exact discontinuity");

    require(host.midiSongPositionFrame(1) == 1,
            "prepared default map should convert SPP against the active sample rate");
    require(host.relocateMidiSongPositionAtNextRender(1),
            "active prepared map should admit a 14-bit Song Position");
    host.setPlaying(true);
    require(renderAndCount(
                host,
                channels.data(),
                channels.size(),
                output.size(),
                allocations),
            "Song Position relocation should render from the new position");
    require(allocations == 0 && host.transportFramePosition() == 3,
            "Song Position should relocate to frame one then advance exactly one block");
}

void testHostRejectsStageBoundRelocationAfterPendingMapReplacement()
{
    talas::runtime::RealtimeGraphHost host;
    require(host.tryStage(engineFor(0.0, 41))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "stage-bound relocation fixture should stage its first map");
    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    (void)renderAndCount(
        host, channels.data(), channels.size(), output.size(), allocations);
    const auto before = host.transportSnapshot();

    host.relocateTransportAtNextRender(7, 41);
    require(host.tryStage(engineFor(1.0, 42))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "replacement musical map should stage before relocation adoption");
    require(!host.midiSongPositionFrame(1, 41).has_value(),
            "pending replacement must hide the prior map from exact SPP conversion");
    require(!renderAndCount(
                host,
                channels.data(),
                channels.size(),
                output.size(),
                allocations),
            "paused replacement should adopt without rendering audio");
    const auto rejected = host.transportSnapshot();
    require(
        rejected.absoluteFrame == before.absoluteFrame
            && rejected.discontinuitySequence
                == before.discontinuitySequence
            && host.rejectedStageBoundRelocationCount() == 1,
        "a relocation bound to the replaced map must fail without moving transport");

    host.relocateTransportAtNextRender(5, 42);
    require(!renderAndCount(
                host,
                channels.data(),
                channels.size(),
                output.size(),
                allocations),
            "exact replacement-map relocation should adopt while paused");
    require(
        host.transportSnapshot().absoluteFrame == 5
            && host.lastStageBoundRelocationFrame() == 5
            && host.rejectedStageBoundRelocationCount() == 1,
        "the exact current stage should remain eligible for relocation");
    require(allocations == 0,
            "stage-bound relocation validation must not allocate");
}

void testM1UtilityChainAndTransportResetDoNotAllocate()
{
    auto graph = m1UtilityGraph();
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(
        host.tryStage(std::move(engine))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "M1 utility engine should stage");
    std::array<float, 8> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    require(
        renderAndCount(
            host, channels.data(), channels.size(), output.size(), allocations),
        "M1 utility callback should render");
    require(allocations == 0,
        "M1 utility callback should not allocate");
    require(
        std::ranges::any_of(
            output,
            [](float sample) { return std::abs(sample) > 1.0e-6F; }),
        "M1 utility callback should produce gated audio");

    host.requestTransportReset();
    std::array<float, 8> resetOutput{};
    std::array<float*, 1> resetChannels{resetOutput.data()};
    require(
        renderAndCount(
            host,
            resetChannels.data(),
            resetChannels.size(),
            resetOutput.size(),
            allocations),
        "M1 utility callback should render after host transport reset");
    require(allocations == 0,
        "M1 utility transport reset callback should not allocate");
    require(host.transportFramePosition() == output.size(),
        "M1 utility Clock should resume from the host-owned reset frame");

    auto freshGraph = m1UtilityGraph();
    auto freshEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(freshGraph);
    require(freshEngine->ready(), std::string(freshEngine->error()));
    talas::runtime::RealtimeGraphHost freshHost;
    freshHost.setPlaying(true);
    require(
        freshHost.tryStage(std::move(freshEngine))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "fresh selective-reset reference should stage");
    std::array<float, 8> freshOutput{};
    std::array<float*, 1> freshChannels{freshOutput.data()};
    require(
        renderAndCount(
            freshHost,
            freshChannels.data(),
            freshChannels.size(),
            freshOutput.size(),
            allocations),
        "fresh selective-reset reference should render");
    require(allocations == 0,
        "fresh selective-reset reference callback should not allocate");
    for (std::size_t frame = 0; frame < resetOutput.size(); ++frame)
    {
        requireNear(
            resetOutput[frame],
            freshOutput[frame],
            "descriptor-selected kernels should match fresh state after reset at frame "
                + std::to_string(frame));
    }
}

void testLoopWrapDoesNotResetDescriptorSelectedKernels()
{
    auto loopGraph = m1UtilityGraph(
        talas::runtime::TransportLoopRegion{0, 8});
    auto monotonicGraph = m1UtilityGraph();
    auto loopEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(loopGraph);
    auto monotonicEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(monotonicGraph);
    require(loopEngine->ready(), std::string(loopEngine->error()));
    require(monotonicEngine->ready(), std::string(monotonicEngine->error()));

    talas::runtime::RealtimeGraphHost loopHost;
    talas::runtime::RealtimeGraphHost monotonicHost;
    loopHost.setPlaying(true);
    monotonicHost.setPlaying(true);
    require(loopHost.tryStage(std::move(loopEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted
            && monotonicHost.tryStage(std::move(monotonicEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "loop reset-negative fixtures should stage");

    std::array<float, 8> loopOutput{};
    std::array<float, 8> monotonicOutput{};
    std::array<float*, 1> loopChannels{loopOutput.data()};
    std::array<float*, 1> monotonicChannels{monotonicOutput.data()};
    for (std::size_t callback = 0; callback < 2; ++callback)
    {
        require(loopHost.render(
                    loopChannels.data(),
                    loopChannels.size(),
                    loopOutput.size())
                && monotonicHost.render(
                    monotonicChannels.data(),
                    monotonicChannels.size(),
                    monotonicOutput.size()),
            "loop reset-negative callbacks should render");
        require(loopOutput == monotonicOutput,
            "loop wrap should change only playhead mapping and preserve every selected kernel state");
    }
    const auto loopSnapshot = loopHost.transportSnapshot();
    require(loopSnapshot.absoluteFrame == 16
            && loopSnapshot.playheadFrame == 0
            && loopSnapshot.discontinuitySequence == 0,
        "loop wrap should not publish a transport discontinuity");
}

void testPauseDoesNotResetDescriptorSelectedKernels()
{
    auto pausedGraph = m1UtilityGraph();
    auto uninterruptedGraph = m1UtilityGraph();
    auto pausedEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(pausedGraph);
    auto uninterruptedEngine =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(uninterruptedGraph);
    require(pausedEngine->ready(), std::string(pausedEngine->error()));
    require(uninterruptedEngine->ready(), std::string(uninterruptedEngine->error()));

    talas::runtime::RealtimeGraphHost pausedHost;
    talas::runtime::RealtimeGraphHost uninterruptedHost;
    pausedHost.setPlaying(true);
    uninterruptedHost.setPlaying(true);
    require(pausedHost.tryStage(std::move(pausedEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted
            && uninterruptedHost.tryStage(std::move(uninterruptedEngine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "pause reset-negative fixtures should stage");

    std::array<float, 8> pausedOutput{};
    std::array<float, 8> uninterruptedOutput{};
    std::array<float*, 1> pausedChannels{pausedOutput.data()};
    std::array<float*, 1> uninterruptedChannels{uninterruptedOutput.data()};
    require(pausedHost.render(
                pausedChannels.data(),
                pausedChannels.size(),
                pausedOutput.size())
            && uninterruptedHost.render(
                uninterruptedChannels.data(),
                uninterruptedChannels.size(),
                uninterruptedOutput.size()),
        "pause reset-negative seed callbacks should render");
    require(pausedOutput == uninterruptedOutput,
        "pause reset-negative fixtures should start from equal state");

    pausedHost.setPlaying(false);
    require(!pausedHost.render(
                pausedChannels.data(),
                pausedChannels.size(),
                pausedOutput.size()),
        "paused callback should remain silent");
    pausedHost.setPlaying(true);
    require(pausedHost.render(
                pausedChannels.data(),
                pausedChannels.size(),
                pausedOutput.size())
            && uninterruptedHost.render(
                uninterruptedChannels.data(),
                uninterruptedChannels.size(),
                uninterruptedOutput.size()),
        "pause reset-negative resume callbacks should render");
    require(pausedOutput == uninterruptedOutput,
        "pause/resume should preserve all descriptor-selected kernel state");
}

void testGraphReplacementStartsFreshAtCurrentTransportSnapshot()
{
    auto graph = m1UtilityGraph();
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
    require(engine->ready(), std::string(engine->error()));
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(std::move(engine))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "graph replacement snapshot fixture should stage");

    std::array<float, 8> seedOutput{};
    std::array<float*, 1> seedChannels{seedOutput.data()};
    require(host.render(
                seedChannels.data(),
                seedChannels.size(),
                seedOutput.size()),
        "graph replacement snapshot seed should render");

    auto replacementGraph = m1UtilityGraph();
    auto replacement =
        std::make_unique<talas::runtime::RealtimeAudioEngine>(replacementGraph);
    require(replacement->ready(), std::string(replacement->error()));
    require(host.tryStage(std::move(replacement))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
        "same-rate replacement should stage at the current epoch");
    std::array<float, 8> replacementOutput{};
    std::array<float*, 1> replacementChannels{replacementOutput.data()};
    require(host.render(
                replacementChannels.data(),
                replacementChannels.size(),
                replacementOutput.size()),
        "same-rate replacement should render from fresh graph state");

    auto referenceGraph = m1UtilityGraph();
    talas::runtime::RealtimeAudioEngine reference(referenceGraph);
    require(reference.ready(), std::string(reference.error()));
    reference.setPlaying(true);
    reference.setTransportSnapshotBeforeRender({
        .epoch = 1,
        .absoluteFrame = seedOutput.size(),
        .playheadFrame = seedOutput.size(),
        .sampleRate = 16.0,
        .playing = true,
    });
    std::array<float, 8> referenceOutput{};
    std::array<float*, 1> referenceChannels{referenceOutput.data()};
    require(reference.render(
                referenceChannels.data(),
                referenceChannels.size(),
                referenceOutput.size()),
        "fresh graph state at the replacement snapshot should render");
    require(replacementOutput == referenceOutput,
        "activated replacement should be fresh while adopting the current host snapshot");
}

void testFeedbackStateSurvivesLoopAndPauseButNotGraphReplacement()
{
    {
        talas::runtime::RealtimeGraphHost loopHost;
        talas::runtime::RealtimeGraphHost monotonicHost;
        loopHost.setPlaying(true);
        monotonicHost.setPlaying(true);
        require(
            loopHost.tryStage(feedbackEngine(
                talas::runtime::TransportLoopRegion{0, 2}))
                    == talas::runtime::RealtimeGraphStageResult::Accepted
                && monotonicHost.tryStage(feedbackEngine())
                    == talas::runtime::RealtimeGraphStageResult::Accepted,
            "feedback loop-wrap fixtures should stage");

        std::array<float, 2> loopInput{1.0F, 0.0F};
        std::array<float, 2> monotonicInput{1.0F, 0.0F};
        std::array<float, 2> loopSamples{};
        std::array<float, 2> monotonicSamples{};
        std::array<const float*, 1> loopInputChannels{
            loopInput.data()};
        std::array<const float*, 1> monotonicInputChannels{
            monotonicInput.data()};
        std::array<float*, 1> loopChannels{loopSamples.data()};
        std::array<float*, 1> monotonicChannels{
            monotonicSamples.data()};
        require(
            loopHost.render(
                {loopInputChannels.data(), loopInputChannels.size()},
                loopChannels.data(), loopChannels.size(), loopSamples.size())
                && monotonicHost.render(
                    {monotonicInputChannels.data(),
                     monotonicInputChannels.size()},
                    monotonicChannels.data(),
                    monotonicChannels.size(),
                    monotonicSamples.size()),
            "feedback loop-wrap seed callbacks should render");
        loopInput.fill(0.0F);
        monotonicInput.fill(0.0F);
        require(
            loopHost.render(
                {loopInputChannels.data(), loopInputChannels.size()},
                loopChannels.data(), loopChannels.size(), loopSamples.size())
                && monotonicHost.render(
                    {monotonicInputChannels.data(),
                     monotonicInputChannels.size()},
                    monotonicChannels.data(),
                    monotonicChannels.size(),
                    monotonicSamples.size()),
            "feedback loop-wrap continuation callbacks should render");
        require(
            loopSamples == monotonicSamples
                && loopSamples
                    == std::array<float, 2>{0.25F, 0.125F}
                && loopHost.transportSnapshot().discontinuitySequence == 0,
            "loop wrap should preserve feedback history without publishing a discontinuity");
    }

    {
        talas::runtime::RealtimeGraphHost pausedHost;
        talas::runtime::RealtimeGraphHost uninterruptedHost;
        pausedHost.setPlaying(true);
        uninterruptedHost.setPlaying(true);
        require(
            pausedHost.tryStage(feedbackEngine())
                    == talas::runtime::RealtimeGraphStageResult::Accepted
                && uninterruptedHost.tryStage(feedbackEngine())
                    == talas::runtime::RealtimeGraphStageResult::Accepted,
            "feedback pause fixtures should stage");

        std::array<float, 2> pausedInput{1.0F, 0.0F};
        std::array<float, 2> uninterruptedInput{1.0F, 0.0F};
        std::array<float, 2> pausedSamples{};
        std::array<float, 2> uninterruptedSamples{};
        std::array<const float*, 1> pausedInputChannels{
            pausedInput.data()};
        std::array<const float*, 1> uninterruptedInputChannels{
            uninterruptedInput.data()};
        std::array<float*, 1> pausedChannels{pausedSamples.data()};
        std::array<float*, 1> uninterruptedChannels{
            uninterruptedSamples.data()};
        require(
            pausedHost.render(
                {pausedInputChannels.data(), pausedInputChannels.size()},
                pausedChannels.data(),
                pausedChannels.size(),
                pausedSamples.size())
                && uninterruptedHost.render(
                    {uninterruptedInputChannels.data(),
                     uninterruptedInputChannels.size()},
                    uninterruptedChannels.data(),
                    uninterruptedChannels.size(),
                    uninterruptedSamples.size()),
            "feedback pause seed callbacks should render");
        pausedHost.setPlaying(false);
        pausedInput.fill(0.0F);
        require(
            !pausedHost.render(
                {pausedInputChannels.data(), pausedInputChannels.size()},
                pausedChannels.data(),
                pausedChannels.size(),
                pausedSamples.size()),
            "paused feedback callback should remain silent");
        pausedHost.setPlaying(true);
        uninterruptedInput.fill(0.0F);
        require(
            pausedHost.render(
                {pausedInputChannels.data(), pausedInputChannels.size()},
                pausedChannels.data(),
                pausedChannels.size(),
                pausedSamples.size())
                && uninterruptedHost.render(
                    {uninterruptedInputChannels.data(),
                     uninterruptedInputChannels.size()},
                    uninterruptedChannels.data(),
                    uninterruptedChannels.size(),
                    uninterruptedSamples.size()),
            "feedback pause continuation callbacks should render");
        require(
            pausedSamples == uninterruptedSamples
                && pausedSamples
                    == std::array<float, 2>{0.25F, 0.125F},
            "pause and resume should preserve feedback history");
    }

    {
        talas::runtime::RealtimeGraphHost host;
        host.setPlaying(true);
        require(
            host.tryStage(feedbackEngine())
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "feedback replacement seed graph should stage");
        std::array<float, 2> input{1.0F, 0.0F};
        std::array<float, 2> samples{};
        std::array<const float*, 1> inputChannels{input.data()};
        std::array<float*, 1> channels{samples.data()};
        require(
            host.render(
                {inputChannels.data(), inputChannels.size()},
                channels.data(), channels.size(), samples.size()),
            "feedback replacement seed callback should render");
        require(
            host.tryStage(feedbackEngine())
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            "fresh feedback replacement should stage");
        input.fill(0.0F);
        require(
            host.render(
                {inputChannels.data(), inputChannels.size()},
                channels.data(), channels.size(), samples.size()),
            "fresh feedback replacement should render");
        require(
            samples == std::array<float, 2>{0.0F, 0.0F},
            "graph replacement should start with fresh feedback state");
    }
}

void testMixingNodeGraphReplacementsKeepStereoOrderWithoutAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    std::array<float, 1> input1{1.0F};
    std::array<float, 1> input2{2.0F};
    std::array<const float*, 2> inputChannels{
        input1.data(), input2.data()};
    std::array<float, 1> left{};
    std::array<float, 1> right{};
    std::array<float*, 2> outputChannels{left.data(), right.data()};
    std::size_t allocations = 0;

    const auto stageAndRequire = [&](
        std::string_view typeId,
        float expectedLeft,
        float expectedRight) {
        require(
            host.tryStage(mixingEngine(typeId))
                == talas::runtime::RealtimeGraphStageResult::Accepted,
            std::string(typeId) + " replacement should stage");
        require(
            renderInputAndCount(
                host,
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                left.size(),
                allocations),
            std::string(typeId) + " replacement should render");
        require(
            allocations == 0,
            std::string(typeId)
                + " replacement callback should not allocate");
        requireNear(
            left[0], expectedLeft,
            std::string(typeId) + " replacement left channel");
        requireNear(
            right[0], expectedRight,
            std::string(typeId) + " replacement right channel");
        (void) host.collectRetired();
    };

    stageAndRequire("talas.pan", 1.0F, 0.0F);
    stageAndRequire("talas.crossfade", 2.0F, 0.0F);
    stageAndRequire("talas.stereoMix2", 1.0F, 2.0F);
}

void testHostStartsNewTransportEpochWhenReplacementSampleRateChanges()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(
        host.tryStage(engineForRate(0.0, 4.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "sample-rate epoch fixture should stage");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(
        host.render(channels.data(), channels.size(), 1),
        "sample-rate epoch fixture should render at the first rate");
    require(host.transportFramePosition() == 1,
            "first sample-rate epoch should advance");
    require(host.transportSampleRate() == 4.0,
            "host should publish the active sample rate");
    const auto firstEpoch = host.transportSnapshot();
    require(firstEpoch.epoch == 1
            && firstEpoch.discontinuitySequence == 0,
        "initial device activation should establish epoch one without a discontinuity");

    require(
        host.tryStage(engineForRate(0.0, 8.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "different-rate replacement should stage");
    require(host.transportSampleRate() == 4.0,
            "pending sample rate must not publish before activation");
    require(
        host.render(channels.data(), channels.size(), output.size()),
        "different-rate replacement should activate and render");
    require(host.transportFramePosition() == 2,
            "different-rate activation should reset before advancing");
    require(host.transportSampleRate() == 8.0,
            "activated sample rate should publish at the callback boundary");
    const auto secondEpoch = host.transportSnapshot();
    require(secondEpoch.epoch == firstEpoch.epoch + 1
            && secondEpoch.discontinuitySequence
                == firstEpoch.discontinuitySequence + 1
            && secondEpoch.absoluteFrame == output.size()
            && secondEpoch.playheadFrame == output.size(),
        "sample-rate replacement should reset frames and publish a new epoch/discontinuity");
}

void testSameTopologyTransitionCrossfadesWithoutCallbackAllocation()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(
        host.tryStage(fixedInputGainEngine(0.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "transition baseline should stage");
    std::array<float, 2> input{1.0F, 1.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    std::size_t allocations = 0;
    require(
        renderInputAndCount(
            host,
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size(),
            allocations)
            && allocations == 0,
        "transition baseline should activate without allocation");

    require(
        host.tryStageSameTopologyTransition(
            fixedInputGainEngine(1.0),
            4)
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "same-topology transition should stage with prepared scratch");
    require(
        renderInputAndCount(
            host,
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size(),
            allocations)
            && allocations == 0,
        "first transition partition should render without allocation");
    requireNear(output[0], 0.25F,
        "transition frame one should use the first linear crossfade step");
    requireNear(output[1], 0.5F,
        "transition frame two should reach the linear midpoint");
    require(host.sameTopologyTransitionActive()
            && host.sameTopologyTransitionCompletionToken() == 0,
        "transition should remain active until all requested frames render");
    require(
        host.tryStageSameTopologyTransition(
            fixedInputGainEngine(0.5),
            4)
            == talas::runtime::RealtimeGraphStageResult::
                PendingEngineAwaitingActivation,
        "host should bound transitions to one active candidate");

    require(
        renderInputAndCount(
            host,
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size(),
            allocations)
            && allocations == 0,
        "final transition partition should render without allocation");
    requireNear(output[0], 0.75F,
        "transition frame three should remain linear across callback partitions");
    requireNear(output[1], 1.0F,
        "transition frame four should land exactly on the candidate");
    require(!host.sameTopologyTransitionActive()
            && host.sameTopologyTransitionCompletionToken() == 1,
        "completed transition should publish one completion token");
    require(host.collectRetired() != nullptr,
        "old transition engine should retire only after crossfade completion");
}

void testZeroFrameSameTopologyTransitionUsesBoundarySwap()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(fixedInputGainEngine(0.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "zero-transition baseline should stage");
    std::array<float, 1> input{1.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 1> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(host.render(
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "zero-transition baseline should activate");
    require(host.tryStageSameTopologyTransition(
            fixedInputGainEngine(1.0),
            0)
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "zero-frame overlay should stage as an ordinary boundary swap");
    require(host.render(
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "zero-frame overlay should render");
    requireNear(output[0], 1.0F,
        "zero-frame overlay should expose the candidate at the callback boundary");
    require(!host.sameTopologyTransitionActive()
            && host.sameTopologyTransitionCompletionToken() == 0,
        "boundary swap should not publish a crossfade completion token");
}

void testSameTopologyTransitionIsBlockPartitionInvariant()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(host.tryStage(fixedInputGainEngine(0.0))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "partition-invariant baseline should stage");
    std::array<float, 2> input{1.0F, 1.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 2> scratch{};
    std::array<float*, 1> outputChannels{scratch.data()};
    require(host.render(
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            scratch.size()),
        "partition-invariant baseline should activate");
    require(host.tryStageSameTopologyTransition(
            fixedInputGainEngine(1.0),
            4)
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "partition-invariant candidate should stage");

    std::array<float, 4> sequence{};
    for (std::size_t frame = 0; frame < 2; ++frame)
    {
        outputChannels[0] = sequence.data() + frame;
        require(host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                1),
            "single-frame transition partition should render");
    }
    outputChannels[0] = sequence.data() + 2;
    require(host.render(
            {inputChannels.data(), inputChannels.size()},
            outputChannels.data(),
            outputChannels.size(),
            2),
        "two-frame transition partition should render");
    require(
        std::abs(sequence[0] - 0.25F) < 1.0e-6F
            && std::abs(sequence[1] - 0.5F) < 1.0e-6F
            && std::abs(sequence[2] - 0.75F) < 1.0e-6F
            && std::abs(sequence[3] - 1.0F) < 1.0e-6F,
        "transition samples must be invariant under one-one-two callback partitioning");
}

void testSameTopologyTransitionProjectsEventValuesToBothEngines()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto active = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        liveGraphFor(1.0));
    auto candidate = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        liveGraphFor(1.0));
    require(active->ready() && candidate->ready(),
        "transition Event Value engines should prepare");
    require(host.tryStage(std::move(active))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "transition Event Value baseline should stage");
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(host.render(
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "transition Event Value baseline should activate");
    require(host.tryStageSameTopologyTransition(
            std::move(candidate),
            4)
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "transition Event Value candidate should stage");
    require(host.render(
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "transition Event Value first partition should render");
    require(host.scheduleEventValue(
            "Gain",
            0.5,
            talas::expr::ValueDomain::Scalar,
            0),
        "Event Value should publish atomically to both transition engines");
    require(host.render(
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "transition Event Value final partition should render");
    require(
        std::abs(output[0] - 0.5F) < 1.0e-6F
            && std::abs(output[1] - 0.5F) < 1.0e-6F,
        "both transition sides should observe the same Event Value; actual="
            + std::to_string(output[0]) + ","
            + std::to_string(output[1]));
    auto retired = host.collectRetired();
    require(retired != nullptr,
        "transition Event Value baseline should retire");
    retired->setPlaying(true);
    require(retired->render(
            outputChannels.data(),
            outputChannels.size(),
            output.size()),
        "retired transition side should remain inspectable");
    requireNear(output[0], 0.5F,
        "retired transition side should prove it received the mirrored Event Value");
}

void testHostClockRemainsAuthoritativeAcrossTransientRenderFailure()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    auto engine = std::make_unique<talas::runtime::RealtimeAudioEngine>(audioInputGraph());
    require(engine->ready(), std::string(engine->error()));
    require(host.tryStage(std::move(engine)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "transport failure fixture should stage");

    std::array<float, 2> invalidInput{
        1.0F,
        std::numeric_limits<float>::quiet_NaN(),
    };
    std::array<const float*, 1> inputChannels{invalidInput.data()};
    std::array<float, 2> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(!host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
            "non-finite input should fail the callback");
    require(host.transportFramePosition() == 2,
            "playing host clock should still cover the failed device callback");
    require(host.transportSnapshot().absoluteFrame == 2,
        "failed callback should still publish the authoritative absolute endpoint");

    std::array<float, 2> validInput{3.0F, 4.0F};
    inputChannels[0] = validInput.data();
    require(host.render(
                {inputChannels.data(), inputChannels.size()},
                outputChannels.data(),
                outputChannels.size(),
                output.size()),
            "callback after transient failure should recover");
    requireNear(output[0], 3.0F, "recovered callback should render current input");
    require(host.transportFramePosition() == 4,
            "recovered session should synchronize to the authoritative host clock");
    require(host.transportSnapshot().absoluteFrame == 4,
        "recovered callback should continue the same absolute host epoch");
}

void testHostPublishesCoherentTransportSnapshotsToConcurrentReaders()
{
    talas::runtime::RealtimeGraphHost host;
    const talas::runtime::TransportLoopRegion loop{2, 6};
    require(
        host.tryStage(loopEngineFor(0.0, loop))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "coherent transport snapshot fixture should stage");
    host.setPlaying(true);

    std::atomic<bool> finished = false;
    std::atomic<bool> coherent = true;
    std::thread audioThread([&] {
        std::array<float, 1> output{};
        std::array<float*, 1> channels{output.data()};
        for (std::size_t callback = 0; callback < 4'000; ++callback)
        {
            if (!host.render(
                    channels.data(),
                    channels.size(),
                    output.size()))
            {
                coherent.store(false, std::memory_order_release);
                break;
            }
        }
        finished.store(true, std::memory_order_release);
    });

    while (!finished.load(std::memory_order_acquire))
    {
        const auto snapshot = host.transportSnapshot();
        if (snapshot.epoch == 0)
        {
            if (snapshot.absoluteFrame != 0
                || snapshot.playheadFrame != 0
                || snapshot.sampleRate != 0.0
                || snapshot.loopEnabled)
            {
                coherent.store(false, std::memory_order_release);
            }
            continue;
        }
        if (snapshot.epoch != 1
            || snapshot.sampleRate != 4.0
            || !snapshot.playing
            || !snapshot.loopEnabled
            || snapshot.loopStartFrame != loop.startFrame
            || snapshot.loopEndFrame != loop.endFrame
            || snapshot.playheadFrame
                != talas::runtime::transportPlayheadFrame(
                    snapshot.absoluteFrame,
                    snapshot))
        {
            coherent.store(false, std::memory_order_release);
        }
    }
    audioThread.join();
    require(coherent.load(std::memory_order_acquire),
        "concurrent readers should never observe fields from different transport publications");
}

void testStableMalformedMusicalPublicationFailsClosed()
{
    talas::runtime::RealtimeGraphHost host;
    require(
        host.tryStage(engineFor(0.0, 91))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "malformed musical publication fixture should stage");
    std::array<float, 1> output{};
    std::array<float*, 1> channels{output.data()};
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.musicalTransportSnapshot().publicationValid,
        "fixture should establish one valid stable fallback");

    talas::runtime::MtcTransportFollowerSnapshot mismatchedMtc;
    mismatchedMtc.identity = {
        .authorityGeneration = 5,
        .sourceGeneration = 6,
        .stageId = 90,
        .sourceSlot = 1,
    };
    require(talas::runtime::makeSmpteTimeCode(
                talas::runtime::SmpteRate::Fps25,
                0, 0, 1, 0,
                talas::runtime::SmpteDirection::Forward,
                mismatchedMtc.timeCode),
        "mismatched MTC fixture should create valid timecode");
    mismatchedMtc.state = talas::runtime::MtcTransportLockState::Locked;
    mismatchedMtc.hasTimeCode = true;
    mismatchedMtc.runningEvidence = true;
    host.setExternalMtc(mismatchedMtc, 25);
    (void)host.render(channels.data(), channels.size(), output.size());
    const auto rejectedMtc = host.musicalTransportSnapshot();
    require(!rejectedMtc.publicationValid
            && !rejectedMtc.usedStableFallback,
        "MTC evidence bound to a different active stage must fail closed");
    host.clearExternalMtc();
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.musicalTransportSnapshot().publicationValid,
        "clearing mismatched MTC should restore a valid internal publication");

    mismatchedMtc.identity.stageId = 91;
    mismatchedMtc.nextExpectedPiece = 0xff;
    host.setExternalMtc(mismatchedMtc, 25);
    (void)host.render(channels.data(), channels.size(), output.size());
    const auto rejectedProgress = host.musicalTransportSnapshot();
    require(!rejectedProgress.publicationValid
            && !rejectedProgress.usedStableFallback,
        "out-of-range MTC quarter-frame progress must fail closed");
    host.clearExternalMtc();
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.musicalTransportSnapshot().publicationValid,
        "clearing malformed MTC progress should restore internal publication");

    talas::runtime::MidiClockPhaseFollowerSnapshot malformed;
    malformed.state = static_cast<talas::runtime::MidiClockLockState>(0xff);
    host.setExternalMusicalTick(
        0,
        91,
        7,
        malformed,
        talas::runtime::MidiClockAnchorKind::Start,
        1.0);
    (void)host.render(channels.data(), channels.size(), output.size());
    const auto rejected = host.musicalTransportSnapshot();
    require(
        !rejected.publicationValid
            && !rejected.usedStableFallback
            && !rejected.externalAuthority,
        "stable malformed evidence must fail closed instead of replaying last-good external truth");
}

void testPreparedAudioBoundaryDefersLaterPendingStage()
{
    talas::runtime::RealtimeGraphHost host;
    require(
        host.tryStage(engineFor(0.0, 101))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "audio-boundary fixture should stage its incumbent");
    std::array<float, 1> output{};
    std::array<float*, 1> channels{output.data()};
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.activeStageId() == 101,
        "audio-boundary fixture should adopt its incumbent");

    host.prepareNextRenderBoundary();
    require(
        host.tryStage(engineFor(1.0, 102))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "a replacement may publish after the current audio boundary is frozen");
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.activeStageId() == 101,
        "the frozen callback must not adopt a stage published after its pre-render drain boundary");
    (void)host.render(channels.data(), channels.size(), output.size());
    require(host.activeStageId() == 102,
        "the following callback should adopt the deferred replacement");
}

void testPreparedStageAdoptsAtItsExactSplitFrame()
{
    talas::runtime::RealtimeGraphHost host;
    require(host.tryStage(engineFor(0.0, 301)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "split-adoption fixture should accept its incumbent");
    host.setPlaying(true);
    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(host.render(channels.data(), 1, 1), "incumbent should render before replacement");
    auto candidate = engineFor(1.0, 302);
    candidate->setPreparedStageTransportFrame(3);
    require(host.tryStage(std::move(candidate)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "a replacement prepared for a later frame should stage");
    (void)host.prepareNextRenderTransportBoundary();
    require(host.activeStageId() == 301 && host.pendingAdoptionFrame() == std::optional<std::uint64_t>{3}
            && host.pendingStageId() == 302,
        "a callback starting before the prepared frame keeps the incumbent and exposes the split frame");
    require(host.render(channels.data(), 1, 2), "the first split segment renders the incumbent");
    (void)host.prepareContinuationRenderTransportBoundary();
    require(host.activeStageId() == 302 && !host.pendingAdoptionFrame().has_value()
            && host.pendingStageId() == 0 && host.lastRejectedPreparedStageId() == 0,
        "the continuation boundary at the prepared frame adopts the replacement");
    require(host.render(channels.data(), 1, 2) && host.transportSnapshot().absoluteFrame == 5,
        "the second split segment renders the replacement");

    (void)host.collectRetired();
    auto late = engineFor(2.0, 303);
    late->setPreparedStageTransportFrame(4);
    require(host.tryStage(std::move(late)) == talas::runtime::RealtimeGraphStageResult::Accepted,
        "a replacement for an already passed frame still stages");
    require(host.render(channels.data(), 1, 1)
            && host.activeStageId() == 302 && host.lastRejectedPreparedStageId() == 303,
        "a replacement whose prepared frame was passed retires instead of waiting");
}

void testMissedPreparedStageRetiresWithoutBlockingRetry()
{
    for (const auto transition : {false, true})
    {
        talas::runtime::RealtimeGraphHost host;
        require(host.tryStage(engineFor(0.0, 201)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "missed-stage fixture should accept its incumbent");
        host.setPlaying(true);
        std::array<float, 1> output{};
        std::array<float*, 1> channels{output.data()};
        require(host.render(channels.data(), 1, 1), "incumbent should render before replacement");
        auto candidate = engineFor(1.0, 202);
        candidate->setPreparedStageTransportFrame(1);
        host.prepareNextRenderBoundary();
        const auto staged = transition
            ? host.tryStageSameTopologyTransition(std::move(candidate), 4)
            : host.tryStage(std::move(candidate));
        require(staged == talas::runtime::RealtimeGraphStageResult::Accepted,
            "guarded normal and transition candidates should publish after a frozen boundary");
        std::size_t allocations = 0;
        require(renderAndCount(host, channels.data(), 1, 1, allocations) && allocations == 0,
            "the frozen callback renders its pinned incumbent");
        require(renderAndCount(host, channels.data(), 1, 1, allocations) && allocations == 0,
            "stale prepared adoption renders the incumbent without allocation");
        require(host.activeStageId() == 201 && host.lastRejectedPreparedStageId() == 202
                && !host.sameTopologyTransitionActive() && host.transportSnapshot().absoluteFrame == 3,
            "rejection preserves transport and releases pending transition authority");
        require(host.realtimeMetricsRaw().stageRejections == 1
                && host.realtimeMetricsRaw().renderFailures == 0,
            "audio-boundary rejection is counted without becoming a render failure");
        require(host.tryStage({}) == talas::runtime::RealtimeGraphStageResult::NullEngine
                && host.realtimeMetricsRaw().stageRejections == 2,
            "control and audio rejection counters contribute independently to telemetry");
        (void)host.collectRetired();
        auto retry = engineFor(1.0, 203);
        retry->setPreparedStageTransportFrame(3);
        require(host.tryStage(std::move(retry)) == talas::runtime::RealtimeGraphStageResult::Accepted,
            "collection frees either retired ownership path for a fresh candidate");
        require(renderAndCount(host, channels.data(), 1, 1, allocations) && allocations == 0
                && host.activeStageId() == 203,
            "a matching prepared frame still activates normally");
        host.clearAfterAudioStopped();
        require(host.lastRejectedPreparedStageId() == 0,
            "clearing the host retires stale stage rejection identity");
    }
}

void testStableFallbackDoesNotCrossHostLifetimeAtSameAddress()
{
    alignas(talas::runtime::RealtimeGraphHost)
        std::byte storage[sizeof(talas::runtime::RealtimeGraphHost)];
    auto* first = std::construct_at(
        reinterpret_cast<talas::runtime::RealtimeGraphHost*>(storage));
    require(
        first->tryStage(engineFor(0.0, 111))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "fallback lifetime fixture should stage its first host");
    std::array<float, 1> output{};
    std::array<float*, 1> channels{output.data()};
    (void)first->render(channels.data(), channels.size(), output.size());
    talas::runtime::MidiClockPhaseFollowerSnapshot follower;
    first->setExternalMusicalTick(
        0,
        111,
        17,
        follower,
        talas::runtime::MidiClockAnchorKind::Start,
        1.0);
    (void)first->render(channels.data(), channels.size(), output.size());
    require(first->musicalTransportSnapshot().externalAuthority,
        "first host should establish external last-good evidence");
    std::destroy_at(first);

    auto* second = std::construct_at(
        reinterpret_cast<talas::runtime::RealtimeGraphHost*>(storage));
    talas::runtime::RealtimeGraphHostTestAccess::setTransportPublicationBusy(
        *second,
        true);
    const auto fallback = second->musicalTransportSnapshot();
    require(
        !fallback.publicationValid
            && fallback.usedStableFallback
            && !fallback.externalAuthority,
        "a same-address host must not inherit another lifetime's stable fallback");
    talas::runtime::RealtimeGraphHostTestAccess::setTransportPublicationBusy(
        *second,
        false);
    std::destroy_at(second);
}

void testHostPublishesTerminalMidiFromExactReplacementStage()
{
    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(
        host.tryStage(terminalEngineFor(51))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "first terminal MIDI engine should stage");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 51
            && output.midiEventCount == 1
            && host.transportFramePosition() == 4,
        "host should publish terminal MIDI from the adopted stage");

    require(
        host.tryStage(terminalEngineFor(52, true))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "replacement terminal MIDI engine should stage");
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 52
            && output.midiEventCount == 1
            && events[0].bytes[0] == 0xb2
            && events[0].bytes[1] == 10
            && events[0].bytes[2] == 127
            && host.activeStageId() == 52,
        "C40 replacement callback should publish only the new controller terminal stage");
    require(host.collectRetired() != nullptr,
        "terminal MIDI replacement should retire the prior stage");

    require(
        host.tryStage(noteTerminalEngineFor(53))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C41 replacement note terminal should stage");
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 53
            && output.midiEventCount == 1
            && events[0].bytes[0] == 0x91
            && events[0].bytes[1] == 69
            && events[0].bytes[2] == 127
            && host.activeStageId() == 53,
        "C41 replacement callback should publish only the note terminal stage");
    require(
        host.collectRetired() != nullptr,
        "C41 replacement should retire the controller stage before C42");

    host.requestTransportReset();
    require(
        host.tryStage(midiFileTerminalEngineFor(54))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C42 replacement MIDI file terminal should stage");
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 54
            && output.midiEventCount == 1
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 60
            && events[0].bytes[2] == 64
            && host.activeStageId() == 54,
        "C42 reset replacement should publish only the new SMF terminal stage");
    require(
        host.collectRetired() != nullptr,
        "C42 replacement should retire the note stage before C43");

    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 1>
        globalEvents{};
    std::array<char, 12> globalName{};
    output.globalControllerEvents = globalEvents;
    output.globalControllerName = globalName;
    host.requestTransportReset();
    require(
        host.tryStage(globalControllerTerminalEngineFor(
            55, "!HostGlobalA", "0.25"))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C43 first global-controller replacement should stage");
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 55
            && output.midiEventCount == 0
            && output.globalControllerEventCount == 1
            && output.globalControllerNameLength == globalName.size()
            && std::string_view{
                   globalName.data(), globalName.size()}
                == "!HostGlobalA"
            && globalEvents[0].frameOffset == 0
            && globalEvents[0].value == 0.25
            && host.activeStageId() == 55,
        "C43 host should publish only the adopted immutable global-controller authority");
    require(
        host.collectRetired() != nullptr,
        "C43 first global-controller replacement should retire C42");

    host.requestTransportReset();
    require(
        host.tryStage(globalControllerTerminalEngineFor(
            56, "!HostGlobalB", "0.75"))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "C43 second global-controller replacement should stage");
    require(
        host.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.stageId == 56
            && output.globalControllerEventCount == 1
            && output.globalControllerNameLength == globalName.size()
            && std::string_view{
                   globalName.data(), globalName.size()}
                == "!HostGlobalB"
            && globalEvents[0].frameOffset == 0
            && globalEvents[0].value == 0.75
            && host.activeStageId() == 56,
        "C43 replacement should publish only the new name, value, and stage");

    output.midiEventCount = 111;
    output.globalControllerEventCount = 112;
    output.globalControllerNameLength = 113;
    output.stageId = 114;
    require(
        host.renderTerminalEvents(0, output)
                == talas::runtime::RealtimeTerminalRenderResult::InvalidArguments
            && output.midiEventCount == 111
            && output.globalControllerEventCount == 112
            && output.globalControllerNameLength == 113
            && output.stageId == 114,
        "C43 host rejection should preserve caller publication metadata");
}
}

int main()
{
    testHostRunsPreparationAfterActivationBeforeRendering();
    testHostActivatesPreparedGraphsAtBlockBoundaries();
    testEmptyHostRendersSilenceWithoutAllocation();
    testGraphMetricsCountCallbacksFailuresAndStageRejections();
    testHostForwardsScheduledEventValuesToActiveAndPendingEngines();
    testHostAcceptsScheduleWhenOnlyPendingEngineHasBinding();
    testHostRejectsScheduleTransactionWhenOneTargetCannotAccept();
    testHostRejectsReprepareUpdateBeforeMutatingAnyGeneration();
    testHostDoesNotLoseSchedulesDuringPendingActivation();
    testHostForwardsCurrentAudioInputAcrossGraphReplacement();
    testHostTransportAdvancesOnlyWhilePlayingAndSurvivesReplacement();
    testHostPublishesLoopedTransportFromMonotonicClockWithoutAllocation();
    testHostAdoptsTransportResetAtCallbackBoundaryWithoutAllocation();
    testInternalContinuationDefersAConcurrentControlReset();
    testHostRelocatesAtCallbackBoundaryWithoutAllocation();
    testHostRejectsStageBoundRelocationAfterPendingMapReplacement();
    testM1UtilityChainAndTransportResetDoNotAllocate();
    testLoopWrapDoesNotResetDescriptorSelectedKernels();
    testPauseDoesNotResetDescriptorSelectedKernels();
    testGraphReplacementStartsFreshAtCurrentTransportSnapshot();
    testFeedbackStateSurvivesLoopAndPauseButNotGraphReplacement();
    testMixingNodeGraphReplacementsKeepStereoOrderWithoutAllocation();
    testSameTopologyTransitionCrossfadesWithoutCallbackAllocation();
    testZeroFrameSameTopologyTransitionUsesBoundarySwap();
    testSameTopologyTransitionIsBlockPartitionInvariant();
    testSameTopologyTransitionProjectsEventValuesToBothEngines();
    testHostStartsNewTransportEpochWhenReplacementSampleRateChanges();
    testHostClockRemainsAuthoritativeAcrossTransientRenderFailure();
    testHostPublishesCoherentTransportSnapshotsToConcurrentReaders();
    testStableMalformedMusicalPublicationFailsClosed();
    testPreparedAudioBoundaryDefersLaterPendingStage();
    testMissedPreparedStageRetiresWithoutBlockingRetry();
    testPreparedStageAdoptsAtItsExactSplitFrame();
    testStableFallbackDoesNotCrossHostLifetimeAtSameAddress();
    testHostPublishesTerminalMidiFromExactReplacementStage();
}
