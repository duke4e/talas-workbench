#include "../../src/runtime/PreparedScoreClipSource.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/ScoreClipMaterialization.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectJson.h"
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <ranges>
#include <stdexcept>

namespace talas::runtime
{
struct RealtimeRenderSessionTestAccess
{
    static auto checkpoint(RealtimeRenderSession& session) { return session.beginOfflineTransaction(); }
};
}

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<std::size_t> allocations{0}, deallocations{0};
}
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept
{
    if (p && countAllocations.load(std::memory_order_relaxed))
        deallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace
{
using namespace talas::runtime;
using namespace talas::project;
constexpr std::size_t sampleCount = 24;
void require(bool value, std::string message) { if (!value) throw std::runtime_error(std::move(message)); }
const auto& registry() { static const auto value = talas::core::makeBuiltinNodeRegistry(); return value; }
ScoreTime seconds(double value) { return {ScoreTimeDomain::Seconds, value, 0}; }
ProjectParameterExpression boolean(std::string id, bool value)
{ ProjectParameterExpression p; p.parameterId = std::move(id); p.boolean = value; return p; }
ProjectNode device(std::string id, unsigned channel)
{
    return {std::move(id), "talas.audioInput", {boolean("channel1", channel == 1),
        boolean("channel2", channel == 2), boolean("channel3", channel == 3)}};
}
ProjectSoundDefinition gateDefinition(std::string id, unsigned channel, bool resumeOnZero = false)
{
    ProjectSoundDefinition result; result.definitionId = std::move(id); result.displayName = "Timeline WaitUntil";
    result.graph.nodes = {
        {"input", "talas.oscillator", {{"frequency", "11 hz"}, {"amplitude", "0"}}},
        device("resumeInput", channel),
        {"resume", "talas.peakDetector", {{"attackTime", "1 ms"}, {"releaseTime", "1 ms"}}},
        {"threshold", "talas.threshold", {{"threshold", "0.5"}}},
        {"wait", "talas.waitUntil", {boolean("resumeOnZero", resumeOnZero)}}};
    result.graph.connections = {{"input", "audioOut", "wait", "audioIn"},
        {"resumeInput", "audioOut", "resume", "audioIn"}, {"resume", "value", "threshold", "value"},
        {"threshold", "value", "wait", "resume"}};
    result.outputs = {{"audioOut", talas::core::PortType::Audio, "wait", "audioOut"}};
    return result;
}
ProjectDocument fixture(bool resumeOnZero = false, bool simultaneous = false)
{
    ProjectDocument result;
    ProjectSoundDefinition background; background.definitionId = "background"; background.displayName = "Background";
    background.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}}};
    background.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    ProjectSoundExposedParameter gain;
    gain.parameterId = "gain"; gain.displayName = "Gain"; gain.defaultValue = {"gain", "0.5"};
    gain.bindings = {{ProjectSoundParameterTarget::EventValue, {}, {}, "Gain", talas::expr::ValueDomain::Scalar}};
    background.exposedParameters = {gain};
    result.soundDefinitions = {background, gateDefinition("gate", 2, resumeOnZero)};
    result.timeline.score.tracks = {
        {"track", "Track", ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 1, ScoreOverlapMode::Layer},
        {"master", "Master", ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 1, ScoreOverlapMode::Layer}};
    result.timeline.score.clips = {
        {"background", "track", "background", seconds(0), seconds(0.020), seconds(0), seconds(0), seconds(0), 1},
        {"gate", "track", "gate", seconds(0.002), seconds(0.004), seconds(0), seconds(0), seconds(0), simultaneous ? 1U : 2U}};
    result.timeline.score.automation = {{"gain", ScoreAutomationScope::Clip, "background", "Gain",
        talas::expr::ValueDomain::Scalar, {{seconds(0.003), 0.25, talas::core::AutomationInterpolation::Step},
                                     {seconds(0.007), 0.75, talas::core::AutomationInterpolation::Step}}}};
    if (simultaneous)
    {
        result.soundDefinitions.push_back(gateDefinition("otherGate", 3));
        auto clip = result.timeline.score.clips[1]; clip.clipId = "otherGate"; clip.soundDefinitionId = "otherGate";
        result.timeline.score.clips.push_back(clip);
    }
    return result;
}
RenderSettings settings(const ProjectDocument& document, std::size_t block = 8)
{
    RenderSettings result; result.sampleRate = 1000; result.maxBlockFrames = block;
    result.maxRealtimeHistoryFrames = 64;
    result.eventValues = {{"Gain", 0.5, talas::expr::ValueDomain::Scalar}};
    const auto score = prepareProjectScore(document, preparedScoreBindingForDocument(document, 1, 1, 1000, 1));
    require(score.ok(), score.error); result.preparedScore = score.schedule;
    return result;
}
std::string diagnostic(const CompileResult& result)
{
    std::string message = "WaitUntil compilation";
    for (const auto& item : result.diagnostics) message += " | " + item.message;
    return message;
}
struct Oracle
{
    std::array<double, sampleCount> audio{};
    std::array<std::uint64_t, sampleCount> cursorAfter{};
    std::array<std::array<double, sampleCount>, 3> inputs{};
};
// Active programs continue on the sample
// clock; gates hold the composition cursor/automation. Same-sample release is an
// explicit native ordering requirement, not an observed ordering claim.
Oracle oracle(bool resumeOnZero, bool simultaneous, bool immediate)
{
    Oracle result;
    for (std::size_t f = 0; f < sampleCount; ++f)
    {
        result.inputs[0][f] = (double(f) + 1) / 64;
        const auto release = immediate || f == 7 || (!simultaneous && f == 15);
        result.inputs[1][f] = resumeOnZero ? (release ? 0 : 1) : (release ? 1 : 0);
        result.inputs[2][f] = immediate || f == 9 ? 1 : 0;
    }
    std::uint64_t composition = 0;
    std::array<bool, 2> released{};
    bool otherReleased = false;
    for (std::size_t f = 0; f < sampleCount; ++f)
    {
        bool held = false;
        for (std::size_t gate = 0; gate < (simultaneous ? 1U : 2U); ++gate)
        {
            if (composition < 2 + gate * 4 || composition >= 6 + gate * 4) continue;
            released[gate] = released[gate] || (resumeOnZero ? result.inputs[1][f] == 0 : result.inputs[1][f] != 0);
            held = held || !released[gate];
        }
        if (simultaneous && composition >= 2 && composition < 6)
        {
            otherReleased = otherReleased || result.inputs[2][f] != 0;
            held = held || !otherReleased;
        }
        const auto gain = composition < 3 ? 0.5 : composition < 7 ? 0.25 : 0.75;
        result.audio[f] = composition < 20 ? gain * std::sin(2 * std::numbers::pi * 7 * f / 1000) : 0;
        if (!held) ++composition;
        result.cursorAfter[f] = composition;
    }
    return result;
}
void testParent(std::size_t block, bool resumeOnZero = false, bool simultaneous = false, bool immediate = false, bool offline = false, bool host = false)
{
    const auto document = fixture(resumeOnZero, simultaneous);
    auto configured = settings(document, block);
    const auto expected = oracle(resumeOnZero, simultaneous, immediate);
    for (std::size_t ch = 0; ch < 3; ++ch)
        configured.audioInputChannels.push_back({static_cast<std::uint32_t>(ch + 1),
            std::vector<double>(expected.inputs[ch].begin(), expected.inputs[ch].end())});
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    require(compiled.graph->renderSettings().preparedScoreClipAudio.empty(), "WaitUntil sources must remain live programs");
    if (offline)
    {
        const auto rendered = renderOffline(*compiled.graph, sampleCount);
        require(rendered.ok(), "offline Timeline hold must complete the requested sample interval");
        require(rendered.mono.size() == sampleCount, "offline sample count");
        for (std::size_t f = 0; f < sampleCount; ++f)
            require(std::abs(rendered.mono[f] - expected.audio[f]) < 1e-12,
                "offline Timeline automation must hold while synthesis advances at sample " + std::to_string(f));
        return;
    }
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt) session.resetTransport();
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min(block, sampleCount - offset);
            std::array<const double*, 3> inputs{};
            for (std::size_t ch = 0; ch < 3; ++ch) inputs[ch] = expected.inputs[ch].data() + offset;
            std::array<double, sampleCount> audio{}; double* output[] = {audio.data()};
            if (host) session.setTransportSnapshotBeforeRender({.absoluteFrame = offset,
                .playheadFrame = offset, .sampleRate = 1000, .playing = true});
            allocations = 0; countAllocations = true;
            const auto ok = session.render({nullptr, inputs.size(), inputs.data()}, output, 1, count);
            countAllocations = false;
            require(ok && allocations == 0, "Timeline hold/release callback must render without allocation");
            for (std::size_t f = 0; f < count; ++f)
                require(std::abs(audio[f] - expected.audio[offset + f]) < 1e-12,
                    "Timeline automation must hold while synthesis advances at sample " + std::to_string(offset + f));
            require(session.preparedScorePlaybackFrame() == expected.cursorAfter[offset + count - 1],
                "parent Score cursor must hold at WaitUntil; expected " + std::to_string(expected.cursorAfter[offset + count - 1])
                    + ", actual " + std::to_string(session.preparedScorePlaybackFrame().value_or(~std::uint64_t{})));
        }
    }
}

