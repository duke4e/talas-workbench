#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"
#include "../support/TimeFrequencyScaleLowOutputReference.h"
#include "../support/TimeFrequencyScaleVariableSourceReference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
constexpr double sampleRate = 8192;
constexpr std::size_t sourceFrames = 4096;
constexpr std::size_t stretchedFrames = 4 * (sourceFrames - 1) + 1;
constexpr std::size_t analysisFrameSize = 1024;
constexpr std::size_t centeredSupportTail = analysisFrameSize / 2 * (1 + 4);
constexpr std::size_t outputFrames = stretchedFrames + analysisFrameSize + analysisFrameSize / 4 - 1
    + centeredSupportTail + 1024;
constexpr auto scalar = talas::expr::ValueDomain::Scalar;
using Parameter = talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
Parameter resource()
{
    Parameter result;
    result.parameterId = "sample";
    result.resource = "Samples/Quarter";
    return result;
}
talas::project::ProjectDocument document(bool connected = false)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"source", "talas.timeFrequencyScale", {
            {"frequencyScale", "1"}, {"rate", "0.25"}, {"gate", "1"},
            {"minInputFreq", "20 hz"}, {"maxInputFreq", "1000 hz"},
            {"maxFreqScale", "4"}, {"detectors", "10"}, resource()}},
        {"out", "talas.audioOutput"}};
    result.graph.connections = {{"source", "audioOut", "out", "audioIn"}};
    if (connected)
    {
        result.graph.nodes.push_back({"rate", "talas.constant", {{"value", "!Rate"}}});
        result.graph.connections.push_back({"rate", "value", "source", "rate"});
    }
    return result;
}
talas::runtime::RenderSettings settings()
{
    talas::runtime::RenderSettings result{sampleRate, 128};
    std::vector<double> samples(sourceFrames);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto frequency = i < sourceFrames / 2 ? 269.0 : 509.0;
        samples[i] = 0.25 * std::sin(2 * std::numbers::pi * frequency * i / sampleRate);
    }
    result.tableResources = {{"Samples/Quarter", std::move(samples)}};
    result.eventValues = {{"Rate", 0.25, scalar}};
    return result;
}
talas::runtime::PreparedRuntimeGraph compile(const talas::project::ProjectDocument& source,
    talas::runtime::RenderSettings configuration)
{
    auto result = talas::runtime::compileProject(source,
        talas::core::makeBuiltinNodeRegistry(), std::move(configuration));
    std::string errors;
    for (const auto& diagnostic : result.diagnostics) errors += diagnostic.message + '\n';
    require(result.ok(), "documented quarter-speed profile should compile: " + errors);
    return std::move(*result.graph);
}
talas::runtime::PreparedRuntimeGraph compile(bool connected = false)
{
    return compile(document(connected), settings());
}
bool renderNoAllocation(talas::runtime::RealtimeRenderSession& session, std::span<double> output)
{
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto ok = session.render(output);
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "quarter-speed callback and rollback must allocate nothing");
    return ok;
}
void renderAll(talas::runtime::RealtimeRenderSession& session, std::span<double> output,
    std::size_t partition)
{
    for (std::size_t offset = 0; offset < output.size(); offset += partition)
        require(renderNoAllocation(session, output.subspan(offset,
            std::min(partition, output.size() - offset))),
            "quarter-speed callback should render: " + std::string{session.error()});
}
void near(std::span<const double> actual, std::span<const double> expected,
    const std::string& label)
{
    require(actual.size() == expected.size(), label + " shape");
    for (std::size_t i = 0; i < actual.size(); ++i)
        require(std::isfinite(actual[i]) && std::abs(actual[i] - expected[i]) < 1e-10,
            label + " frame " + std::to_string(i));
}
double magnitude(std::span<const double> samples, double frequency,
    double measurementSampleRate = sampleRate)
{
    double real = 0;
    double imaginary = 0;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto window = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / samples.size());
        const auto phase = 2 * std::numbers::pi * frequency * i / measurementSampleRate;
        real += samples[i] * window * std::cos(phase);
        imaginary -= samples[i] * window * std::sin(phase);
    }
    return std::hypot(real, imaginary) / samples.size();
}
void requireTone(std::span<const double> window, double frequency, const std::string& label)
{
    const auto wanted = magnitude(window, frequency);
    const auto slowed = magnitude(window, frequency * 0.25);
    const auto other = magnitude(window, frequency == 269 ? 509 : 269);
    require(wanted > 0.01 && wanted > 10 * slowed && wanted > 10 * other,
        label + " must preserve original pitch: wanted=" + std::to_string(wanted)
            + ", slowed=" + std::to_string(slowed) + ", other=" + std::to_string(other));
    double bestFrequency = 0;
    double bestMagnitude = 0;
    for (double probe = frequency - 12; probe <= frequency + 12; probe += 0.5)
    {
        const auto value = magnitude(window, probe);
        if (value > bestMagnitude) { bestMagnitude = value; bestFrequency = probe; }
    }
    require(std::abs(bestFrequency - frequency) <= 1,
        label + " pitch peak must be within1Hz: " + std::to_string(bestFrequency));
}

