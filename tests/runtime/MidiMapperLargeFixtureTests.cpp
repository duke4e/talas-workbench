#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <string>
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

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using Clock = std::chrono::steady_clock;

constexpr double sampleRate = 48000.0;
constexpr std::size_t blockFrames = 256;
constexpr std::uint64_t compileBudgetNs = 2'000'000'000;
constexpr std::uint64_t preparationP95BudgetNs = 250'000'000;
constexpr std::uint64_t callbackP99BudgetNs = 5'333'334;

#if defined(NDEBUG)
constexpr bool optimizedBuild = true;
constexpr std::size_t preparationSamples = 32;
constexpr std::size_t callbackSamples = 256;
#else
constexpr bool optimizedBuild = false;
constexpr std::size_t preparationSamples = 8;
constexpr std::size_t callbackSamples = 16;
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool sanitizerInstrumented = true;
#else
constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitizerInstrumented = true;
#else
constexpr bool sanitizerInstrumented = false;
#endif

// The callback budgets are wall-clock times, which hold only while nothing
// else loads the machine, so they are checked only when TALAS_STRICT_TIMING=1:
// ctest's serial "perf" entry for this binary sets it, and the local gate
// runs that label after the parallel suite. Other runs print the timings.
bool strictTiming()
{
    const char* value = std::getenv("TALAS_STRICT_TIMING");
    return value != nullptr && std::string_view{value} == "1";
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::uint64_t elapsedNs(Clock::time_point started)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started)
            .count());
}

std::uint64_t nearestRank(
    std::vector<std::uint64_t> values,
    std::size_t numerator,
    std::size_t denominator)
{
    std::ranges::sort(values);
    const auto rank = (values.size() * numerator + denominator - 1U)
        / denominator;
    return values[std::max<std::size_t>(1, rank) - 1U];
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression text(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.text = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression choice(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.choice = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression resource(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.resource = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression boolean(
    std::string id,
    bool value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.boolean = value;
    return result;
}

std::vector<talas::project::ProjectParameterExpression> mapperParameters()
{
    return {
        text("map", "velocity"),
        choice("voiceModel", "legacyPassthrough"),
        expression("left", "1"),
        expression("right", "1"),
        expression("channel", "0"),
        text("source", ""),
        resource("midiFile", ""),
        expression("polyphony", "8"),
        expression("lowPitch", "0 nn"),
        expression("highPitch", "127 nn"),
        text("script", ""),
        // Shared, so every Mapper in the chain takes each note.
        boolean("shared", true),
    };
}

talas::project::ProjectDocument maximumMapperDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "osc", "talas.oscillator",
        {expression("frequency", "220 hz"),
         expression("amplitude", "0.25")},
    });
    std::string previous = "osc";
    std::string previousPort = "audioOut";
    for (std::size_t index = 0;
         index < talas::runtime::maximumPreparedMidiMapperInstances;
         ++index)
    {
        auto id = "mapper" + std::to_string(index);
        document.graph.nodes.push_back(
            {id, "talas.midiMapper", mapperParameters()});
        document.graph.connections.push_back(
            {previous, previousPort, id, "audioIn"});
        previous = std::move(id);
        previousPort = "leftOut";
    }
    document.graph.connections.push_back(
        {previous, "leftOut", "out", "audioIn"});
    document.graph.connections.push_back(
        {previous, "rightOut", "out", "rightIn"});
    return document;
}

void testMaximumMapperPreparationAndCallbackBudgets()
{
    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    settings.maxRealtimeEventTransactions = 8;
    const auto compileStarted = Clock::now();
    auto compiled = talas::runtime::compileProject(
        maximumMapperDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    const auto compileNs = elapsedNs(compileStarted);
    require(compiled.ok(), "maximum mapper graph must compile");
    auto graph = std::move(*compiled.graph);
    require(graph.plan().midiMappers.size()
                == talas::runtime::maximumPreparedMidiMapperInstances,
        "maximum mapper graph must retain all 64 mapper plans");
    std::size_t voiceSlots = 0;
    for (const auto& mapper : graph.plan().midiMappers)
        voiceSlots += mapper.configuration.polyphony;
    require(voiceSlots == talas::runtime::maximumPreparedMidiMapperVoices,
        "maximum mapper graph must consume exactly 512 voice slots");

    std::vector<std::uint64_t> preparationDurations;
    preparationDurations.reserve(preparationSamples);
    for (std::size_t sample = 0; sample < preparationSamples + 2; ++sample)
    {
        const auto started = Clock::now();
        talas::runtime::RealtimeRenderSession prepared(graph);
        const auto duration = elapsedNs(started);
        require(prepared.ready(), "maximum mapper session must prepare");
        if (sample >= 2)
            preparationDurations.push_back(duration);
    }
    const auto preparationP95 =
        nearestRank(preparationDurations, 95, 100);

    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), "maximum mapper callback fixture must prepare");
    std::array<double, blockFrames> left{};
    std::array<double, blockFrames> right{};
    std::vector<std::uint64_t> callbackDurations;
    callbackDurations.reserve(callbackSamples);
    double checksum = 0.0;
    for (std::size_t sample = 0; sample < callbackSamples + 4; ++sample)
    {
        const auto noteOn = sample % 2U == 0;
        require(session.scheduleMidiNote(
                    {noteOn
                            ? talas::runtime::RealtimeMidiNoteKind::NoteOn
                            : talas::runtime::RealtimeMidiNoteKind::NoteOff,
                     1, 60, static_cast<std::uint8_t>(noteOn ? 100 : 0)},
                    0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "maximum mapper callback event must be handled");
        left.fill(std::numeric_limits<double>::quiet_NaN());
        right.fill(std::numeric_limits<double>::quiet_NaN());
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto started = Clock::now();
        const auto rendered = session.render(left, right);
        const auto duration = elapsedNs(started);
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered, "maximum mapper callback must render");
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "maximum mapper callback must not allocate");
        require(std::ranges::all_of(left, [](double value) {
                    return std::isfinite(value);
                })
                && std::ranges::all_of(right, [](double value) {
                    return std::isfinite(value);
                }),
            "maximum mapper callback output must remain finite");
        checksum += std::abs(left[sample % blockFrames])
            + std::abs(right[(sample * 17U) % blockFrames]);
        if (sample >= 4)
            callbackDurations.push_back(duration);
    }
    const auto callbackP99 = nearestRank(callbackDurations, 99, 100);
    require(checksum > 1.0e-9,
        "maximum mapper callback checksum must prove audible work");
    require(!optimizedBuild || sanitizerInstrumented || !strictTiming()
            || (compileNs <= compileBudgetNs
                && preparationP95 <= preparationP95BudgetNs
                && callbackP99 <= callbackP99BudgetNs),
        "optimized maximum mapper fixture exceeded its fixed budget");
    std::cout << "MIDI_MAPPER_MAX_FIXTURE compile_ns=" << compileNs
              << " preparation_p95_ns=" << preparationP95
              << " callback_p99_ns=" << callbackP99
              << " checksum=" << checksum << '\n';
}
}

int main()
{
    testMaximumMapperPreparationAndCallbackBudgets();
}