void testParentTimelineLoop()
{
    constexpr std::size_t frames = 96;
    for (const auto loopStart : {1U, 4U})
    for (const auto resumeOnZero : {false, true})
    for (const auto simultaneous : {false, true})
    {
        auto document = fixture(resumeOnZero, simultaneous);
        document.timeline.score.clips[1].repeatCount = 1;
        ProjectSoundDefinition failing; failing.definitionId = "failing"; failing.displayName = "Failure";
        failing.graph.nodes = {device("device", 1), {"gain", "talas.vca", {{"gain", "1e308"}}}};
        failing.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
        failing.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
        document.soundDefinitions.push_back(std::move(failing));
        auto failureClip = document.timeline.score.clips[0];
        failureClip.clipId = "failing"; failureClip.soundDefinitionId = "failing";
        document.timeline.score.clips.push_back(failureClip);
        const auto loopEnd = loopStart + 12;
        document.timeline.loop = {true, loopStart / 1000.0, loopEnd / 1000.0};
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "Timeline loop save/reopen");
        document = reopened.document;
        std::array<std::array<double, frames>, 3> inputs{};
        std::array<double, frames> expected{};
        std::array<std::uint64_t, frames> cursors{};
        std::uint64_t composition = 0, synthAge = 0;
        bool primaryReleased = false, otherReleased = false;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto release = frame % 11 == 7;
            inputs[1][frame] = resumeOnZero ? !release : release;
            inputs[2][frame] = frame % 13 == 9;
            if (composition == loopEnd)
            {
                composition = synthAge = loopStart;
                primaryReleased = otherReleased = false;
            }
            bool held = false;
            if (composition >= 2 && composition < 6)
            {
                primaryReleased |= release;
                otherReleased |= inputs[2][frame] != 0;
                held = !primaryReleased || (simultaneous && !otherReleased);
            }
            const auto gain = composition < 3 ? 0.5 : composition < 7 ? 0.25 : 0.75;
            expected[frame] = gain * std::sin(2 * std::numbers::pi * 7 * synthAge / 1000);
            ++synthAge;
            if (!held) ++composition;
            cursors[frame] = composition;
        }
        auto configured = settings(document, 32);
        configured.transportLoop = TransportLoopRegion{loopStart, loopEnd};
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
            configured.audioInputChannels.push_back({static_cast<std::uint32_t>(channel + 1),
                std::vector<double>(inputs[channel].begin(), inputs[channel].end())});
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), diagnostic(compiled));
        const auto offline = renderOffline(*compiled.graph, frames);
        require(offline.ok() && offline.mono.size() == frames,
            "Timeline loop with a parent WaitUntil must render the requested offline interval");
        for (std::size_t frame = 0; frame < frames; ++frame)
            require(std::abs(offline.mono[frame] - expected[frame]) < 1e-12,
                "offline Timeline loop must hold composition and restart source history at sample "
                    + std::to_string(frame));
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            for (int pass = 0; pass < 2; ++pass)
            {
                allocations = deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0,
                    "Timeline loop reset must not allocate or deallocate");
                for (std::size_t offset = 0; offset < frames; offset += block)
                {
                    const auto count = std::min<std::size_t>(block, frames - offset);
                    const double* channels[] = {inputs[0].data() + offset,
                        inputs[1].data() + offset, inputs[2].data() + offset};
                    std::array<double, 32> audio{}; double* outputs[] = {audio.data()};
                    allocations = deallocations = 0; countAllocations = true;
                    session.setTransportSnapshotBeforeRender({.absoluteFrame = offset,
                        .playheadFrame = offset < loopEnd ? offset : loopStart + (offset - loopEnd) % 12,
                        .sampleRate = 1000, .loopStartFrame = loopStart, .loopEndFrame = loopEnd,
                        .playing = true, .loopEnabled = true});
                    const auto rendered = session.render({nullptr, 3, channels}, outputs, 1, count);
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0,
                        "Timeline hold/loop callback must render without allocation or deallocation");
                    for (std::size_t frame = 0; frame < count; ++frame)
                        require(std::abs(audio[frame] - expected[offset + frame]) < 1e-12,
                            "Timeline loop sample mismatch at " + std::to_string(offset + frame));
                    require(session.preparedScorePlaybackFrame() == cursors[offset + count - 1],
                        "Timeline loop cursor must follow the held composition clock");
                }
            }
        }
        if (resumeOnZero || simultaneous) continue;
        RealtimeRenderSession failed(*compiled.graph), reference(*compiled.graph);
        require(failed.ready() && reference.ready(), "loop rollback sessions prepare");
        std::array<double, 32> fault{}, resume{}, retry{}, independent{};
        resume.fill(1); fault.back() = 10;
        const double* failureInputs[] = {fault.data(), resume.data(), resume.data()};
        double* retryOutputs[] = {retry.data()}; double* referenceOutputs[] = {independent.data()};
        allocations = deallocations = 0; countAllocations = true;
        const auto rejected = failed.render({nullptr, 3, failureInputs}, retryOutputs, 1, 32);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && std::ranges::all_of(retry, [](auto sample) { return sample == 0; })
                && failed.preparedScorePlaybackFrame() == 0,
            "late failure after multiple Timeline wraps must restore the cursor and silence output");
        fault.back() = 0;
        for (int chunk = 0; chunk < 2; ++chunk)
        {
            if (chunk) resume.fill(0);
            require(reference.render({nullptr, 3, failureInputs}, referenceOutputs, 1, 32),
                "independent Timeline loop rollback reference");
            allocations = deallocations = 0; countAllocations = true;
            const auto retried = failed.render({nullptr, 3, failureInputs}, retryOutputs, 1, 32);
            countAllocations = false;
            require(retried && allocations == 0 && deallocations == 0 && retry == independent
                    && failed.preparedScorePlaybackFrame() == reference.preparedScorePlaybackFrame(),
                "Timeline loop retry must restore gate latches, source clocks and every swapped owner");
        }
        configured.preparedScore.reset();
        ProjectRuntimeBridge bridge(document, registry());
        require(bridge.compileAndStage(configured).ok(), "stage public held Timeline loop");
        bridge.setPlaying(true);
        std::array<std::array<float, frames>, 3> deviceInputs{};
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
            std::copy(inputs[channel].begin(), inputs[channel].end(), deviceInputs[channel].begin());
        std::array<float, 32> bridgeAudio{}; float* bridgeOutputs[] = {bridgeAudio.data()};
        for (std::size_t offset = 0; offset < frames; offset += 32)
        {
            const float* channels[] = {deviceInputs[0].data() + offset,
                deviceInputs[1].data() + offset, deviceInputs[2].data() + offset};
            allocations = deallocations = 0; countAllocations = true;
            const auto rendered = bridge.render({channels, 3}, bridgeOutputs, 1, 32);
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0,
                "public held Timeline loop must render without callback heap work");
            for (std::size_t frame = 0; frame < bridgeAudio.size(); ++frame)
                require(std::abs(bridgeAudio[frame] - expected[offset + frame]) < 1e-6,
                    "public bridge must preserve the composition hold through loop wraps");
        }
        std::array<float, 32> silence{}, openGate{}; openGate.fill(1);
        const float* seekInputs[] = {silence.data(), openGate.data(), openGate.data()};
        for (const auto target : {loopEnd - 1, loopEnd + 5})
        {
            require(bridge.requestTransportRelocation(target), "prepare Timeline loop seek");
            auto playhead = target < loopEnd ? target : loopStart + (target - loopEnd) % 12;
            for (const auto count : {1U, 32U})
            {
                allocations = deallocations = 0; countAllocations = true;
                const auto rendered = bridge.render({seekInputs, 3}, bridgeOutputs, 1, count);
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0,
                    "Timeline loop seek and the next callback boundary must use prepared source owners");
                for (std::size_t frame = 0; frame < count; ++frame)
                {
                    if (playhead == loopEnd) playhead = loopStart;
                    const auto gain = playhead < 3 ? 0.5 : playhead < 7 ? 0.25 : 0.75;
                    require(std::abs(bridgeAudio[frame]
                            - gain * std::sin(2 * std::numbers::pi * 7 * playhead / 1000)) < 1e-6,
                        "Timeline loop seek must restore the wrapped composition position and source phase");
                    ++playhead;
                }
            }
        }
    }
}

void testParentLiveInputContinuesDuringWait()
{
    auto document = fixture();
    auto& background = document.soundDefinitions[0];
    background.graph.nodes = {device("liveInput", 1),
        {"gain", "talas.vca", {{"gain", "0.5"}}}};
    background.graph.connections = {{"liveInput", "audioOut", "gain", "audioIn"}};
    background.outputs[0].nodeId = "gain";
    background.exposedParameters.clear();
    document.timeline.score.automation.clear();
    auto configured = settings(document, 8);
    configured.preparedScore.reset();
    configured.audioInputChannels = {{1, std::vector<double>(sampleCount, 0.75)}};
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage live input behind a parent WaitUntil");
    bridge.setPlaying(true);

    const auto expectedClock = oracle(false, false, false);
    std::array<float, sampleCount> live{}, resume{};
    for (std::size_t frame = 0; frame < sampleCount; ++frame)
    {
        live[frame] = static_cast<float>(static_cast<int>(frame % 7) - 3) / 8;
        resume[frame] = static_cast<float>(expectedClock.inputs[1][frame]);
    }
    std::array<float, sampleCount> audio{};
    for (std::size_t offset = 0; offset < sampleCount; offset += 8)
    {
        const float* inputs[] = {live.data() + offset, resume.data() + offset};
        float* outputs[] = {audio.data() + offset};
        allocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 2}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0,
            "live input keeps rendering during a held parent Timeline without callback allocation");
    }
    require(expectedClock.cursorAfter[4] == 2 && live[4] != 0,
        "fixture supplies a distinct device sample while composition is held");
    for (std::size_t frame = 0; frame < sampleCount; ++frame)
    {
        const auto compositionBefore = frame == 0 ? 0 : expectedClock.cursorAfter[frame - 1];
        const auto expected = compositionBefore < 20 ? 0.5F * live[frame] : 0.0F;
        require(std::abs(audio[frame] - expected) < 1e-6F,
            "held Timeline processes the current device sample at frame " + std::to_string(frame));
    }
}

