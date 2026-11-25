#include "talas/project/ProjectVariables.h"
#include "talas/project/VariableLiterals.h"

#include "JsonValue.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <ranges>
#include <unordered_set>

namespace talas::project
{
namespace
{
void appendDiagnostic(
    ProjectVariableValidationResult& result,
    ProjectVariableValidationCode code,
    std::string path,
    std::string message)
{
    if (result.truncated)
        return;
    if (result.diagnostics.size() >= maximumProjectVariableDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back({code, std::move(path), std::move(message)});
}

[[nodiscard]] std::string declarationIdentity(
    const ProjectVariableDeclaration& declaration)
{
    std::string identity;
    identity.reserve(
        declaration.variableId.size()
        + declaration.owner.definitionId.size()
        + declaration.owner.nodeId.size() + 4U);
    switch (declaration.owner.kind)
    {
    case ProjectVariableOwnerKind::Project: identity += "p:"; break;
    case ProjectVariableOwnerKind::SoundDefinition: identity += "s:"; break;
    case ProjectVariableOwnerKind::MidiVoice: identity += "v:"; break;
    case ProjectVariableOwnerKind::WorkspaceSession: identity += "w:"; break;
    }
    identity += declaration.owner.definitionId;
    identity += ':';
    identity += declaration.owner.nodeId;
    identity += ':';
    identity += declaration.variableId;
    return identity;
}

[[nodiscard]] const ProjectSoundDefinition* findSoundDefinition(
    const ProjectDocument& document,
    std::string_view definitionId) noexcept
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] const ProjectNode* findNode(
    const ProjectGraph& graph,
    std::string_view nodeId) noexcept
{
    const auto found = std::ranges::find(
        graph.nodes,
        nodeId,
        &ProjectNode::instanceId);
    return found == graph.nodes.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] bool containsControlCharacter(std::string_view value) noexcept
{
    return std::ranges::any_of(value, [](unsigned char character) {
        return character < 0x20U || character == 0x7fU;
    });
}
}

bool isValidProjectVariableIdentifier(std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > maximumProjectVariableIdentifierBytes
        || !json::isValidUtf8(value))
    {
        return false;
    }
    const auto isStart = [](char character) noexcept {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    };
    const auto isContinue = [isStart](char character) noexcept {
        return isStart(character)
            || (character >= '0' && character <= '9');
    };
    return isStart(value.front())
        && std::ranges::all_of(value.substr(1), isContinue);
}

bool isSupportedProjectVariableDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

