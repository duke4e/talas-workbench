#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
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
    std::vector<ProjectParameterExpression> parameters = {})
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

ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

// These fixtures send their pairs in hertz, as projects before format 93
// did (`pairFrequencies` `legacyHertz`).
ProjectParameterExpression legacyHertzPairs()
{
    ProjectParameterExpression parameter;
    parameter.parameterId = "pairFrequencies";
    parameter.choice = "legacyHertz";
    return parameter;
}

ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

ProjectParameterExpression textParameter(std::string id, std::string text)
{
    ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectDocument sourceFrameDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("source", "talas.syntheticSpectrumFromArray", {
            legacyHertzPairs(),
            {"nbrPartials", "3"},
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", false),
            {"envelope", "1"},
            textParameter("amplitudes", "0.25 0.5 0.75"),
            textParameter("frequencies", "0 0 0"),
            textParameter("bandwidths", "0 0 0"),
        }),
        node("bank", "talas.oscillatorBank", {
            legacyHertzPairs(),
            {"nbrOscillators", "1"},
            {"bankSize", "1"},
            resourceParameter("wavetable", "Flat"),
        }),
    };
    document.graph.connections = {
        connection("source", "amplitudeOut", "bank", "amplitudeIn"),
        connection("source", "frequencyOut", "bank", "frequencyIn"),
        connection("bank", "audioOut", "out", "audioIn"),
    };
    return document;
}

void setParameter(
    talas::project::ProjectNode& target,
    std::string_view id,
    std::string value)
{
    const auto found = std::ranges::find(
        target.parameters, id,
        &talas::project::ProjectParameterExpression::parameterId);
    require(found != target.parameters.end(),
        "fixture should retain parameter " + std::string{id});
    found->expression = std::move(value);
}

void setTextParameter(
    talas::project::ProjectNode& target,
    std::string_view id,
    std::string value)
{
    const auto found = std::ranges::find(
        target.parameters, id,
        &talas::project::ProjectParameterExpression::parameterId);
    require(found != target.parameters.end(),
        "fixture should retain text parameter " + std::string{id});
    found->text = std::move(value);
}

talas::project::ProjectNode& projectNode(
    talas::project::ProjectDocument& document,
    std::string_view id)
{
    const auto found = std::ranges::find(
        document.graph.nodes, id,
        &talas::project::ProjectNode::instanceId);
    require(found != document.graph.nodes.end(),
        "fixture should retain node " + std::string{id});
    return *found;
}

talas::project::ProjectDocument soundsBankDocument(
    bool withDownstreamGain = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("amps", "talas.audioInput"),
        node("freqs", "talas.audioInput", {
            booleanParameter("channel1", false),
            booleanParameter("channel2", true)}),
        node("count", "talas.constant", { {"value", "!BankSourceN"} }),
        node("source", "talas.syntheticSpectrumFromSounds", {
            legacyHertzPairs(),
            {"nbrPartials", "1"},
            booleanParameter("logScale", false),
            booleanParameter("includesBandwidths", false),
        }),
        node("bank", "talas.oscillatorBank", {
            legacyHertzPairs(),
            {"nbrOscillators", "1"},
            {"bankSize", "1"},
            resourceParameter("wavetable", "Flat"),
        }),
    };
    document.graph.connections = {
        connection("count", "value", "source", "nbrPartials"),
        connection("amps", "audioOut", "source", "amplitudes"),
        connection("freqs", "audioOut", "source", "frequenciesAndBandwidths"),
        connection("source", "amplitudeOut", "bank", "amplitudeIn"),
        connection("source", "frequencyOut", "bank", "frequencyIn"),
    };
    if (withDownstreamGain)
    {
        document.graph.nodes.push_back(
            node("gain", "talas.constant", { {"value", "!BankFailureGain"} }));
        document.graph.nodes.push_back(node("vca", "talas.vca"));
        document.graph.connections.push_back(
            connection("bank", "audioOut", "vca", "audioIn"));
        document.graph.connections.push_back(
            connection("gain", "value", "vca", "gain"));
        document.graph.connections.push_back(
            connection("vca", "audioOut", "out", "audioIn"));
    }
    else
    {
        document.graph.connections.push_back(
            connection("bank", "audioOut", "out", "audioIn"));
    }
    return document;
}

