#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

namespace
{
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
    if (!condition) throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance,
    const std::string& message)
{
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

class AllocationCounter
{
public:
    AllocationCounter() {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }
    ~AllocationCounter() {
        countAllocations.store(false, std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

talas::project::ProjectDocument document(bool expanded, double frequency = 128.0,
    double freqScale = 1.5, double rightFrequency = 0.0)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", std::to_string(frequency) + " hz"},
            {"amplitude", "0.05"}}},
        {"out", "talas.audioOutput", {}},
    };
    if (rightFrequency > 0.0)
        result.graph.nodes.push_back({"rightSource", "talas.oscillator", {
            {"frequency", std::to_string(rightFrequency) + " hz"},
            {"amplitude", "0.025"}}});

    if (expanded)
    {
        // Unclipped, as a SingleSideBandRM's own shifters are.
        result.graph.nodes.push_back({"phase", "talas.phaseShiftBy90", {
            {"frequency", std::to_string(frequency) + " hz"}, {"overflow", {}, "unclipped"}}});
        result.graph.nodes.push_back({"quad", "talas.quadOscillator", {{
            "frequency", std::to_string(frequency * (freqScale - 1.0)) + " hz"}}});
        result.graph.connections = {
            {"source", "audioOut", "phase", "audioIn"},
            {"phase", "audioOut", "quad", "envelope"},
            {"phase", "rightOut", "quad", "rightEnvelope"},
            {"quad", "audioOut", "out", "audioIn"},
        };
        if (rightFrequency > 0.0)
            result.graph.connections.push_back({"rightSource", "audioOut", "phase", "rightIn"});
    }
    else
    {
        result.graph.nodes.push_back({"ssb", "talas.singleSideBandRm", {
            {"frequency", std::to_string(frequency) + " hz"},
            {"freqScale", std::to_string(freqScale)}}});
        result.graph.connections = {
            {"source", "audioOut", "ssb", "audioIn"},
            {"ssb", "audioOut", "out", "audioIn"},
        };
        if (rightFrequency > 0.0)
            result.graph.connections.push_back({"rightSource", "audioOut", "ssb", "rightIn"});
    }
    return result;
}

talas::project::ProjectDocument connectedControlDocument(bool expanded)
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"source", "talas.oscillator", {
            {"frequency", "128 hz"}, {"amplitude", "0.05"}}},
        {"out", "talas.audioOutput", {}},
    };
    if (expanded)
    {
        result.graph.nodes.push_back({"phase", "talas.phaseShiftBy90", {
            {"frequency", "128 hz"}, {"overflow", {}, "unclipped"}}});
        result.graph.nodes.push_back({"quad", "talas.quadOscillator", {
            {"frequency", "64 hz"}}});
        result.graph.nodes.push_back({"frequencyControl", "talas.constant", {
            {"value", "!SsbFrequency"}}});
        result.graph.nodes.push_back({"carrierControl", "talas.constant", {
            {"value", "!SsbCarrier"}}});
        result.graph.connections = {
            {"source", "audioOut", "phase", "audioIn"},
            {"frequencyControl", "value", "phase", "frequency"},
            {"phase", "audioOut", "quad", "envelope"},
            {"phase", "rightOut", "quad", "rightEnvelope"},
            {"carrierControl", "value", "quad", "frequency"},
            {"quad", "audioOut", "out", "audioIn"},
        };
    }
    else
    {
        result.graph.nodes.push_back({"ssb", "talas.singleSideBandRm", {
            {"frequency", "128 hz"}, {"freqScale", "1.5"}}});
        result.graph.nodes.push_back({"frequencyControl", "talas.constant", {
            {"value", "!SsbFrequency"}}});
        result.graph.nodes.push_back({"scaleControl", "talas.constant", {
            {"value", "!SsbScale"}}});
        result.graph.connections = {
            {"source", "audioOut", "ssb", "audioIn"},
            {"frequencyControl", "value", "ssb", "frequency"},
            {"scaleControl", "value", "ssb", "freqScale"},
            {"ssb", "audioOut", "out", "audioIn"},
        };
    }
    return result;
}

