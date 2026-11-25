#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(
    std::string id,
    bool value)
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

talas::project::ProjectNode soundInstance(
    std::string id,
    std::string definitionId)
{
    talas::project::ProjectNode node;
    node.instanceId = std::move(id);
    node.soundDefinitionId = std::move(definitionId);
    return node;
}

talas::project::ProjectSoundDefinition oscillatorDefinition()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "oscillatorVoice";
    definition.displayName = "Oscillator Voice";
    definition.inputs.push_back({
        "frequency",
        talas::core::PortType::Control,
        "osc",
        "frequency",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {expression("amplitude", "series(0.1, 0.15, 3)[1]")},
        {},
    });
    return definition;
}

talas::project::ProjectSoundDefinition parameterizedOscillatorDefinition()
{
    auto definition = oscillatorDefinition();
    definition.inputs.clear();
    definition.graph.nodes.front().parameters.push_back(
        expression("frequency", "999 hz"));
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("frequency", "120 hz");
    frequency.aliases = {"pitchHz"};
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    });
    definition.exposedParameters.push_back(std::move(frequency));
    return definition;
}

void appendBinaryDefinitions(
    talas::project::ProjectDocument& document,
    std::string prefix,
    std::size_t depth)
{
    for (std::size_t level = 0; level <= depth; ++level)
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId =
            prefix + std::to_string(level);
        definition.displayName = definition.definitionId;
        definition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            level == depth ? "leaf" : "left",
            "audioOut",
        });
        if (level != depth)
        {
            const auto childId =
                prefix + std::to_string(level + 1);
            definition.graph.nodes.push_back(
                soundInstance("left", childId));
            definition.graph.nodes.push_back(
                soundInstance("right", childId));
        }
        else
        {
            definition.graph.nodes.push_back({
                "leaf",
                "talas.oscillator",
                {
                    expression("frequency", "440 hz"),
                    expression("amplitude", "0"),
                },
                {},
            });
        }
        document.soundDefinitions.push_back(
            std::move(definition));
    }
}

talas::project::ProjectDocument makeTwoVoiceProject()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(oscillatorDefinition());
    document.graph.nodes.push_back({
        "frequency",
        "talas.constant",
        {expression("value", "240")},
        {},
    });
    document.graph.nodes.push_back(
        soundInstance("voiceA", "oscillatorVoice"));
    document.graph.nodes.push_back(
        soundInstance("voiceB", "oscillatorVoice"));
    document.graph.nodes.push_back({"mix", "talas.mixer", {}, {}});
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"frequency", "value", "voiceA", "frequency"},
        {"frequency", "value", "voiceB", "frequency"},
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };
    return document;
}

void testSoundExpansionEmitsDeterministicStructuralManifest()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto first = talas::project::expandProjectSounds(
        makeTwoVoiceProject(),
        registry);
    const auto second = talas::project::expandProjectSounds(
        makeTwoVoiceProject(),
        registry);
    require(first.ok() && second.ok(),
            "structural-manifest fixture should expand");
    require(
        first.manifest.nodes == second.manifest.nodes,
        "structural expansion manifests should be deterministic");

    const auto findLocalOscillator = [](const auto& manifest,
                                        std::string_view instanceId) {
        return std::ranges::find_if(
            manifest.nodes,
            [&](const auto& node) {
                return node.instancePath
                        == std::vector<std::string>{
                            std::string(instanceId)}
                    && node.definitionId == "oscillatorVoice"
                    && node.localNodeId == "osc";
            });
    };
    const auto voiceA = findLocalOscillator(
        first.manifest,
        "voiceA");
    const auto voiceB = findLocalOscillator(
        first.manifest,
        "voiceB");
    require(
        voiceA != first.manifest.nodes.end()
            && voiceB != first.manifest.nodes.end(),
        "each expanded instance should retain its structural source identity");
    require(
        voiceA->scopeOrdinal != 0
            && voiceB->scopeOrdinal != 0
            && voiceA->scopeOrdinal != voiceB->scopeOrdinal,
        "ordinary nested Sound instances should receive distinct non-root scopes");
    require(
        voiceA->flatNodeId != voiceB->flatNodeId,
        "structural manifest should map each scope to its unique flat node");

    const auto rootOutput = std::ranges::find_if(
        first.manifest.nodes,
        [](const auto& node) {
            return node.instancePath.empty()
                && node.definitionId.empty()
                && node.localNodeId == "output";
        });
    require(
        rootOutput != first.manifest.nodes.end()
            && rootOutput->scopeOrdinal == 0
            && rootOutput->flatNodeId == "output",
        "root nodes should retain the global structural scope");
}

void testNestedSoundExpansionCreatesIndependentDeterministicSteps()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 16;
    const auto first = talas::runtime::compileProject(
        makeTwoVoiceProject(),
        registry,
        settings);
    const auto second = talas::runtime::compileProject(
        makeTwoVoiceProject(),
        registry,
        settings);
    require(first.ok() && second.ok(),
            "two reusable Sound instances should compile");

    std::vector<std::string> firstOscillators;
    std::vector<std::string> secondOscillators;
    for (const auto& step : first.graph->plan().steps)
    {
        if (step.typeId == "talas.oscillator")
            firstOscillators.push_back(step.nodeId);
    }
    for (const auto& step : second.graph->plan().steps)
    {
        if (step.typeId == "talas.oscillator")
            secondOscillators.push_back(step.nodeId);
    }
    require(
        firstOscillators.size() == 2
            && firstOscillators[0] != firstOscillators[1],
        "each Sound instance should own a distinct runtime node identity");
    require(
        firstOscillators == secondOscillators,
        "hierarchical runtime identities should be deterministic");

    const auto firstOscillatorStep = std::ranges::find_if(
        first.graph->plan().steps,
        [&](const auto& step) {
            return step.nodeId == firstOscillators[0];
        });
    const auto secondOscillatorStep = std::ranges::find_if(
        first.graph->plan().steps,
        [&](const auto& step) {
            return step.nodeId == firstOscillators[1];
        });
    require(
        firstOscillatorStep != first.graph->plan().steps.end()
            && secondOscillatorStep
                != first.graph->plan().steps.end()
            && firstOscillatorStep->scopeId
                != talas::runtime::globalRuntimeScope
            && secondOscillatorStep->scopeId
                != talas::runtime::globalRuntimeScope
            && firstOscillatorStep->scopeId
                != secondOscillatorStep->scopeId,
        "compiled nested instances should retain distinct prepared scopes");
    for (const auto* oscillator : {
             &*firstOscillatorStep,
             &*secondOscillatorStep})
    {
        const auto frequency = std::ranges::find_if(
            oscillator->inputs,
            [](const auto& input) {
                return input.inputPortId == "frequency";
            });
        require(
            frequency != oscillator->inputs.end()
                && frequency->sourceNodeId == "frequency"
                && frequency->sourceScopeId
                    == talas::runtime::globalRuntimeScope,
            "a root-to-nested boundary should bind the source buffer's structural scope");
    }

    const auto rendered =
        talas::runtime::renderOffline(*first.graph, 16);
    require(rendered.ok() && rendered.mono.size() == 16,
            "expanded Sound instances should render offline");
    const auto expectedSecondFrame =
        0.5 * std::sin(2.0 * std::numbers::pi * 240.0 / 48000.0);
    require(
        std::abs(rendered.mono[1] - expectedSecondFrame) < 1.0e-12,
        "two isolated equal-phase instances should sum without shared-state corruption");
}

void testSoundInterfaceDefaultsOverridesAndAliasesExpandIndependently()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(
        parameterizedOscillatorDefinition());
    auto first = soundInstance("voiceA", "oscillatorVoice");
    auto second = soundInstance("voiceB", "oscillatorVoice");
    second.parameters.push_back(
        expression("frequency", "240 hz"));
    document.graph.nodes.push_back(std::move(first));
    document.graph.nodes.push_back(std::move(second));
    document.graph.nodes.push_back({"mix", "talas.mixer", {}, {}});
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        registry,
        settings);
    require(compiled.ok(),
            "Sound defaults and sparse overrides should compile");
    const auto frequencyFor = [&](std::string_view path) {
        const auto step = std::ranges::find_if(
            compiled.graph->plan().steps,
            [&](const auto& candidate) {
                return candidate.structuralSourcePath == path;
            });
        require(step != compiled.graph->plan().steps.end(),
                "expanded Sound oscillator should retain its source path");
        const auto parameter = std::ranges::find_if(
            step->parameters,
            [](const auto& candidate) {
                return candidate.id == "frequency";
            });
        require(parameter != step->parameters.end(),
                "expanded Sound oscillator should receive its bound frequency");
        return parameter->value;
    };
    require(
        std::abs(frequencyFor("voiceA/oscillatorVoice::osc") - 120.0)
                < 1.0e-12
            && std::abs(
                   frequencyFor("voiceB/oscillatorVoice::osc")
                   - 240.0)
                < 1.0e-12,
        "one Sound instance should use the default while another uses its sparse override");

    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    const auto expected = 0.25
            * std::sin(2.0 * std::numbers::pi * 120.0 / 48000.0)
        + 0.25
            * std::sin(2.0 * std::numbers::pi * 240.0 / 48000.0);
    require(
        rendered.ok()
            && std::abs(rendered.mono[1] - expected) < 1.0e-12,
        "default and override should drive isolated prepared instances");

    auto aliasDocument = document;
    aliasDocument.graph.nodes[1].parameters.front().parameterId =
        "pitchHz";
    aliasDocument.graph.nodes[1].parameters.front().expression =
        "330 hz";
    const auto aliasExpansion = talas::project::expandProjectSounds(
        aliasDocument,
        registry);
    require(aliasExpansion.ok(),
            "explicit Sound parameter alias should resolve during expansion");
    const auto aliasedOscillator = std::ranges::find_if(
        aliasExpansion.document.graph.nodes,
        [](const auto& node) {
            return node.instanceId.find("voiceB")
                    != std::string::npos
                && node.typeId == "talas.oscillator";
        });
    require(
        aliasedOscillator != aliasExpansion.document.graph.nodes.end()
            && std::ranges::any_of(
                aliasedOscillator->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "frequency"
                        && parameter.expression
                            == std::optional<std::string>{"330 hz"};
                }),
        "alias resolution should bind the canonical internal parameter without persisting alias identity downstream");

    auto unknownDocument = document;
    unknownDocument.graph.nodes[1].parameters.front().parameterId =
        "removedParameter";
    const auto unknownExpansion = talas::project::expandProjectSounds(
        unknownDocument,
        registry);
    require(
        !unknownExpansion.ok()
            && unknownExpansion.document.graph.nodes.empty()
            && unknownExpansion.manifest.nodes.empty(),
        "unknown recoverable Sound overrides should fail expansion transactionally until an alias resolves them");

    auto wrongDomainDocument = document;
    wrongDomainDocument.graph.nodes[1].parameters.front().expression =
        "1 s";
    const auto wrongDomainExpansion =
        talas::project::expandProjectSounds(
            wrongDomainDocument,
            registry);
    require(
        !wrongDomainExpansion.ok(),
        "Sound instance override domain mismatch should fail before expansion");
}

