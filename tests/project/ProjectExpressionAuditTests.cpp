#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/NodeRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
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

talas::project::ProjectParameterExpression text(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression value;
    value.parameterId = std::move(id);
    value.text = std::move(source);
    return value;
}

void testAuditWalksEveryPersistedExpressionScope()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "root",
        "talas.constant",
        {expression("value", "[")},
    });
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back({
        "nested",
        "talas.constant",
        {expression("value", "native(1)")},
    });
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "gain";
    exposed.displayName = "Gain";
    exposed.defaultValue = expression("gain", "[1,2]");
    definition.exposedParameters.push_back(std::move(exposed));
    document.soundDefinitions.push_back(std::move(definition));

    const auto audit = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        !audit.ok() && !audit.truncated
            && audit.diagnostics.size() == 3,
        "project audit should report every expression-bearing scope");
    require(
        audit.diagnostics[0].path
                == "$.graph.nodes[0].parameters[0].expression"
            && audit.diagnostics[1].path
                == "$.soundDefinitions[0].graph.nodes[0].parameters[0].expression"
            && audit.diagnostics[2].path
                == "$.soundDefinitions[0].parameters[0].default.expression",
        "project audit should retain stable canonical paths");

    const auto persistence =
        talas::project::validateProjectDocumentForPersistence(document);
    require(
        !persistence.ok()
            && persistence.diagnostics.front().code
                == talas::project::ProjectPersistenceValidationCode::
                    InvalidExpression,
        "persistence preflight should reject expression failures before writing");
}

void testDialectOverrideIsExplicit()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "root",
        "talas.constant",
        {expression("value", "abs(-2)")},
    });
    require(
        talas::project::auditProjectExpressions(
            document,
            talas::expr::ExpressionLanguageVersion::PureV2).ok(),
        "PureV2 audit should accept V2 functions");
    const auto legacy = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::LegacyV1);
    require(
        !legacy.ok() && !legacy.diagnostics.empty(),
        "LegacyV1 audit should reject the same V2-only source");
}

void testProjectDiagnosticBudgetIsFixed()
{
    talas::project::ProjectDocument document;
    for (std::size_t index = 0;
         index <= talas::project::maximumProjectExpressionDiagnostics;
         ++index)
    {
        document.graph.nodes.push_back({
            "node" + std::to_string(index),
            "talas.constant",
            {expression("value", "@")},
        });
    }
    const auto audit = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        !audit.ok() && audit.truncated
            && audit.diagnostics.size()
                == talas::project::maximumProjectExpressionDiagnostics,
        "project expression diagnostics should stop at the fixed limit");
}

void testRegistryAuditIncludesUnmaterializedDefaults()
{
    std::string largeLegacyExpression = "1";
    for (int index = 0; index < 140; ++index)
        largeLegacyExpression += "+1";

    talas::core::NodeRegistry registry;
    talas::core::NodeTypeDescriptor descriptor;
    descriptor.typeId = "test.large-default";
    descriptor.displayName = "Large default";
    descriptor.parameters.push_back({
        "value",
        largeLegacyExpression,
        talas::expr::ValueDomain::Scalar,
    });
    registry.addNodeType(std::move(descriptor));

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "implicit",
        "test.large-default",
        {},
    });
    require(
        talas::project::auditProjectExpressions(
            document,
            talas::expr::ExpressionLanguageVersion::PureV2).ok(),
        "persisted-only audit should not invent descriptor defaults");
    const auto effective = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        !effective.ok() && effective.diagnostics.size() == 1
            && effective.diagnostics.front().path
                == "$.graph.nodes[0].parameters.value.default"
            && effective.diagnostics.front().code
                == talas::expr::Diagnostic::Code::BudgetExceeded,
        "live upgrade audit should include exact unmaterialized defaults");
}

void testRuntimeTextExpressionsAndStaticFailuresAreAudited()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression envelope;
    envelope.parameterId = "envelope";
    envelope.text = "{ 1 s } { native(1) }";
    document.graph.nodes.push_back({
        "envelope",
        "talas.graphicalEnvelope",
        {std::move(envelope)},
    });
    document.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expression("value", "[1][99]")},
    });

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto audit = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        !audit.ok() && audit.diagnostics.size() == 2,
        "upgrade audit should include runtime text and static evaluation failures");
    require(
        audit.diagnostics[0].path
                == "$.graph.nodes[0].parameters[0].text"
            && audit.diagnostics[0].range.offset == 10
            && audit.diagnostics[0].code
                == talas::expr::Diagnostic::Code::UnknownFunction,
        "embedded text diagnostics should retain the exact project path and source offset");
    require(
        audit.diagnostics[1].path
                == "$.graph.nodes[1].parameters[0].expression"
            && audit.diagnostics[1].code
                == talas::expr::Diagnostic::Code::InvalidIndex,
        "static list-index failures should be rejected before staging");
}

void testEveryDialectOwnedRuntimeTextContractIsAudited()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"midiController",
         "talas.midiOutputController",
         {text("value", "{native(1)}")}},
        {"midiBytes",
         "talas.midiOutputEventInBytes",
         {text("bytes", "144\f{native(1)}")}},
        {"characteristic",
         "talas.inputOutputCharacteristic",
         {text("inValues", "{native(1)}")}},
        {"waveshaper",
         "talas.waveshaper",
         {text("coefficients", "native(1)")}},
        {"fir", "talas.firFilter", {text("coefficients", "native(1)")}},
        {"controller",
         "talas.soundToGlobalController",
         {text("value", "native(1)")}},
    };

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "text-default";
    definition.displayName = "Text default";
    definition.graph.nodes.push_back(
        {"shape", "talas.waveshaper", {}});
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "shape";
    exposed.displayName = "Shape";
    exposed.kind = talas::core::ParameterKind::Text;
    exposed.defaultValue = text("shape", "native(1)");
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
    instance.soundDefinitionId = "text-default";
    instance.parameters.push_back(text("shape", "native(1)"));
    document.graph.nodes.push_back(std::move(instance));

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto audit = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        !audit.ok() && audit.diagnostics.size() == 8,
        "every runtime Text parser, exposed default, and Sound override should share the dialect audit");
    for (const auto& diagnostic : audit.diagnostics)
    {
        require(
            diagnostic.code == talas::expr::Diagnostic::Code::UnknownFunction,
            "each omitted runtime Text contract should retain its parser diagnostic");
    }
    require(
        audit.diagnostics.back().path
            == "$.soundDefinitions[0].parameters[0].default.text",
        "exposed Text defaults should retain their canonical project path");
    require(
        audit.diagnostics[6].path
            == "$.graph.nodes[6].parameters[0].text",
        "Sound-instance Text overrides should resolve through the bound internal runtime parameter");
}

