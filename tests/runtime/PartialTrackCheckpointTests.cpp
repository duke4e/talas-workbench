#include "talas/core/BuiltinNodes.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>

namespace
{
std::atomic<bool> countAllocations{false};
std::atomic<std::size_t> allocations{0};
} // namespace
void *operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto *memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}
void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void *memory) noexcept
{
    std::free(memory);
}
void operator delete[](void *memory) noexcept
{
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

bool renderWithoutAllocation(talas::runtime::RealtimeRenderSession &session, std::span<double> output)
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

void testPartialTrackCheckpoint(int profile, bool checkedFailure)
{
    using namespace talas::runtime;
    constexpr std::size_t maximumFrames = 65;
    talas::project::PartialTrackAsset asset{64.0, 640, {{1, 0, {{3, .5, 7.3, 0.0}}}}};
    if (profile == 1)
        asset.frames = {{1, 0, {{3, .5, 7.3, .2}}},
                        {1, 128, {{3, .2, 11.1, 1.0}, {9, .1, 17.0, 0}}},
                        {2, 256, {{5, .6, 9.2, 1.0}}},
                        {2, 384, {{5, .2, 13.1, 0.0}}},
                        {3, 512, {}},
                        {4, 600, {{7, .1, 5.4, .3}}}};
    if (profile == 2)
        asset.frames = {{1, 0, {}}, {2, 256, {}}, {3, 512, {}}};
    const auto encoded = talas::project::encodePartialTrackAsset(asset);
    require(encoded.ok(), "checkpoint partial resource must encode");
    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>();
    snapshot->id = "Analysis/Checkpoint";
    snapshot->kind = talas::core::ResourceSnapshotKind::PartialSpectrum;
    snapshot->generation = 7;
    snapshot->contentHash = std::string(64, 'a');
    snapshot->sampleRate = asset.sampleRate;
    snapshot->frameCount = asset.sourceFrameCount;
    snapshot->encoding = std::string(talas::project::partialTrackAssetEncoding);
    snapshot->partialTrackBytes = encoded.bytes;
    RenderSettings settings{asset.sampleRate, maximumFrames};
    settings.resourceSnapshots.push_back(snapshot);
    settings.eventValues = {{"CheckpointFormant", 8.0, talas::expr::ValueDomain::Scalar}};
    talas::project::ProjectDocument document;
    document.assets.push_back({snapshot->id,
                               talas::project::ProjectAssetKind::PartialSpectrum,
                               "analysis/checkpoint.talaspartial",
                               talas::project::ProjectAssetDisposition::Referenced,
                               snapshot->contentHash,
                               0,
                               asset.sampleRate,
                               asset.sourceFrameCount,
                               snapshot->encoding,
                               7,
                               {}});
    document.graph.nodes = {{"source",
                             "talas.partialTrackPlayerV1",
                             {resource("partialTrack", snapshot->id), {"gain", ".75"}}},
                            {"fir", "talas.firFilter", {text("coefficients", "0.5 0.25 -0.125")}},
                            {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "fir", "audioIn"}};
    if (checkedFailure)
    {
        document.graph.nodes.push_back(
            {"control", "talas.constant", {{"value", "!CheckpointFormant"}}});
        document.graph.nodes.push_back({"filter",
                                        "talas.twoFormantElement",
                                        {{"formant1", "8 hz"},
                                         {"bandwidth1", "2 hz"},
                                         {"scale1", ".75"},
                                         {"formant2", "12 hz"},
                                         {"bandwidth2", "3 hz"},
                                         {"scale2", "-.25"}}});
        document.graph.connections.push_back({"control", "value", "filter", "formant1"});
        document.graph.connections.push_back({"fir", "audioOut", "filter", "audioIn"});
        document.graph.connections.push_back({"filter", "audioOut", "out", "audioIn"});
    }
    else
        document.graph.connections.push_back({"fir", "audioOut", "out", "audioIn"});
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Partial Track checkpoint composition must compile");
    RealtimeRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    if (!candidate.ready())
        throw std::runtime_error(std::string(candidate.error()));
    require(reference.ready(), "uninterrupted Partial Track reference must prepare");
    OfflineRenderSession offline(*compiled.graph);
    std::array<double, maximumFrames> actual{}, expected{};
    std::size_t position = 0;
    double previous1 = 0, previous2 = 0;
    bool audible = false;
    const auto compare = [&](std::size_t frames)
    {
        const auto native = offline.render(frames);
        require(native.ok(), "native Partial Track checkpoint composition must render");
        require(renderWithoutAllocation(candidate, std::span(actual).first(frames)) &&
                    renderWithoutAllocation(reference, std::span(expected).first(frames)),
                "Partial Track checkpoint must continue after failure");
        for (std::size_t i = 0; i < frames; ++i)
        {
            require(
                actual[i] == expected[i],
                "Partial Track phase, point cursor and segment continuation must recover exactly");
            require(actual[i] == native.mono[i],
                    "Partial Track native and realtime PCM must agree exactly");
            if (profile == 0 && !checkedFailure)
            {
                const auto frame = position + i;
                const auto sample =
                    frame < asset.sourceFrameCount
                        ? .375 * std::sin(2.0 * std::numbers::pi * 7.3 * frame / asset.sampleRate)
                        : 0.0;
                const auto filtered = .5 * sample + .25 * previous1 - .125 * previous2;
                require(std::abs(actual[i] - filtered) < 1e-9,
                        "Partial Track tone and FIR must match an independent analytic recurrence");
                previous2 = previous1;
                previous1 = sample;
            }
            audible |= std::abs(actual[i]) > 1e-6;
        }
        position += frames;
    };
    const auto until = [&](std::size_t end)
    {
        constexpr std::array<std::size_t, 4> blocks{65, 17, 31, 7};
        std::size_t block = 0;
        while (position < end)
            compare(std::min(blocks[block++ % blocks.size()], end - position));
    };
    for (const auto start : {95U, 223U, 351U, 479U, 607U})
    {
        until(start);
        if (checkedFailure)
        {
            require(candidate.scheduleEventValue("CheckpointFormant", 32.0,
                                                 talas::expr::ValueDomain::Scalar, 64),
                    "late Partial Track failure must schedule");
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                actual.fill(42.0);
                require(!renderWithoutAllocation(candidate, actual) &&
                            std::ranges::all_of(actual, [](double value) { return value == 0.0; }),
                        "late Partial Track failure must silence the entire callback");
            }
            require(
                candidate.updateEventValue("CheckpointFormant", 8.0, talas::expr::ValueDomain::Scalar),
                "Partial Track failure must allow explicit repair");
            compare(1);
            compare(maximumFrames - 1);
        }
        else
            compare(maximumFrames);
    }
    until(800);
    require(profile == 2 || audible, "Partial Track recovery must include audible history");
    candidate.resetTransport();
    RealtimeRenderSession fresh(*compiled.graph);
    require(fresh.ready(), "fresh Partial Track reset reference must prepare");
    for (int block = 0; block < 10; ++block)
    {
        require(renderWithoutAllocation(candidate, actual) &&
                    renderWithoutAllocation(fresh, expected),
                "reset Partial Track render must allocate nothing");
        require(actual == expected, "Partial Track reset must match fresh segment and track state");
    }
    std::cout << "profile=" << profile << " failure=" << checkedFailure << " passed\n";
}
} // namespace
int main()
{
    for (int profile = 0; profile < 3; ++profile)
        for (bool failure : {false, true})
            testPartialTrackCheckpoint(profile, failure);
}