void testLocalSoundEventValueBindingsAreIsolatedPerInstanceScope()
{
    talas::project::ProjectDocument document;
    auto definition = parameterizedOscillatorDefinition();
    definition.graph.nodes.front().parameters.back().expression =
        "!LocalPitch";
    definition.exposedParameters.front().bindings = {{
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "LocalPitch",
        talas::expr::ValueDomain::Hertz,
    }};
    document.soundDefinitions.push_back(std::move(definition));

    auto first = soundInstance("voiceA", "oscillatorVoice");
    auto second = soundInstance("voiceB", "oscillatorVoice");
    second.parameters.push_back(
        expression("frequency", "240 hz"));
    document.graph.nodes.push_back(std::move(first));
    document.graph.nodes.push_back(std::move(second));
    document.graph.nodes.push_back({"mix", "talas.mixer", {}, {}});
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "definition-local Event Values should prepare without a same-named global binding"
            + (compiled.diagnostics.empty()
                   ? std::string{}
                   : ": " + compiled.diagnostics.front().message));

    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    const auto expected = 0.25
            * std::sin(2.0 * std::numbers::pi * 120.0 / 48000.0)
        + 0.25
            * std::sin(2.0 * std::numbers::pi * 240.0 / 48000.0);
    require(
        rendered.ok()
            && std::abs(rendered.mono[1] - expected) < 1.0e-12,
        "same-named local Event Values should resolve independently in each Sound-instance scope");

    const auto scopeFor = [&](std::string_view path) {
        const auto step = std::ranges::find_if(
            compiled.graph->plan().steps,
            [&](const auto& candidate) {
                return candidate.structuralSourcePath == path;
            });
        require(
            step != compiled.graph->plan().steps.end(),
            "local Event Value test should retain structural scope identity");
        return step->eventValueScopeId;
    };
    const auto firstScope = scopeFor(
        "voiceA/oscillatorVoice::osc");
    const auto secondScope = scopeFor(
        "voiceB/oscillatorVoice::osc");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    std::vector<double> realtime(8, 0.0);
    require(
        session.ready() && session.render(realtime)
            && std::abs(realtime[1] - expected) < 1.0e-12,
        "prepared realtime should preserve local Event Value scope isolation");
    const auto firstTarget = session.eventValueTargetIndex(
        firstScope,
        "LocalPitch",
        talas::expr::ValueDomain::Hertz);
    const auto secondTarget = session.eventValueTargetIndex(
        secondScope,
        "LocalPitch",
        talas::expr::ValueDomain::Hertz);
    require(
        firstTarget.has_value()
            && secondTarget.has_value()
            && firstTarget != secondTarget
            && !session.eventValueTargetIndex(
                    "LocalPitch",
                    talas::expr::ValueDomain::Hertz)
                    .has_value(),
        "same-named local bindings should expose distinct scoped typed targets without masquerading as globals");

    talas::runtime::RealtimeAudioEngine engine(*compiled.graph);
    engine.setStageId(41);
    const auto preparedTarget = engine.eventValueTarget(
        firstScope,
        "LocalPitch",
        talas::expr::ValueDomain::Hertz,
        7);
    require(
        preparedTarget.has_value()
            && !engine.eventValueTarget(
                    secondScope + 1024,
                    "LocalPitch",
                    talas::expr::ValueDomain::Hertz,
                    7)
                    .has_value(),
        "prepared local targets should reject an unknown structural scope");
    engine.setStageId(42);
    talas::runtime::TypedEventPayload stalePayload{};
    stalePayload.eventValue = {
        180.0,
        talas::expr::ValueDomain::Hertz,
    };
    const talas::runtime::PreparedTypedEvent staleUpdate{
        talas::runtime::TypedEventKind::EventValueAssignment,
        *preparedTarget,
        0,
        1,
        stalePayload,
    };
    talas::runtime::RealtimeTypedEventDispatchResult staleDispatch{};
    require(
        !engine.scheduleTypedEvents(
            std::span<const talas::runtime::PreparedTypedEvent>(
                &staleUpdate,
                1),
            std::span<talas::runtime::RealtimeTypedEventDispatchResult>(
                &staleDispatch,
                1))
            && staleDispatch
                == talas::runtime::RealtimeTypedEventDispatchResult::Rejected,
        "replacing the prepared stage should invalidate an old local target without touching the new graph");

    talas::runtime::TypedEventPayload payload{};
    payload.eventValue = {
        180.0,
        talas::expr::ValueDomain::Hertz,
    };
    const talas::runtime::PreparedTypedEvent update{
        talas::runtime::TypedEventKind::EventValueAssignment,
        {
            talas::runtime::TypedEventTargetKind::EventValue,
            *firstTarget,
            1,
            1,
        },
        0,
        1,
        payload,
    };
    talas::runtime::RealtimeTypedEventDispatchResult dispatch{};
    require(
        session.scheduleTypedEvents(
            std::span<const talas::runtime::PreparedTypedEvent>(
                &update,
                1),
            std::span<talas::runtime::RealtimeTypedEventDispatchResult>(
                &dispatch,
                1))
            && dispatch
                == talas::runtime::RealtimeTypedEventDispatchResult::Handled,
        "a scoped typed assignment should target one local binding");
    std::ranges::fill(realtime, 0.0);
    const auto expectedScopedUpdate = 0.25
            * std::sin(
                2.0 * std::numbers::pi
                * (120.0 * 8.0 + 180.0)
                / 48000.0)
        + 0.25
            * std::sin(
                2.0 * std::numbers::pi * 240.0 * 9.0
                / 48000.0);
    require(
        session.render(realtime)
            && std::abs(realtime[1] - expectedScopedUpdate)
                < 1.0e-12,
        "updating one local target should not touch another instance with the same local name");
}

void testGlobalToLocalSoundEventValueProjectionRespectsSparseOverrides()
{
    talas::project::ProjectDocument document;
    auto definition = parameterizedOscillatorDefinition();
    definition.graph.nodes.front().parameters.back().expression =
        "!LocalPitch";
    definition.exposedParameters.front().defaultValue.expression =
        "!GlobalPitch";
    definition.exposedParameters.front().bindings = {{
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "LocalPitch",
        talas::expr::ValueDomain::Hertz,
    }};
    document.soundDefinitions.push_back(std::move(definition));
    auto first = soundInstance("voiceA", "oscillatorVoice");
    auto second = soundInstance("voiceB", "oscillatorVoice");
    second.parameters.push_back(
        expression("frequency", "240 hz"));
    document.graph.nodes = {
        std::move(first),
        std::move(second),
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.eventValues.push_back({
        "GlobalPitch",
        120.0,
        talas::expr::ValueDomain::Hertz,
    });
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "an exposed global-to-local Event Value projection should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    std::vector<double> block(8, 0.0);
    const auto initial = 0.25
            * std::sin(2.0 * std::numbers::pi * 120.0 / 48000.0)
        + 0.25
            * std::sin(2.0 * std::numbers::pi * 240.0 / 48000.0);
    require(
        session.ready() && session.render(block)
            && std::abs(block[1] - initial) < 1.0e-12,
        "the default projection and sparse static override should prepare independently");
    require(
        session.scheduleEventValue(
            "GlobalPitch",
            180.0,
            talas::expr::ValueDomain::Hertz,
            0),
        "the explicit global source should remain schedulable");
    std::ranges::fill(block, 0.0);
    const auto updated = 0.25
            * std::sin(
                2.0 * std::numbers::pi
                * (120.0 * 8.0 + 180.0)
                / 48000.0)
        + 0.25
            * std::sin(
                2.0 * std::numbers::pi * 240.0 * 9.0
                / 48000.0);
    require(
        session.render(block)
            && std::abs(block[1] - updated) < 1.0e-12,
        "a global update should refresh the projected instance while leaving the sparse override independent");
}

void testPreparedSoundVariablesRemainInstanceLocalAndTransactional()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    auto definition = parameterizedOscillatorDefinition();
    definition.exposedParameters.front().defaultValue =
        expression("frequency", "@pitch");
    definition.graph.nodes.front().parameters = {
        expression("amplitude", "0.25"),
    };
    document.soundDefinitions.push_back(std::move(definition));
    document.variableLibrary.variables.push_back({
        "pitch",
        "Instance pitch",
        talas::expr::ValueDomain::Hertz,
        240.0,
        {
            talas::project::ProjectVariableOwnerKind::SoundDefinition,
            "oscillatorVoice",
        },
        true,
    });
    document.graph.nodes = {
        soundInstance("voiceA", "oscillatorVoice"),
        soundInstance("voiceB", "oscillatorVoice"),
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok()
            && compiled.graph->preparedVariables().cells.size() == 2,
        "two Sound instances should prepare two independent variable cells"
            + diagnostics);

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    const talas::project::PreparedProjectVariableIdentity firstIdentity{
        talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "pitch",
        "oscillatorVoice",
        {"voiceA"},
    };
    const talas::project::PreparedProjectVariableIdentity secondIdentity{
        talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "pitch",
        "oscillatorVoice",
        {"voiceB"},
    };
    const auto firstTarget = session.variableTargetIndex(
        firstIdentity,
        talas::expr::ValueDomain::Hertz);
    const auto secondTarget = session.variableTargetIndex(
        secondIdentity,
        talas::expr::ValueDomain::Hertz);
    require(
        session.ready() && firstTarget.has_value()
            && secondTarget.has_value()
            && firstTarget != secondTarget,
        "stable Sound-instance identities should resolve to distinct prepared handles");

    std::vector<double> initial(8, 0.0);
    require(
        session.render(initial),
        "initial variable-backed Sound instances should render");

    talas::runtime::TypedEventPayload payload{};
    payload.variable = {
        180.0,
        talas::expr::ValueDomain::Hertz,
    };
    const talas::runtime::PreparedTypedEvent assignment{
        talas::runtime::TypedEventKind::VariableAssignment,
        {
            talas::runtime::TypedEventTargetKind::Variable,
            *firstTarget,
            1,
            1,
        },
        0,
        1,
        payload,
    };
    talas::runtime::RealtimeTypedEventDispatchResult dispatch{};
    require(
        session.scheduleTypedEvents(
            std::span<const talas::runtime::PreparedTypedEvent>(
                &assignment,
                1),
            std::span<talas::runtime::RealtimeTypedEventDispatchResult>(
                &dispatch,
                1))
            && dispatch
                == talas::runtime::RealtimeTypedEventDispatchResult::Handled,
        "one exact variable assignment should be admitted");
    std::vector<double> updated(8, 0.0);
    const auto expected = 0.25
            * std::sin(
                2.0 * std::numbers::pi
                * (240.0 * 8.0 + 180.0)
                / 48000.0)
        + 0.25
            * std::sin(
                2.0 * std::numbers::pi * 240.0 * 9.0
                / 48000.0);
    require(
        session.render(updated)
            && std::abs(updated[1] - expected) < 1.0e-12,
        "updating one Sound-instance variable must leave the sibling cell unchanged");

    session.resetTransport();
    std::vector<double> restarted(2, 0.0);
    const auto restartedExpected = 0.25
            * std::sin(2.0 * std::numbers::pi * 180.0 / 48000.0)
        + 0.25
            * std::sin(2.0 * std::numbers::pi * 240.0 / 48000.0);
    require(
        session.render(restarted)
            && std::abs(restarted[1] - restartedExpected) < 1.0e-12,
        "transport reset should retain each exact variable cell and recompute derived parameters from it");

    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const std::array<talas::runtime::OfflineVariableAssignment, 1>
        offlineAssignments{{{
            8,
            1,
            1,
            firstIdentity,
            180.0,
            talas::expr::ValueDomain::Hertz,
        }}};
    require(
        offline.scheduleVariableAssignments(offlineAssignments),
        "offline variable scheduling should resolve the same stable identity");
    const auto offlineRendered = offline.render(16);
    require(
        offlineRendered.ok()
            && offlineRendered.mono.size() == 16
            && std::abs(offlineRendered.mono[9] - expected)
                < 1.0e-12,
        "offline and realtime variable transactions should agree at the same frame");

    auto invalid = assignment;
    invalid.sequence = 2;
    invalid.payload.variable.domain =
        talas::expr::ValueDomain::Seconds;
    require(
        !session.scheduleTypedEvents(
            std::span<const talas::runtime::PreparedTypedEvent>(
                &invalid,
                1),
            std::span<talas::runtime::RealtimeTypedEventDispatchResult>(
                &dispatch,
                1))
            && dispatch
                == talas::runtime::RealtimeTypedEventDispatchResult::Rejected,
        "a wrong-domain variable transaction must fail before publication");

    auto unusedVariableDocument = document;
    unusedVariableDocument.soundDefinitions.clear();
    unusedVariableDocument.graph = {};
    unusedVariableDocument.variableLibrary.variables.front().owner = {};
    unusedVariableDocument.variableLibrary.variables.front().domain =
        talas::expr::ValueDomain::Scalar;
    unusedVariableDocument.variableLibrary.variables.front().initialValue = 1.0;
    unusedVariableDocument.graph.nodes = {
        {"oscillator", "talas.oscillator", {
            expression("frequency", "120 hz"),
            expression("amplitude", "0.1")}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    unusedVariableDocument.graph.connections = {
        {"oscillator", "audioOut", "output", "audioIn"},
    };
    const auto unusedCompiled = talas::runtime::compileProject(
        unusedVariableDocument,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(unusedCompiled.ok(), "unused variable offline fixture should compile");
    talas::runtime::OfflineRenderSession unusedOffline(*unusedCompiled.graph);
    const talas::project::PreparedProjectVariableIdentity unusedIdentity{
        talas::project::ProjectVariableOwnerKind::Project,
        "pitch",
        {},
        {},
    };
    const std::array<talas::runtime::OfflineVariableAssignment, 2>
        unusedAssignments{{
            {3, 1, 1, unusedIdentity, 2.0, talas::expr::ValueDomain::Scalar},
            {5, 2, 1, unusedIdentity, 3.0, talas::expr::ValueDomain::Scalar},
        }};
    require(
        unusedOffline.scheduleVariableAssignments(unusedAssignments)
            && unusedOffline.render(8).ok(),
        "unused variables scheduled inside a multi-frame offline request must still be consumed at exact frames");
}

void testSoundInterfaceOneToManyBindingsApplyTransactionally()
{
    talas::project::ProjectDocument document;
    auto definition = parameterizedOscillatorDefinition();
    definition.graph.nodes.push_back({
        "oscB",
        "talas.oscillator",
        {
            expression("frequency", "999 hz"),
            expression("amplitude", "0.25"),
        },
        {},
    });
    definition.graph.nodes.push_back(
        {"mix", "talas.mixer", {}, {}});
    definition.graph.connections = {
        {"osc", "audioOut", "mix", "in1"},
        {"oscB", "audioOut", "mix", "in2"},
    };
    definition.outputs.front().nodeId = "mix";
    definition.exposedParameters.front().bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "oscB",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    });
    document.soundDefinitions.push_back(std::move(definition));
    auto instance = soundInstance("voice", "oscillatorVoice");
    instance.parameters.push_back(
        expression("frequency", "180 hz"));
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"voice", "audioOut", "output", "audioIn"});

    const auto expanded = talas::project::expandProjectSounds(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(expanded.ok(), "one-to-many Sound binding should expand");
    require(
        std::ranges::count_if(
            expanded.document.graph.nodes,
            [](const auto& node) {
                return node.typeId == "talas.oscillator"
                    && std::ranges::any_of(
                        node.parameters,
                        [](const auto& parameter) {
                            return parameter.parameterId == "frequency"
                                && parameter.expression
                                    == std::optional<std::string>{"180 hz"};
                        });
            })
            == 2,
        "one exposed override should apply to every explicit internal binding");
    for (const auto& identity : expanded.manifest.nodes)
    {
        if (identity.definitionId == "oscillatorVoice"
            && (identity.localNodeId == "osc"
                || identity.localNodeId == "oscB"))
        {
            require(
                identity.qualifiedParameterIds
                    == std::vector<std::string>{
                        "i5:voice/d15:oscillatorVoice/p9:frequency"},
                "manifest should retain the stable exposed parameter identity for each bound internal node");
        }
    }
}

