#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/VirtualControlMidi.h"

#include <cmath>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

talas::project::ProjectDocument oscillatorDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        std::move(parameters),
    });
    talas::project::reconcileProjectPresentation(document);
    return document;
}

const talas::project::VirtualControl* findControl(
    const std::vector<talas::project::VirtualControl>& controls,
    std::string_view name)
{
    const auto found = std::ranges::find_if(
        controls,
        [&](const auto& control) { return control.eventName == name; });
    return found == controls.end() ? nullptr : &*found;
}

void testDiscoversStableScalarAndDirectDomainControls()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = oscillatorDocument({
        expression("frequency", "!Frequency"),
        expression("amplitude", "!Gain"),
        expression("envelope", "!KeyDown"),
    });

    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(discovered.ok(), "valid references should discover without diagnostics");
    require(discovered.missingControls.size() == 2,
            "built-in keyboard references should not generate VCS controls");
    require(discovered.missingControls[0].eventName == "Frequency"
                && discovered.missingControls[0].domain
                    == talas::expr::ValueDomain::Hertz,
            "direct frequency reference should infer hertz in source order");
    require(discovered.missingControls[1].eventName == "Gain"
                && discovered.missingControls[1].domain
                    == talas::expr::ValueDomain::Scalar,
            "direct amplitude reference should infer scalar");
}

void testPrefixerDiscoversEffectiveRootEventValues()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"prefix", "talas.prefixer", {textParameter("id", "Lead")}},
        {"lead", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain"),
        }},
    };
    document.graph.connections = {
        {"lead", "audioOut", "prefix", "audioIn"},
        {"prefix", "audioOut", "out", "audioIn"},
    };
    const auto prefixed =
        talas::project::discoverProjectEventValueControls(
            document, registry);
    require(prefixed.ok()
            && prefixed.missingControls.size() == 1
            && findControl(prefixed.missingControls, "LeadGain") != nullptr
            && findControl(prefixed.missingControls, "Gain") == nullptr,
        "VCS discovery should expose the effective prefixed root Event Value");

    document.graph.nodes.push_back({
        "other", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain"),
        },
    });
    document.graph.connections.push_back({
        "other", "audioOut", "out", "rightIn"});
    const auto separated =
        talas::project::synchronizeProjectEventValueControls(
            document, registry);
    require(separated.ok()
            && separated.addedCount == 2
            && findControl(document.virtualControls, "LeadGain") != nullptr
            && findControl(document.virtualControls, "Gain") != nullptr,
        "Prefixer should keep independent prefixed and unprefixed VCS controls");
}

void testAnalogSequencerGeneratedIndexDiscoveryAndRenameScopes()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {textParameter("generatedIndex", "!Stage")},
    });
    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(discovered.ok() && discovered.missingControls.size() == 1
            && discovered.missingControls.front().eventName == "Stage"
            && discovered.missingControls.front().domain
                == talas::expr::ValueDomain::Scalar
            && talas::project::projectReferencesEventValue(
                document, registry, "Stage"),
        "GeneratedIndex should expose and retain a Scalar target even without consumers");

    document.virtualControls.push_back(
        talas::project::makeDefaultVirtualControl(
            "Stage", talas::expr::ValueDomain::Hertz));
    const auto conflict =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(!conflict.ok()
            && std::ranges::any_of(conflict.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.eventName == "Stage";
                }),
        "GeneratedIndex should conflict with a persisted non-Scalar Event Value domain");

    const auto makeWrappedSequence = [](std::string wrapperType,
                                        std::string wrapperId,
                                        std::vector<talas::project::ProjectParameterExpression>
                                            wrapperParameters) {
        talas::project::ProjectDocument wrapped;
        wrapped.graph.nodes = {
            {"output", "talas.audioOutput", {}},
            {wrapperId, std::move(wrapperType),
             std::move(wrapperParameters)},
            {"sequence", "talas.analogSequencer",
             {textParameter("generatedIndex", "!Stage")}},
            {"source", "talas.oscillator",
             {expression("frequency", "440 hz"),
              expression("amplitude", "!Stage")}},
        };
        wrapped.graph.connections = {
            {"source", "audioOut", "sequence", "audioIn"},
            {"sequence", "leftOut", wrapperId, "audioIn"},
            {wrapperId, "audioOut", "output", "audioIn"},
        };
        return wrapped;
    };

    auto prefixed = makeWrappedSequence(
        "talas.prefixer", "prefix",
        {textParameter("id", "Lead")});
    const auto prefixExpansion =
        talas::project::expandProjectSounds(prefixed, registry);
    const auto prefixSequence = std::ranges::find_if(
        prefixExpansion.document.graph.nodes,
        [](const auto& node) {
            return node.typeId == "talas.analogSequencer";
        });
    const auto prefixSource = std::ranges::find(
        prefixExpansion.document.graph.nodes,
        std::string_view{"source"},
        &talas::project::ProjectNode::instanceId);
    bool prefixRenamed = false;
    if (prefixExpansion.ok()
        && prefixSequence != prefixExpansion.document.graph.nodes.end()
        && prefixSource != prefixExpansion.document.graph.nodes.end())
    {
        const auto target = std::ranges::find(
            prefixSequence->parameters,
            std::string_view{"generatedIndex"},
            &talas::project::ProjectParameterExpression::parameterId);
        const auto amplitude = std::ranges::find(
            prefixSource->parameters,
            std::string_view{"amplitude"},
            &talas::project::ProjectParameterExpression::parameterId);
        prefixRenamed = target != prefixSequence->parameters.end()
            && target->text == std::optional<std::string>{"!LeadStage"}
            && amplitude != prefixSource->parameters.end()
            && amplitude->expression
                == std::optional<std::string>{"!LeadStage"};
    }
    require(prefixRenamed,
        "Prefixer should rename the GeneratedIndex target and its consumers together");

    auto replicated = makeWrappedSequence(
        "talas.replicator", "replicate",
        {expression("number", "2"), textParameter("prefix", "Voice1"),
         textParameter("suffix", "")});
    const auto replication =
        talas::project::expandProjectSounds(replicated, registry);
    std::vector<std::string> targets;
    std::vector<std::string> consumers;
    for (const auto& node : replication.document.graph.nodes)
    {
        if (node.typeId == "talas.analogSequencer")
        {
            const auto target = std::ranges::find(
                node.parameters, std::string_view{"generatedIndex"},
                &talas::project::ProjectParameterExpression::parameterId);
            if (target != node.parameters.end() && target->text.has_value())
                targets.push_back(*target->text);
        }
        if (node.typeId == "talas.oscillator")
        {
            const auto amplitude = std::ranges::find(
                node.parameters, std::string_view{"amplitude"},
                &talas::project::ProjectParameterExpression::parameterId);
            if (amplitude != node.parameters.end()
                && amplitude->expression.has_value())
                consumers.push_back(*amplitude->expression);
        }
    }
    std::ranges::sort(targets);
    std::ranges::sort(consumers);
    require(replication.ok()
            && targets == std::vector<std::string>{"!Voice1Stage", "!Voice2Stage"}
            && consumers == targets,
        "Replicator should give each GeneratedIndex target and its consumers the same renamed Event Value");
}

