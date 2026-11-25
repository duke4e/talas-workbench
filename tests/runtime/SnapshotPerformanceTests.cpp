#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/SnapshotPerformance.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>

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

    [[nodiscard]] std::size_t count() const
    {
        return allocationCount.load(std::memory_order_relaxed);
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression value;
    value.parameterId = std::move(id);
    value.expression = std::move(source);
    return value;
}

talas::project::ProjectDocument makeDirectEventValueProject()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back({
        "Pitch",
        talas::expr::ValueDomain::Hertz,
        talas::project::VirtualControlKind::Fader,
        440.0,
        20.0,
        20000.0,
    });
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "!Pitch"),
            expression("amplitude", "0.25"),
        },
    });
    document.graph.nodes.push_back({
        "output",
        "talas.audioOutput",
        {},
    });
    document.graph.connections.push_back({
        "osc",
        "audioOut",
        "output",
        "audioIn",
    });
    return document;
}

talas::project::ProjectDocument makeDirectRampProject()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        0.0,
        1.0,
    });
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain"),
        },
    });
    document.graph.nodes.push_back({
        "output",
        "talas.audioOutput",
        {},
    });
    document.graph.connections.push_back({
        "osc",
        "audioOut",
        "output",
        "audioIn",
    });
    return document;
}

talas::project::ProjectDocument makeDirectVariableProject()
{
    auto document = makeDirectRampProject();
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.virtualControls.clear();
    document.variableLibrary.variables.push_back({
        "gain",
        "Project gain",
        talas::expr::ValueDomain::Scalar,
        0.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    document.graph.nodes.front().parameters[1].expression = "@gain";
    return document;
}

talas::project::ProjectDocument makeOverlayGainProject()
{
    talas::project::ProjectParameterExpression noiseType;
    noiseType.parameterId = "type";
    noiseType.choice = "hotPink";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"output", "talas.audioOutput", {}},
        {"vca", "talas.vca", {expression("gain", "0")}},
        {"source", "talas.noise", {
            expression("initialState", "1"),
            expression("frequency", "0 hz"),
            expression("centerValue", "0"),
            expression("scale", "1"),
            std::move(noiseType),
        }},
    };
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "output", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeOverlayFftProject()
{
    talas::project::ProjectParameterExpression inverse;
    inverse.parameterId = "inverse";
    inverse.boolean = false;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"output", "talas.audioOutput", {}},
        {"fft", "talas.fft", {
            expression("length", "64"),
            std::move(inverse),
        }},
        {"osc", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "1"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "fft", "audioIn"},
        {"fft", "audioOut", "output", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument makeOverlayIngressProject()
{
    auto document = makeOverlayGainProject();
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        1.0,
        0.0,
        1.0,
    });
    document.graph.nodes.push_back({
        "liveGain",
        "talas.constant",
        {expression("value", "!Gain")},
    });
    document.graph.connections.push_back({
        "liveGain",
        "value",
        "vca",
        "gain",
    });
    return document;
}

talas::project::ProjectDocument makeSnapshotControllerProject()
{
    auto document = makeOverlayGainProject();
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotValue quietValue;
    quietValue.number = 0.0;
    talas::project::SnapshotValue loudValue;
    loudValue.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "gainBank";
    bank.displayName = "Gain Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.smoothingSeconds = 1.0;
    bank.parameterPolicies.push_back({
        gainTarget,
        false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots = {
        {"quiet", "Quiet", {{gainTarget, quietValue}}},
        {"loud", "Loud", {{gainTarget, loudValue}}},
    };
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

talas::project::ProjectDocument makeLayeredSnapshotControllerProject()
{
    auto document = makeOverlayGainProject();
    document.graph.nodes.insert(
        document.graph.nodes.begin() + 2,
        {"secondVca", "talas.vca", {expression("gain", "0")}});
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "secondVca", "audioIn"},
        {"secondVca", "audioOut", "output", "audioIn"},
    };
    const auto addBank = [&](
                             std::string bankId,
                             std::string nodeId,
                             double value) {
        talas::project::SnapshotTargetIdentity target;
        target.kind = talas::project::SnapshotTargetKind::NodeParameter;
        target.nodeId = nodeId;
        target.parameterId = "gain";
        talas::project::SnapshotValue stored;
        stored.number = value;
        talas::project::ProjectSnapshotBank bank;
        bank.bankId = std::move(bankId);
        bank.displayName = bank.bankId;
        bank.scope.kind = talas::project::SnapshotScopeKind::Node;
        bank.scope.nodeId = std::move(nodeId);
        bank.parameterPolicies.push_back({
            target,
            false,
            talas::project::SnapshotParameterRange{0.0, 1.0},
        });
        bank.snapshots.push_back({
            "active",
            "Active",
            {{target, stored}},
        });
        document.snapshotLibrary.banks.push_back(std::move(bank));
    };
    addBank("firstBank", "vca", 1.0);
    addBank("secondBank", "secondVca", 0.5);
    return document;
}

const talas::project::SnapshotValueEntry* performanceValue(
    const talas::runtime::SnapshotPerformanceState& state,
    const talas::project::SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        state.currentValues,
        target,
        &talas::project::SnapshotValueEntry::target);
    return found == state.currentValues.end()
        ? nullptr
        : std::addressof(*found);
}

talas::project::ProjectNode soundInstance(
    std::string id,
    std::string definitionId)
{
    talas::project::ProjectNode node;
    node.instanceId = std::move(id);
    node.soundDefinitionId = std::move(definitionId);
    return node;
}

talas::project::ProjectDocument makeBoundSoundProject(
    bool includeNodeBinding)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "dualVoice";
    definition.displayName = "Dual Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "mix",
        "audioOut",
    });
    definition.graph.nodes = {
        {
            "oscA",
            "talas.oscillator",
            {
                expression("frequency", "!PitchA"),
                expression("amplitude", "0.125"),
            },
        },
        {
            "oscB",
            "talas.oscillator",
            {
                expression("frequency", "!PitchB"),
                expression("amplitude", "0.125"),
            },
        },
        {"mix", "talas.mixer", {}},
    };
    definition.graph.connections = {
        {"oscA", "audioOut", "mix", "in1"},
        {"oscB", "audioOut", "mix", "in2"},
    };

    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("frequency", "120 hz");
    frequency.bindings = {
        {
            talas::project::ProjectSoundParameterTarget::EventValue,
            {},
            {},
            "PitchA",
            talas::expr::ValueDomain::Hertz,
        },
        {
            talas::project::ProjectSoundParameterTarget::EventValue,
            {},
            {},
            "PitchB",
            talas::expr::ValueDomain::Hertz,
        },
    };
    if (includeNodeBinding)
    {
        frequency.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            "oscA",
            "frequency",
            {},
            talas::expr::ValueDomain::Hertz,
        });
    }
    definition.exposedParameters.push_back(std::move(frequency));

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(definition));
    auto voice = soundInstance("voice", "dualVoice");
    voice.parameters.push_back(
        expression("frequency", "180 hz"));
    document.graph.nodes = {
        std::move(voice),
        {"output", "talas.audioOutput", {}},
    };
    document.graph.connections.push_back(
        {"voice", "audioOut", "output", "audioIn"});
    return document;
}

talas::project::SnapshotTargetIdentity soundFrequencyIdentity()
{
    talas::project::SnapshotTargetIdentity identity;
    identity.kind =
        talas::project::SnapshotTargetKind::SoundParameter;
    identity.instancePath = {"voice"};
    identity.definitionId = "dualVoice";
    identity.parameterId = "frequency";
    return identity;
}

talas::project::ProjectDocument makeDirectSnapshotControllerProject()
{
    auto document = makeBoundSoundProject(false);
    const auto target = soundFrequencyIdentity();
    talas::project::SnapshotValue one;
    one.domain = talas::expr::ValueDomain::Hertz;
    one.number = 1.0;
    talas::project::SnapshotValue two = one;
    two.number = 2.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "frequencyBank";
    bank.displayName = "Frequency Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "voice";
    bank.smoothingSeconds = 0.5;
    bank.parameterPolicies.push_back({
        target,
        false,
        talas::project::SnapshotParameterRange{1.0, 2.0},
    });
    bank.snapshots = {
        {"one", "One Hertz", {{target, one}}},
        {"two", "Two Hertz", {{target, two}}},
    };
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

talas::project::ProjectDocument makeMixedSnapshotControllerProject()
{
    auto document = makeDirectSnapshotControllerProject();
    document.graph.nodes.insert(
        document.graph.nodes.end() - 1,
        {"vca", "talas.vca", {expression("gain", "0.25")}});
    document.graph.connections = {
        {"voice", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "output", "audioIn"},
    };
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotValue quiet;
    quiet.number = 0.0;
    talas::project::SnapshotValue loud;
    loud.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "gainBank";
    bank.displayName = "Gain Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.smoothingSeconds = 2.0;
    bank.parameterPolicies.push_back({
        gainTarget,
        false,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots = {
        {"quiet", "Quiet", {{gainTarget, quiet}}},
        {"loud", "Loud", {{gainTarget, loud}}},
    };
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

talas::project::ProjectDocument makeTwoInstanceBoundSoundProject()
{
    auto document = makeBoundSoundProject(false);
    auto voiceA = document.graph.nodes.front();
    voiceA.instanceId = "voiceA";
    voiceA.parameters = {expression("frequency", "1 hz")};
    auto voiceB = soundInstance("voiceB", "dualVoice");
    voiceB.parameters = {expression("frequency", "1 hz")};
    document.graph.nodes = {
        std::move(voiceA),
        std::move(voiceB),
        {"rootMix", "talas.mixer", {}},
        {"output", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"voiceA", "audioOut", "rootMix", "in1"},
        {"voiceB", "audioOut", "rootMix", "in2"},
        {"rootMix", "audioOut", "output", "audioIn"},
    };
    return document;
}

void testCompilationPublishesStablePreparedSnapshotTargets()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues.push_back({
        "Pitch",
        440.0,
        talas::expr::ValueDomain::Hertz,
    });
    const auto compiled = talas::runtime::compileProject(
        makeDirectEventValueProject(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(),
            "prepared snapshot target fixture should compile");

    const auto& manifest = compiled.graph->snapshotPreparedTargets();
    talas::project::SnapshotTargetIdentity pitchIdentity;
    pitchIdentity.kind =
        talas::project::SnapshotTargetKind::EventValue;
    pitchIdentity.eventName = "Pitch";
    pitchIdentity.domain = talas::expr::ValueDomain::Hertz;
    const auto pitch = std::ranges::find(
        manifest.targets,
        pitchIdentity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        pitch != manifest.targets.end()
            && pitch->projection
                == talas::runtime::SnapshotPreparedProjection::DirectEventValues
            && pitch->eventValueTargets.size() == 1
            && pitch->eventValueTargets.front().scopeId
                == talas::runtime::globalRuntimeScope
            && pitch->eventValueTargets.front().targetIndex == 1
            && pitch->baselineValue.has_value()
            && pitch->baselineValue->number == 440.0,
        "Virtual Control target should publish one compact direct handle and baseline");

    talas::project::SnapshotTargetIdentity frequencyIdentity;
    frequencyIdentity.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    frequencyIdentity.nodeId = "osc";
    frequencyIdentity.parameterId = "frequency";
    const auto frequency = std::ranges::find(
        manifest.targets,
        frequencyIdentity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        frequency != manifest.targets.end()
            && frequency->projection
                == talas::runtime::SnapshotPreparedProjection::PerformanceOverlay
            && frequency->eventValueTargets.empty()
            && frequency->baselineValue.has_value()
            && frequency->baselineValue->domain
                == talas::expr::ValueDomain::Hertz
            && frequency->baselineValue->number == 440.0,
        "node parameters should retain their compiled baseline but require an overlay");
}

void testSnapshotDirectTransactionPublishesPreparedVariableValue()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeDirectVariableProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "direct variable snapshot fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size()),
        "direct variable snapshot fixture should activate");

    talas::project::SnapshotTargetIdentity identity;
    identity.kind = talas::project::SnapshotTargetKind::Variable;
    identity.variableId = "gain";
    require(
        bridge.snapshotDirectTargetPrepared(identity),
        "snapshot manifest should publish one direct variable handle");
    talas::project::SnapshotValue value;
    value.kind = talas::project::SnapshotValueKind::Number;
    value.domain = talas::expr::ValueDomain::Scalar;
    value.number = 0.75;
    const std::array<talas::project::SnapshotValueEntry, 1> values{{{
        identity,
        value,
    }}};
    require(
        bridge.applyPerformanceDirectValues(
                  values,
                  0.0,
                  bridge.latestStageGeneration())
                .ok()
            && bridge.render(&outputPointer, 1, output.size()),
        "snapshot recall should admit and render one variable transaction");
    const auto published = bridge.preparedVariableValues();
    require(
        published.publicationValid
            && published.values.size() == 1
            && published.values.front().identity.variableId == "gain"
            && published.values.front().value
                == talas::expr::ExpressionValue{
                    0.75,
                    talas::expr::ValueDomain::Scalar},
        "snapshot recall must atomically publish the variable cell value");
}

void testSnapshotDirectTransactionPreservesWorkspaceSessionOwner()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = makeDirectVariableProject();
    document.variableLibrary.variables.front().owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "workspace-session snapshot fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size()),
        "workspace-session snapshot fixture should activate");

    talas::project::SnapshotTargetIdentity identity;
    identity.kind = talas::project::SnapshotTargetKind::Variable;
    identity.variableOwnerKind =
        talas::project::SnapshotVariableOwnerKind::WorkspaceSession;
    identity.variableId = "gain";
    require(
        bridge.snapshotDirectTargetPrepared(identity),
        "snapshot manifest should preserve the workspace-session target owner");
    talas::project::SnapshotValue value;
    value.kind = talas::project::SnapshotValueKind::Number;
    value.domain = talas::expr::ValueDomain::Scalar;
    value.number = 0.5;
    const std::array<talas::project::SnapshotValueEntry, 1> values{{{
        identity,
        value,
    }}};
    require(
        bridge.applyPerformanceDirectValues(
                  values,
                  0.0,
                  bridge.latestStageGeneration())
                .ok()
            && bridge.render(&outputPointer, 1, output.size()),
        "workspace-session snapshot recall should publish one direct value");
    const auto published = bridge.preparedVariableValues();
    require(
        published.publicationValid
            && published.values.size() == 1
            && published.values.front().identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::WorkspaceSession
            && published.values.front().value.value == 0.5,
        "workspace-session snapshot recall must not silently rebind to the project owner");
}

