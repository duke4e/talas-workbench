#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <ranges>
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

void* allocate(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string number(double value)
{
    std::array<char, 128> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
        value, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    require(result.ec == std::errc{}, "reference number must format");
    return {buffer.data(), result.ptr};
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.text = std::move(value);
    return result;
}

std::string candidatePole(double sampleRate, double frequency, double bandwidth)
{
    // Candidate only: the documentation does not specify this frequency/bandwidth map.
    const auto radius = std::exp(-std::numbers::pi * bandwidth / sampleRate);
    const auto angle = 2.0 * std::numbers::pi * frequency / sampleRate;
    return number(radius * std::cos(angle)) + " % "
        + number(radius * std::sin(angle));
}

struct FormantSpec
{
    double sampleRate;
    double formant1;
    double bandwidth1;
    double scale1;
    double formant2;
    double bandwidth2;
    double scale2;
};

talas::project::ProjectDocument formantDocument(
    const FormantSpec& spec, bool expanded)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"input", "talas.audioInput", {{"channel1", {}, {}, true}}},
        {"formants", "talas.twoFormantElement", {
            {"formant1", number(spec.formant1) + " hz"},
            {"bandwidth1", number(spec.bandwidth1) + " hz"},
            {"scale1", number(spec.scale1)},
            {"formant2", number(spec.formant2) + " hz"},
            {"bandwidth2", number(spec.bandwidth2) + " hz"},
            {"scale2", number(spec.scale2)}}},
        {"out", "talas.audioOutput", {}},
    };
    if (expanded)
    {
        auto& formants = result.graph.nodes[1];
        formants.typeId = "talas.dualParallelTwoPoleFilter";
        formants.parameters = {
            textParameter("pole1", candidatePole(
                spec.sampleRate, spec.formant1, spec.bandwidth1)),
            {"scale1", number(spec.scale1)},
            textParameter("pole2", candidatePole(
                spec.sampleRate, spec.formant2, spec.bandwidth2)),
            {"scale2", number(spec.scale2)},
        };
    }
    result.graph.connections = {
        {"input", "audioOut", "formants", "audioIn"},
        {"formants", "audioOut", "out", "audioIn"},
    };
    return result;
}

talas::project::ProjectDocument connectedFormantDocument()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"input", "talas.audioInput", {{"channel1", {}, {}, true}}},
        {"formants", "talas.twoFormantElement", {
            {"formant1", "512 hz"}, {"bandwidth1", "128 hz"}, {"scale1", "0.35"},
            {"formant2", "1792 hz"}, {"bandwidth2", "256 hz"}, {"scale2", "0.2"}}},
        {"out", "talas.audioOutput", {}},
        {"formant1Control", "talas.constant", {{"value", "!Formant1"}}},
        {"bandwidth1Control", "talas.constant", {{"value", "!Bandwidth1"}}},
        {"scale1Control", "talas.constant", {{"value", "!Scale1"}}},
        {"formant2Control", "talas.constant", {{"value", "!Formant2"}}},
        {"bandwidth2Control", "talas.constant", {{"value", "!Bandwidth2"}}},
        {"scale2Control", "talas.constant", {{"value", "!Scale2"}}},
    };
    result.graph.connections = {
        {"input", "audioOut", "formants", "audioIn"},
        {"formants", "audioOut", "out", "audioIn"},
        {"formant1Control", "value", "formants", "formant1"},
        {"bandwidth1Control", "value", "formants", "bandwidth1"},
        {"scale1Control", "value", "formants", "scale1"},
        {"formant2Control", "value", "formants", "formant2"},
        {"bandwidth2Control", "value", "formants", "bandwidth2"},
        {"scale2Control", "value", "formants", "scale2"},
    };
    return result;
}

talas::runtime::PreparedRuntimeGraph compile(const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings)
{
    auto result = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(result.ok(), result.diagnostics.empty() ? "formant fixture must compile"
        : result.diagnostics.front().message);
    return std::move(*result.graph);
}

void requireNear(double actual, double expected, double tolerance,
    const std::string& message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::abs(actual - expected) > tolerance)
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
    [[nodiscard]] std::size_t count() const noexcept
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

std::vector<double> toDoubleSamples(std::span<const float> samples)
{
    std::vector<double> result;
    result.reserve(samples.size());
    for (const auto sample : samples)
        result.push_back(static_cast<double>(sample));
    return result;
}