void testInputOutputCharacteristicValArrayTextAudit()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "mapper",
        "talas.inputOutputCharacteristic",
        {text("inValues", "#(-1 !Middle 1)"),
         text("outValues", "#(-1 0.5 1)")},
    });
    const auto language = talas::expr::ExpressionLanguageVersion::PureV2;
    require(talas::project::auditProjectExpressions(document, language).ok(),
        "fixed-length scalar EventValue arrays should pass project text audit");

    document.graph.nodes[0].parameters[0].text = "#(-1 !Middle broken)";
    const auto malformed = talas::project::auditProjectExpressions(
        document, language);
    require(!malformed.ok() && malformed.diagnostics.size() == 1
            && malformed.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].text",
        "malformed Val arrays should fail at their authored text path");
}

void testAnalogSequencerOptionalArraysRemainEmptyAcrossProfiles()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto language : {
             talas::expr::ExpressionLanguageVersion::VariablesV2,
             talas::expr::ExpressionLanguageVersion::VariablesV4})
    {
        talas::project::ProjectDocument document;
        document.expressionLanguage = language;
        document.graph.nodes.push_back({
            "sequence", "talas.analogSequencer",
            {text("durations", "{0.25 s}"), text("dutyCycles", "{0.5}"),
             text("pitches", ""), text("velocities", "{0.5}"),
             text("keyDowns", " \t"), text("keyPitches", ""),
             text("keyVelocities", ""), text("keyTimbres", ""),
             text("extraValues", "")},
        });
        require(talas::project::auditProjectExpressions(
                    document, registry, language).ok(),
            "empty optional AnalogSequencer arrays must preserve amplitude mode");
        const auto saved = talas::project::serializeProjectDocument(document);
        const auto reopened = talas::project::parseProjectDocument(saved, registry);
        require(reopened.ok()
                && talas::project::serializeProjectDocument(reopened.document) == saved,
            "inactive empty arrays must survive project save and reopen");

        document.graph.nodes.front().parameters[2].text = "{}";
        const auto malformed = talas::project::auditProjectExpressions(
            document, registry, language);
        require(!malformed.ok() && malformed.diagnostics.front().path
                    == "$.graph.nodes[0].parameters[2].text",
            "an empty expression in an active array must still reject");
        document.graph.nodes.front().parameters[2].text = "";
        document.graph.nodes.front().parameters[0].text = "";
        require(!talas::project::auditProjectExpressions(
                    document, registry, language).ok(),
            "required durations must not become an optional empty array");
    }
}

void testAnalogSequencerBareArraysPersistWithExactDiagnostics()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    document.variableLibrary.variables = {{"length", "Length", talas::expr::ValueDomain::Scalar, 20, {}, true}};
    document.graph.nodes.push_back({"sequence", "talas.analogSequencer",
        {text("durations", "  10ms !Length ms {@length ms} 0.03"),
         text("dutyCycles", "0.5 !Duty {1 / 2}"), text("keyDowns", "1 !Down"),
         text("keyPitches", "60 !Pitch {61 nn + 1 nn}"),
         text("keyVelocities", "1 !Velocity"), text("keyTimbres", "0 !Timbre")}});
    require(talas::project::auditProjectExpressions(document, registry, document.expressionLanguage).ok(),
        "bare typed numbers, HotValues and compound members share the expression grammar");
    const auto saved = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved, registry);
    require(reopened.ok() && talas::project::serializeProjectDocument(reopened.document, registry) == saved,
        "mixed bare and brace array spelling survives exact project round-trip");
    document.graph.nodes.front().parameters[0].text = " 10 ms {  20 unknownUnit }";
    const auto invalid = talas::project::auditProjectExpressions(document, registry, document.expressionLanguage);
    require(!invalid.ok() && invalid.diagnostics.front().path == "$.graph.nodes[0].parameters[0].text"
            && invalid.diagnostics.front().range.offset == 13,
        "a compound unit diagnostic retains its offset in the original mixed array");
    document.graph.nodes.front().parameters[0].text = "10 ms !Length";
    require(talas::project::auditProjectExpressions(document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV2).ok(),
        "bare HotValues do not require the newer reference-unit grammar");
    document.graph.nodes.front().parameters[0].text = "10 ms !Length ms";
    require(!talas::project::auditProjectExpressions(document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV2).ok(),
        "bare authoring must preserve frozen reference-unit syntax restrictions");
}

void testAnalogSequencerVariableArraysPersistAndAudit()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV4;
    document.variableLibrary.variables = {
        {"duration", "Duration", talas::expr::ValueDomain::Seconds,
         0.01, {}, true},
        {"duty", "Duty", talas::expr::ValueDomain::Scalar,
         0.5, {}, true},
        {"down", "Down", talas::expr::ValueDomain::Scalar,
         1.0, {}, true},
        {"pitch", "Pitch", talas::expr::ValueDomain::NoteNumber,
         60.0, {}, true},
        {"velocity", "Velocity", talas::expr::ValueDomain::Scalar,
         0.75, {}, true},
        {"timbre", "Timbre", talas::expr::ValueDomain::Scalar,
         0.25, {}, true},
        {"morph", "Morph", talas::expr::ValueDomain::Scalar,
         0.4, {}, true},
    };
    document.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {text("durations", "{@duration}"),
         text("dutyCycles", "{@duty}"),
         text("keyDowns", "{@down}"),
         text("keyPitches", "{@pitch}"),
         text("keyVelocities", "{@velocity}"),
         text("keyTimbres", "{@timbre}"),
         text("extraValues", "#(!Morph {@morph})")},
    });

    const auto audit = talas::project::auditProjectExpressions(
        document, registry,
        talas::expr::ExpressionLanguageVersion::VariablesV4);
    require(audit.ok(),
        "VariablesV4 project audit should accept authored variable references in AnalogSequencer arrays");

    const auto saved = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved, registry);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document, registry)
                == saved
            && talas::project::auditProjectExpressions(
                reopened.document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV4).ok(),
        "AnalogSequencer variable array text should survive project save, reopen and audit");
    const auto& parameters = reopened.document.graph.nodes.front().parameters;
    require(parameters[0].text == "{@duration}"
            && parameters[3].text == "{@pitch}"
            && parameters[6].text == "#(!Morph {@morph})",
        "project persistence should retain exact authored variable-reference bytes");
}