void testTransitiveSoundInterfaceBindingsPreserveQualifiedIdentity()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(
        parameterizedOscillatorDefinition());

    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "wrapper";
    wrapper.displayName = "Wrapper";
    wrapper.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "voice",
        "audioOut",
    });
    wrapper.graph.nodes.push_back(
        soundInstance("voice", "oscillatorVoice"));
    auto frequency = parameterizedOscillatorDefinition()
        .exposedParameters.front();
    frequency.defaultValue.expression = "60 hz";
    frequency.aliases.clear();
    frequency.bindings = {{
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "voice",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    }};
    wrapper.exposedParameters.push_back(std::move(frequency));
    document.soundDefinitions.push_back(std::move(wrapper));

    auto instance = soundInstance("nested", "wrapper");
    instance.parameters.push_back(
        expression("frequency", "90 hz"));
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"nested", "audioOut", "output", "audioIn"});

    const auto expanded = talas::project::expandProjectSounds(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(expanded.ok(),
            "transitive exposed Sound binding should expand");
    const auto oscillator = std::ranges::find_if(
        expanded.document.graph.nodes,
        [](const auto& node) {
            return node.typeId == "talas.oscillator";
        });
    require(
        oscillator != expanded.document.graph.nodes.end()
            && std::ranges::any_of(
                oscillator->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "frequency"
                        && parameter.expression
                            == std::optional<std::string>{"90 hz"};
                }),
        "parent exposed override should bind through the child Sound interface");
    const auto identity = std::ranges::find_if(
        expanded.manifest.nodes,
        [&](const auto& candidate) {
            return candidate.flatNodeId == oscillator->instanceId;
        });
    require(
        identity != expanded.manifest.nodes.end()
            && identity->qualifiedParameterIds
                == std::vector<std::string>{
                    "i6:nested/d7:wrapper/p9:frequency",
                    "i6:nested/i5:voice/d15:oscillatorVoice/p9:frequency"},
        "transitive manifest should preserve every stable interface identity along the binding path");
}

void testEveryTypedSoundInterfaceKindMaterializesDuringExpansion()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "typedVoice";
    definition.displayName = "Typed Voice";
    definition.graph.nodes = {
        {"osc", "talas.oscillator", {}, {}},
        {"sample", "talas.sample", {}, {}},
        {"mix", "talas.mixer", {}, {}},
    };
    definition.graph.connections = {
        {"osc", "audioOut", "mix", "in1"},
        {"sample", "audioOut", "mix", "in2"},
    };
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "mix",
        "audioOut",
    });
    const auto expose = [&](
                            std::string id,
                            std::string name,
                            talas::core::ParameterKind kind,
                            talas::project::ProjectParameterExpression defaultValue,
                            std::vector<std::string> choices,
                            std::string nodeId,
                            std::string parameterId) {
        talas::project::ProjectSoundExposedParameter parameter;
        parameter.parameterId = std::move(id);
        parameter.displayName = std::move(name);
        parameter.kind = kind;
        parameter.defaultValue = std::move(defaultValue);
        parameter.choices = std::move(choices);
        parameter.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            std::move(nodeId),
            std::move(parameterId),
            {},
            talas::expr::ValueDomain::Scalar,
        });
        definition.exposedParameters.push_back(
            std::move(parameter));
    };
    expose(
        "interpolation",
        "Interpolation",
        talas::core::ParameterKind::Choice,
        choiceParameter("interpolation", "linear"),
        {"linear", "none"},
        "osc",
        "interpolation");
    expose(
        "loop",
        "Loop",
        talas::core::ParameterKind::Boolean,
        booleanParameter("loop", false),
        {},
        "sample",
        "setLoop");
    expose(
        "sampleAsset",
        "Sample",
        talas::core::ParameterKind::Resource,
        resourceParameter("sampleAsset", "Samples/Default"),
        {},
        "sample",
        "sample");
    expose(
        "modulation",
        "Modulation",
        talas::core::ParameterKind::Text,
        textParameter("modulation", "phase"),
        {},
        "osc",
        "modulation");
    document.soundDefinitions.push_back(std::move(definition));

    auto instance = soundInstance("typed", "typedVoice");
    instance.parameters = {
        choiceParameter("interpolation", "none"),
        booleanParameter("loop", true),
        resourceParameter("sampleAsset", "Samples/Override"),
        textParameter("modulation", "frequency"),
    };
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"typed", "audioOut", "output", "audioIn"});

    const auto expanded = talas::project::expandProjectSounds(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(expanded.ok(),
            "all typed Sound interface kinds should expand");
    const auto findFlatNode = [&](std::string_view typeId) {
        return std::ranges::find_if(
            expanded.document.graph.nodes,
            [&](const auto& node) {
                return node.typeId == typeId;
            });
    };
    const auto oscillator = findFlatNode("talas.oscillator");
    const auto sample = findFlatNode("talas.sample");
    require(
        oscillator != expanded.document.graph.nodes.end()
            && std::ranges::any_of(
                oscillator->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "interpolation"
                        && parameter.choice
                            == std::optional<std::string>{"none"};
                })
            && std::ranges::any_of(
                oscillator->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "modulation"
                        && parameter.text
                            == std::optional<std::string>{"frequency"};
                }),
        "Choice and Text overrides should materialize on their internal targets");
    require(
        sample != expanded.document.graph.nodes.end()
            && std::ranges::any_of(
                sample->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "setLoop"
                        && parameter.boolean
                            == std::optional<bool>{true};
                })
            && std::ranges::any_of(
                sample->parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "sample"
                        && parameter.resource
                            == std::optional<std::string>{
                                "Samples/Override"};
                }),
        "Boolean and Resource overrides should materialize on their internal targets");
}