void testParentClockedControlContinuesDuringWait()
{
    auto document = fixture();
    auto& background = document.soundDefinitions[0];
    background.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "1"}}},
        {"ramp", "talas.ramp", {{"trigger", "1"}, {"duration", "0.012 s"}}},
        {"gain", "talas.vca", {{"gain", "1"}}}};
    background.graph.connections = {
        {"osc", "audioOut", "gain", "audioIn"},
        {"ramp", "value", "gain", "gain"}};
    background.outputs[0].nodeId = "gain";
    background.exposedParameters.clear();
    document.timeline.score.automation.clear();

    const auto expectedClock = oracle(false, false, false);
    auto configured = settings(document, 8);
    for (std::size_t channel = 0; channel < 2; ++channel)
        configured.audioInputChannels.push_back({channel + 1,
            {expectedClock.inputs[channel].begin(), expectedClock.inputs[channel].end()}});
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    const auto offline = renderOffline(*compiled.graph, sampleCount);
    require(offline.ok(), "clocked control behind parent WaitUntil renders offline");

    for (const auto block : {1U, 3U, 8U})
    {
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), std::string(session.error()));
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min<std::size_t>(block, sampleCount - offset);
            const double* inputs[] = {
                expectedClock.inputs[0].data() + offset,
                expectedClock.inputs[1].data() + offset};
            std::array<double, sampleCount> audio{};
            double* outputs[] = {audio.data()};
            allocations = 0;
            countAllocations = true;
            const auto rendered = session.render(
                {nullptr, 2, inputs}, outputs, 1, count);
            countAllocations = false;
            require(rendered && allocations == 0,
                "clocked control behind parent WaitUntil renders without callback allocation");
            for (std::size_t frame = 0; frame < count; ++frame)
            {
                const auto absoluteFrame = offset + frame;
                const auto expected = std::min(1.0, double(absoluteFrame) / 12.0)
                    * std::sin(2 * std::numbers::pi * 7 * absoluteFrame / 1000);
                require(std::abs(audio[frame] - expected) < 1e-12
                    && std::abs(offline.mono[absoluteFrame] - expected) < 1e-12,
                    "clocked ramp must advance while composition holds at sample "
                        + std::to_string(absoluteFrame));
            }
            require(session.preparedScorePlaybackFrame()
                    == expectedClock.cursorAfter[offset + count - 1],
                "clocked control must not advance parent composition cursor");
        }
    }
    require(expectedClock.cursorAfter[4] == 2,
        "clocked-control fixture must inspect a held Score frame");
}
void testParentTimeExpressionContinuesDuringWait(bool vca = false)
{
    auto document = fixture();
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.soundDefinitions[0].graph.nodes[0].parameters = {
        {"frequency", "7 hz"}, {"amplitude", "1 ramp: 12 ms"}};
    document.soundDefinitions[0].exposedParameters.clear();
    document.timeline.score.automation.clear();
    if (vca)
    {
        auto& sound = document.soundDefinitions[0];
        sound.graph.nodes[0].parameters[1].expression = "1";
        sound.graph.nodes.push_back({"gain", "talas.vca", {{"gain", "1 ramp: 12 ms"}}});
        sound.graph.connections.push_back({"osc", "audioOut", "gain", "audioIn"});
        sound.outputs[0].nodeId = "gain";
    }

    const auto expectedClock = oracle(false, false, false);
    auto configured = settings(document, 8);
    for (std::size_t channel = 0; channel < 2; ++channel)
        configured.audioInputChannels.push_back({channel + 1,
            {expectedClock.inputs[channel].begin(), expectedClock.inputs[channel].end()}});
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    const auto offline = renderOffline(*compiled.graph, sampleCount);
    require(offline.ok() && offline.mono.size() == sampleCount, "timed source parameter behind a parent hold renders offline");
    for (const auto block : {1U, 3U, 8U})
    {
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), std::string(session.error()));
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min<std::size_t>(block, sampleCount - offset);
            const double* inputs[] = {
                expectedClock.inputs[0].data() + offset,
                expectedClock.inputs[1].data() + offset};
            std::array<double, sampleCount> audio{};
            double* outputs[] = {audio.data()};
            allocations = 0;
            countAllocations = true;
            const auto rendered = session.render(
                {nullptr, 2, inputs}, outputs, 1, count);
            countAllocations = false;
            require(rendered && allocations == 0,
                "time expression behind parent WaitUntil renders without callback allocation");
            for (std::size_t frame = 0; frame < count; ++frame)
            {
                const auto absoluteFrame = offset + frame;
                const auto expected = std::min(1.0, double(absoluteFrame) / 12.0)
                    * std::sin(2 * std::numbers::pi * 7 * absoluteFrame / 1000);
            require(std::abs(audio[frame] - expected) < 1e-12
                    && std::abs(offline.mono[absoluteFrame] - expected) < 1e-12,
                "parameter ramp must advance while composition holds at sample "
                        + std::to_string(absoluteFrame));
            }
            require(session.preparedScorePlaybackFrame()
                    == expectedClock.cursorAfter[offset + count - 1],
                "time expression must not advance parent composition cursor");
        }
    }
    require(expectedClock.cursorAfter[4] == 2,
        "time-expression fixture must inspect a held Score frame");
}
void testRealtimeTimeExpressionMillisecondCadence()
{
    ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "500 hz"},
            {"amplitude", "1 ramp: 4 ms"}}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"}};
    RenderSettings configured;
    configured.sampleRate = 4000;
    configured.maxBlockFrames = 8;
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::size_t offset = 0;
    for (const auto count : {3U, 5U, 8U})
    {
        std::array<double, 8> audio{};
        double* outputs[] = {audio.data()};
        allocations = 0;
        countAllocations = true;
        const auto rendered = session.render({}, outputs, 1, count);
        countAllocations = false;
        require(rendered && allocations == 0,
            "millisecond parameter expression callback must not allocate");
        for (std::size_t frame = 0; frame < count; ++frame)
        {
            const auto absolute = offset + frame;
            const auto amplitude = std::min(1.0,
                double(absolute / 4) / 4.0);
            const auto expected = amplitude * std::sin(
                2 * std::numbers::pi * 500.0 * double(absolute) / 4000.0);
            require(std::abs(audio[frame] - expected) < 1e-12,
                "realtime parameter expression must change on millisecond ticks at sample "
                    + std::to_string(absolute));
        }
        offset += count;
    }
    session.resetTransport();
    std::array<double, 6> restarted{};
    double* restartOutput[] = {restarted.data()};
    require(session.render({}, restartOutput, 1, restarted.size()),
        "timed parameter expression must restart after transport reset");
    require(std::abs(restarted[1]) < 1e-12
        && std::abs(restarted[3]) < 1e-12
        && std::abs(restarted[5]
            - 0.25 * std::sin(2 * std::numbers::pi * 500.0 * 5 / 4000.0)) < 1e-12,
        "timed parameter expression must reset its millisecond clock");
}
// A timed oscillator Frequency follows its ramp in both renderers: frozen at
// its starting value of 0 hz it would play silence.
void testTimedOscillatorFrequencyFollowsTime()
{
    ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "500 hz * (1 ramp: 4 ms)"}, {"amplitude", "1"}}},
        {"gain", "talas.vca", {{"gain", "1"}}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {
        {"osc", "audioOut", "gain", "audioIn"},
        {"gain", "audioOut", "out", "audioIn"}};
    RenderSettings configured;
    configured.sampleRate = 4000;
    configured.maxBlockFrames = 8;
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    const auto offline = renderOffline(*compiled.graph, 32);
    require(offline.ok() && offline.mono.size() == 32,
        "timed oscillator frequency must render offline");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 32> audio{};
    for (std::size_t offset = 0; offset < audio.size(); offset += 8)
    {
        double* outputs[] = {audio.data() + offset};
        require(session.render({}, outputs, 1, 8),
            "timed oscillator frequency must render in realtime");
    }
    double loudest = 0.0;
    for (std::size_t frame = 0; frame < audio.size(); ++frame)
    {
        require(audio[frame] == offline.mono[frame],
            "timed oscillator frequency must play alike offline and in realtime at sample "
                + std::to_string(frame));
        loudest = std::max(loudest, std::abs(audio[frame]));
    }
    require(loudest > 0.5, "timed oscillator frequency must not freeze at its initial 0 hz");
}
// Input does not start before Resume. Capturing old
// input into audio history is observably different for a device or live controls.
void testLocalInput(bool oscillator, bool immediate = false, bool offline = false)
{
    auto document = fixture();
    auto& definition = document.soundDefinitions[1];
    definition.graph.nodes[0] = oscillator
        ? ProjectNode{"input", "talas.oscillator", {{"frequency", "11 hz"}, {"amplitude", "!Gain"}}}
        : device("input", 1);
    auto configured = settings(document);
    configured.maxRealtimeHistoryFrames = oscillator || immediate ? 64 : 4;
    const std::size_t release = immediate ? 0 : oscillator ? 5 : 17;
    if (offline)
    {
        configured.audioInputChannels = {{1, {}}, {2, {}}};
        for (std::size_t f = 0; f < sampleCount; ++f)
        {
            configured.audioInputChannels[0].samples.push_back((double(f) + 1) / 64);
            configured.audioInputChannels[1].samples.push_back(f >= release ? 1 : 0);
        }
    }
    const auto compiled = prepareScoreClipSourceIteration(document, registry(), configured, 1, 0);
    require(compiled.ok(), diagnostic(compiled));
    if (offline)
    {
        OfflineRenderSession session(*compiled.graph);
        for (const auto block : {1U, 8U})
        {
            session.reset();
            for (std::size_t offset = 0; offset < sampleCount; offset += block)
            {
                const auto count = std::min<std::size_t>(block, sampleCount - offset);
                const auto rendered = session.render(count);
                require(rendered.ok() && rendered.mono.size() == count, "offline live input long hold render");
                for (std::size_t f = 0; f < count; ++f)
                    require(rendered.mono[f] == (offset + f < release ? 0 : (double(offset + f) + 1) / 64),
                        "offline WaitUntil starts with current device sample at release across partitions/reset");
            }
        }
        return;
    }
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
    if (oscillator) require(session.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 3), "schedule pre-start live input gain");
    for (std::size_t f = 0; f < sampleCount; ++f)
    {
        const std::array values{(double(f) + 1) / 64, f >= release ? 1.0 : 0.0};
        const double* inputs[] = {&values[0], &values[1]}; double sample = 0; double* outputs[] = {&sample};
        allocations = 0; countAllocations = true;
        const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, 1);
        countAllocations = false;
        require(rendered && allocations == 0, "waiting for Input must not consume audio-history capacity");
        const auto expected = f < release ? 0 : oscillator
            ? (f < 3 ? 0.5 : 0.25) * std::sin(2 * std::numbers::pi * 11 * (f - release) / 1000)
            : values[0];
        require(std::abs(sample - expected) < 1e-12,
            "WaitUntil Input must start with current device/control state at sample " + std::to_string(f));
    }
}