void testAnalogSequencerClockedStatefulArraysAreProfileGatedAndAudited()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {text("durations", "{10 ms}"),
         text("dutyCycles", "{0.5}"),
         text("keyDowns", "{1 ramp: 4 ms}"),
         text("keyPitches", "{60 nn}"),
         text("keyVelocities", "{1}"),
         text("keyTimbres", "{0}"),
         text("extraValues", "#(!Morph {1 ramp: 4 ms})")},
    });

    require(talas::project::auditProjectExpressions(
                document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV3).ok(),
        "clocked Ramp members should pass the VariablesV3 AnalogSequencer audit");
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV4;
    require(talas::project::auditProjectExpressions(
                document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV4).ok(),
        "VariablesV4 should retain timed AnalogSequencer member support");

    const auto saved = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved, registry);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document, registry)
                == saved
            && talas::project::auditProjectExpressions(
                reopened.document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV4).ok(),
        "clocked AnalogSequencer member text should survive save, reopen and audit");
    const auto& parameters = reopened.document.graph.nodes.front().parameters;
    require(parameters[2].text == "{1 ramp: 4 ms}"
            && parameters[6].text == "#(!Morph {1 ramp: 4 ms})",
        "project persistence should preserve the exact timed member source bytes");

    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    const auto frozen = talas::project::auditProjectExpressions(
        document, registry,
        talas::expr::ExpressionLanguageVersion::VariablesV2);
    require(!frozen.ok()
            && std::ranges::any_of(frozen.diagnostics, [](const auto& diagnostic) {
                return diagnostic.path
                    == "$.graph.nodes[0].parameters[2].text";
            })
            && std::ranges::any_of(frozen.diagnostics, [](const auto& diagnostic) {
                return diagnostic.path
                    == "$.graph.nodes[0].parameters[6].text";
            }),
        "frozen VariablesV2 should reject ramp: at both persisted array paths");

    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV3;
    document.graph.nodes.front().parameters[2].text =
        "{previous(!Arm, 0)}";
    const auto unclockedKey = talas::project::auditProjectExpressions(
        document, registry,
        talas::expr::ExpressionLanguageVersion::VariablesV3);
    require(!unclockedKey.ok()
            && unclockedKey.diagnostics.front().path
                == "$.graph.nodes[0].parameters[2].text"
            && unclockedKey.diagnostics.front().code
                == talas::expr::Diagnostic::Code::MissingState,
        "unclocked stateful note-array members should keep an exact audit diagnostic");

    document.graph.nodes.front().parameters[2].text = "{1}";
    document.graph.nodes.front().parameters[6].text =
        "#(!Morph {previous(!Arm, 0)})";
    const auto unclockedExtra = talas::project::auditProjectExpressions(
        document, registry,
        talas::expr::ExpressionLanguageVersion::VariablesV3);
    require(!unclockedExtra.ok()
            && std::ranges::any_of(unclockedExtra.diagnostics, [](const auto& diagnostic) {
                return diagnostic.path
                        == "$.graph.nodes[0].parameters[6].text"
                    && diagnostic.code
                        == talas::expr::Diagnostic::Code::MissingState;
            }),
        "unclocked stateful ExtraValues members should remain rejected");
}

void testAnalogSequencerGeneratedIndexIsAuditedAsATargetName()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {text("generatedIndex", "  !Stage7  ")},
    });
    for (const auto language : {
             talas::expr::ExpressionLanguageVersion::LegacyV1,
             talas::expr::ExpressionLanguageVersion::VariablesV4,
         })
    {
        require(talas::project::auditProjectExpressions(
                    document, registry, language).ok(),
            "a valid GeneratedIndex target should not be parsed as a value expression");
    }
    document.graph.nodes.front().parameters.front().text = " \t\r\n";
    require(talas::project::auditProjectExpressions(
                document, registry,
                talas::expr::ExpressionLanguageVersion::VariablesV4).ok(),
        "whitespace-only GeneratedIndex text should remain an empty target");

    for (const auto& invalid : {
             std::string{"Stage"}, std::string{"!Stage Other"},
             std::string{"!"} + std::string(129, 'A'),
             std::string{"!Pitch"}, std::string{"!KeyPitch"},
             std::string{"!KeyNumber"}, std::string{"!KeyDown"},
             std::string{"!KeyVelocity"}, std::string{"!KeyTimbre"},
         })
    {
        document.graph.nodes.front().parameters.front().text = invalid;
        const auto audit = talas::project::auditProjectExpressions(
            document, registry,
            talas::expr::ExpressionLanguageVersion::VariablesV4);
        require(!audit.ok() && audit.diagnostics.size() == 1
                && audit.diagnostics.front().path
                    == "$.graph.nodes[0].parameters[0].text",
            "malformed and reserved GeneratedIndex targets should fail at their authored text path");
    }
}

void testDynamicGlobalControllerFallbackOwnsEventOnlyTextAuthority()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "controller",
        "talas.soundToGlobalController",
        {text("value", "{!Control * 2}")},
    });
    const auto eventAudit = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::PureV2);
    require(
        eventAudit.ok(),
        "C48 project audit should admit bounded Scalar Event Value fallback text");

    document.variableLibrary.variables.push_back({
        "gain",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        0.5,
        {},
        true,
    });
    document.graph.nodes.front().parameters.front().text =
        "{!Control * @gain}";
    const auto variableAudit =
        talas::project::auditProjectExpressions(
            document,
            talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !variableAudit.ok()
            && variableAudit.diagnostics.size() == 1
            && variableAudit.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].text"
            && variableAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::UnknownVariable,
        "C48 embedded fallback should reject project-variable authority even when the declaration exists");
}