talas::project::ProjectDocument stereoInputFailureDocument()
{
    talas::project::ProjectDocument result;
    result.graph.nodes = {
        {"input", "talas.audioInput", {
            {"routing", {}, "stereoPair"},
            {"channel1", {}, {}, true},
            {"channel2", {}, {}, true}}},
        {"ssb", "talas.singleSideBandRm", {
            {"frequency", "0.5 hz"}, {"freqScale", "1.5"}}},
        {"gain", "talas.level", {
            {"left", "!SsbFailureGain"}, {"right", "0"},
            {"noGain", {}, {}, false}}},
        {"out", "talas.audioOutput", {}},
    };
    result.graph.connections = {
        {"input", "audioOut", "ssb", "audioIn"},
        {"input", "rightOut", "ssb", "rightIn"},
        {"ssb", "audioOut", "gain", "audioIn"},
        {"gain", "leftOut", "out", "audioIn"},
    };
    return result;
}

talas::runtime::PreparedRuntimeGraph compile(const talas::project::ProjectDocument& project,
    talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(
        project, talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
    {
        if (!diagnostics.empty()) diagnostics += '\n';
        diagnostics += diagnostic.message;
    }
    require(compiled.ok(), diagnostics.empty() ? "SSB reference graph must compile" : diagnostics);
    return std::move(*compiled.graph);
}

double measureTone(std::span<const double> samples, double sampleRate,
    double frequency, std::size_t firstFrame)
{
    require(firstFrame < samples.size(), "tone projection window must be nonempty");
    double sineProjection = 0.0;
    double cosineProjection = 0.0;
    for (std::size_t frame = firstFrame; frame < samples.size(); ++frame)
    {
        const auto angle = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sineProjection += samples[frame] * std::sin(angle);
        cosineProjection += samples[frame] * std::cos(angle);
    }
    return 2.0 * std::hypot(sineProjection, cosineProjection)
        / static_cast<double>(samples.size() - firstFrame);
}

bool renderAndCount(talas::runtime::RealtimeRenderSession& session,
    std::span<double> output, std::size_t& allocations)
{
    AllocationCounter counter;
    const auto result = session.render(output);
    allocations = counter.count();
    return result;
}

bool renderInputAndCount(talas::runtime::RealtimeRenderSession& session,
    talas::runtime::RealtimeAudioInputBlock input, std::span<double> output,
    std::size_t& allocations)
{
    AllocationCounter counter;
    const auto result = session.render(input, output, {});
    allocations = counter.count();
    return result;
}

void renderIrregularPartitions(
    talas::runtime::RealtimeRenderSession& session,
    std::span<double> output)
{
    constexpr std::array<std::size_t, 3> pattern{1, 7, 31};
    std::size_t offset = 0;
    std::size_t partition = 0;
    std::size_t allocations = 0;
    while (offset < output.size())
    {
        const auto frames = std::min(pattern[partition % pattern.size()], output.size() - offset);
        require(renderAndCount(session, output.subspan(offset, frames), allocations)
                && allocations == 0,
            "irregular SSB callback must render without allocation");
        offset += frames;
        ++partition;
    }
}

void testDirectMatchesExpansionAtUpDownAndUnityScales()
{
    constexpr double sampleRate = 8192.0;
    for (const auto scale : std::array<double, 3>{1.5, 0.5, 1.0})
    {
        const auto direct = compile(document(false, 128.0, scale),
            {sampleRate, 64});
        const auto expanded = compile(document(true, 128.0, scale),
            {sampleRate, 64});
        const auto actual = talas::runtime::renderOffline(direct, 2048);
        const auto reference = talas::runtime::renderOffline(expanded, 2048);
        require(actual.ok() && reference.ok(), "positive, unity, and downward SSB fixtures must render");
        require(actual.mono == reference.mono,
            "SingleSideBandRM must match PhaseShiftBy90 plus summed QuadOscillator");
    }
}

void testFrequencyScaleSelectsExpectedSideband()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 8192;
    constexpr std::size_t firstMeasuredFrame = 4096;
    for (const auto& [scale, wanted, image] :
        std::array<std::array<double, 3>, 3>{{
            {1.5, 192.0, 64.0},
            {0.5, 64.0, 192.0},
            {1.0, 128.0, 128.0},
        }})
    {
        const auto graph = compile(document(false, 128.0, scale),
            {sampleRate, 128});
        const auto rendered = talas::runtime::renderOffline(graph, frames);
        require(rendered.ok() && rendered.mono.size() == frames, "SSB projection must render mono output");
        const auto wantedAmplitude = measureTone(
            rendered.mono, sampleRate, wanted, firstMeasuredFrame);
        require(wantedAmplitude > 0.01,
            "scaled input tone must remain present at the expected sideband");
        for (std::size_t frame = firstMeasuredFrame; frame < frames; ++frame)
        {
            const auto expected = 0.05 * std::sin(2.0 * std::numbers::pi * wanted
                * static_cast<double>(frame) / sampleRate
                - std::numbers::pi / 4.0);
            requireNear(rendered.mono[frame], expected, 1.0e-10,
                "settled output must match the unit-gain, minus-45-degree candidate model");
        }
        if (scale != 1.0)
        {
            const auto imageAmplitude = measureTone(
                rendered.mono, sampleRate, image, firstMeasuredFrame);
            require(imageAmplitude < wantedAmplitude * 1.0e-4,
                "quadrature modulation must suppress the opposite sideband");
        }
    }
}