void testQuarterSpeedPreservesMeasuredPitchAndStretchesRecording()
{
    const auto graph = compile();
    const auto output = talas::runtime::renderOffline(graph, outputFrames);
    require(output.ok(), "quarter-speed offline profile should render");
    const auto samples = std::span<const double>{output.mono};
    const auto& source = *std::ranges::find(graph.plan().steps,
        std::string_view{"source"}, &talas::runtime::CompileStep::nodeId);
    require(source.resolvedLatencyFrames == 1279
        && std::ranges::all_of(samples.first(1279), [](double value) { return value == 0; }),
        "quarter-speed profile keeps exact active latency");
    requireTone(samples.subspan(1279 + 3072, 3072), 269, "first stretched segment");
    requireTone(samples.subspan(1279 + 11264, 3072), 509, "second stretched segment");
    require(std::ranges::all_of(samples.last(1024),
        [](double value) { return std::abs(value) < 1e-12; }),
        "quarter-speed recording must finish after stretched playback and transform tail");
    for (const auto partition : {1u, 7u, 128u})
    {
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(), "quarter-speed profile should prepare: " + std::string{session.error()});
        std::vector<double> actual(outputFrames);
        renderAll(session, actual, partition);
        near(actual, output.mono, "quarter-speed partitions");
        if (partition == 7)
        {
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            session.resetTransport();
            countAllocations.store(false, std::memory_order_relaxed);
            require(allocationCount.load(std::memory_order_relaxed) == 0,
                "quarter-speed reset must allocate nothing");
            renderAll(session, actual, partition);
            near(actual, output.mono, "quarter-speed reset");
        }
    }
}
}