void testPureV3SeriesIsAuditedAcrossProjectAndSoundTextScopes()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "rootSeries",
        "talas.constant",
        {expression("value", "series(1, 2, 4)[2]")},
    });
    document.graph.nodes.push_back({
        "midiSeries",
        "talas.midiOutputController",
        {text("value", "{series(0, 0.25, 4)[3]}")},
    });

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "series-sound";
    definition.displayName = "Series Sound";
    definition.graph.nodes.push_back({
        "shape",
        "talas.waveshaper",
        {},
    });
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "shape";
    exposed.displayName = "Shape";
    exposed.kind = talas::core::ParameterKind::Text;
    exposed.defaultValue = text(
        "shape",
        "series(0.1,0.1,4)[2]");
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
    instance.instanceId = "seriesInstance";
    instance.soundDefinitionId = "series-sound";
    instance.parameters.push_back(text(
        "shape",
        "series(0.2,0.1,4)[1]"));
    document.graph.nodes.push_back(std::move(instance));

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto pureV3 = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV3);
    require(
        pureV3.ok(),
        "PureV3 audit should accept series in root, embedded Text, exposed defaults, and Sound overrides"
            + (pureV3.diagnostics.empty()
                   ? std::string{}
                   : ": " + pureV3.diagnostics.front().path + " "
                       + pureV3.diagnostics.front().message));
    require(
        !talas::project::auditProjectExpressions(
             document,
             registry,
             talas::expr::ExpressionLanguageVersion::PureV2).ok(),
        "the same project must fail under frozen PureV2 authority");
}

void testSessionStateIsAcceptedOnlyByLiveExpressionConsumers()
{
    talas::project::ProjectDocument liveDocument;
    liveDocument.graph.nodes.push_back({
        "live",
        "talas.constant",
        {expression("value", "previous(!Input, 0)")},
    });

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto liveAudit = talas::project::auditProjectExpressions(
        liveDocument,
        registry,
        talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        liveAudit.ok(),
        "SessionV1 should admit state only for a live Event Value expression"
            + (liveAudit.diagnostics.empty()
                   ? std::string{}
                   : ": " + liveAudit.diagnostics.front().path + " "
                       + liveAudit.diagnostics.front().message));

    talas::project::ProjectDocument staticDocument;
    staticDocument.graph.nodes.push_back({
        "shape",
        "talas.waveshaper",
        {text("coefficients", "previous(1,0)")},
    });
    const auto staticAudit = talas::project::auditProjectExpressions(
        staticDocument,
        registry,
        talas::expr::ExpressionLanguageVersion::SessionV1);
    const auto staticDetail = staticAudit.diagnostics.empty()
        ? std::string{" no diagnostic"}
        : " path=" + staticAudit.diagnostics.front().path
            + " range="
            + std::to_string(staticAudit.diagnostics.front().range.offset)
            + ":"
            + std::to_string(staticAudit.diagnostics.front().range.length)
            + " code="
            + std::to_string(static_cast<int>(
                staticAudit.diagnostics.front().code));
    require(
        !staticAudit.ok() && staticAudit.diagnostics.size() == 1
            && staticAudit.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].text"
            && staticAudit.diagnostics.front().range.offset == 0
            && staticAudit.diagnostics.front().range.length == 13
            && staticAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::MissingState,
        "a static Text consumer should reject Session state before staging with its exact path and range"
            + staticDetail);

    talas::project::ProjectDocument midiDocument;
    midiDocument.graph.nodes.push_back({
        "midi",
        "talas.midiOutputEventInBytes",
        {text("bytes", "144 {previous(!Input,0)}")},
    });
    const auto midiAudit = talas::project::auditProjectExpressions(
        midiDocument,
        registry,
        talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        !midiAudit.ok() && midiAudit.diagnostics.size() == 1
            && midiAudit.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].text"
            && midiAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::MissingState,
        "a MIDI-byte expression with Event references should still reject state without a live clock");

    talas::project::ProjectDocument exposedDocument;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "gain";
    exposed.displayName = "Gain";
    exposed.domain = talas::expr::ValueDomain::Scalar;
    exposed.defaultValue = expression(
        "gain",
        "previous(!Input,0)");
    exposed.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "ProjectedGain",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(exposed));
    exposedDocument.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode projectedInstance;
    projectedInstance.instanceId = "projectedVoice";
    projectedInstance.soundDefinitionId = "voice";
    projectedInstance.parameters.push_back(
        expression("gain", "previous(!Input,0)"));
    exposedDocument.graph.nodes.push_back(std::move(projectedInstance));
    const auto exposedAudit = talas::project::auditProjectExpressions(
        exposedDocument,
        registry,
        talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        !exposedAudit.ok() && exposedAudit.diagnostics.size() == 2
            && exposedAudit.diagnostics[0].path
                == "$.graph.nodes[0].parameters[0].expression"
            && exposedAudit.diagnostics[0].code
                == talas::expr::Diagnostic::Code::MissingState
            && exposedAudit.diagnostics[1].path
                == "$.soundDefinitions[0].parameters[0].default.expression"
            && exposedAudit.diagnostics[1].code
                == talas::expr::Diagnostic::Code::MissingState,
        "Sound Event Value projections should reject state in defaults and instance overrides");
    const talas::core::ParameterDescriptor gainDescriptor{
        "gain", "0", talas::expr::ValueDomain::Scalar};
    const auto authoredProjectionAudit =
        talas::project::auditProjectParameterExpression(
            exposedDocument,
            exposedDocument.graph.nodes.front(),
            exposedDocument.graph.nodes.front().parameters.front(),
            gainDescriptor,
            talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        !authoredProjectionAudit.ok()
            && authoredProjectionAudit.diagnostics.size() == 1
            && authoredProjectionAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::MissingState,
        "single-parameter authoring must reject stateful Event Value projections");

    talas::project::ProjectDocument nodeBoundDocument;
    talas::project::ProjectSoundDefinition nodeBoundDefinition;
    nodeBoundDefinition.definitionId = "statefulVoice";
    nodeBoundDefinition.displayName = "Stateful voice";
    nodeBoundDefinition.graph.nodes.push_back({
        "gainNode",
        "talas.constant",
        {},
    });
    talas::project::ProjectSoundExposedParameter nodeBoundExposed;
    nodeBoundExposed.parameterId = "gain";
    nodeBoundExposed.displayName = "Gain";
    nodeBoundExposed.domain = talas::expr::ValueDomain::Scalar;
    nodeBoundExposed.defaultValue =
        expression("gain", "previous(!Input,0)");
    nodeBoundExposed.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "gainNode",
        "value",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    nodeBoundDefinition.exposedParameters.push_back(
        std::move(nodeBoundExposed));
    nodeBoundDocument.soundDefinitions.push_back(
        std::move(nodeBoundDefinition));
    talas::project::ProjectNode nodeBoundInstance;
    nodeBoundInstance.instanceId = "statefulVoiceInstance";
    nodeBoundInstance.soundDefinitionId = "statefulVoice";
    nodeBoundInstance.parameters.push_back(
        expression("gain", "previous(!Input,0)"));
    nodeBoundDocument.graph.nodes.push_back(std::move(nodeBoundInstance));
    const auto nodeBoundAudit = talas::project::auditProjectExpressions(
        nodeBoundDocument,
        registry,
        talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        nodeBoundAudit.ok(),
        "Sound NodeParameter bindings should retain per-instance stateful expression authority"
            + (nodeBoundAudit.diagnostics.empty()
                   ? std::string{}
                   : ": " + nodeBoundAudit.diagnostics.front().path + " "
                       + nodeBoundAudit.diagnostics.front().message));
    const auto authoredNodeBindingAudit =
        talas::project::auditProjectParameterExpression(
            nodeBoundDocument,
            nodeBoundDocument.graph.nodes.front(),
            nodeBoundDocument.graph.nodes.front().parameters.front(),
            gainDescriptor,
            talas::expr::ExpressionLanguageVersion::SessionV1);
    require(
        authoredNodeBindingAudit.ok(),
        "single-parameter authoring must accept stateful NodeParameter bindings");
}

