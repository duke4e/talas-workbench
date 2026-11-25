#include "../../src/runtime/PreparedScoreClipSource.h"
#include "../../src/runtime/PreparedLiveScoreSources.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"
#include "talas/runtime/SnapshotPerformance.h"

#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/runtime/ScoreClipMaterialization.h"
#include "talas/project/ProjectJson.h"
#include <filesystem>
#include <chrono>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <exception>
#include <thread>

namespace { std::atomic<bool> countAllocations{false}; std::atomic<std::size_t> allocations{0}, deallocations{0}; }
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept
{
    if (p && countAllocations.load(std::memory_order_relaxed)) deallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace talas::runtime
{
class ProjectRuntimeBridgeTestAccess
{
public:
    static void prepareNextRenderBoundary(ProjectRuntimeBridge& bridge) noexcept
    {
        bridge.graphHost_.prepareNextRenderBoundary();
    }
};
}

namespace
{
using namespace talas::runtime;
void require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}
const auto& registry() { static const auto r = talas::core::makeBuiltinNodeRegistry(); return r; }
talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}
talas::project::ProjectDocument fixture(bool connected = false)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice"; voice.displayName = "Voice";
    voice.graph.nodes = {{"gain", "talas.vca", {{"gain", "!Gain"}}}};
    voice.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
    if (connected)
        voice.inputs = {{"sourceIn", talas::core::PortType::Audio, "gain", "audioIn"}};
    else
    {
        voice.graph.nodes.push_back({"device", "talas.audioInput", {}});
        voice.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
    }
    document.soundDefinitions.push_back(std::move(voice));
    if (connected)
    {
        talas::project::ProjectSoundDefinition upstream;
        upstream.definitionId = "upstream"; upstream.displayName = "Upstream";
        upstream.graph.nodes = {{"input", "talas.audioInput", {}}};
        upstream.outputs = {{"audioOut", talas::core::PortType::Audio, "input", "audioOut"}};
        document.soundDefinitions.push_back(std::move(upstream));
        talas::project::ProjectNode input; input.instanceId = "deviceInstance"; input.soundDefinitionId = "upstream";
        talas::project::ProjectNode target; target.instanceId = "voiceInstance"; target.soundDefinitionId = "voice";
        document.graph.nodes = {std::move(input), std::move(target)};
        document.graph.connections = {{"deviceInstance", "audioOut", "voiceInstance", "sourceIn"}};
    }
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1, talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1, talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {{"clip", "track", "voice", seconds(0), seconds(0.016),
        seconds(0), seconds(0), seconds(0), 2}};
    if (connected) document.timeline.score.clips[0].sourceInstanceId = "voiceInstance";
    return document;
}
RenderSettings settings(const talas::project::ProjectDocument& document)
{
    RenderSettings result;
    result.sampleRate = 1000; result.maxBlockFrames = 8;
    result.eventValues = {{"Gain", 1.0, talas::expr::ValueDomain::Scalar}};
    // A distinct preparation-time input must never replace future device input.
    result.audioInputChannels = {{1, std::vector<double>(64, 0.75)}};
    const auto prepared = prepareProjectScore(document,
        preparedScoreBindingForDocument(document, 1, 1, 1000, 1));
    require(prepared.ok(), prepared.error);
    result.preparedScore = prepared.schedule;
    return result;
}
void checkLiveInput(const PreparedRuntimeGraph& graph)
{
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<float, 8> first{0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    std::array<float, 8> second{-0.6f, 0.7f, 0.8f, 0.9f, -0.5f, 0.4f, 0.3f, -0.2f};
    std::array<double, 8> output{};
    double* outputs[] = {output.data()};
    for (const auto* samples : {&first, &second})
    {
        const float* inputs[] = {samples->data()};
        allocations = 0; countAllocations = true;
        const auto rendered = session.render({inputs, 1}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0, "live source render without callback allocation");
        for (std::size_t i = 0; i < 8; ++i)
            require(std::abs(output[i] - (*samples)[i]) < 1e-7,
                "prepared source must process new device samples, not preparation-time PCM");
    }
}
void testPreparedSourceRemainsLive()
{
    for (const bool connected : {false, true})
    {
        auto document = fixture(connected);
        auto prepared = prepareScoreClipSourceIteration(document, registry(), settings(document), 0, 0);
        require(prepared.ok(), prepared.diagnostics.empty() ? "source preparation" : prepared.diagnostics.front().message);
        require(!prepared.graph->renderSettings().preparedScore
            && prepared.graph->renderSettings().preparedScoreClipAudio.empty(), "source must not recursively project the parent Score");
        document.soundDefinitions.clear();
        checkLiveInput(*prepared.graph);
    }
}
void testRepeatAutomationOrigins()
{
    auto document = fixture();
    document.soundDefinitions[0].graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}}};
    document.soundDefinitions[0].graph.connections.clear();
    document.soundDefinitions[0].outputs[0].nodeId = "osc";
    document.timeline.score.clips[0].contentOffset = seconds(0.003);
    auto configured = settings(document);
    configured.eventValueChanges = {
        {0, "Gain", 0.25, talas::expr::ValueDomain::Scalar},
        {20, "Gain", 0.5, talas::expr::ValueDomain::Scalar}};
    auto prepared = prepareScoreClipSourceIteration(document, registry(), configured, 0, 1);
    require(prepared.ok(), "prepare second repeat");
    RealtimeRenderSession session(*prepared.graph);
    require(session.ready(), session.error());
    std::array<float, 8> ones{}; ones.fill(1);
    const float* inputs[] = {ones.data()};
    std::array<double, 8> output{}; double* outputs[] = {output.data()};
    require(session.render({inputs, 1}, outputs, 1, 8), "render repeat automation");
    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto expected = (i < 3 ? 1.0 : i < 7 ? 0.25 : 0.5)
            * std::sin(2 * std::numbers::pi * 7 * i / 1000);
        require(std::abs(output[i] - expected) < 1e-12,
            "repeat automation frame " + std::to_string(i) + ": expected "
            + std::to_string(expected) + ", actual " + std::to_string(output[i]));
    }
}
void testScopedAutomationAndLiveControl()
{
    auto document = fixture();
    auto& definition = document.soundDefinitions[0];
    definition.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}}};
    definition.graph.connections.clear(); definition.outputs[0].nodeId = "osc";
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "gain"; exposed.displayName = "Gain";
    exposed.defaultValue = {"gain", "1.0"};
    exposed.bindings = {{talas::project::ProjectSoundParameterTarget::EventValue,
        {}, {}, "Gain", talas::expr::ValueDomain::Scalar}};
    definition.exposedParameters = {exposed};
    talas::project::ProjectNode first; first.instanceId = "first"; first.soundDefinitionId = "voice";
    first.parameters = {{"gain", "0.25"}};
    auto second = first; second.instanceId = "second"; second.parameters[0].expression = "0.5";
    document.graph.nodes = {first, second};
    document.timeline.score.clips[0].sourceInstanceId = "first";
    auto secondClip = document.timeline.score.clips[0];
    secondClip.clipId = "other"; secondClip.sourceInstanceId = "second";
    document.timeline.score.clips.push_back(std::move(secondClip));
    document.timeline.score.automation = {{"gainLane", talas::project::ScoreAutomationScope::Clip,
        "clip", "Gain", talas::expr::ValueDomain::Scalar,
        {{seconds(0), 0.75, talas::core::AutomationInterpolation::Step},
         {seconds(0.004), 0.125, talas::core::AutomationInterpolation::Step}}}};
    const auto configured = settings(document);
    for (std::size_t clip = 0; clip < 2; ++clip)
    {
        auto prepared = prepareScoreClipSourceIteration(document, registry(), configured, clip, 0);
        require(prepared.ok(), "prepare independently scoped clip automation");
        RealtimeRenderSession session(*prepared.graph);
        require(session.ready(), session.error());
        std::array<double, 8> output{};
        allocations = 0; countAllocations = true;
        const auto rendered = session.render(output);
        countAllocations = false;
        require(rendered && allocations == 0, "scoped source render without callback allocation");
        for (std::size_t i = 0; i < 8; ++i)
        {
            const auto gain = clip == 1 ? 0.5 : i < 4 ? 0.75 : 0.125;
            require(std::abs(output[i] - gain * std::sin(2 * std::numbers::pi * 7 * i / 1000)) < 1e-12,
                "clip automation must affect its selected source instance only");
        }
    }
    // A separate global-control source remains editable after preparation.
    document.soundDefinitions[0].exposedParameters.clear();
    document.graph.nodes.clear(); document.timeline.score.clips.resize(1);
    document.timeline.score.clips[0].sourceInstanceId.reset();
    document.timeline.score.automation.clear();
    auto prepared = prepareScoreClipSourceIteration(document, registry(), settings(document), 0, 0);
    require(prepared.ok(), "prepare live-control source");
    RealtimeRenderSession session(*prepared.graph);
    std::array<double, 8> output{};
    require(session.render(output), "render before live update");
    require(session.scheduleEventValue("Gain", 0.375, talas::expr::ValueDomain::Scalar, 2),
        "accept control published after source preparation and playback");
    allocations = 0; countAllocations = true;
    const auto rendered = session.render(output);
    countAllocations = false;
    require(rendered && allocations == 0, "live source control callback allocation");
    for (std::size_t i = 0; i < 8; ++i)
        require(std::abs(output[i] - (i < 2 ? 1.0 : 0.375)
            * std::sin(2 * std::numbers::pi * 7 * (i + 8) / 1000)) < 1e-12,
            "live source control changes at its exact new callback offset");
}
void testLinearScoreAutomationRampsAcrossIterations()
{
    auto document = fixture();
    auto& definition = document.soundDefinitions[0];
    definition.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}}};
    definition.graph.connections.clear(); definition.outputs[0].nodeId = "osc";
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "gain"; exposed.displayName = "Gain";
    exposed.defaultValue = {"gain", "1.0"};
    exposed.bindings = {{talas::project::ProjectSoundParameterTarget::EventValue,
        {}, {}, "Gain", talas::expr::ValueDomain::Scalar}};
    definition.exposedParameters = {exposed};
    talas::project::ProjectNode instance; instance.instanceId = "first"; instance.soundDefinitionId = "voice";
    instance.parameters = {{"gain", "1.0"}};
    document.graph.nodes = {instance};
    document.timeline.score.clips[0].sourceInstanceId = "first";
    document.timeline.score.automation = {{"rampLane", talas::project::ScoreAutomationScope::Clip,
        "clip", "Gain", talas::expr::ValueDomain::Scalar,
        {{seconds(0), 0.0, talas::core::AutomationInterpolation::Linear},
         {seconds(0.032), 1.0, talas::core::AutomationInterpolation::Step}}}};
    const auto configured = settings(document);
    for (std::size_t iteration = 0; iteration < 2; ++iteration)
    {
        auto prepared = prepareScoreClipSourceIteration(document, registry(), configured, 0, iteration);
        require(prepared.ok(), "prepare linear automation iteration: "
            + (prepared.diagnostics.empty() ? std::string{} : prepared.diagnostics.front().message));
        RealtimeRenderSession session(*prepared.graph);
        require(session.ready(), session.error());
        std::array<double, 8> output{};
        for (std::size_t block = 0; block < 2; ++block)
        {
            require(session.render(output), "render linear automation iteration");
            for (std::size_t i = 0; i < 8; ++i)
            {
                const auto local = block * 8 + i;
                const auto gain = static_cast<double>(iteration * 16 + local) / 32.0;
                require(std::abs(output[i] - gain * std::sin(2 * std::numbers::pi * 7 * local / 1000)) < 1e-9,
                    "linear score automation must ramp across clip iteration boundaries: iteration "
                    + std::to_string(iteration) + " frame " + std::to_string(local));
            }
        }
    }
}
void testReplaceTrackPlaysOnlyTheSelectedClip()
{
    auto document = fixture();
    auto& first = document.soundDefinitions[0];
    first.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.25"}}}};
    first.graph.connections.clear(); first.outputs[0].nodeId = "osc";
    auto second = first;
    second.definitionId = "voice2"; second.displayName = "Voice 2";
    second.graph.nodes[0].parameters[1].expression = "0.5";
    document.soundDefinitions.push_back(std::move(second));
    document.timeline.score.tracks[0].overlapMode = talas::project::ScoreOverlapMode::Replace;
    auto& a = document.timeline.score.clips[0];
    a.repeatCount = 1;
    auto b = a;
    b.clipId = "later"; b.soundDefinitionId = "voice2"; b.start = seconds(0.008);
    document.timeline.score.clips.push_back(std::move(b));
    const auto configured = settings(document);
    require(configured.preparedScore->clipAudibleAt(0, 7) && !configured.preparedScore->clipAudibleAt(0, 8)
            && configured.preparedScore->clipAudibleAt(1, 8) && configured.preparedScore->clipAudibleAt(1, 23),
        "Replace schedule should hand the overlap to the later clip");
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), "compile Replace track fixture");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{}; double* outputs[] = {output.data()};
    for (std::size_t block = 0; block < 3; ++block)
    {
        require(session.render({}, outputs, 1, 8), "render Replace track");
        for (std::size_t i = 0; i < 8; ++i)
        {
            const auto frame = block * 8 + i;
            const auto expected = frame < 8
                ? 0.25 * std::sin(2 * std::numbers::pi * 7 * frame / 1000)
                : 0.5 * std::sin(2 * std::numbers::pi * 7 * (frame - 8) / 1000);
            require(std::abs(output[i] - expected) < 1e-9,
                "Replace track must play only the selected clip at frame " + std::to_string(frame)
                + ": expected " + std::to_string(expected) + ", actual " + std::to_string(output[i]));
        }
    }
    const auto materialized = materializeProjectScoreClips(document, registry(), configured);
    require(materialized.ok() && materialized.clips.size() == 2, "materialize Replace track clips");
    const auto& replaced = *materialized.clips[0]->channels[0];
    require(replaced.size() == 16 && replaced[7] != 0.0
            && std::ranges::all_of(replaced.begin() + 8, replaced.end(), [](double value) { return value == 0.0; }),
        "materialized Replace audio must silence the replaced clip where the later clip is selected");
}
void testInvalidSelectionRejects()
{
    auto document = fixture(); auto configured = settings(document);
    require(!prepareScoreClipSourceIteration(document, registry(), configured, 1, 0).ok(), "unknown clip rejects");
    require(!prepareScoreClipSourceIteration(document, registry(), configured, 0, 2).ok(), "unknown repeat rejects");
    configured.sampleRate = 2000;
    require(!prepareScoreClipSourceIteration(document, registry(), configured, 0, 0).ok(), "stale sample-rate binding rejects");
    configured.preparedScore.reset();
    require(!prepareScoreClipSourceIteration(document, registry(), configured, 0, 0).ok(), "missing schedule rejects");
}
// Actual parent acceptance, also retained as a standalone diagnostic probe.
void probeParentScoreLiveInput()
{
    for (const bool connected : {false, true})
    {
        const auto document = fixture(connected);
        const auto compiled = compileProject(document, registry(), settings(document));
        require(compiled.ok(), "parent Score compile");
        require(compiled.graph->renderSettings().preparedScoreClipAudio.empty(), "ordinary parent clips must not become incidental PCM");
        checkLiveInput(*compiled.graph);
    }
}
void testStagedTimelineClipProcessesNewDeviceInput()
{
    auto document = fixture();
    auto configured = settings(document);
    configured.eventValues[0].value = 0.5;
    configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage live-input Timeline clip");
    bridge.setPlaying(true);

    for (const auto [frame, impulse] : {std::pair{2U, 1.0F}, std::pair{5U, -0.75F}})
    {
        std::array<float, 8> input{}, output{};
        input[frame] = impulse;
        const float* inputs[] = {input.data()};
        float* outputs[] = {output.data()};
        require(bridge.render({inputs, 1}, outputs, 1, output.size()),
            "staged Timeline clip must render live device input");
        for (std::size_t index = 0; index < output.size(); ++index)
            require(std::abs(output[index] - (index == frame ? impulse * 0.5F : 0.0F)) < 1e-7F,
                "staged Timeline clip must apply gain to the new input impulse");
    }
}
void testPublicPlanCannotForgeLiveParentOffline()
{
    auto document = fixture(); auto configured = settings(document);
    configured.audioInputChannels = {{1, std::vector<double>(32, 0.75)}};
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), "compile source authority forgery fixture");
    for (const bool keepScore : {true, false})
    {
        auto publicSettings = compiled.graph->renderSettings();
        if (!keepScore) publicSettings.preparedScore.reset();
        auto publicPlan = compiled.graph->plan();
        if (!keepScore) publicPlan.requiresLiveScoreSources = false;
        PreparedRuntimeGraph forged{std::move(publicPlan), std::move(publicSettings)};
        RealtimeRenderSession realtime(forged);
        require(!realtime.ready(), "public Score reconstruction must reject realtime even without its public schedule");
        require(!renderOffline(forged, 8).ok(), "public Score reconstruction must reject offline instead of reading snapshot input");
    }
}
void testParentOfflineRetainsDoubleInput()
{
    for (const bool large : {false, true})
    {
        auto document = fixture();
        document.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", large ? "1e-100" : "1"}};
        auto configured = settings(document);
        const auto sample = large ? 1e100 : 1.0 + std::ldexp(1.0, -40);
        configured.audioInputChannels = {{1, std::vector<double>(32, sample)}};
        auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), "compile exact double offline Score input");
        auto rendered = renderOffline(*compiled.graph, 8);
        require(rendered.ok(), "finite double Score input must not narrow to float infinity");
        const auto expected = sample * (large ? 1e-100 : 1);
        for (double value : rendered.mono)
            require(value == expected, "offline Score input must preserve double snapshot precision");
    }
}
void checkParentStreamRollback(int contentOffset, bool connectedRate = false, bool preparedSeek = false, bool publicBridge = false)
{
    auto document = fixture();
    document.timeline.score.clips[0].contentOffset = seconds(double(contentOffset) / 1000);
    auto& voice = document.soundDefinitions[0];
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = "fileName"; resource.resource = "source";
    voice.graph.nodes = {{"disk", "talas.diskPlayer", {resource, {"duration", "40 s"}}}};
    voice.graph.connections.clear(); voice.outputs[0].nodeId = "disk";
    if (connectedRate)
    {
        voice.graph.nodes.push_back({"rate", "talas.constant", {{"value", "512"}}});
        voice.graph.connections.push_back({"rate", "value", "disk", "rateScale"});
    }
    auto failing = voice; failing.definitionId = "failing";
    failing.graph.nodes = {{"device", "talas.audioInput", {}}, {"gain", "talas.vca", {{"gain", "1e308"}}}};
    failing.graph.connections = {{"device", "audioOut", "gain", "audioIn"}};
    failing.outputs[0].nodeId = "gain";
    document.soundDefinitions.push_back(std::move(failing));
    auto clip = document.timeline.score.clips[0]; clip.clipId = "failing"; clip.soundDefinitionId = "failing"; clip.start = seconds(0.004);
    document.timeline.score.clips.push_back(std::move(clip));
    const auto path = std::filesystem::temp_directory_path() / ("talas-parent-stream-retry-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    std::string error;
    const auto sampleAt = [](std::size_t frame) { return -(1.0 + double(frame % 127)) / 256.0; };
    // The WAV writer stores round(x * 32767) and readers divide by 32767.
    const auto decodedAt = [&](std::size_t frame) { return std::round(sampleAt(frame) * 32767) / 32767; };
    std::vector<std::vector<double>> samples{std::vector<double>(16384)};
    for (std::size_t frame = 0; frame < samples[0].size(); ++frame) samples[0][frame] = sampleAt(frame);
    require(talas::audio::writePcm16Wav(path, 1000, samples, error), error);
    auto hash = talas::project::calculateProjectAssetSha256(path); require(hash.ok(), "hash parent stream retry WAV");
    talas::project::ProjectAsset asset; asset.id = "source"; asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = path.string(); asset.contentHash = hash.contentHash; asset.channelCount = 1;
    asset.sampleRate = 1000; asset.frameCount = 16384; asset.encoding = talas::project::pcm16WavAssetEncoding; asset.generation = 1;
    document.assets.push_back(std::move(asset));
    auto configured = prepareScoreClipMaterializationSettings(document, registry(), std::nullopt, "clip", 1000, 8);
    require(configured.ok(), configured.error);
    const auto rejectsPreparedHistory = preparedSeek && contentOffset == 0 && !connectedRate;
    if (rejectsPreparedHistory)
    {
        configured.settings->audioInputChannels = {{1, std::vector<double>(16, 0)}};
        configured.settings->audioInputChannels[0].samples[2] = 10;
    }
    if (publicBridge)
    {
        configured.settings->audioInputChannels.clear();
        configured.settings->preparedScore.reset();
        ProjectRuntimeBridge bridge(document, registry());
        require(bridge.compileAndStage(*configured.settings).ok(), "stage public streamed seek");
        bridge.setPlaying(true);
        std::array<float, 8> input{}, audio{};
        const float* inputs[] = {input.data()}; float* outputs[] = {audio.data()};
        require(bridge.render({inputs, 1}, outputs, 1, 4), "initial streamed bridge callback");
        for (const auto target : {6U, 14U, 19U, 4U})
        {
            require(bridge.requestTransportRelocation(target), "prepare public stream cursor and DSP state");
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto rendered = bridge.render({inputs, 1}, outputs, 1, 8);
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0,
                "public streamed seek uses prepared windows with control-side retirement");
            for (std::size_t f = 0; f < audio.size(); ++f)
            {
                const auto position = std::size_t(contentOffset) + ((target + f) % 16) * (connectedRate ? 512 : 1);
                const auto expected = contentOffset >= 16384 ? 0.0 : decodedAt(position);
                require(audio[f] == static_cast<float>(expected), "public seek restores exact streamed repeat/offset/rate/EOF samples");
            }
            (void) bridge.collectRetired();
        }
        return;
    }
    auto compiled = compileProject(document, registry(), *configured.settings);
    require(compiled.ok(), "compile parent stream retry");
    RealtimeRenderSession session(*compiled.graph); require(session.ready(), session.error());
    const std::uint64_t transportStart = preparedSeek ? 6 : 0;
    if (preparedSeek)
    {
        if (rejectsPreparedHistory)
            for (std::uint64_t rejectedTarget = 7; rejectedTarget < 12; ++rejectedTarget)
                require(!session.prepareTransportFrameBeforeFirstRender(rejectedTarget, error),
                    "failed later source restores earlier stream windows and leaves room for the next candidate");
        require(session.prepareTransportFrameBeforeFirstRender(transportStart, error), error);
        session.setTransportFrameBeforeFirstRender(transportStart);
    }
    std::array<float, 8> input{}; input.back() = 10;
    const float* inputs[] = {input.data()};
    std::array<double, 8> output{}; double* outputs[] = {output.data()};
    allocations = 0; countAllocations = true;
    const auto failed = session.render({inputs, 1}, outputs, 1, 8);
    countAllocations = false;
    require(!failed && allocations == 0 && std::ranges::all_of(output, [](double value) { return value == 0; }),
        "later source failure must cancel streamed sibling without allocation or caller audio");
    input.fill(0);
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        allocations = 0; countAllocations = true;
        if (attempt >= 2) { session.resetTransport(); session.setTransportFrameBeforeFirstRender(transportStart); }
        const auto rendered = session.render({inputs, 1}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0, "stream retry, continuation and repeated reset without allocation");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
        {
            const auto position = std::size_t(contentOffset) + ((transportStart + frame + (attempt == 1 ? 8 : 0)) % 16) * (connectedRate ? 512 : 1);
            const auto expected = contentOffset >= 16384 ? 0.0 : decodedAt(position);
            require(output[frame] == expected,
                "stream retry, continuation and repeated reset retain exact varying samples and callback position");
        }
    }
}
void testParentStreamRollsBackAfterLaterSourceFailure()
{ for (const auto offset : {0, 2048, 17000}) checkParentStreamRollback(offset); }
void testParentStreamConnectedRateHasExactInitialWindow()
{ checkParentStreamRollback(0, true); }
void testPublicBridgeStreamTransport()
{
    for (const auto offset : {0, 2048, 17000}) checkParentStreamRollback(offset, false, false, true);
    checkParentStreamRollback(0, true, false, true);
}
void testParentTransportResetRestartsSources()
{
    for (const auto offset : {0, 2, 17})
    for (const bool history : {false, true})
    {
        auto document = fixture();
        auto& voice = document.soundDefinitions[0];
        voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}}};
        voice.graph.connections.clear();
        voice.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
        if (history)
        {
            talas::project::ProjectParameterExpression coefficients;
            coefficients.parameterId = "coefficients"; coefficients.text = "0.5 0.25";
            voice.graph.nodes.push_back({"fir", "talas.firFilter", {coefficients}});
            voice.graph.nodes.push_back({"delay", "talas.delayWithFeedback", {{"delay", "2 ms"}, {"feedback", "0.25"}, {"mix", "1"}}});
            voice.graph.connections = {{"osc", "audioOut", "fir", "audioIn"}, {"fir", "audioOut", "delay", "audioIn"}};
            voice.outputs[0].nodeId = "delay";
        }
        document.timeline.score.clips[0].contentOffset = seconds(double(offset) / 1000);
        auto configured = settings(document);
        configured.eventValueChanges = {{12, "Gain", 0.25, talas::expr::ValueDomain::Scalar}};
        auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), "compile parent reset fixture");
        RealtimeRenderSession session(*compiled.graph); require(session.ready(), session.error());
        std::array<double, 8> first{}, reset{}; double* outputs[] = {first.data()};
        require(session.render({}, outputs, 1, 8), "render parent before reset");
        outputs[0] = reset.data();
        for (int repetition = 0; repetition < 3; ++repetition)
        {
            allocations = 0; countAllocations = true;
            session.resetTransport();
            const auto ok = session.render({}, outputs, 1, 8);
            countAllocations = false;
            require(ok && allocations == 0 && first == reset,
                "repeated parent reset must restore source phase, offset automation and FIR/feedback history without allocation");
        }
    }
}
void testParentSourcePreparationFitsWorkerStack()
{
    std::exception_ptr failure;
    std::thread worker([&] {
        try { testParentTransportResetRestartsSources(); }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    if (failure) std::rethrow_exception(failure);
}
void testParentRepeatsOffsetsFadesAndOffline()
{
    auto document = fixture();
    document.soundDefinitions[0].graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.5"}}}};
    document.soundDefinitions[0].graph.connections.clear();
    document.soundDefinitions[0].outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    auto& clip = document.timeline.score.clips[0];
    clip.contentOffset = seconds(0.002); clip.fadeIn = seconds(0.004); clip.fadeOut = seconds(0.005);
    auto compiled = compileProject(document, registry(), settings(document));
    require(compiled.ok(), "parent repeat/offset compile");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 32> oracle{}, actual{};
    for (std::size_t f = 0; f < oracle.size(); ++f)
    {
        double gain = f < 4 ? double(f) / 4 : 1;
        if (32 - f <= 5) gain = std::min(gain, double(31 - f) / 5);
        oracle[f] = 0.5 * gain * std::sin(2 * std::numbers::pi * 7 * (f % 16 + 2) / 1000);
    }
    std::size_t position = 0;
    for (const std::size_t frames : {3, 8, 7, 5, 8, 1})
    {
        double* output[] = {actual.data() + position};
        allocations = 0; countAllocations = true;
        const auto ok = session.render({}, output, 1, frames);
        countAllocations = false;
        require(ok && allocations == 0, "parent active/repeat callback without allocation");
        position += frames;
    }
    const auto offline = renderOffline(*compiled.graph, 32);
    require(offline.ok(), offline.diagnostics.empty() ? "parent offline render" : offline.diagnostics.front().message);
    for (std::size_t f = 0; f < actual.size(); ++f)
        require(std::abs(actual[f] - oracle[f]) < 1e-12 && std::abs(offline.mono[f] - oracle[f]) < 1e-12,
            "live and offline parent preserve repeat phase, content offset and whole-clip fades");
}
void testParentLaterSourceFailureRestoresWholeCall()
{
    for (const auto laterStart : {0.0, 0.004})
    {
        auto document = fixture();
        document.soundDefinitions[0].graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}}};
        document.soundDefinitions[0].graph.connections.clear();
        document.soundDefinitions[0].outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
        auto failing = fixture().soundDefinitions[0]; failing.definitionId = "failing";
        failing.graph.nodes[0].parameters = {{"gain", "1e308"}};
        document.soundDefinitions.push_back(std::move(failing));
        auto clip = document.timeline.score.clips[0]; clip.clipId = "later"; clip.soundDefinitionId = "failing"; clip.start = seconds(laterStart);
        document.timeline.score.clips.push_back(std::move(clip));
        auto compiled = compileProject(document, registry(), settings(document));
        require(compiled.ok(), "parent late source failure compile");
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<float, 8> input{}; input.back() = 10;
        const float* inputs[] = {input.data()}; std::array<double, 8> output{}; output.fill(19);
        double* outputs[] = {output.data()};
        allocations = 0; countAllocations = true;
        const auto failed = !session.render({inputs, 1}, outputs, 1, 8);
        countAllocations = false;
        require(failed && allocations == 0 && std::ranges::all_of(output, [](double v) { return v == 0; }),
            "late parent source failure clears caller audio without allocation");
        input.back() = 0;
        require(session.render({inputs, 1}, outputs, 1, 8), "parent source retry");
        for (std::size_t f = 0; f < 8; ++f)
            require(std::abs(output[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * f / 1000)) < 1e-12,
                "parent retry restores earlier source phase after later source overflow");
    }
}

