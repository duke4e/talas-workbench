#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeOutputRecorder.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
static_assert(noexcept(std::declval<talas::runtime::RealtimeAudioEngine&>().scheduleEventValues(
    std::declval<std::span<const talas::runtime::RealtimeEventValueUpdate>>(), 0)));
static_assert(noexcept(std::declval<talas::runtime::RealtimeAudioEngine&>().scheduleMidiNote(
    std::declval<talas::runtime::RealtimeMidiNoteEvent>(), 0)));

std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(float actual, float expected, float tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCounter()
    {
        countAllocations.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t count() const
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {std::move(fromNode), std::move(fromPort), std::move(toNode), std::move(toPort)};
}

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::runtime::PreparedRuntimeGraph compileMidiVoiceGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("midi", "talas.midiVoice", {
        {"left", "1"}, {"right", "1"}, {"channel", "2"},
        textParameter("source", ""), resourceParameter("midiFile", ""),
        {"polyphony", "2"}, {"lowPitch", "48 nn"}, {"highPitch", "72 nn"},
        textParameter("script", ""), booleanParameter("shared", false),
    }));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {
        {"frequency", "!Pitch"}, {"amplitude", "!KeyDown * !KeyVelocity"},
    }));
    document.graph.connections.push_back(connection("osc", "audioOut", "midi", "audioIn"));
    document.graph.connections.push_back(connection("midi", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("midi", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(compiled.ok(), "MIDI Voice engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileOscillatorVcaGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.nodes.push_back(node("gain", "talas.constant", {{"value", "1"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 2});
    require(compiled.ok(), "audio engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileStereoOscillatorGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("left", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.nodes.push_back(node("right", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.5"}}));
    document.graph.connections.push_back(connection("left", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("right", "audioOut", "out", "rightIn"));

    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 2});
    require(compiled.ok(), "stereo audio engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileRawMidiTerminalGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 64 16r7f")}));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "terminal MIDI engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMidiControllerTerminalGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.midiOutputController",
        {
            {"channel", "3"},
            {"controllerNumber", "10"},
            textParameter("value", "127"),
        }));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(
        compiled.ok(),
        "MIDI controller engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMidiNoteTerminalGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.midiOutputEvent",
        {
            {"frequency", "440 hz"},
            {"amplitude", "1"},
            {"channel", "2"},
            {"gate", "1"},
        }));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(compiled.ok(), "MIDI note engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileMidiFileTerminalGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.midiFileEcho",
        {
            {"lowChannel", "1"},
            {"highChannel", "16"},
            resourceParameter("fileName", "Sequences/C42.mid"),
        }));
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
    require(compiled.ok(), "MIDI file engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileGlobalControllerTerminalGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "terminal",
        "talas.soundToGlobalController",
        {
            textParameter("generatedEvent", "!EngineGlobal"),
            textParameter("value", "0.625"),
        }));
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {8.0, 4});
    require(
        compiled.ok(),
        "global-controller engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileDiskRecorderGraph(
    std::size_t maxBlockFrames = 2)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("recorder", "talas.diskRecorder", {
        resourceParameter("fileName", "Recordings/Engine Capture"),
        textParameter("format", "WAV"),
        textParameter("wordSize", "16"),
        choiceParameter("channels", "mono"),
        {"trigger", "1"},
        {"captureDuration", "0.5 s"},
        booleanParameter("gated", false),
    }));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {
        {"frequency", "0.25 hz"}, {"amplitude", "1"},
    }));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "recorder", "audioIn"));

    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {4.0, maxBlockFrames});
    require(compiled.ok(), "Disk Recorder engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileNonFiniteVcaGainGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.nodes.push_back(node("gainLeft", "talas.constant", {{"value", "1e308"}}));
    document.graph.nodes.push_back(node("gainRight", "talas.constant", {{"value", "1e308"}}));
    document.graph.nodes.push_back(node("gain", "talas.product"));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gainLeft", "value", "gain", "left"));
    document.graph.connections.push_back(connection("gainRight", "value", "gain", "right"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 2});
    require(compiled.ok(), "non-finite gain fixture should compile before realtime rendering");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileLiveEventValueGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.nodes.push_back(node("gain", "talas.constant", {{"value", "!Gain"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings{4.0, 2};
    settings.eventValues.push_back({"Gain", 0.5, talas::expr::ValueDomain::Scalar});
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(compiled.ok(), "live Event Value engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileScheduledEventValueGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("source", "talas.noise", {
        {"initialState", "1"},
        {"frequency", "0 hz"},
        {"centerValue", "0"},
        {"scale", "1"},
        choiceParameter("type", "hotPink"),
    }));
    document.graph.nodes.push_back(node("gain", "talas.constant", {{"value", "!Gain"}}));
    document.graph.connections.push_back(connection("source", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings{4.0, 2};
    settings.eventValues.push_back({"Gain", 1.0, talas::expr::ValueDomain::Scalar});
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(compiled.ok(), "scheduled Event Value engine fixture should compile");
    return std::move(*compiled.graph);
}

talas::runtime::PreparedRuntimeGraph compileAudioInputGraph()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("input", "talas.audioInput"));
    document.graph.connections.push_back(connection("input", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("input", "rightOut", "out", "rightIn"));
    auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {4.0, 2});
    require(compiled.ok(), "audio input engine fixture should compile");
    return std::move(*compiled.graph);
}

bool renderAndCount(
    talas::runtime::RealtimeAudioEngine& engine,
    float* const* channels,
    std::size_t channelCount,
    std::size_t frameCount,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto rendered = engine.render(channels, channelCount, frameCount);
    allocations = counter.count();
    return rendered;
}

void requireSilent(const float* samples, std::size_t frameCount, const std::string& message)
{
    for (std::size_t frame = 0; frame < frameCount; ++frame)
        requireNear(samples[frame], 0.0F, 1.0e-6F, message);
}

void testAudioEngineRendersMonoToStereoWithoutAllocations()
{
    auto graph = compileOscillatorVcaGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> left{4.0F, 4.0F};
    std::array<float, 2> right{5.0F, 5.0F};
    std::array<float, 2> extra{6.0F, 6.0F};
    std::array<float*, 3> channels{left.data(), right.data(), extra.data()};
    std::size_t allocations = 0;

    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "audio engine should render a valid block");
    require(allocations == 0, "audio engine callback should not allocate");
    requireNear(left[0], 0.0F, 1.0e-6F, "left frame 0");
    requireNear(left[1], 1.0F, 1.0e-6F, "left frame 1");
    requireNear(right[0], 0.0F, 1.0e-6F, "right frame 0");
    requireNear(right[1], 1.0F, 1.0e-6F, "right frame 1");
    requireSilent(extra.data(), extra.size(), "extra output channel should be cleared");
}

void testAudioEngineRendersStereoGraphWithoutAllocations()
{
    auto graph = compileStereoOscillatorGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> left{};
    std::array<float, 2> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    std::size_t allocations = 0;
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "stereo graph should render");
    require(allocations == 0, "stereo graph render should not allocate");
    requireNear(left[0], 0.0F, 1.0e-6F, "stereo left frame 0");
    requireNear(left[1], 1.0F, 1.0e-6F, "stereo left frame 1");
    requireNear(right[0], 0.0F, 1.0e-6F, "stereo right frame 0");
    requireNear(right[1], 0.5F, 1.0e-6F, "stereo right frame 1");
}