namespace
{
void testConnectedQuarterSpeedTransitionsAndGateRestart()
{
    auto source = document(true);
    source.graph.nodes.push_back({"gate", "talas.constant", {{"value", "!Gate"}}});
    source.graph.connections.push_back({"gate", "value", "source", "gate"});
    auto configuration = settings();
    configuration.eventValues = {{"Rate", 0.5, scalar}, {"Gate", 1, scalar}};
    const std::vector<talas::runtime::EventValueChange> changes{
        {513, "Rate", 0.25, scalar}, {5001, "Rate", 0.5, scalar},
        {7001, "Rate", 0.25, scalar}, {10001, "Gate", 0, scalar},
        {10101, "Gate", 1, scalar}};
    configuration.eventValueChanges = changes;
    const auto offlineGraph = compile(source, configuration);
    talas::runtime::OfflineRenderSession offline(offlineGraph);
    const auto expected = offline.render(14197);
    require(expected.ok(), "connected quarter-speed timeline must render offline");
    offline.reset();
    const auto reset = offline.render(expected.mono.size());
    require(reset.ok(), "connected quarter-speed offline reset must render");
    near(reset.mono, expected.mono, "offline transition reset");
    const auto fresh = talas::runtime::renderOffline(compile(), 4096);
    require(fresh.ok(), "fresh quarter-speed restart reference must render");
    near(std::span<const double>{expected.mono}.subspan(10101), fresh.mono,
        "Gate restart restores source and original correction state");
    require(std::ranges::all_of(std::span<const double>{expected.mono}.subspan(10001, 100),
        [](double value) { return value == 0; }), "Gate release must be silent");
    configuration.eventValueChanges.clear();
    const auto graph = compile(source, configuration);
    for (const auto partition : {1u, 7u, 128u})
    {
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(), "connected quarter-speed timeline must prepare");
        std::vector<double> actual(expected.mono.size());
        for (std::size_t offset = 0; offset < actual.size(); offset += partition)
        {
            const auto count = std::min<std::size_t>(partition, actual.size() - offset);
            for (const auto& change : changes)
                if (change.frame >= offset && change.frame < offset + count)
                    require(session.scheduleEventValue(change.name, change.value, change.domain,
                        change.frame - offset), "quarter-speed event must schedule");
            require(renderNoAllocation(session, std::span<double>{actual}.subspan(offset, count)),
                "quarter-speed event callback must render");
        }
        near(actual, expected.mono, "connected transitions and Gate partitions");
    }
}

void testQuarterSpeedFailureRestoresChangedProfileAndSource()
{
    const auto graph = compile(true);
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession pristine(graph);
    require(candidate.ready() && pristine.ready(), "quarter-speed retry sessions must prepare");
    std::vector<double> warmup(2040);
    renderAll(candidate, warmup, 128);
    renderAll(pristine, warmup, 128);
    require(candidate.updateEventValue("Rate", 0.5, scalar), "retry transition must start");
    require(candidate.scheduleEventValue("Rate", 0, scalar, 127), "late invalid rate must schedule");
    std::array<double, 128> failed;
    failed.fill(99);
    require(!renderNoAllocation(candidate, failed) && failed == std::array<double, 128>{},
        "late rate failure must discard output and allocate nothing");
    require(candidate.updateEventValue("Rate", 0.5, scalar)
        && pristine.updateEventValue("Rate", 0.5, scalar), "corrected transition must be accepted");
    std::vector<double> retried(4096);
    std::vector<double> expected(retried.size());
    renderAll(candidate, retried, 7);
    renderAll(pristine, expected, 128);
    near(retried, expected, "restored quarter-speed history and in-progress correction ramp");
    auto configuration = settings();
    configuration.eventValueChanges = {{2167, "Rate", 0, scalar}};
    const auto offlineGraph = compile(document(true), configuration);
    talas::runtime::OfflineRenderSession offline(offlineGraph);
    talas::runtime::OfflineRenderSession reference(offlineGraph);
    require(offline.render(2040).ok() && reference.render(2040).ok(),
        "offline quarter-speed retry must start with active history");
    const auto rejected = offline.render(128);
    require(!rejected.ok() && rejected.mono.empty(), "late offline correction must reject");
    const auto prefix = offline.render(127);
    const auto freshPrefix = reference.render(127);
    require(prefix.ok() && freshPrefix.ok(), "valid offline prefix must render after failure");
    near(prefix.mono, freshPrefix.mono, "offline clone restores quarter-speed source and DSP history");
}

void testIndependentControlsRemainFiniteAndPositive()
{
    for (const auto id : {"frequencyScale", "rate"})
    for (const auto invalid : {"0", "-0.25", "1 / 0"})
    {
        auto source = document();
        auto& parameters = source.graph.nodes.front().parameters;
        const auto parameter = std::ranges::find(parameters, std::string_view{id},
            &Parameter::parameterId);
        require(parameter != parameters.end(), "independent control parameter must exist");
        parameter->expression = invalid;
        const auto result = talas::runtime::compileProject(source,
            talas::core::makeBuiltinNodeRegistry(), settings());
        require(!result.ok(), "independent controls must reject " + std::string{id} + "=" + invalid);
    }
}

