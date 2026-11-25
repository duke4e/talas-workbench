#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace talas::dsp
{
struct PreparedTimeFrequencyScaleTestAccess
{
    static int stage(const PreparedTimeFrequencyScale& value)
    { return static_cast<int>(value.frameStage_); }
    static const IncrementalStationaryModel& model(const PreparedTimeFrequencyScale& value)
    { return *value.componentAnalysis_; }
};
struct IncrementalStationaryModelTestAccess
{
    static int stage(const IncrementalStationaryModel& value)
    { return static_cast<int>(value.stage_); }
};
}

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

talas::project::ProjectParameterExpression boolean(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

void testTimeFrequencyScaleCheckpoint(int profile, bool checkedFailure)
{
    using namespace talas::runtime;
    constexpr std::size_t maximumFrames = 257;
    constexpr std::size_t sourceFrames = 16384;
    constexpr std::size_t gateOffFrame = 2560 + 128;
    constexpr std::size_t gateOnFrame = 2560 + maximumFrames;
    RenderSettings settings{48000.0, maximumFrames};
    settings.eventValues = {{"CheckpointFormant", 500.0, talas::expr::ValueDomain::Scalar},
                            {"CheckpointGate", 1.0, talas::expr::ValueDomain::Scalar},
                            {"CheckpointRatio", 1.25, talas::expr::ValueDomain::Scalar},
                            {"CheckpointRate", 0.75, talas::expr::ValueDomain::Scalar}};
    std::vector<double> samples(sourceFrames);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.75 * std::sin(2.0 * std::numbers::pi * 440.0 * i / settings.sampleRate);
    settings.tableResources.push_back({"Samples/Checkpoint", samples});
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"source",
                             "talas.timeFrequencyScale",
                             {{"frequencyScale", profile == 1 ? "1.5" : "1"},
                              {"rate", profile == 1 ? "0.75" : "1"},
                              {"gate", "1"},
                              {"minInputFreq", "20 hz"},
                              {"maxInputFreq", "2000 hz"},
                              {"maxFreqScale", profile == 0 ? "1" : "2"},
                              {"detectors", "10"},
                              boolean("fromDisk", false),
                              resource("sample", "Samples/Checkpoint"),
                              boolean("fromMemoryWriter", false)}},
                            {"gate", "talas.constant", {{"value", "!CheckpointGate"}}},
                            {"fir", "talas.firFilter", {text("coefficients", "0.5 0.25 -0.125")}},
                            {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"gate", "value", "source", "gate"},
                                  {"source", "audioOut", "fir", "audioIn"}};
    if (profile == 2)
    {
        document.graph.nodes.push_back({"ratio", "talas.constant", {{"value", "!CheckpointRatio"}}});
        document.graph.nodes.push_back({"rate", "talas.constant", {{"value", "!CheckpointRate"}}});
        document.graph.connections.push_back({"ratio", "value", "source", "frequencyScale"});
        document.graph.connections.push_back({"rate", "value", "source", "rate"});
    }
    if (checkedFailure)
    {
        document.graph.nodes.push_back(
            {"formant", "talas.constant", {{"value", "!CheckpointFormant"}}});
        document.graph.nodes.push_back({"filter",
                                        "talas.twoFormantElement",
                                        {{"formant1", "500 hz"},
                                         {"bandwidth1", "100 hz"},
                                         {"scale1", "0.75"},
                                         {"formant2", "1000 hz"},
                                         {"bandwidth2", "150 hz"},
                                         {"scale2", "-0.25"}}});
        document.graph.connections.push_back({"formant", "value", "filter", "formant1"});
        document.graph.connections.push_back({"fir", "audioOut", "filter", "audioIn"});
        document.graph.connections.push_back({"filter", "audioOut", "out", "audioIn"});
    }
    else
        document.graph.connections.push_back({"fir", "audioOut", "out", "audioIn"});
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Time Frequency Scale checkpoint graph must compile");
    RealtimeRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    if (!candidate.ready())
        throw std::runtime_error(std::string(candidate.error()));
    require(reference.ready(), "uninterrupted Time Frequency Scale must prepare");
    auto nativeSettings = settings;
    nativeSettings.eventValueChanges = {
        {gateOffFrame, "CheckpointGate", 0.0, talas::expr::ValueDomain::Scalar},
        {gateOnFrame, "CheckpointGate", 1.0, talas::expr::ValueDomain::Scalar}};
    if (profile == 2)
    {
        nativeSettings.eventValueChanges.push_back(
            {gateOnFrame, "CheckpointRatio", 0.75, talas::expr::ValueDomain::Scalar});
        nativeSettings.eventValueChanges.push_back(
            {gateOnFrame, "CheckpointRate", 1.25, talas::expr::ValueDomain::Scalar});
    }
    const auto nativeCompiled =
        compileProject(document, talas::core::makeBuiltinNodeRegistry(), nativeSettings);
    require(nativeCompiled.ok(), "native Time Frequency Scale reference must compile");
    OfflineRenderSession offline(*nativeCompiled.graph);
    std::array<double, maximumFrames> actual{}, expected{};
    std::size_t position = 0, independentPosition = 0;
    double previous1 = 0.0, previous2 = 0.0;
    bool audible = false;
    const auto compare = [&](std::size_t frames)
    {
        const auto native = offline.render(frames);
        if (!native.ok())
        {
            std::cerr << "native failure profile=" << profile << " checked=" << checkedFailure
                      << " position=" << position << " frames=" << frames << '\n';
            for (const auto &diagnostic : native.diagnostics)
                std::cerr << diagnostic.message << '\n';
        }
        require(native.ok(), "native Time Frequency Scale combination must render");
        require(renderWithoutAllocation(candidate, std::span(actual).first(frames)) &&
                    renderWithoutAllocation(reference, std::span(expected).first(frames)),
                "Time Frequency Scale checkpoint must continue after failure");
        for (std::size_t i = 0; i < frames; ++i)
        {
            require(actual[i] == expected[i],
                    "Time Frequency Scale playback, phase and reset history must recover exactly");
            require(std::abs(actual[i] - native.mono[i]) < 1e-9,
                    "Time Frequency Scale checkpoint must match native processing");
            if (profile == 0 && !checkedFailure)
            {
                const auto frame = position + i;
                double sample = 0.0;
                if (frame >= gateOffFrame && frame < gateOnFrame)
                    independentPosition = 0;
                else if (independentPosition < samples.size())
                    sample = samples[independentPosition++];
                const auto filtered = 0.5 * sample + 0.25 * previous1 - 0.125 * previous2;
                require(std::abs(actual[i] - filtered) < 1e-9,
                        "neutral sample playback and FIR must match an independent recurrence");
                previous2 = previous1;
                previous1 = sample;
            }
            audible |= std::abs(actual[i]) > 1e-6;
        }
        position += frames;
    };
    const auto compareUntil = [&](std::size_t end)
    {
        constexpr std::array<std::size_t, 4> blocks{257, 17, 127, 31};
        std::size_t block = 0;
        while (position < end)
            compare(std::min(blocks[block++ % blocks.size()], end - position));
    };
    const auto scheduleBoth = [&](const char *name, double value, std::size_t offset)
    {
        require(
            candidate.scheduleEventValue(name, value, talas::expr::ValueDomain::Scalar, offset) &&
                reference.scheduleEventValue(name, value, talas::expr::ValueDomain::Scalar, offset),
            "Time Frequency Scale control boundary must schedule");
    };
    const auto failureWindow = [&]
    {
        if (checkedFailure)
        {
            require(candidate.scheduleEventValue("CheckpointFormant", 24000.0,
                                                 talas::expr::ValueDomain::Scalar, 256),
                    "late failure must schedule");
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                actual.fill(42.0);
                require(!renderWithoutAllocation(candidate, actual) &&
                            std::ranges::all_of(actual, [](double value) { return value == 0.0; }),
                        "late Time Frequency Scale failure must silence every output sample");
            }
            require(candidate.updateEventValue("CheckpointFormant", 500.0,
                                               talas::expr::ValueDomain::Scalar),
                    "failed Time Frequency Scale callback must allow repair");
            compare(1);
            compare(maximumFrames - 1);
        }
        else
            compare(maximumFrames);
    };
    compareUntil(2560);
    scheduleBoth("CheckpointGate", 0.0, 128);
    failureWindow();
    require(position == gateOnFrame, "gate reset fixture must reach its exact boundary");
    scheduleBoth("CheckpointGate", 1.0, 0);
    if (profile == 2)
    {
        scheduleBoth("CheckpointRatio", 0.75, 0);
        scheduleBoth("CheckpointRate", 1.25, 0);
    }
    const auto postGateRate = profile == 0 ? 1.0 : profile == 1 ? 0.75 : 1.25;
    const auto untilExhaustion =
        static_cast<std::size_t>(std::floor((sourceFrames - 1) / postGateRate) + 1);
    compareUntil(gateOnFrame + untilExhaustion - 128);
    failureWindow();
    compareUntil(position + 1536);
    require(audible, "Time Frequency Scale recovery must cover audible filled history");
    require(
        candidate.updateEventValue("CheckpointGate", 1.0, talas::expr::ValueDomain::Scalar) &&
            candidate.updateEventValue("CheckpointRatio", 1.25, talas::expr::ValueDomain::Scalar) &&
            candidate.updateEventValue("CheckpointRate", 0.75, talas::expr::ValueDomain::Scalar),
        "reset controls must return to their initial values");
    candidate.resetTransport();
    RealtimeRenderSession fresh(*compiled.graph);
    require(fresh.ready(), "fresh reset reference must prepare");
    for (int block = 0; block < 8; ++block)
    {
        require(renderWithoutAllocation(candidate, actual) &&
                    renderWithoutAllocation(fresh, expected),
                "reset Time Frequency Scale must render without allocation");
        require(actual == expected,
                "Time Frequency Scale reset must match fresh playback and phase state");
    }
    std::cout << "profile=" << profile << " failure=" << checkedFailure << " passed\n";
}