void testAudioEngineRendersBoundedDiskRecorderPreviewAndResetWithoutAllocations()
{
    auto graph = compileDiskRecorderGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string{engine.error()});
    engine.setPlaying(true);

    std::array<float, 2> first{};
    std::array<float, 2> second{};
    std::array<float*, 1> firstChannels{first.data()};
    std::array<float*, 1> secondChannels{second.data()};
    std::size_t allocations = 0;
    require(renderAndCount(
                engine, firstChannels.data(), firstChannels.size(),
                first.size(), allocations),
        "Disk Recorder first engine callback should render");
    require(allocations == 0,
        "Disk Recorder first engine callback should not allocate");
    require(renderAndCount(
                engine, secondChannels.data(), secondChannels.size(),
                second.size(), allocations),
        "Disk Recorder second engine callback should render");
    require(allocations == 0,
        "Disk Recorder second engine callback should not allocate");
    requireNear(first[0], 0.0F, 1.0e-6F,
        "Disk Recorder bounded engine frame zero");
    requireNear(first[1], static_cast<float>(std::sin(std::acos(-1.0) / 8.0)),
        1.0e-6F, "Disk Recorder bounded engine frame one");
    requireNear(second[0], 0.0F, 1.0e-6F,
        "Disk Recorder duration should silence frame two");
    requireNear(second[1], 0.0F, 1.0e-6F,
        "Disk Recorder duration should silence frame three");

    engine.resetTransport();
    first.fill(-99.0F);
    require(renderAndCount(
                engine, firstChannels.data(), firstChannels.size(),
                first.size(), allocations),
        "reset Disk Recorder engine callback should render");
    require(allocations == 0,
        "reset Disk Recorder engine callback should not allocate");
    requireNear(first[0], 0.0F, 1.0e-6F,
        "reset Disk Recorder frame zero");
    requireNear(first[1], static_cast<float>(std::sin(std::acos(-1.0) / 8.0)),
        1.0e-6F, "reset Disk Recorder frame one");
}