void testLiveVariableAssignmentFeedsSnapshotCaptureAuthority()
{
    auto document = makeDirectVariableProject();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "liveVariable";
    bank.displayName = "Live Variable";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        document, registry, bank.scope);
    const auto initialized = talas::project::initializeSnapshotScopeValues(
        resolved.targets);
    require(resolved.ok() && initialized.ok(),
        "live variable capture fixture should resolve complete defaults");
    const auto variableTarget = std::ranges::find_if(
        resolved.targets,
        [](const auto& target) {
            return target.target.kind
                == talas::project::SnapshotTargetKind::Variable;
        });
    require(variableTarget != resolved.targets.end(),
        "live variable capture fixture should expose one variable target");
    for (const auto& target : resolved.targets)
    {
        bank.parameterPolicies.push_back({
            target.target,
            true,
            target.valueKind == talas::project::SnapshotValueKind::Number
                ? std::optional<talas::project::SnapshotParameterRange>{{0.0, 1.0}}
                : std::nullopt,
        });
    }
    bank.snapshots.push_back({
        "captured", "Captured", initialized.values});
    document.snapshotLibrary.banks.push_back(std::move(bank));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "live variable capture fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "live variable capture fixture should activate");

    const talas::project::PreparedProjectVariableIdentity identity{
        talas::project::ProjectVariableOwnerKind::Project,
        "gain", {}, {}};
    require(
        bridge.scheduleVariableAssignment(
            identity,
            0.75,
            talas::expr::ValueDomain::Scalar,
            0,
            bridge.latestStageGeneration())
            && bridge.render(&outputPointer, 1, output.size()),
        "live variable capture fixture should publish one direct assignment");

    require(bridge.recallSnapshot("liveVariable", "captured").ok(),
        "locked recall should consume live variable authority");
    require(bridge.render(&outputPointer, 1, output.size()),
        "locked variable recall should render");
    require(bridge.beginSnapshotMorph(
                "liveVariable", "captured", "captured").ok(),
        "locked morph should capture live variable authority");
    require(bridge.updateSnapshotMorph(0.5).ok(),
        "locked variable morph should publish");
    require(bridge.render(&outputPointer, 1, output.size()),
        "locked variable morph should render");
    bridge.endSnapshotMorph();
    const auto randomized =
        bridge.randomizeSnapshotBank("liveVariable", 42);
    require(randomized.ok(),
        "locked randomization should consume live variable authority: "
            + randomized.message
            + (randomized.diagnostics.empty()
                   ? std::string{}
                   : " / " + randomized.diagnostics.front().message));
    require(bridge.render(&outputPointer, 1, output.size()),
        "locked variable randomization should render");
    const auto afterLockedActions = bridge.preparedVariableValues();
    require(
        afterLockedActions.publicationValid
            && afterLockedActions.values.front().value.value == 0.75,
        "locked snapshot actions must retain the coherent live variable instead of a stale controller cache");

    const auto current = bridge.currentSnapshotScopeValues(
        document.snapshotLibrary.banks.front().scope);
    const auto currentVariable = std::ranges::find_if(
        current.values,
        [](const auto& value) {
            return value.target.kind
                    == talas::project::SnapshotTargetKind::Variable
                && value.target.variableId == "gain";
        });
    require(
        current.ok()
            && currentVariable != current.values.end()
            && currentVariable->value.number == 0.75,
        "snapshot current-value projection must read the coherent live variable cell");

    const auto captured = bridge.captureSnapshotPerformance(
        "liveVariable", "captured");
    const auto& capturedValues = bridge.editor().document()
                                     .snapshotLibrary.banks.front()
                                     .snapshots.front().values;
    const auto capturedVariable = std::ranges::find_if(
        capturedValues,
        [](const auto& value) {
            return value.target.kind
                    == talas::project::SnapshotTargetKind::Variable
                && value.target.variableId == "gain";
        });
    require(
        captured.ok()
            && capturedVariable != capturedValues.end()
            && capturedVariable->value.number == 0.75,
        "explicit snapshot capture must store the coherent live variable value instead of a stale baseline");
}