void testQualifiedSoundParameterIdentityEncodesPathSegmentsInjectively()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(
        parameterizedOscillatorDefinition());

    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "wrapper";
    wrapper.displayName = "Wrapper";
    wrapper.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "b",
        "audioOut",
    });
    wrapper.graph.nodes.push_back(
        soundInstance("b", "oscillatorVoice"));
    document.soundDefinitions.push_back(std::move(wrapper));

    document.graph.nodes.push_back(
        soundInstance("a/b", "oscillatorVoice"));
    document.graph.nodes.push_back(
        soundInstance("a", "wrapper"));
    document.graph.nodes.push_back(
        {"mix", "talas.mixer", {}, {}});
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"a/b", "audioOut", "mix", "in1"},
        {"a", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    const auto expanded = talas::project::expandProjectSounds(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(expanded.ok(),
            "qualified-identity collision fixture should expand");
    std::vector<std::string> identities;
    for (const auto& node : expanded.manifest.nodes)
    {
        if (node.definitionId == "oscillatorVoice"
            && node.localNodeId == "osc")
        {
            require(
                node.qualifiedParameterIds.size() == 1,
                "each leaf should retain one exposed frequency identity");
            identities.push_back(
                node.qualifiedParameterIds.front());
        }
    }
    require(
        identities.size() == 2
            && identities[0] != identities[1],
        "instance paths [a/b] and [a,b] must not collide in qualified parameter identity");
}

void testSoundDefinitionsCanNestTransitively()
{
    auto document = makeTwoVoiceProject();
    document.graph = {};
    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "wrapper";
    wrapper.displayName = "Wrapper";
    wrapper.inputs.push_back({
        "frequency",
        talas::core::PortType::Control,
        "voice",
        "frequency",
    });
    wrapper.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "voice",
        "audioOut",
    });
    wrapper.graph.nodes.push_back(
        soundInstance("voice", "oscillatorVoice"));
    document.soundDefinitions.push_back(std::move(wrapper));
    document.graph.nodes.push_back({
        "frequency",
        "talas.constant",
        {expression("value", "120")},
        {},
    });
    document.graph.nodes.push_back(soundInstance("nested", "wrapper"));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"frequency", "value", "nested", "frequency"},
        {"nested", "audioOut", "output", "audioIn"},
    };

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto expanded = talas::project::expandProjectSounds(
        document,
        registry);
    require(expanded.ok(), "transitive Sound fixture should expand");
    const auto oscillatorIdentity = std::ranges::find_if(
        expanded.manifest.nodes,
        [](const auto& identity) {
            return identity.instancePath
                    == std::vector<std::string>{"nested", "voice"}
                && identity.definitionId == "oscillatorVoice"
                && identity.localNodeId == "osc";
        });
    require(
        oscillatorIdentity != expanded.manifest.nodes.end()
            && oscillatorIdentity->sourcePath
                == "nested/voice/oscillatorVoice::osc"
            && oscillatorIdentity->scopeOrdinal > 1,
        "transitive expansion should preserve the complete instance path and child scope");

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        registry,
        settings);
    require(compiled.ok(), "transitively nested Sounds should compile");
    require(
        std::ranges::count_if(
            compiled.graph->plan().steps,
            [](const auto& step) {
                return step.typeId == "talas.oscillator";
            })
            == 1,
        "transitive expansion should emit the nested implementation once");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(
        rendered.ok() && rendered.mono.size() == 8
            && std::abs(
                   rendered.mono[1]
                   - 0.25 * std::sin(
                       2.0 * std::numbers::pi * 120.0 / 48000.0))
                < 1.0e-12,
        "a transitive nested output should retain its source scope at the root terminal boundary");
}

void testNestedCompileDiagnosticsRetainStructuralSourcePath()
{
    auto document = makeTwoVoiceProject();
    document.graph.nodes.erase(
        document.graph.nodes.begin() + 2,
        document.graph.nodes.end());
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"frequency", "value", "voiceA", "frequency"},
        {"voiceA", "audioOut", "output", "audioIn"},
    };
    document.soundDefinitions.front()
        .graph.nodes.front()
        .parameters.front()
        .expression = "unboundControl";

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(!compiled.ok(), "invalid nested parameter fixture should fail compilation");
    require(
        std::ranges::any_of(
            compiled.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.parameterId == "amplitude"
                    && diagnostic.structuralSourcePath
                        == "voiceA/oscillatorVoice::osc";
            }),
        "nested compile diagnostics should retain their instance/definition/local-node path");
}

void testFlatRootCompilationRetainsGlobalScope()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "440 hz"),
            expression("amplitude", "0.25"),
        },
        {},
    });
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"osc", "audioOut", "output", "audioIn"});

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "flat root scope fixture should compile");
    require(
        std::ranges::all_of(
            compiled.graph->plan().steps,
            [](const auto& step) {
                return step.scopeId == talas::runtime::globalRuntimeScope
                    && step.structuralSourcePath == step.nodeId;
            })
            && std::ranges::all_of(
                compiled.graph->plan().buffers,
                [](const auto& buffer) {
                    return buffer.scopeId
                        == talas::runtime::globalRuntimeScope;
                }),
        "flat projects should retain their global prepared plan structure");
}

void testUnusedDefinitionsPreserveReachableRootCompilation()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "440 hz"),
            expression("amplitude", "0.25"),
        },
        {},
    });
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.nodes.push_back({"parked", "talas.vca", {}, {}});
    document.graph.connections.push_back(
        {"osc", "audioOut", "out", "audioIn"});

    appendBinaryDefinitions(
        document,
        "unusedLevel",
        14);
    document.graph.nodes.push_back(
        soundInstance("unusedInstance", "unusedLevel0"));

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        compiled.ok(),
        "an unused Sound definition must not make an unreachable incomplete root node fatal");
}

void testReachableUnknownRootSoundInstanceFailsClosed()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(
        soundInstance("missing", "doesNotExist"));
    document.graph.nodes.push_back(
        {"mix", "talas.overlappingMixer", {}, {}});
    document.graph.nodes.push_back(
        {"out", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"missing", "audioOut", "mix", "in1"},
        {"mix", "leftOut", "out", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        !compiled.ok()
            && std::ranges::any_of(
                compiled.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.message.find(
                               "Unknown Sound definition")
                        != std::string::npos;
                }),
        "a reachable root Sound instance with an unknown definition must fail closed");
}

void testSoundExpansionRejectsCompactExponentialGraphs()
{
    talas::project::ProjectDocument document;
    constexpr std::size_t depth = 14;
    appendBinaryDefinitions(document, "level", depth);
    document.graph.nodes.push_back(soundInstance("root", "level0"));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"root", "audioOut", "out", "audioIn"});

    const auto expanded = talas::project::expandProjectSounds(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(
        !expanded.ok()
            && !expanded.validation.errors.empty()
            && expanded.validation.errors.front().message.find(
                   "expansion limit")
                != std::string::npos,
        "compact exponential Sound graphs should fail at a deterministic expansion limit");
}

void testPreparedRealtimeUsesTheSameExpandedSoundGraph()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 32;
    const auto compiled = talas::runtime::compileProject(
        makeTwoVoiceProject(),
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(compiled.ok(), "nested Sound realtime fixture should compile");

    const auto offline =
        talas::runtime::renderOffline(*compiled.graph, 32);
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(),
            "expanded Sound graph should prepare for realtime rendering");
    std::vector<double> realtimeOutput(32);
    constexpr std::size_t partitions[] = {3, 7, 1, 9, 4, 8};
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        require(
            realtime.render(
                std::span<double>(realtimeOutput)
                    .subspan(offset, partition)),
            "expanded Sound graph should render across irregular prepared partitions");
        offset += partition;
    }
    require(
        offline.ok() && offline.mono.size() == realtimeOutput.size(),
        "offline and prepared realtime should produce matching nested Sound buffers");
    for (std::size_t frame = 0; frame < offline.mono.size(); ++frame)
    {
        require(
            std::abs(offline.mono[frame] - realtimeOutput[frame])
                < 1.0e-12,
            "offline and prepared realtime should share one expanded Sound plan");
    }
}

void testNestedFeedbackPairsKeepIndependentScopedStateAcrossPartitions()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "feedbackCell";
    definition.displayName = "Feedback Cell";
    definition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "mix",
        "in1",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "mix",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "sink",
        "talas.feedbackLoopInput",
        {textParameter("connection", "localLoop")},
        {},
    });
    definition.graph.nodes.push_back({
        "mix",
        "talas.mixer",
        {expression("gain1", "1"), expression("gain2", "0.5")},
        {},
    });
    definition.graph.nodes.push_back({
        "feedback",
        "talas.feedbackLoopOutput",
        {
            textParameter("connection", "localLoop"),
            expression("delay", "0 s"),
        },
        {},
    });
    definition.graph.connections = {
        {"feedback", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "sink", "audioIn"},
    };

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(definition));
    document.graph.nodes.push_back({
        "inputA",
        "talas.audioInput",
        {booleanParameter("channel1", true), booleanParameter("channel2", false)},
        {},
    });
    document.graph.nodes.push_back({
        "inputB",
        "talas.audioInput",
        {booleanParameter("channel1", false), booleanParameter("channel2", true)},
        {},
    });
    document.graph.nodes.push_back(soundInstance("cellA", "feedbackCell"));
    document.graph.nodes.push_back(soundInstance("cellB", "feedbackCell"));
    document.graph.nodes.push_back({"sum", "talas.mixer", {}, {}});
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"inputA", "audioOut", "cellA", "audioIn"},
        {"inputB", "audioOut", "cellB", "audioIn"},
        {"cellA", "audioOut", "sum", "in1"},
        {"cellB", "audioOut", "sum", "in2"},
        {"sum", "audioOut", "output", "audioIn"},
    };

    constexpr std::size_t frameCount = 40;
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = frameCount;
    settings.audioInputChannels = {
        {1, std::vector<double>(frameCount, 0.0)},
        {2, std::vector<double>(frameCount, 0.0)},
    };
    settings.audioInputChannels[0].samples[0] = 1.0;
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok() && compiled.graph->plan().feedbackPairs.size() == 2,
        "two nested feedback instances should compile as two pairs" + diagnostics);
    require(
        compiled.graph->plan().feedbackPairs[0].connectionId == "localLoop"
            && compiled.graph->plan().feedbackPairs[1].connectionId == "localLoop"
            && compiled.graph->plan().feedbackPairs[0].scopeId
                != compiled.graph->plan().feedbackPairs[1].scopeId,
        "same-named nested feedback pairs must retain disjoint structural scopes");

    auto permutedDocument = document;
    std::ranges::reverse(permutedDocument.graph.nodes);
    std::ranges::reverse(permutedDocument.graph.connections);
    const auto permuted = talas::runtime::compileProject(
        permutedDocument,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(
        permuted.ok()
            && permuted.graph->plan().feedbackPairs.size() == 2,
        "nested feedback insertion permutation should compile");
    const auto feedbackIdentity = [](const talas::runtime::RuntimePlan& plan) {
        std::vector<std::tuple<
            std::string,
            talas::runtime::RuntimeScopeId,
            std::string>> identities;
        for (const auto& pair : plan.feedbackPairs)
        {
            identities.emplace_back(
                plan.steps[pair.outputStepIndex].structuralSourcePath,
                pair.scopeId,
                pair.connectionId);
        }
        return identities;
    };
    require(
        feedbackIdentity(compiled.graph->plan())
            == feedbackIdentity(permuted.graph->plan()),
        "nested feedback pair identity and order should not depend on graph insertion order");

    auto aggregateConstrained = settings;
    aggregateConstrained.maxRealtimeFeedbackFrames = 1;
    const auto aggregateOverflow = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        aggregateConstrained);
    require(
        !aggregateOverflow.ok()
            && std::ranges::any_of(
                aggregateOverflow.diagnostics,
                [](const talas::runtime::Diagnostic& diagnostic) {
                    return diagnostic.code
                            == talas::runtime::DiagnosticCode::ParameterEvaluationError
                        && diagnostic.message.find("aggregate feedback history")
                            != std::string::npos;
                }),
        "aggregate feedback history beyond its configured budget should fail preparation");

    const auto contiguous = talas::runtime::renderOffline(*compiled.graph, frameCount);
    require(contiguous.ok(), "contiguous nested feedback render should succeed");
    talas::runtime::OfflineRenderSession partitionedSession(*compiled.graph);
    std::vector<double> partitioned;
    constexpr std::size_t partitions[] = {3, 7, 1, 9, 4, 16};
    for (const auto partition : partitions)
    {
        const auto block = partitionedSession.render(partition);
        require(block.ok(), "partitioned nested feedback render should succeed");
        partitioned.insert(partitioned.end(), block.mono.begin(), block.mono.end());
    }
    require(
        partitioned.size() == contiguous.mono.size(),
        "partitioned nested feedback render should preserve frame count");
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        require(
            std::abs(partitioned[frame] - contiguous.mono[frame]) < 1.0e-12,
            "nested feedback should be invariant across offline partitions");
    }

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "nested feedback plan should prepare for realtime: "
            + std::string(realtime.error()));
    std::vector<float> channel1(frameCount, 0.0F);
    std::vector<float> channel2(frameCount, 0.0F);
    channel1[0] = 1.0F;
    std::vector<double> realtimeOutput(frameCount, 0.0);
    std::size_t offset = 0;
    for (const auto partition : partitions)
    {
        const float* channels[] = {
            channel1.data() + static_cast<std::ptrdiff_t>(offset),
            channel2.data() + static_cast<std::ptrdiff_t>(offset),
        };
        require(
            realtime.render(
                {channels, 2},
                std::span<double>(realtimeOutput).subspan(offset, partition),
                {}),
            "nested feedback should render across irregular realtime partitions");
        offset += partition;
    }
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        require(
            std::abs(realtimeOutput[frame] - contiguous.mono[frame]) < 1.0e-12,
            "offline and prepared realtime nested feedback traces should match");
    }
}