void testAudioEngineRendersRepeatedMaximumDiskRecorderCallbacksWithoutAllocations()
{
    constexpr auto maximumFrames =
        talas::runtime::maximumRealtimeOutputRecorderBlockFrames;
    auto graph = compileDiskRecorderGraph(maximumFrames);
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string{engine.error()});
    engine.setPlaying(true);

    std::vector<float> output(maximumFrames);
    std::array<float*, 1> channels{output.data()};
    for (std::size_t callback = 0; callback < 64; ++callback)
    {
        std::size_t allocations = 0;
        require(renderAndCount(
                    engine,
                    channels.data(),
                    channels.size(),
                    output.size(),
                    allocations),
            "maximum Disk Recorder callback should render");
        require(allocations == 0,
            "maximum Disk Recorder callback should not allocate");
        require(std::ranges::all_of(output, [](float sample) {
            return std::isfinite(sample);
        }), "maximum Disk Recorder callback should remain finite");
    }
}

void testAudioEngineStopsAndRejectsOversizedBlocksWithoutAdvancingState()
{
    auto graph = compileOscillatorVcaGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));

    std::array<float, 2> left{4.0F, 4.0F};
    std::array<float, 2> right{5.0F, 5.0F};
    std::array<float*, 2> channels{left.data(), right.data()};
    std::size_t allocations = 0;

    require(!renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "stopped engine should not render");
    require(allocations == 0, "stopped callback should not allocate");
    requireSilent(left.data(), left.size(), "stopped left channel should be silent");
    requireSilent(right.data(), right.size(), "stopped right channel should be silent");

    engine.setPlaying(true);
    std::array<float, 3> oversized{};
    std::array<float*, 1> oversizedChannels{oversized.data()};
    require(!renderAndCount(engine, oversizedChannels.data(), oversizedChannels.size(), oversized.size(), allocations), "oversized block should not render");
    require(allocations == 0, "oversized callback should not allocate");
    requireSilent(oversized.data(), oversized.size(), "oversized output should be silent");

    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "valid block should render after rejected blocks");
    require(allocations == 0, "valid callback should not allocate");
    requireNear(left[0], 0.0F, 1.0e-6F, "rejected blocks should not advance oscillator phase");
}

void testAudioEngineIgnoresNullOutputChannels()
{
    auto graph = compileOscillatorVcaGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> right{};
    std::array<float*, 2> channels{nullptr, right.data()};
    std::size_t allocations = 0;

    require(renderAndCount(engine, channels.data(), channels.size(), right.size(), allocations), "null left channel should not block right output");
    require(allocations == 0, "null channel callback should not allocate");
    requireNear(right[0], 0.0F, 1.0e-6F, "right frame 0 with null left channel");
    requireNear(right[1], 1.0F, 1.0e-6F, "right frame 1 with null left channel");
}

void testOrderedAudioEngineDoesNotRerouteMissingMonoDestination()
{
    auto graph = compileOscillatorVcaGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> secondDestination{9.0F, 9.0F};
    std::array<float*, 2> channels{nullptr, secondDestination.data()};
    std::size_t allocations = 0;
    bool rendered = true;
    {
        AllocationCounter counter;
        rendered = engine.renderOrdered(
            {},
            channels.data(),
            channels.size(),
            secondDestination.size());
        allocations = counter.count();
    }
    require(!rendered,
            "ordered output must discard a missing mono ordinal instead of using ordinal one");
    require(allocations == 0,
            "ordered missing-destination handling should not allocate");
    requireSilent(
        secondDestination.data(),
        secondDestination.size(),
        "ordered output must clear an untargeted second destination");
}