void testGeneratedControllerHostRouteRespectsManualAndSnapshotValues()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = makeDirectRampProject();
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "generatedGain";
    bank.displayName = "Generated Gain";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        document, registry, bank.scope);
    const auto initialized = talas::project::initializeSnapshotScopeValues(
        resolved.targets);
    require(resolved.ok() && initialized.ok(),
        "generated-controller capture fixture should resolve project values");
    bank.snapshots.push_back({"captured", "Captured", initialized.values});
    document.snapshotLibrary.banks.push_back(std::move(bank));

    talas::runtime::ProjectRuntimeBridge target(document, registry);
    talas::runtime::RenderSettings targetSettings{4.0, 4};
    require(target.compileAndStage(std::move(targetSettings)).ok(),
        "generated-controller target should stage");
    target.setPlaying(true);
    std::array<float, 4> audio{};
    auto* audioPointer = audio.data();
    require(target.render(&audioPointer, 1, audio.size()),
        "generated-controller target should activate");

    talas::project::ProjectDocument source;
    talas::project::ProjectParameterExpression generatedEvent;
    generatedEvent.parameterId = "generatedEvent";
    generatedEvent.text = "!Gain";
    talas::project::ProjectParameterExpression value;
    value.parameterId = "value";
    value.text = "{!Value}";
    talas::project::ProjectParameterExpression override;
    override.parameterId = "allowLiveOverride";
    override.boolean = true;
    source.graph.nodes.push_back({
        "terminal", "talas.triggeredSoundToGlobalController",
        {generatedEvent, value, expression("trigger", "!Gate"), override},
    });
    talas::runtime::RenderSettings sourceSettings{4.0, 2};
    sourceSettings.eventValues = {
        {"Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {"Value", 0.5, talas::expr::ValueDomain::Scalar},
    };
    const auto sourceGraph = talas::runtime::compileProject(
        source, registry, std::move(sourceSettings));
    require(sourceGraph.ok(),
        "generated-controller source should compile");
    talas::runtime::RealtimeRenderSession sourceSession(*sourceGraph.graph);
    require(sourceSession.ready(),
        "generated-controller source should prepare");
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 2> events{};
    std::array<char, 16> name{};
    talas::runtime::RealtimeTerminalOutputBlock terminal{};
    terminal.globalControllerEvents = events;
    terminal.globalControllerName = name;
    const auto generation = target.latestStageGeneration();
    const auto routeGenerated = [&](double expected) {
        require(sourceSession.renderTerminalEvents(2, terminal)
                    == talas::runtime::RealtimeTerminalRenderResult::Rendered
                && terminal.globalControllerEventCount == 1
                && terminal.globalControllerNameLength == 5
                && std::string_view{name.data(), terminal.globalControllerNameLength}
                    == "!Gain"
                && events.front().value == expected
                && events.front().allowLiveOverride,
            "source should publish one exact generated controller event");
        const auto routed = target.applyGlobalControllerEvent(
            {name.data(), terminal.globalControllerNameLength},
            events.front().value, events.front().allowLiveOverride,
            target.latestStageGeneration());
        require(routed.ok(),
            "host should route the generated event to the prepared target: "
                + routed.message);
        require(target.render(&audioPointer, 1, audio.size())
                && std::abs(audio[1] - expected) < 1.0e-6F,
            "generated event should change target audio at the next block");
    };

    talas::project::SnapshotTargetIdentity gain;
    gain.kind = talas::project::SnapshotTargetKind::EventValue;
    gain.eventName = "Gain";
    gain.domain = talas::expr::ValueDomain::Scalar;
    require(target.snapshotDirectTargetPrepared(gain),
        "generated controller should target a prepared direct Event Value");
    routeGenerated(0.5);
    const auto* generated = performanceValue(target.performanceState(), gain);
    require(generated != nullptr && generated->value.number == 0.5
            && target.editor().document().virtualControls.front().value == 0.0
            && !target.editor().canUndo(),
        "generated value should feed performance without authoring an edit");

    const auto beforeRejected = target.performanceState();
    require(!target.applyGlobalControllerEvent("Gain", 0.3, generation).ok()
            && !target.applyGlobalControllerEvent("!Missing", 0.3, generation).ok()
            && !target.applyGlobalControllerEvent(
                "!Gain", std::numeric_limits<double>::quiet_NaN(), generation).ok()
            && !target.applyGlobalControllerEvent("!Gain", -0.1, generation).ok()
            && !target.applyGlobalControllerEvent("!Gain", 0.3, generation + 1).ok()
            && target.performanceState() == beforeRejected
            && !target.editor().canUndo(),
        "invalid or stale generated events should leave performance and authoring unchanged");

    talas::project::PreparedVirtualControlValueEdit manualEdit;
    require(target.editor().prepareVirtualControlValue(
                "Gain", 0.75, manualEdit).ok(),
        "manual control edit should prepare");
    talas::project::SnapshotValue manualValue;
    manualValue.number = 0.75;
    const std::array manualValues{
        talas::project::SnapshotValueEntry{gain, manualValue}};
    require(target.applyPerformanceDirectValues(
                manualValues, 0.0, generation, std::move(manualEdit)).ok()
            && target.render(&audioPointer, 1, audio.size())
            && std::abs(audio[1] - 0.75F) < 1.0e-6F
            && target.editor().document().virtualControls.front().value == 0.75
            && target.editor().canUndo(),
        "manual override should update audible and authored control values");

    require(target.captureSnapshotPerformance(
                "generatedGain", "captured").ok(),
        "Preset capture should accept the current manual performance value");
    const auto& captured = target.editor().document()
                               .snapshotLibrary.banks.front()
                               .snapshots.front().values;
    const auto capturedGain = std::ranges::find(
        captured, gain, &talas::project::SnapshotValueEntry::target);
    require(capturedGain != captured.end()
            && capturedGain->value.number == 0.75
            && target.adoptSnapshotAuthoringEdit().ok(),
        "Preset should store manual override and adopt its authoring edit");
    require(sourceSession.updateEventValue(
                "Gate", 0.0, talas::expr::ValueDomain::Scalar)
            && sourceSession.updateEventValue(
                "Value", 0.25, talas::expr::ValueDomain::Scalar)
            && sourceSession.renderTerminalEvents(2, terminal)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && terminal.globalControllerEventCount == 0
            && sourceSession.updateEventValue(
                "Gate", 1.0, talas::expr::ValueDomain::Scalar),
        "closed source gate should suppress changed values until reopened");
    routeGenerated(0.25);
    require(target.recallSnapshot("generatedGain", "captured").ok()
            && target.render(&audioPointer, 1, audio.size())
            && std::abs(audio[1] - 0.75F) < 1.0e-6F,
        "Preset recall should supersede the last generated value");
    const auto afterRecall = target.performanceState();
    const auto staleAfterRecall = target.applyGlobalControllerEvent(
        "!Gain", 0.2, generation);
    require(target.latestStageGeneration() != generation
            && !staleAfterRecall.ok()
            && target.performanceState() == afterRecall,
        "prior-stage terminal events should reject after Preset recall");
    require(sourceSession.updateEventValue(
                "Value", 0.1, talas::expr::ValueDomain::Scalar),
        "source should accept a later generated value");
    routeGenerated(0.1);
    const auto* finalGain = performanceValue(target.performanceState(), gain);
    require(finalGain != nullptr && finalGain->value.number == 0.1
            && target.editor().document().virtualControls.front().value == 0.75,
        "later generated event should supersede recall without rewriting authored value");

    const auto exclusiveGeneration = target.latestStageGeneration();
    auto exclusiveSource = source;
    exclusiveSource.graph.nodes.front().parameters.back().boolean = false;
    talas::runtime::RenderSettings exclusiveSettings{4.0, 2};
    exclusiveSettings.eventValues = {
        {"Gate", 1.0, talas::expr::ValueDomain::Scalar},
        {"Value", 0.4, talas::expr::ValueDomain::Scalar},
    };
    const auto exclusiveGraph = talas::runtime::compileProject(
        exclusiveSource, registry, std::move(exclusiveSettings));
    require(exclusiveGraph.ok(),
        "generated-only source should compile");
    talas::runtime::RealtimeRenderSession exclusiveSession(*exclusiveGraph.graph);
    require(exclusiveSession.ready()
            && exclusiveSession.renderTerminalEvents(2, terminal)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered
            && terminal.globalControllerEventCount == 1
            && events.front().value == 0.4
            && !events.front().allowLiveOverride,
        "generated-only source should publish disabled AllowLiveOverride");
    require(target.applyGlobalControllerEvent(
                {name.data(), terminal.globalControllerNameLength},
                events.front().value, events.front().allowLiveOverride,
                exclusiveGeneration).ok()
            && target.render(&audioPointer, 1, audio.size())
            && std::abs(audio[1] - 0.4F) < 1.0e-6F,
        "generated-only source should assert its current value");
    talas::project::PreparedVirtualControlValueEdit blockedEdit;
    require(target.editor().prepareVirtualControlValue(
                "Gain", 0.6, blockedEdit).ok(),
        "blocked manual change should still prepare a candidate edit");
    talas::project::SnapshotValue blockedValue;
    blockedValue.number = 0.6;
    const std::array blockedValues{
        talas::project::SnapshotValueEntry{gain, blockedValue}};
    const auto beforeBlocked = target.performanceState();
    require(!target.applyPerformanceDirectValues(
                blockedValues, 0.0, exclusiveGeneration,
                std::move(blockedEdit)).ok()
            && !target.recallSnapshot(
                "generatedGain", "captured").ok()
            && target.performanceState() == beforeBlocked
            && target.editor().document().virtualControls.front().value
                == 0.75,
        "generated-only source should reject manual and Preset overrides without changing authoring");

    require(target.applyGlobalControllerEvent(
                "!Gain", 0.2, true, exclusiveGeneration).ok(),
        "an override-enabled source should release generated-only ownership");
    talas::project::PreparedVirtualControlValueEdit releasedEdit;
    require(target.editor().prepareVirtualControlValue(
                "Gain", 0.6, releasedEdit).ok()
            && target.applyPerformanceDirectValues(
                blockedValues, 0.0, exclusiveGeneration,
                std::move(releasedEdit)).ok()
            && target.render(&audioPointer, 1, audio.size())
            && std::abs(audio[1] - 0.6F) < 1.0e-6F,
        "manual control should work after AllowLiveOverride releases ownership");
    require(target.recallSnapshot("generatedGain", "captured").ok()
            && target.render(&audioPointer, 1, audio.size())
            && std::abs(audio[1] - 0.75F) < 1.0e-6F,
        "Preset recall should work after generated-only ownership is released");
}

void testGeneratedControllerHostRouteUsesVirtualControlDomain()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = makeDirectEventValueProject();
    document.virtualControls.front().eventName = "Frequency";
    document.graph.nodes.front().parameters.front().expression = "!Frequency";
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    talas::runtime::RenderSettings settings{48000.0, 64};
    const auto staged = bridge.compileAndStage(std::move(settings));
    require(staged.ok(),
        "Hertz generated-controller target should stage: " + staged.message);
    bridge.setPlaying(true);
    std::array<float, 64> audio{};
    auto* audioPointer = audio.data();
    require(bridge.render(&audioPointer, 1, audio.size()),
        "Hertz generated-controller target should activate");

    const auto generation = bridge.latestStageGeneration();
    talas::project::SnapshotTargetIdentity pitch;
    pitch.kind = talas::project::SnapshotTargetKind::EventValue;
    pitch.eventName = "Frequency";
    pitch.domain = talas::expr::ValueDomain::Hertz;
    require(bridge.snapshotDirectTargetPrepared(pitch)
            && bridge.applyGlobalControllerEvent(
                "!Frequency", 880.0, generation).ok()
            && bridge.render(&audioPointer, 1, audio.size()),
        "generated value should reach the prepared Hertz Event Value");
    const auto* current = performanceValue(bridge.performanceState(), pitch);
    require(current != nullptr
            && current->value.domain == talas::expr::ValueDomain::Hertz
            && current->value.number == 880.0
            && bridge.editor().document().virtualControls.front().value == 440.0
            && !bridge.editor().canUndo(),
        "generated Hertz value should remain live without rewriting the authored fader");
}

void testMixedOverlayRecallCarriesVariableIntoReplacementStage()
{
    auto document = makeDirectVariableProject();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::SnapshotTargetIdentity variableIdentity;
    variableIdentity.kind =
        talas::project::SnapshotTargetKind::Variable;
    variableIdentity.variableId = "gain";
    talas::project::SnapshotTargetIdentity frequencyIdentity;
    frequencyIdentity.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    frequencyIdentity.nodeId = "osc";
    frequencyIdentity.parameterId = "frequency";
    talas::project::SnapshotValue variableValue;
    variableValue.kind = talas::project::SnapshotValueKind::Number;
    variableValue.domain = talas::expr::ValueDomain::Scalar;
    variableValue.number = 0.5;
    talas::project::SnapshotValue frequencyValue;
    frequencyValue.kind = talas::project::SnapshotValueKind::Number;
    frequencyValue.domain = talas::expr::ValueDomain::Hertz;
    frequencyValue.number = 2.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "mixedBank";
    bank.displayName = "Mixed Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    bank.parameterPolicies = {
        {
            variableIdentity,
            false,
            talas::project::SnapshotParameterRange{0.0, 1.0},
        },
        {
            frequencyIdentity,
            false,
            talas::project::SnapshotParameterRange{1.0, 20.0},
        },
    };
    talas::expr::EvaluationContext evaluation;
    evaluation.sampleRate = 4.0;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        bank.scope,
        evaluation);
    const auto initialized =
        talas::project::initializeSnapshotScopeValues(resolved.targets);
    require(
        resolved.ok() && initialized.ok(),
        "mixed snapshot project scope should have complete defaults");
    auto storedValues = initialized.values;
    for (auto& stored : storedValues)
    {
        if (stored.target == variableIdentity)
            stored.value = variableValue;
        else if (stored.target == frequencyIdentity)
            stored.value = frequencyValue;
    }
    bank.snapshots.push_back({
        "mixed",
        "Mixed",
        std::move(storedValues),
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "mixed overlay/variable snapshot fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size()),
        "mixed overlay/variable snapshot fixture should activate");
    const auto recalled = bridge.recallSnapshot("mixedBank", "mixed");
    require(
        recalled.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    StagedOverlay
            && bridge.render(&outputPointer, 1, output.size()),
        "mixed snapshot should stage its node overlay and variable transaction together: "
            + recalled.message);
    const auto published = bridge.preparedVariableValues();
    require(
        published.publicationValid
            && published.values.size() == 1
            && published.values.front().value
                == talas::expr::ExpressionValue{
                    0.5,
                    talas::expr::ValueDomain::Scalar},
        "mixed overlay adoption must publish the requested variable value in the replacement stage");
}

