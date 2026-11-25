#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/VariableLiterals.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using talas::expr::ValueDomain;
using talas::project::ProjectVariableDeclaration;
using talas::project::ProjectVariableOwner;
using talas::project::ProjectVariableOwnerKind;

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

ProjectVariableDeclaration variable(
    std::string id,
    ProjectVariableOwner owner = {})
{
    ProjectVariableDeclaration result;
    result.variableId = std::move(id);
    result.displayName = "Variable";
    result.owner = std::move(owner);
    return result;
}

ProjectVariableOwner soundOwner(std::string definitionId)
{
    return {ProjectVariableOwnerKind::SoundDefinition,
            std::move(definitionId)};
}

ProjectVariableOwner voiceOwner(
    std::string nodeId,
    std::string definitionId = {})
{
    return {
        ProjectVariableOwnerKind::MidiVoice,
        std::move(definitionId),
        std::move(nodeId),
    };
}

ProjectVariableOwner workspaceOwner()
{
    return {ProjectVariableOwnerKind::WorkspaceSession};
}

void testBoundedValidationAndExactOwnerIdentity()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "outer", .displayName = "Outer"},
        {.definitionId = "inner", .displayName = "Inner"},
    };
    document.graph.nodes.push_back({"rootVoice", "talas.midiVoice"});
    document.soundDefinitions[1].graph.nodes.push_back(
        {"innerVoice", "talas.midiVoice"});
    auto rootVoice = variable("pressure", voiceOwner("rootVoice"));
    rootVoice.snapshotEligible = false;
    auto innerVoice = variable(
        "pressure",
        voiceOwner("innerVoice", "inner"));
    innerVoice.snapshotEligible = false;
    document.variableLibrary.variables = {
        variable("gain"),
        variable("sessionBias", workspaceOwner()),
        variable("gain", soundOwner("outer")),
        variable("gain", soundOwner("inner")),
        variable("cutoff", soundOwner("inner")),
        rootVoice,
        innerVoice,
    };
    require(
        talas::project::validateProjectVariableLibrary(document).ok(),
        "valid project and Sound-definition variables should pass authority validation");

    const std::vector<std::string_view> outerScope{"outer"};
    const std::vector<std::string_view> nestedScope{"outer", "inner"};
    const auto* project =
        talas::project::findVisibleProjectVariableDeclaration(document, "gain");
    const auto* outer = talas::project::findVisibleProjectVariableDeclaration(
        document, "gain", outerScope);
    const auto* inner = talas::project::findVisibleProjectVariableDeclaration(
        document, "gain", nestedScope);
    const auto* nestedCutoff =
        talas::project::findVisibleProjectVariableDeclaration(
            document, "cutoff", nestedScope);
    require(
        project != nullptr
            && project->owner.kind == ProjectVariableOwnerKind::Project
            && outer != nullptr
            && outer->owner.definitionId == "outer"
            && inner != nullptr
            && inner->owner.definitionId == "inner"
            && nestedCutoff != nullptr
            && nestedCutoff->owner.definitionId == "inner"
            && talas::project::findVisibleProjectVariableDeclaration(
                   document,
                   "sessionBias")
                != nullptr,
        "resolution should walk exact lexical Sound scope then project scope");
    require(
        talas::project::findVisibleProjectVariableDeclaration(
            document, "cutoff", outerScope) == nullptr
            && talas::project::findVisibleProjectVariableDeclaration(
                document, "Gain", nestedScope) == nullptr,
        "resolution should not use descendant, display-name, or case fallback");
    const auto* rootVoiceValue =
        talas::project::findVisibleProjectVariableDeclaration(
            document, "pressure", {}, &rootVoice.owner);
    const auto* nestedVoiceValue =
        talas::project::findVisibleProjectVariableDeclaration(
            document, "pressure", nestedScope, &innerVoice.owner);
    require(
        rootVoiceValue != nullptr
            && rootVoiceValue->owner == rootVoice.owner
            && nestedVoiceValue != nullptr
            && nestedVoiceValue->owner == innerVoice.owner
            && talas::project::findVisibleProjectVariableDeclaration(
                   document, "pressure", nestedScope) == nullptr,
        "MIDI Voice resolution should require and prioritize the exact structural owner without leaking into lexical scope");

    document.variableLibrary.variables.push_back(
        variable("gain", soundOwner("inner")));
    const auto duplicate =
        talas::project::validateProjectVariableLibrary(document);
    require(
        !duplicate.ok()
            && duplicate.diagnostics.back().code
                == talas::project::ProjectVariableValidationCode::DuplicateDeclaration,
        "duplicates should be rejected only within the same exact owner scope");

    document.variableLibrary.variables.pop_back();
    document.variableLibrary.variables.push_back(
        variable("gain", workspaceOwner()));
    const auto ambiguous =
        talas::project::validateProjectVariableLibrary(document);
    require(
        !ambiguous.ok()
            && std::ranges::any_of(
                ambiguous.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectVariableValidationCode::AmbiguousGlobalOwner;
                }),
        "project and workspace-session owners must not expose one ambiguous global ID");
}

