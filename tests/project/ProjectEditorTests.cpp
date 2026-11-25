#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGraphClipboard.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectScoreClipFingerprint.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/ProjectVariables.h"

#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::CanvasPosition nodePosition(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId,
    std::optional<std::string_view> soundDefinitionId = std::nullopt)
{
    const auto* presented = talas::project::findProjectNodePresentation(
        document,
        soundDefinitionId,
        nodeId);
    require(presented != nullptr, "expected node presentation entry");
    return presented->position;
}

void requireOk(const talas::project::EditResult& result, const std::string& message)
{
    require(result.ok(), message + ": " + result.message);
}

void requireError(const talas::project::EditResult& result,
                  talas::project::EditError expected,
                  const std::string& message)
{
    require(!result.ok(), message + " should fail");
    require(result.error.has_value(), message + " should report an error");
    require(*result.error == expected, message + " should report the expected error");
}

talas::core::NodeRegistry& builtinRegistry()
{
    static talas::core::NodeRegistry registry = [] {
        talas::core::NodeRegistry builtins;
        talas::core::registerBuiltinNodeTypes(builtins);
        return builtins;
    }();

    return registry;
}

talas::project::ProjectEditor makeEditor()
{
    return {talas::project::ProjectDocument{}, builtinRegistry()};
}

talas::project::MidiControlMapping exactMidiMapping(
    int channel,
    int controller,
    std::string identifier = "device-a")
{
    return {
        channel,
        controller,
        talas::project::MidiControlSourceMode::ExactInput,
        std::move(identifier)};
}

talas::project::ProjectPreservedComponent preservedEditorComponent()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    const auto monoAudio = talas::core::PortSignalContract{
        talas::core::PortSignalRate::Sample,
        talas::core::AudioBusLayoutKind::Mono,
        talas::core::PortEdgeSemantics::Continuous,
        talas::core::SpectralFrameFormat::NotApplicable,
        true,
    };
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        monoAudio,
    });
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        monoAudio,
    });
    component.parameters.push_back({
        "mode",
        talas::core::ParameterKind::Choice,
        talas::expr::ValueDomain::Scalar,
        {"clean", "colour"},
    });
    component.recoveryHint = "Install the matching owner-approved bundle";
    return component;
}

talas::project::ProjectDocument preservedEditorDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    talas::project::ProjectParameterExpression mode;
    mode.parameterId = "mode";
    mode.choice = "colour";
    missing.parameters.push_back(std::move(mode));
    missing.preservedComponent = preservedEditorComponent();
    talas::project::ProjectNode output;
    output.instanceId = "out";
    output.typeId = "talas.audioOutput";
    document.graph.nodes = {
        std::move(oscillator),
        std::move(missing),
        std::move(output),
    };
    document.graph.connections = {
        {"osc", "audioOut", "missing1", "audioIn"},
        {"missing1", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "missing1")
        ->position = {120, 80};

    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    target.nodeId = "missing1";
    target.parameterId = "mode";
    talas::project::SnapshotValue value;
    value.kind = talas::project::SnapshotValueKind::Choice;
    value.choice = "colour";
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "preserved";
    snapshot.displayName = "Preserved";
    snapshot.values.push_back({target, value});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "project";
    bank.displayName = "Project";
    bank.snapshots.push_back(std::move(snapshot));
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

talas::project::ProjectDocument nestedPreservedEditorDocument()
{
    auto document = preservedEditorDocument();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph = std::move(document.graph);
    document.soundDefinitions.push_back(std::move(definition));
    document.snapshotLibrary = {};
    document.presentation = {};
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        "voice",
        "missing1")
        ->position = {120, 80};
    return document;
}

talas::project::ProjectParameterExpression expressionParameter(std::string id, std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
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

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
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

talas::project::ProjectSoundDefinition makeVoiceDefinition(
    std::string definitionId = "voice")
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = "Voice";
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
    definition.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    return definition;
}

talas::project::ProjectSoundExposedParameter exposedFrequency(
    std::string defaultExpression = "440 hz")
{
    talas::project::ProjectSoundExposedParameter parameter;
    parameter.parameterId = "frequency";
    parameter.displayName = "Frequency";
    parameter.kind = talas::core::ParameterKind::Expression;
    parameter.domain = talas::expr::ValueDomain::Hertz;
    parameter.defaultValue = expressionParameter(
        "frequency",
        std::move(defaultExpression));
    parameter.aliases = {"pitchHz"};
    parameter.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    });
    return parameter;
}

void testSoundInstanceOverridesUseTheExposedInterfaceAndUndo()
{
    auto editor = makeEditor();
    auto definition = makeVoiceDefinition();
    definition.inputs.clear();
    definition.exposedParameters.push_back(exposedFrequency());
    requireOk(
        editor.addSoundDefinition(std::move(definition)),
        "editor should add a typed Sound interface");
    requireOk(
        editor.addSoundInstance("voice1", "voice", {}),
        "editor should add an instance of the typed Sound");
    requireOk(
        editor.setParameter(
            "voice1",
            expressionParameter("frequency", "220 hz")),
        "editor should persist a sparse typed Sound override");
    require(
        editor.document().graph.nodes.front().parameters
            == std::vector<talas::project::ProjectParameterExpression>{
                expressionParameter("frequency", "220 hz")},
        "Sound instance should store only its explicit override");
    requireOk(editor.undo(), "Sound override should be undoable");
    require(
        editor.document().graph.nodes.front().parameters.empty(),
        "undo should restore default fallback by removing the sparse override");
    requireOk(editor.redo(), "Sound override should be redoable");
    require(
        editor.document().graph.nodes.front().parameters.front().expression
            == std::optional<std::string>{"220 hz"},
        "redo should restore the sparse Sound override");
    requireOk(
        editor.inlineSoundInstance("voice1"),
        "inlining should preserve the resolved Sound override");
    require(
        editor.document().graph.nodes.size() == 1
            && editor.document().graph.nodes.front().typeId
                == "talas.oscillator"
            && std::ranges::any_of(
                editor.document().graph.nodes.front().parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "frequency"
                        && parameter.expression
                            == std::optional<std::string>{"220 hz"};
                }),
        "inline should materialize the sparse override on its internal target");
    requireOk(editor.undo(),
              "override-preserving inline should remain one undo transaction");
    require(
        editor.document().graph.nodes.front().soundDefinitionId
                == std::optional<std::string>{"voice"}
            && editor.document().graph.nodes.front().parameters.front()
                .expression
                == std::optional<std::string>{"220 hz"},
        "undo should restore the Sound instance and its sparse override");
}

void testSoundInstanceOverrideReplacesItsAliasSpelling()
{
    auto definition = makeVoiceDefinition();
    definition.inputs.clear();
    definition.exposedParameters.push_back(exposedFrequency());
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    // An override saved under the alias of the exposed parameter.
    instance.parameters.push_back(expressionParameter("pitchHz", "330 hz"));
    document.graph.nodes.push_back(std::move(instance));
    talas::project::reconcileProjectPresentation(document);
    talas::project::ProjectEditor editor(std::move(document), builtinRegistry());

    requireOk(
        editor.setParameter(
            "voice1",
            expressionParameter("frequency", "220 hz")),
        "editing an alias-spelled override should succeed");
    require(
        editor.document().graph.nodes.front().parameters
            == std::vector<talas::project::ProjectParameterExpression>{
                expressionParameter("frequency", "220 hz")},
        "the edit should replace the alias override, not add a duplicate");
    require(
        talas::project::validateProjectDocument(
            editor.document(), builtinRegistry()).ok(),
        "the edited instance should still validate");
}

void testSoundInterfaceValidationRejectsInvalidBindingsAndAliases()
{
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("dangling");
        definition.inputs.clear();
        auto parameter = exposedFrequency();
        parameter.bindings.front().nodeId = "missing";
        definition.exposedParameters.push_back(std::move(parameter));
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "Sound interface binding to a missing internal node");
    }
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("ambiguousAlias");
        definition.inputs.clear();
        auto first = exposedFrequency();
        first.parameterId = "first";
        first.defaultValue.parameterId = "first";
        auto second = exposedFrequency();
        second.parameterId = "second";
        second.defaultValue.parameterId = "second";
        second.aliases = {"pitchHz"};
        definition.exposedParameters = {
            std::move(first),
            std::move(second),
        };
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "Sound parameter aliases must resolve unambiguously");
    }
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("wrongDomain");
        definition.inputs.clear();
        auto parameter = exposedFrequency();
        parameter.domain = talas::expr::ValueDomain::Seconds;
        parameter.defaultValue.expression = "1 s";
        definition.exposedParameters.push_back(std::move(parameter));
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "Sound expression binding domain must match the internal parameter");
    }
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("nonExpressionDomain");
        definition.inputs.clear();
        talas::project::ProjectSoundExposedParameter interpolation;
        interpolation.parameterId = "interpolation";
        interpolation.displayName = "Interpolation";
        interpolation.kind = talas::core::ParameterKind::Choice;
        interpolation.domain = talas::expr::ValueDomain::Hertz;
        interpolation.choices = {"linear", "none"};
        interpolation.defaultValue =
            choiceParameter("interpolation", "linear");
        interpolation.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            "osc",
            "interpolation",
            {},
            talas::expr::ValueDomain::Scalar,
        });
        definition.exposedParameters.push_back(
            std::move(interpolation));
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "non-expression Sound interfaces should use the scalar domain");
    }
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("duplicateChoice");
        definition.inputs.clear();
        talas::project::ProjectSoundExposedParameter interpolation;
        interpolation.parameterId = "interpolation";
        interpolation.displayName = "Interpolation";
        interpolation.kind = talas::core::ParameterKind::Choice;
        interpolation.choices = {"linear", "linear"};
        interpolation.defaultValue =
            choiceParameter("interpolation", "linear");
        interpolation.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            "osc",
            "interpolation",
            {},
            talas::expr::ValueDomain::Scalar,
        });
        definition.exposedParameters.push_back(
            std::move(interpolation));
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "Choice-kind Sound interfaces should reject duplicate values");
    }
    {
        auto editor = makeEditor();
        auto definition = makeVoiceDefinition("unknownBindingTarget");
        definition.inputs.clear();
        auto parameter = exposedFrequency();
        parameter.bindings.front().target =
            static_cast<talas::project::ProjectSoundParameterTarget>(99);
        parameter.bindings.front().nodeId.clear();
        parameter.bindings.front().parameterId.clear();
        parameter.bindings.front().eventName = "Frequency";
        parameter.bindings.front().eventDomain =
            talas::expr::ValueDomain::Hertz;
        definition.exposedParameters.push_back(
            std::move(parameter));
        requireError(
            editor.addSoundDefinition(std::move(definition)),
            talas::project::EditError::InvalidSoundDefinition,
            "unknown Sound binding target enum values should fail closed");
    }
}

void testSoundDefinitionsAndInstancesAreValidatedAndUndoable()
{
    auto editor = makeEditor();
    requireOk(
        editor.addSoundDefinition(makeVoiceDefinition()),
        "editor should add a valid Sound definition");
    requireOk(
        editor.addSoundInstance("voice1", "voice", {128, 64}),
        "editor should add a persisted Sound instance");
    require(
        editor.document().graph.nodes.front().soundDefinitionId
            == std::optional<std::string>{"voice"},
        "Sound instance should retain the explicit definition identity");

    requireOk(
        editor.addNode({"control", "talas.constant", {}, {}}),
        "editor should add a control source");
    requireOk(
        editor.connect({"control", "value", "voice1", "frequency"}),
        "dynamic Sound input descriptor should accept a matching connection");
    // Audio and control signals feed each other (ADR-0016); a spectrum
    // does not feed a Sound's control input.
    requireOk(
        editor.addNode({"spectrum", "talas.forwardFftV1", {}, {}}),
        "editor should add a spectrum source");
    requireError(
        editor.connect({"spectrum", "spectrumOut", "voice1", "frequency"}),
        talas::project::EditError::IncompatiblePortTypes,
        "dynamic Sound descriptor should reject mismatched boundary types");
    requireOk(editor.undo(), "adding the spectrum source should undo");
    requireError(
        editor.removeSoundDefinition("voice"),
        talas::project::EditError::SoundDefinitionInUse,
        "referenced Sound definition removal");

    requireOk(
        editor.renameSoundDefinition("voice", "Renamed Voice"),
        "Sound display name should be independently renameable");
    require(
        editor.document().soundDefinitions.front().definitionId == "voice"
            && editor.document().soundDefinitions.front().displayName
                == "Renamed Voice",
        "renaming should preserve stable Sound identity");
    requireOk(editor.undo(), "Sound definition rename should be undoable");
    require(
        editor.document().soundDefinitions.front().displayName == "Voice",
        "undo should restore the Sound display name");
    requireOk(editor.redo(), "Sound definition rename should be redoable");
    require(
        editor.document().soundDefinitions.front().displayName == "Renamed Voice",
        "redo should restore the Sound display name");
}

void testSoundValidationRejectsMissingBoundariesAndRecursion()
{
    auto editor = makeEditor();
    requireError(
        editor.addSoundInstance("missing", "unknown", {}),
        talas::project::EditError::UnknownSoundDefinition,
        "instance referencing a missing Sound definition");

    auto invalidBoundary = makeVoiceDefinition("badBoundary");
    invalidBoundary.inputs.front().nodePortId = "missing";
    requireError(
        editor.addSoundDefinition(std::move(invalidBoundary)),
        talas::project::EditError::InvalidSoundDefinition,
        "Sound boundary referencing a missing internal port");

    auto recursive = makeVoiceDefinition("recursive");
    talas::project::ProjectNode self;
    self.instanceId = "self";
    self.soundDefinitionId = "recursive";
    recursive.graph.nodes.push_back(std::move(self));
    requireError(
        editor.addSoundDefinition(std::move(recursive)),
        talas::project::EditError::InvalidSoundDefinition,
        "directly recursive Sound definition");

    talas::project::ProjectDocument mutuallyRecursive;
    auto first = makeVoiceDefinition("first");
    auto second = makeVoiceDefinition("second");
    talas::project::ProjectNode secondInstance;
    secondInstance.instanceId = "secondInstance";
    secondInstance.soundDefinitionId = "second";
    first.graph.nodes.push_back(secondInstance);
    talas::project::ProjectNode firstInstance;
    firstInstance.instanceId = "firstInstance";
    firstInstance.soundDefinitionId = "first";
    second.graph.nodes.push_back(firstInstance);
    mutuallyRecursive.soundDefinitions = {std::move(first), std::move(second)};
    requireError(
        editor.replaceDocument(std::move(mutuallyRecursive)),
        talas::project::EditError::InvalidSoundDefinition,
        "mutually recursive Sound definitions");
}

// A Script's events bind the variables their
// keywords name in the Input they go to: with `filts start: 0 s freqs: ...
// amps: ...` ?amps and ?freqs are not free, neither asked for as the Sound
// plays nor parameters of a class made from it, while ?nbrFilts, which the
// Script reads, is; taken out of the event, ?amps is free again.
void testScriptBoundVariablesAreNotFree()
{
    const auto documentWith = [](std::string script) {
        talas::project::ProjectDocument document;
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
        talas::project::ProjectNode filts{"filts", "talas.oscillator", {}};
        filts.parameters = {{"frequency", "?freqs hz"}, {"amplitude", "?amps"}};
        talas::project::ProjectNode score{"score", "talas.script", {}};
        score.parameters = {textParameter("script", std::move(script))};
        document.graph.nodes = {filts, score, {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"filts", "audioOut", "score", "inputs"}, {"score", "leftOut", "out", "audioIn"}};
        return document;
    };
    const auto bound = documentWith("1 to: ?nbrFilts do: [:f | filts start: 0 s freqs: f * 100 amps: 1 / f]");
    auto asked = talas::project::undeclaredProjectVariables(bound);
    std::ranges::sort(asked);
    require(asked == std::vector<std::string>{"nbrFilts"},
        "variables the Script's events give should not be asked for");
    talas::project::ProjectEditor editor{bound, builtinRegistry()};
    requireOk(editor.extractNodesToSound({"filts", "score"}, "ambience", "Ambience", "ambience1"), "make a class of the example");
    const auto& definition = editor.document().soundDefinitions.front();
    require(std::ranges::none_of(definition.exposedParameters, [](const auto& exposed) {
                return exposed.parameterId == "amps" || exposed.parameterId == "freqs";
            }),
        "a variable the Script binds should not be a parameter of the class");
    auto freed = talas::project::undeclaredProjectVariables(documentWith("1 to: ?nbrFilts do: [:f | filts start: 0 s freqs: f * 100]"));
    std::ranges::sort(freed);
    require(freed == std::vector<std::string>{"amps", "nbrFilts"},
        "taken out of the event, ?amps should be a free variable again");
}

// Expand (Inline here) expands a Sound one
// level only; a Sound it is made of that is itself a class's Sound stays one,
// to be expanded further on its own.
void testExpandingASoundOneLevelAtATime()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc", "talas.oscillator", {}}, {40, 80}), "add the oscillator");
    requireOk(editor.addNode({"vca", "talas.vca", {}}, {260, 80}), "add the VCA");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {480, 80}), "add the output");
    requireOk(editor.connect({"osc", "audioOut", "vca", "audioIn"}), "feed the VCA");
    requireOk(editor.connect({"vca", "audioOut", "out", "audioIn"}), "connect the VCA");
    requireOk(editor.extractNodesToSound({"vca"}, "amplifier", "Amplifier", "amp"), "make the VCA a Sound of its own");
    requireOk(editor.extractNodesToSound({"amp"}, "stage", "Stage", "stage1"), "make a Sound of that Sound");
    const auto instancesOf = [&](std::string_view definitionId) {
        return std::ranges::count_if(editor.document().graph.nodes, [&](const auto& node) {
            return node.soundDefinitionId == std::optional<std::string>{std::string{definitionId}};
        });
    };
    require(instancesOf("stage") == 1 && instancesOf("amplifier") == 0, "the root holds the outer Sound");
    requireOk(editor.inlineSoundInstance("stage1"), "expand the outer Sound once");
    require(instancesOf("stage") == 0 && instancesOf("amplifier") == 1,
        "expanding should go one level only, the inner Sound left a Sound of its class");
    const auto inner = std::ranges::find_if(editor.document().graph.nodes, [](const auto& node) {
        return node.soundDefinitionId == std::optional<std::string>{"amplifier"};
    });
    requireOk(editor.inlineSoundInstance(inner->instanceId), "expand the inner Sound further");
    require(instancesOf("amplifier") == 0 && editor.validateForRender().ok(), "the Sound is flat and plays");
}

void testExtractAndInlineSoundAreAtomicTopologyPreservingEdits()
{
    auto editor = makeEditor();
    requireOk(
        editor.addNode({"osc", "talas.oscillator", {}}, {40, 80}),
        "extract fixture should add oscillator");
    requireOk(
        editor.addNode({"vca", "talas.vca", {}}, {260, 80}),
        "extract fixture should add VCA");
    requireOk(
        editor.addNode({"out", "talas.audioOutput", {}}, {480, 80}),
        "extract fixture should add output");
    requireOk(
        editor.connect({"osc", "audioOut", "vca", "audioIn"}),
        "extract fixture should connect its input boundary");
    requireOk(
        editor.connect({"vca", "audioOut", "out", "audioIn"}),
        "extract fixture should connect its output boundary");

    requireOk(
        editor.extractNodesToSound(
            {"vca"},
            "amplifier",
            "Amplifier",
            "amp"),
        "selected subgraph should extract to one Sound definition and instance");
    require(
        editor.document().soundDefinitions.size() == 1
            && editor.document().soundDefinitions.front().inputs.size()
                == 1
            && editor.document().soundDefinitions.front().outputs.size()
                == 1,
        "extraction should derive one input and output boundary");
    require(
        editor.document().graph.nodes.size() == 3
            && editor.document().graph.connections.size() == 2
            && editor.validateForRender().ok(),
        "extracted root topology should remain renderable");
    const auto extracted = editor.document();

    requireOk(editor.undo(), "extraction should be one undo transaction");
    require(
        editor.document().soundDefinitions.empty()
            && editor.document().graph.nodes[1].instanceId == "vca",
        "one undo should restore the complete flat topology");
    requireOk(editor.redo(), "extraction should be redoable");
    require(
        editor.document().soundDefinitions.size() == 1
            && editor.document().graph.nodes.size()
                == extracted.graph.nodes.size(),
        "redo should restore definition and instance together");

    requireOk(
        editor.inlineSoundInstance("amp"),
        "Sound instance should inline back into the root graph");
    require(
        std::ranges::none_of(
            editor.document().graph.nodes,
            [](const auto& node) {
                return node.soundDefinitionId.has_value();
            })
            && editor.document().graph.connections.size() == 2
            && editor.validateForRender().ok(),
        "inline should restore a flat renderable topology");
    const auto inlinedVca = std::ranges::find_if(
        editor.document().graph.nodes,
        [](const auto& node) {
            return node.typeId == "talas.vca";
        });
    require(
        inlinedVca != editor.document().graph.nodes.end(),
        "inline should retain the selected node identity");
    require(
        nodePosition(editor.document(), inlinedVca->instanceId)
            == talas::project::CanvasPosition{260, 80},
        "extract and inline should preserve the selected node position");
    requireOk(editor.undo(), "inline should be one undo transaction");
    require(
        std::ranges::any_of(
            editor.document().graph.nodes,
            [](const auto& node) {
                return node.instanceId == "amp"
                    && node.soundDefinitionId
                        == std::optional<std::string>{"amplifier"};
            }),
        "inline undo should restore the Sound instance");
}

void testSoundExtractionAndInliningRejectPositionOverflowTransactionally()
{
    auto extractEditor = makeEditor();
    requireOk(
        extractEditor.addNode(
            {"left", "talas.constant", {}},
            {
                std::numeric_limits<std::int32_t>::min(),
                0,
            }),
        "overflow extraction fixture should add its left node");
    requireOk(
        extractEditor.addNode(
            {"right", "talas.constant", {}},
            {
                std::numeric_limits<std::int32_t>::max(),
                0,
            }),
        "overflow extraction fixture should add its right node");
    const auto beforeExtraction = extractEditor.document();
    const auto extractionHistoryWasAvailable = extractEditor.canUndo();
    requireError(
        extractEditor.extractNodesToSound(
            {"left", "right"},
            "wide",
            "Wide",
            "wide1"),
        talas::project::EditError::InvalidSelection,
        "Sound extraction whose normalized positions exceed int32");
    require(
        extractEditor.document().graph.nodes.size()
                == beforeExtraction.graph.nodes.size()
            && extractEditor.document().graph.nodes[0].instanceId == "left"
            && extractEditor.document().graph.nodes[1].instanceId == "right"
            && extractEditor.document().soundDefinitions.empty()
            && extractEditor.canUndo() == extractionHistoryWasAvailable,
        "failed overflow extraction should not mutate the document or history");

    auto inlineEditor = makeEditor();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "offset";
    definition.displayName = "Offset";
    definition.graph.nodes.push_back({
        "value",
        "talas.constant",
        {},
    });
    requireOk(
        inlineEditor.addSoundDefinition(std::move(definition)),
        "overflow inline fixture should add its Sound definition");
    requireOk(
        inlineEditor.moveNodeInSoundDefinition(
            "offset",
            "value",
            {std::numeric_limits<std::int32_t>::max(), 0}),
        "overflow inline fixture should place its internal node");
    requireOk(
        inlineEditor.addSoundInstance("offset1", "offset", {1, 0}),
        "overflow inline fixture should add its Sound instance");
    const auto historyWasAvailable = inlineEditor.canUndo();
    requireError(
        inlineEditor.inlineSoundInstance("offset1"),
        talas::project::EditError::InvalidSelection,
        "Sound inline whose translated positions exceed int32");
    require(
        inlineEditor.document().graph.nodes.size() == 1
            && inlineEditor.document().graph.nodes.front().soundDefinitionId
                == std::optional<std::string>{"offset"}
            && inlineEditor.canUndo() == historyWasAvailable,
        "failed overflow inline should not mutate the document or history");
}

// Inlining moves nodes out of the Sound's scope, so it must refuse Sounds
// whose meaning depends on that scope instead of silently rebinding it.
void testInlineRejectsSoundsThatDependOnTheirOwnScope()
{
    auto editor = makeEditor();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{"frequency", "220 hz"}, {"amplitude", "!Gain"}}});
    definition.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = {"gain", "1.0"};
    gain.bindings = {{talas::project::ProjectSoundParameterTarget::EventValue,
        {}, {}, "Gain", talas::expr::ValueDomain::Scalar}};
    definition.exposedParameters = {gain};
    requireOk(editor.addSoundDefinition(std::move(definition)),
        "scoped inline fixture should add its Sound definition");
    requireOk(editor.addSoundInstance("voice1", "voice", {}),
        "scoped inline fixture should add its Sound instance");
    requireOk(editor.setParameter("voice1", expressionParameter("gain", "0.25")),
        "scoped inline fixture should override its local Event Value");
    const auto before = editor.document();
    const auto historyWasAvailable = editor.canUndo();
    const auto eventValueInline = editor.inlineSoundInstance("voice1");
    requireError(eventValueInline, talas::project::EditError::InvalidSelection,
        "inlining a Sound with a local Event Value binding");
    require(eventValueInline.message.find("own scope") != std::string::npos,
        "local Event Value inline should be rejected for its scope: " + eventValueInline.message);
    require(editor.document().graph == before.graph && editor.canUndo() == historyWasAvailable,
        "rejected scoped inline should not mutate the document or history");

    auto unusedEditor = makeEditor();
    talas::project::ProjectSoundDefinition unused;
    unused.definitionId = "voice";
    unused.displayName = "Voice";
    unused.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{"frequency", "220 hz"}, {"amplitude", "!GainTrim"}}});
    unused.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    unused.exposedParameters = {gain};
    requireOk(unusedEditor.addSoundDefinition(std::move(unused)),
        "unused-binding inline fixture should add its Sound definition");
    requireOk(unusedEditor.addSoundInstance("voice1", "voice", {}),
        "unused-binding inline fixture should add its Sound instance");
    requireOk(unusedEditor.inlineSoundInstance("voice1"),
        "a local Event Value binding that no internal node reads should not block inlining");

    talas::project::ProjectDocument variables;
    variables.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    talas::project::ProjectEditor variableEditor{variables, builtinRegistry()};
    talas::project::ProjectSoundDefinition owner;
    owner.definitionId = "voice";
    owner.displayName = "Voice";
    owner.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{"frequency", "220 hz"}, {"amplitude", "@Gain"}}});
    owner.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    requireOk(variableEditor.addSoundDefinition(std::move(owner)),
        "owned-variable inline fixture should add its Sound definition");
    talas::project::ProjectVariableDeclaration projectGain;
    projectGain.variableId = "Gain";
    projectGain.displayName = "Project Gain";
    projectGain.initialValue = 0.9;
    auto voiceGain = projectGain;
    voiceGain.displayName = "Voice Gain";
    voiceGain.initialValue = 0.25;
    voiceGain.owner.kind = talas::project::ProjectVariableOwnerKind::SoundDefinition;
    voiceGain.owner.definitionId = "voice";
    requireOk(variableEditor.upsertProjectVariable(projectGain),
        "owned-variable inline fixture should add its project variable");
    requireOk(variableEditor.upsertProjectVariable(voiceGain),
        "owned-variable inline fixture should add its Sound variable");
    requireOk(variableEditor.addSoundInstance("voice1", "voice", {}),
        "owned-variable inline fixture should add its Sound instance");
    const auto variablesBefore = variableEditor.document();
    const auto variableInline = variableEditor.inlineSoundInstance("voice1");
    requireError(variableInline, talas::project::EditError::InvalidSelection,
        "inlining a Sound that owns a variable");
    require(variableInline.message.find("own scope") != std::string::npos,
        "owned-variable inline should be rejected for its scope: " + variableInline.message);
    require(variableEditor.document().graph == variablesBefore.graph,
        "rejected owned-variable inline should not rebind @Gain to the project variable");
}

void testSoundExtractionRejectsTerminalRootNodes()
{
    auto editor = makeEditor();
    requireOk(
        editor.addNode({"osc", "talas.oscillator", {}, {}}),
        "terminal extraction fixture should add its source");
    requireOk(
        editor.addNode({"out", "talas.audioOutput", {}, {}}),
        "terminal extraction fixture should add its output");
    requireOk(
        editor.connect({"osc", "audioOut", "out", "audioIn"}),
        "terminal extraction fixture should connect its output");
    const auto historyWasAvailable = editor.canUndo();

    requireError(
        editor.extractNodesToSound(
            {"out"},
            "terminal",
            "Terminal",
            "terminal1"),
        talas::project::EditError::InvalidSelection,
        "extracting a terminal root node");
    require(
        editor.document().soundDefinitions.empty()
            && std::ranges::any_of(
                editor.document().graph.nodes,
                [](const auto& node) {
                    return node.instanceId == "out"
                        && node.typeId == "talas.audioOutput";
                })
            && editor.canUndo() == historyWasAvailable
            && editor.validateForRender().ok(),
        "rejected terminal extraction should preserve the renderable root graph");
}