void testUncallableDefinitionUsesItsExactProjectPath()
{
    talas::project::ProjectDocument document;
    std::string source = "value";
    for (std::size_t index = 1; index < 128; ++index)
        source += "+value";
    document.expressionDefinitions.definitions.push_back({
        "uncallable",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        std::move(source),
    });

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto audit = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV4);
    require(
        !audit.ok() && audit.diagnostics.size() == 1
            && audit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::BudgetExceeded
            && audit.diagnostics.front().path
                == "$.expressionDefinitions[uncallable].source",
        "an uncallable definition should fail at its exact definition body path");
}

void testDefinitionEvaluationFailureUsesItsExactProjectPath()
{
    talas::project::ProjectDocument document;
    document.expressionDefinitions.definitions.push_back({
        "divideByZero",
        {},
        talas::expr::ValueDomain::Scalar,
        "100 / 0",
    });
    document.graph.nodes.push_back({
        "caller",
        "talas.constant",
        {expression("value", "divideByZero()")},
    });

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto audit = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV4);
    require(
        !audit.ok() && audit.diagnostics.size() == 1
            && audit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::DivisionByZero
            && audit.diagnostics.front().path
                == "$.expressionDefinitions[divideByZero].source"
            && audit.diagnostics.front().range.offset == 0
            && audit.diagnostics.front().range.length == 7,
        "an inlined evaluation failure should retain its exact definition body path and range");
}

void testDefinitionResultDomainFailureUsesItsBodyRange()
{
    talas::project::ProjectDocument document;
    document.expressionDefinitions.definitions.push_back({
        "badDomain",
        {},
        talas::expr::ValueDomain::Hertz,
        "100 + 20",
    });
    document.graph.nodes.push_back({
        "caller",
        "talas.constant",
        {expression("value", "badDomain()")},
    });

    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto audit = talas::project::auditProjectExpressions(
        document,
        registry,
        talas::expr::ExpressionLanguageVersion::PureV4);
    require(
        !audit.ok() && audit.diagnostics.size() == 1
            && audit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::UnitMismatch
            && audit.diagnostics.front().path
                == "$.expressionDefinitions[badDomain].source"
            && audit.diagnostics.front().range.offset == 0
            && audit.diagnostics.front().range.length == 8,
        "a result-domain failure should not map its longer caller range onto the definition body");
}