ProjectVariableValidationResult validateProjectVariableLibrary(
    const ProjectDocument& document)
{
    ProjectVariableValidationResult result;
    result.diagnostics.reserve(maximumProjectVariableDiagnostics);
    const auto& library = document.variableLibrary;
    if (library.version != 2)
    {
        appendDiagnostic(
            result,
            ProjectVariableValidationCode::InvalidVersion,
            "$.variableLibrary.version",
            "Variable library version must be exactly 2");
    }
    if (!talas::expr::supportsVariableReferences(
            document.expressionLanguage)
        && !library.variables.empty())
    {
        appendDiagnostic(
            result,
            ProjectVariableValidationCode::UnsupportedLanguage,
            "$.variableLibrary.variables",
            "Variable declarations require a variable-reference expression authority");
        return result;
    }
    if (library.variables.size() > maximumProjectVariableCount)
    {
        appendDiagnostic(
            result,
            ProjectVariableValidationCode::TooManyVariables,
            "$.variableLibrary.variables",
            "Variable library exceeds its fixed declaration limit");
        return result;
    }

    std::unordered_set<std::string> identities;
    identities.reserve(library.variables.size());
    std::unordered_set<std::string> globalVariableIds;
    globalVariableIds.reserve(library.variables.size());
    for (std::size_t index = 0;
         index < library.variables.size() && !result.truncated;
         ++index)
    {
        const auto& declaration = library.variables[index];
        const auto path = "$.variableLibrary.variables["
            + std::to_string(index) + "]";
        if (!isValidProjectVariableIdentifier(declaration.variableId))
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::InvalidIdentifier,
                path + ".variableId",
                "Variable ID must be a bounded expression identifier");
        }
        if (!identities.insert(declarationIdentity(declaration)).second)
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::DuplicateDeclaration,
                path + ".variableId",
                "Variable ID is duplicated in the same owner scope");
        }
        if ((declaration.owner.kind == ProjectVariableOwnerKind::Project
             || declaration.owner.kind
                    == ProjectVariableOwnerKind::WorkspaceSession)
            && !globalVariableIds.insert(declaration.variableId).second)
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::AmbiguousGlobalOwner,
                path + ".variableId",
                "Project and workspace-session variables cannot share one globally visible ID");
        }
        if (declaration.displayName.empty()
            || declaration.displayName.size()
                > maximumProjectVariableDisplayNameBytes
            || !json::isValidUtf8(declaration.displayName)
            || containsControlCharacter(declaration.displayName))
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::InvalidDisplayName,
                path + ".displayName",
                "Variable display name must be bounded non-empty UTF-8 text");
        }
        if (!isSupportedProjectVariableDomain(declaration.domain))
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::InvalidDomain,
                path + ".domain",
                "Variable domain is not a supported numeric domain");
        }
        if (!std::isfinite(declaration.initialValue))
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::NonFiniteInitialValue,
                path + ".initialValue",
                "Variable initial value must be finite");
        }
        if (declaration.owner.kind == ProjectVariableOwnerKind::Project)
        {
            if (!declaration.owner.definitionId.empty()
                || !declaration.owner.nodeId.empty())
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner",
                    "Project-owned variables cannot name a Sound definition or node");
            }
        }
        else if (declaration.owner.kind
                 == ProjectVariableOwnerKind::SoundDefinition)
        {
            if (!isValidProjectVariableIdentifier(
                    declaration.owner.definitionId))
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner.definitionId",
                    "Sound-owned variables require an exact definition ID");
            }
            else if (std::ranges::none_of(
                         document.soundDefinitions,
                         [&](const ProjectSoundDefinition& definition) {
                             return definition.definitionId
                                 == declaration.owner.definitionId;
                         }))
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::UnknownSoundDefinition,
                    path + ".owner.definitionId",
                    "Variable owner references an unknown Sound definition");
            }
            if (!declaration.owner.nodeId.empty())
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner.nodeId",
                    "Sound-definition variables cannot name a MIDI Voice node");
            }
        }
        else if (declaration.owner.kind
                 == ProjectVariableOwnerKind::MidiVoice)
        {
            if (declaration.snapshotEligible)
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".snapshotEligible",
                    "MIDI-voice variables are runtime-only and cannot be snapshot eligible");
            }
            if (!isValidProjectVariableIdentifier(
                    declaration.owner.nodeId))
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner.nodeId",
                    "MIDI-voice variables require an exact owner node ID");
            }
            const ProjectNode* ownerNode = nullptr;
            if (declaration.owner.definitionId.empty())
            {
                ownerNode = findNode(
                    document.graph,
                    declaration.owner.nodeId);
            }
            else if (!isValidProjectVariableIdentifier(
                         declaration.owner.definitionId))
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner.definitionId",
                    "Definition-local MIDI Voice owners require an exact definition ID");
            }
            else if (const auto* definition = findSoundDefinition(
                         document,
                         declaration.owner.definitionId))
            {
                ownerNode = findNode(
                    definition->graph,
                    declaration.owner.nodeId);
            }
            else
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::UnknownSoundDefinition,
                    path + ".owner.definitionId",
                    "MIDI Voice owner references an unknown Sound definition");
            }
            if (ownerNode == nullptr
                || ownerNode->typeId != "talas.midiVoice")
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::UnknownMidiVoice,
                    path + ".owner.nodeId",
                    "Variable owner must reference an exact talas.midiVoice node");
            }
        }
        else if (declaration.owner.kind
                 == ProjectVariableOwnerKind::WorkspaceSession)
        {
            if (!declaration.owner.definitionId.empty()
                || !declaration.owner.nodeId.empty())
            {
                appendDiagnostic(
                    result,
                    ProjectVariableValidationCode::InvalidOwner,
                    path + ".owner",
                    "Workspace-session variables accept kind only");
            }
        }
        else
        {
            appendDiagnostic(
                result,
                ProjectVariableValidationCode::InvalidOwner,
                path + ".owner.kind",
                "Variable owner kind is invalid");
        }
    }
    return result;
}