void testLiveScoreSegmentsHoldCompositionAndRestoreOfflineAbort()
{
    auto document = fixture();
    document.soundDefinitions[0].graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.5"}}}};
    document.soundDefinitions[0].graph.connections.clear();
    document.soundDefinitions[0].outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.004); clip.contentOffset = seconds(0.001);
    clip.fadeIn = seconds(0.002); clip.fadeOut = seconds(0.002);
    auto configured = settings(document); configured.maxBlockFrames = 16;
    std::string error;
    auto authority = prepareLiveScoreSources(document, registry(), configured, error);
    require(authority != nullptr, error); authority->parentSettings = configured;
    PreparedLiveScorePlayback playback(authority); require(playback.ready(), playback.error());
    require(playback.begin(0, 12), "begin before active-checkpoint rejection");
    bool rejected = false;
    try { auto invalid = playback.checkpoint(); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "offline checkpoint cannot capture an active source callback");
    playback.rollback();
    {
        auto checkpoint = playback.checkpoint();
        require(playback.begin(0, 12) && playback.renderSegment({}, 0, 4, 0, 0, true), "open partial offline source transaction");
        require(!checkpoint->commit(), "offline commit waits for the open source callback");
        // Destruction must abort any open callback before restoring its children.
    }
    std::array<double, 16> reference{};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        allocations = 0; countAllocations = true;
        const auto begun = playback.begin(0, 12);
        const auto first = begun && playback.renderSegment({}, 0, 4, 0, 0, true);
        const auto held = first && playback.renderSegment({}, 4, 5, 4, 4, false);
        const auto resumed = held && playback.renderSegment({}, 4, 3, 9, 9, true);
        countAllocations = false;
        require(begun && first && held && resumed && allocations == 0,
            "source manager separates composition age from DSP age inside one callback");
        const auto* audio = playback.channel(0, 0); require(audio != nullptr, "segmented source audio exists");
        for (std::size_t f = 0; f < 12; ++f)
        {
            const auto phaseFrame = f == 11 ? 1 : f >= 2 ? f - 1 : 0;
            const auto gain = f < 3 ? 0.0 : f == 3 ? 0.25 : 0.5;
            const auto expected = gain * std::sin(2 * std::numbers::pi * 7 * phaseFrame / 1000);
            require(std::abs(audio[f] - expected) < 1e-12,
                "active source continues through hold, and repeat starts from its prepared offset");
            if (attempt == 0) reference[f] = audio[f];
            else require(reference[f] == audio[f], "source manager rollback restores appended source histories");
        }
        if (attempt == 0) playback.rollback(); else require(playback.commit(), "commit complete source manager transaction");
    }
    require(playback.begin(7, 4) && playback.renderSegment({}, 7, 4, 0, 0, true) && playback.commit(),
        "source composition position continues after suspended callback");
    const auto* audio = playback.channel(0, 0);
    for (std::size_t f = 0; f < 4; ++f)
    {
        const auto gain = f == 0 ? 0.5 : f == 1 ? 0.25 : 0.0;
        require(std::abs(audio[f] - gain * std::sin(2 * std::numbers::pi * 7 * (f + 2) / 1000)) < 1e-12,
            "fade and exclusive clip end follow composition time after resume");
    }
}