void testCompleteSoundEventBindingsPublishOneAtomicDirectTarget()
{
    const auto compiled = talas::runtime::compileProject(
        makeBoundSoundProject(false),
        talas::core::makeBuiltinNodeRegistry(),
        {});
    require(
        compiled.ok(),
        "one-to-many Sound Event Value fixture should compile");

    const auto identity = soundFrequencyIdentity();
    const auto& manifest = compiled.graph->snapshotPreparedTargets();
    const auto target = std::ranges::find(
        manifest.targets,
        identity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        target != manifest.targets.end()
            && target->projection
                == talas::runtime::SnapshotPreparedProjection::DirectEventValues
            && target->eventValueTargets.size() == 2
            && target->eventValueTargets[0].targetIndex == 1
            && target->eventValueTargets[1].targetIndex == 2
            && target->eventValueTargets[0].scopeId
                == target->eventValueTargets[1].scopeId
            && target->eventValueTargets[0].scopeId
                != talas::runtime::globalRuntimeScope
            && target->baselineValue.has_value()
            && target->baselineValue->number == 180.0,
        "one semantic Sound parameter should publish its complete scoped binding set as one direct target");
}

void testMixedSoundBindingsNeverPublishPartialDirectHandles()
{
    const auto compiled = talas::runtime::compileProject(
        makeBoundSoundProject(true),
        talas::core::makeBuiltinNodeRegistry(),
        {});
    require(
        compiled.ok(),
        "mixed Sound binding fixture should compile");

    const auto identity = soundFrequencyIdentity();
    const auto& manifest = compiled.graph->snapshotPreparedTargets();
    const auto target = std::ranges::find(
        manifest.targets,
        identity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        target != manifest.targets.end()
            && target->projection
                == talas::runtime::SnapshotPreparedProjection::PerformanceOverlay
            && target->eventValueTargets.empty()
            && target->baselineValue.has_value()
            && target->baselineValue->number == 180.0,
        "a mixed Sound target should keep one overlay baseline and publish no partial direct prefix");
}

void testNodeBoundSoundTargetRetainsItsExactPreparedBaseline()
{
    auto document = makeBoundSoundProject(true);
    auto& definition = document.soundDefinitions.front();
    definition.exposedParameters.front().bindings.erase(
        definition.exposedParameters.front().bindings.begin(),
        definition.exposedParameters.front().bindings.begin() + 2);
    definition.graph.nodes[1].parameters.front().expression =
        "220 hz";
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {});
    require(
        compiled.ok(),
        "node-bound Sound parameter fixture should compile");

    const auto identity = soundFrequencyIdentity();
    const auto& manifest = compiled.graph->snapshotPreparedTargets();
    const auto target = std::ranges::find(
        manifest.targets,
        identity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        target != manifest.targets.end()
            && target->projection
                == talas::runtime::SnapshotPreparedProjection::PerformanceOverlay
            && target->eventValueTargets.empty()
            && target->baselineValue.has_value()
            && target->baselineValue->number == 180.0,
        "node-bound Sound targets should resolve through exact expanded parameter provenance");
}

void testProjectedSoundEventBindingsRemainOverlayOnly()
{
    auto document = makeBoundSoundProject(false);
    document.graph.nodes.front().parameters.clear();
    document.soundDefinitions.front()
        .exposedParameters.front()
        .defaultValue.expression = "!GlobalPitch";
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues.push_back({
        "GlobalPitch",
        150.0,
        talas::expr::ValueDomain::Hertz,
    });
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        "projected Sound Event Value fixture should compile");

    const auto identity = soundFrequencyIdentity();
    const auto& manifest = compiled.graph->snapshotPreparedTargets();
    const auto target = std::ranges::find(
        manifest.targets,
        identity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        target != manifest.targets.end()
            && target->projection
                == talas::runtime::SnapshotPreparedProjection::PerformanceOverlay
            && target->eventValueTargets.empty()
            && target->baselineValue.has_value()
            && target->baselineValue->number == 150.0,
        "derived local Event Values should not masquerade as independently writable direct handles");
}

void testDirectSnapshotRampIsLinearAndBlockPartitionInvariant()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 8;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto compiled = talas::runtime::compileProject(
        makeDirectRampProject(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        "direct ramp fixture should compile");

    talas::project::SnapshotTargetIdentity identity;
    identity.kind =
        talas::project::SnapshotTargetKind::EventValue;
    identity.eventName = "Gain";
    identity.domain = talas::expr::ValueDomain::Scalar;
    const auto prepared = std::ranges::find(
        compiled.graph->snapshotPreparedTargets().targets,
        identity,
        &talas::runtime::SnapshotPreparedTarget::target);
    require(
        prepared
            != compiled.graph->snapshotPreparedTargets().targets.end()
            && prepared->eventValueTargets.size() == 1,
        "direct ramp fixture should publish one compact target");
    const std::array<talas::runtime::SnapshotDirectEventValueUpdate, 1>
        updates{{{
            prepared->eventValueTargets.front().targetIndex,
            1.0,
            talas::expr::ValueDomain::Scalar,
        }}};

    const auto renderTrace = [&](std::span<const std::size_t> partitions) {
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), std::string(session.error()));
        require(
            session.scheduleSnapshotDirectTransaction(
                updates,
                0,
                4)
                == talas::runtime::SnapshotDirectTransactionResult::Accepted,
            "direct snapshot ramp should schedule");
        std::array<double, 6> output{};
        std::size_t offset = 0;
        for (const auto count : partitions)
        {
            require(
                session.render(
                    std::span<double>(output).subspan(offset, count)),
                "direct snapshot ramp partition should render");
            offset += count;
        }
        require(offset == output.size(),
                "direct snapshot ramp partitions should cover the trace");
        return output;
    };

    constexpr std::array<std::size_t, 1> oneBlock{6};
    constexpr std::array<std::size_t, 3> splitBlocks{2, 1, 3};
    const auto contiguous = renderTrace(oneBlock);
    const auto partitioned = renderTrace(splitBlocks);
    const std::array<double, 6> expected{0.0, 0.25, 0.0, -0.75, 0.0, 1.0};
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        require(
            std::abs(contiguous[frame] - expected[frame]) < 1.0e-12
                && std::abs(partitioned[frame] - expected[frame])
                    < 1.0e-12,
            "direct snapshot ramp should use the exact callback-observed linear value at every frame");
    }
}

void testDirectTransactionIsStageScopedAndQueueBounded()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.maxRealtimeEventTransactions = 1;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto compiled = talas::runtime::compileProject(
        makeDirectRampProject(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        "stage-scoped direct transaction fixture should compile");
    const auto eventTarget = std::ranges::find_if(
        compiled.graph->snapshotPreparedTargets().targets,
        [](const auto& target) {
            return target.target.kind
                    == talas::project::SnapshotTargetKind::EventValue
                && target.target.eventName == "Gain";
        });
    require(
        eventTarget
                != compiled.graph->snapshotPreparedTargets().targets.end()
            && eventTarget->eventValueTargets.size() == 1,
        "stage-scoped direct transaction fixture should expose Gain");
    const std::array<talas::runtime::SnapshotDirectEventValueUpdate, 1>
        updates{{{
            eventTarget->eventValueTargets.front().targetIndex,
            1.0,
            talas::expr::ValueDomain::Scalar,
        }}};

    talas::runtime::RealtimeAudioEngine engine(*compiled.graph);
    engine.setStageId(41);
    engine.setSnapshotGeneration(7);
    const talas::runtime::SnapshotDirectTransaction stale{
        41,
        6,
        0,
        0,
        updates,
    };
    require(
        engine.scheduleSnapshotDirectTransaction(stale)
            == talas::runtime::SnapshotDirectTransactionResult::
                StaleStageOrGeneration,
        "stale snapshot generation should reject before queue publication");
    const talas::runtime::SnapshotDirectTransaction current{
        41,
        7,
        0,
        0,
        updates,
    };
    require(
        engine.scheduleSnapshotDirectTransaction(current)
            == talas::runtime::SnapshotDirectTransactionResult::Accepted,
        "current stage and generation should publish one direct transaction");
    require(
        engine.scheduleSnapshotDirectTransaction(current)
            == talas::runtime::SnapshotDirectTransactionResult::QueueFull,
        "a full bounded transaction ring should report queue backpressure");

    engine.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        engine.render(&outputPointer, 1, output.size())
            && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "accepted zero-ramp transaction should become audible at the next sample boundary");

    talas::runtime::RealtimeAudioEngine replacement(*compiled.graph);
    replacement.setStageId(42);
    replacement.setSnapshotGeneration(8);
    require(
        replacement.scheduleSnapshotDirectTransaction(current)
            == talas::runtime::SnapshotDirectTransactionResult::
                StaleStageOrGeneration,
        "a replacement stage should reject an old prepared transaction");
}

void testDirectTransactionRejectsInvalidMemberWithoutPartialPublication()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto compiled = talas::runtime::compileProject(
        makeDirectRampProject(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        "invalid-member atomicity fixture should compile");
    const auto direct = std::ranges::find_if(
        compiled.graph->snapshotPreparedTargets().targets,
        [](const auto& target) {
            return target.target.kind
                    == talas::project::SnapshotTargetKind::EventValue
                && target.target.eventName == "Gain";
        });
    require(
        direct != compiled.graph->snapshotPreparedTargets().targets.end()
            && direct->eventValueTargets.size() == 1,
        "invalid-member atomicity fixture should expose Gain");
    const std::array<talas::runtime::SnapshotDirectEventValueUpdate, 2>
        updates{{
            {
                direct->eventValueTargets.front().targetIndex,
                1.0,
                talas::expr::ValueDomain::Scalar,
            },
            {
                direct->eventValueTargets.front().targetIndex + 100,
                0.5,
                talas::expr::ValueDomain::Scalar,
            },
        }};

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(
        session.ready()
            && session.scheduleSnapshotDirectTransaction(
                   updates,
                   0,
                   0)
                == talas::runtime::SnapshotDirectTransactionResult::
                    InvalidTargetOrPayload,
        "one invalid compact handle should reject the complete direct transaction");
    std::array<double, 4> output{};
    require(
        session.render(output),
        "rejected direct transaction fixture should still render");
    for (const auto sample : output)
    {
        require(
            std::abs(sample) < 1.0e-12,
            "rejected direct transaction must not publish its valid prefix");
    }
}

void testSnapshotSmoothingFrameConversionIsExactAndSaturating()
{
    require(
        talas::runtime::snapshotSmoothingFrames(0.5, 48000.0)
                == 24000
            && talas::runtime::snapshotSmoothingFrames(0.5, 3.0)
                == 2
            && talas::runtime::snapshotSmoothingFrames(-1.0, 48000.0)
                == 0
            && talas::runtime::snapshotSmoothingFrames(
                   std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::max())
                == std::numeric_limits<std::uint64_t>::max(),
        "snapshot smoothing should round at the prepared sample rate and saturate overflow safely");
}

