#include "talas/core/BuiltinNodes.h"
#include "talas/project/PreparedProjectSoundVariables.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectSoundVariables.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using talas::project::ProjectSoundVariableDeclaration;
using talas::project::ProjectSoundVariableKind;
using talas::project::ProjectVariableOwner;
using talas::project::ProjectVariableOwnerKind;

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

ProjectSoundVariableDeclaration soundVariable(
    std::string id,
    std::vector<std::string> candidates,
    ProjectVariableOwner owner = {},
    ProjectSoundVariableKind kind = ProjectSoundVariableKind::Sound)
{
    ProjectSoundVariableDeclaration result;
    result.variableId = std::move(id);
    result.displayName = "Sound Variable";
    result.owner = std::move(owner);
    result.kind = kind;
    result.candidateDefinitionIds = std::move(candidates);
    return result;
}

ProjectVariableOwner definitionOwner(std::string definitionId)
{
    return {
        ProjectVariableOwnerKind::SoundDefinition,
        std::move(definitionId),
        {},
    };
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

void testValidationAndLexicalResolution()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
        {.definitionId = "outer", .displayName = "Outer"},
        {.definitionId = "inner", .displayName = "Inner"},
    };
    document.graph.nodes.push_back({"rootVoice", "talas.midiVoice"});
    auto voice = soundVariable(
        "voiceSound",
        {"toneA", "toneB"},
        voiceOwner("rootVoice"));
    voice.snapshotEligible = false;
    document.soundVariableLibrary.variables = {
        soundVariable("selected", {"toneA", "toneB"}),
        soundVariable(
            "selected",
            {"toneB"},
            definitionOwner("outer")),
        soundVariable(
            "selected",
            {},
            definitionOwner("inner"),
            ProjectSoundVariableKind::SoundCollection),
        voice,
    };
    require(
        talas::project::validateProjectSoundVariableLibrary(document).ok(),
        "valid exact Sound-variable declarations should pass");

    const std::vector<std::string_view> outer{"outer"};
    const std::vector<std::string_view> nested{"outer", "inner"};
    const auto* project =
        talas::project::findVisibleProjectSoundVariableDeclaration(
            document,
            "selected");
    const auto* outerValue =
        talas::project::findVisibleProjectSoundVariableDeclaration(
            document,
            "selected",
            outer);
    const auto* innerValue =
        talas::project::findVisibleProjectSoundVariableDeclaration(
            document,
            "selected",
            nested);
    const auto* voiceValue =
        talas::project::findVisibleProjectSoundVariableDeclaration(
            document,
            "voiceSound",
            {},
            &voice.owner);
    require(
        project != nullptr
            && project->owner.kind == ProjectVariableOwnerKind::Project
            && outerValue != nullptr
            && outerValue->owner.definitionId == "outer"
            && innerValue != nullptr
            && innerValue->owner.definitionId == "inner"
            && voiceValue != nullptr
            && voiceValue->owner == voice.owner,
        "Sound-variable resolution should use exact voice, lexical, then project authority");
    require(
        talas::project::findVisibleProjectSoundVariableDeclaration(
            document,
            "voiceSound") == nullptr,
        "MIDI-voice Sound values must not leak into global lookup");

    // A Variable's or SoundCollectionVariable's name has no
    // question mark but follows a typed variable's rules, a letter first
    // and no spaces.
    const auto named = [&](std::string variableId, ProjectSoundVariableKind kind) {
        auto renamed = document;
        renamed.soundVariableLibrary.variables = {soundVariable(std::move(variableId), {}, {}, kind)};
        const auto validation = talas::project::validateProjectSoundVariableLibrary(renamed);
        return validation.ok()
            || std::ranges::none_of(validation.diagnostics, [](const auto& diagnostic) {
                   return diagnostic.code == talas::project::ProjectSoundVariableValidationCode::InvalidIdentifier;
               });
    };
    for (const auto kind : {ProjectSoundVariableKind::Sound, ProjectSoundVariableKind::SoundCollection})
    {
        require(named("formant2", kind) && named("theDaysAreAhead123456789", kind) && !named("20Jahrhunderts", kind)
                && !named("table Lookup", kind),
            "a Variable's name should follow a typed variable's rules");
    }
}