talas::project::ProjectDocument inRamBankDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("first", "talas.constant", { {"value", "!RamFirst"} }),
        node("source", "talas.spectrumInRam", {
            legacyHertzPairs(),
            {"frequency", "1 hz"},
            {"level", "1"},
            {"timeIndex", "0"},
            resourceParameter("analysis", "Analysis/RAM"),
            {"analysisFrames", "1"},
            {"nbrPartials", "0"},
            {"firstPartial", "2"},
        }),
        node("bank", "talas.oscillatorBank", {
            legacyHertzPairs(),
            {"nbrOscillators", "1"},
            {"bankSize", "1"},
            resourceParameter("wavetable", "Quarter"),
        }),
    };
    document.graph.connections = {
        connection("first", "value", "source", "firstPartial"),
        connection("source", "amplitudeOut", "bank", "amplitudeIn"),
        connection("source", "frequencyOut", "bank", "frequencyIn"),
        connection("bank", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::runtime::RenderSettings settings()
{
    talas::runtime::RenderSettings result{16.0, 31};
    result.tableResources = {{"Flat", {1.0}}, {"Quarter", {0.0, 1.0, 0.0, -1.0}}};
    result.eventValues = {
        {"BankSourceN", 3.0, talas::expr::ValueDomain::Scalar},
        {"BankFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
        {"RamFirst", 2.0, talas::expr::ValueDomain::Scalar},
    };
    return result;
}

talas::runtime::PreparedRuntimeGraph compile(
    const talas::project::ProjectDocument& document,
    const talas::runtime::RenderSettings& renderSettings)
{
    auto result = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), renderSettings);
    require(result.ok(), "source-frame fixture should compile");
    return std::move(*result.graph);
}

bool render(
    talas::runtime::RealtimeRenderSession& session, std::span<double> output,
    std::span<const float> amplitudes = {}, std::span<const float> frequencies = {})
{
    const std::array<const float*, 2> channels{amplitudes.data(), frequencies.data()};
    bool rendered = false;
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        rendered = amplitudes.empty()
            ? session.render(output)
            : session.render({channels.data(), channels.size()}, output, {});
        allocations = counter.count();
    }
    require(allocations == 0, "source-frame callback must allocate nothing");
    return rendered;
}

void reset(talas::runtime::RealtimeRenderSession& session)
{
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        session.resetTransport();
        allocations = counter.count();
    }
    require(allocations == 0, "source-frame reset must allocate nothing");
}

void update(talas::runtime::RealtimeRenderSession& session, std::string_view name, double value)
{
    require(session.updateEventValue(name, value, talas::expr::ValueDomain::Scalar),
        "source-frame control update should succeed");
}

// Independent piecewise triangle for the four-sample Quarter table at Fs=16.
double quarter(std::size_t frame, double frequency)
{
    const auto position = std::fmod(static_cast<double>(frame) * frequency, 16.0) / 4.0;
    return position <= 1.0 ? position : (position <= 3.0 ? 2.0 - position : position - 4.0);
}

// The Quarter table read at a phase in cycles, interpolated as the bank does.
double quarterAtPhase(double phase)
{
    constexpr std::array<double, 4> table{0.0, 1.0, 0.0, -1.0};
    const auto scaled = (phase - std::floor(phase)) * 4.0;
    const auto left = static_cast<std::size_t>(std::floor(scaled)) % 4;
    return table[left] + (table[(left + 1) % 4] - table[left]) * (scaled - std::floor(scaled));
}

void testSourceFrameCountDoesNotCollapseToSelectedBankCount()
{
    constexpr std::size_t frames = 39;
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 16.0;
    settings.maxBlockFrames = 31;
    settings.tableResources.push_back({"Flat", {1.0}});

    const auto compiled = talas::runtime::compileProject(
        sourceFrameDocument(), talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
        "Array source-N=3 OscillatorBank count=1 fixture should compile");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    require(offline.ok() && offline.mono.size() == frames,
        "Array source-N=3 OscillatorBank should render offline");
    for (std::size_t frame = 0; frame < frames; ++frame)
        require(offline.mono[frame] == 0.25,
            "one selected oscillator should retain source partial zero at offline frame "
                + std::to_string(frame));

    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    require(prepared.ready(), "Array source-N=3 OscillatorBank should prepare: "
        + std::string{prepared.error()});
    constexpr std::array partitions{std::size_t{1}, std::size_t{7}, std::size_t{31}};
    std::array<double, frames> actual{};
    std::size_t offset = 0;
    for (const auto count : partitions)
    {
        auto output = std::span<double>{actual}.subspan(offset, count);
        require(prepared.render(output),
            "prepared 1/7/31 source-frame partition should render: "
                + std::string{prepared.error()});
        for (std::size_t frame = 0; frame < count; ++frame)
            require(output[frame] == 0.25,
                "one selected oscillator should retain source partial zero in prepared output");
        offset += count;
    }
    require(std::equal(actual.begin(), actual.end(), offline.mono.begin()),
        "prepared source-frame partitions should match offline output exactly");
}