void testLoopSegmentsKeepSeparateTentativeSourceOwners()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections.push_back({"gain", "audioOut", "mix", "in1"});
    voice.graph.connections.push_back({"osc", "audioOut", "mix", "in2"});
    voice.outputs[0].nodeId = "mix";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.004); clip.repeatCount = 1;
    auto configured = settings(document);
    configured.eventValues[0].value = 0.5;
    std::string error;
    auto authority = prepareLiveScoreSources(document, registry(), configured, error);
    require(authority != nullptr, error);
    authority->parentSettings = configured;
    PreparedLiveScorePlayback beforeWrap(authority), afterWrap(authority);
    require(beforeWrap.ready() && afterWrap.ready(), "prepare independent loop source owners");
    require(afterWrap.prepareTransportPoint(2, error), error);

    std::array<float, 8> input{};
    input[3] = 1.0F; input[7] = -0.5F;
    const float* inputs[] = {input.data()};
    std::array<double, 2> firstResult{};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        beforeWrap.reset();
        require(afterWrap.relocatePrepared(2), "restore exact loop-entry source history");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = beforeWrap.beginSegment(0, 8, 0)
            && beforeWrap.renderSegment({inputs, 1}, 0, 6, 0, 0, true)
            && beforeWrap.sealSegment(6)
            && afterWrap.beginSegment(2, 8, 6)
            && afterWrap.renderSegment({inputs, 1}, 2, 2, 6, 6, true);
        const auto* first = beforeWrap.channel(0, 0);
        const auto* second = afterWrap.channel(0, 0);
        const std::array<double, 2> result{first ? first[3] : 0.0, second ? second[7] : 0.0};
        const auto rejectedLateSegment = attempt == 0
            && !afterWrap.renderSegment({inputs, 1}, 4, 1, 7, 7, true);
        if (attempt == 0) { beforeWrap.rollback(); afterWrap.rollback(); }
        else { require(beforeWrap.commit() && afterWrap.commit(), "commit both loop source segments together"); }
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0,
            "loop source segments retain tentative callback state without allocation");
        if (attempt == 0)
        {
            require(rejectedLateSegment, "a failure after the loop wrap rejects the parent candidate");
            firstResult = result;
        }
        else
            require(result == firstResult, "retry restores both pre-wrap and loop-entry source state");
    }
    require(std::abs(firstResult[0] - 0.5
            - 0.125 * std::sin(2 * std::numbers::pi * 7 / 1000)) < 1e-12,
        "pre-wrap source uses fresh input and exact oscillator age");
    require(std::abs(firstResult[1] + 0.25
            - 0.125 * std::sin(2 * std::numbers::pi * 7 / 1000)) < 1e-12,
        "loop-entry source restarts phase and reads new input after the wrap");
}

void testLoopSourceOfflineAnalysisPublicationOrder()
{
    for (const auto capacity : {2U, 8U})
    for (const bool parentCapture : {false, true})
    for (const bool reverseCommit : {false, true})
    {
        auto document = fixture();
        document.timeline.score.clips[0].repeatCount = 1;
        auto configured = settings(document);
        configured.enableRealtimeAnalysisCapture = true;
        configured.realtimeAnalysisQueueCapacity = capacity;
        configured.realtimeAnalysisTargets.push_back({{"scoreMaterializationSource"}, "voice", "gain", "audioOut",
            RealtimeAnalysisKind::Spectrum, RealtimeAnalysisPayloadKind::AudioBlock, 1, "source-order"});
        std::string error;
        auto authority = prepareLiveScoreSources(document, registry(), configured, error);
        require(authority != nullptr, error);
        authority->parentSettings = configured;
        auto capture = authority->sources[0].graph.analysisCapture();
        require(capture && capture->probeCount() == 1 && capture->bindStage(41), "prepare source analysis capture");
        talas::project::ProjectDocument parent;
        parent.graph.nodes = {{"input", "talas.audioInput", {}}, {"out", "talas.audioOutput", {}}};
        parent.graph.connections = {{"input", "audioOut", "out", "audioIn"}};
        RenderSettings parentSettings;
        parentSettings.sampleRate = 1000; parentSettings.maxBlockFrames = 8;
        parentSettings.enableRealtimeAnalysisCapture = parentCapture;
        if (parentCapture)
            parentSettings.realtimeAnalysisTargets.push_back({{}, {}, "input", "audioOut",
                RealtimeAnalysisKind::Spectrum, RealtimeAnalysisPayloadKind::AudioBlock, 1, "parent-order"});
        auto parentGraph = compileProject(parent, registry(), parentSettings);
        require(parentGraph.ok(), "prepare independent parent analysis capture");
        const auto rootCapture = parentGraph.graph->analysisCapture();
        PreparedSourceCallback publicationOwner(*parentGraph.graph);
        require(publicationOwner.ready(), publicationOwner.error());
        PreparedLiveScorePlayback first(authority), second(authority);
        require(first.ready() && second.ready(), "prepare analysis loop owners");
        require(first.prepareTransportPoint(2, error) && second.prepareTransportPoint(2, error), error);
        std::array<double, 8> values{};
        for (const bool commit : {false, true})
        {
            auto publication = publicationOwner.beginOfflineTransaction();
            require(publication != nullptr, "prepare whole-call analysis journal");
            auto firstCheckpoint = first.checkpoint();
            auto secondCheckpoint = second.checkpoint();
            for (std::size_t segment = 0; segment < 3; ++segment)
            {
                auto& owner = segment == 1 ? second : first;
                std::array<float, 4> input{}; input.fill(static_cast<float>(segment + 1) / 4);
                const float* inputs[]{input.data()};
                std::array<double, 4> output{}; double* outputs[]{output.data()};
                const auto playhead = segment == 0 ? 0U : 2U;
                allocations = 0; deallocations = 0; countAllocations = true;
                const auto rendered = (segment == 0 || owner.relocatePrepared(playhead))
                    && owner.begin(playhead, input.size())
                    && owner.renderSegment({inputs, 1}, playhead, input.size(), 0, 0, true)
                    && owner.commit()
                    && publicationOwner.renderTentative({inputs, 1}, outputs, 1, input.size());
                if (rendered) publicationOwner.commit();
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0,
                    "loop analysis sources render and relocate without callback allocation");
                owner.collectOfflineAnalysis(*publication);
                publication->collectAnalysis(input.size());
                require(capture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord,
                    "offline source analysis remains unpublished before whole-call commit");
                require(!rootCapture || rootCapture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord,
                    "offline parent analysis remains unpublished with its child records");
            }
            if (commit)
            {
                require(reverseCommit ? secondCheckpoint->commit() && firstCheckpoint->commit()
                    : firstCheckpoint->commit() && secondCheckpoint->commit(), "commit both analysis loop owners");
                require(capture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord,
                    "source checkpoint commits cannot publish before the whole-call commit");
                require(publication->commit(), "publish the complete analysis journal");
                const auto latest = capture->copyLatestRecordResult(0, values);
                const auto records = std::min(capacity, 3U);
                require(latest.status == RealtimeAnalysisCopyStatus::Accepted && latest.header.sequence == records
                    && latest.header.absoluteSamplePosition == 2 && latest.header.valueCount == 4
                    && std::ranges::all_of(values.begin(), values.begin() + 4,
                        [records](double value) { return value == static_cast<double>(records) / 4; }),
                    "offline source analysis must publish in render order across reused loop owners");
                require(capture->droppedBlockCount(0) == 3 - records && capture->rejectedRecordCount(0) == 0,
                    "aborted source analysis consumes neither queue records nor sequence numbers");
                if (rootCapture)
                {
                    const auto rootRecord = rootCapture->copyLatestRecordResult(0, values);
                    require(rootRecord.status == RealtimeAnalysisCopyStatus::Accepted
                        && rootRecord.header.sequence == 3 && rootRecord.header.absoluteSamplePosition == 8
                        && rootRecord.header.valueCount == 4
                        && std::ranges::all_of(values.begin(), values.begin() + 4, [](double value) { return value == 0.75; }),
                        "parent and child records retain their own capture identity and source cursor");
                }
            }
        }
    }
}