void testDiscoversChangingCharacteristicArrayMembers()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "mapper", "talas.inputOutputCharacteristic",
        {textParameter("inValues", "#(-1 !MidX 1)"),
         textParameter("outValues", "#(-1 !MidY 1)")},
        {},
    });
    const auto discovered =
        talas::project::discoverProjectEventValueControls(
            document, registry);
    require(discovered.ok()
                && discovered.missingControls.size() == 2
                && discovered.missingControls[0].eventName == "MidX"
                && discovered.missingControls[1].eventName == "MidY"
                && discovered.missingControls[0].domain
                    == talas::expr::ValueDomain::Scalar
                && discovered.missingControls[1].domain
                    == talas::expr::ValueDomain::Scalar,
        "Val array members should become scalar VCS controls");
    require(talas::project::projectReferencesEventValue(
                document, registry, "MidX")
                && talas::project::projectReferencesEventValue(
                    document, registry, "MidY"),
        "array members should count as authored Event Value references");
}

void testDiscoversAnalogSequencerTextArrayMembersAndReopensThem()
{
    using talas::expr::ExpressionLanguageVersion;
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage = ExpressionLanguageVersion::VariablesV3;
    document.variableLibrary.variables.push_back({
        "Trim", "Trim", ValueDomain::Scalar, 0.25,
        {talas::project::ProjectVariableOwnerKind::Project, {}, {}}, true});
    document.graph.nodes.push_back({
        "sequencer", "talas.analogSequencer", {
            textParameter("durations",
                "!ArrayDuration 25 ms {(!TimedDuration ramp: 20 ms) * 10 ms}"),
            textParameter("dutyCycles",
                "!ArrayDuty 0.5 {!TimedDuty ramp: 20 ms}"),
            textParameter("pitches",
                "!ArrayPitch 64 nn {60 nn + ((!TimedPitch ramp: 20 ms) * 10 nn)}"),
            textParameter("velocities",
                "!ArrayVelocity 0.9 {!TimedVelocity ramp: 20 ms}"),
            textParameter("keyDowns", "!ArrayDown {!TimedDown ramp: 20 ms}"),
            textParameter("keyPitches", "!KeyPitch !KeyNumber !ArrayKeyPitch"),
            textParameter("keyVelocities", "!KeyVelocity !ArrayKeyVelocity"),
            textParameter("keyTimbres", "!KeyTimbre !ArrayTimbre"),
            textParameter("extraValues",
                "#(!PublishedScalar 0.2 {!ExtraValue + @Trim})"),
        },
    });
    talas::project::reconcileProjectPresentation(document);

    const auto json = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(json, registry);
    require(reopened.ok(), "AnalogSequencer text arrays should survive save/reopen");
    const auto reopenedSequencer = std::ranges::find(
        reopened.document.graph.nodes, std::string_view{"sequencer"},
        &talas::project::ProjectNode::instanceId);
    require(reopenedSequencer != reopened.document.graph.nodes.end(),
        "saved AnalogSequencer should remain in its root graph");
    for (const auto parameterId : {
             "durations", "dutyCycles", "pitches", "velocities",
             "keyDowns", "keyPitches", "keyVelocities", "keyTimbres",
             "extraValues"})
    {
        const auto original = std::ranges::find(
            document.graph.nodes.front().parameters,
            std::string_view{parameterId},
            &talas::project::ProjectParameterExpression::parameterId);
        const auto restored = std::ranges::find(
            reopenedSequencer->parameters,
            std::string_view{parameterId},
            &talas::project::ProjectParameterExpression::parameterId);
        require(original != document.graph.nodes.front().parameters.end()
                && restored != reopenedSequencer->parameters.end()
                && original->text == restored->text,
            "save/reopen should preserve authored array member text byte-for-byte");
    }

    const auto discovered = talas::project::discoverProjectEventValueControls(
        reopened.document, registry);
    require(discovered.ok(), "typed and timed array members should infer controls");
    const auto expectControl = [&](std::string_view name, ValueDomain domain) {
        const auto* control = findControl(discovered.missingControls, name);
        require(control != nullptr && control->domain == domain,
            "AnalogSequencer array member should infer its parameter domain");
    };
    expectControl("ArrayDuration", ValueDomain::Seconds);
    expectControl("TimedDuration", ValueDomain::Scalar);
    expectControl("ArrayDuty", ValueDomain::Scalar);
    expectControl("TimedDuty", ValueDomain::Scalar);
    expectControl("ArrayPitch", ValueDomain::NoteNumber);
    expectControl("TimedPitch", ValueDomain::Scalar);
    expectControl("ArrayVelocity", ValueDomain::Scalar);
    expectControl("TimedVelocity", ValueDomain::Scalar);
    expectControl("ArrayDown", ValueDomain::Scalar);
    expectControl("TimedDown", ValueDomain::Scalar);
    expectControl("ArrayKeyPitch", ValueDomain::NoteNumber);
    expectControl("ArrayKeyVelocity", ValueDomain::Scalar);
    expectControl("ArrayTimbre", ValueDomain::Scalar);
    expectControl("ExtraValue", ValueDomain::Scalar);
    require(discovered.missingControls.size() == 14,
        "built-in key events and ExtraValues publication targets must not become missing controls");

    for (const auto eventName : {
             "ArrayDuration", "TimedDuration", "ArrayDuty", "TimedDuty",
             "ArrayPitch", "TimedPitch", "ArrayVelocity", "TimedVelocity",
             "ArrayDown", "TimedDown", "ArrayKeyPitch", "ArrayKeyVelocity",
             "ArrayTimbre", "ExtraValue"})
    {
        require(talas::project::projectReferencesEventValue(
                    reopened.document, registry, eventName),
            "text-array EventValue members should count as project references");
    }
    require(!talas::project::projectReferencesEventValue(
                reopened.document, registry, "PublishedScalar"),
        "ExtraValues leading publication names are targets, not member references");
}