void testLocalDependencyOwnership(bool nested, bool failure)
{
    auto document = fixture();
    auto& definition = document.soundDefinitions[1];
    definition.graph.nodes[0].parameters = {{"frequency", "11 hz"}, {"amplitude", "0.25"}};
    if (nested)
    {
        definition.graph.nodes.push_back(device("innerResumeInput", 3));
        definition.graph.nodes.push_back({"innerPeak", "talas.peakDetector", {{"attackTime", "1 ms"}, {"releaseTime", "1 ms"}}});
        definition.graph.nodes.push_back({"innerThreshold", "talas.threshold", {{"threshold", "0.5"}}});
        definition.graph.nodes.push_back({"innerWait", "talas.waitUntil", {}});
        definition.graph.connections[0] = {"innerWait", "audioOut", "wait", "audioIn"};
        definition.graph.connections.push_back({"input", "audioOut", "innerWait", "audioIn"});
        definition.graph.connections.push_back({"innerResumeInput", "audioOut", "innerPeak", "audioIn"});
        definition.graph.connections.push_back({"innerPeak", "value", "innerThreshold", "value"});
        definition.graph.connections.push_back({"innerThreshold", "value", "innerWait", "resume"});
    }
    else
    {
        definition.graph.nodes.push_back({"mix", "talas.mixer", {}});
        definition.graph.connections.push_back({"input", "audioOut", "mix", "in1"});
        definition.graph.connections.push_back({"wait", "audioOut", "mix", "in2"});
        definition.outputs[0].nodeId = "mix";
    }
    if (failure)
    {
        definition.graph.nodes.push_back(device("failureInput", 1));
        definition.graph.nodes.push_back({"overflow", "talas.vca", {{"gain", "1e308"}}});
        definition.graph.nodes.push_back({"finalMix", "talas.mixer", {}});
        definition.graph.connections.push_back({"failureInput", "audioOut", "overflow", "audioIn"});
        definition.graph.connections.push_back({"overflow", "audioOut", "finalMix", "in2"});
        definition.graph.connections.push_back({definition.outputs[0].nodeId, "audioOut", "finalMix", "in1"});
        definition.outputs[0].nodeId = "finalMix";
    }
    auto configured = settings(document, 24);
    auto compiled = prepareScoreClipSourceIteration(document, registry(), configured, 1, 0);
    require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
    std::array<std::array<double, sampleCount>, 3> data{};
    for (std::size_t f = 0; f < sampleCount; ++f)
    {
        data[1][f] = f >= 5 ? 1 : 0;
        // The earlier pulse must not latch an inner gate whose Input scope has
        // not yet started. Its next pulse releases it at sample 9.
        data[2][f] = f == 3 || f >= 9 ? 1 : 0;
    }
    std::array<const double*, 3> inputs{data[0].data(), data[1].data(), data[2].data()};
    std::array<double, sampleCount> audio{}; double* outputs[] = {audio.data()};
    if (failure)
    {
        data[0].back() = 10; audio.fill(19);
        allocations = 0; countAllocations = true;
        const auto rendered = session.render({nullptr, 3, inputs.data()}, outputs, 1, sampleCount);
        countAllocations = false;
        require(!rendered && allocations == 0 && std::ranges::all_of(audio, [](auto value) { return value == 0; }),
            "late failure after gate release aborts all caller audio without allocation");
        data[0].back() = 0;
    }
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt) session.resetTransport();
        allocations = 0; countAllocations = true;
        const auto rendered = session.render({nullptr, 3, inputs.data()}, outputs, 1, sampleCount);
        countAllocations = false;
        require(rendered && allocations == 0, "shared/nested input callback and retry without allocation");
        for (std::size_t f = 0; f < sampleCount; ++f)
        {
            const auto expected = nested
                ? (f < 9 ? 0 : 0.25 * std::sin(2 * std::numbers::pi * 11 * (f - 9) / 1000))
                : (f < 5 ? 0.25 : 0.5) * std::sin(2 * std::numbers::pi * 11 * f / 1000);
            require(std::abs(audio[f] - expected) < 1e-12,
                "shared producer runs once; nested input begins only after its own live release at sample " + std::to_string(f));
        }
    }
}
void testLocalFeedback(bool gated)
{
    auto definition = gateDefinition("gate", 2);
    ProjectParameterExpression connectionName; connectionName.parameterId = "connection"; connectionName.text = "loop";
    definition.graph.nodes[0] = device("input", 1);
    definition.graph.nodes.push_back({"feedback", "talas.feedbackLoopOutput", {connectionName, {"delay", "0 s"}}});
    definition.graph.nodes.push_back({"sink", "talas.feedbackLoopInput", {connectionName}});
    definition.graph.nodes.push_back({"feedbackMix", "talas.mixer", {{"gain1", "1"}, {"gain2", "0.5"}}});
    definition.graph.connections.push_back({"input", "audioOut", "feedbackMix", "in1"});
    definition.graph.connections.push_back({"feedback", "audioOut", "feedbackMix", "in2"});
    definition.graph.connections.push_back({"feedbackMix", "audioOut", "sink", "audioIn"});
    if (gated) definition.graph.connections[0] = {"feedbackMix", "audioOut", "wait", "audioIn"};
    else
    {
        // Silence the independent Wait input. The feedback branch must continue
        // while its sibling is held, and its deferred write must occur once.
        definition.graph.nodes.push_back({"silent", "talas.oscillator", {{"amplitude", "0"}}});
        definition.graph.connections[0] = {"silent", "audioOut", "wait", "audioIn"};
        definition.graph.nodes.push_back({"mix", "talas.mixer", {}});
        definition.graph.connections.push_back({"feedbackMix", "audioOut", "mix", "in1"});
        definition.graph.connections.push_back({"wait", "audioOut", "mix", "in2"});
        definition.outputs[0].nodeId = "mix";
    }
    ProjectDocument document; document.graph = definition.graph;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({definition.outputs[0].nodeId, "audioOut", "out", "audioIn"});
    RenderSettings configured; configured.sampleRate = 1000; configured.maxBlockFrames = sampleCount;
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph);
    if (!session.ready())
        for (const auto& step : compiled.graph->plan().steps)
        {
            const auto value = [](const auto& v) { return v ? std::to_string(*v) : "unknown"; };
            std::cerr << step.nodeId << " " << step.typeId << " resolved=" << value(step.resolvedLatencyFrames)
                << " in=" << value(step.cumulativeInputLatencyFrames) << " out=" << value(step.cumulativeOutputLatencyFrames) << '\n';
        }
    require(session.ready(), std::string(session.error()));
    std::array<std::array<double, sampleCount>, 2> data{};
    data[0][0] = 1; data[0][5] = 1;
    for (std::size_t f = 5; f < sampleCount; ++f) data[1][f] = 1;
    for (const auto block : {1U, 8U})
    {
        session.resetTransport(); double previous = 0;
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min<std::size_t>(block, sampleCount - offset);
            const double* inputs[] = {data[0].data() + offset, data[1].data() + offset};
            std::array<double, sampleCount> audio{}; double* outputs[] = {audio.data()};
            allocations = 0; countAllocations = true;
            const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, count);
            countAllocations = false;
            require(rendered && allocations == 0, "WaitUntil feedback callback must render without allocation");
            for (std::size_t f = 0; f < count; ++f)
            {
                const auto expected = gated && offset + f < 5 ? 0 : data[0][offset + f] + previous * 0.5;
                require(std::abs(audio[f] - expected) < 1e-12, "feedback pair must follow Input activation at sample " + std::to_string(offset + f));
                previous = expected;
            }
        }
    }
}
void testOfflineNativeCompatibility(bool disk)
{
    ProjectDocument document;
    document.graph.nodes = {
        {"input", "talas.oscillator", {{"frequency", "11 hz"}, {"amplitude", "@gain"}}},
        {"resume", "talas.constant", {{"value", "!Resume"}}},
        {"wait", "talas.waitUntil", {}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"input", "audioOut", "wait", "audioIn"},
        {"resume", "value", "wait", "resume"}, {"wait", "audioOut", "out", "audioIn"}};
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables.push_back({"gain", "Gain", talas::expr::ValueDomain::Scalar, 0.5});
    RenderSettings configured; configured.sampleRate = 1000; configured.maxBlockFrames = 8;
    configured.maxRealtimeHistoryFrames = 0;
    configured.eventValues = {{"Resume", 0, talas::expr::ValueDomain::Scalar}};
    configured.eventValueChanges = {{5, "Resume", 1, talas::expr::ValueDomain::Scalar}};
    const auto path = std::filesystem::temp_directory_path() / ("talas-wait-offline-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{disk ? path : std::filesystem::path{}};
    if (disk)
    {
        ProjectParameterExpression resource; resource.parameterId = "fileName"; resource.resource = "source";
        document.graph.nodes[0] = {"input", "talas.diskPlayer", {resource, {"duration", "1 s"}}};
        std::string error; std::vector<std::vector<double>> audio{std::vector<double>(64)};
        for (std::size_t f = 0; f < audio[0].size(); ++f) audio[0][f] = -(double(f) + 1) / 128;
        require(talas::audio::writePcm16Wav(path, 1000, audio, error), error);
        const auto hash = calculateProjectAssetSha256(path); require(hash.ok(), "hash WaitUntil WAV");
        ProjectAsset asset; asset.id = "source"; asset.kind = ProjectAssetKind::Audio;
        asset.path = path.string(); asset.contentHash = hash.contentHash; asset.channelCount = 1;
        asset.sampleRate = 1000; asset.frameCount = 64; asset.encoding = pcm16WavAssetEncoding; asset.generation = 1;
        document.assets.push_back(std::move(asset));
        auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(document, registry(), std::nullopt);
        require(loaded.ok(), "load WaitUntil WAV"); configured.resourceSnapshots = std::move(loaded.snapshots);
    }
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    require(validateOfflineRuntimeGraph(*compiled.graph).ok(), "native offline WaitUntil preflight retains existing support");
    OfflineRenderSession session(*compiled.graph);
    if (!disk)
    {
        const std::array assignments{OfflineVariableAssignment{3, 1, 1,
            compiled.graph->preparedVariables().cells.front().identity, 0.25, talas::expr::ValueDomain::Scalar}};
        require(session.scheduleVariableAssignments(assignments), "WaitUntil must retain native offline variable assignment support");
    }
    const auto rendered = session.render(sampleCount);
    require(rendered.ok() && rendered.mono.size() == sampleCount, "native offline WaitUntil compatibility render");
    for (std::size_t f = 0; f < sampleCount; ++f)
    {
        // The WAV writer stores round(x * 32767) and readers divide by 32767.
        const auto expected = f < 5 ? 0 : disk ? std::round(-(double(f - 5) + 1) / 128 * 32767) / 32767
            : 0.25 * std::sin(2 * std::numbers::pi * 11 * (f - 5) / 1000);
        require(std::abs(rendered.mono[f] - expected) < 1e-12, "offline Input must start with preserved native resource/variable state at " + std::to_string(f) + " expected=" + std::to_string(expected) + " actual=" + std::to_string(rendered.mono[f]));
    }
}

void testParentRollback()
{
    auto document = fixture();
    ProjectSoundDefinition failing; failing.definitionId = "failing"; failing.displayName = "Failure";
    failing.graph.nodes = {device("device", 3), {"gain", "talas.vca", {{"gain", "1e308"}}}};
    failing.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
    failing.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
    document.soundDefinitions.push_back(std::move(failing));
    auto clip = document.timeline.score.clips[0]; clip.clipId = "failing"; clip.soundDefinitionId = "failing";
    document.timeline.score.clips.push_back(clip);
    auto expected = oracle(false, false, false); expected.inputs[2].fill(0);
    auto configured = settings(document, sampleCount);
    const auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
    std::array<const double*, 3> inputs{expected.inputs[0].data(), expected.inputs[1].data(), expected.inputs[2].data()};
    std::array<double, sampleCount> audio{}; double* outputs[] = {audio.data()};
    expected.inputs[2].back() = 10; audio.fill(19);
    allocations = 0; countAllocations = true;
    const auto failed = session.render({nullptr, 3, inputs.data()}, outputs, 1, sampleCount);
    countAllocations = false;
    require(!failed && allocations == 0 && std::ranges::all_of(audio, [](double v) { return v == 0; }),
        "late parent failure must discard the complete held/released transaction");
    require(session.preparedScorePlaybackFrame() == 0, "late failure must restore parent composition cursor");
    expected.inputs[2].back() = 0;
    allocations = 0; countAllocations = true;
    const auto retried = session.render({nullptr, 3, inputs.data()}, outputs, 1, sampleCount);
    countAllocations = false;
    require(retried && allocations == 0, "parent hold retry must render without allocation");
    for (std::size_t f = 0; f < sampleCount; ++f)
        require(std::abs(audio[f] - expected.audio[f]) < 1e-12,
            "parent failure restores source phase, gate latch and automation at " + std::to_string(f));
    require(session.preparedScorePlaybackFrame() == expected.cursorAfter.back(), "parent retry composition position");
}

void testParentMidiFile()
{
    auto document = fixture(); auto& sound = document.soundDefinitions[0];
    ProjectParameterExpression source, resource, script, shared;
    source.parameterId = "source"; source.text = "MIDI File";
    resource.parameterId = "midiFile"; resource.resource = "notes";
    script.parameterId = "script"; script.text = "";
    shared.parameterId = "shared"; shared.boolean = false;
    sound.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "!Pitch"}, {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
        {"voice", "talas.midiVoice", {source, resource, script, shared, {"channel", "1"},
            {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, {"polyphony", "1"}, {"left", "1"}, {"right", "0.5"}}}};
    sound.graph.connections = {{"osc", "audioOut", "voice", "audioIn"}};
    sound.outputs[0].nodeId = "voice"; sound.outputs[0].nodePortId = "leftOut";
    sound.exposedParameters.clear(); document.timeline.score.automation.clear();
    const auto expected = oracle(false, false, false);
    auto configured = settings(document, 8);
    configured.eventValues.insert(configured.eventValues.end(), {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}});
    configured.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
        'M','T','r','k',0,0,0,12, 0,0x90,60,100, 3,0x80,60,0, 0,0xff,0x2f,0}}};
    for (std::size_t ch = 0; ch < 2; ++ch)
        configured.audioInputChannels.push_back({ch + 1, {expected.inputs[ch].begin(), expected.inputs[ch].end()}});
    const auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), diagnostic(compiled));
    const auto offline = renderOffline(*compiled.graph, sampleCount); require(offline.ok(), "parent MIDI File offline render");
    for (const auto block : {1U, 8U})
    {
        RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
        double phase = 0;
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min<std::size_t>(block, sampleCount - offset);
            const double* inputs[] = {expected.inputs[0].data() + offset, expected.inputs[1].data() + offset};
            std::array<double, sampleCount> audio{}; double* outputs[] = {audio.data()};
            allocations = 0; countAllocations = true;
            const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, count);
            countAllocations = false;
            require(rendered && allocations == 0, "parent MIDI File hold has no callback allocations");
            for (std::size_t f = 0; f < count; ++f)
            {
                // The file note-off at composition 3 waits until sample 8.
                // Its five-sample attack completes at sample 5 during the hold.
                const auto sample = offset + f;
                const auto value = sample >= 5 && sample < 8 ? 100.0 / 127 * std::sin(2 * std::numbers::pi * phase) : 0;
                require(std::abs(audio[f] - value) < 1e-11 && std::abs(offline.mono[sample] - value) < 1e-11,
                    "parent MIDI File reader holds while attack/oscillator continue at " + std::to_string(sample));
                phase += 440 * std::pow(2.0, (60 - 69) / 12.0) / 1000; phase -= std::floor(phase);
            }
        }
    }
}
void testParentLoopScheduledVoiceSources()
{
    constexpr std::size_t frames = 96;
    for (const auto sourceKind : {0U, 1U, 2U, 3U, 4U})
    for (const auto loopStart : {1U, 4U, 8U})
    {
        const auto scripted = sourceKind == 2 || sourceKind == 3;
        const auto mapper = sourceKind == 1 || sourceKind == 3;
        const auto analog = sourceKind == 4;
        auto document = fixture();
        document.timeline.score.clips[1].repeatCount = 1;
        const auto loopEnd = loopStart + 12;
        document.timeline.loop = {true, loopStart / 1000.0, loopEnd / 1000.0};
        auto& sound = document.soundDefinitions[0];
        ProjectParameterExpression source, file, program, model;
        source.parameterId = "source"; source.text = scripted ? "Script" : "MIDI File";
        file.parameterId = "midiFile"; file.resource = scripted ? "" : "notes";
        program.parameterId = "script";
        program.text = scripted ? "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn. "
            "self controller: !cc18 setTo: 1 atTime: 3 ms." : "";
        model.parameterId = "voiceModel"; model.choice = "polyphonicInput";
        sound.graph.nodes = {
            {"osc", "talas.oscillator", {{"frequency", "100 hz + !cc18 * 100 hz"},
                {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
            {"voice", mapper ? "talas.midiMapper" : "talas.midiVoice", {source, file, program,
                boolean("shared", false), {"channel", "1"}, {"polyphony", "1"},
                {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, {"left", "1"}, {"right", "0.5"}}}};
        if (mapper) sound.graph.nodes[1].parameters.push_back(model);
        if (analog)
        {
            const auto text = [](std::string id, std::string value) {
                ProjectParameterExpression result;
                result.parameterId = std::move(id); result.text = std::move(value); return result;
            };
            // A DutyCycle of 1 retriggers here, as before format 76.
            ProjectParameterExpression retriggers;
            retriggers.parameterId = "dutyCycleOne"; retriggers.choice = "legacyRetriggers";
            sound.graph.nodes[0].parameters[0].expression = "100 hz + !KeyTimbre * 100 hz";
            sound.graph.nodes[1].typeId = "talas.analogSequencer";
            sound.graph.nodes[1].parameters = {{"left", "1"}, {"right", "0.5"},
                {"gate", "1"}, {"step", "1"}, {"rate", "1"}, {"startIndex", "0"},
                {"endIndex", "1"}, {"polyphony", "1"}, boolean("loop", false),
                text("durations", "#(0.01 0.01)"), text("dutyCycles", "#(1 1)"),
                text("keyDowns", "#(1 1)"), text("keyPitches", "#(60 64)"),
                text("keyVelocities", "#(0.25 0.5)"), text("keyTimbres", "#(0 1)"), retriggers};
        }
        sound.graph.connections = {{"osc", "audioOut", "voice", "audioIn"}};
        sound.outputs[0].nodeId = "voice"; sound.outputs[0].nodePortId = "leftOut";
        sound.exposedParameters.clear(); document.timeline.score.automation.clear();
        ProjectSoundDefinition failing; failing.definitionId = "failing"; failing.displayName = "Failure";
        failing.graph.nodes = {device("device", 1), {"gain", "talas.vca", {{"gain", "1e308"}}}};
        failing.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
        failing.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
        document.soundDefinitions.push_back(std::move(failing));
        auto failureClip = document.timeline.score.clips[0];
        failureClip.clipId = "failing"; failureClip.soundDefinitionId = "failing";
        document.timeline.score.clips.push_back(failureClip);
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "scheduled voice Timeline hold/loop project persists");
        document = reopened.document;
        auto configured = settings(document, 32);
        configured.transportLoop = TransportLoopRegion{loopStart, loopEnd};
        configured.eventValues.insert(configured.eventValues.end(), {{"cc18", 0}, {"KeyTimbre", 0},
            {"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}});
        configured.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
            'M','T','r','k',0,0,0,16, 0,0x90,60,127, 3,0xb0,18,127,
            7,0x80,60,0, 0,0xff,0x2f,0}}};
        std::array<double, frames> silence{}, resume{}, expected{};
        std::array<std::uint64_t, frames> cursors{};
        std::uint64_t composition = 0, sourceSample = 0;
        double phase = 0;
        bool released = false;
        struct AnalogOracle
        {
            std::size_t sampleFrame = 0, attackAt = 5;
            bool secondStage = false, active = false;
            double phase = 0, velocity = 0, timbre = 0;
            double sample(std::uint64_t compositionFrame)
            {
                if (compositionFrame >= 10 && !secondStage)
                {
                    secondStage = true; active = false; attackAt = sampleFrame + 5;
                }
                if (sampleFrame == attackAt)
                {
                    active = true; velocity = secondStage ? 0.5 : 0.25;
                    timbre = secondStage ? 1 : 0;
                }
                if (compositionFrame >= 20) active = false;
                const auto value = active ? velocity * std::sin(2 * std::numbers::pi * phase) : 0;
                phase += 0.1 + 0.1 * timbre; phase -= std::floor(phase); ++sampleFrame;
                return value;
            }
        } analogState;
        const auto historyPhase = [](std::size_t age) {
            double value = 0;
            for (std::size_t frame = 0; frame < age; ++frame)
            { value += frame < 3 ? 0.1 : 0.2; value -= std::floor(value); }
            return value;
        };
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            resume[frame] = frame % 11 == 7;
            if (composition == loopEnd)
            {
                composition = sourceSample = loopStart;
                phase = historyPhase(loopStart); released = false;
                analogState = {};
                if (analog)
                    for (std::size_t history = 0; history < loopStart; ++history)
                        (void)analogState.sample(history);
            }
            // Note/control deadlines use composition time. The five-sample
            // delayed attack and oscillator use the continuing source sample clock.
            expected[frame] = analog ? analogState.sample(composition)
                : sourceSample >= 5 && composition < 10 ? std::sin(2 * std::numbers::pi * phase) : 0;
            phase += composition < 3 ? 0.1 : 0.2; phase -= std::floor(phase);
            ++sourceSample;
            if (composition >= 2 && composition < 6) released |= resume[frame] != 0;
            if (composition < 2 || composition >= 6 || released) ++composition;
            cursors[frame] = composition;
        }
        configured.audioInputChannels = {{1, {silence.begin(), silence.end()}},
            {2, {resume.begin(), resume.end()}}};
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), diagnostic(compiled));
        const auto offline = renderOffline(*compiled.graph, frames);
        require(offline.ok() && offline.mono.size() == frames, "scheduled voice Timeline hold/loop offline interval");
        for (std::size_t frame = 0; frame < frames; ++frame)
            require(std::abs(offline.mono[frame] - expected[frame]) < 1e-11,
                "MIDI hold/loop independent sample mismatch at " + std::to_string(frame)
                    + ", loop=" + std::to_string(loopStart) + ", script=" + std::to_string(scripted)
                    + ", mapper=" + std::to_string(mapper) + ", analog=" + std::to_string(analog));
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            for (int pass = 0; pass < 2; ++pass)
            {
                allocations = deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, "MIDI hold/loop reset heap work");
                for (std::size_t offset = 0; offset < frames; offset += block)
                {
                    const auto count = std::min<std::size_t>(block, frames - offset);
                    const double* inputs[] = {silence.data(), resume.data() + offset};
                    std::array<double, 32> audio{}; double* outputs[] = {audio.data()};
                    allocations = deallocations = 0; countAllocations = true;
                    const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, count);
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0,
                        "MIDI hold/loop callbacks must not allocate or deallocate");
                    for (std::size_t frame = 0; frame < count; ++frame)
                        require(std::abs(audio[frame] - expected[offset + frame]) < 1e-11,
                            "MIDI hold/loop realtime sample mismatch");
                    require(session.preparedScorePlaybackFrame() == cursors[offset + count - 1],
                        "MIDI note/controller clocks must not advance the held Timeline");
                }
            }
        }
        RealtimeRenderSession failed(*compiled.graph), reference(*compiled.graph);
        require(failed.ready() && reference.ready(), "MIDI hold/loop rollback prepares");
        std::array<double, 32> fault{}, openGate{}, actual{}, independent{};
        openGate.fill(1); fault.back() = 10;
        const double* inputs[] = {fault.data(), openGate.data()};
        double* outputs[] = {actual.data()}; double* referenceOutputs[] = {independent.data()};
        allocations = deallocations = 0; countAllocations = true;
        const auto rejected = failed.render({nullptr, 2, inputs}, outputs, 1, 32);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && failed.preparedScorePlaybackFrame() == 0
                && std::ranges::all_of(actual, [](auto value) { return value == 0; }),
            "late MIDI failure after loop wraps must leave no partial output or cursor");
        fault.back() = 0;
        for (int chunk = 0; chunk < 2; ++chunk)
        {
            if (chunk) openGate.fill(0);
            require(reference.render({nullptr, 2, inputs}, referenceOutputs, 1, 32), "MIDI rollback reference renders");
            allocations = deallocations = 0; countAllocations = true;
            const auto retried = failed.render({nullptr, 2, inputs}, outputs, 1, 32);
            countAllocations = false;
            require(retried && allocations == 0 && deallocations == 0 && actual == independent
                    && failed.preparedScorePlaybackFrame() == reference.preparedScorePlaybackFrame(),
                "MIDI loop failure retry must retain exact notes, controllers, phase and gate latches");
        }
    }
}