void testDirectSnapshotSchedulingAndCallbackDoNotAllocate()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto compiled = talas::runtime::compileProject(
        makeDirectRampProject(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(), "allocation fixture should compile");
    const auto target = std::ranges::find_if(
        compiled.graph->snapshotPreparedTargets().targets,
        [](const auto& candidate) {
            return candidate.target.kind
                    == talas::project::SnapshotTargetKind::EventValue
                && candidate.target.eventName == "Gain";
        });
    require(
        target != compiled.graph->snapshotPreparedTargets().targets.end()
            && target->eventValueTargets.size() == 1,
        "allocation fixture should expose Gain");
    const std::array<talas::runtime::SnapshotDirectEventValueUpdate, 1>
        updates{{{
            target->eventValueTargets.front().targetIndex,
            1.0,
            talas::expr::ValueDomain::Scalar,
        }}};
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));

    talas::runtime::SnapshotDirectTransactionResult scheduleResult{};
    std::size_t scheduleAllocations = 0;
    {
        AllocationCounter counter;
        scheduleResult = session.scheduleSnapshotDirectTransaction(
            updates,
            0,
            4);
        scheduleAllocations = counter.count();
    }
    std::array<double, 4> output{};
    bool rendered = false;
    std::size_t renderAllocations = 0;
    {
        AllocationCounter counter;
        rendered = session.render(output);
        renderAllocations = counter.count();
    }
    require(
        scheduleResult
                == talas::runtime::SnapshotDirectTransactionResult::Accepted
            && scheduleAllocations == 0
            && rendered
            && renderAllocations == 0,
        "direct snapshot publication and callback ramp processing must use only prepared storage");
}

void testBridgeResolvesStableTargetsIntoLatestPreparedTransaction()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeDirectRampProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "bridge direct snapshot fixture should stage");

    talas::project::SnapshotValueEntry overlayOnly;
    overlayOnly.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    overlayOnly.target.nodeId = "osc";
    overlayOnly.target.parameterId = "frequency";
    overlayOnly.value.domain = talas::expr::ValueDomain::Hertz;
    overlayOnly.value.number = 2.0;
    require(
        bridge.scheduleSnapshotDirectValues(
            std::span<const talas::project::SnapshotValueEntry>(
                &overlayOnly,
                1),
            0.0)
            == talas::runtime::SnapshotDirectTransactionResult::
                InvalidTargetOrPayload,
        "bridge direct path should reject an overlay-only stable target without publishing a prefix");

    talas::project::SnapshotValueEntry gain;
    gain.target.kind =
        talas::project::SnapshotTargetKind::EventValue;
    gain.target.eventName = "Gain";
    gain.target.domain = talas::expr::ValueDomain::Scalar;
    gain.value.domain = talas::expr::ValueDomain::Scalar;
    gain.value.number = 1.0;
    require(
        bridge.scheduleSnapshotDirectValues(
            std::span<const talas::project::SnapshotValueEntry>(
                &gain,
                1),
            0.0)
            == talas::runtime::SnapshotDirectTransactionResult::Accepted,
        "bridge should resolve the stable Event Value identity into its latest compact target");

    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size())
            && std::abs(output[1] - 1.0F) < 1.0e-6F,
        "a direct action queued against the pending latest stage should become audible when adopted");
    const auto telemetry = bridge.realtimeControlTelemetry();
    require(
        telemetry.preparedSnapshotDirectTransactions == 1
            && telemetry.invalidTargetOrPayload == 1
            && telemetry.aggregateRejected == 1,
        "bridge telemetry should distinguish the accepted direct transaction from its rejected overlay preflight");
}

void testBridgeFlattensOneSoundValueIntoItsCompleteDirectBindingSet()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeBoundSoundProject(false),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "one-to-many bridge fixture should stage");

    talas::project::SnapshotValueEntry frequency;
    frequency.target = soundFrequencyIdentity();
    frequency.value.domain = talas::expr::ValueDomain::Hertz;
    frequency.value.number = 2.0;
    require(
        bridge.scheduleSnapshotDirectValues(
            std::span<const talas::project::SnapshotValueEntry>(
                &frequency,
                1),
            0.0)
            == talas::runtime::SnapshotDirectTransactionResult::Accepted,
        "one stable Sound value should resolve into all prepared Event Value handles");

    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size())
            && std::abs(output[1] - 0.25F) < 1.0e-6F,
        "all one-to-many Sound bindings should become audible at the same sample boundary");
}

void testExecutionTopologyFingerprintSeparatesAuthoringLayersAndStructure()
{
    const auto base = makeDirectRampProject();
    auto valuesOnly = base;
    valuesOnly.virtualControls.front().value = 0.75;
    valuesOnly.graph.nodes.front().parameters.front().expression =
        "2 hz";
    valuesOnly.graph.nodes.front().parameters.back().expression =
        "0.5";
    require(
        talas::runtime::snapshotExecutionTopologyFingerprint(base)
            == talas::runtime::snapshotExecutionTopologyFingerprint(
                valuesOnly),
        "topology fingerprint should ignore live numeric/choice/boolean payloads and Virtual Control values");

    auto authoringOnly = valuesOnly;
    authoringOnly.metadata.title = "Renamed";
    authoringOnly.presentation.structureLayouts.clear();
    talas::project::ProjectSnapshotBank authoringBank;
    authoringBank.bankId = "authoringOnly";
    authoringBank.displayName = "Authoring Only";
    authoringOnly.snapshotLibrary.banks.push_back(
        std::move(authoringBank));
    require(
        talas::runtime::snapshotExecutionTopologyFingerprint(base)
            == talas::runtime::snapshotExecutionTopologyFingerprint(
                authoringOnly),
        "execution topology should ignore metadata, presentation, and the non-executable snapshot library");

    auto changedConnection = valuesOnly;
    changedConnection.graph.connections.front().fromPort =
        "differentPort";
    require(
        talas::runtime::snapshotExecutionTopologyFingerprint(base)
            != talas::runtime::snapshotExecutionTopologyFingerprint(
                changedConnection),
        "topology fingerprint should retain exact connection endpoints");

    auto changedType = valuesOnly;
    changedType.graph.nodes.front().typeId = "talas.noise";
    require(
        talas::runtime::snapshotExecutionTopologyFingerprint(base)
            != talas::runtime::snapshotExecutionTopologyFingerprint(
                changedType),
        "topology fingerprint should retain node type identity");

    auto changedControlShape = valuesOnly;
    changedControlShape.virtualControls.front().maximum = 2.0;
    require(
        talas::runtime::snapshotExecutionTopologyFingerprint(base)
            != talas::runtime::snapshotExecutionTopologyFingerprint(
                changedControlShape),
        "topology fingerprint should retain Virtual Control domain and range shape");
}

void testPreparedExecutionFingerprintSeparatesValuesFromRuntimeShape()
{
    talas::runtime::RuntimePlan plan;
    talas::runtime::CompileStep step;
    step.nodeId = "osc";
    step.typeId = "talas.oscillator";
    talas::runtime::ParameterValue amplitude;
    amplitude.id = "amplitude";
    amplitude.value = 0.25;
    step.parameters.push_back(amplitude);
    plan.steps.push_back(step);
    plan.buffers.push_back({
        "osc",
        "audioOut",
        talas::core::PortType::Audio,
        64,
    });
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 64;
    const talas::runtime::PreparedRuntimeGraph base{plan, settings};

    auto valueOnlyPlan = plan;
    valueOnlyPlan.steps.front().parameters.front().value = 0.75;
    valueOnlyPlan.steps.front().parameters.front().boolean = true;
    const talas::runtime::PreparedRuntimeGraph valuesOnly{
        valueOnlyPlan,
        settings};
    require(
        talas::runtime::snapshotPreparedExecutionTopologyFingerprint(base)
            == talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
                valuesOnly),
        "prepared fingerprint should ignore live numeric/choice/boolean parameter payloads");

    auto changedSupportPlan = valueOnlyPlan;
    changedSupportPlan.steps.front().offlineSupport =
        talas::core::ExecutionSupport::Subset;
    changedSupportPlan.steps.front().preparedRealtimeSupport =
        talas::core::ExecutionSupport::Supported;
    const talas::runtime::PreparedRuntimeGraph changedSupport{
        changedSupportPlan,
        settings};
    require(
        talas::runtime::snapshotPreparedExecutionTopologyFingerprint(base)
            == talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
                changedSupport),
        "prepared fingerprint should ignore support metadata when bindings and kernel topology are unchanged");

    auto changedBufferPlan = valueOnlyPlan;
    changedBufferPlan.buffers.front().frames = 128;
    const talas::runtime::PreparedRuntimeGraph changedBuffer{
        changedBufferPlan,
        settings};
    require(
        talas::runtime::snapshotPreparedExecutionTopologyFingerprint(base)
            != talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
                changedBuffer),
        "prepared fingerprint should retain exact buffer preparation shape");

    auto changedResourcePlan = valueOnlyPlan;
    changedResourcePlan.steps.front().parameters.front().resource =
        "differentAsset";
    const talas::runtime::PreparedRuntimeGraph changedResource{
        changedResourcePlan,
        settings};
    require(
        talas::runtime::snapshotPreparedExecutionTopologyFingerprint(base)
            != talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
                changedResource),
        "prepared fingerprint should retain resource identities");
}

void testPerformanceOverlayCompilesAgainstAnEphemeralParameterCandidate()
{
    const auto document = makeDirectRampProject();
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    talas::project::SnapshotValueEntry amplitude;
    amplitude.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    amplitude.target.nodeId = "osc";
    amplitude.target.parameterId = "amplitude";
    amplitude.value.number = 0.5;
    const auto compiled =
        talas::runtime::compileProjectPerformanceOverlay(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings),
            std::span<const talas::project::SnapshotValueEntry>(
                &amplitude,
                1));
    auto baselineSettings = talas::runtime::RenderSettings{};
    baselineSettings.sampleRate = 4.0;
    baselineSettings.maxBlockFrames = 4;
    baselineSettings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    const auto baseline = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(baselineSettings));
    require(
        compiled.ok() && baseline.ok()
            && document.graph.nodes.front()
                   .parameters.back()
                   .expression
                == std::optional<std::string>{"!Gain"},
        "overlay compilation should prepare an ephemeral parameter candidate without mutating authoring bytes");
    require(
        talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
            *baseline.graph)
            == talas::runtime::snapshotPreparedExecutionTopologyFingerprint(
                *compiled.graph),
        "ordinary node-value overlay should preserve the prepared execution topology");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    std::array<double, 4> output{};
    require(
        session.ready()
            && session.render(output)
            && std::abs(output[1] - 0.5) < 1.0e-12,
        "compiled node-parameter overlay should become the prepared baseline");
}

void testPerformanceOverlayTargetsOnlyOneConcreteSoundInstance()
{
    const auto document = makeTwoInstanceBoundSoundProject();
    talas::project::SnapshotValueEntry frequency;
    frequency.target = soundFrequencyIdentity();
    frequency.target.instancePath = {"voiceA"};
    frequency.value.domain = talas::expr::ValueDomain::Hertz;
    frequency.value.number = 2.0;
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    const auto compiled =
        talas::runtime::compileProjectPerformanceOverlay(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings),
            std::span<const talas::project::SnapshotValueEntry>(
                &frequency,
                1));
    require(
        compiled.ok()
            && document.graph.nodes.front().parameters.front().expression
                == std::optional<std::string>{"1 hz"}
            && document.graph.nodes[1].parameters.front().expression
                == std::optional<std::string>{"1 hz"},
        "Sound overlay should compile from an ephemeral concrete-instance candidate only");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    std::array<double, 4> output{};
    const auto expectedFirstAudibleSample =
        0.25 + 0.25 * std::sqrt(0.5);
    require(
        session.ready()
            && session.render(output)
            && std::abs(output[1] - expectedFirstAudibleSample)
                < 1.0e-12,
        "overlaying voiceA must leave voiceB at its independent one-hertz baseline");
}