void testQuarterSpeedPreservesLowSourceTonesWithAndWithoutDc()
{
    constexpr double renderRate = 48000;
    std::string failures;
    for (const auto sourceFrequency : {40.0, 80.0, 160.0})
        for (const auto dc : {0.0, 0.1})
        {
            auto configuration = settings();
            configuration.sampleRate = renderRate;
            auto& recording = configuration.tableResources.front().samples;
            recording.resize(12000);
            for (std::size_t i = 0; i < recording.size(); ++i)
                recording[i] = dc + 0.25 * std::sin(
                    2 * std::numbers::pi * sourceFrequency * i / renderRate);
            const auto graph = compile(document(), configuration);
            const auto output = talas::runtime::renderOffline(graph, 48000);
            require(output.ok(), "low source tone must render offline");
            const auto window = std::span<const double>{output.mono}.subspan(8192, 24000);
            const auto wanted = magnitude(window, sourceFrequency, renderRate);
            const auto slowed = magnitude(window, sourceFrequency * 0.25, renderRate);
            double peak = 0;
            double peakValue = 0;
            for (auto probe = sourceFrequency - 6; probe <= sourceFrequency + 6; probe += 0.5)
            {
                const auto value = magnitude(window, probe, renderRate);
                if (value > peakValue) { peakValue = value; peak = probe; }
            }
            const auto label = "source=" + std::to_string(sourceFrequency) + ", DC=" + std::to_string(dc);
            std::cout << label << ", wanted=" << wanted << ", slowed=" << slowed << ", peak=" << peak << '\n';
            if (!(wanted > 0.02 && wanted > 10 * slowed && std::abs(peak - sourceFrequency) <= 1))
                failures += label + " does not retain original pitch\n";
            talas::runtime::RealtimeRenderSession session(graph);
            require(session.ready(), "low source tone must prepare");
            std::vector<double> prepared(output.mono.size());
            renderAll(session, prepared, 128);
            near(prepared, output.mono, "low tone prepared/offline source");
        }
    require(failures.empty(), failures);
}

void testDefaultMinimumFrequencyBindsLowLatency()
{
    // Without MinInputFreq the window covers two periods of the 100 Hz
    // default: 1024 frames at 44.1 and 48 kHz, 2048 at 96 kHz.
    for (const auto profile : {std::pair{44100.0, 1279u}, std::pair{48000.0, 1279u},
            std::pair{96000.0, 2559u}})
    {
        auto configuration = settings();
        configuration.sampleRate = profile.first;
        auto source = document();
        std::erase_if(source.graph.nodes.front().parameters,
            [](const Parameter& parameter) { return parameter.parameterId == "minInputFreq"; });
        const auto graph = compile(source, configuration);
        const auto& step = *std::ranges::find(graph.plan().steps,
            std::string_view{"source"}, &talas::runtime::CompileStep::nodeId);
        require(step.resolvedLatencyFrames == profile.second,
            "the default MinInputFreq must bind the low-latency analysis window");
    }
}