void testLiveVcaGainExpression()
{
    for (const auto connected : {false, true})
    for (const auto sampleRate : {1000U, 4000U})
    {
        ProjectDocument document;
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
        document.graph.nodes = {device("device", 1),
            {"vca", "talas.vca", {{"gain", "!Gain * (1 ramp: 12 ms)"}}},
            {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"device", "audioOut", "vca", "audioIn"},
            {"vca", "audioOut", "out", "audioIn"}};
        if (connected)
        {
            document.graph.nodes.push_back({"override", "talas.constant", {{"value", "0.75"}}});
            document.graph.connections.push_back({"override", "value", "vca", "gain"});
        }
        RenderSettings configured; configured.sampleRate = sampleRate; configured.maxBlockFrames = 24;
        configured.eventValues = {{"Gain", 0.25}};
        configured.eventValueChanges = {{3, "Gain", 0.5}, {9, "Gain", -0.25}};
        std::array<double, 24> deviceSamples{}, expected{};
        for (std::size_t frame = 0; frame < deviceSamples.size(); ++frame)
        {
            deviceSamples[frame] = (double(frame) + 1) / 32;
            const auto gain = connected ? 0.75 : (frame < 3 ? 0.25 : frame < 9 ? 0.5 : -0.25)
                * std::min(1.0, double(frame / (sampleRate / 1000)) / 12);
            expected[frame] = deviceSamples[frame] * gain;
        }
        configured.audioInputChannels = {{1, {deviceSamples.begin(), deviceSamples.end()}}};
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), diagnostic(compiled));
        const auto offline = renderOffline(*compiled.graph, deviceSamples.size());
        require(offline.ok() && offline.mono.size() == deviceSamples.size(), "live VCA offline render");
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
            require(std::abs(offline.mono[frame] - expected[frame]) < 1e-12,
                "VCA expression follows timed state and EventValues unless an input overrides it");
        for (const auto block : {1U, 7U, 24U})
        {
            RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            for (int pass = 0; pass < 2; ++pass)
            {
                allocations = deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, "live VCA reset must not touch the heap");
                for (std::size_t offset = 0; offset < expected.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, expected.size() - offset);
                    const double* inputs[] = {deviceSamples.data() + offset};
                    std::array<double, 24> audio{}; double* outputs[] = {audio.data()};
                    allocations = deallocations = 0; countAllocations = true;
                    const auto rendered = session.render({nullptr, 1, inputs}, outputs, 1, count);
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0, "live VCA callback heap work");
                    for (std::size_t frame = 0; frame < count; ++frame)
                        require(std::abs(audio[frame] - expected[offset + frame]) < 1e-12,
                            "VCA gain changes must be independent of callback partitions and reset");
                }
            }
        }
    }
}