void testMalformedAndOverCapacityLibrariesFailClosed()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "voice", .displayName = "Voice"},
    };
    auto malformed = variable("1 invalid", soundOwner("missing"));
    malformed.displayName = std::string(257, 'x');
    malformed.initialValue = std::numeric_limits<double>::infinity();
    document.variableLibrary.version = 1;
    document.variableLibrary.variables = {std::move(malformed)};
    const auto validation =
        talas::project::validateProjectVariableLibrary(document);
    require(
        !validation.ok() && validation.diagnostics.size() == 5
            && validation.diagnostics[0].path
                == "$.variableLibrary.version"
            && validation.diagnostics[1].path
                == "$.variableLibrary.variables[0].variableId"
            && validation.diagnostics[2].path
                == "$.variableLibrary.variables[0].displayName"
            && validation.diagnostics[3].path
                == "$.variableLibrary.variables[0].initialValue"
            && validation.diagnostics[4].path
                == "$.variableLibrary.variables[0].owner.definitionId",
        "malformed declaration diagnostics should retain stable exact paths");

    document.variableLibrary.version = 2;
    document.variableLibrary.variables.clear();
    document.variableLibrary.variables.reserve(
        talas::project::maximumProjectVariableCount + 1U);
    for (std::size_t index = 0;
         index <= talas::project::maximumProjectVariableCount;
         ++index)
    {
        document.variableLibrary.variables.push_back(
            variable("v" + std::to_string(index)));
    }
    const auto overCapacity =
        talas::project::validateProjectVariableLibrary(document);
    require(
        !overCapacity.ok()
            && overCapacity.diagnostics.size() == 1
            && overCapacity.diagnostics.front().code
                == talas::project::ProjectVariableValidationCode::TooManyVariables,
        "over-capacity libraries should reject before per-declaration traversal");
}

void testIdentifierAndDomainAuthority()
{
    require(
        talas::project::isValidProjectVariableIdentifier("_gain2")
            && !talas::project::isValidProjectVariableIdentifier("")
            && !talas::project::isValidProjectVariableIdentifier("gain-value")
            && !talas::project::isValidProjectVariableIdentifier(
                std::string(129, 'x')),
        "variable IDs should use the bounded expression identifier grammar");
    for (const auto domain : {
             ValueDomain::Scalar,
             ValueDomain::Hertz,
             ValueDomain::Seconds,
             ValueDomain::NoteNumber,
             ValueDomain::Samples})
    {
        require(
            talas::project::isSupportedProjectVariableDomain(domain),
            "every existing finite numeric expression domain should be supported");
    }
}