void testSourceWindowPreparationAndConnectedHintAuthority()
{
    auto configuration = settings();
    configuration.sampleRate = 48000;
    auto& recording = configuration.tableResources.front().samples;
    recording.resize(18000);
    for (std::size_t i = 0; i < recording.size(); ++i)
        recording[i] = 0.1 + 0.25 * std::sin(2 * std::numbers::pi * 80 * i / 48000);
    const auto setMinimum = [](auto& source, std::string expression) {
        std::ranges::find(source.graph.nodes.front().parameters,
            std::string_view{"minInputFreq"}, &Parameter::parameterId)->expression = std::move(expression);
    };
    for (const auto profile : {std::pair{"20 hz", 10239u}, std::pair{"40 hz", 5119u},
            std::pair{"160 hz", 1279u}})
    {
        auto source = document();
        setMinimum(source, profile.first);
        const auto graph = compile(source, configuration);
        const auto& step = *std::ranges::find(graph.plan().steps,
            std::string_view{"source"}, &talas::runtime::CompileStep::nodeId);
        require(step.resolvedLatencyFrames == profile.second
            && step.cumulativeOutputLatencyFrames == profile.second,
            "source frequency hint must bind exact analysis latency");
    }
    auto unsupported = document();
    setMinimum(unsupported, "0.1 hz");
    require(!talas::runtime::compileProject(unsupported, talas::core::makeBuiltinNodeRegistry(),
        configuration).ok(), "analysis windows beyond explicit native capacity must reject");

    configuration.eventValues.push_back({"Minimum", 20, scalar});
    std::vector<double> firstOutput;
    for (const auto fallback : {"0.1 hz", "900 hz"})
    {
        auto source = document();
        setMinimum(source, fallback);
        source.graph.nodes.push_back({"minimum", "talas.constant", {{"value", "!Minimum"}}});
        source.graph.connections.push_back({"minimum", "value", "source", "minInputFreq"});
        const auto graph = compile(source, configuration);
        const auto& step = *std::ranges::find(graph.plan().steps,
            std::string_view{"source"}, &talas::runtime::CompileStep::nodeId);
        require(step.resolvedLatencyFrames == 10239,
            "connected minimum hint must not select a window through its unused fallback");
        talas::runtime::RealtimeRenderSession candidate(graph), reference(graph);
        require(candidate.ready() && reference.ready(), "connected source hint must prepare");
        const auto offline = talas::runtime::renderOffline(graph, 20000);
        require(offline.ok(), "connected source hint must render offline");
        std::vector<double> actual(20000), expected(20000);
        renderAll(candidate, actual, 128);
        renderAll(reference, expected, 128);
        near(actual, offline.mono, "connected hint native parity");
        if (firstOutput.empty()) firstOutput = actual;
        else near(actual, firstOutput, "unused connected hint fallback");
        require(candidate.scheduleEventValue("Minimum", 10, scalar, 127),
            "unsupported connected hint must schedule after progressed source analysis");
        std::array<double, 128> failed;
        failed.fill(99);
        require(!renderNoAllocation(candidate, failed) && failed == std::array<double, 128>{},
            "late larger-window request must fail atomically without allocation");
        require(candidate.updateEventValue("Minimum", 20, scalar),
            "prepared hint correction must succeed");
        actual.resize(9000);
        expected.resize(9000);
        renderAll(candidate, actual, 7);
        renderAll(reference, expected, 128);
        near(actual, expected, "larger-window rejection restores long source and phase histories");
    }
}