void testPublicScoreSourceAnalysisTargets()
{
    auto document = fixture(true);
    auto other = document.timeline.score.clips[0];
    other.clipId = "other"; other.start = seconds(0.008); other.duration = seconds(0.008); other.repeatCount = 1;
    document.timeline.score.clips.push_back(other);
    auto configured = settings(document);
    configured.enableRealtimeAnalysisCapture = true;
    configured.realtimeAnalysisTargets.push_back({{"voiceInstance"}, "voice", "gain", "audioOut",
        RealtimeAnalysisKind::Spectrum, RealtimeAnalysisPayloadKind::AudioBlock, 1, "public-source"});
    for (std::size_t frame = 0; frame < 32; ++frame)
        configured.audioInputChannels[0].samples[frame] = static_cast<double>(frame + 1) / 64;
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), compiled.diagnostics.empty() ? "compile public Score source analysis"
        : compiled.diagnostics.front().message);
    const auto captures = compiled.graph->scoreSourceAnalysisCaptures();
    require(captures && captures->size() == 3 && !compiled.graph->analysisCapture(),
        "public Score exposes independent source captures without inventing a parent probe");
    require((*captures)[0].clipId == "clip" && (*captures)[0].iterationOrdinal == 0
        && (*captures)[1].clipId == "clip" && (*captures)[1].iterationOrdinal == 1
        && (*captures)[2].clipId == "other" && (*captures)[2].iterationOrdinal == 0
        && (*captures)[0].capture != (*captures)[1].capture && (*captures)[0].capture != (*captures)[2].capture,
        "source analysis retains exact clip and repeat identity");
    const auto rendered = renderOffline(*compiled.graph, 24);
    require(rendered.ok() && rendered.mono.size() == 24, "offline public Score source analysis renders");
    for (std::size_t frame = 0; frame < 24; ++frame)
        require(rendered.mono[frame] == static_cast<double>(frame + 1) / 64 * (frame >= 8 && frame < 16 ? 2 : 1),
            "overlapping analysis sources retain independent live audio");
    for (std::size_t source = 0; source < captures->size(); ++source)
    {
        const auto& identity = (*captures)[source];
        require(identity.soundDefinitionId == "voice" && identity.sourceInstanceId == "voiceInstance",
            "public capture retains its Sound binding");
        const auto probe = identity.capture->probeIndexForClientId("public-source");
        require(probe.has_value(), "source trace client identity survives projection");
        std::array<double, 8> values{};
        const auto record = identity.capture->copyLatestRecordResult(*probe, values);
        const auto expectedPosition = source == 0 ? 8U : 0U;
        const auto inputPosition = source == 1 ? 16U : 8U;
        require(record.status == RealtimeAnalysisCopyStatus::Accepted && record.header.valueCount == 8
            && record.header.absoluteSamplePosition == expectedPosition && record.header.sequence == (source == 0 ? 2U : 1U),
            "public offline source records preserve independent positions and callback counts");
        for (std::size_t frame = 0; frame < values.size(); ++frame)
            require(values[frame] == static_cast<double>(inputPosition + frame + 1) / 64,
                "public offline source analysis captures the selected signal before Score mixing");
    }
    auto selected = configured;
    auto& target = selected.realtimeAnalysisTargets[0];
    target.instancePath.clear(); target.scoreClipId = "clip"; target.scoreIterationOrdinal = 1;
    auto selectedGraph = compileProject(document, registry(), selected);
    require(selectedGraph.ok(), "compile one exact clip repeat analysis target");
    const auto selectedCaptures = selectedGraph.graph->scoreSourceAnalysisCaptures();
    require(selectedCaptures && selectedCaptures->size() == 1 && selectedCaptures->front().clipId == "clip"
        && selectedCaptures->front().iterationOrdinal == 1, "exact repeat selector cannot observe its sibling");
    auto firstRepeat = selected;
    firstRepeat.realtimeAnalysisTargets[0].scoreIterationOrdinal = 0;
    auto firstRepeatGraph = compileProject(document, registry(), firstRepeat);
    auto otherClip = firstRepeat;
    otherClip.realtimeAnalysisTargets[0].scoreClipId = "other";
    auto otherClipGraph = compileProject(document, registry(), otherClip);
    require(firstRepeatGraph.ok() && otherClipGraph.ok()
        && snapshotPreparedExecutionTopologyFingerprint(*firstRepeatGraph.graph)
            != snapshotPreparedExecutionTopologyFingerprint(*selectedGraph.graph)
        && snapshotPreparedExecutionTopologyFingerprint(*firstRepeatGraph.graph)
            != snapshotPreparedExecutionTopologyFingerprint(*otherClipGraph.graph),
        "clip and repeat analysis selectors remain part of prepared stage authority");
    for (int invalid = 0; invalid < 4; ++invalid)
    {
        auto rejected = selected;
        auto& wrong = rejected.realtimeAnalysisTargets[0];
        if (invalid == 0) wrong.scoreClipId = "missing";
        if (invalid == 1) wrong.scoreIterationOrdinal = 9;
        if (invalid == 2) wrong.nodeId = "missing";
        if (invalid == 3) wrong.scoreClipId.reset();
        require(!compileProject(document, registry(), rejected).ok(),
            "invalid source analysis identity must reject instead of silently disappearing");
    }
    auto closed = fixture();
    auto closedSettings = settings(closed); closedSettings.enableRealtimeAnalysisCapture = true;
    closedSettings.realtimeAnalysisTargets = selected.realtimeAnalysisTargets;
    require(compileProject(closed, registry(), closedSettings).ok(),
        "exact clip-relative analysis also resolves a Sound without a root instance");
    talas::project::ProjectDocument standalone;
    standalone.graph.nodes = {{"input", "talas.audioInput", {}}, {"out", "talas.audioOutput", {}}};
    standalone.graph.connections = {{"input", "audioOut", "out", "audioIn"}};
    RenderSettings invalidStandalone;
    invalidStandalone.enableRealtimeAnalysisCapture = true;
    invalidStandalone.realtimeAnalysisTargets.push_back({{}, {}, "input", "audioOut",
        RealtimeAnalysisKind::Spectrum, RealtimeAnalysisPayloadKind::AudioBlock, 1, "invalid", "clip", 0});
    const auto rejectedStandalone = compileProject(standalone, registry(), invalidStandalone);
    require(!rejectedStandalone.ok() && !rejectedStandalone.diagnostics.empty()
        && rejectedStandalone.diagnostics.front().message == "Score analysis selectors require a live Score clip",
        "a standalone graph cannot silently ignore a Score selector");
}

void testPublicScoreSourceAnalysisLoopAndSeek()
{
    auto document = fixture(true);
    document.timeline.loop = {true, 0.002, 0.006};
    auto configured = settings(document);
    configured.enableRealtimeAnalysisCapture = true;
    configured.transportLoop = TransportLoopRegion{2, 6};
    configured.realtimeAnalysisTargets.push_back({{"voiceInstance"}, "voice", "gain", "audioOut",
        RealtimeAnalysisKind::Spectrum, RealtimeAnalysisPayloadKind::AudioBlock, 1, "loop-source"});
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage public source analysis through Score loops");
    auto captures = bridge.latestScoreSourceAnalysisCaptures();
    require(captures && captures->size() == 2 && bridge.latestAnalysisCaptureSampleRate() == 1000,
        "bridge exposes source analysis and its sample rate without a parent capture");
    auto capture = captures->front().capture;
    const auto stage = capture->boundStageId();
    require(stage != 0 && capture->bindingGeneration() == 1
        && captures->back().capture->boundStageId() == stage && captures->back().capture->bindingGeneration() == 1,
        "all repeat captures bind to the parent stage once across every loop owner");
    bridge.setPlaying(true);
    std::array<double, 8> values{};
    for (std::size_t block = 0; block < 3; ++block)
    {
        if (block == 2)
        {
            require(bridge.requestTransportRelocation(9), "prepare source analysis seek inside the loop");
            require(capture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord,
                "seek history preparation must not publish source analysis");
        }
        std::array<float, 8> input{}, output{};
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = static_cast<float>(block * 8 + frame + 1) / 64;
        const float* inputs[]{input.data()}; float* outputs[]{output.data()};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 1}, outputs, 1, input.size());
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0 && output == input,
            "public source analysis loops and seek adoption preserve allocation-free live input");
        const auto record = capture->copyLatestRecordResult(0, values);
        const auto count = block == 2 ? 3U : 2U;
        require(record.status == RealtimeAnalysisCopyStatus::Accepted && record.header.stageId == stage
            && record.header.bindingGeneration == 1 && record.header.absoluteSamplePosition == 2
            && record.header.valueCount == count, "loop analysis publishes the latest source segment with its stage");
        for (std::size_t frame = 0; frame < count; ++frame)
            require(values[frame] == input[input.size() - count + frame],
                "public loop analysis consumes new device samples after each source restart");
    }
    require(captures->back().capture->copyLatestRecordResult(0, values).status == RealtimeAnalysisCopyStatus::NoRecord,
        "an inactive repeat cannot inherit the active repeat's evidence");
    require(bridge.compileAndStage(configured).ok(), "replace the stage while source analysis is active");
    const auto replacement = bridge.latestScoreSourceAnalysisCaptures();
    require(replacement && replacement != captures && replacement->front().capture != capture
        && replacement->front().capture->boundStageId() != stage
        && replacement->front().capture->bindingGeneration() == 1,
        "replacement analysis retains a new exact stage identity");
}