void testMalformedAndAggregateBoundsFailClosed()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "tone", .displayName = "Tone"},
    };
    auto malformed = soundVariable(
        "1bad",
        {"tone", "tone", "missing"});
    malformed.initialSelectedOrdinal = 3;
    document.soundVariableLibrary.variables = {std::move(malformed)};
    const auto validation =
        talas::project::validateProjectSoundVariableLibrary(document);
    require(
        !validation.ok()
            && std::ranges::any_of(
                validation.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectSoundVariableValidationCode::DuplicateCandidateDefinition;
                })
            && std::ranges::any_of(
                validation.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectSoundVariableValidationCode::UnknownCandidateDefinition;
                })
            && std::ranges::any_of(
                validation.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::ProjectSoundVariableValidationCode::InvalidInitialSelection;
                }),
        "malformed candidate identity and selection must fail closed");

    document.soundDefinitions.clear();
    for (std::size_t index = 0;
         index < talas::project::maximumProjectSoundVariableCandidates;
         ++index)
    {
        document.soundDefinitions.push_back({
            .definitionId = "sound" + std::to_string(index),
            .displayName = "Sound",
        });
    }
    std::vector<std::string> candidates;
    for (const auto& definition : document.soundDefinitions)
        candidates.push_back(definition.definitionId);
    document.soundVariableLibrary.variables.clear();
    for (std::size_t index = 0; index < 17; ++index)
    {
        document.soundVariableLibrary.variables.push_back(soundVariable(
            "collection" + std::to_string(index),
            candidates,
            {},
            ProjectSoundVariableKind::SoundCollection));
    }
    const auto overCapacity =
        talas::project::validateProjectSoundVariableLibrary(document);
    require(
        !overCapacity.ok()
            && overCapacity.diagnostics.size() == 1
            && overCapacity.diagnostics.front().code
                == talas::project::ProjectSoundVariableValidationCode::TooManyCandidateReferences,
        "aggregate candidate capacity should reject before declaration traversal");
}

void testPreparedInstanceAndVoiceMaterialization()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
        {.definitionId = "container", .displayName = "Container"},
    };
    document.graph.nodes.push_back({"rootVoice", "talas.midiVoice"});
    auto voice = soundVariable(
        "voiceSound",
        {"toneA", "toneB"},
        voiceOwner("rootVoice"));
    voice.snapshotEligible = false;
    document.soundVariableLibrary.variables = {
        soundVariable("projectSound", {"toneA", "toneB"}),
        soundVariable(
            "localSound",
            {"toneB"},
            definitionOwner("container")),
        voice,
    };

    talas::project::ProjectSoundExpansionManifest expansion;
    expansion.scopes = {
        {1, 0, {"instanceA"}, "container"},
        {2, 0, {"instanceB"}, "container"},
    };
    const std::vector<talas::project::ProjectMidiVoiceIsland> islands{
        {
            .flatOwnerNodeId = "rootVoice",
            .localOwnerNodeId = "rootVoice",
            .parentScopeOrdinal = 0,
            .firstVoiceScopeOrdinal = 3,
            .voiceCount = 2,
        },
    };
    const auto prepared = talas::project::prepareProjectSoundVariables(
        document,
        expansion,
        islands);
    require(
        prepared.ok() && prepared.table.cells.size() == 5,
        "preparation should create project, two instance, and two voice cells");
    require(
        talas::project::resolvePreparedProjectSoundVariableCell(
            prepared.table,
            1,
            "localSound")
                .has_value()
            && talas::project::resolvePreparedProjectSoundVariableCell(
                prepared.table,
                2,
                "projectSound")
                .has_value(),
        "prepared resolution should walk exact instance scope to project");
    const auto voiceCell = std::ranges::find_if(
        prepared.table.cells,
        [](const auto& cell) {
            return cell.identity.variableId == "voiceSound"
                && cell.identity.voiceOrdinal
                    == std::optional<std::uint16_t>{1};
        });
    require(
        voiceCell != prepared.table.cells.end()
            && voiceCell->scopeOrdinal == 4
            && voiceCell->candidateDefinitionIds
                == std::vector<std::string>{"toneA", "toneB"},
        "voice cells should preserve exact owner ordinal and candidate identity");
    require(
        talas::project::findPreparedProjectSoundVariableCell(
            prepared.table,
            voiceCell->identity)
                .has_value(),
        "prepared exact identity lookup should not depend on display order");
}