void testFormatTwentyEightRoundTripMigrationAndSchemaClosure()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    document.soundDefinitions = {
        {.definitionId = "voice", .displayName = "Voice"},
    };
    document.graph.nodes.push_back({"rootVoice", "talas.midiVoice"});
    auto cutoff = variable("cutoff", soundOwner("voice"));
    cutoff.displayName = "Cutoff";
    cutoff.domain = ValueDomain::Hertz;
    cutoff.initialValue = 440.0;
    cutoff.snapshotEligible = false;
    auto gain = variable("gain");
    gain.displayName = "Gain";
    gain.initialValue = 0.25;
    auto session = variable("sessionBias", workspaceOwner());
    session.displayName = "Session Bias";
    session.initialValue = 0.5;
    auto pressure = variable("pressure", voiceOwner("rootVoice"));
    pressure.displayName = "Pressure";
    pressure.snapshotEligible = false;
    document.variableLibrary.variables = {
        gain,
        cutoff,
        session,
        pressure,
    };

    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::Variable;
    gainTarget.variableId = "gain";
    talas::project::SnapshotValue gainValue;
    gainValue.number = 0.75;
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "loud";
    snapshot.displayName = "Loud";
    snapshot.values.push_back({gainTarget, gainValue});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "variables";
    bank.displayName = "Variables";
    bank.snapshots.push_back(snapshot);
    document.snapshotLibrary.banks.push_back(bank);

    talas::project::SnapshotTargetIdentity sessionTarget;
    sessionTarget.kind = talas::project::SnapshotTargetKind::Variable;
    sessionTarget.variableOwnerKind =
        talas::project::SnapshotVariableOwnerKind::WorkspaceSession;
    sessionTarget.variableId = "sessionBias";
    document.snapshotLibrary.banks.front().snapshots.front().values.push_back(
        {sessionTarget, gainValue});

    const auto canonical =
        talas::project::serializeProjectDocument(document);
    require(
        canonical.find(R"json("version": 156)json")
                != std::string::npos
            && canonical.find(
                R"json("expressionLanguage": "variablesV1")json")
                != std::string::npos
            && canonical.find("\"variableLibrary\": {")
                != std::string::npos
            && canonical.find(R"json("version": 2)json")
                != std::string::npos
            && canonical.find(R"json("kind": "midiVoice")json")
                != std::string::npos
            && canonical.find(R"json("kind": "workspaceSession")json")
                != std::string::npos
            && canonical.find(
                R"json("ownerKind": "workspaceSession")json")
                != std::string::npos
            && canonical.find(R"json("variableId": "cutoff")json")
                < canonical.find(R"json("variableId": "gain")json"),
        "format 29 should serialize library-v2 owners and targets canonically");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && reopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV1
            && talas::project::validateProjectVariableLibrary(
                   reopened.document).ok()
            && reopened.document.snapshotLibrary
                == document.snapshotLibrary
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "format 29 variable declarations should reopen byte-stably");

    talas::project::ProjectDocument legacyDocument;
    legacyDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    legacyDocument.soundDefinitions = document.soundDefinitions;
    legacyDocument.variableLibrary.variables = {gain, cutoff};
    auto formatTwentySeven =
        talas::project::serializeProjectDocument(legacyDocument);
    const auto legacyVersion = formatTwentySeven.find(
        R"json("version": 156)json");
    const auto legacyLibraryVersion = formatTwentySeven.find(
        "  \"variableLibrary\": {\n    \"version\": 2,");
    require(
        legacyVersion != std::string::npos
            && legacyLibraryVersion != std::string::npos,
        "legacy migration fixture should expose both schema versions");
    formatTwentySeven.replace(
        legacyVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  27)json");
    formatTwentySeven.replace(
        legacyLibraryVersion,
        std::string_view{
            "  \"variableLibrary\": {\n    \"version\": 2,"}.size(),
        "  \"variableLibrary\": {\n    \"version\": 1,");
    constexpr std::string_view soundLibrary =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    constexpr std::string_view generatorLibrary =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    constexpr std::string_view liveSetLibrary =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto soundLibraryPosition = formatTwentySeven.find(soundLibrary);
    require(
        soundLibraryPosition != std::string::npos,
        "legacy migration fixture should expose the future Sound-variable library");
    formatTwentySeven.erase(soundLibraryPosition, soundLibrary.size());
    const auto generatorLibraryPosition =
        formatTwentySeven.find(generatorLibrary);
    require(
        generatorLibraryPosition != std::string::npos,
        "legacy fixture should expose the future generator library");
    formatTwentySeven.erase(
        generatorLibraryPosition, generatorLibrary.size());
    const auto liveSetLibraryPosition =
        formatTwentySeven.find(liveSetLibrary);
    require(
        liveSetLibraryPosition != std::string::npos,
        "legacy fixture should expose the future live-set library");
    formatTwentySeven.erase(
        liveSetLibraryPosition, liveSetLibrary.size());
    const auto migrated =
        talas::project::parseProjectDocument(formatTwentySeven);
    require(
        migrated.ok() && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.variableLibrary.version == 2
            && migrated.document.variableLibrary.variables
                == std::vector<ProjectVariableDeclaration>{cutoff, gain},
        "format 27 library v1 should migrate losslessly to format 29 library v2");

    auto missing = canonical;
    const auto libraryBegin = missing.find("  \"variableLibrary\": {");
    const auto metadataBegin = missing.find("  \"metadata\":", libraryBegin);
    require(libraryBegin != std::string::npos
            && metadataBegin != std::string::npos,
        "variable schema fixture should expose its root field");
    missing.erase(libraryBegin, metadataBegin - libraryBegin);
    require(
        !talas::project::parseProjectDocument(missing).ok(),
        "format 29 should require explicit variable-library authority");

    auto premature = canonical;
    const auto version = premature.find(R"json("version": 156)json");
    const auto language = premature.find(
        R"json("expressionLanguage": "variablesV1")json");
    require(version != std::string::npos && language != std::string::npos,
        "variable schema fixture should expose format/language authority");
    // The language follows the version, so it is replaced first.
    premature.replace(
        language,
        std::string_view{
            R"json("expressionLanguage": "variablesV1")json"}.size(),
        R"json("expressionLanguage": "sessionV1")json");
    premature.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  26)json");
    const auto prematureSoundLibrary = premature.find(soundLibrary);
    require(
        prematureSoundLibrary != std::string::npos,
        "premature variable fixture should expose the future Sound-variable library");
    premature.erase(prematureSoundLibrary, soundLibrary.size());
    const auto prematureGeneratorLibrary = premature.find(generatorLibrary);
    require(
        prematureGeneratorLibrary != std::string::npos,
        "premature fixture should expose the future generator library");
    premature.erase(prematureGeneratorLibrary, generatorLibrary.size());
    const auto prematureResult =
        talas::project::parseProjectDocument(premature);
    require(
        !prematureResult.ok()
            && prematureResult.errors.front().path
                == "$.variableLibrary",
        "format 26 should reject premature variable-library fields");

    auto unknownField = canonical;
    const auto variableId = unknownField.find(
        "        \"variableId\": \"cutoff\",\n");
    require(variableId != std::string::npos,
        "variable schema fixture should expose a declaration");
    unknownField.insert(
        variableId,
        "        \"unexpected\": true,\n");
    const auto unknownResult =
        talas::project::parseProjectDocument(unknownField);
    require(
        !unknownResult.ok()
            && unknownResult.errors.front().path
                == "$.variableLibrary.variables[0]",
        "variable declarations should reject unknown fields at their exact path");
}