void testAudioEngineSilencesRejectedControlBlocksWithoutAllocations()
{
    auto graph = compileNonFiniteVcaGainGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> left{4.0F, 4.0F};
    std::array<float, 2> right{5.0F, 5.0F};
    std::array<float*, 2> channels{left.data(), right.data()};
    std::size_t allocations = 0;

    require(!renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations),
            "engine should reject a non-finite control block");
    require(allocations == 0, "rejected control block should not allocate");
    requireSilent(left.data(), left.size(), "rejected control block should clear left output");
    requireSilent(right.data(), right.size(), "rejected control block should clear right output");
}

void testAudioEngineUpdatesLiveEventValuesBetweenCallbacks()
{
    auto graph = compileLiveEventValueGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "initial live Event Value engine block should render");
    require(allocations == 0, "initial live Event Value engine block should not allocate");
    requireNear(left[1], 0.5F, 1.0e-6F, "initial live Event Value engine gain");

    require(engine.updateEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar),
            "engine should accept a known live Event Value update");
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations), "updated live Event Value engine block should render");
    require(allocations == 0, "updated live Event Value engine block should not allocate");
    requireNear(left[1], -0.25F, 1.0e-6F, "updated live Event Value engine gain");
}

void testAudioEngineSchedulesLiveEventValuesInsideNextCallback()
{
    auto graph = compileScheduledEventValueGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);
    require(engine.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "audio engine should forward a scheduled Event Value");

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations),
            "scheduled Event Value audio block should render");
    require(allocations == 0, "scheduled Event Value audio block should not allocate");
    requireNear(left[0], 1.0F, 1.0e-6F, "scheduled Event Value engine frame zero");
    requireNear(left[1], 0.25F, 1.0e-6F, "scheduled Event Value engine frame one");
}

void testAudioEngineRendersBorrowedInputAndHandlesAliasing()
{
    auto graph = compileAudioInputGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    engine.setPlaying(true);

    std::array<float, 2> input{1.0F, 2.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 2> left{};
    std::array<float, 2> right{};
    std::array<float, 2> extra{9.0F, 9.0F};
    std::array<float*, 3> outputs{left.data(), right.data(), extra.data()};
    std::size_t allocations = 0;
    bool rendered = false;
    {
        AllocationCounter counter;
        rendered = engine.render({inputChannels.data(), inputChannels.size()}, outputs.data(), outputs.size(), left.size());
        allocations = counter.count();
    }
    require(rendered, "audio engine should render borrowed hardware input");
    require(allocations == 0, "audio engine input callback should not allocate");
    requireNear(left[0], 1.0F, 1.0e-6F, "audio engine input left frame zero");
    requireNear(right[1], 2.0F, 1.0e-6F, "audio engine input right frame one");
    requireNear(extra[0], 0.0F, 1.0e-6F, "audio engine should clear extra output channels");

    std::array<float, 2> aliased{3.0F, 4.0F};
    inputChannels[0] = aliased.data();
    std::array<float*, 1> aliasedOutput{aliased.data()};
    require(engine.render({inputChannels.data(), inputChannels.size()}, aliasedOutput.data(), aliasedOutput.size(), aliased.size()),
            "audio engine should support aliased full-duplex input and output storage");
    requireNear(aliased[0], 3.0F, 1.0e-6F, "aliased audio input frame zero");
    requireNear(aliased[1], 4.0F, 1.0e-6F, "aliased audio input frame one");

    left.fill(7.0F);
    std::array<float*, 1> missingOutput{left.data()};
    require(!engine.render({}, missingOutput.data(), missingOutput.size(), left.size()),
            "missing selected engine input should reject the callback");
    requireNear(left[0], 0.0F, 1.0e-6F, "rejected engine input should clear output");
}

void testAudioEngineForwardsAtomicEventValueBatches()
{
    auto graph = compileLiveEventValueGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    engine.setPlaying(true);
    const std::array<talas::runtime::RealtimeEventValueUpdate, 1> updates{{
        {"Gain", 0.25, talas::expr::ValueDomain::Scalar},
    }};
    require(engine.scheduleEventValues(updates, 1), "audio engine should forward an atomic Event Value batch");

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations),
            "audio engine batch should render");
    require(allocations == 0, "audio engine batch render should not allocate");
    requireNear(left[0], 0.0F, 1.0e-6F, "audio engine batch frame zero");
    requireNear(left[1], 0.25F, 1.0e-6F, "audio engine batch exact offset");
}