const ProjectVariableDeclaration* findVisibleProjectVariableDeclaration(
    const ProjectDocument& document,
    std::string_view variableId,
    std::span<const std::string_view> soundDefinitionScope,
    const ProjectVariableOwner* midiVoiceOwner,
    std::span<const ProjectVariableOwner> midiVoiceAncestors) noexcept
{
    if (!isValidProjectVariableIdentifier(variableId))
        return nullptr;
    if (midiVoiceOwner != nullptr
        && midiVoiceOwner->kind == ProjectVariableOwnerKind::MidiVoice)
    {
        const auto voice = std::ranges::find_if(
            document.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner == *midiVoiceOwner;
            });
        if (voice != document.variableLibrary.variables.end())
            return &*voice;
    }
    for (const auto& ancestor : midiVoiceAncestors)
    {
        if (ancestor.kind != ProjectVariableOwnerKind::MidiVoice) continue;
        const auto voice = std::ranges::find_if(document.variableLibrary.variables,
            [&](const auto& declaration) {
                return declaration.variableId == variableId && declaration.owner == ancestor;
            });
        if (voice != document.variableLibrary.variables.end()) return &*voice;
    }
    for (auto scope = soundDefinitionScope.rbegin();
         scope != soundDefinitionScope.rend();
         ++scope)
    {
        const auto found = std::ranges::find_if(
            document.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner.kind
                        == ProjectVariableOwnerKind::SoundDefinition
                    && declaration.owner.definitionId == *scope;
            });
        if (found != document.variableLibrary.variables.end())
            return &*found;
    }
    const auto global = std::ranges::find_if(
        document.variableLibrary.variables,
        [&](const ProjectVariableDeclaration& declaration) {
            return declaration.variableId == variableId
                && (declaration.owner.kind
                        == ProjectVariableOwnerKind::Project
                    || declaration.owner.kind
                        == ProjectVariableOwnerKind::WorkspaceSession);
        });
    return global == document.variableLibrary.variables.end()
        ? nullptr
        : &*global;
}

std::map<std::string, std::set<std::string>> scriptBoundVariables(const ProjectGraph& graph)
{
    std::map<std::string, std::set<std::string>> bound;
    for (const auto& script : graph.nodes)
    {
        if (script.typeId != "talas.script")
            continue;
        const auto text = std::ranges::find(script.parameters, "script", &ProjectParameterExpression::parameterId);
        if (text == script.parameters.end() || !text->text.has_value())
            continue;
        const auto parsed = smalltalk::parse(*text->text);
        if (!parsed.ok())
            continue;
        const auto keywords = smalltalk::eventKeywords(*parsed.program);
        for (const auto& connection : graph.connections)
        {
            if (connection.toNode != script.instanceId || (connection.toPort != "inputs" && connection.toPort != "audioIn"))
                continue;
            const auto sent = keywords.find(connection.fromNode);
            if (sent == keywords.end() || sent->second.empty())
                continue;
            // The Input and every Sound feeding it.
            std::vector<std::string> pending{connection.fromNode};
            std::set<std::string> seen;
            while (!pending.empty())
            {
                const auto nodeId = pending.back();
                pending.pop_back();
                if (!seen.insert(nodeId).second)
                    continue;
                bound[nodeId].insert(sent->second.begin(), sent->second.end());
                for (const auto& upstream : graph.connections)
                {
                    if (upstream.toNode == nodeId)
                        pending.push_back(upstream.fromNode);
                }
            }
        }
    }
    return bound;
}