void testAnalogSequencerArrayDiscoveryPreservesReusableSoundScope()
{
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.variableLibrary.variables = {
        {"Gain", "Project Gain", ValueDomain::NoteNumber, 60.0,
         {talas::project::ProjectVariableOwnerKind::Project, {}, {}}, true},
        {"Gain", "Voice Gain", ValueDomain::Scalar, 0.25,
         {talas::project::ProjectVariableOwnerKind::SoundDefinition, "voice", {}}, true},
    };
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {textParameter("pitches", "!LocalPitch 60 nn"),
         textParameter("dutyCycles", "{@Gain + !ScopedControl}")},
    });
    talas::project::ProjectSoundExposedParameter localPitch;
    localPitch.parameterId = "pitch";
    localPitch.displayName = "Pitch";
    localPitch.kind = talas::core::ParameterKind::Expression;
    localPitch.domain = ValueDomain::NoteNumber;
    localPitch.defaultValue = expression("pitch", "60 nn");
    localPitch.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {}, {}, "LocalPitch", ValueDomain::NoteNumber});
    voice.exposedParameters.push_back(std::move(localPitch));
    document.soundDefinitions.push_back(std::move(voice));
    document.graph.nodes.push_back({
        "global-sequence", "talas.analogSequencer",
        {textParameter("keyPitches", "!GlobalPitch 67 nn")},
    });
    talas::project::reconcileProjectPresentation(document);

    const auto discovered = talas::project::discoverProjectEventValueControls(
        document, registry);
    require(discovered.ok()
            && discovered.missingControls.size() == 2
            && findControl(discovered.missingControls, "GlobalPitch") != nullptr
            && findControl(discovered.missingControls, "ScopedControl") != nullptr
            && findControl(discovered.missingControls, "ScopedControl")->domain
                == ValueDomain::Scalar
            && findControl(discovered.missingControls, "LocalPitch") == nullptr,
        "reusable Sound array members should retain local EventValue scope and nearest project-variable scope");
    require(!talas::project::projectReferencesEventValue(
                document, registry, "LocalPitch")
            && talas::project::projectReferencesEventValue(
                document, registry, "ScopedControl")
            && talas::project::projectReferencesEventValue(
                document, registry, "GlobalPitch"),
        "local reusable Sound array references must not leak into project controls");
}

void testAnalogSequencerTextReferencesUsePrefixerAndReplicatorNames()
{
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto makeWrapped = [](std::string wrapperType,
                                std::string wrapperId,
                                std::vector<talas::project::ProjectParameterExpression>
                                    wrapperParameters) {
        talas::project::ProjectDocument document;
        document.graph.nodes = {
            {"output", "talas.audioOutput", {}},
            {wrapperId, std::move(wrapperType),
             std::move(wrapperParameters)},
            {"sequence", "talas.analogSequencer",
             {textParameter("keyPitches", "!ArrayPitch"),
              textParameter("extraValues",
                  "#(!Published { !ArrayExtra })")}},
            {"source", "talas.oscillator",
             {expression("frequency", "440 hz"),
              expression("amplitude", "1")}},
        };
        document.graph.connections = {
            {"source", "audioOut", "sequence", "audioIn"},
            {"sequence", "leftOut", wrapperId, "audioIn"},
            {wrapperId, "audioOut", "output", "audioIn"},
        };
        talas::project::reconcileProjectPresentation(document);
        return document;
    };

    const auto prefixed = makeWrapped(
        "talas.prefixer", "prefix", {textParameter("id", "Lead")});
    const auto prefixedDiscovery =
        talas::project::discoverProjectEventValueControls(prefixed, registry);
    const auto prefixExpansion =
        talas::project::expandProjectSounds(prefixed, registry);
    const auto expandedPrefixSequence = std::ranges::find_if(
        prefixExpansion.document.graph.nodes,
        [](const auto& node) {
            return node.typeId == "talas.analogSequencer";
        });
    bool prefixExtraValuesRenamed = false;
    if (prefixExpansion.ok()
        && expandedPrefixSequence != prefixExpansion.document.graph.nodes.end())
    {
        const auto extraValues = std::ranges::find(
            expandedPrefixSequence->parameters,
            std::string_view{"extraValues"},
            &talas::project::ProjectParameterExpression::parameterId);
        prefixExtraValuesRenamed = extraValues != expandedPrefixSequence->parameters.end()
            && extraValues->text
                == std::optional<std::string>{
                    "#(!LeadPublished { !LeadArrayExtra })"};
    }
    require(prefixedDiscovery.ok()
            && prefixedDiscovery.missingControls.size() == 2
            && findControl(prefixedDiscovery.missingControls, "LeadArrayPitch")
                != nullptr
            && findControl(prefixedDiscovery.missingControls, "LeadArrayPitch")->domain
                == ValueDomain::NoteNumber
            && findControl(prefixedDiscovery.missingControls, "LeadArrayExtra")
                != nullptr
            && findControl(prefixedDiscovery.missingControls, "LeadPublished")
                == nullptr
            && prefixExtraValuesRenamed
            && talas::project::projectReferencesEventValue(
                prefixed, registry, "LeadArrayPitch")
            && talas::project::projectReferencesEventValue(
                prefixed, registry, "LeadArrayExtra")
            && !talas::project::projectReferencesEventValue(
                prefixed, registry, "LeadPublished")
            && !talas::project::projectReferencesEventValue(
                prefixed, registry, "ArrayPitch"),
        "Prefixer should rename AnalogSequencer array references and ExtraValues targets together");

    const auto replicated = makeWrapped(
        "talas.replicator", "replicate",
        {expression("number", "2"), textParameter("prefix", "Voice1"),
         textParameter("suffix", "")});
    const auto replicatedDiscovery =
        talas::project::discoverProjectEventValueControls(replicated, registry);
    require(replicatedDiscovery.ok()
            && replicatedDiscovery.missingControls.size() == 4
            && findControl(replicatedDiscovery.missingControls,
                "Voice1ArrayPitch") != nullptr
            && findControl(replicatedDiscovery.missingControls,
                "Voice2ArrayPitch") != nullptr
            && findControl(replicatedDiscovery.missingControls,
                "Voice1ArrayExtra") != nullptr
            && findControl(replicatedDiscovery.missingControls,
                "Voice2ArrayExtra") != nullptr
            && findControl(replicatedDiscovery.missingControls,
                "ArrayPitch") == nullptr,
        "Replicator should rename AnalogSequencer Text-array references per voice");
}