void testFeedbackPairNamesNeverCrossSoundScopes()
{
    talas::project::ProjectSoundDefinition inputHalf;
    inputHalf.definitionId = "feedbackInputHalf";
    inputHalf.displayName = "Feedback Input Half";
    inputHalf.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "sink",
        "audioIn",
    });
    inputHalf.graph.nodes.push_back({
        "sink",
        "talas.feedbackLoopInput",
        {textParameter("connection", "sameName")},
        {},
    });

    talas::project::ProjectSoundDefinition outputHalf;
    outputHalf.definitionId = "feedbackOutputHalf";
    outputHalf.displayName = "Feedback Output Half";
    outputHalf.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "source",
        "audioOut",
    });
    outputHalf.graph.nodes.push_back({
        "source",
        "talas.feedbackLoopOutput",
        {
            textParameter("connection", "sameName"),
            expression("delay", "0 s"),
        },
        {},
    });

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(inputHalf));
    document.soundDefinitions.push_back(std::move(outputHalf));
    document.graph.nodes.push_back({"input", "talas.audioInput", {}, {}});
    document.graph.nodes.push_back(soundInstance("sinkCell", "feedbackInputHalf"));
    document.graph.nodes.push_back(soundInstance("sourceCell", "feedbackOutputHalf"));
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"input", "audioOut", "sinkCell", "audioIn"},
        {"sourceCell", "audioOut", "output", "audioIn"},
    };

    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {});
    require(!compiled.ok(), "same feedback name in different Sound scopes must not pair");
    require(
        std::ranges::any_of(
            compiled.diagnostics,
            [](const talas::runtime::Diagnostic& diagnostic) {
                return diagnostic.code
                        == talas::runtime::DiagnosticCode::GraphValidationFailed
                    && diagnostic.parameterId == "connection"
                    && diagnostic.message.find("no matching")
                        != std::string::npos;
            }),
        "cross-scope feedback endpoints should fail with a pairing diagnostic");
}

void testNestedSoundRetainsLegacyPreparedMidiVoiceLowering()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "midiRegion";
    definition.displayName = "MIDI Region";
    definition.outputs = {
        {
            "leftOut",
            talas::core::PortType::Audio,
            "midi",
            "leftOut",
        },
        {
            "rightOut",
            talas::core::PortType::Audio,
            "midi",
            "rightOut",
        },
    };
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "440 hz"),
            expression(
                "amplitude",
                "!KeyDown * @voiceGain * @instanceScale"),
        },
        {},
    });
    definition.graph.nodes.push_back({
        "midi",
        "talas.midiVoice",
        {
            expression("left", "1"),
            expression("right", "1"),
            expression("channel", "1"),
            textParameter("source", ""),
            resourceParameter("midiFile", ""),
            expression("polyphony", "2"),
            expression("lowPitch", "0 nn"),
            expression("highPitch", "127 nn"),
            textParameter("script", ""),
            booleanParameter("shared", false),
        },
        {},
    });
    definition.graph.connections.push_back(
        {"osc", "audioOut", "midi", "audioIn"});
    document.soundDefinitions.push_back(std::move(definition));
    document.variableLibrary.variables.push_back({
        "instanceScale",
        "Instance Scale",
        talas::expr::ValueDomain::Scalar,
        0.5,
        {
            talas::project::ProjectVariableOwnerKind::SoundDefinition,
            "midiRegion",
        },
        true,
    });
    document.variableLibrary.variables.push_back({
        "voiceGain",
        "Voice Gain",
        talas::expr::ValueDomain::Scalar,
        0.75,
        {
            talas::project::ProjectVariableOwnerKind::MidiVoice,
            "midiRegion",
            "midi",
        },
        false,
    });
    document.graph.nodes.push_back(
        soundInstance("region", "midiRegion"));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"region", "leftOut", "output", "audioIn"},
        {"region", "rightOut", "output", "rightIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 32;
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok()
            && compiled.graph->plan().midiVoices.size() == 1
            && compiled.graph->plan().midiVoices.front().voices.size()
                == 2,
        "nested Sound expansion should preserve legacy prepared MIDI Voice lowering"
            + diagnostics);

    const auto& midiPlan = compiled.graph->plan().midiVoices.front();
    require(
        midiPlan.ownerNodeId == "midi"
            && midiPlan.ownerDefinitionId == "midiRegion"
            && midiPlan.ownerInstancePath
                == std::vector<std::string>{"region"},
        "nested MIDI voice plan should retain definition-local owner and exact instance path");
    const auto& ownerStep =
        compiled.graph->plan().steps[midiPlan.ownerStepIndex];
    require(
        ownerStep.scopeId != talas::runtime::globalRuntimeScope
            && midiPlan.voices[0].scopeId != ownerStep.scopeId
            && midiPlan.voices[1].scopeId != ownerStep.scopeId
            && midiPlan.voices[0].scopeId
                != midiPlan.voices[1].scopeId,
        "nested structural and MIDI voice scopes should use one collision-free allocator");

    const auto& variables = compiled.graph->preparedVariables();
    const auto structuralCells = std::ranges::count_if(
        variables.cells,
        [](const auto& cell) {
            return cell.identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::SoundDefinition;
        });
    require(
        structuralCells == 1 && variables.cells.size() == 3,
        "a Sound-local cell should remain structural while only voice-owned cells multiply per slot");
    for (std::size_t voiceIndex = 0;
         voiceIndex < midiPlan.voices.size();
         ++voiceIndex)
    {
        const auto& voice = midiPlan.voices[voiceIndex];
        require(
            voice.voiceOrdinal == voiceIndex
                && voice.variableCellCount == 1
                && voice.firstVariableCellIndex < variables.cells.size(),
            "each nested voice should publish one exact variable-cell range");
        const auto& voiceCell =
            variables.cells[voice.firstVariableCellIndex];
        require(
            voiceCell.scopeOrdinal == voice.scopeId
                && voiceCell.identity.ownerNodeId == "midi"
                && voiceCell.identity.definitionId == "midiRegion"
                && voiceCell.identity.instancePath
                    == std::vector<std::string>{"region"}
                && voiceCell.identity.voiceOrdinal
                    == std::optional<std::uint16_t>{
                        static_cast<std::uint16_t>(voiceIndex)},
            "nested voice cells should retain exact expanded owner identity");
        const auto oscillator = std::ranges::find_if(
            compiled.graph->plan().steps.begin()
                + static_cast<std::ptrdiff_t>(voice.firstStepIndex),
            compiled.graph->plan().steps.begin()
                + static_cast<std::ptrdiff_t>(
                    voice.firstStepIndex + voice.stepCount),
            [](const auto& step) {
                return step.typeId == "talas.oscillator";
            });
        require(
            oscillator
                != compiled.graph->plan().steps.begin()
                    + static_cast<std::ptrdiff_t>(
                        voice.firstStepIndex + voice.stepCount),
            "nested voice range should retain the exact oscillator clone");
        const auto amplitude = std::ranges::find(
            oscillator->parameters,
            std::string{"amplitude"},
            &talas::runtime::ParameterValue::id);
        require(
            amplitude != oscillator->parameters.end()
                && amplitude->variableCellIndices.size() == 2
                && std::ranges::find(
                       amplitude->variableCellIndices,
                       static_cast<std::uint32_t>(
                           voice.firstVariableCellIndex))
                    != amplitude->variableCellIndices.end(),
            "each nested oscillator clone should bind its own voice cell alongside the shared structural cell");
    }

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        std::string(
            "a nested MIDI Voice owner should prepare in its structural Sound scope: ")
            + std::string(realtime.error()));
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const std::array<talas::runtime::OfflineVariableAssignment, 1>
        forbiddenVoiceAssignment{{{
            0,
            1,
            1,
            variables.cells[
                midiPlan.voices.front().firstVariableCellIndex]
                .identity,
            1.0,
            talas::expr::ValueDomain::Scalar,
        }}};
    require(
        !offline.scheduleVariableAssignments(
            forbiddenVoiceAssignment),
        "offline generic variable scheduling must not bypass MIDI Voice slot generation authority");
    talas::runtime::OfflineMidiVoiceEvent nestedNote;
    nestedNote.frame = 0;
    nestedNote.batchId = 2;
    nestedNote.sequence = 1;
    nestedNote.kind =
        talas::runtime::OfflineMidiVoiceEventKind::NoteOn;
    nestedNote.channel = 1;
    nestedNote.note = 60;
    nestedNote.velocity = 127;
    talas::runtime::OfflineMidiVoiceEvent nestedAssignment;
    nestedAssignment.frame = 0;
    nestedAssignment.batchId = 2;
    nestedAssignment.sequence = 2;
    nestedAssignment.kind =
        talas::runtime::OfflineMidiVoiceEventKind::VariableAssignment;
    nestedAssignment.target =
        variables.cells[midiPlan.voices.front().firstVariableCellIndex]
            .identity;
    nestedAssignment.voiceGeneration = 1;
    nestedAssignment.value = 0.9;
    nestedAssignment.domain = talas::expr::ValueDomain::Scalar;
    const std::array nestedEvents{
        nestedNote,
        nestedAssignment,
    };
    require(
        offline.scheduleMidiVoiceEvents(nestedEvents),
        "offline nested Sound voice should accept its exact instance and generation");
    const auto offlineResult = offline.render(320);
    require(
        offlineResult.ok()
            && std::ranges::any_of(
                std::span<const double>(offlineResult.mono).subspan(250),
                [](double sample) { return std::abs(sample) > 1.0e-6; }),
        "offline nested Sound voice should use the shared allocator and render its exact clone");
    std::vector<double> output(17);
    require(
        realtime.render(output),
        "a structurally scoped nested MIDI Voice should render without scope collision");
}