void testSoundDefinitionGraphsAreEditableAsOneScopedUndoModel()
{
    auto editor = makeEditor();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    requireOk(
        editor.addSoundDefinition(std::move(definition)),
        "scoped editor fixture should add its definition");

    requireOk(
        editor.addNodeToSoundDefinition(
            "voice",
            {"osc", "talas.oscillator", {}},
            {32, 64}),
        "definition editor should add a built-in node");
    requireOk(
        editor.addNodeToSoundDefinition(
            "voice",
            {"vca", "talas.vca", {}},
            {256, 64}),
        "definition editor should add a second built-in node");
    requireOk(
        editor.connectInSoundDefinition(
            "voice",
            {"osc", "audioOut", "vca", "audioIn"}),
        "definition editor should connect compatible ports");
    requireOk(
        editor.setParameterInSoundDefinition(
            "voice",
            "osc",
            expressionParameter("frequency", "330 hz")),
        "definition editor should update a node parameter");

    const auto& edited = editor.document().soundDefinitions.front();
    require(
        edited.graph.nodes.size() == 2
            && edited.graph.connections.size() == 1
            && edited.graph.nodes.front().parameters.front().expression
                == std::optional<std::string>{"330 hz"},
        "scoped edits should remain inside the selected definition graph");
    require(
        editor.document().graph.nodes.empty(),
        "scoped edits must not leak into the root graph");

    editor.beginHistoryTransaction();
    requireOk(
        editor.moveNodeInSoundDefinition("voice", "osc", {64, 96}),
        "definition drag should accept its first position");
    requireOk(
        editor.moveNodeInSoundDefinition("voice", "osc", {96, 128}),
        "definition drag should accept its final position");
    editor.endHistoryTransaction();
    requireOk(editor.undo(), "definition drag should undo as one transaction");
    require(
        nodePosition(editor.document(), "osc", "voice")
            == talas::project::CanvasPosition{32, 64},
        "one undo should restore the pre-drag definition position");
    requireOk(editor.redo(), "definition drag should redo as one transaction");
    require(
        nodePosition(editor.document(), "osc", "voice")
            == talas::project::CanvasPosition{96, 128},
        "redo should restore the final definition position");

    requireError(
        editor.connectInSoundDefinition(
            "voice",
            {"osc", "audioOut", "vca", "audioIn"}),
        talas::project::EditError::DuplicateInputConnection,
        "definition editor duplicate input connection");
    requireOk(
        editor.disconnectInSoundDefinition("voice", "vca", "audioIn"),
        "definition editor should disconnect a scoped input");
    requireOk(
        editor.removeNodeFromSoundDefinition("voice", "vca"),
        "definition editor should remove a scoped node");
    require(
        editor.document().soundDefinitions.front().graph.nodes.size() == 1
            && editor.document().soundDefinitions.front().graph.connections.empty(),
        "scoped removal should clear attached scoped connections");
}

void testEditorBuildsPatchIncrementallyAndValidatesForRender()
{
    auto editor = makeEditor();

    requireOk(editor.addNode({"osc1", "talas.oscillator", {expressionParameter("frequency", "220 hz")}}, {32, 64}),
              "editor should add an oscillator");
    requireOk(editor.addNode({"vca1", "talas.vca", {}}, {256, 64}), "editor should add an incomplete VCA");
    requireOk(editor.addNode({"out1", "talas.audioOutput", {}}, {480, 64}), "editor should add an incomplete output");

    require(editor.document().graph.nodes.size() == 3, "incremental graph should retain all nodes");
    require(nodePosition(editor.document(), "osc1").x == 32, "node x should be retained");
    require(!editor.validateForRender().ok(), "incomplete incremental graph should not be renderable yet");

    requireOk(editor.connect({"osc1", "audioOut", "vca1", "audioIn"}), "editor should connect compatible audio ports");
    requireOk(editor.connect({"vca1", "audioOut", "out1", "audioIn"}), "editor should complete the audio path");
    require(editor.validateForRender().ok(), "completed graph should validate for render");
}

void testMoveAndConnectionErrorsAreTransactional()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc1", "talas.oscillator", {}}), "editor should add oscillator");
    requireOk(editor.addNode({"osc2", "talas.oscillator", {}}), "editor should add second oscillator");
    requireOk(editor.addNode({"vca1", "talas.vca", {}}), "editor should add VCA");

    requireOk(editor.moveNode("osc1", {320, -48}), "editor should move a known node");
    require(nodePosition(editor.document(), "osc1").x == 320, "move should commit x position");
    requireError(editor.moveNode("missing", {1, 1}), talas::project::EditError::UnknownNodeId,
                 "moving an unknown node");
    require(nodePosition(editor.document(), "osc1").x == 320, "failed move should not mutate position");

    requireOk(editor.connect({"osc1", "audioOut", "vca1", "audioIn"}), "first destination connection should succeed");
    // Audio and control signals feed each other (ADR-0016).
    requireOk(editor.connect({"osc1", "audioOut", "osc2", "frequency"}),
              "an audio output should feed a control input");
    // A VCA takes many Inputs; an Oscillator's Frequency one.
    requireError(editor.connect({"vca1", "audioOut", "osc2", "frequency"}),
                 talas::project::EditError::DuplicateInputConnection,
                 "duplicate destination connection");
    require(editor.document().graph.connections.size() == 2, "failed duplicate connection should not mutate graph");

    // A spectrum does not feed a control input.
    requireOk(editor.addNode({"fft1", "talas.forwardFftV1", {}}), "editor should add a spectrum source");
    requireError(editor.connect({"fft1", "spectrumOut", "vca1", "gain"}),
                 talas::project::EditError::IncompatiblePortTypes,
                 "connecting a spectrum output to a control input");
    require(editor.document().graph.connections.size() == 2, "failed incompatible connection should not mutate graph");

    requireError(editor.connect({"osc1", "missing", "vca1", "audioIn"}),
                 talas::project::EditError::UnknownPort,
                 "connecting an unknown source port");
    require(editor.document().graph.connections.size() == 2, "failed unknown-port connection should not mutate graph");
}

// The Mixer adds every Sound in its Inputs (ADR-0017): its input takes
// many connections, each once, and disconnecting removes the latest.
void testMixerInputsTakeManyConnections()
{
    auto editor = makeEditor();
    for (const auto* id : {"osc1", "osc2", "osc3"})
        requireOk(editor.addNode({id, "talas.oscillator", {}}), "editor should add an oscillator");
    requireOk(editor.addNode({"mix", "talas.mixerV2", {}}), "editor should add the Mixer");
    requireOk(editor.addNode({"out1", "talas.audioOutput", {}}), "editor should add an output");

    for (const auto* id : {"osc1", "osc2", "osc3"})
        requireOk(editor.connect({id, "audioOut", "mix", "inputs"}), "Mixer Inputs should take another Sound");
    requireError(editor.connect({"osc2", "audioOut", "mix", "inputs"}),
                 talas::project::EditError::DuplicateInputConnection,
                 "connecting the same Sound to Inputs twice");
    requireOk(editor.connect({"mix", "leftOut", "out1", "audioIn"}), "Mixer should feed the output");
    require(editor.validateForRender().ok(), "a Mixer of three Sounds should validate for render");

    requireOk(editor.disconnect("mix", "inputs"), "Mixer Inputs should disconnect");
    const auto& connections = editor.document().graph.connections;
    require(std::ranges::count(connections, std::string{"inputs"}, &talas::core::Connection::toPort) == 2
            && std::ranges::find(connections, talas::core::Connection{"osc3", "audioOut", "mix", "inputs"})
                == connections.end(),
        "disconnecting Inputs should remove only its latest connection");

    // A collection field is the only kind that takes more
    // than one Sound; a single Sound's input refuses a second.
    requireOk(editor.addNode({"vca", "talas.forcedProcessorAssignment", {}}), "editor should add a one-input Sound");
    requireOk(editor.connect({"osc1", "audioOut", "vca", "audioIn"}), "a one-input Sound should take a Sound");
    requireError(editor.connect({"osc2", "audioOut", "vca", "audioIn"}), talas::project::EditError::DuplicateInputConnection,
        "a single Sound's input should refuse a second Sound");
}

void testRemovalDisconnectAndParameterValidation()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc1", "talas.oscillator", {}, {}}), "editor should add oscillator");
    requireOk(editor.addNode({"vca1", "talas.vca", {}, {}}), "editor should add VCA");
    requireOk(editor.addNode({"out1", "talas.audioOutput", {}, {}}), "editor should add output");
    requireOk(editor.connect({"osc1", "audioOut", "vca1", "audioIn"}), "editor should connect oscillator to VCA");
    requireOk(editor.connect({"vca1", "audioOut", "out1", "audioIn"}), "editor should connect VCA to output");

    requireError(editor.setParameter("osc1", expressionParameter("missing", "1")),
                 talas::project::EditError::UnknownParameter,
                 "setting unknown parameter");
    requireError(editor.setParameter("osc1", booleanParameter("frequency", true)),
                 talas::project::EditError::InvalidParameterValue,
                 "setting a parameter with the wrong value kind");
    requireError(editor.setParameter("osc1", choiceParameter("interpolation", "cubic")),
                 talas::project::EditError::InvalidParameterValue,
                 "setting a choice outside its descriptor values");
    requireOk(editor.setParameter("osc1", expressionParameter("frequency", "330 hz")),
              "setting known expression parameter");
    require(editor.document().graph.nodes[0].parameters.size() == 1, "known parameter should be added once");
    requireOk(editor.setParameter("osc1", expressionParameter("frequency", "440 hz")),
              "setting known parameter should replace its value");
    require(editor.document().graph.nodes[0].parameters.size() == 1, "parameter replacement should not duplicate parameter");

    const auto beforeInvalidExpression =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setParameter(
            "osc1", expressionParameter("frequency", "native(1)")),
        talas::project::EditError::InvalidParameterValue,
        "authoring a function outside the owning Pure v2 dialect");
    requireError(
        editor.setParameter(
            "osc1", expressionParameter("frequency", "[1][99]")),
        talas::project::EditError::InvalidParameterValue,
        "authoring a statically invalid Pure v2 list expression");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeInvalidExpression,
        "dialect and static-expression rejection should preserve exact project bytes");

    requireOk(
        editor.addNode({"envelope", "talas.graphicalEnvelope", {}, {}}),
        "runtime text-expression fixture should add Graphical Envelope");
    talas::project::ProjectParameterExpression invalidEnvelope;
    invalidEnvelope.parameterId = "envelope";
    invalidEnvelope.text = "{ 1 s } { native(1) }";
    const auto beforeInvalidText =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setParameter("envelope", std::move(invalidEnvelope)),
        talas::project::EditError::InvalidParameterValue,
        "authoring an invalid runtime text expression");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeInvalidText,
        "runtime text-expression rejection should preserve exact project bytes");
    requireOk(
        editor.removeNode("envelope"),
        "runtime text-expression fixture should be removed");

    requireOk(editor.disconnect("vca1", "audioIn"), "disconnect should remove a known destination connection");
    require(editor.document().graph.connections.size() == 1, "disconnect should remove only its target connection");
    requireOk(editor.removeNode("vca1"), "removing a node should succeed");
    require(editor.document().graph.nodes.size() == 2, "removing a node should remove it from the document");
    require(editor.document().graph.connections.empty(), "removing a node should remove incident connections");
}

void testEditorRejectsDuplicateIdsUnknownTypesAndCycles()
{
    auto editor = makeEditor();
    requireError(editor.addNode({"", "talas.oscillator", {}, {}}), talas::project::EditError::InvalidNodeId,
                 "adding empty node id");
    requireError(editor.addNode({"bad1", "talas.missing", {}, {}}), talas::project::EditError::UnknownNodeType,
                 "adding unknown node type");
    requireError(editor.addNode({"badParams", "talas.oscillator", {
                     expressionParameter("frequency", "220 hz"),
                     expressionParameter("frequency", "440 hz"),
                 }, {}}),
                 talas::project::EditError::DuplicateParameter,
                 "adding duplicate node parameters");
    requireOk(editor.addNode({"vca1", "talas.vca", {}, {}}), "editor should add VCA");
    requireError(editor.addNode({"vca1", "talas.vca", {}, {}}), talas::project::EditError::DuplicateNodeId,
                 "adding duplicate node id");
    requireError(editor.connect({"vca1", "audioOut", "vca1", "audioIn"}), talas::project::EditError::InvalidGraph,
                 "connecting a direct cycle");
    require(editor.document().graph.connections.empty(), "failed cycle should not mutate graph");
}

void testEditorUndoRedoAndDragTransactions()
{
    auto editor = makeEditor();
    requireError(editor.undo(), talas::project::EditError::NoUndoAvailable, "undo with no history");
    requireError(editor.redo(), talas::project::EditError::NoRedoAvailable, "redo with no history");

    requireOk(editor.addNode({"osc1", "talas.oscillator", {}}), "editor should add an undoable node");
    require(editor.canUndo(), "adding a node should create undo history");
    requireOk(editor.undo(), "undo should remove added node");
    require(editor.document().graph.nodes.empty(), "undo should restore empty document");
    require(editor.canRedo(), "undo should create redo history");
    requireOk(editor.redo(), "redo should restore added node");
    require(editor.document().graph.nodes.size() == 1, "redo should restore node");

    editor.beginHistoryTransaction();
    requireOk(editor.moveNode("osc1", {80, 16}), "first drag update should succeed");
    requireOk(editor.moveNode("osc1", {240, 96}), "second drag update should succeed");
    editor.endHistoryTransaction();
    require(nodePosition(editor.document(), "osc1").x == 240, "drag should commit final x position");

    requireOk(editor.undo(), "undo should revert the whole drag transaction");
    require(nodePosition(editor.document(), "osc1").x == 0, "single undo should restore pre-drag position");
    requireOk(editor.redo(), "redo should restore the whole drag transaction");
    require(nodePosition(editor.document(), "osc1").x == 240, "single redo should restore final drag position");

    requireOk(editor.undo(), "undo should restore pre-drag state before a new edit");
    requireOk(editor.moveNode("osc1", {300, 100}), "new edit after undo should succeed");
    requireError(editor.redo(), talas::project::EditError::NoRedoAvailable, "new edit should invalidate redo history");
}

void testGraphSelectionEditsAreDeterministicAtomicAndScoped()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc", "talas.oscillator", {}}, {32, 64}),
        "selection fixture should add oscillator");
    requireOk(editor.addNode({"vca", "talas.vca", {}}, {256, 64}),
        "selection fixture should add VCA");
    requireOk(editor.connect({"osc", "audioOut", "vca", "audioIn"}),
        "selection fixture should connect selected nodes");
    const auto baseline = talas::project::serializeProjectDocument(
        editor.document());

    std::vector<std::string> duplicated;
    const std::array selection{std::string{"vca"}, std::string{"osc"}};
    requireOk(editor.duplicateNodes(selection, duplicated),
        "root selected subgraph should duplicate atomically");
    require(duplicated == std::vector<std::string>({"osc-copy", "vca-copy"}),
        "duplicate ids should follow document order rather than selection order");
    require(nodePosition(editor.document(), "osc-copy")
                == talas::project::CanvasPosition{64, 96}
            && nodePosition(editor.document(), "vca-copy")
                == talas::project::CanvasPosition{288, 96}
            && std::ranges::any_of(
                editor.document().graph.connections,
                [](const auto& connection) {
                    return connection.fromNode == "osc-copy"
                        && connection.toNode == "vca-copy";
                }),
        "duplicate should preserve translated presentation and internal topology");
    std::vector<std::string> secondDuplicate;
    requireOk(editor.duplicateNodes(selection, secondDuplicate),
        "second root duplicate should choose collision-free ids");
    require(secondDuplicate
            == std::vector<std::string>({"osc-copy-2", "vca-copy-2"}),
        "duplicate collision suffixes should be deterministic");
    requireOk(editor.undo(), "one undo should remove only the second duplicate");
    requireOk(editor.undo(), "one more undo should remove the first duplicate");
    require(talas::project::serializeProjectDocument(editor.document()) == baseline,
        "duplicate undo should restore exact root graph bytes");

    editor.beginHistoryTransaction();
    requireOk(editor.moveNodes(selection, {32, 0}),
        "first held-key selection nudge should succeed");
    requireOk(editor.moveNodes(selection, {0, 1}),
        "second held-key selection nudge should succeed");
    editor.endHistoryTransaction();
    require(nodePosition(editor.document(), "osc")
                == talas::project::CanvasPosition{64, 65}
            && nodePosition(editor.document(), "vca")
                == talas::project::CanvasPosition{288, 65},
        "selection nudge should preserve relative geometry");
    requireOk(editor.undo(),
        "one undo should restore the complete held-key nudge gesture");
    require(talas::project::serializeProjectDocument(editor.document()) == baseline,
        "held-key undo should restore exact root presentation bytes");
    requireOk(editor.removeNodes(selection),
        "root multi-delete should remove the complete selected set");
    require(editor.document().graph.nodes.empty()
            && editor.document().graph.connections.empty(),
        "root multi-delete should remove selected nodes and incident topology");
    requireOk(editor.undo(), "one undo should restore root multi-delete");
    require(talas::project::serializeProjectDocument(editor.document()) == baseline,
        "root multi-delete undo should restore exact topology and presentation");

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    requireOk(editor.addSoundDefinition(std::move(definition)),
        "nested selection fixture should add a Sound definition");
    requireOk(editor.addNodeToSoundDefinition(
            "voice", {"nestedOsc", "talas.oscillator", {}}, {10, 20}),
        "nested selection fixture should add oscillator");
    requireOk(editor.addNodeToSoundDefinition(
            "voice", {"nestedVca", "talas.vca", {}}, {90, 20}),
        "nested selection fixture should add VCA");
    requireOk(editor.connectInSoundDefinition(
            "voice", {"nestedOsc", "audioOut", "nestedVca", "audioIn"}),
        "nested selection fixture should connect nodes");
    const auto nestedBaseline = talas::project::serializeProjectDocument(
        editor.document());
    const std::array nestedSelection{
        std::string{"nestedOsc"}, std::string{"nestedVca"}};
    requireOk(editor.duplicateNodesInSoundDefinition(
            "voice", nestedSelection, duplicated),
        "nested selected subgraph should duplicate in its exact Sound scope");
    require(duplicated
            == std::vector<std::string>({"nestedOsc-copy", "nestedVca-copy"}),
        "nested duplicate should retain deterministic ids");
    requireOk(editor.undo(), "one undo should remove nested duplicate only");
    require(talas::project::serializeProjectDocument(editor.document())
            == nestedBaseline,
        "nested duplicate undo should restore exact Sound bytes");
    requireOk(editor.removeNodesFromSoundDefinition("voice", nestedSelection),
        "nested multi-delete should remove the complete selected set");
    requireOk(editor.undo(), "one undo should restore nested multi-delete");
    require(talas::project::serializeProjectDocument(editor.document())
            == nestedBaseline,
        "nested multi-delete undo should restore topology and presentation");

    auto rejected = makeEditor();
    requireOk(rejected.addNode(
            {"edge", "talas.noise", {}},
            {std::numeric_limits<std::int32_t>::max(), 0}),
        "rejected-selection fixture should add its boundary node");
    const auto rejectedBaseline = talas::project::serializeProjectDocument(
        rejected.document());
    const std::array unknownSelection{std::string{"missing"}};
    requireError(
        rejected.duplicateNodes(unknownSelection, duplicated),
        talas::project::EditError::UnknownNodeId,
        "unknown duplicate selection");
    const std::array duplicateSelection{
        std::string{"edge"}, std::string{"edge"}};
    requireError(
        rejected.duplicateNodes(duplicateSelection, duplicated),
        talas::project::EditError::InvalidSelection,
        "duplicate node identity in selection");
    const std::array edgeSelection{std::string{"edge"}};
    requireError(
        rejected.duplicateNodes(edgeSelection, duplicated),
        talas::project::EditError::InvalidSelection,
        "duplicate position overflow");
    require(duplicated.empty(),
        "failed single-node duplicate should not expose output identities");
    require(
        talas::project::serializeProjectDocument(rejected.document())
            == rejectedBaseline,
        "failed selection edits should preserve exact bytes");
    requireOk(rejected.undo(),
        "failed selection edits should not shadow the preceding history entry");
    require(rejected.document().graph.nodes.empty(),
        "one undo after rejected edits should remove the original fixture node");

    auto partial = makeEditor();
    requireOk(partial.addNode(
            {"safe", "talas.noise", {}}, {0, 0}),
        "partial-output fixture should add a safe first node");
    requireOk(partial.addNode(
            {"edge", "talas.noise", {}},
            {std::numeric_limits<std::int32_t>::max(), 0}),
        "partial-output fixture should add an overflowing later node");
    const std::array partialSelection{
        std::string{"safe"}, std::string{"edge"}};
    requireError(
        partial.duplicateNodes(partialSelection, duplicated),
        talas::project::EditError::InvalidSelection,
        "later duplicate position overflow");
    require(duplicated.empty(),
        "failed multi-duplicate should not expose partial output identities");

    auto cancelled = makeEditor();
    requireOk(cancelled.addNode(
            {"movable", "talas.noise", {}}, {10, 20}),
        "cancelled gesture fixture should add a node");
    const auto cancelledBaseline = talas::project::serializeProjectDocument(
        cancelled.document());
    cancelled.beginHistoryTransaction();
    requireOk(cancelled.moveNodes(
            std::array{std::string{"movable"}}, {32, 1}),
        "cancelled gesture fixture should mutate inside its transaction");
    cancelled.cancelHistoryTransaction();
    require(
        talas::project::serializeProjectDocument(cancelled.document())
            == cancelledBaseline,
        "cancelled history transaction should restore exact pre-gesture bytes");
    requireOk(cancelled.undo(),
        "cancelled transaction should not shadow the preceding history entry");
    require(cancelled.document().graph.nodes.empty(),
        "undo after cancellation should remove the original fixture node");

    auto owned = makeEditor();
    requireOk(owned.addNode(
            {"movable", "talas.noise", {}}, {10, 20}),
        "owned gesture fixture should add a node");
    const auto token = owned.beginOwnedHistoryTransaction();
    requireOk(owned.moveNode("movable", {42, 21}, token),
        "owned gesture should accept its matching transaction token");
    requireOk(owned.addNode(
            {"unrelated", "talas.noise", {}}, {80, 20}),
        "unrelated edit should commit outside the owned gesture");
    requireError(
        owned.moveNode("movable", {90, 90}, token),
        talas::project::EditError::InvalidSelection,
        "stale owned gesture update after unrelated commit");
    owned.cancelOwnedHistoryTransaction(token);
    require(nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{42, 21}
            && std::ranges::any_of(
                owned.document().graph.nodes,
                [](const auto& node) {
                    return node.instanceId == "unrelated";
                }),
        "stale owned cancellation must not revert a later unrelated edit");
    requireOk(owned.undo(),
        "first undo should remove only the unrelated edit");
    require(owned.document().graph.nodes.size() == 1
            && nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{42, 21},
        "unrelated edit should own an independent history entry");
    requireOk(owned.undo(),
        "second undo should restore the owned drag position");
    require(nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{10, 20},
        "owned drag should remain one isolated history entry");

    const auto applyToken = owned.beginOwnedHistoryTransaction();
    requireOk(owned.moveNode("movable", {20, 30}, applyToken),
        "owned apply-document fixture should move its node");
    auto wholeDocument = owned.document();
    wholeDocument.metadata.title = "Unrelated prepared edit";
    requireOk(owned.applyDocumentEdit(std::move(wholeDocument)),
        "prepared document edit should isolate an active owned gesture");
    owned.cancelOwnedHistoryTransaction(applyToken);
    require(owned.document().metadata.title == "Unrelated prepared edit"
            && nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{20, 30},
        "stale owned cancellation must not revert a prepared document edit");
    requireOk(owned.undo(),
        "first prepared-edit undo should preserve the completed owned drag");
    require(owned.document().metadata.title.empty()
            && nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{20, 30},
        "prepared document edit should own an independent history entry");
    requireOk(owned.undo(),
        "second prepared-edit undo should restore the owned drag");

    auto realtime = makeEditor();
    requireOk(realtime.addNode(
            {"movable", "talas.noise", {}}, {10, 20}),
        "realtime isolation fixture should add a movable node");
    talas::project::VirtualControl live;
    live.eventName = "Live";
    live.value = 0.0;
    requireOk(realtime.addVirtualControl(live),
        "realtime isolation fixture should add one Event Value");
    talas::project::VirtualControl sibling;
    sibling.eventName = "Sibling";
    sibling.value = 0.0;
    requireOk(realtime.addVirtualControl(sibling),
        "realtime isolation fixture should add a sibling Event Value");
    const auto realtimeToken = realtime.beginOwnedHistoryTransaction();
    requireOk(realtime.moveNode("movable", {30, 40}, realtimeToken),
        "realtime isolation fixture should move inside its owned gesture");
    requireOk(realtime.setVirtualControlValue("Live", 0.5),
        "live Event Value edit should isolate the active owned gesture");
    realtime.cancelOwnedHistoryTransaction(realtimeToken);
    require(nodePosition(realtime.document(), "movable")
                == talas::project::CanvasPosition{30, 40}
            && realtime.document().virtualControls.front().value == 0.5,
        "live Event Value publication must not be reverted with a stale drag token");
    requireOk(realtime.undo(),
        "first realtime isolation undo should remove only the Event Value edit");
    require(nodePosition(realtime.document(), "movable")
                == talas::project::CanvasPosition{30, 40}
            && realtime.document().virtualControls.front().value == 0.0,
        "Event Value edit should own an independent history entry");

    talas::project::PreparedVirtualControlValueEdit prepared;
    requireOk(realtime.prepareVirtualControlValue("Live", 0.75, prepared),
        "precomputed Event Value edit should prepare before a drag starts");
    requireOk(realtime.setVirtualControlValue("Sibling", 0.25),
        "sibling Event Value should change after the first edit was prepared");
    const auto preparedToken = realtime.beginOwnedHistoryTransaction();
    requireOk(realtime.moveNode("movable", {60, 70}, preparedToken),
        "precomputed Event Value fixture should move inside its owned gesture");
    realtime.commitVirtualControlValue(std::move(prepared));
    require(nodePosition(realtime.document(), "movable")
                == talas::project::CanvasPosition{60, 70}
            && std::ranges::find(
                    realtime.document().virtualControls,
                    "Live",
                    &talas::project::VirtualControl::eventName)->value == 0.75
            && std::ranges::find(
                    realtime.document().virtualControls,
                    "Sibling",
                    &talas::project::VirtualControl::eventName)->value == 0.25,
        "precomputed Event Value publication must preserve the in-flight drag state");
    realtime.cancelOwnedHistoryTransaction(preparedToken);
    require(nodePosition(realtime.document(), "movable")
                == talas::project::CanvasPosition{30, 40}
            && std::ranges::find(
                    realtime.document().virtualControls,
                    "Live",
                    &talas::project::VirtualControl::eventName)->value == 0.75
            && std::ranges::find(
                    realtime.document().virtualControls,
                    "Sibling",
                    &talas::project::VirtualControl::eventName)->value == 0.25,
        "precomputed Event Value publication should preserve sibling state and survive drag cancellation");
}

bool hasConnection(
    const talas::project::ProjectGraph& graph,
    const talas::core::Connection& expected)
{
    return std::ranges::find(graph.connections, expected)
        != graph.connections.end();
}

// A GraphicalEnvelope's Envelope marks its
// loop with loopStart and loopEnd after breakpoints' values; the editor's
// field takes them (the graphical envelope editor writes them).
void testGraphicalEnvelopeLoopMarksAreEditable()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"env", "talas.graphicalEnvelope", {}}), "add a Graphical Envelope");
    talas::project::ProjectParameterExpression envelope;
    envelope.parameterId = "envelope";
    envelope.text = "{0 s} {0} {0.1 s} {1} loopStart {0.5 s} {0.6} loopEnd {1 s} {0}";
    requireOk(editor.setParameter("env", envelope), "an Envelope with loop marks should be accepted");
    envelope.text = "{0 s} {0} loopy {1 s} {1}";
    requireError(editor.setParameter("env", envelope), talas::project::EditError::InvalidParameterValue,
        "other words between an Envelope's breakpoints");
}

// An effect dropped on a Timeline clip plays the
// clip's Sound through it, minus removes it, and a Sound dropped with
// Option replaces the affected Sound.
void testTimelineClipEffects()
{
    talas::project::ProjectDocument document;
    for (const auto& [id, name, type] : {std::tuple{"pad", "Pad", "talas.oscillator"}, std::tuple{"bass", "Bass", "talas.noise"}})
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = id;
        definition.displayName = name;
        definition.graph.nodes.push_back({"tone", type, {}});
        definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(definition));
    }
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "track1";
    track.displayName = "Track 1";
    track.outputTrackId = "master";
    document.timeline.score.tracks = {master, track};
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    document.timeline.score.clips.push_back({"clip1", "track1", "pad", zero,
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0}, zero, zero, zero, 1});
    talas::project::ProjectEditor editor(document, builtinRegistry());
    const auto clipSound = [&] { return editor.document().timeline.score.clips.front().soundDefinitionId; };
    const auto definition = [&](const std::string& id) {
        return *std::ranges::find(editor.document().soundDefinitions, id, &talas::project::ProjectSoundDefinition::definitionId);
    };
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.applyEffectToScoreClip("clip1", "talas.filter"), "an effect dropped on a clip");
    const auto wrapper = definition(clipSound());
    require(clipSound() != "pad" && wrapper.displayName.ends_with("(Pad)")
            && wrapper.graph.nodes.size() == 2 && wrapper.graph.nodes[0].soundDefinitionId == std::optional<std::string>{"pad"}
            && wrapper.graph.nodes[1].typeId == "talas.filter" && wrapper.graph.connections.size() == 1
            && wrapper.graph.connections[0].toNode == "effect",
        "the clip should play its Sound through the effect, named Effect(Sound)");
    requireOk(editor.undo(), "undo the effect");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "one undo should take the effect back");
    requireOk(editor.redo(), "redo the effect");
    requireOk(editor.replaceScoreClipInput("clip1", "bass"), "a Sound dropped with Option on the clip");
    const auto replaced = definition(clipSound());
    require(replaced.displayName.ends_with("(Bass)")
            && replaced.graph.nodes[0].soundDefinitionId == std::optional<std::string>{"bass"}
            && definition(wrapper.definitionId).graph.nodes[0].soundDefinitionId == std::optional<std::string>{"pad"},
        "the clip's input should be replaced in a copy, the first wrapper kept");
    requireOk(editor.removeLastScoreClipEffect("clip1"), "minus on the clip");
    require(clipSound() == "bass", "minus should leave the clip playing the affected Sound");
    requireError(editor.removeLastScoreClipEffect("clip1"), talas::project::EditError::InvalidScore,
        "minus on a clip without an effect");
    requireError(editor.replaceScoreClipInput("clip1", "pad"), talas::project::EditError::InvalidScore,
        "replacing the input of a Sound without one");
    requireError(editor.applyEffectToScoreClip("clip1", "talas.constant"), talas::project::EditError::IncompatiblePortTypes,
        "an effect without an input");
    requireError(editor.applyEffectToScoreClip("missing", "talas.filter"), talas::project::EditError::InvalidScore,
        "an unknown clip");

    // The effect may be a Sound of the project, with an
    // Input for the clip's Sound.
    talas::project::ProjectSoundDefinition echo;
    echo.definitionId = "echo";
    echo.displayName = "Echo";
    echo.graph.nodes.push_back({"filter", "talas.filter", {}});
    echo.inputs.push_back({"audioIn", talas::core::PortType::Audio, "filter", "audioIn"});
    echo.outputs.push_back({"audioOut", talas::core::PortType::Audio, "filter", "audioOut"});
    requireOk(editor.addSoundDefinition(echo), "add a Sound with an Input");
    requireOk(editor.applySoundEffectToScoreClip("clip1", "echo"), "a Sound dropped on a clip as its effect");
    const auto echoed = definition(clipSound());
    require(echoed.displayName == "Echo(Bass)" && echoed.graph.nodes.size() == 2
            && echoed.graph.nodes[0].soundDefinitionId == std::optional<std::string>{"bass"}
            && echoed.graph.nodes[1].soundDefinitionId == std::optional<std::string>{"echo"}
            && echoed.graph.connections.size() == 1 && echoed.graph.connections[0].toPort == "audioIn",
        "a Sound from the Sound Browser or a Sound file window should be an effect on the clip too");
    requireError(editor.applySoundEffectToScoreClip("clip1", "pad"), talas::project::EditError::IncompatiblePortTypes,
        "a Sound without an Input is no effect");
}