void testVariablesUseExactProjectAndSoundScope()
{
    talas::project::ProjectDocument document;
    document.variableLibrary.variables = {
        {"gain",
         "Gain",
         talas::expr::ValueDomain::Scalar,
         0.5,
         {},
         true},
        {"bias",
         "Bias",
         talas::expr::ValueDomain::Scalar,
         0.0,
         {talas::project::ProjectVariableOwnerKind::SoundDefinition, "voice"},
         true},
    };
    document.graph.nodes.push_back({
        "root",
        "talas.constant",
        {expression("value", "@gain")},
    });
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back({
        "nested",
        "talas.constant",
        {expression("value", "@bias + @gain")},
    });
    document.soundDefinitions.push_back(std::move(definition));

    require(
        talas::project::auditProjectExpressions(
            document,
            talas::expr::ExpressionLanguageVersion::VariablesV1).ok(),
        "project variables and exact Sound-owned variables should resolve in their lexical scopes");
    const auto projectReferences =
        talas::project::findProjectVariableReferencePaths(
            document, {}, "gain");
    const auto soundReferences =
        talas::project::findProjectVariableReferencePaths(
            document,
            {talas::project::ProjectVariableOwnerKind::SoundDefinition,
             "voice"},
            "bias");
    require(
        projectReferences.paths
                == std::vector<std::string>{
                    "$.graph.nodes[0].parameters[0].expression",
                    "$.soundDefinitions[0].graph.nodes[0].parameters[0].expression"}
            && soundReferences.paths
                == std::vector<std::string>{
                    "$.soundDefinitions[0].graph.nodes[0].parameters[0].expression"},
        "variable confirmation authority should expose exact reference paths in lexical scope");

    talas::project::ProjectDocument voiceDocument;
    auto rootVoiceGain = talas::project::ProjectVariableDeclaration{
        "voiceGain",
        "Root voice gain",
        talas::expr::ValueDomain::Scalar,
        0.25,
        {talas::project::ProjectVariableOwnerKind::MidiVoice, {}, "midi"},
        false,
    };
    voiceDocument.variableLibrary.variables.push_back(rootVoiceGain);
    voiceDocument.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"midi", "talas.midiVoice", {}},
        {"osc", "talas.oscillator", {
            expression("amplitude", "@voiceGain"),
        }},
        {"outside", "talas.constant", {
            expression("value", "0.5"),
        }},
    };
    voiceDocument.graph.connections = {
        {"osc", "audioOut", "midi", "audioIn"},
        {"midi", "leftOut", "out", "audioIn"},
    };
    require(
        talas::project::auditProjectExpressions(
            voiceDocument,
            talas::expr::ExpressionLanguageVersion::VariablesV1).ok(),
        "an expression inside an exact MIDI Voice island should resolve its structural owner variable");
    const auto voiceReferences =
        talas::project::findProjectVariableReferencePaths(
            voiceDocument, rootVoiceGain.owner, "voiceGain");
    require(
        voiceReferences.paths
            == std::vector<std::string>{
                "$.graph.nodes[2].parameters[0].expression"},
        "MIDI Voice declaration confirmation should retain only exact upstream-island reference paths");
    const auto voiceBytes =
        talas::project::serializeProjectDocument(voiceDocument);
    const auto voiceRoundTrip =
        talas::project::parseProjectDocument(voiceBytes);
    require(
        voiceRoundTrip.ok()
            && talas::project::serializeProjectDocument(
                   voiceRoundTrip.document) == voiceBytes,
        "format-29 persistence should round-trip an exact MIDI Voice variable reference without weakening its island scope");
    voiceDocument.graph.nodes.back().parameters.front().expression =
        "@voiceGain";
    const auto outsideVoice = talas::project::auditProjectExpressions(
        voiceDocument,
        talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !outsideVoice.ok()
            && outsideVoice.diagnostics.size() == 1
            && outsideVoice.diagnostics.front().path
                == "$.graph.nodes[3].parameters[0].expression",
        "a MIDI Voice variable must remain unavailable outside its exact upstream island");

    talas::project::ProjectDocument definitionVoiceDocument;
    talas::project::ProjectSoundDefinition voiceDefinition;
    voiceDefinition.definitionId = "voice";
    voiceDefinition.displayName = "Voice";
    voiceDefinition.graph.nodes = {
        {"midi", "talas.midiVoice", {}},
        {"osc", "talas.oscillator", {
            expression("amplitude", "@localGain"),
        }},
    };
    voiceDefinition.graph.connections = {
        {"osc", "audioOut", "midi", "audioIn"},
    };
    definitionVoiceDocument.soundDefinitions.push_back(
        std::move(voiceDefinition));
    auto localVoiceGain = rootVoiceGain;
    localVoiceGain.variableId = "localGain";
    localVoiceGain.displayName = "Definition voice gain";
    localVoiceGain.owner.definitionId = "voice";
    definitionVoiceDocument.variableLibrary.variables.push_back(
        localVoiceGain);
    require(
        talas::project::auditProjectExpressions(
            definitionVoiceDocument,
            talas::expr::ExpressionLanguageVersion::VariablesV1).ok()
            && talas::project::findProjectVariableReferencePaths(
                   definitionVoiceDocument,
                   localVoiceGain.owner,
                   localVoiceGain.variableId)
                   .paths
                == std::vector<std::string>{
                    "$.soundDefinitions[0].graph.nodes[1].parameters[0].expression"},
        "definition-local MIDI Voice references should resolve through exact definition and owner-node identity");

    auto definitionReferenceDocument = document;
    definitionReferenceDocument.expressionDefinitions.definitions.push_back({
        "readGain", {}, talas::expr::ValueDomain::Scalar, "@gain"});
    definitionReferenceDocument.graph.nodes.front().parameters.front().expression =
        "readGain()";
    const auto definitionReferences =
        talas::project::findProjectVariableReferencePaths(
            definitionReferenceDocument, {}, "gain");
    require(
        definitionReferences.paths
            == std::vector<std::string>{
                "$.expressionDefinitions[readGain].source",
                "$.soundDefinitions[0].graph.nodes[0].parameters[0].expression"},
        "variable confirmation authority should preserve exact definition origins and every direct reference path");

    auto overrideDocument = document;
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "value";
    exposed.displayName = "Value";
    exposed.defaultValue = expression("value", "@bias");
    exposed.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "nested",
        "value",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    overrideDocument.soundDefinitions.front().exposedParameters.push_back(
        std::move(exposed));
    talas::project::ProjectNode instance;
    instance.instanceId = "voiceInstance";
    instance.soundDefinitionId = "voice";
    instance.parameters.push_back(expression("value", "@bias"));
    overrideDocument.graph.nodes.push_back(std::move(instance));
    const auto overrideAudit = talas::project::auditProjectExpressions(
        overrideDocument,
        talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !overrideAudit.ok()
            && overrideAudit.diagnostics.front().path
                == "$.graph.nodes[1].parameters[0].expression"
            && overrideAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::UnknownVariable,
        "a Sound-instance override must resolve variables in its parent lexical scope rather than the child definition");

    talas::project::ProjectDocument midiVariableDocument;
    midiVariableDocument.variableLibrary.variables.push_back({
        "gain", "Gain", talas::expr::ValueDomain::Scalar, 0.5, {}, true});
    midiVariableDocument.graph.nodes.push_back({
        "midi",
        "talas.midiOutputEventInBytes",
        {text("bytes", "144 {!Fader + @gain}")},
    });
    const auto midiVariableAudit = talas::project::auditProjectExpressions(
        midiVariableDocument,
        talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !midiVariableAudit.ok()
            && midiVariableAudit.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].text"
            && midiVariableAudit.diagnostics.front().code
                == talas::expr::Diagnostic::Code::UnknownVariable
            && midiVariableAudit.diagnostics.front().range.offset == 14
            && midiVariableAudit.diagnostics.front().range.length == 5,
        "MIDI-byte Text must reject the exact project-variable token that its runtime consumer cannot resolve");

    midiVariableDocument.expressionDefinitions.definitions.push_back({
        "readGain", {}, talas::expr::ValueDomain::Scalar, "@gain"});
    midiVariableDocument.graph.nodes.front().parameters.front().text =
        "144 {!Fader + readGain()}";
    const auto midiDefinitionVariableAudit =
        talas::project::auditProjectExpressions(
            midiVariableDocument,
            talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !midiDefinitionVariableAudit.ok()
            && midiDefinitionVariableAudit.diagnostics.front().path
                == "$.expressionDefinitions[readGain].source"
            && midiDefinitionVariableAudit.diagnostics.front().range.offset == 0
            && midiDefinitionVariableAudit.diagnostics.front().range.length == 5,
        "MIDI-byte Text must preserve the exact definition-owned variable origin");

    document.graph.nodes.front().parameters.front().expression = "@bias";
    const auto outOfScope = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !outOfScope.ok() && outOfScope.diagnostics.size() == 1
            && outOfScope.diagnostics.front().code
                == talas::expr::Diagnostic::Code::UnknownVariable
            && outOfScope.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].expression"
            && outOfScope.diagnostics.front().range.offset == 0
            && outOfScope.diagnostics.front().range.length == 5,
        "a Sound-owned variable should fail with the exact root reference range outside its scope");

    document.graph.nodes.front().parameters.front().expression = "@gain";
    document.expressionDefinitions.definitions.push_back({
        "readMissing",
        {},
        talas::expr::ValueDomain::Scalar,
        "@missing",
    });
    document.graph.nodes.front().parameters.front().expression =
        "readMissing()";
    const auto definitionFailure = talas::project::auditProjectExpressions(
        document,
        talas::expr::ExpressionLanguageVersion::VariablesV1);
    require(
        !definitionFailure.ok()
            && definitionFailure.diagnostics.size() == 1
            && definitionFailure.diagnostics.front().path
                == "$.expressionDefinitions[readMissing].source"
            && definitionFailure.diagnostics.front().range.offset == 0
            && definitionFailure.diagnostics.front().range.length == 8,
        "definition-expanded variable failures should retain the exact definition source path and range");
}
void testVoiceNumberAuditRequiresVoiceOrReusableSoundContext()
{
    using namespace talas::project;
    using Language = talas::expr::ExpressionLanguageVersion;
    ProjectDocument document;
    document.expressionLanguage = Language::VoiceV1;
    document.graph.nodes = {{"osc", "talas.oscillator", {expression("frequency", "?VoiceNumber * 7 hz")}}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto outside = auditProjectExpressions(document, registry, Language::VoiceV1);
    require(!outside.ok() && outside.diagnostics.size() == 1
        && outside.diagnostics.front().code == talas::expr::Diagnostic::Code::UnknownIdentifier
        && outside.diagnostics.front().range.offset == 0
        && outside.diagnostics.front().range.length == 12,
        "root concrete parameters require a real voice owner for VoiceNumber");
    require(!auditProjectExpressions(document, Language::VoiceV1).ok(),
        "registry-independent persistence audit must reject unowned VoiceNumber");
    document.graph.nodes.push_back({"voice", "talas.midiVoice", {}});
    document.graph.connections = {{"osc", "audioOut", "voice", "audioIn"}};
    require(auditProjectExpressions(document, registry, Language::VoiceV1).ok(),
        "voice-owned parameter must admit preparation specialization");
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    const auto frequency = std::ranges::find(oscillator->parameters, "frequency", &talas::core::ParameterDescriptor::id);
    require(frequency != oscillator->parameters.end(), "oscillator frequency schema must exist");
    require(auditProjectParameterExpression(document, document.graph.nodes[0],
        document.graph.nodes[0].parameters[0], *frequency, Language::VoiceV1).ok(),
        "single-parameter editor audit must admit the same voice binding");
    document.graph.nodes[0].parameters[0].expression = "?VoiceNumber * 1 s";
    require(!auditProjectExpressions(document, registry, Language::VoiceV1).ok(),
        "voice substitution must retain parameter domain validation");
    require(!auditProjectParameterExpression(document, document.graph.nodes[0],
        document.graph.nodes[0].parameters[0], *frequency, Language::VoiceV1).ok(),
        "single-parameter audit must reject invalid units after voice substitution");
    document.expressionDefinitions.definitions.push_back({"voiceRate", {}, talas::expr::ValueDomain::Hertz,
        "?VoiceNumber * 7 hz"});
    document.graph.nodes[0].parameters[0].expression = "voiceRate()";
    require(auditProjectExpressions(document, registry, Language::VoiceV1).ok(),
        "definition-expanded voice reference must retain its owner");
    document.graph.nodes.pop_back();
    document.graph.connections.clear();
    const auto definitionFailure = auditProjectExpressions(document, registry, Language::VoiceV1);
    require(!definitionFailure.ok() && definitionFailure.diagnostics.front().path
        == "$.expressionDefinitions[voiceRate].source"
        && definitionFailure.diagnostics.front().range.length == 12,
        "unowned definition reference must retain its definition source diagnostic");
    ProjectSoundDefinition sound;
    sound.definitionId = "tone"; sound.displayName = "Tone";
    sound.graph = std::move(document.graph);
    sound.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    ProjectSoundExposedParameter exposed;
    exposed.parameterId = "frequency"; exposed.displayName = "Frequency";
    exposed.domain = talas::expr::ValueDomain::Hertz;
    exposed.defaultValue = expression("frequency", "?VoiceNumber * 3 hz");
    exposed.bindings.push_back({ProjectSoundParameterTarget::NodeParameter, "osc", "frequency"});
    sound.exposedParameters.push_back(std::move(exposed));
    document.graph = {};
    document.soundDefinitions.push_back(std::move(sound));
    require(auditProjectExpressions(document, registry, Language::VoiceV1).ok(),
        "reusable Sound parameters and defaults may retain a voice template until expansion");
    auto& templateNode = document.soundDefinitions.front().graph.nodes.front();
    require(auditProjectParameterExpression(document, templateNode, templateNode.parameters.front(),
        *frequency, Language::VoiceV1).ok(), "Sound-template inspector must preserve deferred voice binding");
}

void testReplicatorVoiceAuditRetainsAuthoredOffsets()
{
    using namespace talas::project;
    ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {
            expression("frequency",
                "(?VoiceNumber + ?NumberVoices) * 1 hz")}},
        {"rep", "talas.replicator", {expression("number", "3")}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "rep", "audioIn"},
        {"rep", "audioOut", "out", "audioIn"},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(auditProjectExpressions(
                document, registry, document.expressionLanguage).ok()
            && auditProjectExpressions(
                document, document.expressionLanguage).ok(),
        "Replicator-owned copy variables should pass both project audits");
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    const auto frequency = std::ranges::find(
        oscillator->parameters, std::string_view{"frequency"},
        &talas::core::ParameterDescriptor::id);
    require(frequency != oscillator->parameters.end()
            && auditProjectParameterExpression(
                   document, document.graph.nodes[0],
                   document.graph.nodes[0].parameters[0],
                   *frequency, document.expressionLanguage).ok(),
        "Replicator-owned editor parameter should admit the same binding");

    document.graph.nodes[0].parameters[0].expression =
        "1 / (?NumberVoices - 1) * 1 hz";
    require(auditProjectExpressions(
                document, registry, document.expressionLanguage).ok(),
        "audit should use the authored static Number when checking total voices");
    document.graph.nodes[1].parameters[0].expression = "1";
    require(!auditProjectExpressions(
                document, registry, document.expressionLanguage).ok(),
        "a one-copy zero denominator should reject during audit");
    document.graph.nodes[1].parameters[0].expression = "3";

    const std::string malformed =
        "(?VoiceNumber + ?NumberVoices) * 1 hz + /";
    document.graph.nodes[0].parameters[0].expression = malformed;
    const auto invalid = auditProjectExpressions(
        document, registry, document.expressionLanguage);
    require(!invalid.ok() && !invalid.diagnostics.empty()
            && invalid.diagnostics.front().path
                == "$.graph.nodes[0].parameters[0].expression"
            && invalid.diagnostics.front().range.offset
                == malformed.find_last_of('/'),
        "voice specialization must preserve source offsets in parse diagnostics");

    document.graph.nodes.erase(document.graph.nodes.begin() + 1);
    document.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"},
    };
    document.graph.nodes[0].parameters[0].expression =
        "?VoiceNumber * 1 hz";
    require(!auditProjectExpressions(
                document, registry, document.expressionLanguage).ok(),
        "copy variables outside Replicator scope must remain invalid");
    document.graph.nodes[0].parameters[0].expression =
        "?NumberVoices * 1 hz";
    require(!auditProjectExpressions(
                document, registry, document.expressionLanguage).ok(),
        "total-voice variables outside Replicator or reusable templates must reject");
}