void testMultipleNestedPreparedMidiVoiceRegionsUseDisjointScopesAndRouting()
{
    const auto makeRegion = [](
                                std::string definitionId,
                                int channel,
                                double frequency) {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = std::move(definitionId);
        definition.displayName = definition.definitionId;
        definition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            "midi",
            "leftOut",
        });
        definition.graph.nodes.push_back({
            "osc",
            "talas.oscillator",
            {
                expression(
                    "frequency",
                    std::to_string(frequency) + " hz"),
                expression("amplitude", "!KeyDown"),
            },
            {},
        });
        definition.graph.nodes.push_back({
            "midi",
            "talas.midiVoice",
            {
                expression("left", "1"),
                expression("right", "1"),
                expression("channel", std::to_string(channel)),
                textParameter("source", ""),
                resourceParameter("midiFile", ""),
                expression("polyphony", "1"),
                expression("lowPitch", "0 nn"),
                expression("highPitch", "127 nn"),
                textParameter("script", ""),
                booleanParameter("shared", false),
            },
            {},
        });
        definition.graph.connections.push_back(
            {"osc", "audioOut", "midi", "audioIn"});
        return definition;
    };

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(
        makeRegion("regionA", 1, 100.0));
    document.soundDefinitions.push_back(
        makeRegion("regionB", 2, 200.0));
    document.graph.nodes = {
        soundInstance("first", "regionA"),
        soundInstance("second", "regionB"),
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"first", "audioOut", "mix", "in1"},
        {"second", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok()
            && compiled.graph->plan().midiVoices.size() == 2,
        "two nested MIDI Voice regions should lower independently"
            + diagnostics);

    std::vector<talas::runtime::RuntimeScopeId> scopes;
    for (const auto& region : compiled.graph->plan().midiVoices)
    {
        require(
            region.voices.size() == 1,
            "each prepared region should retain its own polyphony");
        scopes.push_back(region.voices.front().scopeId);
        const auto& clone = compiled.graph->plan().steps[
            region.voices.front().firstStepIndex];
        require(
            clone.scopeId == region.voices.front().scopeId
                && clone.eventValueScopeId
                    != clone.scopeId,
            "a MIDI clone should use a disjoint runtime scope while preserving its structural Event Value scope");
    }
    require(
        scopes[0] != scopes[1],
        "separate MIDI regions must never share voice scope state");

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        realtime.ready(),
        "multiple nested MIDI regions should prepare for realtime: "
            + std::string(realtime.error()));
    require(
        realtime.scheduleMidiNote(
            {talas::runtime::RealtimeMidiNoteKind::NoteOn, 3, 60, 127},
            0)
            == talas::runtime::RealtimeMidiDispatchResult::NotHandled,
        "a note outside every prepared region should fall through");
    require(
        realtime.scheduleMidiNote(
            {talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127},
            0)
            == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "the first channel should route only to its prepared region");
    std::vector<double> firstBlock(8, 0.0);
    require(
        realtime.render(firstBlock)
            && std::ranges::any_of(firstBlock, [](double sample) {
                   return std::abs(sample) > 1.0e-6;
               }),
        "the first prepared region should render after its delayed attack");
    require(
        realtime.scheduleMidiNote(
            {talas::runtime::RealtimeMidiNoteKind::NoteOn, 2, 64, 127},
            0)
            == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "the second channel should route to the other prepared region");
    std::vector<double> secondBlock(8, 0.0);
    require(
        realtime.render(secondBlock)
            && std::ranges::any_of(secondBlock, [](double sample) {
                   return std::abs(sample) > 1.0e-6;
               }),
        "both prepared regions should render with independent allocator and voice state");
}

enum class NestedSoundSelectorPlacement
{
    ChildVoice,
    ChildInputSound,
    ChildSoundAncestor,
};