std::vector<double> deterministicInput(std::size_t frames)
{
    std::vector<double> result(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto position = static_cast<double>(frame);
        result[frame] = 0.12 * std::sin(0.137 * position)
            + 0.07 * std::cos(0.291 * position)
            + (frame == 0 ? 0.125 : 0.0);
    }
    return result;
}

talas::runtime::OfflineRenderResult renderOfflineFormants(
    const FormantSpec& spec, bool expanded, std::span<const double> input)
{
    auto settings = talas::runtime::RenderSettings{spec.sampleRate, 64};
    settings.audioInputChannels.push_back({
        1, std::vector<double>(input.begin(), input.end())});
    const auto graph = compile(formantDocument(spec, expanded), std::move(settings));
    return talas::runtime::renderOffline(graph, input.size());
}

bool renderInputAndCount(talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input, std::size_t offset,
    std::span<double> output, std::size_t& allocations)
{
    const float* channel = input.data() + offset;
    const std::array<const float*, 1> channels{channel};
    const talas::runtime::RealtimeAudioInputBlock block{
        channels.data(), channels.size()};
    AllocationCounter counter;
    const auto result = session.render(block, output, std::span<double>{});
    allocations = counter.count();
    return result;
}

void testTwoFormantElementMatchesFixedZeroPoleExpansionAcrossRates()
{
    const std::array<FormantSpec, 3> specs{{
        {8192.0, 512.0, 128.0, 0.3, 1792.0, 256.0, -0.2},
        {16000.0, 900.0, 140.0, -1.0, 4200.0, 750.0, 0.4},
        {44100.0, 2400.0, 220.0, 0.0, 11000.0, 1300.0, 1.0},
    }};
    constexpr std::size_t frames = 193;
    for (const auto& spec : specs)
    {
        const auto input = deterministicInput(frames);
        const auto direct = renderOfflineFormants(spec, false, input);
        const auto expanded = renderOfflineFormants(spec, true, input);
        require(direct.ok() && expanded.ok()
                && direct.mono.size() == frames
                && expanded.mono.size() == frames,
            "both documented formant constructions must render at each test rate");
        require(direct.mono == expanded.mono,
            "TwoFormantElement must match two fixed-zero conjugate-pole sections with independent signed scales");
    }
}

void testTwoFormantBranchesScaleAndSumIndependently()
{
    constexpr FormantSpec base{
        8192.0, 512.0, 128.0, 0.3, 1792.0, 256.0, -0.2};
    constexpr std::size_t frames = 257;
    const auto input = deterministicInput(frames);
    auto firstOnlySpec = base;
    firstOnlySpec.scale1 = 0.3;
    firstOnlySpec.scale2 = 0.0;
    auto secondOnlySpec = base;
    secondOnlySpec.scale1 = 0.0;
    secondOnlySpec.scale2 = -0.2;

    const auto combined = renderOfflineFormants(base, false, input);
    const auto firstOnly = renderOfflineFormants(firstOnlySpec, false, input);
    const auto secondOnly = renderOfflineFormants(secondOnlySpec, false, input);
    require(combined.ok() && firstOnly.ok() && secondOnly.ok(),
        "independent formant section scale fixtures must render");
    for (std::size_t frame = 0; frame < frames; ++frame)
        requireNear(combined.mono[frame],
            firstOnly.mono[frame] + secondOnly.mono[frame], 1.0e-14,
            "TwoFormantElement must add independently scaled signed section outputs");

    auto signReversed = firstOnlySpec;
    signReversed.scale1 = -0.3;
    const auto negativeFirst = renderOfflineFormants(signReversed, false, input);
    require(negativeFirst.ok(), "negative first-formant scale fixture must render");
    for (std::size_t frame = 0; frame < frames; ++frame)
        requireNear(negativeFirst.mono[frame], -firstOnly.mono[frame], 1.0e-14,
            "negative Scale1 must invert only the first parallel section");
}