void testPerformanceOverlayRejectsTheCompleteInvalidCandidate()
{
    const auto document = makeDirectRampProject();
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.eventValues.push_back({
        "Gain",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    talas::project::SnapshotValueEntry amplitude;
    amplitude.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    amplitude.target.nodeId = "osc";
    amplitude.target.parameterId = "amplitude";
    amplitude.value.number = 0.5;
    const std::array duplicate{amplitude, amplitude};
    const auto duplicateResult =
        talas::runtime::compileProjectPerformanceOverlay(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            settings,
            duplicate);
    require(
        !duplicateResult.ok()
            && !duplicateResult.graph.has_value()
            && duplicateResult.diagnostics.size() == 1,
        "duplicate overlay targets must reject the complete candidate");

    amplitude.value.domain = talas::expr::ValueDomain::Seconds;
    const auto wrongDomainResult =
        talas::runtime::compileProjectPerformanceOverlay(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings),
            std::span<const talas::project::SnapshotValueEntry>(
                &amplitude,
                1));
    require(
        !wrongDomainResult.ok()
            && !wrongDomainResult.graph.has_value()
            && document.graph.nodes.front().parameters.back().expression
                == std::optional<std::string>{"!Gain"},
        "kind/domain failure must reject atomically without touching authoring bytes");
}

void testPerformanceOverlayBoundsTheCompleteCrossBankLiveState()
{
    auto document = makeOverlayGainProject();
    std::vector<talas::project::SnapshotValueEntry> values;
    constexpr std::size_t targetCount =
        talas::project::maximumSnapshotValuesPerSnapshot + 1;
    values.reserve(targetCount);
    for (std::size_t index = 0; index < targetCount; ++index)
    {
        const auto nodeId = "live" + std::to_string(index);
        document.graph.nodes.push_back({
            nodeId,
            "talas.constant",
            {expression("value", "0")},
        });
        talas::project::SnapshotValueEntry value;
        value.target.kind =
            talas::project::SnapshotTargetKind::NodeParameter;
        value.target.nodeId = nodeId;
        value.target.parameterId = "value";
        value.value.number = 0.5;
        values.push_back(std::move(value));
    }
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    const auto compiled =
        talas::runtime::compileProjectPerformanceOverlay(
            document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(settings),
            values);
    require(compiled.ok(),
        "the complete accumulated live overlay may exceed one snapshot while remaining bounded by the semantic target universe");
}

void testBridgeStagesOverlayThroughABoundedSameTopologyTransition()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto authoring = makeOverlayGainProject();
    const auto authoringBytes =
        talas::project::serializeProjectDocument(authoring);
    talas::runtime::ProjectRuntimeBridge bridge(authoring, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "overlay bridge baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "overlay bridge baseline should activate");

    talas::project::SnapshotValueEntry gain;
    gain.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    gain.target.nodeId = "vca";
    gain.target.parameterId = "gain";
    gain.value.number = 1.0;
    const auto staged = bridge.compileAndStagePerformanceOverlay(
        std::span<const talas::project::SnapshotValueEntry>(&gain, 1),
        1.0);
    require(staged.ok()
            && talas::project::serializeProjectDocument(
                   bridge.editor().document()) == authoringBytes
            && !bridge.editor().canUndo(),
        "performance overlay should stage without authoring bytes or undo mutation");
    std::size_t renderAllocations = 0;
    bool rendered = false;
    {
        AllocationCounter counter;
        rendered = bridge.render(&outputPointer, 1, output.size());
        renderAllocations = counter.count();
    }
    require(rendered && renderAllocations == 0,
        "overlay transition callback must use only prepared storage");
    require(
        std::abs(output[0] - 0.25F) < 1.0e-6F
            && std::abs(output[1] - 0.5F) < 1.0e-6F
            && std::abs(output[2] - 0.75F) < 1.0e-6F
            && std::abs(output[3] - 1.0F) < 1.0e-6F,
        "bridge overlay should crossfade linearly and land exactly on the candidate");
    require(bridge.collectRetired(),
        "completed bridge overlay should retire its baseline engine");
}

void testBridgeRejectsOverlayThatChangesPreparedExecutionShape()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeOverlayFftProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "FFT overlay baseline should stage");
    talas::project::SnapshotValueEntry length;
    length.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    length.target.nodeId = "fft";
    length.target.parameterId = "length";
    length.value.number = 128.0;
    const auto rejected = bridge.compileAndStagePerformanceOverlay(
        std::span<const talas::project::SnapshotValueEntry>(&length, 1),
        0.25);
    require(
        rejected.outcome
                == talas::runtime::ProjectStageOutcome::TopologyMismatch
            && !rejected.ok()
            && !bridge.editor().canUndo(),
        "overlay that changes FFT buffers or latency must reject before stage publication");
}

void testBridgeCoalescesRapidOverlaysToTheNewestCompleteCandidate()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeOverlayGainProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "coalescing baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "coalescing baseline should activate");

    const auto makeGain = [](double value) {
        talas::project::SnapshotValueEntry gain;
        gain.target.kind =
            talas::project::SnapshotTargetKind::NodeParameter;
        gain.target.nodeId = "vca";
        gain.target.parameterId = "gain";
        gain.value.number = value;
        return gain;
    };
    auto gain = makeGain(0.25);
    require(bridge.compileAndStagePerformanceOverlay(
            std::span<const talas::project::SnapshotValueEntry>(&gain, 1),
            2.0).outcome
            == talas::runtime::ProjectStageOutcome::Staged,
        "first long overlay should begin its transition");
    require(bridge.render(&outputPointer, 1, output.size()),
        "first half of the long overlay should render");

    gain = makeGain(0.5);
    require(bridge.compileAndStagePerformanceOverlay(
            std::span<const talas::project::SnapshotValueEntry>(&gain, 1),
            2.0).outcome
            == talas::runtime::ProjectStageOutcome::OverlayCoalesced,
        "first rapid update should occupy the bounded coalesced slot");
    gain = makeGain(1.0);
    require(bridge.compileAndStagePerformanceOverlay(
            std::span<const talas::project::SnapshotValueEntry>(&gain, 1),
            2.0).outcome
            == talas::runtime::ProjectStageOutcome::OverlayCoalesced,
        "newer rapid update should replace the older coalesced candidate");
    require(bridge.render(&outputPointer, 1, output.size()),
        "first transition should finish before coalesced adoption");
    require(bridge.collectRetired(),
        "completion-token service should collect and stage the newest candidate");

    require(bridge.render(&outputPointer, 1, output.size()),
        "newest coalesced transition should render its first half");
    require(
        std::abs(output[0] - 0.34375F) < 1.0e-6F
            && std::abs(output[1] - 0.4375F) < 1.0e-6F
            && std::abs(output[2] - 0.53125F) < 1.0e-6F
            && std::abs(output[3] - 0.625F) < 1.0e-6F,
        "coalesced transition should move directly from 0.25 to the newest 1.0 candidate");
    require(bridge.render(&outputPointer, 1, output.size())
            && std::abs(output.back() - 1.0F) < 1.0e-6F,
        "newest coalesced transition should land exactly on one");
    require(bridge.collectRetired(),
        "newest coalesced transition should retire its predecessor");
}

void testBridgeMirrorsRealtimeIngressAcrossAnOverlayTransition()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeOverlayIngressProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "overlay ingress baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "overlay ingress baseline should activate");
    const auto originalStage = bridge.latestStageGeneration();
    const auto gainId = bridge.realtimeEventValueId("Gain");
    require(gainId.has_value()
            && bridge.enqueueEventValueAtTimeForStage(
                *gainId,
                0.5,
                talas::expr::ValueDomain::Scalar,
                0.0,
                originalStage),
        "old-stage overlay ingress should queue before replacement");

    talas::project::SnapshotValueEntry ignoredVcaParameter;
    ignoredVcaParameter.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    ignoredVcaParameter.target.nodeId = "vca";
    ignoredVcaParameter.target.parameterId = "gain";
    ignoredVcaParameter.value.number = 0.5;
    require(bridge.compileAndStagePerformanceOverlay(
            std::span<const talas::project::SnapshotValueEntry>(
                &ignoredVcaParameter,
                1),
            2.0).ok(),
        "overlay ingress transition should stage");
    require(bridge.render(&outputPointer, 1, output.size()),
        "overlay ingress transition first half should render");
    require(std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 1.0F) < 1.0e-6F;
            })
            && bridge.realtimeControlTelemetry()
                    .staleGenerationOrStage == 1,
        "a prior-stage command must reach neither side of the replacement crossfade");
    require(gainId.has_value()
            && bridge.enqueueEventValue(
                *gainId,
                0.5,
                talas::expr::ValueDomain::Scalar,
                0),
        "overlay ingress Event Value should enqueue against the latest stage");
    std::size_t allocations = 0;
    bool rendered = false;
    {
        AllocationCounter counter;
        rendered = bridge.render(
            &outputPointer,
            1,
            output.size());
        allocations = counter.count();
    }
    require(rendered && allocations == 0,
        "mirrored overlay ingress should remain callback-allocation free");
    require(std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 0.5F) < 1.0e-6F;
            }),
        "both transition engines should apply the same ingress value at the same sample boundary");
    require(bridge.collectRetired(),
        "mirrored overlay ingress transition should retire normally");
}