void testAudioEngineForwardsPreparedMidiNotesWithoutAllocations()
{
    auto graph = compileMidiVoiceGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setPlaying(true);
    require(engine.scheduleMidiNote(
                {talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 0)
            == talas::runtime::RealtimeMidiDispatchResult::NotHandled,
            "engine should preserve prepared MIDI channel filtering");

    talas::runtime::RealtimeMidiDispatchResult dispatch{};
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        dispatch = engine.scheduleMidiNote(
            {talas::runtime::RealtimeMidiNoteKind::NoteOn, 2, 60, 127}, 0);
        allocations = counter.count();
    }
    require(dispatch == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "engine should forward a matching prepared MIDI note");
    require(allocations == 0, "engine MIDI forwarding should not allocate");

    std::array<float, 16> left{};
    std::array<float, 16> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    require(renderAndCount(engine, channels.data(), channels.size(), left.size(), allocations),
            "engine should render its forwarded prepared MIDI note");
    require(allocations == 0, "engine prepared MIDI render should not allocate");
    require(std::ranges::any_of(std::span<const float>(left).subspan(5), [](float sample) {
                return std::abs(sample) > 1.0e-6F;
            }),
            "forwarded prepared MIDI note should sound after its delayed attack");
}

void testAudioEngineBindsPreparedNodeTimingToExactStage()
{
    auto graph = compileOscillatorVcaGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string(engine.error()));
    engine.setStageId(73);
    engine.setPlaying(true);

    std::array<float, 2> left{};
    std::array<float*, 1> channels{left.data()};
    std::size_t allocations = 0;
    require(renderAndCount(
                engine,
                channels.data(),
                channels.size(),
                left.size(),
                allocations),
        "stage-bound timing fixture should render");
    require(allocations == 0,
        "stage-bound timing publication must not allocate");

    const auto timing = engine.preparedNodeTiming();
    require(timing.stageId == 73,
        "prepared node timing must carry the exact engine stage");
    require(!timing.records.empty()
            && std::ranges::count_if(
                timing.records,
                [](const auto& record) {
                    return record.publicationValid;
                }) == 1,
        "one callback should publish exactly one round-robin node sample");
}