void testNestedVoiceVariableAuditAndReferenceOwnership()
{
    using namespace talas::project;
    ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    const ProjectVariableOwner parent{ProjectVariableOwnerKind::MidiVoice, {}, "parent"};
    const ProjectVariableOwner child{ProjectVariableOwnerKind::MidiVoice, {}, "child"};
    document.variableLibrary.variables = {
        {"gain", "Project gain", talas::expr::ValueDomain::Scalar, 0.9, {}, true},
        {"gain", "Parent gain", talas::expr::ValueDomain::Scalar, 0.4, parent, false},
    };
    document.graph.nodes = {
        {"tone", "talas.oscillator", {expression("amplitude", "@gain + 0.1")}},
        {"child", "talas.midiVoice", {}},
        {"parent", "talas.midiVoice", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"tone", "audioOut", "child", "audioIn"},
        {"child", "leftOut", "parent", "audioIn"},
        {"parent", "leftOut", "out", "audioIn"},
    };
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    const auto nestedAudit = auditProjectExpressions(document, registry, document.expressionLanguage);
    std::string nestedDiagnostics;
    for (const auto& diagnostic : nestedAudit.diagnostics)
        nestedDiagnostics += "\n" + diagnostic.path + ": " + diagnostic.message;
    require(nestedAudit.ok(),
        "nested voice audit must bind parent variables before project fallbacks"
            + nestedDiagnostics);
    require(findProjectVariableReferencePaths(document, parent, "gain").paths.size() == 1
        && findProjectVariableReferencePaths(document, {}, "gain").paths.empty(),
        "reference discovery must assign an inherited variable to its exact parent owner");
    require(parseProjectDocument(serializeProjectDocument(document)).ok(),
        "nested parent variable references must survive persistence preflight");

    auto numbered = document;
    numbered.expressionLanguage = talas::expr::ExpressionLanguageVersion::VoiceV1;
    numbered.graph.nodes.front().parameters.front().expression = "?VoiceNumber / 10";
    require(auditProjectExpressions(numbered, registry, numbered.expressionLanguage).ok(),
        "nested Input graphs must retain VoiceNumber authority in the voice expression dialect");

    document.variableLibrary.variables.push_back(
        {"gain", "Child gain", talas::expr::ValueDomain::Hertz, 220.0, child, false});
    require(!auditProjectExpressions(document, registry, document.expressionLanguage).ok(),
        "nearest child declaration must shadow a valid parent even when its domain is invalid");
    document.variableLibrary.variables.back().domain = talas::expr::ValueDomain::Scalar;
    document.variableLibrary.variables.back().initialValue = 0.1;
    require(auditProjectExpressions(document, registry, document.expressionLanguage).ok()
        && findProjectVariableReferencePaths(document, parent, "gain").paths.empty()
        && findProjectVariableReferencePaths(document, child, "gain").paths.size() == 1,
        "audit evaluation and reference discovery must agree on nearest child shadowing");
    document.variableLibrary.variables.push_back(
        {"secret", "Parent only", talas::expr::ValueDomain::Scalar, 0.5, parent, false});
    document.graph.nodes.push_back({"outside", "talas.constant", {expression("value", "@secret")}});
    const auto outside = auditProjectExpressions(document, registry, document.expressionLanguage);
    require(!outside.ok() && outside.diagnostics.size() == 1
        && outside.diagnostics.front().path == "$.graph.nodes[4].parameters[0].expression",
        "nested lookup must keep parent-owned declarations invisible outside its Input region");
}

}