void testFixedZeroAtDcAndNonzeroNyquistResponse()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 8192;
    constexpr std::size_t firstSettledFrame = 2048;
    constexpr FormantSpec spec{
        sampleRate, 512.0, 128.0, 1.0, 1792.0, 256.0, 0.0};
    auto settings = talas::runtime::RenderSettings{sampleRate, 128};
    settings.audioInputChannels.push_back({1, std::vector<double>(frames)});
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto alternating = frame % 2 == 0 ? 1.0 : -1.0;
        settings.audioInputChannels[0].samples[frame] = 0.5 + 0.25 * alternating;
    }
    const auto graph = compile(formantDocument(spec, false), std::move(settings));
    const auto rendered = talas::runtime::renderOffline(graph, frames);
    require(rendered.ok() && rendered.mono.size() == frames,
        "fixed-zero physical-oracle fixture must render");

    double settledMean = 0.0;
    double nyquistProjection = 0.0;
    for (std::size_t frame = firstSettledFrame; frame < frames; ++frame)
    {
        const auto alternating = frame % 2 == 0 ? 1.0 : -1.0;
        settledMean += rendered.mono[frame];
        nyquistProjection += rendered.mono[frame] * alternating;
    }
    const auto measuredFrames = static_cast<double>(frames - firstSettledFrame);
    settledMean /= measuredFrames;
    nyquistProjection = std::abs(nyquistProjection / measuredFrames);
    requireNear(settledMean, 0.0, 1.0e-10,
        "the documented fixed zeros at z=1 must reject a settled DC input");
    require(nyquistProjection > 0.05,
        "the documented fixed zeros at z=1 must leave a nonzero Nyquist response");
}

struct FormantControls
{
    std::size_t frame;
    double formant1;
    double bandwidth1;
    double scale1;
    double formant2;
    double bandwidth2;
    double scale2;
};

std::array<talas::runtime::EventValueChange, 12> scheduledControlChanges()
{
    return {{
        {37, "Formant1", 768.0, talas::expr::ValueDomain::Scalar},
        {37, "Bandwidth1", 64.0, talas::expr::ValueDomain::Scalar},
        {37, "Scale1", -0.4, talas::expr::ValueDomain::Scalar},
        {37, "Formant2", 2304.0, talas::expr::ValueDomain::Scalar},
        {37, "Bandwidth2", 160.0, talas::expr::ValueDomain::Scalar},
        {37, "Scale2", 0.25, talas::expr::ValueDomain::Scalar},
        {83, "Formant1", 384.0, talas::expr::ValueDomain::Scalar},
        {83, "Bandwidth1", 192.0, talas::expr::ValueDomain::Scalar},
        {83, "Scale1", 0.65, talas::expr::ValueDomain::Scalar},
        {83, "Formant2", 3072.0, talas::expr::ValueDomain::Scalar},
        {83, "Bandwidth2", 384.0, talas::expr::ValueDomain::Scalar},
        {83, "Scale2", -0.1, talas::expr::ValueDomain::Scalar},
    }};
}

std::array<FormantControls, 3> controlSegments()
{
    return {{
        {0, 512.0, 128.0, 0.35, 1792.0, 256.0, -0.2},
        {37, 768.0, 64.0, -0.4, 2304.0, 160.0, 0.25},
        {83, 384.0, 192.0, 0.65, 3072.0, 384.0, -0.1},
    }};
}

std::array<talas::runtime::RealtimeEventValueUpdate, 6> realtimeUpdates(
    const FormantControls& control)
{
    return {{
        {"Formant1", control.formant1, talas::expr::ValueDomain::Scalar},
        {"Bandwidth1", control.bandwidth1, talas::expr::ValueDomain::Scalar},
        {"Scale1", control.scale1, talas::expr::ValueDomain::Scalar},
        {"Formant2", control.formant2, talas::expr::ValueDomain::Scalar},
        {"Bandwidth2", control.bandwidth2, talas::expr::ValueDomain::Scalar},
        {"Scale2", control.scale2, talas::expr::ValueDomain::Scalar},
    }};
}

void renderScheduledIrregularPartitions(
    talas::runtime::RealtimeRenderSession& session,
    std::span<const float> input, std::span<double> output)
{
    constexpr std::array<std::size_t, 3> pattern{1, 7, 31};
    const auto controls = controlSegments();
    std::size_t offset = 0;
    std::size_t partition = 0;
    std::size_t allocations = 0;
    while (offset < output.size())
    {
        const auto frames = std::min(
            pattern[partition % pattern.size()], output.size() - offset);
        if (offset == 8)
        {
            const auto updates = realtimeUpdates(controls[1]);
            // The public API accepts at most three values per transaction.
            require(frames == 31
                    && session.scheduleEventValues(std::span{updates}.first(3), 29)
                    && session.scheduleEventValues(std::span{updates}.subspan(3), 29),
                "first live coefficient update must stage inside a 31-frame callback");
        }
        else if (offset == 79)
        {
            const auto updates = realtimeUpdates(controls[2]);
            require(frames == 7
                    && session.scheduleEventValues(std::span{updates}.first(3), 4)
                    && session.scheduleEventValues(std::span{updates}.subspan(3), 4),
                "second live coefficient update must stage inside a 7-frame callback");
        }
        require(renderInputAndCount(session, input, offset,
                    output.subspan(offset, frames), allocations)
                && allocations == 0,
            "scheduled prepared formant callbacks must render without allocation");
        offset += frames;
        ++partition;
    }
}