void runNestedMidiVoiceSoundSelectionFixture(
    NestedSoundSelectorPlacement placement)
{
    constexpr std::string_view selectorContainerId = "selectorContainer";
    constexpr std::string_view selectorLeafId = "selectorLeaf";
    const auto makeTone = [](std::string definitionId, std::string value) {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = std::move(definitionId);
        definition.displayName = definition.definitionId;
        definition.graph.nodes.push_back({
            "constant",
            "talas.valToSound",
            {expression("value", std::move(value))},
            {},
        });
        definition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            "constant",
            "audioOut",
        });
        return definition;
    };

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(makeTone("toneA", "0"));
    document.soundDefinitions.push_back(makeTone("toneB", "0.5"));
    const std::vector<talas::project::ProjectParameterExpression> childParameters{
        expression("left", "1"),
        expression("right", "1"),
        expression("channel", "1"),
        expression("polyphony", "1"),
        expression("lowPitch", "0 nn"),
        expression("highPitch", "127 nn"),
        textParameter("source", "Script"),
        resourceParameter("midiFile", ""),
        textParameter("script",
            "self keyDownAt: 0 ms duration: 2000 ms frequency: 60 nn velocity: 1."),
        booleanParameter("shared", false),
    };
    const std::vector<talas::project::ProjectParameterExpression> parentParameters{
        expression("left", "1"),
        expression("right", "1"),
        expression("channel", "1"),
        expression("polyphony", "2"),
        expression("lowPitch", "0 nn"),
        expression("highPitch", "127 nn"),
        textParameter("source", "Script"),
        resourceParameter("midiFile", ""),
        textParameter(
            "script",
            "self keyDownAt: 0 ms duration: 1000 ms frequency: 60 nn velocity: 1. "
            "self keyDownAt: 10 ms duration: 1000 ms frequency: 64 nn velocity: 1. "
            "self keyDownAt: 600 ms duration: 1000 ms frequency: 67 nn velocity: 1."),
        booleanParameter("shared", false),
    };
    const auto selectorNode = talas::project::ProjectNode{
        "selector",
        "talas.variable",
        {textParameter("variableId", "selected")},
        {},
    };
    std::string selectionOwnerDefinitionId;
    if (placement != NestedSoundSelectorPlacement::ChildVoice)
    {
        talas::project::ProjectSoundDefinition container;
        container.definitionId = std::string(selectorContainerId);
        container.displayName = container.definitionId;
        if (placement == NestedSoundSelectorPlacement::ChildInputSound)
        {
            container.graph.nodes.push_back(selectorNode);
            container.outputs.push_back({
                "audioOut", talas::core::PortType::Audio, "selector", "audioOut"});
        }
        else
        {
            talas::project::ProjectSoundDefinition leaf;
            leaf.definitionId = std::string(selectorLeafId);
            leaf.displayName = leaf.definitionId;
            leaf.graph.nodes.push_back(selectorNode);
            leaf.outputs.push_back({
                "audioOut", talas::core::PortType::Audio, "selector", "audioOut"});
            document.soundDefinitions.push_back(std::move(leaf));
            container.graph.nodes.push_back(
                soundInstance("nestedSelector", std::string(selectorLeafId)));
            container.outputs.push_back({
                "audioOut", talas::core::PortType::Audio,
                "nestedSelector", "audioOut"});
        }
        document.soundDefinitions.push_back(std::move(container));
        selectionOwnerDefinitionId = std::string(selectorContainerId);
        document.graph.nodes.push_back(
            soundInstance("input", std::string(selectorContainerId)));
    }
    else
    {
        document.graph.nodes.push_back(selectorNode);
    }
    document.graph.nodes.push_back({"inner", "talas.midiVoice", childParameters, {}});
    document.graph.nodes.push_back({"outer", "talas.midiVoice", parentParameters, {}});
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {placement == NestedSoundSelectorPlacement::ChildVoice ? "selector" : "input",
            "audioOut", "inner", "audioIn"},
        {"inner", "leftOut", "outer", "audioIn"},
        {"outer", "leftOut", "output", "audioIn"},
        {"outer", "rightOut", "output", "rightIn"},
    };

    talas::project::ProjectSoundVariableDeclaration selection;
    selection.variableId = "selected";
    selection.displayName = "Selected Sound";
    selection.candidateDefinitionIds = {"toneA", "toneB"};
    selection.initialSelectedOrdinal = 0;
    selection.snapshotEligible = false;
    if (placement == NestedSoundSelectorPlacement::ChildVoice)
    {
        selection.owner.kind = talas::project::ProjectVariableOwnerKind::MidiVoice;
        selection.owner.nodeId = "inner";
    }
    else
    {
        selection.owner.kind = talas::project::ProjectVariableOwnerKind::SoundDefinition;
        selection.owner.definitionId = selectionOwnerDefinitionId;
    }
    document.soundVariableLibrary.variables.push_back(std::move(selection));

    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "nested Sound selection project should survive save/reopen");
    document = reopened.document;

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 16;
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok()
            && compiled.graph->plan().midiVoices.size() == 3,
        "a polyphonic Script parent should lower one parent and two child Script owners"
            + diagnostics);

    const auto& plans = compiled.graph->plan().midiVoices;
    const auto parentPlanIterator = std::ranges::find_if(
        plans,
        [](const auto& plan) {
            return plan.ownerNodeId == "outer";
        });
    require(
        parentPlanIterator != plans.end()
            && parentPlanIterator->voices.size() == 2,
        "the Script parent should retain two independently addressed voice slots");
    std::array<const talas::runtime::MidiVoicePlan*, 2> childPlans{};
    for (const auto& plan : plans)
    {
        if (plan.ownerNodeId != "inner")
            continue;
        require(
            plan.voiceAncestors.size() == 1
                && plan.voiceAncestors.back().ownerNodeId == "outer"
                && plan.voices.size() == 1,
            "each Script child owner should retain one exact parent ancestry and one child slot");
        const auto parentOrdinal = plan.voiceAncestors.back().voiceOrdinal;
        require(
            parentOrdinal < childPlans.size()
                && childPlans[parentOrdinal] == nullptr
                && plan.parentVoiceScopeId.has_value()
                && *plan.parentVoiceScopeId
                    == parentPlanIterator->voices[parentOrdinal].scopeId,
            "each child Script owner should belong to its exact parent voice scope");
        childPlans[parentOrdinal] = &plan;
    }
    require(
        childPlans[0] != nullptr && childPlans[1] != nullptr,
        "both parent slots should own a scheduled Script child owner");

    const auto& prepared = compiled.graph->preparedSoundVariables();
    std::array<std::size_t, 2> selectionCells{};
    std::array<bool, 2> foundSelectionCell{};
    for (std::size_t index = 0; index < prepared.cells.size(); ++index)
    {
        const auto& cell = prepared.cells[index];
        const auto& identity = prepared.cells[index].identity;
        if (identity.variableId != "selected")
            continue;
        std::optional<std::uint16_t> parentOrdinal;
        if (placement == NestedSoundSelectorPlacement::ChildVoice)
        {
            if (identity.ownerKind
                    != talas::project::ProjectVariableOwnerKind::MidiVoice
                || identity.ownerNodeId != "inner")
                continue;
            if (!identity.voiceAncestors.empty())
                parentOrdinal = identity.voiceAncestors.back().voiceOrdinal;
        }
        else
        {
            if (identity.ownerKind
                    != talas::project::ProjectVariableOwnerKind::SoundDefinition
                || identity.definitionId != selectionOwnerDefinitionId)
                continue;
            // Sound-owned declarations also have a canonical cell. Only the
            // projected cell below the child voice represents this live input.
            if (identity.voiceAncestors.size() != 2)
                continue;
            parentOrdinal = identity.voiceAncestors.front().voiceOrdinal;
        }
        require(
            parentOrdinal.has_value()
                && *parentOrdinal < selectionCells.size()
                && !foundSelectionCell[*parentOrdinal]
                && cell.kind == talas::project::ProjectSoundVariableKind::Sound
                && cell.candidateDefinitionIds
                    == std::vector<std::string>{"toneA", "toneB"}
                && cell.initialSelectedOrdinal == 0
                && (placement == NestedSoundSelectorPlacement::ChildVoice
                    ? identity.voiceAncestors
                            == childPlans[*parentOrdinal]->voiceAncestors
                        && identity.definitionId
                            == childPlans[*parentOrdinal]->ownerDefinitionId
                        && identity.voiceOrdinal
                            == std::optional<std::uint16_t>{0}
                    : identity.voiceAncestors.size() == 2
                        && identity.voiceAncestors.front()
                            == childPlans[*parentOrdinal]->voiceAncestors.back()
                        && identity.voiceAncestors.back()
                            == talas::project::ProjectMidiVoiceAncestorIdentity{
                                childPlans[*parentOrdinal]->ownerNodeId,
                                childPlans[*parentOrdinal]->ownerDefinitionId,
                                childPlans[*parentOrdinal]->ownerInstancePath,
                                0}
                        && identity.voiceOrdinal == std::nullopt
                        && identity.ownerNodeId.empty()
                        && identity.instancePath
                            == std::vector<std::string>{"input"}
                        && cell.scopeOrdinal
                            != childPlans[*parentOrdinal]->voices.front().scopeId),
            placement == NestedSoundSelectorPlacement::ChildVoice
                ? "each child voice-owned selector identity should retain its exact parent ancestry"
                : "each projected Sound cell should retain full parent and child ancestry without a direct voice ordinal");
        selectionCells[*parentOrdinal] = index;
        foundSelectionCell[*parentOrdinal] = true;
    }
    require(
        foundSelectionCell[0] && foundSelectionCell[1],
        "both active nested parent slots should own separate Sound cells");
    for (std::size_t ordinal = 0; ordinal < selectionCells.size(); ++ordinal)
    {
        const auto& voice = childPlans[ordinal]->voices.front();
        const auto& indices = voice.soundVariableCellIndices;
        const auto exactIndex = std::ranges::find(indices, selectionCells[ordinal]);
        const auto inDeclaredRange = selectionCells[ordinal]
                >= voice.firstSoundVariableCellIndex
            && selectionCells[ordinal]
                < voice.firstSoundVariableCellIndex + voice.soundVariableCellCount;
        require(
            exactIndex != indices.end() || inDeclaredRange,
            "each nested child voice plan should claim its exact selector cell");
        if (placement != NestedSoundSelectorPlacement::ChildVoice)
        {
            require(
                exactIndex != indices.end()
                    && prepared.cells[selectionCells[ordinal]].scopeOrdinal
                        != voice.scopeId,
                "projected Sound selector cells should be explicitly owned below, and separately from, the physical child voice scope");
        }
    }

    std::array<const talas::runtime::CompileStep*, 2> selectorSteps{};
    for (std::size_t ordinal = 0; ordinal < selectorSteps.size(); ++ordinal)
    {
        for (const auto& step : compiled.graph->plan().steps)
        {
            if (step.soundVariableCellIndex != selectionCells[ordinal])
                continue;
            require(
                selectorSteps[ordinal] == nullptr
                    && step.typeId == "talas.variable",
                "each projected selector cell should bind to one exact variable step");
            selectorSteps[ordinal] = &step;
        }
        require(
            selectorSteps[ordinal] != nullptr,
            "each parent-owned selector cell should have one compiled selector step");
        const auto physicalVoiceScope =
            childPlans[ordinal]->voices.front().scopeId;
        const auto declarationScope =
            prepared.cells[selectionCells[ordinal]].scopeOrdinal;
        require(
            selectorSteps[ordinal]->scopeId == physicalVoiceScope,
            "each selector operation should run in its exact child voice scope");
        if (placement == NestedSoundSelectorPlacement::ChildInputSound)
        {
            require(
                selectorSteps[ordinal]->eventValueScopeId == declarationScope
                    && declarationScope != physicalVoiceScope,
                "a selector in its owner Sound should resolve from that projected Sound cell scope");
        }
        else if (placement
            == NestedSoundSelectorPlacement::ChildSoundAncestor)
        {
            require(
                selectorSteps[ordinal]->eventValueScopeId != declarationScope
                    && selectorSteps[ordinal]->eventValueScopeId
                        != physicalVoiceScope
                    && declarationScope != physicalVoiceScope,
                "a selector in a further child Sound should resolve its enclosing declaration from the nearest projected lexical scope");
        }
    }

    auto audibleDocument = document;
    audibleDocument.soundVariableLibrary.variables.front().initialSelectedOrdinal = 1;
    const auto audibleCompiled = talas::runtime::compileProject(
        audibleDocument, talas::core::makeBuiltinNodeRegistry(), settings);
    require(audibleCompiled.ok(), "initially audible nested selectors should compile");
    const auto offline = talas::runtime::OfflineRenderSession(*audibleCompiled.graph).render(32);
    require(offline.ok(), offline.diagnostics.empty()
        ? "nested selected Sound should render offline" : offline.diagnostics.front().message);
    require(offline.mono.size() == 32 && offline.right.size() == 32,
        "nested selector offline output should retain both full channels");
    for (std::size_t frame = 0; frame < offline.mono.size(); ++frame)
    {
        // Parent Inputs start at 5/15. Each child immediately allocates its
        // note, so its constant Sound contributes before the child's Attack.
        const auto expected = (frame >= 5 ? 0.5 : 0.0) + (frame >= 15 ? 0.5 : 0.0);
        require(std::abs(offline.mono[frame] - expected) < 1.0e-12
                && std::abs(offline.right[frame] - expected) < 1.0e-12,
            "offline nested selector must follow both independent parent Input origins");
    }

    talas::runtime::RealtimeRenderSession candidate(*compiled.graph);
    talas::runtime::RealtimeRenderSession control(*compiled.graph);
    require(
        candidate.ready() && control.ready(),
        "nested Sound selection sessions should prepare: "
            + std::string(candidate.error()) + " " + std::string(control.error()));

    for (const bool replaceCell : {false, true})
    {
        auto forged = *compiled.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        const auto selector = std::ranges::find_if(plan.steps, [&](const auto& step) {
            return step.soundVariableCellIndex == selectionCells[0];
        });
        require(selector != plan.steps.end(), "first child must retain its selector step");
        if (replaceCell)
            selector->soundVariableCellIndex = selectionCells[1];
        else
        {
            const auto siblingScope =
                placement == NestedSoundSelectorPlacement::ChildVoice
                ? childPlans[1]->voices.front().scopeId
                : selectorSteps[1]->eventValueScopeId;
            require(
                siblingScope != selectorSteps[0]->eventValueScopeId,
                "sibling selector scopes should have distinct prepared identities");
            selector->eventValueScopeId = siblingScope;
        }
        require(!talas::runtime::RealtimeRenderSession(forged).ready()
                && !talas::runtime::validateOfflineRuntimeGraph(forged).ok(),
            "replacing a child selector cell or exact projected EventValue scope with its sibling must reject");
    }

    const auto childValueForParentSlot = [&](
                                const talas::runtime::RealtimeRenderSession& session,
                                std::uint16_t ordinal) {
        const auto snapshot = session.preparedSoundVariableValues();
        require(
            snapshot.publicationValid,
            "nested Sound selection should publish its exact child slot state");
        const auto found = std::ranges::find_if(
            snapshot.values,
            [&](const auto& value) {
                return value.identity == prepared.cells[selectionCells[ordinal]].identity;
            });
        require(
            found != snapshot.values.end(),
            "nested Sound selection publication should retain parent-projected cell identity");
        return *found;
    };

    const auto renderBlock = [](talas::runtime::RealtimeRenderSession& session) {
        std::array<double, 16> output{};
        require(session.render(output), "nested Sound selection should render its prepared block");
        return output;
    };
    const auto maxAbs = [](const auto& samples) {
        double result = 0.0;
        for (const auto sample : samples)
            result = std::max(result, std::abs(sample));
        return result;
    };

    const auto activateTwoVoices = [&](talas::runtime::RealtimeRenderSession& session,
                                      std::uint64_t expectedGeneration = 1) {
        const auto initial = renderBlock(session);
        const auto second = renderBlock(session);
        require(
            std::ranges::all_of(initial, [](double sample) { return sample == 0.0; })
                && std::ranges::all_of(second, [](double sample) { return sample == 0.0; }),
            "the Script parent should activate two initially silent child Script owners");
        for (std::uint16_t ordinal = 0; ordinal < 2; ++ordinal)
        {
            const auto value = childValueForParentSlot(session, ordinal);
            require(
                value.voiceGeneration == expectedGeneration && value.voiceActive
                    && value.selectedDefinitionId == "toneA",
                "both scheduled child slots should begin with the current generation and initial Sound");
        }
    };
    activateTwoVoices(candidate);
    activateTwoVoices(control);

    const auto makeSelectionEvent = [&](talas::runtime::RealtimeRenderSession& session,
                                        std::uint16_t ordinal,
                                        std::string_view definitionId,
                                        std::uint64_t voiceGeneration,
                                        std::uint64_t sequence) {
        const auto target = session.voiceSoundVariableTargetIndex(
            prepared.cells[selectionCells[ordinal]].identity,
            definitionId,
            voiceGeneration);
        require(
            target.has_value(),
            "live nested Sound selector should resolve its exact child generation and parent ancestry");
        talas::runtime::TypedEventPayload payload{};
        payload.soundVariable = {target->selectedOrdinal};
        return talas::runtime::PreparedTypedEvent{
            talas::runtime::TypedEventKind::SoundVariableAssignment,
            {
                talas::runtime::TypedEventTargetKind::SoundVariable,
                target->index,
                1,
                1,
                voiceGeneration,
            },
            0,
            sequence,
            payload,
        };
    };
    const auto scheduleOneSelection = [&](talas::runtime::RealtimeRenderSession& session,
                                          std::uint16_t ordinal,
                                          std::string_view definitionId,
                                          std::uint64_t sequence) {
        const auto generation = childValueForParentSlot(session, ordinal).voiceGeneration;
        const std::array events{
            makeSelectionEvent(session, ordinal, definitionId, generation, sequence),
        };
        std::array<talas::runtime::RealtimeTypedEventDispatchResult, 1> results{};
        require(
            session.scheduleTypedEvents(events, results),
            "exact nested Sound selection should enter the realtime transaction queue");
    };

    scheduleOneSelection(candidate, 0, "toneB", 1);
    scheduleOneSelection(control, 0, "toneB", 1);
    for (int block = 0; block < 8; ++block)
    {
        (void)renderBlock(candidate);
        (void)renderBlock(control);
    }
    const auto oneSelected = renderBlock(candidate);
    const auto oneSelectedControl = renderBlock(control);
    require(
        oneSelected == oneSelectedControl
            && std::abs(maxAbs(oneSelected) - 0.5) < 1.0e-12
            && childValueForParentSlot(candidate, 0).selectedDefinitionId == "toneB"
            && childValueForParentSlot(candidate, 1).selectedDefinitionId == "toneA",
        "selecting one parent-owned child should render the 0.5-amplitude candidate while leaving its sibling on silence");

    const auto appliedBeforeRejection = makeSelectionEvent(
        candidate,
        1,
        "toneB",
        childValueForParentSlot(candidate, 1).voiceGeneration,
        2);
    auto staleSelection = makeSelectionEvent(
        candidate,
        0,
        "toneA",
        childValueForParentSlot(candidate, 0).voiceGeneration,
        3);
    ++staleSelection.target.voiceGeneration;
    const std::array rejectedBatch{appliedBeforeRejection, staleSelection};
    std::array<talas::runtime::RealtimeTypedEventDispatchResult, 2> rejectedResults{};
    std::array<double, 16> rejectedOutput{};
    rejectedOutput.fill(9.0);
    require(
        candidate.scheduleTypedEvents(rejectedBatch, rejectedResults)
            && !candidate.render(rejectedOutput)
            && std::ranges::all_of(rejectedOutput, [](double value) { return value == 0.0; }),
        "a stale second child target should roll back an earlier valid selection in the same transaction");

    // Hierarchical callback rollback retains input queues. The invalid batch
    // must reject unchanged retries; a transport reset explicitly cancels it.
    rejectedOutput.fill(9.0);
    require(
        !candidate.render(rejectedOutput)
            && std::ranges::all_of(rejectedOutput, [](double value) { return value == 0.0; })
            && childValueForParentSlot(candidate, 0).voiceGeneration == 1
            && childValueForParentSlot(candidate, 1).voiceGeneration == 1
            && childValueForParentSlot(candidate, 0).selectedDefinitionId == "toneB"
            && childValueForParentSlot(candidate, 1).selectedDefinitionId == "toneA",
        "repeated rejection should preserve both child selections and generations");
    for (auto* session : {&candidate, &control})
    {
        session->resetTransport();
        activateTwoVoices(*session, 2);
        scheduleOneSelection(*session, 0, "toneB", 4);
        for (int block = 0; block < 9; ++block) (void)renderBlock(*session);
    }
    const auto expectedContinuation = renderBlock(control);
    const auto actualContinuation = renderBlock(candidate);
    require(actualContinuation == expectedContinuation
            && std::abs(maxAbs(actualContinuation) - 0.5) < 1.0e-12,
        "reset should cancel the invalid batch and recover exact child selection audio");

    scheduleOneSelection(candidate, 1, "toneB", 5);
    scheduleOneSelection(control, 1, "toneB", 5);
    for (int block = 0; block < 8; ++block)
    {
        (void)renderBlock(candidate);
        (void)renderBlock(control);
    }
    const auto bothSelected = renderBlock(candidate);
    const auto bothSelectedControl = renderBlock(control);
    require(
        bothSelected == bothSelectedControl
            && std::abs(maxAbs(bothSelected) - 1.0) < 1.0e-12,
        "both parent-scoped child selectors should contribute their prepared Sound branches");

    for (int block = 0; block < 18; ++block)
    {
        (void)renderBlock(candidate);
        (void)renderBlock(control);
    }
    const auto reused = renderBlock(candidate);
    const auto reusedControl = renderBlock(control);
    const auto firstAfterReuse = childValueForParentSlot(candidate, 0);
    const auto siblingAfterReuse = childValueForParentSlot(candidate, 1);
    require(
        reused == reusedControl
            && std::abs(maxAbs(reused) - 0.5) < 1.0e-12
            && firstAfterReuse.voiceGeneration == 3
            && firstAfterReuse.voiceActive
            && firstAfterReuse.selectedDefinitionId == "toneA"
            && siblingAfterReuse.voiceGeneration == 2
            && siblingAfterReuse.voiceActive
            && siblingAfterReuse.selectedDefinitionId == "toneB"
            && !candidate.voiceSoundVariableTargetIndex(
                    prepared.cells[selectionCells[0]].identity, "toneB", 2)
                    .has_value()
            && candidate.voiceSoundVariableTargetIndex(
                    prepared.cells[selectionCells[0]].identity, "toneB", 3)
                    .has_value(),
        "parent reuse should reset only its selector, preserve the sibling's 0.5-amplitude branch, and reject the stale generation");
}