void testChangingSoundsPairsAndEmptyRenderPreserveSourceCadence()
{
    constexpr std::size_t frames = 39;
    std::array<float, frames> amplitudes{};
    std::array<float, frames> frequencies{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        amplitudes[frame] = static_cast<float>(frame % 5 + 1) * 0.125F;
        frequencies[frame] = static_cast<float>(frame % 4 + 1);
    }
    auto document = soundsBankDocument();
    auto& bank = projectNode(document, "bank");
    setParameter(bank, "nbrOscillators", "2");
    bank.parameters.back().resource = "Quarter";
    auto renderSettings = settings();
    renderSettings.audioInputChannels = {
        {1, {amplitudes.begin(), amplitudes.end()}},
        {2, {frequencies.begin(), frequencies.end()}},
    };
    const auto graph = compile(document, renderSettings);
    const auto offline = talas::runtime::renderOffline(graph, frames);
    require(offline.ok(), "Sounds N3/M2 should render despite unused fallback N1");

    std::array<double, 2> retainedAmplitudes{};
    std::array<double, 2> retainedFrequencies{};
    // Each oscillator advances its own phase by its partial's frequency.
    std::array<double, 2> phases{};
    std::array<double, frames> expected{};
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto partial = frame % 3;
        if (partial < 2)
        {
            retainedAmplitudes[partial] = amplitudes[frame];
            retainedFrequencies[partial] = frequencies[frame];
        }
        for (std::size_t selected = 0; selected < 2; ++selected)
        {
            expected[frame] += retainedAmplitudes[selected]
                * quarterAtPhase(phases[selected]);
            phases[selected] += retainedFrequencies[selected] / 16.0;
            phases[selected] -= std::floor(phases[selected]);
        }
        require(offline.mono[frame] == expected[frame],
            "Sounds selected pairs must follow the independent triangle oracle");
    }

    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "Sounds dynamic input graph should prepare");
    for (int pass = 0; pass < 2; ++pass)
    {
        std::array<double, frames> actual{};
        std::size_t offset = 0;
        for (const auto size : {1U, 7U, 31U})
        {
            require(render(session, std::span{actual}.subspan(offset, size),
                        std::span{amplitudes}.subspan(offset, size),
                        std::span{frequencies}.subspan(offset, size)),
                "Sounds native-float 1/7/31 callbacks should render");
            offset += size;
        }
        require(actual == expected, "Sounds callbacks/reset should match offline and oracle exactly");
        reset(session);
    }
    talas::runtime::OfflineRenderSession split{graph};
    const auto prefix = split.render(2);
    const auto empty = split.render(0);
    const auto suffix = split.render(37);
    require(prefix.ok() && empty.ok() && empty.mono.empty() && suffix.ok()
            && std::equal(prefix.mono.begin(), prefix.mono.end(), expected.begin())
            && std::equal(suffix.mono.begin(), suffix.mono.end(), expected.begin() + 2),
        "empty render at an unselected phase must preserve the source cursor");
}