void testSnapshotPerformanceControllerRecallMorphRandomResetAndCapture()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto authoring = makeSnapshotControllerProject();
    const auto authoringBytes =
        talas::project::serializeProjectDocument(authoring);
    talas::runtime::ProjectRuntimeBridge bridge(authoring, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "snapshot controller baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "snapshot controller baseline should activate");

    const auto recalled = bridge.recallSnapshot("gainBank", "loud");
    require(
        recalled.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    StagedOverlay
            && recalled.ok()
            && bridge.performanceState().activeBankId == "gainBank"
            && bridge.performanceState().activeSnapshotId == "loud"
            && !bridge.editor().canUndo()
            && talas::project::serializeProjectDocument(
                   bridge.editor().document()) == authoringBytes,
        "recall should publish an overlay while preserving project bytes and undo");
    require(bridge.render(&outputPointer, 1, output.size())
            && std::abs(output[0] - 0.25F) < 1.0e-6F
            && std::abs(output[3] - 1.0F) < 1.0e-6F,
        "recall should use bank smoothing and land on the stored value");
    require(bridge.collectRetired(),
        "recalled overlay should retire the baseline");

    const auto beforeFailure = bridge.performanceState();
    const auto missing = bridge.recallSnapshot("gainBank", "missing");
    require(!missing.ok()
            && bridge.performanceState() == beforeFailure,
        "failed recall should leave the complete performance state unchanged");

    require(bridge.resetSnapshotPerformance().ok(),
        "performance reset should stage the project baseline");
    require(bridge.render(&outputPointer, 1, output.size())
            && std::ranges::all_of(output, [](float sample) {
                return std::abs(sample) < 1.0e-6F;
            }),
        "performance reset should restore the authoring baseline");
    require(bridge.collectRetired(),
        "performance reset should retire the live overlay");

    require(bridge.beginSnapshotMorph(
            "gainBank",
            "quiet",
            "loud").ok(),
        "compatible morph endpoints should begin one gesture");
    const auto morphed = bridge.updateSnapshotMorph(0.5);
    require(morphed.ok()
            && bridge.performanceState().morphPosition == 0.5,
        "morph update should publish its pure midpoint state");
    require(bridge.render(&outputPointer, 1, output.size())
            && std::abs(output.back() - 0.5F) < 1.0e-6F,
        "morph midpoint should become audible through bank smoothing");
    bridge.endSnapshotMorph();
    require(!bridge.performanceState().morphActive,
        "ending morph should leave its value active but close the gesture");
    require(bridge.collectRetired(),
        "morph transition should retire its baseline");

    const auto randomized = bridge.randomizeSnapshotBank(
        "gainBank",
        0x123456789abcdef0ULL);
    require(randomized.ok()
            && bridge.performanceState().lastRandomSeed
                == 0x123456789abcdef0ULL,
        "randomize should publish and retain its explicit deterministic seed");
    require(bridge.render(&outputPointer, 1, output.size()),
        "randomized performance state should render");
    require(bridge.collectRetired(),
        "randomized transition should retire its predecessor");

    const auto liveBeforeCapture = bridge.performanceState();
    const auto captured = bridge.captureSnapshotPerformance(
        "gainBank",
        "quiet");
    require(captured.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    CapturedProjectEdit
            && captured.ok()
            && bridge.editor().canUndo()
            && bridge.performanceState() == liveBeforeCapture,
        "explicit capture should create one project edit without altering live state");
    require(bridge.editor().undo().ok()
            && !bridge.editor().canUndo()
            && bridge.performanceState() == liveBeforeCapture,
        "explicit capture should be exactly one undoable project transaction without altering live state");
}

void testCurrentScopeCaptureRequiresTheAcceptedEffectivePerformanceState()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeSnapshotControllerProject(),
        registry);
    talas::project::SnapshotScopeIdentity scope;
    scope.kind = talas::project::SnapshotScopeKind::Node;
    scope.nodeId = "vca";
    require(
        !bridge.snapshotPerformanceDocumentCurrent()
            && !bridge.currentSnapshotScopeValues(scope).ok(),
        "scope capture before an accepted stage must reject instead of inventing descriptor defaults");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "current-scope fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "current-scope fixture should activate");
    const auto baseline = bridge.currentSnapshotScopeValues(scope);
    require(
        bridge.snapshotPerformanceDocumentCurrent()
            && baseline.ok()
            && baseline.values.size() == 1
            && baseline.values.front().value.number == 0.0,
        "scope capture should return the complete accepted baseline value");
    require(bridge.recallSnapshot("gainBank", "loud").ok(),
        "current-scope fixture should recall a live overlay");
    const auto live = bridge.currentSnapshotScopeValues(scope);
    require(
        live.ok()
            && live.values.size() == 1
            && live.values.front().value.number == 1.0,
        "scope capture should prefer the current accepted live performance value");
    require(
        bridge.editor().setParameter(
            "vca",
            expression("gain", "0.5")).ok(),
        "current-scope fixture should create an unstaged authoring edit");
    require(
        !bridge.snapshotPerformanceDocumentCurrent()
            && !bridge.currentSnapshotScopeValues(scope).ok(),
        "scope capture after an unstaged edit must reject stale runtime values");
}

void testSnapshotPerformanceControllerUsesCompleteDirectTransactions()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto authoring = makeDirectSnapshotControllerProject();
    const auto authoringBytes =
        talas::project::serializeProjectDocument(authoring);
    talas::runtime::ProjectRuntimeBridge bridge(authoring, registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 8;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "direct snapshot controller baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 8> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "direct snapshot controller baseline should activate");

    const auto recalled = bridge.recallSnapshot(
        "frequencyBank",
        "two");
    const auto* current = performanceValue(
        bridge.performanceState(),
        soundFrequencyIdentity());
    require(
        recalled.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    AppliedDirect
            && current != nullptr
            && current->value.domain
                == talas::expr::ValueDomain::Hertz
            && current->value.number == 2.0
            && !bridge.editor().canUndo()
            && talas::project::serializeProjectDocument(
                   bridge.editor().document()) == authoringBytes,
        "a fully direct Sound snapshot should publish one complete transaction without touching authoring state");
}

void testDirectActionSupersedesACoalescedOverlayAsOneCompleteCandidate()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeMixedSnapshotControllerProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "mixed snapshot baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "mixed snapshot baseline should activate");

    require(bridge.recallSnapshot("gainBank", "loud").ok(),
        "first overlay action should begin a long transition");
    require(bridge.render(&outputPointer, 1, output.size()),
        "first half of the long transition should render");
    require(bridge.recallSnapshot("gainBank", "quiet").ok()
            && bridge.snapshotTransitionCoalesced(),
        "second overlay action should occupy the coalesced slot");

    const auto directDuringCoalescing = bridge.recallSnapshot(
        "frequencyBank",
        "two");
    require(
        directDuringCoalescing.ok()
            && directDuringCoalescing.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    CoalescedOverlay
            && bridge.snapshotTransitionCoalesced(),
        "a direct-eligible action during coalescing must replace the slot with one complete overlay candidate instead of being lost: outcome="
            + std::to_string(static_cast<int>(
                directDuringCoalescing.outcome))
            + "; message=" + directDuringCoalescing.message);

    require(bridge.render(&outputPointer, 1, output.size()),
        "first transition should finish");
    require(bridge.collectRetired(),
        "completion should stage the complete newest candidate");
    require(bridge.render(&outputPointer, 1, output.size())
            && bridge.render(&outputPointer, 1, output.size()),
        "replacement transition should finish");
    require(bridge.collectRetired(),
        "replacement transition should retire normally");
    const auto* frequency = performanceValue(
        bridge.performanceState(),
        soundFrequencyIdentity());
    require(
        frequency != nullptr
            && frequency->value.number == 2.0
            && bridge.recallSnapshot("frequencyBank", "one").outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    AppliedDirect,
        "the staged candidate and subsequent direct path must agree on the accepted live frequency");
}

void testSnapshotPerformanceControllerPreservesOtherLiveBanks()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeLayeredSnapshotControllerProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "layered snapshot baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "layered snapshot baseline should activate");
    require(bridge.recallSnapshot("firstBank", "active").ok()
            && bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "first live bank should stage and retire normally");
    require(bridge.recallSnapshot("secondBank", "active").ok()
            && bridge.render(&outputPointer, 1, output.size())
            && std::ranges::all_of(output, [](float sample) {
                return std::abs(sample - 0.5F) < 1.0e-6F;
            }),
        "a second bank overlay should preserve the first bank's live value in the complete runtime candidate");
}

void testSnapshotPerformanceControllerRebasesParameterOnlyStages()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeSnapshotControllerProject(),
        registry);
    const auto makeSettings = [] {
        auto settings = talas::runtime::RenderSettings{};
        settings.sampleRate = 4.0;
        settings.maxBlockFrames = 4;
        return settings;
    };
    require(bridge.compileAndStage(makeSettings()).ok(),
        "rebase baseline should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "rebase baseline should activate");
    require(bridge.recallSnapshot("gainBank", "loud").ok()
            && bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "rebase fixture should establish one live overlay");

    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    talas::project::SnapshotTargetIdentity centerTarget;
    centerTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    centerTarget.nodeId = "source";
    centerTarget.parameterId = "centerValue";
    const auto revisionBeforeEdit =
        bridge.performanceState().projectRevision;

    require(bridge.editor().setParameter(
            "source",
            expression("centerValue", "0.25")).ok(),
        "unrelated parameter edit should succeed");
    const auto beforeStaleRecall = bridge.performanceState();
    require(!bridge.recallSnapshot("gainBank", "quiet").ok()
            && bridge.performanceState() == beforeStaleRecall,
        "an unstaged project edit should reject performance work atomically");
    const auto rebased = bridge.compileAndStage(makeSettings());
    const auto* carriedGain = performanceValue(
        bridge.performanceState(),
        gainTarget);
    const auto* editedCenter = performanceValue(
        bridge.performanceState(),
        centerTarget);
    require(rebased.ok()
            && carriedGain != nullptr
            && carriedGain->value.number == 1.0
            && editedCenter != nullptr
            && editedCenter->value.number == 0.25
            && bridge.performanceState().activeBankId == "gainBank"
            && bridge.performanceState().activeSnapshotId == "loud"
            && bridge.performanceState().projectRevision
                > revisionBeforeEdit,
        "a parameter-only stage should rebase edited baselines while preserving unaffected live targets");
    require(bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "the rebased parameter-only stage should activate and retire normally");

    require(bridge.editor().setParameter(
            "vca",
            expression("gain", "0.5")).ok(),
        "conflicting live-target edit should succeed");
    const auto conflicted = bridge.compileAndStage(makeSettings());
    const auto* invalidatedGain = performanceValue(
        bridge.performanceState(),
        gainTarget);
    require(conflicted.ok()
            && invalidatedGain != nullptr
            && invalidatedGain->value.number == 0.5
            && bridge.performanceState().activeSnapshotId.empty()
            && conflicted.message.find("invalidated 1")
                != std::string::npos,
        "a parameter edit that conflicts with a live value should explicitly invalidate that target");
    require(bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "the conflict-invalidated stage should activate normally");

    require(bridge.editor().addNode({
            "extra",
            "talas.constant",
            {expression("value", "1")},
        }).ok(),
        "structural lifecycle edit should succeed");
    const auto structural = bridge.compileAndStage(makeSettings());
    require(structural.ok()
            && bridge.performanceState().activeBankId.empty()
            && bridge.performanceState().activeSnapshotId.empty(),
        "a structural stage should start a fresh baseline performance state");
    require(bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "the fresh structural baseline should activate normally");

    auto replacement = makeSnapshotControllerProject();
    replacement.metadata.title = "Opened replacement";
    require(bridge.editor().replaceDocument(
            std::move(replacement)).ok(),
        "document replacement fixture should adopt a new authoring document");
    bridge.resetResourceIdentityHistory();
    require(bridge.performanceState().currentValues.empty()
            && !bridge.recallSnapshot("gainBank", "loud").ok(),
        "New/Open/Recovery identity reset should invalidate the previous live controller before staging");
    require(bridge.compileAndStage(makeSettings()).ok(),
        "replacement project should stage from a fresh baseline");
    const auto* replacementGain = performanceValue(
        bridge.performanceState(),
        gainTarget);
    require(replacementGain != nullptr
            && replacementGain->value.number == 0.0
            && bridge.performanceState().activeBankId.empty(),
        "New/Open/Recovery should never carry a same-topology live overlay into the replacement project");
}