void testConnectedRateRetainsIndependentChangingSourcePitch()
{
    const talas::test::TimeFrequencyScaleVariableSourceReference reference;
    std::vector<talas::runtime::EventValueChange> changes;
    for (std::size_t i = 1; i < reference.changes.size(); ++i)
        changes.push_back({reference.changes[i].first, "Rate", reference.changes[i].second, scalar});
    for (const auto chirp : {false, true})
    {
        talas::runtime::RenderSettings configuration{reference.sampleRate, 128};
        configuration.tableResources = {{"Samples/Quarter", reference.recording(chirp)}};
        configuration.eventValues = {{"Rate", reference.changes.front().second, scalar}};
        configuration.eventValueChanges = changes;
        const auto offlineGraph = compile(document(true), configuration);
        const auto& step = *std::ranges::find(offlineGraph.plan().steps,
            std::string_view{"source"}, &talas::runtime::CompileStep::nodeId);
        require(step.resolvedLatencyFrames == reference.latency
            && step.cumulativeOutputLatencyFrames == reference.latency,
            "changing-source oracle must bind the compiler's exact native latency");
        const auto offline = talas::runtime::renderOffline(offlineGraph, reference.outputFrames);
        require(offline.ok(), "changing-source connected timeline must render offline");
        reference.verify(offline.mono, chirp, "offline");

        configuration.eventValueChanges.clear();
        const auto preparedGraph = compile(document(true), configuration);
        for (const auto partition : {31u, 128u})
        {
            talas::runtime::RealtimeRenderSession session(preparedGraph);
            require(session.ready(), "changing-source connected timeline must prepare");
            std::vector<double> actual(reference.outputFrames);
            const auto render = [&] {
                for (std::size_t offset = 0; offset < actual.size(); offset += partition)
                {
                    const auto count = std::min<std::size_t>(partition, actual.size() - offset);
                    for (const auto& change : changes)
                        if (change.frame >= offset && change.frame < offset + count)
                            require(session.scheduleEventValue(change.name, change.value, change.domain,
                                change.frame - offset), "changing-source Rate event must schedule");
                    require(renderNoAllocation(session, std::span(actual).subspan(offset, count)),
                        "changing-source Rate callback must render: " + std::string{session.error()});
                }
            };
            render();
            reference.verify(actual, chirp, "prepared/" + std::to_string(partition));
            near(actual, offline.mono, "changing-source complete callback timeline");
            if (partition == 128)
            {
                allocationCount.store(0, std::memory_order_relaxed);
                countAllocations.store(true, std::memory_order_relaxed);
                session.resetTransport();
                countAllocations.store(false, std::memory_order_relaxed);
                require(allocationCount.load(std::memory_order_relaxed) == 0,
                    "changing-source reset must allocate nothing");
                render();
                near(actual, offline.mono, "changing-source reset rewinds source clock and event timeline");
            }
        }
    }
}

void testIndependentFrequencyAndTimeControls()
{
    auto source = document();
    std::ranges::find(source.graph.nodes.front().parameters,
        std::string_view{"frequencyScale"}, &Parameter::parameterId)->expression = "2";
    const auto graph = compile(source, settings());
    const auto output = talas::runtime::renderOffline(graph, outputFrames);
    require(output.ok(), "independent scale2/rate.25 must render");
    for (const auto profile : {std::pair{3072u, 269.0}, std::pair{11264u, 509.0}})
    {
        const auto measured = std::span(output.mono).subspan(1279 + profile.first, 3072);
        const auto wanted = magnitude(measured, 2 * profile.second);
        require(wanted > 0.02 && wanted > 10 * magnitude(measured, profile.second)
            && wanted > 10 * magnitude(measured, 0.25 * profile.second),
            "quarter-speed source positions must retain independently doubled pitch");
    }
    talas::runtime::RealtimeRenderSession session(graph);
    require(session.ready(), "independent scale2/rate.25 must prepare");
    std::vector<double> actual(outputFrames);
    renderAll(session, actual, 31);
    near(actual, output.mono, "independent scale2/rate.25 complete timeline");
}