void testAudioEngineForwardsBoundedTerminalMidiWithStageIdentity()
{
    auto graph = compileRawMidiTerminalGraph();
    talas::runtime::RealtimeAudioEngine engine(graph);
    require(engine.ready(), std::string{engine.error()});
    engine.setStageId(41);
    engine.setPlaying(true);
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    std::size_t allocations = 0;
    talas::runtime::RealtimeTerminalRenderResult result;
    {
        AllocationCounter counter;
        result = engine.renderTerminalEvents(4, output);
        allocations = counter.count();
    }
    require(
        result == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && allocations == 0 && output.stageId == 41
            && output.midiEventCount == 1
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 64
            && events[0].bytes[2] == 0x7f,
        "terminal MIDI engine should preserve bounded bytes and exact stage: allocations="
            + std::to_string(allocations)
            + " stage=" + std::to_string(output.stageId)
            + " count=" + std::to_string(output.midiEventCount)
            + " frame=" + std::to_string(events[0].frameOffset)
            + " bytes=" + std::to_string(events[0].byteCount)
            + "/" + std::to_string(events[0].bytes[0])
            + "/" + std::to_string(events[0].bytes[1])
            + "/" + std::to_string(events[0].bytes[2]));
    std::array<float, 4> audio{};
    std::array<float*, 1> channels{audio.data()};
    require(
        !engine.render(channels.data(), channels.size(), audio.size()),
        "terminal MIDI engine should reject audio rendering");

    auto controllerGraph = compileMidiControllerTerminalGraph();
    talas::runtime::RealtimeAudioEngine controller(controllerGraph);
    require(controller.ready(), std::string{controller.error()});
    controller.setStageId(42);
    controller.setPlaying(true);
    {
        AllocationCounter counter;
        result = controller.renderTerminalEvents(4, output);
        allocations = counter.count();
    }
    require(
        result == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && allocations == 0
            && output.stageId == 42
            && output.midiEventCount == 1
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0xb2
            && events[0].bytes[1] == 10
            && events[0].bytes[2] == 127,
        "C40 controller engine should preserve bounded CC bytes and exact stage");

    auto noteGraph = compileMidiNoteTerminalGraph();
    talas::runtime::RealtimeAudioEngine note(noteGraph);
    require(note.ready(), std::string{note.error()});
    note.setStageId(43);
    note.setPlaying(true);
    {
        AllocationCounter counter;
        result = note.renderTerminalEvents(4, output);
        allocations = counter.count();
    }
    require(
        result == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && allocations == 0
            && output.stageId == 43
            && output.midiEventCount == 1
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0x91
            && events[0].bytes[1] == 69
            && events[0].bytes[2] == 127,
        "C41 note engine should preserve bounded note bytes and exact stage");

    auto midiFileGraph = compileMidiFileTerminalGraph();
    talas::runtime::RealtimeAudioEngine midiFile(midiFileGraph);
    require(midiFile.ready(), std::string{midiFile.error()});
    midiFile.setStageId(44);
    midiFile.setPlaying(true);
    {
        AllocationCounter counter;
        result = midiFile.renderTerminalEvents(4, output);
        allocations = counter.count();
    }
    require(
        result == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && allocations == 0
            && output.stageId == 44
            && output.midiEventCount == 1
            && events[0].frameOffset == 0
            && events[0].byteCount == 3
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 60
            && events[0].bytes[2] == 64,
        "C42 MIDI file engine should preserve exact scheduled bytes and stage");

    auto globalGraph = compileGlobalControllerTerminalGraph();
    talas::runtime::RealtimeAudioEngine global(globalGraph);
    require(global.ready(), std::string{global.error()});
    global.setStageId(45);
    global.setPlaying(true);
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 1>
        globalEvents{};
    std::array<char, 13> globalName{};
    talas::runtime::RealtimeTerminalOutputBlock globalOutput{};
    globalOutput.globalControllerEvents = globalEvents;
    globalOutput.globalControllerName = globalName;
    {
        AllocationCounter counter;
        result = global.renderTerminalEvents(4, globalOutput);
        allocations = counter.count();
    }
    require(
        result == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && allocations == 0
            && globalOutput.stageId == 45
            && globalOutput.midiEventCount == 0
            && globalOutput.globalControllerEventCount == 1
            && globalOutput.globalControllerNameLength
                == globalName.size()
            && std::string_view{
                   globalName.data(), globalName.size()}
                == "!EngineGlobal"
            && globalEvents[0].frameOffset == 0
            && globalEvents[0].value == 0.625,
        "C43 engine should preserve exact caller-owned global-controller event, name, and stage");

    globalOutput.midiEventCount = 101;
    globalOutput.globalControllerEventCount = 102;
    globalOutput.globalControllerNameLength = 103;
    globalOutput.stageId = 104;
    require(
        global.renderTerminalEvents(0, globalOutput)
                == talas::runtime::RealtimeTerminalRenderResult::InvalidArguments
            && globalOutput.midiEventCount == 101
            && globalOutput.globalControllerEventCount == 102
            && globalOutput.globalControllerNameLength == 103
            && globalOutput.stageId == 104,
        "C43 engine rejection should preserve caller publication metadata");
}
}

int main()
{
    testAudioEngineRendersMonoToStereoWithoutAllocations();
    testAudioEngineRendersStereoGraphWithoutAllocations();
    testAudioEngineRendersBoundedDiskRecorderPreviewAndResetWithoutAllocations();
    testAudioEngineRendersRepeatedMaximumDiskRecorderCallbacksWithoutAllocations();
    testAudioEngineStopsAndRejectsOversizedBlocksWithoutAdvancingState();
    testAudioEngineIgnoresNullOutputChannels();
    testOrderedAudioEngineDoesNotRerouteMissingMonoDestination();
    testAudioEngineSilencesRejectedControlBlocksWithoutAllocations();
    testAudioEngineUpdatesLiveEventValuesBetweenCallbacks();
    testAudioEngineSchedulesLiveEventValuesInsideNextCallback();
    testAudioEngineForwardsAtomicEventValueBatches();
    testAudioEngineForwardsPreparedMidiNotesWithoutAllocations();
    testAudioEngineBindsPreparedNodeTimingToExactStage();
    testAudioEngineForwardsBoundedTerminalMidiWithStageIdentity();
    testAudioEngineRendersBorrowedInputAndHandlesAliasing();
}