struct SectionState
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

double advanceFixedZeroSection(SectionState& state, double input,
    double sampleRate, double centerFrequency, double bandwidth, double scale)
{
    // Independent direct-form recurrence for numerator (1 - z^-1)^2 and
    // candidate conjugate poles. The pole map is a test candidate, not a
    // claim about undocumented historical coefficients.
    const auto radius = std::exp(-std::numbers::pi * bandwidth / sampleRate);
    const auto angle = 2.0 * std::numbers::pi * centerFrequency / sampleRate;
    const auto a1 = -2.0 * radius * std::cos(angle);
    const auto a2 = radius * radius;
    const auto y = ((input - 2.0 * state.x1) + state.x2)
        - a1 * state.y1 - a2 * state.y2;
    state.x2 = state.x1;
    state.x1 = input;
    state.y2 = state.y1;
    state.y1 = y;
    return y * scale;
}

std::vector<double> renderIndependentControlOracle(
    std::span<const float> input, double sampleRate,
    const std::array<FormantControls, 3>& segments)
{
    std::vector<double> output(input.size());
    SectionState first;
    SectionState second;
    std::size_t segment = 0;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        while (segment + 1 < segments.size()
            && frame >= segments[segment + 1].frame)
            ++segment;
        const auto& control = segments[segment];
        const auto sample = static_cast<double>(input[frame]);
        const auto firstOutput = advanceFixedZeroSection(
            first, sample, sampleRate, control.formant1,
            control.bandwidth1, control.scale1);
        const auto secondOutput = advanceFixedZeroSection(
            second, sample, sampleRate, control.formant2,
            control.bandwidth2, control.scale2);
        output[frame] = firstOutput + secondOutput;
    }
    return output;
}

talas::runtime::RenderSettings connectedSettings(std::size_t maxBlockFrames,
    std::span<const float> input, bool scheduled)
{
    auto settings = talas::runtime::RenderSettings{8192.0, maxBlockFrames};
    settings.eventValues = {
        {"Formant1", 512.0, talas::expr::ValueDomain::Scalar},
        {"Bandwidth1", 128.0, talas::expr::ValueDomain::Scalar},
        {"Scale1", 0.35, talas::expr::ValueDomain::Scalar},
        {"Formant2", 1792.0, talas::expr::ValueDomain::Scalar},
        {"Bandwidth2", 256.0, talas::expr::ValueDomain::Scalar},
        {"Scale2", -0.2, talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels.push_back({1, toDoubleSamples(input)});
    if (scheduled)
    {
        const auto changes = scheduledControlChanges();
        settings.eventValueChanges.assign(changes.begin(), changes.end());
    }
    return settings;
}

void testConnectedScheduledCoefficientsMatchIndependentOracleAndPreparedRealtime()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 128;
    std::vector<float> input(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto position = static_cast<double>(frame);
        input[frame] = static_cast<float>(0.15 * std::sin(0.113 * position)
            + 0.09 * std::cos(0.277 * position)
            + (frame == 0 ? 0.1 : 0.0));
    }

    const auto offlineGraph = compile(
        connectedFormantDocument(), connectedSettings(31, input, true));
    const auto realtimeGraph = compile(
        connectedFormantDocument(), connectedSettings(31, input, false));
    const auto formantStep = std::ranges::find(
        realtimeGraph.plan().steps,
        std::string_view{"talas.twoFormantElement"},
        &talas::runtime::CompileStep::typeId);
    require(formantStep != realtimeGraph.plan().steps.end()
            && formantStep->offlineSupport == talas::core::ExecutionSupport::Subset
            && formantStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "connected TwoFormantElement must retain offline Subset and prepared Supported authority");
    talas::runtime::OfflineRenderSession offline(offlineGraph);
    const auto expected = offline.render(frames);
    talas::runtime::RealtimeRenderSession realtime(realtimeGraph);
    require(expected.ok() && realtime.ready(),
        "connected formant coefficient schedule must prepare offline and realtime");
    const auto oracle = renderIndependentControlOracle(
        input, sampleRate, controlSegments());
    require(expected.mono.size() == frames,
        "scheduled connected coefficient output must contain every sample");
    for (std::size_t frame = 0; frame < frames; ++frame)
        requireNear(expected.mono[frame], oracle[frame], 1.0e-12,
            "scheduled connected coefficients must match the independent fixed-zero recurrence");

    std::array<double, frames> actual{};
    renderScheduledIrregularPartitions(realtime, input, actual);
    require(std::ranges::equal(actual, expected.mono),
        "prepared native-float input must match offline across 1/7/31-frame partitions");

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        realtime.resetTransport();
        resetAllocations = counter.count();
    }
    require(resetAllocations == 0,
        "prepared TwoFormantElement reset must not allocate");
    std::array<double, frames> resetOutput{};
    renderScheduledIrregularPartitions(realtime, input, resetOutput);
    require(resetOutput == actual,
        "prepared reset must restore both section histories and scheduled controls");

    offline.reset();
    const auto replay = offline.render(frames);
    require(replay.ok() && replay.mono == expected.mono,
        "offline reset must replay both formant sections and the connected-control schedule");
}