void testPublicScoreLoopKeepsLiveInputAndExactSourcePhase()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections.push_back({"gain", "audioOut", "mix", "in1"});
    voice.graph.connections.push_back({"osc", "audioOut", "mix", "in2"});
    voice.outputs[0].nodeId = "mix";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.004); clip.repeatCount = 1;
    document.timeline.loop.enabled = true;
    document.timeline.loop.startSeconds = 0.002;
    document.timeline.loop.endSeconds = 0.006;
    auto configured = settings(document);
    configured.eventValues[0].value = 0.5;
    configured.transportLoop = TransportLoopRegion{2, 6};
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), "compile public live Score loop");

    std::array<float, 24> input{};
    for (std::size_t frame = 0; frame < input.size(); ++frame)
        input[frame] = static_cast<float>(static_cast<int>(frame % 7) - 3) / 8.0F;
    std::array<double, 24> reference{};
    for (const auto& blocks : {std::vector<std::size_t>{8, 8, 8},
             std::vector<std::size_t>{3, 5, 7, 1, 8}})
    {
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<double, 24> rendered{};
        std::size_t offset = 0;
        for (const auto count : blocks)
        {
            const float* inputs[] = {input.data() + offset};
            double* outputs[] = {rendered.data() + offset};
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto ok = session.render({inputs, 1}, outputs, 1, count);
            countAllocations = false;
            require(ok && allocations == 0 && deallocations == 0,
                "public Score loop renders each callback without allocation or destruction");
            offset += count;
        }
        require(offset == input.size(), "loop fixture covers every input frame");
        for (std::size_t frame = 0; frame < rendered.size(); ++frame)
        {
            const auto playhead = frame < 6 ? frame : 2 + (frame - 6) % 4;
            const auto expected = playhead < 2 ? 0.0
                : 0.5 * input[frame] + 0.125 * std::sin(
                    2 * std::numbers::pi * 7 * (playhead - 2) / 1000);
            require(std::abs(rendered[frame] - expected) < 1e-12,
                "public Score loop consumes fresh input and restarts source phase at exact sample");
            if (blocks[0] == 8) reference[frame] = rendered[frame];
            else require(rendered[frame] == reference[frame],
                "public Score loop is independent of callback partitioning");
        }
    }

    configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage public live Score loop");
    bridge.setPlaying(true);
    std::array<float, 24> staged{};
    for (std::size_t offset = 0; offset < staged.size(); offset += 8)
    {
        const float* inputs[] = {input.data() + offset};
        float* outputs[] = {staged.data() + offset};
        require(bridge.render({inputs, 1}, outputs, 1, 8),
            "staged public Score loop renders continuing device input");
    }
    for (std::size_t frame = 0; frame < staged.size(); ++frame)
        require(std::abs(staged[frame] - reference[frame]) < 1e-6,
            "staged public Score loop frame " + std::to_string(frame)
                + " expected " + std::to_string(reference[frame])
                + " actual " + std::to_string(staged[frame]));

    require(bridge.requestTransportRelocation(9), "prepare a public seek into the loop");
    std::array<float, 4> seekInput{0.125F, -0.25F, 0.375F, -0.5F}, seekOutput{};
    const float* seekInputs[] = {seekInput.data()};
    float* seekOutputs[] = {seekOutput.data()};
    require(bridge.render({seekInputs, 1}, seekOutputs, 1, seekOutput.size()),
        "public loop seek renders at its exact local composition position");
    require(bridge.transportFramePosition() == 5,
        "loop seek publishes the wrapped host playhead: "
            + std::to_string(bridge.transportFramePosition()));
    for (std::size_t frame = 0; frame < seekOutput.size(); ++frame)
    {
        const auto playhead = 2 + (9 + frame - 2) % 4;
        const auto expected = 0.5 * seekInput[frame] + 0.125 * std::sin(
            2 * std::numbers::pi * 7 * (playhead - 2) / 1000);
        require(std::abs(seekOutput[frame] - expected) < 1e-6,
            "public loop seek reconstructs source phase and reads fresh input");
    }
    std::array<float, 8> continuedInput{0.5F, 0.375F, 0.25F, 0.125F,
        -0.125F, -0.25F, -0.375F, -0.5F}, continuedOutput{};
    const float* continuedInputs[] = {continuedInput.data()};
    float* continuedOutputs[] = {continuedOutput.data()};
    require(bridge.render({continuedInputs, 1}, continuedOutputs, 1,
                continuedOutput.size()),
        "public loop seek keeps enough prepared owners for later returns");
    for (std::size_t frame = 0; frame < continuedOutput.size(); ++frame)
    {
        const auto playhead = 2 + (13 + frame - 2) % 4;
        const auto expected = 0.5 * continuedInput[frame] + 0.125 * std::sin(
            2 * std::numbers::pi * 7 * (playhead - 2) / 1000);
        require(std::abs(continuedOutput[frame] - expected) < 1e-6,
            "public loop seek continues through later wraps with exact source phase");
    }
}

void testPublicScoreLoopRestoresMidiSourceClocks()
{
    for (const bool script : {false, true})
    for (const bool mapper : {false, true})
    for (const bool waitInput : {false, true})
    for (const auto loopStart : {4U, 10U})
    {
        auto document = fixture();
        auto& sound = document.soundDefinitions[0];
        talas::project::ProjectParameterExpression source, file, program, shared, model;
        source.parameterId = "source"; source.text = script ? "Script" : "MIDI File";
        file.parameterId = "midiFile"; file.resource = script ? "" : "notes";
        program.parameterId = "script";
        program.text = script ? "self keyDownAt: 0 ms duration: 12 ms frequency: 60 nn. "
            "self controller: !cc18 setTo: 1 atTime: 6 ms." : "";
        shared.parameterId = "shared"; shared.boolean = false;
        model.parameterId = "voiceModel"; model.choice = "polyphonicInput";
        sound.graph.nodes = {
            {"osc", "talas.oscillator", {{"frequency", "100 hz + !cc18 * 100 hz"},
                {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
            {"midi", mapper ? "talas.midiMapper" : "talas.midiVoice", {source, file, program, shared,
                {"channel", "1"}, {"polyphony", "1"}, {"lowPitch", "0 nn"},
                {"highPitch", "127 nn"}, {"left", "1"}, {"right", "0.5"}}}};
        if (mapper) sound.graph.nodes[1].parameters.push_back(model);
        sound.graph.connections = {{"osc", "audioOut", "midi", "audioIn"}};
        sound.outputs = {{"audioOut", talas::core::PortType::Audio, "midi", "leftOut"}};
        if (waitInput)
        {
            sound.graph.nodes.push_back({"ready", "talas.constant", {{"value", "!KeyDown > 0"}}});
            sound.graph.nodes.push_back({"wait", "talas.waitUntil", {{"resume", "0"}}});
            sound.graph.connections = {{"osc", "audioOut", "wait", "audioIn"},
                {"ready", "value", "wait", "resume"}, {"wait", "audioOut", "midi", "audioIn"}};
        }
        auto& clip = document.timeline.score.clips[0];
        clip.start = seconds(0.002); clip.duration = seconds(0.020); clip.repeatCount = 1;
        const auto loopEnd = loopStart + 12;
        document.timeline.loop = {true, loopStart / 1000.0, loopEnd / 1000.0};
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "looped MIDI source project must survive persistence");
        document = reopened.document;
        auto configured = settings(document);
        configured.maxBlockFrames = 32;
        configured.transportLoop = TransportLoopRegion{loopStart, loopEnd};
        configured.eventValues.insert(configured.eventValues.end(), {{"cc18", 0},
            {"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}});
        configured.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
            'M','T','r','k',0,0,0,16, 0,0x90,60,127, 6,0xb0,18,127,
            6,0x80,60,0, 0,0xff,0x2f,0}}};
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), compiled.diagnostics.empty() ? "compile looped MIDI source"
            : compiled.diagnostics.front().message);
        if (waitInput)
        {
            auto isolated = prepareScoreClipSourceIteration(document, registry(), configured, 0, 0);
            require(isolated.ok(), "prepare the exact wrapped voice scope for forgery checks");
            auto& plan = const_cast<RuntimePlan&>(isolated.graph->plan());
            auto& wait = *std::ranges::find(plan.steps, "talas.waitUntil", &CompileStep::typeId);
            wait.parameters.front().variableScopeId = globalRuntimeScope;
            require(!RealtimeRenderSession(*isolated.graph).ready(),
                "a forged Resume scope cannot use the compiled voice's ancestor admission");
        }
        std::array<double, 20> sourceAudio{};
        double phase = 0;
        for (std::size_t frame = 0; frame < sourceAudio.size(); ++frame)
        {
            if (waitInput && frame < 5) continue;
            if (frame >= 5 && frame < 12) sourceAudio[frame] = std::sin(2 * std::numbers::pi * phase);
            phase = std::fmod(phase + (frame < 6 ? 0.1 : 0.2), 1.0);
        }
        const auto expectedAt = [&](std::size_t frame) {
            const auto playhead = frame < loopEnd ? frame : loopStart + (frame - loopEnd) % 12;
            return playhead < 2 ? 0.0 : sourceAudio[playhead - 2];
        };
        const auto offline = OfflineRenderSession(*compiled.graph).render(64);
        require(offline.ok(), offline.diagnostics.empty() ? "offline looped MIDI source"
            : offline.diagnostics.front().message);
        require(offline.mono.size() == 64, "offline MIDI loop must render the entire requested interval");
        for (std::size_t frame = 0; frame < offline.mono.size(); ++frame)
            require(std::abs(offline.mono[frame] - expectedAt(frame)) < 1e-11,
                "offline MIDI loop reconstructs note, CC, gate and phase at " + std::to_string(frame));
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(*compiled.graph);
            require(session.ready(), session.error());
            for (int pass = 0; pass < 2; ++pass)
            {
                std::array<double, 64> audio{};
                allocations = 0; deallocations = 0; countAllocations = true;
                bool rendered = true;
                for (std::size_t offset = 0; offset < audio.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, audio.size() - offset);
                    double* outputs[] = {audio.data() + offset};
                    rendered &= session.render({}, outputs, 1, count);
                }
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0,
                    "MIDI loop callbacks must not allocate or destroy source owners");
                for (std::size_t frame = 0; frame < audio.size(); ++frame)
                    require(std::abs(audio[frame] - expectedAt(frame)) < 1e-11,
                        "partitioned MIDI loop restores the exact source entry at " + std::to_string(frame));
                allocations = 0; deallocations = 0; countAllocations = true;
                session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, "MIDI loop reset must not allocate");
            }
        }
        configured.preparedScore.reset();
        ProjectRuntimeBridge bridge(document, registry());
        require(bridge.compileAndStage(configured).ok(), "stage the public MIDI Score loop");
        bridge.setPlaying(true);
        std::array<float, 32> audio{};
        float* output[] = {audio.data()};
        require(bridge.render(output, 1, audio.size()), "render staged MIDI Score loop");
        for (std::size_t frame = 0; frame < audio.size(); ++frame)
            require(std::abs(audio[frame] - expectedAt(frame)) < 1e-6, "staged MIDI loop matches its source clock");
        require(bridge.requestTransportRelocation(19), "prepare relocation inside a looping MIDI source");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto relocated = bridge.render(output, 1, audio.size());
        countAllocations = false;
        require(relocated && allocations == 0 && deallocations == 0,
            "MIDI source relocation and subsequent loop returns must not allocate");
        for (std::size_t frame = 0; frame < audio.size(); ++frame)
            require(std::abs(audio[frame] - expectedAt(frame + 19)) < 1e-6,
                "relocated MIDI loop preserves pending attack and controller history at " + std::to_string(frame));

        if (!script && !mapper)
        {
            auto failureDocument = document;
            auto failureSound = fixture().soundDefinitions.front();
            failureSound.definitionId = "failing";
            failureSound.graph.nodes[0].parameters = {{"gain", "1e308"}};
            failureDocument.soundDefinitions.push_back(std::move(failureSound));
            auto failureClip = failureDocument.timeline.score.clips.front();
            failureClip.clipId = "failing"; failureClip.soundDefinitionId = "failing";
            failureClip.start = seconds(0); failureClip.duration = seconds(0.1);
            failureDocument.timeline.score.clips.push_back(std::move(failureClip));
            auto failureSettings = settings(failureDocument);
            failureSettings.maxBlockFrames = configured.maxBlockFrames;
            failureSettings.transportLoop = configured.transportLoop;
            failureSettings.eventValues = configured.eventValues;
            failureSettings.binaryResources = configured.binaryResources;
            const auto failureGraph = compileProject(failureDocument, registry(), failureSettings);
            require(failureGraph.ok(), failureGraph.diagnostics.empty() ? "compile looped MIDI rollback"
                : failureGraph.diagnostics.front().message);
            RealtimeRenderSession failed(*failureGraph.graph), reference(*failureGraph.graph);
            require(failed.ready() && reference.ready(), "prepare MIDI loop rollback sessions");
            std::array<float, 32> input{};
            input.back() = 10;
            const float* inputs[] = {input.data()};
            std::array<double, 32> retry{}, fresh{};
            double* retryOutputs[] = {retry.data()};
            double* freshOutputs[] = {fresh.data()};
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto rendered = failed.render({inputs, 1}, retryOutputs, 1, retry.size());
            countAllocations = false;
            require(!rendered && allocations == 0 && deallocations == 0
                    && std::ranges::all_of(retry, [](double value) { return value == 0; }),
                "failure after multiple wraps restores MIDI source owners without allocation or destruction");
            input.back() = 0;
            require(reference.render({inputs, 1}, freshOutputs, 1, fresh.size()), "render corrected MIDI loop reference");
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto recovered = failed.render({inputs, 1}, retryOutputs, 1, retry.size());
            countAllocations = false;
            require(recovered && allocations == 0 && deallocations == 0,
                "corrected MIDI loop retry preserves complete source transactions");
            for (std::size_t frame = 0; frame < retry.size(); ++frame)
                require(std::abs(retry[frame] - expectedAt(frame)) < 1e-11 && retry[frame] == fresh[frame],
                    "MIDI loop retry restores note/control cursor, Wait latch and oscillator phase");
        }
    }
}

void testPublicScoreLoopRollsBackAfterPostWrapFailure()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}}};
    voice.graph.connections.clear(); voice.outputs[0].nodeId = "osc";
    auto failing = fixture().soundDefinitions[0];
    failing.definitionId = "failing";
    failing.graph.nodes[0].parameters = {{"gain", "1e308"}};
    document.soundDefinitions.push_back(std::move(failing));
    auto clip = document.timeline.score.clips[0];
    clip.clipId = "failing"; clip.soundDefinitionId = "failing";
    clip.start = seconds(0.002); clip.duration = seconds(0.004); clip.repeatCount = 1;
    document.timeline.score.clips.push_back(std::move(clip));
    auto configured = settings(document);
    configured.transportLoop = TransportLoopRegion{2, 4};
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), "compile post-wrap source failure fixture");
    RealtimeRenderSession failedSession(*compiled.graph), freshSession(*compiled.graph);
    require(failedSession.ready() && freshSession.ready(), "prepare loop rollback source owners");

    std::array<float, 8> input{};
    input.back() = 10;
    const float* inputs[] = {input.data()};
    std::array<double, 8> retried{}, fresh{};
    retried.fill(19);
    double* retryOutput[] = {retried.data()};
    double* freshOutput[] = {fresh.data()};
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto failed = !failedSession.render({inputs, 1}, retryOutput, 1, 8);
    countAllocations = false;
    require(failed && allocations == 0 && deallocations == 0
            && std::ranges::all_of(retried, [](double value) { return value == 0; }),
        "post-wrap failure rejects and clears the entire public callback without allocation");
    input.back() = 0;
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto retriedOk = failedSession.render({inputs, 1}, retryOutput, 1, 8);
    const auto freshOk = freshSession.render({inputs, 1}, freshOutput, 1, 8);
    countAllocations = false;
    require(retriedOk && freshOk && allocations == 0 && deallocations == 0,
        "retry and fresh loop callbacks both render without allocation");
    require(retried == fresh, "post-wrap failure restores the exact original source owners and score cursor");
}