void testParentLoopNestedSources(unsigned childKind, bool overlappingParents = false)
{
    constexpr std::size_t frames = 96;
    const auto scheduledChild = childKind != 0;
    const auto scriptChild = childKind == 2 || childKind == 4;
    const auto mapperChild = childKind == 3 || childKind == 4;
    const auto enclosingController = scheduledChild ? (overlappingParents ? 0.25 : 0.125) : 0;
    for (const auto parentKind : {0U, 1U, 2U}) // File, literal Script, Analog.
    for (const auto loopStart : {1U, 4U, 8U})
    {
        auto document = fixture();
        document.timeline.score.clips[1].repeatCount = 1;
        const auto loopLength = overlappingParents ? 24U : 12U;
        const auto loopEnd = loopStart + loopLength;
        if (overlappingParents) document.timeline.score.clips[0].duration = seconds(0.04);
        document.timeline.loop = {true, loopStart / 1000.0, loopEnd / 1000.0};
        auto& sound = document.soundDefinitions[0];
        const auto text = [](std::string id, std::string value) {
            ProjectParameterExpression result;
            result.parameterId = std::move(id); result.text = std::move(value); return result;
        };
        // A DutyCycle of 1 retriggers here, as before format 76.
        const auto retriggers = [] {
            ProjectParameterExpression result;
            result.parameterId = "dutyCycleOne"; result.choice = "legacyRetriggers"; return result;
        };
        const auto analogParameters = [&](bool child) {
            return std::vector<ProjectParameterExpression>{{"left", "1"}, {"right", "0.5"},
                {"gate", "1"}, {"step", "1"}, {"rate", "1"}, {"startIndex", "0"},
                {"endIndex", child ? "1" : "0"}, {"polyphony", "1"}, boolean("loop", false),
                text("durations", child ? "#(0.01 0.01)" : "#(0.01)"),
                text("dutyCycles", "#(1)"), text("keyDowns", "#(1)"),
                text("keyPitches", "#(60)"), text("keyVelocities", child ? "#(0.25 0.5)" : "#(1)"),
                text("keyTimbres", "#(0)"), retriggers()};
        };
        ProjectParameterExpression source, file, program;
        source.parameterId = "source"; source.text = parentKind == 1 ? "Script" : "MIDI File";
        file.parameterId = "midiFile"; file.resource = parentKind == 1 ? "" : "notes";
        program.parameterId = "script";
        program.text = parentKind == 1 ? "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn." : "";
        std::vector<ProjectParameterExpression> parent{source, file, program, boolean("shared", false),
            {"channel", "1"}, {"polyphony", "1"}, {"lowPitch", "0 nn"},
            {"highPitch", "127 nn"}, {"left", "1"}, {"right", "0.5"}};
        if (parentKind == 2) parent = analogParameters(false);
        if (overlappingParents)
            for (auto& parameter : parent)
            {
                if (parameter.parameterId == "polyphony") parameter.expression = "2";
                if (parameter.parameterId == "script" && parentKind == 1)
                    parameter.text = "self keyDownAt: 0 ms duration: 12 ms frequency: 60 nn. "
                        "self keyDownAt: 8 ms duration: 12 ms frequency: 64 nn. "
                        "self keyDownAt: 16 ms duration: 12 ms frequency: 67 nn.";
                if (parameter.parameterId == "endIndex") parameter.expression = "2";
                if (parameter.parameterId == "durations") parameter.text = "#(0.008 0.008 0.012)";
                if (parameter.parameterId == "keyPitches") parameter.text = "#(60 64 67)";
            }
        sound.graph.nodes = {device("device", 1),
            {"gain", "talas.vca", {{"gain", "(!KeyDown > 0) * !KeyVelocity"}}},
            {"inner", "talas.analogSequencer", analogParameters(true)},
            {"outer", parentKind == 2 ? "talas.analogSequencer" : "talas.midiVoice", parent}};
        sound.graph.connections = {{"device", "audioOut", "gain", "audioIn"},
            {"gain", "audioOut", "inner", "audioIn"}, {"inner", "leftOut", "outer", "audioIn"}};
        sound.outputs = {{"audioOut", talas::core::PortType::Audio, "outer", "leftOut"}};
        if (scheduledChild)
        {
            auto& child = sound.graph.nodes[2];
            child.typeId = mapperChild ? "talas.midiMapper" : "talas.midiVoice";
            ProjectParameterExpression childFile, model;
            childFile.parameterId = "midiFile"; childFile.resource = scriptChild ? "" : "child-notes";
            model.parameterId = "voiceModel"; model.choice = "polyphonicInput";
            child.parameters = {text("source", scriptChild ? "Script" : "MIDI File"), childFile,
                text("script", scriptChild
                    ? "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn velocity: 0.25. "
                      "self keyDownAt: 10 ms duration: 10 ms frequency: 64 nn velocity: 0.5. "
                      "self controller: !cc18 setTo: 0.25 atTime: 3 ms. "
                      "self controller: !cc18 setTo: 0.75 atTime: 13 ms." : ""),
                boolean("shared", false), {"channel", "1"}, {"polyphony", "1"},
                {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, {"left", "1"}, {"right", "0.5"}};
            if (mapperChild)
            {
                child.parameters.push_back(model);
                child.parameters.push_back(text("map", ""));
            }
            if (overlappingParents && scriptChild)
                *child.parameters[2].text += " self controller: !cc19 setTo: 0.75 atTime: 7 ms. "
                    "self controller: !cc19 setTo: 0.5 atTime: 13 ms.";
            sound.graph.nodes[1].parameters[0].expression = overlappingParents
                ? "(!KeyDown > 0) * (!KeyVelocity + !cc18 + !cc19)"
                : "(!KeyDown > 0) * (!KeyVelocity + !cc18)";
            sound.graph.nodes.push_back({"globalController", "talas.valToSound",
                {{"value", overlappingParents ? "!cc18 + !cc19" : "!cc18"}}});
            sound.graph.nodes.push_back({"mix", "talas.mixer", {}});
            sound.graph.connections.insert(sound.graph.connections.end(), {
                {"outer", "leftOut", "mix", "in1"}, {"globalController", "audioOut", "mix", "in2"}});
            sound.outputs = {{"audioOut", talas::core::PortType::Audio, "mix", "audioOut"}};
        }
        sound.exposedParameters.clear(); document.timeline.score.automation.clear();
        ProjectSoundDefinition failing; failing.definitionId = "failing"; failing.displayName = "Failure";
        failing.graph.nodes = {device("fault", 3), {"scale", "talas.vca", {{"gain", "1e308"}}}};
        failing.graph.connections = {{"fault", "audioOut", "scale", "audioIn"}};
        failing.outputs = {{"audioOut", talas::core::PortType::Audio, "scale", "audioOut"}};
        document.soundDefinitions.push_back(std::move(failing));
        auto failureClip = document.timeline.score.clips[0];
        failureClip.clipId = "failing"; failureClip.soundDefinitionId = "failing";
        document.timeline.score.clips.push_back(failureClip);
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "nested scheduled voice Timeline loop persists");
        document = reopened.document;
        auto configured = settings(document, 32);
        configured.transportLoop = TransportLoopRegion{loopStart, loopEnd};
        configured.eventValues.insert(configured.eventValues.end(),
            {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}, {"cc18", 0.125}});
        if (overlappingParents && scheduledChild) configured.eventValues.push_back({"cc19", 0.125});
        configured.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
            'M','T','r','k',0,0,0,12, 0,0x90,60,127, 10,0x80,60,0, 0,0xff,0x2f,0}}};
        if (overlappingParents)
            configured.binaryResources[0].bytes = {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
                'M','T','r','k',0,0,0,28, 0,0x90,60,127, 8,0x90,64,127, 4,0x80,60,0,
                4,0x90,67,127, 4,0x80,64,0, 8,0x80,67,0, 0,0xff,0x2f,0};
        if (scheduledChild)
        {
            configured.binaryResources.push_back({"child-notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
                'M','T','r','k',0,0,0,28, 0,0x90,60,32, 3,0xb0,18,32, 7,0x80,60,0,
                0,0x90,64,64, 3,0xb0,18,96, 7,0x80,64,0, 0,0xff,0x2f,0}});
            if (overlappingParents)
                configured.binaryResources.back().bytes = {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
                    'M','T','r','k',0,0,0,36, 0,0x90,60,32, 3,0xb0,18,32, 4,0xb0,19,96,
                    3,0x80,60,0, 0,0x90,64,64, 3,0xb0,18,96, 0,0xb0,19,64,
                    7,0x80,64,0, 0,0xff,0x2f,0};
        }
        struct ChildOracle
        {
            unsigned kind = 0;
            bool extraController = false;
            std::size_t sampleFrame = 0, childOrigin = 0, childAttackAt = 0;
            bool childStarted = false, secondStage = false, active = false;
            double velocity = 0;
            double sample(std::uint64_t compositionFrame)
            {
                // Parent Attack opens its Input at source sample five. VCA
                // gain observes the child directly, without a control-to-audio
                // interpolation stage after the child control expression.
                if (!childStarted && sampleFrame == 5)
                {
                    childStarted = true; childOrigin = compositionFrame;
                    childAttackAt = sampleFrame + 5;
                }
                if (childStarted)
                {
                    if (!secondStage && compositionFrame >= childOrigin + 10)
                    {
                        secondStage = true; active = false; childAttackAt = sampleFrame + 5;
                    }
                    if (sampleFrame == childAttackAt)
                    {
                        active = true;
                        velocity = kind == 1 || kind == 3
                            ? (secondStage ? 64.0 : 32.0) / 127
                            : secondStage ? 0.5 : 0.25;
                    }
                    if (compositionFrame >= childOrigin + 20) active = false;
                }
                ++sampleFrame;
                if (!active) return 0;
                if (kind == 0) return velocity;
                const auto scripted = kind == 2 || kind == 4;
                const auto age = compositionFrame - childOrigin;
                const auto controller = age < 3 ? 0.125
                    : age < 13 ? (scripted ? 0.25 : 32.0 / 127)
                    : (scripted ? 0.75 : 96.0 / 127);
                // The first CC coincides with another parent's reuse. The
                // second exposes this slot's reset value before its next CC.
                const auto secondController = !extraController ? 0 : age < 7 ? 0.125
                    : age < 13 ? (scripted ? 0.75 : 96.0 / 127)
                    : (scripted ? 0.5 : 64.0 / 127);
                return velocity + controller + secondController;
            }
        };
        struct NestedOracle
        {
            unsigned kind = 0;
            bool overlappingParents = false;
            std::size_t nextParentNote = 0;
            std::array<ChildOracle, 2> children{};
            std::array<bool, 2> occupied{};
            double sample(std::uint64_t compositionFrame)
            {
                while (nextParentNote < (overlappingParents ? 3U : 1U)
                    && compositionFrame >= nextParentNote * 8)
                {
                    // Notes 0/8 occupy slots 0/1; note 16 reuses the older
                    // released slot 0, cancelling its child's pending Attack.
                    const auto slot = nextParentNote++ % 2;
                    children[slot] = {kind, overlappingParents}; occupied[slot] = true;
                }
                double value = 0;
                for (std::size_t slot = 0; slot < children.size(); ++slot)
                    if (occupied[slot]) value += children[slot].sample(compositionFrame);
                // Every parent releases after its delayed Attack. Release
                // retains Input; only the next assignment replaces its child.
                return value;
            }
        } state{childKind, overlappingParents};
        std::uint64_t composition = 0;
        bool released = false;
        bool sawSimultaneousChildren = false, sawReusedControllerDefault = false;
        std::array<double, frames> live{}, resume{}, expected{}, silence{};
        std::array<std::uint64_t, frames> cursors{};
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            // Device samples stay on physical time, including source restarts.
            live[frame] = (double(frame) + 1) / 128;
            resume[frame] = frame % 11 == 7;
            if (composition == loopEnd)
            {
                composition = loopStart; released = false; state = {childKind, overlappingParents};
                for (std::size_t history = 0; history < loopStart; ++history) (void)state.sample(history);
            }
            expected[frame] = live[frame] * state.sample(composition) + enclosingController;
            sawSimultaneousChildren |= state.children[0].active && state.children[1].active;
            sawReusedControllerDefault |= state.nextParentNote == 3 && state.children[0].active
                && composition - state.children[0].childOrigin < 7;
            if (composition >= 2 && composition < 6) released |= resume[frame] != 0;
            if (composition < 2 || composition >= 6 || released) ++composition;
            cursors[frame] = composition;
        }
        if (overlappingParents && loopStart == 8)
            require(sawSimultaneousChildren, "overlap fixture must include audible child overlap after parent reuse");
        if (overlappingParents && scheduledChild && loopStart >= 4)
            require(sawReusedControllerDefault, "reuse fixture must expose the reset CC value before a new controller can overwrite it");
        configured.audioInputChannels = {{1, {live.begin(), live.end()}}, {2, {resume.begin(), resume.end()}},
            {3, {silence.begin(), silence.end()}}};
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), diagnostic(compiled));
        const auto offline = renderOffline(*compiled.graph, frames);
        require(offline.ok() && offline.mono.size() == frames, "nested parent Timeline loop offline render");
        for (std::size_t frame = 0; frame < frames; ++frame)
            require(std::abs(offline.mono[frame] - expected[frame]) < 1e-12,
                "nested held-loop source oracle at " + std::to_string(frame)
                    + ", parent=" + std::to_string(parentKind) + ", child=" + std::to_string(childKind)
                    + ", overlap=" + std::to_string(overlappingParents)
                    + ", loop=" + std::to_string(loopStart)
                    + ", expected=" + std::to_string(expected[frame]) + ", actual=" + std::to_string(offline.mono[frame]));
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), std::string(session.error()));
            for (int pass = 0; pass < 2; ++pass)
            {
                allocations = deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, "nested Timeline loop reset heap work");
                for (std::size_t offset = 0; offset < frames; offset += block)
                {
                    const auto count = std::min<std::size_t>(block, frames - offset);
                    const double* inputs[] = {live.data() + offset, resume.data() + offset, silence.data()};
                    std::array<double, 32> audio{}; double* outputs[] = {audio.data()};
                    allocations = deallocations = 0; countAllocations = true;
                    const auto rendered = session.render({nullptr, 3, inputs}, outputs, 1, count);
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0,
                        "nested Timeline hold/loop callback heap work");
                    for (std::size_t frame = 0; frame < count; ++frame)
                        require(std::abs(audio[frame] - expected[offset + frame]) < 1e-12,
                            "nested child must retain its own composition origin across parent holds and loops");
                    require(session.preparedScorePlaybackFrame() == cursors[offset + count - 1],
                        "nested child must not advance the held parent Timeline");
                }
            }
        }
        RealtimeRenderSession failed(*compiled.graph), reference(*compiled.graph);
        require(failed.ready() && reference.ready(), "nested Timeline rollback sessions prepare");
        std::array<double, 32> input{}, openGate{}, fault{}, actual{}, independent{};
        input.fill(0.5); openGate.fill(1);
        const double* channels[] = {input.data(), openGate.data(), fault.data()};
        double* outputs[] = {actual.data()}; double* referenceOutputs[] = {independent.data()};
        {
            auto checkpoint = RealtimeRenderSessionTestAccess::checkpoint(failed);
            require(bool(checkpoint), "nested whole-call checkpoint prepares");
            for (int chunk = 0; chunk < 2; ++chunk)
                require(failed.render({nullptr, 3, channels}, outputs, 1, 32),
                    "nested source advances through several tentative loop wraps");
        }
        require(failed.preparedScorePlaybackFrame() == 0,
            "nested whole-call rollback restores the original Timeline cursor");
        fault.back() = 10;
        allocations = deallocations = 0; countAllocations = true;
        const auto rejected = failed.render({nullptr, 3, channels}, outputs, 1, 32);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && failed.preparedScorePlaybackFrame() == 0
                && std::ranges::all_of(actual, [](auto sample) { return sample == 0; }),
            "nested late failure after wraps must restore the whole callback without heap work");
        fault.back() = 0;
        for (int chunk = 0; chunk < 2; ++chunk)
        {
            if (chunk) openGate.fill(0);
            require(reference.render({nullptr, 3, channels}, referenceOutputs, 1, 32),
                "nested rollback reference renders");
            allocations = deallocations = 0; countAllocations = true;
            const auto rendered = failed.render({nullptr, 3, channels}, outputs, 1, 32);
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0 && actual == independent
                    && failed.preparedScorePlaybackFrame() == reference.preparedScorePlaybackFrame(),
                "nested retry restores child origins, pending attacks, scoped gain and gate latches");
        }
        configured.preparedScore.reset();
        ProjectRuntimeBridge bridge(document, registry());
        require(bridge.compileAndStage(configured).ok(), "stage nested held Timeline loop");
        bridge.setPlaying(true);
        std::array<float, frames> bridgeLive{}, bridgeResume{}, bridgeSilence{};
        std::copy(live.begin(), live.end(), bridgeLive.begin());
        std::copy(resume.begin(), resume.end(), bridgeResume.begin());
        std::array<float, 32> bridgeAudio{}; float* bridgeOutputs[] = {bridgeAudio.data()};
        for (std::size_t offset = 0; offset < frames; offset += 32)
        {
            const float* inputs[] = {bridgeLive.data() + offset, bridgeResume.data() + offset, bridgeSilence.data()};
            allocations = deallocations = 0; countAllocations = true;
            const auto rendered = bridge.render({inputs, 3}, bridgeOutputs, 1, 32);
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0, "nested bridge callback heap work");
            for (std::size_t frame = 0; frame < bridgeAudio.size(); ++frame)
                require(std::abs(bridgeAudio[frame] - expected[offset + frame]) < 1e-6,
                    "public bridge preserves nested held-loop clocks and live device input");
        }
        bridgeLive.fill(0.5F); bridgeResume.fill(1);
        const float* seekInputs[] = {bridgeLive.data(), bridgeResume.data(), bridgeSilence.data()};
        for (const auto target : {loopEnd - 1, loopEnd + 5})
        {
            require(bridge.requestTransportRelocation(target), "prepare nested Timeline loop seek");
            auto playhead = target < loopEnd ? target : loopStart + (target - loopEnd) % loopLength;
            NestedOracle sought{childKind, overlappingParents};
            for (std::size_t history = 0; history < playhead; ++history) (void)sought.sample(history);
            for (const auto count : {1U, 32U})
            {
                allocations = deallocations = 0; countAllocations = true;
                const auto rendered = bridge.render({seekInputs, 3}, bridgeOutputs, 1, count);
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0, "nested seek adoption/loop callback heap work");
                for (std::size_t frame = 0; frame < count; ++frame)
                {
                    if (playhead == loopEnd)
                    {
                        playhead = loopStart; sought = {childKind, overlappingParents};
                        for (std::size_t history = 0; history < loopStart; ++history) (void)sought.sample(history);
                    }
                    require(std::abs(bridgeAudio[frame] - (0.5 * sought.sample(playhead)
                                + enclosingController)) < 1e-6,
                        "nested seek reconstructs parent and child clocks at wrapped composition time");
                    ++playhead;
                }
            }
        }
    }
}