// An opt-in diagnostic of the public render call, including rollback snapshots.
// This is separate from the clean-source, fixed-scenario performance campaign.
void probeComponentModelDeadline(std::size_t window, std::size_t blockFrames,
    bool traceRecordedMiss = false, std::size_t partialCount = 8, std::size_t traceOffset = 345568)
{
    using namespace talas::runtime;
    constexpr double sampleRate = 48000, tau = 2 * std::numbers::pi;
    constexpr std::array legacyFrequencies{91.1,171.3,251.9,332.3,411.7,493.1,573.7,653.9};
    std::vector<double> frequencies(partialCount);
    for (std::size_t k = 0; k < partialCount; ++k)
        frequencies[k] = partialCount == 8 ? legacyFrequencies[k] : 30+32*k;
    const auto deadlineUs = 1e6 * blockFrames / sampleRate;
    std::vector<double> samples(40 * 48000, .1);
    for (std::size_t i = 0; i < samples.size(); ++i)
        for (std::size_t partial = 0; partial < frequencies.size(); ++partial)
            samples[i] += .25 / (partial + 1) * std::sin(tau * frequencies[partial]
                * 8192 / window * i / sampleRate + .37 - .63 * partial);

    // Precompute the matching kernel trajectory in a separate pass. Status
    // classification and expected output never run inside the timed callback.
    talas::dsp::PreparedTimeFrequencyScale kernel(sampleRate, blockFrames, window, .5, 2);
    std::vector<double> expected(16 * 48000 + 3 * window);
    std::vector<unsigned char> phases;
    phases.reserve((expected.size() + blockFrames - 1) / blockFrames);
    std::size_t position = 0, settledEnd = 0;
    while (position < expected.size())
    {
        const auto trace = [&] (const char* boundary) {
            const auto& model = talas::dsp::PreparedTimeFrequencyScaleTestAccess::model(kernel);
            std::cout << "recorded_model_state offset=" << position << " boundary=" << boundary
                << " frame_stage=" << talas::dsp::PreparedTimeFrequencyScaleTestAccess::stage(kernel)
                << " model_stage=" << talas::dsp::IncrementalStationaryModelTestAccess::stage(model)
                << " model_work=" << model.consumedWork() << " orders=" << model.attemptedOrders()
                << " assignments=" << model.attemptedAssignments() << '\n';
        };
        const auto tracedBlock = traceRecordedMiss && position == traceOffset;
        if (tracedBlock) trace("before");
        phases.push_back(kernel.componentModelMix() == 1 ? 2 : kernel.componentAnalysisPending() ? 1 : 0);
        const auto frames = std::min(blockFrames, expected.size() - position);
        require(kernel.processSource(samples, .5, 2, {5,7,position}, 0,
            std::span(expected).subspan(position, frames)) == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "model deadline kernel trajectory must render");
        if (tracedBlock) { trace("after"); return; }
        position += frames;
        if (settledEnd == 0 && kernel.componentModelMix() == 1) settledEnd = position + 3 * window;
        if (settledEnd != 0 && position >= settledEnd) break;
    }
    require(!traceRecordedMiss, "recorded model miss offset must be reached by the untimed trace");
    require(settledEnd != 0 && kernel.componentModelMix() == 1, "model deadline covers full component confidence");
    expected.resize(position);
    const auto measured = std::span(expected).last(2 * window);
    for (std::size_t partial = 0; partial < frequencies.size(); ++partial)
    {
        std::complex<double> projection{};
        for (std::size_t i = 0; i < measured.size(); ++i)
            projection += measured[i] * (.5 - .5 * std::cos(tau * i / measured.size()))
                * std::polar(1.0, -tau * frequencies[partial] * 8192 / window * .5 * i / sampleRate);
        require(std::abs(4 * std::abs(projection) / measured.size() - .25 / (partial + 1)) < .002,
            "model deadline retains every independently measured component gain");
    }

    RenderSettings settings{sampleRate, blockFrames};
    settings.tableResources.push_back({"Samples/ModelDeadline", std::move(samples)});
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"source", "talas.timeFrequencyScale",
        {{"frequencyScale", ".5"}, {"rate", "2"}, {"gate", "1"},
         {"minInputFreq", window == 8192 ? "20 hz" : "1.5 hz"}, {"maxInputFreq", "2000 hz"},
         {"maxFreqScale", "2"}, {"detectors", "10"}, boolean("fromDisk", false),
         resource("sample", "Samples/ModelDeadline"), boolean("fromMemoryWriter", false)}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "out", "audioIn"}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "model deadline public graph compiles");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "model deadline public graph prepares");
    std::vector<double> output(blockFrames);
    std::array<std::vector<double>, 3> timings, cpuTimings;
    std::array<std::vector<std::size_t>, 3> offsets;
    for (auto& values : timings) values.reserve(phases.size());
    for (auto& values : cpuTimings) values.reserve(phases.size());
    for (auto& values : offsets) values.reserve(phases.size());
    const auto threadCpuTime = []() -> std::uint64_t {
#if defined(_WIN32)
        throw std::runtime_error("this diagnostic requires a thread CPU clock");
#else
        timespec value{};
        require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, "thread CPU clock is available");
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
#endif
    };
    for (std::size_t block = 0, offset = 0; offset < expected.size(); ++block)
    {
        const auto frames = std::min(blockFrames, expected.size() - offset);
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto cpuStart = threadCpuTime();
        const auto start = std::chrono::steady_clock::now();
        const auto success = session.render(std::span(output).first(frames));
        const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        const auto cpuElapsed = static_cast<double>(threadCpuTime() - cpuStart) / 1000;
        countAllocations.store(false, std::memory_order_relaxed);
        require(success && allocations.load(std::memory_order_relaxed) == 0,
            "model deadline complete callback must succeed without allocation");
        require(std::equal(output.begin(), output.begin() + frames, expected.begin() + offset),
            "model deadline public output matches the classified kernel trajectory exactly");
        if (frames == blockFrames)
        {
            timings[phases[block]].push_back(elapsed);
            cpuTimings[phases[block]].push_back(cpuElapsed);
            offsets[phases[block]].push_back(offset);
        }
        offset += frames;
    }
    const std::array names{"startup_or_transition", "pending_analysis", "active_model"};
    for (std::size_t phase = 0; phase < timings.size(); ++phase)
    {
        auto& values = timings[phase];
        require(!values.empty(), "model deadline samples every relevant state");
        const auto misses = std::ranges::count_if(values, [&](double value) { return value > deadlineUs; });
        auto sorted = values, sortedCpu = cpuTimings[phase];
        const auto cpuMisses = std::ranges::count_if(sortedCpu, [&](double value) { return value > deadlineUs; });
        std::ranges::sort(sorted);
        std::ranges::sort(sortedCpu);
        std::cout << "model_callback window=" << window << " block=" << blockFrames
            << " partials=" << partialCount << " rate=2 kind=" << names[phase] << " samples=" << values.size()
            << " p99_us=" << sorted[(values.size() - 1) * 99 / 100] << " max_us=" << sorted.back()
            << " p99_cpu_us=" << sortedCpu[(values.size() - 1) * 99 / 100] << " max_cpu_us=" << sortedCpu.back()
            << " deadline_us=" << deadlineUs << " misses=" << misses << " cpu_misses=" << cpuMisses
            << " allocations=0 gain_pass=1" << std::endl;
        std::vector<std::size_t> order(values.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::sort(order, [&](auto first, auto second) { return values[first] > values[second]; });
        for (std::size_t rank = 0; rank < std::min<std::size_t>(8, order.size()); ++rank)
        {
            const auto index = order[rank], offset = offsets[phase][index], hop = window / 4;
            const auto nextCapture = offset < window - 1 ? window - 1
                : offset + (hop - (offset - (window - 1)) % hop) % hop;
            std::cout << "model_callback_outlier window=" << window << " block=" << blockFrames
                << " kind=" << names[phase] << " offset=" << offset << " capture=" << (nextCapture < offset + blockFrames)
                << " wall_us=" << values[index] << " cpu_us=" << cpuTimings[phase][index] << '\n';
        }
    }
}