// A Sound is renamed (select it, press Enter, type
// the new name); its lines, layout and the definition's ports and bindings
// follow. Clean up moves every Sound as one edit.
void testRenamedSoundsKeepTheirLinesAndCleanUpIsOneEdit()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise", "talas.noise", {}}, {10, 20}), "add noise");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"noise", "audioOut", "out", "audioIn"}), "connect noise");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.renameNode(std::nullopt, "noise", "tense anticipation"), "rename noise");
    require(hasConnection(editor.document().graph, {"tense anticipation", "audioOut", "out", "audioIn"})
            && editor.document().graph.connections.size() == 1
            && nodePosition(editor.document(), "tense anticipation") == talas::project::CanvasPosition{10, 20},
        "a renamed Sound should keep its line and its place");
    requireOk(editor.undo(), "one undo should take the rename back");
    require(talas::project::serializeProjectDocument(editor.document()) == before,
        "undoing the rename should restore the old name");
    requireError(editor.renameNode(std::nullopt, "noise", "out"), talas::project::EditError::DuplicateNodeId,
        "a name another Sound has");
    requireError(editor.renameNode(std::nullopt, "noise", ""), talas::project::EditError::InvalidNodeId, "an empty name");
    requireError(editor.renameNode(std::nullopt, "noise", "a/b"), talas::project::EditError::InvalidNodeId,
        "a name with a slash");
    requireError(editor.renameNode(std::nullopt, "noise", " padded"), talas::project::EditError::InvalidNodeId,
        "a name starting with a space");
    requireError(editor.renameNode(std::nullopt, "missing", "other"), talas::project::EditError::UnknownNodeId,
        "an unknown Sound");
    requireOk(editor.renameNode(std::nullopt, "noise", "noise"), "renaming to the same name changes nothing");
    require(talas::project::serializeProjectDocument(editor.document()) == before,
        "renaming to the same name should leave the project as it was");

    auto definition = makeVoiceDefinition();
    definition.exposedParameters.push_back(exposedFrequency());
    requireOk(editor.addSoundDefinition(std::move(definition)), "add the voice definition");
    requireOk(editor.renameNode(std::string_view{"voice"}, "osc", "tone"), "rename inside a definition");
    const auto& voice = *std::ranges::find(
        editor.document().soundDefinitions, "voice", &talas::project::ProjectSoundDefinition::definitionId);
    require(voice.graph.nodes.front().instanceId == "tone" && voice.inputs.front().nodeId == "tone"
            && voice.outputs.front().nodeId == "tone"
            && voice.exposedParameters.front().bindings.front().nodeId == "tone",
        "a renamed Sound inside a definition should keep its ports and parameter bindings");

    const std::vector<talas::project::ProjectNodePresentation> arranged{{"noise", {0, 300}}, {"out", {230, 300}}};
    const auto beforeArrange = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.arrangeNodes(std::nullopt, arranged), "arrange the Sounds");
    require(nodePosition(editor.document(), "noise") == talas::project::CanvasPosition{0, 300}
            && nodePosition(editor.document(), "out") == talas::project::CanvasPosition{230, 300},
        "Clean up should move each Sound to its place");
    requireOk(editor.undo(), "one undo should take the arrangement back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeArrange,
        "undoing Clean up should restore every Sound's place");
    const std::vector<talas::project::ProjectNodePresentation> unknown{{"noise", {5, 5}}, {"ghost", {0, 0}}};
    requireError(editor.arrangeNodes(std::nullopt, unknown), talas::project::EditError::UnknownNodeId,
        "an arrangement naming an unknown Sound");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeArrange,
        "a refused arrangement should move nothing");
}

// Deleting a Sound from the middle of a signal path
// gives its place to its input, which then feeds what it fed, in one undo
// step; the first Sound of a chain cannot be deleted while another reads
// it, and a Sound nothing reads goes on its own. Of several inputs, the one
// named takes its place.
// Revert drops the changes to a Sound's
// parameters since it was opened, leaving the other Sounds' changes, in one
// undo step.
const talas::project::ProjectNode* findNode(const talas::project::ProjectDocument& document, std::string_view nodeId)
{
    const auto found = std::ranges::find(document.graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
    return found == document.graph.nodes.end() ? nullptr : &*found;
}

void testRevertDropsTheChangesSinceTheSoundWasOpened()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc", "talas.oscillator", {}}, {0, 0}), "add the oscillator");
    requireOk(editor.addNode({"gain", "talas.vca", {}}, {200, 0}), "add the gain");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {400, 0}), "add output");
    requireOk(editor.connect({"osc", "audioOut", "gain", "audioIn"}), "connect the oscillator");
    requireOk(editor.connect({"gain", "audioOut", "out", "audioIn"}), "connect the gain");
    const auto opened = findNode(editor.document(), "osc")->parameters;
    requireOk(editor.setParameter("osc", {"amplitude", "0.3"}), "change the amplitude");
    requireOk(editor.setParameter("osc", {"frequency", "220 hz"}), "change the frequency");
    requireOk(editor.setParameter("gain", {"gain", "0.5"}), "change the other Sound");
    const auto edited = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.revertNodeParameters(std::nullopt, "osc", opened), "revert the oscillator");
    require(findNode(editor.document(), "osc")->parameters == opened
            && findNode(editor.document(), "gain")->parameters != std::vector<talas::project::ProjectParameterExpression>{},
        "Revert should drop the oscillator's changes and keep the other Sound's");
    requireOk(editor.undo(), "one undo should bring the changes back");
    require(talas::project::serializeProjectDocument(editor.document()) == edited, "undoing Revert should restore the edits");
    requireError(editor.revertNodeParameters(std::nullopt, "missing", {}), talas::project::EditError::UnknownNodeId,
        "an unknown Sound");
}

// A Mixer's Inputs field lists the
// Sounds that feed it in the diagram, in order; removing one from the field
// removes its line, and moving one changes the order.
void testInputsFieldListsRemovesAndReorders()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"disk", "talas.noise", {}}, {0, 0}), "add the first Sound");
    requireOk(editor.addNode({"sample", "talas.noise", {}}, {0, 100}), "add the second Sound");
    requireOk(editor.addNode({"extra", "talas.noise", {}}, {0, 200}), "add a Sound we do not want");
    requireOk(editor.addNode({"mix", "talas.mixerV2", {}}, {200, 100}), "add the mixer");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {400, 100}), "add output");
    for (const auto* source : {"disk", "sample", "extra"})
        requireOk(editor.connect({source, "audioOut", "mix", "inputs"}), "connect an input");
    requireOk(editor.connect({"mix", "leftOut", "out", "audioIn"}), "connect the mixer");
    require(editor.inputsOf(std::nullopt, "mix", "inputs") == std::vector<std::string>{"disk", "sample", "extra"},
        "the Inputs field should list the Sounds feeding the Mixer in the diagram");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.setInputs(std::nullopt, "mix", "inputs", {"sample", "disk"}),
        "remove the unwanted input and reorder the rest");
    require(editor.inputsOf(std::nullopt, "mix", "inputs") == std::vector<std::string>{"sample", "disk"}
            && !hasConnection(editor.document().graph, {"extra", "audioOut", "mix", "inputs"})
            && std::ranges::any_of(editor.document().graph.nodes, [](const auto& node) { return node.instanceId == "extra"; }),
        "the field's order should be the inputs' order, a removed input's line gone and the Sound left");
    requireOk(editor.undo(), "one undo should restore the field");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "undoing should restore the inputs");
    requireError(editor.setInputs(std::nullopt, "mix", "inputs", {"ghost"}), talas::project::EditError::UnknownNodeId,
        "a Sound not in the structure");
    // A Sample dropped into the Inputs field becomes one of the
    // Mixer's inputs, the last, in one undo step.
    const auto beforeDrop = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.addInputSound(std::nullopt, "mix", "inputs", {"sample1", "talas.sample", {}}),
        "drop a Sample into the Mixer's Inputs field");
    require(editor.inputsOf(std::nullopt, "mix", "inputs") == std::vector<std::string>{"disk", "sample", "extra", "sample1"},
        "the dropped Sample should be one of the Mixer's inputs, the last");
    requireOk(editor.undo(), "one undo should take the drop back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeDrop, "undoing should take the dropped Sound away");
    requireError(editor.addInputSound(std::nullopt, "mix", "inputs", {"disk", "talas.noise", {}}),
        talas::project::EditError::DuplicateNodeId, "a dropped Sound needs a name of its own");
    // Or the dropped Sound replaces one of the Sounds already
    // there, taking its place; the one replaced, feeding nothing else, goes.
    requireOk(editor.addInputSound(std::nullopt, "mix", "inputs", {"player", "talas.noise", {}}, 1),
        "drop a Sound into the Inputs field to replace the second");
    require(editor.inputsOf(std::nullopt, "mix", "inputs") == std::vector<std::string>{"disk", "player", "extra"}
            && std::ranges::none_of(editor.document().graph.nodes, [](const auto& node) { return node.instanceId == "sample"; }),
        "the dropped Sound should take the replaced one's place, and the replaced Sound go");
    requireOk(editor.undo(), "one undo should take the replacement back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeDrop, "undoing should bring the replaced Sound back");
    requireError(editor.addInputSound(std::nullopt, "mix", "inputs", {"player", "talas.noise", {}}, 3),
        talas::project::EditError::InvalidSelection, "there is no fourth Sound to replace");
}

// Paste special puts the Sound in the
// clipboard itself, not a copy, in the selected Sound's place, so two
// Sounds processed in different ways share one input.
void testPasteSpecialSharesTheSoundItself()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise1", "talas.noise", {}}, {0, 0}), "add the first noise");
    requireOk(editor.addNode({"noise2", "talas.noise", {}}, {0, 200}), "add the second noise");
    requireOk(editor.addNode({"low", "talas.filter", {}}, {200, 0}), "add a filter");
    requireOk(editor.addNode({"high", "talas.filter", {}}, {200, 200}), "add another filter");
    requireOk(editor.addNode({"mix", "talas.mixerV2", {}}, {400, 100}), "add the mixer");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 100}), "add output");
    for (const auto& [from, port, to, input] : {std::tuple{"noise1", "audioOut", "low", "audioIn"},
             std::tuple{"noise2", "audioOut", "high", "audioIn"}, std::tuple{"low", "audioOut", "mix", "inputs"},
             std::tuple{"high", "audioOut", "mix", "inputs"}, std::tuple{"mix", "leftOut", "out", "audioIn"}})
        requireOk(editor.connect({from, port, to, input}), "connect the Sounds");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.shareNodeInPlaceOf(std::nullopt, "noise2", "noise1"), "paste special the first noise");
    const auto& graph = editor.document().graph;
    require(hasConnection(graph, {"noise1", "audioOut", "low", "audioIn"}) && hasConnection(graph, {"noise1", "audioOut", "high", "audioIn"})
            && std::ranges::none_of(graph.nodes, [](const auto& node) { return node.instanceId == "noise2"; }),
        "both filters should share the one noise");
    requireOk(editor.undo(), "one undo should bring the second noise back");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "undoing Paste special should restore the graph");
    requireError(editor.shareNodeInPlaceOf(std::nullopt, "noise1", "noise1"), talas::project::EditError::InvalidSelection,
        "a Sound in its own place");
}

// Copy an input Sound, then Paste special it into
// each input field that needs it: the fields share the one Sound, a field of
// many Sounds taking it as its last.
void testPasteSpecialIntoInputFieldsSharesTheSound()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise1", "talas.noise", {}}, {0, 0}), "add the first noise");
    requireOk(editor.addNode({"noise2", "talas.noise", {}}, {0, 200}), "add the second noise");
    requireOk(editor.addNode({"low", "talas.filter", {}}, {200, 0}), "add a filter");
    requireOk(editor.addNode({"high", "talas.filter", {}}, {200, 200}), "add another filter");
    requireOk(editor.addNode({"mix", "talas.mixerV2", {}}, {400, 100}), "add the mixer");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 100}), "add output");
    for (const auto& [from, port, to, input] : {std::tuple{"noise1", "audioOut", "low", "audioIn"},
             std::tuple{"noise2", "audioOut", "high", "audioIn"}, std::tuple{"low", "audioOut", "mix", "inputs"},
             std::tuple{"high", "audioOut", "mix", "inputs"}, std::tuple{"mix", "leftOut", "out", "audioIn"}})
        requireOk(editor.connect({from, port, to, input}), "connect the Sounds");
    requireOk(editor.shareNodeInInput(std::nullopt, "high", "audioIn", "noise1"), "paste special the noise into a field");
    require(editor.inputsOf(std::nullopt, "high", "audioIn") == std::vector<std::string>{"noise1"}
            && editor.inputsOf(std::nullopt, "low", "audioIn") == std::vector<std::string>{"noise1"}
            && std::ranges::none_of(editor.document().graph.nodes, [](const auto& node) { return node.instanceId == "noise2"; }),
        "both filters should share the one noise, the one it replaced gone");
    const auto shared = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.shareNodeInInput(std::nullopt, "mix", "inputs", "noise1"), "paste special it into the Mixer's Inputs too");
    require(editor.inputsOf(std::nullopt, "mix", "inputs") == std::vector<std::string>{"low", "high", "noise1"},
        "a field of many Sounds should take the shared Sound as its last");
    requireOk(editor.undo(), "one undo should take it out of the Inputs again");
    require(talas::project::serializeProjectDocument(editor.document()) == shared, "undoing should restore the Inputs");
    requireError(editor.shareNodeInInput(std::nullopt, "high", "audioIn", "noise1"), talas::project::EditError::InvalidSelection,
        "the field holds the Sound already");
    requireError(editor.shareNodeInInput(std::nullopt, "high", "nowhere", "noise1"), talas::project::EditError::UnknownPort,
        "a field the Sound does not have");
    // A field's Sound used elsewhere too stays for its other uses.
    requireOk(editor.shareNodeInInput(std::nullopt, "low", "audioIn", "high"), "paste special the other filter into the first");
    require(editor.inputsOf(std::nullopt, "low", "audioIn") == std::vector<std::string>{"high"}
            && editor.inputsOf(std::nullopt, "high", "audioIn") == std::vector<std::string>{"noise1"},
        "the Sound the field held should stay where else it is used");
    // A Sound dragged from the signal flow into a field,
    // Control or Option held, is shared there: the field takes the Sound
    // itself, as the Workbench's drop of a signal flow Sound does.
    auto inputs = editor.inputsOf(std::nullopt, "mix", "inputs");
    inputs.emplace_back("noise1");
    requireOk(editor.setInputs(std::nullopt, "mix", "inputs", inputs), "drag the noise into the Mixer's Inputs");
    require(hasConnection(editor.document().graph, {"noise1", "audioOut", "mix", "inputs"})
            && hasConnection(editor.document().graph, {"noise1", "audioOut", "high", "audioIn"})
            && std::ranges::count_if(editor.document().graph.nodes, [](const auto& node) { return node.typeId == "talas.noise"; }) == 1,
        "the dragged Sound should feed both fields, one Sound shared");
}

// Moving a Sound within its signal flow diagram is
// only cosmetic: the Sound, its parameters and its lines stay as they were.
void testMovingASoundIsOnlyCosmetic()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"source", "talas.noise", {}}, {0, 0}), "add the source");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {300, 0}), "add output");
    requireOk(editor.connect({"source", "audioOut", "out", "audioIn"}), "connect the source");
    const auto graph = editor.document().graph;
    const auto presentation = editor.document().presentation;
    requireOk(editor.moveNode("source", {120, 240}), "move the source");
    require(editor.document().graph == graph && !(editor.document().presentation == presentation),
        "moving a Sound should change only where it is drawn");
}

void testDeletingASoundGivesItsPlaceToItsInput()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"source", "talas.noise", {}}, {0, 0}), "add the source");
    requireOk(editor.addNode({"resonator", "talas.filter", {}}, {300, 0}), "add the resonator");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"source", "audioOut", "resonator", "audioIn"}), "connect the source");
    requireOk(editor.connect({"resonator", "audioOut", "out", "audioIn"}), "connect the resonator");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireError(editor.replaceNodeWithItsInput(std::nullopt, "source"), talas::project::EditError::InvalidGraph,
        "the first Sound of the chain should not be deleted while the resonator reads it");
    requireOk(editor.replaceNodeWithItsInput(std::nullopt, "resonator"), "delete the resonator");
    const auto& spliced = editor.document().graph;
    require(hasConnection(spliced, {"source", "audioOut", "out", "audioIn"}) && spliced.connections.size() == 1
            && std::ranges::none_of(spliced.nodes, [](const auto& node) { return node.instanceId == "resonator"; }),
        "the source should take the resonator's place in the signal path");
    requireOk(editor.undo(), "one undo should bring the resonator back");
    require(talas::project::serializeProjectDocument(editor.document()) == before,
        "undoing the deletion should restore the chain");

    requireOk(editor.addNode({"second", "talas.noise", {}}, {0, 200}), "add a second source");
    requireOk(editor.addNode({"mix", "talas.mixerV2", {}}, {300, 200}), "add a mixer");
    requireOk(editor.addNode({"spare", "talas.noise", {}}, {0, 400}), "add a Sound nothing reads");
    requireOk(editor.disconnect("out", "audioIn"), "free the output");
    requireOk(editor.connect({"resonator", "audioOut", "mix", "inputs"}), "mix the resonator");
    requireOk(editor.connect({"second", "audioOut", "mix", "inputs"}), "mix the second source");
    requireOk(editor.connect({"mix", "leftOut", "out", "audioIn"}), "connect the mixer");
    requireOk(editor.replaceNodeWithItsInput(std::nullopt, "mix", std::string_view{"second"}),
        "delete the mixer, keeping the second source");
    require(hasConnection(editor.document().graph, {"second", "audioOut", "out", "audioIn"}),
        "the named input should take the mixer's place");
    requireOk(editor.replaceNodeWithItsInput(std::nullopt, "spare"), "a Sound nothing reads should simply go");
    require(std::ranges::none_of(editor.document().graph.nodes, [](const auto& node) { return node.instanceId == "spare"; }),
        "the spare Sound should be gone");
}

// A Filter dropped on a HarmonicResonator that a
// GenericSource feeds replaces the resonator only (Replace), the source then
// feeding the Filter, or the resonator and its input too (Replace all), the
// source lost; a Sound that feeds another Sound as well stays, without its
// line to the replaced one.
void testReplaceAllTakesTheInputsAway()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"source", "talas.genericSource", {}}, {0, 0}), "add the source");
    requireOk(editor.addNode({"resonator", "talas.harmonicResonator", {}}, {300, 0}), "add the resonator");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"source", "leftOut", "resonator", "audioIn"}), "feed the resonator");
    requireOk(editor.connect({"resonator", "audioOut", "out", "audioIn"}), "connect the resonator");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    const auto hasNode = [&](std::string_view id) {
        return std::ranges::any_of(editor.document().graph.nodes, [&](const auto& node) { return node.instanceId == id; });
    };
    requireOk(editor.replaceNode(std::nullopt, "resonator", {"filter", "talas.filter", {}}), "Replace");
    require(hasConnection(editor.document().graph, {"source", "leftOut", "filter", "audioIn"})
            && hasConnection(editor.document().graph, {"filter", "audioOut", "out", "audioIn"}),
        "Replace should keep the GenericSource feeding the Filter");
    requireOk(editor.undo(), "undo the Replace");
    requireOk(editor.replaceNode(std::nullopt, "resonator", {"filter", "talas.filter", {}}, true), "Replace all");
    require(!hasNode("source") && !hasNode("resonator") && hasConnection(editor.document().graph, {"filter", "audioOut", "out", "audioIn"})
            && hasConnection(editor.document().graph, {"noise", "audioOut", "filter", "audioIn"})
            && editor.document().graph.connections.size() == 2,
        "Replace all should lose the GenericSource, the Filter taking its default input");
    requireOk(editor.undo(), "undo the Replace all");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "one undo should take back Replace all");

    requireOk(editor.addNode({"other", "talas.audioOutput", {}}, {600, 200}), "add a second output");
    requireOk(editor.connect({"source", "leftOut", "other", "audioIn"}), "the source feeds it too");
    requireOk(editor.replaceNode(std::nullopt, "resonator", {"filter", "talas.filter", {}}, true), "Replace all once more");
    require(hasNode("source") && hasConnection(editor.document().graph, {"source", "leftOut", "other", "audioIn"})
            && !hasConnection(editor.document().graph, {"source", "leftOut", "filter", "audioIn"}),
        "a Sound feeding another as well should stay, without its line to the Filter");

    // A SoundCollectionVariable cannot be dropped
    // onto a Sound's icon; it goes into an Inputs field.
    const auto kept = talas::project::serializeProjectDocument(editor.document());
    requireError(editor.replaceNode(std::nullopt, "filter", {"voices", "talas.soundCollectionVariable", {}}),
        talas::project::EditError::InvalidGraph, "a SoundCollectionVariable should not replace a Sound");
    require(talas::project::serializeProjectDocument(editor.document()) == kept, "a refused drop should leave the Sounds as they were");
}

// A Sound of several inputs dropped on a line asks
// which input takes it, and Delete which input takes a deleted Sound's
// place; a stereo input's right channel is no choice of its own.
// Wherever a Sound may be dragged from the
// prototypes, any other Sound serves as well, as a Sound of the Sound file
// window: dropped on a line it goes between the Sounds the line joined.
void testAnySoundServesWhereAPrototypeDoes()
{
    talas::project::ProjectSoundDefinition echo;
    echo.definitionId = "echo";
    echo.displayName = "Echo";
    echo.inputs.push_back({"audioIn", talas::core::PortType::Audio, "filter", "audioIn"});
    echo.outputs.push_back({"audioOut", talas::core::PortType::Audio, "filter", "audioOut"});
    echo.graph.nodes.push_back({"filter", "talas.filter", {}});
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(echo));
    talas::project::ProjectEditor editor(std::move(document), builtinRegistry());
    requireOk(editor.addNode({"noise", "talas.noise", {}}, {0, 0}), "add noise");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"noise", "audioOut", "out", "audioIn"}), "connect noise");
    talas::project::ProjectNode instance;
    instance.instanceId = "echo1";
    instance.soundDefinitionId = "echo";
    requireOk(editor.insertNodeOnConnection(std::nullopt, instance, {300, 0}, {"noise", "audioOut", "out", "audioIn"}),
        "a Sound of the Sound file window dropped on the line should be inserted as a prototype is");
    const auto& graph = editor.document().graph;
    require(hasConnection(graph, {"noise", "audioOut", "echo1", "audioIn"}) && hasConnection(graph, {"echo1", "audioOut", "out", "audioIn"})
            && !hasConnection(graph, {"noise", "audioOut", "out", "audioIn"}),
        "the line's source should feed the Sound and the Sound its destination");
}

void testInsertionAndDeleteAskWhichInput()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise", "talas.noise", {}}, {0, 0}), "add noise");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"noise", "audioOut", "out", "audioIn"}), "connect noise");
    const talas::core::Connection line{"noise", "audioOut", "out", "audioIn"};
    require(editor.inputsTakingConnection(std::nullopt, {"fade", "talas.crossfade", {}}, line)
                == std::vector<std::string>{"snd1", "snd2"},
        "a Crossfade dropped on the line should offer both its inputs");
    require(editor.inputsTakingConnection(std::nullopt, {"level", "talas.attenuator", {}}, line)
                == std::vector<std::string>{"audioIn"},
        "an Attenuator's right channel should be no choice of its own");
    requireOk(editor.insertNodeOnConnection(std::nullopt, {"fade", "talas.crossfade", {}}, {300, 0}, line,
                  std::string_view{"snd2"}),
        "insert the Crossfade into its second input");
    require(hasConnection(editor.document().graph, {"noise", "audioOut", "fade", "snd2"})
            && hasConnection(editor.document().graph, {"fade", "leftOut", "out", "audioIn"}),
        "the line's source should feed the input chosen");
    requireError(editor.insertNodeOnConnection(std::nullopt, {"other", "talas.crossfade", {}}, {300, 100},
                     {"fade", "leftOut", "out", "audioIn"}, std::string_view{"missing"}),
        talas::project::EditError::IncompatiblePortTypes, "an input the Sound lacks should be refused");
    requireOk(editor.addNode({"tone", "talas.oscillator", {}}, {0, 200}), "add a tone");
    requireOk(editor.connect({"tone", "audioOut", "fade", "snd1"}), "feed the first input");
    require(editor.inputsReplacingNode(std::nullopt, "fade") == std::vector<std::string>{"noise", "tone"},
        "deleting the Crossfade should offer both Sounds feeding it");
    requireOk(editor.replaceNodeWithItsInput(std::nullopt, "fade", std::string_view{"tone"}),
        "delete the Crossfade, keeping the tone");
    require(hasConnection(editor.document().graph, {"tone", "audioOut", "out", "audioIn"}),
        "the input chosen should take the Crossfade's place");
}

// A Sound dropped on a line is
// inserted between the Sounds the line joined; dropped on a Sound it
// replaces that Sound. Copy and Paste carry the selected
// Sounds with the connections among them.
// A Sound dropped on the gray circle with
// a plus at a line's middle is mixed with the Sound feeding the line.
void testASoundDroppedOnALinesPlusIsMixedWithIt()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise", "talas.noise", {}}, {0, 0}), "add noise");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"noise", "audioOut", "out", "audioIn"}), "connect noise");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.mixNodeOnConnection(std::nullopt, {"mix", "talas.mixerV2", {}}, {"osc", "talas.oscillator", {}}, {300, 0},
                  {"noise", "audioOut", "out", "audioIn"}),
        "a Sound dropped on the line's plus should be mixed in");
    const auto& graph = editor.document().graph;
    require(hasConnection(graph, {"noise", "audioOut", "mix", "inputs"}) && hasConnection(graph, {"osc", "audioOut", "mix", "inputs"})
            && hasConnection(graph, {"mix", "leftOut", "out", "audioIn"}) && !hasConnection(graph, {"noise", "audioOut", "out", "audioIn"}),
        "the line's source and the dropped Sound should both feed the Mixer, which takes the line");
    require(nodePosition(editor.document(), "mix") == talas::project::CanvasPosition{300, 0},
        "the Mixer should sit where the plus was");
    require(!editor.mixNodeOnConnection(std::nullopt, {"x", "talas.mixerV2", {}}, {"x", "talas.oscillator", {}}, {0, 0},
                {"mix", "leftOut", "out", "audioIn"}).ok(),
        "the Mixer and the dropped Sound should need names of their own");
    requireOk(editor.undo(), "one undo should take the mix back");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "undoing should restore the line");
}