void testPublicScoreLoopSeekRetainsInteriorEntryOwnerPool()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections.push_back({"gain", "audioOut", "mix", "in1"});
    voice.graph.connections.push_back({"osc", "audioOut", "mix", "in2"});
    voice.outputs[0].nodeId = "mix";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.008); clip.repeatCount = 1;
    document.timeline.loop.enabled = true;
    document.timeline.loop.startSeconds = 0.003;
    document.timeline.loop.endSeconds = 0.007;
    auto configured = settings(document);
    configured.preparedScore.reset();
    configured.eventValues[0].value = 0.5;
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage interior-entry loop fixture");
    bridge.setPlaying(true);
    std::array<float, 8> zeros{}, output{};
    const float* firstInputs[] = {zeros.data()};
    float* outputs[] = {output.data()};
    require(bridge.render({firstInputs, 1}, outputs, 1, output.size()),
        "render before interior-entry loop seek");
    require(bridge.requestTransportRelocation(12), "prepare interior loop seek");
    std::array<float, 8> input{0.125F, -0.25F, 0.375F, -0.5F,
        0.5F, -0.375F, 0.25F, -0.125F};
    const float* inputs[] = {input.data()};
    for (std::uint64_t start : {12U, 20U})
    {
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 1}, outputs, 1, output.size());
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0,
            "interior-entry seek and later wraps use prepared owners without allocation");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
        {
            const auto playhead = 3 + (start + frame - 3) % 4;
            const auto expected = 0.5 * input[frame] + 0.125 * std::sin(
                2 * std::numbers::pi * 7 * (playhead - 2) / 1000);
            require(std::abs(output[frame] - expected) < 1e-6,
                "interior-entry seek retains exact source age through repeated returns");
        }
    }
}

void testPublicScoreLoopRestartsStreamAtEachReturn()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = "fileName"; resource.resource = "source";
    voice.graph.nodes = {{"disk", "talas.diskPlayer", {resource, {"duration", "40 s"}}}};
    voice.graph.connections.clear(); voice.outputs[0].nodeId = "disk";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.004); clip.repeatCount = 1;
    document.timeline.loop.enabled = true;
    document.timeline.loop.startSeconds = 0.002;
    document.timeline.loop.endSeconds = 0.006;
    const auto path = std::filesystem::temp_directory_path() / ("talas-score-loop-stream-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup()
        { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    std::vector<std::vector<double>> samples{std::vector<double>(64)};
    for (std::size_t frame = 0; frame < samples[0].size(); ++frame)
        samples[0][frame] = (static_cast<int>(frame % 11) - 5) / 32.0;
    std::string error;
    require(talas::audio::writePcm16Wav(path, 1000, samples, error), error);
    auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "hash loop stream WAV");
    talas::project::ProjectAsset asset;
    asset.id = "source"; asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = path.string(); asset.contentHash = hash.contentHash;
    asset.channelCount = 1; asset.sampleRate = 1000; asset.frameCount = 64;
    asset.encoding = talas::project::pcm16WavAssetEncoding; asset.generation = 1;
    document.assets.push_back(std::move(asset));
    auto preparedSettings = prepareScoreClipMaterializationSettings(
        document, registry(), std::nullopt, "clip", 1000, 8);
    require(preparedSettings.ok(), preparedSettings.error);
    auto configured = *preparedSettings.settings;
    configured.transportLoop = TransportLoopRegion{2, 6};
    auto compiled = compileProject(document, registry(), configured);
    require(compiled.ok(), "compile public loop stream");
    RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 24> output{};
    for (std::size_t offset = 0; offset < output.size(); offset += 8)
    {
        double* outputs[] = {output.data() + offset};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = session.render({}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0,
            "streamed Score loop renders without callback allocation or destruction");
    }
    require(output[2] != 0 && output[2] != output[3],
        "loop stream has a distinguishing nonzero source prefix");
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto expected = frame < 2 ? 0.0 : output[2 + (frame - 2) % 4];
        require(output[frame] == expected,
            "streamed source restores its exact file position at every loop return");
    }
    configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage public loop stream");
    bridge.setPlaying(true);
    std::array<float, 24> staged{};
    for (std::size_t offset = 0; offset < staged.size(); offset += 8)
    {
        float* outputs[] = {staged.data() + offset};
        require(bridge.render(outputs, 1, 8), "staged streamed Score loop renders");
    }
    for (std::size_t frame = 0; frame < staged.size(); ++frame)
        require(std::abs(staged[frame] - output[frame]) < 1e-6,
            "staged Score stream follows the same looped source positions");
}

void testPublicScoreReplacementPreparesActiveRepeat()
{
    auto document = fixture(true);
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes[0].parameters = {{"gain", "0.5"}};
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections = {{"gain", "audioOut", "mix", "in1"}, {"osc", "audioOut", "mix", "in2"}};
    voice.outputs[0].nodeId = "mix";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.012); clip.contentOffset = seconds(0.002);
    auto configured = settings(document); configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage the initial non-looping Score source");
    bridge.setPlaying(true);
    std::array<float, 8> input{}, output{};
    input.fill(0.5F); const float* inputs[]{input.data()}; float* outputs[]{output.data()};
    require(bridge.render({inputs, 1}, outputs, 1, 8) && bridge.render({inputs, 1}, outputs, 1, 8),
        "enter the second repeat before source replacement");
    auto replacement = document;
    replacement.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.25"}};
    require(bridge.compileAndStageDocument(replacement, configured).ok(), "prepare active non-looping source history");
    for (std::size_t frame = 0; frame < input.size(); ++frame) input[frame] = static_cast<float>(frame + 1) / 32;
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto rendered = bridge.render({inputs, 1}, outputs, 1, input.size());
    countAllocations = false;
    require(rendered && allocations == 0 && deallocations == 0,
        "non-looping replacement adopts its active repeat without callback allocation");
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto expected = 0.25 * input[frame]
            + 0.125 * std::sin(2 * std::numbers::pi * 7 * (frame + 4) / 1000);
        require(std::abs(output[frame] - expected) < 1e-7,
            "replacement retains content offset, repeat phase and current device input");
    }
}

void testPublicScoreReplacementRejectsMissedBoundary()
{
    auto document = fixture(true);
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes[0].parameters = {{"gain", "0.5"}};
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections = {{"gain", "audioOut", "mix", "in1"}, {"osc", "audioOut", "mix", "in2"}};
    voice.outputs[0].nodeId = "mix";
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.012); clip.contentOffset = seconds(0.002);
    clip.repeatCount = 4;
    auto configured = settings(document); configured.preparedScore.reset();
    configured.enableRealtimeAnalysisCapture = true;
    RealtimeAnalysisTargetSpec target;
    target.instancePath = {"voiceInstance"}; target.containingDefinitionId = "voice";
    target.nodeId = "gain"; target.clientId = "stage-boundary";
    configured.realtimeAnalysisTargets = {target};
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage missed-boundary Score fixture");
    bridge.setPlaying(true);
    std::array<float, 8> input{}, output{};
    input.fill(0.5F); const float* inputs[]{input.data()}; float* outputs[]{output.data()};
    require(bridge.render({inputs, 1}, outputs, 1, 8) && bridge.render({inputs, 1}, outputs, 1, 8),
        "enter an active repeat before staging across a frozen audio boundary");
    const auto originalStage = bridge.activeStageId();
    (void)bridge.realtimeMetrics(); // Establish the active metrics epoch before rejection.
    auto replacement = document;
    replacement.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.25"}};

    // The callback can pin its old target before the control thread publishes
    // the candidate, then advance beyond that candidate's prepared source history.
    ProjectRuntimeBridgeTestAccess::prepareNextRenderBoundary(bridge);
    require(bridge.compileAndStageDocument(replacement, configured).ok(),
        "publish a Score replacement after the current callback target is frozen");
    const auto candidateStatus = bridge.preparedGraphStatus();
    require(candidateStatus.awaitingActivation && !candidateStatus.activationRejected,
        "a newly published candidate reports pending activation");
    const auto candidateCaptures = bridge.latestScoreSourceAnalysisCaptures();
    require(candidateCaptures && candidateCaptures->size() == 4,
        "pending source captures remain observable until a candidate is rejected");
    const auto checkBlock = [&](std::uint64_t absoluteFrame, double gain) {
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = static_cast<float>(frame + absoluteFrame + 1) / 128;
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 1}, outputs, 1, input.size());
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0,
            "missed-boundary rendering must retain valid source history without callback heap activity");
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            const auto sourceFrame = (absoluteFrame + frame - 2) % 12 + 2;
            const auto expected = gain * input[frame]
                + 0.125 * std::sin(2 * std::numbers::pi * 7 * sourceFrame / 1000);
            require(std::abs(output[frame] - expected) < 1e-7,
                "missed-boundary audio preserves repeat phase, content offset and current input");
        }
    };
    checkBlock(16, 0.5);
    require(bridge.activeStageId() == originalStage, "the frozen callback keeps its original target");
    checkBlock(24, 0.5);
    require(bridge.activeStageId() == originalStage,
        "a missed non-looping source history must never replace the valid active graph");
    const auto rejectedStatus = bridge.preparedGraphStatus();
    require(rejectedStatus.activationRejected && !rejectedStatus.awaitingActivation
            && rejectedStatus.prepared == candidateStatus.prepared
            && rejectedStatus.activeStageId == originalStage,
        "a rejected candidate reports failure rather than waiting forever or claiming active authority");
    require(!bridge.latestScoreSourceAnalysisCaptures() && !bridge.latestAnalysisCapture()
            && bridge.latestAnalysisCaptureSampleRate() == 0,
        "analysis accessors do not advertise captures for a rejected replacement");
    std::array<double, 8> values{};
    for (const auto& source : *candidateCaptures)
        require(source.capture->copyLatestRecordResult(0, values).status != RealtimeAnalysisCopyStatus::Accepted,
            "rejected source captures never publish audio evidence");
    require(bridge.realtimeMetrics().stageRejections == 1,
        "missed prepared source history is observable in rejection telemetry");
    require(bridge.collectRetired(), "the rejected candidate is retired on the control thread");
    require(bridge.compileAndStageDocument(replacement, configured).ok(),
        "a missed source boundary releases the pending slot for an explicit retry");
    checkBlock(32, 0.25);
    require(bridge.activeStageId() != originalStage,
        "a fresh candidate becomes audible at its matching prepared boundary");
    const auto activeStatus = bridge.preparedGraphStatus();
    require(!activeStatus.activationRejected && !activeStatus.awaitingActivation
            && activeStatus.prepared->stageId == activeStatus.activeStageId
            && bridge.latestScoreSourceAnalysisCaptures() != nullptr,
        "retry restores exact active status and capture availability");

    bridge.setPlaying(false);
    ProjectRuntimeBridgeTestAccess::prepareNextRenderBoundary(bridge);
    require(bridge.compileAndStageDocument(replacement, configured).ok(),
        "prepare another replacement while transport is paused");
    allocations = 0; deallocations = 0; countAllocations = true;
    (void)bridge.render({inputs, 1}, outputs, 1, input.size());
    (void)bridge.render({inputs, 1}, outputs, 1, input.size());
    countAllocations = false;
    const auto pausedStatus = bridge.preparedGraphStatus();
    require(allocations == 0 && deallocations == 0
            && !pausedStatus.awaitingActivation && !pausedStatus.activationRejected
            && pausedStatus.activeStageId != activeStatus.activeStageId
            && bridge.musicalTransportSnapshot().transport.absoluteFrame == 40,
        "a frozen paused callback does not invalidate a candidate when its prepared frame remains current");
}