void testAnalogSequencerTextOverrideWinsOverReusableSoundDefault()
{
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes = {
        {"source", "talas.oscillator",
         {expression("frequency", "440 hz"), expression("amplitude", "1")}},
        {"sequence", "talas.analogSequencer",
         {textParameter("pitches", "!DefaultPitch")}},
    };
    voice.graph.connections.push_back({
        "source", "audioOut", "sequence", "audioIn"});
    voice.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "sequence", "leftOut"});
    talas::project::ProjectSoundExposedParameter pitches;
    pitches.parameterId = "pitches";
    pitches.displayName = "Pitches";
    pitches.kind = talas::core::ParameterKind::Text;
    // Text parameters have no domain of their own. The bound Analog Sequencer
    // field supplies NoteNumber semantics to the discovered member.
    pitches.domain = ValueDomain::Scalar;
    pitches.defaultValue = textParameter("pitches", "!DefaultPitch");
    pitches.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "sequence", "pitches", {}, ValueDomain::NoteNumber});
    voice.exposedParameters.push_back(std::move(pitches));

    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice-instance";
    instance.soundDefinitionId = "voice";
    instance.parameters.push_back(
        textParameter("pitches", "!OverridePitch"));
    document.graph.nodes = {
        std::move(instance), {"output", "talas.audioOutput", {}},
    };
    document.graph.connections.push_back({
        "voice-instance", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(document);

    const auto discovered = talas::project::discoverProjectEventValueControls(
        document, registry);
    require(discovered.ok()
            && discovered.missingControls.size() == 1
            && findControl(discovered.missingControls, "OverridePitch") != nullptr
            && findControl(discovered.missingControls, "OverridePitch")->domain
                == ValueDomain::NoteNumber
            && findControl(discovered.missingControls, "DefaultPitch") == nullptr,
        "an instantiated reusable Sound Text override should replace its default array references");
    require(talas::project::projectReferencesEventValue(
                document, registry, "OverridePitch")
            && !talas::project::projectReferencesEventValue(
                document, registry, "DefaultPitch"),
        "reference guards should use effective Text overrides and omit overridden defaults");
}

void testExpressionOverrideUsesItsAuthoredVariableScope()
{
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({
        "source", "talas.oscillator",
        {expression("frequency", "440 hz"), expression("amplitude", "0.5")},
    });
    voice.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "source", "audioOut"});
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.kind = talas::core::ParameterKind::Expression;
    gain.domain = ValueDomain::Scalar;
    gain.defaultValue = expression("gain", "0.5");
    gain.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "source", "amplitude", {}, ValueDomain::Scalar});
    voice.exposedParameters.push_back(std::move(gain));

    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.variableLibrary.variables = {
        {"Drive", "Project Drive", ValueDomain::Scalar, 0.25,
         {talas::project::ProjectVariableOwnerKind::Project, {}, {}}, true},
        {"Drive", "Voice Drive", ValueDomain::NoteNumber, 60.0,
         {talas::project::ProjectVariableOwnerKind::SoundDefinition, "voice", {}}, true},
    };
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice-instance";
    instance.soundDefinitionId = "voice";
    instance.parameters.push_back(
        expression("gain", "@Drive + !OverrideGain"));
    document.graph.nodes = {
        std::move(instance), {"output", "talas.audioOutput", {}},
    };
    document.graph.connections.push_back({
        "voice-instance", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(document);

    const auto discovered = talas::project::discoverProjectEventValueControls(
        document, registry);
    require(discovered.ok()
            && discovered.missingControls.size() == 1
            && findControl(discovered.missingControls, "OverrideGain") != nullptr
            && findControl(discovered.missingControls, "OverrideGain")->domain
                == ValueDomain::Scalar,
        "an exposed Expression override should resolve variables in the caller's lexical scope");
    require(talas::project::projectReferencesEventValue(
                document, registry, "OverrideGain"),
        "reference guards should honor the source scope of exposed Expression overrides");
}

void testAnalogSequencerDiscoveryUsesParentSoundVariableFallback()
{
    using talas::expr::ValueDomain;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSoundDefinition inner;
    inner.definitionId = "inner";
    inner.displayName = "Inner";
    inner.graph.nodes = {
        {"source", "talas.oscillator",
         {expression("frequency", "440 hz"), expression("amplitude", "1")}},
        {"sequence", "talas.analogSequencer",
         {textParameter("dutyCycles", "{@Drive + !NestedControl}")}},
    };
    inner.graph.connections.push_back({
        "source", "audioOut", "sequence", "audioIn"});
    inner.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "sequence", "leftOut"});

    talas::project::ProjectSoundDefinition outer;
    outer.definitionId = "outer";
    outer.displayName = "Outer";
    talas::project::ProjectNode child;
    child.instanceId = "inner-instance";
    child.soundDefinitionId = "inner";
    outer.graph.nodes.push_back(std::move(child));
    outer.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "inner-instance", "audioOut"});

    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.variableLibrary.variables = {
        {"Drive", "Project Drive", ValueDomain::NoteNumber, 60.0,
         {talas::project::ProjectVariableOwnerKind::Project, {}, {}}, true},
        {"Drive", "Outer Drive", ValueDomain::Scalar, 0.25,
         {talas::project::ProjectVariableOwnerKind::SoundDefinition, "outer", {}}, true},
    };
    document.soundDefinitions = {std::move(inner), std::move(outer)};
    talas::project::ProjectNode instance;
    instance.instanceId = "outer-instance";
    instance.soundDefinitionId = "outer";
    document.graph.nodes = {
        std::move(instance), {"output", "talas.audioOutput", {}},
    };
    document.graph.connections.push_back({
        "outer-instance", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(document);

    const auto discovered = talas::project::discoverProjectEventValueControls(
        document, registry);
    require(discovered.ok()
            && discovered.missingControls.size() == 1
            && findControl(discovered.missingControls, "NestedControl") != nullptr
            && findControl(discovered.missingControls, "NestedControl")->domain
                == ValueDomain::Scalar,
        "an instantiated nested Sound should resolve a visible parent-Sound variable before project fallback");
}

void testAnalogSequencerTextDiscoveryRespectsFrozenProfilesAndMalformedText()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument frozen;
    frozen.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV2;
    frozen.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {textParameter("pitches", "{!FrozenRamp ramp: 10 ms} 60 nn")},
    });
    const auto frozenDiscovery =
        talas::project::discoverProjectEventValueControls(frozen, registry);
    require(frozenDiscovery.ok() && frozenDiscovery.missingControls.empty()
            && !talas::project::projectReferencesEventValue(
                frozen, registry, "FrozenRamp"),
        "VariablesV2 array members must keep rejecting later timed expression syntax");

    talas::project::ProjectDocument malformed;
    malformed.graph.nodes.push_back({
        "sequence", "talas.analogSequencer", {
            textParameter("durations", "!MalformedDuration +"),
            textParameter("extraValues", "#(!PublishedScalar not-a-number)"),
        },
    });
    const auto malformedDiscovery =
        talas::project::discoverProjectEventValueControls(malformed, registry);
    require(malformedDiscovery.ok()
            && malformedDiscovery.missingControls.empty()
            && !talas::project::projectReferencesEventValue(
                malformed, registry, "MalformedDuration")
            && !talas::project::projectReferencesEventValue(
                malformed, registry, "PublishedScalar"),
        "malformed AnalogSequencer text must not create guessed EventValue references");

    talas::project::ProjectDocument unresolvedVariable;
    unresolvedVariable.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV3;
    unresolvedVariable.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {textParameter("dutyCycles", "{@Missing + !VariableDependent}")},
    });
    const auto unresolvedDiscovery =
        talas::project::discoverProjectEventValueControls(
            unresolvedVariable, registry);
    require(!unresolvedDiscovery.ok()
            && std::ranges::any_of(unresolvedDiscovery.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.eventName == "VariableDependent"
                        && diagnostic.message.find("no visible declaration")
                            != std::string::npos;
                })
            && talas::project::projectReferencesEventValue(
                unresolvedVariable, registry, "VariableDependent"),
        "mixed array references with unresolved variables should diagnose rather than vanish from inference");
}