void testParentLoopNestedAnalogSources()
{
    testParentLoopNestedSources(0);
}

void testParentMidiScript()
{
    auto document = fixture(); auto& sound = document.soundDefinitions[0];
    ProjectParameterExpression source, resource, script, shared;
    source.parameterId = "source"; source.text = "Script";
    resource.parameterId = "midiFile"; resource.resource = "";
    script.parameterId = "script";
    script.text = "self keyDownAt: 3 ms duration: 10 ms frequency: 445 hz velocity: 0.7.";
    shared.parameterId = "shared"; shared.boolean = false;
    sound.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "!Pitch"}, {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
        {"voice", "talas.midiVoice", {source, resource, script, shared, {"channel", "1"},
            {"lowPitch", "0 nn"}, {"highPitch", "127 nn"}, {"polyphony", "1"}, {"left", "1"}, {"right", "0.5"}}}};
    sound.graph.connections = {{"osc", "audioOut", "voice", "audioIn"}};
    sound.outputs[0].nodeId = "voice"; sound.outputs[0].nodePortId = "leftOut";
    sound.exposedParameters.clear(); document.timeline.score.automation.clear();
    const auto expectedInputs = oracle(false, false, false);
    auto configured = settings(document, 8);
    configured.eventValues.insert(configured.eventValues.end(),
        {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}});
    for (std::size_t ch = 0; ch < 2; ++ch)
        configured.audioInputChannels.push_back({ch + 1,
            {expectedInputs.inputs[ch].begin(), expectedInputs.inputs[ch].end()}});
    const auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), diagnostic(compiled));
    const auto offline = renderOffline(*compiled.graph, sampleCount);
    require(offline.ok(), offline.diagnostics.empty() ? "parent MIDI Script offline render"
        : offline.diagnostics.front().message);
    require(std::ranges::all_of(offline.mono | std::views::take(13),
        [](double value) { return value == 0.0; }),
        "script note after WaitUntil must remain silent while composition is held");
    require(std::ranges::any_of(offline.mono | std::views::drop(14),
        [](double value) { return std::abs(value) > 1e-6; }),
        "script note must start after Resume advances the composition cursor");
    for (const auto block : {1U, 8U})
    {
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), std::string(session.error()));
        for (std::size_t offset = 0; offset < sampleCount; offset += block)
        {
            const auto count = std::min<std::size_t>(block, sampleCount - offset);
            const double* inputs[] = {expectedInputs.inputs[0].data() + offset,
                expectedInputs.inputs[1].data() + offset};
            std::array<double, sampleCount> audio{}; double* outputs[] = {audio.data()};
            allocations = 0; countAllocations = true;
            const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, count);
            countAllocations = false;
            require(rendered && allocations == 0,
                "parent MIDI Script hold renders without callback allocation");
            for (std::size_t frame = 0; frame < count; ++frame)
                require(std::abs(audio[frame] - offline.mono[offset + frame]) < 1e-11,
                    "parent MIDI Script offline/realtime composition mismatch at "
                        + std::to_string(offset + frame));
        }
    }
}
void testNestedLocalClockOwner()
{
    auto document = fixture(false, true);
    auto nested = document.soundDefinitions[1]; nested.definitionId = "nestedGate";
    document.soundDefinitions.push_back(nested);
    ProjectNode child; child.instanceId = "child"; child.soundDefinitionId = "nestedGate";
    auto& wrapper = document.soundDefinitions[1]; wrapper.graph.nodes = {child}; wrapper.graph.connections.clear();
    wrapper.outputs[0].nodeId = "child";
    auto expected = oracle(false, true, true); expected.inputs[1].fill(0); // Local owner never releases.
    auto configured = settings(document, 8);
    const auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
    for (std::size_t offset = 0; offset < sampleCount; offset += 8)
    {
        const double* inputs[] = {expected.inputs[0].data() + offset, expected.inputs[1].data() + offset, expected.inputs[2].data() + offset};
        std::array<double, 8> audio{}; double* outputs[] = {audio.data()};
        require(session.render({nullptr, 3, inputs}, outputs, 1, 8), "local nested owner coexists with an immediate global gate");
        for (std::size_t f = 0; f < 8; ++f)
            require(std::abs(audio[f] - expected.audio[offset + f]) < 1e-12, "nested local gate must not hold its parent's Timeline clock");
        require(session.preparedScorePlaybackFrame() == offset + 8, "only the exact primary clock owner can hold the Timeline");
    }
}
void testParentCachedPlayback(bool delayed = false, bool loop = false)
{
    for (const auto mode : {ScoreClipRenderMode::Cached, ScoreClipRenderMode::Frozen})
    {
        auto document = fixture();
        if (loop) document.timeline.loop = {true, 0.002, 0.006};
        ProjectSoundDefinition failing; failing.definitionId = "failing"; failing.displayName = "Failure";
        failing.graph.nodes = {device("device", 3), {"gain", "talas.vca", {{"gain", "1e308"}}}};
        failing.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
        failing.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
        document.soundDefinitions.push_back(std::move(failing));
        auto failureClip = document.timeline.score.clips[0];
        failureClip.clipId = "failing"; failureClip.soundDefinitionId = "failing";
        document.timeline.score.clips.push_back(failureClip);
        if (delayed) document.timeline.score.clips[0].start = seconds(0.004);
        const auto path = std::filesystem::temp_directory_path() / ("talas-wait-cache-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        std::vector<std::vector<double>> audio{std::vector<double>(20)};
        for (std::size_t f = 0; f < 20; ++f) audio[0][f] = -(double(f) + 1) / 128;
        std::string error; require(talas::audio::writePcm16Wav(path, 1000, audio, error), error);
        const auto hash = calculateProjectAssetSha256(path); require(hash.ok(), "hash Timeline cache");
        const auto fingerprint = scoreClipSourceFingerprint(document, registry(), "background", 1000, error);
        require(!fingerprint.empty(), error);
        ProjectAsset asset; asset.id = "cache"; asset.kind = ProjectAssetKind::Audio;
        asset.path = path.string(); asset.contentHash = hash.contentHash; asset.channelCount = 1;
        asset.sampleRate = 1000; asset.frameCount = 20; asset.encoding = pcm16WavAssetEncoding; asset.generation = 1;
        document.assets.push_back(asset);
        document.timeline.score.clips[0].renderArtifact = ScoreClipRenderArtifact{mode, fingerprint, 1000, 20, {asset.id}};
        auto configured = settings(document, 8);
        if (loop) configured.transportLoop = TransportLoopRegion{2, 6};
        talas::core::ResourceSnapshot snapshot; snapshot.id = asset.id; snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
        snapshot.generation = 1; snapshot.contentHash = asset.contentHash; snapshot.resolvedPath = path;
        snapshot.channelCount = 1; snapshot.sampleRate = 1000; snapshot.frameCount = 20; snapshot.encoding = asset.encoding;
        configured.resourceSnapshots.push_back(std::make_shared<const talas::core::ResourceSnapshot>(snapshot));
        auto expected = oracle(false, false, false);
        for (std::size_t ch = 0; ch < 2; ++ch)
            configured.audioInputChannels.push_back({ch + 1, {expected.inputs[ch].begin(), expected.inputs[ch].end()}});
        const auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), diagnostic(compiled));
        require(compiled.graph->renderSettings().preparedScoreClipAudio.size() == 1, "explicit cache remains authoritative PCM");
        const auto& preparedPcm = *compiled.graph->renderSettings().preparedScoreClipAudio[0]->channels[0];
        // The first hold is at composition 2; composition 4 is first reached
        // at physical sample 9. A future cached clip must remain silent until then.
        const std::size_t firstSample = delayed ? 9 : 0;
        std::array<double, sampleCount> loopSamples{};
        if (loop)
        {
            std::uint64_t composition = 0, pcmFrame = 0;
            bool released = false, playing = false;
            for (std::size_t frame = 0; frame < sampleCount; ++frame)
            {
                if (composition == 6)
                {
                    composition = 2; released = playing = false;
                }
                if (composition >= (delayed ? 4U : 0U))
                {
                    if (!playing) { pcmFrame = composition - (delayed ? 4U : 0U); playing = true; }
                    loopSamples[frame] = pcmFrame < preparedPcm.size() ? preparedPcm[pcmFrame] : 0;
                    ++pcmFrame;
                }
                if (composition >= 2) released |= expected.inputs[1][frame] != 0;
                if (composition < 2 || released) ++composition;
                expected.cursorAfter[frame] = composition;
            }
        }
        const auto expectedSample = [&](std::size_t sample) {
            if (loop) return loopSamples[sample];
            return sample >= firstSample && sample - firstSample < preparedPcm.size()
                ? preparedPcm[sample - firstSample] : 0.0;
        };
        const auto offline = renderOffline(*compiled.graph, sampleCount); require(offline.ok(), "cached hold offline render");
        // The whole offline call can roll back several successful callbacks.
        RealtimeRenderSession checkpointSession(*compiled.graph);
        require(checkpointSession.ready(), std::string(checkpointSession.error()));
        {
            auto checkpoint = RealtimeRenderSessionTestAccess::checkpoint(checkpointSession);
            require(bool(checkpoint), "cache offline checkpoint must prepare");
            for (std::size_t offset = 0; offset < 16; offset += 8)
            {
                std::array<double, sampleCount> failureInput{};
                const double* inputs[] = {expected.inputs[0].data() + offset, expected.inputs[1].data() + offset, failureInput.data()};
                std::array<double, 8> scratch{}; double* outputs[] = {scratch.data()};
                require(checkpointSession.render({nullptr, 3, inputs}, outputs, 1, 8), "cache tentative offline chunks");
            }
        }
        require(checkpointSession.preparedScorePlaybackFrame() == 0, "cache whole-call rollback restores composition");
        std::array<double, 8> silentFailureInput{};
        const double* firstInputs[] = {expected.inputs[0].data(), expected.inputs[1].data(), silentFailureInput.data()};
        std::array<double, 8> firstOutput{}; double* firstOutputs[] = {firstOutput.data()};
        require(checkpointSession.render({nullptr, 3, firstInputs}, firstOutputs, 1, 8), "cache whole-call retry");
        for (std::size_t f = 0; f < 8; ++f)
            require(std::abs(firstOutput[f] - expectedSample(f)) < 1e-12, "cache offline rollback restores playback sample position");
        for (const auto block : {1U, 8U})
        {
            RealtimeRenderSession session(*compiled.graph); require(session.ready(), std::string(session.error()));
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (attempt) session.resetTransport();
                for (std::size_t offset = 0; offset < sampleCount; offset += block)
                {
                    const auto count = std::min<std::size_t>(block, sampleCount - offset);
                    std::array<double, sampleCount> failureInput{};
                    const double* inputs[] = {expected.inputs[0].data() + offset, expected.inputs[1].data() + offset, failureInput.data()};
                    std::array<double, sampleCount> output{}; double* outputs[] = {output.data()};
                    if (offset == 0 || (loop && offset == 8))
                    {
                        failureInput[count - 1] = 10; std::fill_n(output.data(), count, 19);
                        allocations = 0; countAllocations = true;
                        const auto failed = session.render({nullptr, 3, inputs}, outputs, 1, count);
                        countAllocations = false;
                        require(!failed && allocations == 0 && std::ranges::all_of(output, [](double v) { return v == 0; }),
                            "cache callback failure rolls back without allocations and silences caller output");
                        require(session.preparedScorePlaybackFrame() == (offset ? expected.cursorAfter[offset - 1] : 0),
                            "cache callback rollback restores composition");
                        failureInput[count - 1] = 0;
                    }
                    allocations = 0; countAllocations = true;
                    const auto rendered = session.render({nullptr, 3, inputs}, outputs, 1, count);
                    countAllocations = false;
                    require(rendered && allocations == 0, "cached playback during a hold must render without allocations");
                    for (std::size_t f = 0; f < count; ++f)
                    {
                        const auto sample = offset + f;
                        const auto value = expectedSample(sample);
                        require(std::abs(output[f] - value) < 1e-12 && std::abs(offline.mono[sample] - value) < 1e-12,
                            "cached file playback continues on sample time until EOF while the composition holds at " + std::to_string(sample)
                                + " realtime=" + std::to_string(output[f]) + " offline=" + std::to_string(offline.mono[sample])
                                + " expected=" + std::to_string(value));
                    }
                }
            }
        }
    }
}