void testDroppedSoundsInsertOnLinesReplaceSoundsAndPasteCopies()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"noise", "talas.noise", {}}, {0, 0}), "add noise");
    requireOk(editor.addNode({"out", "talas.audioOutput", {}}, {600, 0}), "add output");
    requireOk(editor.connect({"noise", "audioOut", "out", "audioIn"}), "connect noise");
    const auto beforeInsert = talas::project::serializeProjectDocument(editor.document());

    requireOk(editor.insertNodeOnConnection(
            std::nullopt,
            {"filter", "talas.filter", {}},
            {300, 10},
            {"noise", "audioOut", "out", "audioIn"}),
        "a filter dropped on the line should be inserted");
    const auto& inserted = editor.document().graph;
    require(hasConnection(inserted, {"noise", "audioOut", "filter", "audioIn"})
            && hasConnection(inserted, {"filter", "audioOut", "out", "audioIn"})
            && !hasConnection(inserted, {"noise", "audioOut", "out", "audioIn"})
            && inserted.connections.size() == 2,
        "the line's source should feed the filter and the filter its destination");
    require(nodePosition(editor.document(), "filter")
            == talas::project::CanvasPosition{300, 10},
        "the inserted Sound should sit where it was dropped");
    requireOk(editor.undo(), "one undo should take the insertion back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeInsert,
        "undoing the insertion should restore the line");

    requireOk(editor.insertNodeOnConnection(
            std::nullopt,
            {"osc", "talas.oscillator", {}},
            {300, 10},
            {"noise", "audioOut", "out", "audioIn"}),
        "an oscillator dropped on an audio line should be inserted");
    require(hasConnection(editor.document().graph, {"noise", "audioOut", "osc", "modulator"}),
        "an audio line should feed the first audio input, not a control input");
    requireOk(editor.undo(), "undo the oscillator insertion");
    requireError(
        editor.insertNodeOnConnection(
            std::nullopt,
            {"constant", "talas.constant", {}},
            {300, 10},
            {"noise", "audioOut", "out", "audioIn"}),
        talas::project::EditError::IncompatiblePortTypes,
        "a Sound without inputs dropped on a line");
    requireError(
        editor.insertNodeOnConnection(
            std::nullopt,
            {"filter", "talas.filter", {}},
            {300, 10},
            {"noise", "audioOut", "out", "rightIn"}),
        talas::project::EditError::InvalidGraph,
        "a drop on a line that is no longer there");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeInsert,
        "refused insertions should leave the graph alone");

    // Replacing: noise -> filter -> out, a constant on the filter's frequency.
    requireOk(editor.insertNodeOnConnection(
            std::nullopt,
            {"filter", "talas.filter", {}},
            {300, 10},
            {"noise", "audioOut", "out", "audioIn"}),
        "insert the filter to replace");
    requireOk(editor.addNode({"cutoff", "talas.constant", {}}, {150, 200}), "add constant");
    requireOk(editor.connect({"cutoff", "value", "filter", "frequency"}), "connect constant");
    const auto beforeReplace = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.replaceNode(std::nullopt, "filter", {"shelf", "talas.lowShelvingFilter", {}}),
        "a shelving filter dropped on the filter should replace it");
    const auto& replaced = editor.document().graph;
    require(std::ranges::none_of(replaced.nodes, [](const auto& node) {
                return node.instanceId == "filter";
            }),
        "the replaced Sound should be gone");
    require(hasConnection(replaced, {"noise", "audioOut", "shelf", "audioIn"})
            && hasConnection(replaced, {"shelf", "audioOut", "out", "audioIn"})
            && hasConnection(replaced, {"cutoff", "value", "shelf", "cutoffFreq"})
            && replaced.connections.size() == 3,
        "the replacement should take the old Sound's output and inputs, by name or else the first free input of the same kind");
    require(nodePosition(editor.document(), "shelf")
            == talas::project::CanvasPosition{300, 10},
        "the replacement should take the old Sound's place");
    requireOk(editor.undo(), "one undo should take the replacement back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforeReplace,
        "undoing the replacement should restore the old Sound and its connections");
    requireOk(editor.replaceNode(std::nullopt, "noise", {"osc", "talas.oscillator", {}}),
        "an oscillator dropped on the noise should replace it");
    require(hasConnection(editor.document().graph, {"osc", "audioOut", "filter", "audioIn"}),
        "the replacing source should feed what the noise fed");
    requireOk(editor.undo(), "undo the source replacement");
    requireError(editor.replaceNode(std::nullopt, "missing", {"osc", "talas.oscillator", {}}),
        talas::project::EditError::UnknownNodeId, "replacing a missing Sound");

    // Copy and paste.
    requireOk(editor.setParameter("noise", {"frequency", std::string{"440 hz"}}),
        "give the noise a parameter value to copy");
    const std::array copied{std::string{"filter"}, std::string{"noise"}};
    const auto copy = talas::project::copyProjectGraphFragment(
        editor.document(), std::nullopt, copied);
    require(copy.fragment.has_value(), "copying the selection should succeed: " + copy.message);
    require(copy.fragment->nodes.size() == 2
            && copy.fragment->connections
                == std::vector<talas::core::Connection>{{"noise", "audioOut", "filter", "audioIn"}},
        "a copy should carry the selected Sounds and only the connections among them");
    const auto text = talas::project::serializeProjectGraphFragment(*copy.fragment);
    const auto parsed = talas::project::parseProjectGraphFragment(text);
    require(parsed.fragment.has_value() && *parsed.fragment == *copy.fragment,
        "the clipboard text should give back the copied Sounds: " + parsed.message);
    require(!talas::project::parseProjectGraphFragment("hello").fragment.has_value()
            && !talas::project::parseProjectGraphFragment(R"({"talasGraphFragment":2,"nodes":[],"connections":[]})").fragment.has_value(),
        "other clipboard text should not paste as Sounds");

    const auto beforePaste = talas::project::serializeProjectDocument(editor.document());
    std::vector<std::string> pasted;
    requireOk(editor.pasteGraphFragment(std::nullopt, *parsed.fragment, {500, 400}, pasted),
        "pasting into the same graph should succeed");
    require(pasted == std::vector<std::string>({"noise-copy", "filter-copy"}),
        "pasted copies should take fresh ids beside their originals");
    require(nodePosition(editor.document(), "noise-copy")
                == talas::project::CanvasPosition{500, 400}
            && nodePosition(editor.document(), "filter-copy")
                == talas::project::CanvasPosition{800, 410},
        "the pasted group should keep its arrangement with its corner at the paste point");
    require(hasConnection(editor.document().graph, {"noise-copy", "audioOut", "filter-copy", "audioIn"}),
        "the pasted Sounds should keep their connection");
    const auto pastedNoise = std::ranges::find(
        editor.document().graph.nodes, std::string{"noise-copy"}, &talas::project::ProjectNode::instanceId);
    require(pastedNoise != editor.document().graph.nodes.end()
            && std::ranges::any_of(pastedNoise->parameters, [](const auto& parameter) {
                   return parameter.parameterId == "frequency"
                       && parameter.expression == std::optional<std::string>{"440 hz"};
               }),
        "pasted Sounds should keep their parameter values");
    // A pasted or dragged Sound is a copy, so
    // changing it leaves the original as it was.
    requireOk(editor.setParameter("noise-copy", {"frequency", std::string{"880 hz"}}), "change the copy");
    const auto original = std::ranges::find(
        editor.document().graph.nodes, std::string{"noise"}, &talas::project::ProjectNode::instanceId);
    require(std::ranges::any_of(original->parameters, [](const auto& parameter) {
                return parameter.parameterId == "frequency" && parameter.expression == std::optional<std::string>{"440 hz"};
            }),
        "changing the copy should leave the original unchanged");
    requireOk(editor.undo(), "undo the change to the copy");
    requireOk(editor.undo(), "one undo should take the paste back");
    require(talas::project::serializeProjectDocument(editor.document()) == beforePaste,
        "undoing the paste should restore the graph");

    auto other = makeEditor();
    requireOk(other.pasteGraphFragment(std::nullopt, *parsed.fragment, {0, 0}, pasted),
        "pasting into another project should succeed");
    require(pasted == std::vector<std::string>({"noise", "filter"}),
        "pasted Sounds should keep their ids where nothing else has them");
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    requireOk(other.addSoundDefinition(std::move(definition)), "add a Sound definition");
    requireOk(other.pasteGraphFragment(std::string_view{"voice"}, *parsed.fragment, {10, 20}, pasted),
        "pasting into a Sound definition should succeed");
    require(nodePosition(other.document(), "noise", std::string_view{"voice"})
            == talas::project::CanvasPosition{10, 20},
        "a paste into a Sound definition should land in its graph");
    auto bad = *parsed.fragment;
    bad.nodes.front().typeId = "talas.notAPrototype";
    const auto beforeBad = talas::project::serializeProjectDocument(other.document());
    requireError(other.pasteGraphFragment(std::nullopt, bad, {0, 0}, pasted),
        talas::project::EditError::UnknownNodeType, "pasting an unknown prototype");
    require(pasted.empty()
            && talas::project::serializeProjectDocument(other.document()) == beforeBad,
        "a refused paste should change nothing");
}

void testHistoryTransactionsKeepTheirOwners()
{
    auto owned = makeEditor();
    requireOk(owned.addNode({"movable", "talas.noise", {}}, {10, 20}),
        "owner fixture should add a node");
    const auto gesture = owned.beginOwnedHistoryTransaction();
    requireOk(owned.moveNode("movable", {30, 40}, gesture),
        "owned gesture should accept its first move");
    owned.endHistoryTransaction();
    requireOk(owned.moveNode("movable", {50, 60}, gesture),
        "an unowned end must leave another owner's gesture open");
    owned.endOwnedHistoryTransaction(gesture);
    requireOk(owned.undo(), "owned gesture should undo");
    require(nodePosition(owned.document(), "movable")
            == talas::project::CanvasPosition{10, 20},
        "the whole owned gesture should undo as one step");

    const auto drag = owned.beginOwnedHistoryTransaction();
    requireOk(owned.moveNode("movable", {70, 80}, drag),
        "second owned gesture should move");
    talas::project::VirtualControl first;
    first.eventName = "First";
    talas::project::VirtualControl second;
    second.eventName = "Second";
    owned.beginHistoryTransaction();
    requireOk(owned.addVirtualControl(first),
        "grouped edit should add its first Event Value");
    requireOk(owned.addVirtualControl(second),
        "grouped edit should add its second Event Value");
    owned.endHistoryTransaction();
    requireOk(owned.undo(), "grouped edits should undo");
    require(owned.document().virtualControls.empty()
            && nodePosition(owned.document(), "movable")
                == talas::project::CanvasPosition{70, 80},
        "an unowned group opened during a gesture should record only its own edits");

    auto values = makeEditor();
    talas::project::VirtualControl level;
    level.eventName = "Level";
    talas::project::VirtualControl other;
    other.eventName = "Other";
    requireOk(values.addVirtualControl(level),
        "value fixture should add Level");
    requireOk(values.addVirtualControl(other),
        "value fixture should add Other");
    const auto valueOf = [&](std::string_view eventName) {
        return std::ranges::find(
            values.document().virtualControls,
            eventName,
            &talas::project::VirtualControl::eventName)->value;
    };
    const auto slider = values.beginValueHistoryTransaction();
    require(values.ownsHistoryTransaction(slider)
            && values.hasValueHistoryTransaction(),
        "a value transaction should report its owner");
    requireOk(values.setVirtualControlValue("Level", 0.25),
        "value transaction should record its gesture value");
    requireOk(values.setVirtualControlValue("Other", 0.5),
        "value transaction should record a concurrent controller value");
    requireOk(values.setVirtualControlValue("Level", 0.75),
        "value transaction should record its final gesture value");
    requireOk(values.addNode({"osc", "talas.oscillator", {}, {}}),
        "structural edit should apply during a value gesture");
    require(!values.ownsHistoryTransaction(slider)
            && !values.hasValueHistoryTransaction(),
        "a structural edit must close the value transaction first");
    values.endOwnedHistoryTransaction(slider);
    requireOk(values.undo(), "structural edit should undo");
    require(values.document().graph.nodes.empty()
            && valueOf("Level") == 0.75 && valueOf("Other") == 0.5,
        "the structural edit should undo on its own");
    requireOk(values.undo(), "value gesture should undo");
    require(valueOf("Level") == 0.0 && valueOf("Other") == 0.0,
        "the value gesture should undo as one step");

    const auto dispatched = values.beginValueHistoryTransaction();
    talas::project::PreparedVirtualControlValueEdit prepared;
    requireOk(values.prepareVirtualControlValue("Level", 1.0, prepared),
        "dispatched value should prepare inside a value transaction");
    values.commitVirtualControlValue(std::move(prepared));
    values.endOwnedHistoryTransaction(dispatched);
    requireOk(values.undo(), "dispatched value should undo");
    require(valueOf("Level") == 0.0,
        "a prepared value should join the value transaction's step");

    auto timeline = makeEditor();
    requireOk(timeline.addTimelineEventValue(
            {1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25}),
        "Timeline token fixture should add an event");
    const auto markerDrag = timeline.beginOwnedHistoryTransaction();
    requireOk(timeline.updateTimelineEventValue(
            0, {1.5, "Gain", talas::expr::ValueDomain::Scalar, 0.25}, markerDrag),
        "marker drag should move inside its owned transaction");
    requireOk(timeline.setTimelineLoop({true, 0.0, 4.0}, markerDrag),
        "loop drag should edit inside its owned transaction");
    requireOk(timeline.addVirtualControl(level),
        "foreign edit should close the marker drag");
    requireError(timeline.updateTimelineEventValue(
            0, {2.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25}, markerDrag),
        talas::project::EditError::InvalidSelection,
        "a closed marker drag must not keep editing");
    requireOk(timeline.undo(), "foreign edit should undo");
    requireOk(timeline.undo(), "marker drag should undo");
    require(timeline.document().timeline.eventValues.front().timeSeconds == 1.0
            && !timeline.document().timeline.loop.enabled,
        "the marker and loop drag should undo as one step");
}

void testEditorHistoryRestoresConnectionsAndNodeRemoval()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc1", "talas.oscillator", {}, {}}), "editor should add oscillator for history fixture");
    requireOk(editor.addNode({"vca1", "talas.vca", {}, {}}), "editor should add VCA for history fixture");
    requireOk(editor.addNode({"out1", "talas.audioOutput", {}, {}}), "editor should add output for history fixture");
    requireOk(editor.connect({"osc1", "audioOut", "vca1", "audioIn"}), "editor should add first history connection");
    requireOk(editor.connect({"vca1", "audioOut", "out1", "audioIn"}), "editor should add second history connection");

    requireOk(editor.removeNode("vca1"), "removing connected node should succeed");
    require(editor.document().graph.nodes.size() == 2, "node removal should update history fixture document");
    require(editor.document().graph.connections.empty(), "node removal should remove history fixture connections");
    requireOk(editor.undo(), "undo should restore removed node snapshot");
    require(editor.document().graph.nodes.size() == 3, "undo should restore removed node");
    require(editor.document().graph.connections.size() == 2, "undo should restore incident connections");
    requireOk(editor.redo(), "redo should remove node snapshot again");
    require(editor.document().graph.nodes.size() == 2, "redo should remove node again");
    require(editor.document().graph.connections.empty(), "redo should remove incident connections again");
}

void testEditorReplacesOnlyValidatedDocumentsAndClearsHistory()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"old", "talas.oscillator", {}}), "editor should create initial history");

    talas::project::ProjectDocument replacement;
    replacement.metadata.title = "Replacement";
    replacement.graph.nodes.push_back({"new", "talas.noise", {}});
    talas::project::reconcileProjectPresentation(replacement);
    talas::project::findProjectNodePresentation(
        replacement,
        std::nullopt,
        "new")->position = {72, 24};
    requireOk(editor.replaceDocument(replacement), "valid replacement document should load");
    require(editor.document().metadata.title == "Replacement", "replacement metadata should commit");
    require(editor.document().graph.nodes.size() == 1 && editor.document().graph.nodes[0].instanceId == "new",
            "replacement nodes should commit");
    require(!editor.canUndo() && !editor.canRedo(), "loading a document should clear history");

    talas::project::ProjectDocument invalid;
    invalid.metadata.title = "Invalid";
    invalid.graph.nodes.push_back({"bad", "talas.unknown", {}, {}});
    requireError(editor.replaceDocument(invalid), talas::project::EditError::UnknownNodeType,
                 "invalid replacement document");
    require(editor.document().metadata.title == "Replacement", "invalid replacement should leave current document unchanged");
    require(editor.document().graph.nodes[0].instanceId == "new", "invalid replacement should leave current nodes unchanged");

    auto invalidTimeline = replacement;
    invalidTimeline.timeline.eventValues = {
        {1.0, "Gain", talas::expr::ValueDomain::Scalar, 1.0},
        {0.5, "Gain", talas::expr::ValueDomain::Scalar, 0.0},
    };
    requireError(editor.replaceDocument(std::move(invalidTimeline)), talas::project::EditError::InvalidTimeline,
                 "replacement document with unordered Timeline events");
    require(editor.document().metadata.title == "Replacement",
            "invalid Timeline replacement should leave current document unchanged");

    auto invalidSampleMap = replacement;
    talas::project::ProjectSampleMap map;
    map.mapId = "invalidMap";
    map.displayName = "Invalid Map";
    talas::project::ProjectSampleRegion region;
    region.regionId = "invalidRegion";
    region.assetId = "missingAudio";
    region.rootNote = 200;
    map.regions.push_back(std::move(region));
    invalidSampleMap.sampleMapLibrary.maps.push_back(std::move(map));
    requireError(
        editor.replaceDocument(std::move(invalidSampleMap)),
        talas::project::EditError::InvalidSampleMap,
        "replacement document with an invalid sample map");
    require(
        editor.document().metadata.title == "Replacement",
        "invalid sample-map replacement should leave current document unchanged");
}

void testPreparedWholeDocumentEditIsFailureAtomicAndOneUndo()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc", "talas.oscillator", {}}),
        "whole-document edit fixture should create one prior edit");
    const auto before = talas::project::serializeProjectDocument(
        editor.document());
    auto candidate = editor.document();
    candidate.graph.nodes.push_back({"filter", "talas.filter", {}});
    requireOk(editor.applyDocumentEdit(candidate),
        "validated prepared document should commit as one edit");
    require(editor.document().graph.nodes.size() == 2,
        "prepared document should publish atomically");
    requireOk(editor.undo(),
        "one undo should restore the complete prepared document edit");
    require(talas::project::serializeProjectDocument(editor.document()) == before,
        "whole-document undo should restore exact prior bytes");

    auto invalid = editor.document();
    invalid.graph.nodes.push_back({"bad", "talas.unknown", {}});
    requireError(editor.applyDocumentEdit(std::move(invalid)),
        talas::project::EditError::UnknownNodeType,
        "invalid prepared document should fail closed");
    require(talas::project::serializeProjectDocument(editor.document()) == before,
        "failed prepared edit should preserve the prior document and history");
}

void testVirtualControlsValidateAndParticipateInUndoHistory()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.25;

    requireOk(editor.addVirtualControl(gain), "editor should add a valid Event Value control");
    require(editor.document().virtualControls.size() == 1, "added Event Value should be retained");
    require(editor.document().virtualControls.front().value == 0.25, "added Event Value should retain its value");

    requireError(editor.addVirtualControl(gain), talas::project::EditError::DuplicateVirtualControl,
                 "duplicate Event Value name");

    auto invalidName = gain;
    invalidName.eventName = "Invalid-Name";
    requireError(editor.addVirtualControl(invalidName), talas::project::EditError::InvalidVirtualControl,
                 "Event Value with an invalid name");

    auto invalidRange = gain;
    invalidRange.eventName = "OutOfRange";
    invalidRange.value = 1.5;
    requireError(editor.addVirtualControl(invalidRange), talas::project::EditError::InvalidVirtualControl,
                 "Event Value outside its persisted range");

    requireOk(editor.setVirtualControlValue("Gain", 0.75), "editor should update a known Event Value");
    require(editor.document().virtualControls.front().value == 0.75, "updated Event Value should retain its new value");
    requireOk(editor.undo(), "editor should undo an Event Value value change");
    require(editor.document().virtualControls.front().value == 0.25, "undo should restore the Event Value value");
    requireOk(editor.undo(), "editor should undo Event Value creation");
    require(editor.document().virtualControls.empty(), "undo should remove the created Event Value");
}

void testVirtualControlPresentationEditsAreUndoable()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    requireOk(
        editor.addVirtualControl(gain, {24, 48}),
        "editor should add a positioned Performance control");
    const auto presented = [&]() {
        return talas::project::findProjectPerformanceControlPresentation(
            editor.document(),
            "Gain",
            talas::expr::ValueDomain::Scalar);
    };
    require(
        presented() != nullptr
            && presented()->position == talas::project::CanvasPosition{24, 48}
            && presented()->visible,
        "new Performance control should publish its portable presentation");

    requireOk(
        editor.moveVirtualControl("Gain", {320, -24}),
        "Performance control move should succeed");
    requireOk(
        editor.setVirtualControlVisible("Gain", false),
        "Performance control visibility should be editable");
    require(
        presented()->position == talas::project::CanvasPosition{320, -24}
            && !presented()->visible,
        "Performance presentation edits should publish atomically");

    requireOk(editor.undo(), "visibility edit should be undoable");
    require(presented()->visible, "visibility undo should restore visible state");
    requireOk(editor.undo(), "Performance move should be undoable");
    require(
        presented()->position == talas::project::CanvasPosition{24, 48},
        "move undo should restore the original portable position");
    requireOk(editor.redo(), "Performance move should be redoable");
    require(
        presented()->position == talas::project::CanvasPosition{320, -24},
        "move redo should restore the edited portable position");

    requireError(
        editor.moveVirtualControl("Missing", {}),
        talas::project::EditError::UnknownVirtualControl,
        "moving an unknown Performance control");
    requireError(
        editor.setVirtualControlVisible("Missing", false),
        talas::project::EditError::UnknownVirtualControl,
        "changing visibility for an unknown Performance control");
}

void testVirtualControlsRejectMidiMappingCollisionsAndUnknownUpdates()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.midiMapping = exactMidiMapping(2, 74);
    requireOk(editor.addVirtualControl(gain), "editor should add a MIDI-mapped Event Value");

    auto cutoff = gain;
    cutoff.eventName = "Cutoff";
    requireError(editor.addVirtualControl(cutoff), talas::project::EditError::DuplicateMidiMapping,
                 "duplicate MIDI mapping");
    requireError(editor.setVirtualControlValue("Missing", 0.5), talas::project::EditError::UnknownVirtualControl,
                 "updating an unknown Event Value");

    talas::project::ProjectDocument invalidReplacement;
    invalidReplacement.virtualControls.push_back(gain);
    invalidReplacement.virtualControls.push_back(cutoff);
    requireError(editor.replaceDocument(std::move(invalidReplacement)), talas::project::EditError::DuplicateMidiMapping,
                 "loading a project with duplicate MIDI mappings");
    require(editor.document().virtualControls.size() == 1 && editor.document().virtualControls.front().eventName == "Gain",
            "failed replacement should leave persisted Event Values unchanged");
}

void testVirtualControlMidiMappingsCanBeChangedAndCleared()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    talas::project::VirtualControl cutoff;
    cutoff.eventName = "Cutoff";
    requireOk(editor.addVirtualControl(gain), "editor should add gain Event Value");
    requireOk(editor.addVirtualControl(cutoff), "editor should add cutoff Event Value");

    const auto mapping = exactMidiMapping(3, 21);
    requireOk(editor.setVirtualControlMidiMapping("Gain", mapping), "editor should persist a MIDI mapping");
    const auto& storedMapping = editor.document().virtualControls.front().midiMapping;
    require(storedMapping.has_value() && storedMapping->channel == 3 && storedMapping->controller == 21,
            "MIDI mapping should be retained");
    requireError(editor.setVirtualControlMidiMapping("Cutoff", mapping), talas::project::EditError::DuplicateMidiMapping,
                 "MIDI mapping collision should be rejected");
    requireOk(editor.setVirtualControlMidiMapping("Gain", std::nullopt), "editor should clear an existing MIDI mapping");
    require(!editor.document().virtualControls.front().midiMapping.has_value(), "cleared MIDI mapping should be absent");
    requireOk(editor.setVirtualControlMidiMapping("Cutoff", mapping), "released MIDI mapping should be reusable");
}

void testVirtualControlMidiLearnConflictsAndExplicitReplacementAreAtomic()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.midiMapping = exactMidiMapping(3, 21);
    talas::project::VirtualControl cutoff;
    cutoff.eventName = "Cutoff";
    requireOk(editor.addVirtualControl(gain),
              "learn conflict fixture should add mapped Gain");
    requireOk(editor.addVirtualControl(cutoff),
              "learn conflict fixture should add Cutoff");

    const auto beforeConflict =
        talas::project::serializeProjectDocument(editor.document());
    const auto rejected = editor.learnVirtualControlMidiMapping(
        "Cutoff", exactMidiMapping(3, 21));
    require(
        rejected.error
                == talas::project::EditError::DuplicateMidiMapping
            && rejected.message.find("Gain") != std::string::npos
            && rejected.message.find("unchanged") != std::string::npos,
        "rejected learn conflict should name the existing owner and recovery state");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeConflict,
        "rejected learn conflict should preserve exact project bytes");

    const auto invalidPolicy =
        editor.learnVirtualControlMidiMapping(
            "Cutoff",
            exactMidiMapping(3, 21),
            static_cast<talas::project::MidiMappingConflictPolicy>(99));
    require(
        !invalidPolicy.ok()
            && talas::project::serializeProjectDocument(
                   editor.document())
                == beforeConflict,
        "unknown learn conflict policy should fail closed without mutation");

    requireOk(
        editor.learnVirtualControlMidiMapping(
            "Cutoff",
            exactMidiMapping(3, 21),
            talas::project::MidiMappingConflictPolicy::ReplaceExisting),
        "explicit learn replacement should move one MIDI source atomically");
    require(
        !editor.document().virtualControls[0].midiMapping.has_value()
            && editor.document().virtualControls[1].midiMapping.has_value()
            && editor.document().virtualControls[1].midiMapping->channel == 3
            && editor.document().virtualControls[1].midiMapping->controller == 21,
        "explicit replacement should leave one exact new owner");
    requireOk(editor.undo(),
              "explicit MIDI mapping replacement should be one Undo step");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeConflict,
        "one Undo should restore exact prior MIDI mapping ownership");
}

void testVirtualControlMidiAuthoringRequiresExactSourceAuthority()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    requireOk(editor.addVirtualControl(gain),
              "exact-authority fixture should add Gain");

    const auto beforeLegacy =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setVirtualControlMidiMapping(
            "Gain", talas::project::MidiControlMapping{2, 74}),
        talas::project::EditError::InvalidVirtualControl,
        "current authoring should reject legacy-any MIDI authority");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeLegacy,
        "rejected legacy authoring should not mutate the document");

    talas::project::VirtualControl legacy;
    legacy.eventName = "Legacy";
    legacy.midiMapping = talas::project::MidiControlMapping{2, 74};
    requireError(
        editor.addVirtualControl(legacy),
        talas::project::EditError::InvalidVirtualControl,
        "new Event Values should not author compatibility-only mappings");

    talas::project::VirtualControl cutoff;
    cutoff.eventName = "Cutoff";
    cutoff.midiMapping = exactMidiMapping(2, 74, "device-b");
    requireOk(
        editor.addVirtualControl(cutoff),
        "different exact inputs should share a channel and controller");
    requireOk(
        editor.setVirtualControlMidiMapping(
            "Gain", exactMidiMapping(2, 74, "device-a")),
        "first exact input should map independently");

    auto malformed = editor.document();
    malformed.virtualControls.front().midiMapping->sourceMode =
        static_cast<talas::project::MidiControlSourceMode>(99);
    requireError(
        editor.replaceDocument(std::move(malformed)),
        talas::project::EditError::InvalidVirtualControl,
        "document replacement should reject malformed MIDI authority");

    talas::project::ProjectDocument migrated;
    migrated.virtualControls.push_back(std::move(legacy));
    requireOk(
        editor.replaceDocument(std::move(migrated)),
        "document replacement should retain migrated legacy authority");
}

void testTimelineEventsValidateAndSortChronologically()
{
    auto editor = makeEditor();
    requireOk(editor.addTimelineEventValue({2.0, "Gain", talas::expr::ValueDomain::Scalar, 0.75}),
              "editor should add a later Timeline Event Value");
    requireOk(editor.addTimelineEventValue({0.5, "Cutoff", talas::expr::ValueDomain::Hertz, 880.0}),
              "editor should add and sort an earlier Timeline Event Value");

    const auto& events = editor.document().timeline.eventValues;
    require(events.size() == 2, "Timeline should retain both Event Values");
    require(events[0].timeSeconds == 0.5 && events[0].eventName == "Cutoff",
            "Timeline should sort Event Values chronologically");
    require(events[1].timeSeconds == 2.0 && events[1].eventName == "Gain",
            "Timeline should retain the later Event Value");

    requireError(editor.addTimelineEventValue({0.5, "Cutoff", talas::expr::ValueDomain::Scalar, 0.25}),
                 talas::project::EditError::InvalidTimeline,
                 "duplicate name and time Timeline Event Value");
    requireError(editor.addTimelineEventValue({-0.25, "Gain", talas::expr::ValueDomain::Scalar, 0.5}),
                 talas::project::EditError::InvalidTimeline,
                 "negative Timeline Event Value time");
    require(editor.document().timeline.eventValues.size() == 2,
            "failed Timeline additions should not mutate the document");
}