void testConnectedPitchAndTimeRemainIndependent()
{
    const talas::test::TimeFrequencyScaleVariableSourceReference reference;
    const std::array<std::pair<std::size_t, double>, 5> scales{{
        {0, 2}, {144037, 0.5}, {246037, 4}, {432053, 0.25}, {510037, 1.5}}};
    // The Rate=.5 event starts an N-frame spectral/raw blend with unknown
    // relative phase. Measure after that declared transition, with unchanged
    // pitch/gain gates and full support around the later Scale4 ramp.
    const std::array centers{1.25, 2.25, 4.5, 5.75, 8.0, 9.75, 11.25, 12.0};
    auto source = document(true);
    std::ranges::find(source.graph.nodes.front().parameters,
        std::string_view{"frequencyScale"}, &Parameter::parameterId)->expression = "2";
    source.graph.nodes.push_back({"scale", "talas.constant", {{"value", "!Scale"}}});
    source.graph.connections.push_back({"scale", "value", "source", "frequencyScale"});
    talas::runtime::RenderSettings configuration{reference.sampleRate, 128};
    configuration.tableResources = {{"Samples/Quarter", reference.recording(false)}};
    configuration.eventValues = {{"Rate", reference.changes.front().second, scalar},
        {"Scale", scales.front().second, scalar}};
    std::vector<talas::runtime::EventValueChange> changes;
    for (std::size_t i = 1; i < reference.changes.size(); ++i)
        changes.push_back({reference.changes[i].first, "Rate", reference.changes[i].second, scalar});
    for (std::size_t i = 1; i < scales.size(); ++i)
        changes.push_back({scales[i].first, "Scale", scales[i].second, scalar});
    std::ranges::sort(changes, {}, &talas::runtime::EventValueChange::frame);
    configuration.eventValueChanges = changes;
    const auto offline = talas::runtime::renderOffline(compile(source, configuration), reference.outputFrames);
    require(offline.ok(), "independently automated scale/rate must render offline");
    constexpr std::size_t neutralChange = 192037, neutralSettled = neutralChange + reference.window;
    const auto& recording = configuration.tableResources.front().samples;
    double neutralError = 0;
    for (std::size_t frame = neutralSettled; frame < neutralSettled + 10240; ++frame)
    {
        const auto position = reference.clock[frame];
        const auto lower = static_cast<std::size_t>(position);
        const auto fraction = position - lower;
        const auto expected = recording[lower] * (1 - fraction) + recording[lower + 1] * fraction;
        neutralError = std::max(neutralError, std::abs(offline.mono[reference.latency + frame] - expected));
    }
    const auto beforeNeutral = magnitude(std::span(offline.mono).subspan(
        reference.latency + neutralChange - 10240, 10240), 86.5, reference.sampleRate);
    const auto afterNeutral = magnitude(std::span(offline.mono).subspan(
        reference.latency + neutralSettled, 10240), 86.5, reference.sampleRate);
    std::cout << "independent_neutral before=" << beforeNeutral << " after=" << afterNeutral
        << " interpolation_error=" << neutralError << '\n';
    require(neutralError < 1e-12 && beforeNeutral > 0.045 && afterNeutral > 0.045,
        "neutral transition endpoints must retain pitch and settled exact source interpolation");
    reference.verify(offline.mono, false, "independent/offline", scales, centers);
    configuration.eventValueChanges.clear();
    const auto graph = compile(source, configuration);
    for (const auto partition : {31u, 128u})
    {
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(), "independently automated scale/rate must prepare");
        std::vector<double> actual(reference.outputFrames);
        const auto render = [&] {
            for (std::size_t offset = 0; offset < actual.size(); offset += partition)
            {
                const auto count = std::min<std::size_t>(partition, actual.size() - offset);
                for (const auto& change : changes)
                    if (change.frame >= offset && change.frame < offset + count)
                        require(session.scheduleEventValue(change.name, change.value, change.domain,
                            change.frame - offset), "independent control event must schedule");
                // Exercise a truly invalid value after valid work, with audible
                // active history and a pending spectral frame. The same public
                // call must be entirely silent and retry without advancing time.
                if (offset <= 300017 && 300017 < offset + count)
                {
                    require(session.scheduleEventValue("Rate", 0, scalar, 300017 - offset),
                        "late independent-control failure must schedule");
                    auto failed = std::span(actual).subspan(offset, count);
                    std::ranges::fill(failed, 99);
                    require(!renderNoAllocation(session, failed)
                        && std::ranges::all_of(failed, [](double value) { return value == 0; }),
                        "late independent-control failure must silence the complete call");
                    require(session.updateEventValue("Rate", 0.5, scalar),
                        "independent-control retry must restore current valid rate");
                }
                require(renderNoAllocation(session, std::span(actual).subspan(offset, count)),
                    "independently automated controls must render");
            }
        };
        render();
        reference.verify(actual, false, "independent/prepared/" + std::to_string(partition), scales, centers);
        near(actual, offline.mono, "independent scale/rate changes and failure retry");
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        session.resetTransport();
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "independent scale/rate reset must allocate nothing");
        render();
        near(actual, offline.mono, "independent scale/rate reset replay");
    }
}