void testSnapshotPerformanceControllerHonorsLocksAndRandomMerge()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto lockedDocument = makeSnapshotControllerProject();
    lockedDocument.snapshotLibrary.banks.front()
        .parameterPolicies.front().locked = true;
    talas::runtime::ProjectRuntimeBridge lockedBridge(
        std::move(lockedDocument),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(lockedBridge.compileAndStage(settings).ok(),
        "locked controller fixture should stage");
    lockedBridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(lockedBridge.render(&outputPointer, 1, output.size()),
        "locked controller fixture should activate");
    require(lockedBridge.recallSnapshot("gainBank", "loud").ok(),
        "locked recall should remain a valid complete action");
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    const auto* held = performanceValue(
        lockedBridge.performanceState(),
        gainTarget);
    require(held != nullptr && held->value.number == 0.0,
        "locked recall should intentionally hold the current live value");

    talas::runtime::ProjectRuntimeBridge randomBridge(
        makeSnapshotControllerProject(),
        registry);
    require(randomBridge.compileAndStage(std::move(settings)).ok(),
        "random-merge controller fixture should stage");
    randomBridge.setPlaying(true);
    require(randomBridge.render(&outputPointer, 1, output.size()),
        "random-merge controller fixture should activate");
    const auto merged = randomBridge.randomMergeSnapshotBank(
        "gainBank",
        0xfeedfaceULL,
        1.0);
    const auto* randomized = performanceValue(
        randomBridge.performanceState(),
        gainTarget);
    require(merged.ok()
            && randomized != nullptr
            && randomized->value.number >= 0.0
            && randomized->value.number <= 1.0
            && randomBridge.performanceState().lastRandomSeed
                == 0xfeedfaceULL
            && !randomBridge.editor().canUndo(),
        "random merge should publish a bounded deterministic live value without project edits");
}

void testSnapshotPerformanceControllerRejectsStageAndRangeFailuresAtomically()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeSnapshotControllerProject(),
        registry);
    const auto makeSettings = [] {
        auto settings = talas::runtime::RenderSettings{};
        settings.sampleRate = 4.0;
        settings.maxBlockFrames = 4;
        return settings;
    };
    require(bridge.compileAndStage(makeSettings()).ok(),
        "failure-atomic controller fixture should prepare a pending baseline");
    const auto beforeBusyStage = bridge.performanceState();
    const auto busyRecall = bridge.recallSnapshot(
        "gainBank",
        "loud");
    require(!busyRecall.ok()
            && bridge.performanceState() == beforeBusyStage,
        "a host stage rejection should leave the complete controller state unchanged");

    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "failure-atomic controller baseline should activate");
    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    gainTarget.nodeId = "vca";
    gainTarget.parameterId = "gain";
    require(bridge.editor().setSnapshotParameterPolicy(
            "gainBank",
            {gainTarget, false, std::nullopt}).ok(),
        "missing-range fixture should remove the explicit numeric range");
    require(bridge.compileAndStage(makeSettings()).ok()
            && bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "missing-range project edit should stage normally before the live action");
    const auto beforeMissingRange = bridge.performanceState();
    const auto missingRange = bridge.randomizeSnapshotBank(
        "gainBank",
        1);
    require(!missingRange.ok()
            && !missingRange.diagnostics.empty()
            && missingRange.diagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::
                    MissingRandomRange
            && bridge.performanceState() == beforeMissingRange,
        "missing numeric ranges should reject randomization without inventing defaults or mutating live state");
}

void testLockToggleWithoutPolicyKeepsTheBankRandomizable()
{
    auto document = makeDirectEventValueProject();
    // Pitch is reserved for the keyboard's note-number Event Value.
    document.virtualControls.front().eventName = "Cutoff";
    document.graph.nodes.front().parameters.front().expression = "!Cutoff";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "pitchBank";
    bank.displayName = "Pitch";
    bank.scope.kind = talas::project::SnapshotScopeKind::Project;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        document, registry, bank.scope);
    const auto initialized = talas::project::initializeSnapshotScopeValues(
        resolved.targets);
    require(resolved.ok() && initialized.ok(),
        "lock-toggle fixture should resolve complete defaults");
    talas::project::SnapshotTargetIdentity pitch;
    pitch.kind = talas::project::SnapshotTargetKind::EventValue;
    pitch.eventName = "Cutoff";
    pitch.domain = talas::expr::ValueDomain::Hertz;
    for (const auto& target : resolved.targets)
    {
        if (target.target == pitch)
            continue;
        bank.parameterPolicies.push_back({
            target.target,
            true,
            target.valueKind == talas::project::SnapshotValueKind::Number
                ? std::optional<talas::project::SnapshotParameterRange>{{0.0, 1.0}}
                : std::nullopt,
        });
    }
    // Workbench Lock on a target without a policy stores no range.
    bank.parameterPolicies.push_back({pitch, true, std::nullopt});
    bank.snapshots.push_back({"captured", "Captured", initialized.values});
    document.snapshotLibrary.banks.push_back(std::move(bank));

    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry);
    const auto makeSettings = [] {
        auto settings = talas::runtime::RenderSettings{};
        settings.sampleRate = 4.0;
        settings.maxBlockFrames = 4;
        return settings;
    };
    const auto staged = bridge.compileAndStage(makeSettings());
    require(staged.ok(), "lock-toggle fixture should stage: " + staged.message);
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "lock-toggle fixture should activate");
    const auto locked = bridge.randomizeSnapshotBank("pitchBank", 7);
    require(locked.ok(),
        "one locked target without a range must not disable Randomize: "
            + locked.message);
    const auto* held = performanceValue(bridge.performanceState(), pitch);
    require(held != nullptr && held->value.number == 440.0,
        "the locked target should keep its value");

    require(bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "the locked randomization should adopt");
    require(bridge.editor().setSnapshotParameterPolicy(
            "pitchBank", {pitch, false, std::nullopt}).ok()
            && bridge.compileAndStage(makeSettings()).ok()
            && bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "unlocking should stage normally");
    const auto unlocked = bridge.randomizeSnapshotBank("pitchBank", 7);
    const auto* randomized =
        performanceValue(bridge.performanceState(), pitch);
    require(unlocked.ok() && randomized != nullptr
            && randomized->value.number >= 20.0
            && randomized->value.number <= 20000.0,
        "an unlocked target without a stored range should use the control range the Workbench shows");
}

void testSnapshotPerformanceControllerAdoptsOnlyAuthoringLayerEdits()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        makeSnapshotControllerProject(),
        registry);
    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(std::move(settings)).ok(),
        "snapshot authoring adoption fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()),
        "snapshot authoring adoption fixture should activate");
    require(bridge.recallSnapshot("gainBank", "loud").ok()
            && bridge.render(&outputPointer, 1, output.size())
            && bridge.collectRetired(),
        "snapshot authoring adoption should begin with a live state");
    const auto currentBeforeEdit =
        bridge.performanceState().currentValues;
    const auto revisionBeforeEdit =
        bridge.performanceState().projectRevision;
    require(bridge.editor().renameSnapshotBank(
            "gainBank",
            "Renamed Gain Bank").ok(),
        "snapshot authoring-only editor command should succeed");
    const auto adopted = bridge.adoptSnapshotAuthoringEdit();
    require(adopted.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    AdoptedAuthoringEdit
            && adopted.ok()
            && bridge.performanceState().currentValues
                == currentBeforeEdit
            && bridge.performanceState().projectRevision
                > revisionBeforeEdit
            && bridge.performanceState().activeBankId == "gainBank"
            && bridge.performanceState().activeSnapshotId.empty()
            && bridge.recallSnapshot("gainBank", "quiet").ok(),
        "snapshot-library edits should adopt without recompiling, preserve audible values, and re-resolve selection metadata");

    require(bridge.editor().addNode({
            "structural",
            "talas.constant",
            {expression("value", "1")},
        }).ok(),
        "structural adoption rejection fixture should edit the graph");
    const auto beforeStructuralAdoption = bridge.performanceState();
    require(!bridge.adoptSnapshotAuthoringEdit().ok()
            && bridge.performanceState()
                == beforeStructuralAdoption,
        "authoring-layer adoption must reject structural edits without mutating live state");
}
}

int main()
{
    testCompilationPublishesStablePreparedSnapshotTargets();
    testSnapshotDirectTransactionPublishesPreparedVariableValue();
    testSnapshotDirectTransactionPreservesWorkspaceSessionOwner();
    testLiveVariableAssignmentFeedsSnapshotCaptureAuthority();
    testGeneratedControllerHostRouteRespectsManualAndSnapshotValues();
    testGeneratedControllerHostRouteUsesVirtualControlDomain();
    testMixedOverlayRecallCarriesVariableIntoReplacementStage();
    testCompleteSoundEventBindingsPublishOneAtomicDirectTarget();
    testMixedSoundBindingsNeverPublishPartialDirectHandles();
    testNodeBoundSoundTargetRetainsItsExactPreparedBaseline();
    testProjectedSoundEventBindingsRemainOverlayOnly();
    testDirectSnapshotRampIsLinearAndBlockPartitionInvariant();
    testDirectTransactionIsStageScopedAndQueueBounded();
    testDirectTransactionRejectsInvalidMemberWithoutPartialPublication();
    testSnapshotSmoothingFrameConversionIsExactAndSaturating();
    testDirectSnapshotSchedulingAndCallbackDoNotAllocate();
    testBridgeResolvesStableTargetsIntoLatestPreparedTransaction();
    testBridgeFlattensOneSoundValueIntoItsCompleteDirectBindingSet();
    testExecutionTopologyFingerprintSeparatesAuthoringLayersAndStructure();
    testPreparedExecutionFingerprintSeparatesValuesFromRuntimeShape();
    testPerformanceOverlayCompilesAgainstAnEphemeralParameterCandidate();
    testPerformanceOverlayTargetsOnlyOneConcreteSoundInstance();
    testPerformanceOverlayRejectsTheCompleteInvalidCandidate();
    testPerformanceOverlayBoundsTheCompleteCrossBankLiveState();
    testBridgeStagesOverlayThroughABoundedSameTopologyTransition();
    testBridgeRejectsOverlayThatChangesPreparedExecutionShape();
    testBridgeCoalescesRapidOverlaysToTheNewestCompleteCandidate();
    testBridgeMirrorsRealtimeIngressAcrossAnOverlayTransition();
    testSnapshotPerformanceControllerRecallMorphRandomResetAndCapture();
    testCurrentScopeCaptureRequiresTheAcceptedEffectivePerformanceState();
    testSnapshotPerformanceControllerUsesCompleteDirectTransactions();
    testDirectActionSupersedesACoalescedOverlayAsOneCompleteCandidate();
    testSnapshotPerformanceControllerPreservesOtherLiveBanks();
    testSnapshotPerformanceControllerRebasesParameterOnlyStages();
    testSnapshotPerformanceControllerHonorsLocksAndRandomMerge();
    testSnapshotPerformanceControllerRejectsStageAndRangeFailuresAtomically();
    testSnapshotPerformanceControllerAdoptsOnlyAuthoringLayerEdits();
    testLockToggleWithoutPolicyKeepsTheBankRandomizable();
    return 0;
}
