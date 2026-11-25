#include "talas/app/PerformanceRuntimeDispatcher.h"
#include "talas/core/BuiltinNodes.h"

#include <ranges>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(std::string id, std::string source)
{
    talas::project::ProjectParameterExpression value;
    value.parameterId = std::move(id);
    value.expression = std::move(source);
    return value;
}

talas::project::ProjectDocument performanceDocument()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back({
        "MacroA",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        -1.0,
        1.0,
    });
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.25,
        0.0,
        1.0,
    });
    document.virtualControls.push_back({
        "MacroB",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        -1.0,
        1.0,
    });
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain + !MacroA + !MacroB"),
        },
    });
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    document.graph.connections.push_back({"osc", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(document);
    auto* macro = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    const auto* gain = talas::project::findProjectPerformanceControlPresentation(
        document, "Gain", talas::expr::ValueDomain::Scalar);
    auto* macroB = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroB", talas::expr::ValueDomain::Scalar);
    macro->kind = talas::project::ProjectPerformanceControlKind::Macro;
    macro->modulationSlots = {{gain->target, 1.0}};
    macroB->kind = talas::project::ProjectPerformanceControlKind::Macro;
    macroB->modulationSlots = {{gain->target, 0.5}};
    return document;
}

double performanceValue(const talas::runtime::ProjectRuntimeBridge& bridge, std::string_view eventName)
{
    const auto found = std::ranges::find_if(
        bridge.performanceState().currentValues,
        [&](const auto& value)
        {
            return value.target.kind == talas::project::SnapshotTargetKind::EventValue &&
                   value.target.eventName == eventName;
        });
    require(found != bridge.performanceState().currentValues.end(),
            "dispatcher fixture should retain the exact performance target");
    return found->value.number;
}

double performanceValue(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::project::SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        bridge.performanceState().currentValues,
        target,
        &talas::project::SnapshotValueEntry::target);
    require(found != bridge.performanceState().currentValues.end(),
            "dispatcher fixture should retain the exact Sound performance target");
    return found->value.number;
}