void testDownwardPitchNearDcAndLateRetry()
{
    using Reference = talas::test::TimeFrequencyScaleLowOutputReference;
    for (const auto profile : {Reference{20, 0.0625, 0.1}, Reference{20, 0.125, 0},
             Reference{20, 0.25, 0.1}})
    {
        auto source = document(true);
        std::ranges::find(source.graph.nodes.front().parameters,
            std::string_view{"frequencyScale"}, &Parameter::parameterId)->expression = std::to_string(profile.scale);
        talas::runtime::RenderSettings configuration{profile.sampleRate, 128};
        configuration.tableResources = {{"Samples/Quarter", profile.recording()}};
        configuration.eventValues = {{"Rate", profile.rate, scalar}};
        const auto graph = compile(source, configuration);
        const auto offline = talas::runtime::renderOffline(graph, profile.outputFrames());
        require(offline.ok() && profile.verify(offline.mono, "offline"),
            "downward pitch near DC must satisfy the independent offline reference");
        for (const auto partition : {31u, 128u})
        {
            talas::runtime::RealtimeRenderSession session(graph);
            require(session.ready(), "downward pitch near DC must prepare");
            std::vector<double> actual(profile.outputFrames());
            const auto render = [&] {
                constexpr std::size_t failureFrame = 200817;
                for (std::size_t offset = 0; offset < actual.size(); offset += partition)
                {
                    const auto count = std::min<std::size_t>(partition, actual.size() - offset);
                    auto block = std::span(actual).subspan(offset, count);
                    if (offset <= failureFrame && failureFrame < offset + count)
                    {
                        require(session.scheduleEventValue("Rate", 0, scalar, failureFrame - offset),
                            "low-output late invalid control must schedule");
                        std::ranges::fill(block, 99);
                        require(!renderNoAllocation(session, block)
                            && std::ranges::all_of(block, [](double value) { return value == 0; }),
                            "low-output failure must silence the complete callback");
                        require(session.updateEventValue("Rate", profile.rate, scalar),
                            "low-output corrected rate must restore");
                    }
                    require(renderNoAllocation(session, block), "low-output corrected callback must render");
                }
            };
            render();
            require(profile.verify(actual, "prepared/" + std::to_string(partition)),
                "prepared downward pitch must satisfy the independent reference");
            near(actual, offline.mono, "low-output complete timeline and late retry");
            if (partition == 128)
            {
                allocationCount.store(0, std::memory_order_relaxed);
                countAllocations.store(true, std::memory_order_relaxed);
                session.resetTransport();
                countAllocations.store(false, std::memory_order_relaxed);
                require(allocationCount.load(std::memory_order_relaxed) == 0, "low-output reset must allocate nothing");
                render();
                near(actual, offline.mono, "low-output reset and retry replay");
            }
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--low-output-probe")
        {
            testDownwardPitchNearDcAndLateRetry();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--independent-controls-probe")
        {
            testIndependentFrequencyAndTimeControls();
            testConnectedPitchAndTimeRemainIndependent();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--variable-source-probe")
        {
            testConnectedRateRetainsIndependentChangingSourcePitch();
            return 0;
        }
        require(argc == 1, "unknown quarter-speed reference test arguments");
        testDownwardPitchNearDcAndLateRetry();
        testIndependentFrequencyAndTimeControls();
        testConnectedPitchAndTimeRemainIndependent();
        testConnectedRateRetainsIndependentChangingSourcePitch();
        testQuarterSpeedPreservesMeasuredPitchAndStretchesRecording();
        testConnectedQuarterSpeedTransitionsAndGateRestart();
        testQuarterSpeedFailureRestoresChangedProfileAndSource();
        testIndependentControlsRemainFiniteAndPositive();
        testQuarterSpeedPreservesLowSourceTonesWithAndWithoutDc();
        testSourceWindowPreparationAndConnectedHintAuthority();
        testDefaultMinimumFrequencyBindsLowLatency();
        std::cout << "TimeFrequencyScale quarter-speed reference tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
