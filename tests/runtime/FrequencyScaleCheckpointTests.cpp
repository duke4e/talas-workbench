#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <cmath>
#include <numbers>
#include <iostream>
#include <algorithm>
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


talas::project::ProjectParameterExpression resource(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}
talas::project::ProjectParameterExpression text(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

void testFrequencyScaleCoexistence(int profile, bool firBefore)
{
    using namespace talas::runtime;
    constexpr std::size_t sectionFrames = 1280;
    constexpr std::size_t maximumFrames = 257;
    RenderSettings settings{48000.0, maximumFrames};
    settings.eventValues = {{"CheckpointFormant", 500.0, talas::expr::ValueDomain::Scalar}};
    std::vector<double> ratios;
    for (double ratio : {1.0, 1.5, 0.75, 1.25, 1.0})
        ratios.insert(ratios.end(), sectionFrames, ratio);
    settings.tableResources.push_back({"Tables/CheckpointRatios", ratios});
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "0.75"}}},
        // 440 hz, as a fraction of half the sample rate.
        {"tracker", "talas.constant", {{"value", "440 / 24000"}}},
        {"scale", "talas.frequencyScale", {{"frequencyScale", profile == 1 ? "1.5" : "1"},
            {"maxScale", profile == 0 ? "1" : "2"}, resource("window", "Private"), {"delay", "0.001"}}},
        {"fir", "talas.firFilter", {text("coefficients", "0.5 0.25 -0.125")}},
        {"formantControl", "talas.constant", {{"value", "!CheckpointFormant"}}},
        {"formants", "talas.twoFormantElement", {{"formant1", "500 hz"}, {"bandwidth1", "100 hz"},
            {"scale1", "0.75"}, {"formant2", "1000 hz"}, {"bandwidth2", "150 hz"}, {"scale2", "-0.25"}}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"tracker", "value", "scale", "freqTracker"},
        {"formantControl", "value", "formants", "formant1"}, {"formants", "audioOut", "out", "audioIn"}};
    const auto first = firBefore ? "fir" : "scale";
    const auto second = firBefore ? "scale" : "fir";
    document.graph.connections.push_back({"source", "audioOut", first, "audioIn"});
    document.graph.connections.push_back({first, "audioOut", second, "audioIn"});
    document.graph.connections.push_back({second, "audioOut", "formants", "audioIn"});
    if (profile == 2)
    {
        document.graph.nodes.push_back({"ratio", "talas.triggeredTableRead",
            {{"trigger", "1"}, resource("wavetable", "Tables/CheckpointRatios"), {"gate", "1"}}});
        document.graph.connections.push_back({"ratio", "value", "scale", "frequencyScale"});
    }
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Frequency Scale checkpoint graph must compile");
    RealtimeRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    if (!candidate.ready()) throw std::runtime_error(std::string(candidate.error()));
    require(reference.ready(), "uninterrupted checkpoint reference must prepare");
    OfflineRenderSession offline(*compiled.graph);
    std::array<double, maximumFrames> actual{}, expected{};
    bool audible = false;
    const std::array<std::size_t, 5> partitions{257, 127, 251, 17, 256};
    for (std::size_t section = 0; section < 5; ++section)
    {
        std::size_t consumed = 0, partition = 0;
        while (consumed < sectionFrames)
        {
            const auto frames = std::min(partitions[partition++ % partitions.size()], sectionFrames - consumed);
            // Each section exceeds the 1071-frame pre-delay plus spectral latency.
            // Fail after a complete 256-frame hop at a ratio change, while
            // spectral phase/identity and ratio transition state is live.
            if (section > 0 && consumed == 0)
            {
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    require(candidate.scheduleEventValue("CheckpointFormant", 24000.0,
                        talas::expr::ValueDomain::Scalar, 256), "late invalid formant must schedule");
                    actual.fill(42.0);
                    require(!renderWithoutAllocation(candidate, std::span(actual).first(frames))
                        && std::ranges::all_of(std::span(actual).first(frames), [](double value) { return value == 0.0; }),
                        "repeated segmented failure must silence the whole callback without allocation");
                    require(candidate.updateEventValue("CheckpointFormant", 500.0, talas::expr::ValueDomain::Scalar),
                        "formant must recover after failed callback");
                }
            }
            const auto offlineBlock = offline.render(frames);
            require(offlineBlock.ok(), "independent offline Frequency Scale combination must render");
            require(renderWithoutAllocation(candidate, std::span(actual).first(frames))
                && renderWithoutAllocation(reference, std::span(expected).first(frames)),
                "checkpointed Frequency Scale must continue after failure");
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                require(actual[frame] == expected[frame],
                    "Frequency Scale delay, spectral history, transition and timestamp must recover exactly");
                require(std::abs(actual[frame] - offlineBlock.mono[frame]) < 1e-9,
                    "Frequency Scale and checked filter composition must agree with offline processing");
                audible |= std::abs(actual[frame]) > 1e-6;
            }
            consumed += frames;
        }
    }
    require(audible, "checkpoint oracle must include audible filled history");
    candidate.resetTransport();
    RealtimeRenderSession fresh(*compiled.graph);
    for (int block = 0; block < 8; ++block)
    {
        require(renderWithoutAllocation(candidate, std::span(actual).first(160))
            && renderWithoutAllocation(fresh, std::span(expected).first(160))
            && std::ranges::equal(std::span(actual).first(160), std::span(expected).first(160)),
            "reset must clear all Frequency Scale and adjacent filter history");
    }
    std::cout << "profile=" << profile << " firBefore=" << firBefore << " passed" << std::endl;
}

