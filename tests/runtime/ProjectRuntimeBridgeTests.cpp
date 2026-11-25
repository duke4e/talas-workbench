#include "talas/core/BuiltinNodes.h"
#include "talas/project/PreparedProjectGenerators.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <numbers>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace talas::runtime
{
class ProjectRuntimeBridgeTestAccess
{
public:
    // Models a control thread inside a render-target producer transaction.
    static bool holdSessionProducerGate(ProjectRuntimeBridge& bridge) noexcept
    {
        return !bridge.sessionProducerGate_.test_and_set(std::memory_order_acquire);
    }

    static void releaseSessionProducerGate(ProjectRuntimeBridge& bridge) noexcept
    {
        bridge.sessionProducerGate_.clear(std::memory_order_release);
    }

    static void setInitialStageLeadFrames(
        ProjectRuntimeBridge& bridge, std::uint64_t frames) noexcept
    {
        bridge.initialStageLeadFrames_ = frames;
    }

    static std::uint64_t deferredRealtimeControlDrains(
        const ProjectRuntimeBridge& bridge) noexcept
    {
        return bridge.deferredRealtimeControlDrainCount_.load(std::memory_order_relaxed);
    }

    static void seedMidiClockPhaseNearOverflow(
        ProjectRuntimeBridge& bridge) noexcept
    {
        constexpr auto pulseAdvance =
            static_cast<std::uint64_t>(midiClockProjectTicksPerPulse)
            * (std::uint64_t{1} << midiClockPhaseFractionBits);
        bridge.audioMidiClockPulseTargetQ24_ =
            std::numeric_limits<std::uint64_t>::max()
            - pulseAdvance - 1;
        bridge.audioMidiClockMusicalTickQ24_ =
            bridge.audioMidiClockPulseTargetQ24_;
        bridge.audioMidiClockMusicalPhaseAnchored_ = true;
    }

    static double seedMidiClockFollowerNearTimestampOverflow(
        ProjectRuntimeBridge& bridge) noexcept
    {
        const auto interval = static_cast<std::int64_t>(
            MidiClockPhaseFollower::intervalNanosecondsForBpm(120));
        const auto first = std::numeric_limits<std::int64_t>::max()
            - interval * 5;
        bridge.audioMidiClockPhaseFollower_.resetIdentity();
        for (std::int64_t index = 0; index < 4; ++index)
        {
            (void)bridge.audioMidiClockPhaseFollower_.observeNanoseconds(
                first + index * interval);
        }
        const auto overflowingArrival =
            std::numeric_limits<std::int64_t>::max() - interval / 2;
        return static_cast<double>(overflowingArrival)
            / static_cast<double>(midiClockNanosecondsPerSecond);
    }

    static void exhaustStageGeneration(
        ProjectRuntimeBridge& bridge) noexcept
    {
        bridge.nextStageGeneration_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustEngineStageIdentity(
        ProjectRuntimeBridge& bridge) noexcept
    {
        bridge.nextEngineStageId_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustSnapshotProjectRevision(
        ProjectRuntimeBridge& bridge) noexcept
    {
        bridge.nextSnapshotProjectRevision_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    // Models a click whose transport snapshot was taken before the callback
    // that was rendering when its request reached the ingress.
    static bool ageQueuedLiveSetRequest(
        ProjectRuntimeBridge& bridge, std::uint64_t frames) noexcept
    {
        RealtimeLiveSetRequest request;
        if (!bridge.liveSetIngress_.tryPop(request)
            || request.resolvedFrame < frames)
        {
            return false;
        }
        request.resolvedFrame -= frames;
        return bridge.liveSetIngress_.tryPush(request);
    }
};
}

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

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
static_assert(noexcept(std::declval<const talas::runtime::ProjectRuntimeBridge&>().transportFramePosition()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>()
    .requestTransportRelocation(0)));
static_assert(noexcept(std::declval<const talas::runtime::ProjectRuntimeBridge&>().transportTimeSeconds()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>().stopTransport()));
static_assert(noexcept(std::declval<const talas::runtime::ProjectRuntimeBridge&>().realtimeControlTelemetry()));
static_assert(noexcept(std::declval<const talas::runtime::ProjectRuntimeBridge&>().midiClockTelemetry()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>().revokeMidiClockAuthorityForSourceLoss()));
static_assert(noexcept(std::declval<const talas::runtime::ProjectRuntimeBridge&>().mtcTelemetry()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>().revokeMtcAuthorityForSourceLoss()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>().realtimeMetrics()));
static_assert(noexcept(std::declval<talas::runtime::ProjectRuntimeBridge&>().resetRealtimeMetrics()));

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

constexpr std::uint64_t workbenchLiveSetSourceGeneration = 101;
constexpr std::uint64_t workbenchLiveSetRouteGeneration = 201;

void publishWorkbenchLiveSetAuthority(
    talas::runtime::ProjectRuntimeBridge& bridge) noexcept
{
    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Workbench,
        workbenchLiveSetSourceGeneration,
        workbenchLiveSetRouteGeneration);
}

talas::runtime::LiveSetActivationResult requestWorkbenchLiveSetActivation(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::string_view liveSetId,
    std::string_view cellId) noexcept
{
    return bridge.requestLiveSetActivation(
        liveSetId,
        cellId,
        talas::runtime::RealtimeLiveSetSourceKind::Workbench,
        workbenchLiveSetSourceGeneration,
        workbenchLiveSetRouteGeneration);
}

talas::project::ProjectParameterExpression expressionParameter(std::string id, std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression choiceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

// The track's frequency is in hz in these fixtures, as in projects before
// format 53; spectra now carry a fraction of half the sample rate.
std::vector<talas::project::ProjectParameterExpression> hertzSelector(
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    if (std::ranges::none_of(parameters, [](const auto& parameter) {
            return parameter.parameterId == "freqUnits";
        }))
        parameters.push_back(choiceParameter("freqUnits", "legacyHertz"));
    return parameters;
}

talas::project::ProjectDocument makePlayableDocument(double frequency, std::string gainExpression = "1")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"osc", "talas.oscillator", {expressionParameter("frequency", std::to_string(frequency) + " hz"), expressionParameter("amplitude", "1")}},
        {"gain", "talas.constant", {expressionParameter("value", std::move(gainExpression))}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeDynamicControlUtilitiesBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"oscillator", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
        {"index", "talas.constant", {
            expressionParameter("value", "1"),
        }},
        {"quantizer", "talas.quantizer", {
            expressionParameter("stepSize", "!C44Step"),
            expressionParameter("offset", "!C44Offset"),
        }},
        {"gate", "talas.gate", {
            expressionParameter("lowThreshold", "!C44Low"),
            expressionParameter("highThreshold", "!C44High"),
        }},
        {"logic", "talas.logic", {
            choiceParameter("operation", "xor"),
            expressionParameter("left", "0"),
            expressionParameter("right", "!C44Right"),
        }},
    };
    document.graph.connections = {
        {"index", "value", "quantizer", "value"},
        {"quantizer", "value", "gate", "value"},
        {"gate", "gate", "logic", "left"},
        {"logic", "gate", "vca", "gain"},
        {"oscillator", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings dynamicControlUtilitiesBridgeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C44Step", 0.5, talas::expr::ValueDomain::Scalar},
        {"C44Offset", 0.0, talas::expr::ValueDomain::Scalar},
        {"C44Low", 0.25, talas::expr::ValueDomain::Scalar},
        {"C44High", 0.75, talas::expr::ValueDomain::Scalar},
        {"C44Right", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {1, "C44High", 0.4, talas::expr::ValueDomain::Scalar},
        {2, "C44Step", 0.1, talas::expr::ValueDomain::Scalar},
        {3, "C44Low", 0.05, talas::expr::ValueDomain::Scalar},
        {3, "C44High", 0.08, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeDynamicSlewTimesBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"oscillator", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
        {"target", "talas.constant", {
            expressionParameter("value", "!C45Target"),
        }},
        {"slew", "talas.slew", {
            expressionParameter("riseTime", "!C45Rise"),
            expressionParameter("fallTime", "!C45Fall"),
        }},
    };
    document.graph.connections = {
        {"target", "value", "slew", "value"},
        {"slew", "value", "vca", "gain"},
        {"oscillator", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings dynamicSlewTimesBridgeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C45Target", 0.0, talas::expr::ValueDomain::Scalar},
        {"C45Rise", 0.5, talas::expr::ValueDomain::Seconds},
        {"C45Fall", 0.5, talas::expr::ValueDomain::Seconds},
    };
    settings.eventValueChanges = {
        {1, "C45Target", 1.0, talas::expr::ValueDomain::Scalar},
        {2, "C45Rise", 0.0, talas::expr::ValueDomain::Seconds},
        {3, "C45Target", 0.0, talas::expr::ValueDomain::Scalar},
        {3, "C45Fall", 0.0, talas::expr::ValueDomain::Seconds},
        {5, "C45Target", 1.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeDynamicClockParametersBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"oscillator", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
        {"clock", "talas.clock", {
            expressionParameter("frequency", "!C46Frequency"),
            expressionParameter("dutyCycle", "!C46Duty"),
            booleanParameter("enabled", true),
        }},
    };
    document.graph.connections = {
        {"clock", "gate", "vca", "gain"},
        {"oscillator", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings dynamicClockParametersBridgeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C46Frequency", 1.0, talas::expr::ValueDomain::Hertz},
        {"C46Duty", 0.5, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {3, "C46Frequency", 2.0, talas::expr::ValueDomain::Hertz},
        {5, "C46Duty", 1.0, talas::expr::ValueDomain::Scalar},
        {7, "C46Duty", 0.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeDynamicClockDividerParametersBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"oscillator", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
        {"clock", "talas.clock", {
            expressionParameter("frequency", "2 hz"),
            expressionParameter("dutyCycle", "0.5"),
            booleanParameter("enabled", true),
        }},
        {"divider", "talas.clockDivider", {
            expressionParameter("division", "!C47Division"),
            expressionParameter("steps", "!C47Steps"),
            expressionParameter("reset", "0"),
        }},
    };
    document.graph.connections = {
        {"clock", "gate", "divider", "clock"},
        {"divider", "step", "vca", "gain"},
        {"oscillator", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings dynamicClockDividerParametersBridgeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C47Division", 1.0, talas::expr::ValueDomain::Scalar},
        {"C47Steps", 5.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {5, "C47Steps", 2.0, talas::expr::ValueDomain::Scalar},
        {9, "C47Division", 2.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeLiveChannellerSelectorsBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
        {"leftSelector", "talas.constant", {
            expressionParameter("value", "!C49Left"),
        }},
        {"rightSelector", "talas.constant", {
            expressionParameter("value", "!C49Right"),
        }},
        {"rightSource", "talas.oscillator", {
            expressionParameter("frequency", "2 hz"),
            expressionParameter("amplitude", "0.5"),
        }},
        {"channeller", "talas.channeller", {
            booleanParameter("leftChannel", false),
            booleanParameter("rightChannel", true),
        }},
    };
    document.graph.connections = {
        {"source", "audioOut", "channeller", "audioIn"},
        {"rightSource", "audioOut", "channeller", "rightIn"},
        {"leftSelector", "value", "channeller", "leftChannel"},
        {"rightSelector", "value", "channeller", "rightChannel"},
        {"channeller", "leftOut", "out", "audioIn"},
        {"channeller", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings liveChannellerSelectorsBridgeSettings()
{
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {
        {"C49Left", -1.0, talas::expr::ValueDomain::Scalar},
        {"C49Right", 1.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "C49Left", 1.0, talas::expr::ValueDomain::Scalar},
        {4, "C49Right", 0.0, talas::expr::ValueDomain::Scalar},
        {5, "C49Left", -0.5, talas::expr::ValueDomain::Scalar},
        {5, "C49Right", 0.25, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeLiveSpectrumModifierSelectorsBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"spectrum", "talas.syntheticSpectrumFromArray", {
            expressionParameter("nbrPartials", "2"),
            booleanParameter("logScale", false),
            booleanParameter("sendBandwidths", true),
            expressionParameter("envelope", "1"),
            textParameter("amplitudes", "0.2 0.5"),
            textParameter("frequencies", "100 200"),
            textParameter("bandwidths", "1 2"),
        }},
        {"selectControl", "talas.constant", {
            expressionParameter("value", "!C50Select"),
        }},
        {"hearAllControl", "talas.constant", {
            expressionParameter("value", "!C50HearAll"),
        }},
        {"seedControl", "talas.constant", {
            expressionParameter("value", "!C51Seed"),
        }},
        {"modifier", "talas.spectrumModifier", {
            booleanParameter("select", false),
            expressionParameter("loTrack", "1"),
            expressionParameter("hiTrack", "2"),
            expressionParameter("loFreq", "0 hz"),
            expressionParameter("hiFreq", "1000 hz"),
            expressionParameter("freqHysteresis", "0 hz"),
            expressionParameter("freqScale", "0.5"),
            expressionParameter("freqOffset", "10 hz"),
            expressionParameter("loAmp", "0"),
            expressionParameter("hiAmp", "1"),
            expressionParameter("ampHysteresis", "0"),
            expressionParameter("ampScale", "2"),
            expressionParameter("ampOffset", "0.25"),
            expressionParameter("probability", "1"),
            expressionParameter("seed", "0.5"),
            booleanParameter("hearAll", true),
        }},
        {"selector", "talas.spectrumTrackSelector", hertzSelector({
            expressionParameter("track", "2"),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })},
    };
    document.graph.connections = {
        {"spectrum", "spectrumOut", "modifier", "spectrumIn"},
        {"selectControl", "value", "modifier", "select"},
        {"hearAllControl", "value", "modifier", "hearAll"},
        {"seedControl", "value", "modifier", "seed"},
        {"modifier", "spectrumOut", "selector", "spectrumIn"},
        {"selector", "leftOut", "out", "audioIn"},
        {"selector", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings liveSpectrumModifierSelectorsBridgeSettings()
{
    talas::runtime::RenderSettings settings{1000.0, 4};
    settings.eventValues = {
        {"C50Select", 1.0, talas::expr::ValueDomain::Scalar},
        {"C50HearAll", 0.0, talas::expr::ValueDomain::Scalar},
        {"C51Seed", 0.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

talas::project::ProjectDocument makeC60SpectrumOnDiskBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.spectrumOnDisk", {
            expressionParameter("level", "1"),
            expressionParameter("rateScale", "1"),
            expressionParameter("trigger", "0"),
            resourceParameter("fileName", "Analysis/C60"),
            expressionParameter("analysisFrames", "3"),
            expressionParameter("analysisFrequency", "100 hz"),
            expressionParameter("firstPartial", "1"),
        }},
        {"selector", "talas.spectrumTrackSelector", hertzSelector({
            expressionParameter("track", "1"),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })},
    };
    document.graph.connections = {
        {"source", "spectrumOut", "selector", "spectrumIn"},
        {"selector", "leftOut", "out", "audioIn"},
        {"selector", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings c60SpectrumOnDiskBridgeSettings()
{
    talas::runtime::RenderSettings settings{1000.0, 4};
    settings.tableResources = {{"Analysis/C60", {
        0.1, 1.0, 0.1,
        0.5, 2.0, 0.2,
        0.9, 3.0, 0.3,
    }}};
    return settings;
}

talas::project::ProjectDocument makeC61SumOfSinesBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.sumOfSines", {
            expressionParameter("onDuration", "1 s"),
            resourceParameter("analysis0", "Analysis/C61"),
            expressionParameter("analysisFrequency0", "1 hz"),
            resourceParameter("analysis1", "Analysis/C61"),
            expressionParameter("analysisFrequency1", "3 hz"),
            expressionParameter("dbMorph", "0"),
            expressionParameter("pchMorph", "0.5"),
            expressionParameter("nbrPartials", "1"),
            expressionParameter("bankSize", "1"),
            expressionParameter("timeIndex", "0"),
            expressionParameter("gate", "1"),
            booleanParameter("loop", false),
            expressionParameter("loopStart", "0"),
            expressionParameter("loopEnd", "0"),
            booleanParameter("ctrlTime", false),
            expressionParameter("envelope", "1"),
        }},
    };
    document.graph.connections = {
        {"source", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings c61SumOfSinesBridgeSettings()
{
    talas::runtime::RenderSettings settings{16.0, 4};
    settings.tableResources = {{"Analysis/C61", {1.0}}};
    return settings;
}

talas::project::ProjectDocument makeC63SumOfSinesBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.sumOfSines", {
            expressionParameter("onDuration", "1 s"),
            expressionParameter("frequency0", "1 hz"),
            expressionParameter("frequency1", "1 hz"),
            resourceParameter("analysis0", "Analysis/C63"),
            expressionParameter("analysisFrequency0", "1 hz"),
            expressionParameter("analysisFrames0", "3"),
            resourceParameter("analysis1", ""),
            expressionParameter("analysisFrequency1", "1 hz"),
            expressionParameter("analysisFrames1", "1"),
            expressionParameter("dbMorph", "0"),
            expressionParameter("pchMorph", "0"),
            expressionParameter("nbrPartials", "1"),
            expressionParameter("bankSize", "1"),
            expressionParameter("timeIndex", "0"),
            expressionParameter("gate", "1"),
            booleanParameter("loop", true),
            expressionParameter("loopStart", "0.25"),
            expressionParameter("loopEnd", "0.75"),
            booleanParameter("ctrlTime", false),
            expressionParameter("envelope", "1"),
        }},
    };
    document.graph.connections = {
        {"source", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::runtime::RenderSettings c63SumOfSinesBridgeSettings()
{
    talas::runtime::RenderSettings settings{4.0, 4};
    settings.tableResources = {
        {"Analysis/C63", {1.0, 0.5, 0.25}},
    };
    return settings;
}

talas::project::ProjectDocument makeC58SpectrumInRamBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.spectrumInRam", {
            expressionParameter("frequency", "100 hz"),
            expressionParameter("level", "1"),
            expressionParameter("timeIndex", "0"),
            resourceParameter("analysis", "Analysis/C58"),
            expressionParameter("analysisFrames", "3"),
            expressionParameter("nbrPartials", "1"),
            expressionParameter("firstPartial", "1"),
        }},
        {"selector", "talas.spectrumTrackSelector", hertzSelector({
            expressionParameter("track", "1"),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })},
    };
    document.graph.connections = {
        {"source", "spectrumOut", "selector", "spectrumIn"},
        {"selector", "leftOut", "out", "audioIn"},
        {"selector", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings c58SpectrumInRamBridgeSettings()
{
    talas::runtime::RenderSettings settings{1000.0, 4};
    settings.tableResources = {{"Analysis/C58", {
        0.1, 1.0, 0.1,
        0.5, 2.0, 0.2,
        0.9, 3.0, 0.3,
    }}};
    return settings;
}

talas::project::ProjectDocument makeC59SpectrumInRamDefaultsBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.spectrumInRam", {
            resourceParameter("analysis", "Analysis/C59"),
            expressionParameter("analysisFrequency", "125 hz"),
        }},
        {"selector", "talas.spectrumTrackSelector", hertzSelector({
            expressionParameter("track", "3"),
            booleanParameter("amp", true),
            booleanParameter("freq", true),
        })},
    };
    document.graph.connections = {
        {"source", "spectrumOut", "selector", "spectrumIn"},
        {"selector", "leftOut", "out", "audioIn"},
        {"selector", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings c59SpectrumInRamDefaultsBridgeSettings()
{
    talas::runtime::RenderSettings settings{1000.0, 4};
    settings.tableResources = {{"Analysis/C59", {
        0.2, 1.0, 0.1,
        0.4, 2.0, 0.2,
        0.6, 3.0, 0.3,
    }}};
    return settings;
}

talas::project::ProjectDocument makeC57VoicedUnvoicedBridgeDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"source", "talas.spectrumOnDisk", {
            expressionParameter("frequency", "100 hz"),
            expressionParameter("level", "1"),
            expressionParameter("rateScale", "1"),
            expressionParameter("trigger", "0"),
            resourceParameter("fileName", "Analysis/C57"),
            expressionParameter("analysisFrames", "5"),
            expressionParameter("nbrPartials", "2"),
            expressionParameter("firstPartial", "1"),
        }},
        {"voiced", "talas.spectrumVoicedUnvoiced", {}},
    };
    document.graph.connections = {
        {"source", "spectrumOut", "voiced", "spectrumIn"},
        {"voiced", "leftOut", "out", "audioIn"},
        {"voiced", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::runtime::RenderSettings c57VoicedUnvoicedBridgeSettings()
{
    talas::runtime::RenderSettings settings{1000.0, 4};
    settings.tableResources = {{"Analysis/C57", {
        1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
        1.0, 1.0, 0.1, 0.5, 2.6, 0.2,
        1.0, 1.0, 0.1, 0.5, 2.6, 0.2,
        1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
        1.0, 1.0, 0.1, 0.5, 2.0, 0.2,
    }}};
    return settings;
}

talas::project::ProjectDocument makeRawMidiTerminalDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiOutputEventInBytes",
        {textParameter("bytes", "16r90 64 16r7f")},
    }};
    return document;
}

talas::project::ProjectDocument makeMidiControllerTerminalDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiOutputController",
        {
            expressionParameter("channel", "3"),
            expressionParameter("controllerNumber", "10"),
            textParameter("value", "127"),
        },
    }};
    return document;
}

talas::project::ProjectDocument makeMidiNoteTerminalDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiOutputEvent",
        {
            expressionParameter("frequency", "440 hz"),
            expressionParameter("amplitude", "1"),
            expressionParameter("channel", "2"),
            expressionParameter("gate", "1"),
        },
    }};
    return document;
}

talas::project::ProjectDocument makeMidiFileTerminalDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.midiFileEcho",
        {
            expressionParameter("lowChannel", "1"),
            expressionParameter("highChannel", "16"),
            resourceParameter("fileName", "Sequences/C42.mid"),
        },
    }};
    return document;
}

talas::project::ProjectDocument makeGlobalControllerTerminalDocument(
    std::string generatedEvent,
    std::string value)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{
        "terminal",
        "talas.soundToGlobalController",
        {
            textParameter(
                "generatedEvent", std::move(generatedEvent)),
            textParameter("value", std::move(value)),
        },
    }};
    return document;
}

talas::runtime::RenderSettings midiFileSettings()
{
    talas::runtime::RenderSettings result{8.0, 4};
    result.binaryResources.push_back({
        "Sequences/C42.mid",
        {
            'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
            'M', 'T', 'r', 'k', 0, 0, 0, 12,
            0, 0x90, 60, 64, 96, 0x80, 60, 0, 0, 0xff, 0x2f, 0,
        }});
    return result;
}

talas::project::ProjectDocument makeParameterTransformerBridgeDocument(
    std::string transformation)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"transform", "talas.parameterTransformer", {
            textParameter("transformation", std::move(transformation)),
        }},
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "3 hz"),
            expressionParameter("amplitude", "1"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "transform", "audioIn"},
        {"transform", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeMissingComponentDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        {},
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    missing.preservedComponent = std::move(component);
    document.graph.nodes = {
        std::move(missing),
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"missing1", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeScheduledEventDocument()
{
    talas::project::ProjectParameterExpression type;
    type.parameterId = "type";
    type.choice = "hotPink";

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"source", "talas.noise", {
            expressionParameter("initialState", "1"),
            expressionParameter("frequency", "0 hz"),
            expressionParameter("centerValue", "0"),
            expressionParameter("scale", "1"),
            std::move(type),
        }},
        {"gain", "talas.constant", {expressionParameter("value", "!Gain")}},
    };
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeMidiGateDocument(std::string gainExpression = "!KeyDown * !KeyVelocity")
{
    auto document = makeScheduledEventDocument();
    document.graph.nodes.back().parameters.front().expression = std::move(gainExpression);
    return document;
}

talas::project::ProjectDocument makePolyphonicMidiVoiceDocument(
    std::size_t polyphony = 2,
    std::uint8_t channel = 1,
    std::string ownerGain = "1 - !KeyDown")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"midi", "talas.midiVoice", {
            expressionParameter("left", ownerGain),
            expressionParameter("right", std::move(ownerGain)),
            expressionParameter("channel", std::to_string(channel)),
            textParameter("source", ""),
            resourceParameter("midiFile", ""),
            expressionParameter("polyphony", std::to_string(polyphony)),
            expressionParameter("lowPitch", "0 nn"),
            expressionParameter("highPitch", "127 nn"),
            textParameter("script", ""),
            booleanParameter("shared", false),
        }},
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "!Pitch"),
            expressionParameter("amplitude", "!KeyDown * !KeyVelocity"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "midi", "audioIn"},
        {"midi", "leftOut", "out", "audioIn"},
        {"midi", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::project::ProjectDocument makeMappedMidiMapperDocument(
    std::string mode = "gate")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"midi", "talas.midiMapper", {
            textParameter("map", std::move(mode)),
            choiceParameter("voiceModel", "legacyPassthrough"),
            expressionParameter("left", "1"),
            expressionParameter("right", "1"),
            expressionParameter("channel", "0"),
            textParameter("source", ""),
            resourceParameter("midiFile", ""),
            expressionParameter("polyphony", "4"),
            expressionParameter("lowPitch", "0 nn"),
            expressionParameter("highPitch", "127 nn"),
            textParameter("script", ""),
            booleanParameter("shared", false),
        }},
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "midi", "audioIn"},
        {"midi", "leftOut", "out", "audioIn"},
        {"midi", "rightOut", "out", "rightIn"},
    };
    return document;
}

talas::project::ProjectDocument makePolyphonicMidiMapperDocument()
{
    auto document = makePolyphonicMidiVoiceDocument(2, 1, "1");
    auto& mapper = document.graph.nodes[1];
    mapper.typeId = "talas.midiMapper";
    mapper.parameters.push_back(textParameter("map", ""));
    mapper.parameters.push_back(
        choiceParameter("voiceModel", "polyphonicInput"));
    return document;
}

talas::project::ProjectDocument makeAudioInputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeDisplayDocument(std::string typeId)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"display", std::move(typeId), {}},
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "1 hz"),
            expressionParameter("amplitude", "1"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "display", "audioIn"},
        {"display", "audioOut", "out", "audioIn"},
    };
    return document;
}

const talas::core::NodeRegistry& registry()
{
    static const talas::core::NodeRegistry instance = talas::core::makeBuiltinNodeRegistry();
    return instance;
}

talas::runtime::RenderSettings settings()
{
    talas::runtime::RenderSettings result;
    result.sampleRate = 4.0;
    result.maxBlockFrames = 4;
    return result;
}

talas::core::ResourceSnapshotPtr resourceSnapshot(
    talas::core::ResourceSnapshotKind kind,
    std::uint64_t generation,
    char hashDigit)
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Shared/Resource";
    snapshot.kind = kind;
    snapshot.generation = generation;
    snapshot.contentHash.assign(64, hashDigit);
    switch (kind)
    {
        case talas::core::ResourceSnapshotKind::Table:
            snapshot.tableSamples = {0.25};
            break;
        case talas::core::ResourceSnapshotKind::Binary:
            snapshot.binaryBytes = {0x42};
            break;
        case talas::core::ResourceSnapshotKind::Audio:
            snapshot.resolvedPath = "/private/tmp/talas-unused-audio.wav";
            snapshot.channelCount = 1;
            snapshot.sampleRate = 48'000.0;
            snapshot.frameCount = 1;
            snapshot.encoding = "pcm16-wav";
            break;
        case talas::core::ResourceSnapshotKind::PartialSpectrum:
            snapshot.partialTrackBytes = {0x42};
            break;
        }
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

talas::runtime::RenderSettings resourceSettings(
    talas::core::ResourceSnapshotPtr snapshot)
{
    auto result = settings();
    result.resourceSnapshots.push_back(std::move(snapshot));
    return result;
}

talas::runtime::RenderSettings polyphonicMidiSettings(std::size_t queueCapacity = 16)
{
    talas::runtime::RenderSettings result{1000.0, 16};
    result.maxRealtimeEventTransactions = queueCapacity;
    return result;
}

talas::runtime::RenderSettings liveEventSettings()
{
    auto result = settings();
    result.eventValues.push_back({"Gain", 0.5, talas::expr::ValueDomain::Scalar});
    return result;
}

bool hasAudibleSample(const std::array<float, 4>& output)
{
    for (const auto sample : output)
    {
        if (std::abs(sample) > 1.0e-4F)
            return true;
    }
    return false;
}

bool render(talas::runtime::ProjectRuntimeBridge& bridge, std::array<float, 4>& output)
{
    output.fill(0.0F);
    std::array<float*, 1> channels{output.data()};
    return bridge.render(channels.data(), channels.size(), output.size());
}

template <std::size_t FrameCount>
bool renderFrames(talas::runtime::ProjectRuntimeBridge& bridge, std::array<float, FrameCount>& output)
{
    output.fill(0.0F);
    std::array<float*, 1> channels{output.data()};
    return bridge.render(channels.data(), channels.size(), output.size());
}

bool renderAndCount(talas::runtime::ProjectRuntimeBridge& bridge,
                    std::array<float, 4>& output,
                    std::size_t& allocations)
{
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = render(bridge, output);
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    return rendered;
}

bool renderStereoAndCount(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::array<float, 4>& left,
    std::array<float, 4>& right,
    std::size_t& allocations)
{
    left.fill(0.0F);
    right.fill(0.0F);
    std::array<float*, 2> channels{left.data(), right.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render(
        channels.data(), channels.size(), left.size());
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    return rendered;
}

void testBridgeCompilesStagesAndRendersEditableDocument()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);

    const auto stage = bridge.compileAndStage(settings());
    require(stage.ok(), "supported project should compile and stage");
    require(stage.stageResult == talas::runtime::RealtimeGraphStageResult::Accepted,
            "first prepared graph should stage immediately");

    std::array<float, 4> output{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, output, allocations), "staged graph should render after activation");
    require(allocations == 0, "bridge render should not allocate");
    require(hasAudibleSample(output), "staged graph should render audible output");
}

// Compile & load compiles and loads a
// Sound without starting it; Restart starts it.
void testCompileAndLoadWaitsForRestart()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    const auto stage = bridge.compileAndStage(settings());
    require(stage.ok() && stage.stageResult == talas::runtime::RealtimeGraphStageResult::Accepted,
        "Compile & load should compile and load the Sound");
    std::array<float, 4> output{};
    std::size_t allocations = 0;
    (void) renderAndCount(bridge, output, allocations);
    require(!hasAudibleSample(output), "a Sound compiled and loaded should not start");
    bridge.setPlaying(true);
    require(renderAndCount(bridge, output, allocations) && hasAudibleSample(output),
        "Restart should start the loaded Sound");
}

void testCheckedBiquadExampleRendersThroughOrderedPreparedBridge()
{
    const auto sourceRoot = std::filesystem::path{__FILE__}
        .parent_path().parent_path().parent_path();
    const auto projectPath = sourceRoot
        / "examples/c17_checked_biquad_family.talas.json";
    std::ifstream input(projectPath);
    require(input.good(), "C17 bridge fixture should be readable");
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    const auto parsed = talas::project::parseProjectDocument(json);
    require(parsed.ok(), "C17 bridge fixture should parse");
    talas::runtime::ProjectRuntimeBridge bridge(
        parsed.document, registry());
    talas::runtime::RenderSettings profile{48000.0, 128};
    const auto staged = bridge.compileAndStageDocument(
        parsed.document, profile);
    require(staged.ok(), "C17 bridge fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    require(bridge.renderOrdered({}, 0.0, channels.data(),
                channels.size(), left.size()),
        "C17 ordered prepared bridge should render its first block");
    require(std::ranges::any_of(left, [](float sample) {
        return std::isfinite(sample) && std::abs(sample) > 1.0e-6F;
    }), "C17 ordered prepared bridge should produce audible left output");
}

void testBridgeRendersTerminalMidiWithExactRestageIdentity()
{
    const auto document = makeRawMidiTerminalDocument();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    const auto first = bridge.compileAndStage(settings());
    require(
        first.ok() && first.outputChannelCount == 0,
        "terminal MIDI bridge fixture should stage with zero audio outputs");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 2> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    require(
        bridge.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != 0
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 64
            && events[0].bytes[2] == 0x7f,
        "terminal MIDI bridge should return one exact stage-bound packet");

    const auto previousStage = output.stageId;
    require(
        bridge.compileAndStageDocument(document, settings()).ok(),
        "terminal MIDI bridge restage should prepare");
    require(
        bridge.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == bridge.activeStageId()
            && output.stageId != previousStage,
        "terminal MIDI bridge restage should publish only pristine new-stage state");
    require(
        bridge.collectRetired(),
        "terminal MIDI bridge should collect the retired stage locally");

    const auto controllerDocument =
        makeMidiControllerTerminalDocument();
    talas::runtime::ProjectRuntimeBridge controller(
        controllerDocument, registry());
    controller.setPlaying(true);
    const auto controllerStage = controller.compileAndStage(settings());
    require(
        controllerStage.ok() && controllerStage.outputChannelCount == 0,
        "C40 controller bridge fixture should stage as terminal-only");
    require(
        controller.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == controller.activeStageId()
            && output.stageId != 0
            && events[0].bytes[0] == 0xb2
            && events[0].bytes[1] == 10
            && events[0].bytes[2] == 127,
        "C40 controller bridge should return one exact stage-bound CC packet");
    const auto previousControllerStage = output.stageId;
    require(
        controller.compileAndStageDocument(
            controllerDocument, settings()).ok()
            && controller.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == controller.activeStageId()
            && output.stageId != previousControllerStage,
        "C40 controller restage should publish pristine new-stage state");

    const auto noteDocument = makeMidiNoteTerminalDocument();
    talas::runtime::ProjectRuntimeBridge note(noteDocument, registry());
    note.setPlaying(true);
    const auto noteStage = note.compileAndStage(settings());
    require(
        noteStage.ok() && noteStage.outputChannelCount == 0,
        "C41 note bridge fixture should stage as terminal-only");
    require(
        note.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == note.activeStageId()
            && output.stageId != 0
            && events[0].bytes[0] == 0x91
            && events[0].bytes[1] == 69
            && events[0].bytes[2] == 127,
        "C41 note bridge should return one exact stage-bound note packet");
    const auto previousNoteStage = output.stageId;
    require(
        note.compileAndStageDocument(noteDocument, settings()).ok()
            && note.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == note.activeStageId()
            && output.stageId != previousNoteStage,
        "C41 note restage should publish pristine new-stage state");

    const auto midiFileDocument = makeMidiFileTerminalDocument();
    talas::runtime::ProjectRuntimeBridge midiFile(
        midiFileDocument, registry());
    midiFile.setPlaying(true);
    const auto midiFileStage =
        midiFile.compileAndStage(midiFileSettings());
    require(
        midiFileStage.ok() && midiFileStage.outputChannelCount == 0,
        "C42 MIDI file bridge fixture should stage as terminal-only");
    require(
        midiFile.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == midiFile.activeStageId()
            && output.stageId != 0
            && events[0].frameOffset == 0
            && events[0].bytes[0] == 0x90
            && events[0].bytes[1] == 60
            && events[0].bytes[2] == 64,
        "C42 MIDI file bridge should return one exact stage-bound packet");
    const auto previousMidiFileStage = output.stageId;
    midiFile.stopTransport();
    require(
        midiFile.compileAndStageDocument(
            midiFileDocument, midiFileSettings()).ok(),
        "C42 MIDI file bridge restage should prepare");
    midiFile.setPlaying(true);
    require(
        midiFile.renderTerminalEvents(4, output)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && output.midiEventCount == 1
            && output.stageId == midiFile.activeStageId()
            && output.stageId != previousMidiFileStage
            && events[0].bytes[0] == 0x90,
        "C42 reset restage should publish pristine new-stage schedule state");

    const auto globalDocumentA =
        makeGlobalControllerTerminalDocument(
            "!BridgeGlobalA", "0.25");
    talas::runtime::ProjectRuntimeBridge global(
        globalDocumentA, registry());
    global.setPlaying(true);
    const auto globalStage = global.compileAndStage(settings());
    require(
        globalStage.ok() && globalStage.outputChannelCount == 0,
        "C43 global-controller bridge fixture should stage as terminal-only");
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 1>
        globalEvents{};
    std::array<char, 14> globalName{};
    talas::runtime::RealtimeTerminalOutputBlock globalOutput{};
    globalOutput.globalControllerEvents = globalEvents;
    globalOutput.globalControllerName = globalName;
    require(
        global.renderTerminalEvents(4, globalOutput)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && globalOutput.stageId == global.activeStageId()
            && globalOutput.stageId != 0
            && globalOutput.midiEventCount == 0
            && globalOutput.globalControllerEventCount == 1
            && globalOutput.globalControllerNameLength
                == globalName.size()
            && std::string_view{
                   globalName.data(), globalName.size()}
                == "!BridgeGlobalA"
            && globalEvents[0].frameOffset == 0
            && globalEvents[0].value == 0.25,
        "C43 bridge should return one exact stage-bound global-controller event");
    const auto previousGlobalStage = globalOutput.stageId;
    const auto globalDocumentB =
        makeGlobalControllerTerminalDocument(
            "!BridgeGlobalB", "{!BridgeValue}");
    auto globalSettingsB = settings();
    globalSettingsB.eventValues = {
        {"BridgeValue", 0.75,
         talas::expr::ValueDomain::Scalar},
    };
    require(
        global.compileAndStageDocument(
            globalDocumentB, globalSettingsB).ok()
            && global.requestTransportRelocation(0),
        "C48 dynamic global-controller restage and frame-zero relocation should prepare");
    require(
        global.renderTerminalEvents(4, globalOutput)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && globalOutput.stageId == global.activeStageId()
            && globalOutput.stageId != previousGlobalStage
            && globalOutput.globalControllerEventCount == 1
            && globalOutput.globalControllerNameLength
                == globalName.size()
            && std::string_view{
                   globalName.data(), globalName.size()}
                == "!BridgeGlobalB"
            && globalEvents[0].frameOffset == 0
            && globalEvents[0].value == 0.75,
        "C48 bridge restage should publish only the new immutable name, dynamic fallback value, and stage");
    require(
        global.collectRetired(),
        "C43 bridge should collect the retired global-controller stage locally");

    globalOutput.midiEventCount = 121;
    globalOutput.globalControllerEventCount = 122;
    globalOutput.globalControllerNameLength = 123;
    globalOutput.stageId = 124;
    require(
        global.renderTerminalEvents(0, globalOutput)
                == talas::runtime::RealtimeTerminalRenderResult::InvalidArguments
            && globalOutput.midiEventCount == 121
            && globalOutput.globalControllerEventCount == 122
            && globalOutput.globalControllerNameLength == 123
            && globalOutput.stageId == 124,
        "C43 bridge rejection should preserve caller publication metadata");
}

void testBridgeStagesDocumentOnBoundedWorkerStack()
{
    const auto document = makePlayableDocument(2.0);
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bool staged = false;
    std::thread worker([&] {
        staged = bridge.compileAndStageDocument(document, settings()).ok();
    });
    worker.join();
    require(
        staged,
        "document staging should reuse registry authority on a bounded worker stack");
}

void testBridgeRejectsBeforePreparedIdentityWraparound()
{
    talas::runtime::ProjectRuntimeBridge stageGeneration(
        makePlayableDocument(1.0), registry());
    talas::runtime::ProjectRuntimeBridgeTestAccess::exhaustStageGeneration(
        stageGeneration);
    require(
        stageGeneration.compileAndStage(settings()).outcome
            == talas::runtime::ProjectStageOutcome::StageRejected,
        "prepared stage generation exhaustion must fail closed before wraparound");

    talas::runtime::ProjectRuntimeBridge engineStage(
        makePlayableDocument(1.0), registry());
    talas::runtime::ProjectRuntimeBridgeTestAccess::exhaustEngineStageIdentity(
        engineStage);
    require(
        engineStage.compileAndStage(settings()).outcome
            == talas::runtime::ProjectStageOutcome::StageRejected,
        "engine stage identity exhaustion must fail closed before wraparound");

    talas::runtime::ProjectRuntimeBridge projectRevision(
        makePlayableDocument(1.0), registry());
    talas::runtime::ProjectRuntimeBridgeTestAccess::
        exhaustSnapshotProjectRevision(projectRevision);
    require(
        projectRevision.compileAndStage(settings()).outcome
            == talas::runtime::ProjectStageOutcome::StageRejected,
        "snapshot project revision exhaustion must fail closed before wraparound");

    talas::runtime::ProjectRuntimeBridge authoringRevision(
        makePlayableDocument(1.0), registry());
    require(
        authoringRevision.compileAndStage(settings()).ok(),
        "snapshot authoring revision exhaustion fixture must stage");
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "exhaustion";
    bank.displayName = "Exhaustion";
    require(
        authoringRevision.editor().addSnapshotBank(std::move(bank)).ok(),
        "snapshot authoring revision exhaustion fixture must create an authoring edit");
    talas::runtime::ProjectRuntimeBridgeTestAccess::
        exhaustSnapshotProjectRevision(authoringRevision);
    const auto exhaustedAuthoring =
        authoringRevision.adoptSnapshotAuthoringEdit();
    require(
        !exhaustedAuthoring.ok()
            && !exhaustedAuthoring.diagnostics.empty()
            && exhaustedAuthoring.diagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::CapacityExceeded,
        "snapshot authoring must reject before its revision identity wraps");
}

void testBridgeResolvesStableVariableIdentityAndRejectsStaleStage()
{
    auto document = makePlayableDocument(240.0);
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    const auto oscillator = std::ranges::find(
        document.graph.nodes,
        std::string{"osc"},
        &talas::project::ProjectNode::instanceId);
    require(
        oscillator != document.graph.nodes.end(),
        "variable bridge fixture should contain its oscillator");
    oscillator->parameters.front().expression = "@pitch";
    document.variableLibrary.variables.push_back({
        "pitch",
        "Project pitch",
        talas::expr::ValueDomain::Hertz,
        240.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "variable-backed bridge fixture should stage");
    const auto stageGeneration = bridge.latestStageGeneration();
    const talas::project::PreparedProjectVariableIdentity identity{
        talas::project::ProjectVariableOwnerKind::Project,
        "pitch",
        {},
        {},
    };
    std::array<float, 4> initial{};
    require(
        render(bridge, initial)
            && bridge.scheduleVariableAssignment(
                identity,
                120.0,
                talas::expr::ValueDomain::Hertz,
                0,
                stageGeneration),
        "bridge should resolve and queue one exact project variable assignment");
    std::array<float, 4> updated{};
    const auto variableRendered = render(bridge, updated);
    const auto variableTelemetry = bridge.realtimeControlTelemetry();
    require(
        variableRendered
            && variableTelemetry.preparedTypedEvents >= 1
            && variableTelemetry.aggregateRejected == 0,
        "bridge variable assignment should reach typed-event DSP admission at the next callback");
    const auto assignedValues = bridge.preparedVariableValues();
    require(
        assignedValues.publicationValid
            && assignedValues.values.size() == 1
            && assignedValues.values.front().identity == identity
            && assignedValues.values.front().value
                == talas::expr::ExpressionValue{
                    120.0,
                    talas::expr::ValueDomain::Hertz},
        "bridge should publish one coherent live variable value");

    require(
        bridge.compileAndStage(settings()).ok(),
        "compatible variable restage should prepare");
    require(
        !bridge.scheduleVariableAssignment(
                identity,
                60.0,
                talas::expr::ValueDomain::Hertz,
                0,
                stageGeneration),
        "an old variable stage generation must not rebind across replacement");
    require(
        render(bridge, updated),
        "compatible variable restage should adopt on the next callback");
    const auto transferredValues = bridge.preparedVariableValues();
    require(
        transferredValues.publicationValid
            && transferredValues.stageId == bridge.activeStageId()
            && transferredValues.values.size() == 1
            && transferredValues.values.front().value
                == talas::expr::ExpressionValue{
                    120.0,
                    talas::expr::ValueDomain::Hertz},
        "compatible exact-identity restage should atomically transfer the live value");

    const auto replacementGeneration = bridge.latestStageGeneration();
    require(
        bridge.resetAllVariables(replacementGeneration)
                == talas::runtime::SnapshotDirectTransactionResult::Accepted
            && render(bridge, updated),
        "reset-all should enter one prepared variable transaction");
    const auto explicitlyResetValues = bridge.preparedVariableValues();
    require(
        explicitlyResetValues.publicationValid
            && explicitlyResetValues.values.size() == 1
            && explicitlyResetValues.values.front().value
                == talas::expr::ExpressionValue{
                    240.0,
                    talas::expr::ValueDomain::Hertz},
        "reset-all must restore the exact declaration initial value");

    require(
        bridge.scheduleVariableAssignment(
            identity,
            180.0,
            talas::expr::ValueDomain::Hertz,
            0,
            replacementGeneration)
            && render(bridge, updated),
        "capture fixture should publish one new project-owned live value");
    const auto documentGeneration =
        bridge.editor().documentAuthorityGeneration();
    auto nestedIdentity = identity;
    nestedIdentity.voiceAncestors.push_back({"parent", {}, {}, 0});
    require(!bridge.captureProjectVariableInitialValue(
                nestedIdentity, replacementGeneration, documentGeneration).ok()
            && bridge.editor().documentAuthorityGeneration() == documentGeneration,
        "a projected voice identity cannot capture a project declaration or create an edit");
    require(
        bridge.captureProjectVariableInitialValue(
                  identity,
                  replacementGeneration,
                  documentGeneration)
                .ok()
            && bridge.editor().document()
                    .variableLibrary.variables.front().initialValue
                == 180.0
            && bridge.editor().undo().ok()
            && bridge.editor().document()
                    .variableLibrary.variables.front().initialValue
                == 240.0,
        "project-owned capture should use exact live/document authority and create one undo entry");

    require(
        bridge.scheduleVariableAssignment(
            identity,
            120.0,
            talas::expr::ValueDomain::Hertz,
            0,
            replacementGeneration)
            && render(bridge, updated),
        "lifecycle reset fixture should first restore one noninitial live value");

    bridge.resetResourceIdentityHistory();
    require(
        bridge.compileAndStage(settings()).ok()
            && render(bridge, updated),
        "explicit lifecycle reset should stage a fresh variable graph");
    const auto resetValues = bridge.preparedVariableValues();
    require(
        resetValues.publicationValid
            && resetValues.values.size() == 1
            && resetValues.values.front().value
                == talas::expr::ExpressionValue{
                    240.0,
                    talas::expr::ValueDomain::Hertz},
        "explicit lifecycle reset must restore declaration initial values");
}

void testBridgeSchedulesWorkspaceVariableBatchAtomically()
{
    auto document = makePlayableDocument(220.0, "!Gain");
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        1.0,
        0.0,
        1.0,
    });
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {
        {
            "localBias",
            "Local bias",
            talas::expr::ValueDomain::Scalar,
            0.0,
            {talas::project::ProjectVariableOwnerKind::WorkspaceSession},
            true,
        },
        {
            "localRate",
            "Local rate",
            talas::expr::ValueDomain::Hertz,
            440.0,
            {talas::project::ProjectVariableOwnerKind::WorkspaceSession},
            false,
        },
    };
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(),
        "workspace variable batch fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output),
        "workspace variable batch fixture should adopt its exact stage");
    const auto generation = bridge.latestStageGeneration();
    const std::array assignments{
        talas::runtime::ProjectVariableAssignment{
            {talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                "localBias", {}, {}},
            {0.25, talas::expr::ValueDomain::Scalar}},
        talas::runtime::ProjectVariableAssignment{
            {talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                "localRate", {}, {}},
            {880.0, talas::expr::ValueDomain::Hertz}},
    };
    require(
        bridge.scheduleVariableAssignments(assignments, generation)
                == talas::runtime::SnapshotDirectTransactionResult::Accepted
            && render(bridge, output),
        "one exact workspace variable batch should reach the callback");
    const auto published = bridge.preparedVariableValues();
    require(
        published.publicationValid && published.values.size() == 2
            && published.values[0].value.value == 0.25
            && published.values[1].value.value == 880.0,
        "callback acknowledgment should publish the complete workspace batch coherently");

    const std::array duplicate{
        assignments[0],
        assignments[0],
    };
    require(
        bridge.scheduleVariableAssignments(duplicate, generation)
            == talas::runtime::SnapshotDirectTransactionResult::InvalidTargetOrPayload,
        "duplicate workspace identities should reject the whole batch before callback admission");
}

void testBridgeDefersControlDrainWhileControlThreadProduces()
{
    auto document = makePlayableDocument(1.0, "!Gain");
    document.virtualControls.push_back({
        "Gain", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 1.0, 0.0, 1.0,
    });
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "producer gate fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output) && render(bridge, output) && hasAudibleSample(output),
            "producer gate fixture should start audible");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(gainId.has_value()
                && bridge.enqueueEventValue(*gainId, 0.0, talas::expr::ValueDomain::Scalar),
            "a control should be accepted into the ingress");
    require(talas::runtime::ProjectRuntimeBridgeTestAccess::holdSessionProducerGate(bridge),
            "the test should hold the render-target producer gate");
    const auto rendered = render(bridge, output);
    talas::runtime::ProjectRuntimeBridgeTestAccess::releaseSessionProducerGate(bridge);
    require(rendered && hasAudibleSample(output)
                && talas::runtime::ProjectRuntimeBridgeTestAccess::deferredRealtimeControlDrains(bridge) == 1,
            "the audio thread must defer, not race, a control drain while a control thread produces");
    require(render(bridge, output) && render(bridge, output) && !hasAudibleSample(output),
            "the deferred control must apply on a later callback instead of being lost");
}

void testBridgeSerializesConcurrentSessionProducers()
{
    auto document = makePlayableDocument(1.0, "!Gain");
    document.virtualControls.push_back({
        "Gain", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 1.0, 0.0, 1.0,
    });
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.variableLibrary.variables = {
        {"localBias", "Local bias", talas::expr::ValueDomain::Scalar, 0.0,
         {talas::project::ProjectVariableOwnerKind::WorkspaceSession}, true},
    };
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "concurrent producer fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output), "concurrent producer fixture should adopt its stage");
    const auto generation = bridge.latestStageGeneration();
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(gainId.has_value(), "concurrent producer fixture should publish its control");

    std::atomic<bool> stop = false;
    std::atomic<std::size_t> accepted = 0;
    std::thread control([&] {
        for (std::size_t index = 0; !stop.load(std::memory_order_acquire); ++index)
        {
            const std::array assignment{
                talas::runtime::ProjectVariableAssignment{
                    {talas::project::ProjectVariableOwnerKind::WorkspaceSession, "localBias", {}, {}},
                    {static_cast<double>(index % 4) * 0.25, talas::expr::ValueDomain::Scalar}},
            };
            if (bridge.scheduleVariableAssignments(assignment, generation)
                == talas::runtime::SnapshotDirectTransactionResult::Accepted)
            {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });
    bool renderedAll = true;
    for (std::size_t callback = 0; callback < 4000; ++callback)
    {
        (void)bridge.enqueueEventValue(
            *gainId, static_cast<double>(callback % 2), talas::expr::ValueDomain::Scalar);
        renderedAll = render(bridge, output) && renderedAll;
    }
    stop.store(true, std::memory_order_release);
    control.join();
    require(renderedAll && accepted.load() > 0,
            "concurrent control-thread and audio-thread producers should both make progress");
}

void testBridgeSchedulesAndTransfersExactSoundSelections()
{
    auto document = makePlayableDocument(220.0);
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
        {.definitionId = "toneC", .displayName = "Tone C"},
    };
    talas::project::ProjectSoundVariableDeclaration selected;
    selected.variableId = "selected";
    selected.displayName = "Selected";
    selected.candidateDefinitionIds = {"toneA", "toneB"};
    talas::project::ProjectSoundVariableDeclaration workspace = selected;
    workspace.variableId = "workspace";
    workspace.displayName = "Workspace";
    workspace.candidateDefinitionIds = {"toneA", "toneC"};
    workspace.owner.kind = talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    document.soundVariableLibrary.variables = {
        std::move(selected),
        std::move(workspace),
    };

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "Sound-selection bridge fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output), "Sound-selection bridge fixture should activate");
    const auto generation = bridge.latestStageGeneration();
    const talas::project::PreparedProjectSoundVariableIdentity projectIdentity{
        talas::project::ProjectVariableOwnerKind::Project,
        "selected",
    };
    const talas::project::PreparedProjectSoundVariableIdentity workspaceIdentity{
        talas::project::ProjectVariableOwnerKind::WorkspaceSession,
        "workspace",
    };
    const std::array assignments{
        talas::runtime::ProjectSoundVariableAssignment{projectIdentity, "toneB"},
        talas::runtime::ProjectSoundVariableAssignment{workspaceIdentity, "toneC"},
    };
    require(
        bridge.scheduleSoundVariableAssignments(assignments, generation) ==
                talas::runtime::SnapshotDirectTransactionResult::Accepted &&
            render(bridge, output),
        "exact project/workspace Sound selections should commit in one "
        "callback transaction"
    );
    auto publication = bridge.preparedSoundVariableValues();
    require(
        publication.publicationValid && publication.values.size() == 2 &&
            publication.values[0].selectedDefinitionId == "toneB" &&
            publication.values[1].selectedDefinitionId == "toneC",
        "bridge should publish exact selected definition IDs for the "
        "complete Sound batch"
    );

    require(
        bridge.compileAndStage(settings()).ok() && render(bridge, output),
        "compatible Sound-selection restage should activate"
    );
    publication = bridge.preparedSoundVariableValues();
    require(
        publication.publicationValid && publication.stageId == bridge.activeStageId() &&
            publication.values[0].selectedDefinitionId == "toneB" &&
            publication.values[1].selectedDefinitionId == "toneC",
        "compatible restage should transfer Sound selections by exact "
        "definition ID"
    );

    const auto replacementGeneration = bridge.latestStageGeneration();
    require(
        !bridge.scheduleSoundVariableAssignment(projectIdentity, "toneA", 0, generation) &&
            bridge.resetAllSoundVariables(replacementGeneration) ==
                talas::runtime::SnapshotDirectTransactionResult::Accepted &&
            render(bridge, output),
        "stale Sound generation should reject and reset-all should restore "
        "declaration selections"
    );
    publication = bridge.preparedSoundVariableValues();
    require(
        publication.values[0].selectedDefinitionId == "toneA" &&
            publication.values[1].selectedDefinitionId == "toneA",
        "Sound reset-all should restore each declaration's exact initial "
        "candidate"
    );

    require(
        bridge.scheduleSoundVariableAssignment(
            projectIdentity,
            "toneB",
            0,
            replacementGeneration)
            && render(bridge, output),
        "project Sound selection should be current before capture");
    const auto captureAuthority =
        bridge.editor().documentAuthorityGeneration();
    auto nestedIdentity = projectIdentity;
    nestedIdentity.voiceAncestors.push_back({"parent", {}, {}, 0});
    require(!bridge.captureProjectSoundVariableInitialSelection(
                nestedIdentity, replacementGeneration, captureAuthority).ok()
            && bridge.editor().documentAuthorityGeneration() == captureAuthority,
        "a projected Sound identity cannot capture a project declaration or create an edit");
    require(
        bridge.captureProjectSoundVariableInitialSelection(
            projectIdentity,
            replacementGeneration,
            captureAuthority)
            .ok(),
        "capture should commit the coherent project-owned live Sound "
        "selection");
    require(
        bridge.editor().document().soundVariableLibrary.variables.front()
                .initialSelectedOrdinal
            == 1,
        "capture should retain the exact selected definition as the new "
        "project initial selection");
    require(
        !bridge.captureProjectSoundVariableInitialSelection(
             projectIdentity,
             replacementGeneration,
             captureAuthority)
             .ok()
            && !bridge.captureProjectSoundVariableInitialSelection(
                    projectIdentity,
                    generation,
                    bridge.editor().documentAuthorityGeneration())
                    .ok()
            && !bridge.captureProjectSoundVariableInitialSelection(
                    workspaceIdentity,
                    replacementGeneration,
                    bridge.editor().documentAuthorityGeneration())
                    .ok(),
        "capture should reject stale document/stage authority and "
        "non-project ownership");
}

void testBridgeTreatsEmptySoundCollectionResetAsExactNoOp()
{
    auto document = makePlayableDocument(220.0);
    talas::project::ProjectSoundVariableDeclaration emptyCollection;
    emptyCollection.variableId = "emptyCollection";
    emptyCollection.displayName = "Empty Collection";
    emptyCollection.kind =
        talas::project::ProjectSoundVariableKind::SoundCollection;
    document.soundVariableLibrary.variables.push_back(
        std::move(emptyCollection));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "empty SoundCollection bridge fixture should stage");
    std::array<float, 4> output{};
    require(
        render(bridge, output),
        "empty SoundCollection bridge fixture should activate");
    const auto generation = bridge.latestStageGeneration();
    const talas::project::PreparedProjectSoundVariableIdentity identity{
        talas::project::ProjectVariableOwnerKind::Project,
        "emptyCollection",
    };
    require(
        bridge.resetSoundVariables(
            std::span<const talas::project::PreparedProjectSoundVariableIdentity>{
                &identity,
                1},
            generation)
            == talas::runtime::SnapshotDirectTransactionResult::Accepted,
        "resetting an empty SoundCollection should be an exact accepted no-op");
    require(
        bridge.resetAllSoundVariables(generation)
            == talas::runtime::SnapshotDirectTransactionResult::Accepted,
        "reset-all should accept an empty SoundCollection without indexing a candidate");
    const auto publication = bridge.preparedSoundVariableValues();
    require(
        publication.publicationValid && publication.values.size() == 1
            && publication.values.front().identity == identity
            && publication.values.front().selectedDefinitionId.empty(),
        "empty SoundCollection publication should retain an explicit empty selection");
}

void testArbitraryDocumentStageCannotInheritEditorVariableHistory()
{
    auto document = makePlayableDocument(240.0);
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    const auto oscillator = std::ranges::find(
        document.graph.nodes,
        std::string{"osc"},
        &talas::project::ProjectNode::instanceId);
    require(oscillator != document.graph.nodes.end(),
        "cross-document variable fixture should contain its oscillator");
    oscillator->parameters.front().expression = "@pitch";
    document.variableLibrary.variables.push_back({
        "pitch", "Pitch", talas::expr::ValueDomain::Hertz, 240.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}}, true,
    });
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(),
        "editor-document variable fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output),
        "editor-document variable fixture should activate");
    const talas::project::PreparedProjectVariableIdentity identity{
        talas::project::ProjectVariableOwnerKind::Project,
        "pitch", {}, {},
    };
    require(
        bridge.scheduleVariableAssignment(
            identity, 120.0, talas::expr::ValueDomain::Hertz, 0,
            bridge.latestStageGeneration())
            && render(bridge, output),
        "editor-document variable history fixture should assign one live value");

    auto other = document;
    other.variableLibrary.variables.front().initialValue = 360.0;
    require(
        bridge.compileAndStageDocument(other, settings()).ok()
            && render(bridge, output),
        "arbitrary replacement document should stage independently");
    const auto otherValues = bridge.preparedVariableValues();
    require(
        otherValues.publicationValid
            && otherValues.values.size() == 1
            && otherValues.values.front().value
                == talas::expr::ExpressionValue{
                    360.0, talas::expr::ValueDomain::Hertz},
        "arbitrary document staging must start from its own declared value even when stable IDs match");

    require(
        bridge.compileAndStage(settings()).ok()
            && render(bridge, output),
        "returning to the editor document should stage independently");
    const auto editorValues = bridge.preparedVariableValues();
    require(
        editorValues.publicationValid
            && editorValues.values.size() == 1
            && editorValues.values.front().value
                == talas::expr::ExpressionValue{
                    240.0, talas::expr::ValueDomain::Hertz},
        "an external staged document must not seed later editor-document variable history");
}

void testBridgeBindsVoiceVariablesToExactSlotGeneration()
{
    auto document = makePolyphonicMidiVoiceDocument(1, 1, "1");
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    auto oscillator = std::ranges::find(
        document.graph.nodes,
        std::string{"osc"},
        &talas::project::ProjectNode::instanceId);
    require(oscillator != document.graph.nodes.end(),
        "voice-variable bridge fixture should contain its oscillator");
    auto amplitude = std::ranges::find(
        oscillator->parameters,
        std::string{"amplitude"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(amplitude != oscillator->parameters.end(),
        "voice-variable bridge fixture should expose amplitude");
    talas::project::ProjectVariableDeclaration variable;
    variable.variableId = "voiceGain";
    variable.displayName = "Voice gain";
    variable.initialValue = 0.25;
    variable.snapshotEligible = false;
    variable.owner.kind =
        talas::project::ProjectVariableOwnerKind::MidiVoice;
    variable.owner.nodeId = "midi";
    document.variableLibrary.variables.push_back(variable);

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    auto voiceSettings = settings();
    voiceSettings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto staged = bridge.compileAndStage(voiceSettings);
    require(staged.ok(),
        "voice-variable bridge fixture should stage: "
            + staged.message
            + (staged.diagnostics.empty()
                    ? std::string{}
                    : " / " + staged.diagnostics.front().message));
    std::array<float, 4> output{};
    require(render(bridge, output),
        "voice-variable bridge fixture should activate");
    const auto initial = bridge.preparedVariableValues();
    require(initial.publicationValid && initial.values.size() == 1,
        "voice-variable bridge fixture should publish one prepared slot");
    const auto identity = initial.values.front().identity;
    require(identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice
            && identity.voiceOrdinal == std::optional<std::uint16_t>{0}
            && initial.values.front().voiceGeneration == 0,
        "inactive voice telemetry should retain exact identity without inventing a generation");
    require(!bridge.scheduleVariableAssignment(
                identity, 0.5, talas::expr::ValueDomain::Scalar, 0,
                bridge.latestStageGeneration()),
        "generic variable API must not write a voice-owned cell");

    require(bridge.enqueueMidiNoteOn(1, 60, 127, 1.0)
            && render(bridge, output),
        "note-on should allocate the prepared voice slot");
    const auto allocated = bridge.preparedVariableValues();
    require(allocated.publicationValid
                && allocated.values.front().voiceGeneration != 0
                && allocated.values.front().value.value == 0.25
                && allocated.values.front().midiChannel == 1
                && allocated.values.front().midiNote == 60
                && allocated.values.front().midiVelocity == 127
                && allocated.values.front().voiceActive
                && !allocated.values.front().voiceReleasing,
        "allocation should publish a nonzero generation and reset its exact cell range");
    const auto firstGeneration =
        allocated.values.front().voiceGeneration;
    require(bridge.scheduleVoiceVariableAssignment(
                identity, firstGeneration, 0.75,
                talas::expr::ValueDomain::Scalar, 0,
                bridge.latestStageGeneration())
            && render(bridge, output),
        "exact live slot generation should admit one voice assignment");
    require(bridge.preparedVariableValues().values.front().value.value
                == 0.75,
        "accepted voice assignment should publish its exact live value");

    require(bridge.enqueueMidiNoteOff(1, 60, 2.0)
            && render(bridge, output),
        "note-off should enter the release tail");
    const auto released = bridge.preparedVariableValues();
    require(released.values.front().voiceGeneration == firstGeneration
                && released.values.front().value.value == 0.75
                && !released.values.front().voiceActive
                && released.values.front().voiceReleasing,
        "release tail should retain both slot generation and voice cells");

    require(bridge.enqueueMidiNoteOn(1, 61, 127, 3.0)
            && render(bridge, output),
        "retrigger should reuse the released voice slot");
    const auto reused = bridge.preparedVariableValues();
    require(reused.values.front().voiceGeneration != firstGeneration
                && reused.values.front().value.value == 0.25,
        "reuse should advance generation and restore declaration initial values");
    require(!bridge.scheduleVoiceVariableAssignment(
                identity, firstGeneration, 1.0,
                talas::expr::ValueDomain::Scalar, 0,
                bridge.latestStageGeneration()),
        "stale voice generation must not rebind after slot reuse");
    require(bridge.compileAndStage(voiceSettings).ok()
                && render(bridge, output),
        "compatible voice-variable restage should remain stageable");
    const auto restaged = bridge.preparedVariableValues();
    require(restaged.publicationValid
                && restaged.values.front().voiceGeneration == 0
                && restaged.values.front().value.value == 0.25,
        "voice values must not transfer across prepared stage replacement");
}

void testBridgeMetricsUseNonDestructiveEpochBaselines()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(1.0),
        registry());
    auto metrics = bridge.realtimeMetrics();
    require(
        metrics.epoch == 1 && metrics.activeStageId == 0
            && metrics.renderCallbacks == 0
            && metrics.renderFailures == 0,
        "new bridge metrics should expose an empty initial epoch");

    std::array<float, 4> output{};
    require(
        !render(bridge, output),
        "bridge without a staged graph should fail closed");
    metrics = bridge.realtimeMetrics();
    require(
        metrics.epoch == 1 && metrics.renderCallbacks == 1
            && metrics.renderFailures == 1,
        "bridge metrics should compose graph callback failures");

    bridge.resetRealtimeMetrics();
    metrics = bridge.realtimeMetrics();
    require(
        metrics.epoch == 2 && metrics.renderCallbacks == 0
            && metrics.renderFailures == 0,
        "metrics reset should advance the baseline without resetting producers");

    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "metrics stage fixture should compile");
    std::size_t allocations = 0;
    require(
        renderAndCount(bridge, output, allocations),
        "metrics stage fixture should adopt and render");
    require(allocations == 0, "metrics producers must not allocate in render");
    metrics = bridge.realtimeMetrics();
    require(
        metrics.epoch == 3 && metrics.activeStageId != 0
            && metrics.renderCallbacks == 0
            && metrics.renderFailures == 0,
        "first observation of an adopted stage should begin a clean epoch");

    require(render(bridge, output), "adopted metrics graph should keep rendering");
    metrics = bridge.realtimeMetrics();
    require(
        metrics.epoch == 3 && metrics.renderCallbacks == 1
            && metrics.renderFailures == 0,
        "same-stage callbacks should accumulate after the stage baseline");
}

void testFailedCompilePreservesActiveGraph()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "fixture graph should stage");

    std::array<float, 4> before{};
    require(render(bridge, before), "fixture graph should activate and render");
    require(hasAudibleSample(before), "fixture graph should be audible");

    require(bridge.editor().disconnect("out", "audioIn").ok(), "editor should permit temporarily incomplete graph");
    auto failedSettings = settings();
    failedSettings.eventValues.push_back({"Uncompiled", 0.5, talas::expr::ValueDomain::Scalar});
    const auto failed = bridge.compileAndStage(std::move(failedSettings));
    require(!failed.ok(), "incomplete graph should not compile");
    require(failed.outcome == talas::runtime::ProjectStageOutcome::CompileFailed,
            "compile diagnostics should be reported distinctly");
    require(!failed.diagnostics.empty(), "failed compile should preserve diagnostics");
    require(!bridge.realtimeEventValueId("Uncompiled").has_value(),
            "failed compilation should not publish Event Value registry entries");

    std::array<float, 4> after{};
    require(render(bridge, after), "failed compile must leave active graph rendering");
    require(hasAudibleSample(after), "active graph should remain audible after failed compile");
}

void testResourceHistoryAllowsOnlyTableBinaryCrossKindChanges()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(1.0), registry());
    const auto tableOne = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Table, 1, '1');
    require(
        bridge.compileAndStage(resourceSettings(tableOne)).ok(),
        "initial Table resource identity should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(
        render(bridge, output),
        "initial Table resource graph should adopt before replacement");

    const auto audioFromTable = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 2, '9');
    require(
        bridge.compileAndStage(resourceSettings(audioFromTable)).outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Table-to-Audio history must reject at a higher generation");
    const auto preservedTable = bridge.acceptedResourceIdentity(tableOne->id);
    require(
        preservedTable.has_value()
            && preservedTable->kind
                == talas::core::ResourceSnapshotKind::Table
            && preservedTable->generation == 1
            && preservedTable->contentHash == tableOne->contentHash,
        "rejected Table-to-Audio history must preserve the prior identity");

    const auto binaryTwo = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Binary, 2, '2');
    const auto binaryStage = bridge.compileAndStage(
        resourceSettings(binaryTwo));
    require(
        binaryStage.ok(),
        "higher-generation Table-to-Binary identity should remain allowed: "
            + binaryStage.message);
    require(
        render(bridge, output),
        "Table-to-Binary replacement should adopt before another stage");
    const auto acceptedBinary = bridge.acceptedResourceIdentity(
        binaryTwo->id);
    require(
        acceptedBinary.has_value()
            && acceptedBinary->kind
                == talas::core::ResourceSnapshotKind::Binary
            && acceptedBinary->generation == 2
            && acceptedBinary->contentHash == binaryTwo->contentHash,
        "accepted history should record the exact Binary replacement");

    const auto audioThree = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 3, '3');
    const auto rejectedAudio = bridge.compileAndStage(
        resourceSettings(audioThree));
    require(
        rejectedAudio.outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Binary-to-Audio history must reject until explicit replacement policy exists");
    const auto preservedBinary = bridge.acceptedResourceIdentity(
        binaryTwo->id);
    require(
        preservedBinary.has_value()
            && preservedBinary->kind
                == talas::core::ResourceSnapshotKind::Binary
            && preservedBinary->generation == 2
            && preservedBinary->contentHash == binaryTwo->contentHash,
        "rejected Audio cross-kind history must preserve the prior identity");

    const auto tableThree = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Table, 3, '7');
    require(
        bridge.compileAndStage(resourceSettings(tableThree)).ok(),
        "higher-generation Binary-to-Table identity should remain allowed");
    require(
        render(bridge, output),
        "Binary-to-Table replacement should adopt before another stage");
    const auto acceptedTable = bridge.acceptedResourceIdentity(tableThree->id);
    require(
        acceptedTable.has_value()
            && acceptedTable->kind
                == talas::core::ResourceSnapshotKind::Table
            && acceptedTable->generation == 3
            && acceptedTable->contentHash == tableThree->contentHash,
        "accepted history should record the exact Table replacement");

    talas::runtime::ProjectRuntimeBridge audioBridge(
        makePlayableDocument(1.0), registry());
    const auto audioOne = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 1, '4');
    require(
        audioBridge.compileAndStage(resourceSettings(audioOne)).ok(),
        "an initial Audio identity should enter the generic product ledger");
    audioBridge.setPlaying(true);
    require(
        render(audioBridge, output),
        "initial Audio identity graph should adopt before replacement");

    require(
        audioBridge.compileAndStage(resourceSettings(audioOne)).ok(),
        "same-generation identical Audio identity should remain idempotent");
    require(
        render(audioBridge, output),
        "idempotent Audio stage should adopt before another stage");

    const auto audioOneDrift = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 1, '8');
    require(
        audioBridge.compileAndStage(resourceSettings(audioOneDrift)).outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "same-generation Audio content drift must reject");
    const auto audioTwoSameContent = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 2, '4');
    require(
        audioBridge.compileAndStage(resourceSettings(audioTwoSameContent)).outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "higher-generation Audio identity without new content must reject");

    const auto audioTwo = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Audio, 2, '5');
    require(
        audioBridge.compileAndStage(resourceSettings(audioTwo)).ok(),
        "same-kind higher-generation Audio replacement should remain allowed");
    require(
        render(audioBridge, output),
        "Audio-to-Audio replacement should adopt before another stage");

    require(
        audioBridge.compileAndStage(resourceSettings(audioOne)).outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Audio generation rollback must reject");

    const auto tableFromAudio = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Table, 3, '6');
    const auto rejectedTable = audioBridge.compileAndStage(
        resourceSettings(tableFromAudio));
    require(
        rejectedTable.outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Audio-to-Table history must reject until explicit replacement policy exists");
    const auto binaryFromAudio = resourceSnapshot(
        talas::core::ResourceSnapshotKind::Binary, 3, '7');
    require(
        audioBridge.compileAndStage(resourceSettings(binaryFromAudio)).outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "Audio-to-Binary history must reject at a higher generation");
    const auto preservedAudio = audioBridge.acceptedResourceIdentity(
        audioTwo->id);
    require(
        preservedAudio.has_value()
            && preservedAudio->kind
                == talas::core::ResourceSnapshotKind::Audio
            && preservedAudio->generation == 2
            && preservedAudio->contentHash == audioTwo->contentHash,
        "rejected Table cross-kind history must preserve the prior Audio identity");
}

void testFailedIsolatedDocumentStagePreservesActiveGraphAndEditorDocument()
{
    auto source = makePlayableDocument(1.0);
    const auto sourceNodeCount = source.graph.nodes.size();
    talas::runtime::ProjectRuntimeBridge bridge(std::move(source), registry());
    require(bridge.compileAndStage(settings()).ok(), "isolated-stage fixture should stage its project");
    bridge.setPlaying(true);

    std::array<float, 4> first{};
    require(render(bridge, first), "isolated-stage fixture should activate and render");

    talas::project::ProjectDocument invalidPreview;
    invalidPreview.graph.nodes.push_back({"missing", "talas.notRegistered", {}, {}});
    const auto rejected = bridge.compileAndStageDocument(
        invalidPreview,
        settings());
    require(
        rejected.outcome == talas::runtime::ProjectStageOutcome::CompileFailed,
        "invalid isolated document should fail before graph staging");
    require(
        bridge.editor().document().graph.nodes.size() == sourceNodeCount,
        "isolated staging must not replace the editable project document");

    auto invalidProfile = makePlayableDocument(
        1.0,
        "series(0.25, 0.25, 3)[1]");
    invalidProfile.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV2;
    const auto profileRejected = bridge.compileAndStageDocument(
        invalidProfile,
        settings());
    require(
        profileRejected.outcome
            == talas::runtime::ProjectStageOutcome::CompileFailed
            && bridge.editor().document().graph.nodes.size()
                == sourceNodeCount,
        "frozen PureV2 must reject PureV3 syntax without replacing editor or stage authority");

    std::array<float, 4> after{};
    require(render(bridge, after), "active graph should keep rendering after isolated stage failure");
    require(
        std::ranges::any_of(after, [](float sample) {
            return std::isfinite(sample) && std::abs(sample) > 1.0e-6F;
        }),
        "failed isolated staging must leave the previously active prepared graph audible");
}

void testMissingComponentStageFailsBeforeReplacementAndPreservesActiveGraph()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(1.0),
        registry());
    require(
        bridge.compileAndStage(settings()).ok(),
        "missing-component rollback fixture should stage its valid generation");
    bridge.setPlaying(true);

    std::array<float, 4> before{};
    require(
        render(bridge, before) && hasAudibleSample(before),
        "missing-component rollback fixture should activate audibly");

    const auto failed = bridge.compileAndStageDocument(
        makeMissingComponentDocument(),
        settings());
    require(
        failed.outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed
            && !failed.diagnostics.empty()
            && failed.diagnostics.front().nodeId == "missing1"
            && failed.diagnostics.front().message.find(
                   "org.example.component")
                != std::string::npos,
        "preserved missing component should fail with its scoped execution diagnostic before staging");

    std::array<float, 4> after{};
    require(
        render(bridge, after) && hasAudibleSample(after),
        "failed missing-component replacement must leave the active prepared generation audible");
}

void testPendingStageDoesNotReplaceFirstPreparedGraph()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "first graph should stage");
    require(bridge.editor().setParameter("osc", expressionParameter("frequency", "2 hz")).ok(),
            "editor should update staged successor source document");

    const auto pending = bridge.compileAndStage(settings());
    require(!pending.ok(), "second graph should wait while the first is pending activation");
    require(pending.outcome == talas::runtime::ProjectStageOutcome::StageRejected,
            "pending stage should report staging rejection");
    require(pending.stageResult == talas::runtime::RealtimeGraphStageResult::PendingEngineAwaitingActivation,
            "pending stage should preserve exact host result");

    std::array<float, 4> output{};
    require(render(bridge, output), "first pending graph should activate and render");
    require(output[1] > 0.9F, "first staged 1 Hz graph should remain pending graph");
}

void testRejectedStageDoesNotPublishRealtimeEventIds()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    require(bridge.compileAndStage(settings()).ok(), "registry rollback fixture should stage first graph");

    auto rejectedSettings = settings();
    rejectedSettings.eventValues.push_back({"NeverStaged", 0.5, talas::expr::ValueDomain::Scalar});
    const auto rejected = bridge.compileAndStage(std::move(rejectedSettings));
    require(rejected.outcome == talas::runtime::ProjectStageOutcome::StageRejected,
            "second pending graph should reject staging");
    require(!bridge.realtimeEventValueId("NeverStaged").has_value(),
            "stage rejection must not publish Event Value IDs");
}

void testRegistryConflictDoesNotPartiallyPublishIds()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);
    auto initialSettings = settings();
    initialSettings.eventValues.push_back({"Legacy", 1.0, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(initialSettings)).ok(), "registry conflict fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output), "registry conflict fixture should activate");

    auto conflictingSettings = settings();
    conflictingSettings.eventValues.push_back({"PartiallyPublished", 1.0, talas::expr::ValueDomain::Scalar});
    conflictingSettings.eventValues.push_back({"Legacy", 440.0, talas::expr::ValueDomain::Hertz});
    const auto conflict = bridge.compileAndStage(std::move(conflictingSettings));
    require(conflict.outcome == talas::runtime::ProjectStageOutcome::EngineCreationFailed,
            "registry domain conflict should reject before staging");
    require(!bridge.realtimeEventValueId("PartiallyPublished").has_value(),
            "registry conflict must not partially publish earlier bindings");
}

void testLatestStageEventLookupExcludesHistoricalAndRejectedBindings()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);
    auto firstSettings = settings();
    firstSettings.eventValues.push_back({"Legacy", 1.0, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(firstSettings)).ok(), "latest-stage first graph should stage");
    require(bridge.realtimeEventValueIdForLatestStage("Legacy", talas::expr::ValueDomain::Scalar).has_value(),
            "first successful stage should publish latest-stage membership");
    std::array<float, 4> output{};
    require(render(bridge, output), "latest-stage first graph should activate");

    auto secondSettings = settings();
    secondSettings.eventValues.push_back({"Current", 1.0, talas::expr::ValueDomain::Scalar});
    require(bridge.compileAndStage(std::move(secondSettings)).ok(), "latest-stage replacement should stage");
    require(!bridge.realtimeEventValueIdForLatestStage("Legacy", talas::expr::ValueDomain::Scalar).has_value(),
            "historical registry ID should not belong to latest stage");
    require(bridge.realtimeEventValueIdForLatestStage("Current", talas::expr::ValueDomain::Scalar).has_value(),
            "replacement binding should belong to latest stage");

    auto rejectedSettings = settings();
    rejectedSettings.eventValues.push_back({"NeverActive", 1.0, talas::expr::ValueDomain::Scalar});
    require(!bridge.compileAndStage(std::move(rejectedSettings)).ok(), "pending third graph should reject staging");
    require(!bridge.realtimeEventValueIdForLatestStage("NeverActive", talas::expr::ValueDomain::Scalar).has_value(),
            "rejected binding should not enter latest-stage membership");
    require(bridge.realtimeEventValueIdForLatestStage("Current", talas::expr::ValueDomain::Scalar).has_value(),
            "rejected stage should preserve prior latest-stage membership");
}

void testRetiredEngineIsCollectedExplicitly()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "first graph should stage");

    std::array<float, 4> output{};
    require(render(bridge, output), "first graph should activate");
    require(bridge.editor().setParameter("osc", expressionParameter("frequency", "2 hz")).ok(),
            "editor should update replacement graph");
    require(bridge.compileAndStage(settings()).ok(), "replacement graph should stage after first activation");
    require(render(bridge, output), "replacement graph should activate");

    require(bridge.collectRetired(), "retired first engine should be collected on the control thread");
    require(!bridge.collectRetired(), "retired engine collection should be idempotent after collection");
}

void testParameterTransformerStageReplacementAndRejectedCandidateAreAtomic()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeParameterTransformerBridgeDocument(
            "set frequency = 1 hz"),
        registry());
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settings()).ok(),
        "initial transformed bridge graph should stage");
    std::array<float, 4> output{};
    require(
        render(bridge, output) && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "initial transformed bridge graph should render its rewritten frequency");

    auto replacement = makeParameterTransformerBridgeDocument(
        "set frequency = 0 hz");
    require(
        bridge.compileAndStageDocument(replacement, settings()).ok()
            && render(bridge, output),
        "replacement transformed graph should stage and activate");
    require(
        std::ranges::all_of(output, [](float sample) {
            return std::abs(sample) < 1.0e-6F;
        }),
        "replacement transformed graph should use its own prepared rewrite");
    require(
        bridge.collectRetired(),
        "replacement should retire the prior transformed engine");

    auto rejected = replacement;
    rejected.graph.connections.push_back(
        {"osc", "audioOut", "out", "rightIn"});
    require(
        !bridge.compileAndStageDocument(rejected, settings()).ok(),
        "shared upstream transformed candidate should reject before staging");
    require(
        render(bridge, output)
            && std::ranges::all_of(output, [](float sample) {
                return std::abs(sample) < 1.0e-6F;
            }),
        "rejected transformed candidate must preserve the old active graph");
}

void testC44DynamicUtilitiesRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeDynamicControlUtilitiesBridgeDocument();
    const auto renderSettings = dynamicControlUtilitiesBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C44 bridge and reference should stage dynamic utilities");

    std::array<float, 4> first{};
    std::array<float, 4> referenceFirst{};
    std::size_t allocations = 0;
    require(
        renderAndCount(bridge, first, allocations)
            && allocations == 0
            && renderAndCount(reference, referenceFirst, allocations)
            && allocations == 0
            && first == referenceFirst
            && first[0] == 0.0F
            && std::abs(first[1]) > 0.5F
            && first[2] == 0.0F
            && std::abs(first[3]) > 0.5F,
        "C44 bridge should preserve exact sample-boundary utility controls without allocation");

    const auto firstStage = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridge replacementReference(
        document, registry());
    replacementReference.setPlaying(true);
    require(
        replacementReference.compileAndStage(renderSettings).ok()
            && replacementReference.requestTransportRelocation(4),
        "C44 replacement reference should stage at the inherited transport frame");
    std::array<float, 4> expectedRestaged{};
    require(
        renderAndCount(
            replacementReference, expectedRestaged, allocations)
            && allocations == 0,
        "C44 replacement reference should render without allocation");
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C44 bridge should restage the same dynamic utility document");
    std::array<float, 4> restaged{};
    require(
        renderAndCount(bridge, restaged, allocations)
            && allocations == 0
            && restaged == expectedRestaged,
        "C44 restage should reconstruct dynamic utility state at the inherited transport frame under a new stage");
    require(
        bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C44 restage should adopt a new stage and retire the old engine");

    auto rejected = document;
    const auto quantizer = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"quantizer"},
        &talas::project::ProjectNode::instanceId);
    require(
        quantizer != rejected.graph.nodes.end(),
        "C44 rejected replacement fixture should expose Quantizer");
    const auto stepSize = std::ranges::find(
        quantizer->parameters,
        std::string_view{"stepSize"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        stepSize != quantizer->parameters.end(),
        "C44 rejected replacement fixture should expose stepSize");
    stepSize->expression = "0";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C44 invalid Quantizer replacement should reject before adoption");

    std::array<float, 4> afterRejected{};
    std::array<float, 4> referenceAfterRejected{};
    require(
        renderAndCount(bridge, afterRejected, allocations)
            && allocations == 0
            && renderAndCount(
                replacementReference,
                referenceAfterRejected,
                allocations)
            && allocations == 0
            && afterRejected == referenceAfterRejected,
        "C44 rejected replacement should preserve the active stage and its exact state");
}

void testC45DynamicSlewRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeDynamicSlewTimesBridgeDocument();
    const auto renderSettings = dynamicSlewTimesBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C45 bridge and reference should stage dynamic Slew times");

    std::array<float, 4> first{};
    std::array<float, 4> referenceFirst{};
    std::size_t allocations = 0;
    require(
        renderAndCount(bridge, first, allocations)
            && allocations == 0
            && renderAndCount(reference, referenceFirst, allocations)
            && allocations == 0
            && first == referenceFirst
            && first[0] == 0.0F
            && first[1] > 0.0F
            && std::abs(first[2]) > std::abs(first[1])
            && first[3] == 0.0F,
        "C45 bridge should preserve exact scheduled Slew times without allocation");

    const auto firstStage = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridge replacementReference(
        document, registry());
    replacementReference.setPlaying(true);
    require(
        replacementReference.compileAndStage(renderSettings).ok()
            && replacementReference.requestTransportRelocation(4),
        "C45 replacement reference should stage at the inherited transport frame");
    std::array<float, 4> expectedRestaged{};
    require(
        renderAndCount(
            replacementReference, expectedRestaged, allocations)
            && allocations == 0,
        "C45 replacement reference should render without allocation");
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C45 bridge should restage the same dynamic Slew document");
    std::array<float, 4> restaged{};
    require(
        renderAndCount(bridge, restaged, allocations)
            && allocations == 0
            && restaged == expectedRestaged,
        "C45 restage should reconstruct Slew state at the inherited transport frame under a new stage");
    require(
        bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C45 restage should adopt a new stage and retire the old engine");

    auto rejected = document;
    const auto slew = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"slew"},
        &talas::project::ProjectNode::instanceId);
    require(
        slew != rejected.graph.nodes.end(),
        "C45 rejected replacement fixture should expose Slew");
    const auto riseTime = std::ranges::find(
        slew->parameters,
        std::string_view{"riseTime"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        riseTime != slew->parameters.end(),
        "C45 rejected replacement fixture should expose riseTime");
    riseTime->expression = "-1 s";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C45 invalid Slew replacement should reject before adoption");

    std::array<float, 4> afterRejected{};
    std::array<float, 4> referenceAfterRejected{};
    require(
        renderAndCount(bridge, afterRejected, allocations)
            && allocations == 0
            && renderAndCount(
                replacementReference,
                referenceAfterRejected,
                allocations)
            && allocations == 0
            && afterRejected == referenceAfterRejected,
        "C45 rejected replacement should preserve the active stage and its exact state");
}

void testC46DynamicClockRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeDynamicClockParametersBridgeDocument();
    const auto renderSettings = dynamicClockParametersBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C46 bridge and reference should stage dynamic Clock parameters");

    std::array<float, 4> first{};
    std::array<float, 4> referenceFirst{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, first, allocations)
            && allocations == 0
            && renderAndCount(reference, referenceFirst, allocations)
            && allocations == 0
            && first == referenceFirst
            && first[0] == 0.0F
            && first[1] > 0.5F
            && first[2] > 0.5F
            && first[3] > 0.5F,
        "C46 bridge should preserve exact phase-continuous Clock automation without allocation");

    const auto firstStage = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridge replacementReference(
        document, registry());
    replacementReference.setPlaying(true);
    require(replacementReference.compileAndStage(renderSettings).ok()
            && replacementReference.requestTransportRelocation(4),
        "C46 replacement reference should stage at the inherited transport frame");
    std::array<float, 4> expectedRestaged{};
    require(renderAndCount(
                replacementReference, expectedRestaged, allocations)
            && allocations == 0,
        "C46 replacement reference should render without allocation");
    require(bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C46 bridge should restage the same dynamic Clock document");
    std::array<float, 4> restaged{};
    require(renderAndCount(bridge, restaged, allocations)
            && allocations == 0
            && restaged == expectedRestaged,
        "C46 restage should rebase Clock phase at inherited transport under a new stage");
    require(bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C46 restage should adopt a new stage and retire the old engine");

    auto rejected = document;
    const auto clock = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"clock"},
        &talas::project::ProjectNode::instanceId);
    require(clock != rejected.graph.nodes.end(),
        "C46 rejected replacement fixture should expose Clock");
    const auto frequency = std::ranges::find(
        clock->parameters,
        std::string_view{"frequency"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(frequency != clock->parameters.end(),
        "C46 rejected replacement fixture should expose frequency");
    frequency->expression = "0 hz";
    require(!bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C46 invalid Clock replacement should reject before adoption");

    std::array<float, 4> afterRejected{};
    std::array<float, 4> referenceAfterRejected{};
    require(renderAndCount(bridge, afterRejected, allocations)
            && allocations == 0
            && renderAndCount(
                replacementReference,
                referenceAfterRejected,
                allocations)
            && allocations == 0
            && afterRejected == referenceAfterRejected,
        "C46 rejected replacement should preserve the active stage and Clock state");
}

void testC47DynamicClockDividerRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeDynamicClockDividerParametersBridgeDocument();
    const auto renderSettings = dynamicClockDividerParametersBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C47 bridge and reference should stage dynamic Divider parameters");

    std::array<float, 4> first{};
    std::array<float, 4> referenceFirst{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, first, allocations)
            && allocations == 0
            && renderAndCount(reference, referenceFirst, allocations)
            && allocations == 0
            && first == referenceFirst,
        "C47 bridge should preserve exact Divider state without allocation");

    const auto firstStage = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridge replacementReference(
        document, registry());
    replacementReference.setPlaying(true);
    require(replacementReference.compileAndStage(renderSettings).ok()
            && replacementReference.requestTransportRelocation(4),
        "C47 replacement reference should stage at the inherited transport frame");
    std::array<float, 4> expectedRestaged{};
    require(renderAndCount(
                replacementReference, expectedRestaged, allocations)
            && allocations == 0,
        "C47 replacement reference should render without allocation");
    require(bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C47 bridge should restage the same dynamic Divider document");
    std::array<float, 4> restaged{};
    require(renderAndCount(bridge, restaged, allocations)
            && allocations == 0
            && restaged == expectedRestaged,
        "C47 restage should reconstruct Divider state at inherited transport under a new stage");
    require(bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C47 restage should adopt a new stage and retire the old engine");

    auto rejected = document;
    const auto divider = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"divider"},
        &talas::project::ProjectNode::instanceId);
    require(divider != rejected.graph.nodes.end(),
        "C47 rejected replacement fixture should expose Divider");
    const auto division = std::ranges::find(
        divider->parameters,
        std::string_view{"division"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(division != divider->parameters.end(),
        "C47 rejected replacement fixture should expose division");
    division->expression = "0";
    require(!bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C47 invalid Divider replacement should reject before adoption");

    std::array<float, 4> afterRejected{};
    std::array<float, 4> referenceAfterRejected{};
    require(renderAndCount(bridge, afterRejected, allocations)
            && allocations == 0
            && renderAndCount(
                replacementReference,
                referenceAfterRejected,
                allocations)
            && allocations == 0
            && afterRejected == referenceAfterRejected,
        "C47 rejected replacement should preserve the active stage and Divider state");
}

void testC49LiveChannellerRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeLiveChannellerSelectorsBridgeDocument();
    const auto renderSettings = liveChannellerSelectorsBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C49 bridge and reference should stage live Channeller selectors");

    std::array<float, 4> firstLeft{};
    std::array<float, 4> firstRight{};
    std::array<float, 4> referenceLeft{};
    std::array<float, 4> referenceRight{};
    std::size_t allocations = 0;
    require(
        renderStereoAndCount(
            bridge, firstLeft, firstRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference, referenceLeft, referenceRight, allocations)
            && allocations == 0
            && firstLeft == referenceLeft
            && firstRight == referenceRight
            && firstLeft[0] == 0.0F
            && std::abs(firstLeft[1] - 0.5F) < 1.0e-6F
            && firstLeft[2] > 0.99F
            && std::abs(firstRight[1] - 0.5F) < 1.0e-6F
            && std::abs(firstRight[2]) < 1.0e-6F,
        "C49 bridge should preserve exact independent selector frames without allocation");

    const auto firstStage = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridge replacementReference(
        document, registry());
    replacementReference.setPlaying(true);
    require(
        replacementReference.compileAndStage(renderSettings).ok()
            && replacementReference.requestTransportRelocation(4),
        "C49 replacement reference should stage at inherited transport");
    std::array<float, 4> expectedLeft{};
    std::array<float, 4> expectedRight{};
    require(
        renderStereoAndCount(
            replacementReference,
            expectedLeft,
            expectedRight,
            allocations)
            && allocations == 0,
        "C49 replacement reference should render without allocation");
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C49 bridge should restage the same live-selector document");
    std::array<float, 4> restagedLeft{};
    std::array<float, 4> restagedRight{};
    require(
        renderStereoAndCount(
            bridge, restagedLeft, restagedRight, allocations)
            && allocations == 0
            && restagedLeft == expectedLeft
            && restagedRight == expectedRight,
        "C49 restage should reconstruct exact selector authority at inherited transport");
    require(
        bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C49 restage should adopt a new stage and retire the old engine");

    auto rejected = document;
    const auto channeller = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"channeller"},
        &talas::project::ProjectNode::instanceId);
    require(
        channeller != rejected.graph.nodes.end(),
        "C49 rejected replacement fixture should expose Channeller");
    const auto leftFallback = std::ranges::find(
        channeller->parameters,
        std::string_view{"leftChannel"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        leftFallback != channeller->parameters.end(),
        "C49 rejected replacement fixture should expose left fallback");
    leftFallback->boolean.reset();
    leftFallback->expression = "1";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C49 non-Boolean fallback replacement should reject before adoption");

    std::array<float, 4> afterRejectedLeft{};
    std::array<float, 4> afterRejectedRight{};
    std::array<float, 4> referenceAfterLeft{};
    std::array<float, 4> referenceAfterRight{};
    require(
        renderStereoAndCount(
            bridge,
            afterRejectedLeft,
            afterRejectedRight,
            allocations)
            && allocations == 0
            && renderStereoAndCount(
                replacementReference,
                referenceAfterLeft,
                referenceAfterRight,
                allocations)
            && allocations == 0
            && afterRejectedLeft == referenceAfterLeft
            && afterRejectedRight == referenceAfterRight,
        "C49 rejected replacement should preserve active selector routing");
}

void testC50LiveSpectrumModifierRestageAndRejectedReplacementAreAtomic()
{
    const auto document =
        makeLiveSpectrumModifierSelectorsBridgeDocument();
    const auto renderSettings =
        liveSpectrumModifierSelectorsBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    const auto stagedBridge = bridge.compileAndStage(renderSettings);
    const auto stagedReference = reference.compileAndStage(renderSettings);
    require(
        stagedBridge.ok() && stagedReference.ok(),
        "C51 bridge and reference should stage live Spectrum selectors and seed: "
            + stagedBridge.message + " / " + stagedReference.message
            + (stagedBridge.diagnostics.empty()
                ? std::string{}
                : " / " + stagedBridge.diagnostics.front().message)
            + (stagedReference.diagnostics.empty()
                ? std::string{}
                : " / " + stagedReference.diagnostics.front().message));

    std::array<float, 4> firstLeft{};
    std::array<float, 4> firstRight{};
    std::array<float, 4> referenceLeft{};
    std::array<float, 4> referenceRight{};
    std::size_t allocations = 0;
    require(
        renderStereoAndCount(
            bridge, firstLeft, firstRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference, referenceLeft, referenceRight, allocations)
            && allocations == 0
            && firstLeft == referenceLeft
            && firstRight == referenceRight
            && firstLeft[0] > 1.24F
            && firstRight[0] > 109.9F,
        "C51 bridge should render exact live-selector/seed Spectrum output without allocation");

    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C51 bridge should restage the same live-control document");
    std::array<float, 4> restagedLeft{};
    std::array<float, 4> restagedRight{};
    std::array<float, 4> expectedLeft{};
    std::array<float, 4> expectedRight{};
    require(
        renderStereoAndCount(
            bridge, restagedLeft, restagedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference, expectedLeft, expectedRight, allocations)
            && allocations == 0
            && restagedLeft == expectedLeft
            && restagedRight == expectedRight
            && bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C51 restage should atomically reconstruct exact selector/seed authority");

    auto rejected = document;
    const auto modifier = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"modifier"},
        &talas::project::ProjectNode::instanceId);
    require(
        modifier != rejected.graph.nodes.end(),
        "C50 rejected replacement fixture should expose Spectrum Modifier");
    const auto seedFallback = std::ranges::find(
        modifier->parameters,
        std::string_view{"seed"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        seedFallback != modifier->parameters.end(),
        "C51 rejected replacement fixture should expose seed fallback");
    seedFallback->expression = "1.5";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C51 out-of-range seed fallback replacement should reject before adoption");

    std::array<float, 4> afterRejectedLeft{};
    std::array<float, 4> afterRejectedRight{};
    std::array<float, 4> afterReferenceLeft{};
    std::array<float, 4> afterReferenceRight{};
    require(
        renderStereoAndCount(
            bridge,
            afterRejectedLeft,
            afterRejectedRight,
            allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference,
                afterReferenceLeft,
                afterReferenceRight,
                allocations)
            && allocations == 0
            && afterRejectedLeft == afterReferenceLeft
            && afterRejectedRight == afterReferenceRight,
        "C51 rejected replacement should preserve active Spectrum selector/seed routing");
}

void testC60SpectrumOnDiskDefaultsRestageAndRejectReplacementAtomically()
{
    const auto document = makeC60SpectrumOnDiskBridgeDocument();
    const auto renderSettings = c60SpectrumOnDiskBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C60 bridge and reference should stage default metadata and partial tail");

    std::array<float, 4> firstLeft{};
    std::array<float, 4> firstRight{};
    std::array<float, 4> referenceLeft{};
    std::array<float, 4> referenceRight{};
    std::size_t allocations = 0;
    require(
        renderStereoAndCount(
            bridge, firstLeft, firstRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference, referenceLeft, referenceRight, allocations)
            && allocations == 0
            && firstLeft == referenceLeft
            && firstRight == referenceRight
            && firstLeft.front() == 0.1F
            && firstRight.front() == 100.0F,
        "C60 bridge should publish the initial default scan frame without allocation");

    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C60 bridge should restage the same default scan document");
    talas::runtime::ProjectRuntimeBridge fresh(document, registry());
    fresh.setPlaying(true);
    require(
        fresh.compileAndStage(renderSettings).ok(),
        "C60 fresh default replacement reference should stage");
    std::array<float, 4> restagedLeft{};
    std::array<float, 4> restagedRight{};
    std::array<float, 4> freshLeft{};
    std::array<float, 4> freshRight{};
    require(
        renderStereoAndCount(
            bridge, restagedLeft, restagedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh, freshLeft, freshRight, allocations)
            && allocations == 0
            && restagedLeft == freshLeft
            && restagedRight == freshRight
            && restagedLeft.front() == 0.1F
            && restagedRight.front() == 100.0F
            && bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C60 default restage should reconstruct metadata, cursor, and trigger state exactly");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(
        source != rejected.graph.nodes.end(),
        "C60 rejected replacement fixture should expose Spectrum On Disk");
    const auto analysisFrames = std::ranges::find(
        source->parameters,
        std::string_view{"analysisFrames"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        analysisFrames != source->parameters.end(),
        "C60 rejected replacement fixture should expose analysisFrames");
    analysisFrames->expression = "0";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C60 invalid layout replacement should reject before adoption");

    std::array<float, 4> afterRejectedLeft{};
    std::array<float, 4> afterRejectedRight{};
    std::array<float, 4> afterFreshLeft{};
    std::array<float, 4> afterFreshRight{};
    require(
        renderStereoAndCount(
            bridge,
            afterRejectedLeft,
            afterRejectedRight,
            allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh,
                afterFreshLeft,
                afterFreshRight,
                allocations)
            && allocations == 0
            && afterRejectedLeft == afterFreshLeft
            && afterRejectedRight == afterFreshRight
            && afterRejectedLeft.front() == 0.5F
            && afterRejectedRight.front() == 200.0F,
        "C60 rejected replacement should preserve active default metadata and scan generation");
}

void testC61SumOfSinesDefaultsRestageAndRejectReplacementAtomically()
{
    const auto document = makeC61SumOfSinesBridgeDocument();
    const auto renderSettings = c61SumOfSinesBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C61 bridge and reference should stage independent analysis-frequency defaults");

    std::array<float, 4> firstLeft{};
    std::array<float, 4> firstRight{};
    std::array<float, 4> referenceLeft{};
    std::array<float, 4> referenceRight{};
    std::size_t allocations = 0;
    require(
        renderStereoAndCount(bridge, firstLeft, firstRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                reference, referenceLeft, referenceRight, allocations)
            && allocations == 0
            && firstLeft == referenceLeft
            && firstRight == referenceRight
            && firstLeft.front() == 0.0F
            && firstLeft[2] == 1.0F,
        "C61 bridge should publish the analytic 2 Hz phase sequence without allocation");

    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C61 bridge should restage the same sentinel document");
    talas::runtime::ProjectRuntimeBridge fresh(document, registry());
    fresh.setPlaying(true);
    require(fresh.compileAndStage(renderSettings).ok(),
            "C61 fresh replacement reference should stage");
    std::array<float, 4> restagedLeft{};
    std::array<float, 4> restagedRight{};
    std::array<float, 4> freshLeft{};
    std::array<float, 4> freshRight{};
    require(
        renderStereoAndCount(
            bridge, restagedLeft, restagedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh, freshLeft, freshRight, allocations)
            && allocations == 0
            && restagedLeft == freshLeft
            && restagedRight == freshRight
            && bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C61 restage should reconstruct independent metadata and phase state exactly");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(source != rejected.graph.nodes.end(),
            "C61 rejected replacement fixture should expose SumOfSines");
    const auto metadata = std::ranges::find(
        source->parameters,
        std::string_view{"analysisFrequency0"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(metadata != source->parameters.end(),
            "C61 rejected replacement fixture should expose analysisFrequency0");
    metadata->expression = "0 hz";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C61 invalid metadata replacement should reject before adoption");

    std::array<float, 4> afterRejectedLeft{};
    std::array<float, 4> afterRejectedRight{};
    std::array<float, 4> afterFreshLeft{};
    std::array<float, 4> afterFreshRight{};
    require(
        renderStereoAndCount(
            bridge, afterRejectedLeft, afterRejectedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh, afterFreshLeft, afterFreshRight, allocations)
            && allocations == 0
            && afterRejectedLeft == afterFreshLeft
            && afterRejectedRight == afterFreshRight,
        "C61 rejected replacement should preserve the active resolved-frequency generation");
}

void testC63SumOfSinesAutomaticLoopRestageAndRejectReplacementAtomically()
{
    const auto document = makeC63SumOfSinesBridgeDocument();
    const auto renderSettings = c63SumOfSinesBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(renderSettings).ok(),
        "C63 bridge should stage automatic loop traversal");

    std::array<float, 4> firstLeft{};
    std::array<float, 4> firstRight{};
    std::size_t allocations = 0;
    require(
        renderStereoAndCount(
            bridge, firstLeft, firstRight, allocations)
            && allocations == 0,
        "C63 bridge should publish automatic loop audio without allocation");
    constexpr std::array<double, 4> expected{0.0, 0.75, 0.0, -0.75};
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(std::abs(
                    static_cast<double>(firstLeft[frame])
                    - expected[frame]) <= 1.0e-6,
            "C63 bridge automatic loop prefix and wrap");
    }

    const auto firstStage = bridge.activeStageId();
    require(bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C63 bridge should restage the automatic loop document");
    talas::runtime::ProjectRuntimeBridge fresh(document, registry());
    fresh.setPlaying(true);
    require(fresh.compileAndStage(renderSettings).ok(),
        "C63 fresh bridge reference should stage");
    std::array<float, 4> restagedLeft{};
    std::array<float, 4> restagedRight{};
    std::array<float, 4> freshLeft{};
    std::array<float, 4> freshRight{};
    require(
        renderStereoAndCount(
            bridge, restagedLeft, restagedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh, freshLeft, freshRight, allocations)
            && allocations == 0
            && restagedLeft == freshLeft
            && restagedRight == freshRight
            && bridge.activeStageId() != firstStage
            && bridge.collectRetired(),
        "C63 restage should reconstruct independent cursor and Gate state");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(source != rejected.graph.nodes.end(),
        "C63 rejected replacement fixture should expose SumOfSines");
    const auto loopEnd = std::ranges::find(
        source->parameters,
        std::string_view{"loopEnd"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(loopEnd != source->parameters.end(),
        "C63 rejected replacement fixture should expose loopEnd");
    loopEnd->expression = "0.125";
    require(!bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C63 reversed loop replacement should reject before adoption");

    std::array<float, 4> afterRejectedLeft{};
    std::array<float, 4> afterRejectedRight{};
    std::array<float, 4> afterFreshLeft{};
    std::array<float, 4> afterFreshRight{};
    require(
        renderStereoAndCount(
            bridge, afterRejectedLeft, afterRejectedRight, allocations)
            && allocations == 0
            && renderStereoAndCount(
                fresh, afterFreshLeft, afterFreshRight, allocations)
            && allocations == 0
            && afterRejectedLeft == afterFreshLeft
            && afterRejectedRight == afterFreshRight,
        "C63 rejected replacement should preserve active loop cursor state");
}

void testC58SpectrumInRamRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeC58SpectrumInRamBridgeDocument();
    const auto renderSettings = c58SpectrumInRamBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C58 bridge and reference should stage normalized immutable frames");

    const auto renderExpected = [](
        talas::runtime::ProjectRuntimeBridge& target) {
        std::array<float, 4> left{};
        std::array<float, 4> right{};
        std::size_t allocations = 0;
        require(
            renderStereoAndCount(target, left, right, allocations)
                && allocations == 0,
            "C58 bridge transaction should render without allocation");
        for (std::size_t frame = 0; frame < left.size(); ++frame)
        {
            require(
                left[frame] == 0.5F && right[frame] == 200.0F,
                "C58 bridge should select the exact normalized middle frame");
        }
    };

    renderExpected(bridge);
    renderExpected(reference);
    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C58 bridge should restage the same normalized table document");
    renderExpected(bridge);
    renderExpected(reference);
    require(
        bridge.activeStageId() != firstStage && bridge.collectRetired(),
        "C58 restage should publish a fresh stateless generation");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(
        source != rejected.graph.nodes.end(),
        "C58 rejected replacement fixture should expose SpectrumInRAM");
    const auto analysisFrames = std::ranges::find(
        source->parameters,
        std::string_view{"analysisFrames"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        analysisFrames != source->parameters.end(),
        "C58 rejected replacement fixture should expose analysisFrames");
    analysisFrames->expression = "2";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C58 non-divisible fixed-width replacement should reject before adoption");
    renderExpected(bridge);
    renderExpected(reference);
}

void testC59SpectrumInRamDefaultsRestageAndRejectAtomically()
{
    const auto document = makeC59SpectrumInRamDefaultsBridgeDocument();
    const auto renderSettings = c59SpectrumInRamDefaultsBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok(),
        "C59 bridge should stage project-authored default analysis metadata");

    const auto renderExpected = [&] {
        std::array<float, 4> left{};
        std::array<float, 4> right{};
        std::size_t allocations = 0;
        require(
            renderStereoAndCount(bridge, left, right, allocations)
                && allocations == 0
                && std::ranges::all_of(
                    left, [](float sample) { return sample == 0.6F; })
                && std::ranges::all_of(
                    right, [](float sample) { return sample == 375.0F; }),
            "C59 bridge should publish exact default pitch/full-range output without allocation");
    };
    renderExpected();
    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C59 bridge should restage the same immutable default metadata document");
    renderExpected();
    require(
        bridge.activeStageId() != firstStage && bridge.collectRetired(),
        "C59 restage should publish a fresh stateless generation");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(
        source != rejected.graph.nodes.end(),
        "C59 rejected replacement fixture should expose SpectrumInRAM");
    const auto analysisFrequency = std::ranges::find(
        source->parameters,
        std::string_view{"analysisFrequency"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        analysisFrequency != source->parameters.end(),
        "C59 rejected replacement fixture should expose analysisFrequency");
    analysisFrequency->expression = "0 hz";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C59 non-positive analysis metadata replacement should reject before adoption");
    renderExpected();
}

void testC57VoicedUnvoicedRestageAndRejectedReplacementAreAtomic()
{
    const auto document = makeC57VoicedUnvoicedBridgeDocument();
    const auto renderSettings = c57VoicedUnvoicedBridgeSettings();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    bridge.setPlaying(true);
    reference.setPlaying(true);
    require(
        bridge.compileAndStage(renderSettings).ok()
            && reference.compileAndStage(renderSettings).ok(),
        "C57 bridge and reference should stage transition frames");

    const auto renderExpected = [](
        talas::runtime::ProjectRuntimeBridge& target,
        float expected) {
        std::array<float, 4> left{};
        std::array<float, 4> right{};
        std::size_t allocations = 0;
        require(
            renderStereoAndCount(target, left, right, allocations)
                && allocations == 0,
            "C57 bridge transaction should render without allocation");
        for (std::size_t frame = 0; frame < left.size(); ++frame)
        {
            require(
                left[frame] == expected && right[frame] == expected,
                "C57 bridge transaction should publish the exact stereo transition value");
        }
    };

    renderExpected(bridge, 1.0F);
    renderExpected(reference, 1.0F);
    renderExpected(bridge, 0.5F);
    renderExpected(reference, 0.5F);
    const auto firstStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(document, renderSettings).ok(),
        "C57 bridge should restage the same transition document");
    talas::runtime::ProjectRuntimeBridge fresh(document, registry());
    fresh.setPlaying(true);
    require(
        fresh.compileAndStage(renderSettings).ok(),
        "C57 fresh restage reference should stage");
    renderExpected(bridge, 1.0F);
    renderExpected(fresh, 1.0F);
    require(
        bridge.activeStageId() != firstStage && bridge.collectRetired(),
        "C57 restage should publish a fresh transition-state generation");

    auto rejected = document;
    const auto source = std::ranges::find(
        rejected.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    require(
        source != rejected.graph.nodes.end(),
        "C57 rejected replacement fixture should expose Spectrum On Disk");
    const auto analysisFrames = std::ranges::find(
        source->parameters,
        std::string_view{"analysisFrames"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        analysisFrames != source->parameters.end(),
        "C57 rejected replacement fixture should expose analysisFrames");
    analysisFrames->expression = "0";
    require(
        !bridge.compileAndStageDocument(rejected, renderSettings).ok(),
        "C57 invalid replacement should reject before adoption");
    renderExpected(bridge, 0.5F);
    renderExpected(fresh, 0.5F);
}

void testBridgeUpdatesLiveEventValuesOnActiveGraph()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0, "!Gain"), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "live Event Value graph should stage");

    std::array<float, 4> output{};
    require(render(bridge, output), "live Event Value graph should activate and render");
    require(std::abs(output[1] - 0.5F) < 1.0e-6F, "initial bridge Event Value gain");

    require(bridge.updateEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar),
            "bridge should update active Event Value without recompiling");
    require(render(bridge, output), "updated bridge Event Value graph should render");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F, "updated bridge Event Value gain");
}

void testBridgeSchedulesLiveEventValuesInsideNextCallback()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "scheduled bridge fixture should stage");
    require(bridge.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "bridge should forward a scheduled Event Value");

    std::array<float, 4> output{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, output, allocations), "scheduled bridge graph should render");
    require(allocations == 0, "scheduled bridge render should not allocate");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "bridge should preserve the initial Event Value at frame zero");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F, "bridge should apply scheduled Event Value at its exact frame");
}

void testBridgeDrainsStableRealtimeControlCommandsWithoutAllocations()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry(), 4);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "control ingress fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "compiled Event Value should receive a stable realtime ID");
    require(bridge.realtimeEventValueId("Gain") == gainId, "repeated Event Value lookup should keep its ID");
    require(!bridge.realtimeEventValueId("Missing").has_value(), "unknown Event Value should not receive an ID");
    require(!bridge.enqueueEventValue(0, 0.5, talas::expr::ValueDomain::Scalar), "zero Event Value ID should reject");
    require(!bridge.enqueueEventValue(*gainId, 220.0, talas::expr::ValueDomain::Hertz),
            "registered Event Value domain mismatch should reject before enqueue");

    require(bridge.enqueueEventValue(*gainId, 0.25, talas::expr::ValueDomain::Scalar, 1),
            "first equal-offset ingress command should enqueue");
    require(bridge.enqueueEventValue(*gainId, 0.75, talas::expr::ValueDomain::Scalar, 1),
            "second equal-offset ingress command should enqueue");
    std::array<float, 4> output{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, output, allocations), "control ingress callback should render");
    require(allocations == 0, "control ingress drain and scheduler evaluation should not allocate");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "control ingress should preserve pre-offset value");
    require(std::abs(output[1] - 0.75F) < 1.0e-6F, "equal-offset ingress should keep submission order");

    require(bridge.enqueueEventValue(*gainId, 0.25, talas::expr::ValueDomain::Scalar, 0),
            "zero-frame callback fixture should enqueue a command");
    std::array<float*, 1> outputChannels{output.data()};
    require(!bridge.render(outputChannels.data(), outputChannels.size(), 0),
            "zero-frame callback should reject without draining ingress");
    require(render(bridge, output), "command retained across zero-frame callback should render");
    require(std::abs(output[0] - 0.25F) < 1.0e-6F,
            "zero-frame callback must not consume the pending command");

    require(bridge.enqueueEventValue(*gainId, 0.5, talas::expr::ValueDomain::Scalar, output.size()),
            "ingress should accept an exact callback-end event");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    require(renderAndCount(bridge, output, allocations), "callback-end ingress command should preserve the current callback");
    require(allocations == 0, "callback-end ingress deferral should not allocate");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore,
            "exact callback-end ingress should defer without rejection");
    require(std::abs(output.back() - 0.25F) < 1.0e-6F,
            "callback-end ingress should not become visible in the preceding callback");
    require(renderAndCount(bridge, output, allocations), "deferred callback-end ingress should render next");
    require(allocations == 0, "deferred callback-end activation should not allocate");
    require(std::abs(output.front() - 0.5F) < 1.0e-6F,
            "callback-end ingress should activate at the following callback frame zero");
    require(
        bridge.realtimeControlTelemetry().dspRejection == rejectedBefore,
        "callback-end deferral should not alter DSP-rejection telemetry");
}

void testBridgeControlIngressRejectsOverflowWithoutOverwriting()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry(), 2);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "bounded ingress fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "bounded ingress fixture should register Gain");
    require(bridge.enqueueEventValue(*gainId, 0.25, talas::expr::ValueDomain::Scalar, 0),
            "bounded ingress first command");
    require(bridge.enqueueEventValue(*gainId, 0.5, talas::expr::ValueDomain::Scalar, 1),
            "bounded ingress second command");
    require(!bridge.enqueueEventValue(*gainId, 0.75, talas::expr::ValueDomain::Scalar, 2),
            "full bounded ingress should reject without overwrite");

    std::array<float, 4> output{};
    require(render(bridge, output), "bounded ingress accepted commands should render");
    require(std::abs(output[0] - 0.25F) < 1.0e-6F, "bounded ingress first accepted command");
    require(std::abs(output[1] - 0.5F) < 1.0e-6F, "bounded ingress second accepted command");
}

void testBridgeExposesReasonSpecificTypedEventTelemetry()
{
    talas::runtime::ProjectRuntimeBridge invalid(makeScheduledEventDocument(), registry(), 2);
    invalid.setPlaying(true);
    require(invalid.compileAndStage(liveEventSettings()).ok(), "invalid telemetry fixture should stage");
    require(
        !invalid.enqueueEventValue(0, 0.5, talas::expr::ValueDomain::Scalar),
        "invalid telemetry fixture should reject a zero target");
    auto telemetry = invalid.realtimeControlTelemetry();
    require(
        telemetry.invalidTargetOrPayload == 1
            && telemetry.queueOverflow == 0
            && telemetry.dspRejection == 0
            && invalid.rejectedRealtimeControlCount() == 0,
        "producer validation should be distinct from the compatible DSP rejection aggregate");

    talas::runtime::ProjectRuntimeBridge overflow(makeScheduledEventDocument(), registry(), 2);
    overflow.setPlaying(true);
    require(overflow.compileAndStage(liveEventSettings()).ok(), "overflow telemetry fixture should stage");
    const auto overflowId = overflow.realtimeEventValueId("Gain");
    require(overflowId.has_value(), "overflow telemetry fixture should resolve Gain");
    require(overflow.enqueueEventValue(*overflowId, 0.1, talas::expr::ValueDomain::Scalar), "first overflow fixture command");
    require(overflow.enqueueEventValue(*overflowId, 0.2, talas::expr::ValueDomain::Scalar), "second overflow fixture command");
    require(!overflow.enqueueEventValue(*overflowId, 0.3, talas::expr::ValueDomain::Scalar), "third overflow fixture command should backpressure");
    telemetry = overflow.realtimeControlTelemetry();
    require(
        telemetry.queueOverflow == 1
            && telemetry.dspRejection == 0
            && overflow.rejectedRealtimeControlCount() == 0,
        "queue admission backpressure should have its own monotonic counter");

    talas::runtime::ProjectRuntimeBridge stale(makeScheduledEventDocument(), registry(), 4);
    stale.setPlaying(true);
    require(stale.compileAndStage(liveEventSettings()).ok(), "stale telemetry fixture should stage");
    const auto staleId = stale.realtimeEventValueId("Gain");
    require(staleId.has_value(), "stale telemetry fixture should resolve Gain");
    require(stale.enqueueEventValue(*staleId, 0.25, talas::expr::ValueDomain::Scalar), "stale command should enqueue");
    stale.resetRealtimeControlTiming();
    std::array<float, 4> output{};
    require(render(stale, output), "stale telemetry fixture should render");
    telemetry = stale.realtimeControlTelemetry();
    require(
        telemetry.staleGenerationOrStage == 1
            && telemetry.preparedTypedEvents == 0,
        "generation invalidation should count the discarded command before typed preparation");

    talas::runtime::ProjectRuntimeBridge late(makeScheduledEventDocument(), registry(), 4);
    late.setPlaying(true);
    require(late.compileAndStage(liveEventSettings()).ok(), "late telemetry fixture should stage");
    const auto lateId = late.realtimeEventValueId("Gain");
    require(lateId.has_value(), "late telemetry fixture should resolve Gain");
    std::array<float*, 1> channels{output.data()};
    require(late.render({}, 10.0, channels.data(), channels.size(), output.size()), "late telemetry baseline should render");
    require(late.enqueueEventValueAtTime(*lateId, 0.25, talas::expr::ValueDomain::Scalar, 9.0), "late command should enter the bounded queue");
    require(late.render({}, 11.0, channels.data(), channels.size(), output.size()), "late telemetry callback should render");
    telemetry = late.realtimeControlTelemetry();
    require(
        telemetry.lateOrClampedTiming == 1
            && telemetry.preparedTypedEvents == 1
            && telemetry.batchOverflow == 0,
        "late timestamp clamping and typed preparation should be separately observable");
}

void testBridgeControlIngressRetainsOffsetsAcrossShortCallbacks()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "short ingress callback fixture should stage");
    require(bridge.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 3),
            "bridge should accept an offset inside the prepared maximum");

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(channels.data(), channels.size(), output.size()), "first short ingress callback should render");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F && std::abs(output[1] - 0.5F) < 1.0e-6F,
            "retained ingress event should not move into the first short callback");
    require(bridge.render(channels.data(), channels.size(), output.size()), "second short ingress callback should render");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "retained ingress event should preserve global frame two");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F, "retained ingress event should apply at global frame three");
}

void testBridgeControlIngressTargetsPendingReplacement()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "ingress replacement active graph should stage");
    std::array<float, 4> output{};
    require(render(bridge, output), "ingress replacement active graph should activate");
    require(bridge.compileAndStage(liveEventSettings()).ok(), "ingress replacement pending graph should stage");
    require(bridge.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 1),
            "ingress command should enqueue while replacement is pending");
    require(render(bridge, output), "pending replacement should activate with ingress command");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "replacement ingress frame zero");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F, "replacement ingress should reach the newly active graph");
}

void testBridgeControlIngressSupportsConcurrentProducers()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry(), 32);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "concurrent bridge ingress graph should stage");
    std::array<float, 4> output{};
    require(render(bridge, output), "concurrent bridge ingress graph should activate");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "concurrent bridge ingress should register Gain");

    constexpr std::size_t producerCount = 3;
    constexpr std::size_t commandsPerProducer = 500;
    std::atomic<std::size_t> producersDone = 0;
    std::array<std::thread, producerCount> producers;
    for (std::size_t producer = 0; producer < producerCount; ++producer)
    {
        producers[producer] = std::thread([&, producer] {
            const auto value = 0.25 * static_cast<double>(producer + 1);
            for (std::size_t command = 0; command < commandsPerProducer; ++command)
            {
                while (!bridge.enqueueEventValue(*gainId, value, talas::expr::ValueDomain::Scalar, 0))
                    std::this_thread::yield();
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::size_t callbacks = 0;
    while (producersDone.load(std::memory_order_acquire) != producerCount)
    {
        require(render(bridge, output), "concurrent bridge ingress callback should render");
        require(++callbacks < 100000, "concurrent bridge ingress producers should make progress");
    }
    for (auto& producer : producers)
        producer.join();
    for (std::size_t drain = 0; drain < 4; ++drain)
        require(render(bridge, output), "concurrent bridge ingress tail should drain");
    const auto telemetry = bridge.realtimeControlTelemetry();
    require(
        bridge.rejectedRealtimeControlCount() == 0,
        "valid concurrent bridge ingress commands should not be rejected after enqueue"
            ": aggregate=" + std::to_string(telemetry.aggregateRejected)
            + ", dsp=" + std::to_string(telemetry.dspRejection)
            + ", stale=" + std::to_string(telemetry.staleGenerationOrStage)
            + ", invalid=" + std::to_string(telemetry.invalidTargetOrPayload)
            + ", batchOverflow=" + std::to_string(telemetry.batchOverflow));
}

void testBridgeMapsArrivalTimestampsIntoCurrentCallback()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "timestamp ingress fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "timestamp ingress fixture should register Gain");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "timestamp ingress baseline callback should render");
    require(bridge.enqueueEventValueAtTime(*gainId, 0.25, talas::expr::ValueDomain::Scalar, 10.25),
            "timestamped Event Value should enqueue");
    require(bridge.render({}, 11.0, channels.data(), channels.size(), output.size()),
            "timestamped ingress callback should render");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "timestamp ingress should preserve frame zero");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F, "timestamp ingress should map quarter-second arrival to frame one");

    require(bridge.enqueueEventValueAtTime(*gainId, 0.75, talas::expr::ValueDomain::Scalar, 11.5),
            "stale-before-reset timestamp should enqueue");
    bridge.resetRealtimeControlTiming();
    require(bridge.render({}, 20.0, channels.data(), channels.size(), output.size()),
            "post-reset timestamp callback should render");
    require(std::abs(output[0] - 0.25F) < 1.0e-6F,
            "timing reset should discard stale ingress and retain the active value");
}

void testControllerRecordingCapturesAcceptedCallbackEvidence()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 16);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "controller recording fixture should stage");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto stage = bridge.latestStageGeneration();
    require(gainId.has_value() && stage != 0,
            "controller recording fixture should resolve exact target authority");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
            "controller recording baseline callback should adopt the stage");
    const auto takeStart = bridge.transportFramePosition();
    const std::array<talas::project::ControllerRecordingTarget, 1> targets{{{
        *gainId,
        "recordedGain",
        talas::project::ScoreAutomationScope::Track,
        "track",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::ScoreTimeDomain::Seconds,
        0,
    }}};
    const auto armed = bridge.beginControllerRecording(targets);
    require(armed.accepted && armed.takeGeneration != 0,
            "controller recording should arm against the exact accepted stage");

    const talas::runtime::ControllerRecordingIngressEvidence evidence{
        talas::project::ControllerRecordingOrigin::Midi,
        10'250'000'000,
        71,
        72,
    };
    require(bridge.enqueueEventValueAtTimeForStage(
                *gainId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10.25,
                stage,
                evidence),
            "accepted MIDI performance command should retain recording evidence");
    require(bridge.render(
                {}, 11.0, channels.data(), channels.size(), output.size()),
            "controller recording event callback should render");
    require(bridge.enqueueControllerRecordingObservationAtTimeForStage(
                *gainId,
                0.75,
                talas::expr::ValueDomain::Scalar,
                11'500'000'000,
                stage,
                {
                    talas::project::ControllerRecordingOrigin::Ui,
                    11'500'000'000,
                    73,
                    0,
                }),
            "accepted UI performance transaction should retain its original timestamp");
    require(bridge.render(
                {}, 12.0, channels.data(), channels.size(), output.size()),
            "controller recording observation callback should render");
    bridge.stopControllerRecording();
    require(bridge.render(
                {}, 13.0, channels.data(), channels.size(), output.size()),
            "controller recording stop should linearize at an audio boundary");

    const auto take = bridge.collectControllerRecordingTake();
    require(take.has_value()
                && take->takeGeneration == armed.takeGeneration
                && take->stageGeneration == stage
                && take->startFrame == takeStart
                && take->terminal
                    == talas::project::ControllerRecordingTerminal::Completed
                && take->events.size() == 2,
            "completed recording should publish one complete generation-bound take");
    const auto& event = take->events.front();
    require(event.eventId == *gainId
                && event.value == 0.25
                && event.absoluteFrame == takeStart + 1
                && event.sourceTimestampNanoseconds == 10'250'000'000
                && event.stageGeneration == stage
                && event.routeGeneration == 71
                && event.sourceGeneration == 72
                && event.origin
                    == talas::project::ControllerRecordingOrigin::Midi,
            "raw take must retain exact callback frame and source/route/stage evidence");
    const auto& observed = take->events.back();
    require(observed.eventId == *gainId
                && observed.value == 0.75
                && observed.absoluteFrame == takeStart + 6
                && observed.sourceTimestampNanoseconds == 11'500'000'000
                && observed.stageGeneration == stage
                && observed.routeGeneration == 73
                && observed.sourceGeneration == 0
                && observed.origin
                    == talas::project::ControllerRecordingOrigin::Ui,
            "accepted UI observation must map its original steady timestamp to the exact callback frame");

    require(bridge.beginControllerRecording(targets).accepted
                && bridge.render(
                    {}, 14.0, channels.data(), channels.size(), output.size()),
            "transport-stop recording should be adopted before Stop");
    bridge.stopTransport();
    static_cast<void>(bridge.render(
        {}, 15.0, channels.data(), channels.size(), output.size()));
    const auto stoppedTake = bridge.collectControllerRecordingTake();
    require(stoppedTake.has_value()
                && stoppedTake->terminal
                    == talas::project::ControllerRecordingTerminal::Completed,
            "ordinary transport Stop must complete rather than relabel the take as SourceLost");

    auto staleDocument = bridge.editor().document();
    staleDocument.metadata.title += " stale-authoring-edit";
    require(bridge.editor().applyDocumentEdit(std::move(staleDocument)).ok(),
            "stale-stage recording regression setup must edit the authoring document");
    require(!bridge.beginControllerRecording(targets).accepted,
            "controller recording must reject an editor document that is not the exact accepted stage");
}

void testControllerRecordingStopBeforeAdoptionPublishesNoStaleEvents()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 8);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "stop-before-adoption fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output),
            "stop-before-adoption fixture should adopt its graph");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(gainId.has_value(),
            "stop-before-adoption fixture should resolve target");
    const std::array<talas::project::ControllerRecordingTarget, 1> targets{{{
        *gainId,
        "recordedGain",
        talas::project::ScoreAutomationScope::Track,
        "track",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::ScoreTimeDomain::Seconds,
        0,
    }}};
    require(bridge.beginControllerRecording(targets).accepted,
            "stop-before-adoption fixture should arm");
    bridge.stopControllerRecording(
        talas::project::ControllerRecordingTerminal::Cancelled);
    require(render(bridge, output),
            "stop-before-adoption should publish at the next boundary");
    const auto take = bridge.collectControllerRecordingTake();
    require(take.has_value()
                && take->terminal
                    == talas::project::ControllerRecordingTerminal::Cancelled
                && take->events.empty(),
            "a stopped unadopted take must not inherit prior audio-thread events");
}

void testControllerRecordingTakeGenerationAndStopPriority()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 16);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "controller recording generation fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
            "controller recording generation baseline should render");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto stage = bridge.latestStageGeneration();
    require(gainId.has_value() && stage != 0,
            "controller recording generation fixture should resolve Gain");
    const std::array<talas::project::ControllerRecordingTarget, 1> targets{{{
        *gainId,
        "recordedGain",
        talas::project::ScoreAutomationScope::Track,
        "track",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::ScoreTimeDomain::Seconds,
        0,
    }}};
    require(bridge.enqueueEventValueAtTimeForStage(
                *gainId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10.25,
                stage,
                {
                    talas::project::ControllerRecordingOrigin::Midi,
                    10'250'000'000,
                    51,
                    52,
                }),
            "pre-arm command should remain a valid performance update");
    require(bridge.beginControllerRecording(targets).accepted
                && bridge.render(
                    {}, 11.0, channels.data(), channels.size(), output.size()),
            "arm and pre-arm command should share a deterministic boundary");
    bridge.stopControllerRecording();
    require(bridge.render(
                {}, 12.0, channels.data(), channels.size(), output.size()),
            "first generation stop should publish");
    const auto first = bridge.collectControllerRecordingTake();
    require(first.has_value() && first->events.empty(),
            "a command admitted before arm must never contaminate the new take generation");

    require(bridge.beginControllerRecording(targets).accepted
                && bridge.render(
                    {}, 13.0, channels.data(), channels.size(), output.size()),
            "second take generation should be audio-adopted before its event");
    require(bridge.enqueueEventValueAtTimeForStage(
                *gainId,
                0.75,
                talas::expr::ValueDomain::Scalar,
                13.25,
                stage,
                {
                    talas::project::ControllerRecordingOrigin::Osc,
                    13'250'000'000,
                    61,
                    62,
                }),
            "pre-stop command should bind to the active take generation");
    std::thread completed([&] {
        bridge.stopControllerRecording(
            talas::project::ControllerRecordingTerminal::Completed);
    });
    std::thread sourceLost([&] {
        bridge.stopControllerRecording(
            talas::project::ControllerRecordingTerminal::SourceLost);
    });
    completed.join();
    sourceLost.join();
    require(bridge.render(
                {}, 14.0, channels.data(), channels.size(), output.size()),
            "concurrent stop requests should linearize after draining prior-generation commands");
    const auto second = bridge.collectControllerRecordingTake();
    require(second.has_value()
                && second->terminal
                    == talas::project::ControllerRecordingTerminal::SourceLost
                && second->events.size() == 1
                && second->events.front().value == 0.75,
            "failure terminal priority must dominate Completed while retaining a command admitted before stop");
}

void testLateControllerStopCannotCrossTakeGeneration()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 16);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "late-stop generation fixture should stage");
    std::array<float, 4> output{};
    require(render(bridge, output),
            "late-stop generation fixture should adopt its graph");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(gainId.has_value(),
            "late-stop generation fixture should resolve Gain");
    const std::array<talas::project::ControllerRecordingTarget, 1> targets{{{
        *gainId,
        "recordedGain",
        talas::project::ScoreAutomationScope::Track,
        "track",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::ScoreTimeDomain::Seconds,
        0,
    }}};
    require(bridge.beginControllerRecording(targets).accepted
                && render(bridge, output),
            "late-stop generation G should be adopted");

    std::binary_semaphore stopLoaded{0};
    std::binary_semaphore releaseLateStop{0};
    std::atomic<bool> holdLateStop = true;
    bridge.setControllerRecordingStopAfterLoadHookForTests([&] {
        if (!holdLateStop.exchange(false, std::memory_order_acq_rel))
            return;
        stopLoaded.release();
        releaseLateStop.acquire();
    });
    std::thread lateStop([&] {
        bridge.stopControllerRecording(
            talas::project::ControllerRecordingTerminal::SourceLost);
    });
    stopLoaded.acquire();

    bridge.stopControllerRecording(
        talas::project::ControllerRecordingTerminal::Completed);
    require(render(bridge, output),
            "generation G should complete while its stale stop is retained");
    const auto first = bridge.collectControllerRecordingTake();
    require(first.has_value()
                && first->terminal
                    == talas::project::ControllerRecordingTerminal::Completed,
            "generation G should publish its already-linearized completion");
    require(bridge.beginControllerRecording(targets).accepted
                && render(bridge, output),
            "generation G+1 should arm before the retained G stop resumes");

    releaseLateStop.release();
    lateStop.join();
    bridge.setControllerRecordingStopAfterLoadHookForTests({});
    bridge.stopControllerRecording(
        talas::project::ControllerRecordingTerminal::Completed);
    require(render(bridge, output),
            "generation G+1 should complete after the retained G stop");
    const auto second = bridge.collectControllerRecordingTake();
    require(second.has_value()
                && second->takeGeneration != first->takeGeneration
                && second->terminal
                    == talas::project::ControllerRecordingTerminal::Completed,
            "a retained stop tagged G must not poison or stop generation G+1");
}

void testControllerRecordingPunchSeekAndSourceLossFailClosed()
{
    const auto makeTarget = [](talas::runtime::ProjectRuntimeBridge& bridge) {
        const auto gainId = bridge.realtimeEventValueIdForLatestStage(
            "Gain", talas::expr::ValueDomain::Scalar);
        require(gainId.has_value(),
                "controller recording terminal fixture should resolve Gain");
        return talas::project::ControllerRecordingTarget{
            *gainId,
            "recordedGain",
            talas::project::ScoreAutomationScope::Track,
            "track",
            "Gain",
            talas::expr::ValueDomain::Scalar,
            talas::project::ScoreTimeDomain::Seconds,
            0,
        };
    };

    talas::runtime::ProjectRuntimeBridge punch(
        makeScheduledEventDocument(), registry(), 16);
    punch.setPlaying(true);
    require(punch.compileAndStage(liveEventSettings()).ok(),
            "controller recording punch fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(punch.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
            "controller recording punch baseline should render");
    const auto punchStart = punch.transportFramePosition();
    const std::array punchTargets{makeTarget(punch)};
    require(punch.beginControllerRecording(
                punchTargets, punchStart + 1, punchStart + 2).accepted,
            "controller recording punch range should arm");
    const auto punchStage = punch.latestStageGeneration();
    require(punch.enqueueEventValueAtTimeForStage(
                punchTargets.front().eventId,
                0.75,
                talas::expr::ValueDomain::Scalar,
                10.25,
                punchStage,
                {
                    talas::project::ControllerRecordingOrigin::Osc,
                    10'250'000'000,
                    31,
                    32,
                }),
            "controller recording in-punch event should enqueue");
    require(punch.render(
                {}, 11.0, channels.data(), channels.size(), output.size())
                && punch.render(
                    {}, 12.0, channels.data(), channels.size(), output.size()),
            "controller recording punch should cross its terminal boundary");
    const auto punched = punch.collectControllerRecordingTake();
    require(punched.has_value()
                && punched->terminal
                    == talas::project::ControllerRecordingTerminal::PunchComplete
                && punched->endFrame == punchStart + 2
                && punched->events.size() == 1
                && punched->events.front().absoluteFrame == punchStart + 1,
            "punch completion must publish only in-range exact evidence");

    talas::runtime::ProjectRuntimeBridge seek(
        makeScheduledEventDocument(), registry(), 8);
    seek.setPlaying(true);
    require(seek.compileAndStage(liveEventSettings()).ok()
                && render(seek, output),
            "controller recording seek fixture should stage");
    const std::array seekTargets{makeTarget(seek)};
    require(seek.beginControllerRecording(seekTargets).accepted
                && seek.requestTransportRelocation(0)
                && render(seek, output),
            "accepted backward seek should terminate the armed take at a boundary");
    const auto seeked = seek.collectControllerRecordingTake();
    require(seeked.has_value() &&
                seeked->terminal == talas::project::ControllerRecordingTerminal::Seeked &&
                seeked->events.empty() && seeked->endFrame >= seeked->startFrame &&
                !talas::project::controllerRecordingTakeValidationError(*seeked).has_value(),
            "seek must not publish a partial automation lane");

    talas::runtime::ProjectRuntimeBridge sourceLoss(
        makeScheduledEventDocument(), registry(), 8);
    sourceLoss.setPlaying(true);
    require(sourceLoss.compileAndStage(liveEventSettings()).ok()
                && render(sourceLoss, output),
            "controller recording source-loss fixture should stage");
    const std::array sourceTargets{makeTarget(sourceLoss)};
    require(sourceLoss.beginControllerRecording(sourceTargets).accepted,
            "controller recording source-loss fixture should arm");
    sourceLoss.resetRealtimeControlTiming();
    require(render(sourceLoss, output),
            "source-loss terminal should publish at an audio boundary");
    const auto lost = sourceLoss.collectControllerRecordingTake();
    require(lost.has_value()
                && lost->terminal
                    == talas::project::ControllerRecordingTerminal::SourceLost
                && lost->events.empty(),
            "source generation loss must retain raw take truth without a partial lane");

    talas::runtime::ProjectRuntimeBridge stageChange(
        makeScheduledEventDocument(), registry(), 8);
    stageChange.setPlaying(true);
    require(stageChange.compileAndStage(liveEventSettings()).ok()
                && render(stageChange, output),
            "controller recording stage-change fixture should stage");
    const std::array stageTargets{makeTarget(stageChange)};
    require(stageChange.beginControllerRecording(stageTargets).accepted
                && render(stageChange, output)
                && stageChange.compileAndStage(liveEventSettings()).ok()
                && render(stageChange, output),
            "replacement graph adoption should close an already-adopted prior-stage take");
    const auto stageChanged = stageChange.collectControllerRecordingTake();
    require(stageChanged.has_value()
                && stageChanged->terminal
                    == talas::project::ControllerRecordingTerminal::StageChanged
                && stageChanged->events.empty(),
            "stage replacement must fail closed without publishing a partial lane");

    talas::runtime::ProjectRuntimeBridge deviceChange(
        makeScheduledEventDocument(), registry(), 8);
    deviceChange.setPlaying(true);
    require(deviceChange.compileAndStage(liveEventSettings()).ok()
                && render(deviceChange, output),
            "controller recording device-change fixture should stage");
    const std::array deviceTargets{makeTarget(deviceChange)};
    require(deviceChange.beginControllerRecording(deviceTargets).accepted,
            "controller recording device-change fixture should arm");
    deviceChange.clearAfterAudioStopped();
    const auto deviceChanged = deviceChange.collectControllerRecordingTake();
    require(deviceChanged.has_value()
                && deviceChanged->terminal
                    == talas::project::ControllerRecordingTerminal::DeviceChanged
                && deviceChanged->events.empty(),
            "device replacement must close the raw take without a partial lane");
}

void testControllerRecordingLoopAndCancelRetainAbsoluteRawEvidence()
{
    auto document = makeScheduledEventDocument();
    document.timeline.loop = {true, 0.0, 1.0};
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document), registry(), 16);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "controller recording loop fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
            "controller recording loop baseline should render");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto stage = bridge.latestStageGeneration();
    require(gainId.has_value() && stage != 0,
            "controller recording loop fixture should resolve exact authority");
    const std::array<talas::project::ControllerRecordingTarget, 1> targets{{{
        *gainId,
        "recordedGain",
        talas::project::ScoreAutomationScope::Track,
        "track",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::ScoreTimeDomain::Seconds,
        0,
    }}};
    require(bridge.beginControllerRecording(targets).accepted,
            "controller recording loop fixture should arm");
    for (int callback = 0; callback < 2; ++callback)
    {
        const auto arrival = 10.25 + static_cast<double>(callback);
        require(bridge.enqueueEventValueAtTimeForStage(
                    *gainId,
                    0.25 + static_cast<double>(callback) * 0.5,
                    talas::expr::ValueDomain::Scalar,
                    arrival,
                    stage,
                    {
                        talas::project::ControllerRecordingOrigin::Osc,
                        static_cast<std::int64_t>(
                            std::llround(arrival * 1'000'000'000.0)),
                        41,
                        42,
                    }),
                "controller recording loop event should enqueue");
        require(bridge.render(
                    {},
                    11.0 + static_cast<double>(callback),
                    channels.data(),
                    channels.size(),
                    output.size()),
                "controller recording loop callback should render");
    }
    bridge.stopControllerRecording(
        talas::project::ControllerRecordingTerminal::Cancelled);
    require(bridge.render(
                {}, 13.0, channels.data(), channels.size(), output.size()),
            "controller recording cancel should publish at a boundary");
    const auto cancelled = bridge.collectControllerRecordingTake();
    require(cancelled.has_value(),
            "looping controller recording should publish a cancelled raw take");
    require(cancelled->terminal
                == talas::project::ControllerRecordingTerminal::Cancelled,
            "looping controller recording should preserve the cancel terminal");
    require(cancelled->events.size() == 2,
            "looping controller recording should retain both raw events");
    require(cancelled->events[0].absoluteFrame >= cancelled->startFrame
                && cancelled->events[1].absoluteFrame
                    == cancelled->events[0].absoluteFrame + 4,
            "looping must preserve monotonic absolute raw frames while cancel retains evidence without an approvable lane");
}

void testBridgeSchedulesAbsoluteSteadyControlsAcrossCallbacks()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 4);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "absolute scheduled-control fixture should stage");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto stage = bridge.latestStageGeneration();
    require(gainId.has_value() && stage != 0,
            "absolute scheduled-control fixture should resolve Gain");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    const auto renderAtWithoutAllocations = [&](double callbackTime) {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = bridge.render(
            {}, callbackTime, channels.data(), channels.size(), output.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(allocationCount.load(std::memory_order_relaxed) == 0,
            "absolute scheduled-control callback must not allocate");
        return rendered;
    };
    require(bridge.render(
                {}, 9.0, channels.data(), channels.size(), output.size()),
            "absolute scheduled-control baseline should render");
    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10'500'000'000,
                stage),
            "absolute control inside the next callback should enqueue");
    require(renderAtWithoutAllocations(10.0),
            "absolute scheduled-control callback should render");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F
                && std::abs(output[1] - 0.5F) < 1.0e-6F
                && std::abs(output[2] - 0.25F) < 1.0e-6F,
            "absolute deadline should map to the exact callback sample");

    const auto lateBeforeBoundary =
        bridge.realtimeControlTelemetry().scheduledLate;
    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.4,
                talas::expr::ValueDomain::Scalar,
                11'000'000'000,
                stage)
            && renderAtWithoutAllocations(11.0),
        "an exact callback-start deadline should apply at frame zero");
    require(std::abs(output[0] - 0.4F) < 1.0e-6F
                && bridge.realtimeControlTelemetry().scheduledLate
                    == lateBeforeBoundary,
        "the closed callback start boundary must not be classified late");

    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.45,
                talas::expr::ValueDomain::Scalar,
                11'100'000'000,
                stage)
            && bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.75,
                talas::expr::ValueDomain::Scalar,
                12'500'000'000,
                stage),
        "late and future absolute controls should enqueue together");
    require(renderAtWithoutAllocations(11.25),
            "late control should apply while the future control survives");
    auto telemetry = bridge.realtimeControlTelemetry();
    require(std::abs(output[0] - 0.45F) < 1.0e-6F
                && telemetry.scheduledLate == lateBeforeBoundary + 1
                && telemetry.scheduledPending == 1
                && telemetry.scheduledPendingHighWatermark == 1
                && telemetry.scheduledApplied == 3,
            "late control should clamp to frame zero while future authority remains pending");
    require(renderAtWithoutAllocations(12.0),
            "future absolute control should render when due");
    require(std::abs(output[2] - 0.75F) < 1.0e-6F,
            "retained future command should keep its exact sample offset");

    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.1,
                talas::expr::ValueDomain::Scalar,
                13'250'000'000,
                stage)
                && bridge.enqueueEventValueAtSteadyTimeForStage(
                    *gainId,
                    0.9,
                    talas::expr::ValueDomain::Scalar,
                    13'250'000'000,
                    stage),
            "equal-deadline commands should both enqueue");
    require(renderAtWithoutAllocations(13.0),
            "equal-deadline controls should render");
    require(std::abs(output[1] - 0.9F) < 1.0e-6F,
            "equal deadlines should retain ingress sequence order");

    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.2,
                talas::expr::ValueDomain::Scalar,
                20'000'000'000,
                stage),
            "stale future command should enter the bounded queue");
    require(renderAtWithoutAllocations(14.0),
            "stale future command should become pending");
    bridge.resetRealtimeControlTiming();
    require(renderAtWithoutAllocations(15.0),
            "generation reset should retire future pending controls");
    telemetry = bridge.realtimeControlTelemetry();
    require(telemetry.scheduledPending == 0
                && telemetry.scheduledStale == 1
                && telemetry.scheduledApplied == 6,
            "generation reset must discard exact future authority before DSP");

    for (std::size_t index = 0; index < 4; ++index)
    {
        require(bridge.enqueueEventValueAtSteadyTimeForStage(
                    *gainId,
                    0.1 + static_cast<double>(index) * 0.1,
                    talas::expr::ValueDomain::Scalar,
                    30'000'000'000
                        + static_cast<std::int64_t>(index),
                    stage),
            "fixed pending capacity fixture should enqueue");
    }
    require(renderAtWithoutAllocations(16.0),
        "fixed pending capacity should fill without allocation");
    require(bridge.enqueueEventValueAtSteadyTimeForStage(
                *gainId,
                0.9,
                talas::expr::ValueDomain::Scalar,
                31'000'000'000,
                stage),
        "one additional future command should reach callback admission");
    require(renderAtWithoutAllocations(17.0),
        "pending saturation should fail closed without stopping audio");
    telemetry = bridge.realtimeControlTelemetry();
    require(telemetry.scheduledPending == 4
                && telemetry.scheduledPendingHighWatermark == 4
                && telemetry.scheduledPendingFull == 1,
        "fixed pending saturation should reject only the excess command");
    bridge.resetRealtimeControlTiming();
    require(renderAtWithoutAllocations(18.0),
        "pending saturation fixture should clear at generation reset");
    telemetry = bridge.realtimeControlTelemetry();
    require(telemetry.scheduledPending == 0
                && telemetry.scheduledStale == 5,
        "reset should retire every retained future command exactly once");
}

void testBridgeAbsoluteSteadySchedulingIsPartitionInvariant()
{
    talas::runtime::ProjectRuntimeBridge full(
        makeScheduledEventDocument(), registry(), 4);
    talas::runtime::ProjectRuntimeBridge split(
        makeScheduledEventDocument(), registry(), 4);
    full.setPlaying(true);
    split.setPlaying(true);
    require(full.compileAndStage(liveEventSettings()).ok()
                && split.compileAndStage(liveEventSettings()).ok(),
        "partition-invariant scheduled fixtures should stage");

    std::array<float, 4> fullOutput{};
    std::array<float, 2> splitFirst{};
    std::array<float, 2> splitSecond{};
    std::array<float*, 1> fullChannels{fullOutput.data()};
    std::array<float*, 1> splitFirstChannels{splitFirst.data()};
    std::array<float*, 1> splitSecondChannels{splitSecond.data()};
    require(full.render(
                {}, 9.0, fullChannels.data(), 1, fullOutput.size())
            && split.render(
                {}, 9.0, splitFirstChannels.data(), 1, splitFirst.size())
            && split.render(
                {}, 9.5, splitSecondChannels.data(), 1, splitSecond.size()),
        "partition-invariant fixtures should render equal baseline time");

    const auto fullId = full.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto splitId = split.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    require(fullId.has_value() && splitId.has_value()
            && full.enqueueEventValueAtSteadyTimeForStage(
                *fullId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10'500'000'000,
                full.latestStageGeneration())
            && split.enqueueEventValueAtSteadyTimeForStage(
                *splitId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10'500'000'000,
                split.latestStageGeneration()),
        "partition-invariant fixtures should accept the same absolute deadline");
    require(full.render(
                {}, 10.0, fullChannels.data(), 1, fullOutput.size())
            && split.render(
                {}, 10.0, splitFirstChannels.data(), 1, splitFirst.size())
            && split.render(
                {}, 10.5, splitSecondChannels.data(), 1, splitSecond.size()),
        "one full and two half callbacks should render the same interval");
    const std::array<float, 4> splitOutput{
        splitFirst[0], splitFirst[1], splitSecond[0], splitSecond[1]};
    for (std::size_t frame = 0; frame < fullOutput.size(); ++frame)
    {
        require(std::abs(fullOutput[frame] - splitOutput[frame]) < 1.0e-6F,
            "absolute deadline should map to the same global sample across callback partitions");
    }
}

void testBridgeStageReplacementInvalidatesAbsolutePending()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry(), 4);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
        "stage-replacement scheduled fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 9.0, channels.data(), channels.size(), output.size()),
        "stage-replacement scheduled fixture should activate");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto originalStage = bridge.latestStageGeneration();
    require(gainId.has_value()
                && bridge.enqueueEventValueAtSteadyTimeForStage(
                    *gainId,
                    0.25,
                    talas::expr::ValueDomain::Scalar,
                    20'000'000'000,
                    originalStage)
                && bridge.render(
                    {}, 10.0, channels.data(), channels.size(), output.size())
                && bridge.realtimeControlTelemetry().scheduledPending == 1,
        "old-stage future control should first enter the pending set");

    require(bridge.compileAndStage(liveEventSettings()).ok()
                && bridge.latestStageGeneration() != originalStage,
        "replacement should publish a new exact stage generation");
    const auto staleBefore =
        bridge.realtimeControlTelemetry().scheduledStale;
    require(bridge.render(
                {}, 11.0, channels.data(), channels.size(), output.size()),
        "replacement callback should adopt the new stage");
    const auto telemetry = bridge.realtimeControlTelemetry();
    require(telemetry.scheduledPending == 0
                && telemetry.scheduledStale == staleBefore + 1
                && telemetry.scheduledApplied == 0,
        "stage replacement must retire old future authority before its deadline");
}

void testBridgeRejectsExplicitStageCommandAfterReplacement()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "explicit-stage ingress fixture should stage");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto originalStage = bridge.latestStageGeneration();
    require(gainId.has_value() && originalStage != 0,
            "explicit-stage ingress fixture should resolve its exact target");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
            "explicit-stage ingress baseline should activate");
    require(bridge.enqueueEventValueAtTimeForStage(
                *gainId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                10.25,
                originalStage),
            "exact original-stage command should enter the bounded queue");
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "replacement stage should publish after the old command is queued");
    require(bridge.latestStageGeneration() != originalStage,
            "replacement should advance the exact stage generation");
    require(bridge.render(
                {}, 11.0, channels.data(), channels.size(), output.size()),
            "replacement callback should render while rejecting stale ingress");
    require(bridge.realtimeControlTelemetry().staleGenerationOrStage == 1,
            "queued command bound to the prior stage should reject at DSP admission");
    require(std::all_of(
                output.begin(), output.end(), [](float sample) {
                    return std::abs(sample - 0.5F) < 1.0e-6F;
                }),
            "stale explicit-stage command must not alter the replacement graph");
}

void testBridgeTimestampMappingUsesPinnedGraphSampleRate()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "pinned sample-rate fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "pinned sample-rate fixture should register Gain");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "pinned sample-rate baseline should render");
    auto replacementSettings = liveEventSettings();
    replacementSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(std::move(replacementSettings)).ok(),
            "pinned sample-rate replacement should stage at a different rate");
    require(bridge.enqueueEventValueAtTime(*gainId, 0.25, talas::expr::ValueDomain::Scalar, 10.25),
            "pinned sample-rate command should enqueue");
    require(bridge.render({}, 10.5, channels.data(), channels.size(), output.size()),
            "pinned sample-rate callback should render");
    require(std::abs(output[1] - 0.5F) < 1.0e-6F, "replacement graph should preserve the value through frame one");
    require(std::abs(output[2] - 0.25F) < 1.0e-6F, "replacement graph sample rate should map arrival to frame two");
}

void testBridgeLegacySchedulingReportsQueueAdmission()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "queue-admission fixture should stage");
    require(bridge.scheduleEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar, 5),
            "legacy scheduling API should report successful queue admission");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    std::array<float, 4> output{};
    require(render(bridge, output), "queue-admission rejection callback should continue rendering");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore + 1,
            "DSP rejection after queue admission should be observable through telemetry");
}

void testBridgeRejectsInvalidRealtimeExpressionWithoutAllocating()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(1.0, "1 / !Gain"), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "invalid-expression fixture should stage while Gain is nonzero");

    std::array<float, 4> output{};
    require(render(bridge, output), "invalid-expression fixture should activate");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "invalid-expression fixture should register Gain");
    require(bridge.enqueueEventValue(*gainId, 0.0, talas::expr::ValueDomain::Scalar),
            "zero divisor should reach audio-thread validation");

    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    std::size_t allocations = 0;
    require(renderAndCount(bridge, output, allocations), "invalid expression command must not stop audio rendering");
    require(allocations == 0, "invalid expression rejection must not allocate in the audio callback");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore + 1,
            "invalid expression command should increment rejection telemetry");
    require(std::abs(output[1] - 2.0F) < 1.0e-6F,
            "invalid expression command should retain the previous valid snapshot");

    require(bridge.enqueueEventValue(*gainId, 0.25, talas::expr::ValueDomain::Scalar),
            "valid command after rejection should enqueue");
    require(renderAndCount(bridge, output, allocations), "valid command after rejection should render");
    require(allocations == 0, "valid rollback recovery must remain allocation free");
    require(std::abs(output[1] - 4.0F) < 1.0e-6F,
            "valid command after rejection should prove Event Value rollback");
}

void testBridgeTimingResetRemainsSingleConsumerDuringRendering()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry(), 64);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(liveEventSettings()).ok(), "timing reset fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "timing reset fixture should register Gain");

    std::atomic<bool> stop = false;
    std::thread resetter([&] {
        for (std::size_t index = 0; index < 2000; ++index)
        {
            bridge.resetRealtimeControlTiming();
            (void)bridge.enqueueEventValueAtTime(
                *gainId,
                index % 2 == 0 ? 0.25 : 0.75,
                talas::expr::ValueDomain::Scalar,
                10.0 + static_cast<double>(index) * 0.001);
        }
        stop.store(true, std::memory_order_release);
    });

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    double callbackTime = 10.0;
    while (!stop.load(std::memory_order_acquire))
    {
        require(bridge.render({}, callbackTime, channels.data(), channels.size(), output.size()),
                "render should continue across concurrent timing resets");
        callbackTime += 1.0;
    }
    resetter.join();
    for (std::size_t tail = 0; tail < 8; ++tail)
    {
        require(bridge.render({}, callbackTime, channels.data(), channels.size(), output.size()),
                "timing reset tail should drain on the audio consumer");
        callbackTime += 1.0;
    }
}

void testBridgeSortsFullReverseControlBatchWithoutAllocating()
{
    constexpr std::size_t frameCount = 256;
    talas::runtime::ProjectRuntimeBridge bridge(makeScheduledEventDocument(), registry(), frameCount);
    bridge.setPlaying(true);
    auto renderSettings = liveEventSettings();
    renderSettings.sampleRate = static_cast<double>(frameCount);
    renderSettings.maxBlockFrames = frameCount;
    renderSettings.maxRealtimeEventTransactions = frameCount;
    require(bridge.compileAndStage(std::move(renderSettings)).ok(), "reverse batch fixture should stage");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "reverse batch fixture should register Gain");

    for (std::size_t offset = frameCount; offset-- > 0;)
    {
        require(bridge.enqueueEventValue(
                    *gainId,
                    static_cast<double>(offset + 1) / static_cast<double>(frameCount),
                    talas::expr::ValueDomain::Scalar,
                    offset),
                "full reverse batch should fit bounded ingress");
    }

    std::array<float, frameCount> output{};
    std::array<float*, 1> channels{output.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render(channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "full reverse batch should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "heap ordering and full-batch scheduling should not allocate");
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        require(std::abs(output[frame] - static_cast<float>(frame + 1) / static_cast<float>(frameCount)) < 1.0e-6F,
                "full reverse batch should apply each command at its exact sorted frame");
    }
}

void testBridgeSchedulesMonophonicMidiNotesAtExactSamples()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeMidiGateDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "MIDI note gate fixture should stage with shared built-ins");

    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "MIDI note baseline callback should render");
    require(bridge.enqueueMidiNoteOn(2, 64, 127, 10.25), "valid MIDI note-on should enqueue as one command");
    std::size_t allocations = 0;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto noteOnRendered = bridge.render({}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    allocations = allocationCount.load(std::memory_order_relaxed);
    require(noteOnRendered, "MIDI note-on callback should render");
    require(allocations == 0, "MIDI note-on drain and atomic batch should not allocate");
    require(std::abs(output[0]) < 1.0e-6F, "MIDI note-on should preserve pre-arrival frame");
    require(std::abs(output[1] - 1.0F) < 1.0e-6F, "MIDI note-on should open gate at exact arrival frame");

    require(bridge.enqueueMidiNoteOff(1, 64, 11.1), "other-channel note-off should enqueue");
    require(bridge.enqueueMidiNoteOff(2, 63, 11.2), "other-note note-off should enqueue");
    require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
            "unrelated MIDI note-offs should not stop rendering");
    require(std::abs(output[3] - 1.0F) < 1.0e-6F, "unrelated note-offs should leave active gate open");

    require(bridge.enqueueMidiNoteOff(2, 64, 12.25), "matching MIDI note-off should enqueue");
    require(bridge.render({}, 13.0, channels.data(), channels.size(), output.size()),
            "matching MIDI note-off callback should render");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F, "matching note-off should preserve pre-arrival frame");
    require(std::abs(output[1]) < 1.0e-6F, "matching note-off should close gate at exact arrival frame");
}

// MIDI notes off, a note-off for every note
// on every MIDI channel, sent a part of the control queue at a time,
// releases a note left hanging.
void testMidiNotesOffReleasesAHangingNote()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeMidiGateDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "MIDI note gate fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()), "the baseline should render");
    require(bridge.enqueueMidiNoteOn(5, 61, 127, 10.25)
            && bridge.render({}, 11.0, channels.data(), channels.size(), output.size()) && std::abs(output[3] - 1.0F) < 1.0e-6F,
        "the note should be held");
    auto time = 11.25;
    for (int channel = 1; channel <= 16; channel += 4, time += 1.0)
    {
        for (int sent = channel; sent < channel + 4; ++sent)
        {
            for (int note = 0; note <= 127; ++note)
                require(bridge.enqueueMidiNoteOff(sent, note, time), "each note-off should enqueue");
        }
        require(bridge.render({}, time + 0.75, channels.data(), channels.size(), output.size()), "the note-offs should render");
        require((channel < 5) == (std::abs(output[3] - 1.0F) < 1.0e-6F) && (channel < 5 || std::abs(output[3]) < 1.0e-6F),
            "the note should be held until its channel's note-offs arrive, then released");
    }
}

void testBridgeZeroVelocityNoteOnMatchesNoteOffAndRetainsVelocity()
{
    for (const bool zeroVelocityNoteOn : {false, true})
    {
        talas::runtime::ProjectRuntimeBridge bridge(
            makeMidiGateDocument("!KeyVelocity + !KeyDown * 0.1"), registry());
        bridge.setPlaying(true);
        require(bridge.compileAndStage(settings()).ok(),
            "live zero-velocity MIDI fixture should stage");
        std::array<float, 4> output{};
        std::array<float*, 1> channels{output.data()};
        require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "live zero-velocity baseline should render");
        require(bridge.enqueueMidiNoteOn(1, 60, 100, 10.25),
            "live velocity fixture attack should enqueue");
        require(bridge.render({}, 11.0, channels.data(), channels.size(), output.size()),
            "live velocity fixture attack should render");
        const auto heldGain = 100.0F / 127.0F + 0.1F;
        require(std::abs(output[0]) < 1.0e-6F
                && std::abs(output[1] - heldGain) < 1.0e-6F,
            "live attack must publish its original velocity at the arrival frame");

        const auto releaseAccepted = zeroVelocityNoteOn
            ? bridge.enqueueMidiNoteOn(1, 60, 0, 11.25)
            : bridge.enqueueMidiNoteOff(1, 60, 11.25);
        require(releaseAccepted, "live release should enqueue");
        require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
            "live release should render");
        require(std::abs(output[0] - heldGain) < 1.0e-6F
                && std::abs(output[1] - 100.0F / 127.0F) < 1.0e-6F,
            "zero-velocity release must close the gate while retaining attack velocity");
    }
}

void testReprepareEventValueIsRefusedAlone()
{
    // The VCA's latency follows its gain, so LatencyFrames needs a new
    // preparation while Level is an ordinary live control.
    auto registry = talas::core::makeBuiltinNodeRegistry();
    registry.setNodeTypeProcessingContract(
        "talas.vca",
        talas::core::NodeStateModel::Stateless,
        false,
        {
            talas::core::LatencyKind::PreparationTime,
            0,
            "gain",
            talas::core::PreparedLatencyRule::ParameterFrames,
        });
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {expressionParameter("gain", "!LatencyFrames")}},
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "1000 hz"),
            expressionParameter("amplitude", "!Level"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    document.virtualControls.push_back({
        "LatencyFrames", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader, 1.0, 0.0, 16.0});
    document.virtualControls.push_back({
        "Level", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader, 0.0, 0.0, 1.0});

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    auto renderSettings = settings();
    renderSettings.sampleRate = 48'000.0;
    renderSettings.maxBlockFrames = 64;
    bridge.setPlaying(true);
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), "reprepare fixture should stage: " + staged.message);
    std::array<float, 64> output{};
    require(renderFrames(bridge, output), "reprepare fixture baseline should render");
    const auto latencyId = bridge.realtimeEventValueIdForLatestStage(
        "LatencyFrames", talas::expr::ValueDomain::Scalar);
    const auto levelId = bridge.realtimeEventValueIdForLatestStage(
        "Level", talas::expr::ValueDomain::Scalar);
    require(latencyId.has_value() && levelId.has_value()
            && bridge.enqueueEventValue(
                *latencyId, 2.0, talas::expr::ValueDomain::Scalar)
            && bridge.enqueueEventValue(
                *levelId, 1.0, talas::expr::ValueDomain::Scalar),
        "both controls should enter the same callback batch");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    float peak = 0.0F;
    for (int callback = 0; callback < 4; ++callback)
    {
        require(renderFrames(bridge, output), "the batch callback should render");
        for (const auto sample : output)
            peak = std::max(peak, std::abs(sample));
    }
    require(peak > 0.5F
            && bridge.rejectedRealtimeControlCount() == rejectedBefore + 1U,
        "only the control that needs a new preparation may be refused: peak "
            + std::to_string(peak) + ", rejected "
            + std::to_string(bridge.rejectedRealtimeControlCount() - rejectedBefore));
}

void testRelocationStartsANoteWaitingForItsRetriggerDelay()
{
    // One voice whose level is 0 only while it waits for its delayed attack.
    auto document = makePolyphonicMidiVoiceDocument(1, 1, "1");
    document.graph.nodes.back().parameters.back().expression = "!KeyDown + 1";
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto renderSettings = settings();
    renderSettings.sampleRate = 48'000.0;
    renderSettings.maxBlockFrames = 64;
    bridge.setPlaying(true);
    require(bridge.compileAndStage(renderSettings).ok(),
        "retrigger relocation fixture should stage");
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    double time = 10.0;
    const auto renderNext = [&] {
        const auto rendered = bridge.render(
            {}, time, channels.data(), channels.size(), output.size());
        time += 64.0 / 48'000.0;
        return rendered;
    };
    require(renderNext(), "retrigger relocation baseline should render");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, time),
        "the note should enqueue");
    require(renderNext(), "the note should render");
    // The 5 ms retrigger delay (240 frames) has not elapsed yet.
    require(bridge.requestTransportRelocation(0),
        "the relocation should be accepted");
    float peak = 0.0F;
    for (int callback = 0; callback < 10; ++callback)
    {
        require(renderNext(), "the relocated transport should render");
        for (const auto sample : output)
            peak = std::max(peak, std::abs(sample));
    }
    require(peak > 1.0F,
        "a relocation must not leave the held note silent in its retrigger state: peak "
            + std::to_string(peak));
}

void testBridgeRoutesBuiltInMidiControlChange()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(1.0, "!cc18"), registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.defaultMidiChannel = 2;
    require(bridge.compileAndStage(renderSettings).ok(),
        "referenced built-in MIDI controller should stage without a manual binding");
    require(bridge.realtimeEventValueIdForLatestStage(
        "cc18", talas::expr::ValueDomain::Scalar).has_value(),
        "built-in controller binding should belong to the accepted stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "built-in controller baseline callback should render");
    require(!bridge.enqueueMidiControlChange(1, 18, 127, 10.25),
        "other MIDI channel should not control the built-in Event Value");
    require(!bridge.enqueueMidiControlChange(2, 128, 1, 10.25)
            && !bridge.enqueueMidiControlChange(2, 18, 128, 10.25),
        "invalid controller numbers and values should reject");
    require(bridge.enqueueMidiControlChange(2, 18, 64, 10.25, 9),
        "default-channel CC18 should enqueue at its arrival time");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render(
        {}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "built-in CC scheduling and render should remain allocation-free");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] - 64.0F / 127.0F) < 1.0e-6F,
        "CC18 should change the global Event Value on the exact arrival frame");
    bridge.clearAfterAudioStopped();
    require(!bridge.enqueueMidiControlChange(2, 18, 127, 11.25),
        "built-in CC must not enter a cleared audio stage");
}

// ADR-0024: the default map names follow their controllers on the default
// MIDI channel and !PitchBend follows pitch bend, unless the project maps the
// name itself.
void testBridgeRoutesDefaultMidiMapNames()
{
    const auto staged = [](talas::project::ProjectDocument document) {
        auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(
            std::move(document), registry());
        bridge->setPlaying(true);
        auto renderSettings = settings();
        renderSettings.defaultMidiChannel = 2;
        require(bridge->compileAndStage(renderSettings).ok(),
            "a default map name should stage without a manual binding");
        return bridge;
    };
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};

    auto volume = staged(makePlayableDocument(1.0, "!Volume"));
    require(volume->render({}, 10.0, channels.data(), channels.size(), output.size()),
        "the !Volume baseline should render");
    require(!volume->enqueueMidiControlChange(1, 7, 127, 10.25),
        "another MIDI channel should not set !Volume");
    require(volume->enqueueMidiControlChange(2, 7, 64, 10.25, 9),
        "controller 7 on the default channel should set !Volume");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = volume->render(
        {}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "a default map name should render without allocating");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] - 64.0F / 127.0F) < 1.0e-6F,
        "controller 7 should set !Volume on its arrival frame");

    auto bend = staged(makePlayableDocument(1.0, "!PitchBend"));
    require(bend->render({}, 10.0, channels.data(), channels.size(), output.size()),
        "the !PitchBend baseline should render");
    require(bend->enqueueMidiPitchBend(2, 0, 10.25),
        "pitch bend on the default channel should set !PitchBend");
    require(bend->render({}, 11.0, channels.data(), channels.size(), output.size())
            && std::abs(output[1] + 1.0F) < 1.0e-6F,
        "the lowest pitch bend should set !PitchBend to -1");

    // A Virtual Control with its own MIDI mapping keeps the name from the
    // default map.
    auto mappedDocument = makePlayableDocument(1.0, "!Volume");
    talas::project::VirtualControl control;
    control.eventName = "Volume";
    control.midiMapping = talas::project::MidiControlMapping{2, 20};
    mappedDocument.virtualControls.push_back(control);
    auto mapped = staged(std::move(mappedDocument));
    require(mapped->render({}, 10.0, channels.data(), channels.size(), output.size()),
        "the mapped !Volume baseline should render");
    require(!mapped->enqueueMidiControlChange(2, 7, 127, 10.25),
        "controller 7 should not set a !Volume the project maps itself");
}

// ADR-0024 amendment: a project's own global map
// replaces the Default map; an association takes its channel, range and
// grid.
void testBridgeRoutesProjectGlobalMap()
{
    const auto staged = [](talas::project::ProjectDocument document) {
        auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(
            std::move(document), registry());
        bridge->setPlaying(true);
        auto renderSettings = settings();
        renderSettings.defaultMidiChannel = 2;
        require(bridge->compileAndStage(renderSettings).ok(),
            "a global map name should stage without a manual binding");
        return bridge;
    };
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    auto document = makePlayableDocument(1.0, "!Brightness");
    document.globalMap = "!Brightness is: (`MIDIController74 channel: 3; min: 0.5 max: 0.75 grid: 0.05).\n";
    auto bright = staged(document);
    require(bright->render({}, 10.0, channels.data(), channels.size(), output.size()),
        "the !Brightness baseline should render");
    require(!bright->enqueueMidiControlChange(2, 74, 127, 10.25),
        "controller 74 on another channel than the map's should not set !Brightness");
    require(bright->enqueueMidiControlChange(3, 74, 80, 10.25, 9),
        "controller 74 on channel 3 should set !Brightness");
    require(bright->render({}, 11.0, channels.data(), channels.size(), output.size())
            && std::abs(output[1] - 0.65F) < 1.0e-6F,
        "the controller should be scaled to 0.5 to 0.75 and kept on its grid of 0.05");
    auto volumeDocument = makePlayableDocument(1.0, "!Volume");
    volumeDocument.globalMap = document.globalMap;
    auto volume = staged(volumeDocument);
    require(volume->render({}, 10.0, channels.data(), channels.size(), output.size()),
        "the !Volume baseline should render");
    require(!volume->enqueueMidiControlChange(2, 7, 127, 10.25),
        "a project's own global map should replace the Default map's !Volume");
    // An Event Value no map line names takes nothing from
    // the MIDI input; the surface, a Tool or OSC still sets it.
    auto wobbleDocument = makePlayableDocument(1.0, "!Wobble");
    wobbleDocument.globalMap = document.globalMap;
    auto wobble = staged(wobbleDocument);
    require(wobble->render({}, 10.0, channels.data(), channels.size(), output.size()), "the !Wobble baseline should render");
    const auto resting = output[1];
    auto fromMidi = false;
    for (const auto& [channel, controller] : {std::pair{2, 1}, std::pair{2, 7}, std::pair{1, 74}, std::pair{3, 1}})
        fromMidi = wobble->enqueueMidiControlChange(channel, controller, 100, 10.25) || fromMidi;
    require(!fromMidi && wobble->render({}, 11.0, channels.data(), channels.size(), output.size()) && output[1] == resting,
        "an Event Value no map line names should take nothing from MIDI");
    const auto wobbleId = wobble->realtimeEventValueIdForLatestStage("Wobble", talas::expr::ValueDomain::Scalar);
    require(wobbleId.has_value() && wobble->enqueueEventValue(*wobbleId, 0.25, talas::expr::ValueDomain::Scalar)
            && wobble->render({}, 12.0, channels.data(), channels.size(), output.size()) && std::abs(output[1] - 0.25F) < 1.0e-6F,
        "the surface should still set an Event Value no map line names");
}

// A global map may
// name any legal variable name as an Event Value's source. Only MIDI's own
// sources reach an Event Value from outside, so one mapped to another
// source takes nothing from MIDI; the surface or a tool sets it, within
// the range its options give.
void testGlobalMapOtherEventSourcesTakeNoMidi()
{
    auto document = makePlayableDocument(1.0, "!Brightness");
    document.globalMap = "!Brightness is: (`Glide min: 0.5 max: 0.75).\n";
    auto bridge = std::make_unique<talas::runtime::ProjectRuntimeBridge>(std::move(document), registry());
    bridge->setPlaying(true);
    auto renderSettings = settings();
    renderSettings.defaultMidiChannel = 2;
    require(bridge->compileAndStage(renderSettings).ok(), "an Event Value of another source should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge->render({}, 10.0, channels.data(), channels.size(), output.size())
            && std::abs(output[1] - 0.5F) < 1.0e-6F,
        "an Event Value of another source should start at its minimum");
    auto fromMidi = false;
    for (const auto& [channel, controller] : {std::pair{2, 0}, std::pair{2, 7}, std::pair{1, 74}, std::pair{16, 127}})
        fromMidi = bridge->enqueueMidiControlChange(channel, controller, 100, 10.25) || fromMidi;
    require(!fromMidi && bridge->render({}, 11.0, channels.data(), channels.size(), output.size())
            && std::abs(output[1] - 0.5F) < 1.0e-6F,
        "an Event Value of another source should take nothing from MIDI");
    const auto id = bridge->realtimeEventValueIdForLatestStage("Brightness", talas::expr::ValueDomain::Scalar);
    require(id.has_value() && bridge->enqueueEventValue(*id, 0.7, talas::expr::ValueDomain::Scalar)
            && bridge->render({}, 12.0, channels.data(), channels.size(), output.size())
            && std::abs(output[1] - 0.7F) < 1.0e-6F,
        "the surface or a tool should set an Event Value of another source");
}

void testBridgeBuiltInMidiControlRangeEndpoints()
{
    for (const auto controller : {0, 127})
    {
        std::string name = "cc";
        if (controller < 10) name += '0';
        name += std::to_string(controller);
        talas::runtime::ProjectRuntimeBridge bridge(
            makePlayableDocument(1.0, "!" + name), registry());
        bridge.setPlaying(true);
        auto renderSettings = settings();
        renderSettings.defaultMidiChannel = 2;
        require(bridge.compileAndStage(renderSettings).ok(),
            "built-in controller endpoint should stage");
        std::array<float, 4> output{};
        std::array<float*, 1> channels{output.data()};
        require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "built-in controller endpoint baseline should render");
        require(bridge.enqueueMidiControlChange(2, controller, 127, 10.25),
            "built-in controller endpoint should enqueue");
        require(bridge.render({}, 11.0, channels.data(), channels.size(), output.size())
                && std::abs(output[1] - 1.0F) < 1.0e-6F,
            "CC00 and CC127 should both reach their canonical built-in names");
    }
}

void testBridgeRoutesOneBasedMidiProgramNumber()
{
    auto document = makeScheduledEventDocument();
    document.graph.nodes.back().parameters.front().expression =
        "!MIDIProgramNumber / 128";
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document), registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.defaultMidiChannel = 2;
    renderSettings.eventValues.push_back({"MIDIProgramNumber", 0.0});
    require(bridge.compileAndStage(renderSettings).ok(),
        "MIDI program Event Value fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "MIDI program baseline should render");
    require(!bridge.enqueueMidiProgramChange(1, 127, 10.25)
            && !bridge.enqueueMidiProgramChange(2, 128, 10.25),
        "unselected channel and invalid program must reject");
    require(bridge.enqueueMidiProgramChange(2, 0, 10.25),
        "raw program zero should enqueue");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto firstRendered = bridge.render(
        {}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(firstRendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "MIDI program ingress must render without callback allocation");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] - 1.0F / 128.0F) < 1.0e-6F,
        "raw program zero must become one at its arrival frame");
    require(bridge.enqueueMidiProgramChange(2, 127, 11.25),
        "raw program 127 should enqueue");
    require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
        "MIDI program upper endpoint should render");
    require(std::abs(output[0] - 1.0F / 128.0F) < 1.0e-6F
            && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "raw program 127 must become 128 at its arrival frame: "
            + std::to_string(output[0]) + ", "
            + std::to_string(output[1]));
    bridge.clearAfterAudioStopped();
    require(!bridge.enqueueMidiProgramChange(2, 0, 12.25),
        "MIDI program must not enter a cleared stage");
}

void testBridgeNormalizesMidiChannelPressure()
{
    auto document = makeScheduledEventDocument();
    document.graph.nodes.back().parameters.front().expression =
        "!MIDIChannelPressure";
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document), registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.defaultMidiChannel = 2;
    renderSettings.eventValues.push_back({"MIDIChannelPressure", 0.0});
    require(bridge.compileAndStage(renderSettings).ok(),
        "MIDI channel-pressure Event Value fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "MIDI pressure baseline should render");
    require(!bridge.enqueueMidiChannelPressure(1, 127, 10.25)
            && !bridge.enqueueMidiChannelPressure(2, 128, 10.25),
        "unselected channel and invalid pressure must reject");
    require(bridge.enqueueMidiChannelPressure(2, 64, 10.25),
        "midpoint channel pressure should enqueue");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto firstRendered = bridge.render(
        {}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(firstRendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "MIDI channel-pressure ingress must not allocate in callback");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] - 64.0F / 127.0F) < 1.0e-6F,
        "pressure 64 must normalize at its arrival frame");
    require(bridge.enqueueMidiChannelPressure(2, 127, 11.25),
        "maximum channel pressure should enqueue");
    require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
        "maximum channel pressure should render");
    require(std::abs(output[0] - 64.0F / 127.0F) < 1.0e-6F
            && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "pressure 127 must normalize to one after the midpoint");
    require(bridge.enqueueMidiChannelPressure(2, 0, 12.25),
        "zero channel pressure should enqueue");
    require(bridge.render({}, 13.0, channels.data(), channels.size(), output.size()),
        "zero channel pressure should render");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F
            && std::abs(output[1]) < 1.0e-6F,
        "pressure zero must normalize to zero at its arrival frame");
    bridge.clearAfterAudioStopped();
    require(!bridge.enqueueMidiChannelPressure(2, 127, 13.25),
        "MIDI pressure must not enter a cleared stage");
}

void testBridgeNormalizesMidiPitchBend()
{
    auto document = makeScheduledEventDocument();
    document.graph.nodes.back().parameters.front().expression =
        "!MIDIPitchBend";
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document), registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.defaultMidiChannel = 2;
    renderSettings.eventValues.push_back({"MIDIPitchBend", 0.0});
    require(bridge.compileAndStage(renderSettings).ok(),
        "MIDI pitch-bend Event Value fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "MIDI bend baseline should render");
    require(!bridge.enqueueMidiPitchBend(1, 0, 10.25)
            && !bridge.enqueueMidiPitchBend(2, 16384, 10.25),
        "unselected channel and invalid bend must reject");
    require(bridge.enqueueMidiPitchBend(2, 0, 10.25),
        "full-down MIDI bend should enqueue");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto downRendered = bridge.render(
        {}, 11.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(downRendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "MIDI bend ingress must not allocate in callback");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] + 1.0F) < 1.0e-6F,
        "full-down MIDI bend must reach -1 at its arrival frame");
    require(bridge.enqueueMidiPitchBend(2, 8192, 11.25),
        "center MIDI bend should enqueue");
    require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
        "center MIDI bend must render");
    require(std::abs(output[0] + 1.0F) < 1.0e-6F
            && std::abs(output[1]) < 1.0e-6F,
        "center MIDI bend must reach zero at its arrival frame");
    require(bridge.enqueueMidiPitchBend(2, 16383, 12.25),
        "full-up MIDI bend should enqueue");
    require(bridge.render({}, 13.0, channels.data(), channels.size(), output.size()),
        "full-up MIDI bend must render");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "full-up MIDI bend must reach +1 at its arrival frame");
    bridge.clearAfterAudioStopped();
    require(!bridge.enqueueMidiPitchBend(2, 8192, 13.25),
        "MIDI bend must not enter a cleared stage");
}

void testBridgeRoutesBuiltInMidiControlThroughVoices()
{
    for (const bool mapper : {false, true})
    {
        auto document = mapper
            ? makePolyphonicMidiMapperDocument()
            : makePolyphonicMidiVoiceDocument(2, 2, "1");
        for (auto& parameter : document.graph.nodes[1].parameters)
            if (parameter.parameterId == "channel")
                parameter.expression = "2";
        document.graph.nodes[2].parameters[0].expression = "100 hz";
        document.graph.nodes[2].parameters[1].expression =
            "!cc18 * !KeyDown";
        talas::runtime::ProjectRuntimeBridge bridge(document, registry());
        bridge.setPlaying(true);
        auto renderSettings = settings();
        renderSettings.sampleRate = 1000.0;
        renderSettings.maxBlockFrames = 64;
        renderSettings.defaultMidiChannel = 2;
        require(bridge.compileAndStage(renderSettings).ok(),
            "live Voice or Mapper built-in CC fixture should stage");
        std::array<float, 64> output{};
        std::array<float*, 1> channels{output.data()};
        require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "live CC voice baseline should render");
        require(bridge.enqueueMidiNoteOn(2, 60, 127, 10.0)
                && bridge.enqueueMidiControlChange(2, 18, 127, 10.02),
            "live note and default-channel CC should enter one prepared stage");
        require(bridge.render({}, 10.064, channels.data(), channels.size(), output.size()),
            "live CC voice callback should render");
        const auto quietBefore = std::ranges::all_of(
            std::span(output).subspan(0, 20),
            [](float sample) { return std::abs(sample) < 1.0e-6F; });
        const auto loudAfter = std::ranges::any_of(
            std::span(output).subspan(21, 10),
            [](float sample) { return std::abs(sample) > 0.3F; });
        require(quietBefore && loudAfter,
            "live CC should modulate the triggered Input: mapper="
                + std::to_string(mapper) + " frame19=" + std::to_string(output[19])
                + " frame21=" + std::to_string(output[21])
                + " frame25=" + std::to_string(output[25]));
    }
}

void testBridgeRoutesBuiltInMidiButtonsAndSwitches()
{
    for (const auto [name, controller] : {
             std::pair{"bt00", 0}, std::pair{"bt127", 127},
             std::pair{"sw00", 0}, std::pair{"sw127", 127}})
    {
        auto document = makeScheduledEventDocument();
        document.graph.nodes.back().parameters.front().expression =
            "!" + std::string(name);
        talas::runtime::ProjectRuntimeBridge bridge(
            std::move(document), registry());
        bridge.setPlaying(true);
        auto renderSettings = settings();
        renderSettings.defaultMidiChannel = 2;
        const auto staged = bridge.compileAndStage(renderSettings);
        require(staged.ok(),
            "built-in MIDI button or switch must stage: "
                + staged.message);
        require(bridge.realtimeEventValueIdForLatestStage(
                    name, talas::expr::ValueDomain::Scalar).has_value(),
            "the referenced button or switch needs an implicit stage binding");
        std::array<float, 4> output{};
        std::array<float*, 1> channels{output.data()};
        require(bridge.render({}, 10.0, channels.data(), channels.size(),
                    output.size()),
            "built-in MIDI button or switch baseline must render");
        require(!bridge.enqueueMidiControlChange(1, controller, 127, 10.25)
                && bridge.enqueueMidiControlChange(2, controller, 63, 10.25),
            "only the selected channel may drive the built-in alias");
        require(bridge.render({}, 11.0, channels.data(), channels.size(),
                    output.size())
                && std::ranges::all_of(output, [](float value) {
                    return std::abs(value) < 1.0e-6F;
                }),
            "MIDI CC values below the bounded binary threshold remain off");
        require(bridge.enqueueMidiControlChange(2, controller, 64, 11.25),
            "the binary on threshold must enqueue");
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto on = bridge.render({}, 12.0, channels.data(),
            channels.size(), output.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(on && allocationCount.load(std::memory_order_relaxed) == 0
                && std::abs(output[0]) < 1.0e-6F
                && std::abs(output[1] - 1.0F) < 1.0e-6F,
            "built-in button or switch must turn on at its arrival frame");
        require(bridge.enqueueMidiControlChange(2, controller, 0, 12.25)
                && bridge.render({}, 13.0, channels.data(),
                    channels.size(), output.size())
                && std::abs(output[0] - 1.0F) < 1.0e-6F
                && std::abs(output[1]) < 1.0e-6F,
            "built-in button or switch must turn off at its arrival frame");
    }
}

void testBridgeRoutesMidiControlThroughLocalMapperMap()
{
    auto document = makePolyphonicMidiMapperDocument();
    auto& mapper = document.graph.nodes[1];
    for (auto& parameter : mapper.parameters)
    {
        if (parameter.parameterId == "channel")
            parameter.expression = "2";
        if (parameter.parameterId == "map")
            parameter.text = "!LocalLevel is: `MIDIController18";
    }
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "mappedControllerInput";
    tone.displayName = "Mapped Controller Input";
    tone.graph.nodes = {
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "100 hz"),
            expressionParameter("amplitude", "!LocalLevel * (!KeyDown > 0)"),
        }},
    };
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "mappedControllerInput";
    document.graph.nodes[2] = std::move(source);
    document.graph.connections[0].fromNode = "source";

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 1000.0;
    renderSettings.maxBlockFrames = 64;
    renderSettings.defaultMidiChannel = 2;
    renderSettings.eventValues = {{"LocalLevel", 0.25}};
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), "local Mapper controller source must stage: "
        + staged.message + " "
        + (staged.diagnostics.empty() ? std::string{}
                                      : staged.diagnostics.front().message));
    require(bridge.realtimeEventValueIdForLatestStage(
        "cc18", talas::expr::ValueDomain::Scalar).has_value(),
        "local Mapper Map must expose an implicit global cc18 stage binding");
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "local Mapper controller baseline must render");
    require(bridge.enqueueMidiNoteOn(2, 60, 127, 10.0),
        "local Mapper controller note must enter the stage");
    require(!bridge.enqueueMidiControlChange(1, 18, 127, 10.01),
        "other-channel controller must not reach the local Map");
    require(bridge.enqueueMidiControlChange(2, 18, 127, 10.02),
        "default-channel controller must enter the local Map stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render(
        {}, 10.064, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "local Mapper controller callback must render without allocation");
    require(std::ranges::all_of(std::span(output).subspan(0, 20),
                [](float sample) { return std::abs(sample) < 1.0e-6F; })
            && std::ranges::any_of(std::span(output).subspan(21, 10),
                [](float sample) { return std::abs(sample) > 0.3F; }),
        "direct local Mapper controller source must modulate its triggered Input at frame 20");

    for (auto& parameter : document.graph.nodes[1].parameters)
        if (parameter.parameterId == "map")
            parameter.text =
                "!LocalLevel is: (`MIDIController18 min: 0.2 max: 0.8)";
    talas::runtime::ProjectRuntimeBridge rangedBridge(document, registry());
    rangedBridge.setPlaying(true);
    const auto rangedStage = rangedBridge.compileAndStage(renderSettings);
    require(rangedStage.ok(), "ranged local Mapper Map must stage: "
        + rangedStage.message + " "
        + (rangedStage.diagnostics.empty() ? std::string{}
                                            : rangedStage.diagnostics.front().message));
    require(rangedBridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
        "ranged local Mapper baseline must render");
    require(std::ranges::all_of(output,
                [](float sample) { return std::abs(sample) < 1.0e-6F; }),
        "ranged local Mapper Input must remain silent before a note");
    require(rangedBridge.enqueueMidiNoteOn(2, 60, 127, 10.0)
            && !rangedBridge.enqueueMidiControlChange(1, 18, 127, 10.01)
            && rangedBridge.enqueueMidiControlChange(2, 18, 127, 10.02),
        "ranged local Mapper Map must admit only the default-channel CC");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rangedRendered = rangedBridge.render(
        {}, 10.064, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rangedRendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "ranged local Mapper callback must render without allocation");
    require(std::abs(output[12]) > 0.1F
            && std::abs(output[22]) > 0.4F
            && std::abs(output[22] - 4.0F * output[12]) < 1.0e-5F,
        "ranged local Mapper live CC must move from min to max at frame 20: "
            + std::to_string(output[12]) + ", "
            + std::to_string(output[22]));

    for (auto& parameter : document.graph.nodes[1].parameters)
        if (parameter.parameterId == "map")
            parameter.text =
                "!LocalLevel is: (`MIDIController07 channel: 5)\n"
                "!DefaultLevel is: `MIDIController07";
    for (auto& parameter : document.soundDefinitions[0].graph.nodes[0].parameters)
        if (parameter.parameterId == "amplitude")
            parameter.expression =
                "(!LocalLevel + !DefaultLevel) * (!KeyDown > 0) / 2";
    auto channelSettings = renderSettings;
    channelSettings.eventValues.push_back({"DefaultLevel", 0.0});
    talas::runtime::ProjectRuntimeBridge channelBridge(document, registry());
    channelBridge.setPlaying(true);
    const auto channelStage = channelBridge.compileAndStage(channelSettings);
    require(channelStage.ok(), "channel-tagged local Mapper Map must stage: "
        + channelStage.message + " "
        + (channelStage.diagnostics.empty() ? std::string{}
                                             : channelStage.diagnostics.front().message));
    require(channelBridge.realtimeEventValueIdForLatestStage(
                "cc07", talas::expr::ValueDomain::Scalar).has_value()
            && channelBridge.realtimeEventValueIdForLatestStage(
                "cc07_ch05", talas::expr::ValueDomain::Scalar).has_value(),
        "default and tagged channels need independent stage bindings");
    require(channelBridge.render(
                {}, 10.0, channels.data(), channels.size(), output.size()),
        "channel-tagged local Mapper baseline must render");
    require(channelBridge.enqueueMidiNoteOn(2, 60, 127, 10.0)
            && !channelBridge.enqueueMidiControlChange(1, 7, 127, 10.005)
            && channelBridge.enqueueMidiControlChange(2, 7, 127, 10.01)
            && channelBridge.enqueueMidiControlChange(5, 7, 127, 10.02),
        "one CC number must route separately to default and tagged channels");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto channelRendered = channelBridge.render(
        {}, 10.064, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(channelRendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "channel-tagged Mapper callback must render without allocation");
    require(std::abs(output[12]) > 0.25F
            && std::abs(output[22] - 2.0F * output[12]) < 1.0e-5F,
        "default and tagged CC7 must independently add half the Input gain: "
            + std::to_string(output[12]) + ", "
            + std::to_string(output[22]));
}

void testBridgeRoutesDelayedKeyboardSourceThroughLocalMapperMap()
{
    auto document = makePolyphonicMidiMapperDocument();
    auto& mapper = document.graph.nodes[1];
    for (auto& parameter : mapper.parameters)
    {
        if (parameter.parameterId == "channel")
            parameter.expression = "2";
        if (parameter.parameterId == "map")
            parameter.text =
                "!LocalGate is: `MIDIKeyDown\n"
                "!LocalPitch is: `MIDIKeyNumber\n"
                "!LocalVelocity is: `MIDIKeyVelocity";
    }
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "mappedKeyboardBridgeInput";
    tone.displayName = "Mapped Keyboard Bridge Input";
    tone.graph.nodes = {
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "7 hz"),
            expressionParameter("amplitude", "!LocalGate"),
        }},
        {"pitchOsc", "talas.oscillator", {
            expressionParameter("frequency", "!LocalPitch"),
            expressionParameter("amplitude", "!LocalVelocity * (!LocalGate > 0)"),
        }},
    };
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"},
        {"rightOut", talas::core::PortType::Audio,
            "pitchOsc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "mappedKeyboardBridgeInput";
    document.graph.nodes[2] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    document.graph.connections.push_back({
        "source", "rightOut", "midi", "rightIn"});

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 1000.0;
    renderSettings.maxBlockFrames = 64;
    renderSettings.defaultMidiChannel = 2;
    renderSettings.eventValues = {{"LocalGate", 0.0},
        {"LocalPitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"LocalVelocity", 0.0}};
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), "keyboard Map bridge must stage: "
        + staged.message + " "
        + (staged.diagnostics.empty() ? std::string{}
                                      : staged.diagnostics.front().message));
    std::array<float, 64> output{}, rightOutput{};
    std::array<float*, 2> channels{output.data(), rightOutput.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "keyboard Map bridge baseline must render");
    require(bridge.enqueueMidiNoteOn(2, 60, 64, 10.0),
        "keyboard Map bridge note-on must enqueue");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto attacked = bridge.render(
        {}, 10.064, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(attacked && allocationCount.load(std::memory_order_relaxed) == 0,
        "keyboard Map live callback must not allocate");
    require(output[1] < -0.001F && output[6] > 0.001F,
        "keyboard Map must expose negative retrigger then positive gate: "
            + std::to_string(output[1]) + ", " + std::to_string(output[6])
            + ", " + std::to_string(output[12]));
    const auto hz60 = 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
    for (std::size_t frame = 0; frame < 9; ++frame)
    {
        const auto expected = frame < 5 ? 0.0
            : (64.0 / 127.0) * std::sin(
                2.0 * std::numbers::pi
                * static_cast<double>(64 + frame) * hz60 / 1000.0);
        require(std::abs(rightOutput[frame] - expected) < 1.0e-5F,
            "keyboard Map live pitch and normalized velocity frame "
                + std::to_string(frame) + ": "
                + std::to_string(rightOutput[frame]) + ", expected "
                + std::to_string(expected));
    }
    require(bridge.enqueueMidiNoteOff(2, 60, 10.064),
        "keyboard Map bridge note-off must enqueue");
    require(bridge.render({}, 10.128, channels.data(), channels.size(), output.size()),
        "keyboard Map bridge release must render");
    require(std::ranges::all_of(output, [](float value) {
            return std::abs(value) < 1.0e-6F;
        }) && std::ranges::all_of(rightOutput, [](float value) {
            return std::abs(value) < 1.0e-6F;
        }),
        "keyboard Map note-off must clear gate and audible pitch voice");
}

void testBridgeCombinesHeldKeyAndLivePitchBend()
{
    auto document = makePolyphonicMidiMapperDocument();
    for (auto& parameter : document.graph.nodes[1].parameters)
    {
        if (parameter.parameterId == "channel")
            parameter.expression = "2";
        if (parameter.parameterId == "map")
            parameter.text =
                "!Pitch is: (`MIDIKeyNumber + (12 * `MIDIPitchBend)) nn.";
    }
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "liveBentKeyboardInput";
    tone.displayName = "Live Bent Keyboard Input";
    tone.graph.nodes = {
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "!Pitch"),
            expressionParameter("amplitude", "(!KeyDown > 0)"),
        }},
    };
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "liveBentKeyboardInput";
    document.graph.nodes[2] = std::move(source);
    document.graph.connections[0].fromNode = "source";

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 1000.0;
    renderSettings.maxBlockFrames = 64;
    renderSettings.defaultMidiChannel = 2;
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), "live bend composition must stage: "
        + staged.message + " "
        + (staged.diagnostics.empty() ? std::string{}
                                      : staged.diagnostics.front().message));
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "live bend composition baseline must render");
    require(std::ranges::all_of(output, [](float value) {
            return std::abs(value) < 1.0e-6F;
        }), "untriggered bend composition must be silent");
    require(bridge.enqueueMidiNoteOn(2, 60, 127, 10.0)
            && bridge.enqueueMidiPitchBend(2, 16383, 10.02)
            && bridge.enqueueMidiPitchBend(2, 0, 10.04),
        "held note and live bend changes must enter the stage");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render(
        {}, 10.064, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "live bend composition callback must not allocate");
    auto phase = std::fmod(
        64.0 * 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0) / 1000.0,
        1.0);
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto bend = frame < 20 ? 0.0 : frame < 40 ? 1.0 : -1.0;
        const auto pitch = 60.0 + 12.0 * bend;
        const auto expected = frame < 5 ? 0.0
            : std::sin(2.0 * std::numbers::pi * phase);
        require(std::abs(output[frame] - expected) < 1.0e-5F,
            "live held-note bend must preserve oscillator phase at frame "
                + std::to_string(frame) + ": "
                + std::to_string(output[frame]) + ", expected "
                + std::to_string(expected));
        phase = std::fmod(phase + 440.0
            * std::pow(2.0, (pitch - 69.0) / 12.0) / 1000.0, 1.0);
    }
    require(bridge.enqueueMidiNoteOff(2, 60, 10.064)
            && bridge.render({}, 10.128, channels.data(), channels.size(),
                output.size()),
        "live bend composition note release must render");
    require(std::ranges::all_of(output, [](float value) {
            return std::abs(value) < 1.0e-6F;
        }), "releasing held note must silence the bent Input");
}

void testBridgeSelfNamedLocalMapUsesVirtualControlOnly()
{
    auto document = makePolyphonicMidiMapperDocument();
    for (auto& parameter : document.graph.nodes[1].parameters)
    {
        if (parameter.parameterId == "channel")
            parameter.expression = "2";
        if (parameter.parameterId == "map")
            parameter.text = "!LocalLevel is: `LocalLevel";
    }
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "virtualOnlyMapInput";
    tone.displayName = "Virtual Only Map Input";
    tone.graph.nodes = {
        {"osc", "talas.oscillator", {
            expressionParameter("frequency", "100 hz"),
            expressionParameter("amplitude",
                "!LocalLevel * (!KeyDown > 0)"),
        }},
    };
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "virtualOnlyMapInput";
    document.graph.nodes[2] = std::move(source);
    document.graph.connections[0].fromNode = "source";

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    const auto replaced = bridge.editor().replaceDocument(document);
    require(replaced.ok(), "self-named Map must pass project authoring");
    const auto& controls = bridge.editor().document().virtualControls;
    const auto control = std::ranges::find(controls, "LocalLevel",
        &talas::project::VirtualControl::eventName);
    require(control != controls.end()
            && control->kind == talas::project::VirtualControlKind::Fader
            && !control->midiMapping.has_value(),
        "self-named Map target must be an unmapped virtual fader");

    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 1000.0;
    renderSettings.maxBlockFrames = 64;
    renderSettings.defaultMidiChannel = 2;
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), "self-named local Map must stage: "
        + staged.message + " "
        + (staged.diagnostics.empty() ? std::string{}
                                      : staged.diagnostics.front().message));
    const auto levelId = bridge.realtimeEventValueIdForLatestStage(
        "LocalLevel", talas::expr::ValueDomain::Scalar);
    require(levelId.has_value(),
        "self-named local Map must expose its virtual Event Value");
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(),
                output.size()),
        "self-named local Map baseline must render");
    require(std::ranges::all_of(output,
                [](float sample) { return std::abs(sample) < 1.0e-6F; }),
        "virtual-only Input must remain silent before a note");
    require(bridge.enqueueMidiNoteOn(2, 60, 127, 10.0)
            && !bridge.enqueueMidiControlChange(2, 18, 127, 10.01)
            && bridge.enqueueEventValue(*levelId, 0.5,
                talas::expr::ValueDomain::Scalar, 20),
        "unmapped physical CC must not replace the virtual fader assignment");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render({}, 10.064, channels.data(),
        channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
        "self-named local Map callback must not allocate");
    require(std::ranges::all_of(std::span(output).subspan(0, 20),
                [](float sample) { return std::abs(sample) < 1.0e-6F; })
            && std::ranges::any_of(std::span(output).subspan(21, 10),
                [](float sample) { return std::abs(sample) > 0.2F; }),
        "virtual fader must drive only its triggered local Input at frame 20");
}

void testBridgeRoutesMappedMidiBeforeFallbackAndRevokesOldStage()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeMappedMidiMapperDocument("velocity"), registry(), 2);
    talas::runtime::ProjectRuntimeBridge reference(
        makeMappedMidiMapperDocument(""), registry(), 2);
    bridge.setPlaying(true);
    reference.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 4.0;
    renderSettings.maxBlockFrames = 4;
    require(bridge.compileAndStage(renderSettings).ok(),
        "mapped MIDI bridge fixture should stage");
    require(reference.compileAndStage(renderSettings).ok(),
        "blank MIDI Mapper reference should stage");
    std::array<float, 4> output{};
    std::array<float, 4> referenceOutput{};
    std::array<float*, 1> channels{output.data()};
    std::array<float*, 1> referenceChannels{referenceOutput.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
        "mapped MIDI baseline should activate the stage");
    require(reference.render({}, 10.0, referenceChannels.data(),
                referenceChannels.size(), referenceOutput.size()),
        "blank mapper baseline should activate the reference stage");
    require(bridge.enqueueMidiNoteOn(2, 64, 64, 10.25),
        "mapped MIDI note-on should enqueue through the product bridge");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    require(bridge.render({}, 11.0, channels.data(), channels.size(), output.size()),
        "mapped MIDI note-on callback should render");
    require(reference.render({}, 11.0, referenceChannels.data(),
                referenceChannels.size(), referenceOutput.size()),
        "blank mapper note-on reference should render");
    require(std::abs(output[0]) < 1.0e-6F
            && std::abs(output[1]
                    - referenceOutput[1] * (64.0F / 127.0F))
                < 1.0e-6F,
        "mapped MIDI velocity should begin at the exact arrival frame");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore,
        "handled mapper input should suppress fallback without rejection");

    require(bridge.enqueueMidiNoteOff(2, 64, 11.25),
        "mapped MIDI note-off should enqueue");
    require(bridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
        "mapped MIDI note-off callback should render");
    require(reference.render({}, 12.0, referenceChannels.data(),
                referenceChannels.size(), referenceOutput.size()),
        "blank mapper note-off reference should render");
    require(std::abs(output[0]
                - referenceOutput[0] * (64.0F / 127.0F)) < 1.0e-6F
            && std::abs(output[1]) < 1.0e-6F,
        "mapped MIDI note-off should close at the exact arrival frame");

    require(bridge.enqueueMidiNoteOn(1, 60, 127, 12.25),
        "old-stage mapper note should enqueue before replacement");
    require(bridge.compileAndStage(renderSettings).ok(),
        "replacement mapped MIDI stage should compile");
    const auto staleBefore = bridge.rejectedRealtimeControlCount();
    require(bridge.render({}, 13.0, channels.data(), channels.size(), output.size()),
        "replacement mapped MIDI stage should render closed");
    require(std::ranges::all_of(output, [](float sample) {
            return std::abs(sample) < 1.0e-6F;
        }),
        "replacement must not inherit old-stage held mapper state");
    require(bridge.rejectedRealtimeControlCount() == staleBefore + 1,
        "old-stage mapped MIDI command should reject once");
}

void testBridgeAcceptsMidiNoteBetweenStageAndFirstCallback()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeMidiGateDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "pre-activation MIDI fixture should stage");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, 1.0),
            "note after accepted stage should target the pending graph before first callback");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 2.0, channels.data(), channels.size(), output.size()),
            "first callback should activate pending graph and consume its MIDI note");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F,
            "first-callback MIDI note should use timestamp fallback at frame zero");
    require(bridge.rejectedRealtimeControlCount() == 0,
            "accepted-stage MIDI note should not be rejected before first callback");
}

void testBridgeSchedulesMidiAgainstSubsetBindingsAndRollsBackRejectedState()
{
    talas::runtime::ProjectRuntimeBridge subsetBridge(makeMidiGateDocument("!KeyDown"), registry());
    subsetBridge.setPlaying(true);
    require(subsetBridge.compileAndStage(settings()).ok(), "KeyDown-only MIDI subset should stage");
    require(subsetBridge.enqueueMidiNoteOn(1, 60, 1, 1.0), "KeyDown-only note should enqueue");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(subsetBridge.render({}, 2.0, channels.data(), channels.size(), output.size()),
            "KeyDown-only MIDI subset should render");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F, "KeyDown-only note should open gate without Pitch or velocity bindings");

    talas::runtime::ProjectRuntimeBridge rollbackBridge(makeMidiGateDocument("!KeyDown"), registry());
    rollbackBridge.setPlaying(true);
    auto rollbackSettings = settings();
    rollbackSettings.maxRealtimeEventTransactions = 1;
    require(rollbackBridge.compileAndStage(std::move(rollbackSettings)).ok(), "MIDI rollback fixture should stage");
    require(rollbackBridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "MIDI rollback baseline should render");
    const auto keyDownId = rollbackBridge.realtimeEventValueIdForLatestStage(
        "KeyDown", talas::expr::ValueDomain::Scalar);
    require(keyDownId.has_value(), "MIDI rollback fixture should register KeyDown");
    require(rollbackBridge.enqueueEventValue(*keyDownId, 0.5, talas::expr::ValueDomain::Scalar, 0),
            "MIDI rollback fixture should enqueue capacity-consuming scalar command");
    require(rollbackBridge.enqueueMidiNoteOn(1, 60, 127, 10.1),
            "MIDI rollback note should reach scheduler capacity rejection");
    const auto rejectedBefore = rollbackBridge.rejectedRealtimeControlCount();
    require(rollbackBridge.render({}, 11.0, channels.data(), channels.size(), output.size()),
            "MIDI rollback rejection should not stop rendering");
    require(rollbackBridge.rejectedRealtimeControlCount() == rejectedBefore + 1,
            "rejected MIDI atomic batch should increment telemetry once");
    require(std::abs(output[0] - 0.5F) < 1.0e-6F, "accepted scalar command should remain after rejected MIDI batch");
    require(rollbackBridge.enqueueMidiNoteOff(1, 60, 11.1), "post-rejection note-off should enqueue");
    require(rollbackBridge.render({}, 12.0, channels.data(), channels.size(), output.size()),
            "post-rejection note-off callback should render");
    require(std::abs(output[3] - 0.5F) < 1.0e-6F,
            "rejected note-on must not commit active identity consumed by later note-off");
}

void testBridgeHandlesMidiReplacementVelocityZeroAndStageIdentity()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeMidiGateDocument(), registry(), 2);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "MIDI replacement fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 20.0, channels.data(), channels.size(), output.size()),
            "MIDI replacement baseline should publish active stage ID");

    require(bridge.enqueueMidiNoteOn(1, 60, 64, 20.1), "first monophonic note should enqueue");
    require(bridge.enqueueMidiNoteOn(2, 67, 127, 20.2), "newer monophonic note should share one queue slot");
    require(bridge.render({}, 21.0, channels.data(), channels.size(), output.size()),
            "newer monophonic note should render");
    require(std::abs(output[3] - 1.0F) < 1.0e-6F, "newer note velocity should replace prior velocity");

    require(bridge.enqueueMidiNoteOn(1, 60, 0, 21.1), "velocity-zero old note should enqueue as note-off");
    require(bridge.render({}, 22.0, channels.data(), channels.size(), output.size()),
            "velocity-zero old note callback should render");
    require(std::abs(output[3] - 1.0F) < 1.0e-6F, "old velocity-zero note must not close newer active pair");
    require(bridge.enqueueMidiNoteOn(2, 67, 0, 22.1), "velocity-zero active note should enqueue as note-off");
    require(bridge.render({}, 23.0, channels.data(), channels.size(), output.size()),
            "velocity-zero active note callback should render");
    require(std::abs(output[3]) < 1.0e-6F, "velocity-zero active note should close gate");

    require(bridge.enqueueMidiNoteOn(2, 70, 127, 23.1), "pre-replacement note should enqueue for old active stage");
    require(bridge.compileAndStage(settings()).ok(), "MIDI replacement graph should stage");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    require(bridge.render({}, 24.0, channels.data(), channels.size(), output.size()),
            "replacement graph should activate and render");
    require(std::abs(output[3]) < 1.0e-6F, "old-stage note must not reopen replacement default gate");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore + 1,
            "stale stage note should increment rejection telemetry");

    require(!bridge.enqueueMidiNoteOn(0, 60, 1, 24.0), "invalid MIDI channel should reject");
    require(!bridge.enqueueMidiNoteOn(1, 128, 1, 24.0), "invalid MIDI note should reject");
    require(!bridge.enqueueMidiNoteOn(1, 60, 128, 24.0), "invalid MIDI velocity should reject");
    require(!bridge.enqueueMidiNoteOff(17, 60, 24.0), "invalid note-off channel should reject");
    require(!bridge.enqueueMidiNoteOff(1, 60, std::numeric_limits<double>::infinity()),
            "non-finite MIDI timestamp should reject");
}

void testBridgeRoutesPreparedMidiBeforeMonophonicFallbackWithoutAllocations()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePolyphonicMidiVoiceDocument(1), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(polyphonicMidiSettings()).ok(),
            "prepared MIDI bridge fixture should stage");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, 1.0),
            "prepared MIDI note should enqueue before first activation");

    std::array<float, 16> output{};
    std::array<float*, 1> channels{output.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = bridge.render({}, 2.0, channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered, "prepared MIDI bridge note should render");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared MIDI bridge drain and render should not allocate");
    require(std::ranges::any_of(std::span<const float>(output).subspan(5), [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
            "handled prepared MIDI should sound without mutating the global monophonic KeyDown");
    require(bridge.rejectedRealtimeControlCount() == 0,
            "handled prepared MIDI should not increment rejection telemetry");

    require(bridge.enqueueMidiNoteOff(1, 60, 2.1), "prepared MIDI note-off should enqueue");
    require(bridge.render({}, 3.0, channels.data(), channels.size(), output.size()),
            "prepared MIDI release should render");
    for (std::size_t frame = 2; frame < output.size(); ++frame)
        require(std::abs(output[frame]) < 1.0e-6F,
                "prepared MIDI note-off should release its voice at the mapped frame");
}

void testBridgeRejectsPreparedMidiWithoutFallingBackOrMutatingVoiceState()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePolyphonicMidiVoiceDocument(1), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(polyphonicMidiSettings(1)).ok(),
            "prepared MIDI rejection fixture should stage");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, 1.0),
            "first prepared MIDI command should enqueue");
    require(bridge.enqueueMidiNoteOn(1, 72, 127, 1.0),
            "second prepared MIDI command should reach the prepared scheduler");
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();

    std::array<float, 16> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 2.0, channels.data(), channels.size(), output.size()),
            "transactionally rejected prepared MIDI batch should leave rendering available");
    require(bridge.rejectedRealtimeControlCount() == rejectedBefore + 1,
            "prepared MIDI scheduler rejection should increment telemetry exactly once");
    require(std::ranges::none_of(output, [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
            "prepared MIDI capacity rejection must not partially commit the first note or fall back to global KeyDown");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, 2.1),
            "valid prepared MIDI should enqueue after transactional rollback");
    require(bridge.render({}, 3.0, channels.data(), channels.size(), output.size()),
            "prepared MIDI scheduler should recover after batch rollback");
    require(std::ranges::any_of(std::span<const float>(output).subspan(5), [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
            "valid prepared MIDI after rollback should prove queue and voice state recovery");
}

void testBridgePolyphonicMapperRollsBackAndReplacesVoiceStage()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePolyphonicMidiMapperDocument(), registry());
    bridge.setPlaying(true);
    const auto renderSettings = polyphonicMidiSettings(1);
    require(bridge.compileAndStage(renderSettings).ok(),
        "polyphonic Mapper must stage through the product bridge");
    require(bridge.enqueueMidiNoteOn(1, 60, 127, 1.0)
            && bridge.enqueueMidiNoteOn(1, 72, 127, 1.0),
        "two Mapper notes must reach the prepared scheduler");

    std::array<float, 16> output{};
    std::array<float*, 1> channels{output.data()};
    const auto rejectedBefore = bridge.rejectedRealtimeControlCount();
    require(bridge.render({}, 2.0, channels.data(), channels.size(),
                output.size())
            && bridge.rejectedRealtimeControlCount()
                == rejectedBefore + 1
            && std::ranges::none_of(output, [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
        "rejected Mapper batch must leave both Input voices silent");

    require(bridge.enqueueMidiNoteOn(1, 60, 127, 2.1)
            && bridge.render({}, 3.0, channels.data(), channels.size(),
                output.size())
            && std::ranges::any_of(output, [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
        "Mapper voice must recover after the rejected batch");

    require(bridge.enqueueMidiNoteOn(1, 72, 127, 3.1)
            && bridge.compileAndStage(renderSettings).ok(),
        "replacement Mapper stage must prepare while an old note is queued");
    const auto staleBefore = bridge.rejectedRealtimeControlCount();
    require(bridge.render({}, 4.0, channels.data(), channels.size(),
                output.size())
            && std::ranges::none_of(output, [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            })
            && bridge.rejectedRealtimeControlCount() == staleBefore + 1,
        "replacement Mapper stage must discard old held and queued voices");
    require(bridge.enqueueMidiNoteOn(1, 64, 127, 4.1)
            && bridge.render({}, 5.0, channels.data(), channels.size(),
                output.size())
            && std::ranges::any_of(output, [](float sample) {
                return std::abs(sample) > 1.0e-5F;
            }),
        "replacement Mapper stage must accept its own new note");
}

void testBridgeRetainsSameFrameMidiIngressOrderForPreparedVoiceStealing()
{
    talas::runtime::ProjectRuntimeBridge ordered(makePolyphonicMidiVoiceDocument(1), registry());
    talas::runtime::ProjectRuntimeBridge reference(makePolyphonicMidiVoiceDocument(1), registry());
    ordered.setPlaying(true);
    reference.setPlaying(true);
    require(ordered.compileAndStage(polyphonicMidiSettings()).ok(),
            "ordered prepared MIDI fixture should stage");
    require(reference.compileAndStage(polyphonicMidiSettings()).ok(),
            "ordered prepared MIDI reference should stage");
    require(ordered.enqueueMidiNoteOn(1, 60, 127, 1.0),
            "first same-frame prepared note should enqueue");
    require(ordered.enqueueMidiNoteOn(1, 72, 127, 1.0),
            "second same-frame prepared note should enqueue");
    require(reference.enqueueMidiNoteOn(1, 72, 127, 1.0),
            "same-frame prepared reference note should enqueue");

    std::array<float, 16> orderedOutput{};
    std::array<float, 16> referenceOutput{};
    std::array<float*, 1> orderedChannels{orderedOutput.data()};
    std::array<float*, 1> referenceChannels{referenceOutput.data()};
    require(ordered.render({}, 2.0, orderedChannels.data(), orderedChannels.size(), orderedOutput.size()),
            "same-frame prepared notes should render");
    require(reference.render({}, 2.0, referenceChannels.data(), referenceChannels.size(), referenceOutput.size()),
            "same-frame prepared reference should render");
    for (std::size_t frame = 0; frame < orderedOutput.size(); ++frame)
    {
        require(std::abs(orderedOutput[frame] - referenceOutput[frame]) < 1.0e-6F,
                "same-frame ingress sequence should deterministically leave the newer stolen pitch active");
    }
}

void testBridgeStagesPersistedVirtualControlValues()
{
    auto document = makePlayableDocument(1.0, "!Gain");
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.25;
    document.virtualControls.push_back(gain);

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(),
            "persisted Virtual Control should provide the initial realtime Event Value");

    std::array<float, 4> output{};
    require(render(bridge, output), "Virtual Control graph should activate and render");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F,
            "persisted Virtual Control value should start the Event Value without a caller binding");

    // A value the caller gives for this stage, as a plugin's parameter or
    // the CLI's --event, wins over the saved one.
    talas::runtime::ProjectRuntimeBridge given(std::move(document), registry());
    given.setPlaying(true);
    auto givenSettings = settings();
    givenSettings.eventValues.push_back({"Gain", 0.75, talas::expr::ValueDomain::Scalar});
    require(given.compileAndStage(std::move(givenSettings)).ok(),
            "a caller binding for a persisted Virtual Control should stage");
    output.fill(0.0F);
    require(render(given, output), "caller-bound Virtual Control graph should activate and render");
    require(std::abs(output[1] - 0.75F) < 1.0e-6F,
            "caller binding should override the persisted Virtual Control value");
}

void testProjectRenderSettingsPreparePersistedControlsTimelineAndMidiDefaults()
{
    auto document = makePlayableDocument(1.0, "!Gain + !KeyDown");
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.25;
    document.virtualControls.push_back(gain);
    document.timeline.eventValues = {
        {0.375, "Gain", talas::expr::ValueDomain::Scalar, 0.75,
         talas::core::AutomationInterpolation::Linear},
    };
    document.timeline.loop = {true, 0.125, 1.25};

    auto renderSettings = settings();
    renderSettings.sampleRate = 8.0;
    const auto prepared = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        renderSettings);

    require(prepared.ok(), "shared project render settings should prepare");
    const auto gainBindings = std::ranges::count_if(
        prepared.settings->eventValues,
        [](const auto& binding) { return binding.name == "Gain"; });
    require(gainBindings == 1, "persisted Virtual Control should add one binding");
    const auto preparedGain = std::ranges::find_if(
        prepared.settings->eventValues,
        [](const auto& binding) { return binding.name == "Gain"; });
    require(preparedGain != prepared.settings->eventValues.end()
                && std::abs(preparedGain->value - 0.25) < 1.0e-12,
            "persisted Virtual Control value should start the Event Value");

    auto givenSettings = renderSettings;
    givenSettings.eventValues = {{"Gain", 0.5, talas::expr::ValueDomain::Scalar}};
    const auto given = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        std::move(givenSettings));
    require(given.ok(), "a caller binding for a persisted Virtual Control should prepare");
    require(std::ranges::count_if(
                given.settings->eventValues,
                [](const auto& binding) { return binding.name == "Gain"; })
                == 1,
            "a caller binding should not gain a second binding from its Virtual Control");
    const auto givenGain = std::ranges::find_if(
        given.settings->eventValues,
        [](const auto& binding) { return binding.name == "Gain"; });
    require(givenGain != given.settings->eventValues.end()
                && std::abs(givenGain->value - 0.5) < 1.0e-12,
            "a caller binding should win over the persisted Virtual Control value");

    auto wrongDomainSettings = renderSettings;
    wrongDomainSettings.eventValues = {{"Gain", 440.0, talas::expr::ValueDomain::Hertz}};
    const auto wrongDomain = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        std::move(wrongDomainSettings));
    require(!wrongDomain.ok()
                && wrongDomain.error.find("Virtual Control (Gain)") != std::string::npos,
            "a caller binding in another domain than its Virtual Control should fail clearly");
    require(prepared.settings->eventValueChanges.size() == 1
                && prepared.settings->eventValueChanges.front().frame == 3
                && prepared.settings->eventValueChanges.front().interpolation
                    == talas::core::AutomationInterpolation::Linear,
            "persisted Timeline should round to the nearest absolute sample and preserve interpolation");
    require(
        prepared.settings->transportLoop
            == talas::runtime::TransportLoopRegion{1, 10},
        "persisted Timeline loop should round both bounds to exact transport frames");
    const auto keyDown = std::ranges::find_if(
        prepared.settings->eventValues,
        [](const auto& binding) { return binding.name == "KeyDown"; });
    require(keyDown != prepared.settings->eventValues.end() && keyDown->value == 0.0,
            "referenced built-in MIDI Event Value should receive its default");

    auto invalidSettings = settings();
    invalidSettings.sampleRate = 0.0;
    const auto invalid = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        std::move(invalidSettings));
    require(!invalid.ok() && invalid.error.find("Timeline") != std::string::npos,
            "invalid Timeline sample rate should fail with context");

    auto pitchDocument = makePlayableDocument(1.0);
    pitchDocument.graph.nodes[2].parameters[0].expression = "!Pitch";
    pitchDocument.timeline.eventValues = {
        {0.5, "Pitch", talas::expr::ValueDomain::NoteNumber, 69.0},
    };
    auto pitchSettings = settings();
    pitchSettings.sampleRate = 4.0;
    const auto preparedPitch = talas::runtime::prepareProjectRenderSettings(
        pitchDocument,
        registry(),
        std::move(pitchSettings));
    require(preparedPitch.ok(), "future Pitch Timeline should prepare");
    const auto pitch = std::ranges::find_if(
        preparedPitch.settings->eventValues,
        [](const auto& binding) { return binding.name == "Pitch"; });
    require(pitch != preparedPitch.settings->eventValues.end()
                && pitch->domain == talas::expr::ValueDomain::NoteNumber
                && pitch->value == 60.0,
            "future Pitch Timeline should retain the built-in note-60 initial value");
    require(preparedPitch.settings->eventValueChanges.size() == 1
                && preparedPitch.settings->eventValueChanges.front().frame == 2
                && preparedPitch.settings->eventValueChanges.front().value == 69.0,
            "future Pitch Timeline should still schedule the persisted change");
}

void testProjectRenderSettingsSupplyDiscoveredEventValueDefaults()
{
    const auto document = makePlayableDocument(1.0, "!AutoGain");

    const auto automatic = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        settings());
    require(automatic.ok(),
            "shared settings should discover a missing Event Value binding");
    const auto automaticGain = std::ranges::find_if(
        automatic.settings->eventValues,
        [](const auto& binding) { return binding.name == "AutoGain"; });
    require(automaticGain != automatic.settings->eventValues.end()
                && automaticGain->domain == talas::expr::ValueDomain::Scalar
                && automaticGain->value == 0.0,
            "discovered scalar control should receive its persisted default");

    auto matchingHost = settings();
    matchingHost.eventValues.push_back({
        "AutoGain",
        0.75,
        talas::expr::ValueDomain::Scalar,
    });
    const auto matched = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        std::move(matchingHost));
    require(matched.ok(), "matching host binding should satisfy discovery");
    const auto matchedGain = std::ranges::find_if(
        matched.settings->eventValues,
        [](const auto& binding) { return binding.name == "AutoGain"; });
    require(matchedGain != matched.settings->eventValues.end()
                && matchedGain->value == 0.75,
            "missing persisted control should not overwrite a matching host binding");

    auto conflictingHost = settings();
    conflictingHost.eventValues.push_back({
        "AutoGain",
        440.0,
        talas::expr::ValueDomain::Hertz,
    });
    const auto conflicted = talas::runtime::prepareProjectRenderSettings(
        document,
        registry(),
        std::move(conflictingHost));
    require(!conflicted.ok()
                && conflicted.error.find("AutoGain") != std::string::npos,
            "host binding with the wrong discovered domain should fail clearly");
}

void testProjectRenderSettingsDiscoverBuiltinsInDescriptorDefaults()
{
    talas::core::NodeRegistry defaultRegistry;
    defaultRegistry.addNodeType({
        "test.defaultBuiltin",
        "Default Builtin",
        "Test",
        {},
        {},
        {{
            "level",
            "!KeyDown",
            talas::expr::ValueDomain::Scalar,
        }},
    });
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "default",
        "test.defaultBuiltin",
        {},
        {},
    });

    const auto prepared = talas::runtime::prepareProjectRenderSettings(
        document,
        defaultRegistry,
        settings());
    require(prepared.ok(),
            "built-ins referenced by effective descriptor defaults should prepare");
    const auto keyDown = std::ranges::find_if(
        prepared.settings->eventValues,
        [](const auto& binding) { return binding.name == "KeyDown"; });
    require(keyDown != prepared.settings->eventValues.end()
                && keyDown->domain == talas::expr::ValueDomain::Scalar
                && keyDown->value == 0.0,
            "descriptor-default KeyDown should receive its shared runtime default");
}

void testBridgeStagesReferencedBuiltinMidiEventDefaults()
{
    auto pitchDocument = makePlayableDocument(1.0);
    pitchDocument.graph.nodes[2].parameters[0].expression = "!Pitch";
    talas::runtime::ProjectRuntimeBridge pitchBridge(std::move(pitchDocument), registry());
    pitchBridge.setPlaying(true);
    auto pitchSettings = settings();
    pitchSettings.sampleRate = 48000.0;
    require(pitchBridge.compileAndStage(std::move(pitchSettings)).ok(),
            "referenced Pitch should receive a shared default binding");
    const auto pitchId = pitchBridge.realtimeEventValueIdForLatestStage("Pitch", talas::expr::ValueDomain::NoteNumber);
    require(pitchId.has_value(), "default Pitch should publish latest-stage note-number membership");
    std::array<float, 4> pitchOutput{};
    require(render(pitchBridge, pitchOutput), "default Pitch graph should render");
    const auto middleCHz = 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
    require(std::abs(pitchOutput[1] - static_cast<float>(std::sin(2.0 * std::numbers::pi * middleCHz / 48000.0))) < 1.0e-6F,
            "default Pitch should be MIDI note 60");

    talas::runtime::ProjectRuntimeBridge gateBridge(
        makePlayableDocument(1.0, "!KeyDown + !KeyVelocity"), registry());
    gateBridge.setPlaying(true);
    require(gateBridge.compileAndStage(settings()).ok(),
            "referenced KeyDown and KeyVelocity should receive shared defaults");
    std::array<float, 4> gateOutput{};
    require(render(gateBridge, gateOutput), "default keyboard scalar graph should render");
    require(std::abs(gateOutput[1]) < 1.0e-6F, "default KeyDown and KeyVelocity should both be zero");
}

void testBridgePreservesExplicitBuiltinMidiBindings()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(1.0, "!KeyDown + !KeyVelocity"), registry());
    bridge.setPlaying(true);
    auto explicitSettings = settings();
    explicitSettings.eventValues = {
        {"KeyDown", 0.25, talas::expr::ValueDomain::Scalar},
        {"KeyVelocity", 0.5, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(explicitSettings)).ok(),
            "explicit built-in bindings should remain authoritative");
    std::array<float, 4> output{};
    require(render(bridge, output), "explicit built-in graph should render");
    require(std::abs(output[1] - 0.75F) < 1.0e-6F,
            "explicit KeyDown and KeyVelocity values should not be replaced by defaults");

    talas::runtime::ProjectRuntimeBridge unresolved(makePlayableDocument(1.0, "!Custom"), registry());
    require(unresolved.compileAndStage(settings()).ok(),
            "arbitrary valid Event Values should receive discovered control defaults");
    require(
        unresolved.realtimeEventValueIdForLatestStage(
            "Custom",
            talas::expr::ValueDomain::Scalar)
            .has_value(),
        "discovered Event Value defaults should publish latest-stage membership");

    talas::runtime::ProjectRuntimeBridge malformed(makePlayableDocument(1.0, "!KeyDown +"), registry());
    require(!malformed.compileAndStage(settings()).ok(),
            "malformed built-in expression must fail instead of triggering default discovery");

    auto virtualPitchDocument = makePlayableDocument(1.0);
    virtualPitchDocument.graph.nodes[2].parameters[0].expression = "!Pitch";
    talas::project::VirtualControl virtualPitch;
    virtualPitch.eventName = "Pitch";
    virtualPitch.domain = talas::expr::ValueDomain::NoteNumber;
    virtualPitch.value = 69.0;
    virtualPitch.minimum = 0.0;
    virtualPitch.maximum = 127.0;
    virtualPitchDocument.virtualControls.push_back(std::move(virtualPitch));
    talas::runtime::ProjectRuntimeBridge virtualPitchBridge(std::move(virtualPitchDocument), registry());
    virtualPitchBridge.setPlaying(true);
    auto virtualPitchSettings = settings();
    virtualPitchSettings.sampleRate = 48000.0;
    require(virtualPitchBridge.compileAndStage(std::move(virtualPitchSettings)).ok(),
            "persisted virtual Pitch should remain authoritative over the shared default");
    std::array<float, 4> virtualPitchOutput{};
    require(render(virtualPitchBridge, virtualPitchOutput), "persisted virtual Pitch graph should render");
    require(std::abs(virtualPitchOutput[1] - static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 / 48000.0))) < 1.0e-6F,
            "persisted virtual Pitch should override MIDI note 60 default with note 69");
}

void testBridgeRendersBorrowedAudioInputEndToEnd()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeAudioInputDocument(), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "bridge AudioInput graph should stage");

    std::array<float, 4> input{1.0F, 2.0F, 3.0F, 4.0F};
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float, 4> output{};
    std::array<float*, 1> outputChannels{output.data()};
    require(bridge.render({inputChannels.data(), inputChannels.size()}, outputChannels.data(), outputChannels.size(), output.size()),
            "bridge should render borrowed AudioInput end to end");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F, "bridge AudioInput frame zero");
    require(std::abs(output[3] - 4.0F) < 1.0e-6F, "bridge AudioInput frame three");
}

void testBridgePublishesAnalysisCaptureOnlyAfterAcceptedStage()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeDisplayDocument("talas.oscilloscopeDisplay"),
        registry());
    bridge.setPlaying(true);
    auto firstSettings = settings();
    firstSettings.enableRealtimeAnalysisCapture = true;
    require(bridge.compileAndStage(std::move(firstSettings)).ok(),
            "first analysis graph should stage");
    const auto firstCapture = bridge.latestAnalysisCapture();
    require(firstCapture != nullptr
                && firstCapture->preferredProbeIndex("display").has_value(),
            "accepted scope stage should publish its capture");
    const auto firstProbe = firstCapture->preferredProbeIndex("display");
    require(firstCapture->probeSpec(*firstProbe)->kind
                == talas::runtime::RealtimeAnalysisKind::Oscilloscope,
            "accepted scope stage should publish scope metadata");
    auto metrics = bridge.realtimeMetrics();
    require(
        metrics.activeStageId == 0 && metrics.analysisProbeCount == 0,
        "pending analysis capture must not be attributed to the old active stage");

    require(bridge.editor().replaceDocument(
                makeDisplayDocument("talas.spectrumAnalyzerDisplay")).ok(),
            "editor should accept replacement analyzer document");
    auto rejectedSettings = settings();
    rejectedSettings.enableRealtimeAnalysisCapture = true;
    const auto rejected = bridge.compileAndStage(std::move(rejectedSettings));
    require(rejected.outcome == talas::runtime::ProjectStageOutcome::StageRejected,
            "second stage should reject while first graph awaits activation");
    require(bridge.latestAnalysisCapture() == firstCapture,
            "rejected stage must preserve last accepted analysis capture");

    std::array<float, 4> output{};
    require(render(bridge, output), "first analysis stage should activate");
    metrics = bridge.realtimeMetrics();
    require(
        metrics.activeStageId != 0 && metrics.analysisProbeCount == 1,
        "adopted analysis capture should bind to its exact active stage");
    auto replacementSettings = settings();
    replacementSettings.enableRealtimeAnalysisCapture = true;
    require(bridge.compileAndStage(std::move(replacementSettings)).ok(),
            "analyzer replacement should stage after activation");
    const auto replacementCapture = bridge.latestAnalysisCapture();
    require(replacementCapture != nullptr && replacementCapture != firstCapture,
            "accepted replacement should publish new capture ownership");
    const auto replacementProbe = replacementCapture->preferredProbeIndex("display");
    require(replacementProbe.has_value()
                && replacementCapture->probeSpec(*replacementProbe)->kind
                    == talas::runtime::RealtimeAnalysisKind::Spectrum,
            "accepted replacement should publish spectrum metadata");
    metrics = bridge.realtimeMetrics();
    require(
        metrics.analysisProbeCount == 0,
        "pending replacement capture must not be projected onto the incumbent stage");

    require(bridge.editor().disconnect("out", "audioIn").ok(),
            "editor should allow an invalid analysis graph");
    auto failedSettings = settings();
    failedSettings.enableRealtimeAnalysisCapture = true;
    require(bridge.compileAndStage(std::move(failedSettings)).outcome
                == talas::runtime::ProjectStageOutcome::CompileFailed,
            "invalid replacement should fail compilation");
    require(bridge.latestAnalysisCapture() == replacementCapture,
            "failed compile must preserve last accepted analysis capture");
}

void testBridgeExposesAcceptedSampleRateTransportAndStopsAtZero()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(0.0), registry());
    require(bridge.transportFramePosition() == 0, "bridge transport should start at zero");
    require(bridge.transportTimeSeconds() == 0.0, "unstaged bridge transport time should be zero");
    require(bridge.compileAndStage(settings()).ok(), "bridge transport fixture should stage");

    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output), "bridge transport fixture should render");
    require(bridge.transportFramePosition() == 4, "bridge should expose the host frame position");
    require(std::abs(bridge.transportTimeSeconds() - 1.0) < 1.0e-12,
            "bridge should convert transport frames with the accepted sample rate");

    auto replacementSettings = settings();
    replacementSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(std::move(replacementSettings)).ok(),
            "different-rate bridge replacement should stage");
    require(std::abs(bridge.transportTimeSeconds() - 1.0) < 1.0e-12,
            "pending replacement must not change the displayed transport time");
    require(render(bridge, output),
            "different-rate bridge replacement should activate");
    require(bridge.transportFramePosition() == 4,
            "different-rate bridge replacement should start a new frame epoch");
    require(std::abs(bridge.transportTimeSeconds() - 0.5) < 1.0e-12,
            "bridge should publish the new sample rate only after activation");

    bridge.setPlaying(false);
    require(!render(bridge, output), "paused bridge transport should not render");
    require(bridge.transportFramePosition() == 4, "pause should preserve bridge transport");

    bridge.stopTransport();
    require(bridge.transportFramePosition() == 4,
            "stop reset should remain asynchronous until the next callback");
    require(!render(bridge, output), "stopped bridge should adopt reset while silent");
    require(bridge.transportFramePosition() == 0, "stopped bridge transport should reset to zero");
    require(bridge.transportTimeSeconds() == 0.0, "stopped bridge time should reset to zero");
}

void testInternalTransportRelocationAdoptsAtOneAudioBoundary()
{
    talas::runtime::ProjectRuntimeBridge bridge(makePlayableDocument(0.0), registry());
    require(bridge.compileAndStage(settings()).ok(),
            "internal relocation fixture should stage");
    bridge.setPlaying(true);
    require(bridge.requestTransportRelocation(120),
            "internal relocation should publish one bounded control request");
    std::array<float, 4> output{};
    require(render(bridge, output),
            "internal relocation fixture should render its adoption callback");
    require(bridge.transportFramePosition() == 124,
            "the audio boundary should relocate to the exact requested frame before advancing the callback");

    require(bridge.requestTransportRelocation(240),
            "a second relocation should bind to the currently accepted stage");
    require(bridge.compileAndStage(settings()).ok(),
            "the relocation invalidation fixture should accept a replacement stage");
    require(render(bridge, output),
            "the replacement stage should render after rejecting prior-stage seek authority");
    require(bridge.transportFramePosition() != 244,
            "a seek queued for the prior stage must not relocate the replacement graph");

    require(bridge.requestTransportRelocation(360),
            "an internal relocation should publish before authority replacement");
    bridge.configureMtcAuthority(9, 1, 1);
    bridge.configureMtcAuthority(0, 0, 0);
    (void) render(bridge, output);
    bridge.setPlaying(true);
    require(render(bridge, output),
            "the graph should render after an external-authority round trip");
    require(bridge.transportFramePosition() != 364,
            "an internal seek queued before an external-authority epoch must remain revoked after returning to Internal");
}

void testStopInvalidatesPreResetRealtimeIngress()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makeScheduledEventDocument(),
        registry());
    require(bridge.compileAndStage(liveEventSettings()).ok(),
            "Stop ingress fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output), "Stop ingress fixture should activate");
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value(), "Stop ingress fixture should publish Gain");
    require(bridge.enqueueEventValue(
                *gainId,
                0.75,
                talas::expr::ValueDomain::Scalar,
                0),
            "pre-Stop ingress should enqueue");

    bridge.stopTransport();
    require(!render(bridge, output), "Stop callback should reset while silent");
    bridge.setPlaying(true);
    require(render(bridge, output), "reset engine should resume");
    require(std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 0.5F) < 1.0e-6F;
            }),
            "pre-Stop ingress must not cross the reset discontinuity");
}

void testBridgeStagesPersistedTimelineAtExactAbsoluteFramesWithoutAllocation()
{
    auto document = makeScheduledEventDocument();
    document.timeline.eventValues = {
        {0.5, "Gain", talas::expr::ValueDomain::Scalar, 0.25},
    };
    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry());
    require(bridge.compileAndStage(settings()).ok(), "persisted Timeline should compile and stage");
    bridge.setPlaying(true);

    std::array<float, 4> output{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, output, allocations), "persisted Timeline graph should render");
    require(allocations == 0, "persisted Timeline callback must not allocate");
    require(std::abs(output[0]) < 1.0e-6F && std::abs(output[1]) < 1.0e-6F,
            "Timeline Event Value should retain its zero default before the exact frame");
    require(std::abs(output[2] - 0.25F) < 1.0e-6F && std::abs(output[3] - 0.25F) < 1.0e-6F,
            "Timeline Event Value should activate at its exact absolute frame");

    require(bridge.compileAndStage(settings()).ok(),
            "replacement Timeline graph should stage after first activation");
    require(renderAndCount(bridge, output, allocations), "replacement Timeline graph should render");
    require(allocations == 0, "replacement Timeline callback must not allocate");
    require(std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 0.25F) < 1.0e-6F;
            }),
            "replacement graph should reconstruct Timeline state at the shared host frame");

    bridge.stopTransport();
    require(!render(bridge, output), "stopped Timeline should adopt reset while silent");
    bridge.setPlaying(true);
    require(render(bridge, output), "same engine should resume from reset Timeline frame zero");
    require(std::abs(output[0]) < 1.0e-6F && std::abs(output[1]) < 1.0e-6F
                && std::abs(output[2] - 0.25F) < 1.0e-6F
                && std::abs(output[3] - 0.25F) < 1.0e-6F,
            "transport reset must rewind the active engine Timeline clock");
}

void testGeneratedTimelineUsesExistingPreparedTypedEventScheduler()
{
    auto document = makeScheduledEventDocument();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.domain = talas::expr::ValueDomain::Scalar;
    gain.value = 0.0;
    document.virtualControls.push_back(gain);

    talas::project::ProjectGeneratorProgram program;
    program.generatorId = "runtimeProof";
    program.displayName = "Runtime proof";
    program.source.terminals = {"note"};
    program.source.grammar = "start: note";
    program.source.seed = 1;
    program.source.maximumRewritePasses = 1;
    program.source.maximumOutputSymbols = 1;
    program.output.timelineStepMicroseconds = 500000;
    program.output.bindings.push_back({
        "note",
        {talas::project::ProjectGeneratorTimelineEventAction{
            "Gain",
            talas::expr::ValueDomain::Scalar,
            0.25,
            talas::core::AutomationInterpolation::Step,
        }},
    });
    document.generatorLibrary.programs.push_back(program);

    const auto prepared = talas::project::prepareProjectGenerators(
        document.generatorLibrary);
    require(prepared.ok() && prepared.prepared.programs.size() == 1,
        "generated Timeline runtime proof should prepare one exact program");
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> current{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> next{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> output{};
    talas::generative::ContextFreeGeneratorScratch scratch{
        current, next, output};
    auto generated = talas::project::generateProjectTimelineEvents(
        prepared.prepared.programs.front().program,
        prepared.prepared.programs.front().output,
        scratch,
        {500000});
    require(generated.ok() && generated.events.size() == 1,
        "generated Timeline runtime proof should plan one bounded event");

    talas::project::ProjectEditor editor(document, registry());
    talas::project::ProjectGeneratorEditAuthority authority;
    authority.documentGeneration = editor.documentAuthorityGeneration();
    authority.canonicalDocumentBytes =
        talas::project::serializeProjectDocument(editor.document());
    require(editor.applyGeneratedTimelineEvents(
                std::move(generated.events), authority).ok(),
        "generated Timeline runtime proof should commit one exact batch");

    talas::runtime::ProjectRuntimeBridge bridge(editor.document(), registry());
    require(bridge.compileAndStage(settings()).ok(),
        "generated Timeline runtime proof should stage through the ordinary project bridge");
    bridge.setPlaying(true);
    std::array<float, 4> rendered{};
    std::size_t allocations = 0;
    require(renderAndCount(bridge, rendered, allocations),
        "generated Timeline runtime proof should render");
    require(allocations == 0
            && std::abs(rendered[0]) < 1.0e-6F
            && std::abs(rendered[1]) < 1.0e-6F
            && std::abs(rendered[2] - 0.25F) < 1.0e-6F
            && std::abs(rendered[3] - 0.25F) < 1.0e-6F,
        "generated Timeline event should execute sample-exactly through the existing prepared typed-event scheduler");
}

void testPersistedTimelineFollowsVariableCallbacksPauseAndPreparedCapacity()
{
    auto document = makeScheduledEventDocument();
    document.timeline.eventValues = {
        {0.75, "Gain", talas::expr::ValueDomain::Scalar, 0.5},
    };
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(settings()).ok(), "variable-block Timeline should stage");
    bridge.setPlaying(true);

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(channels.data(), channels.size(), output.size()),
            "first short Timeline callback should render");
    require(std::ranges::all_of(output, [](float sample) { return std::abs(sample) < 1.0e-6F; }),
            "Timeline event should remain pending after a short callback");

    bridge.setPlaying(false);
    require(!bridge.render(channels.data(), channels.size(), output.size()),
            "paused Timeline callback should remain silent");
    bridge.setPlaying(true);
    require(bridge.render(channels.data(), channels.size(), output.size()),
            "resumed Timeline callback should render");
    require(std::abs(output[0]) < 1.0e-6F && std::abs(output[1] - 0.5F) < 1.0e-6F,
            "pause must not move the exact Timeline event frame");

    auto saturatedSettings = settings();
    saturatedSettings.maxRealtimeEventTransactions = 1;
    document.timeline.eventValues.push_back(
        {1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25});
    talas::runtime::ProjectRuntimeBridge saturated(std::move(document), registry());
    const auto rejected = saturated.compileAndStage(std::move(saturatedSettings));
    require(rejected.outcome == talas::runtime::ProjectStageOutcome::EngineRejected,
            "Timeline larger than prepared transaction capacity should reject before playback");
}

void testFutureTimelineCoexistsWithIndependentLiveControl()
{
    auto document = makeScheduledEventDocument();
    document.graph.nodes.back().parameters.front().expression = "!Gain * !Cutoff";
    document.timeline.eventValues = {
        {0.5, "Gain", talas::expr::ValueDomain::Scalar, 0.5},
    };
    auto renderSettings = settings();
    renderSettings.eventValues = {
        {"Gain", 1.0, talas::expr::ValueDomain::Scalar},
        {"Cutoff", 1.0, talas::expr::ValueDomain::Scalar},
    };
    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry());
    require(bridge.compileAndStage(std::move(renderSettings)).ok(),
            "Timeline/live-control coexistence graph should stage");
    bridge.setPlaying(true);
    const auto cutoffId = bridge.realtimeEventValueId("Cutoff");
    require(cutoffId.has_value(), "coexistence graph should publish Cutoff");
    require(bridge.enqueueEventValue(
                *cutoffId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                1),
            "future Timeline must not block an independent live control");

    std::array<float, 4> output{};
    require(render(bridge, output), "Timeline/live-control coexistence graph should render");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F,
            "coexistence graph should preserve initial frame");
    require(std::abs(output[1] - 0.25F) < 1.0e-6F,
            "independent live control should activate before the future Timeline event");
    require(std::abs(output[2] - 0.125F) < 1.0e-6F
                && std::abs(output[3] - 0.125F) < 1.0e-6F,
            "Timeline activation should retain the independent live control value");
    require(bridge.rejectedRealtimeControlCount() == 0,
            "valid live control must not be rejected by a future Timeline event");

    require(bridge.enqueueEventValue(
                *cutoffId,
                0.5,
                talas::expr::ValueDomain::Scalar,
                0),
            "live control should remain available after Timeline activation");
    require(render(bridge, output), "post-Timeline live control callback should render");
    require(std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 0.25F) < 1.0e-6F;
            }),
            "post-Timeline live control must retain the active Timeline value");

    auto laterControlDocument = makeScheduledEventDocument();
    laterControlDocument.graph.nodes.back().parameters.front().expression =
        "!Gain * !Cutoff";
    laterControlDocument.timeline.eventValues = {
        {0.5, "Gain", talas::expr::ValueDomain::Scalar, 0.5},
    };
    auto laterControlSettings = settings();
    laterControlSettings.eventValues = {
        {"Gain", 1.0, talas::expr::ValueDomain::Scalar},
        {"Cutoff", 1.0, talas::expr::ValueDomain::Scalar},
    };
    talas::runtime::ProjectRuntimeBridge laterControl(
        std::move(laterControlDocument),
        registry());
    require(laterControl.compileAndStage(std::move(laterControlSettings)).ok(),
            "Timeline-before-control graph should stage");
    laterControl.setPlaying(true);
    const auto laterCutoffId = laterControl.realtimeEventValueId("Cutoff");
    require(laterCutoffId.has_value(), "Timeline-before-control graph should publish Cutoff");
    require(laterControl.enqueueEventValue(
                *laterCutoffId,
                0.25,
                talas::expr::ValueDomain::Scalar,
                3),
            "control after Timeline in the same callback should enqueue");
    require(render(laterControl, output), "Timeline-before-control graph should render");
    require(std::abs(output[0] - 1.0F) < 1.0e-6F
                && std::abs(output[1] - 1.0F) < 1.0e-6F
                && std::abs(output[2] - 0.5F) < 1.0e-6F
                && std::abs(output[3] - 0.125F) < 1.0e-6F,
            "later control must merge with, not undo, the earlier Timeline value");
}

void testMidiClockAuthorityControlsTransportAtCallbackBoundaries()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
            "MIDI Clock transport graph should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 1.0, channels.data(), channels.size(), output.size());

    bridge.configureMidiClockAuthority(7, 2, 11);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                2,
                11,
                7,
                1.1),
            "exact MIDI Clock Start should enqueue");
    require(bridge.render(
                {}, 1.2, channels.data(), channels.size(), output.size()),
            "MIDI Clock Start should play at the next callback boundary");
    require(bridge.transportFramePosition() == output.size(),
            "MIDI Clock Start should reset then advance from zero");
    const auto beforeRejectedLocalStop = bridge.transportFramePosition();
    bridge.setPlaying(false);
    bridge.stopTransport();
    require(bridge.render(
                {}, 1.22, channels.data(), channels.size(), output.size()),
            "local transport writers must not stop an external Clock authority");
    require(
        bridge.transportFramePosition()
            == beforeRejectedLocalStop + output.size(),
        "external MIDI Clock must remain the sole transport writer");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStop,
                2,
                11,
                7,
                1.25),
            "exact MIDI Clock Stop should enqueue");
    (void)bridge.render(
        {}, 1.3, channels.data(), channels.size(), output.size());
    const auto held = bridge.transportFramePosition();
    (void)bridge.render(
        {}, 1.35, channels.data(), channels.size(), output.size());
    require(bridge.transportFramePosition() == held,
            "MIDI Clock Stop should preserve the held position");
    bridge.setPlaying(true);
    (void)bridge.render(
        {}, 1.37, channels.data(), channels.size(), output.size());
    require(bridge.transportFramePosition() == held,
            "local playback requests must not bypass an external Clock Stop");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                2,
                11,
                7,
                1.39),
        "fresh Clock evidence should precede the transport resume fixture");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                2,
                11,
                7,
                1.4),
            "exact MIDI Clock Continue should enqueue");
    require(bridge.render(
                {}, 1.45, channels.data(), channels.size(), output.size()),
            "MIDI Clock Continue should resume at the next callback");
    require(bridge.transportFramePosition() == held + output.size(),
            "MIDI Clock Continue should resume without resetting");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                2,
                11,
                7,
                1.5),
            "second MIDI Clock Start should enqueue");
    require(bridge.render(
                {}, 1.55, channels.data(), channels.size(), output.size()),
            "second MIDI Clock Start should render");
    require(bridge.transportFramePosition() == output.size(),
            "second MIDI Clock Start should explicitly reset to zero");

    require(!bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                3,
                11,
                7,
                1.6),
            "a different physical source should not inherit Clock authority");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStop,
                2,
                11,
                7,
                1.58),
            "ordered Stop should enqueue before Song Position");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                2,
                11,
                7,
                1.6,
                1),
            "Song Position Pointer should enqueue from the exact authority");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                2,
                11,
                7,
                1.61),
            "ordered Continue should enqueue after Song Position");
    require(bridge.render(
                {}, 1.62, channels.data(), channels.size(), output.size()),
            "Song Position Pointer should relocate before rendering");
    require(bridge.transportFramePosition() == 1 + output.size(),
            "one MIDI Song Position unit should map to one exact 120-BPM sixteenth note");

    bridge.configureMidiClockAuthority(8, 4, 12);
    const auto replacing = bridge.midiClockTelemetry();
    require(
        replacing.authorityGeneration == 8
            && replacing.externalAuthority
            && !replacing.running,
        "replacement telemetry must not project the prior authority's running state");
    require(!bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                2,
                11,
                7,
                1.65),
            "the previous source generation should be revoked");
    (void)bridge.render(
        {}, 1.7, channels.data(), channels.size(), output.size());
    const auto adoptedReplacement = bridge.midiClockTelemetry();
    require(
        adoptedReplacement.pulseSequence == 0
            && adoptedReplacement.lastTickTimeSeconds == 0.0
            && adoptedReplacement.lockState
                == talas::runtime::MidiClockLockState::Waiting,
        "authority replacement must discard the previous source's pulse identity evidence");
    const auto revokedPosition = bridge.transportFramePosition();
    (void)bridge.render(
        {}, 1.75, channels.data(), channels.size(), output.size());
    require(bridge.transportFramePosition() == revokedPosition,
            "authority replacement should stop and hold");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                4,
                12,
                8,
                10.0),
            "replacement authority Start should enqueue");
    require(bridge.render(
                {}, 10.1, channels.data(), channels.size(), output.size()),
            "replacement authority should start");
    const auto beforeTimeout = bridge.transportFramePosition();
    (void)bridge.render(
        {}, 10.7, channels.data(), channels.size(), output.size());
    require(bridge.transportFramePosition() == beforeTimeout,
            "missing MIDI Clock signal should stop before the timed-out callback advances");

    const auto telemetry = bridge.midiClockTelemetry();
    require(telemetry.authorityGeneration == 8
                && telemetry.externalAuthority
                && !telemetry.running
                && telemetry.acceptedStarts == 3
                && telemetry.acceptedContinues == 2
                && telemetry.acceptedSongPositions == 1
                && telemetry.lastSongPosition == 1
                && telemetry.lastRelocationFrame == 1
                && telemetry.acceptedStops == 2
                && telemetry.wrongSource >= 1
                && telemetry.staleAuthority >= 1
                && telemetry.unsupportedSongPosition == 0
                && telemetry.songPositionConversionFailures == 0
                && telemetry.signalTimeouts == 1
                && telemetry.signalStale,
            "MIDI Clock telemetry should expose exact accepted and rejected lifecycle state: starts="
                + std::to_string(telemetry.acceptedStarts)
                + " continues=" + std::to_string(telemetry.acceptedContinues)
                + " stops=" + std::to_string(telemetry.acceptedStops)
                + " spp=" + std::to_string(telemetry.acceptedSongPositions)
                + " last=" + std::to_string(telemetry.lastSongPosition)
                + " frame=" + std::to_string(telemetry.lastRelocationFrame)
                + " wrong=" + std::to_string(telemetry.wrongSource)
                + " stale=" + std::to_string(telemetry.staleAuthority)
                + " unsupported=" + std::to_string(telemetry.unsupportedSongPosition)
                + " conversion=" + std::to_string(telemetry.songPositionConversionFailures)
                + " timeouts=" + std::to_string(telemetry.signalTimeouts)
                + " staleSignal=" + std::to_string(telemetry.signalStale));
    require(
        bridge.midiClockTelemetry().signalStale,
        "timed-out MIDI Clock state should remain visible until a valid signal or authority change");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                4,
                12,
                8,
                10.75),
            "a fresh exact Clock tick should enqueue after timeout");
    (void)bridge.render(
        {}, 10.8, channels.data(), channels.size(), output.size());
    require(!bridge.midiClockTelemetry().signalStale,
            "a fresh exact Clock signal should clear persistent stale state");

    bridge.configureMidiClockAuthority(8, 5, 13);
    const auto reusedGeneration = bridge.midiClockTelemetry();
    require(!reusedGeneration.externalAuthority
                && reusedGeneration.invalidRecords
                    > telemetry.invalidRecords,
            "reusing one authority generation for a different exact source should fail closed");

    bridge.configureMidiClockAuthority(9, 6, 14);
    const auto sourceLossesBefore =
        bridge.midiClockTelemetry().sourceLosses;
    bridge.revokeMidiClockAuthorityForSourceLoss();
    const auto sourceLost = bridge.midiClockTelemetry();
    require(
        !sourceLost.externalAuthority
            && sourceLost.sourceLosses == sourceLossesBefore + 1,
        "physical Clock source loss should revoke authority and remain monotonic telemetry");
    const auto beforeImmediateInternalPlay =
        bridge.transportFramePosition();
    bridge.setPlaying(true);
    require(bridge.render(
                {},
                10.9,
                channels.data(),
                channels.size(),
                output.size()),
            "an immediate Internal play request should survive pending Clock revocation adoption");
    require(
        bridge.transportFramePosition()
            == beforeImmediateInternalPlay + output.size(),
        "the callback must apply the newer Internal play request after adopting the older Clock stop boundary");
}

// With a time code offset of a second, time code
// 00:00:02:00 is a second into the Timeline, and code before the offset
// holds it at its start.
void testMtcFollowsTheTimeCodeOffset()
{
    auto document = makePlayableDocument(4.0);
    document.timeline.timeCodeOffsetSeconds = 1.0;
    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry());
    auto renderSettings = settings();
    renderSettings.sampleRate = 24.0;
    renderSettings.maxBlockFrames = 2;
    require(bridge.compileAndStage(renderSettings).ok(), "an offset Timeline should stage");
    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render({}, 1.0, channels.data(), channels.size(), output.size());
    bridge.configureMtcAuthority(41, 2, 11);
    const std::array<std::uint8_t, 10> locate{0xF0, 0x7F, 0x7F, 0x01, 0x01, 0, 0, 2, 0, 0xF7};
    require(bridge.enqueueMtc(locate, 2, 11, 41, 1.01), "a full-frame locate should enqueue");
    (void)bridge.render({}, 1.02, channels.data(), channels.size(), output.size());
    require(bridge.mtcTelemetry().mappedProjectFrame == 24,
        "time code at the offset plus a second is a second into the Timeline");
    const std::array<std::uint8_t, 10> early{0xF0, 0x7F, 0x7F, 0x01, 0x01, 0, 0, 0, 12, 0xF7};
    require(bridge.enqueueMtc(early, 2, 11, 41, 1.03), "an earlier locate should enqueue");
    (void)bridge.render({}, 1.04, channels.data(), channels.size(), output.size());
    require(bridge.mtcTelemetry().mappedProjectFrame == 0, "time code before the offset holds the Timeline at its start");
}

void testMtcAuthorityLocatesAcquiresAdvancesAndTimesOut()
{
    auto document = makePlayableDocument(2.0);
    document.timeline.loop = {true, 0.0, 1.0};
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document), registry());
    auto renderSettings = settings();
    renderSettings.sampleRate = 24.0;
    renderSettings.maxBlockFrames = 2;
    require(bridge.compileAndStage(renderSettings).ok(),
        "MTC transport graph should stage at one sample per 24 fps frame");
    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 1.0, channels.data(), channels.size(), output.size());

    bridge.configureMtcAuthority(31, 2, 11);
    const std::array<std::uint8_t, 10> locate{
        0xF0, 0x7F, 0x7F, 0x01, 0x01, 0, 0, 1, 0, 0xF7};
    require(bridge.enqueueMtc(locate, 2, 11, 31, 1.01),
        "exact full-frame MTC should enqueue");
    (void)bridge.render(
        {}, 1.02, channels.data(), channels.size(), output.size());
    auto telemetry = bridge.mtcTelemetry();
    require(telemetry.externalAuthority && !telemetry.running
            && telemetry.follower.state
                == talas::runtime::MtcTransportLockState::Located
            && telemetry.mappedProjectFrame == 24
            && bridge.transportFramePosition() == 0,
        "full-frame MTC should relocate once and remain held");

    const auto enqueuePieces = [&](std::uint8_t rawFrame,
                                   double startTime,
                                   std::uint8_t firstType,
                                   std::uint8_t endType) {
        const std::array<std::uint8_t, 8> values{
            static_cast<std::uint8_t>(rawFrame & 0x0F),
            static_cast<std::uint8_t>((rawFrame >> 4) & 0x01),
            1, 0, 0, 0, 0, 0};
        for (auto type = firstType; type < endType; ++type)
        {
            const std::array<std::uint8_t, 2> message{
                0xF1,
                static_cast<std::uint8_t>((type << 4) | values[type])};
            require(bridge.enqueueMtc(
                        message, 2, 11, 31,
                        startTime
                            + static_cast<double>(type - firstType) * 0.001),
                "one exact forward quarter-frame piece should enqueue");
        }
    };
    const auto enqueueSet = [&](std::uint8_t rawFrame, double startTime) {
        enqueuePieces(rawFrame, startTime, 0, 8);
    };

    enqueueSet(0, 1.03);
    enqueuePieces(2, 1.039, 0, 1);
    require(bridge.render(
                {}, 1.04, channels.data(), channels.size(), output.size()),
        "a complete set followed by one partial piece should still resume after held locate");
    telemetry = bridge.mtcTelemetry();
    require(telemetry.running
            && telemetry.follower.state
                == talas::runtime::MtcTransportLockState::Locked
            && telemetry.follower.nextExpectedPiece == 1
            && telemetry.follower.timeCode.frame == 2
            && telemetry.mappedProjectFrame == 26
            && bridge.transportFramePosition() == 4,
        "fresh coherent MTC should retain its completion action when a later partial piece is drained");

    const auto beforeLocalWriter = bridge.transportFramePosition();
    bridge.setPlaying(false);
    bridge.stopTransport();
    enqueuePieces(2, 1.05, 1, 8);
    require(bridge.render(
                {}, 1.06, channels.data(), channels.size(), output.size()),
        "continuous MTC should advance without a hard seek");
    telemetry = bridge.mtcTelemetry();
    require(bridge.transportFramePosition() == beforeLocalWriter + 2
            && telemetry.running
            && telemetry.follower.acceptedCompleteSets == 2
            && telemetry.follower.discontinuities == 1,
        "continuous exact MTC should compare against the absolute cursor across a Timeline loop");

    const auto beforeRejectedEvidence = bridge.transportFramePosition();
    const std::array<std::uint8_t, 1> malformed{0xF1};
    require(!bridge.enqueueMtc(malformed, 2, 11, 31, 1.07),
        "malformed MTC should reject at producer admission");
    (void)bridge.render(
        {}, 1.08, channels.data(), channels.size(), output.size());
    telemetry = bridge.mtcTelemetry();
    require(!telemetry.running
            && telemetry.follower.state
                == talas::runtime::MtcTransportLockState::Acquiring
            && telemetry.follower.discontinuityReason
                == talas::runtime::MtcDiscontinuityReason::EvidenceRejected
            && telemetry.mappedProjectFrame == 28
            && telemetry.follower.discontinuities == 2
            && bridge.transportFramePosition() == beforeRejectedEvidence,
        "producer-side malformed evidence should immediately stop and hold transport");
    require(!bridge.enqueueMtc(malformed, 2, 11, 31, 1.085),
        "repeated malformed MTC should remain rejected while held");
    (void)bridge.render(
        {}, 1.086, channels.data(), channels.size(), output.size());
    telemetry = bridge.mtcTelemetry();
    require(telemetry.mappedProjectFrame == 28
            && telemetry.follower.discontinuities == 2,
        "repeated rejection should preserve the last safe mapping without recounting the same held incident");

    enqueueSet(4, 1.09);
    require(bridge.render(
                {}, 1.10, channels.data(), channels.size(), output.size()),
        "one fresh complete set should reacquire after producer-side rejection");
    require(bridge.mtcTelemetry().running,
        "fresh complete MTC should be required before transport resumes");

    enqueueSet(6, 1.11);
    enqueueSet(8, 1.12);
    require(!bridge.render(
                {}, 1.13, channels.data(), channels.size(), output.size()),
        "a multi-set backlog should hold while performing one bounded catch-up relocation");
    telemetry = bridge.mtcTelemetry();
    require(!telemetry.running
            && telemetry.mappedProjectFrame == 34
            && telemetry.follower.discontinuityReason
                == talas::runtime::MtcDiscontinuityReason::ForwardJump,
        "multiple advanced sets in one drain must not leave the DSP cursor behind the final accepted position");

    enqueueSet(10, 1.14);
    require(bridge.render(
                {}, 1.15, channels.data(), channels.size(), output.size()),
        "one fresh complete set should reacquire after backlog catch-up");

    const auto beforeReorderedEvidence = bridge.transportFramePosition();
    const std::array<std::uint8_t, 2> reordered{0xF1, 0x10};
    require(bridge.enqueueMtc(reordered, 2, 11, 31, 1.16),
        "one reordered exact MTC piece should enqueue");
    (void)bridge.render(
        {}, 1.17, channels.data(), channels.size(), output.size());
    telemetry = bridge.mtcTelemetry();
    require(!telemetry.running
            && telemetry.follower.state
                == talas::runtime::MtcTransportLockState::Acquiring
            && telemetry.follower.discontinuityReason
                == talas::runtime::MtcDiscontinuityReason::EvidenceRejected
            && bridge.transportFramePosition() == beforeReorderedEvidence,
        "reordered MTC evidence should immediately stop and hold the current position");

    (void)bridge.render(
        {}, 1.70, channels.data(), channels.size(), output.size());
    const auto held = bridge.transportFramePosition();
    telemetry = bridge.mtcTelemetry();
    require(!telemetry.running
            && telemetry.follower.state
                == talas::runtime::MtcTransportLockState::Stale
            && telemetry.follower.staleTimeouts == 1,
        "complete-set-bound timeout should stop and remain stale");
    const std::array<std::uint8_t, 2> partial{0xF1, 0x00};
    require(bridge.enqueueMtc(partial, 2, 11, 31, 1.71),
        "one partial set may enqueue after timeout");
    (void)bridge.render(
        {}, 1.80, channels.data(), channels.size(), output.size());
    require(bridge.transportFramePosition() == held
            && !bridge.mtcTelemetry().running,
        "one partial set must neither resume nor extend stale MTC");

    bridge.revokeMtcAuthorityForSourceLoss();
    (void)bridge.render(
        {}, 1.90, channels.data(), channels.size(), output.size());
    telemetry = bridge.mtcTelemetry();
    require(!telemetry.externalAuthority && telemetry.sourceLosses == 1,
        "MTC source loss should revoke authority at the callback boundary");
}

void testMtcMessagesUpdateTimeEventSources()
{
    for (const bool realTime : {false, true})
    {
        auto document = makeScheduledEventDocument();
        document.graph.nodes.back().parameters.front().expression = realTime
            ? "!RealTime" : "!MIDITimeCode";
        talas::runtime::ProjectRuntimeBridge bridge(
            std::move(document), registry());
        bridge.setPlaying(true);
        auto renderSettings = settings();
        renderSettings.sampleRate = 24.0;
        renderSettings.maxBlockFrames = 2;
        const auto staged = bridge.compileAndStage(renderSettings);
        require(staged.ok(), "MTC time Event Source must stage: "
            + staged.message);
        std::array<float, 2> output{};
        std::array<float*, 1> channels{output.data()};
        require(bridge.render(
                {}, 1.0, channels.data(), channels.size(), output.size()),
            "MTC time Event Source baseline must render");
        bridge.configureMtcAuthority(31, 2, 11);
        const std::array<std::uint8_t, 10> locate{
            0xF0, 0x7F, 0x7F, 0x01, 0x01, 0, 0, 1, 0, 0xF7};
        require(bridge.enqueueMtc(locate, 2, 11, 31, 1.01),
            "full-frame MTC position must enter the Event Source stage");
        (void)bridge.render(
            {}, 1.02, channels.data(), channels.size(), output.size());
        require(bridge.mtcTelemetry().follower.hasTimeCode,
            "full-frame MTC must publish a decoded position while held");
        const auto enqueueSet = [&](std::uint8_t rawFrame,
                                    double startTime) {
            const std::array<std::uint8_t, 8> values{
                static_cast<std::uint8_t>(rawFrame & 0x0F),
                static_cast<std::uint8_t>((rawFrame >> 4) & 0x01),
                1, 0, 0, 0, 0, 0};
            for (std::uint8_t type = 0; type < 8; ++type)
            {
                const std::array<std::uint8_t, 2> message{
                    0xF1,
                    static_cast<std::uint8_t>((type << 4) | values[type])};
                require(bridge.enqueueMtc(
                        message, 2, 11, 31,
                        startTime + static_cast<double>(type) * 0.001),
                    "coherent MTC quarter-frame piece must enqueue");
            }
        };
        enqueueSet(0, 1.03);
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto firstRendered = bridge.render(
            {}, 1.04, channels.data(), channels.size(), output.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(firstRendered && allocationCount.load(
                std::memory_order_relaxed) == 0,
            "MTC acquisition and Event Source update must not allocate");
        const auto firstExpected = realTime ? 0.0 : 1.0 + 2.0 / 24.0;
        require(std::abs(output[0] - firstExpected) < 1.0e-5F,
            "MTC acquisition must publish its received position or elapsed zero: "
                + std::to_string(output[0]));
        enqueueSet(2, 1.12);
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto secondRendered = bridge.render(
            {}, 1.13, channels.data(), channels.size(), output.size());
        countAllocations.store(false, std::memory_order_relaxed);
        require(secondRendered && allocationCount.load(
                std::memory_order_relaxed) == 0,
            "second complete MTC position must render without allocation");
        const auto secondExpected = realTime ? 0.09 : 1.0 + 4.0 / 24.0;
        const auto secondMtc = bridge.mtcTelemetry();
        const auto secondControl = bridge.realtimeControlTelemetry();
        require(std::abs(output[0] - firstExpected) < 1.0e-5F
                && std::abs(output[1] - secondExpected) < 1.0e-5F,
            "MTC second message must update its Event Source: "
                + std::to_string(output[0]) + ", "
                + std::to_string(output[1]) + ", sets "
                + std::to_string(secondMtc.follower.acceptedCompleteSets)
                + ", frame "
                + std::to_string(secondMtc.follower.timeCode.frame)
                + ", applied "
                + std::to_string(secondControl.preparedTypedEvents)
                + ", stale "
                + std::to_string(secondControl.staleGenerationOrStage)
                + ", rejected "
                + std::to_string(secondControl.aggregateRejected));
        const std::array<std::uint8_t, 2> partial{0xF1, 0x04};
        require(bridge.enqueueMtc(partial, 2, 11, 31, 1.215)
                && bridge.render(
                    {}, 1.22, channels.data(), channels.size(),
                    output.size())
                && std::abs(output[0] - secondExpected) < 1.0e-5F,
            "partial MTC evidence must leave the last complete Event Source value unchanged");
    }
}

// The level of a Sound whose level is `level`, staged at 1 kHz and played
// live in 4 ms callbacks for `seconds`, while a MIDI input sends these
// timing clock messages (their kinds and the seconds into the play they
// arrive at), with no clock authority: a frame a millisecond, from the
// Sound's start. A message at m seconds lands near the frame 1000 m + 4.
std::vector<double> liveTimingClockLevel(
    std::string level,
    const std::vector<std::pair<talas::runtime::RealtimeControlCommandKind, double>>& messages,
    double seconds,
    std::size_t* counted = nullptr)
{
    auto document = makeMidiGateDocument(level);
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry());
    bridge.setPlaying(true);
    auto renderSettings = settings();
    renderSettings.sampleRate = 1000.0;
    renderSettings.maxBlockFrames = 4;
    const auto staged = bridge.compileAndStage(renderSettings);
    require(staged.ok(), level + " should stage: " + staged.message);
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    constexpr double start = 10.0;
    require(bridge.render({}, start, channels.data(), channels.size(), output.size()),
        level + " should render before the clock");
    std::vector<double> played(output.begin(), output.end());
    std::size_t next = 0;
    std::size_t accepted = 0;
    const auto callbacks = static_cast<std::size_t>(std::llround(seconds * 250.0));
    for (std::size_t callback = 1; callback <= callbacks; ++callback)
    {
        const auto end = static_cast<double>(callback) * 0.004;
        while (next < messages.size() && messages[next].second < end)
        {
            accepted += bridge.enqueueMidiTimingClock(messages[next].first, start + messages[next].second) ? 1 : 0;
            ++next;
        }
        require(bridge.render({}, start + end, channels.data(), channels.size(), output.size()),
            level + " should render with the clock");
        played.insert(played.end(), output.begin(), output.end());
    }
    if (counted != nullptr)
        *counted = accepted;
    for (const auto value : played)
        require(std::abs(value) < 1.0e-6 || std::abs(value - 1.0) < 1.0e-6,
            level + " should be a trigger of 0 and 1, not " + std::to_string(value));
    return played;
}

// The rising edges of a trigger from the frame `from` to before `to`.
int risesBetween(const std::vector<double>& level, std::size_t from, std::size_t to)
{
    auto rises = 0;
    for (std::size_t frame = from; frame < to && frame < level.size(); ++frame)
        rises += level[frame] > 0.5 && (frame == 0 || level[frame - 1] <= 0.5) ? 1 : 0;
    return rises;
}

// 24 timing clock messages a second, as a device at 60 bpm sends, from
// `from` seconds into the play.
void appendLiveClockSecond(
    std::vector<std::pair<talas::runtime::RealtimeControlCommandKind, double>>& messages,
    double from,
    std::size_t count = 24)
{
    for (std::size_t message = 0; message < count; ++message)
        messages.emplace_back(talas::runtime::RealtimeControlCommandKind::MidiClockTick,
            from + static_cast<double>(message) / 24.0 + 0.0005);
}

// `MIDITimingClock is a trigger sequence an
// external MIDI device or software sends, once for each MIDI timing
// message, 24 a second from a device at 60 bpm: here from a live MIDI
// input, whatever transport the project follows; a Stop holds it until a
// Start.
void testLiveMidiTimingClockTriggersOncePerMessage()
{
    using Kind = talas::runtime::RealtimeControlCommandKind;
    std::vector<std::pair<Kind, double>> messages;
    appendLiveClockSecond(messages, 0.0);
    messages.emplace_back(Kind::MidiClockStop, 1.0);
    appendLiveClockSecond(messages, 1.0, 12);
    messages.emplace_back(Kind::MidiClockStart, 1.5);
    appendLiveClockSecond(messages, 1.5, 12);
    std::size_t counted = 0;
    const auto level = liveTimingClockLevel("`MIDITimingClock", messages, 2.0, &counted);
    require(counted == 36, "the 36 running clock messages should count, not " + std::to_string(counted));
    const auto first = risesBetween(level, 0, 1000);
    require(first == 24, "24 timing clock messages in a second should give 24 triggers, not " + std::to_string(first));
    const auto stopped = risesBetween(level, 1000, 1500);
    const auto started = risesBetween(level, 1500, 2004);
    require(stopped == 0 && started == 12, "a Stop should hold the clock until a Start: "
            + std::to_string(stopped) + ", " + std::to_string(started));
    const auto silent = liveTimingClockLevel("`MIDITimingClock", {}, 1.0);
    require(risesBetween(silent, 0, silent.size()) == 0, "`MIDITimingClock should not trigger without clock messages");
    // A Sound that reads no clock takes no message.
    talas::runtime::ProjectRuntimeBridge plain(makeMidiGateDocument("1"), registry());
    require(plain.compileAndStage(settings()).ok()
            && !plain.enqueueMidiTimingClock(Kind::MidiClockTick, 1.0),
        "a Sound without the clock should not take its messages");
}

// !TimingClock, the global map's `MIDITimingClock
// extractClock, triggers as the live MIDI timing clock does, 24 a second,
// once it comes; before, it stands in at the Sound's tempo (48 a second
// at 120 bpm).
void testLiveMidiTimingClockDrivesTimingClock()
{
    std::vector<std::pair<talas::runtime::RealtimeControlCommandKind, double>> messages;
    // From 1.26 s, landing from the frame 1264, a stand-in's beat having
    // risen at 1250 and fallen by 1261.
    appendLiveClockSecond(messages, 1.26);
    const auto clocked = liveTimingClockLevel("!TimingClock", messages, 2.3);
    const auto standIn = risesBetween(clocked, 0, 1000);
    require(standIn == 48, "!TimingClock should stand in at 120 bpm before the clock, 48 a second, not "
            + std::to_string(standIn));
    const auto followed = risesBetween(clocked, 1260, 2260);
    require(followed == 24, "!TimingClock should trigger once for each of 24 live clock messages in a second, not "
            + std::to_string(followed));
    const auto extracted = liveTimingClockLevel("`MIDITimingClock extractClock", messages, 2.3);
    require(risesBetween(extracted, 0, 1260) == 0 && risesBetween(extracted, 1260, 2260) == 24,
        "`MIDITimingClock extractClock should trigger 24 times in the clock's second and not before");
}

void testMidiSongPositionRejectsQueuedPriorMusicalStage()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
            "stale SPP fixture should stage its first musical map");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 1.0, channels.data(), channels.size(), output.size());

    bridge.configureMidiClockAuthority(17, 3, 29);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                3,
                29,
                17,
                1.1,
                1),
            "SPP should capture the exact current musical stage");
    constexpr double oldStagePulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    3,
                    29,
                    17,
                    1.11 + static_cast<double>(index) * oldStagePulse),
            "Clock pulse should capture the exact current musical stage");
    }
    auto replacementMap =
        bridge.editor().document().timeline.musicalTime;
    replacementMap.tempo.front().microsecondsPerQuarter = 2000000;
    require(bridge.editor().setMusicalTime(replacementMap).ok(),
            "replacement musical map should edit atomically");
    require(bridge.compileAndStage(settings()).ok(),
            "replacement musical map should publish a new stage");
    const auto before = bridge.musicalTransportSnapshot().transport;
    require(!bridge.render(
                {}, 1.2, channels.data(), channels.size(), output.size()),
            "paused replacement should reject prior-stage SPP without audio");
    const auto after = bridge.musicalTransportSnapshot().transport;
    const auto rejected = bridge.midiClockTelemetry();
    require(
        after.absoluteFrame == before.absoluteFrame
            && rejected.acceptedSongPositions == 0
            && rejected.songPositionConversionFailures == 1
            && rejected.acceptedTicks == 0
            && rejected.staleAuthority == 4
            && rejected.lockState
                == talas::runtime::MidiClockLockState::Waiting,
        "queued SPP and phase evidence from the prior map must not enter the replacement graph");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                3,
                29,
                17,
                1.3,
                1),
            "fresh SPP should capture the replacement musical stage");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                3,
                29,
                17,
                1.31),
            "fresh Continue should follow the replacement-map SPP");
    require(bridge.render(
                {}, 1.4, channels.data(), channels.size(), output.size()),
            "fresh replacement-map SPP should render");
    require(
        bridge.transportFramePosition() == 2 + output.size()
            && bridge.midiClockTelemetry().lastRelocationFrame == 2,
        "fresh SPP must use the exact replacement tempo map");
}

void testMaximumMusicalMapAndClockQueueRemainPrepared()
{
    auto document = makePlayableDocument(220.0);
    auto& tempo = document.timeline.musicalTime.tempo;
    tempo.reserve(talas::project::maximumMusicalMapEvents);
    for (std::uint64_t tick = 1;
         tick < talas::project::maximumMusicalMapEvents;
         ++tick)
    {
        tempo.push_back({tick, 500000});
    }
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
            "maximum musical-map fixture should prepare off-callback");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 1.0, channels.data(), channels.size(), output.size());
    bridge.configureMidiClockAuthority(31, 4, 37);
    for (std::uint16_t position = 0; position < 1024; ++position)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                    4,
                    37,
                    31,
                    1.1 + static_cast<double>(position) / 10000.0,
                    position),
                "maximum valid Clock queue should admit every exact SPP record");
    }
    std::size_t allocations = 0;
    require(!renderAndCount(bridge, output, allocations),
            "prepared maximum SPP batch should relocate while paused");
    const auto telemetry = bridge.midiClockTelemetry();
    require(
        allocations == 0
            && telemetry.acceptedSongPositions == 1024
            && telemetry.lastSongPosition == 1023
            && telemetry.songPositionConversionFailures == 0,
        "maximum map and queue must remain allocation-free with coherent final telemetry");

    constexpr double pulseInterval = 60.0 / (120.0 * 24.0);
    for (std::uint16_t pulseIndex = 0; pulseIndex < 1024; ++pulseIndex)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    4,
                    37,
                    31,
                    10.0
                        + static_cast<double>(pulseIndex)
                            * pulseInterval),
            "maximum valid Clock queue should admit every exact pulse");
    }
    allocations = 0;
    require(!renderAndCount(bridge, output, allocations),
        "prepared maximum Clock batch should remain paused");
    require(
        allocations == 0
            && bridge.midiClockTelemetry().acceptedTicks == 1024,
        "maximum phase-follower drain must remain allocation-free and fixed-budget");
}

void testMusicalTransportPresentationUsesActiveExactStage()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
            "musical presentation fixture should stage its first map");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output),
            "musical presentation fixture should render");
    const auto original = bridge.musicalTransportSnapshot();
    const auto originalTick = talas::project::sampleFrameToMusicalTick(
        talas::project::ProjectMusicalTime{}, 4, 4);
    const auto expectedOriginal = originalTick.has_value()
        ? talas::project::musicalTickToPosition(
              talas::project::ProjectMusicalTime{}, *originalTick)
        : std::optional<talas::project::MusicalPosition>{};
    require(
        original.transport.playheadFrame == 4
            && original.musicalPosition == expectedOriginal,
        "presentation should derive exact position from the active default map: frame="
            + std::to_string(original.transport.playheadFrame)
            + " bar="
            + std::to_string(original.musicalPosition.has_value()
                  ? original.musicalPosition->bar : 0)
            + " beat="
            + std::to_string(original.musicalPosition.has_value()
                  ? original.musicalPosition->beat : 0)
            + " tick="
            + std::to_string(original.musicalPosition.has_value()
                  ? original.musicalPosition->tick : 0));

    auto editedMap = bridge.editor().document().timeline.musicalTime;
    editedMap.tempo.front().microsecondsPerQuarter = 2000000;
    require(bridge.editor().setMusicalTime(editedMap).ok(),
            "presentation fixture should edit a replacement map");
    require(
        bridge.musicalTransportSnapshot().musicalPosition
            == original.musicalPosition,
        "unstaged editor map must not reinterpret the active transport");

    require(bridge.compileAndStage(settings()).ok(),
            "presentation fixture should stage the edited map");
    require(render(bridge, output),
            "replacement musical map should adopt at the callback boundary");
    const auto replacement = bridge.musicalTransportSnapshot();
    const auto replacementTick = talas::project::sampleFrameToMusicalTick(
        editedMap, 8, 4);
    const auto expectedReplacement = replacementTick.has_value()
        ? talas::project::musicalTickToPosition(
              editedMap, *replacementTick)
        : std::optional<talas::project::MusicalPosition>{};
    require(
        replacement.transport.playheadFrame == 8
            && replacement.musicalPosition == expectedReplacement
            && replacement.musicalPosition != original.musicalPosition,
        "presentation should switch atomically to the adopted stage map and exact frame");
}

void testFarFutureTempoOverflowDoesNotRejectEarlySongPosition()
{
    auto document = makePlayableDocument(220.0);
    document.timeline.musicalTime.tempo.push_back({
        std::numeric_limits<std::uint64_t>::max(),
        500000,
    });
    talas::runtime::ProjectRuntimeBridge bridge(
        std::move(document),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
            "a valid far-future tempo suffix should not reject the prepared graph");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 1.0, channels.data(), channels.size(), output.size());
    bridge.configureMidiClockAuthority(43, 5, 47);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                5,
                47,
                43,
                1.1,
                1),
            "early SPP should remain representable before the overflowing suffix");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                5,
                47,
                43,
                1.11),
            "early SPP fixture should resume after relocation");
    require(bridge.render(
                {}, 1.2, channels.data(), channels.size(), output.size()),
            "early SPP should render under the valid prefix");
    require(
        bridge.transportFramePosition() == 1 + output.size()
            && bridge.midiClockTelemetry()
                .songPositionConversionFailures == 0,
        "only a target entering the unrepresentable suffix may fail closed");
}

void testMidiClockPublishesLockedExternalMusicalPhase()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48000.0;
    realtimeSettings.maxBlockFrames = 512;
    require(bridge.compileAndStage(realtimeSettings).ok(),
        "external musical-phase fixture should stage");
    std::array<float, 480> output{};
    std::array<float*, 1> channels{output.data()};
    bridge.configureMidiClockAuthority(59, 6, 61);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    6,
                    61,
                    59,
                    1.0 + static_cast<double>(index) * pulse),
            "exact 120 BPM pulse should enqueue");
    }
    require(!bridge.render(
                {}, 1.063, channels.data(), channels.size(), output.size()),
        "clock may acquire while stopped without producing audio");
    const auto acquired = bridge.midiClockTelemetry();
    require(
        acquired.lockState == talas::runtime::MidiClockLockState::Locked
            && acquired.tempoAcquisitions == 1
            && acquired.acceptedTicks == 4
            && acquired.estimatedMicrosecondsPerQuarter == 500000,
        "four exact pulses should publish one coherent 120 BPM lock");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                6,
                61,
                59,
                1.07),
        "Start should anchor external phase at zero");
    require(bridge.render(
                {}, 1.08, channels.data(), channels.size(), output.size()),
        "locked external transport should render");
    const auto running = bridge.musicalTransportSnapshot();
    require(
        running.transport.playing
            && running.publicationValid
            && !running.usedStableFallback
            && running.musicalPosition.has_value()
            && running.musicalPosition->bar == 1
            && running.musicalPosition->beat == 1
            && running.musicalPosition->tick == 19
            && running.externalAuthority
            && running.externalAuthorityGeneration == 59
            && running.externalPulseSequence == 4
            && running.externalMicrosecondsPerQuarter == 500000
            && running.externalLockState
                == talas::runtime::MidiClockLockState::Locked
            && running.externalLastAnchor
                == talas::runtime::MidiClockAnchorKind::Start,
        "the first pending-stage callback should adopt and publish nineteen complete project ticks with exact lock evidence without relocating DSP");
    const auto discontinuity = running.transport.discontinuitySequence;

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStop,
                6,
                61,
                59,
                1.09),
        "Stop should hold external musical phase");
    (void)bridge.render(
        {}, 1.10, channels.data(), channels.size(), output.size());
    const auto held = bridge.musicalTransportSnapshot();
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                6,
                61,
                59,
                1.104),
        "Clock while stopped should remain valid tempo evidence");
    (void)bridge.render(
        {}, 1.11, channels.data(), channels.size(), output.size());
    const auto stoppedTick = bridge.musicalTransportSnapshot();
    require(
        !stoppedTick.transport.playing
            && stoppedTick.musicalPosition == held.musicalPosition
            && stoppedTick.transport.discontinuitySequence == discontinuity,
        "Clock while stopped must not advance phase or invent a discontinuity");

    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockContinue,
                6,
                61,
                59,
                1.12),
        "Continue should request resumption from held phase");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiSongPosition,
                6,
                61,
                59,
                1.95,
                2),
        "late SPP should remain a position directive, not pulse evidence");
    (void)bridge.render(
        {}, 2.0, channels.data(), channels.size(), output.size());
    const auto stale = bridge.midiClockTelemetry();
    require(
        !stale.running
            && stale.signalStale
            && stale.lockState == talas::runtime::MidiClockLockState::Stale
            && stale.lastSongPosition == 2,
        "Continue and SPP must not refresh the pulse timeout; terminal stale should stop and hold the accepted position");
}

void testStoppedMidiClockLockReachesTerminalStale()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    require(bridge.compileAndStage(settings()).ok(),
        "stopped timeout fixture should stage");
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 0.9, channels.data(), channels.size(), output.size());
    bridge.configureMidiClockAuthority(63, 6, 64);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    6,
                    64,
                    63,
                    1.0 + static_cast<double>(index) * pulse),
            "stopped timeout fixture should admit lock pulses");
    }
    (void)bridge.render(
        {}, 1.07, channels.data(), channels.size(), output.size());
    require(
        bridge.midiClockTelemetry().lockState
            == talas::runtime::MidiClockLockState::Locked,
        "Clock may acquire while transport remains stopped");
    (void)bridge.render(
        {}, 2.0, channels.data(), channels.size(), output.size());
    const auto stale = bridge.midiClockTelemetry();
    require(
        !stale.running
            && stale.signalStale
            && stale.lockState == talas::runtime::MidiClockLockState::Stale
            && stale.signalTimeouts == 1,
        "stopped Clock evidence must reach the same terminal stale boundary");
    (void)bridge.render(
        {}, 2.1, channels.data(), channels.size(), output.size());
    require(
        bridge.midiClockTelemetry().signalTimeouts == 1,
        "persistent stale state must not count one timeout on every callback");
    const auto staleRejections =
        bridge.midiClockTelemetry().staleAuthority;
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                6,
                64,
                63,
                2.2),
        "stale authority Start should enter the bounded ingress before evidence validation");
    (void)bridge.render(
        {}, 2.21, channels.data(), channels.size(), output.size());
    const auto rejectedResume = bridge.midiClockTelemetry();
    require(
        !rejectedResume.running
            && rejectedResume.signalStale
            && rejectedResume.staleAuthority == staleRejections + 1,
        "terminal Stale must reject Start without a freshly reacquired pulse lock");
}

void testMidiClockPhaseStateOverflowFailsLockClosed()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48000.0;
    realtimeSettings.maxBlockFrames = 512;
    require(bridge.compileAndStage(realtimeSettings).ok(),
        "phase overflow fixture should stage");
    std::array<float, 480> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 0.9, channels.data(), channels.size(), output.size());
    bridge.configureMidiClockAuthority(81, 8, 82);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    8,
                    82,
                    81,
                    1.0 + static_cast<double>(index) * pulse),
            "phase overflow fixture should admit lock pulses");
    }
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                8,
                82,
                81,
                1.07),
        "phase overflow fixture should admit Start");
    require(bridge.render(
                {}, 1.08, channels.data(), channels.size(), output.size()),
        "phase overflow fixture should start from a valid lock");
    talas::runtime::ProjectRuntimeBridgeTestAccess::
        seedMidiClockPhaseNearOverflow(bridge);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                8,
                82,
                81,
                1.0 + 4.0 * pulse),
        "near-overflow phase fixture should admit its next exact pulse");
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStop,
                8,
                82,
                81,
                1.0 + 4.0 * pulse + 0.001),
        "near-overflow phase fixture should admit a later Stop in the same drain");
    (void)bridge.render(
        {}, 1.09, channels.data(), channels.size(), output.size());
    const auto failed = bridge.midiClockTelemetry();
    require(
        !failed.running
            && failed.signalStale
            && failed.lockState == talas::runtime::MidiClockLockState::Stale
            && failed.estimatorOverflows == 1,
        "unrepresentable Q24 phase must stop, discard lock, and count one estimator overflow");
}

void testMidiClockFollowerOverflowStopsRunningTransport()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48000.0;
    realtimeSettings.maxBlockFrames = 512;
    require(bridge.compileAndStage(realtimeSettings).ok(),
        "follower overflow fixture should stage");
    std::array<float, 480> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(
        {}, 0.9, channels.data(), channels.size(), output.size());
    bridge.configureMidiClockAuthority(83, 8, 84);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    8,
                    84,
                    83,
                    1.0 + static_cast<double>(index) * pulse),
            "follower overflow fixture should admit lock pulses");
    }
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                8,
                84,
                83,
                1.07),
        "follower overflow fixture should admit Start");
    require(bridge.render(
                {}, 1.08, channels.data(), channels.size(), output.size()),
        "follower overflow fixture should start from a valid lock");
    const auto arrival = talas::runtime::ProjectRuntimeBridgeTestAccess::
        seedMidiClockFollowerNearTimestampOverflow(bridge);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                8,
                84,
                83,
                arrival),
        "near-timestamp-limit pulse should enter bounded ingress");
    (void)bridge.render(
        {}, arrival, channels.data(), channels.size(), output.size());
    const auto failed = bridge.midiClockTelemetry();
    require(
        !failed.running
            && failed.signalStale
            && failed.lockState == talas::runtime::MidiClockLockState::Stale
            && failed.estimatorOverflows == 1,
        "follower arithmetic overflow must use the shared fail-closed transport path");
}

void testMidiClockFirstPendingStageCallbackUsesAcceptedProfile()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48000.0;
    realtimeSettings.maxBlockFrames = 512;
    require(bridge.compileAndStage(realtimeSettings).ok(),
        "pending external-phase fixture should stage without prior adoption");
    bridge.configureMidiClockAuthority(71, 7, 72);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    7,
                    72,
                    71,
                    1.0 + static_cast<double>(index) * pulse),
            "pending stage should admit exact Clock pulse evidence");
    }
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                7,
                72,
                71,
                1.07),
        "pending stage should admit its exact Start anchor");
    std::array<float, 480> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render(
                {}, 1.08, channels.data(), channels.size(), output.size()),
        "first callback should adopt and render the pending external stage");
    const auto snapshot = bridge.musicalTransportSnapshot();
    require(
        snapshot.transport.playing
            && snapshot.stageId != 0
            && snapshot.externalAuthorityGeneration == 71
            && snapshot.externalMicrosecondsPerQuarter == 500000
            && snapshot.externalLockState
                == talas::runtime::MidiClockLockState::Locked
            && snapshot.musicalPosition.has_value()
            && snapshot.musicalPosition->tick == 19,
        "first adoption must use the accepted stage id and sample rate for external phase");
}

talas::runtime::RealtimeMusicalTransportSnapshot
renderExternalClockPartitionFixture(
    double sampleRate,
    std::size_t partitionFrames)
{
    talas::runtime::ProjectRuntimeBridge bridge(
        makePlayableDocument(220.0),
        registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = sampleRate;
    realtimeSettings.maxBlockFrames = 512;
    require(bridge.compileAndStage(realtimeSettings).ok(),
        "partition fixture should stage");
    std::array<float, 512> output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render({}, 0.9, channels.data(), channels.size(), 1);
    bridge.configureMidiClockAuthority(71, 7, 73);
    constexpr double pulse = 60.0 / (120.0 * 24.0);
    for (int index = 0; index < 4; ++index)
    {
        require(bridge.enqueueMidiClock(
                    talas::runtime::RealtimeControlCommandKind::MidiClockTick,
                    7,
                    73,
                    71,
                    1.0 + static_cast<double>(index) * pulse),
            "partition fixture pulse should enqueue");
    }
    (void)bridge.render({}, 1.063, channels.data(), channels.size(), 1);
    require(bridge.enqueueMidiClock(
                talas::runtime::RealtimeControlCommandKind::MidiClockStart,
                7,
                73,
                71,
                1.07),
        "partition fixture Start should enqueue");
    std::size_t renderedFrames = 0;
    auto callbackTime = 1.07;
    while (renderedFrames < 512)
    {
        const auto current = std::min(
            partitionFrames,
            std::size_t{512} - renderedFrames);
        callbackTime += static_cast<double>(current) / sampleRate;
        require(bridge.render(
                    {},
                    callbackTime,
                    channels.data(),
                    channels.size(),
                    current),
            "partitioned external Clock render should succeed");
        renderedFrames += current;
    }
    return bridge.musicalTransportSnapshot();
}

void testExternalMusicalPhaseIsPartitionInvariant()
{
    for (const auto sampleRate : {44100.0, 48000.0, 96000.0})
    {
        const auto reference = renderExternalClockPartitionFixture(
            sampleRate, 512);
        for (const auto partition : {std::size_t{1}, std::size_t{7},
                 std::size_t{64}, std::size_t{256}})
        {
            const auto candidate = renderExternalClockPartitionFixture(
                sampleRate, partition);
            require(
                candidate.musicalPosition == reference.musicalPosition
                    && candidate.externalLockState
                        == talas::runtime::MidiClockLockState::Locked
                    && candidate.externalMicrosecondsPerQuarter == 500000
                    && candidate.transport.discontinuitySequence
                        == reference.transport.discontinuitySequence,
                "external musical phase should be invariant across callback partitioning");
        }
    }
}

void testBridgePublishesCompletePreparedLiveSetCells()
{
    auto document = makePlayableDocument(220.0, "!Gain");
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.domain = talas::expr::ValueDomain::Scalar;
    gain.value = 1.0;
    gain.minimum = 0.0;
    gain.maximum = 1.0;
    document.virtualControls.push_back(std::move(gain));
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "liveVoice";
    sound.displayName = "Live Voice";
    sound.graph.nodes.push_back({
        "tone",
        "talas.oscillator",
        {expressionParameter("frequency", "330 hz"),
         expressionParameter("amplitude", "0.25")},
    });
    sound.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "tone",
        "audioOut",
    });
    document.soundDefinitions.push_back(std::move(sound));

    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    gainTarget.eventName = "Gain";
    gainTarget.domain = talas::expr::ValueDomain::Scalar;
    talas::project::SnapshotValue gainValue;
    gainValue.number = 0.5;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "liveBank";
    bank.displayName = "Live Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    bank.parameterPolicies.push_back({
        gainTarget,
        false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots.push_back({
        "half",
        "Half",
        {{gainTarget, gainValue}},
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));
    document.timeline.score.tracks.push_back({
        "master",
        "Master",
        talas::project::ScoreTrackKind::Master,
        talas::project::ScoreTimeDomain::Seconds,
        {},
        1,
        talas::project::ScoreOverlapMode::Layer,
    });
    document.timeline.score.markers.push_back({
        "region",
        "Region",
        {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, 960},
        talas::project::ScoreTime{
            talas::project::ScoreTimeDomain::MusicalTicks,
            0.0,
            1920},
    });
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "performance";
    liveSet.displayName = "Performance";
    liveSet.cells = {
        {"sound", "Sound", 0, 0, {}, talas::project::ProjectLiveSetSoundTarget{"liveVoice", {}}, {}},
        {"snapshot",
         "Snapshot",
         0,
         1,
         {},
         talas::project::ProjectLiveSetSnapshotTarget{"liveBank", "half"},
         {}},
        {"region", "Region", 0, 2, {}, talas::project::ProjectLiveSetScoreRegionTarget{"region"}, {}},
        {"stop", "Stop", 0, 3, {}, talas::project::ProjectLiveSetStopTarget{}, {}},
        {"queuedStop",
         "Queued Stop",
         0,
         4,
         {talas::project::ProjectLiveSetQuantizationKind::NextBeat, 0},
         talas::project::ProjectLiveSetStopTarget{},
         {}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    talas::project::ProjectLiveTool tool;
    tool.toolId = "transport";
    tool.displayName = "Transport";
    tool.initialStateId = "ready";
    tool.states.push_back({"ready", "Ready"});
    tool.states.push_back({"latched", "Latched"});
    tool.variables.push_back({
        "enabled",
        "Enabled",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    talas::project::ProjectLiveToolTransition stopTransition;
    stopTransition.transitionId = "stop";
    stopTransition.fromStateId = "ready";
    stopTransition.toStateId = "ready";
    stopTransition.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    stopTransition.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    talas::project::ProjectLiveToolCommand setGain;
    setGain.kind =
        talas::project::ProjectLiveToolCommandKind::SetEventValue;
    setGain.eventName = "Gain";
    setGain.eventDomain = talas::expr::ValueDomain::Scalar;
    setGain.value = 0.25;
    stopTransition.commands.push_back(std::move(setGain));
    tool.transitions.push_back(std::move(stopTransition));
    talas::project::ProjectLiveToolTransition guardedTransition;
    guardedTransition.transitionId = "guarded";
    guardedTransition.fromStateId = "ready";
    guardedTransition.toStateId = "ready";
    guardedTransition.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    guardedTransition.trigger.variableId = "enabled";
    guardedTransition.guardExpression = "@enabled";
    guardedTransition.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    tool.transitions.push_back(std::move(guardedTransition));
    document.liveSetLibrary.tools.push_back(std::move(tool));

    const auto liveValidation =
        talas::project::validateProjectLiveSetLibrary(document);
    auto liveDetail = std::string{"Live Set fixture should validate"};
    for (const auto& diagnostic : liveValidation.diagnostics)
        liveDetail += " | " + diagnostic.path + ": " + diagnostic.message;
    require(liveValidation.ok(), liveDetail);

    try
    {
        (void)talas::project::serializeProjectDocument(document);
    }
    catch (const std::exception& error)
    {
        require(false, std::string{"Live Set fixture should serialize: "}
                + error.what());
    }

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48000.0;
    const auto staged = [&]() {
        try
        {
            return bridge.compileAndStage(realtimeSettings);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(
                std::string{"Live Set fixture stage should not throw: "}
                + error.what());
        }
    }();
    auto stageDetail = staged.message;
    for (const auto& diagnostic : staged.diagnostics)
        stageDetail += " | " + diagnostic.message;
    require(staged.ok(), stageDetail);
    const auto prepared = bridge.latestPreparedLiveSet();
    const auto preparedTools = bridge.latestPreparedLiveTools();
    require(prepared != nullptr && prepared->cells().size() == 5 &&
                prepared->binding().stageGeneration == bridge.latestStageGeneration() &&
                prepared->binding().stageId == bridge.latestPreparedStageId(),
            "accepted stage should publish one exact immutable Live Set authority");
    require(preparedTools != nullptr && preparedTools->tools().size() == 1
                && preparedTools->binding() == prepared->binding(),
            "accepted stage should publish Live Tools with the exact same immutable Live Set authority");
    require(preparedTools->tools().front().transitions.front().commands.size() == 2
                && preparedTools->tools().front().transitions.front().commands.back()
                    .typedTarget.has_value()
                && preparedTools->tools().front().transitions.front().commands.back()
                    .typedTarget->stageId == prepared->binding().stageId,
            "accepted stage should replace string-only scalar destinations with exact numeric engine handles");
    auto cellDetail = std::string{
        "all valid Sound, Snapshot, Score, and Stop cells should prepare completely"};
    for (const auto& cell : prepared->cells())
    {
        if (const auto* failure = std::get_if<
                talas::runtime::PreparedLiveSetFailedProgram>(&cell.program))
        {
            cellDetail += " | " + cell.cellId + ": " + failure->error;
        }
    }
    require(std::ranges::all_of(
                prepared->cells(),
                &talas::runtime::PreparedLiveSetCell::ready),
            cellDetail);
    const auto& soundProgram =
        std::get<talas::runtime::PreparedLiveSetSoundProgram>(
            prepared->cells()[0].program);
    const auto& snapshotProgram =
        std::get<talas::runtime::PreparedLiveSetSnapshotProgram>(
            prepared->cells()[1].program);
    const auto& scoreProgram =
        std::get<talas::runtime::PreparedLiveSetScoreRegion>(
            prepared->cells()[2].program);
    require(soundProgram.graph != nullptr,
            "Sound cell should retain a reusable prepared graph");
    require(snapshotProgram.overlayGraph == nullptr
                && snapshotProgram.eventValueUpdates.size() == 1
                && snapshotProgram.eventValueUpdates.front().value == 0.5,
            "Event Value Snapshot cell should retain one exact direct transaction");
    require(scoreProgram.startFrame == 24000
                && scoreProgram.endFrame == 48000,
            "Score cell should retain the exact ranged marker frames");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output), "Live Set activation fixture should adopt its prepared stage");
    const auto preparedStageGeneration = bridge.latestStageGeneration();
    const auto rejectedVariableTrigger = bridge.requestLiveToolTrigger({
        .toolId = "transport",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::VariableChange,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Variable,
        .identifier = "enabled",
        .variableBoolean = false,
        .hasVariableValue = true,
        .sourceGeneration = preparedStageGeneration,
        .routeGeneration = preparedStageGeneration,
    });
    require(rejectedVariableTrigger.ok() && render(bridge, output),
        "a guard-rejected variable trigger should be accepted into the exact callback authority");
    const auto rejectedState = bridge.realtimeLiveToolState();
    const auto rejectedTelemetry = bridge.realtimeLiveToolTelemetry();
    require(rejectedState.has_value()
                && !rejectedState->variables[0].boolean
                && rejectedTelemetry.acceptedTriggers == 1
                && rejectedTelemetry.rejectedTriggers == 1
                && rejectedTelemetry.guardRejected == 1
                && rejectedTelemetry.noMatchingTransition == 0
                && rejectedTelemetry.invalidTrigger == 0
                && rejectedTelemetry.commandRejected == 0,
        "callback telemetry must preserve the exact guard-rejection reason while rejected values hold last-good state");
    bridge.publishLiveToolSourceAuthority(
        talas::runtime::RealtimeLiveToolSourceKind::Workbench, 401, 409);
    const auto staleToolTrigger = bridge.requestLiveToolTrigger({
        .toolId = "transport",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::Manual,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Workbench,
        .sourceGeneration = 401,
        .routeGeneration = 410,
    });
    require(staleToolTrigger.outcome
                == talas::runtime::LiveToolTriggerOutcome::StaleAuthority,
            "Live Tool request must reject a mismatched route generation before ingress");
    const auto toolTrigger = bridge.requestLiveToolTrigger({
        .toolId = "transport",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::Manual,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Offline,
        .sourceGeneration = preparedStageGeneration,
        .routeGeneration = preparedStageGeneration,
    });
    std::size_t toolAllocations = 0;
    require(toolTrigger.ok()
                && renderAndCount(bridge, output, toolAllocations)
                && toolAllocations == 0,
            "exact Workbench Live Tool trigger should execute one zero-allocation callback transition");
    const auto toolTelemetry = bridge.realtimeLiveToolTelemetry();
    require(toolTelemetry.acceptedTriggers == 2
                && toolTelemetry.appliedTransitions == 1
                && toolTelemetry.rejectedTriggers == 1
                && toolTelemetry.guardRejected == 1
                && toolTelemetry.stopCommands == 1
                && toolTelemetry.setEventValueCommands == 1
                && toolTelemetry.activateCellCommands == 0
                && toolTelemetry.setProjectVariableCommands == 0
                && toolTelemetry.setWorkspaceVariableCommands == 0,
            "the offline-authorized prepared command trace must match the distinct realtime callback executor and publish bounded telemetry");
    const auto initialToolState = bridge.realtimeLiveToolState();
    require(initialToolState.has_value()
                && initialToolState->toolCount == 1
                && initialToolState->variableCount == 1
                && initialToolState->stateIndices[0] == 0
                && !initialToolState->variables[0].boolean,
            "audio adoption should publish one complete exact Live Tool runtime state");
    auto restoredToolState = *initialToolState;
    restoredToolState.stateIndices[0] = 1;
    restoredToolState.variables[0].boolean = true;
    restoredToolState.lastTriggerSequences[0] = 77;
    std::uint64_t restoreRequestGeneration = 0;
    require(bridge.requestLiveToolStateRestore(
                restoredToolState, &restoreRequestGeneration)
                && restoreRequestGeneration != 0
                && bridge.appliedLiveToolStateRestoreGeneration()
                    < restoreRequestGeneration,
            "a definition-compatible Live Tool session state should enqueue for restore");
    bridge.setPlaying(true);
    require(render(bridge, output),
            "a definition-compatible Live Tool session state should restore at one callback boundary");
    const auto publishedRestoredState = bridge.realtimeLiveToolState();
    require(publishedRestoredState.has_value()
                && publishedRestoredState->stateIndices[0] == 1
                && publishedRestoredState->variables[0].boolean
                && publishedRestoredState->lastTriggerSequences[0] == 0
                && bridge.appliedLiveToolStateRestoreGeneration()
                    == restoreRequestGeneration,
            "restored Live Tool state should publish coherently with an exact callback acknowledgment while runtime-only trigger sequence restarts cleanly");
    auto staleToolState = restoredToolState;
    ++staleToolState.stageGeneration;
    require(!bridge.requestLiveToolStateRestore(staleToolState),
            "Live Tool session restore must reject stale stage authority before publication");
    bridge.setPlaying(true);
    publishWorkbenchLiveSetAuthority(bridge);
    const auto mainProgramOutput = output;

    const auto soundActivation = requestWorkbenchLiveSetActivation(bridge, "performance", "sound");
    std::size_t activationAllocations = 0;
    require(soundActivation.ok() && bridge.realtimeLiveSetTelemetry().completedActivations == 0 &&
                soundActivation.resolvedFrame == bridge.transportFramePosition() &&
                renderAndCount(bridge, output, activationAllocations) && hasAudibleSample(output) &&
                output != mainProgramOutput,
            "Sound cell should switch to its fully prepared program at one exact "
            "callback boundary");
    require(activationAllocations == 0,
            "Sound cell activation and render must "
            "allocate zero times on the callback");
    auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 1 && telemetry.activeCellIndexPlusOne == 1 &&
                telemetry.activeRequestGeneration == soundActivation.requestGeneration,
            "Sound activation should publish its exact cell and request identity");

    const auto snapshotActivation = requestWorkbenchLiveSetActivation(bridge, "performance", "snapshot");
    require(snapshotActivation.ok() &&
                snapshotActivation.resolvedFrame == bridge.transportFramePosition() &&
                renderAndCount(bridge, output, activationAllocations),
            "immediate Snapshot cell should execute at the next exact callback "
            "boundary and apply its direct Event Value transaction");
    require(activationAllocations == 0,
            "Snapshot cell activation and render must allocate zero times on the "
            "callback");
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 2 && telemetry.activeCellIndexPlusOne == 2 &&
                telemetry.activeRequestGeneration == snapshotActivation.requestGeneration,
            "Snapshot activation should publish its exact cell and request identity");

    const auto scoreActivation = requestWorkbenchLiveSetActivation(bridge, "performance", "region");
    require(scoreActivation.ok() && renderAndCount(bridge, output, activationAllocations) &&
                bridge.transportFramePosition() == 24004,
            "Score-region cell should relocate and render from its exact ranged "
            "marker start");
    require(activationAllocations == 0,
            "Score cell activation and render must "
            "allocate zero times on the callback");
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 3 && telemetry.activeCellIndexPlusOne == 3 &&
                telemetry.scoreRegionActive,
            "Score-region activation should publish one active bounded region");

    const auto losingSoundActivation = requestWorkbenchLiveSetActivation(bridge, "performance", "sound");
    const auto stopActivation = requestWorkbenchLiveSetActivation(bridge, "performance", "stop");
    require(losingSoundActivation.ok() && stopActivation.ok() &&
                losingSoundActivation.resolvedFrame == stopActivation.resolvedFrame,
            "same-frame Sound and Stop cells should enter one exact realtime "
            "authority");
    static_cast<void>(renderAndCount(bridge, output, activationAllocations));
    require(activationAllocations == 0,
            "Stop cell activation must allocate zero times on the callback");
    require(bridge.transportFramePosition() == 24004,
            "Stop cell should hold transport at its exact activation boundary");
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.acceptedRequests == 5 && telemetry.completedActivations == 4 &&
                telemetry.activeCellIndexPlusOne == 0 &&
                telemetry.activeRequestGeneration == 0 &&
                telemetry.terminalRequestGeneration
                    >= stopActivation.requestGeneration &&
                !telemetry.scoreRegionActive,
            "last accepted same-frame Stop should win before Sound side effects "
            "and clear every active program identity");

    const auto staleQueuedStop = requestWorkbenchLiveSetActivation(bridge, "performance", "queuedStop");
    require(staleQueuedStop.ok() && staleQueuedStop.resolvedFrame == 48000 &&
                bridge.requestTransportRelocation(0),
            "future quantized activation should freeze its original transport "
            "authority");
    static_cast<void>(renderAndCount(bridge, output, activationAllocations));
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(activationAllocations == 0 && telemetry.acceptedRequests == 6 &&
                telemetry.completedActivations == 4 && telemetry.staleAuthority >= 1 &&
                telemetry.terminalRequestGeneration
                    >= staleQueuedStop.requestGeneration &&
                telemetry.activeCellIndexPlusOne == 0,
            "transport discontinuity should revoke the queued prior-authority "
            "cell before execution");
}

void testBridgeMixesIndependentLiveSetColumns()
{
    auto document = makePlayableDocument(1.0, "0");
    const auto addSound = [&](std::string id, int frequency) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone",
            "talas.oscillator",
            {expressionParameter(
                 "frequency", std::to_string(frequency) + " hz"),
             expressionParameter("amplitude", "0.2")},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    };
    addSound("voiceA", 220);
    addSound("voiceB", 330);
    addSound("voiceC", 440);
    addSound("voiceD", 550);
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "parallel";
    liveSet.displayName = "Parallel";
    liveSet.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"voiceA", {}}, {}},
        {"b", "B", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{"voiceB", {}}, {}},
        {"c", "C", 1, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"voiceC", {}}, {}},
        {"muteA", "Mute A", 2, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{true}, {}},
        {"unmuteA", "Unmute A", 3, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{false}, {}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    talas::project::ProjectLiveSet nextSet;
    nextSet.liveSetId = "next";
    nextSet.displayName = "Next";
    nextSet.cells.push_back({
        "d", "D", 0, 2, {},
        talas::project::ProjectLiveSetSoundTarget{"voiceD", {}}, {}});
    document.liveSetLibrary.liveSets.push_back(std::move(nextSet));
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok()
                && reopened.document.liveSetLibrary == document.liveSetLibrary,
            "track mute cells should retain their exact state after save and reopen");

    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge aOnly(document, registry());
    talas::runtime::ProjectRuntimeBridge bOnly(document, registry());
    talas::runtime::ProjectRuntimeBridge cOnly(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48'000.0;
    for (auto* bridge : {&mixed, &aOnly, &bOnly, &cOnly})
    {
        require(bridge->compileAndStage(realtimeSettings).ok(),
                "parallel Live Set fixture should stage");
        bridge->setPlaying(true);
        publishWorkbenchLiveSetAuthority(*bridge);
        std::array<float, 4> silence{};
        require(render(*bridge, silence) && !hasAudibleSample(silence),
                "parallel Live Set fixture should adopt its silent root");
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> aOutput{};
    std::array<float, 4> bOutput{};
    std::array<float, 4> cOutput{};
    const auto compareMix = [&](const auto& first, const auto& second) {
        for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
            require(std::abs(mixedOutput[frame] - first[frame] - second[frame])
                        < 1.0e-5F,
                    "parallel output must equal independently rendered Sound programs");
    };
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "a").ok()
                && requestWorkbenchLiveSetActivation(aOnly, "parallel", "a").ok(),
            "first independent Sound column should launch");
    require(render(mixed, mixedOutput) && render(aOnly, aOutput)
                && render(bOnly, bOutput) && render(cOnly, cOutput),
            "first independent Sound column should render");
    require(hasAudibleSample(aOutput),
            "first source must be audible before parallel mixing");
    std::size_t callbackAllocations = 0;
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "muteA").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(aOnly, aOutput) && render(bOnly, bOutput)
                && render(cOnly, cOutput) && callbackAllocations == 0
                && !hasAudibleSample(mixedOutput),
            "muting the only active Sound should output silence while its clock advances");
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "unmuteA").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(aOnly, aOutput) && render(bOnly, bOutput)
                && render(cOnly, cOutput) && callbackAllocations == 0,
            "unmuting the only active Sound should render without allocation");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - aOutput[frame]) < 1.0e-5F,
                "single-column unmute must resume at the reference Sound phase");
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "b").ok()
                && requestWorkbenchLiveSetActivation(bOnly, "parallel", "b").ok(),
            "second independent Sound column should launch");
    for (int block = 0; block < 3; ++block)
    {
        require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                    && render(aOnly, aOutput) && render(bOnly, bOutput)
                    && render(cOnly, cOutput),
                "parallel Sound columns should keep rendering");
        require(callbackAllocations == 0,
                "parallel Sound mixing must allocate zero times on the callback");
        require(hasAudibleSample(bOutput),
                "second source must be audible during parallel mixing");
        compareMix(aOutput, bOutput);
    }
    auto telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.soundCellIndexPlusOneByColumn[0] == 1
                && telemetry.soundCellIndexPlusOneByColumn[1] == 2
                && telemetry.mutedSoundColumnsMask == 0,
            "both active Sound cells should publish independent column identities");
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "muteA").ok(),
            "first Sound column should accept an exact mute cell");
    for (int block = 0; block < 3; ++block)
    {
        require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                    && render(aOnly, aOutput) && render(bOnly, bOutput)
                    && render(cOnly, cOutput),
                "muted Sound should keep time while the other column remains audible");
        require(callbackAllocations == 0,
                "track mute must allocate zero times on the callback");
        for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
            require(std::abs(mixedOutput[frame] - bOutput[frame]) < 1.0e-5F,
                    "muting one column should leave the other column unchanged");
    }
    telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.soundCellIndexPlusOneByColumn[0] == 1
                && telemetry.soundCellIndexPlusOneByColumn[1] == 2
                && telemetry.mutedSoundColumnsMask == 1,
            "track mute should retain both active Sound identities and publish its column bit");
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "unmuteA").ok(),
            "first Sound column should accept an exact unmute cell");
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(aOnly, aOutput) && render(bOnly, bOutput)
                && render(cOnly, cOutput) && callbackAllocations == 0,
            "unmute should render without resetting either Sound");
    compareMix(aOutput, bOutput);
    require(mixed.realtimeLiveSetTelemetry().mutedSoundColumnsMask == 0,
            "unmute should clear only its column mute bit");
    require(requestWorkbenchLiveSetActivation(mixed, "parallel", "c").ok()
                && requestWorkbenchLiveSetActivation(cOnly, "parallel", "c").ok(),
            "replacing one Sound column should launch independently");
    for (int block = 0; block < 3; ++block)
    {
        require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                    && render(aOnly, aOutput) && render(bOnly, bOutput)
                    && render(cOnly, cOutput),
                "same-column replacement should keep the other Sound running");
        require(callbackAllocations == 0,
                "same-column replacement must allocate zero times on the callback");
        compareMix(cOutput, bOutput);
    }
    const auto nextActivation = requestWorkbenchLiveSetActivation(
        mixed, "next", "d");
    require(nextActivation.ok(),
            "switching Live Sets should accept the new Sound cell");
    require(renderAndCount(mixed, mixedOutput, callbackAllocations),
            "switching Live Sets should render the new Sound cell");
    require(callbackAllocations == 0,
            "switching Live Sets must allocate zero times on the callback");
    require(hasAudibleSample(mixedOutput),
            "switching Live Sets should make the new Sound audible: sample="
                + std::to_string(mixedOutput[1]) + " active="
                + std::to_string(mixed.realtimeLiveSetTelemetry()
                                     .activeCellIndexPlusOne)
                + " terminal="
                + std::to_string(static_cast<int>(
                    mixed.realtimeLiveSetTelemetry().terminalOutcome)));
    telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.soundCellIndexPlusOneByColumn[0] == 0
                && telemetry.soundCellIndexPlusOneByColumn[1] == 0
                && telemetry.soundCellIndexPlusOneByColumn[2] == 6,
            "switching Live Sets should clear every Sound from the previous set");
    const auto staleMute = requestWorkbenchLiveSetActivation(
        mixed, "parallel", "muteA");
    require(staleMute.ok() && render(mixed, mixedOutput),
            "old-set mute request should reach its exact realtime boundary");
    telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.terminalRequestGeneration == staleMute.requestGeneration
                && telemetry.terminalOutcome
                    == talas::runtime::RealtimeLiveSetTerminalOutcome::Failed
                && telemetry.soundCellIndexPlusOneByColumn[2] == 6
                && telemetry.mutedSoundColumnsMask == 0,
            "a mute cell from another Live Set must not change the active set");
}

void testBridgeRoutesLiveSetTrackThroughEffect()
{
    auto document = makePlayableDocument(1.0, "0");
    const auto addSource = [&](std::string id, int frequency) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone", "talas.oscillator",
            {expressionParameter(
                 "frequency", std::to_string(frequency) + " hz"),
             expressionParameter("amplitude", "0.2")},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    };
    addSource("routedSource", 220);
    addSource("independentSource", 330);
    talas::project::ProjectSoundDefinition effect;
    effect.definitionId = "halfEffect";
    effect.displayName = "Half Effect";
    effect.graph.nodes = {
        {"amp", "talas.vca", {}},
        {"half", "talas.constant", {expressionParameter("value", "0.5")}},
    };
    effect.graph.connections.push_back({
        "half", "value", "amp", "gain"});
    effect.inputs.push_back({
        "audioIn", talas::core::PortType::Audio, "amp", "audioIn"});
    effect.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "amp", "audioOut"});
    document.soundDefinitions.push_back(std::move(effect));
    document.graph.nodes.push_back({"effectInput", "talas.audioInput", {}});
    document.graph.nodes.push_back({
        "effectInstance", "", {}, std::string{"halfEffect"}});
    document.graph.connections.push_back({
        "effectInput", "audioOut", "effectInstance", "audioIn"});

    talas::project::ProjectLiveSet set;
    set.liveSetId = "routed";
    set.displayName = "Routed";
    set.cells = {
        {"source", "Source", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"routedSource", {}}, {}},
        {"effect", "Effect", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{
             "halfEffect", std::string{"effectInstance"}}, {}},
        {"independent", "Independent", 0, 2, {},
         talas::project::ProjectLiveSetSoundTarget{"independentSource", {}}, {}},
        {"mute", "Mute", 1, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{true}, {}},
        {"unmute", "Unmute", 2, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{false}, {}},
    };
    set.routes.push_back({0, 1});
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    require(talas::project::validateProjectLiveSetLibrary(document).ok(),
            "source-to-effect route should validate before preparation");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok()
                && reopened.document.liveSetLibrary == document.liveSetLibrary,
            "source-to-effect route should survive save and reopen");

    auto referenceDocument = document;
    referenceDocument.liveSetLibrary.liveSets.front().routes.clear();
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge sourceOnly(
        referenceDocument, registry());
    talas::runtime::ProjectRuntimeBridge independentOnly(
        referenceDocument, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48'000.0;
    for (auto* bridge : {&mixed, &sourceOnly, &independentOnly})
    {
        const auto staged = bridge->compileAndStage(realtimeSettings);
        require(staged.ok(),
                "routed Live Set fixture should stage: " + staged.message
                    + (staged.diagnostics.empty()
                           ? std::string{}
                           : " / " + staged.diagnostics.front().message
                               + " @ "
                               + staged.diagnostics.front().structuralSourcePath
                               + " # " + staged.diagnostics.front().nodeId
                               + " / " + staged.diagnostics.front().portId));
        bridge->setPlaying(true);
        publishWorkbenchLiveSetAuthority(*bridge);
        std::array<float, 4> output{};
        std::array<float, 4> silentInput{};
        const std::array<const float*, 1> inputChannels{
            silentInput.data()};
        std::array<float*, 1> outputChannels{output.data()};
        require(bridge->render(
                    {inputChannels.data(), inputChannels.size(), nullptr},
                    outputChannels.data(), outputChannels.size(),
                    output.size()),
                "routed Live Set fixture should adopt its stage");
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> sourceOutput{};
    std::array<float, 4> independentOutput{};
    const auto renderWithSilentInput = [](auto& bridge,
                                          auto& output) {
        output.fill(0.0F);
        std::array<float, 4> silentInput{};
        const std::array<const float*, 1> inputChannels{
            silentInput.data()};
        std::array<float*, 1> outputChannels{output.data()};
        return bridge.render(
            {inputChannels.data(), inputChannels.size(), nullptr},
            outputChannels.data(), outputChannels.size(),
            output.size());
    };
    const auto renderAll = [&] {
        const auto mixedRendered = renderWithSilentInput(
            mixed, mixedOutput);
        const auto sourceRendered = renderWithSilentInput(
            sourceOnly, sourceOutput);
        const auto independentRendered = renderWithSilentInput(
            independentOnly, independentOutput);
        return mixedRendered && sourceRendered && independentRendered;
    };
    const auto sourceRequest = requestWorkbenchLiveSetActivation(
        mixed, "routed", "source");
    const auto referenceRequest = requestWorkbenchLiveSetActivation(
        sourceOnly, "routed", "source");
    const auto sourceRendered = renderAll();
    require(sourceRequest.ok() && referenceRequest.ok()
                && sourceRendered && hasAudibleSample(sourceOutput)
                && !hasAudibleSample(mixedOutput),
            "a source routed to an inactive effect must not leak into master output");
    require(requestWorkbenchLiveSetActivation(
                mixed, "routed", "effect").ok()
                && renderAll(),
            "effect track should activate over its already-running source");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - 0.5F * sourceOutput[frame])
                    < 1.0e-5F,
                "effect route should output the processed source once");
    require(requestWorkbenchLiveSetActivation(
                mixed, "routed", "independent").ok()
                && requestWorkbenchLiveSetActivation(
                    independentOnly, "routed", "independent").ok(),
            "independent source should launch beside the effect route");
    std::size_t callbackAllocations = 0;
    for (int block = 0; block < 3; ++block)
    {
        require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                    && render(sourceOnly, sourceOutput)
                    && render(independentOnly, independentOutput)
                    && callbackAllocations == 0
                    && hasAudibleSample(independentOutput),
                "routed effect and independent source should render without callback allocation");
        for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
            require(std::abs(mixedOutput[frame]
                             - 0.5F * sourceOutput[frame]
                             - independentOutput[frame]) < 1.0e-5F,
                    "master mix should contain processed route and independent source");
    }
    require(requestWorkbenchLiveSetActivation(
                mixed, "routed", "mute").ok()
                && renderAll(),
            "source track mute should execute while its effect remains active");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - independentOutput[frame])
                    < 1.0e-5F,
                "muted source should not feed its effect track");
    require(requestWorkbenchLiveSetActivation(
                mixed, "routed", "unmute").ok()
                && renderAll(),
            "source track should unmute without restarting the route");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - 0.5F * sourceOutput[frame]
                         - independentOutput[frame]) < 1.0e-5F,
                "unmuted source should resume its effect at the continuing phase");
}

void testBridgeRendersFanOutFanInAndChainedGridRoutes()
{
    auto document = makePlayableDocument(1.0, "0");
    const auto addSource = [&](std::string id, int frequency) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone", "talas.oscillator",
            {expressionParameter(
                 "frequency", std::to_string(frequency) + " hz"),
             expressionParameter("amplitude", "0.2")},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    };
    const auto addEffect = [&](std::string id, std::string gain) {
        talas::project::ProjectSoundDefinition effect;
        effect.definitionId = id;
        effect.displayName = id;
        effect.graph.nodes = {
            {"amp", "talas.vca", {}},
            {"gain", "talas.constant", {expressionParameter("value", gain)}},
        };
        effect.graph.connections.push_back({
            "gain", "value", "amp", "gain"});
        effect.inputs.push_back({
            "audioIn", talas::core::PortType::Audio, "amp", "audioIn"});
        effect.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "amp", "audioOut"});
        document.soundDefinitions.push_back(std::move(effect));
        const auto inputId = "input" + id;
        const auto instanceId = "instance" + id;
        document.graph.nodes.push_back({inputId, "talas.audioInput", {}});
        document.graph.nodes.push_back({
            instanceId, "", {}, id});
        document.graph.connections.push_back({
            inputId, "audioOut", instanceId, "audioIn"});
    };
    addSource("sourceA", 220);
    addSource("sourceB", 330);
    addEffect("half", "0.5");
    addEffect("quarter", "0.25");

    talas::project::ProjectLiveSet set;
    set.liveSetId = "branches";
    set.displayName = "Branches";
    set.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"sourceA", {}}, {}},
        {"b", "B", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{"sourceB", {}}, {}},
        {"half", "Half", 0, 2, {},
         talas::project::ProjectLiveSetSoundTarget{
             "half", std::string{"instancehalf"}}, {}},
        {"quarter", "Quarter", 0, 3, {},
         talas::project::ProjectLiveSetSoundTarget{
             "quarter", std::string{"instancequarter"}}, {}},
        {"muteA", "Mute A", 1, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{true}, {}},
        {"unmuteA", "Unmute A", 2, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{false}, {}},
    };
    set.routes = {{0, 2}, {0, 3}, {1, 2}, {2, 3}};
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    require(talas::project::validateProjectLiveSetLibrary(document).ok(),
            "fan-out, fan-in and chained routes should validate together");
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok()
                && reopened.document.liveSetLibrary == document.liveSetLibrary,
            "branched Live Set routes should survive save and reopen");

    auto referenceDocument = document;
    referenceDocument.liveSetLibrary.liveSets.front().routes.clear();
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceA(
        referenceDocument, registry());
    talas::runtime::ProjectRuntimeBridge referenceB(
        referenceDocument, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48'000.0;
    for (auto* bridge : {&mixed, &referenceA, &referenceB})
    {
        require(bridge->compileAndStage(realtimeSettings).ok(),
                "branched Live Set fixture should stage");
        bridge->setPlaying(true);
        publishWorkbenchLiveSetAuthority(*bridge);
        std::array<float, 4> output{};
        require(render(*bridge, output),
                "branched Live Set fixture should adopt its stage");
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> sourceA{};
    std::array<float, 4> sourceB{};
    const auto renderAll = [&] {
        const auto mixedRendered = render(mixed, mixedOutput);
        const auto aRendered = render(referenceA, sourceA);
        const auto bRendered = render(referenceB, sourceB);
        return mixedRendered && aRendered && bRendered;
    };
    require(requestWorkbenchLiveSetActivation(mixed, "branches", "a").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceA, "branches", "a").ok()
                && renderAll() && hasAudibleSample(sourceA)
                && !hasAudibleSample(mixedOutput),
            "the first branched source should stay off master");
    require(requestWorkbenchLiveSetActivation(mixed, "branches", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceB, "branches", "b").ok()
                && renderAll() && hasAudibleSample(sourceB)
                && !hasAudibleSample(mixedOutput),
            "both routed sources should stay off master");
    require(requestWorkbenchLiveSetActivation(
                mixed, "branches", "half").ok()
                && renderAll() && !hasAudibleSample(mixedOutput),
            "a chained effect should feed its destination instead of master");
    require(requestWorkbenchLiveSetActivation(
                mixed, "branches", "quarter").ok()
                && renderAll(),
            "the final effect should receive both direct and chained branches");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - 0.375F * sourceA[frame]
                         - 0.125F * sourceB[frame]) < 1.0e-5F,
                "fan-out, fan-in and chaining should preserve exact branch gains");
    std::size_t callbackAllocations = 0;
    for (int block = 0; block < 3; ++block)
    {
        require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                    && render(referenceA, sourceA)
                    && render(referenceB, sourceB)
                    && callbackAllocations == 0,
                "branched Live Set routing should avoid callback allocations");
        for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
            require(std::abs(mixedOutput[frame]
                             - 0.375F * sourceA[frame]
                             - 0.125F * sourceB[frame]) < 1.0e-5F,
                    "branched routing should remain phase-aligned across callbacks");
    }
    require(requestWorkbenchLiveSetActivation(
                mixed, "branches", "muteA").ok()
                && renderAll(),
            "branched source mute should execute");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - 0.125F * sourceB[frame])
                    < 1.0e-5F,
                "mute should remove every branch of the source");
    require(requestWorkbenchLiveSetActivation(
                mixed, "branches", "unmuteA").ok()
                && renderAll(),
            "branched source should unmute without restarting");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - 0.375F * sourceA[frame]
                         - 0.125F * sourceB[frame]) < 1.0e-5F,
                "all branches should resume at the continuing source phase");
}

void testPeriodicLiveToolTicksSurviveLargeCallbacksAndLoopWraps()
{
    auto document = makePlayableDocument(220.0, "!Gain");
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.5,
        0.0,
        1.0,
    });
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "periodic";
    liveSet.displayName = "Periodic";
    talas::project::ProjectLiveSetCell stopCell;
    stopCell.cellId = "stop";
    stopCell.displayName = "Stop";
    stopCell.target = talas::project::ProjectLiveSetStopTarget{};
    liveSet.cells.push_back(std::move(stopCell));
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    talas::project::ProjectLiveTool tool;
    tool.toolId = "eighths";
    tool.displayName = "Eighths";
    tool.initialStateId = "ready";
    tool.states = {{"ready", "Ready"}};
    talas::project::ProjectLiveToolTransition transition;
    transition.transitionId = "tick";
    transition.fromStateId = "ready";
    transition.toStateId = "ready";
    transition.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::PeriodicTicks;
    transition.trigger.periodTicks = 480;
    // Stop would halt the transport; setting an Event Value keeps it running.
    talas::project::ProjectLiveToolCommand setGain;
    setGain.kind = talas::project::ProjectLiveToolCommandKind::SetEventValue;
    setGain.eventName = "Gain";
    setGain.eventDomain = talas::expr::ValueDomain::Scalar;
    setGain.value = 0.25;
    transition.commands.push_back(std::move(setGain));
    tool.transitions.push_back(std::move(transition));
    document.liveSetLibrary.tools.push_back(std::move(tool));
    // Two beats at 120 BPM: ticks 0-1919, 25 frames per tick at 48 kHz.
    document.timeline.loop = {true, 0.0, 1.0};

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto renderSettings = settings();
    renderSettings.sampleRate = 48'000.0;
    renderSettings.maxBlockFrames = 2048;
    require(bridge.compileAndStage(renderSettings).ok(),
        "periodic Live Tool fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 2048> output{};
    constexpr std::size_t callbacks = 75;
    for (std::size_t callback = 0; callback < callbacks; ++callback)
    {
        require(renderFrames(bridge, output),
            "periodic Live Tool fixture should render");
    }
    // Every eighth note reached by the last callback start fires once:
    // four per pass for three passes, then tick 0 of the fourth pass.
    const auto telemetry = bridge.realtimeLiveToolTelemetry();
    require(telemetry.acceptedTriggers == 13
            && telemetry.rejectedTriggers == 0
            && telemetry.queueOverflow == 0,
        "82-tick callbacks and loop wraps must keep every periodic trigger: accepted "
            + std::to_string(telemetry.acceptedTriggers) + ", rejected "
            + std::to_string(telemetry.rejectedTriggers));
}

void testBridgeAppliesAutomaticLiveToolTriggers()
{
    auto document = makePlayableDocument(220.0, "!Gain");
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        0.0,
        1.0,
    });
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "automatic";
    liveSet.displayName = "Automatic";
    talas::project::ProjectLiveSetCell stopCell;
    stopCell.cellId = "stop";
    stopCell.displayName = "Stop";
    stopCell.target = talas::project::ProjectLiveSetStopTarget{};
    liveSet.cells.push_back(std::move(stopCell));
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));

    const auto makeTool = [](std::string toolId,
                              talas::project::ProjectLiveToolTrigger trigger) {
        talas::project::ProjectLiveTool tool;
        tool.toolId = std::move(toolId);
        tool.displayName = tool.toolId;
        tool.initialStateId = "ready";
        tool.states = {{"ready", "Ready"}, {"fired", "Fired"}};
        if (trigger.kind
            == talas::project::ProjectLiveToolTriggerKind::VariableChange)
        {
            tool.variables.push_back({
                trigger.variableId,
                "Enabled",
                talas::project::ProjectLiveToolVariableKind::Boolean,
                0.0,
                false,
                std::nullopt,
            });
        }
        talas::project::ProjectLiveToolTransition transition;
        transition.transitionId = "fire";
        transition.fromStateId = "ready";
        transition.toStateId = "fired";
        transition.trigger = std::move(trigger);
        transition.commands.push_back({
            talas::project::ProjectLiveToolCommandKind::Stop,
        });
        tool.transitions.push_back(std::move(transition));
        return tool;
    };
    talas::project::ProjectLiveToolTrigger manualTrigger;
    manualTrigger.kind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    document.liveSetLibrary.tools.push_back(
        makeTool("manual", std::move(manualTrigger)));
    talas::project::ProjectLiveToolTrigger eventTrigger;
    eventTrigger.kind = talas::project::ProjectLiveToolTriggerKind::EventValue;
    eventTrigger.eventName = "Gain";
    eventTrigger.eventDomain = talas::expr::ValueDomain::Scalar;
    document.liveSetLibrary.tools.push_back(
        makeTool("event", std::move(eventTrigger)));
    talas::project::ProjectLiveToolTrigger variableTrigger;
    variableTrigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    variableTrigger.variableId = "enabled";
    document.liveSetLibrary.tools.push_back(
        makeTool("variable", std::move(variableTrigger)));
    talas::project::ProjectLiveToolTrigger midiTrigger;
    midiTrigger.kind =
        talas::project::ProjectLiveToolTriggerKind::MidiProgram;
    midiTrigger.midiProgram = {2, 5, "exact-midi"};
    document.liveSetLibrary.tools.push_back(
        makeTool("midi", std::move(midiTrigger)));
    talas::project::ProjectLiveToolTrigger oscTrigger;
    oscTrigger.kind = talas::project::ProjectLiveToolTriggerKind::Osc;
    oscTrigger.osc.address = "/auto";
    document.liveSetLibrary.tools.push_back(
        makeTool("osc", std::move(oscTrigger)));
    talas::project::ProjectLiveToolTrigger periodicTrigger;
    periodicTrigger.kind =
        talas::project::ProjectLiveToolTriggerKind::PeriodicTicks;
    periodicTrigger.periodTicks = 1;
    document.liveSetLibrary.tools.push_back(
        makeTool("periodic", std::move(periodicTrigger)));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto renderSettings = settings();
    renderSettings.sampleRate = 48'000.0;
    require(bridge.compileAndStage(renderSettings).ok(),
        "automatic Live Tool trigger fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output),
        "automatic Live Tool trigger fixture should adopt before control-side trigger admission");
    const auto gainId = bridge.realtimeEventValueIdForLatestStage(
        "Gain", talas::expr::ValueDomain::Scalar);
    const auto stageGeneration = bridge.latestStageGeneration();
    bridge.publishLiveToolSourceAuthority(
        talas::runtime::RealtimeLiveToolSourceKind::MidiProgram, 701, 703);
    bridge.publishLiveToolSourceAuthority(
        talas::runtime::RealtimeLiveToolSourceKind::Osc, 709, 711);
    const auto manual = bridge.requestLiveToolTrigger({
        .toolId = "manual",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::Manual,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Offline,
        .sourceGeneration = stageGeneration,
        .routeGeneration = stageGeneration,
    });
    const auto variable = bridge.requestLiveToolTrigger({
        .toolId = "variable",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::VariableChange,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Variable,
        .identifier = "enabled",
        .variableBoolean = true,
        .hasVariableValue = true,
        .sourceGeneration = stageGeneration,
        .routeGeneration = stageGeneration,
    });
    const auto midi = bridge.requestLiveToolTrigger({
        .toolId = "midi",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::MidiProgram,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::MidiProgram,
        .identifier = "exact-midi",
        .midiChannel = 2,
        .midiProgram = 5,
        .sourceGeneration = 701,
        .routeGeneration = 703,
    });
    const auto osc = bridge.requestLiveToolTrigger({
        .toolId = "osc",
        .triggerKind = talas::project::ProjectLiveToolTriggerKind::Osc,
        .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Osc,
        .identifier = "/auto",
        .sourceGeneration = 709,
        .routeGeneration = 711,
    });
    const auto triggerDetail =
        "all explicit trigger sources and the automatic Event source should enter their exact authorities; outcomes "
        + std::to_string(static_cast<int>(manual.outcome)) + "/"
        + std::to_string(static_cast<int>(variable.outcome)) + "/"
        + std::to_string(static_cast<int>(midi.outcome)) + "/"
        + std::to_string(static_cast<int>(osc.outcome));
    require(manual.ok() && variable.ok() && midi.ok() && osc.ok()
                && gainId.has_value()
                && bridge.enqueueEventValue(
                    *gainId, 0.75, talas::expr::ValueDomain::Scalar),
        triggerDetail);
    bridge.setPlaying(true);
    require(render(bridge, output),
        "automatic Live Tool trigger fixture should consume every explicit and Event trigger");
    const auto state = bridge.realtimeLiveToolState();
    const auto telemetry = bridge.realtimeLiveToolTelemetry();
    require(state.has_value() && state->toolCount == 6
                && std::all_of(
                    state->stateIndices.begin(),
                    state->stateIndices.begin() + 6,
                    [](std::uint8_t value) { return value == 1; })
                && state->variables[0].boolean
                && telemetry.acceptedTriggers == 6
                && telemetry.appliedTransitions == 6
                && telemetry.rejectedTriggers == 0
                && telemetry.stopCommands == 6,
        "manual, Event, variable, MIDI, OSC, and periodic sources must reproduce the six-command portable trace through the distinct production callback executor");
}

void testBridgeActivatesPreparedLiveSetOverlaySnapshot()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {expressionParameter("gain", "0")}},
        {"osc",
         "talas.oscillator",
         {expressionParameter("frequency", "1 hz"),
          expressionParameter("amplitude", "!Level")}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotValue loudValue;
    loudValue.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "overlayBank";
    bank.displayName = "Overlay Bank";
    bank.smoothingSeconds = 2.0;
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.parameterPolicies.push_back({
        gainTarget,
        false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots.push_back({
        "loud",
        "Loud",
        {{gainTarget, loudValue}},
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "overlaySet";
    set.displayName = "Overlay Set";
    set.cells.push_back({
        "overlay",
        "Overlay",
        0,
        0,
        {},
        talas::project::ProjectLiveSetSnapshotTarget{"overlayBank", "loud"},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    auto realtimeSettings = settings();
    realtimeSettings.eventValues.push_back({
        "Level", 0.0, talas::expr::ValueDomain::Scalar});
    const auto staged = bridge.compileAndStage(realtimeSettings);
    require(staged.ok(), staged.message);
    const auto prepared = bridge.latestPreparedLiveSet();
    require(
        prepared != nullptr && prepared->cells().size() == 1 &&
            std::get<talas::runtime::PreparedLiveSetSnapshotProgram>(prepared->cells().front().program)
                    .overlayGraph != nullptr &&
            std::get<talas::runtime::PreparedLiveSetSnapshotProgram>(prepared->cells().front().program)
                    .rampFrames == 8,
        "Node-parameter Snapshot cell should retain one complete prepared "
        "overlay engine");
    std::array<float, 4> output{};
    require(render(bridge, output) && !hasAudibleSample(output),
            "overlay fixture baseline should render its silent main stage");
    const auto levelId = bridge.realtimeEventValueIdForLatestStage(
        "Level", talas::expr::ValueDomain::Scalar);
    require(levelId.has_value()
                && bridge.enqueueEventValue(
                    *levelId, 1.0, talas::expr::ValueDomain::Scalar)
                && render(bridge, output)
                && !hasAudibleSample(output),
            "an unrelated live Event Value should change after overlay preparation while the zero-gain baseline remains silent");
    publishWorkbenchLiveSetAuthority(bridge);
    const auto activation = requestWorkbenchLiveSetActivation(bridge, "overlaySet", "overlay");
    std::size_t allocations = 0;
    require(activation.ok() && bridge.realtimeLiveSetTelemetry().completedActivations == 0 &&
                renderAndCount(bridge, output, allocations) && allocations == 0 &&
                hasAudibleSample(output) && std::abs(output[1]) > 0.2F &&
                std::abs(output[1]) < 0.3F,
            "prepared overlay Snapshot should begin its exact preallocated ramp "
            "at one zero-allocation callback boundary without reverting the unrelated live Event Value");
    auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 0 && telemetry.activeCellIndexPlusOne == 0,
            "overlay Snapshot must remain pending until its prepared ramp finishes");
    require(renderAndCount(bridge, output, allocations) && allocations == 0,
            "second ramp partition should finish without callback allocation");
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 1 && telemetry.activeCellIndexPlusOne == 1 &&
                telemetry.activeRequestGeneration == activation.requestGeneration,
            "overlay Snapshot should publish its exact active identity");
}

void testOversizedCallbackDuringLiveSetRampKeepsTheRamp()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {expressionParameter("gain", "0")}},
        {"osc",
         "talas.oscillator",
         {expressionParameter("frequency", "1 hz"),
          expressionParameter("amplitude", "1")}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotValue loudValue;
    loudValue.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "overlayBank";
    bank.displayName = "Overlay Bank";
    bank.smoothingSeconds = 2.0;
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.parameterPolicies.push_back({
        gainTarget, false, talas::project::SnapshotParameterRange{0.0, 1.0}});
    bank.snapshots.push_back({"loud", "Loud", {{gainTarget, loudValue}}});
    document.snapshotLibrary.banks.push_back(std::move(bank));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "overlaySet";
    set.displayName = "Overlay Set";
    set.cells.push_back({
        "overlay", "Overlay", 0, 0, {},
        talas::project::ProjectLiveSetSnapshotTarget{"overlayBank", "loud"},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    const auto staged = bridge.compileAndStage(settings());
    require(staged.ok(), staged.message);
    std::array<float, 4> output{};
    require(render(bridge, output),
        "oversized-ramp fixture should render its baseline");
    publishWorkbenchLiveSetAuthority(bridge);
    const auto activation =
        requestWorkbenchLiveSetActivation(bridge, "overlaySet", "overlay");
    require(activation.ok() && render(bridge, output)
            && bridge.realtimeLiveSetTelemetry().completedActivations == 0,
        "the overlay Snapshot ramp should begin");

    std::array<float, 5> oversized{};
    require(!renderFrames(bridge, oversized),
        "a callback larger than the prepared block must be rejected");
    require(render(bridge, output) && render(bridge, output)
            && bridge.realtimeLiveSetTelemetry().completedActivations == 1,
        "a rejected oversized callback must not cancel the prepared ramp");
}

talas::project::ProjectDocument makeLoopedInputGainDocument(std::string gain, double loopEndSeconds = 1.75)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"input", "talas.audioInput", {}},
        {"gain", "talas.constant", {expressionParameter("value", std::move(gain))}},
    };
    document.graph.connections = {
        {"input", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    document.timeline.loop = {true, 0.0, loopEndSeconds};
    return document;
}

template <std::size_t FrameCount>
bool renderOnes(talas::runtime::ProjectRuntimeBridge& bridge, std::array<float, FrameCount>& output)
{
    std::array<float, FrameCount> input{};
    input.fill(1.0F);
    std::array<const float*, 1> inputChannels{input.data()};
    std::array<float*, 1> outputChannels{output.data()};
    output.fill(0.0F);
    return bridge.render(
        {inputChannels.data(), inputChannels.size()},
        outputChannels.data(),
        outputChannels.size(),
        output.size());
}

void testBridgeAdoptsLoopRestageExactlyAtItsPreparedFrame()
{
    talas::runtime::ProjectRuntimeBridge bridge(makeLoopedInputGainDocument("0.25"), registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "loop restage fixture should stage");
    std::array<float, 4> output{};
    for (int callback = 0; callback < 3; ++callback)
    {
        require(renderOnes(bridge, output) && output[3] == 0.25F,
                "loop restage fixture should play its first graph");
    }
    talas::runtime::ProjectRuntimeBridgeTestAccess::setInitialStageLeadFrames(bridge, 9);
    require(bridge.editor()
                .setParameter("gain", expressionParameter("value", "0.5"))
                .ok()
            && bridge.compileAndStage(settings()).ok(),
            "an edit made while the loop plays should stage");
    std::vector<float> played;
    for (int callback = 0; callback < 4; ++callback)
    {
        require(renderOnes(bridge, output), "the restaged loop should keep rendering");
        played.insert(played.end(), output.begin(), output.end());
    }
    // Frames 12..27; the replacement was prepared for frame 21, inside the
    // third callback and at no loop-phase or callback boundary.
    for (std::size_t index = 0; index < played.size(); ++index)
    {
        require(played[index] == (index < 9 ? 0.25F : 0.5F),
                "the restaged graph must become audible exactly at its prepared frame "
                "(frame " + std::to_string(12 + index) + " played "
                    + std::to_string(played[index]) + ")");
    }
}

void testBridgeAdoptsTerminalRestageExactlyAtItsPreparedFrame()
{
    // ADR-0005: a staged replacement becomes active exactly at its prepared
    // frame. A terminal callback that frame falls inside renders up to it
    // with the incumbent and on from it with the replacement, as an audio
    // callback does, and reports both stages.
    auto document = makeMidiNoteTerminalDocument();
    document.timeline.loop = {true, 0.0, 1.75};
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    const auto first = bridge.compileAndStage(settings());
    require(first.ok() && first.outputChannelCount == 0,
            "looped terminal-only fixture should stage with zero audio outputs");
    std::array<talas::runtime::RealtimeMidiOutputEvent, 8> events{};
    talas::runtime::RealtimeTerminalOutputBlock output{events};
    for (int callback = 0; callback < 5; ++callback)
    {
        require(bridge.renderTerminalEvents(4, output)
                        == talas::runtime::RealtimeTerminalRenderResult::Rendered
                    && output.previousStageId == 0 && output.adoptionFrameOffset == 0,
                "looped terminal-only fixture should render");
    }
    const auto incumbent = bridge.activeStageId();
    talas::runtime::ProjectRuntimeBridgeTestAccess::setInitialStageLeadFrames(bridge, 2);
    require(bridge.editor()
                .setParameter("terminal", expressionParameter("channel", "3"))
                .ok(),
            "the terminal fixture's channel should be editable");
    // Prepared for frame 22, inside the block 20..23.
    const auto restage = bridge.compileAndStage(settings());
    require(restage.ok(),
            "a terminal-only engine should take a restage prepared inside a block: " + restage.message);
    require(bridge.renderTerminalEvents(4, output)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered,
            "the block holding the prepared frame should render");
    std::string sent;
    for (std::size_t index = 0; index < output.midiEventCount; ++index)
    {
        sent += " " + std::to_string(output.midiEvents[index].frameOffset) + ":"
            + std::to_string(output.midiEvents[index].bytes[0]);
    }
    require(output.stageId != incumbent && output.stageId == bridge.activeStageId()
                && output.previousStageId == incumbent && output.adoptionFrameOffset == 2,
            "the replacement should be adopted at frame 22, two frames into the block (events" + sent + ")");
    const auto replacementNote = std::ranges::find_if(
        output.midiEvents.first(output.midiEventCount),
        [](const auto& event) { return event.bytes[0] == 0x92; });
    require(replacementNote != output.midiEvents.first(output.midiEventCount).end()
                && replacementNote->frameOffset == 2
                && std::ranges::none_of(output.midiEvents.first(output.midiEventCount),
                    [](const auto& event) { return event.frameOffset < 2 && (event.bytes[0] & 0x0f) == 2; }),
            "the replacement's note should start at its prepared frame and no sooner (events" + sent + ")");
    for (int callback = 0; callback < 4; ++callback)
    {
        require(bridge.renderTerminalEvents(4, output)
                        == talas::runtime::RealtimeTerminalRenderResult::Rendered
                    && bridge.activeStageId() == output.stageId
                    && output.previousStageId == 0
                    && !bridge.preparedGraphStatus().awaitingActivation
                    && !bridge.preparedGraphStatus().activationRejected,
                "the adopted replacement should keep playing with nothing pending or rejected");
    }
}

void testBridgeKeepsMidiNotesOnTheIncumbentWhileAReplacementWaits()
{
    auto document = makeMidiGateDocument("!KeyDown");
    document.timeline.loop = {true, 0.0, 1.75};
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    bridge.setPlaying(true);
    require(bridge.compileAndStage(settings()).ok(), "pending MIDI fixture should stage");
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(bridge.render({}, 10.0, channels.data(), channels.size(), output.size()),
            "pending MIDI fixture should render its first graph");
    talas::runtime::ProjectRuntimeBridgeTestAccess::setInitialStageLeadFrames(bridge, 9);
    require(bridge.compileAndStage(settings()).ok()
                && bridge.preparedGraphStatus().awaitingActivation,
            "a replacement staged during playback should wait for its prepared frame");
    require(bridge.enqueueMidiNoteOn(1, 60, 100, 10.25)
                && bridge.render({}, 11.0, channels.data(), channels.size(), output.size()),
            "a note played while the replacement waits should render");
    require(output[0] == 0.0F && output[1] == 1.0F && output[3] == 1.0F
                && bridge.preparedGraphStatus().awaitingActivation,
            "notes played while a replacement waits must sound on the incumbent");
}

void testBridgeRestagesPlayingLoopWithoutWaitingForItsPhase()
{
    // The audio thread runs far faster than real time, so staging always
    // overlaps callbacks. 8000009 loop frames share no factor with the
    // 64-frame callbacks: a callback starts at a given loop phase only once
    // every 8000009 callbacks, far beyond the two-second budget below.
    talas::runtime::ProjectRuntimeBridge bridge(
        makeLoopedInputGainDocument("0.25", 8000009.0 / 8000.0), registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 8000.0;
    realtimeSettings.maxBlockFrames = 64;
    bridge.setPlaying(true);
    require(bridge.compileAndStage(realtimeSettings).ok(), "playing loop fixture should stage");
    std::atomic<bool> stop = false;
    std::atomic<std::size_t> callbacks = 0;
    std::thread audio([&] {
        std::array<float, 64> block{};
        while (!stop.load(std::memory_order_acquire))
        {
            (void)renderOnes(bridge, block);
            callbacks.fetch_add(1, std::memory_order_release);
        }
    });
    while (callbacks.load(std::memory_order_acquire) < 20)
        std::this_thread::yield();
    const auto edited = bridge.editor()
                            .setParameter("gain", expressionParameter("value", "0.5"))
                            .ok();
    // Like the Workbench's "Retry Score Change", stage again when a
    // replacement missed its frame; it must never stay pending.
    talas::runtime::PreparedGraphStatusSnapshot status;
    bool adopted = false;
    bool stuck = false;
    for (int attempt = 0; edited && attempt < 5 && !adopted && !stuck; ++attempt)
    {
        if (!bridge.compileAndStage(realtimeSettings).ok())
            continue;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            status = bridge.preparedGraphStatus();
        } while (status.awaitingActivation && std::chrono::steady_clock::now() < deadline);
        stuck = status.awaitingActivation;
        adopted = !status.awaitingActivation && !status.activationRejected
            && status.prepared != nullptr && status.activeStageId == status.prepared->stageId;
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    require(edited, "the loop fixture edit should apply");
    require(!stuck && adopted,
            "a restaged playing loop must become audible promptly instead of waiting for a "
            "callback that starts at its loop phase");
}

void testBridgeExecutesQuantizedLaunchAcrossIrregularPartitions()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"osc",
         "talas.oscillator",
         {expressionParameter("frequency", "1 hz"), expressionParameter("amplitude", "0.25")}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"},
    };
    document.timeline.musicalTime.tempo = {{0, 500000}};
    document.timeline.musicalTime.meter = {{0, 4, 4}};
    talas::project::ProjectLiveSet set;
    set.liveSetId = "quantized";
    set.displayName = "Quantized";
    set.cells.push_back({
        "nextBeatStop",
        "Next Beat Stop",
        0,
        0,
        {talas::project::ProjectLiveSetQuantizationKind::NextBeat, 0},
        talas::project::ProjectLiveSetStopTarget{},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(realtimeSettings).ok(),
            "irregular-partition Live Set fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 1> first{};
    require(renderFrames(bridge, first) && bridge.transportFramePosition() == 1,
            "fixture should adopt and advance to an off-grid frame");
    publishWorkbenchLiveSetAuthority(bridge);

    const auto activation = requestWorkbenchLiveSetActivation(bridge, "quantized", "nextBeatStop");
    require(activation.ok() && activation.resolvedFrame == 4,
            "NextBeat should freeze the exact next beat at frame four");
    std::array<float, 2> beforeBoundary{};
    require(renderFrames(bridge, beforeBoundary) && bridge.transportFramePosition() == 3 &&
                bridge.realtimeLiveSetTelemetry().completedActivations == 0,
            "an irregular callback ending before the boundary must not launch early");
    std::array<float, 3> crossingBoundary{};
    const auto crossed = renderFrames(bridge, crossingBoundary);
    const auto crossedTelemetry = bridge.realtimeLiveSetTelemetry();
    require(!crossed && bridge.transportFramePosition() == 4,
            "the next irregular callback should split exactly at the quantized "
            "frame and hold after Stop | rendered=" +
                std::to_string(crossed) +
                " frame=" + std::to_string(bridge.transportFramePosition()) +
                " completed=" + std::to_string(crossedTelemetry.completedActivations) +
                " failed=" + std::to_string(crossedTelemetry.failedActivations) +
                " stale=" + std::to_string(crossedTelemetry.staleAuthority));
    const auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 1 && telemetry.activeCellIndexPlusOne == 0 &&
                telemetry.activeRequestGeneration == 0,
            "quantized Stop should complete at the exact split boundary without "
            "leaving an active-program identity");
}

void testBridgeLaunchesImmediateRequestThatMissedItsCallback()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"osc",
         "talas.oscillator",
         {expressionParameter("frequency", "1 hz"), expressionParameter("amplitude", "0.25")}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"},
    };
    talas::project::ProjectLiveSet set;
    set.liveSetId = "immediate";
    set.displayName = "Immediate";
    set.cells.push_back({
        "immediateStop",
        "Immediate Stop",
        0,
        0,
        {talas::project::ProjectLiveSetQuantizationKind::Immediate, 0},
        talas::project::ProjectLiveSetStopTarget{},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(realtimeSettings).ok(),
            "Immediate Live Set fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> block{};
    require(renderFrames(bridge, block) && bridge.transportFramePosition() == 4,
            "Immediate fixture should adopt and advance one callback");
    publishWorkbenchLiveSetAuthority(bridge);
    const auto activation = requestWorkbenchLiveSetActivation(bridge, "immediate", "immediateStop");
    require(activation.ok()
                && talas::runtime::ProjectRuntimeBridgeTestAccess::ageQueuedLiveSetRequest(bridge, 4),
            "Immediate request should be accepted and aged behind the rendering callback");
    (void)renderFrames(bridge, block);
    const auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 1 && telemetry.staleAuthority == 0,
            "an Immediate launch that arrives during a callback must start at the next "
            "callback instead of being discarded as stale | completed="
                + std::to_string(telemetry.completedActivations)
                + " stale=" + std::to_string(telemetry.staleAuthority));
}

void testBridgeRevokesLiveSetRequestsAfterEditorDocumentChange()
{
    auto document = makePlayableDocument(1.0);
    talas::project::ProjectLiveSet set;
    set.liveSetId = "documentBound";
    set.displayName = "Document Bound";
    set.cells.push_back({
        "nextBeatStop",
        "Next Beat Stop",
        0,
        0,
        {talas::project::ProjectLiveSetQuantizationKind::NextBeat, 0},
        talas::project::ProjectLiveSetStopTarget{},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(realtimeSettings).ok(),
            "document-bound Live Set fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 1> first{};
    require(renderFrames(bridge, first) && bridge.transportFramePosition() == 1,
            "document-bound fixture should adopt its exact editor stage");
    publishWorkbenchLiveSetAuthority(bridge);
    const auto queued =
        requestWorkbenchLiveSetActivation(bridge, "documentBound", "nextBeatStop");
    require(queued.ok() && queued.resolvedFrame == 4,
            "future activation should freeze the accepted editor document authority");

    require(bridge.editor()
                .setParameter("osc", expressionParameter("frequency", "2 hz"))
                .ok(),
            "fixture should commit a newer editor document before execution");
    require(requestWorkbenchLiveSetActivation(bridge, "documentBound", "nextBeatStop").outcome ==
                talas::runtime::LiveSetActivationOutcome::StaleAuthority,
            "new requests must reject until the changed editor document is staged");
    std::array<float, 4> crossingOldBoundary{};
    require(renderFrames(bridge, crossingOldBoundary) &&
                bridge.transportFramePosition() == 5,
            "the previously active graph should continue while its stale Live Set request is revoked");
    const auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 0 && telemetry.staleAuthority >= 1 &&
                telemetry.activeCellIndexPlusOne == 0,
            "queued prior-document activation must be discarded before Stop side effects");
}

void testBridgeRevokesMappedLiveSetRequestsOnRouteReplacement()
{
    auto document = makePlayableDocument(110.0);
    talas::project::ProjectLiveSet set;
    set.liveSetId = "mapped";
    set.displayName = "Mapped";
    set.cells.push_back({
        "stop",
        "Stop",
        0,
        0,
        {},
        talas::project::ProjectLiveSetStopTarget{},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(settings()).ok(),
            "mapped Live Set authority fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output), "mapped Live Set fixture should adopt its prepared stage");

    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::MidiProgram, 11, 17);
    require(bridge.requestLiveSetActivation(
                       "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::Workbench, 0, 0)
                    .outcome == talas::runtime::LiveSetActivationOutcome::StaleAuthority
                && bridge.requestLiveSetActivation(
                       "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::Offline, 0, 0)
                    .outcome == talas::runtime::LiveSetActivationOutcome::StaleAuthority,
            "Workbench and Offline activation must not use zero-generation wildcard authority");
    require(bridge.requestLiveSetActivation(
                       "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::Tool, 0, 0)
                    .outcome == talas::runtime::LiveSetActivationOutcome::StaleAuthority,
            "tool activation must carry an exact tool and command-route generation");
    require(bridge.requestLiveSetActivation(
                      "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::MidiProgram, 11, 16)
                    .outcome == talas::runtime::LiveSetActivationOutcome::StaleAuthority,
            "mapped activation must reject a mismatched route generation");
    const auto queued = bridge.requestLiveSetActivation(
        "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::MidiProgram, 11, 17);
    require(queued.ok(), "matching source and route should admit one exact request");
    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::MidiProgram, 11, 19);
    static_cast<void>(render(bridge, output));
    auto telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.acceptedRequests == 1 && telemetry.completedActivations == 0 &&
                telemetry.staleAuthority >= 1,
            "same-source route replacement must revoke the queued prior-route "
            "request before Stop side effects");

    const auto fresh = bridge.requestLiveSetActivation(
        "mapped", "stop", talas::runtime::RealtimeLiveSetSourceKind::MidiProgram, 11, 19);
    require(fresh.ok(), "fresh route generation should remain admissible after replacement");
    static_cast<void>(render(bridge, output));
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.completedActivations == 1 && telemetry.activeCellIndexPlusOne == 0 &&
                telemetry.activeRequestGeneration == 0,
            "fresh mapped Stop should execute under the coherent replacement "
            "authority without becoming an active program");

    require(bridge.compileAndStage(settings()).ok(),
            "replacement stage should prepare after the mapped activation");
    static_cast<void>(render(bridge, output));
    telemetry = bridge.realtimeLiveSetTelemetry();
    require(telemetry.activeCellIndexPlusOne == 0 && telemetry.activeRequestGeneration == 0,
            "stage adoption must revoke the prior stage's active Live Set identity");
}

void testBridgePublishesPreparedLiveSetOwnershipWithoutDataRace()
{
    auto document = makePlayableDocument(55.0);
    talas::project::ProjectLiveSet set;
    set.liveSetId = "concurrent";
    set.displayName = "Concurrent";
    set.cells.push_back({
        "stop", "Stop", 0, 0, {}, talas::project::ProjectLiveSetStopTarget{}, {}});
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(settings()).ok(),
            "concurrent publication fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output),
            "concurrent publication fixture should adopt its first stage");
    publishWorkbenchLiveSetAuthority(bridge);

    std::atomic<bool> start = false;
    std::atomic<bool> stop = false;
    std::atomic<bool> invalid = false;
    std::atomic<std::size_t> observations = 0;
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        while (!stop.load(std::memory_order_acquire))
        {
            const auto prepared = bridge.latestPreparedLiveSet();
            if (prepared != nullptr)
            {
                if (prepared->cells().size() != 1
                    || prepared->findCell("concurrent", "stop")
                        != std::optional<std::size_t>{0})
                {
                    invalid.store(true, std::memory_order_relaxed);
                    break;
                }
                observations.fetch_add(1, std::memory_order_relaxed);
            }
            const auto result = requestWorkbenchLiveSetActivation(
                bridge, "concurrent", "stop");
            if (result.outcome
                    != talas::runtime::LiveSetActivationOutcome::Accepted
                && result.outcome
                    != talas::runtime::LiveSetActivationOutcome::StaleAuthority
                && result.outcome
                    != talas::runtime::LiveSetActivationOutcome::QueueFull
                && result.outcome
                    != talas::runtime::LiveSetActivationOutcome::NotPrepared)
            {
                invalid.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });
    start.store(true, std::memory_order_release);
    for (std::size_t iteration = 0; iteration < 20; ++iteration)
    {
        require(bridge.compileAndStage(settings()).ok(),
                "concurrent replacement stage should remain publishable");
        bridge.setPlaying(true);
        static_cast<void>(render(bridge, output));
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    require(observations.load(std::memory_order_relaxed) != 0
                && !invalid.load(std::memory_order_relaxed),
            "concurrent source readers must retain one complete prepared Live Set owner across replacement publication");
}

void testBridgeRoutesRealtimeControlsToActiveSoundProgram()
{
    auto document = makePlayableDocument(1.0, "0");
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "controlledVoice";
    sound.displayName = "Controlled Voice";
    sound.graph.nodes = {
        {"tone",
         "talas.oscillator",
         {expressionParameter("frequency", "1 hz"), expressionParameter("amplitude", "!Gain")}},
    };
    sound.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "tone",
        "audioOut",
    });
    document.soundDefinitions.push_back(std::move(sound));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "controls";
    set.displayName = "Controls";
    set.cells.push_back({
        "voice",
        "Voice",
        0,
        0,
        {},
        talas::project::ProjectLiveSetSoundTarget{"controlledVoice", {}},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    auto realtimeSettings = settings();
    realtimeSettings.eventValues.push_back({"Gain", 0.0, talas::expr::ValueDomain::Scalar});
    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    require(bridge.compileAndStage(realtimeSettings).ok(),
            "controlled Sound Live Set fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    require(render(bridge, output) && !hasAudibleSample(output),
            "controlled Sound fixture should begin on the silent main program");
    publishWorkbenchLiveSetAuthority(bridge);
    require(requestWorkbenchLiveSetActivation(bridge, "controls", "voice").ok() && render(bridge, output) &&
                !hasAudibleSample(output),
            "Sound program should activate with its exact initial Event Value");
    const auto gainId =
        bridge.realtimeEventValueIdForLatestStage("Gain", talas::expr::ValueDomain::Scalar);
    require(gainId.has_value() &&
                bridge.enqueueEventValue(*gainId, 1.0, talas::expr::ValueDomain::Scalar) &&
                render(bridge, output) && hasAudibleSample(output),
            "accepted Event Value commands must resolve and execute in the "
            "active Sound program rather than the inaudible main graph");
}

void testBridgeSharesNamedControlsAcrossParallelGridSounds()
{
    auto document = makePlayableDocument(1.0, "0");
    const auto addSound = [&](std::string id, int frequency,
                              std::string localControl) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone", "talas.oscillator",
            {expressionParameter(
                 "frequency", std::to_string(frequency) + " hz"),
             expressionParameter(
                 "amplitude", "!Shared * !" + localControl)},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    };
    addSound("soundA", 220, "LocalA");
    addSound("soundB", 330, "LocalB");
    talas::project::ProjectLiveSet set;
    set.liveSetId = "controls";
    set.displayName = "Controls";
    set.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"soundA", {}}, {}},
        {"b", "B", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{"soundB", {}}, {}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 48'000.0;
    realtimeSettings.eventValues.push_back({
        "Shared", 0.0, talas::expr::ValueDomain::Scalar});
    realtimeSettings.eventValues.push_back({
        "LocalA", 1.0, talas::expr::ValueDomain::Scalar});
    realtimeSettings.eventValues.push_back({
        "LocalB", 1.0, talas::expr::ValueDomain::Scalar});
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceA(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceB(document, registry());
    for (auto* bridge : {&mixed, &referenceA, &referenceB})
    {
        require(bridge->compileAndStage(realtimeSettings).ok(),
                "parallel named-control fixture should stage");
        bridge->setPlaying(true);
        std::array<float, 4> output{};
        require(render(*bridge, output) && !hasAudibleSample(output),
                "parallel named-control fixture should adopt silently");
        publishWorkbenchLiveSetAuthority(*bridge);
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> outputA{};
    std::array<float, 4> outputB{};
    const auto renderAll = [&] {
        const auto mixedRendered = render(mixed, mixedOutput);
        const auto aRendered = render(referenceA, outputA);
        const auto bRendered = render(referenceB, outputB);
        return mixedRendered && aRendered && bRendered;
    };
    std::size_t callbackAllocations = 0;
    const auto renderAllAndCount = [&] {
        const auto mixedRendered = renderAndCount(
            mixed, mixedOutput, callbackAllocations);
        const auto aRendered = render(referenceA, outputA);
        const auto bRendered = render(referenceB, outputB);
        return mixedRendered && aRendered && bRendered
            && callbackAllocations == 0;
    };
    const auto enqueue = [](auto& bridge, std::string_view name,
                            double value, std::size_t offset = 0) {
        const auto id = bridge.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        return id.has_value() && bridge.enqueueEventValue(
            *id, value, talas::expr::ValueDomain::Scalar, offset);
    };
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "a").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceA, "controls", "a").ok()
                && renderAll(),
            "first controlled track should activate");
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceB, "controls", "b").ok()
                && renderAll() && !hasAudibleSample(mixedOutput),
            "both tracks should begin with Shared at zero");
    require(enqueue(mixed, "Shared", 1.0)
                && enqueue(referenceA, "Shared", 1.0)
                && enqueue(referenceB, "Shared", 1.0)
                && renderAllAndCount() && hasAudibleSample(outputA)
                && hasAudibleSample(outputB),
            "same-named control should reach both active Sound programs");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - outputA[frame] - outputB[frame]) < 1.0e-5F,
                "Shared must control both active tracks, even when B launched last");

    require(enqueue(mixed, "LocalA", 0.0, 2)
                && enqueue(referenceA, "LocalA", 0.0, 2)
                && renderAllAndCount(),
            "a uniquely named control should reach the earlier active track");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - outputA[frame] - outputB[frame]) < 1.0e-5F,
                "LocalA offset must affect only A at the matching sample");
    require(renderAll() && !hasAudibleSample(outputA)
                && hasAudibleSample(outputB),
            "LocalA should leave B running after A becomes silent");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - outputB[frame])
                    < 1.0e-5F,
                "one uniquely named control must not alter the other track");

    require(enqueue(mixed, "LocalA", 1.0)
                && enqueue(referenceA, "LocalA", 1.0)
                && enqueue(mixed, "LocalB", 0.0)
                && enqueue(referenceB, "LocalB", 0.0)
                && renderAllAndCount() && hasAudibleSample(outputA)
                && !hasAudibleSample(outputB),
            "unique controls should remain independent in both directions");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - outputA[frame])
                    < 1.0e-5F,
                "LocalB must affect only B while A continues");
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(referenceA, outputA)
                && render(referenceB, outputB)
                && callbackAllocations == 0,
            "parallel named-control dispatch should avoid callback allocations");
}

void testBridgeCarriesNamedControlToLaterGridSound()
{
    auto document = makePlayableDocument(1.0, "0");
    const auto addSound = [&](std::string id, int frequency,
                              std::string localControl) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone", "talas.oscillator",
            {expressionParameter("frequency", std::to_string(frequency) + " hz"),
             expressionParameter("amplitude", "!Shared * !" + localControl)},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    };
    addSound("soundA", 220, "LocalA");
    addSound("soundB", 330, "LocalB");
    talas::project::ProjectLiveSet set;
    set.liveSetId = "controls";
    set.displayName = "Controls";
    set.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"soundA", {}}, {}},
        {"b", "B", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{"soundB", {}}, {}},
        {"stop", "Stop", 0, 2, {},
         talas::project::ProjectLiveSetStopTarget{}, {}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    auto initial = settings();
    initial.sampleRate = 48'000.0;
    initial.eventValues.push_back({
        "Shared", 0.0, talas::expr::ValueDomain::Scalar});
    initial.eventValues.push_back({
        "LocalA", 1.0, talas::expr::ValueDomain::Scalar});
    initial.eventValues.push_back({
        "LocalB", 1.0, talas::expr::ValueDomain::Scalar});
    auto laterReference = initial;
    laterReference.eventValues.front().value = 1.0;
    auto afterStopReference = laterReference;
    afterStopReference.eventValues.back().value = 0.5;
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceA(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceB(document, registry());
    talas::runtime::ProjectRuntimeBridge referenceAfterStop(document, registry());
    for (auto* bridge : {&mixed, &referenceA, &referenceB,
                         &referenceAfterStop})
    {
        require(bridge->compileAndStage(
                    bridge == &referenceAfterStop ? afterStopReference
                    : bridge == &referenceB ? laterReference
                    : initial).ok(),
                "late named-control fixture should stage");
        bridge->setPlaying(true);
        std::array<float, 4> output{};
        require(render(*bridge, output) && !hasAudibleSample(output),
                "late named-control fixture should adopt silently");
        publishWorkbenchLiveSetAuthority(*bridge);
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> outputA{};
    std::array<float, 4> outputB{};
    std::array<float, 4> outputAfterStop{};
    const auto enqueue = [](auto& bridge, std::string_view name,
                            double value, std::size_t offset = 0) {
        const auto id = bridge.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        return id.has_value() && bridge.enqueueEventValue(
            *id, value, talas::expr::ValueDomain::Scalar, offset);
    };
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "a").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceA, "controls", "a").ok()
                && render(mixed, mixedOutput)
                && render(referenceA, outputA)
                && render(referenceAfterStop, outputAfterStop),
            "first Sound should activate");
    require(enqueue(mixed, "Shared", 1.0)
                && enqueue(referenceA, "Shared", 1.0)
                && render(mixed, mixedOutput)
                && render(referenceA, outputA)
                && render(referenceAfterStop, outputAfterStop)
                && hasAudibleSample(outputA),
            "the first Sound should receive the changed shared value");
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceB, "controls", "b").ok(),
            "second Sound should queue after the shared control changes");
    std::size_t callbackAllocations = 0;
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(referenceA, outputA)
                && render(referenceB, outputB)
                && render(referenceAfterStop, outputAfterStop)
                && callbackAllocations == 0,
            "later Sound activation should avoid callback allocations");
    require(hasAudibleSample(outputB),
            "the independent later Sound should start with its shared value");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame]
                         - outputA[frame] - outputB[frame]) < 1.0e-5F,
                "later Sound should inherit the previously changed shared value");
    require(enqueue(mixed, "LocalB", 0.5)
                && render(mixed, mixedOutput)
                && render(referenceAfterStop, outputAfterStop),
            "second global value should change before Stop");
    require(requestWorkbenchLiveSetActivation(
                mixed, "controls", "stop").ok(),
            "Stop should queue after the shared control changes");
    (void)renderAndCount(mixed, mixedOutput, callbackAllocations);
    require(render(referenceAfterStop, outputAfterStop),
            "idle reference should continue through Stop");
    require(callbackAllocations == 0,
            "Stop should avoid callback allocations");
    mixed.setPlaying(true);
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceAfterStop, "controls", "b").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(referenceAfterStop, outputAfterStop)
                && callbackAllocations == 0,
            "Grid should restart after Stop without callback allocations");
    require(hasAudibleSample(outputAfterStop),
            "independent restarted Sound should use the retained shared value");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - outputAfterStop[frame])
                    < 1.0e-5F,
                "Grid Stop must retain Shared for a later Sound launch");
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceAfterStop, "controls", "b").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(referenceAfterStop, outputAfterStop)
                && callbackAllocations == 0,
            "relaunching one active Sound should avoid callback allocations");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - outputAfterStop[frame])
                    < 1.0e-5F,
                "relaunching one Sound should retain its shared controls");
    require(requestWorkbenchLiveSetActivation(
                mixed, "controls", "stop").ok(),
            "second Stop should queue before idle control updates");
    (void)renderAndCount(mixed, mixedOutput, callbackAllocations);
    require(callbackAllocations == 0,
            "second Stop should avoid callback allocations");
    mixed.setPlaying(true);
    require(enqueue(mixed, "Shared", 0.0, 2)
                && enqueue(mixed, "LocalB", 1.0)
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && callbackAllocations == 0,
            "idle controls should accept root-baseline values");
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && callbackAllocations == 0
                && mixed.realtimeLiveSetTelemetry()
                       .activeCellIndexPlusOne == 2,
            "Sound B should launch after idle baseline controls");
    require(!hasAudibleSample(mixedOutput),
            "explicit idle Shared zero should override the older retained one");
    require(requestWorkbenchLiveSetActivation(
                mixed, "controls", "stop").ok(),
            "third Stop should queue before nonzero idle controls");
    (void)renderAndCount(mixed, mixedOutput, callbackAllocations);
    require(callbackAllocations == 0,
            "third Stop should avoid callback allocations");
    mixed.setPlaying(true);
    require(enqueue(mixed, "Shared", 0.25, 2)
                && enqueue(mixed, "LocalB", 0.75)
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && callbackAllocations == 0,
            "controls should apply to the root while Grid Sounds are inactive");
    auto idleReferenceSettings = afterStopReference;
    idleReferenceSettings.eventValues.front().value = 0.25;
    idleReferenceSettings.eventValues.back().value = 0.75;
    talas::runtime::ProjectRuntimeBridge referenceIdle(document, registry());
    require(referenceIdle.compileAndStage(idleReferenceSettings).ok(),
            "idle-control reference should stage");
    referenceIdle.setPlaying(true);
    std::array<float, 4> idleOutput{};
    require(render(referenceIdle, idleOutput)
                && !hasAudibleSample(idleOutput),
            "idle-control reference should adopt silently");
    publishWorkbenchLiveSetAuthority(referenceIdle);
    require(requestWorkbenchLiveSetActivation(mixed, "controls", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    referenceIdle, "controls", "b").ok()
                && renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(referenceIdle, idleOutput)
                && callbackAllocations == 0,
            "later Sound should start after idle controls without allocations");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - idleOutput[frame]) < 1.0e-5F,
                "idle control updates should override the retained Stop values");
}

void testBridgeRejectsSameSoundRestartWithoutAlteringPlayback()
{
    auto document = makePlayableDocument(1.0, "0");
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "controlled";
    sound.displayName = "Controlled";
    sound.graph.nodes.push_back({
        "tone", "talas.oscillator",
        {expressionParameter("frequency", "330 hz"),
         expressionParameter("amplitude", "!Shared * !LocalB")},
    });
    sound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    document.soundDefinitions.push_back(std::move(sound));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "bounded";
    set.displayName = "Bounded";
    set.cells.push_back({
        "b", "B", 0, 0, {},
        talas::project::ProjectLiveSetSoundTarget{"controlled", {}}, {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    auto limited = settings();
    limited.sampleRate = 48'000.0;
    limited.maxRealtimeEventTransactions = 1;
    limited.eventValues.push_back({
        "Shared", 0.0, talas::expr::ValueDomain::Scalar});
    limited.eventValues.push_back({
        "LocalB", 1.0, talas::expr::ValueDomain::Scalar});
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    for (auto* bridge : {&mixed, &reference})
    {
        require(bridge->compileAndStage(limited).ok(),
                "bounded restart fixture should stage");
        bridge->setPlaying(true);
        std::array<float, 4> output{};
        require(render(*bridge, output) && !hasAudibleSample(output),
                "bounded restart fixture should adopt silently");
        publishWorkbenchLiveSetAuthority(*bridge);
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> referenceOutput{};
    const auto renderBoth = [&] {
        return render(mixed, mixedOutput)
            && render(reference, referenceOutput);
    };
    const auto enqueueBoth = [&](std::string_view name, double value) {
        const auto mixedId = mixed.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        const auto referenceId = reference.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        return mixedId.has_value() && referenceId.has_value()
            && mixed.enqueueEventValue(
                *mixedId, value, talas::expr::ValueDomain::Scalar)
            && reference.enqueueEventValue(
                *referenceId, value, talas::expr::ValueDomain::Scalar);
    };
    require(requestWorkbenchLiveSetActivation(mixed, "bounded", "b").ok()
                && requestWorkbenchLiveSetActivation(
                    reference, "bounded", "b").ok()
                && renderBoth(),
            "controlled Sound should activate in both renders");
    require(enqueueBoth("Shared", 1.0) && renderBoth()
                && enqueueBoth("LocalB", 0.5) && renderBoth()
                && hasAudibleSample(referenceOutput),
            "both controls should reach the active Sound in separate callbacks");
    require(requestWorkbenchLiveSetActivation(mixed, "bounded", "b").ok(),
            "same-cell relaunch should queue for capacity preflight");
    std::size_t callbackAllocations = 0;
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(reference, referenceOutput)
                && callbackAllocations == 0,
            "rejected relaunch should keep rendering without allocation");
    const auto telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.failedActivations == 1
                && telemetry.activeCellIndexPlusOne == 1,
            "rejected relaunch should report failure and keep its active cell");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - referenceOutput[frame])
                    < 1.0e-5F,
                "insufficient control queue capacity must preserve active playback");
}

void testBridgeRejectsCrossSetLaunchWithoutStoppingActiveSound()
{
    auto document = makePlayableDocument(1.0, "0");
    for (const auto& [id, frequency] : {
             std::pair{"first", "220 hz"},
             std::pair{"second", "330 hz"}})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({
            "tone", "talas.oscillator",
            {expressionParameter("frequency", frequency),
             expressionParameter("amplitude", "!Shared * !LocalB")},
        });
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    }
    for (const auto& [setId, cellId, soundId] : {
             std::tuple{"setA", "a", "first"},
             std::tuple{"setB", "b", "second"}})
    {
        talas::project::ProjectLiveSet set;
        set.liveSetId = setId;
        set.displayName = setId;
        set.cells.push_back({
            cellId, cellId, 0, 0, {},
            talas::project::ProjectLiveSetSoundTarget{soundId, {}}, {},
        });
        document.liveSetLibrary.liveSets.push_back(std::move(set));
    }
    auto limited = settings();
    limited.sampleRate = 48'000.0;
    limited.maxRealtimeEventTransactions = 1;
    limited.eventValues.push_back({
        "Shared", 0.0, talas::expr::ValueDomain::Scalar});
    limited.eventValues.push_back({
        "LocalB", 1.0, talas::expr::ValueDomain::Scalar});
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    for (auto* bridge : {&mixed, &reference})
    {
        require(bridge->compileAndStage(limited).ok(),
                "cross-set capacity fixture should stage");
        bridge->setPlaying(true);
        std::array<float, 4> output{};
        require(render(*bridge, output) && !hasAudibleSample(output),
                "cross-set capacity fixture should adopt silently");
        publishWorkbenchLiveSetAuthority(*bridge);
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> referenceOutput{};
    const auto renderBoth = [&] {
        return render(mixed, mixedOutput)
            && render(reference, referenceOutput);
    };
    const auto enqueueBoth = [&](std::string_view name, double value) {
        const auto mixedId = mixed.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        const auto referenceId = reference.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        return mixedId.has_value() && referenceId.has_value()
            && mixed.enqueueEventValue(
                *mixedId, value, talas::expr::ValueDomain::Scalar)
            && reference.enqueueEventValue(
                *referenceId, value, talas::expr::ValueDomain::Scalar);
    };
    require(requestWorkbenchLiveSetActivation(mixed, "setA", "a").ok()
                && requestWorkbenchLiveSetActivation(
                    reference, "setA", "a").ok()
                && renderBoth(),
            "first Live Set Sound should activate in both renders");
    require(enqueueBoth("Shared", 1.0) && renderBoth()
                && enqueueBoth("LocalB", 0.5) && renderBoth()
                && hasAudibleSample(referenceOutput),
            "both controls should reach the active Sound separately");
    require(requestWorkbenchLiveSetActivation(mixed, "setB", "b").ok(),
            "cross-set launch should queue for capacity admission");
    std::size_t callbackAllocations = 0;
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(reference, referenceOutput)
                && callbackAllocations == 0,
            "rejected cross-set launch should keep rendering without allocation");
    const auto telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.failedActivations == 1
                && telemetry.activeCellIndexPlusOne == 1,
            "rejected cross-set launch should retain the first cell identity");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - referenceOutput[frame])
                    < 1.0e-5F,
                "rejected cross-set launch must preserve active PCM and phase");
}

void testBridgeRejectsFirstRoutedSoundWithoutStoppingOverlay()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {expressionParameter("gain", "0")}},
        {"osc", "talas.oscillator",
         {expressionParameter("frequency", "220 hz"),
          expressionParameter("amplitude", "0.2")}},
        {"effectInput", "talas.audioInput", {}},
        {"effectInstance", "", {}, std::string{"halfEffect"}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "out", "audioIn"},
        {"effectInput", "audioOut", "effectInstance", "audioIn"},
    };
    talas::project::ProjectSoundDefinition controlled;
    controlled.definitionId = "controlled";
    controlled.displayName = "Controlled";
    controlled.graph.nodes.push_back({
        "tone", "talas.oscillator",
        {expressionParameter("frequency", "330 hz"),
         expressionParameter("amplitude", "!Shared * !LocalB")},
    });
    controlled.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    document.soundDefinitions.push_back(std::move(controlled));
    talas::project::ProjectSoundDefinition effect;
    effect.definitionId = "halfEffect";
    effect.displayName = "Half Effect";
    effect.graph.nodes = {
        {"amp", "talas.vca", {}},
        {"half", "talas.constant", {expressionParameter("value", "0.5")}},
    };
    effect.graph.connections.push_back({"half", "value", "amp", "gain"});
    effect.inputs.push_back({
        "audioIn", talas::core::PortType::Audio, "amp", "audioIn"});
    effect.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "amp", "audioOut"});
    document.soundDefinitions.push_back(std::move(effect));
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotValue loudValue;
    loudValue.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "overlayBank";
    bank.displayName = "Overlay Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.parameterPolicies.push_back({
        gainTarget, false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots.push_back({"loud", "Loud", {{gainTarget, loudValue}}});
    document.snapshotLibrary.banks.push_back(std::move(bank));
    talas::project::ProjectLiveSet set;
    set.liveSetId = "routed";
    set.displayName = "Routed";
    set.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"controlled", {}}, {}},
        {"b", "B", 1, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"controlled", {}}, {}},
        {"effect", "Effect", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{
             "halfEffect", std::string{"effectInstance"}}, {}},
        {"stop", "Stop", 0, 2, {},
         talas::project::ProjectLiveSetStopTarget{}, {}},
        {"overlay", "Overlay", 0, 3, {},
         talas::project::ProjectLiveSetSnapshotTarget{
             "overlayBank", "loud"}, {}},
    };
    set.routes.push_back({0, 1});
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    auto limited = settings();
    limited.sampleRate = 48'000.0;
    limited.maxRealtimeEventTransactions = 1;
    limited.eventValues.push_back({
        "Shared", 0.0, talas::expr::ValueDomain::Scalar});
    limited.eventValues.push_back({
        "LocalB", 1.0, talas::expr::ValueDomain::Scalar});
    talas::runtime::ProjectRuntimeBridge mixed(document, registry());
    talas::runtime::ProjectRuntimeBridge reference(document, registry());
    for (auto* bridge : {&mixed, &reference})
    {
        const auto staged = bridge->compileAndStage(limited);
        require(staged.ok(),
                "routed overlay capacity fixture should stage: "
                    + staged.message);
        bridge->setPlaying(true);
        std::array<float, 4> output{};
        require(render(*bridge, output) && !hasAudibleSample(output),
                "routed overlay fixture should adopt silently");
        publishWorkbenchLiveSetAuthority(*bridge);
    }
    std::array<float, 4> mixedOutput{};
    std::array<float, 4> referenceOutput{};
    const auto renderBoth = [&] {
        return render(mixed, mixedOutput)
            && render(reference, referenceOutput);
    };
    const auto enqueueBoth = [&](std::string_view name, double value) {
        const auto mixedId = mixed.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        const auto referenceId = reference.realtimeEventValueIdForLatestStage(
            name, talas::expr::ValueDomain::Scalar);
        return mixedId.has_value() && referenceId.has_value()
            && mixed.enqueueEventValue(
                *mixedId, value, talas::expr::ValueDomain::Scalar)
            && reference.enqueueEventValue(
                *referenceId, value, talas::expr::ValueDomain::Scalar);
    };
    require(requestWorkbenchLiveSetActivation(mixed, "routed", "a").ok()
                && requestWorkbenchLiveSetActivation(
                    reference, "routed", "a").ok()
                && renderBoth(),
            "first routed Sound should activate in both renders");
    require(enqueueBoth("Shared", 1.0) && renderBoth()
                && enqueueBoth("LocalB", 0.5) && renderBoth(),
            "two controls should reach the routed Sound separately");
    require(requestWorkbenchLiveSetActivation(mixed, "routed", "stop").ok()
                && requestWorkbenchLiveSetActivation(
                    reference, "routed", "stop").ok(),
            "Stop should retain both routed Sound controls");
    static_cast<void>(render(mixed, mixedOutput));
    static_cast<void>(render(reference, referenceOutput));
    for (auto* bridge : {&mixed, &reference})
        bridge->setPlaying(true);
    require(requestWorkbenchLiveSetActivation(
                mixed, "routed", "overlay").ok()
                && requestWorkbenchLiveSetActivation(
                    reference, "routed", "overlay").ok()
                && renderBoth()
                && hasAudibleSample(referenceOutput),
            "overlay Snapshot should become the audible prior program");
    require(requestWorkbenchLiveSetActivation(mixed, "routed", "b").ok(),
            "routed Sound should queue for capacity admission");
    std::size_t callbackAllocations = 0;
    require(renderAndCount(mixed, mixedOutput, callbackAllocations)
                && render(reference, referenceOutput)
                && callbackAllocations == 0,
            "rejected routed launch should keep rendering without allocation");
    const auto telemetry = mixed.realtimeLiveSetTelemetry();
    require(telemetry.failedActivations == 1
                && telemetry.activeCellIndexPlusOne == 5,
            "rejected routed launch should retain overlay identity");
    for (std::size_t frame = 0; frame < mixedOutput.size(); ++frame)
        require(std::abs(mixedOutput[frame] - referenceOutput[frame])
                    < 1.0e-5F,
                "rejected routed launch must preserve overlay PCM and phase");
}

void testBridgeRevokesActiveScoreRegionOnExternalSeek()
{
    auto document = makePlayableDocument(1.0);
    document.timeline.score.tracks.push_back({
        "master",
        "Master",
        talas::project::ScoreTrackKind::Master,
        talas::project::ScoreTimeDomain::Seconds,
        {},
        1,
        talas::project::ScoreOverlapMode::Layer,
    });
    document.timeline.score.markers.push_back({
        "region",
        "Region",
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
        talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, 2.0, 0},
    });
    talas::project::ProjectLiveSet set;
    set.liveSetId = "scoreSeek";
    set.displayName = "Score Seek";
    set.cells.push_back({
        "region",
        "Region",
        0,
        0,
        {},
        talas::project::ProjectLiveSetScoreRegionTarget{"region"},
        {},
    });
    document.liveSetLibrary.liveSets.push_back(std::move(set));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry());
    auto realtimeSettings = settings();
    realtimeSettings.sampleRate = 8.0;
    require(bridge.compileAndStage(realtimeSettings).ok(),
            "Score-region seek fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 1> oneFrame{};
    require(renderFrames(bridge, oneFrame), "Score-region seek fixture should adopt its stage");
    publishWorkbenchLiveSetAuthority(bridge);
    require(requestWorkbenchLiveSetActivation(bridge, "scoreSeek", "region").ok(),
            "Score-region activation should be admitted");
    std::array<float, 2> twoFrames{};
    require(renderFrames(bridge, twoFrames) && bridge.transportFramePosition() == 10 &&
                bridge.realtimeLiveSetTelemetry().scoreRegionActive,
            "Score region should relocate to frame eight and begin its bounded "
            "range");

    require(bridge.requestTransportRelocation(0),
            "external seek should publish one new transport discontinuity");
    require(renderFrames(bridge, oneFrame) && bridge.transportFramePosition() == 1 &&
                !bridge.realtimeLiveSetTelemetry().scoreRegionActive &&
                bridge.realtimeLiveSetTelemetry().activeCellIndexPlusOne == 0 &&
                bridge.realtimeLiveSetTelemetry().activeRequestGeneration == 0,
            "external seek must revoke the active bounded Score region without "
            "reusing its old end frame or identity");

    require(requestWorkbenchLiveSetActivation(bridge, "scoreSeek", "region").ok(),
            "Score region should remain reusable after an external seek");
    std::array<float, 4> firstRegionHalf{};
    require(renderFrames(bridge, firstRegionHalf) &&
                bridge.transportFramePosition() == 12 &&
                bridge.realtimeLiveSetTelemetry().scoreRegionActive,
            "Score region should remain active before its exact end frame");
    std::array<float, 4> secondRegionHalf{};
    const auto renderedRegion = renderFrames(bridge, secondRegionHalf);
    const auto completedRegion = bridge.realtimeLiveSetTelemetry();
    require(renderedRegion && bridge.transportFramePosition() == 16 &&
                !completedRegion.scoreRegionActive &&
                completedRegion.activeCellIndexPlusOne == 0 &&
                completedRegion.activeRequestGeneration == 0,
            "the callback rendering the exact Score-region end must also clear "
            "its active identity | rendered=" + std::to_string(renderedRegion) +
                " frame=" + std::to_string(bridge.transportFramePosition()) +
                " active=" + std::to_string(completedRegion.scoreRegionActive) +
                " cell=" + std::to_string(completedRegion.activeCellIndexPlusOne) +
                " request=" + std::to_string(completedRegion.activeRequestGeneration));
}
}

int main()
{
    testBridgeCompilesStagesAndRendersEditableDocument();
    testCompileAndLoadWaitsForRestart();
    testCheckedBiquadExampleRendersThroughOrderedPreparedBridge();
    testBridgeRendersTerminalMidiWithExactRestageIdentity();
    testBridgeStagesDocumentOnBoundedWorkerStack();
    testBridgeRejectsBeforePreparedIdentityWraparound();
    testBridgeResolvesStableVariableIdentityAndRejectsStaleStage();
    testBridgeSchedulesWorkspaceVariableBatchAtomically();
    testBridgeDefersControlDrainWhileControlThreadProduces();
    testBridgeSerializesConcurrentSessionProducers();
    testBridgeSchedulesAndTransfersExactSoundSelections();
    testBridgeTreatsEmptySoundCollectionResetAsExactNoOp();
    testArbitraryDocumentStageCannotInheritEditorVariableHistory();
    testBridgeBindsVoiceVariablesToExactSlotGeneration();
    testBridgeMetricsUseNonDestructiveEpochBaselines();
    testFailedCompilePreservesActiveGraph();
    testResourceHistoryAllowsOnlyTableBinaryCrossKindChanges();
    testFailedIsolatedDocumentStagePreservesActiveGraphAndEditorDocument();
    testMissingComponentStageFailsBeforeReplacementAndPreservesActiveGraph();
    testPendingStageDoesNotReplaceFirstPreparedGraph();
    testRejectedStageDoesNotPublishRealtimeEventIds();
    testRegistryConflictDoesNotPartiallyPublishIds();
    testLatestStageEventLookupExcludesHistoricalAndRejectedBindings();
    testRetiredEngineIsCollectedExplicitly();
    testParameterTransformerStageReplacementAndRejectedCandidateAreAtomic();
    testC44DynamicUtilitiesRestageAndRejectedReplacementAreAtomic();
    testC45DynamicSlewRestageAndRejectedReplacementAreAtomic();
    testC46DynamicClockRestageAndRejectedReplacementAreAtomic();
    testC47DynamicClockDividerRestageAndRejectedReplacementAreAtomic();
    testC49LiveChannellerRestageAndRejectedReplacementAreAtomic();
    testC50LiveSpectrumModifierRestageAndRejectedReplacementAreAtomic();
    testC60SpectrumOnDiskDefaultsRestageAndRejectReplacementAtomically();
    testC61SumOfSinesDefaultsRestageAndRejectReplacementAtomically();
    testC63SumOfSinesAutomaticLoopRestageAndRejectReplacementAtomically();
    testC58SpectrumInRamRestageAndRejectedReplacementAreAtomic();
    testC59SpectrumInRamDefaultsRestageAndRejectAtomically();
    testC57VoicedUnvoicedRestageAndRejectedReplacementAreAtomic();
    testBridgeUpdatesLiveEventValuesOnActiveGraph();
    testBridgeSchedulesLiveEventValuesInsideNextCallback();
    testBridgeDrainsStableRealtimeControlCommandsWithoutAllocations();
    testBridgeControlIngressRejectsOverflowWithoutOverwriting();
    testBridgeExposesReasonSpecificTypedEventTelemetry();
    testBridgeControlIngressRetainsOffsetsAcrossShortCallbacks();
    testBridgeControlIngressTargetsPendingReplacement();
    testBridgeControlIngressSupportsConcurrentProducers();
    testBridgeMapsArrivalTimestampsIntoCurrentCallback();
    testControllerRecordingCapturesAcceptedCallbackEvidence();
    testControllerRecordingStopBeforeAdoptionPublishesNoStaleEvents();
    testControllerRecordingTakeGenerationAndStopPriority();
    testLateControllerStopCannotCrossTakeGeneration();
    testControllerRecordingPunchSeekAndSourceLossFailClosed();
    testControllerRecordingLoopAndCancelRetainAbsoluteRawEvidence();
    testBridgeSchedulesAbsoluteSteadyControlsAcrossCallbacks();
    testBridgeAbsoluteSteadySchedulingIsPartitionInvariant();
    testBridgeStageReplacementInvalidatesAbsolutePending();
    testBridgeRejectsExplicitStageCommandAfterReplacement();
    testBridgeTimestampMappingUsesPinnedGraphSampleRate();
    testBridgeLegacySchedulingReportsQueueAdmission();
    testBridgeRejectsInvalidRealtimeExpressionWithoutAllocating();
    testBridgeTimingResetRemainsSingleConsumerDuringRendering();
    testBridgeSortsFullReverseControlBatchWithoutAllocating();
    testBridgeSchedulesMonophonicMidiNotesAtExactSamples();
    testMidiNotesOffReleasesAHangingNote();
    testBridgeZeroVelocityNoteOnMatchesNoteOffAndRetainsVelocity();
    testBridgeRoutesBuiltInMidiControlChange();
    testRelocationStartsANoteWaitingForItsRetriggerDelay();
    testReprepareEventValueIsRefusedAlone();
    testBridgeBuiltInMidiControlRangeEndpoints();
    testBridgeRoutesDefaultMidiMapNames();
    testBridgeRoutesProjectGlobalMap();
    testGlobalMapOtherEventSourcesTakeNoMidi();
    testBridgeRoutesOneBasedMidiProgramNumber();
    testBridgeNormalizesMidiChannelPressure();
    testBridgeNormalizesMidiPitchBend();
    testBridgeRoutesBuiltInMidiControlThroughVoices();
    testBridgeRoutesBuiltInMidiButtonsAndSwitches();
    testBridgeRoutesMidiControlThroughLocalMapperMap();
    testBridgeRoutesDelayedKeyboardSourceThroughLocalMapperMap();
    testBridgeCombinesHeldKeyAndLivePitchBend();
    testBridgeSelfNamedLocalMapUsesVirtualControlOnly();
    testBridgeRoutesMappedMidiBeforeFallbackAndRevokesOldStage();
    testBridgeAcceptsMidiNoteBetweenStageAndFirstCallback();
    testBridgeSchedulesMidiAgainstSubsetBindingsAndRollsBackRejectedState();
    testBridgeHandlesMidiReplacementVelocityZeroAndStageIdentity();
    testBridgeRoutesPreparedMidiBeforeMonophonicFallbackWithoutAllocations();
    testBridgeRejectsPreparedMidiWithoutFallingBackOrMutatingVoiceState();
    testBridgePolyphonicMapperRollsBackAndReplacesVoiceStage();
    testBridgeRetainsSameFrameMidiIngressOrderForPreparedVoiceStealing();
    testBridgeStagesPersistedVirtualControlValues();
    testProjectRenderSettingsPreparePersistedControlsTimelineAndMidiDefaults();
    testProjectRenderSettingsSupplyDiscoveredEventValueDefaults();
    testProjectRenderSettingsDiscoverBuiltinsInDescriptorDefaults();
    testBridgeStagesReferencedBuiltinMidiEventDefaults();
    testBridgePreservesExplicitBuiltinMidiBindings();
    testBridgeRendersBorrowedAudioInputEndToEnd();
    testBridgePublishesAnalysisCaptureOnlyAfterAcceptedStage();
    testBridgeExposesAcceptedSampleRateTransportAndStopsAtZero();
    testInternalTransportRelocationAdoptsAtOneAudioBoundary();
    testStopInvalidatesPreResetRealtimeIngress();
    testBridgeStagesPersistedTimelineAtExactAbsoluteFramesWithoutAllocation();
    testGeneratedTimelineUsesExistingPreparedTypedEventScheduler();
    testPersistedTimelineFollowsVariableCallbacksPauseAndPreparedCapacity();
    testFutureTimelineCoexistsWithIndependentLiveControl();
    testMidiClockAuthorityControlsTransportAtCallbackBoundaries();
    testMtcAuthorityLocatesAcquiresAdvancesAndTimesOut();
    testMtcFollowsTheTimeCodeOffset();
    testMtcMessagesUpdateTimeEventSources();
    testLiveMidiTimingClockTriggersOncePerMessage();
    testLiveMidiTimingClockDrivesTimingClock();
    testMidiSongPositionRejectsQueuedPriorMusicalStage();
    testMaximumMusicalMapAndClockQueueRemainPrepared();
    testMusicalTransportPresentationUsesActiveExactStage();
    testFarFutureTempoOverflowDoesNotRejectEarlySongPosition();
    testMidiClockPublishesLockedExternalMusicalPhase();
    testStoppedMidiClockLockReachesTerminalStale();
    testMidiClockPhaseStateOverflowFailsLockClosed();
    testMidiClockFollowerOverflowStopsRunningTransport();
    testMidiClockFirstPendingStageCallbackUsesAcceptedProfile();
    testExternalMusicalPhaseIsPartitionInvariant();
    testBridgePublishesCompletePreparedLiveSetCells();
    testBridgeMixesIndependentLiveSetColumns();
    testBridgeRoutesLiveSetTrackThroughEffect();
    testBridgeRendersFanOutFanInAndChainedGridRoutes();
    testBridgeAppliesAutomaticLiveToolTriggers();
    testPeriodicLiveToolTicksSurviveLargeCallbacksAndLoopWraps();
    testBridgeActivatesPreparedLiveSetOverlaySnapshot();
    testOversizedCallbackDuringLiveSetRampKeepsTheRamp();
    testBridgeExecutesQuantizedLaunchAcrossIrregularPartitions();
    testBridgeAdoptsLoopRestageExactlyAtItsPreparedFrame();
    testBridgeAdoptsTerminalRestageExactlyAtItsPreparedFrame();
    testBridgeKeepsMidiNotesOnTheIncumbentWhileAReplacementWaits();
    testBridgeRestagesPlayingLoopWithoutWaitingForItsPhase();
    testBridgeLaunchesImmediateRequestThatMissedItsCallback();
    testBridgeRevokesLiveSetRequestsAfterEditorDocumentChange();
    testBridgeRevokesMappedLiveSetRequestsOnRouteReplacement();
    testBridgePublishesPreparedLiveSetOwnershipWithoutDataRace();
    testBridgeRoutesRealtimeControlsToActiveSoundProgram();
    testBridgeSharesNamedControlsAcrossParallelGridSounds();
    testBridgeCarriesNamedControlToLaterGridSound();
    testBridgeRejectsSameSoundRestartWithoutAlteringPlayback();
    testBridgeRejectsCrossSetLaunchWithoutStoppingActiveSound();
    testBridgeRejectsFirstRoutedSoundWithoutStoppingOverlay();
    testBridgeRevokesActiveScoreRegionOnExternalSeek();
    return 0;
}