void testScoreEditsAreValidatedAtomicAndUndoable()
{
    auto document = talas::project::ProjectDocument{};
    document.soundDefinitions.push_back(makeVoiceDefinition("voiceDefinition"));
    talas::project::ProjectEditor editor{std::move(document), builtinRegistry()};
    const auto ticks = [](std::uint64_t value)
    { return talas::project::ScoreTime{talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value}; };

    talas::project::ProjectScoreEditBatch initial;
    initial.tracks = {
        {"voice",
         "Voice",
         talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::MusicalTicks,
         "master",
         2,
         talas::project::ScoreOverlapMode::Layer},
        {"master",
         "Master",
         talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds,
         {},
         2,
         talas::project::ScoreOverlapMode::Layer},
    };
    initial.clips.push_back({"intro",
                             "voice",
                             "voiceDefinition",
                             ticks(0),
                             ticks(960),
                             ticks(0),
                             ticks(0),
                             ticks(0),
                             1});
    initial.automation.push_back({"gainLane",
                                  talas::project::ScoreAutomationScope::Track,
                                  "voice",
                                  "Gain",
                                  talas::expr::ValueDomain::Scalar,
                                  {{ticks(0), 0.25, talas::core::AutomationInterpolation::Linear}}});
    initial.markers.push_back({"verse", "Verse", ticks(0), ticks(960)});
    requireOk(editor.applyScoreEdits(std::move(initial)),
              "one bulk Score edit should author a valid closed score");
    const auto authored = editor.document().timeline.score;
    require(authored.tracks.size() == 2 && authored.clips.size() == 1 &&
                authored.automation.size() == 1 && authored.markers.size() == 1,
            "bulk Score edit should commit every entity atomically");
    const auto beforeReferencedDefinitionRemoval =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.removeSoundDefinition("voiceDefinition"),
        talas::project::EditError::SoundDefinitionInUse,
        "Score clip should retain its exact Sound-definition authority");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeReferencedDefinitionRemoval,
        "failed Score-referenced Sound removal should leave the document unchanged");
    requireOk(editor.undo(), "one undo should remove the complete bulk Score edit");
    require(editor.document().timeline.score == talas::project::ProjectScore{},
            "bulk Score edit should create exactly one history record");
    requireOk(editor.redo(), "redo should restore the complete bulk Score edit");
    require(editor.document().timeline.score == authored,
            "redo should restore exact Score identity and document order");

    auto voice = editor.document().timeline.score.tracks.front();
    voice.displayName = "Lead";
    requireOk(editor.updateScoreTrack(voice), "Score track should update by exact identity");
    requireOk(editor.addScoreTrack({"music",
                                    "Music",
                                    talas::project::ScoreTrackKind::Submix,
                                    talas::project::ScoreTimeDomain::MusicalTicks,
                                    "master",
                                    2,
                                    talas::project::ScoreOverlapMode::Layer}),
              "Score track should add through the validated editor boundary");
    requireOk(editor.moveScoreTrack("music", 0),
              "Score track should move without changing its stable identity");
    require(editor.document().timeline.score.tracks.front().trackId == "music",
            "Score track move should preserve the requested document order");
    requireOk(editor.removeScoreTrack("music"), "unused Score track should remove");

    requireOk(editor.addScoreClip({"layer",
                                   "voice",
                                   "voiceDefinition",
                                   ticks(960),
                                   ticks(960),
                                   ticks(0),
                                   ticks(0),
                                   ticks(0),
                                   1}),
              "Score clip should add by exact stable identity");
    requireOk(editor.moveScoreClip("layer", 0), "Score clip should move in document order");
    auto layer = editor.document().timeline.score.clips.front();
    layer.repeatCount = 2;
    requireOk(editor.updateScoreClip(layer), "Score clip should update in place");
    // A clip is its Sound's bar in time, which
    // can move and change its duration later.
    layer.start = ticks(480);
    layer.duration = ticks(1920);
    requireOk(editor.updateScoreClip(layer), "a clip should move and change its duration");
    require(editor.document().timeline.score.clips.front().start == ticks(480)
            && editor.document().timeline.score.clips.front().duration == ticks(1920)
            && editor.document().timeline.score.clips.front().soundDefinitionId == layer.soundDefinitionId,
        "the moved clip should keep its Sound at its new start and duration");
    requireOk(editor.removeScoreClip("layer"), "Score clip should remove by exact identity");

    requireOk(
        editor.addScoreAutomationLane({"panLane",
                                       talas::project::ScoreAutomationScope::Track,
                                       "voice",
                                       "Pan",
                                       talas::expr::ValueDomain::Scalar,
                                       {{ticks(0), 0.0, talas::core::AutomationInterpolation::Step}}}),
        "Score automation should add by exact target authority");
    requireOk(editor.moveScoreAutomationLane("panLane", 0),
              "Score automation should move in document order");
    auto pan = editor.document().timeline.score.automation.front();
    pan.points.front().value = 0.5;
    requireOk(editor.updateScoreAutomationLane(pan),
              "Score automation should update its exact lane");
    requireOk(editor.removeScoreAutomationLane("panLane"),
              "Score automation should remove by exact identity");

    requireOk(editor.addScoreMarker({"outro", "Outro", ticks(1920), std::nullopt}),
              "Score marker should add by exact identity");
    requireOk(editor.moveScoreMarker("outro", 0), "Score marker should move in document order");
    auto outro = editor.document().timeline.score.markers.front();
    outro.title = "Finale";
    requireOk(editor.updateScoreMarker(outro), "Score marker should update in place");
    requireOk(editor.removeScoreMarker("outro"), "Score marker should remove");

    auto invalid = editor.document();
    invalid.timeline.score.clips.front().soundDefinitionId = "missingDefinition";
    requireError(editor.replaceDocument(std::move(invalid)),
                 talas::project::EditError::InvalidScore,
                 "document replacement should reject stale Score definition authority");
    require(editor.document().timeline.score.clips.front().soundDefinitionId == "voiceDefinition",
            "failed Score replacement should leave the document unchanged");

    const auto gestureBaseline = talas::project::serializeProjectDocument(
        editor.document());
    auto token = editor.beginOwnedHistoryTransaction();
    auto dragged = editor.document().timeline.score.clips.front();
    dragged.start.tick += 10;
    requireOk(editor.updateScoreClip(dragged, token),
              "the first owned Score gesture update should retain its history owner");
    dragged.start.tick += 10;
    requireOk(editor.updateScoreClip(dragged, token),
              "a repeated owned Score gesture update should remain in one transaction");
    editor.cancelOwnedHistoryTransaction(token);
    require(talas::project::serializeProjectDocument(editor.document()) == gestureBaseline,
            "cancelling an owned Score gesture should restore its exact baseline bytes");

    token = editor.beginOwnedHistoryTransaction();
    dragged = editor.document().timeline.score.clips.front();
    dragged.start.tick += 10;
    requireOk(editor.updateScoreClip(dragged, token),
              "a committed owned Score gesture should accept its first update");
    dragged.start.tick += 10;
    requireOk(editor.updateScoreClip(dragged, token),
              "a committed owned Score gesture should accept its repeated update");
    editor.endOwnedHistoryTransaction(token);
    requireOk(editor.undo(),
              "one undo should consume the complete owned Score gesture");
    require(talas::project::serializeProjectDocument(editor.document()) == gestureBaseline,
            "one undo should restore the exact pre-gesture Score bytes");

    token = editor.beginOwnedHistoryTransaction();
    dragged = editor.document().timeline.score.clips.front();
    dragged.start.tick += 10;
    requireOk(editor.updateScoreClip(dragged, token),
              "a Score gesture should accept its live owner token");
    auto sibling = editor.document().timeline.score.markers.front();
    sibling.title = "Sibling authority change";
    requireOk(editor.updateScoreMarker(std::move(sibling)),
              "an unrelated Score edit should close the owned transaction");
    const auto afterSibling = talas::project::serializeProjectDocument(editor.document());
    dragged.start.tick += 10;
    requireError(
        editor.updateScoreClip(dragged, token),
        talas::project::EditError::InvalidSelection,
        "a stale Score gesture token should fail closed after sibling authority changes");
    require(
        talas::project::serializeProjectDocument(editor.document()) == afterSibling,
        "a rejected stale Score gesture should not overwrite sibling authority");
}

// A Timeline marker goes in at a time typed for
// it, a selected one moves to a precise time typed for it, and Delete takes
// it away; each is one undo step.
void testTimelineMarkersAtTypedTimesMoveAndDelete()
{
    auto document = talas::project::ProjectDocument{};
    document.soundDefinitions.push_back(makeVoiceDefinition("voiceDefinition"));
    talas::project::ProjectEditor editor{std::move(document), builtinRegistry()};
    const auto seconds = [](double value)
    { return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0}; };
    talas::project::ProjectScoreEditBatch tracks;
    tracks.tracks = {{"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 2,
        talas::project::ScoreOverlapMode::Layer}};
    requireOk(editor.applyScoreEdits(std::move(tracks)), "the Timeline should take a master track");
    const auto before = talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.addScoreMarker({"chorus", "Chorus", seconds(2.5), std::nullopt}), "a marker should go in at a typed time");
    require(editor.document().timeline.score.markers.size() == 1
            && editor.document().timeline.score.markers.front().start == seconds(2.5),
        "the marker should be at the time typed for it");
    auto moved = editor.document().timeline.score.markers.front();
    moved.start = seconds(3.25);
    requireOk(editor.updateScoreMarker(moved), "a selected marker should move to a precise time");
    require(editor.document().timeline.score.markers.front().start == seconds(3.25), "the marker should be at its new time");
    requireOk(editor.removeScoreMarker("chorus"), "Delete should take a selected marker away");
    require(editor.document().timeline.score.markers.empty(), "the deleted marker should be gone");
    requireOk(editor.undo(), "undo the deletion");
    requireOk(editor.undo(), "undo the move");
    require(editor.document().timeline.score.markers.front().start == seconds(2.5), "one undo should take back each step");
    requireOk(editor.undo(), "undo the marker");
    require(talas::project::serializeProjectDocument(editor.document()) == before, "three undos should leave the Timeline as it was");
}

void testScoreClipRenderArtifactPublicationIsAtomicAndUndoable()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(makeVoiceDefinition("voiceDefinition"));
    document.soundDefinitions.front().inputs.clear();
    document.timeline.score.tracks = {
        {"voice",
         "Voice",
         talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds,
         "master",
         1,
         talas::project::ScoreOverlapMode::Layer},
        {"master",
         "Master",
         talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds,
         {},
         1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips.push_back({
        "clip",
        "voice",
        "voiceDefinition",
        {talas::project::ScoreTimeDomain::Seconds, 0.0, 0},
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
        {talas::project::ScoreTimeDomain::Seconds, 0.0, 0},
        {talas::project::ScoreTimeDomain::Seconds, 0.0, 0},
        {talas::project::ScoreTimeDomain::Seconds, 0.0, 0},
        1,
    });

    talas::project::ProjectAsset asset;
    asset.id = "score-cache-clip-0";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "score-cache/clip-0.wav";
    asset.disposition = talas::project::ProjectAssetDisposition::Embedded;
    asset.contentHash = std::string(64, 'b');
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = 48000;
    asset.encoding = std::string{talas::project::pcm16WavAssetEncoding};
    asset.provenance = "Score clip materialization test";

    talas::project::ScoreClipRenderArtifact artifact;
    artifact.mode = talas::project::ScoreClipRenderMode::Cached;
    artifact.sampleRate = 48000;
    artifact.frameCount = 48000;
    artifact.channelAssetIds = {asset.id};

    std::string fingerprintError;
    artifact.sourceFingerprint =
        talas::project::projectScoreClipSourceFingerprint(
            document,
            builtinRegistry(),
            "clip",
            artifact.sampleRate,
            fingerprintError);
    require(
        talas::project::isCanonicalProjectAssetContentHash(
            artifact.sourceFingerprint),
        "artifact publication fixture should derive exact source authority: "
            + fingerprintError);

    talas::project::ProjectEditor editor{document, builtinRegistry()};
    talas::project::ScoreClipRenderArtifactPublication publication;
    publication.expectedDocumentAuthorityGeneration =
        editor.documentAuthorityGeneration();
    publication.expectedCanonicalDocumentBytes =
        talas::project::serializeProjectDocument(editor.document());
    publication.clipId = "clip";
    publication.artifact = artifact;
    publication.assets = {asset};

    auto mismatchedPublication = publication;
    mismatchedPublication.artifact.sourceFingerprint = std::string(64, 'a');
    requireError(
        editor.publishScoreClipRenderArtifact(
            std::move(mismatchedPublication)),
        talas::project::EditError::InvalidScore,
        "terminal publication must reject a caller-supplied dependency fingerprint");
    require(editor.document().assets.empty()
                && !editor.document().timeline.score.clips.front()
                        .renderArtifact.has_value()
                && !editor.canUndo(),
            "fingerprint rejection must be document- and history-atomic");

    requireOk(
        editor.publishScoreClipRenderArtifact(publication),
        "Score clip artifact publication should commit atomically");
    require(
        editor.document().assets.size() == 1
            && editor.document().assets.front() == asset
            && editor.document().timeline.score.clips.front().renderArtifact
                == artifact,
        "artifact publication should install every channel asset and the exact record");
    const auto publishedBytes =
        talas::project::serializeProjectDocument(editor.document());
    requireOk(editor.undo(), "artifact publication should be one Undo");
    require(
        editor.document().assets.empty()
            && !editor.document().timeline.score.clips.front().renderArtifact.has_value()
            && !editor.canUndo(),
        "one Undo should remove the complete artifact publication");
    requireOk(editor.redo(), "artifact publication should redo atomically");
    require(
        talas::project::serializeProjectDocument(editor.document()) == publishedBytes,
        "redo should restore exact artifact publication bytes");

    requireOk(
        editor.clearScoreClipRenderArtifact("clip"),
        "clearing a Score clip artifact should succeed");
    require(
        !editor.document().timeline.score.clips.front().renderArtifact.has_value()
            && editor.document().assets.size() == 1,
        "clearing an artifact record should retain its reusable immutable asset");
    requireOk(editor.undo(), "artifact clearing should be one Undo");
    require(
        talas::project::serializeProjectDocument(editor.document()) == publishedBytes,
        "undo should restore the exact artifact record");

    talas::project::ProjectEditor staleEditor{document, builtinRegistry()};
    auto stalePublication = publication;
    requireOk(
        staleEditor.addScoreMarker({
            "authority-change",
            "Authority change",
            {talas::project::ScoreTimeDomain::Seconds, 0.25, 0},
            std::nullopt,
        }),
        "stale-publication fixture should advance document authority");
    const auto afterAuthorityChange =
        talas::project::serializeProjectDocument(staleEditor.document());
    requireError(
        staleEditor.publishScoreClipRenderArtifact(std::move(stalePublication)),
        talas::project::EditError::InvalidScore,
        "artifact publication should reject stale canonical document authority");
    require(
        talas::project::serializeProjectDocument(staleEditor.document())
            == afterAuthorityChange,
        "stale artifact publication should not mutate document or assets");
    requireOk(staleEditor.undo(), "only the authority-changing edit should remain undoable");
    require(!staleEditor.canUndo(),
            "rejected stale publication should not create history");

    auto collisionDocument = document;
    auto collisionAsset = asset;
    collisionAsset.contentHash = std::string(64, 'c');
    collisionDocument.assets.push_back(std::move(collisionAsset));
    talas::project::ProjectEditor collisionEditor{
        std::move(collisionDocument), builtinRegistry()};
    auto collisionPublication = publication;
    collisionPublication.expectedDocumentAuthorityGeneration =
        collisionEditor.documentAuthorityGeneration();
    collisionPublication.expectedCanonicalDocumentBytes =
        talas::project::serializeProjectDocument(collisionEditor.document());
    const auto beforeCollision =
        talas::project::serializeProjectDocument(collisionEditor.document());
    requireError(
        collisionEditor.publishScoreClipRenderArtifact(
            std::move(collisionPublication)),
        talas::project::EditError::InvalidAsset,
        "artifact publication should reject a conflicting asset identity");
    require(
        talas::project::serializeProjectDocument(collisionEditor.document())
                == beforeCollision
            && !collisionEditor.canUndo(),
        "asset collision rejection should be failure-atomic and history-free");

    talas::project::ProjectEditor abaEditor{document, builtinRegistry()};
    auto abaPublication = publication;
    abaPublication.expectedDocumentAuthorityGeneration =
        abaEditor.documentAuthorityGeneration();
    requireOk(
        abaEditor.addScoreMarker({
            "aba-change",
            "ABA authority change",
            {talas::project::ScoreTimeDomain::Seconds, 0.5, 0},
            std::nullopt,
        }),
        "ABA fixture should advance authority");
    requireOk(abaEditor.undo(), "ABA fixture should restore identical bytes");
    require(
        talas::project::serializeProjectDocument(abaEditor.document())
            == abaPublication.expectedCanonicalDocumentBytes,
        "ABA fixture must restore exact canonical bytes");
    requireError(
        abaEditor.publishScoreClipRenderArtifact(std::move(abaPublication)),
        talas::project::EditError::InvalidScore,
        "artifact publication must reject edit-undo ABA despite identical bytes");
    require(!abaEditor.canUndo(),
            "rejected ABA publication must not create history");
}

void testTimelineEventsUpdateRemoveAndParticipateInHistory()
{
    auto editor = makeEditor();
    requireOk(editor.addTimelineEventValue({1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25}),
              "editor should add first Timeline Event Value");
    requireOk(editor.addTimelineEventValue({3.0, "Cutoff", talas::expr::ValueDomain::Hertz, 440.0}),
              "editor should add second Timeline Event Value");

    requireOk(editor.updateTimelineEventValue(
                  1, {0.25, "Cutoff", talas::expr::ValueDomain::Hertz, 660.0,
                      talas::core::AutomationInterpolation::Linear}),
              "editor should update and reorder a Timeline Event Value");
    const auto& updatedEvents = editor.document().timeline.eventValues;
    require(updatedEvents[0].eventName == "Cutoff" && updatedEvents[0].timeSeconds == 0.25
                && updatedEvents[0].value == 660.0
                && updatedEvents[0].interpolation
                    == talas::core::AutomationInterpolation::Linear,
            "Timeline update should retain values and restore chronological order");
    require(updatedEvents[1].eventName == "Gain", "Timeline update should retain the other event");

    requireError(editor.updateTimelineEventValue(
                     0, {1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.5}),
                 talas::project::EditError::InvalidTimeline,
                 "updating to a duplicate Timeline name and time");
    requireError(editor.updateTimelineEventValue(
                     4, {2.0, "Missing", talas::expr::ValueDomain::Scalar, 0.0}),
                 talas::project::EditError::UnknownTimelineEvent,
                 "updating an unknown Timeline Event Value");
    requireError(editor.removeTimelineEventValue(4),
                 talas::project::EditError::UnknownTimelineEvent,
                 "removing an unknown Timeline Event Value");
    require(editor.document().timeline.eventValues.size() == 2,
            "unknown Timeline indices should not mutate the document");

    requireOk(editor.removeTimelineEventValue(1), "editor should remove a Timeline Event Value");
    require(editor.document().timeline.eventValues.size() == 1
                && editor.document().timeline.eventValues.front().eventName == "Cutoff",
            "Timeline removal should remove only the selected event");
    requireOk(editor.undo(), "undo should restore a removed Timeline Event Value");
    require(editor.document().timeline.eventValues.size() == 2,
            "undo should restore the removed Timeline Event Value");
    requireOk(editor.undo(), "undo should restore a Timeline update");
    require(editor.document().timeline.eventValues[0].eventName == "Gain"
                && editor.document().timeline.eventValues[1].eventName == "Cutoff"
                && editor.document().timeline.eventValues[1].timeSeconds == 3.0,
            "undo should restore the Timeline order before update");
    requireOk(editor.redo(), "redo should restore the Timeline update");
    require(editor.document().timeline.eventValues.front().eventName == "Cutoff"
                && editor.document().timeline.eventValues.front().interpolation
                    == talas::core::AutomationInterpolation::Linear,
            "redo should restore the reordered Timeline Event Value and interpolation");

    auto invalidInterpolation = editor.document().timeline.eventValues.front();
    invalidInterpolation.interpolation =
        static_cast<talas::core::AutomationInterpolation>(99);
    requireError(
        editor.updateTimelineEventValue(0, invalidInterpolation),
        talas::project::EditError::InvalidTimeline,
        "unknown Timeline interpolation");

    auto noOpEditor = makeEditor();
    const talas::project::TimelineEventValue unchanged{1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25};
    requireOk(noOpEditor.addTimelineEventValue(unchanged), "editor should add a no-op history fixture");
    requireOk(noOpEditor.updateTimelineEventValue(0, unchanged), "unchanged Timeline update should succeed");
    requireOk(noOpEditor.undo(), "one undo should cross an unchanged Timeline update");
    require(noOpEditor.document().timeline.eventValues.empty(),
            "unchanged Timeline update should not add a history entry");

    auto dragEditor = makeEditor();
    requireOk(
        dragEditor.addTimelineEventValue(
            {1.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25}),
        "drag transaction fixture should add a Timeline event");
    dragEditor.beginHistoryTransaction();
    requireOk(
        dragEditor.updateTimelineEventValue(
            0,
            {1.5, "Gain", talas::expr::ValueDomain::Scalar, 0.25}),
        "drag transaction should accept its first marker position");
    requireOk(
        dragEditor.updateTimelineEventValue(
            0,
            {2.0, "Gain", talas::expr::ValueDomain::Scalar, 0.25}),
        "drag transaction should accept its final marker position");
    dragEditor.endHistoryTransaction();
    require(
        dragEditor.document().timeline.eventValues.front().timeSeconds == 2.0,
        "drag transaction should retain the final marker position");
    requireOk(
        dragEditor.undo(),
        "one undo should revert the complete Timeline marker drag");
    require(
        dragEditor.document().timeline.eventValues.front().timeSeconds == 1.0,
        "Timeline marker drag undo should restore the initial position");
}

void testTimelineLoopIsValidatedAndUndoable()
{
    auto editor = makeEditor();
    const talas::project::TimelineLoopRegion loop{true, 1.25, 3.75};
    requireOk(editor.setTimelineLoop(loop), "editor should set a valid Timeline loop");
    require(editor.document().timeline.loop == loop,
            "valid Timeline loop should commit");

    requireOk(editor.setTimelineLoop(loop),
              "reapplying the identical Timeline loop should be a no-op");
    requireOk(editor.undo(), "one undo should revert the Timeline loop edit");
    require(editor.document().timeline.loop
                == talas::project::TimelineLoopRegion{false, 0.0, 4.0},
            "undo should restore the default Timeline loop");
    requireOk(editor.redo(), "redo should restore the Timeline loop edit");
    require(editor.document().timeline.loop == loop,
            "redo should restore the complete Timeline loop");

    for (const auto invalid : {
             talas::project::TimelineLoopRegion{true, -1.0, 2.0},
             talas::project::TimelineLoopRegion{true, 2.0, 2.0},
             talas::project::TimelineLoopRegion{true, 3.0, 2.0},
             talas::project::TimelineLoopRegion{
                 true,
                 0.0,
                 std::numeric_limits<double>::infinity()},
         })
    {
        requireError(editor.setTimelineLoop(invalid),
                     talas::project::EditError::InvalidTimeline,
                     "invalid Timeline loop");
        require(editor.document().timeline.loop == loop,
                "failed Timeline loop edit must not mutate the document");
    }
}

// The file a DiskSplicer's disk button splices
// in, held by the project though no Resource parameter names it.
void testAddingAnAssetNoParameterNames()
{
    auto editor = makeEditor();
    const talas::project::ProjectAsset asset{"take.wav", talas::project::ProjectAssetKind::Table, "assets/take.wav"};
    requireOk(editor.addAsset(asset), "editor should add a project asset");
    require(editor.document().assets == std::vector<talas::project::ProjectAsset>{asset}, "the asset should be held");
    requireOk(editor.addAsset(asset), "the same asset again should change nothing");
    auto moved = asset;
    moved.path = "elsewhere/take.wav";
    requireError(editor.addAsset(moved), talas::project::EditError::InvalidAsset, "another asset of the same id");
    requireOk(editor.undo(), "one undo should remove the asset");
    require(editor.document().assets.empty(), "the asset should be one history entry");
}

void testResourceAssetBindingIsAtomicValidatedAndUndoable()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"sample", "talas.sample", {}, {}}),
              "editor should add a resource-backed Sample node");
    const talas::project::ProjectAsset asset{
        "Samples/Hit",
        talas::project::ProjectAssetKind::Table,
        "assets/hit.wav",
    };
    requireOk(editor.setResourceParameterAsset("sample", "sample", asset),
              "editor should bind a project asset to a Resource parameter");
    require(editor.document().assets.size() == 1
                && editor.document().assets.front().id == "Samples/Hit",
            "resource binding should register the project asset");
    require(editor.document().graph.nodes.front().parameters.size() == 1
                && editor.document().graph.nodes.front().parameters.front().resource
                    == std::optional<std::string>{"Samples/Hit"},
            "resource binding should update the node parameter");

    requireOk(editor.undo(), "one undo should revert resource binding and asset registration");
    require(editor.document().assets.empty()
                && editor.document().graph.nodes.front().parameters.empty(),
            "resource binding should be one atomic history entry");
    requireOk(editor.redo(), "redo should restore resource binding and asset registration");
    require(editor.document().assets.size() == 1
                && editor.document().graph.nodes.front().parameters.front().resource
                    == std::optional<std::string>{"Samples/Hit"},
            "redo should restore the complete resource binding transaction");
    requireOk(editor.setResourceParameterAsset("sample", "sample", asset),
              "unchanged resource binding should succeed");
    requireOk(editor.undo(), "one undo should cross an unchanged resource binding");
    require(editor.document().assets.empty()
                && editor.document().graph.nodes.front().parameters.empty(),
            "unchanged resource binding should not create a history entry");
    requireOk(editor.redo(), "redo should restore resource binding after no-op history proof");

    requireError(editor.setResourceParameterAsset("sample", "missing", asset),
                 talas::project::EditError::UnknownParameter,
                 "binding an asset to an unknown parameter");
    requireError(editor.setResourceParameterAsset("sample", "frequency", asset),
                 talas::project::EditError::InvalidParameterValue,
                 "binding an asset to a non-Resource parameter");
    auto invalidAsset = asset;
    invalidAsset.id.clear();
    requireError(editor.setResourceParameterAsset("sample", "sample", invalidAsset),
                 talas::project::EditError::InvalidAsset,
                 "binding an invalid project asset");

    auto invalidDocument = editor.document();
    invalidDocument.assets.push_back(asset);
    requireError(editor.replaceDocument(std::move(invalidDocument)),
                 talas::project::EditError::DuplicateAsset,
                 "loading duplicate project asset ids");

    auto soundEditor = makeEditor();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "sampleVoice";
    definition.displayName = "Sample Voice";
    definition.graph.nodes.push_back({
        "player",
        "talas.sample",
        {},
        {},
    });
    talas::project::ProjectSoundExposedParameter exposedSample;
    exposedSample.parameterId = "sample";
    exposedSample.displayName = "Sample";
    exposedSample.kind = talas::core::ParameterKind::Resource;
    exposedSample.defaultValue = resourceParameter("sample", "");
    exposedSample.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "player",
        "sample",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(
        std::move(exposedSample));
    requireOk(
        soundEditor.addSoundDefinition(std::move(definition)),
        "editor should accept a Resource-kind Sound interface");
    requireOk(
        soundEditor.addSoundInstance(
            "sampleVoice1",
            "sampleVoice",
            {}),
        "editor should instantiate a Resource-kind Sound interface");
    requireOk(
        soundEditor.setResourceParameterAsset(
            "sampleVoice1",
            "sample",
            asset),
        "asset binding should resolve Resource-kind exposed Sound parameters");
    require(
        soundEditor.document().assets.size() == 1
            && soundEditor.document().graph.nodes.front()
                   .parameters
                == std::vector<talas::project::ProjectParameterExpression>{
                    resourceParameter("sample", "Samples/Hit")},
        "asset binding should persist one sparse Resource-kind Sound override");
}

void testAssetRelocationAndReplacementPreserveGenerationRules()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"sample", "talas.sample", {}, {}}),
              "editor should add a resource-backed node");
    talas::project::ProjectAsset original;
    original.id = "Samples/Hit";
    original.kind = talas::project::ProjectAssetKind::Table;
    original.path = "assets/hit.txt";
    original.contentHash.assign(64, 'a');
    original.generation = 3;
    original.provenance = "imported";
    requireOk(
        editor.setResourceParameterAsset("sample", "sample", original),
        "editor should bind the verified baseline generation");

    auto relocated = original;
    relocated.path = "moved/hit.txt";
    requireOk(
        editor.relocateProjectAsset(relocated),
        "same verified content should relocate without a new generation");
    require(
        editor.document().assets.front().path == "moved/hit.txt"
            && editor.document().assets.front().generation == 3,
        "relocation should change only path and preserve generation");
    requireOk(editor.undo(), "asset relocation should be undoable");
    require(
        editor.document().assets.front() == original,
        "undo should restore the complete pre-relocation manifest");
    requireOk(editor.redo(), "asset relocation should be redoable");

    auto metadataMutation = relocated;
    metadataMutation.provenance = "silently changed";
    requireError(
        editor.relocateProjectAsset(metadataMutation),
        talas::project::EditError::InvalidAsset,
        "relocation that changes non-path metadata");

    auto replacement = relocated;
    replacement.path = "replacement/hit.txt";
    replacement.contentHash.assign(64, 'b');
    replacement.generation = 999;
    replacement.provenance = "accepted replacement";
    requireOk(
        editor.acceptProjectAssetReplacement(replacement),
        "different verified bytes should be accepted as a new generation");
    require(
        editor.document().assets.front().path
                == "replacement/hit.txt"
            && editor.document().assets.front().contentHash
                == std::string(64, 'b')
            && editor.document().assets.front().generation == 4,
        "replacement should derive the next generation instead of trusting caller input");
    requireOk(editor.undo(), "accepted replacement should be undoable");
    require(
        editor.document().assets.front() == relocated,
        "undo should restore the active same-content relocation generation");

    auto sameContentReplacement = relocated;
    sameContentReplacement.path = "another/hit.txt";
    requireError(
        editor.acceptProjectAssetReplacement(sameContentReplacement),
        talas::project::EditError::InvalidAsset,
        "same-content replacement");
    auto unverifiedRelocation = relocated;
    unverifiedRelocation.contentHash.clear();
    requireError(
        editor.relocateProjectAsset(unverifiedRelocation),
        talas::project::EditError::InvalidAsset,
        "unverified relocation");
}