void testConnectedSoundsCountChangesRebaseTheArrivalIndex()
{
    auto renderSettings = settings();
    renderSettings.eventValues[0].value = 4.0;
    const auto graph = compile(soundsBankDocument(), renderSettings);
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "connected Sounds count graph should prepare");
    const std::array<float, 4> zeros{};
    const std::array<float, 3> prefixAmplitudes{0.125F, 0.25F, 0.375F};
    std::array<double, 3> prefix{};
    require(render(session, prefix, prefixAmplitudes, std::span{zeros}.first(3))
            && prefix == std::array<double, 3>{0.125, 0.125, 0.125},
        "N4 prefix should leave bank at source phase three");
    update(session, "BankSourceN", 2.0);
    const std::array<float, 4> first{0.5F, 0.625F, 0.75F, 0.875F};
    std::array<double, 4> actual{};
    require(render(session, actual, first, zeros)
            && actual == std::array<double, 4>{0.125, 0.625, 0.625, 0.875},
        "N4 to N2 should rebase phase three to one, retaining skipped amplitudes");
    update(session, "BankSourceN", 3.0);
    const std::array<float, 4> second{0.25F, 0.375F, 0.5F, 0.625F};
    require(render(session, actual, second, zeros)
            && actual == std::array<double, 4>{0.875, 0.875, 0.5, 0.5},
        "N2 to N3 should retain phase one and update only at phase zero");
}