void testFormatTwentyNineRoundTripAndMigration()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
    };
    auto declaration = soundVariable(
        "playlist",
        {"toneA", "toneB"},
        {},
        ProjectSoundVariableKind::SoundCollection);
    declaration.initialSelectedOrdinal = 1;
    document.soundVariableLibrary.variables = {declaration};
    const auto canonical = talas::project::serializeProjectDocument(document);
    require(
        canonical.find(R"json("version": 156)json") != std::string::npos
            && canonical.find(R"json("soundVariableLibrary": {)json")
                != std::string::npos
            && canonical.find(R"json("initialSelectedOrdinal": 1)json")
                != std::string::npos,
        "format 29 should serialize exact Sound-variable authority");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.soundVariableLibrary
                == document.soundVariableLibrary
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "format 29 Sound-variable bytes should reopen canonically");

    constexpr std::string_view emptySoundLibrary =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    constexpr std::string_view emptyGeneratorLibrary =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    constexpr std::string_view emptyLiveSetLibrary =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    talas::project::ProjectDocument emptyDocument;
    emptyDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    auto formatTwentyEight =
        talas::project::serializeProjectDocument(emptyDocument);
    const auto version = formatTwentyEight.find(R"json("version": 156)json");
    require(version != std::string::npos, "fixture should expose format 29");
    formatTwentyEight.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  28)json");
    const auto library = formatTwentyEight.find(emptySoundLibrary);
    require(
        library != std::string::npos,
        "fixture should expose the empty Sound-variable library");
    formatTwentyEight.erase(library, emptySoundLibrary.size());
    const auto generatorLibrary =
        formatTwentyEight.find(emptyGeneratorLibrary);
    require(
        generatorLibrary != std::string::npos,
        "fixture should expose the future generator library");
    formatTwentyEight.erase(
        generatorLibrary, emptyGeneratorLibrary.size());
    const auto liveSetLibrary =
        formatTwentyEight.find(emptyLiveSetLibrary);
    require(
        liveSetLibrary != std::string::npos,
        "fixture should expose the future live-set library");
    formatTwentyEight.erase(
        liveSetLibrary, emptyLiveSetLibrary.size());
    const auto migrated =
        talas::project::parseProjectDocument(formatTwentyEight);
    require(
        migrated.ok()
            && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.soundVariableLibrary.variables.empty(),
        "format 28 should migrate with an empty Sound-variable library");

    auto missing = talas::project::serializeProjectDocument(emptyDocument);
    const auto missingLibrary = missing.find(emptySoundLibrary);
    missing.erase(missingLibrary, emptySoundLibrary.size());
    const auto missingResult = talas::project::parseProjectDocument(missing);
    require(
        !missingResult.ok()
            && std::ranges::any_of(
                missingResult.errors,
                [](const auto& error) {
                    return error.path == "$.soundVariableLibrary";
                }),
        "format 29 should require explicit Sound-variable library authority");

    auto fractional = canonical;
    const auto ordinal = fractional.find(
        R"json("initialSelectedOrdinal": 1)json");
    require(ordinal != std::string::npos, "fixture should expose exact ordinal");
    fractional.replace(
        ordinal,
        std::string_view{
            R"json("initialSelectedOrdinal": 1)json"}.size(),
        R"json("initialSelectedOrdinal": 1.00000000000000001)json");
    const auto fractionalResult =
        talas::project::parseProjectDocument(fractional);
    require(
        !fractionalResult.ok()
            && std::ranges::any_of(
                fractionalResult.errors,
                [](const auto& error) {
                    return error.path.find("initialSelectedOrdinal")
                        != std::string::npos;
                }),
        "mathematically fractional Sound selection tokens must reject");
}