int main()
{
    testReplicatorVoiceAuditRetainsAuthoredOffsets();
    testVoiceNumberAuditRequiresVoiceOrReusableSoundContext();
    testAuditWalksEveryPersistedExpressionScope();
    testDialectOverrideIsExplicit();
    testProjectDiagnosticBudgetIsFixed();
    testRegistryAuditIncludesUnmaterializedDefaults();
    testRuntimeTextExpressionsAndStaticFailuresAreAudited();
    testEveryDialectOwnedRuntimeTextContractIsAudited();
    testInputOutputCharacteristicValArrayTextAudit();
    testAnalogSequencerOptionalArraysRemainEmptyAcrossProfiles();
    testAnalogSequencerBareArraysPersistWithExactDiagnostics();
    testAnalogSequencerVariableArraysPersistAndAudit();
    testAnalogSequencerClockedStatefulArraysAreProfileGatedAndAudited();
    testAnalogSequencerGeneratedIndexIsAuditedAsATargetName();
    testDynamicGlobalControllerFallbackOwnsEventOnlyTextAuthority();
    testPureV3SeriesIsAuditedAcrossProjectAndSoundTextScopes();
    testSessionStateIsAcceptedOnlyByLiveExpressionConsumers();
    testUncallableDefinitionUsesItsExactProjectPath();
    testDefinitionEvaluationFailureUsesItsExactProjectPath();
    testDefinitionResultDomainFailureUsesItsBodyRange();
    testVariablesUseExactProjectAndSoundScope();
    testNestedVoiceVariableAuditAndReferenceOwnership();
    return 0;
}