void testCoefficientFailureAndScheduledRetryRestoreFilterState()
{
    constexpr double sampleRate = 8192.0;
    auto settings = talas::runtime::RenderSettings{sampleRate, 16};
    settings.eventValues = {
        {"Formant1", 512.0, talas::expr::ValueDomain::Scalar},
        {"Bandwidth1", 128.0, talas::expr::ValueDomain::Scalar},
        {"Scale1", 0.35, talas::expr::ValueDomain::Scalar},
        {"Formant2", 1792.0, talas::expr::ValueDomain::Scalar},
        {"Bandwidth2", 256.0, talas::expr::ValueDomain::Scalar},
        {"Scale2", -0.2, talas::expr::ValueDomain::Scalar},
    };
    const auto graph = compile(connectedFormantDocument(), std::move(settings));
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession reference(graph);
    require(candidate.ready() && reference.ready(),
        "connected coefficient failure fixtures must prepare");

    const std::array<float, 8> warmInput{
        0.125F, -0.25F, 0.1875F, -0.0625F,
        0.25F, -0.125F, 0.0625F, -0.1875F};
    std::array<double, 8> warmCandidate{};
    std::array<double, 8> warmReference{};
    std::size_t allocations = 0;
    require(renderInputAndCount(candidate, warmInput, 0, warmCandidate, allocations)
            && allocations == 0
            && renderInputAndCount(reference, warmInput, 0, warmReference, allocations)
            && allocations == 0
            && warmCandidate == warmReference,
        "coefficient rollback fixture must establish matching live histories");

    const std::array invalidBandwidth{
        talas::runtime::RealtimeEventValueUpdate{
            "Bandwidth1", std::numeric_limits<double>::min(),
            talas::expr::ValueDomain::Scalar},
    };
    require(candidate.scheduleEventValues(invalidBandwidth, 2),
        "finite bandwidth that rounds the candidate radius to one must stage");
    const std::array<float, 4> retryInput{0.1F, -0.2F, 0.15F, -0.05F};
    std::array<double, 4> rejected;
    rejected.fill(1.0);
    require(!renderInputAndCount(candidate, retryInput, 0, rejected, allocations)
            && allocations == 0 && rejected == std::array<double, 4>{},
        "unstable candidate pole radius must reject the whole callback");
    require(candidate.currentGlobalEventValue(
                "Bandwidth1", talas::expr::ValueDomain::Scalar) == 128.0,
        "failed coefficient update must restore the committed bandwidth value");

    const std::array validBandwidth{
        talas::runtime::RealtimeEventValueUpdate{
            "Bandwidth1", 128.0, talas::expr::ValueDomain::Scalar},
    };
    require(candidate.scheduleEventValues(validBandwidth, 2),
        "restored bandwidth must stage for retry at the failed frame");
    std::array<double, 4> retryCandidate{};
    std::array<double, 4> retryReference{};
    require(renderInputAndCount(candidate, retryInput, 0,
                retryCandidate, allocations)
            && allocations == 0
            && renderInputAndCount(reference, retryInput, 0,
                retryReference, allocations)
            && allocations == 0
            && retryCandidate == retryReference,
        "coefficient retry must restore both histories, input position, and event state");
}
}

int main()
{
    try
    {
        testTwoFormantElementMatchesFixedZeroPoleExpansionAcrossRates();
        testTwoFormantBranchesScaleAndSumIndependently();
        testFixedZeroAtDcAndNonzeroNyquistResponse();
        testConnectedScheduledCoefficientsMatchIndependentOracleAndPreparedRealtime();
        testCoefficientFailureAndScheduledRetryRestoreFilterState();
        std::cout << "Two-formant reference tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