void testDiscoversControlsInsideReusableSoundDefinitions()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {expression("frequency", "!NestedFrequency")},
        {},
    });
    document.soundDefinitions.push_back(std::move(definition));

    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(
        discovered.ok() && discovered.missingControls.size() == 1
            && discovered.missingControls.front().eventName
                == "NestedFrequency"
            && discovered.missingControls.front().domain
                == talas::expr::ValueDomain::Hertz,
        "Event Value discovery should include reusable Sound definition graphs");
    require(
        talas::project::projectReferencesEventValue(
            document,
            registry,
            "NestedFrequency"),
        "Event Value reference guards should include reusable Sound definitions");
}

void testPrefixerDiscoversReusableAndDormantSoundControls()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({
        "osc", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain"),
        },
    });
    voice.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectSoundDefinition dormant;
    dormant.definitionId = "dormant";
    dormant.displayName = "Dormant";
    dormant.graph.nodes.push_back({
        "osc", "talas.oscillator", {
            expression("frequency", "!DormantFrequency"),
            expression("amplitude", "1"),
        },
    });
    dormant.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});

    talas::project::ProjectNode instance;
    instance.instanceId = "voiceInstance";
    instance.soundDefinitionId = voice.definitionId;
    talas::project::ProjectDocument document;
    document.soundDefinitions = {std::move(voice), std::move(dormant)};
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"prefix", "talas.prefixer", {textParameter("id", "Lead")}},
        std::move(instance),
    };
    document.graph.connections = {
        {"voiceInstance", "audioOut", "prefix", "audioIn"},
        {"prefix", "audioOut", "out", "audioIn"},
    };
    const auto discovered =
        talas::project::discoverProjectEventValueControls(
            document, registry);
    require(discovered.ok()
            && discovered.missingControls.size() == 2
            && findControl(discovered.missingControls, "LeadGain") != nullptr
            && findControl(discovered.missingControls,
                   "DormantFrequency") != nullptr
            && findControl(discovered.missingControls, "Gain") == nullptr,
        "reusable Sound behind Prefixer should expose its effective control while dormant definitions keep their own controls");
    require(talas::project::projectReferencesEventValue(
                document, registry, "LeadGain")
            && !talas::project::projectReferencesEventValue(
                document, registry, "Gain")
            && talas::project::projectReferencesEventValue(
                document, registry, "DormantFrequency"),
        "reference guards should use expanded names for instantiated Sounds and authored names for dormant definitions");

    const auto synchronized =
        talas::project::synchronizeProjectEventValueControls(
            document, registry);
    require(synchronized.ok() && synchronized.addedCount == 2
            && findControl(document.virtualControls, "LeadGain") != nullptr,
        "reusable Sound Prefixer controls should persist in VCS authoring");

    auto internal = document;
    internal.virtualControls.clear();
    auto& internalDefinition = internal.soundDefinitions.front();
    internalDefinition.graph.nodes.push_back({
        "inside", "talas.prefixer", {textParameter("id", "Inner")},
    });
    internalDefinition.graph.connections.push_back({
        "osc", "audioOut", "inside", "audioIn"});
    internalDefinition.outputs.front().nodeId = "inside";
    internal.graph.nodes.erase(
        std::ranges::find_if(
            internal.graph.nodes,
            [](const auto& node) {
                return node.typeId == "talas.prefixer";
            }));
    internal.graph.connections = {
        {"voiceInstance", "audioOut", "out", "audioIn"},
    };
    const auto internalDiscovery =
        talas::project::discoverProjectEventValueControls(
            internal, registry);
    require(internalDiscovery.ok()
            && internalDiscovery.missingControls.size() == 2
            && findControl(
                   internalDiscovery.missingControls, "InnerGain")
                   != nullptr
            && findControl(internalDiscovery.missingControls, "Gain")
                   == nullptr,
        "Prefixer inside an instantiated reusable Sound should expose its effective control");
    require(talas::project::projectReferencesEventValue(
                internal, registry, "InnerGain")
            && !talas::project::projectReferencesEventValue(
                internal, registry, "Gain"),
        "reference guards should see Prefixer inside an instantiated definition");

    auto internalDefault = internal;
    internalDefault.soundDefinitions.front().graph.nodes.back()
        .parameters.clear();
    const auto internalDefaultDiscovery =
        talas::project::discoverProjectEventValueControls(
            internalDefault, registry);
    require(internalDefaultDiscovery.ok()
            && findControl(
                internalDefaultDiscovery.missingControls, "oscGain")
                != nullptr
            && findControl(
                internalDefaultDiscovery.missingControls, "VoiceGain")
                == nullptr,
        "Prefixer inside a reusable Sound should default to its direct input name");
}

void testUnconnectedRootNodesKeepControlsBesideSoundInstances()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({
        "osc", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Gain"),
        },
    });
    voice.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "pitch";
    frequency.displayName = "Pitch";
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("pitch", "1 hz");
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc", "frequency", {}, talas::expr::ValueDomain::Scalar});
    voice.exposedParameters.push_back(std::move(frequency));

    talas::project::ProjectNode wired;
    wired.instanceId = "wired";
    wired.soundDefinitionId = voice.definitionId;
    talas::project::ProjectNode spare;
    spare.instanceId = "spare";
    spare.soundDefinitionId = voice.definitionId;
    spare.parameters.push_back(expression("pitch", "!SparePitch"));
    talas::project::ProjectDocument document;
    document.soundDefinitions = {std::move(voice)};
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        std::move(wired),
        std::move(spare),
        {"loose", "talas.oscillator", {
            expression("frequency", "!LooseFrequency"),
            expression("amplitude", "1"),
        }},
    };
    document.graph.connections = {
        {"wired", "audioOut", "out", "audioIn"},
    };

    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(discovered.ok()
            && findControl(discovered.missingControls, "Gain") != nullptr
            && findControl(
                   discovered.missingControls, "LooseFrequency") != nullptr
            && findControl(
                   discovered.missingControls, "SparePitch") != nullptr,
        "unconnected root nodes beside a Sound instance should keep their controls");
    require(talas::project::projectReferencesEventValue(
                document, registry, "LooseFrequency")
            && talas::project::projectReferencesEventValue(
                document, registry, "SparePitch"),
        "a control used only by an unconnected node must not look unused");
}