void testAssetResolutionBatchIsFailureAtomicAndOneUndo()
{
    talas::project::ProjectAsset first;
    first.id = "Samples/First";
    first.kind = talas::project::ProjectAssetKind::Binary;
    first.path = "missing/first.bin";
    first.contentHash.assign(64, 'a');
    first.generation = 2;
    first.provenance = "first";

    auto second = first;
    second.id = "Samples/Second";
    second.path = "missing/second.bin";
    second.contentHash.assign(64, 'b');
    second.generation = 5;
    second.provenance = "second";

    talas::project::ProjectDocument document;
    document.assets = {first, second};
    talas::project::ProjectEditor editor(document, builtinRegistry());

    auto relocated = first;
    relocated.path = "found/first.bin";
    auto replacement = second;
    replacement.path = "found/second.bin";
    replacement.contentHash.assign(64, 'c');
    replacement.generation = 999;
    replacement.provenance = "replacement";

    requireOk(
        editor.applyProjectAssetResolutions({
            {
                talas::project::ProjectAssetResolutionMode::Relocation,
                first.generation,
                first.contentHash,
                relocated,
            },
            {
                talas::project::ProjectAssetResolutionMode::Replacement,
                second.generation,
                second.contentHash,
                replacement,
            },
        }),
        "one batch should apply exact relocation and explicit replacement");
    require(
        editor.document().assets[0] == relocated
            && editor.document().assets[1].path == "found/second.bin"
            && editor.document().assets[1].contentHash
                == std::string(64, 'c')
            && editor.document().assets[1].generation == 6,
        "batch resolution should preserve relocation identity and derive replacement generation");
    requireOk(editor.undo(), "one undo should revert the complete asset batch");
    require(
        editor.document().assets == document.assets
            && !editor.canUndo(),
        "batch resolution should create exactly one history entry");
    requireOk(editor.redo(), "one redo should restore the complete asset batch");

    const auto beforeFailure = editor.document();
    auto stale = relocated;
    stale.path = "other/first.bin";
    requireError(
        editor.applyProjectAssetResolutions({{
            talas::project::ProjectAssetResolutionMode::Relocation,
            1,
            first.contentHash,
            stale,
        }}),
        talas::project::EditError::InvalidAsset,
        "stale resolution batch");
    require(
        editor.document().assets == beforeFailure.assets,
        "a stale batch should leave every asset unchanged");

    requireError(
        editor.applyProjectAssetResolutions({
            {
                talas::project::ProjectAssetResolutionMode::Relocation,
                beforeFailure.assets[0].generation,
                beforeFailure.assets[0].contentHash,
                beforeFailure.assets[0],
            },
            {
                talas::project::ProjectAssetResolutionMode::Relocation,
                beforeFailure.assets[0].generation,
                beforeFailure.assets[0].contentHash,
                beforeFailure.assets[0],
            },
        }),
        talas::project::EditError::DuplicateAsset,
        "duplicate asset in one resolution batch");
    require(
        editor.document().assets == beforeFailure.assets,
        "a duplicate-id batch should remain failure-atomic");

    talas::project::ProjectAsset unverified;
    unverified.id = "Samples/Legacy";
    unverified.kind = talas::project::ProjectAssetKind::Binary;
    unverified.path = "missing/legacy.bin";
    unverified.generation = 1;
    talas::project::ProjectDocument legacyDocument;
    legacyDocument.assets.push_back(unverified);
    talas::project::ProjectEditor legacyEditor(
        legacyDocument,
        builtinRegistry());
    auto inspected = unverified;
    inspected.path = "found/legacy.bin";
    inspected.contentHash.assign(64, 'd');
    requireOk(
        legacyEditor.applyProjectAssetResolutions({{
            talas::project::ProjectAssetResolutionMode::InitialInspection,
            1,
            {},
            inspected,
        }}),
        "batch initial inspection should combine first hash and relocation");
    require(
        legacyEditor.document().assets.front() == inspected
            && legacyEditor.document().assets.front().generation == 1,
        "initial batch inspection should preserve generation and fill only path/hash");
    requireOk(
        legacyEditor.undo(),
        "initial inspection and relocation should be one undo entry");
    require(
        legacyEditor.document().assets == legacyDocument.assets
            && !legacyEditor.canUndo(),
        "one undo should restore the complete unverified manifest");

    talas::project::ProjectEditor noOpEditor(document, builtinRegistry());
    requireOk(
        noOpEditor.applyProjectAssetResolutions({{
            talas::project::ProjectAssetResolutionMode::Relocation,
            first.generation,
            first.contentHash,
            first,
        }}),
        "unchanged exact relocation should be a successful no-op");
    require(
        !noOpEditor.canUndo(),
        "no-op resolution should not create history");

    auto maximumGeneration = second;
    maximumGeneration.generation =
        talas::project::maximumProjectAssetJsonInteger;
    talas::project::ProjectDocument maximumDocument;
    maximumDocument.assets.push_back(maximumGeneration);
    talas::project::ProjectEditor maximumEditor(
        maximumDocument,
        builtinRegistry());
    auto overflow = maximumGeneration;
    overflow.contentHash.assign(64, 'e');
    requireError(
        maximumEditor.applyProjectAssetResolutions({{
            talas::project::ProjectAssetResolutionMode::Replacement,
            maximumGeneration.generation,
            maximumGeneration.contentHash,
            overflow,
        }}),
        talas::project::EditError::InvalidAsset,
        "replacement generation overflow");
    require(
        maximumEditor.document().assets == maximumDocument.assets
            && !maximumEditor.canUndo(),
        "generation overflow should preserve the complete document and history");
}

void testAcceptedAssetInspectionPreservesMigratedGeneration()
{
    const auto migrated = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 9,
  "metadata": { "title": "Migrated asset" },
  "virtualControls": [],
  "assets": [
    { "id": "Samples/Legacy", "kind": "table", "path": "assets/legacy.txt" }
  ],
  "timeline": {
    "loop": { "enabled": false, "startSeconds": 0, "endSeconds": 4 },
    "eventValues": []
  },
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
    "outputChannels": [],
    "midiInputDevices": []
  },
  "soundDefinitions": [],
  "graph": { "nodes": [], "connections": [] }
})json");
    require(migrated.ok(), "format-9 fixture should migrate");
    require(
        migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.assets.size() == 1
            && migrated.document.assets.front().contentHash.empty()
            && migrated.document.assets.front().generation == 1,
        "migrated asset should begin as an unverified generation-one manifest");

    talas::project::ProjectEditor editor{
        migrated.document,
        builtinRegistry(),
    };
    auto inspected = editor.document().assets.front();
    inspected.contentHash.assign(64, 'd');

    auto changedPath = inspected;
    changedPath.path = "assets/other.txt";
    requireError(
        editor.acceptProjectAssetInspection(changedPath),
        talas::project::EditError::InvalidAsset,
        "inspection that changes a migrated asset path");
    auto changedProvenance = inspected;
    changedProvenance.provenance = "invented";
    requireError(
        editor.acceptProjectAssetInspection(changedProvenance),
        talas::project::EditError::InvalidAsset,
        "inspection that changes migrated provenance");
    auto changedGeneration = inspected;
    changedGeneration.generation = 2;
    requireError(
        editor.acceptProjectAssetInspection(changedGeneration),
        talas::project::EditError::InvalidAsset,
        "inspection that changes migrated generation");

    requireOk(
        editor.acceptProjectAssetInspection(inspected),
        "verified inspection should fill a migrated asset hash");
    require(
        editor.document().assets.front().contentHash
                == std::string(64, 'd')
            && editor.document().assets.front().generation == 1,
        "first verification should preserve migrated generation one");
    requireOk(
        editor.acceptProjectAssetInspection(inspected),
        "re-accepting the exact verified manifest should be a no-op");
    requireOk(editor.undo(), "accepted inspection should be undoable");
    require(
        editor.document().assets.front().contentHash.empty()
            && editor.document().assets.front().generation == 1,
        "undo should restore the unverified migrated manifest without changing generation");
    requireOk(editor.redo(), "accepted inspection should be redoable");

    talas::project::ProjectDocument audioDocument;
    talas::project::ProjectAsset audio;
    audio.id = "Samples/Audio";
    audio.kind = talas::project::ProjectAssetKind::Audio;
    audio.path = "assets/audio.wav";
    audioDocument.assets.push_back(audio);
    talas::project::ProjectEditor audioEditor{
        std::move(audioDocument),
        builtinRegistry(),
    };
    auto incompleteAudio = audio;
    incompleteAudio.contentHash.assign(64, 'e');
    incompleteAudio.channelCount = 2;
    requireError(
        audioEditor.acceptProjectAssetInspection(incompleteAudio),
        talas::project::EditError::InvalidAsset,
        "audio inspection with an incomplete media shape");
}

void testRuntimeAssetIdentityReconciliationRemainsMonotonic()
{
    auto editor = makeEditor();
    requireOk(
        editor.addNode({"sample", "talas.sample", {}, {}}),
        "runtime reconciliation fixture should add a resource node");
    talas::project::ProjectAsset generationOne;
    generationOne.id = "Samples/Hit";
    generationOne.kind =
        talas::project::ProjectAssetKind::Table;
    generationOne.path = "assets/hit.txt";
    generationOne.contentHash.assign(64, 'a');
    generationOne.generation = 1;
    requireOk(
        editor.setResourceParameterAsset(
            "sample",
            "sample",
            generationOne),
        "runtime reconciliation fixture should bind generation one");

    auto restoredOlderBytes = generationOne;
    requireOk(
        editor.reconcileProjectAssetRuntimeIdentity(
            restoredOlderBytes,
            2,
            std::string(64, 'b')),
        "restoring bytes older than the runtime ledger should mint a new generation");
    require(
        editor.document().assets.front().generation == 3
            && editor.document().assets.front().contentHash
                == std::string(64, 'a'),
        "runtime reconciliation should derive generation three from accepted generation two");

    requireOk(
        editor.undo(),
        "runtime reconciliation should remain an undoable document edit");
    auto restoredAcceptedBytes = generationOne;
    restoredAcceptedBytes.contentHash.assign(64, 'b');
    requireOk(
        editor.reconcileProjectAssetRuntimeIdentity(
            restoredAcceptedBytes,
            2,
            std::string(64, 'b')),
        "bytes matching the staged ledger should restore its accepted identity");
    require(
        editor.document().assets.front().generation == 2
            && editor.document().assets.front().contentHash
                == std::string(64, 'b'),
        "matching runtime bytes should not consume another generation");

    auto aheadOfLedger =
        editor.document().assets.front();
    aheadOfLedger.contentHash.assign(64, 'a');
    requireOk(
        editor.acceptProjectAssetReplacement(
            aheadOfLedger),
        "document should be able to accept generation three before it is staged");
    require(
        editor.document().assets.front().generation == 3,
        "unstaged document replacement should derive generation three");
    requireOk(
        editor.reconcileProjectAssetRuntimeIdentity(
            editor.document().assets.front(),
            2,
            std::string(64, 'b')),
        "bytes matching a valid document identity ahead of the ledger should remain generation three");
    require(
        editor.document().assets.front().generation == 3,
        "metadata-only reconciliation must not consume an ahead generation");

    auto changedAheadBytes =
        editor.document().assets.front();
    changedAheadBytes.contentHash.assign(64, 'c');
    requireOk(
        editor.reconcileProjectAssetRuntimeIdentity(
            changedAheadBytes,
            2,
            std::string(64, 'b')),
        "changed bytes should advance beyond both document and runtime histories");
    require(
        editor.document().assets.front().generation == 4
            && editor.document().assets.front().contentHash
                == std::string(64, 'c'),
        "ahead-ledger reconciliation should mint generation four");

    talas::project::ProjectDocument invalidAheadDocument;
    auto invalidAhead = generationOne;
    invalidAhead.generation = 3;
    invalidAhead.contentHash.assign(64, 'b');
    invalidAheadDocument.assets.push_back(
        invalidAhead);
    talas::project::ProjectEditor invalidAheadEditor{
        std::move(invalidAheadDocument),
        builtinRegistry(),
    };
    requireOk(
        invalidAheadEditor.reconcileProjectAssetRuntimeIdentity(
            invalidAhead,
            2,
            std::string(64, 'b')),
        "higher generation with unchanged accepted bytes should normalize to the exact ledger identity");
    require(
        invalidAheadEditor.document().assets.front().generation == 2
            && invalidAheadEditor.document().assets.front().contentHash
                == std::string(64, 'b'),
        "invalid higher same-content identity should not loop or consume another generation");
}

void testAssetPathRebasingIsValidatedAndPreservesHistory()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"sample", "talas.sample", {}, {}}),
              "editor should add a resource-backed Sample node");
    talas::project::ProjectAsset asset{
        "Shape",
        talas::project::ProjectAssetKind::Table,
        "/audio/tables/shape.txt",
    };
    asset.contentHash.assign(64, 'c');
    asset.generation = 4;
    asset.provenance = "verified import";
    requireOk(editor.setResourceParameterAsset(
                  "sample",
                  "sample",
                  asset),
              "editor should bind an absolute asset before first save");

    requireOk(editor.rebaseAssetPathsForProjectMove(
                  std::nullopt,
                  std::filesystem::path("/projects/session/sound.talas.json")),
              "first save should rebase absolute asset paths");
    require(editor.document().assets.front().path == "../../audio/tables/shape.txt",
            "first save should store the asset relative to the project directory");
    require(
        editor.document().assets.front().contentHash
                == std::string(64, 'c')
            && editor.document().assets.front().generation == 4
            && editor.document().assets.front().provenance
                == "verified import",
        "path rebasing should preserve accepted hash, generation, and provenance");
    requireOk(editor.undo(), "rebasing should preserve undo history");
    require(editor.document().assets.empty(),
            "undo should still remove the asset after path rebasing");
    requireOk(editor.redo(), "rebasing should preserve redo history");
    require(editor.document().assets.front().path == "../../audio/tables/shape.txt",
            "redo history should be rebased with the current document");

    const auto beforeNoOp = editor.document().assets.front().path;
    requireOk(editor.rebaseAssetPathsForProjectMove(
                  std::filesystem::path("/projects/session/sound.talas.json"),
                  std::filesystem::path("/projects/session/sound.talas.json")),
              "saving to the same project path should be a no-op");
    require(editor.document().assets.front().path == beforeNoOp,
            "same-path save should retain the canonical relative asset path");

    requireError(editor.rebaseAssetPathsForProjectMove(
                     std::filesystem::path("/projects/session/sound.talas.json"),
                     {}),
                 talas::project::EditError::InvalidAsset,
                 "rebasing to an empty project path");
    require(editor.document().assets.front().path == beforeNoOp,
            "failed asset path rebasing should not mutate the document");
}

void testEventValueControlsSynchronizeAtomicallyWithGraphEdits()
{
    auto editor = makeEditor();
    requireOk(editor.addNode({"osc", "talas.oscillator", {}, {}}),
              "event discovery fixture should add oscillator");

    const auto updated = editor.setParameter(
        "osc",
        expressionParameter("frequency", "!Frequency"));
    requireOk(updated, "parameter Event Value should synchronize");
    require(updated.message.find("generated 1 Event Value control")
                != std::string::npos,
            "successful synchronization should report its generated control");
    require(editor.document().virtualControls.size() == 1
                && editor.document().virtualControls.front().eventName
                    == "Frequency"
                && editor.document().virtualControls.front().domain
                    == talas::expr::ValueDomain::Hertz,
            "parameter edit should atomically add a hertz control");

    requireOk(editor.undo(),
              "one undo should revert parameter and generated control together");
    require(editor.document().graph.nodes.front().parameters.empty()
                && editor.document().virtualControls.empty(),
            "generated control must share the parameter history boundary");
    requireOk(editor.redo(),
              "one redo should restore parameter and generated control together");
    require(editor.document().graph.nodes.front().parameters.size() == 1
                && editor.document().virtualControls.size() == 1,
            "redo should restore the complete synchronized edit");

    requireError(
        editor.removeVirtualControl("Frequency"),
        talas::project::EditError::InvalidVirtualControl,
        "removing a graph-referenced generated control");
    requireOk(editor.setParameter(
                  "osc",
                  expressionParameter("frequency", "440 hz")),
              "removing the graph reference should succeed");
    require(editor.document().virtualControls.size() == 1,
            "unused generated controls should not be deleted automatically");
    requireOk(editor.removeVirtualControl("Frequency"),
              "unreferenced generated control should be removable");
    require(editor.document().virtualControls.empty(),
            "explicit removal should delete an unused control");

    auto withNodeControl = makeEditor();
    const auto added = withNodeControl.addNode({
        "osc",
        "talas.oscillator",
        {expressionParameter("amplitude", "!Gain")},
        {},
    });
    requireOk(added, "Add Node should synchronize expression references");
    require(withNodeControl.document().virtualControls.size() == 1
                && withNodeControl.document().virtualControls.front().eventName
                    == "Gain",
            "Add Node should generate a missing control in the same candidate");
    requireOk(withNodeControl.undo(),
              "undo should remove synchronized node and control together");
    require(withNodeControl.document().graph.nodes.empty()
                && withNodeControl.document().virtualControls.empty(),
            "Add Node synchronization should be one history step");

    auto replacementEditor = makeEditor();
    talas::project::ProjectDocument replacement;
    replacement.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {expressionParameter("frequency", "!Sweep")},
        {},
    });
    const auto replaced = replacementEditor.replaceDocument(
        std::move(replacement));
    requireOk(replaced, "Replace Document should synchronize missing controls");
    require(replacementEditor.document().virtualControls.size() == 1
                && replacementEditor.document().virtualControls.front().eventName
                    == "Sweep"
                && replacementEditor.document().virtualControls.front().domain
                    == talas::expr::ValueDomain::Hertz,
            "loaded legacy document should gain its inferred control");
    require(!replacementEditor.canUndo(),
            "loaded synchronization should establish one clean replacement boundary");
}

void testLoadedFormatElevenMoveSynchronizesPortablePresentation()
{
    talas::project::ProjectDocument source;
    source.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {},
    });
    talas::project::reconcileProjectPresentation(source);
    talas::project::findProjectNodePresentation(
        source,
        std::nullopt,
        "osc")->position = {32, 64};
    const auto loaded = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(source));
    require(loaded.ok(), "format-eleven editor fixture should reopen");

    talas::project::ProjectEditor editor{loaded.document, builtinRegistry()};
    requireOk(
        editor.moveNode("osc", {480, -96}),
        "moving a loaded format-eleven node should succeed");
    require(
        editor.document().presentation.structureLayouts.size() == 1
            && editor.document().presentation.structureLayouts.front().nodes
                == std::vector<talas::project::ProjectNodePresentation>{
                    {"osc", {480, -96}}},
        "editor commit should synchronize portable presentation before publication");

    const auto canonical = talas::project::serializeProjectDocument(
        editor.document());
    require(
        canonical.find(R"json("nodeId": "osc",
            "x": 480,
            "y": -96)json") != std::string::npos,
        "saved format-eleven presentation should retain the edited position");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && nodePosition(reopened.document, "osc")
                == talas::project::CanvasPosition{480, -96},
        "edited portable position should survive save and reopen");
}

void testSnapshotLibraryCommandsAreAtomicSingleUndoEdits()
{
    auto editor = makeEditor();
    talas::project::ProjectSnapshot initial;
    initial.snapshotId = "initial";
    initial.displayName = "Initial";
    talas::project::ProjectSnapshotBank firstBank;
    firstBank.bankId = "main-bank";
    firstBank.displayName = "Main Bank";
    firstBank.snapshots.push_back(initial);

    requireOk(
        editor.addSnapshotBank(firstBank),
        "adding a populated snapshot bank should succeed");
    require(
        editor.document().snapshotLibrary.banks ==
            std::vector<talas::project::ProjectSnapshotBank>{firstBank},
        "snapshot bank command should publish the complete candidate");
    requireOk(editor.undo(), "one undo should remove the complete bank");
    require(
        editor.document().snapshotLibrary.banks.empty(),
        "bank creation should be one history entry");
    requireOk(editor.redo(), "one redo should restore the complete bank");

    const auto beforeRename = editor.document();
    requireOk(
        editor.renameSnapshot("main-bank", "initial", "Opening"),
        "snapshot rename should succeed");
    requireOk(editor.undo(), "one undo should revert snapshot rename");
    require(
        editor.document().snapshotLibrary == beforeRename.snapshotLibrary,
        "snapshot rename should create exactly one history entry");
    requireOk(editor.redo(), "one redo should restore snapshot rename");

    const auto beforeClone = editor.document();
    requireOk(
        editor.cloneSnapshot(
            "main-bank",
            "initial",
            "alternate",
            "Alternate"),
        "snapshot clone should succeed");
    requireOk(editor.undo(), "one undo should remove the cloned snapshot");
    require(
        editor.document().snapshotLibrary == beforeClone.snapshotLibrary,
        "snapshot clone should create exactly one history entry");
    requireOk(editor.redo(), "one redo should restore the cloned snapshot");

    requireOk(
        editor.moveSnapshot("main-bank", "alternate", 0),
        "snapshot reorder should succeed");
    require(
        editor.document().snapshotLibrary.banks.front()
                .snapshots.front()
                .snapshotId
            == "alternate",
        "snapshot reorder should retain stable identity");
    requireOk(editor.undo(), "one undo should revert snapshot reorder");
    require(
        editor.document().snapshotLibrary.banks.front()
                .snapshots.front()
                .snapshotId
            == "initial",
        "snapshot reorder should be one history entry");
    requireOk(editor.redo(), "one redo should restore snapshot reorder");

    talas::project::SnapshotTargetIdentity eventTarget;
    eventTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    eventTarget.eventName = "Gain";
    eventTarget.domain = talas::expr::ValueDomain::Scalar;
    const talas::project::SnapshotParameterPolicy policy{
        eventTarget,
        true,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    };
    const auto beforePolicy = editor.document();
    requireOk(
        editor.setSnapshotParameterPolicy("main-bank", policy),
        "setting a range and lock should succeed");
    requireOk(editor.undo(), "one undo should remove the range and lock");
    require(
        editor.document().snapshotLibrary == beforePolicy.snapshotLibrary,
        "range and lock should share one history entry");
    requireOk(editor.redo(), "one redo should restore the range and lock");

    talas::project::SnapshotValue liveValue;
    liveValue.number = 0.625;
    const std::vector<talas::project::SnapshotValueEntry> captured{
        {eventTarget, liveValue},
    };
    const auto beforeCapture = editor.document();
    requireOk(
        editor.captureSnapshotValues(
            "main-bank",
            "alternate",
            captured),
        "explicit performance capture should succeed");
    requireOk(editor.undo(), "one undo should revert the complete capture");
    require(
        editor.document().snapshotLibrary == beforeCapture.snapshotLibrary,
        "explicit capture should create exactly one history entry");
    requireOk(editor.redo(), "one redo should restore the complete capture");

    const auto beforeFailure = editor.document();
    requireError(
        editor.cloneSnapshot(
            "main-bank",
            "alternate",
            "initial",
            "Duplicate"),
        talas::project::EditError::DuplicateSnapshot,
        "cloning onto an existing snapshot identity");
    require(
        editor.document().snapshotLibrary == beforeFailure.snapshotLibrary,
        "failed snapshot commands must preserve document and history state");
}

void testSnapshotInitializeAndExplicitMigrationAreSingleUndoEdits()
{
    auto editor = makeEditor();
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.4;
    gain.minimum = 0.1;
    gain.maximum = 0.9;
    requireOk(
        editor.addVirtualControl(gain),
        "snapshot initialization fixture should add a Virtual Control");
    talas::project::ProjectSnapshotBank defaults;
    defaults.bankId = "defaults";
    defaults.displayName = "Defaults";
    requireOk(
        editor.addSnapshotBank(defaults),
        "snapshot initialization fixture should add a project bank");

    const auto beforeInitialize = editor.document();
    requireOk(
        editor.initializeSnapshot(
            "defaults",
            "opening",
            "Opening"),
        "initializing a snapshot from semantic defaults should succeed");
    const auto& initialized =
        editor.document().snapshotLibrary.banks.front();
    require(
        initialized.snapshots.size() == 1
            && initialized.snapshots.front().values.size() == 1
            && initialized.snapshots.front().values.front().value.number
                == 0.4
            && initialized.parameterPolicies.size() == 1
            && initialized.parameterPolicies.front().range
                == talas::project::SnapshotParameterRange{0.1, 0.9},
        "initialize should use the Virtual Control value and seed its range policy");
    requireOk(editor.undo(), "one undo should revert snapshot initialization");
    require(
        editor.document().snapshotLibrary
            == beforeInitialize.snapshotLibrary,
        "snapshot initialization should create exactly one history entry");
    requireOk(editor.redo(), "one redo should restore snapshot initialization");

    auto migrationEditor = makeEditor();
    requireOk(
        migrationEditor.addVirtualControl(gain),
        "snapshot migration fixture should add a Virtual Control");
    talas::project::SnapshotTargetIdentity oldTarget;
    oldTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    oldTarget.eventName = "OldGain";
    talas::project::SnapshotTargetIdentity newTarget;
    newTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    newTarget.eventName = "Gain";
    talas::project::SnapshotValue oldValue;
    oldValue.number = 0.5;
    talas::project::ProjectSnapshot legacy;
    legacy.snapshotId = "legacy";
    legacy.displayName = "Legacy";
    legacy.values.push_back({oldTarget, oldValue});
    talas::project::ProjectSnapshotBank migrationBank;
    migrationBank.bankId = "migration";
    migrationBank.displayName = "Migration";
    migrationBank.parameterPolicies.push_back({
        oldTarget,
        false,
        talas::project::SnapshotParameterRange{0.1, 0.9},
    });
    migrationBank.snapshots.push_back(legacy);
    requireOk(
        migrationEditor.addSnapshotBank(migrationBank),
        "snapshot migration fixture should preserve a stale target");

    const auto beforeMigration = migrationEditor.document();
    requireOk(
        migrationEditor.migrateSnapshotTarget(
            "migration",
            oldTarget,
            newTarget),
        "explicitly migrating a compatible stale target should succeed");
    const auto& migrated =
        migrationEditor.document().snapshotLibrary.banks.front();
    require(
        migrated.parameterPolicies.front().target == newTarget
            && migrated.snapshots.front().values.front().target
                == newTarget,
        "explicit migration should update policy and stored values together");
    requireOk(
        migrationEditor.undo(),
        "one undo should revert explicit target migration");
    require(
        migrationEditor.document().snapshotLibrary
            == beforeMigration.snapshotLibrary,
        "explicit target migration should create exactly one history entry");
}

void testPreservedMissingNodeEditingAndRepairAreFailureAtomic()
{
    const auto original = preservedEditorDocument();
    const auto originalBytes =
        talas::project::serializeProjectDocument(original);
    auto editor = makeEditor();
    requireOk(
        editor.replaceDocument(original),
        "editor should open a structurally valid preserved missing node");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "opening a missing node should preserve graph, capsule, state, snapshot, and presentation bytes");

    requireOk(
        editor.moveNode("missing1", {300, 180}),
        "missing node should remain movable");
    requireOk(editor.undo(), "one undo should restore missing-node movement");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "movement undo should restore the complete original missing-node document");

    requireOk(
        editor.disconnect("missing1", "audioIn"),
        "an existing edge into a missing node should remain disconnectable");
    require(
        editor.document().graph.connections.size() == 1,
        "disconnect should remove only the selected incoming edge");
    requireOk(editor.undo(), "one undo should restore the missing-node edge");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "disconnect undo should restore topology and every preserved byte");

    requireOk(
        editor.disconnectPreservedComponent("missing1"),
        "one preserved-node action should disconnect every incident edge");
    require(
        editor.document().graph.connections.empty(),
        "preserved-node disconnect should remove incoming and outgoing edges atomically");
    requireOk(
        editor.undo(),
        "one undo should restore every incident preserved-node edge");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "preserved-node disconnect undo should restore the complete document exactly");

    const auto beforeBlockedEdit =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setParameter(
            "missing1",
            choiceParameter("mode", "clean")),
        talas::project::EditError::UnresolvedComponent,
        "parameter mutation on an unresolved component");
    requireError(
        editor.connect({"missing1", "audioOut", "out", "audioIn"}),
        talas::project::EditError::UnresolvedComponent,
        "new connection involving an unresolved component");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeBlockedEdit,
        "blocked missing-node edits should publish no partial document");

    requireOk(editor.removeNode("missing1"), "missing node should be deletable");
    require(
        editor.document().graph.nodes.size() == 2
            && editor.document().graph.connections.empty(),
        "delete should remove the node and both incident connections");
    requireOk(editor.undo(), "one undo should restore a deleted missing node");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "delete undo should restore capsule, state, parameters, edges, snapshot, and presentation exactly");

    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor repairEditor(original, registry);
    requireError(
        repairEditor.repairPreservedComponent("missing1"),
        talas::project::EditError::UnresolvedComponent,
        "repair without a registered descriptor");
    auto descriptor = talas::project::makeProjectPreservedComponentDescriptor(
        "org.example.component",
        preservedEditorComponent());
    descriptor.typeVersion = "1.2.3";
    descriptor.stateSchemaVersion = 4;
    registry.addNodeType(std::move(descriptor));
    requireOk(
        repairEditor.repairPreservedComponent("missing1"),
        "exact trusted descriptor should repair without identity mutation");
    require(
        !repairEditor.canUndo()
            && talas::project::serializeProjectDocument(
                   repairEditor.document())
                == originalBytes,
        "exact repair should retain the evergreen capsule and create no document history");
}