void testEditorEditsCaptureAndDefinitionProtection()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
    };
    auto declaration = soundVariable(
        "selected",
        {"toneA", "toneB"},
        {},
        ProjectSoundVariableKind::SoundCollection);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(std::move(document), registry);
    require(
        editor.upsertProjectSoundVariable(declaration).ok()
            && editor.document().soundVariableLibrary.variables.size() == 1,
        "Sound-variable upsert should publish one validated declaration");
    require(
        editor.undo().ok()
            && editor.document().soundVariableLibrary.variables.empty()
            && editor.redo().ok(),
        "Sound-variable authoring should be one reversible history edit");

    const auto captureAuthority = editor.documentAuthorityGeneration();
    require(
        editor.captureProjectSoundVariableInitialSelection(
                  "selected",
                  "toneB",
                  captureAuthority)
            .ok()
            && editor.document().soundVariableLibrary.variables.front()
                    .initialSelectedOrdinal == 1,
        "capture should resolve an exact candidate identity to the initial ordinal");
    require(
        !editor.captureProjectSoundVariableInitialSelection(
                   "selected",
                   "toneA",
                   captureAuthority)
             .ok()
            && editor.document().soundVariableLibrary.variables.front()
                    .initialSelectedOrdinal == 1,
        "stale capture authority should leave the selected identity unchanged");

    auto replacement = declaration;
    replacement.displayName = "Replacement";
    require(
        !editor.replaceProjectSoundVariable(
                   definitionOwner("toneA"),
                   "selected",
                   replacement)
             .ok()
            && editor.document().soundVariableLibrary.variables.front()
                    .displayName == declaration.displayName,
        "replacement should require the exact prior owner and ID");
    require(
        !editor.removeSoundDefinition("toneB").ok()
            && editor.document().soundDefinitions.size() == 2,
        "candidate Sound definitions must not be removed behind a declaration");
    require(
        editor.removeProjectSoundVariable({}, "selected").ok()
            && editor.removeSoundDefinition("toneB").ok(),
        "removing the declaration should release its exact candidate authority");
}