void testDeclaredLocalSoundEventValuesStayScopedWhileOverridesDiscoverGlobals()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
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
        {expression("frequency", "!LocalPitch")},
        {},
    });
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expression("frequency", "120 hz");
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "LocalPitch",
        talas::expr::ValueDomain::Hertz,
    });
    definition.exposedParameters.push_back(std::move(frequency));
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode instance;
    instance.instanceId = "voiceA";
    instance.soundDefinitionId = "voice";
    instance.parameters.push_back(
        expression("frequency", "!GlobalPitch"));
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(
        {"output", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"voiceA", "audioOut", "output", "audioIn"});

    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(
        discovered.ok()
            && discovered.missingControls.size() == 1
            && discovered.missingControls.front().eventName
                == "GlobalPitch"
            && discovered.missingControls.front().domain
                == talas::expr::ValueDomain::Hertz,
        "declared local Event Values should stay scoped while explicit Sound overrides discover their global sources");
    require(
        !talas::project::projectReferencesEventValue(
            document,
            registry,
            "LocalPitch")
            && talas::project::projectReferencesEventValue(
                document,
                registry,
                "GlobalPitch"),
        "global reference guards should exclude declared local bindings and include Sound-instance override sources");

    auto withPrefixer = document;
    withPrefixer.graph.nodes.push_back({
        "extra", "talas.oscillator", {
            expression("frequency", "1 hz"),
            expression("amplitude", "!Extra"),
        },
    });
    withPrefixer.graph.nodes.push_back({
        "prefix", "talas.prefixer", {textParameter("id", "Lead")},
    });
    withPrefixer.graph.connections.push_back({
        "extra", "audioOut", "prefix", "audioIn"});
    withPrefixer.graph.connections.push_back({
        "prefix", "audioOut", "output", "rightIn"});
    const auto expandedDiscovery =
        talas::project::discoverProjectEventValueControls(
            withPrefixer, registry);
    require(expandedDiscovery.ok()
            && expandedDiscovery.missingControls.size() == 2
            && findControl(
                   expandedDiscovery.missingControls, "GlobalPitch")
                   != nullptr
            && findControl(
                   expandedDiscovery.missingControls, "LeadExtra")
                   != nullptr
            && findControl(
                   expandedDiscovery.missingControls, "LocalPitch")
                   == nullptr
            && findControl(
                   expandedDiscovery.missingControls, "Extra")
                   == nullptr,
        "Prefixer discovery should retain unrelated local Sound bindings and global overrides");
    require(talas::project::projectReferencesEventValue(
                withPrefixer, registry, "GlobalPitch")
            && talas::project::projectReferencesEventValue(
                withPrefixer, registry, "LeadExtra")
            && !talas::project::projectReferencesEventValue(
                withPrefixer, registry, "LocalPitch")
            && !talas::project::projectReferencesEventValue(
                withPrefixer, registry, "Extra"),
        "effective reference guards should preserve local Sound scope while applying Prefixer");
}

void testInfersScaledCoupledAndAmbiguousDomains()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();

    const auto scaled = talas::project::discoverProjectEventValueControls(
        oscillatorDocument({
            expression("frequency", "!PitchControl * 12 nn + 60 nn"),
        }),
        registry);
    require(scaled.ok() && scaled.missingControls.size() == 1
                && scaled.missingControls[0].domain
                    == talas::expr::ValueDomain::Scalar,
            "unit-scaled normalized control should infer scalar");

    const auto coupled = talas::project::discoverProjectEventValueControls(
        oscillatorDocument({
            expression("frequency", "!Base + !Offset"),
        }),
        registry);
    require(coupled.ok() && coupled.missingControls.size() == 2,
            "coupled frequency references should be solvable");
    require(coupled.missingControls[0].domain
                == talas::expr::ValueDomain::Hertz
            && coupled.missingControls[1].domain
                == talas::expr::ValueDomain::Hertz,
            "both operands of a frequency sum should infer hertz");

    const auto ambiguous = talas::project::discoverProjectEventValueControls(
        oscillatorDocument({
            expression("amplitude", "!Numerator / !Denominator"),
        }),
        registry);
    require(ambiguous.ok() && ambiguous.missingControls.size() == 2,
            "ambiguous ratio should still produce controls");
    require(ambiguous.missingControls[0].domain
                == talas::expr::ValueDomain::Scalar
            && ambiguous.missingControls[1].domain
                == talas::expr::ValueDomain::Scalar,
            "ambiguous ratio should prefer normalized scalar controls");
}

void testExistingAndTimelineDomainsAreAuthoritative()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto existing = oscillatorDocument({
        expression("frequency", "!Frequency"),
    });
    auto preserved =
        talas::project::makeDefaultVirtualControl(
            "Frequency",
            talas::expr::ValueDomain::Hertz,
            talas::project::VirtualControlKind::Toggle);
    preserved.value = 880.0;
    preserved.minimum = 100.0;
    preserved.maximum = 1000.0;
    preserved.midiMapping = talas::project::MidiControlMapping{2, 74};
    existing.virtualControls.push_back(preserved);
    talas::project::reconcileProjectPresentation(existing);
    talas::project::findProjectPerformanceControlPresentation(
        existing,
        "Frequency",
        talas::expr::ValueDomain::Hertz)->position = {320, 48};

    const auto synchronized =
        talas::project::synchronizeProjectEventValueControls(existing, registry);
    require(synchronized.ok() && synchronized.addedCount == 0,
            "existing authoritative control should not be duplicated");
    require(existing.virtualControls.front().kind
                == talas::project::VirtualControlKind::Toggle
            && existing.virtualControls.front().value == 880.0
            && existing.virtualControls.front().midiMapping.has_value()
            && existing.virtualControls.front().midiMapping->channel == 2
            && existing.virtualControls.front().midiMapping->controller == 74
            && talas::project::findProjectPerformanceControlPresentation(
                   existing,
                   "Frequency",
                   talas::expr::ValueDomain::Hertz)->position.x == 320,
            "synchronization should preserve all existing control metadata");

    auto timeline = oscillatorDocument({
        expression("frequency", "!Sweep"),
    });
    timeline.timeline.eventValues.push_back({
        1.0,
        "Sweep",
        talas::expr::ValueDomain::Hertz,
        660.0,
    });
    const auto timelineDiscovery =
        talas::project::discoverProjectEventValueControls(timeline, registry);
    require(timelineDiscovery.ok()
                && timelineDiscovery.missingControls.size() == 1
                && timelineDiscovery.missingControls[0].domain
                    == talas::expr::ValueDomain::Hertz,
            "Timeline domain should constrain a generated visible control");
}