void testNestedPreservedMissingNodeEditingAndRepairAreFailureAtomic()
{
    const auto original = nestedPreservedEditorDocument();
    const auto originalBytes =
        talas::project::serializeProjectDocument(original);
    auto editor = makeEditor();
    requireOk(
        editor.replaceDocument(original),
        "editor should open a preserved missing node inside a Sound definition");

    requireOk(
        editor.moveNodeInSoundDefinition(
            "voice",
            "missing1",
            {300, 180}),
        "nested missing node should remain movable");
    requireOk(editor.undo(), "one undo should restore nested missing-node movement");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "nested movement undo should restore the complete original document");

    requireOk(
        editor.disconnectInSoundDefinition(
            "voice",
            "missing1",
            "audioIn"),
        "an existing edge into a nested missing node should remain disconnectable");
    requireOk(editor.undo(), "one undo should restore the nested missing-node edge");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "nested disconnect undo should restore topology and preserved bytes");

    requireOk(
        editor.disconnectPreservedComponentInSoundDefinition(
            "voice",
            "missing1"),
        "one nested preserved-node action should disconnect every incident edge");
    require(
        editor.document().soundDefinitions.front()
            .graph.connections.empty(),
        "nested preserved-node disconnect should be failure-atomic");
    requireOk(
        editor.undo(),
        "one undo should restore every nested preserved-node edge");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "nested preserved-node disconnect undo should restore the complete document exactly");

    const auto beforeBlockedEdit =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setParameterInSoundDefinition(
            "voice",
            "missing1",
            choiceParameter("mode", "clean")),
        talas::project::EditError::UnresolvedComponent,
        "nested parameter mutation on an unresolved component");
    requireError(
        editor.connectInSoundDefinition(
            "voice",
            {"missing1", "audioOut", "out", "audioIn"}),
        talas::project::EditError::UnresolvedComponent,
        "new nested connection involving an unresolved component");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == beforeBlockedEdit,
        "blocked nested missing-node edits should publish no partial document");

    requireOk(
        editor.removeNodeFromSoundDefinition("voice", "missing1"),
        "nested missing node should be deletable");
    requireOk(editor.undo(), "one undo should restore a nested deleted missing node");
    require(
        talas::project::serializeProjectDocument(editor.document())
            == originalBytes,
        "nested delete undo should restore capsule, parameters, edges, and presentation exactly");

    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor repairEditor(original, registry);
    requireError(
        repairEditor.repairPreservedComponentInSoundDefinition(
            "voice",
            "missing1"),
        talas::project::EditError::UnresolvedComponent,
        "nested repair without a registered descriptor");
    auto descriptor = talas::project::makeProjectPreservedComponentDescriptor(
        "org.example.component",
        preservedEditorComponent());
    descriptor.typeVersion = "1.2.3";
    descriptor.stateSchemaVersion = 4;
    registry.addNodeType(std::move(descriptor));
    requireOk(
        repairEditor.repairPreservedComponentInSoundDefinition(
            "voice",
            "missing1"),
        "exact trusted descriptor should repair a nested component without identity mutation");
    require(
        !repairEditor.canUndo()
            && talas::project::serializeProjectDocument(
                   repairEditor.document())
                == originalBytes,
        "exact nested repair should retain the capsule and create no document history");
}

void testPerformancePresentationUsesOneUndoWithoutMovingStructure()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    requireOk(editor.addNode(
        {"osc", "talas.oscillator", {}}, {48, 64}),
        "Performance history fixture should add one Structure node");
    talas::project::VirtualControl macro;
    macro.eventName = "MacroA";
    macro.minimum = -1.0;
    macro.maximum = 1.0;
    requireOk(editor.addVirtualControl(macro, {80, 96}),
        "Performance history fixture should add a macro source");
    talas::project::VirtualControl destination;
    destination.eventName = "Gain";
    requireOk(editor.addVirtualControl(destination, {240, 96}),
        "Performance history fixture should add a macro destination");

    const auto baseline = talas::project::serializeProjectDocument(
        editor.document());
    const auto structureBefore =
        talas::project::findProjectNodePresentation(
            editor.document(), std::nullopt, "osc")->position;
    const auto macroId =
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), "MacroA", talas::expr::ValueDomain::Scalar)
            ->controlId;
    const auto destinationTarget =
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), "Gain", talas::expr::ValueDomain::Scalar)
            ->target;
    const auto destinationId =
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), "Gain", talas::expr::ValueDomain::Scalar)
            ->controlId;
    const auto macroTarget =
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), "MacroA", talas::expr::ValueDomain::Scalar)
            ->target;

    editor.beginHistoryTransaction();
    requireOk(editor.movePerformanceControl(macroId, {400, 320}),
        "Performance control should move independently");
    requireOk(editor.setPerformanceControlKind(
        macroId, talas::project::ProjectPerformanceControlKind::Macro),
        "Performance control should change to a macro visual kind");
    requireOk(editor.setPerformanceControlModulationSlots(
        macroId, {{destinationTarget, -0.5}}),
        "Performance macro should accept one exact bounded destination");
    requireOk(editor.setPerformanceControlKind(
        destinationId, talas::project::ProjectPerformanceControlKind::Macro),
        "cycle fixture should make the destination a macro");
    requireError(editor.setPerformanceControlModulationSlots(
            destinationId, {{macroTarget, 0.5}}),
        talas::project::EditError::InvalidPerformancePresentation,
        "Performance authoring should reject a macro cycle before commit");
    requireOk(editor.setPerformanceControlKind(
        destinationId, talas::project::ProjectPerformanceControlKind::Knob),
        "cycle fixture should restore the destination visual kind");
    requireOk(editor.addPerformanceMeter(
        {"master-left", 1, {560, 320}, true}),
        "Performance panel should add an output meter");
    editor.endHistoryTransaction();

    const auto* changed =
        talas::project::findProjectPerformanceControlPresentationById(
            editor.document(), macroId);
    require(changed != nullptr
            && changed->position == talas::project::CanvasPosition{400, 320}
            && changed->kind
                == talas::project::ProjectPerformanceControlKind::Macro
            && changed->modulationSlots
                == std::vector<talas::project::ProjectPerformanceModulationSlot>{
                    {destinationTarget, -0.5}}
            && editor.document().presentation.performanceMeters.size() == 1
            && talas::project::findProjectNodePresentation(
                    editor.document(), std::nullopt, "osc")->position
                == structureBefore,
        "Performance authoring should preserve Structure and publish one "
        "coherent portable panel state");
    requireOk(editor.undo(),
        "one undo should restore the complete Performance authoring gesture");
    require(talas::project::serializeProjectDocument(editor.document()) == baseline,
        "one undo should restore control layout, kind, modulation, meter, and "
        "unchanged Structure exactly");
}

void testOwningSoundDeletionReconcilesPerformanceDependenciesInOneUndo()
{
    auto editor = makeEditor();
    auto definition = makeVoiceDefinition();
    definition.inputs.clear();
    definition.exposedParameters.push_back(exposedFrequency());
    requireOk(editor.addSoundDefinition(std::move(definition)),
        "Performance deletion fixture should add a typed Sound");
    requireOk(editor.addSoundInstance("voice1", "voice", {}),
        "Performance deletion fixture should add a concrete Sound instance");

    talas::project::SnapshotTargetIdentity soundTarget;
    soundTarget.kind = talas::project::SnapshotTargetKind::SoundParameter;
    soundTarget.instancePath = {"voice1"};
    soundTarget.definitionId = "voice";
    soundTarget.parameterId = "frequency";
    requireOk(editor.addPerformanceControl(
            soundTarget,
            talas::project::ProjectPerformanceControlKind::Knob,
            "Voice frequency"),
        "exact Sound target should be addable to Performance");

    talas::project::VirtualControl macro;
    macro.eventName = "MacroA";
    macro.minimum = -1.0;
    macro.maximum = 1.0;
    requireOk(editor.addVirtualControl(macro),
        "Performance deletion fixture should add one macro source");
    const auto* source =
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), "MacroA", talas::expr::ValueDomain::Scalar);
    require(source != nullptr,
        "macro source should have one stable portable presentation");
    const auto sourceId = source->controlId;
    requireOk(editor.setPerformanceControlKind(
            sourceId,
            talas::project::ProjectPerformanceControlKind::Macro),
        "source presentation should become a macro");
    requireOk(editor.setPerformanceControlModulationSlots(
            sourceId, {{soundTarget, 0.5}}),
        "macro should reference the concrete Sound target");

    const auto baseline = talas::project::serializeProjectDocument(
        editor.document());
    requireOk(editor.removeNode("voice1"),
        "owning Sound instance deletion should commit");
    const auto* reconciledSource =
        talas::project::findProjectPerformanceControlPresentationById(
            editor.document(), sourceId);
    require(
        talas::project::findProjectPerformanceControlPresentation(
            editor.document(), soundTarget)
                == nullptr
            && reconciledSource != nullptr
            && reconciledSource->modulationSlots.empty(),
        "owning Sound deletion should remove its panel item and every dependent modulation slot transactionally");
    requireOk(editor.undo(),
        "one undo should restore the owning Sound and Performance dependencies");
    require(talas::project::serializeProjectDocument(editor.document()) == baseline,
        "one undo should restore the exact Sound, portable control, modulation depth, and Structure bytes");
}

void testDocumentAuthorityAdvancesAtTheMutationBoundary()
{
    auto editor = makeEditor();
    const auto initial = editor.documentAuthorityGeneration();
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "authorityOsc";
    oscillator.typeId = "talas.oscillator";
    requireOk(
        editor.addNode(std::move(oscillator), {10, 20}),
        "authority fixture node should be added");
    const auto afterAdd = editor.documentAuthorityGeneration();
    require(afterAdd > initial,
        "successful edits must advance document authority");

    talas::project::ProjectNode duplicate;
    duplicate.instanceId = "authorityOsc";
    duplicate.typeId = "talas.oscillator";
    require(!editor.addNode(std::move(duplicate), {20, 30}).ok(),
        "duplicate edit should fail");
    require(editor.documentAuthorityGeneration() == afterAdd,
        "rejected edits must retain document authority");

    requireOk(editor.undo(), "authority fixture undo should succeed");
    require(editor.documentAuthorityGeneration() > afterAdd,
        "undo must advance authority before publishing the restored document");
}

void testExpressionLanguageUpgradeIsAuditedAtomicAndUndoable()
{
    auto legacy = talas::project::ProjectDocument{};
    legacy.formatVersion = 22;
    legacy.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    std::string oversized = "1";
    for (int index = 0; index < 140; ++index)
        oversized += "+1";
    legacy.graph.nodes.push_back({
        "legacyConstant",
        "talas.constant",
        {expressionParameter("value", oversized)},
    });
    talas::project::ProjectEditor rejected{legacy, builtinRegistry()};
    const auto rejectedAuthority =
        rejected.documentAuthorityGeneration();
    const auto rejectedUpgrade =
        rejected.upgradeExpressionsToPureV3();
    requireError(
        rejectedUpgrade,
        talas::project::EditError::InvalidParameterValue,
        "over-budget legacy expression upgrade");
    require(
        rejectedUpgrade.message.find("$.graph.nodes[0]")
                != std::string::npos
            && rejectedUpgrade.message.find("bytes")
                != std::string::npos
            && rejected.document().formatVersion == 22
            && rejected.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::LegacyV1
            && !rejected.canUndo()
            && rejected.documentAuthorityGeneration()
                == rejectedAuthority,
        "failed upgrade should preserve document, history, authority, and exact range evidence");

    legacy.graph.nodes.front().parameters.front().expression = "!Frequency";
    const auto exactLegacyBytes =
        talas::project::serializeProjectDocument(legacy);
    talas::project::ProjectEditor accepted{legacy, builtinRegistry()};
    requireOk(
        accepted.upgradeExpressionsToPureV3(),
        "compatible legacy project should upgrade");
    require(
        accepted.document().formatVersion == 24
            && accepted.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3
            && accepted.document().virtualControls.empty()
            && accepted.canUndo(),
        "upgrade should not synchronize derived Event Value state");
    auto upgradedExceptAuthority = accepted.document();
    upgradedExceptAuthority.formatVersion = 22;
    upgradedExceptAuthority.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    require(
        talas::project::serializeProjectDocument(upgradedExceptAuthority)
            == exactLegacyBytes,
        "upgrade should change only format and language authority");
    requireOk(accepted.undo(), "upgrade should undo");
    require(
        accepted.document().formatVersion == 22
            && accepted.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::LegacyV1,
        "undo should restore exact legacy format and language");
    requireOk(accepted.redo(), "upgrade should redo");
    require(
        accepted.document().formatVersion == 24
            && accepted.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3,
        "redo should restore exact Pure v3 authority");

    talas::project::ProjectDocument pureV2;
    pureV2.formatVersion = 23;
    pureV2.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV2;
    pureV2.graph.nodes.push_back({
        "pureV2Constant",
        "talas.constant",
        {expressionParameter("value", "[1, 2][1]")},
    });
    talas::project::ProjectEditor upgradedV2{
        pureV2,
        builtinRegistry(),
    };
    requireOk(
        upgradedV2.upgradeExpressionsToPureV3(),
        "compatible PureV2 project should upgrade to PureV3");
    require(
        upgradedV2.document().formatVersion == 24
            && upgradedV2.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3,
        "PureV2 upgrade should commit exact format-24 PureV3 authority");
    requireOk(upgradedV2.undo(), "PureV2 upgrade should undo");
    require(
        upgradedV2.document().formatVersion == 23
            && upgradedV2.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV2,
        "undo should restore exact format-23 PureV2 authority");
    requireOk(upgradedV2.redo(), "PureV2 upgrade should redo");
    require(
        upgradedV2.document().formatVersion == 24
            && upgradedV2.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3,
        "redo should restore PureV3 after a PureV2 upgrade");
}

void testPureV4DefinitionEditsAreAtomicOrderedAndUndoable()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectEditor editor{document, builtinRegistry()};
    requireOk(
        editor.upsertExpressionDefinition({
            "scale",
            {{"value", talas::expr::ValueDomain::Scalar}},
            talas::expr::ValueDomain::Scalar,
            "value * 2",
        }),
        "valid PureV4 definition should be accepted");
    requireOk(
        editor.upsertExpressionDefinition({
            "amplify",
            {{"value", talas::expr::ValueDomain::Scalar}},
            talas::expr::ValueDomain::Scalar,
            "scale(value)",
        }),
        "valid dependent PureV4 definition should be accepted");
    require(
        editor.document().expressionDefinitions.definitions.size() == 2
            && editor.document().expressionDefinitions.definitions[0].name
                == "amplify"
            && editor.document().expressionDefinitions.definitions[1].name
                == "scale",
        "accepted definitions should be stored in canonical name order");

    const auto beforeCycle = editor.document();
    requireError(
        editor.upsertExpressionDefinition({
            "scale",
            {{"value", talas::expr::ValueDomain::Scalar}},
            talas::expr::ValueDomain::Scalar,
            "amplify(value)",
        }),
        talas::project::EditError::InvalidExpressionDefinition,
        "recursive PureV4 definition replacement");
    require(
        editor.document().expressionDefinitions
            == beforeCycle.expressionDefinitions,
        "rejected definition replacement should preserve exact library authority");

    requireError(
        editor.removeExpressionDefinition("scale"),
        talas::project::EditError::ExpressionDefinitionInUse,
        "definition referenced by another definition");
    requireOk(
        editor.removeExpressionDefinition("amplify"),
        "unreferenced definition should be removable");
    requireOk(
        editor.removeExpressionDefinition("scale"),
        "dependency should be removable after its caller");
    require(
        editor.document().expressionDefinitions.definitions.empty()
            && editor.canUndo(),
        "definition removal should be one undoable document edit");
    requireOk(editor.undo(), "definition removal should undo");
    require(
        editor.document().expressionDefinitions.definitions.size() == 1
            && editor.document().expressionDefinitions.definitions[0].name
                == "scale",
        "undo should restore the exact removed definition");
}

void testPureV3UpgradeToPureV4IsOneUndo()
{
    talas::project::ProjectDocument document;
    document.formatVersion = 24;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV3;
    talas::project::ProjectEditor editor{document, builtinRegistry()};
    requireOk(
        editor.upgradeExpressionsToPureV4(),
        "compatible PureV3 project should upgrade to PureV4");
    require(
        editor.document().formatVersion == 25
            && editor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV4
            && editor.document().expressionDefinitions.definitions.empty(),
        "PureV4 upgrade should publish format-25 empty-library authority");
    requireOk(editor.undo(), "PureV4 upgrade should undo");
    require(
        editor.document().formatVersion == 24
            && editor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3,
        "undo should restore exact format-24 PureV3 authority");
}

void testPureV4UpgradeToSessionV1PreservesDefinitionsAndUndo()
{
    talas::project::ProjectDocument document;
    document.formatVersion = 25;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV4;
    document.expressionDefinitions.definitions.push_back({
        "scale",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value * 2",
    });
    document.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expressionParameter("value", "scale(!Input)")},
    });
    talas::project::reconcileProjectPresentation(document);
    const auto before = document;

    talas::project::ProjectEditor editor{document, builtinRegistry()};
    requireOk(
        editor.upgradeExpressionsToSessionV1(),
        "compatible PureV4 project should upgrade to SessionV1");
    require(
        editor.document().formatVersion == 26
            && editor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::SessionV1
            && editor.document().expressionDefinitions
                == before.expressionDefinitions,
        "SessionV1 upgrade should retain exact definition authority");

    requireOk(
        editor.setParameter(
            "constant",
            expressionParameter("value", "previous(!Input, 0)")),
        "SessionV1 should author a live stateful parameter expression");
    requireError(
        editor.setParameter(
            "constant",
            expressionParameter("value", "previous(1, 0)")),
        talas::project::EditError::InvalidParameterValue,
        "stateful expression without a live update clock");
    requireOk(editor.undo(), "stateful parameter edit should undo");
    requireOk(editor.undo(), "SessionV1 upgrade should undo");
    require(
        editor.document().formatVersion == before.formatVersion
            && editor.document().expressionLanguage
                == before.expressionLanguage
            && editor.document().expressionDefinitions
                == before.expressionDefinitions
            && editor.document().graph.nodes == before.graph.nodes,
        "undo should restore exact format-25 PureV4 bytes and definitions");
}

void testVariablesV1UpgradeAndDeclarationEditsAreAtomicAndUndoable()
{
    talas::project::ProjectDocument sessionDocument;
    sessionDocument.formatVersion = 26;
    sessionDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::SessionV1;
    talas::project::ProjectEditor upgradeEditor{
        sessionDocument,
        builtinRegistry()};
    talas::project::ProjectVariableDeclaration premature;
    premature.variableId = "premature";
    premature.displayName = "Premature";
    requireError(
        upgradeEditor.upsertProjectVariable(premature),
        talas::project::EditError::InvalidProjectVariable,
        "variable authoring before VariablesV1 upgrade");
    requireOk(
        upgradeEditor.upgradeExpressionsToVariablesV1(),
        "SessionV1 should upgrade explicitly to VariablesV1");
    require(
        upgradeEditor.document().formatVersion == 34
            && upgradeEditor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV1
            && upgradeEditor.document().variableLibrary.variables.empty(),
        "VariablesV1 upgrade should publish current-format empty-library authority");
    requireOk(upgradeEditor.undo(), "VariablesV1 upgrade should be one Undo");
    require(
        upgradeEditor.document().formatVersion == 26
            && upgradeEditor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::SessionV1,
        "undo should restore exact SessionV1 format authority");

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(
        {.definitionId = "voice", .displayName = "Voice"});
    talas::project::ProjectEditor editor{document, builtinRegistry()};

    talas::project::ProjectVariableDeclaration projectGain;
    projectGain.variableId = "gain";
    projectGain.displayName = "Project Gain";
    projectGain.initialValue = 0.25;
    requireOk(
        editor.upsertProjectVariable(projectGain),
        "project variable should be authored through the editor boundary");

    auto renamedGain = projectGain;
    renamedGain.variableId = "projectGain";
    requireOk(
        editor.replaceProjectVariable({}, "gain", renamedGain),
        "unreferenced variable identity should replace atomically");
    requireOk(editor.undo(), "variable identity replacement should be one Undo");
    require(
        editor.document().variableLibrary.variables.front().variableId
            == "gain",
        "variable identity Undo should restore the exact prior declaration");

    auto voiceGain = projectGain;
    voiceGain.displayName = "Voice Gain";
    voiceGain.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    voiceGain.owner.definitionId = "voice";
    requireOk(
        editor.upsertProjectVariable(voiceGain),
        "same variable ID should remain distinct in an exact Sound owner");
    require(
        editor.document().variableLibrary.variables.size() == 2,
        "project and Sound variables should retain separate stable identities");
    requireError(
        editor.removeSoundDefinition("voice"),
        talas::project::EditError::SoundDefinitionInUse,
        "Sound definition owning a variable declaration");

    auto sessionBias = projectGain;
    sessionBias.variableId = "sessionBias";
    sessionBias.displayName = "Session Bias";
    sessionBias.owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    requireOk(
        editor.upsertProjectVariable(sessionBias),
        "workspace-session variable should be authored through its exact owner");
    auto renamedSessionBias = sessionBias;
    renamedSessionBias.displayName = "Renamed Session Bias";
    requireOk(
        editor.replaceProjectVariable(
            sessionBias.owner,
            sessionBias.variableId,
            renamedSessionBias),
        "workspace-session variable should replace through its exact identity");
    requireError(
        editor.captureProjectVariableInitialValue(
            "sessionBias",
            0.5,
            editor.documentAuthorityGeneration()),
        talas::project::EditError::UnknownProjectVariable,
        "workspace-session variables should remain outside project-initial capture");
    requireOk(
        editor.removeProjectVariable(
            renamedSessionBias.owner,
            renamedSessionBias.variableId),
        "workspace-session variable should remove through its exact identity");

    requireOk(
        editor.addNode({
            "constant",
            "talas.constant",
            {expressionParameter("value", "@gain")},
        }),
        "declared project variable should be authorable in root expressions");
    requireError(
        editor.removeProjectVariable({}, "gain"),
        talas::project::EditError::ProjectVariableInUse,
        "referenced project variable removal");
    requireError(
        editor.replaceProjectVariable({}, "gain", renamedGain),
        talas::project::EditError::ProjectVariableInUse,
        "referenced project variable identity replacement");
    auto incompatibleGain = projectGain;
    incompatibleGain.domain = talas::expr::ValueDomain::Hertz;
    requireError(
        editor.upsertProjectVariable(incompatibleGain),
        talas::project::EditError::ProjectVariableInUse,
        "referenced variable domain edit should rerun the expression audit");

    const auto captureAuthority = editor.documentAuthorityGeneration();
    requireOk(
        editor.captureProjectVariableInitialValue(
            "gain", 0.75, captureAuthority),
        "exact current project-variable value should capture explicitly");
    require(
        editor.document().variableLibrary.variables.front().initialValue
            == 0.75,
        "capture should update only the project declaration initial value");
    requireOk(editor.undo(), "variable capture should be one Undo");
    require(
        editor.document().variableLibrary.variables.front().initialValue
            == 0.25,
        "undo should restore the prior declaration initial value");
    requireError(
        editor.captureProjectVariableInitialValue(
            "gain", 0.5, captureAuthority),
        talas::project::EditError::StaleProjectVariableCapture,
        "stale variable capture authority");

    requireOk(
        editor.removeNode("constant"),
        "reference owner should be removable before variable deletion");
    requireOk(
        editor.removeProjectVariable({}, "gain"),
        "unreferenced project variable should be removable");
    require(
        editor.document().variableLibrary.variables.size() == 1
            && editor.document().variableLibrary.variables.front().owner.kind
                == talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "removing the project variable must not rebind or remove the Sound variable");

    talas::project::ProjectDocument voiceDocument;
    talas::project::ProjectSoundDefinition voiceDefinition;
    voiceDefinition.definitionId = "ownedVoice";
    voiceDefinition.displayName = "Owned Voice";
    voiceDefinition.graph.nodes.push_back({
        "voiceSource",
        "talas.oscillator",
        {
            expressionParameter("frequency", "440 hz"),
            expressionParameter("amplitude", "0"),
        },
    });
    voiceDefinition.graph.nodes.push_back({"voiceNode", "talas.midiVoice"});
    voiceDefinition.graph.connections.push_back({
        "voiceSource",
        "audioOut",
        "voiceNode",
        "audioIn",
    });
    voiceDocument.soundDefinitions.push_back(std::move(voiceDefinition));
    talas::project::ProjectVariableDeclaration voicePressure;
    voicePressure.variableId = "pressure";
    voicePressure.displayName = "Voice Pressure";
    voicePressure.snapshotEligible = false;
    voicePressure.owner.kind =
        talas::project::ProjectVariableOwnerKind::MidiVoice;
    voicePressure.owner.definitionId = "ownedVoice";
    voicePressure.owner.nodeId = "voiceNode";
    talas::project::ProjectEditor voiceEditor{
        std::move(voiceDocument),
        builtinRegistry()};
    requireOk(
        voiceEditor.upsertProjectVariable(voicePressure),
        "MIDI-voice variable should be authored through its exact node owner");
    requireError(
        voiceEditor.removeSoundDefinition("ownedVoice"),
        talas::project::EditError::SoundDefinitionInUse,
        "Sound definition owning a MIDI-voice variable declaration");
    auto renamedVoicePressure = voicePressure;
    renamedVoicePressure.displayName = "Renamed Voice Pressure";
    requireOk(
        voiceEditor.replaceProjectVariable(
            voicePressure.owner,
            voicePressure.variableId,
            renamedVoicePressure),
        "MIDI-voice variable should replace through definition and node identity");
    requireOk(
        voiceEditor.removeProjectVariable(
            renamedVoicePressure.owner,
            renamedVoicePressure.variableId),
        "MIDI-voice variable should remove through definition and node identity");
}

void testVariablesV2UpgradeIsExplicitAtomicAndUndoable()
{
    talas::project::ProjectDocument document;
    document.formatVersion = 34;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expressionParameter("value", "5")},
    });
    talas::project::reconcileProjectPresentation(document);
    const auto before = document;

    talas::project::ProjectEditor editor{document, builtinRegistry()};
    requireOk(
        editor.upgradeExpressionsToVariablesV2(),
        "VariablesV1 should upgrade explicitly to VariablesV2");
    require(
        editor.document().formatVersion == talas::project::currentProjectFormatVersion
            && editor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2
            && editor.document().graph.nodes == before.graph.nodes,
        "VariablesV2 upgrade should preserve project bytes beyond exact format/language authority");
    requireOk(
        editor.setParameter(
            "constant",
            expressionParameter("value", "5 rem: 2")),
        "VariablesV2 should author checked remainder expressions");
    requireOk(editor.undo(), "VariablesV2 expression edit should undo");
    requireOk(editor.undo(), "VariablesV2 upgrade should be one Undo");
    require(
        editor.document().formatVersion == before.formatVersion
            && editor.document().expressionLanguage
                == before.expressionLanguage
            && editor.document().graph.nodes == before.graph.nodes,
        "VariablesV2 upgrade Undo should restore exact format-34 VariablesV1 authority");
    requireOk(editor.redo(), "VariablesV2 upgrade should redo");
    require(
        editor.document().formatVersion == talas::project::currentProjectFormatVersion
            && editor.document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "VariablesV2 upgrade Redo should restore exact new authority");

    talas::project::ProjectDocument session;
    session.formatVersion = 26;
    session.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::SessionV1;
    talas::project::ProjectEditor sessionEditor{session, builtinRegistry()};
    requireError(
        sessionEditor.upgradeExpressionsToVariablesV2(),
        talas::project::EditError::InvalidParameterValue,
        "VariablesV2 upgrade should require the explicit VariablesV1 predecessor");
}

void testVariablesV3UpgradeAndTimedExpressionAreAtomicAndUndoable()
{
    using namespace talas::project;
    using Language = talas::expr::ExpressionLanguageVersion;
    ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {expressionParameter("frequency", "500 hz"),
            expressionParameter("amplitude", "1")}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    reconcileProjectPresentation(document);
    ProjectEditor editor{document, builtinRegistry()};
    requireError(editor.setParameter("osc",
            expressionParameter("amplitude", "1 ramp: 4 ms")),
        EditError::InvalidParameterValue,
        "VariablesV2 authoring must reject timed syntax before upgrade");
    requireOk(editor.upgradeExpressionsToVariablesV3(),
        "VariablesV2 must upgrade explicitly to VariablesV3");
    require(editor.document().formatVersion == talas::project::currentProjectFormatVersion
            && editor.document().expressionLanguage == Language::VariablesV3
            && editor.document().graph == document.graph,
        "timed-language upgrade must preserve the authored graph");
    requireOk(editor.upgradeExpressionsToVariablesV3(),
        "VariablesV3 upgrade must be idempotent");
    requireOk(editor.setParameter("osc",
            expressionParameter("amplitude", "1 ramp: 4 ms")),
        "timed amplitude must be editable after explicit upgrade");
    requireError(editor.setParameter("osc",
            expressionParameter("amplitude", "1 ramp: 0 ms")),
        EditError::InvalidParameterValue,
        "timed authoring must reject zero ramp duration");
    const auto canonical = serializeProjectDocument(editor.document());
    const auto reopened = parseProjectDocument(canonical);
    require(reopened.ok()
            && reopened.document.expressionLanguage == Language::VariablesV3
            && reopened.document.graph == editor.document().graph
            && serializeProjectDocument(reopened.document) == canonical,
        "timed expression edit must reopen with exact source bytes");
    requireOk(editor.undo(), "timed expression edit must undo");
    requireOk(editor.undo(), "VariablesV3 upgrade must undo as one action");
    require(editor.document().expressionLanguage == Language::VariablesV2
            && editor.document().graph == document.graph,
        "upgrade Undo must restore exact VariablesV2 project");
    requireOk(editor.redo(), "VariablesV3 upgrade must redo");
    requireOk(editor.redo(), "timed expression edit must redo");
    require(serializeProjectDocument(editor.document()) == canonical,
        "redo must restore the exact timed-expression project");
    document.expressionLanguage = Language::VoiceV1;
    ProjectEditor voiceEditor{document, builtinRegistry()};
    requireError(voiceEditor.upgradeExpressionsToVariablesV3(),
        EditError::InvalidParameterValue,
        "VariablesV3 upgrade must require its VariablesV2 predecessor");
}