void testNeutralIndependentOracle()
{
    using namespace talas::runtime;
    RenderSettings settings{32.0, 7};
    settings.eventValues = {{"NeutralGain", 1.0, talas::expr::ValueDomain::Scalar}};
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "8"}}},
        {"tracker", "talas.constant", {{"value", "1"}}},
        {"scale", "talas.frequencyScale", {{"frequencyScale", "1"}, {"maxScale", "1"},
            resource("window", "Private"), {"delay", "0.09375"}}},
        {"fir", "talas.firFilter", {text("coefficients", "0.5 0.25")}},
        {"gain", "talas.constant", {{"value", "!NeutralGain"}}},
        {"vca", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "scale", "audioIn"},
        {"tracker", "value", "scale", "freqTracker"}, {"scale", "audioOut", "fir", "audioIn"},
        {"fir", "audioOut", "vca", "audioIn"}, {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "neutral analytical checkpoint graph must compile");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "neutral Frequency Scale/FIR checkpoint graph must prepare");
    std::array<double, 7> output{};
    std::size_t position = 0;
    const auto render = [&](std::size_t frames) {
        require(renderWithoutAllocation(session, std::span(output).first(frames)), "neutral retry must render");
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto n = position + frame;
            const auto sample = [](std::size_t index) { return std::sin(2 * std::numbers::pi_v<double> * index / 32); };
            const auto oracle = (n >= 3 ? 4 * sample(n - 3) : 0) + (n >= 4 ? 2 * sample(n - 4) : 0);
            require(std::abs(output[frame] - oracle) < 1e-12,
                "neutral Frequency Scale ring and FIR history must match independent delayed sine recurrence");
        }
        position += frames;
    };
    render(6);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        require(session.scheduleEventValue("NeutralGain", 1e308, talas::expr::ValueDomain::Scalar, 1),
            "neutral overflow must occur after a successful segment");
        output.fill(42);
        require(!renderWithoutAllocation(session, output) && output == std::array<double, 7>{},
            "late neutral overflow must roll back and silence repeated attempts");
        require(session.updateEventValue("NeutralGain", 1, talas::expr::ValueDomain::Scalar), "neutral gain must recover");
    }
    for (std::size_t frames : {7, 1, 5, 2, 7, 7}) render(frames);
    session.resetTransport();
    position = 0;
    for (std::size_t frames : {2, 7, 5}) render(frames);
}
}
int main()
{
    testNeutralIndependentOracle();
    for (int profile = 0; profile < 3; ++profile)
        for (bool before : {false, true}) testFrequencyScaleCoexistence(profile, before);
}