void testPublicScoreLoopKeepsActiveClipAcrossStageReplacement()
{
    auto document = fixture();
    document.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.5"}};
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.008); clip.repeatCount = 1;
    document.timeline.loop.enabled = true;
    document.timeline.loop.startSeconds = 0.002;
    document.timeline.loop.endSeconds = 0.006;
    auto configured = settings(document);
    configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage first active loop clip");
    bridge.setPlaying(true);
    std::array<float, 8> input{}, output{};
    input.fill(1);
    const float* inputs[] = {input.data()};
    float* outputs[] = {output.data()};
    require(bridge.render({inputs, 1}, outputs, 1, output.size()),
        "render first loop stage through its return");
    for (std::size_t frame = 0; frame < output.size(); ++frame)
        require(std::abs(output[frame] - (frame < 2 ? 0.0F : 0.5F)) < 1e-6F,
            "first loop stage has the expected live clip gain");

    auto replacement = document;
    replacement.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.25"}};
    require(bridge.compileAndStageDocument(replacement, configured).ok(),
        "stage replacement while loop playhead is inside the clip");
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto rendered = bridge.render({inputs, 1}, outputs, 1, output.size());
    countAllocations = false;
    require(rendered && allocations == 0 && deallocations == 0,
        "replacement loop stage adopts and renders without callback allocation: rendered="
            + std::to_string(rendered) + " allocations=" + std::to_string(allocations)
            + " deallocations=" + std::to_string(deallocations));
    for (float sample : output)
        require(std::abs(sample - 0.25F) < 1e-6F,
            "replacement loop stage continues its active clip at the host playhead");
}

void testLoopStageWaitsForItsPreparedLocalPhase()
{
    auto document = fixture();
    document.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.5"}};
    auto& clip = document.timeline.score.clips[0];
    clip.start = seconds(0.002); clip.duration = seconds(0.008); clip.repeatCount = 1;
    auto configured = settings(document);
    configured.transportLoop = TransportLoopRegion{2, 6};
    auto firstGraph = compileProject(document, registry(), configured);
    require(firstGraph.ok(), "compile first host loop stage");
    auto first = std::make_unique<RealtimeAudioEngine>(*firstGraph.graph);
    require(first->ready(), first->error());
    first->setStageId(1);
    RealtimeGraphHost host;
    require(host.tryStage(std::move(first)) == RealtimeGraphStageResult::Accepted,
        "queue first host loop stage");
    host.setPlaying(true);
    std::array<float, 8> input{}, output{};
    input.fill(1);
    const float* inputs[] = {input.data()};
    float* outputs[] = {output.data()};
    require(host.render({inputs, 1}, outputs, 1, 8)
            && host.activeStageId() == 1,
        "first loop stage reaches a playhead inside its active clip");

    auto replacement = document;
    replacement.soundDefinitions[0].graph.nodes[0].parameters = {{"gain", "0.25"}};
    auto replacementSettings = settings(replacement);
    replacementSettings.transportLoop = TransportLoopRegion{2, 6};
    auto replacementGraph = compileProject(replacement, registry(), replacementSettings);
    require(replacementGraph.ok(), "compile replacement host loop stage");
    auto second = std::make_unique<RealtimeAudioEngine>(*replacementGraph.graph);
    require(second->ready(), second->error());
    second->setStageId(2);
    std::uint64_t budget = 1000000;
    std::string error;
    auto point = second->prepareTransportRelocation(9, budget, error);
    require(point && second->installTransportRelocation(*point),
        "prepare replacement for a later local loop phase");
    second->setPreparedStageTransportFrame(9);
    require(host.tryStage(std::move(second)) == RealtimeGraphStageResult::Accepted,
        "queue later-phase loop stage");

    allocations = 0; deallocations = 0; countAllocations = true;
    const auto keptOld = host.render({inputs, 1}, outputs, 1, 1);
    const auto oldSample = output[0];
    const auto oldStage = host.activeStageId();
    const auto adoptedNew = host.render({inputs, 1}, outputs, 1, 1);
    const auto newSample = output[0];
    const auto newStage = host.activeStageId();
    countAllocations = false;
    require(keptOld && adoptedNew && allocations == 0 && deallocations == 0,
        "phase-gated stage keeps rendering on the audio thread without allocation");
    require(oldStage == 1 && std::abs(oldSample - 0.5F) < 1e-6F
            && newStage == 2 && std::abs(newSample - 0.25F) < 1e-6F,
        "old stage plays until the prepared local phase, then new stage becomes audible");
}


void testPreparedParentTransportRetainsSourcePhaseAndInput()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"mix", "talas.mixer", {}});
    voice.graph.connections.push_back({"gain", "audioOut", "mix", "in1"});
    voice.graph.connections.push_back({"osc", "audioOut", "mix", "in2"});
    voice.outputs[0].nodeId = "mix";
    document.timeline.score.clips[0].contentOffset = seconds(0.002);
    for (const auto target : {5U, 19U})
    {
        auto configured = settings(document);
        const auto compiled = compileProject(document, registry(), configured);
        require(compiled.ok(), "prepare parent transport fixture");
        RealtimeRenderSession session(*compiled.graph);
        std::string error;
        require(session.prepareTransportFrameBeforeFirstRender(target, error), error);
        require(!session.prepareTransportFrameBeforeFirstRender(target + 1, error), "one unpublished candidate retains its exact prepared target");
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            std::array<float, 4> input{0.125f, -0.25f, 0.375f, -0.5f};
            const float* inputs[] = {input.data()};
            std::array<double, 4> audio{}; double* outputs[] = {audio.data()};
            allocations = 0; countAllocations = true;
            if (attempt) session.resetTransport();
            session.setTransportFrameBeforeFirstRender(target);
            const auto rendered = session.render({inputs, 1}, outputs, 1, 4);
            countAllocations = false;
            require(rendered && allocations == 0, "prepared parent seek adopts DSP with no callback replay or allocation");
            for (std::size_t f = 0; f < audio.size(); ++f)
            {
                const auto age = 2 + target % 16 + f;
                const auto expected = double(input[f]) + 0.125 * std::sin(2 * std::acos(-1.0) * 7 * age / 1000);
                require(std::abs(audio[f] - expected) < 1e-12, "prepared source phase follows target age and current device input stays live");
            }
            require(session.preparedScorePlaybackFrame() == target + 4, "parent and child adopt the same prepared composition frame");
        }
        require(!session.prepareTransportFrameBeforeFirstRender(target, error), "published/rendered source preparation is rejected");
    }
}

void testPreparedParentStreamTransport()
{
    for (const auto offset : {0, 2048, 17000}) checkParentStreamRollback(offset, false, true);
    checkParentStreamRollback(0, true, true);
}
void testPreparedParentTransportHistoryAndLateFailure()
{
    auto document = fixture();
    talas::project::ProjectParameterExpression coefficients;
    coefficients.parameterId = "coefficients"; coefficients.text = "0.5 0.25";
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes = {{"device", "talas.audioInput", {}}, {"fir", "talas.firFilter", {coefficients}},
        {"delay", "talas.delayWithFeedback", {{"delay", "2 ms"}, {"feedback", "0.25"}, {"mix", "1"}}}};
    voice.graph.connections = {{"device", "audioOut", "fir", "audioIn"}, {"fir", "audioOut", "delay", "audioIn"}};
    voice.outputs[0].nodeId = "delay";
    auto failing = fixture().soundDefinitions[0]; failing.definitionId = "failing";
    failing.graph.nodes[0].parameters = {{"gain", "1e308"}};
    talas::project::ProjectParameterExpression ch1, ch2;
    ch1.parameterId = "channel1"; ch1.boolean = false; ch2.parameterId = "channel2"; ch2.boolean = true;
    failing.graph.nodes[1].parameters = {ch1, ch2};
    document.soundDefinitions.push_back(std::move(failing));
    auto failureClip = document.timeline.score.clips[0]; failureClip.clipId = "failure"; failureClip.soundDefinitionId = "failing";
    document.timeline.score.clips.push_back(failureClip);
    auto configured = settings(document);
    for (std::size_t f = 0; f < configured.audioInputChannels[0].samples.size(); ++f)
        configured.audioInputChannels[0].samples[f] = double((f * 3) % 7) / 8;
    auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), "history seek fixture");
    RealtimeRenderSession session(*compiled.graph); std::string error;
    require(session.prepareTransportFrameBeforeFirstRender(6, error), error);
    std::array<float, 8> input{0.25f, -0.5f, 0.125f, 0.625f, -0.125f, 0.5f, 0.375f, -0.25f}, failureInput{};
    std::array<double, 14> history{}, expected{};
    for (std::size_t f = 0; f < history.size(); ++f)
        history[f] = f < 6 ? configured.audioInputChannels[0].samples[f] : double(input[f - 6]);
    for (std::size_t f = 2; f < expected.size(); ++f)
        expected[f] = 0.5 * history[f - 2] + (f > 2 ? 0.25 * history[f - 3] : 0) + 0.25 * expected[f - 2];
    const float* inputs[] = {input.data(), failureInput.data()};
    std::array<double, 8> audio{}; double* outputs[] = {audio.data()};
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        allocations = 0; countAllocations = true;
        if (attempt) session.resetTransport();
        session.setTransportFrameBeforeFirstRender(6);
        failureInput.back() = 10; audio.fill(19);
        const auto failed = !session.render({inputs, 2}, outputs, 1, 8);
        countAllocations = false;
        require(failed && allocations == 0 && std::ranges::all_of(audio, [](double v) { return v == 0; }), "prepared seek callback failure is atomic");
        require(session.preparedScorePlaybackFrame() == 6, "failed callback retains adopted composition position");
        failureInput.back() = 0;
        allocations = 0; countAllocations = true;
        const auto rendered = session.render({inputs, 2}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0, "prepared FIR/feedback history retry is allocation free");
        for (std::size_t f = 0; f < audio.size(); ++f)
            require(std::abs(audio[f] - expected[f + 6]) < 1e-12, "prepared seek restores full live-processing history before current input");
    }
}
void testFailedTransportPreparationPreservesInitialSources()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}}};
    voice.graph.connections.clear(); voice.outputs[0].nodeId = "osc";
    auto failing = fixture().soundDefinitions[0]; failing.definitionId = "failing";
    failing.graph.nodes[0].parameters = {{"gain", "1e308"}};
    document.soundDefinitions.push_back(std::move(failing));
    auto clip = document.timeline.score.clips[0]; clip.clipId = "failure"; clip.soundDefinitionId = "failing";
    document.timeline.score.clips.push_back(clip);
    auto configured = settings(document); configured.audioInputChannels[0].samples[5] = 10;
    auto compiled = compileProject(document, registry(), configured); require(compiled.ok(), "failed preparation fixture");
    RealtimeRenderSession session(*compiled.graph); std::string error;
    require(!session.prepareTransportFrameBeforeFirstRender(6, error), "later source preparation must reject overflowing history");
    require(session.prepareTransportFrameBeforeFirstRender(3, error), error);
    std::array<float, 8> input{}; const float* inputs[] = {input.data()};
    std::array<double, 8> audio{}; double* outputs[] = {audio.data()};
    for (const auto start : {0U, 3U})
    {
        allocations = 0; countAllocations = true;
        session.resetTransport(); session.setTransportFrameBeforeFirstRender(start);
        const auto rendered = session.render({inputs, 1}, outputs, 1, 8);
        countAllocations = false;
        require(rendered && allocations == 0, "failed candidate preserves default reset and permits a later prepared target");
        for (std::size_t f = 0; f < audio.size(); ++f)
            require(std::abs(audio[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * (start + f) / 1000)) < 1e-12,
                "failed preparation cannot advance an earlier source's phase");
    }
}
void testTransportPreparationBudgetRejectsBeforePreroll()
{
    auto document = fixture(); document.timeline.score.clips[0].duration = seconds(2000);
    auto compiled = compileProject(document, registry(), settings(document)); require(compiled.ok(), "long live clip preparation");
    RealtimeRenderSession session(*compiled.graph); std::string error;
    require(!session.prepareTransportFrameBeforeFirstRender(1000001, error) && error.find("budget") != std::string::npos,
        "distant transport preparation must obey the aggregate work limit");
    require(session.prepareTransportFrameBeforeFirstRender(3, error), error);
}

}
void testPublicBridgeLiveTransportReconstruction()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}});
    voice.graph.nodes.push_back({"sum", "talas.mixer", {}});
    voice.graph.connections.push_back({"gain", "audioOut", "sum", "in1"});
    voice.graph.connections.push_back({"osc", "audioOut", "sum", "in2"});
    voice.outputs[0].nodeId = "sum";
    document.timeline.score.clips[0].contentOffset = seconds(0.002);
    auto configured = settings(document); configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage public live transport fixture");
    bridge.setPlaying(true);
    std::array<float, 4> input{0.1F, -0.2F, 0.3F, -0.4F}, output{};
    const float* inputs[] = {input.data()}; float* outputs[] = {output.data()};
    require(bridge.render({inputs, 1}, outputs, 1, 4), "initial live bridge callback");
    for (const auto target : {5U, 19U, 3U, 16U, 31U, 32U})
    {
        const auto before = bridge.transportFramePosition();
        require(bridge.requestTransportRelocation(target + 1), "prepare superseded public seek");
        require(bridge.requestTransportRelocation(target), "public live seek request prepares");
        require(bridge.transportFramePosition() == before, "control preparation leaves playing cursor unchanged");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = bridge.render({inputs, 1}, outputs, 1, 4);
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0,
            "public live seek adopts and renders without allocation or destruction");
        require(bridge.transportFramePosition() == target + 4, "public live seek publishes exact frame");
        for (std::size_t f = 0; f < output.size(); ++f)
        {
            const auto frame = target + f;
            const auto expected = frame < 32
                ? input[f] + 0.125 * std::sin(2 * std::numbers::pi * 7 * (2 + frame % 16) / 1000) : 0;
            require(std::abs(output[f] - expected) < 1e-6, "public seek reconstructs phase and uses fresh device input");
        }
        (void) bridge.collectRetired();
    }
}