namespace
{
// A class's string field takes its text as typed, never a variable.
[[nodiscard]] bool takesTextAsTyped(const ProjectDocument& document, const ProjectNode& node, std::string_view parameterId)
{
    if (!node.soundDefinitionId.has_value())
        return false;
    const auto definition = std::ranges::find(
        document.soundDefinitions, *node.soundDefinitionId, &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return false;
    return std::ranges::any_of(definition->exposedParameters, [&](const ProjectSoundExposedParameter& exposed) {
        return exposed.fieldKind == ProjectSoundFieldKind::String
            && (exposed.parameterId == parameterId || std::ranges::find(exposed.aliases, parameterId) != exposed.aliases.end());
    });
}
} // namespace

std::vector<std::string> undeclaredProjectVariables(const ProjectDocument& document)
{
    std::vector<std::string> names;
    const auto declared = [&](const std::string& name) {
        return std::ranges::any_of(document.variableLibrary.variables,
                   [&](const auto& declaration) { return declaration.variableId == name; })
            || std::ranges::any_of(document.soundVariableLibrary.variables,
                [&](const auto& declaration) { return declaration.variableId == name; });
    };
    // A Sound definition's class variables take their instances' values.
    const auto bindsClassVariable = [](const ProjectSoundDefinition* definition, const std::string& name) {
        return definition != nullptr
            && std::ranges::any_of(definition->exposedParameters, [&](const ProjectSoundExposedParameter& exposed) {
                   return std::ranges::any_of(exposed.bindings, [&](const ProjectSoundParameterBinding& binding) {
                       return binding.target == ProjectSoundParameterTarget::Variable && binding.variableName == name;
                   });
               });
    };
    const auto add = [&](const std::string& name, const ProjectSoundDefinition* definition) {
        if (!declared(name) && !bindsClassVariable(definition, name) && std::ranges::find(names, name) == names.end())
            names.push_back(name);
    };
    // A Script's ?variables, outside its strings and
    // comments, are asked for too.
    const auto readScript = [&](const std::string& script, const ProjectSoundDefinition* definition) {
        for (std::size_t at = 0; at < script.size(); ++at)
        {
            if (script[at] == '\'' || script[at] == '"')
            {
                const auto closing = script.find(script[at], at + 1);
                if (closing == std::string::npos)
                    return;
                at = closing;
                continue;
            }
            const auto letter = [&](std::size_t index) {
                return index < script.size() && std::isalpha(static_cast<unsigned char>(script[index])) != 0;
            };
            if (script[at] != '?' || !letter(at + 1))
                continue;
            auto end = at + 1;
            while (end < script.size() && (std::isalnum(static_cast<unsigned char>(script[end])) != 0 || script[end] == '_'))
                ++end;
            add(script.substr(at + 1, end - at - 1), definition);
            at = end - 1;
        }
    };
    const auto read = [&](const ProjectGraph& graph, const ProjectSoundDefinition* definition) {
        // A variable a Script's events give is not
        // asked for.
        const auto bound = scriptBoundVariables(graph);
        for (const auto& node : graph.nodes)
        {
            const auto given = bound.find(node.instanceId);
            for (const auto& parameter : node.parameters)
            {
                if (parameter.parameterId == "script" && parameter.text.has_value())
                    readScript(*parameter.text, definition);
                // A
                // text or file field wholly ?name, or an array field's
                // ?names, read values the dialog asks for too: a string, a
                // symbol, a character, an array or a number.
                else if ((parameter.text.has_value() || parameter.resource.has_value())
                    && !takesTextAsTyped(document, node, parameter.parameterId))
                {
                    for (const auto& name : fieldVariableReferences(
                             parameter.text.has_value() ? *parameter.text : *parameter.resource))
                    {
                        if (given == bound.end() || !given->second.contains(name))
                            add(name, definition);
                    }
                }
                if (!parameter.expression.has_value())
                    continue;
                const auto parsed = talas::expr::parse(
                    *parameter.expression, document.expressionLanguage, document.expressionDefinitions);
                if (!parsed.ok())
                    continue;
                for (const auto& name : parsed.expression->variableReferences())
                {
                    if (given == bound.end() || !given->second.contains(name))
                        add(name, definition);
                }
            }
        }
    };
    read(document.graph, nullptr);
    for (const auto& definition : document.soundDefinitions)
        read(definition.graph, &definition);
    return names;
}

std::vector<ScriptCompilerWarning> scriptCompilerWarnings(const ProjectDocument& document)
{
    std::vector<ScriptCompilerWarning> warnings;
    const auto read = [&](const ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            if (node.typeId != "talas.script")
                continue;
            const auto script = std::ranges::find(node.parameters, "script", &ProjectParameterExpression::parameterId);
            if (script == node.parameters.end() || !script->text.has_value())
                continue;
            const auto parsed = smalltalk::parse(*script->text);
            if (!parsed.ok())
                continue;
            for (const auto& name : smalltalk::unreadTemporaries(*parsed.program))
                warnings.push_back({node.instanceId, "The Script's variable " + name + " is assigned a value that is never used"});
        }
    };
    read(document.graph);
    for (const auto& definition : document.soundDefinitions)
        read(definition.graph);
    return warnings;
}
}