void testFiniteAliasedCarrierRemainsAdmitted()
{
    // The SSB composite retains its finite-carrier admission profile even
    // when the derived carrier lies above Nyquist. This does not assert that
    // the documented construction has this aliasing behavior.
    const auto graph = compile(document(false, 128.0, 40.0),
        {8192.0, 128});
    const auto rendered = talas::runtime::renderOffline(graph, 1024);
    require(rendered.ok() && rendered.mono.size() == 1024
            && std::ranges::all_of(rendered.mono, [](double sample) {
                return std::isfinite(sample);
            }),
        "finite aliased SSB carrier must remain admitted by the composite path");
    talas::runtime::RealtimeRenderSession realtime(graph);
    std::array<double, 1024> output{};
    require(realtime.ready(), "finite aliased SSB carrier must prepare");
    renderIrregularPartitions(realtime, output);
    require(std::ranges::equal(output, rendered.mono),
        "finite aliased SSB carrier must agree across both backends");
}

void testExplicitRightInputMatchesExpansionAndContributes()
{
    constexpr double sampleRate = 8192.0;
    constexpr std::size_t frames = 8192;
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document(false, 128.0, 1.5, 256.0)));
    require(reopened.ok(), "separate SSB right input must survive save/reopen");
    const auto direct = compile(reopened.document, {sampleRate, 128});
    const auto expanded = compile(document(true, 128.0, 1.5, 256.0),
        {sampleRate, 128});
    const auto duplicated = compile(document(false, 128.0, 1.5),
        {sampleRate, 128});
    const auto actual = talas::runtime::renderOffline(direct, frames);
    const auto reference = talas::runtime::renderOffline(expanded, frames);
    const auto defaultRight = talas::runtime::renderOffline(duplicated, frames);
    require(actual.ok() && reference.ok() && defaultRight.ok(),
        "explicit and duplicated SSB input fixtures must render");
    require(actual.mono == reference.mono,
        "explicit SSB rightIn must follow the documented phase-pair expansion");
    const auto rightUpperSideband = measureTone(
        actual.mono, sampleRate, 320.0, 4096);
    require(rightUpperSideband > 0.005,
        "explicit rightIn content must contribute to the mono SSB output");
    double difference = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame)
        difference = std::max(difference,
            std::abs(actual.mono[frame] - defaultRight.mono[frame]));
    require(difference > 1.0e-4,
        "explicit rightIn must differ from the default duplicated-left input");
    talas::runtime::RealtimeRenderSession realtime(direct);
    std::array<double, frames> output{};
    require(realtime.ready(), "reopened stereo-input SSB graph must prepare");
    renderIrregularPartitions(realtime, output);
    require(std::ranges::equal(output, actual.mono),
        "both independently driven SSB histories must agree across callback partitions");
}