talas::project::ProjectDocument publicSeekOscillatorFixture()
{
    auto document = fixture();
    auto& voice = document.soundDefinitions[0];
    voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.125"}}}};
    voice.graph.connections.clear(); voice.outputs[0].nodeId = "osc";
    return document;
}

void testDetachedTransportCandidateLifecycle()
{
    auto document = publicSeekOscillatorFixture();
    auto compiled = compileProject(document, registry(), settings(document));
    require(compiled.ok(), "candidate lifecycle compile");
    RealtimeRenderSession session(*compiled.graph), foreign(*compiled.graph);
    std::array<double, 4> output{}; double* outputs[] = {output.data()};
    require(session.render({}, outputs, 1, 4), "render before detached preparation");
    std::uint64_t budget = 1000000; std::string error;
    auto candidate = session.prepareTransportRelocation(19, budget, error);
    require(candidate != nullptr && budget == 999997, "detached candidate charges exact active source history");
    require(!foreign.canInstallTransportRelocation(*candidate)
        && !foreign.installTransportRelocation(*candidate), "identical foreign session cannot consume source ownership");
    require(session.render({}, outputs, 1, 4), "existing source continues during preparation");
    for (std::size_t f = 0; f < 4; ++f)
        require(std::abs(output[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * (4 + f) / 1000)) < 1e-12,
            "detached preparation leaves active source phase untouched");
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto installed = session.installTransportRelocation(*candidate);
    session.resetTransport(); session.setTransportFrameBeforeFirstRender(19);
    const auto rendered = session.render({}, outputs, 1, 4);
    const auto reused = session.installTransportRelocation(*candidate);
    countAllocations = false;
    require(installed && rendered && !reused && allocations == 0 && deallocations == 0,
        "one-shot installation retains old owners without realtime destruction");
    for (std::size_t f = 0; f < 4; ++f)
        require(std::abs(output[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * (3 + f) / 1000)) < 1e-12,
            "detached candidate keeps exact repeat origin");
    std::optional<RealtimeRenderSession> retired(std::in_place, *compiled.graph);
    budget = 1000000;
    auto oldPoint = retired->prepareTransportRelocation(5, budget, error);
    require(oldPoint != nullptr, error);
    retired.reset(); retired.emplace(*compiled.graph);
    require(!retired->installTransportRelocation(*oldPoint), "retired session address cannot authorize an old candidate");
}

void testPublicSeekFailedPreparationPreservesPendingRequest()
{
    auto document = publicSeekOscillatorFixture();
    auto failing = fixture().soundDefinitions[0]; failing.definitionId = "failing";
    failing.graph.nodes[0].parameters = {{"gain", "1e308"}};
    document.soundDefinitions.push_back(std::move(failing));
    auto clip = document.timeline.score.clips[0]; clip.clipId = "failure"; clip.soundDefinitionId = "failing";
    document.timeline.score.clips.push_back(clip);
    auto configured = settings(document); configured.preparedScore.reset();
    configured.audioInputChannels[0].samples[5] = 10;
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage failed public preparation");
    bridge.setPlaying(true);
    std::array<float, 4> input{}, output{}; const float* inputs[] = {input.data()}; float* outputs[] = {output.data()};
    require(bridge.render({inputs, 1}, outputs, 1, 4), "initial callback before failed request");
    require(bridge.requestTransportRelocation(3), "valid pending source candidate");
    require(!bridge.requestTransportRelocation(6), "later source history overflow rejects candidate");
    require(bridge.transportFramePosition() == 4, "failed preparation cannot move active transport");
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto rendered = bridge.render({inputs, 1}, outputs, 1, 4);
    countAllocations = false;
    require(rendered && allocations == 0 && deallocations == 0 && bridge.transportFramePosition() == 7,
        "failed request preserves the earlier prepared request and realtime retirement");
    for (std::size_t f = 0; f < 4; ++f)
        require(std::abs(output[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * (3 + f) / 1000)) < 1e-6,
            "failed request cannot change the earlier candidate's source history");
}

void testPublicSeekRejectsStageAuthorityAndStopChanges()
{
    for (int change = 0; change < 3; ++change)
    {
        auto document = publicSeekOscillatorFixture();
        auto configured = settings(document); configured.preparedScore.reset();
        ProjectRuntimeBridge bridge(document, registry());
        require(bridge.compileAndStage(configured).ok(), "stage stale seek fixture");
        bridge.setPlaying(true);
        std::array<float, 4> output{}; float* outputs[] = {output.data()};
        require(bridge.render(outputs, 1, 4), "initial stale seek callback");
        require(bridge.requestTransportRelocation(19), "prepare request before invalidation");
        if (change == 0)
        {
            require(bridge.compileAndStage(configured).ok(), "replace stage while seek is pending");
            // A replacement still needs its own prepared source state at the
            // preserved host frame; the stale request must not supply that state.
            (void) bridge.render(outputs, 1, 4);
            require(bridge.transportFramePosition() != 23, "old-stage target cannot relocate replacement sources");
            require(bridge.requestTransportRelocation(3), "new stage has independently prepared seek");
        }
        else if (change == 1)
        {
            bridge.configureMtcAuthority(9, 1, 1); bridge.configureMtcAuthority(0, 0, 0);
            (void) bridge.render(outputs, 1, 4);
            require(bridge.transportFramePosition() != 23, "external authority round trip revokes prepared target");
            require(bridge.requestTransportRelocation(3), "internal seek after authority round trip");
        }
        else
        {
            bridge.stopTransport();
            (void) bridge.render(outputs, 1, 4);
            require(bridge.transportFramePosition() == 0, "stop revokes pending seek and resets to zero");
            require(bridge.requestTransportRelocation(3), "seek remains reusable after stop");
        }
        bridge.setPlaying(true);
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = bridge.render(outputs, 1, 4);
        countAllocations = false;
        require(rendered && allocations == 0 && deallocations == 0 && bridge.transportFramePosition() == 7,
            "fresh candidate works after rejected authority with bounded realtime adoption");
        for (std::size_t f = 0; f < 4; ++f)
            require(std::abs(output[f] - 0.125 * std::sin(2 * std::numbers::pi * 7 * (3 + f) / 1000)) < 1e-6,
                "new authority owns exact reconstructed phase");
    }
}

void testPublicSeekPreparationWhileAudioRenders()
{
    auto document = publicSeekOscillatorFixture();
    document.timeline.score.clips[0].duration = seconds(50);
    document.timeline.score.clips[0].repeatCount = 1;
    auto configured = settings(document); configured.preparedScore.reset();
    ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(configured).ok(), "stage concurrent source preparation");
    bridge.setPlaying(true);
    std::atomic<bool> healthy = true;
    std::atomic<std::uint64_t> callbacks = 0;
    std::jthread audio([&](std::stop_token stop) {
        float sample = 0; float* output[] = {&sample};
        while (!stop.stop_requested())
        {
            const auto rendered = bridge.render(output, 1, 1);
            const auto after = bridge.transportFramePosition();
            const auto frame = after ? after - 1 : 0;
            const auto expected = frame < 50000
                ? 0.125 * std::sin(2 * std::numbers::pi * 7 * frame / 1000) : 0;
            if (!rendered || !after || std::abs(sample - expected) > 1e-6)
            { healthy.store(false); break; }
            callbacks.fetch_add(1, std::memory_order_release);
        }
    });
    while (healthy.load() && callbacks.load(std::memory_order_acquire) < 8) std::this_thread::yield();
    for (std::uint64_t request = 0; request < 64 && healthy.load(); ++request)
    {
        const auto before = callbacks.load(std::memory_order_acquire);
        require(bridge.requestTransportRelocation(3 + (request * 7) % 53), "prepare seek concurrently with active source rendering");
        while (healthy.load() && callbacks.load(std::memory_order_acquire) < before + 2) std::this_thread::yield();
        (void) bridge.collectRetired();
    }
    audio.request_stop(); audio.join();
    require(healthy.load() && callbacks.load() >= 136, "concurrent preparation and source retirement preserve every rendered sample");
    require(bridge.requestTransportRelocation(19), "final request after concurrent preparation");
    float sample = 0; float* output[] = {&sample};
    require(bridge.render(output, 1, 1) && bridge.transportFramePosition() == 20
        && std::abs(sample - 0.125 * std::sin(2 * std::numbers::pi * 7 * 19 / 1000)) < 1e-6,
        "newest prepared request owns the final exact source position");
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--parent-score-probe")
    {
        try { probeParentScoreLiveInput(); return 0; }
        catch (const std::exception& e) { std::cerr << "FAIL parent-score-live-input: " << e.what() << '\n'; return 1; }
    }
    bool failed = false;
    for (const auto& [name, test] : std::array{
        std::pair{"prepared-source-remains-live", &testPreparedSourceRemainsLive},
        std::pair{"repeat-automation-origins", &testRepeatAutomationOrigins},
        std::pair{"scoped-automation-live-control", &testScopedAutomationAndLiveControl},
        std::pair{"linear-automation-ramps-across-iterations", &testLinearScoreAutomationRampsAcrossIterations},
        std::pair{"replace-track-selected-clip", &testReplaceTrackPlaysOnlyTheSelectedClip},
        std::pair{"invalid-selection", &testInvalidSelectionRejects},
        std::pair{"parent-live-input", &probeParentScoreLiveInput},
        std::pair{"staged-timeline-live-input", &testStagedTimelineClipProcessesNewDeviceInput},
        std::pair{"parent-public-authority-forgery", &testPublicPlanCannotForgeLiveParentOffline},
        std::pair{"parent-offline-double-input", &testParentOfflineRetainsDoubleInput},
        std::pair{"parent-repeats-offsets-fades-offline", &testParentRepeatsOffsetsFadesAndOffline},
        std::pair{"parent-later-source-failure", &testParentLaterSourceFailureRestoresWholeCall},
        std::pair{"parent-stream-later-source-failure", &testParentStreamRollsBackAfterLaterSourceFailure},
        std::pair{"parent-stream-connected-rate-window", &testParentStreamConnectedRateHasExactInitialWindow},
        std::pair{"prepared-parent-transport", &testPreparedParentTransportRetainsSourcePhaseAndInput},
        std::pair{"public-bridge-live-transport", &testPublicBridgeLiveTransportReconstruction},
        std::pair{"public-bridge-stream-transport", &testPublicBridgeStreamTransport},
        std::pair{"detached-transport-lifecycle", &testDetachedTransportCandidateLifecycle},
        std::pair{"public-seek-failed-preparation", &testPublicSeekFailedPreparationPreservesPendingRequest},
        std::pair{"public-seek-stale-authority", &testPublicSeekRejectsStageAuthorityAndStopChanges},
        std::pair{"public-seek-concurrent-preparation", &testPublicSeekPreparationWhileAudioRenders},
        std::pair{"prepared-parent-stream-transport", &testPreparedParentStreamTransport},
        std::pair{"prepared-parent-history-failure", &testPreparedParentTransportHistoryAndLateFailure},
        std::pair{"failed-transport-preparation", &testFailedTransportPreparationPreservesInitialSources},
        std::pair{"transport-preparation-budget", &testTransportPreparationBudgetRejectsBeforePreroll},
        std::pair{"parent-transport-reset", &testParentTransportResetRestartsSources},
        std::pair{"source-segment-clock-offline", &testLiveScoreSegmentsHoldCompositionAndRestoreOfflineAbort},
        std::pair{"loop-separate-tentative-sources", &testLoopSegmentsKeepSeparateTentativeSourceOwners},
        std::pair{"loop-source-offline-analysis-order", &testLoopSourceOfflineAnalysisPublicationOrder},
        std::pair{"public-score-source-analysis-targets", &testPublicScoreSourceAnalysisTargets},
        std::pair{"public-score-source-analysis-loop-seek", &testPublicScoreSourceAnalysisLoopAndSeek},
        std::pair{"public-score-loop-live-input", &testPublicScoreLoopKeepsLiveInputAndExactSourcePhase},
        std::pair{"public-score-loop-midi-source-clocks", &testPublicScoreLoopRestoresMidiSourceClocks},
        std::pair{"public-score-loop-post-wrap-rollback", &testPublicScoreLoopRollsBackAfterPostWrapFailure},
        std::pair{"public-score-loop-interior-seek", &testPublicScoreLoopSeekRetainsInteriorEntryOwnerPool},
        std::pair{"public-score-loop-stream", &testPublicScoreLoopRestartsStreamAtEachReturn},
        std::pair{"public-score-loop-stage-replacement", &testPublicScoreLoopKeepsActiveClipAcrossStageReplacement},
        std::pair{"public-score-active-repeat-stage-replacement", &testPublicScoreReplacementPreparesActiveRepeat},
        std::pair{"public-score-missed-stage-boundary", &testPublicScoreReplacementRejectsMissedBoundary},
        std::pair{"score-loop-stage-phase-gate", &testLoopStageWaitsForItsPreparedLocalPhase},
        std::pair{"parent-worker-stack", &testParentSourcePreparationFitsWorkerStack}})
    {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { failed = true; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