void testConflictsDiagnoseAndMalformedExpressionsAreNotGuessed()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto conflict = oscillatorDocument({
        expression("amplitude", "!Gain"),
    });
    conflict.virtualControls.push_back(talas::project::makeDefaultVirtualControl(
        "Gain",
        talas::expr::ValueDomain::Hertz));

    const auto conflicted =
        talas::project::discoverProjectEventValueControls(conflict, registry);
    require(!conflicted.ok() && !conflicted.diagnostics.empty(),
            "existing domain incompatible with an expression should diagnose");
    require(conflicted.diagnostics.front().nodeId == "osc"
                && conflicted.diagnostics.front().parameterId == "amplitude",
            "domain conflict should identify its node and parameter");

    const auto malformed = talas::project::discoverProjectEventValueControls(
        oscillatorDocument({
            expression("amplitude", "!Broken +"),
        }),
        registry);
    require(malformed.ok() && malformed.missingControls.empty(),
            "malformed expressions should be left to compile diagnostics without guessed controls");
}

void testInferenceIgnoresNumericWitnessCoincidencesAndCapsDepth()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto coincidentalZero =
        talas::project::discoverProjectEventValueControls(
            oscillatorDocument({
                expression("amplitude", "1 / (!Gain - 0.5)"),
            }),
            registry);
    require(coincidentalZero.ok()
                && coincidentalZero.missingControls.size() == 1
                && coincidentalZero.missingControls.front().domain
                    == talas::expr::ValueDomain::Scalar,
            "one unlucky numeric witness must not reject a valid scalar domain");

    const auto distinctNames =
        talas::project::discoverProjectEventValueControls(
            oscillatorDocument({
                expression(
                    "amplitude",
                    "1 / (!Event97 - !Event122)"),
            }),
            registry);
    require(distinctNames.ok()
                && distinctNames.missingControls.size() == 2,
            "distinct Event Value names must receive distinct inference witnesses");

    std::string wideExpression = "!Event0";
    for (int index = 1; index < 65; ++index)
        wideExpression += " + !Event" + std::to_string(index);
    auto legacyWideDocument = oscillatorDocument({
        expression("amplitude", std::move(wideExpression)),
    });
    legacyWideDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    const auto tooDeep =
        talas::project::discoverProjectEventValueControls(
            legacyWideDocument,
            registry);
    require(!tooDeep.ok()
                && tooDeep.diagnostics.front().message.find("reference limit")
                    != std::string::npos,
            "domain inference must reject excessive recursion depth with a bounded diagnostic");
}

void testPureV4DefinitionReferencesParticipateInDiscovery()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = oscillatorDocument({
        expression("amplitude", "affine(!Base)"),
    });
    document.expressionDefinitions.definitions.push_back({
        "affine",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value + !Step",
    });
    const auto discovered =
        talas::project::discoverProjectEventValueControls(
            document,
            registry);
    require(
        discovered.ok()
            && discovered.missingControls.size() == 2
            && discovered.missingControls[0].eventName == "Base"
            && discovered.missingControls[1].eventName == "Step"
            && discovered.missingControls[0].domain
                == talas::expr::ValueDomain::Scalar
            && discovered.missingControls[1].domain
                == talas::expr::ValueDomain::Scalar,
        "PureV4 definition bodies and arguments should retain exact Event Value discovery authority");
}

// Event Values of certain names start with ranges
// other than (0,1), their case aside, a name ending in * standing for the
// names it begins: the Virtual Control Surface's new faders take them,
// while a project's controls not yet made keep (0,1) and 0 as it plays.
// A Smalltalk program in a field gives its hot
// value as the Sound compiles, so faderMaker's !fader06 shows up on the
// surface as a new fader.
void testAFieldsProgramMakesItsFader()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = oscillatorDocument({
        expression("frequency", "1 hz"),
        expression("amplitude", "| faderMaker | \"A block to construct a fader!\"\n"
                                "faderMaker := [ :str :nbr | | nbrString |\n"
                                "nbr < 10 ifTrue: [nbrString := '0', nbr printString] ifFalse: [nbrString := nbr printString].\n"
                                "(str, nbrString) asHotValue].\n"
                                "faderMaker value: 'fader' value: 6"),
    });
    const auto synchronized = talas::project::synchronizeProjectEventValueControls(document, registry);
    require(synchronized.ok() && findControl(document.virtualControls, "fader06") != nullptr,
        "the program's hot value !fader06 should show up on the surface as a new fader");
}

// A SoundToGlobalController's GeneratedEvent
// renamed !Result shows on the surface, so the result of its calculation
// is displayed there.
void testAGeneratedEventShowsOnTheSurface()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    talas::project::ProjectNode controller;
    controller.instanceId = "calc";
    controller.typeId = "talas.soundToGlobalController";
    talas::project::ProjectParameterExpression generated;
    generated.parameterId = "generatedEvent";
    generated.text = "!Result";
    talas::project::ProjectParameterExpression value;
    value.parameterId = "value";
    value.text = "0.5";
    controller.parameters = {generated, value};
    document.graph.nodes.push_back(controller);
    const auto synchronized = talas::project::synchronizeProjectEventValueControls(document, registry);
    require(synchronized.ok() && findControl(document.virtualControls, "Result") != nullptr,
        "the GeneratedEvent !Result should show on the surface");
}

// A Val Event Value's name is not case
// sensitive; the Virtual Control Surface gives !SnailsburyTails +
// !snaiLsBuryTailS one fader, named for the first it saw.
void testNamesWithoutCase()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = oscillatorDocument({
        expression("frequency", "1 hz"),
        expression("amplitude", "!SnailsburyTails + !snaiLsBuryTailS + !SNAILSBURYTAILS"),
    });
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    const auto synchronized = talas::project::synchronizeProjectEventValueControls(document, registry);
    require(synchronized.ok() && document.virtualControls.size() == 1
            && document.virtualControls.front().eventName == "SnailsburyTails",
        "one fader should stand for the name however it is cased, named for the first spelling");
}