void testNestedMidiVoiceSoundSelectionIsPerVoiceAndTransactional()
{
    runNestedMidiVoiceSoundSelectionFixture(
        NestedSoundSelectorPlacement::ChildVoice);
}

void testNestedMidiVoiceSoundSelectionUsesProjectedSoundCell()
{
    runNestedMidiVoiceSoundSelectionFixture(
        NestedSoundSelectorPlacement::ChildInputSound);
}

void testNestedMidiVoiceSoundSelectionUsesNearestSoundAncestor()
{
    runNestedMidiVoiceSoundSelectionFixture(
        NestedSoundSelectorPlacement::ChildSoundAncestor);
}

void testReusableSoundVoiceVariablesMaterializePerExactInstance()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "sharedVoice";
    definition.displayName = "Shared Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "midi",
        "leftOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", "220 hz"),
            expression("amplitude", "@voiceGain * !KeyDown"),
        },
        {},
    });
    definition.graph.nodes.push_back({
        "midi",
        "talas.midiVoice",
        {
            expression("left", "1"),
            expression("right", "1"),
            expression("channel", "1"),
            textParameter("source", ""),
            resourceParameter("midiFile", ""),
            expression("polyphony", "1"),
            expression("lowPitch", "0 nn"),
            expression("highPitch", "127 nn"),
            textParameter("script", ""),
            booleanParameter("shared", false),
        },
        {},
    });
    definition.graph.connections.push_back(
        {"osc", "audioOut", "midi", "audioIn"});
    document.soundDefinitions.push_back(std::move(definition));
    document.variableLibrary.variables.push_back({
        "voiceGain",
        "Voice Gain",
        talas::expr::ValueDomain::Scalar,
        0.5,
        {
            talas::project::ProjectVariableOwnerKind::MidiVoice,
            "sharedVoice",
            "midi",
        },
        false,
    });
    document.graph.nodes = {
        soundInstance("second", "sharedVoice"),
        soundInstance("first", "sharedVoice"),
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"first", "audioOut", "mix", "in1"},
        {"second", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 16;
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        settings);
    std::string diagnostics;
    for (const auto& diagnostic : compiled.diagnostics)
        diagnostics += "\n" + diagnostic.message;
    require(
        compiled.ok()
            && compiled.graph->plan().midiVoices.size() == 2
            && compiled.graph->preparedVariables().cells.size() == 2,
        "one reusable Sound voice declaration should expand independently for two exact instances"
            + diagnostics);

    const auto& plans = compiled.graph->plan().midiVoices;
    const auto& cells = compiled.graph->preparedVariables().cells;
    require(
        plans[0].ownerInstancePath
                == std::vector<std::string>{"first"}
            && plans[1].ownerInstancePath
                == std::vector<std::string>{"second"},
        "reusable Sound voice plans should use deterministic exact instance ordering");
    for (std::size_t index = 0; index < plans.size(); ++index)
    {
        require(
            plans[index].ownerNodeId == "midi"
                && plans[index].ownerDefinitionId == "sharedVoice"
                && plans[index].voices.size() == 1
                && plans[index].voices.front().variableCellCount == 1
                && plans[index].voices.front().firstVariableCellIndex
                    == index
                && cells[index].identity.instancePath
                    == plans[index].ownerInstancePath
                && cells[index].identity.voiceOrdinal
                    == std::optional<std::uint16_t>{0},
            "each expanded reusable Sound instance should own a disjoint exact voice-cell range");
    }
}
}

int main()
{
    testSoundExpansionEmitsDeterministicStructuralManifest();
    testNestedSoundExpansionCreatesIndependentDeterministicSteps();
    testSoundInterfaceDefaultsOverridesAndAliasesExpandIndependently();
    testLocalSoundEventValueBindingsAreIsolatedPerInstanceScope();
    testGlobalToLocalSoundEventValueProjectionRespectsSparseOverrides();
    testPreparedSoundVariablesRemainInstanceLocalAndTransactional();
    testSoundInterfaceOneToManyBindingsApplyTransactionally();
    testTransitiveSoundInterfaceBindingsPreserveQualifiedIdentity();
    testEveryTypedSoundInterfaceKindMaterializesDuringExpansion();
    testQualifiedSoundParameterIdentityEncodesPathSegmentsInjectively();
    testSoundDefinitionsCanNestTransitively();
    testNestedCompileDiagnosticsRetainStructuralSourcePath();
    testFlatRootCompilationRetainsGlobalScope();
    testUnusedDefinitionsPreserveReachableRootCompilation();
    testReachableUnknownRootSoundInstanceFailsClosed();
    testSoundExpansionRejectsCompactExponentialGraphs();
    testPreparedRealtimeUsesTheSameExpandedSoundGraph();
    testNestedFeedbackPairsKeepIndependentScopedStateAcrossPartitions();
    testFeedbackPairNamesNeverCrossSoundScopes();
    testNestedSoundRetainsLegacyPreparedMidiVoiceLowering();
    testMultipleNestedPreparedMidiVoiceRegionsUseDisjointScopesAndRouting();
    testNestedMidiVoiceSoundSelectionIsPerVoiceAndTransactional();
    testNestedMidiVoiceSoundSelectionUsesProjectedSoundCell();
    testNestedMidiVoiceSoundSelectionUsesNearestSoundAncestor();
    testReusableSoundVoiceVariablesMaterializePerExactInstance();
    return 0;
}