void testConnectedSchedulesMatchExpansionAcrossRealtimePartitions()
{
    constexpr std::size_t frames = 320;
    talas::runtime::RenderSettings directSettings{8192.0, 31};
    directSettings.eventValues = {
        {"SsbFrequency", 128.0, talas::expr::ValueDomain::Scalar},
        {"SsbScale", 1.5, talas::expr::ValueDomain::Scalar},
    };
    directSettings.eventValueChanges = {
        {64, "SsbFrequency", 96.0, talas::expr::ValueDomain::Scalar},
        {128, "SsbScale", 0.5, talas::expr::ValueDomain::Scalar},
        {128, "SsbFrequency", 128.0, talas::expr::ValueDomain::Scalar},
        {256, "SsbScale", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto expandedSettings = directSettings;
    expandedSettings.eventValues = {
        {"SsbFrequency", 128.0, talas::expr::ValueDomain::Scalar},
        {"SsbCarrier", 64.0, talas::expr::ValueDomain::Scalar},
    };
    expandedSettings.eventValueChanges = {
        {64, "SsbFrequency", 96.0, talas::expr::ValueDomain::Scalar},
        {64, "SsbCarrier", 48.0, talas::expr::ValueDomain::Scalar},
        {128, "SsbCarrier", -64.0, talas::expr::ValueDomain::Scalar},
        {128, "SsbFrequency", 128.0, talas::expr::ValueDomain::Scalar},
        {256, "SsbCarrier", 0.0, talas::expr::ValueDomain::Scalar},
    };

    const auto direct = compile(connectedControlDocument(false), directSettings);
    const auto expanded = compile(connectedControlDocument(true), expandedSettings);
    const auto offlineDirect = talas::runtime::renderOffline(direct, frames);
    const auto offlineExpanded = talas::runtime::renderOffline(expanded, frames);
    talas::runtime::RealtimeRenderSession directSession(direct);
    talas::runtime::RealtimeRenderSession expandedSession(expanded);
    require(offlineDirect.ok() && offlineExpanded.ok()
            && directSession.ready() && expandedSession.ready(),
        "connected SSB controls and corresponding expansion must prepare");
    require(offlineDirect.mono == offlineExpanded.mono,
        "scheduled SSB controls must match scheduled phase and carrier controls");

    std::array<double, frames> realtimeDirect{};
    std::array<double, frames> realtimeExpanded{};
    renderIrregularPartitions(directSession, realtimeDirect);
    renderIrregularPartitions(expandedSession, realtimeExpanded);
    require(std::ranges::equal(realtimeDirect, offlineDirect.mono)
            && std::ranges::equal(realtimeExpanded, offlineExpanded.mono),
        "connected SSB and expansion must match offline across irregular realtime partitions");

    std::size_t resetAllocations = 0;
    {
        AllocationCounter counter;
        directSession.resetTransport();
        expandedSession.resetTransport();
        resetAllocations = counter.count();
    }
    std::array<double, frames> resetDirect{};
    std::array<double, frames> resetExpanded{};
    require(resetAllocations == 0,
        "SSB and expanded graph transport reset must not allocate");
    renderIrregularPartitions(directSession, resetDirect);
    renderIrregularPartitions(expandedSession, resetExpanded);
    require(resetDirect == realtimeDirect && resetExpanded == realtimeExpanded,
        "transport reset must restore scheduled SSB filter and carrier state");
}

void testLateDownstreamFailureRestoresBothInputHistories()
{
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"SsbFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto graph = compile(stereoInputFailureDocument(), settings);
    talas::runtime::RealtimeRenderSession candidate(graph);
    talas::runtime::RealtimeRenderSession reference(graph);
    require(candidate.ready() && reference.ready(),
        "stereo SSB failure fixtures must prepare");

    std::array<const float*, 2> channelPointers{};
    const auto makeInput = [&](std::array<float, 4>& left,
                               std::array<float, 4>& right) {
        channelPointers = {left.data(), right.data()};
        return talas::runtime::RealtimeAudioInputBlock{
            channelPointers.data(), channelPointers.size()};
    };
    std::array<float, 4> warmLeft{0.1F, -0.2F, 0.15F, -0.1F};
    std::array<float, 4> warmRight{-0.15F, 0.25F, -0.05F, 0.2F};
    std::array<double, 4> candidateWarm{};
    std::array<double, 4> referenceWarm{};
    std::size_t allocations = 0;
    require(renderInputAndCount(candidate, makeInput(warmLeft, warmRight),
                candidateWarm, allocations)
            && allocations == 0
            && renderInputAndCount(reference, makeInput(warmLeft, warmRight),
                referenceWarm, allocations)
            && allocations == 0
            && candidateWarm == referenceWarm,
        "both SSB filter histories must warm independently without allocation");

    require(candidate.updateEventValue("SsbFailureGain",
                std::numeric_limits<double>::max(),
                talas::expr::ValueDomain::Scalar),
        "SSB failure fixture must arm a late downstream overflow");
    std::array<float, 4> failingLeft{1000000.0F, 1000000.0F, 1000000.0F, 1000000.0F};
    std::array<float, 4> failingRight{-250000.0F, -250000.0F, -250000.0F, -250000.0F};
    std::array<double, 4> rejected;
    rejected.fill(17.0);
    require(!renderInputAndCount(candidate, makeInput(failingLeft, failingRight),
                rejected, allocations)
            && allocations == 0
            && rejected == std::array<double, 4>{},
        "late downstream overflow must publish no partial SSB callback");
    require(candidate.updateEventValue("SsbFailureGain", 1.0,
                talas::expr::ValueDomain::Scalar),
        "SSB failure fixture must restore its valid downstream gain");

    std::array<float, 4> retryLeft{0.25F, -0.15F, 0.05F, -0.2F};
    std::array<float, 4> retryRight{-0.05F, 0.2F, -0.3F, 0.1F};
    std::array<double, 4> retry{};
    std::array<double, 4> expected{};
    require(renderInputAndCount(candidate, makeInput(retryLeft, retryRight),
                retry, allocations)
            && allocations == 0
            && renderInputAndCount(reference, makeInput(retryLeft, retryRight),
                expected, allocations)
            && allocations == 0
            && retry == expected,
        "failed SSB callback must restore both filter histories and carrier phase for retry");

}

void testOfflineFailureRestoresBothHistoriesCarrierAndSchedule()
{
    auto settings = talas::runtime::RenderSettings{8.0, 4};
    settings.eventValues = {
        {"SsbFailureGain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {7, "SsbFailureGain", std::numeric_limits<double>::max(),
            talas::expr::ValueDomain::Scalar},
    };
    settings.audioInputChannels = {
        {1, {0.1, -0.2, 0.15, -0.1, 0.25, -0.15, 0.05, -0.2}},
        {2, {-0.15, 0.25, -0.05, 0.2, -0.05, 0.2, -0.3, 1000000.0}},
    };
    auto graph = compile(stereoInputFailureDocument(), settings);
    talas::runtime::OfflineRenderSession candidate(graph);
    talas::runtime::OfflineRenderSession reference(graph);
    const auto warm = candidate.render(4);
    const auto referenceWarm = reference.render(4);
    require(warm.ok() && referenceWarm.ok() && warm.mono == referenceWarm.mono,
        "offline SSB rollback fixture must warm both histories and carrier phase");
    const auto rejected = candidate.render(4);
    require(!rejected.ok() && rejected.mono.empty() && rejected.channels.empty(),
        "late offline SSB failure must publish no partial output");
    const auto retry = candidate.render(3);
    const auto expected = reference.render(3);
    require(retry.ok() && expected.ok() && retry.mono == expected.mono,
        "offline retry must restore both SSB histories, carrier phase and input position");
    const auto repeated = candidate.render(1);
    require(!repeated.ok() && repeated.mono.empty(),
        "offline SSB retry must preserve the scheduled absolute failure frame");
    candidate.reset();
    const auto reset = candidate.render(7);
    const auto fresh = talas::runtime::renderOffline(graph, 7);
    require(reset.ok() && fresh.ok() && reset.mono == fresh.mono,
        "offline SSB reset must restore both histories, carrier phase and schedule");
}

void testRightInputRequiresCompilerOwnedAuthority()
{
    const auto compiled = compile(document(false, 128.0, 1.5, 256.0),
        {8192.0, 31});
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto graph = compiled;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(graph.plan());
        const auto ssb = std::ranges::find(plan.steps, "ssb",
            &talas::runtime::CompileStep::nodeId);
        require(ssb != plan.steps.end(), "authority fixture must own its SSB step");
        const auto rightInput = std::ranges::find(ssb->inputs, "rightIn",
            &talas::runtime::InputBinding::inputPortId);
        const auto output = std::ranges::find_if(plan.buffers, [](const auto& buffer) {
            return buffer.nodeId == "ssb" && buffer.portId == "audioOut";
        });
        require(rightInput != ssb->inputs.end() && output != plan.buffers.end(),
            "authority fixture must own separate right input and mono output");
        if (mutation == 0)
            --plan.buffers[rightInput->bufferIndex].frames;
        else if (mutation == 1)
            rightInput->bufferIndex = static_cast<std::size_t>(output - plan.buffers.begin());
        else
            plan.buffers[rightInput->bufferIndex].nodeId += "Forged";
        talas::runtime::RealtimeRenderSession realtime(graph);
        const auto offline = talas::runtime::renderOffline(graph, 4);
        require(!realtime.ready() && !offline.ok() && offline.mono.empty(),
            "both backends must reject forged SSB right buffer, binding or producer");
    }
}
}

int main()
{
    try
    {
        testDirectMatchesExpansionAtUpDownAndUnityScales();
        testFrequencyScaleSelectsExpectedSideband();
        testFiniteAliasedCarrierRemainsAdmitted();
        testExplicitRightInputMatchesExpansionAndContributes();
        testConnectedSchedulesMatchExpansionAcrossRealtimePartitions();
        testLateDownstreamFailureRestoresBothInputHistories();
        testOfflineFailureRestoresBothHistoriesCarrierAndSchedule();
        testRightInputRequiresCompilerOwnedAuthority();
        std::cout << "SingleSideBandRM reference tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