void probeCallbackDeadline(double minimumFrequency, double rate)
{
    using namespace talas::runtime;
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockFrames = 128, measuredBlocks = 1024;
    constexpr double deadlineUs = 1e6 * blockFrames / sampleRate;
    const auto window = talas::dsp::timeFrequencyScaleAnalysisFrameSize(sampleRate, minimumFrequency);
    const auto warmupFrames = 2 * window;
    const auto measuredFrames = blockFrames * measuredBlocks;
    RenderSettings settings{sampleRate, blockFrames};
    settings.eventValues = {{"ProbeFormant", 500.0, talas::expr::ValueDomain::Scalar}};
    std::vector<double> samples(static_cast<std::size_t>(
        (warmupFrames + measuredFrames + window) * std::max(1.0, rate)));
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.1 + 0.25 * std::sin(2.0 * std::numbers::pi * 40.0 * i / sampleRate);
    settings.tableResources.push_back({"Samples/Deadline", std::move(samples)});
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"source", "talas.timeFrequencyScale",
                             {{"frequencyScale", "1"}, {"rate", std::to_string(rate)},
                              {"gate", "1"}, {"minInputFreq", std::to_string(minimumFrequency) + " hz"},
                              {"maxInputFreq", "2000 hz"}, {"maxFreqScale", "2"},
                              {"detectors", "10"}, boolean("fromDisk", false),
                              resource("sample", "Samples/Deadline"),
                              boolean("fromMemoryWriter", false)}},
                            {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "out", "audioIn"}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "deadline source graph must compile");
    RealtimeRenderSession candidate(*compiled.graph);
    require(candidate.ready(), "deadline source graph must prepare");
    std::array<double, blockFrames> scratch{};
    for (std::size_t position = 0; position < warmupFrames; position += blockFrames)
        require(renderWithoutAllocation(candidate, scratch), "deadline warmup must render");

    std::vector<double> rendered(measuredFrames);
    std::array<double, measuredBlocks> durations{};
    const auto timedRender = [](RealtimeRenderSession &session, std::span<double> output,
                               bool expectedSuccess)
    {
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        const auto success = session.render(output);
        const auto duration = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocations.load(std::memory_order_relaxed) == 0,
                "timed callback and rollback must not allocate C++ storage");
        require(success == expectedSuccess, "timed callback must have the expected outcome");
        return duration;
    };
    for (std::size_t block = 0; block < measuredBlocks; ++block)
        durations[block] = timedRender(candidate,
            std::span(rendered).subspan(block * blockFrames, blockFrames), true);

    // The timing fixture must still produce the original pitch and DC. These
    // projections are independent of the renderer and not a same-engine oracle.
    std::complex<double> wanted{}, slowed{};
    double mean = 0.0;
    for (std::size_t i = 0; i < rendered.size(); ++i)
    {
        require(std::isfinite(rendered[i]), "timing output must stay finite");
        const auto weight = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / rendered.size());
        const auto phase = -2.0 * std::numbers::pi * 40.0 * i / sampleRate;
        wanted += rendered[i] * weight * std::complex<double>{std::cos(phase), std::sin(phase)};
        slowed += rendered[i] * weight
            * std::complex<double>{std::cos(phase * rate), std::sin(phase * rate)};
        mean += rendered[i] / rendered.size();
    }
    const auto desiredMagnitude = std::abs(wanted) / rendered.size();
    const auto wrongMagnitude = std::abs(slowed) / rendered.size();
    require(desiredMagnitude > 0.04 && desiredMagnitude > 10.0 * wrongMagnitude
                && std::abs(mean - 0.1) < 0.01,
            "timing fixture must preserve independent pitch, gain and DC evidence");
    const auto report = [&](std::string_view kind, auto &timings)
    {
        const auto misses = std::ranges::count_if(timings,
            [](double duration) { return duration > deadlineUs; });
        std::ranges::sort(timings);
        std::cout << "callback_probe window=" << window << " rate=" << rate
                  << " kind=" << kind << " samples=" << timings.size()
                  << " p50_us=" << timings[timings.size() / 2]
                  << " p95_us=" << timings[(timings.size() - 1) * 95 / 100]
                  << " p99_us=" << timings[(timings.size() - 1) * 99 / 100]
                  << " max_us=" << timings.back() << " deadline_us=" << deadlineUs
                  << " misses=" << misses << " allocations=0" << std::endl;
    };
    report("success", durations);
    std::cout << "callback_oracle window=" << window << " rate=" << rate
              << " desired=" << desiredMagnitude << " wrong=" << wrongMagnitude
              << " mean=" << mean << std::endl;

    // A downstream checked DSP failure occurs AFTER a real synthesis boundary.
    // Both attempts must restore it, and repaired output must match a pristine
    // session exactly, including subsequent synthesis frames.
    document.graph.nodes.push_back({"formant", "talas.constant", {{"value", "!ProbeFormant"}}});
    document.graph.nodes.push_back({"filter", "talas.twoFormantElement",
        {{"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "0.75"},
         {"formant2", "1000 hz"}, {"bandwidth2", "150 hz"}, {"scale2", "-0.25"}}});
    document.graph.connections = {{"source", "audioOut", "filter", "audioIn"},
                                 {"formant", "value", "filter", "formant1"},
                                 {"filter", "audioOut", "out", "audioIn"}};
    const auto failureGraph = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(failureGraph.ok(), "deadline rollback graph must compile");
    RealtimeRenderSession failing(*failureGraph.graph), reference(*failureGraph.graph);
    require(failing.ready() && reference.ready(), "deadline rollback graph must prepare");
    std::array<double, blockFrames> expected{};
    std::size_t position = 0;
    const auto compareUntil = [&](std::size_t end)
    {
        while (position < end)
        {
            const auto frames = std::min(blockFrames, end - position);
            require(renderWithoutAllocation(failing, std::span(scratch).first(frames))
                    && renderWithoutAllocation(reference, std::span(expected).first(frames)),
                    "deadline rollback continuation must render");
            require(std::equal(scratch.begin(), scratch.begin() + frames, expected.begin()),
                    "deadline rollback continuation must exactly match a pristine session");
            position += frames;
        }
    };
    std::array<double, 16> failures{};
    std::array<double, 8> repairs{};
    std::array<double, 16> midHopFailures{};
    std::array<double, 8> midHopRepairs{};
    const auto failAndRepair = [&](std::size_t trial, auto& failedTimings, auto& repairedTimings)
    {
        require(failing.scheduleEventValue("ProbeFormant", 24000.0,
                    talas::expr::ValueDomain::Scalar, blockFrames - 1),
                "deadline late failure must schedule");
        for (std::size_t attempt = 0; attempt < 2; ++attempt)
        {
            scratch.fill(42.0);
            failedTimings[trial * 2 + attempt] = timedRender(failing, scratch, false);
            require(std::ranges::all_of(scratch, [](double value) { return value == 0.0; }),
                    "deadline failed callback must silence the entire output");
        }
        require(failing.updateEventValue("ProbeFormant", 500.0, talas::expr::ValueDomain::Scalar),
                "deadline failure must allow repair");
        repairedTimings[trial] = timedRender(failing, scratch, true);
        require(renderWithoutAllocation(reference, expected) && scratch == expected,
                "deadline repaired callback must exactly match a pristine session");
        require(std::ranges::any_of(scratch, [](double value) { return std::abs(value) > 1e-6; }),
                "deadline rollback must cover audible state");
        position += blockFrames;
    };
    for (std::size_t trial = 0; trial < repairs.size(); ++trial)
    {
        const auto boundary = warmupFrames + trial * (window / 4);
        compareUntil(boundary - 64);
        failAndRepair(trial, failures, repairs);
        // Sample different points through the pending FFT/phase/OLA job. These
        // snapshots must preserve partial work, not merely the old completed hop.
        compareUntil(boundary + (trial + 1) * (window / 4) / 9);
        failAndRepair(trial, midHopFailures, midHopRepairs);
    }
    compareUntil(position + window);
    report("failure_restore", failures);
    report("repaired_retry", repairs);
    report("mid_hop_failure_restore", midHopFailures);
    report("mid_hop_repaired_retry", midHopRepairs);
}
} // namespace
int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--dense-model-deadline-probe")
    {
        for (const auto window : {8192U, 65536U})
            for (const auto block : {32U, 128U}) probeComponentModelDeadline(window, block, false, 16);
        return 0;
    }
    if (argc >= 2 && argc <= 5 && (std::string_view(argv[1]) == "--model-state-trace"
        || std::string_view(argv[1]) == "--dense-model-state-trace"))
    {
        const auto offset = argc >= 3 ? std::stoull(argv[2]) : 345568;
        const auto window = argc >= 4 ? std::stoull(argv[3]) : 65536;
        const auto block = argc >= 5 ? std::stoull(argv[4]) : 32;
        require((window == 8192 || window == 65536) && (block == 32 || block == 128)
            && offset % block == 0, "trace uses a declared window and aligned callback");
        probeComponentModelDeadline(window, block, true,
            std::string_view(argv[1]) == "--dense-model-state-trace" ? 16 : 8,
            offset);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--model-deadline-probe")
    {
        for (const auto window : {8192U, 65536U})
            for (const auto block : {32U, 128U}) probeComponentModelDeadline(window, block);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--deadline-probe")
    {
        for (const auto minimum : {20.0, 10.0, 5.0, 1.5})
            for (const auto rate : {0.25, 2.0})
                probeCallbackDeadline(minimum, rate);
        return 0;
    }
    require(argc == 1, "unknown checkpoint test arguments");
    for (int profile = 0; profile < 3; ++profile)
        for (bool failure : {false, true})
            testTimeFrequencyScaleCheckpoint(profile, failure);
}