void testNamesGiveTheirEventValuesRanges()
{
    using talas::project::documentedEventValueRange;
    const auto range = [](std::string_view name) { return documentedEventValueRange(name); };
    require(range("Interval") == std::pair{-24.0, 24.0} && range("interval7") == std::pair{-24.0, 24.0}
            && range("SmallInterval") == std::pair{0.0, 12.0} && range("GainDB") == std::pair{-128.0, 128.0}
            && range("Gain") == std::pair{0.0, 10.0} && range("BPM") == std::pair{0.0, 2000.0}
            && range("Beats") == std::pair{1.0, 16.0} && range("BoostCut2") == std::pair{-12.0, 12.0}
            && range("FreqLow") == std::pair{0.0, 1000.0} && range("Frequency") == std::pair{0.0, 10000.0}
            && range("TimeConstant") == std::pair{0.0001, 5.0} && range("ThreshDB") == std::pair{-60.0, 0.0}
            && range("Swing") == std::pair{0.0, 0.5} && range("Fader7") == std::nullopt
            && range("Sw01") == std::nullopt && range("Amp") == std::nullopt,
        "names should give their documented ranges, case aside");
    require(documentedEventValueRange("FreqHigh", 44100.0) == std::pair{8000.0, 22050.0},
        "!FreqHigh should reach half the sample rate");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = oscillatorDocument({
        expression("frequency", "1 hz"),
        expression("amplitude", "!Interval + !GainDB + !Beats + !BoostCut3 + !Fader7"),
    });
    const auto playing = talas::project::discoverProjectEventValueControls(document, registry);
    const auto* beats = findControl(playing.missingControls, "Beats");
    require(playing.ok() && beats != nullptr && beats->minimum == 0.0 && beats->maximum == 1.0 && beats->value == 0.0,
        "a control not yet made should play from 0 in (0,1)");
    const auto synchronized = talas::project::synchronizeProjectEventValueControls(document, registry);
    const auto made = [&](std::string_view name, double minimum, double maximum, double value) {
        const auto* control = findControl(document.virtualControls, name);
        return control != nullptr && control->minimum == minimum && control->maximum == maximum && control->value == value;
    };
    require(synchronized.ok() && made("Interval", -24.0, 24.0, 0.0) && made("GainDB", -128.0, 128.0, 0.0)
            && made("Beats", 1.0, 16.0, 1.0) && made("BoostCut3", -12.0, 12.0, 0.0) && made("Fader7", 0.0, 1.0, 0.0),
        "the Virtual Control Surface's new faders should take their names' ranges");
}

void testDefaultControlsCoverEveryDomainAndKind()
{
    const auto scalar = talas::project::makeDefaultVirtualControl(
        "Scalar",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Gate);
    require(scalar.kind == talas::project::VirtualControlKind::Gate
                && scalar.value == 0.0 && scalar.minimum == 0.0
                && scalar.maximum == 1.0,
            "scalar gate defaults should be normalized");

    const auto hertz = talas::project::makeDefaultVirtualControl(
        "Frequency",
        talas::expr::ValueDomain::Hertz);
    require(hertz.value == 440.0 && hertz.minimum == 20.0
                && hertz.maximum == 20000.0,
            "hertz defaults should be immediately audible and editable");

    const auto seconds = talas::project::makeDefaultVirtualControl(
        "Duration",
        talas::expr::ValueDomain::Seconds);
    require(seconds.value == 0.0 && seconds.maximum == 10.0,
            "seconds defaults should cover a practical range");

    const auto note = talas::project::makeDefaultVirtualControl(
        "Pitch",
        talas::expr::ValueDomain::NoteNumber);
    require(note.value == 60.0 && note.maximum == 127.0,
            "note-number defaults should use middle C and MIDI range");

    const auto samples = talas::project::makeDefaultVirtualControl(
        "Offset",
        talas::expr::ValueDomain::Samples);
    require(samples.value == 0.0 && samples.maximum == 48000.0,
            "sample defaults should cover one nominal second");
}

void testBuiltInMidiButtonsAndSwitchesDiscoverTheirControlKinds()
{
    using talas::project::VirtualControlKind;
    const auto parsedButton =
        talas::project::parseBuiltInMidiControllerName("bt00");
    const auto parsedSwitch =
        talas::project::parseBuiltInMidiControllerName("sw127");
    require(parsedButton && parsedButton->number == 0
            && parsedButton->kind == VirtualControlKind::Gate
            && parsedSwitch && parsedSwitch->number == 127
            && parsedSwitch->kind == VirtualControlKind::Toggle,
        "built-in MIDI aliases must retain controller number and VCS type");
    for (const auto invalid : {"bt0", "sw007", "bt128", "sw12x",
             "Bt00", "bt00_ch02"})
        require(!talas::project::parseBuiltInMidiControllerName(invalid),
            "noncanonical built-in MIDI aliases must remain ordinary names");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = oscillatorDocument({
        expression("frequency", "100 hz"),
        expression("amplitude", "(!bt00 + !bt127 + !sw00 + !sw127) / 4"),
    });
    const auto discovered =
        talas::project::discoverProjectEventValueControls(document, registry);
    require(discovered.ok() && discovered.missingControls.size() == 4,
        "all four endpoint aliases must be discovered as scalar controls");
    for (const auto& control : discovered.missingControls)
        require(control.domain == talas::expr::ValueDomain::Scalar
                && control.minimum == 0.0 && control.maximum == 1.0
                && control.kind
                    == (control.eventName.starts_with("bt")
                        ? VirtualControlKind::Gate
                        : VirtualControlKind::Toggle),
            "btNN must be momentary and swNN must be a checkbox");
    const auto synchronized =
        talas::project::synchronizeProjectEventValueControls(
            document, registry);
    require(synchronized.ok() && document.virtualControls.size() == 4,
        "authoring must persist built-in button and checkbox controls");
}
}

int main()
{
    testDiscoversStableScalarAndDirectDomainControls();
    testNamesGiveTheirEventValuesRanges();
    testNamesWithoutCase();
    testAGeneratedEventShowsOnTheSurface();
    testPrefixerDiscoversEffectiveRootEventValues();
    testAnalogSequencerGeneratedIndexDiscoveryAndRenameScopes();
    testDiscoversChangingCharacteristicArrayMembers();
    testDiscoversAnalogSequencerTextArrayMembersAndReopensThem();
    testAnalogSequencerArrayDiscoveryPreservesReusableSoundScope();
    testAnalogSequencerTextReferencesUsePrefixerAndReplicatorNames();
    testAnalogSequencerTextOverrideWinsOverReusableSoundDefault();
    testExpressionOverrideUsesItsAuthoredVariableScope();
    testAnalogSequencerDiscoveryUsesParentSoundVariableFallback();
    testAnalogSequencerTextDiscoveryRespectsFrozenProfilesAndMalformedText();
    testDiscoversControlsInsideReusableSoundDefinitions();
    testPrefixerDiscoversReusableAndDormantSoundControls();
    testUnconnectedRootNodesKeepControlsBesideSoundInstances();
    testDeclaredLocalSoundEventValuesStayScopedWhileOverridesDiscoverGlobals();
    testInfersScaledCoupledAndAmbiguousDomains();
    testExistingAndTimelineDomainsAreAuthoritative();
    testConflictsDiagnoseAndMalformedExpressionsAreNotGuessed();
    testInferenceIgnoresNumericWitnessCoincidencesAndCapsDepth();
    testPureV4DefinitionReferencesParticipateInDiscovery();
    testDefaultControlsCoverEveryDomainAndKind();
    testBuiltInMidiButtonsAndSwitchesDiscoverTheirControlKinds();
    testAFieldsProgramMakesItsFader();
    return 0;
}