talas::project::ProjectDocument soundMacroDocument()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "!Pitch"),
            expression("amplitude", "0.125"),
        },
    });
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("frequency", "180 hz");
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "Pitch",
        talas::expr::ValueDomain::Hertz,
    });
    definition.exposedParameters.push_back(std::move(frequency));

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode voice;
    voice.instanceId = "voice1";
    voice.soundDefinitionId = "voice";
    voice.parameters.push_back(expression("frequency", "180 hz"));
    document.graph.nodes = {
        std::move(voice),
        {"output", "talas.audioOutput", {}},
    };
    document.graph.connections.push_back(
        {"voice1", "audioOut", "output", "audioIn"});
    document.virtualControls.push_back({
        "MacroA",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        -1.0,
        1.0,
    });
    talas::project::reconcileProjectPresentation(document);

    talas::project::SnapshotTargetIdentity soundTarget;
    soundTarget.kind = talas::project::SnapshotTargetKind::SoundParameter;
    soundTarget.instancePath = {"voice1"};
    soundTarget.definitionId = "voice";
    soundTarget.parameterId = "frequency";
    talas::project::ProjectPerformanceControlPresentation soundControl;
    soundControl.controlId = "voice-frequency";
    soundControl.target = soundTarget;
    soundControl.kind = talas::project::ProjectPerformanceControlKind::Knob;
    soundControl.displayName = "Voice frequency";
    document.presentation.performanceControls.push_back(
        std::move(soundControl));
    auto* macro = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    macro->kind = talas::project::ProjectPerformanceControlKind::Macro;
    macro->modulationSlots = {{soundTarget, 0.5}};

    talas::project::SnapshotValue baseline;
    baseline.domain = talas::expr::ValueDomain::Hertz;
    baseline.number = 180.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "voice-frequency-bank";
    bank.displayName = "Voice frequency";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "voice1";
    bank.parameterPolicies.push_back({
        soundTarget,
        false,
        talas::project::SnapshotParameterRange{100.0, 260.0},
    });
    bank.snapshots.push_back({
        "baseline",
        "Baseline",
        {{soundTarget, baseline}},
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

void testDispatcherPublishesOneBoundStageTransactionAndPersistsOnlySource()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(performanceDocument(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    settings.maxRealtimeEventTransactions = 1;
    settings.eventValues = {
        {"MacroA", 0.0, talas::expr::ValueDomain::Scalar},
        {"Gain", 0.25, talas::expr::ValueDomain::Scalar},
        {"MacroB", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(settings)).ok(),
            "Performance dispatcher fixture should stage");
    const auto generation = bridge.latestStageGeneration();
    const auto* macro = talas::project::findProjectPerformanceControlPresentation(
        bridge.editor().document(), "MacroA", talas::expr::ValueDomain::Scalar);
    require(macro != nullptr, "Performance dispatcher should expose MacroA");

    talas::app::PerformanceRuntimeDispatcher dispatcher(bridge);
    const auto accepted = dispatcher.dispatch(macro->controlId, 1.0, generation);
    require(accepted.ok() && accepted.appliedValues.size() == 2 &&
                performanceValue(bridge, "MacroA") == 1.0 &&
                performanceValue(bridge, "Gain") == 0.75 &&
                bridge.editor().document().virtualControls[0].value == 1.0 &&
                bridge.editor().document().virtualControls[1].value == 0.25 &&
                bridge.realtimeControlTelemetry().preparedSnapshotDirectTransactions == 1,
            "one Performance gesture should publish source and macro fan-out in "
            "one transaction while persisting only the source base value");

    const auto beforeStale = bridge.performanceState();
    const auto stale = dispatcher.dispatch(macro->controlId, -1.0, generation - 1);
    require(!stale.ok() && stale.outcome == talas::app::PerformanceDispatchOutcome::StaleAuthority &&
                bridge.performanceState() == beforeStale,
            "stale Performance authority should preserve the accepted state");

    bool observedQueueFull = false;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto beforeAttempt = bridge.performanceState();
        const auto beforeAuthoring = bridge.editor().document().virtualControls[0].value;
        const auto candidate =
            dispatcher.dispatch(macro->controlId, attempt % 2 == 0 ? -1.0 : 1.0, generation);
        if (candidate.ok())
            continue;
        require(candidate.outcome == talas::app::PerformanceDispatchOutcome::RuntimeRejected &&
                    bridge.performanceState() == beforeAttempt &&
                    bridge.editor().document().virtualControls[0].value == beforeAuthoring &&
                    bridge.snapshotPerformanceDocumentCurrent(),
                "rejected Performance admission should preserve runtime and "
                "authoring state");
        observedQueueFull = true;
        break;
    }
    require(observedQueueFull, "queue-full Performance admission should preserve the prior state");
}

void testGestureUsesOnePreparedAuthoringTransaction()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(performanceDocument(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    settings.maxRealtimeEventTransactions = 8;
    settings.eventValues = {
        {"MacroA", 0.0, talas::expr::ValueDomain::Scalar},
        {"Gain", 0.25, talas::expr::ValueDomain::Scalar},
        {"MacroB", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(settings)).ok(), "gesture fixture should stage");
    const auto generation = bridge.latestStageGeneration();
    const auto macroId = talas::project::findProjectPerformanceControlPresentation(
                             bridge.editor().document(), "MacroA", talas::expr::ValueDomain::Scalar)
                             ->controlId;
    talas::app::PerformanceRuntimeDispatcher dispatcher(bridge);

    bridge.editor().beginHistoryTransaction();
    require(dispatcher.dispatch(macroId, 0.5, generation).ok() &&
                dispatcher.dispatch(macroId, -0.5, generation).ok(),
            "one gesture should accept multiple prepared direct updates");
    bridge.editor().endHistoryTransaction();
    require(bridge.editor().canUndo() && bridge.editor().undo().ok() &&
                bridge.editor().document().virtualControls[0].value == 0.0 &&
                !bridge.editor().canUndo(),
            "one Performance gesture should create exactly one authoring undo");
}

void testLayoutEditAdoptsWithoutChangingPreparedStage()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(performanceDocument(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    settings.eventValues = {
        {"MacroA", 0.0, talas::expr::ValueDomain::Scalar},
        {"Gain", 0.25, talas::expr::ValueDomain::Scalar},
        {"MacroB", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(settings)).ok(),
            "layout-adoption fixture should stage");
    const auto generation = bridge.latestStageGeneration();
    const auto macroId = talas::project::findProjectPerformanceControlPresentation(
                             bridge.editor().document(), "MacroA", talas::expr::ValueDomain::Scalar)
                             ->controlId;
    require(bridge.editor().movePerformanceControl(macroId, {320, 224}).ok() &&
                !bridge.snapshotPerformanceDocumentCurrent(),
            "portable layout edit should change authoring bytes without staging");
    talas::app::PerformanceRuntimeDispatcher dispatcher(bridge);
    require(!dispatcher.dispatch(macroId, 0.5, generation).ok(),
            "unadopted layout revision should fail closed");
    require(bridge.adoptSnapshotAuthoringEdit().ok() &&
                bridge.latestStageGeneration() == generation,
            "layout-only authoring should adopt without changing the prepared stage");
    require(dispatcher.dispatch(macroId, 0.5, generation).ok(),
            "adopted layout-only revision should retain direct Performance control");
}

void testSequentialMacrosRecomputeFromPersistedBaseAndAllSources()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(performanceDocument(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    settings.maxRealtimeEventTransactions = 8;
    settings.eventValues = {
        {"MacroA", 0.0, talas::expr::ValueDomain::Scalar},
        {"Gain", 0.25, talas::expr::ValueDomain::Scalar},
        {"MacroB", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(settings)).ok(),
            "multi-macro dispatcher fixture should stage");
    const auto generation = bridge.latestStageGeneration();
    const auto& document = bridge.editor().document();
    const auto macroA =
        talas::project::findProjectPerformanceControlPresentation(
            document, "MacroA", talas::expr::ValueDomain::Scalar)->controlId;
    const auto macroB =
        talas::project::findProjectPerformanceControlPresentation(
            document, "MacroB", talas::expr::ValueDomain::Scalar)->controlId;
    talas::app::PerformanceRuntimeDispatcher dispatcher(bridge);

    require(dispatcher.dispatch(macroA, 0.5, generation).ok()
                && performanceValue(bridge, "Gain") == 0.5,
            "first macro should offset the persisted destination base");
    require(dispatcher.dispatch(macroB, 1.0, generation).ok()
                && performanceValue(bridge, "Gain") == 0.75,
            "second macro should combine with the retained first source");
    require(dispatcher.dispatch(macroA, -0.5, generation).ok()
                && performanceValue(bridge, "Gain") == 0.25
                && bridge.editor().document().virtualControls[1].value == 0.25,
            "revisiting a macro should recompute from the persisted base "
            "without accumulating prior fan-out");
}

void testSoundMacroRecomputesFromPreparedProjectBaseline()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(soundMacroDocument(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 4;
    settings.maxRealtimeEventTransactions = 8;
    settings.eventValues = {
        {"MacroA", 0.0, talas::expr::ValueDomain::Scalar},
    };
    require(bridge.compileAndStage(std::move(settings)).ok(),
            "Sound macro dispatcher fixture should stage");
    require(bridge.recallSnapshot("voice-frequency-bank", "baseline").ok(),
            "Sound macro fixture should select its exact range authority");
    const auto generation = bridge.latestStageGeneration();
    const auto& document = bridge.editor().document();
    const auto* macro = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    const auto* target =
        talas::project::findProjectPerformanceControlPresentationById(
            document, "voice-frequency");
    require(macro != nullptr && target != nullptr,
            "Sound macro fixture should retain both exact controls");
    talas::app::PerformanceRuntimeDispatcher dispatcher(bridge);

    const auto first = dispatcher.dispatch(macro->controlId, 0.5, generation);
    require(first.ok(),
            "Sound macro should dispatch: " + first.message);
    const auto firstValue = performanceValue(bridge, target->target);
    require(firstValue == 200.0,
            "Sound macro should offset the prepared project baseline, got "
                + std::to_string(firstValue));
    const auto second = dispatcher.dispatch(macro->controlId, -0.5, generation);
    require(second.ok(),
            "repeated Sound macro should dispatch: " + second.message);
    const auto secondValue = performanceValue(bridge, target->target);
    require(secondValue == 160.0,
            "repeated Sound macro movement should recompute without accumulating fan-out, got "
                + std::to_string(secondValue));
}
} // namespace

int main()
{
    testDispatcherPublishesOneBoundStageTransactionAndPersistsOnlySource();
    testGestureUsesOnePreparedAuthoringTransaction();
    testLayoutEditAdoptsWithoutChangingPreparedStage();
    testSequentialMacrosRecomputeFromPersistedBaseAndAllSources();
    testSoundMacroRecomputesFromPreparedProjectBaseline();
    return 0;
}