// A variable's value
// typed as a Smalltalk literal prints back as Smalltalk writes it, and a
// field reads it as the literal typed in place of the variable; declared
// variables and numbers in expressions are left to the project's variables.
void testVariableLiteralsReadAndPrint()
{
    using talas::project::parseVariableLiteral;
    using talas::project::variableLiteralText;
    for (const std::string_view typed : {"0.25", "-3", "'it''s'", "#Square21", "#'two words'", "$a", "#(1 0.5 -1)"})
    {
        const auto literal = parseVariableLiteral(typed);
        require(literal.has_value() && variableLiteralText(*literal) == typed,
            "a literal should print back as it was typed");
    }
    require(variableLiteralText(*parseVariableLiteral("{1. 2. 3}")) == "#(1 2 3)"
            && talas::project::variableLiteralFieldText(*parseVariableLiteral("'it''s'")) == "it's",
        "a brace array should print as an Array and a string field read its characters");
    require(talas::project::isArrayFieldText("-1 {?x} ?y 60 nn") && !talas::project::isArrayFieldText("?x collect: [:i | i]")
            && !talas::project::isArrayFieldText(""),
        "an array field holds numbers, braced elements and variables");
    require(talas::project::fieldVariableReferences("-1 {?x * 2} ?y") == std::vector<std::string>{"x", "y"}
            && talas::project::fieldVariableReferences(" ?file ") == std::vector<std::string>{"file"}
            && talas::project::fieldVariableReferences("?x collect: [:i | i]").empty(),
        "an array field's variables and a field wholly a variable should be read, code left alone");

    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression list;
    list.parameterId = "outValues";
    list.text = "?curve";
    talas::project::ProjectParameterExpression gain;
    gain.parameterId = "gain";
    gain.expression = "?level * (0 of: ?curve)";
    talas::project::ProjectParameterExpression script;
    script.parameterId = "script";
    script.text = "note start: 0 s wavetable: ?name. '?name'";
    document.graph.nodes = {{"map", "talas.inputOutputCharacteristic", {list, gain}}, {"score", "talas.script", {script}}};
    const talas::project::VariableLiteralEnvironment environment{{"curve", *parseVariableLiteral("#(1 0 1)")},
        {"level", talas::project::numberLiteral(0.5)}, {"name", *parseVariableLiteral("'Saw21'")}};
    const auto given = talas::project::withVariableLiterals(document, environment);
    require(given.graph.nodes[0].parameters[0].text == "1 0 1"
            && given.graph.nodes[0].parameters[1].expression == "?level * (0 of: #(1 0 1))"
            && given.graph.nodes[1].parameters[0].text == "note start: 0 s wavetable: 'Saw21'. '?name'",
        "each field should read the literal as it reads it typed, numbers left to the project's variables");
    document.variableLibrary.variables.push_back({"curve", "curve", talas::expr::ValueDomain::Scalar, 0.0, {}, true});
    require(talas::project::withVariableLiterals(document, environment).graph.nodes[0].parameters[0].text == "?curve",
        "a declared variable should keep its declaration's value");
    const auto within = talas::project::withVariableEnvironment(document, environment);
    require(within.variableLibrary.variables.size() == 2 && within.variableLibrary.variables[1].variableId == "level"
            && within.variableLibrary.variables[1].initialValue == 0.5,
        "within the environment a number no declaration names should be declared at its value");
}
}

int main()
{
    testBoundedValidationAndExactOwnerIdentity();
    testMalformedAndOverCapacityLibrariesFailClosed();
    testIdentifierAndDomainAuthority();
    testFormatTwentyEightRoundTripMigrationAndSchemaClosure();
    testVariableLiteralsReadAndPrint();
    return 0;
}