void testPreparedSoundVariableLookupAtDeepMidiVoiceScope()
{
    talas::project::PreparedProjectSoundVariableTable table;
    table.scopes.push_back({0, 0, {}, {}});
    // The deepest accepted Sound has 64 scope edges; a MIDI voice adds one.
    for (std::uint32_t scope = 1; scope <= 65; ++scope)
        table.scopes.push_back({scope, scope - 1, {}, {}});
    talas::project::PreparedProjectSoundVariableCell global;
    global.identity.variableId = "GlobalSound";
    table.cells.push_back(std::move(global));
    require(
        talas::project::resolvePreparedProjectSoundVariableCell(
            table, 65, "GlobalSound")
            == std::optional<std::size_t>{0},
        "a MIDI voice under the deepest Sound must inherit a global Sound variable");

    talas::project::PreparedProjectSoundVariableCell parent;
    parent.identity.variableId = "GlobalSound";
    parent.scopeOrdinal = 64;
    table.cells.push_back(std::move(parent));
    require(
        talas::project::resolvePreparedProjectSoundVariableCell(
            table, 65, "GlobalSound")
            == std::optional<std::size_t>{1},
        "deep Sound-variable lookup must prefer the nearest Sound binding");
}
void testNestedSelectorMaterializationUsesProjectedLexicalOrder()
{
    using namespace talas::project;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    ProjectDocument base;
    for (const auto* id : {"toneA", "toneB"})
    {
        ProjectSoundDefinition tone;
        tone.definitionId = id;
        tone.displayName = id;
        tone.graph.nodes = {{"tone", "talas.oscillator", {}}};
        tone.outputs = {{"audio", talas::core::PortType::Audio, "tone", "audioOut"}};
        base.soundDefinitions.push_back(std::move(tone));
    }
    ProjectParameterExpression selected;
    selected.parameterId = "variableId";
    selected.text = "selected";
    base.graph.nodes = {
        {"selector", "talas.variable", {selected}},
        {"child", "talas.midiVoice", {{"polyphony", "3"}}},
        {"parent", "talas.midiVoice", {{"polyphony", "2"}}},
        {"out", "talas.audioOutput", {}},
    };
    base.graph.connections = {
        {"selector", "audioOut", "child", "audioIn"},
        {"child", "leftOut", "parent", "audioIn"},
        {"parent", "leftOut", "out", "audioIn"},
    };
    auto parentSelection = soundVariable("selected", {"toneB"}, voiceOwner("parent"));
    parentSelection.snapshotEligible = false;
    base.soundVariableLibrary.variables = {
        soundVariable("selected", {"toneA"}), parentSelection};
    for (int variant = 0; variant < 3; ++variant)
    {
        auto document = base;
        if (variant != 0)
        {
            auto childSelection = soundVariable("selected", {"toneA"}, voiceOwner("child"));
            childSelection.snapshotEligible = false;
            document.soundVariableLibrary.variables.push_back(std::move(childSelection));
        }
        if (variant == 2)
        {
            ProjectSoundDefinition input;
            input.definitionId = "inputSound";
            input.displayName = "Input Sound";
            input.graph.nodes.push_back(document.graph.nodes.front());
            input.outputs = {{"audioOut", talas::core::PortType::Audio, "selector", "audioOut"}};
            document.soundDefinitions.push_back(std::move(input));
            document.graph.nodes.front() = {};
            document.graph.nodes.front().instanceId = "selector";
            document.graph.nodes.front().soundDefinitionId = "inputSound";
            document.soundVariableLibrary.variables.push_back(
                soundVariable("selected", {"toneB"}, definitionOwner("inputSound")));
        }
        auto expanded = expandProjectSounds(document, registry);
        require(expanded.ok(), "nested selector source should expand");
        const auto templates = discoverProjectMidiVoiceHierarchyTemplates(
            expanded.document, expanded.manifest);
        require(templates.ok(), "nested selector should have exact hierarchy templates");
        require(materializeProjectSoundVariableBranches(document, expanded, templates.islands).ok(),
            "nested selector should materialize its nearest lexical declaration");
        const auto expected = std::vector<std::string>{variant == 1 ? "toneA" : "toneB"};
        require(expanded.manifest.soundVariableSelectors.size() == 1
            && expanded.manifest.soundVariableSelectors.front().candidateDefinitionIds == expected,
            "parent fallback, child shadow and descendant Sound shadow must select the matching branches");
        const auto instances = discoverProjectMidiVoiceInstanceHierarchy(expanded.document,
            expanded.manifest, {{"parent", 2}, {"child", 3}});
        require(instances.ok(), "materialized branches must retain instance ownership");
        const auto table = prepareProjectSoundVariables(document, expanded.manifest, instances.islands);
        require(table.ok(), "materialized selector cells must prepare in every parent instance");
        const auto sourceScope = expanded.manifest.soundVariableSelectors.front().scopeOrdinal;
        std::size_t checked = 0;
        for (const auto& instance : instances.islands)
        {
            if (instance.flatOwnerNodeId != "child") continue;
            for (std::size_t voice = 0; voice < instance.voiceCount; ++voice)
            {
                auto scope = instance.firstVoiceScopeOrdinal + static_cast<std::uint32_t>(voice);
                if (sourceScope != instance.sourceScopeOrdinal)
                {
                    const auto projection = std::ranges::find_if(instance.scopeProjections,
                        [&](const auto& candidate) {
                            return candidate.sourceScopeOrdinal == sourceScope
                                && candidate.voiceAncestors.back().voiceOrdinal == voice;
                        });
                    require(projection != instance.scopeProjections.end(), "selector source must project");
                    scope = projection->scopeOrdinal;
                }
                const auto cell = resolvePreparedProjectSoundVariableCell(table.table, scope, "selected");
                require(cell && table.table.cells[*cell].candidateDefinitionIds == expected,
                    "branch materialization and all six projected cell lookups must agree");
                ++checked;
            }
        }
        require(checked == 6, "both parent voices must retain all three selector contexts");
    }
}

}

int main()
{
    testValidationAndLexicalResolution();
    testMalformedAndAggregateBoundsFailClosed();
    testPreparedInstanceAndVoiceMaterialization();
    testFormatTwentyNineRoundTripAndMigration();
    testEditorEditsCaptureAndDefinitionProtection();
    testPreparedSoundVariableLookupAtDeepMidiVoiceScope();
    testNestedSelectorMaterializationUsesProjectedLexicalOrder();
    return 0;
}