void testValUpgradeKeepsEveryMeaning()
{
    using namespace talas::project;
    using Language = talas::expr::ExpressionLanguageVersion;

    ProjectDocument document;
    document.expressionLanguage = Language::VariablesV4;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "220 hz * 2"}, {"amplitude", "1 - 0.5 * 1"}}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};

    ProjectEditor editor{document, builtinRegistry()};
    const auto blocked = editor.upgradeExpressionsToValV1();
    requireError(blocked, EditError::InvalidParameterValue,
        "Val upgrade must refuse an expression its precedence would change");
    require(blocked.message.find("parentheses") != std::string::npos
            && editor.document().expressionLanguage == Language::VariablesV4,
        "the refusal should ask for parentheses and leave the project unchanged");

    requireOk(editor.setParameter("osc", {"amplitude", "1 - (0.5 * 1)"}),
        "parenthesizing should keep the VariablesV4 meaning");
    requireOk(editor.upgradeExpressionsToValV1(),
        "a project whose meanings survive should upgrade to Val");
    require(editor.document().expressionLanguage == Language::ValV1
            && editor.document().formatVersion == currentProjectFormatVersion,
        "the upgrade should select ValV1 in the current format");
    requireOk(editor.upgradeExpressionsToValV1(),
        "the Val upgrade should be idempotent");
    requireOk(editor.undo(), "the Val upgrade should undo as one action");
    require(editor.document().expressionLanguage == Language::VariablesV4,
        "undo should restore VariablesV4");

    document.expressionLanguage = Language::VoiceV1;
    ProjectEditor voiceEditor{document, builtinRegistry()};
    requireError(voiceEditor.upgradeExpressionsToValV1(),
        EditError::InvalidParameterValue,
        "Val upgrade should start from a Variables profile");
}

void testVariablesV4UpgradeAuditsDefaultsAndIsAtomicAndUndoable()
{
    using namespace talas::project;
    using Language = talas::expr::ExpressionLanguageVersion;

    ProjectDocument variablesV2;
    ProjectEditor variablesV2Editor{variablesV2, builtinRegistry()};
    requireOk(variablesV2Editor.upgradeExpressionsToVariablesV4(),
        "VariablesV2 should upgrade directly to VariablesV4");
    require(variablesV2Editor.document().formatVersion
                == currentProjectFormatVersion
            && variablesV2Editor.document().expressionLanguage
                == Language::VariablesV4,
        "direct VariablesV2 upgrade should select format 40 and VariablesV4");
    requireOk(variablesV2Editor.upgradeExpressionsToVariablesV4(),
        "VariablesV4 upgrade should be idempotent");
    requireOk(variablesV2Editor.undo(),
        "VariablesV2 to VariablesV4 upgrade should be one undo action");
    require(variablesV2Editor.document().expressionLanguage == Language::VariablesV2,
        "upgrade undo should restore VariablesV2 authority");
    requireOk(variablesV2Editor.redo(),
        "VariablesV2 to VariablesV4 upgrade should redo");

    ProjectDocument document;
    document.formatVersion = 37;
    document.expressionLanguage = Language::VariablesV3;
    document.graph.nodes = {
        {"sample", "talas.sample", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"sample", "audioOut", "out", "audioIn"},
    };
    ProjectVariableDeclaration scale;
    scale.variableId = "Scale";
    scale.displayName = "Scale";
    scale.domain = talas::expr::ValueDomain::Scalar;
    scale.initialValue = 0.5;
    document.variableLibrary.variables.push_back(scale);
    auto definition = makeVoiceDefinition("auditedDefault");
    ProjectSoundExposedParameter amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.displayName = "Amplitude";
    amplitude.kind = talas::core::ParameterKind::Expression;
    amplitude.domain = talas::expr::ValueDomain::Scalar;
    amplitude.defaultValue = expressionParameter(
        "amplitude", "1");
    amplitude.bindings.push_back({
        ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "amplitude",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(amplitude);
    document.soundDefinitions.push_back(definition);

    ProjectEditor editor{document, builtinRegistry()};
    requireError(
        editor.setParameter(
            "sample",
            expressionParameter("attackTime", "(!Scale * 5) ms")),
        EditError::InvalidParameterValue,
        "VariablesV3 authoring should reject postfix units on compound expressions");
    requireOk(editor.upgradeExpressionsToVariablesV4(),
        "VariablesV3 should upgrade after auditing Sound effective defaults");
    require(editor.document().formatVersion == currentProjectFormatVersion
            && editor.document().expressionLanguage == Language::VariablesV4
            && editor.document().soundDefinitions[0]
                .exposedParameters[0].defaultValue.expression
                == std::optional<std::string>{"1"},
        "upgrade should preserve authored defaults while advancing authority");
    requireOk(
        editor.setParameter(
            "sample",
            expressionParameter("attackTime", "(!Scale * 5) ms")),
        "VariablesV4 should author postfix units on compound expressions");
    requireOk(
        editor.setParameter(
            "sample",
            expressionParameter("scale", "!Scale db")),
        "VariablesV4 should author postfix decibel units on variable references");
    const auto canonical = serializeProjectDocument(editor.document());
    const auto reopened = parseProjectDocument(canonical);
    require(reopened.ok()
            && reopened.document.expressionLanguage == Language::VariablesV4
            && reopened.document.graph == editor.document().graph
            && reopened.document.soundDefinitions[0]
                .exposedParameters[0].defaultValue.expression
                == std::optional<std::string>{"1"}
            && serializeProjectDocument(reopened.document) == canonical,
        "VariablesV4 node edits and audited effective defaults should reopen with exact source bytes");

    requireOk(editor.undo(), "decibel unit edit should undo");
    requireOk(editor.undo(), "compound postfix unit edit should undo");
    requireOk(editor.undo(), "VariablesV4 upgrade should undo as one transaction");
    require(editor.document().formatVersion == 37
            && editor.document().expressionLanguage == Language::VariablesV3
            && editor.document().graph == document.graph
            && editor.document().soundDefinitions[0]
                .exposedParameters[0].defaultValue.expression
                == std::optional<std::string>{"1"},
        "undo should restore format, authority, graph and effective-default source bytes");
    requireOk(editor.redo(), "VariablesV4 upgrade should redo");
    requireOk(editor.redo(), "compound postfix unit edit should redo");
    requireOk(editor.redo(), "decibel unit edit should redo");
    require(serializeProjectDocument(editor.document()) == canonical,
        "redo should restore the exact VariablesV4 project bytes");

    auto invalidDefaultDocument = document;
    invalidDefaultDocument.soundDefinitions[0]
        .exposedParameters[0].defaultValue.expression = "1 + ";
    ProjectEditor invalidDefaultEditor{
        invalidDefaultDocument, builtinRegistry()};
    requireError(
        invalidDefaultEditor.upgradeExpressionsToVariablesV4(),
        EditError::InvalidParameterValue,
        "VariablesV4 upgrade should audit Sound effective defaults");
    require(invalidDefaultEditor.document().formatVersion == 37
            && invalidDefaultEditor.document().expressionLanguage
                == Language::VariablesV3
            && invalidDefaultEditor.document().soundDefinitions[0]
                .exposedParameters[0].defaultValue.expression
                == std::optional<std::string>{"1 + "},
        "failed effective-default audit should leave the document unchanged");

    document.expressionLanguage = Language::VoiceV1;
    ProjectEditor otherProfile{document, builtinRegistry()};
    requireError(otherProfile.upgradeExpressionsToVariablesV4(),
        EditError::InvalidParameterValue,
        "VariablesV4 upgrade should require VariablesV2 or VariablesV3 authority");
}

void testVoiceV1UpgradeAndExpressionEditAreAtomicAndUndoable()
{
    using namespace talas::project;
    using Language = talas::expr::ExpressionLanguageVersion;
    ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {expressionParameter("frequency", "7 hz")}},
        {"voice", "talas.midiVoice", {}}, {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "voice", "audioIn"},
        {"voice", "leftOut", "out", "audioIn"}};
    reconcileProjectPresentation(document);
    ProjectEditor editor{document, builtinRegistry()};
    requireOk(editor.upgradeExpressionsToVoiceV1(), "VariablesV2 must upgrade explicitly to VoiceV1");
    require(editor.document().formatVersion == talas::project::currentProjectFormatVersion && editor.document().expressionLanguage == Language::VoiceV1
        && editor.document().graph == document.graph, "voice upgrade must preserve authored graph data");
    requireOk(editor.upgradeExpressionsToVoiceV1(), "VoiceV1 upgrade must be idempotent");
    requireOk(editor.setParameter("osc", expressionParameter("frequency", "?VoiceNumber * 7 hz")),
        "voice-owned expression must be editable after explicit upgrade");
    const auto canonical = serializeProjectDocument(editor.document());
    const auto reopened = parseProjectDocument(canonical);
    require(reopened.ok() && reopened.document.expressionLanguage == Language::VoiceV1
        && reopened.document.graph == editor.document().graph
        && serializeProjectDocument(reopened.document) == canonical,
        "VoiceV1 edit must reopen and serialize without replacing the authored voice template");
    requireError(editor.setParameter("voice", expressionParameter("polyphony", "?VoiceNumber")),
        EditError::InvalidParameterValue, "voice owner configuration cannot consume its child voice ordinal");
    requireError(editor.setParameter("osc", expressionParameter("frequency", "?VoiceNumber * 1 s")),
        EditError::InvalidParameterValue, "voice-aware editor audit must preserve units");
    require(serializeProjectDocument(editor.document()) == canonical,
        "failed voice edits must preserve the complete document");
    requireOk(editor.undo(), "voice expression edit must undo");
    requireOk(editor.undo(), "voice profile upgrade must undo as one action");
    require(editor.document().expressionLanguage == Language::VariablesV2
        && editor.document().graph == document.graph, "undo must restore exact old profile and expression bytes");
    requireOk(editor.redo(), "voice profile upgrade must redo");
    requireOk(editor.redo(), "voice expression edit must redo");
    require(serializeProjectDocument(editor.document()) == canonical,
        "redo must restore the exact serialized voice project");
    document.expressionLanguage = Language::PureV4;
    ProjectEditor oldProfile{document, builtinRegistry()};
    requireError(oldProfile.upgradeExpressionsToVoiceV1(), EditError::InvalidParameterValue,
        "voice profile upgrade must require its explicit VariablesV2 predecessor");
}

void testSoundInstanceTextOverrideUsesBoundRuntimeDialect()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "text-sound";
    definition.displayName = "Text Sound";
    definition.graph.nodes.push_back(
        {"shape", "talas.waveshaper", {}});
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "shape";
    exposed.displayName = "Shape";
    exposed.kind = talas::core::ParameterKind::Text;
    exposed.defaultValue = textParameter("shape", "1");
    exposed.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "shape",
        "coefficients",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(exposed));
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode instance;
    instance.instanceId = "textInstance";
    instance.soundDefinitionId = "text-sound";
    document.graph.nodes.push_back(std::move(instance));

    talas::project::ProjectEditor editor{document, builtinRegistry()};
    const auto before =
        talas::project::serializeProjectDocument(editor.document());
    requireError(
        editor.setParameter(
            "textInstance", textParameter("shape", "native(1)")),
        talas::project::EditError::InvalidParameterValue,
        "authoring a Sound-instance Text override outside the bound runtime dialect");
    require(
        talas::project::serializeProjectDocument(editor.document()) == before,
        "rejected Sound-instance Text overrides should preserve exact project bytes");
}
}

// Duplicate makes a copy of the selected Sound, its
// signal flow laid out as the original's, in one undo step.
void testDuplicatingASound()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    pad.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    document.soundDefinitions.push_back(pad);
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(document, std::string_view{"pad"}, "tone")->position = {120, 40};
    talas::project::ProjectEditor editor(std::move(document), builtinRegistry());
    requireOk(editor.duplicateSoundDefinition("pad", "pad1", "Pad 1"), "the Sound should duplicate");
    const auto& sounds = editor.document().soundDefinitions;
    require(sounds.size() == 2 && sounds.back().definitionId == "pad1" && sounds.back().displayName == "Pad 1"
            && sounds.back().graph == sounds.front().graph,
        "the duplicate should be a copy of the Sound under its new name");
    const auto* placed = talas::project::findProjectNodePresentation(editor.document(), std::string_view{"pad1"}, "tone");
    require(placed != nullptr && placed->position == talas::project::CanvasPosition{120, 40},
        "the duplicate's signal flow should be laid out as the original's");
    require(!editor.duplicateSoundDefinition("pad", "pad1", "Pad 2").ok(), "a duplicate needs a free id");
    requireOk(editor.undo(), "the duplicate should undo");
    require(editor.document().soundDefinitions.size() == 1, "one undo step should take the duplicate back");
}

// Undo belongs to the window in front. An edit made
// in the Sound editor is undone with the Sound editor in front; with
// another window in front Undo is grayed out, until the Sound editor is
// clicked to the front again. Redo follows the same window, a gesture keeps
// the window it began in, and an edit made in no window in particular is
// undone in any.
void testUndoBelongsToTheWindowInFront()
{
    using Window = talas::project::ProjectEditor::HistoryWindow;
    constexpr Window soundEditor = 7;
    constexpr Window soundFileWindow = 9;
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    pad.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    document.soundDefinitions.push_back(pad);
    talas::project::reconcileProjectPresentation(document);
    talas::project::ProjectEditor editor(std::move(document), builtinRegistry());
    const auto name = [&] { return editor.document().soundDefinitions.front().displayName; };

    requireOk(editor.renameSoundDefinition("pad", "Any"), "an edit in no window in particular");
    require(editor.canUndoIn(soundEditor) && editor.canUndoIn(soundFileWindow),
        "an edit made in no window in particular should be undone in any");
    requireOk(editor.undoIn(soundFileWindow), "undo it from any window");

    editor.setHistoryWindow(soundEditor);
    requireOk(editor.renameSoundDefinition("pad", "Warm pad"), "an edit in the Sound editor");
    editor.setHistoryWindow(soundFileWindow);
    require(editor.canUndo() && !editor.canUndoIn(soundFileWindow),
        "Undo should be grayed out with another window in front");
    requireError(editor.undoIn(soundFileWindow), talas::project::EditError::NoUndoAvailable,
        "Undo with another window in front");
    require(name() == "Warm pad", "the refused Undo should leave the Sound editor's edit");
    editor.setHistoryWindow(soundEditor);
    require(editor.canUndoIn(soundEditor), "with the Sound editor clicked to the front, Undo is there again");
    requireOk(editor.undoIn(soundEditor), "Undo in the Sound editor");
    require(name() == "Pad", "Undo in the Sound editor should take its edit back");
    require(editor.canRedoIn(soundEditor) && !editor.canRedoIn(soundFileWindow),
        "Redo should belong to the window whose edit was undone");
    requireOk(editor.redoIn(soundEditor), "redo it in the Sound editor");
    require(name() == "Warm pad", "Redo should do the edit again");

    // A newer edit in the Sound file window is undone there first.
    editor.setHistoryWindow(soundFileWindow);
    requireOk(editor.duplicateSoundDefinition("pad", "pad2", "Pad 2"), "an edit in the Sound file window");
    require(!editor.canUndoIn(soundEditor) && editor.canUndoIn(soundFileWindow),
        "the Sound file window's newer edit should gray Undo out in the Sound editor");
    requireOk(editor.undoIn(soundFileWindow), "undo in the Sound file window");
    require(editor.document().soundDefinitions.size() == 1 && editor.canUndoIn(soundEditor)
            && !editor.canUndoIn(soundFileWindow),
        "then the Sound editor's edit is the newest, undone there only");

    // A gesture keeps the window it began in.
    editor.setHistoryWindow(soundEditor);
    editor.beginHistoryTransaction();
    editor.setHistoryWindow(soundFileWindow);
    requireOk(editor.renameSoundDefinition("pad", "Bright pad"), "an edit within the gesture");
    require(editor.canUndoIn(soundEditor) && !editor.canUndoIn(soundFileWindow),
        "an open gesture belongs to the window it began in");
    editor.endHistoryTransaction();
    require(editor.canUndoIn(soundEditor) && !editor.canUndoIn(soundFileWindow),
        "a gesture should keep the window it began in");
    requireOk(editor.undoIn(soundEditor), "undo the gesture");
    require(name() == "Warm pad", "the gesture should undo as one step");

    // A new document in place: nothing to undo, and it is counted.
    const auto replacements = editor.documentReplacements();
    requireOk(editor.replaceDocument(editor.document()), "another document in place");
    require(editor.documentReplacements() == replacements + 1 && !editor.canUndoIn(soundEditor) && !editor.canUndo(),
        "a document put in place should be counted and leave nothing to undo");
}

// Format 140:
// Collect... moves the selected Sounds and collections into a new named
// collection, a collection may hold collections, a duplicate of one holds
// a duplicate of each Sound, deleting one leaves its Sounds where it was,
// and the collections are saved with the Sound file.
void testCollectingSounds()
{
    talas::project::ProjectDocument document;
    for (const auto* id : {"a", "b", "c"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = std::string{id} + " sound";
        sound.graph.nodes.push_back({"tone", "talas.oscillator", {}});
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    }
    talas::project::reconcileProjectPresentation(document);
    talas::project::ProjectEditor editor(std::move(document), builtinRegistry());
    std::string drums;
    requireOk(editor.collectSounds(std::nullopt, {"a", "b"}, {}, "Drum Loops", drums), "Collect should make a collection");
    require(drums == "DrumLoops" && editor.document().soundCollections.size() == 1
            && editor.document().soundCollections.front().soundIds == std::vector<std::string>{"a", "b"},
        "the selected Sounds go into a collection of the given name");
    std::string inner;
    requireOk(editor.collectSounds(std::string_view{"DrumLoops"}, {"b"}, {}, "Fills", inner), "a collection within one");
    const auto& outer = editor.document().soundCollections.front();
    require(outer.soundIds == std::vector<std::string>{"a"} && outer.collectionIds == std::vector<std::string>{inner},
        "a collection holds Sounds and other collections, a Sound moving into the new one");
    std::string empty;
    require(!editor.collectSounds(std::nullopt, {}, {}, "Nothing", empty).ok(), "Collect needs Sounds selected");
    requireOk(editor.renameSoundCollection("DrumLoops", "Loops"), "a collection should rename");

    const auto saved = talas::project::serializeProjectDocument(editor.document());
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok() && reopened.document.soundCollections == editor.document().soundCollections,
        "format 140: the collections should be saved with the Sound file");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 139");
    require(!talas::project::parseProjectDocument(older).ok(), "format 139 should refuse Sound collections");

    std::string copy;
    requireOk(editor.duplicateSoundCollection("DrumLoops", copy), "a collection should duplicate");
    const auto duplicate = std::ranges::find(editor.document().soundCollections, copy, &talas::project::ProjectSoundCollection::collectionId);
    require(duplicate != editor.document().soundCollections.end() && duplicate->soundIds.size() == 1
            && duplicate->soundIds.front() != "a" && duplicate->collectionIds.size() == 1
            && editor.document().soundDefinitions.size() == 5,
        "a duplicate of a collection holds a duplicate of each of its Sounds");
    requireOk(editor.undo(), "the duplicate should undo");
    requireOk(editor.removeSoundCollection(inner), "a collection should delete");
    require(editor.document().soundCollections.size() == 1
            && editor.document().soundCollections.front().soundIds == std::vector<std::string>{"a", "b"},
        "deleting a collection leaves its Sounds where it was");
    requireOk(editor.removeSoundDefinition("a"), "a collected Sound should still delete");
    require(editor.document().soundCollections.front().soundIds == std::vector<std::string>{"b"},
        "a deleted Sound leaves its collection");
}

void testReplacementCheckKeepsTheCurrentProject()
{
    auto editor = makeEditor();
    requireOk(
        editor.addNode({"osc", "talas.oscillator", {}}),
        "fixture should add a node");
    const auto before =
        talas::project::serializeProjectDocument(editor.document());
    const auto generation = editor.documentAuthorityGeneration();

    talas::project::ProjectDocument invalid;
    invalid.graph.nodes.push_back({"loop", "talas.vca", {}});
    invalid.graph.connections.push_back({"loop", "audioOut", "loop", "audioIn"});
    talas::project::reconcileProjectPresentation(invalid);
    const auto rejected = editor.checkReplacementDocument(invalid);
    require(!rejected.ok()
            && !editor.replaceDocument(invalid).ok(),
        "the check should reject exactly what replaceDocument rejects");
    require(talas::project::serializeProjectDocument(editor.document()) == before
            && editor.documentAuthorityGeneration() == generation
            && editor.canUndo(),
        "a rejected replacement must leave the document, authority and history untouched");

    talas::project::ProjectDocument valid;
    valid.metadata.title = "Replacement";
    require(editor.checkReplacementDocument(valid).ok()
            && talas::project::serializeProjectDocument(editor.document()) == before,
        "checking an accepted replacement must not adopt it");
}

// Frequency, Duration and Time fields need
// their units: a number entered bare, other than 0, is refused, telling
// what to type; with units, as an Event Value or a variable it is taken.
void testFrequencyAndDurationFieldsNeedTheirUnits()
{
    for (const auto language : {talas::expr::ExpressionLanguageVersion::LegacyV1, talas::expr::ExpressionLanguageVersion::ValV1})
    {
    talas::project::ProjectDocument document;
    document.expressionLanguage = language;
    document.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    document.graph.nodes.push_back({"env", "talas.adsr", {}});
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(document, registry);
    const auto set = [&](std::string nodeId, std::string parameterId, std::string expression) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(parameterId);
        parameter.expression = std::move(expression);
        return editor.setParameter(nodeId, parameter);
    };
    const auto bare = set("osc", "frequency", "440");
    require(!bare.ok() && bare.message.find("440 hz") != std::string::npos,
        "a Frequency field should refuse a number without its units: " + bare.message);
    for (const auto* accepted : {"440 hz", "69 nn", "4 a", "!Pitch", "2 * 220 hz"})
        requireOk(set("osc", "frequency", accepted), std::string{"a Frequency field should take "} + accepted);
    const auto bareTime = set("env", "attackTime", "2");
    require(!bareTime.ok() && bareTime.message.find("2 s") != std::string::npos,
        "a Duration field should refuse a number without its units: " + bareTime.message);
    for (const auto* accepted : {"2 s", "20 ms"})
        requireOk(set("env", "attackTime", accepted), std::string{"a Duration field should take "} + accepted);
    requireOk(set("osc", "amplitude", "0.5"), "a field of no units should take a bare number");
    }
}

// Format 156: the Edit menu's Field
// Reading sets how the project's fields read rounded and truncated, a beat
// and a number given to inSOSPitch, as one undo step; a beat tempo outside
// 1 to 1000 is refused.
void testFieldReadingSettingsAreOneUndoableEdit()
{
    auto editor = makeEditor();
    requireOk(editor.setFieldReadingSettings(talas::expr::RoundingRule::ManualTable, 60.0,
                  talas::expr::UnitlessSOSPitch::Linear),
        "the chosen field reading should be set");
    const auto& definitions = editor.document().expressionDefinitions;
    require(definitions.rounding == talas::expr::RoundingRule::ManualTable
            && definitions.fieldBeatTempoBpm == std::optional<double>{60.0}
            && definitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Linear,
        "the project should keep the chosen field reading");
    const auto parsed = talas::expr::parse("-2.5 rounded", talas::expr::ExpressionLanguageVersion::ValV1, definitions);
    require(parsed.ok() && talas::expr::evaluate(*parsed.expression, {}).value.value == -2.0,
        "the project's fields should round by the floor-based rule");
    requireError(editor.setFieldReadingSettings(talas::expr::RoundingRule::Smalltalk, 0.0,
                     talas::expr::UnitlessSOSPitch::Hertz),
        talas::project::EditError::InvalidParameterValue, "a beat tempo of 0");
    requireOk(editor.undo(), "the field reading should undo");
    require(editor.document().expressionDefinitions == talas::expr::ExpressionDefinitionLibrary{},
        "undo should restore Smalltalk's rounding, the Timeline's beat and hertz in one step");
}

int main()
{
    testFrequencyAndDurationFieldsNeedTheirUnits();
    testFieldReadingSettingsAreOneUndoableEdit();
    testSoundInstanceOverridesUseTheExposedInterfaceAndUndo();
    testSoundInstanceOverrideReplacesItsAliasSpelling();
    testSoundInterfaceValidationRejectsInvalidBindingsAndAliases();
    testSoundDefinitionsAndInstancesAreValidatedAndUndoable();
    testSoundValidationRejectsMissingBoundariesAndRecursion();
    testMixerInputsTakeManyConnections();
    testExpandingASoundOneLevelAtATime();
    testScriptBoundVariablesAreNotFree();
    testExtractAndInlineSoundAreAtomicTopologyPreservingEdits();
    testSoundExtractionAndInliningRejectPositionOverflowTransactionally();
    testSoundExtractionRejectsTerminalRootNodes();
    testInlineRejectsSoundsThatDependOnTheirOwnScope();
    testSoundDefinitionGraphsAreEditableAsOneScopedUndoModel();
    testEditorBuildsPatchIncrementallyAndValidatesForRender();
    testMoveAndConnectionErrorsAreTransactional();
    testRemovalDisconnectAndParameterValidation();
    testEditorRejectsDuplicateIdsUnknownTypesAndCycles();
    testEditorUndoRedoAndDragTransactions();
    testGraphSelectionEditsAreDeterministicAtomicAndScoped();
    testMovingASoundIsOnlyCosmetic();
    testRevertDropsTheChangesSinceTheSoundWasOpened();
    testPasteSpecialSharesTheSoundItself();
    testPasteSpecialIntoInputFieldsSharesTheSound();
    testInputsFieldListsRemovesAndReorders();
    testDeletingASoundGivesItsPlaceToItsInput();
    testInsertionAndDeleteAskWhichInput();
    testAnySoundServesWhereAPrototypeDoes();
    testReplaceAllTakesTheInputsAway();
    testDroppedSoundsInsertOnLinesReplaceSoundsAndPasteCopies();
    testASoundDroppedOnALinesPlusIsMixedWithIt();
    testRenamedSoundsKeepTheirLinesAndCleanUpIsOneEdit();
    testGraphicalEnvelopeLoopMarksAreEditable();
    testTimelineClipEffects();
    testHistoryTransactionsKeepTheirOwners();
    testEditorHistoryRestoresConnectionsAndNodeRemoval();
    testEditorReplacesOnlyValidatedDocumentsAndClearsHistory();
    testPreparedWholeDocumentEditIsFailureAtomicAndOneUndo();
    testVirtualControlsValidateAndParticipateInUndoHistory();
    testVirtualControlPresentationEditsAreUndoable();
    testPerformancePresentationUsesOneUndoWithoutMovingStructure();
    testOwningSoundDeletionReconcilesPerformanceDependenciesInOneUndo();
    testDocumentAuthorityAdvancesAtTheMutationBoundary();
    testExpressionLanguageUpgradeIsAuditedAtomicAndUndoable();
    testPureV4DefinitionEditsAreAtomicOrderedAndUndoable();
    testPureV3UpgradeToPureV4IsOneUndo();
    testPureV4UpgradeToSessionV1PreservesDefinitionsAndUndo();
    testVariablesV1UpgradeAndDeclarationEditsAreAtomicAndUndoable();
    testVariablesV2UpgradeIsExplicitAtomicAndUndoable();
    testVariablesV3UpgradeAndTimedExpressionAreAtomicAndUndoable();
    testVariablesV4UpgradeAuditsDefaultsAndIsAtomicAndUndoable();
    testValUpgradeKeepsEveryMeaning();
    testVoiceV1UpgradeAndExpressionEditAreAtomicAndUndoable();
    testSoundInstanceTextOverrideUsesBoundRuntimeDialect();
    testVirtualControlsRejectMidiMappingCollisionsAndUnknownUpdates();
    testVirtualControlMidiMappingsCanBeChangedAndCleared();
    testVirtualControlMidiLearnConflictsAndExplicitReplacementAreAtomic();
    testVirtualControlMidiAuthoringRequiresExactSourceAuthority();
    testScoreEditsAreValidatedAtomicAndUndoable();
    testTimelineMarkersAtTypedTimesMoveAndDelete();
    testScoreClipRenderArtifactPublicationIsAtomicAndUndoable();
    testTimelineEventsValidateAndSortChronologically();
    testTimelineEventsUpdateRemoveAndParticipateInHistory();
    testTimelineLoopIsValidatedAndUndoable();
    testAddingAnAssetNoParameterNames();
    testResourceAssetBindingIsAtomicValidatedAndUndoable();
    testAssetRelocationAndReplacementPreserveGenerationRules();
    testAssetResolutionBatchIsFailureAtomicAndOneUndo();
    testAcceptedAssetInspectionPreservesMigratedGeneration();
    testRuntimeAssetIdentityReconciliationRemainsMonotonic();
    testAssetPathRebasingIsValidatedAndPreservesHistory();
    testEventValueControlsSynchronizeAtomicallyWithGraphEdits();
    testLoadedFormatElevenMoveSynchronizesPortablePresentation();
    testSnapshotLibraryCommandsAreAtomicSingleUndoEdits();
    testSnapshotInitializeAndExplicitMigrationAreSingleUndoEdits();
    testPreservedMissingNodeEditingAndRepairAreFailureAtomic();
    testNestedPreservedMissingNodeEditingAndRepairAreFailureAtomic();
    testReplacementCheckKeepsTheCurrentProject();
    testDuplicatingASound();
    testUndoBelongsToTheWindowInFront();
    testCollectingSounds();
    return 0;
}