void testInRamRemainingCountAndSelectionChangesUseActualHeader()
{
    auto renderSettings = settings();
    renderSettings.tableResources.push_back({"Analysis/RAM", {
        0.125, 0.0, 0.0, 0.25, 1.0, 0.0,
        0.5, 2.0, 0.0, 0.75, 3.0, 0.0,
    }});
    const auto graph = compile(inRamBankDocument(), renderSettings);
    const auto offline = talas::runtime::renderOffline(graph, 39);
    require(offline.ok(), "InRAM remaining-count sentinel from first two should select three pairs: "
        + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
    for (std::size_t frame = 0; frame < 39; ++frame)
        require(offline.mono[frame] == 0.25 * quarter(frame, 1.0),
            "InRAM selected first pair must survive its remaining source period");
    talas::runtime::RealtimeRenderSession session{graph};
    require(session.ready(), "remaining-count InRAM pair graph should prepare");
    for (int pass = 0; pass < 2; ++pass)
    {
        std::array<double, 39> actual{};
        std::size_t offset = 0;
        for (const auto size : {1U, 7U, 31U})
        {
            require(render(session, std::span{actual}.subspan(offset, size)),
                "InRAM 1/7/31 callbacks should render");
            offset += size;
        }
        require(std::equal(actual.begin(), actual.end(), offline.mono.begin()),
            "InRAM callbacks and reset must match offline exactly");
        reset(session);
    }
    std::array<double, 5> prefix{};
    require(render(session, prefix), "InRAM prefix should leave phase two of three");
    update(session, "RamFirst", 4.0);
    std::array<double, 4> shrunk{};
    require(render(session, shrunk), "InRAM changed selection should reduce remaining N to one");
    // The oscillator keeps the phase it reached at 1 hz and goes on at 3 hz.
    for (std::size_t frame = 0; frame < shrunk.size(); ++frame)
        require(shrunk[frame]
                == 0.75 * quarterAtPhase(5.0 / 16.0 + static_cast<double>(frame) * 3.0 / 16.0),
            "InRAM N3 to N1 must rebase both producer and consumer");
    update(session, "RamFirst", 2.0);
    std::array<double, 3> grown{};
    require(render(session, grown), "InRAM selection should restore actual N3");
    for (std::size_t frame = 0; frame < grown.size(); ++frame)
        require(grown[frame]
                == 0.25 * quarterAtPhase(1.0 / 16.0 + static_cast<double>(frame) / 16.0),
            "InRAM N1 to N3 should restart selection from aligned phase zero");
}

void testFailuresRestoreUnselectedPhaseAndRetainedPairs()
{
    auto renderSettings = settings();
    renderSettings.tableResources.front().samples = {4.0};
    const auto graph = compile(soundsBankDocument(true), renderSettings);
    talas::runtime::RealtimeRenderSession candidate{graph};
    talas::runtime::RealtimeRenderSession reference{graph};
    require(candidate.ready() && reference.ready(), "late-failure sessions should prepare");
    const std::array<float, 1> firstAmplitude{0.5F};
    const std::array<float, 1> firstFrequency{};
    std::array<double, 1> prefix{};
    require(render(candidate, prefix, firstAmplitude, firstFrequency)
            && render(reference, prefix, firstAmplitude, firstFrequency)
            && prefix[0] == 2.0,
        "late-failure prefix should leave the bank at unselected phase one");
    const std::array<float, 3> amplitudes{0.25F, 0.125F, 1.0F};
    const std::array<float, 3> frequencies{};
    update(candidate, "BankFailureGain", std::numeric_limits<double>::max());
    std::array<double, 3> rejected{17.0, 18.0, 19.0};
    require(!render(candidate, rejected, amplitudes, frequencies)
            && rejected == std::array<double, 3>{},
        "late downstream overflow must silence the whole callback after bank updates");
    update(candidate, "BankFailureGain", 1.0);
    std::array<double, 3> retry{};
    std::array<double, 3> expected{};
    require(render(candidate, retry, amplitudes, frequencies)
            && render(reference, expected, amplitudes, frequencies)
            && retry == expected && retry == std::array<double, 3>{2.0, 2.0, 4.0},
        "late retry must restore selected values and the unselected source phase");

    // A rejected N change must also undo the newly introduced phase normalization.
    renderSettings.eventValues[0].value = 4.0;
    const auto fourGraph = compile(soundsBankDocument(), renderSettings);
    talas::runtime::RealtimeRenderSession normalized{fourGraph};
    talas::runtime::RealtimeRenderSession untouched{fourGraph};
    require(normalized.ready() && untouched.ready(), "count-rebase rollback sessions should prepare");
    const std::array<float, 3> warmAmplitudes{0.5F, 0.25F, 0.125F};
    require(render(normalized, retry, warmAmplitudes, frequencies)
            && render(untouched, expected, warmAmplitudes, frequencies),
        "N4 warmup should leave phase three");
    update(normalized, "BankSourceN", 2.0);
    const std::array<float, 1> badFrequency{8.0F};
    require(!render(normalized, prefix, firstAmplitude, badFrequency) && prefix[0] == 0.0,
        "Nyquist at the rebased unselected phase must reject");
    update(normalized, "BankSourceN", 4.0);
    const std::array<float, 4> repairedAmplitudes{0.25F, 0.125F, 0.75F, 0.5F};
    const std::array<float, 4> repairedFrequencies{};
    std::array<double, 4> normalizedRetry{};
    std::array<double, 4> untouchedOutput{};
    require(render(normalized, normalizedRetry, repairedAmplitudes, repairedFrequencies)
            && render(untouched, untouchedOutput, repairedAmplitudes, repairedFrequencies)
            && normalizedRetry == untouchedOutput
            && normalizedRetry == std::array<double, 4>{2.0, 0.5, 0.5, 0.5},
        "failed count normalization must restore original phase three before retry");

    auto offlineSettings = settings();
    offlineSettings.eventValues[0].value = 4.0;
    offlineSettings.audioInputChannels = {
        {1, {0.125, 0.25, 0.375, 0.5}}, {2, {0.0, 0.0, 0.0, 8.0}},
    };
    const auto offlineGraph = compile(soundsBankDocument(), offlineSettings);
    talas::runtime::OfflineRenderSession offline{offlineGraph};
    require(!offline.render(4).ok(), "offline render must reject late unselected Nyquist");
    const auto offlineRetry = offline.render(3);
    require(offlineRetry.ok() && offlineRetry.mono == std::vector<double>{0.125, 0.125, 0.125},
        "offline failed whole-call must restore input position and retained pairs");
}

void testRawAndCrossedAudioPairsKeepTheirImplicitPeriod()
{
    auto raw = soundsBankDocument();
    std::erase_if(raw.graph.nodes, [](const auto& candidate) {
        return candidate.instanceId == "source" || candidate.instanceId == "count";
    });
    raw.graph.connections = {
        connection("amps", "audioOut", "bank", "amplitudeIn"),
        connection("freqs", "audioOut", "bank", "frequencyIn"),
        connection("bank", "audioOut", "out", "audioIn"),
    };
    auto rawSettings = settings();
    rawSettings.audioInputChannels = {{1, {0.125, 0.25, 0.375}}, {2, {0.0, 0.0, 0.0}}};
    const auto rawGraph = compile(raw, rawSettings);
    const auto rawOffline = talas::runtime::renderOffline(rawGraph, 3);
    const std::array<double, 3> rawExpected{0.125, 0.25, 0.375};
    require(rawOffline.ok()
            && std::equal(rawExpected.begin(), rawExpected.end(), rawOffline.mono.begin()),
        "ordinary raw pairs should retain implicit N=M=1 offline");
    talas::runtime::RealtimeRenderSession rawSession{rawGraph};
    require(rawSession.ready(), "raw pair graph should prepare");
    const std::array<float, 3> amplitudes{0.125F, 0.25F, 0.375F};
    const std::array<float, 3> frequencies{};
    std::array<double, 3> rawActual{};
    require(render(rawSession, rawActual, amplitudes, frequencies) && rawActual == rawExpected,
        "ordinary raw pairs should retain implicit N=M=1 in prepared callbacks");

    auto crossed = sourceFrameDocument();
    auto other = projectNode(crossed, "source");
    other.instanceId = "other";
    setTextParameter(other, "amplitudes", "0.75 0.625 0.5");
    crossed.graph.nodes.push_back(std::move(other));
    crossed.graph.connections[1].fromNode = "other";
    const auto crossedGraph = compile(crossed, settings());
    const auto crossedOffline = talas::runtime::renderOffline(crossedGraph, 6);
    const std::array<double, 6> crossedExpected{0.25, 0.5, 0.75, 0.25, 0.5, 0.75};
    require(crossedOffline.ok()
            && std::equal(crossedExpected.begin(), crossedExpected.end(), crossedOffline.mono.begin()),
        "crossed known sources must preserve the old raw convention");
    talas::runtime::RealtimeRenderSession crossedSession{crossedGraph};
    require(crossedSession.ready(), "crossed known pair graph should remain admitted");
    std::array<double, 6> crossedActual{};
    require(render(crossedSession, crossedActual) && crossedActual == crossedExpected,
        "crossed source metadata must not be inferred from either lane alone");
}

void testTooFewSourcePairsAndForgedMetadataReject()
{
    auto tooFew = sourceFrameDocument();
    setParameter(projectNode(tooFew, "source"), "nbrPartials", "1");
    setParameter(projectNode(tooFew, "bank"), "nbrOscillators", "2");
    const auto graph = compile(tooFew, settings());
    require(!talas::runtime::renderOffline(graph, 3).ok(), "source N below M must reject offline");
    talas::runtime::RealtimeRenderSession prepared{graph};
    std::array<double, 3> rejected{1.0, 1.0, 1.0};
    require(!prepared.ready() || (!render(prepared, rejected) && rejected == std::array<double, 3>{}),
        "source N below M must reject in the prepared path");

    const auto valid = compile(sourceFrameDocument(), settings());
    for (int mutation = 0; mutation < 4; ++mutation)
    {
        auto plan = valid.plan();
        auto& bank = *std::ranges::find(plan.steps, std::string_view{"bank"},
            &talas::runtime::CompileStep::nodeId);
        auto& source = *std::ranges::find(plan.steps, std::string_view{"source"},
            &talas::runtime::CompileStep::nodeId);
        if (mutation == 0)
            ++bank.inputs.front().sourceScopeId;
        else if (mutation == 1)
            bank.inputs.push_back(bank.inputs.front());
        else if (mutation == 2)
        {
            auto output = std::ranges::find(source.outputs, std::string_view{"spectrumOut"},
                &talas::runtime::OutputBinding::outputPortId);
            require(output != source.outputs.end(), "source must declare the count output");
            output->bufferIndex = bank.inputs.front().bufferIndex;
        }
        else
        {
            const auto output = std::ranges::find(source.outputs, std::string_view{"spectrumOut"},
                &talas::runtime::OutputBinding::outputPortId);
            require(output != source.outputs.end(), "source must declare the count output");
            plan.buffers.push_back(plan.buffers[output->bufferIndex]);
        }
        const talas::runtime::PreparedRuntimeGraph forged{std::move(plan), valid.renderSettings()};
        const auto offline = talas::runtime::renderOffline(forged, 3);
        talas::runtime::RealtimeRenderSession realtime{forged};
        require(!offline.ok() && !realtime.ready(),
            "forged pair scope/input list/count output authority must reject in both paths");
    }
}
}

int main()
{
    try
    {
        testSourceFrameCountDoesNotCollapseToSelectedBankCount();
        testChangingSoundsPairsAndEmptyRenderPreserveSourceCadence();
        testConnectedSoundsCountChangesRebaseTheArrivalIndex();
        testInRamRemainingCountAndSelectionChangesUseActualHeader();
        testFailuresRestoreUnselectedPhaseAndRetainedPairs();
        testRawAndCrossedAudioPairsKeepTheirImplicitPeriod();
        testTooFewSourcePairsAndForgedMetadataReject();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Oscillator Bank source-frame reference test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