void testPreparedTimelineJump()
{
    auto document = fixture(); document.timeline.score.clips[1].repeatCount = 1;
    const auto compiled = compileProject(document, registry(), settings(document, 8));
    require(compiled.ok(), diagnostic(compiled));
    RealtimeRenderSession session(*compiled.graph); std::string error;
    require(session.prepareTransportFrameBeforeFirstRender(8, error), error);
    std::array<double, 8> silence{}, audio{}; const double* inputs[] = {silence.data(), silence.data()};
    double* outputs[] = {audio.data()};
    for (std::uint64_t attempt = 0; attempt < 3; ++attempt)
    {
        for (std::uint64_t frame : {8, 16})
        {
            allocations = 0; countAllocations = true;
            session.setTransportSnapshotBeforeRender({.epoch = attempt + 1, .absoluteFrame = frame, .playheadFrame = frame,
                .sampleRate = 1000, .discontinuitySequence = attempt + 1, .playing = true});
            const auto rendered = session.render({nullptr, 2, inputs}, outputs, 1, 8);
            countAllocations = false;
            require(rendered && allocations == 0, "prepared Timeline jump and host discontinuity adopt without allocation");
            for (std::size_t f = 0; f < audio.size(); ++f)
            {
                const auto sample = frame + f;
                const auto expected = sample < 20 ? 0.75 * std::sin(2 * std::numbers::pi * 7 * sample / 1000) : 0;
                require(std::abs(audio[f] - expected) < 1e-12, "jump beyond WaitUntil restores source phase and automation without waiting for Resume");
            }
            require(session.preparedScorePlaybackFrame() == frame + 8, "jumped Timeline cursor advances beyond the skipped gate");
        }
    }
}

void testPublicBridgeTimelineJump()
{
    auto document = fixture(); document.timeline.score.clips[1].repeatCount = 1;
    auto configured = settings(document, 8); configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage public Timeline jump");
    bridge.setPlaying(true);
    std::array<float, 8> silence{}, audio{};
    const float* inputs[] = {silence.data(), silence.data()}; float* outputs[] = {audio.data()};
    require(bridge.render({inputs, 2}, outputs, 1, 8), "public Timeline initially reaches its waiting gate");
    for (const auto target : {8U, 2U, 8U, 2U, 8U})
    {
        require(bridge.requestTransportRelocation(target), "prepare public Timeline jump while active");
        allocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 2}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0, "public Timeline jump adopts without callback reconstruction");
        for (std::size_t f = 0; f < audio.size(); ++f)
        {
            const auto expected = (target == 2 ? 0.5 : 0.75)
                * std::sin(2 * std::numbers::pi * 7 * (target + f) / 1000);
            require(std::abs(audio[f] - expected) < 1e-6,
                "public jump skips the WaitUntil right edge or re-enters its held composition with continuing synthesis");
        }
    }
}

}
int main()
{
    bool failed = false;
    for (const auto& [name, test] : std::array<std::pair<const char*, void(*)()>, 48>{{
        {"parent-time-vca-gain-during-wait", +[] { testParentTimeExpressionContinuesDuringWait(true); }},
        {"live-vca-gain-expression", +[] { testLiveVcaGainExpression(); }},
        {"parent-loop-nested-analog-sources", +[] { testParentLoopNestedAnalogSources(); }},
        {"parent-loop-nested-file-voice", +[] { testParentLoopNestedSources(1); }},
        {"parent-loop-nested-script-voice", +[] { testParentLoopNestedSources(2); }},
        {"parent-loop-nested-file-mapper", +[] { testParentLoopNestedSources(3); }},
        {"parent-loop-nested-script-mapper", +[] { testParentLoopNestedSources(4); }},
        {"parent-loop-overlapping-analog", +[] { testParentLoopNestedSources(0, true); }},
        {"parent-loop-overlapping-file-voice", +[] { testParentLoopNestedSources(1, true); }},
        {"parent-loop-overlapping-script-voice", +[] { testParentLoopNestedSources(2, true); }},
        {"parent-loop-overlapping-file-mapper", +[] { testParentLoopNestedSources(3, true); }},
        {"parent-loop-overlapping-script-mapper", +[] { testParentLoopNestedSources(4, true); }},
        {"parent-loop-scheduled-voice-sources", +[] { testParentLoopScheduledVoiceSources(); }},
        {"parent-timeline-loop", +[] { testParentTimelineLoop(); }},
        {"parent-cached-timeline-loop", +[] { testParentCachedPlayback(false, true); }},
        {"parent-delayed-cache-timeline-loop", +[] { testParentCachedPlayback(true, true); }},
        {"parent-immediate-control", +[] { testParent(8, false, false, true); }},
        {"local-device-immediate-control", +[] { testLocalInput(false, true); }},
        {"parent-clock-partition-1", +[] { testParent(1); }},
        {"parent-clock-partition-3", +[] { testParent(3); }},
        {"parent-live-input-during-wait", +[] { testParentLiveInputContinuesDuringWait(); }},
        {"parent-clocked-control-during-wait", +[] { testParentClockedControlContinuesDuringWait(); }},
        {"parent-time-expression-during-wait", +[] { testParentTimeExpressionContinuesDuringWait(); }},
        {"realtime-time-expression-millisecond-cadence", +[] { testRealtimeTimeExpressionMillisecondCadence(); }},
        {"timed-oscillator-frequency-follows-time", +[] { testTimedOscillatorFrequencyFollowsTime(); }},
        {"parent-clock-resume-on-zero", +[] { testParent(8, true); }},
        {"parent-simultaneous-gates", +[] { testParent(8, false, true); }},
        {"parent-offline-clock", +[] { testParent(8, false, false, false, true); }},
        {"local-live-input-after-long-hold", +[] { testLocalInput(false); }},
        {"local-oscillator-controls-before-start", +[] { testLocalInput(true); }},
        {"local-shared-input", +[] { testLocalDependencyOwnership(false, false); }},
        {"local-nested-input", +[] { testLocalDependencyOwnership(true, false); }},
        {"local-shared-input-rollback", +[] { testLocalDependencyOwnership(false, true); }},
        {"local-nested-input-rollback", +[] { testLocalDependencyOwnership(true, true); }},
        {"local-offline-live-input", +[] { testLocalInput(false, false, true); }},
        {"local-independent-feedback", +[] { testLocalFeedback(false); }},
        {"local-gated-feedback", +[] { testLocalFeedback(true); }},
        {"local-offline-variable-assignment", +[] { testOfflineNativeCompatibility(false); }},
        {"local-offline-wav-source", +[] { testOfflineNativeCompatibility(true); }},
        {"parent-host-snapshots", +[] { testParent(3, false, false, false, false, true); }},
        {"parent-late-failure-rollback", +[] { testParentRollback(); }},
        {"parent-midi-file-clock", +[] { testParentMidiFile(); }},
        {"parent-midi-script-clock", +[] { testParentMidiScript(); }},
        {"parent-local-clock-owner", +[] { testNestedLocalClockOwner(); }},
        {"parent-prepared-jump", +[] { testPreparedTimelineJump(); }},
        {"public-bridge-timeline-jump", +[] { testPublicBridgeTimelineJump(); }},
        {"parent-delayed-cache-clock", +[] { testParentCachedPlayback(true); }},
        {"parent-cached-frozen-clock", +[] { testParentCachedPlayback(); }} }})
    {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { failed = true; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
