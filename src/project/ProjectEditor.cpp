#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectClassFromExample.h"
#include "talas/project/ProjectGlobalMap.h"

#include "talas/project/ProjectDiskCache.h"
#include "talas/core/StaticResourceList.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectParameterTransformer.h"
#include "talas/project/ProjectSampleMaps.h"
#include "talas/project/ProjectScoreClipFingerprint.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/generative/TalasGrammar.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/VirtualControlOsc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::size_t historyLimit = 128;

// The window a history's newest entry was made in,
// 0 when it has none or the tags do not follow it.
[[nodiscard]] std::uint64_t newestWindow(const std::vector<std::uint64_t>& windows, std::size_t entries) noexcept
{
    return entries != 0 && windows.size() == entries ? windows.back() : 0;
}

[[nodiscard]] bool isAsciiEventNameStart(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool isAsciiEventNameContinue(char value)
{
    return isAsciiEventNameStart(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] bool isValidEventName(std::string_view eventName)
{
    return !eventName.empty()
        && isAsciiEventNameStart(eventName.front())
        && std::ranges::all_of(eventName.substr(1), isAsciiEventNameContinue);
}

[[nodiscard]] bool isKnownPerformanceControlKind(
    ProjectPerformanceControlKind kind)
{
    switch (kind)
    {
    case ProjectPerformanceControlKind::Knob:
    case ProjectPerformanceControlKind::Fader:
    case ProjectPerformanceControlKind::Gate:
    case ProjectPerformanceControlKind::Toggle:
    case ProjectPerformanceControlKind::Macro:
        return true;
    }
    return false;
}

[[nodiscard]] EditResult success(std::string message = {});
[[nodiscard]] EditResult failure(EditError error, std::string message);

[[nodiscard]] EditResult validatePerformanceSlots(
    const ProjectDocument& document,
    const SnapshotTargetIdentity& source,
    ProjectPerformanceControlKind kind,
    const std::vector<ProjectPerformanceModulationSlot>& slots)
{
    if (slots.size() > maximumPerformanceModulationSlotsPerControl)
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance control exceeds the eight-slot modulation limit");
    }
    if (kind != ProjectPerformanceControlKind::Macro && !slots.empty())
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Only Performance macro controls accept modulation slots");
    }
    std::unordered_set<std::string> destinations;
    for (const auto& slot : slots)
    {
        const auto identity = snapshotTargetIdentityString(slot.target);
        if (!std::isfinite(slot.depth) || slot.depth < -1.0
            || slot.depth > 1.0 || slot.target == source
            || !isProjectPerformanceTargetAvailable(document, slot.target)
            || !destinations.insert(identity).second)
        {
            return failure(EditError::InvalidPerformancePresentation,
                "Performance modulation requires unique available targets, "
                "no self-route, and finite depth in [-1, 1]");
        }
    }
    return success();
}

[[nodiscard]] bool isValidValueDomain(talas::expr::ValueDomain domain)
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

[[nodiscard]] bool isValidAutomationInterpolation(
    talas::core::AutomationInterpolation interpolation)
{
    switch (interpolation)
    {
    case talas::core::AutomationInterpolation::Step:
    case talas::core::AutomationInterpolation::Linear:
        return true;
    }
    return false;
}

[[nodiscard]] EditResult success(std::string message)
{
    return {std::nullopt, std::move(message)};
}

[[nodiscard]] EditResult failure(EditError error, std::string message)
{
    return {error, std::move(message)};
}

[[nodiscard]] std::optional<EditResult>
freezeAuthoredExternalComponent(
    ProjectNode& node,
    const talas::core::NodeTypeDescriptor& descriptor)
{
    if (node.preservedComponent.has_value()
        || (descriptor.provenance.origin
                != talas::core::NodeOrigin::External
            && descriptor.provenance.origin
                != talas::core::NodeOrigin::IsolatedComponent))
    {
        return std::nullopt;
    }
    auto component = makeProjectPreservedComponent(descriptor);
    if (!component.has_value())
    {
        return failure(
            EditError::InvalidGraph,
            "External component interface cannot be preserved within project limits");
    }
    node.preservedComponent = std::move(*component);
    return std::nullopt;
}

[[nodiscard]] std::optional<EditResult>
freezeAuthoredExternalComponents(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    const auto freezeGraph = [&](ProjectGraph& graph)
        -> std::optional<EditResult> {
        for (auto& node : graph.nodes)
        {
            if (node.soundDefinitionId.has_value()
                || node.preservedComponent.has_value())
            {
                continue;
            }
            const auto* descriptor = registry.findNodeType(node.typeId);
            if (descriptor == nullptr)
                continue;
            if (const auto preservation =
                    freezeAuthoredExternalComponent(node, *descriptor);
                preservation.has_value())
            {
                return preservation;
            }
        }
        return std::nullopt;
    };
    for (auto& definition : document.soundDefinitions)
    {
        if (const auto result = freezeGraph(definition.graph);
            result.has_value())
        {
            return result;
        }
    }
    return freezeGraph(document.graph);
}

[[nodiscard]] std::optional<EditResult>
unresolvedComponentEditFailure(
    const ProjectNode& node,
    const talas::core::NodeRegistry& registry)
{
    if (!node.preservedComponent.has_value())
        return std::nullopt;
    const auto availability =
        classifyProjectComponentAvailability(
            node.typeId,
            &*node.preservedComponent,
            registry);
    if (availability.executable())
        return std::nullopt;
    return failure(
        EditError::UnresolvedComponent,
        availability.message);
}

[[nodiscard]] std::optional<CanvasPosition> translatedPosition(
    CanvasPosition position,
    std::int64_t deltaX,
    std::int64_t deltaY)
{
    const auto translatedX =
        static_cast<std::int64_t>(position.x) + deltaX;
    const auto translatedY =
        static_cast<std::int64_t>(position.y) + deltaY;
    if (translatedX < std::numeric_limits<std::int32_t>::min()
        || translatedX > std::numeric_limits<std::int32_t>::max()
        || translatedY < std::numeric_limits<std::int32_t>::min()
        || translatedY > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    return CanvasPosition{
        static_cast<std::int32_t>(translatedX),
        static_cast<std::int32_t>(translatedY),
    };
}

[[nodiscard]] EditResult eventValueSyncOutcome(
    const ProjectEventValueSyncResult& sync)
{
    if (sync.addedCount != 0)
    {
        return success(
            "Automatically generated "
            + std::to_string(sync.addedCount)
            + " Event Value control"
            + (sync.addedCount == 1 ? std::string{} : "s"));
    }
    if (!sync.diagnostics.empty())
    {
        const auto& diagnostic = sync.diagnostics.front();
        auto message = "Event Value discovery warning: "
            + diagnostic.message;
        if (!diagnostic.eventName.empty())
            message += " (" + diagnostic.eventName + ")";
        return success(std::move(message));
    }
    return success();
}

[[nodiscard]] EditResult validateAuthoredParameterExpression(
    const ProjectDocument& document,
    const ProjectNode& node,
    const ProjectParameterExpression& parameter,
    const talas::core::ParameterDescriptor& descriptor,
    talas::expr::ExpressionLanguageVersion language)
{
    try
    {
        if (node.typeId == "talas.parameterTransformer"
            && parameter.parameterId == "transformation"
            && parameter.text.has_value())
        {
            const auto parsed = parseProjectParameterTransformation(
                *parameter.text);
            if (!parsed.ok())
            {
                if (parsed.diagnostics.empty())
                {
                    return failure(
                        EditError::InvalidParameterValue,
                        "Parameter transformation validation failed within fixed limits");
                }
                const auto& diagnostic = parsed.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    "Parameter transformation bytes "
                        + std::to_string(diagnostic.sourceRange.offset)
                        + "-"
                        + std::to_string(
                            diagnostic.sourceRange.offset
                            + diagnostic.sourceRange.length)
                        + ": " + diagnostic.message);
            }
            for (const auto& rule : parsed.rules)
            {
                if (rule.kind
                        != ProjectParameterTransformationRuleKind::Set
                    || talas::expr::parse(
                           rule.operand,
                           document.expressionLanguage,
                           document.expressionDefinitions).ok())
                {
                    continue;
                }
                return failure(
                    EditError::InvalidParameterValue,
                    "Parameter transformation bytes "
                        + std::to_string(rule.sourceRange.offset)
                        + "-"
                        + std::to_string(
                            rule.sourceRange.offset
                            + rule.sourceRange.length)
                        + ": Set rule contains an invalid project-language expression");
            }
        }
        const auto audit = auditProjectParameterExpression(
            document, node, parameter, descriptor, language);
        if (audit.ok())
            return success();
        if (audit.diagnostics.empty())
        {
            return failure(
                EditError::InvalidParameterValue,
                "Parameter expression audit exceeded the diagnostic limit");
        }
        const auto& diagnostic = audit.diagnostics.front();
        return failure(
            EditError::InvalidParameterValue,
            diagnostic.path + " bytes "
                + std::to_string(diagnostic.range.offset) + "-"
                + std::to_string(
                    diagnostic.range.offset + diagnostic.range.length)
                + ": " + diagnostic.message);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the parameter expression audit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Parameter expression audit exceeded supported capacity");
    }
}

// A codeString field's Smalltalk parsed, a syntax error refused with its
// line, column and the code there.
[[nodiscard]] std::optional<EditResult> codeStringFailure(std::string_view fieldName, const std::string& code)
{
    const auto parsed = smalltalk::parse(code);
    if (parsed.ok() || !parsed.diagnostic.has_value())
        return std::nullopt;
    const auto offset = std::min(parsed.diagnostic->range.offset, code.size());
    const auto lineStart = code.rfind('\n', offset == 0 ? 0 : offset - 1);
    const auto column = lineStart == std::string::npos || offset == 0 ? offset + 1 : offset - lineStart;
    const auto line = 1 + std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
    auto there = code.substr(offset, std::min<std::size_t>(24, code.find('\n', offset) - offset));
    return failure(EditError::InvalidParameterValue,
        std::string{fieldName} + ", line " + std::to_string(line) + ", column " + std::to_string(column) + ": "
            + parsed.diagnostic->message + (there.empty() ? std::string{" (at the end)"} : " (at \"" + there + "\")"));
}

// The kind of field a class's parameter has: a value field takes constants, variables and
// expressions but no Event Values (its constant expression is evaluated as
// it is entered, as every field's is); a codeString field's Smalltalk is
// parsed and a syntax error refused with its line, column and the code
// there, which a value field's byte range does not give.
[[nodiscard]] std::optional<EditResult> classFieldKindFailure(const ProjectDocument& document,
    const ProjectSoundExposedParameter& exposed, const ProjectParameterExpression& parameter)
{
    if (exposed.fieldKind == ProjectSoundFieldKind::Value && parameter.expression.has_value())
    {
        const auto parsed = talas::expr::parse(*parameter.expression, document.expressionLanguage, document.expressionDefinitions);
        if (!parsed.ok())
            return std::nullopt;
        if (!parsed.expression->eventReferences().empty())
        {
            return failure(EditError::InvalidParameterValue,
                exposed.displayName + " is a value field: it takes constants, variables and expressions, not Event Values like !"
                    + parsed.expression->eventReferences().front() + " (a hotValue field takes them)");
        }
    }
    if (exposed.fieldKind == ProjectSoundFieldKind::CodeString && parameter.text.has_value())
        return codeStringFailure(exposed.displayName, *parameter.text);
    // Format 147: a complex field takes a number or a complex number, as
    // `0.5 % 0.25`; a polesAndZeroes
    // field a list of them; a grammar field a ContextFreeGrammar's
    // production rules.
    if ((exposed.fieldKind == ProjectSoundFieldKind::Complex
            || exposed.fieldKind == ProjectSoundFieldKind::PolesAndZeroes)
        && parameter.text.has_value())
    {
        if (const auto syntax = codeStringFailure(exposed.displayName, *parameter.text))
            return syntax;
        const auto complex = exposed.fieldKind == ProjectSoundFieldKind::Complex;
        const auto check = complex
            ? "| value | value := (" + *parameter.text + "). value isComplex or: [value isNumber]"
            : "| value | value := (" + *parameter.text + "). value isArray and: [((value collect: [:each | each isComplex or: [each isNumber]]) includes: false) not]";
        smalltalk::EvaluationHost host;
        const auto evaluated = smalltalk::evaluate(check, host);
        if (!evaluated.ok() || evaluated.printed != "true")
        {
            return failure(EditError::InvalidParameterValue,
                exposed.displayName
                    + (complex ? " is a complex field: it takes a number or a complex number, as 0.5 % 0.25"
                               : " is a polesAndZeroes field: it takes a list of complex poles and zeroes, as {0.9 % 0.1. 0.9 % -0.1}"));
        }
    }
    if (exposed.fieldKind == ProjectSoundFieldKind::Grammar && parameter.text.has_value())
    {
        const auto rules = talas::generative::talas_grammar::parseRules(*parameter.text);
        if (!rules.ok())
        {
            return failure(EditError::InvalidParameterValue,
                exposed.displayName + " is a grammar field, a ContextFreeGrammar's production rules: " + rules.error);
        }
    }
    return std::nullopt;
}

// The codeString field is
// the Script field of a Script and of the FileInterpreters (the
// ParameterTransformer's Transformation field is parsed as its own
// transformation).
[[nodiscard]] bool scriptCodeStringField(const ProjectNode& node, std::string_view parameterId)
{
    return parameterId == "script"
        && (node.typeId == "talas.script" || node.typeId == "talas.textFileInterpreter" || node.typeId == "talas.midiFileInterpreter"
            || node.typeId == "talas.limeInterpreter");
}

[[nodiscard]] ProjectNode* findNode(ProjectDocument& document, std::string_view nodeId)
{
    const auto found = std::ranges::find_if(document.graph.nodes, [&](ProjectNode& node) {
        return node.instanceId == nodeId;
    });
    if (found == document.graph.nodes.end())
        return nullptr;
    return &*found;
}

[[nodiscard]] const ProjectNode* findNode(const ProjectDocument& document, std::string_view nodeId)
{
    const auto found = std::ranges::find_if(document.graph.nodes, [&](const ProjectNode& node) {
        return node.instanceId == nodeId;
    });
    if (found == document.graph.nodes.end())
        return nullptr;
    return &*found;
}

[[nodiscard]] ProjectNode* findNode(
    ProjectGraph& graph,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        graph.nodes,
        [&](ProjectNode& node) {
            return node.instanceId == nodeId;
        });
    return found == graph.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectNode* findNode(
    const ProjectGraph& graph,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        graph.nodes,
        [&](const ProjectNode& node) {
            return node.instanceId == nodeId;
        });
    return found == graph.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] ProjectSoundDefinition* findSoundDefinition(
    ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectSoundDefinition* findSoundDefinition(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectParameterExpression& resolveExposedValue(
    const ProjectSoundExposedParameter& exposed,
    const ProjectNode& instance)
{
    const auto overrideValue = std::ranges::find_if(
        instance.parameters,
        [&](const auto& value) {
            return value.parameterId == exposed.parameterId
                || std::ranges::find(
                       exposed.aliases,
                       value.parameterId)
                    != exposed.aliases.end();
        });
    return overrideValue == instance.parameters.end()
        ? exposed.defaultValue
        : *overrideValue;
}

// Inlined nodes leave the Sound's scope, so anything resolved in that scope
// would silently rebind to the enclosing graph.
[[nodiscard]] bool referencesName(
    const std::optional<std::string>& text,
    char sigil,
    std::string_view name) noexcept
{
    if (!text.has_value() || name.empty())
        return false;
    const auto identifierCharacter = [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9') || value == '_';
    };
    for (auto position = text->find(sigil); position != std::string::npos;
         position = text->find(sigil, position + 1))
    {
        const auto end = position + 1 + name.size();
        if (text->compare(position + 1, name.size(), name) == 0
            && (end == text->size() || !identifierCharacter((*text)[end])))
        {
            return true;
        }
    }
    return false;
}

// Inlined nodes leave the Sound's scope, so anything they resolve in that
// scope would silently rebind to the enclosing graph.
[[nodiscard]] std::optional<std::string> inlineScopeDependency(
    const ProjectDocument& document,
    const ProjectSoundDefinition& definition)
{
    const auto graphReferences = [&](char sigil, std::string_view name) {
        return std::ranges::any_of(definition.graph.nodes, [&](const ProjectNode& node) {
            return std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return referencesName(parameter.expression, sigil, name)
                    || referencesName(parameter.text, sigil, name);
            });
        });
    };
    const auto prefix = "Sound '" + definition.definitionId + "' ";
    for (const auto& exposed : definition.exposedParameters)
    {
        for (const auto& binding : exposed.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::EventValue
                && graphReferences('!', binding.eventName))
            {
                return prefix + "reads Event Value '" + binding.eventName
                    + "' from its own scope and cannot be inlined";
            }
        }
    }
    const auto ownedByDefinition = [&](const ProjectVariableOwner& owner) {
        return owner.kind == ProjectVariableOwnerKind::SoundDefinition
            && owner.definitionId == definition.definitionId;
    };
    for (const auto& variable : document.variableLibrary.variables)
    {
        if (ownedByDefinition(variable.owner) && graphReferences('@', variable.variableId))
        {
            return prefix + "reads variable '" + variable.variableId
                + "' from its own scope and cannot be inlined";
        }
    }
    for (const auto& variable : document.soundVariableLibrary.variables)
    {
        if (ownedByDefinition(variable.owner))
        {
            return prefix + "owns Sound variable '" + variable.variableId
                + "' in its own scope and cannot be inlined";
        }
    }
    if (graphReferences('?', "VoiceNumber"))
        return prefix + "uses ?VoiceNumber from its own scope and cannot be inlined";
    return std::nullopt;
}

// The variables an example's fields read become the class's parameters
//, each bound to its ?name in the fields. A
// parameter defaults to the project's variable itself where the project
// declares it, so the Sound plays as before, and otherwise to the first
// field's own default.
void exposeExampleVariables(
    ProjectSoundDefinition& definition,
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const ProjectGraph* exampleLines = nullptr)
{
    // A variable a Script's events give stays bound,
    // not a parameter (the example's lines are those of the project's graph,
    // or of the Sound file window's Sound it was made from).
    const auto bound = scriptBoundVariables(exampleLines != nullptr ? *exampleLines : document.graph);
    for (const auto& node : definition.graph.nodes)
    {
        const auto* type = registry.findNodeType(node.typeId);
        const auto given = bound.find(node.instanceId);
        for (const auto& parameter : node.parameters)
        {
            if (!parameter.expression.has_value())
                continue;
            const auto parsed = talas::expr::parse(
                *parameter.expression, document.expressionLanguage, document.expressionDefinitions);
            if (!parsed.ok())
                continue;
            for (const auto& reference : parsed.expression->variableReferenceMetadata())
            {
                if (reference.name == "VoiceNumber" || (given != bound.end() && given->second.contains(reference.name))
                    || std::ranges::any_of(definition.exposedParameters, [&](const ProjectSoundExposedParameter& exposed) {
                           return exposed.parameterId == reference.name;
                       }))
                {
                    continue;
                }
                const talas::core::ParameterDescriptor* field = nullptr;
                if (type != nullptr)
                {
                    const auto found = std::ranges::find(type->parameters, parameter.parameterId,
                        &talas::core::ParameterDescriptor::id);
                    if (found != type->parameters.end())
                        field = &*found;
                }
                ProjectSoundExposedParameter exposed;
                exposed.parameterId = reference.name;
                exposed.displayName = reference.name;
                exposed.displayName.front() = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(exposed.displayName.front())));
                exposed.kind = talas::core::ParameterKind::Expression;
                exposed.domain = field != nullptr ? field->domain : talas::expr::ValueDomain::Scalar;
                exposed.defaultValue.parameterId = reference.name;
                exposed.defaultValue.expression = findVisibleProjectVariableDeclaration(document, reference.name) != nullptr
                    ? "?" + reference.name
                    : field != nullptr && !field->defaultExpression.empty() ? field->defaultExpression
                                                                           : std::string{"0"};
                exposed.bindings.push_back({ProjectSoundParameterTarget::Variable, {}, {}, {},
                    talas::expr::ValueDomain::Scalar, reference.name});
                definition.exposedParameters.push_back(std::move(exposed));
            }
        }
    }
}

void applySoundInterfaceBindingsForInline(
    const ProjectSoundDefinition& definition,
    const ProjectNode& instance,
    ProjectNode& internalNode)
{
    // The class's ?variables take the instance's values in its fields
    //, before any parameter sets a whole field.
    std::vector<std::pair<std::string, std::string>> variables;
    for (const auto& exposed : definition.exposedParameters)
    {
        const auto& resolved = resolveExposedValue(exposed, instance);
        for (const auto& binding : exposed.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::Variable && resolved.expression.has_value())
                variables.emplace_back(binding.variableName, *resolved.expression);
        }
    }
    if (!variables.empty())
    {
        for (auto& parameter : internalNode.parameters)
        {
            if (parameter.expression.has_value())
                parameter.expression = withClassVariables(*parameter.expression, variables);
        }
    }
    for (const auto& exposed : definition.exposedParameters)
    {
        const auto& resolved = resolveExposedValue(
            exposed,
            instance);
        for (const auto& binding : exposed.bindings)
        {
            if (binding.target
                    != ProjectSoundParameterTarget::NodeParameter
                || binding.nodeId != internalNode.instanceId)
            {
                continue;
            }
            auto materialized = resolved;
            materialized.parameterId = binding.parameterId;
            const auto existing = std::ranges::find_if(
                internalNode.parameters,
                [&](const auto& value) {
                    return value.parameterId
                        == binding.parameterId;
                });
            if (existing == internalNode.parameters.end())
                internalNode.parameters.push_back(std::move(materialized));
            else
                *existing = std::move(materialized);
        }
    }
}

[[nodiscard]] const talas::core::PortDescriptor* findPort(const std::vector<talas::core::PortDescriptor>& ports,
                                                        std::string_view portId)
{
    const auto found = std::ranges::find_if(ports, [&](const talas::core::PortDescriptor& port) {
        return port.id == portId;
    });
    if (found == ports.end())
        return nullptr;
    return &*found;
}

// Disconnecting an input that accepts many connections removes its most
// recent one; any other input loses its connection.
void removeInputConnections(
    std::vector<talas::core::Connection>& connections,
    const talas::core::PortDescriptor& input,
    std::string_view toNode)
{
    const auto into = [&](const talas::core::Connection& connection) {
        return connection.toNode == toNode && connection.toPort == input.id;
    };
    if (!input.acceptsMany)
    {
        std::erase_if(connections, into);
        return;
    }
    for (auto index = connections.size(); index > 0; --index)
    {
        if (into(connections[index - 1]))
        {
            connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(index - 1));
            return;
        }
    }
}

[[nodiscard]] const talas::core::ParameterDescriptor* findParameter(const talas::core::NodeTypeDescriptor& descriptor,
                                                                   std::string_view parameterId)
{
    const auto found = std::ranges::find_if(descriptor.parameters, [&](const talas::core::ParameterDescriptor& parameter) {
        return parameter.id == parameterId;
    });
    if (found == descriptor.parameters.end())
        return nullptr;
    return &*found;
}

[[nodiscard]] std::size_t parameterValueCount(const ProjectParameterExpression& parameter)
{
    return static_cast<std::size_t>(parameter.expression.has_value())
        + static_cast<std::size_t>(parameter.choice.has_value())
        + static_cast<std::size_t>(parameter.boolean.has_value())
        + static_cast<std::size_t>(parameter.resource.has_value())
        + static_cast<std::size_t>(parameter.text.has_value());
}

[[nodiscard]] EditResult validateParameterValue(const ProjectParameterExpression& parameter,
                                                 const talas::core::ParameterDescriptor& descriptor)
{
    if (parameterValueCount(parameter) != 1)
    {
        return failure(EditError::InvalidParameterValue,
                       "Parameter must provide exactly one of expression, choice, boolean, resource, or text");
    }

    switch (descriptor.kind)
    {
    case talas::core::ParameterKind::Expression:
        if (parameter.expression.has_value())
            return success();
        break;
    case talas::core::ParameterKind::Choice:
        if (parameter.choice.has_value()
            && std::ranges::find(descriptor.choices, *parameter.choice) != descriptor.choices.end())
        {
            return success();
        }
        break;
    case talas::core::ParameterKind::Boolean:
        if (parameter.boolean.has_value())
            return success();
        break;
    case talas::core::ParameterKind::Resource:
        if (parameter.resource.has_value())
        {
            if (talas::core::beginsStaticStringResourceSelection(
                    *parameter.resource)
                && !talas::core::selectBoundedStaticStringResource(
                    *parameter.resource).has_value())
            {
                return failure(EditError::InvalidParameterValue,
                    "Resource collection requires literal Strings and a one-based in-range at: selector");
            }
            return success();
        }
        break;
    case talas::core::ParameterKind::Text:
        if (parameter.text.has_value())
            return success();
        break;
    }

    return failure(EditError::InvalidParameterValue, "Parameter value does not match its descriptor kind");
}

[[nodiscard]] EditResult validateNodeParameters(const ProjectNode& node,
                                                 const talas::core::NodeTypeDescriptor& descriptor)
{
    std::unordered_set<std::string> parameterIds;
    for (const auto& parameter : node.parameters)
    {
        if (!parameterIds.insert(parameter.parameterId).second)
        {
            return failure(EditError::DuplicateParameter,
                           "Node contains duplicate parameter: " + parameter.parameterId);
        }

        const auto* descriptorParameter = findParameter(descriptor, parameter.parameterId);
        if (descriptorParameter == nullptr)
        {
            return failure(EditError::UnknownParameter,
                           "Unknown parameter for node type " + descriptor.typeId + ": " + parameter.parameterId);
        }

        const auto valueResult = validateParameterValue(parameter, *descriptorParameter);
        if (!valueResult.ok())
            return valueResult;
    }

    return success();
}

[[nodiscard]] EditResult validateVirtualControl(const VirtualControl& control)
{
    if (!isValidEventName(control.eventName))
        return failure(EditError::InvalidVirtualControl, "Event Value name must be a valid identifier");
    if (!std::isfinite(control.value) || !std::isfinite(control.minimum) || !std::isfinite(control.maximum))
        return failure(EditError::InvalidVirtualControl, "Event Value values must be finite");
    if (!(control.minimum < control.maximum))
        return failure(EditError::InvalidVirtualControl, "Event Value minimum must be less than maximum");
    if (control.value < control.minimum || control.value > control.maximum)
        return failure(EditError::InvalidVirtualControl, "Event Value must be within its persisted range");
    if (control.midiMapping.has_value()
        && !isValidMidiControlMapping(*control.midiMapping))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "MIDI mapping must identify a valid exact or migrated legacy input");
    }
    if (control.oscMapping.has_value()
        && !isValidOscControlMapping(*control.oscMapping))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "OSC mapping must use one exact bounded address");
    }
    if (control.oscEgressMapping.has_value()
        && !isValidOscEgressMapping(*control.oscEgressMapping))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "OSC feedback mapping must use one exact bounded address");
    }
    return success();
}

[[nodiscard]] EditResult validateVirtualControls(const std::vector<VirtualControl>& controls);

// The dormant widgets, at most 256, of Event Values
// the surface has no control of.
[[nodiscard]] EditResult validateDormantVirtualControls(const ProjectDocument& document)
{
    if (document.dormantVirtualControls.size() > maximumDormantVirtualControls)
        return failure(EditError::InvalidVirtualControl, "A surface keeps at most 256 dormant widgets");
    for (const auto& dormant : document.dormantVirtualControls)
    {
        if (std::ranges::any_of(document.virtualControls, [&](const VirtualControl& control) { return control.eventName == dormant.eventName; }))
            return failure(EditError::DuplicateVirtualControl, "A dormant widget's Event Value has a control: " + dormant.eventName);
    }
    return validateVirtualControls(document.dormantVirtualControls);
}

[[nodiscard]] EditResult validateVirtualControls(const std::vector<VirtualControl>& controls)
{
    std::unordered_set<std::string> eventNames;
    std::vector<const MidiControlMapping*> midiMappings;
    std::unordered_set<std::string> oscAddresses;
    std::size_t oscMappingCount = 0;
    std::unordered_set<std::string> oscEgressAddresses;
    std::size_t oscEgressMappingCount = 0;
    for (const auto& control : controls)
    {
        const auto result = validateVirtualControl(control);
        if (!result.ok())
            return result;
        if (!eventNames.insert(control.eventName).second)
            return failure(EditError::DuplicateVirtualControl, "Duplicate Event Value name: " + control.eventName);
        if (control.midiMapping.has_value())
        {
            if (std::ranges::any_of(
                    midiMappings,
                    [&](const auto* existing) {
                        return midiControlMappingsConflict(
                            *existing, *control.midiMapping);
                    }))
            {
                return failure(
                    EditError::DuplicateMidiMapping,
                    "Duplicate MIDI controller mapping for the same input authority");
            }
            midiMappings.push_back(&*control.midiMapping);
        }
        if (control.oscMapping.has_value())
        {
            if (++oscMappingCount > maximumOscControlMappings)
            {
                return failure(
                    EditError::DuplicateOscMapping,
                    "OSC mappings exceed the fixed 128-route limit");
            }
            if (!oscAddresses.insert(control.oscMapping->address).second)
            {
                return failure(
                    EditError::DuplicateOscMapping,
                    "Duplicate exact OSC address");
            }
        }
        if (control.oscEgressMapping.has_value())
        {
            if (++oscEgressMappingCount > maximumOscControlMappings)
            {
                return failure(
                    EditError::DuplicateOscMapping,
                    "OSC feedback mappings exceed the fixed 128-route limit");
            }
            if (!oscEgressAddresses.insert(
                    control.oscEgressMapping->address).second)
            {
                return failure(
                    EditError::DuplicateOscMapping,
                    "Duplicate exact OSC feedback address");
            }
        }
    }
    return success();
}

[[nodiscard]] EditResult validateAsset(const ProjectAsset& asset)
{
    const auto invalid = validateProjectAssetManifest(asset);
    return invalid.has_value()
        ? failure(EditError::InvalidAsset, invalid->message)
        : success();
}

[[nodiscard]] EditResult validateAssets(const std::vector<ProjectAsset>& assets)
{
    std::unordered_set<std::string> ids;
    for (const auto& asset : assets)
    {
        const auto result = validateAsset(asset);
        if (!result.ok())
            return result;
        if (!ids.insert(asset.id).second)
        {
            return failure(
                EditError::DuplicateAsset,
                "Duplicate project asset id/generation: "
                    + asset.id + "/"
                    + std::to_string(asset.generation));
        }
    }
    return success();
}

[[nodiscard]] EditResult validateDeviceSettings(
    const ProjectDeviceSettings& settings)
{
    if (!isValidOscIngressSettings(settings.oscIngress))
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "OSC ingress must be disabled on port 9000 or enabled on port 1024...65535");
    }
    if (!isValidOscEgressSettings(settings.oscEgress))
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "OSC feedback must be disabled on port 9001 or enabled on port 1024...65535");
    }
    if (!isValidOscEndpointPair(settings.oscIngress, settings.oscEgress))
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "OSC ingress and feedback ports must differ while both are enabled");
    }
    switch (settings.transportSync.sourceMode)
    {
    case TransportSyncSourceMode::Internal:
        if (!settings.transportSync.sourceIdentifier.empty())
        {
            return failure(
                EditError::InvalidDeviceSettings,
                "Internal transport authority requires an empty source identifier");
        }
        break;
    case TransportSyncSourceMode::MidiClockExactInput:
    case TransportSyncSourceMode::MidiTimeCodeExactInput:
        if (!settings.configured
            || !isValidMidiSourceIdentifier(
                settings.transportSync.sourceIdentifier)
            || std::ranges::find(
                   settings.midiInputDeviceIdentifiers,
                   settings.transportSync.sourceIdentifier)
                == settings.midiInputDeviceIdentifiers.end())
        {
            return failure(
                EditError::InvalidDeviceSettings,
                "Exact transport authority requires one enabled bounded MIDI input identifier");
        }
        break;
    default:
        return failure(
            EditError::InvalidDeviceSettings,
            "Unknown transport sync authority");
    }
    if (!settings.configured)
        return success();
    if (!std::isfinite(settings.sampleRate) || settings.sampleRate <= 0.0)
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "Configured audio sample rate must be positive and finite");
    }
    if (settings.bufferSize < 1 || settings.bufferSize > 1048576)
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "Configured audio buffer size must be between 1 and 1048576 frames");
    }
    const auto validateChannels = [](const auto& channels) {
        std::unordered_set<std::int32_t> unique;
        return std::ranges::all_of(channels, [&](const auto channel) {
            return channel >= 1 && channel <= 8
                && unique.insert(channel).second;
        });
    };
    if (!validateChannels(settings.inputChannels)
        || !validateChannels(settings.outputChannels))
    {
        return failure(
            EditError::InvalidDeviceSettings,
            "Audio channel lists must contain unique one-based channels 1...8");
    }
    std::unordered_set<std::string> identifiers;
    for (const auto& identifier : settings.midiInputDeviceIdentifiers)
    {
        if (!isValidMidiSourceIdentifier(identifier)
            || !identifiers.insert(identifier).second)
        {
            return failure(
                EditError::InvalidDeviceSettings,
                "MIDI input device identifiers must be unique and non-empty");
        }
    }
    return success();
}

[[nodiscard]] EditResult validateTimeline(const ProjectTimeline& timeline)
{
    if (const auto error =
            projectMusicalTimeValidationError(timeline.musicalTime))
    {
        return failure(EditError::InvalidTimeline, *error);
    }
    if (!std::isfinite(timeline.loop.startSeconds)
        || !std::isfinite(timeline.loop.endSeconds)
        || timeline.loop.startSeconds < 0.0
        || timeline.loop.endSeconds <= timeline.loop.startSeconds)
    {
        return failure(
            EditError::InvalidTimeline,
            "Timeline loop requires finite non-negative bounds with end later than start");
    }

    double previousTime = 0.0;
    bool hasPreviousTime = false;
    std::unordered_set<std::string> namesAtCurrentTime;
    for (const auto& event : timeline.eventValues)
    {
        if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0
            || !std::isfinite(event.value) || !isValidEventName(event.eventName)
            || !isValidValueDomain(event.domain)
            || !isValidAutomationInterpolation(event.interpolation))
        {
            return failure(
                EditError::InvalidTimeline,
                "Timeline Event Values require a finite non-negative time, identifier name, and finite value");
        }
        if (hasPreviousTime && event.timeSeconds < previousTime)
            return failure(EditError::InvalidTimeline, "Timeline Event Values must be in chronological order");
        if (!hasPreviousTime || event.timeSeconds != previousTime)
            namesAtCurrentTime.clear();
        if (!namesAtCurrentTime.insert(event.eventName).second)
            return failure(EditError::InvalidTimeline, "Duplicate Event Value at the same Timeline time");
        previousTime = event.timeSeconds;
        hasPreviousTime = true;
    }
    return success();
}

[[nodiscard]] std::vector<std::string_view> scoreSoundDefinitionIds(
    const ProjectDocument& document)
{
    std::vector<std::string_view> result;
    result.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
        result.push_back(definition.definitionId);
    return result;
}

[[nodiscard]] EditResult validateScore(const ProjectDocument& document)
{
    const auto definitionIds = scoreSoundDefinitionIds(document);
    if (const auto error = projectScoreValidationError(
            document.timeline.score,
            definitionIds))
    {
        return failure(EditError::InvalidScore, error->message);
    }
    if (const auto error = validateProjectScoreSourceBindings(document))
        return failure(EditError::InvalidScore, error->message);
    return success();
}

template <typename Entity, typename Identity>
[[nodiscard]] bool moveScoreEntity(
    std::vector<Entity>& entities,
    std::string_view identity,
    std::size_t destinationIndex,
    Identity member)
{
    if (destinationIndex >= entities.size())
        return false;
    const auto found = std::ranges::find(entities, identity, member);
    if (found == entities.end())
        return false;
    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(entities.begin(), found));
    if (sourceIndex < destinationIndex)
    {
        std::rotate(
            entities.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
            entities.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1U),
            entities.begin() + static_cast<std::ptrdiff_t>(destinationIndex + 1U));
    }
    else if (sourceIndex > destinationIndex)
    {
        std::rotate(
            entities.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
            entities.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
            entities.begin() + static_cast<std::ptrdiff_t>(sourceIndex + 1U));
    }
    return true;
}

[[nodiscard]] bool timelineEventsEqual(const TimelineEventValue& lhs, const TimelineEventValue& rhs)
{
    return lhs.timeSeconds == rhs.timeSeconds
        && lhs.eventName == rhs.eventName
        && lhs.domain == rhs.domain
        && lhs.value == rhs.value
        && lhs.interpolation == rhs.interpolation;
}

void sortTimeline(ProjectTimeline& timeline)
{
    std::ranges::stable_sort(timeline.eventValues, {}, &TimelineEventValue::timeSeconds);
}

[[nodiscard]] bool containsCycle(const ProjectDocument& document, const talas::core::NodeRegistry& registry)
{
    const auto validation =
        validateProjectDocumentForEditing(document, registry);
    return std::ranges::any_of(validation.errors, [](const talas::core::ValidationError& error) {
        return error.code == talas::core::ValidationCode::CycleDetected;
    });
}

[[nodiscard]] EditResult projectValidationFailure(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    bool allowIncompleteDefinitions = false)
{
    const auto validation =
        validateProjectDocumentForEditing(document, registry);
    const auto found = std::ranges::find_if(
        validation.errors,
        [allowIncompleteDefinitions](const auto& error) {
            return error.code != talas::core::ValidationCode::MissingRequiredInput
                || (!allowIncompleteDefinitions
                    && error.nodeId.find('/') != std::string::npos);
        });
    if (found == validation.errors.end())
        return success();

    const auto inDefinition =
        found->nodeId.find('/') != std::string::npos
        || found->code == talas::core::ValidationCode::InvalidSoundBoundary
        || found->code
            == talas::core::ValidationCode::RecursiveSoundDefinition;
    if (inDefinition)
        return failure(EditError::InvalidSoundDefinition, found->message);

    switch (found->code)
    {
    case talas::core::ValidationCode::UnknownNodeType:
        return failure(EditError::UnknownNodeType, found->message);
    case talas::core::ValidationCode::UnknownSoundDefinition:
        return failure(EditError::UnknownSoundDefinition, found->message);
    case talas::core::ValidationCode::UnknownNodeInstance:
        return failure(EditError::UnknownNodeId, found->message);
    case talas::core::ValidationCode::UnknownPort:
        return failure(EditError::UnknownPort, found->message);
    case talas::core::ValidationCode::PortTypeMismatch:
        return failure(EditError::IncompatiblePortTypes, found->message);
    case talas::core::ValidationCode::DuplicateInputConnection:
        return failure(EditError::DuplicateInputConnection, found->message);
    case talas::core::ValidationCode::DuplicateNodeInstance:
        return failure(EditError::DuplicateNodeId, found->message);
    case talas::core::ValidationCode::InvalidProjectAsset:
        return failure(EditError::InvalidAsset, found->message);
    case talas::core::ValidationCode::DuplicateProjectAsset:
        return failure(EditError::DuplicateAsset, found->message);
    case talas::core::ValidationCode::InvalidSampleMap:
        return failure(EditError::InvalidSampleMap, found->message);
    case talas::core::ValidationCode::CycleDetected:
    case talas::core::ValidationCode::InvalidNodeIdentity:
    case talas::core::ValidationCode::InvalidProjectVariable:
        return failure(EditError::InvalidGraph, found->message);
    case talas::core::ValidationCode::InvalidLiveSet:
        return failure(EditError::InvalidLiveSet, found->message);
    case talas::core::ValidationCode::InvalidParameterTransformation:
        return failure(EditError::InvalidParameterValue, found->message);
    case talas::core::ValidationCode::MissingRequiredInput:
    case talas::core::ValidationCode::InvalidSoundBoundary:
    case talas::core::ValidationCode::RecursiveSoundDefinition:
    case talas::core::ValidationCode::SoundExpansionLimitExceeded:
        break;
    }
    return failure(EditError::InvalidSoundDefinition, found->message);
}

struct DiskCacheAuthoringTarget
{
    ProjectNode* node = nullptr;
    std::string fileNameParameterId = "fileName";
    std::string recordParameterId = "record";
    std::optional<std::string> effectiveFileName;
    bool effectiveRecord = false;
};

[[nodiscard]] DiskCacheAuthoringTarget findAuthoredDiskCacheTarget(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view sourcePath,
    std::string& error)
{
    DiskCacheAuthoringTarget result;
    const auto expanded = expandProjectSounds(document, registry);
    if (!expanded.ok())
    {
        error = "DiskCache capture could not resolve reusable-Sound authority";
        return result;
    }
    const auto identity = std::ranges::find(
        expanded.manifest.nodes,
        sourcePath,
        &ExpandedSoundNodeIdentity::sourcePath);
    if (identity == expanded.manifest.nodes.end())
    {
        error = "DiskCache capture target no longer exists";
        return result;
    }
    const auto effectiveNode = std::ranges::find(
        expanded.document.graph.nodes,
        identity->flatNodeId,
        &ProjectNode::instanceId);
    if (effectiveNode == expanded.document.graph.nodes.end()
        || effectiveNode->typeId != "talas.diskCache")
    {
        error = "DiskCache capture target no longer resolves exactly";
        return result;
    }
    const auto effectiveRecord = std::ranges::find(
        effectiveNode->parameters,
        std::string_view{"record"},
        &ProjectParameterExpression::parameterId);
    result.effectiveRecord = effectiveRecord
            != effectiveNode->parameters.end()
        && effectiveRecord->boolean == std::optional<bool>{true};
    const auto effectiveFileName = std::ranges::find(
        effectiveNode->parameters,
        std::string_view{"fileName"},
        &ProjectParameterExpression::parameterId);
    if (effectiveFileName != effectiveNode->parameters.end())
        result.effectiveFileName = effectiveFileName->resource;

    const auto assignDirectNode = [&](ProjectNode* node) {
        if (node == nullptr || node->typeId != "talas.diskCache")
        {
            error = "DiskCache capture authored target no longer resolves exactly";
            return false;
        }
        result.node = node;
        return true;
    };
    if (identity->instancePath.empty())
    {
        const auto node = std::ranges::find(
            document.graph.nodes,
            identity->localNodeId,
            &ProjectNode::instanceId);
        assignDirectNode(
            node == document.graph.nodes.end() ? nullptr : &*node);
        return result;
    }

    const auto definition = std::ranges::find(
        document.soundDefinitions,
        identity->definitionId,
        &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
    {
        error = "DiskCache capture definition owner no longer exists";
        return result;
    }
    const auto definitionNode = std::ranges::find(
        definition->graph.nodes,
        identity->localNodeId,
        &ProjectNode::instanceId);
    if (definitionNode == definition->graph.nodes.end()
        || definitionNode->typeId != "talas.diskCache")
    {
        error = "DiskCache capture definition target no longer resolves exactly";
        return result;
    }

    const auto expandedUseCount = std::ranges::count_if(
        expanded.manifest.nodes,
        [&](const auto& candidate) {
            return candidate.definitionId == identity->definitionId
                && candidate.localNodeId == identity->localNodeId;
        });
    if (expandedUseCount == 1)
    {
        result.node = &*definitionNode;
        return result;
    }

    if (identity->instancePath.size() != 1)
    {
        error = "DiskCache capture requires direct exposed parameters for a multiply-instantiated nested target";
        return result;
    }
    const auto findExposed = [&](std::string_view internalParameterId,
                                 talas::core::ParameterKind kind)
        -> const ProjectSoundExposedParameter* {
        const ProjectSoundExposedParameter* found = nullptr;
        for (const auto& exposed : definition->exposedParameters)
        {
            if (exposed.kind != kind)
                continue;
            const auto bindsTarget = std::ranges::any_of(
                exposed.bindings,
                [&](const auto& binding) {
                    return binding.target
                            == ProjectSoundParameterTarget::NodeParameter
                        && binding.nodeId == identity->localNodeId
                        && binding.parameterId == internalParameterId;
                });
            if (!bindsTarget)
                continue;
            if (found != nullptr)
                return nullptr;
            found = &exposed;
        }
        return found;
    };
    const auto* exposedFileName = findExposed(
        "fileName", talas::core::ParameterKind::Resource);
    const auto* exposedRecord = findExposed(
        "record", talas::core::ParameterKind::Boolean);
    if (exposedFileName == nullptr || exposedRecord == nullptr
        || exposedFileName == exposedRecord)
    {
        error = "DiskCache capture cannot isolate this Sound instance without exact fileName and record bindings";
        return result;
    }
    const auto instance = std::ranges::find(
        document.graph.nodes,
        identity->instancePath.front(),
        &ProjectNode::instanceId);
    if (instance == document.graph.nodes.end()
        || instance->soundDefinitionId
            != std::optional<std::string>{identity->definitionId})
    {
        error = "DiskCache capture exact Sound instance no longer exists";
        return result;
    }
    result.node = &*instance;
    result.fileNameParameterId = exposedFileName->parameterId;
    result.recordParameterId = exposedRecord->parameterId;
    return result;
}

void setDiskCacheParameter(
    ProjectNode& node,
    ProjectParameterExpression value)
{
    const auto found = std::ranges::find(
        node.parameters,
        value.parameterId,
        &ProjectParameterExpression::parameterId);
    if (found == node.parameters.end())
        node.parameters.push_back(std::move(value));
    else
        *found = std::move(value);
}

[[nodiscard]] ProjectGraph* graphForEditScope(
    ProjectDocument& document,
    std::optional<std::string_view> definitionId)
{
    if (!definitionId.has_value())
        return &document.graph;
    auto* definition = findSoundDefinition(document, *definitionId);
    return definition == nullptr ? nullptr : &definition->graph;
}

[[nodiscard]] EditResult validateGraphNodeSelection(
    const ProjectGraph& graph,
    std::span<const std::string> nodeIds,
    std::unordered_set<std::string>& selected)
{
    selected.clear();
    if (nodeIds.empty())
        return failure(EditError::InvalidSelection, "Graph node selection is empty");
    selected.reserve(nodeIds.size());
    for (const auto& nodeId : nodeIds)
    {
        if (nodeId.empty() || findNode(graph, nodeId) == nullptr)
        {
            return failure(
                EditError::UnknownNodeId,
                "Unknown selected node: " + nodeId);
        }
        if (!selected.insert(nodeId).second)
        {
            return failure(
                EditError::InvalidSelection,
                "Graph node selection contains a duplicate id: " + nodeId);
        }
    }
    return success();
}

[[nodiscard]] std::optional<std::string> nextDuplicateNodeId(
    std::string_view sourceId,
    std::unordered_set<std::string>& used)
{
    const auto maximumAttempts = used.size() + 1;
    for (std::size_t attempt = 0; attempt < maximumAttempts; ++attempt)
    {
        auto candidate = std::string{sourceId} + "-copy";
        if (attempt != 0)
            candidate += "-" + std::to_string(attempt + 1);
        if (used.insert(candidate).second)
            return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] EditResult duplicateGraphSelection(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    std::span<const std::string> nodeIds,
    CanvasPosition offset,
    std::vector<std::string>& duplicatedNodeIds,
    const talas::core::NodeRegistry& registry)
{
    duplicatedNodeIds.clear();
    std::vector<std::string> successfulDuplicateIds;
    reconcileProjectPresentation(candidate);
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    std::unordered_set<std::string> selected;
    if (const auto result = validateGraphNodeSelection(
            *graph, nodeIds, selected);
        !result.ok())
    {
        return result;
    }

    std::unordered_set<std::string> used;
    used.reserve(graph->nodes.size() + selected.size());
    for (const auto& node : graph->nodes)
        used.insert(node.instanceId);

    std::unordered_map<std::string, std::string> duplicateIds;
    duplicateIds.reserve(selected.size());
    std::vector<ProjectNode> duplicateNodes;
    duplicateNodes.reserve(selected.size());
    std::vector<ProjectNodePresentation> duplicatePresentation;
    duplicatePresentation.reserve(selected.size());
    for (const auto& node : graph->nodes)
    {
        if (!selected.contains(node.instanceId))
            continue;
        const auto* presented = findProjectNodePresentation(
            candidate, definitionId, node.instanceId);
        if (presented == nullptr)
        {
            return failure(
                EditError::InvalidSelection,
                "Selected node is missing portable presentation state: "
                    + node.instanceId);
        }
        const auto translated = translatedPosition(
            presented->position, offset.x, offset.y);
        if (!translated.has_value())
        {
            return failure(
                EditError::InvalidSelection,
                "Duplicated node position exceeds the supported canvas range");
        }
        auto clone = node;
        const auto duplicateId = nextDuplicateNodeId(node.instanceId, used);
        if (!duplicateId.has_value())
        {
            duplicatedNodeIds.clear();
            return failure(
                EditError::InvalidSelection,
                "Unable to allocate a collision-free duplicate node id");
        }
        clone.instanceId = *duplicateId;
        duplicateIds.emplace(node.instanceId, clone.instanceId);
        successfulDuplicateIds.push_back(clone.instanceId);
        duplicatePresentation.push_back({clone.instanceId, *translated});
        duplicateNodes.push_back(std::move(clone));
    }

    std::vector<talas::core::Connection> duplicateConnections;
    for (const auto& connection : graph->connections)
    {
        const auto from = duplicateIds.find(connection.fromNode);
        const auto to = duplicateIds.find(connection.toNode);
        if (from == duplicateIds.end() || to == duplicateIds.end())
            continue;
        auto clone = connection;
        clone.fromNode = from->second;
        clone.toNode = to->second;
        duplicateConnections.push_back(std::move(clone));
    }
    graph->nodes.insert(
        graph->nodes.end(),
        std::make_move_iterator(duplicateNodes.begin()),
        std::make_move_iterator(duplicateNodes.end()));
    graph->connections.insert(
        graph->connections.end(),
        std::make_move_iterator(duplicateConnections.begin()),
        std::make_move_iterator(duplicateConnections.end()));
    auto* layout = findProjectStructureLayout(candidate, definitionId);
    if (layout == nullptr)
        return failure(EditError::InvalidSelection, "Graph presentation scope is unavailable");
    layout->nodes.insert(
        layout->nodes.end(),
        std::make_move_iterator(duplicatePresentation.begin()),
        std::make_move_iterator(duplicatePresentation.end()));

    const auto validation = projectValidationFailure(candidate, registry, true);
    if (!validation.ok())
        return validation;
    duplicatedNodeIds = std::move(successfulDuplicateIds);
    return success();
}

[[nodiscard]] EditResult removeGraphSelection(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    std::span<const std::string> nodeIds)
{
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    std::unordered_set<std::string> selected;
    if (const auto result = validateGraphNodeSelection(
            *graph, nodeIds, selected);
        !result.ok())
    {
        return result;
    }
    std::erase_if(graph->nodes, [&](const auto& node) {
        return selected.contains(node.instanceId);
    });
    std::erase_if(graph->connections, [&](const auto& connection) {
        return selected.contains(connection.fromNode)
            || selected.contains(connection.toNode);
    });
    // A removed replaceable input leaves the Sound without one.
    if (definitionId.has_value())
    {
        if (auto* definition = findSoundDefinition(candidate, *definitionId))
        {
            std::erase_if(definition->inputs, [&](const ProjectSoundPort& port) {
                return port.replacesSound && selected.contains(port.nodeId);
            });
        }
    }
    reconcileProjectPresentation(candidate);
    return success();
}

// A node's new name in every place that names it within its graph scope
//.
[[nodiscard]] EditResult renameGraphNode(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    std::string_view oldId,
    const std::string& newId)
{
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    auto* node = findNode(*graph, oldId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string{oldId});
    const auto badCharacter = std::ranges::any_of(newId, [](char character) {
        return static_cast<unsigned char>(character) < 0x20 || character == 0x7F || character == '/';
    });
    if (newId.empty() || newId.size() > 128 || badCharacter || newId.front() == ' ' || newId.back() == ' ')
    {
        return failure(
            EditError::InvalidNodeId,
            "A Sound's name is 1 to 128 bytes without control characters, '/' or a space at either end");
    }
    if (newId == oldId)
        return success();
    if (findNode(*graph, newId) != nullptr)
        return failure(EditError::DuplicateNodeId, "Duplicate node instance id: " + newId);
    reconcileProjectPresentation(candidate);
    const std::string old{oldId};
    node->instanceId = newId;
    for (auto& connection : graph->connections)
    {
        if (connection.fromNode == old)
            connection.fromNode = newId;
        if (connection.toNode == old)
            connection.toNode = newId;
    }
    if (auto* presented = findProjectNodePresentation(candidate, definitionId, old))
        presented->nodeId = newId;
    const std::string scope{definitionId.value_or(std::string_view{})};
    if (definitionId.has_value())
    {
        auto* definition = findSoundDefinition(candidate, *definitionId);
        for (auto* ports : {&definition->inputs, &definition->outputs})
        {
            for (auto& port : *ports)
            {
                if (port.nodeId == old)
                    port.nodeId = newId;
            }
        }
        for (auto& exposed : definition->exposedParameters)
        {
            for (auto& binding : exposed.bindings)
            {
                if (binding.nodeId == old)
                    binding.nodeId = newId;
            }
        }
    }
    const auto renameOwner = [&](ProjectVariableOwner& owner) {
        if (owner.kind == ProjectVariableOwnerKind::MidiVoice && owner.definitionId == scope && owner.nodeId == old)
            owner.nodeId = newId;
    };
    for (auto& variable : candidate.variableLibrary.variables)
        renameOwner(variable.owner);
    for (auto& variable : candidate.soundVariableLibrary.variables)
        renameOwner(variable.owner);
    // An instance path names a Sound instance in the root graph first, then
    // in the definition each one before it plays.
    const auto targetScope = definitionId.has_value() ? std::optional<std::string>{scope} : std::nullopt;
    const auto renamePath = [&](std::vector<std::string>& path) {
        const ProjectGraph* containing = &candidate.graph;
        std::optional<std::string> containingDefinition;
        for (auto& element : path)
        {
            if (containing == nullptr)
                return;
            if (containingDefinition == targetScope && element == old)
                element = newId;
            const auto* instance = findNode(*containing, element);
            if (instance == nullptr || !instance->soundDefinitionId.has_value())
                return;
            containingDefinition = *instance->soundDefinitionId;
            const auto* definition = findSoundDefinition(candidate, *containingDefinition);
            containing = definition == nullptr ? nullptr : &definition->graph;
        }
    };
    const auto inScope = [&](const std::vector<std::string>& path, const std::optional<std::string>& containing) {
        if (path.empty())
            return containing == targetScope;
        // The node a path's target names lies in the last instance's
        // definition.
        const auto* graphOf = static_cast<const ProjectGraph*>(&candidate.graph);
        std::optional<std::string> last;
        for (const auto& element : path)
        {
            const auto* instance = graphOf == nullptr ? nullptr : findNode(*graphOf, element);
            if (instance == nullptr || !instance->soundDefinitionId.has_value())
                return false;
            last = *instance->soundDefinitionId;
            const auto* definition = findSoundDefinition(candidate, *last);
            graphOf = definition == nullptr ? nullptr : &definition->graph;
        }
        return last == targetScope;
    };
    const auto renameTarget = [&](SnapshotTargetIdentity& target) {
        renamePath(target.instancePath);
        if (!target.nodeId.empty() && target.nodeId == old && inScope(target.instancePath, target.containingDefinitionId))
            target.nodeId = newId;
    };
    for (auto& bank : candidate.snapshotLibrary.banks)
    {
        renamePath(bank.scope.instancePath);
        if (bank.scope.nodeId == old && inScope(bank.scope.instancePath, bank.scope.containingDefinitionId))
            bank.scope.nodeId = newId;
        for (auto& policy : bank.parameterPolicies)
            renameTarget(policy.target);
        for (auto& snapshot : bank.snapshots)
        {
            for (auto& entry : snapshot.values)
                renameTarget(entry.target);
        }
    }
    for (auto& control : candidate.presentation.performanceControls)
    {
        renameTarget(control.target);
        for (auto& slot : control.modulationSlots)
            renameTarget(slot.target);
    }
    return success();
}

// The checks addNode makes of a node about to join a graph scope.
[[nodiscard]] EditResult validateNodeForGraph(
    const ProjectDocument& document,
    const ProjectGraph& graph,
    ProjectNode& node,
    const talas::core::NodeRegistry& registry)
{
    if (node.instanceId.empty())
        return failure(EditError::InvalidNodeId, "Node instance id cannot be empty");
    if (findNode(graph, node.instanceId) != nullptr)
        return failure(EditError::DuplicateNodeId, "Duplicate node instance id: " + node.instanceId);
    if (node.typeId.empty() == !node.soundDefinitionId.has_value())
    {
        return failure(
            EditError::InvalidGraph,
            "Node must reference exactly one built-in type or Sound definition");
    }
    if (node.soundDefinitionId.has_value()
        && findSoundDefinition(document, *node.soundDefinitionId) == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + *node.soundDefinitionId);
    }
    const auto descriptor = describeProjectNode(document, node, registry);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Unknown node type: " + node.typeId);
    if (const auto preservation =
            freezeAuthoredExternalComponent(node, *descriptor);
        preservation.has_value())
    {
        return *preservation;
    }
    return validateNodeParameters(node, *descriptor);
}

// Any signal may feed any input, so an input of the source's own kind
// (an audio input for an audio line) comes before the others.
[[nodiscard]] const talas::core::PortDescriptor* firstCompatibleInput(
    const talas::core::NodeTypeDescriptor& descriptor,
    const talas::core::PortDescriptor& source,
    const std::function<bool(const talas::core::PortDescriptor&)>& available = {})
{
    for (const auto sameKind : {true, false})
    {
        for (const auto& input : descriptor.inputs)
        {
            if (!input.compilerPopulated
                && (input.type == source.type) == sameKind
                && talas::core::portDescriptorsCompatible(source, input)
                && (!available || available(input)))
            {
                return &input;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] const talas::core::PortDescriptor* firstCompatibleOutput(
    const talas::core::NodeTypeDescriptor& descriptor,
    const talas::core::PortDescriptor& destination)
{
    for (const auto sameKind : {true, false})
    {
        for (const auto& output : descriptor.outputs)
        {
            if (!output.compilerPopulated
                && (output.type == destination.type) == sameKind
                && talas::core::portDescriptorsCompatible(output, destination))
            {
                return &output;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] EditResult insertGraphNodeOnConnection(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    ProjectNode node,
    CanvasPosition position,
    const talas::core::Connection& wire,
    const talas::core::NodeRegistry& registry,
    std::optional<std::string_view> inputPortId)
{
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    const auto existing = std::ranges::find(graph->connections, wire);
    if (existing == graph->connections.end())
        return failure(EditError::InvalidGraph, "The connection to insert into no longer exists");
    if (const auto result = validateNodeForGraph(candidate, *graph, node, registry);
        !result.ok())
    {
        return result;
    }
    const auto* fromNode = findNode(*graph, wire.fromNode);
    const auto* toNode = findNode(*graph, wire.toNode);
    if (fromNode == nullptr || toNode == nullptr)
        return failure(EditError::UnknownNodeId, "The connection names a missing node");
    const auto fromDescriptor = describeProjectNode(candidate, *fromNode, registry);
    const auto toDescriptor = describeProjectNode(candidate, *toNode, registry);
    const auto nodeDescriptor = describeProjectNode(candidate, node, registry);
    if (!fromDescriptor.has_value() || !toDescriptor.has_value()
        || !nodeDescriptor.has_value())
    {
        return failure(EditError::UnknownNodeType, "Connection references a node with an unknown type");
    }
    const auto* source = findPort(fromDescriptor->outputs, wire.fromPort);
    const auto* destination = findPort(toDescriptor->inputs, wire.toPort);
    if (source == nullptr || destination == nullptr)
        return failure(EditError::UnknownPort, "The connection names a missing port");
    const auto* input = firstCompatibleInput(*nodeDescriptor, *source, [&](const talas::core::PortDescriptor& port) {
        return !inputPortId.has_value() || port.id == *inputPortId;
    });
    if (input == nullptr)
    {
        return failure(
            EditError::IncompatiblePortTypes,
            inputPortId.has_value()
                ? nodeDescriptor->displayName + " has no input " + std::string{*inputPortId} + " for this connection's signal"
                : nodeDescriptor->displayName + " has no input for this connection's signal");
    }
    const auto* output = firstCompatibleOutput(*nodeDescriptor, *destination);
    if (output == nullptr)
    {
        return failure(
            EditError::IncompatiblePortTypes,
            nodeDescriptor->displayName + " has no output for this connection's destination");
    }
    const auto nodeId = node.instanceId;
    const auto inputId = input->id;
    const auto outputId = output->id;
    const auto index = existing - graph->connections.begin();
    graph->connections.erase(existing);
    graph->connections.insert(
        graph->connections.begin() + index,
        {nodeId, outputId, wire.toNode, wire.toPort});
    graph->connections.insert(
        graph->connections.begin() + index,
        {wire.fromNode, wire.fromPort, nodeId, inputId});
    graph->nodes.push_back(std::move(node));
    reconcileProjectPresentation(candidate);
    if (auto* presented = findProjectNodePresentation(candidate, definitionId, nodeId))
        presented->position = position;
    if (containsCycle(candidate, registry))
        return failure(EditError::InvalidGraph, "Insertion would create a direct graph cycle");
    return success();
}

[[nodiscard]] EditResult replaceGraphNode(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    std::string_view replacedNodeId,
    ProjectNode replacement,
    const talas::core::NodeRegistry& registry)
{
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    const auto* replaced = findNode(*graph, replacedNodeId);
    if (replaced == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node to replace: " + std::string{replacedNodeId});
    if (const auto result = validateNodeForGraph(candidate, *graph, replacement, registry);
        !result.ok())
    {
        return result;
    }
    const auto replacedDescriptor = describeProjectNodeForEditing(candidate, *replaced, registry);
    const auto newDescriptor = describeProjectNode(candidate, replacement, registry);
    if (!replacedDescriptor.has_value() || !newDescriptor.has_value())
        return failure(EditError::UnknownNodeType, "Replacement references a node with an unknown type");
    const auto* presented = findProjectNodePresentation(candidate, definitionId, replacedNodeId);
    const auto position = presented == nullptr ? CanvasPosition{} : presented->position;
    const auto oldId = std::string{replacedNodeId};
    const auto newId = replacement.instanceId;

    // Each output keeps feeding its destinations from the replacement's
    // output of the same name, or else its first output that fits.
    const auto mappedOutput = [&](const talas::core::Connection& connection)
        -> std::optional<std::string> {
        const auto* toNode = findNode(*graph, connection.toNode);
        if (toNode == nullptr)
            return std::nullopt;
        const auto toDescriptor = describeProjectNodeForEditing(candidate, *toNode, registry);
        if (!toDescriptor.has_value())
            return std::nullopt;
        const auto* destination = findPort(toDescriptor->inputs, connection.toPort);
        if (destination == nullptr)
            return std::nullopt;
        if (const auto* same = findPort(newDescriptor->outputs, connection.fromPort);
            same != nullptr && talas::core::portDescriptorsCompatible(*same, *destination))
        {
            return same->id;
        }
        const auto* first = firstCompatibleOutput(*newDescriptor, *destination);
        return first == nullptr ? std::nullopt : std::optional<std::string>{first->id};
    };
    std::unordered_set<std::string> claimedInputs;
    const auto mappedInput = [&](const talas::core::Connection& connection)
        -> std::optional<std::string> {
        const auto* fromNode = findNode(*graph, connection.fromNode);
        if (fromNode == nullptr)
            return std::nullopt;
        const auto fromDescriptor = describeProjectNodeForEditing(candidate, *fromNode, registry);
        if (!fromDescriptor.has_value())
            return std::nullopt;
        const auto* source = findPort(fromDescriptor->outputs, connection.fromPort);
        if (source == nullptr)
            return std::nullopt;
        const auto available = [&](const talas::core::PortDescriptor& input) {
            return !input.compilerPopulated
                && talas::core::portDescriptorsCompatible(*source, input)
                && (input.acceptsMany || !claimedInputs.contains(input.id));
        };
        if (const auto* same = findPort(newDescriptor->inputs, connection.toPort);
            same != nullptr && available(*same))
        {
            claimedInputs.insert(same->id);
            return same->id;
        }
        const auto* first = firstCompatibleInput(*newDescriptor, *source, available);
        if (first == nullptr)
            return std::nullopt;
        claimedInputs.insert(first->id);
        return first->id;
    };

    // Inputs named alike claim their ports before the rest take what is free.
    std::vector<talas::core::Connection> rewired;
    rewired.reserve(graph->connections.size());
    std::vector<std::size_t> deferredInputs;
    for (std::size_t index = 0; index < graph->connections.size(); ++index)
    {
        const auto& connection = graph->connections[index];
        if (connection.toNode == oldId
            && findPort(newDescriptor->inputs, connection.toPort) == nullptr)
        {
            deferredInputs.push_back(index);
        }
    }
    std::unordered_map<std::size_t, std::optional<std::string>> inputPorts;
    for (std::size_t index = 0; index < graph->connections.size(); ++index)
    {
        const auto& connection = graph->connections[index];
        if (connection.toNode == oldId
            && std::ranges::find(deferredInputs, index) == deferredInputs.end())
        {
            inputPorts.emplace(index, mappedInput(connection));
        }
    }
    for (const auto index : deferredInputs)
        inputPorts.emplace(index, mappedInput(graph->connections[index]));
    for (std::size_t index = 0; index < graph->connections.size(); ++index)
    {
        auto connection = graph->connections[index];
        if (connection.fromNode == oldId && connection.toNode == oldId)
            continue;
        if (connection.fromNode == oldId)
        {
            const auto port = mappedOutput(connection);
            if (!port.has_value())
                continue;
            connection.fromNode = newId;
            connection.fromPort = *port;
        }
        else if (connection.toNode == oldId)
        {
            const auto& port = inputPorts.at(index);
            if (!port.has_value())
                continue;
            connection.toNode = newId;
            connection.toPort = *port;
        }
        rewired.push_back(std::move(connection));
    }
    graph->connections = std::move(rewired);

    if (definitionId.has_value())
    {
        auto* definition = findSoundDefinition(candidate, *definitionId);
        for (auto& port : definition->outputs)
        {
            if (port.nodeId != oldId)
                continue;
            const auto* same = findPort(newDescriptor->outputs, port.nodePortId);
            const auto* first = newDescriptor->outputs.empty()
                ? nullptr
                : &newDescriptor->outputs.front();
            const auto* chosen = same != nullptr && same->type == port.type
                ? same
                : (first != nullptr && first->type == port.type ? first : nullptr);
            if (chosen == nullptr)
            {
                return failure(
                    EditError::InvalidGraph,
                    newDescriptor->displayName + " has no output for the Sound's output " + port.portId);
            }
            port.nodeId = newId;
            port.nodePortId = chosen->id;
        }
        for (auto& port : definition->inputs)
        {
            if (port.nodeId != oldId)
                continue;
            if (port.replacesSound)
            {
                // The replaceable input stays with the Sound in its place.
                const auto output = std::ranges::find(newDescriptor->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
                if (output == newDescriptor->outputs.end() || replacement.soundDefinitionId.has_value())
                {
                    port.nodeId.clear();
                    continue;
                }
                port.nodeId = newId;
                if (findPort(newDescriptor->outputs, port.nodePortId) == nullptr)
                    port.nodePortId = output->id;
                continue;
            }
            const auto* same = findPort(newDescriptor->inputs, port.nodePortId);
            if (same == nullptr || same->type != port.type)
            {
                return failure(
                    EditError::InvalidGraph,
                    newDescriptor->displayName + " has no input for the Sound's input " + port.portId);
            }
            port.nodeId = newId;
        }
        // A Sound that cannot be the replaceable input leaves the Sound
        // without one.
        std::erase_if(definition->inputs, [](const ProjectSoundPort& port) {
            return port.replacesSound && port.nodeId.empty();
        });
        for (auto& exposed : definition->exposedParameters)
        {
            for (auto& binding : exposed.bindings)
            {
                if (binding.target == ProjectSoundParameterTarget::NodeParameter
                    && binding.nodeId == oldId)
                {
                    binding.nodeId = newId;
                }
            }
        }
    }

    const auto replacedIndex = std::ranges::find(
        graph->nodes, oldId, &ProjectNode::instanceId) - graph->nodes.begin();
    graph->nodes[static_cast<std::size_t>(replacedIndex)] = std::move(replacement);
    reconcileProjectPresentation(candidate);
    if (auto* newPresentation = findProjectNodePresentation(candidate, definitionId, newId))
        newPresentation->position = position;
    if (containsCycle(candidate, registry))
        return failure(EditError::InvalidGraph, "Replacement would create a direct graph cycle");
    return success();
}

[[nodiscard]] EditResult pasteGraphFragmentInto(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    const ProjectGraphFragment& fragment,
    CanvasPosition origin,
    std::vector<std::string>& pastedNodeIds,
    const talas::core::NodeRegistry& registry)
{
    pastedNodeIds.clear();
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    if (fragment.nodes.empty())
        return failure(EditError::InvalidSelection, "There are no Sounds to paste");
    if (fragment.positions.size() != fragment.nodes.size())
        return failure(EditError::InvalidSelection, "Copied Sounds are missing their positions");
    auto minimumX = fragment.positions.front().x;
    auto minimumY = fragment.positions.front().y;
    for (const auto& position : fragment.positions)
    {
        minimumX = std::min(minimumX, position.x);
        minimumY = std::min(minimumY, position.y);
    }

    std::unordered_set<std::string> used;
    used.reserve(graph->nodes.size() + fragment.nodes.size());
    for (const auto& node : graph->nodes)
        used.insert(node.instanceId);
    std::unordered_map<std::string, std::string> pastedIds;
    std::vector<ProjectNode> nodes;
    std::vector<ProjectNodePresentation> presentation;
    for (std::size_t index = 0; index < fragment.nodes.size(); ++index)
    {
        auto node = fragment.nodes[index];
        if (node.preservedComponent.has_value())
            return failure(EditError::InvalidGraph, "A preserved component cannot be pasted: " + node.instanceId);
        const auto sourceId = node.instanceId;
        if (pastedIds.contains(sourceId))
            return failure(EditError::DuplicateNodeId, "Copied Sounds repeat the id " + sourceId);
        std::optional<std::string> id;
        if (!sourceId.empty() && used.insert(sourceId).second)
            id = sourceId;
        else
            id = nextDuplicateNodeId(sourceId, used);
        if (!id.has_value())
            return failure(EditError::InvalidSelection, "Unable to allocate a collision-free node id");
        node.instanceId = *id;
        if (const auto result = validateNodeForGraph(candidate, *graph, node, registry);
            !result.ok())
        {
            return result;
        }
        const auto translated = translatedPosition(
            origin,
            static_cast<std::int64_t>(fragment.positions[index].x) - minimumX,
            static_cast<std::int64_t>(fragment.positions[index].y) - minimumY);
        if (!translated.has_value())
            return failure(EditError::InvalidSelection, "Pasted node position exceeds the supported canvas range");
        pastedIds.emplace(sourceId, *id);
        presentation.push_back({*id, *translated});
        pastedNodeIds.push_back(*id);
        nodes.push_back(std::move(node));
    }
    std::vector<talas::core::Connection> connections;
    for (const auto& connection : fragment.connections)
    {
        const auto from = pastedIds.find(connection.fromNode);
        const auto to = pastedIds.find(connection.toNode);
        if (from == pastedIds.end() || to == pastedIds.end())
            return failure(EditError::UnknownNodeId, "A copied connection names no copied Sound");
        connections.push_back({from->second, connection.fromPort, to->second, connection.toPort});
    }
    graph->nodes.insert(
        graph->nodes.end(),
        std::make_move_iterator(nodes.begin()),
        std::make_move_iterator(nodes.end()));
    graph->connections.insert(
        graph->connections.end(),
        std::make_move_iterator(connections.begin()),
        std::make_move_iterator(connections.end()));
    reconcileProjectPresentation(candidate);
    for (const auto& placed : presentation)
    {
        if (auto* presented = findProjectNodePresentation(candidate, definitionId, placed.nodeId))
            presented->position = placed.position;
    }
    if (containsCycle(candidate, registry))
        return failure(EditError::InvalidGraph, "Pasted connections would create a direct graph cycle");
    return success();
}

[[nodiscard]] EditResult moveGraphSelection(
    ProjectDocument& candidate,
    std::optional<std::string_view> definitionId,
    std::span<const std::string> nodeIds,
    CanvasPosition delta)
{
    reconcileProjectPresentation(candidate);
    const auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    std::unordered_set<std::string> selected;
    if (const auto result = validateGraphNodeSelection(
            *graph, nodeIds, selected);
        !result.ok())
    {
        return result;
    }
    std::vector<std::pair<ProjectNodePresentation*, CanvasPosition>> moved;
    moved.reserve(selected.size());
    for (const auto& node : graph->nodes)
    {
        if (!selected.contains(node.instanceId))
            continue;
        auto* presented = findProjectNodePresentation(
            candidate, definitionId, node.instanceId);
        if (presented == nullptr)
        {
            return failure(
                EditError::InvalidSelection,
                "Selected node is missing portable presentation state: "
                    + node.instanceId);
        }
        const auto translated = translatedPosition(
            presented->position, delta.x, delta.y);
        if (!translated.has_value())
        {
            return failure(
                EditError::InvalidSelection,
                "Moved node position exceeds the supported canvas range");
        }
        moved.emplace_back(presented, *translated);
    }
    for (auto& [presented, position] : moved)
        presented->position = position;
    return success();
}

[[nodiscard]] EditResult validateSnapshotCandidate(
    const ProjectDocument& document)
{
    std::string serialized;
    try
    {
        serialized = serializeProjectDocument(document);
    }
    catch (const std::invalid_argument& error)
    {
        return failure(EditError::InvalidSnapshot, error.what());
    }
    catch (const std::length_error& error)
    {
        return failure(EditError::InvalidSnapshot, error.what());
    }
    const auto parsed = parseProjectDocument(serialized);
    if (parsed.ok()
        && parsed.document.snapshotLibrary == document.snapshotLibrary)
    {
        return success();
    }
    if (!parsed.errors.empty())
    {
        return failure(
            EditError::InvalidSnapshot,
            parsed.errors.front().path + ": "
                + parsed.errors.front().message);
    }
    return failure(
        EditError::InvalidSnapshot,
        "Snapshot library is not canonically representable");
}

[[nodiscard]] EditResult validateSampleMapCandidate(
    const ProjectDocument& document)
{
    const auto diagnostics = projectSampleMapDiagnostics(document);
    if (diagnostics.empty())
        return success();

    return failure(
        EditError::InvalidSampleMap,
        diagnostics.front().path + ": "
            + diagnostics.front().message);
}

[[nodiscard]] bool sameInspectedAssetShape(
    const ProjectAsset& left,
    const ProjectAsset& right)
{
    return left.kind == right.kind
        && left.channelCount == right.channelCount
        && left.sampleRate == right.sampleRate
        && left.frameCount == right.frameCount
        && left.encoding == right.encoding;
}

[[nodiscard]] EditResult normalizeSampleMapImportBatch(
    ProjectDocument& candidate,
    ProjectSampleMapImportBatch& batch)
{
    std::unordered_set<std::string> expectedAssetIds;
    for (const auto& expectedAsset : batch.expectedAssets)
    {
        if (!expectedAssetIds.insert(expectedAsset.id).second)
        {
            return failure(
                EditError::DuplicateAsset,
                "Duplicate expected sample-map import asset identity: "
                    + expectedAsset.id);
        }
        if (!std::ranges::any_of(
                batch.assets,
                [&](const auto& asset) {
                    return asset.id == expectedAsset.id;
                }))
        {
            return failure(
                EditError::InvalidAsset,
                "Expected sample-map import asset has no mutation candidate: "
                    + expectedAsset.id);
        }
    }
    std::unordered_set<std::string> candidateAssetIds;
    for (const auto& asset : batch.assets)
    {
        if (!candidateAssetIds.insert(asset.id).second)
        {
            return failure(
                EditError::DuplicateAsset,
                "Duplicate sample-map import asset identity: "
                    + asset.id);
        }
        const auto assetValidation = validateAsset(asset);
        if (!assetValidation.ok())
            return assetValidation;
        if (asset.contentHash.empty())
        {
            return failure(
                EditError::InvalidAsset,
                "Sample-map import assets must be fully inspected");
        }
        if (!std::ranges::any_of(
                batch.regions,
                [&](const auto& region) {
                    return region.assetId == asset.id;
                }))
        {
            return failure(
                EditError::InvalidAsset,
                "Sample-map import asset is not referenced by a candidate region: "
                    + asset.id);
        }

        const auto sameIdentity = std::ranges::find(
            candidate.assets,
            asset.id,
            &ProjectAsset::id);
        const auto expectedExisting = std::ranges::find(
            batch.expectedAssets,
            asset.id,
            &ProjectAsset::id);
        if (sameIdentity != candidate.assets.end())
        {
            if (expectedExisting != batch.expectedAssets.end()
                && *sameIdentity != *expectedExisting)
            {
                return failure(
                    EditError::StaleAssetDisposition,
                    "Project asset changed after sample-map import preparation began: "
                        + asset.id);
            }
            if (sameIdentity->contentHash.empty())
            {
                if (expectedExisting == batch.expectedAssets.end())
                {
                    return failure(
                        EditError::StaleAssetDisposition,
                        "Completing an existing sample-map asset requires its expected revision: "
                            + asset.id);
                }
                auto expected = *sameIdentity;
                expected.path = asset.path;
                expected.disposition = asset.disposition;
                expected.contentHash = asset.contentHash;
                if (expected.kind == ProjectAssetKind::Audio)
                {
                    if (expected.channelCount == 0U)
                        expected.channelCount = asset.channelCount;
                    if (expected.sampleRate == 0.0)
                        expected.sampleRate = asset.sampleRate;
                    if (expected.frameCount == 0U)
                        expected.frameCount = asset.frameCount;
                    if (expected.encoding.empty())
                        expected.encoding = asset.encoding;
                }
                if (asset != expected)
                {
                    return failure(
                        EditError::DuplicateAsset,
                        "Sample-map import cannot rewrite an unverified existing identity: "
                            + asset.id);
                }
                *sameIdentity = asset;
                continue;
            }
            if (sameIdentity->contentHash != asset.contentHash
                || !sameInspectedAssetShape(*sameIdentity, asset))
            {
                return failure(
                    EditError::DuplicateAsset,
                    "Sample-map import asset conflicts with existing identity: "
                        + asset.id);
            }
            auto expected = *sameIdentity;
            expected.path = asset.path;
            expected.disposition = asset.disposition;
            if (asset != expected)
            {
                return failure(
                    EditError::DuplicateAsset,
                    "Sample-map import may change only path and disposition for an exact existing identity: "
                        + asset.id);
            }
            if (asset != *sameIdentity
                && expectedExisting == batch.expectedAssets.end())
            {
                return failure(
                    EditError::StaleAssetDisposition,
                    "Changing an existing sample-map asset requires its expected revision: "
                        + asset.id);
            }
            *sameIdentity = asset;
            continue;
        }

        if (expectedExisting != batch.expectedAssets.end())
        {
            return failure(
                EditError::StaleAssetDisposition,
                "Expected sample-map import asset no longer exists: "
                    + asset.id);
        }

        const auto sameBytes = std::ranges::find(
            candidate.assets,
            asset.contentHash,
            &ProjectAsset::contentHash);
        if (sameBytes != candidate.assets.end())
        {
            if (!sameInspectedAssetShape(*sameBytes, asset))
            {
                return failure(
                    EditError::InvalidAsset,
                    "Exact-hash sample-map import has inconsistent media shape: "
                        + asset.id);
            }
            for (auto& region : batch.regions)
            {
                if (region.assetId == asset.id)
                    region.assetId = sameBytes->id;
            }
            continue;
        }

        candidate.assets.push_back(asset);
    }
    return validateAssets(candidate.assets);
}

[[nodiscard]] EditResult validateImportRegionIdentities(
    const std::vector<ProjectSampleRegion>& regions)
{
    std::unordered_set<std::string> regionIds;
    for (const auto& region : regions)
    {
        if (!regionIds.insert(region.regionId).second)
        {
            return failure(
                EditError::DuplicateSampleRegion,
                "Duplicate sample-map import region identity: "
                    + region.regionId);
        }
    }
    return success();
}

[[nodiscard]] ProjectSnapshotBank* findSnapshotBank(
    ProjectDocument& document,
    std::string_view bankId)
{
    const auto found = std::ranges::find_if(
        document.snapshotLibrary.banks,
        [&](const auto& bank) {
            return bank.bankId == bankId;
        });
    return found == document.snapshotLibrary.banks.end()
        ? nullptr
        : &*found;
}

[[nodiscard]] ProjectSnapshot* findSnapshot(
    ProjectSnapshotBank& bank,
    std::string_view snapshotId)
{
    const auto found = std::ranges::find_if(
        bank.snapshots,
        [&](const auto& snapshot) {
            return snapshot.snapshotId == snapshotId;
        });
    return found == bank.snapshots.end() ? nullptr : &*found;
}

[[nodiscard]] bool sameVariableIdentity(
    const ProjectVariableDeclaration& declaration,
    const ProjectVariableOwner& owner,
    std::string_view variableId) noexcept
{
    return declaration.variableId == variableId
        && declaration.owner == owner;
}

[[nodiscard]] EditResult variableValidationFailure(
    const ProjectDocument& document)
{
    const auto validation = validateProjectVariableLibrary(document);
    if (validation.ok())
        return success();
    if (!validation.diagnostics.empty())
    {
        const auto& diagnostic = validation.diagnostics.front();
        const auto error = diagnostic.code
                == ProjectVariableValidationCode::DuplicateDeclaration
            ? EditError::DuplicateProjectVariable
            : EditError::InvalidProjectVariable;
        return failure(
            error,
            diagnostic.path + ": " + diagnostic.message);
    }
    return failure(
        EditError::InvalidProjectVariable,
        "Variable validation exceeded its bounded diagnostic capacity");
}

[[nodiscard]] bool sameSoundVariableIdentity(
    const ProjectSoundVariableDeclaration& declaration,
    const ProjectVariableOwner& owner,
    std::string_view variableId) noexcept
{
    return declaration.variableId == variableId
        && declaration.owner == owner;
}

[[nodiscard]] EditResult soundVariableValidationFailure(
    const ProjectDocument& document)
{
    const auto validation = validateProjectSoundVariableLibrary(document);
    if (validation.ok())
        return success();
    if (!validation.diagnostics.empty())
    {
        const auto& diagnostic = validation.diagnostics.front();
        const auto error = diagnostic.code
                == ProjectSoundVariableValidationCode::DuplicateDeclaration
            ? EditError::DuplicateProjectVariable
            : EditError::InvalidProjectVariable;
        return failure(
            error,
            diagnostic.path + ": " + diagnostic.message);
    }
    return failure(
        EditError::InvalidProjectVariable,
        "Sound-variable validation exceeded its bounded diagnostic capacity");
}

[[nodiscard]] EditResult generatorValidationFailure(
    const ProjectDocument& document)
{
    const auto validation = validateProjectGeneratorLibrary(
        document.generatorLibrary);
    if (validation.ok())
        return success();
    if (!validation.diagnostics.empty())
    {
        const auto& diagnostic = validation.diagnostics.front();
        return failure(
            diagnostic.code
                    == ProjectGeneratorValidationCode::DuplicateProgram
                ? EditError::DuplicateProjectGenerator
                : EditError::InvalidProjectGenerator,
            diagnostic.path + " bytes "
                + std::to_string(diagnostic.sourceRange.begin) + "-"
                + std::to_string(diagnostic.sourceRange.end) + ": "
                + diagnostic.message);
    }
    return failure(
        EditError::InvalidProjectGenerator,
        "Generator validation exceeded its bounded diagnostic capacity");
}

[[nodiscard]] EditResult generatorTargetValidationFailure(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    const auto validation =
        validateProjectGeneratorOutputTargets(document, registry);
    if (validation.ok())
        return success();
    if (!validation.diagnostics.empty())
    {
        const auto& diagnostic = validation.diagnostics.front();
        return failure(
            EditError::InvalidProjectGenerator,
            diagnostic.path + ": " + diagnostic.message);
    }
    return failure(
        EditError::InvalidProjectGenerator,
        "Generator target validation exceeded its bounded diagnostic capacity");
}

[[nodiscard]] EditResult liveSetValidationFailure(
    const ProjectDocument& document)
{
    try
    {
        const auto validation = validateProjectLiveSetLibrary(document);
        if (validation.ok())
            return success();
        if (!validation.diagnostics.empty())
        {
            const auto& diagnostic = validation.diagnostics.front();
            return failure(
                EditError::InvalidLiveSet,
                diagnostic.path + ": " + diagnostic.message);
        }
        return failure(
            EditError::InvalidLiveSet,
            "Live-set validation exceeded its bounded diagnostic capacity");
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate live-set validation state");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Live-set validation state exceeded supported capacity");
    }
}
}

std::optional<ProjectSoundDefinition> classFromSoundFileExample(const ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view exampleId, std::string classId, std::string className)
{
    const auto* example = findSoundDefinition(document, exampleId);
    if (example == nullptr || classId.empty() || className.empty() || findSoundDefinition(document, classId) != nullptr)
        return std::nullopt;
    auto made = *example;
    made.definitionId = std::move(classId);
    made.displayName = std::move(className);
    exposeExampleVariables(made, document, registry, &example->graph);
    return made;
}

std::optional<ProjectSoundDefinition> soundFileClassInstance(const ProjectDocument& document, std::string_view classId)
{
    const auto* made = findSoundDefinition(document, classId);
    if (made == nullptr)
        return std::nullopt;
    ProjectSoundDefinition instance;
    const auto base = std::string{classId} + "Instance";
    instance.definitionId = base;
    for (std::size_t number = 2; findSoundDefinition(document, instance.definitionId) != nullptr; ++number)
        instance.definitionId = base + std::to_string(number);
    instance.displayName = made->displayName.empty() ? made->definitionId : made->displayName;
    // The Sound made from the class, named as it is, in letters and digits.
    std::string nodeId;
    for (const auto character : instance.displayName)
    {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0)
            nodeId.push_back(nodeId.empty() ? static_cast<char>(std::tolower(static_cast<unsigned char>(character))) : character);
    }
    if (nodeId.empty() || std::isdigit(static_cast<unsigned char>(nodeId.front())) != 0)
        nodeId = "sound" + nodeId;
    ProjectNode node;
    node.instanceId = nodeId;
    node.soundDefinitionId = made->definitionId;
    instance.graph.nodes.push_back(std::move(node));
    for (const auto& port : made->inputs)
        instance.inputs.push_back({port.portId, port.type, nodeId, port.portId});
    for (const auto& port : made->outputs)
        instance.outputs.push_back({port.portId, port.type, nodeId, port.portId});
    return instance;
}

ProjectEditor::ProjectEditor(ProjectDocument document, const talas::core::NodeRegistry& registry)
    : document_(std::move(document))
    , registry_(registry)
{
    undoWindows_.reserve(historyLimit + 1U);
    redoWindows_.reserve(historyLimit + 1U);
    if (const auto preservation =
            freezeAuthoredExternalComponents(document_, registry_);
        preservation.has_value())
    {
        throw std::invalid_argument(preservation->message);
    }
    reconcileProjectPresentation(document_);
}

const ProjectDocument& ProjectEditor::document() const
{
    return document_;
}

std::uint64_t ProjectEditor::documentAuthorityGeneration() const noexcept
{
    return documentAuthorityGeneration_.load(std::memory_order_acquire);
}

void ProjectEditor::advanceDocumentAuthority() noexcept
{
    const auto generation = documentAuthorityGeneration_.load(
        std::memory_order_relaxed);
    if (generation != UINT64_MAX)
    {
        documentAuthorityGeneration_.store(
            generation + 1U, std::memory_order_release);
    }
}

EditResult ProjectEditor::checkReplacementDocument(
    ProjectDocument document) const
{
    ProjectEventValueSyncResult sync;
    return prepareReplacementDocument(document, sync);
}

EditResult ProjectEditor::prepareReplacementDocument(
    ProjectDocument& document,
    ProjectEventValueSyncResult& sync) const
{
    if (const auto preservation =
            freezeAuthoredExternalComponents(document, registry_);
        preservation.has_value())
    {
        return *preservation;
    }
    sync = synchronizeProjectEventValueControls(document, registry_);
    reconcileProjectPresentation(document);
    const auto virtualControlResult = validateVirtualControls(document.virtualControls);
    if (!virtualControlResult.ok())
        return virtualControlResult;
    if (const auto dormant = validateDormantVirtualControls(document); !dormant.ok())
        return dormant;
    const auto assetResult = validateAssets(document.assets);
    if (!assetResult.ok())
        return assetResult;
    const auto timelineResult = validateTimeline(document.timeline);
    if (!timelineResult.ok())
        return timelineResult;
    const auto scoreResult = validateScore(document);
    if (!scoreResult.ok())
        return scoreResult;
    const auto deviceSettingsResult =
        validateDeviceSettings(document.deviceSettings);
    if (!deviceSettingsResult.ok())
        return deviceSettingsResult;
    const auto sampleMapResult = validateSampleMapCandidate(document);
    if (!sampleMapResult.ok())
        return sampleMapResult;
    const auto snapshotResult = validateSnapshotCandidate(document);
    if (!snapshotResult.ok())
        return snapshotResult;
    const auto liveSetResult = liveSetValidationFailure(document);
    if (!liveSetResult.ok())
        return liveSetResult;

    return projectValidationFailure(document, registry_);
}

EditResult ProjectEditor::replaceDocument(ProjectDocument document)
{
    ProjectEventValueSyncResult sync;
    if (const auto prepared = prepareReplacementDocument(document, sync);
        !prepared.ok())
    {
        return prepared;
    }

    advanceDocumentAuthority();
    document_ = std::move(document);
    undoHistory_.clear();
    redoHistory_.clear();
    undoWindows_.clear();
    redoWindows_.clear();
    ++documentReplacements_;
    transactionBefore_.reset();
    transactionChanged_ = false;
    transactionOwner_ = 0;
    valueTransaction_ = false;
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::applyDocumentEdit(
    ProjectDocument document,
    HistoryTransactionToken transactionToken)
{
    return applyDocumentEditInternal(
        std::move(document), transactionToken, true);
}

EditResult ProjectEditor::prepareDiskCacheCapture(
    DiskCacheCapturePublication publication,
    PreparedDiskCacheCaptureEdit& prepared)
{
    prepared.expectedDocumentAuthorityGeneration_ = 0;
    prepared.changesDocument_ = false;
    const auto currentGeneration = documentAuthorityGeneration();
    if (publication.expectedDocumentAuthorityGeneration == 0
        || publication.expectedDocumentAuthorityGeneration == UINT64_MAX
        || publication.expectedDocumentAuthorityGeneration
            != currentGeneration)
    {
        return failure(
            EditError::StaleDiskCacheCapture,
            "DiskCache capture document authority is stale");
    }
    if (transactionBefore_.has_value() || transactionOwner_ != 0)
    {
        return failure(
            EditError::StaleDiskCacheCapture,
            "DiskCache capture cannot join an active project history transaction");
    }
    try
    {
        if (publication.expectedCanonicalDocumentBytes
            != serializeProjectDocument(document_))
        {
            return failure(
                EditError::StaleDiskCacheCapture,
                "DiskCache capture project bytes are stale");
        }
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not verify DiskCache capture project bytes");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "DiskCache capture project bytes exceed supported capacity");
    }
    catch (const std::exception& exception)
    {
        return failure(
            EditError::InvalidDiskCacheCapture,
            "Could not serialize DiskCache capture authority: "
                + std::string{exception.what()});
    }

    if (publication.sourcePath.empty()
        || publication.sourcePath.size() > 4096
        || diskCacheAssetAuthority(publication.asset)
            != DiskCacheAssetAuthority::CapturedPlayback
        || !isValidDiskCacheAssetPath(publication.asset.path))
    {
        return failure(
            EditError::InvalidDiskCacheCapture,
            "DiskCache capture publication metadata is invalid");
    }
    if (const auto assetValidation = validateAsset(publication.asset);
        !assetValidation.ok())
    {
        return failure(
            EditError::InvalidDiskCacheCapture,
            assetValidation.message);
    }

    const auto expectedFingerprint =
        diskCacheCaptureFingerprint(publication.asset.provenance);
    std::string fingerprintError;
    const auto actualFingerprint = projectDiskCacheSourceFingerprint(
        document_,
        registry_,
        publication.sourcePath,
        static_cast<std::uint32_t>(publication.asset.sampleRate),
        fingerprintError);
    if (!expectedFingerprint.has_value()
        || actualFingerprint != *expectedFingerprint)
    {
        return failure(
            EditError::StaleDiskCacheCapture,
            fingerprintError.empty()
                ? "DiskCache capture source fingerprint is stale"
                : std::move(fingerprintError));
    }

    try
    {
        auto candidate = document_;
        std::string targetError;
        auto target = findAuthoredDiskCacheTarget(
            candidate,
            registry_,
            publication.sourcePath,
            targetError);
        if (target.node == nullptr)
        {
            return failure(
                EditError::StaleDiskCacheCapture,
                std::move(targetError));
        }
        if (!target.effectiveRecord)
        {
            return failure(
                EditError::StaleDiskCacheCapture,
                "DiskCache target is no longer armed for capture");
        }
        if (target.effectiveFileName.has_value()
            && !target.effectiveFileName->empty()
            && *target.effectiveFileName != publication.asset.id)
        {
            return failure(
                EditError::StaleDiskCacheCapture,
                "DiskCache destination asset identity changed during capture");
        }

        const auto existingAsset = std::ranges::find(
            candidate.assets,
            publication.asset.id,
            &ProjectAsset::id);
        const auto appendsAsset = existingAsset == candidate.assets.end();
        const auto generation = nextDiskCacheCaptureAssetGeneration(
            candidate, publication.asset.id);
        if (!generation.ok()
            || publication.asset.generation != *generation.generation)
        {
            return failure(
                EditError::InvalidDiskCacheCapture,
                "DiskCache capture cannot replace unrelated, exhausted, or non-advancing asset authority");
        }
        if (!appendsAsset
            && publication.asset.path == existingAsset->path)
        {
            return failure(
                EditError::InvalidDiskCacheCapture,
                "DiskCache recapture requires a unique generation path");
        }
        if (std::ranges::any_of(
                candidate.assets,
                [&](const auto& asset) {
                    return asset.id != publication.asset.id
                        && asset.path == publication.asset.path;
                }))
        {
            return failure(
                EditError::InvalidDiskCacheCapture,
                "DiskCache capture destination path belongs to another asset");
        }

        const auto capturedAssetId = publication.asset.id;
        if (appendsAsset)
            candidate.assets.push_back(std::move(publication.asset));
        else
            *existingAsset = std::move(publication.asset);

        ProjectParameterExpression capturedFileName;
        capturedFileName.parameterId = target.fileNameParameterId;
        capturedFileName.resource = capturedAssetId;
        ProjectParameterExpression playbackMode;
        playbackMode.parameterId = target.recordParameterId;
        playbackMode.boolean = false;
        setDiskCacheParameter(*target.node, std::move(capturedFileName));
        setDiskCacheParameter(*target.node, std::move(playbackMode));

        reconcileProjectPresentation(candidate);
        if (const auto assetsResult = validateAssets(candidate.assets);
            !assetsResult.ok())
        {
            return failure(
                EditError::InvalidDiskCacheCapture,
                assetsResult.message);
        }
        if (const auto projectResult =
                projectValidationFailure(candidate, registry_);
            !projectResult.ok())
        {
            return failure(
                EditError::InvalidDiskCacheCapture,
                projectResult.message);
        }

        if (undoHistory_.size() < historyLimit
            && undoHistory_.capacity() < undoHistory_.size() + 1U)
        {
            undoHistory_.reserve(undoHistory_.size() + 1U);
        }
        prepared.document_ = std::move(candidate);
        prepared.expectedDocumentAuthorityGeneration_ = currentGeneration;
        prepared.changesDocument_ = true;
        return success("Prepared exact DiskCache capture publication");
    }
    catch (const std::bad_alloc&)
    {
        prepared.expectedDocumentAuthorityGeneration_ = 0;
        prepared.changesDocument_ = false;
        return failure(
            EditError::AllocationFailed,
            "Could not prepare DiskCache capture publication");
    }
    catch (const std::length_error&)
    {
        prepared.expectedDocumentAuthorityGeneration_ = 0;
        prepared.changesDocument_ = false;
        return failure(
            EditError::AllocationFailed,
            "DiskCache capture publication exceeds supported capacity");
    }
}

bool ProjectEditor::diskCacheCaptureStillCurrent(
    const PreparedDiskCacheCaptureEdit& prepared) const noexcept
{
    return prepared.changesDocument_
        && prepared.expectedDocumentAuthorityGeneration_ != 0
        && prepared.expectedDocumentAuthorityGeneration_
            == documentAuthorityGeneration()
        && !transactionBefore_.has_value()
        && transactionOwner_ == 0;
}

bool ProjectEditor::commitPreparedDiskCacheCapture(
    PreparedDiskCacheCaptureEdit prepared) noexcept
{
    static_assert(std::is_nothrow_move_constructible_v<ProjectDocument>);
    static_assert(std::is_nothrow_move_assignable_v<ProjectDocument>);
    if (!diskCacheCaptureStillCurrent(prepared))
        return false;
    if (undoHistory_.size() < historyLimit
        && undoHistory_.capacity() < undoHistory_.size() + 1U)
    {
        return false;
    }
    if (undoHistory_.size() == historyLimit)
        undoHistory_.erase(undoHistory_.begin());
    advanceDocumentAuthority();
    undoHistory_.push_back(std::move(document_));
    pushUndoWindow(historyWindow_);
    document_ = std::move(prepared.document_);
    clearRedoHistory();
    return true;
}

EditResult ProjectEditor::applyDocumentEditInternal(
    ProjectDocument document,
    HistoryTransactionToken transactionToken,
    bool synchronizeDerivedState,
    bool isolateHistory)
{
    if (const auto preservation =
            freezeAuthoredExternalComponents(document, registry_);
        preservation.has_value())
    {
        return *preservation;
    }
    ProjectEventValueSyncResult sync;
    if (synchronizeDerivedState)
    {
        sync = synchronizeProjectEventValueControls(document, registry_);
        reconcileProjectPresentation(document);
    }
    const auto virtualControlResult = validateVirtualControls(document.virtualControls);
    if (!virtualControlResult.ok())
        return virtualControlResult;
    if (const auto dormant = validateDormantVirtualControls(document); !dormant.ok())
        return dormant;
    const auto assetResult = validateAssets(document.assets);
    if (!assetResult.ok())
        return assetResult;
    const auto timelineResult = validateTimeline(document.timeline);
    if (!timelineResult.ok())
        return timelineResult;
    const auto scoreResult = validateScore(document);
    if (!scoreResult.ok())
        return scoreResult;
    const auto deviceSettingsResult =
        validateDeviceSettings(document.deviceSettings);
    if (!deviceSettingsResult.ok())
        return deviceSettingsResult;
    const auto sampleMapResult = validateSampleMapCandidate(document);
    if (!sampleMapResult.ok())
        return sampleMapResult;
    const auto snapshotResult = validateSnapshotCandidate(document);
    if (!snapshotResult.ok())
        return snapshotResult;
    const auto liveSetResult = liveSetValidationFailure(document);
    if (!liveSetResult.ok())
        return liveSetResult;
    const auto projectResult = projectValidationFailure(document, registry_);
    if (!projectResult.ok())
        return projectResult;

    static_assert(std::is_nothrow_move_constructible_v<ProjectDocument>);
    static_assert(std::is_nothrow_move_assignable_v<ProjectDocument>);
    const auto ownsActiveTransaction = transactionToken != 0
        && transactionOwner_ == transactionToken
        && !valueTransaction_
        && !isolateHistory;
    const auto historyInsertions =
        !transactionBefore_.has_value()
            ? 1U
            : isolateHistory
            ? (transactionChanged_ ? 2U : 1U)
            : ownsActiveTransaction
            ? 1U
            : (transactionOwner_ != 0 && !ownsActiveTransaction
                   ? (transactionChanged_ ? 2U : 1U)
                   : 0U);
    const auto requiredHistoryCapacity = std::min(
        historyLimit,
        undoHistory_.size() + historyInsertions);
    if (undoHistory_.capacity() < requiredHistoryCapacity)
    {
        try
        {
            undoHistory_.reserve(requiredHistoryCapacity);
        }
        catch (const std::bad_alloc&)
        {
            return failure(
                EditError::AllocationFailed,
                "Could not reserve project history for the prepared edit");
        }
        catch (const std::length_error&)
        {
            return failure(
                EditError::AllocationFailed,
                "Prepared edit history exceeds supported capacity");
        }
    }
    if (transactionBefore_.has_value()
        && (isolateHistory
            || (transactionOwner_ != 0 && !ownsActiveTransaction)))
        closeHistoryTransaction();
    advanceDocumentAuthority();
    if (transactionBefore_.has_value())
    {
        document_ = std::move(document);
        transactionChanged_ = true;
    }
    else
    {
        if (undoHistory_.size() == historyLimit)
            undoHistory_.erase(undoHistory_.begin());
        undoHistory_.push_back(std::move(document_));
        pushUndoWindow(historyWindow_);
        document_ = std::move(document);
        clearRedoHistory();
    }
    return synchronizeDerivedState ? eventValueSyncOutcome(sync) : success();
}

EditResult ProjectEditor::upgradeExpressionsToPureV3()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::PureV3)
    {
        return success("Project already uses Pure expression v3");
    }
    if (document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::LegacyV1
        && document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::PureV2)
    {
        return failure(
            EditError::InvalidParameterValue,
            "Project expression language is unknown");
    }

    ProjectExpressionAuditResult audit;
    try
    {
        audit = auditProjectExpressions(
            document_,
            registry_,
            talas::expr::ExpressionLanguageVersion::PureV3);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Pure expression v3 compatibility audit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Pure expression v3 compatibility audit exceeded supported capacity");
    }
    if (!audit.ok())
    {
        if (!audit.diagnostics.empty())
        {
            const auto& diagnostic = audit.diagnostics.front();
            return failure(
                EditError::InvalidParameterValue,
                diagnostic.path + " bytes "
                    + std::to_string(diagnostic.range.offset) + "-"
                    + std::to_string(
                        diagnostic.range.offset + diagnostic.range.length)
                    + ": " + diagnostic.message);
        }
        return failure(
            EditError::InvalidParameterValue,
            "Pure expression v3 compatibility audit exceeded the diagnostic limit");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::PureV3;
        candidate.formatVersion = 24;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Pure expression v3 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Pure expression v3 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToPureV4()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::PureV4)
    {
        return success("Project already uses Pure expression v4");
    }
    if (document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::LegacyV1
        && document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::PureV2
        && document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::PureV3)
    {
        return failure(
            EditError::InvalidParameterValue,
            "Project expression language is unknown");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::PureV4;
        candidate.formatVersion = 25;
        candidate.expressionDefinitions.definitions.clear();
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::PureV4);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "Pure expression v4 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Pure expression v4 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Pure expression v4 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToSessionV1()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::SessionV1)
    {
        return success("Project already uses bounded session expressions");
    }
    if (document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::LegacyV1
        && !talas::expr::isPureExpressionLanguage(
            document_.expressionLanguage))
    {
        return failure(
            EditError::InvalidParameterValue,
            "Project expression language is unknown");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::SessionV1;
        candidate.formatVersion = 26;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::SessionV1);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "Session expression compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the SessionV1 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "SessionV1 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToVariablesV1()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV1)
    {
        return success("Project already uses persistent variables v1");
    }
    if (document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::LegacyV1
        && !talas::expr::isPureExpressionLanguage(
            document_.expressionLanguage)
        && document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::SessionV1)
    {
        return failure(
            EditError::InvalidParameterValue,
            "Project expression language is unknown");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV1;
        candidate.formatVersion = 34;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::VariablesV1);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "VariablesV1 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the VariablesV1 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "VariablesV1 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToVariablesV2()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        return success("Project already uses persistent variables v2");
    }
    if (document_.expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV1)
    {
        return failure(
            EditError::InvalidParameterValue,
            "VariablesV2 upgrade requires VariablesV1 expression authority");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        candidate.formatVersion = currentProjectFormatVersion;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::VariablesV2);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "VariablesV2 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the VariablesV2 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "VariablesV2 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToVariablesV3()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV3)
    {
        return success("Project already uses timed variables v3");
    }
    if (document_.expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        return failure(
            EditError::InvalidParameterValue,
            "VariablesV3 upgrade requires VariablesV2 expression authority");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV3;
        candidate.formatVersion = currentProjectFormatVersion;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::VariablesV3);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "VariablesV3 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the VariablesV3 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "VariablesV3 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToValV1()
{
    const auto previous = document_.expressionLanguage;
    if (previous == talas::expr::ExpressionLanguageVersion::ValV1)
        return success("Project already uses Val v1");
    if (previous != talas::expr::ExpressionLanguageVersion::VariablesV2
        && previous != talas::expr::ExpressionLanguageVersion::VariablesV3
        && previous != talas::expr::ExpressionLanguageVersion::VariablesV4)
    {
        return failure(
            EditError::InvalidParameterValue,
            "Val v1 upgrade requires Variables v2, v3 or v4 expression authority");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::ValV1;
        candidate.formatVersion = currentProjectFormatVersion;
        const auto audit = auditProjectExpressionsKeepingMeaning(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::ValV1,
            previous);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "Val v1 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Val v1 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Val v1 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToVariablesV4()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV4)
    {
        return success("Project already uses variables v4");
    }
    if (document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::VariablesV2
        && document_.expressionLanguage
            != talas::expr::ExpressionLanguageVersion::VariablesV3)
    {
        return failure(
            EditError::InvalidParameterValue,
            "VariablesV4 upgrade requires VariablesV2 or VariablesV3 expression authority");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV4;
        candidate.formatVersion = currentProjectFormatVersion;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::VariablesV4);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "VariablesV4 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the VariablesV4 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "VariablesV4 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upgradeExpressionsToVoiceV1()
{
    if (document_.expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VoiceV1)
    {
        return success("Project already uses voice expressions v1");
    }
    if (document_.expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        return failure(
            EditError::InvalidParameterValue,
            "VoiceV1 upgrade requires VariablesV2 expression authority");
    }

    try
    {
        auto candidate = document_;
        candidate.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VoiceV1;
        candidate.formatVersion = currentProjectFormatVersion;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            talas::expr::ExpressionLanguageVersion::VoiceV1);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidParameterValue,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidParameterValue,
                "VoiceV1 compatibility audit exceeded the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the VoiceV1 project candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "VoiceV1 project candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectVariable(
    ProjectVariableDeclaration declaration)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& existing) {
                return sameVariableIdentity(
                    existing,
                    declaration.owner,
                    declaration.variableId);
            });
        if (found == candidate.variableLibrary.variables.end())
        {
            candidate.variableLibrary.variables.push_back(
                std::move(declaration));
        }
        else
        {
            if (*found == declaration)
                return success();
            *found = std::move(declaration);
        }
        const auto variableValidation = variableValidationFailure(candidate);
        if (!variableValidation.ok())
            return variableValidation;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            candidate.expressionLanguage);
        if (!audit.ok())
        {
            const auto message = audit.diagnostics.empty()
                ? std::string{
                      "Variable edit exceeded the expression diagnostic limit"}
                : audit.diagnostics.front().path + ": "
                    + audit.diagnostics.front().message;
            return failure(EditError::ProjectVariableInUse, message);
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the variable declaration edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Variable declaration edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::replaceProjectVariable(
    ProjectVariableOwner expectedOwner,
    std::string_view expectedVariableId,
    ProjectVariableDeclaration declaration)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& existing) {
                return sameVariableIdentity(
                    existing,
                    expectedOwner,
                    expectedVariableId);
            });
        if (found == candidate.variableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Variable declaration authority changed before the edit was committed");
        }
        if (*found == declaration)
            return success();
        *found = std::move(declaration);
        const auto variableValidation = variableValidationFailure(candidate);
        if (!variableValidation.ok())
            return variableValidation;
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            candidate.expressionLanguage);
        if (!audit.ok())
        {
            const auto message = audit.diagnostics.empty()
                ? std::string{
                      "Variable replacement exceeded the expression diagnostic limit"}
                : audit.diagnostics.front().path + ": "
                    + audit.diagnostics.front().message;
            return failure(EditError::ProjectVariableInUse, message);
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the variable replacement edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Variable replacement edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectVariable(
    ProjectVariableOwner owner,
    std::string_view variableId)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& declaration) {
                return sameVariableIdentity(
                    declaration, owner, variableId);
            });
        if (found == candidate.variableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Unknown project variable: " + std::string{variableId});
        }
        candidate.variableLibrary.variables.erase(found);
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            candidate.expressionLanguage);
        if (!audit.ok())
        {
            const auto message = audit.diagnostics.empty()
                ? std::string{
                      "Variable removal exceeded the expression diagnostic limit"}
                : audit.diagnostics.front().path + ": "
                    + audit.diagnostics.front().message;
            return failure(EditError::ProjectVariableInUse, message);
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the variable removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Variable removal edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::captureProjectVariableInitialValue(
    std::string_view variableId,
    double value,
    std::uint64_t expectedDocumentAuthorityGeneration)
{
    if (!std::isfinite(value))
    {
        return failure(
            EditError::InvalidProjectVariable,
            "Captured project variable value must be finite");
    }
    if (documentAuthorityGeneration()
        != expectedDocumentAuthorityGeneration)
    {
        return failure(
            EditError::StaleProjectVariableCapture,
            "Project variable capture authority changed");
    }
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner.kind
                        == ProjectVariableOwnerKind::Project;
            });
        if (found == candidate.variableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Unknown project-owned variable: "
                    + std::string{variableId});
        }
        if (found->initialValue == value)
            return success();
        found->initialValue = value;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the variable capture edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Variable capture edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectSoundVariable(
    ProjectSoundVariableDeclaration declaration)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& existing) {
                return sameSoundVariableIdentity(
                    existing,
                    declaration.owner,
                    declaration.variableId);
            });
        if (found == candidate.soundVariableLibrary.variables.end())
        {
            candidate.soundVariableLibrary.variables.push_back(
                std::move(declaration));
        }
        else
        {
            if (*found == declaration)
                return success();
            *found = std::move(declaration);
        }
        const auto validation = soundVariableValidationFailure(candidate);
        if (!validation.ok())
            return validation;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Sound-variable declaration edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Sound-variable declaration edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::replaceProjectSoundVariable(
    ProjectVariableOwner expectedOwner,
    std::string_view expectedVariableId,
    ProjectSoundVariableDeclaration declaration)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& existing) {
                return sameSoundVariableIdentity(
                    existing,
                    expectedOwner,
                    expectedVariableId);
            });
        if (found == candidate.soundVariableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Sound-variable authority changed before commit");
        }
        if (*found == declaration)
            return success();
        *found = std::move(declaration);
        const auto validation = soundVariableValidationFailure(candidate);
        if (!validation.ok())
            return validation;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Sound-variable replacement edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Sound-variable replacement exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectSoundVariable(
    ProjectVariableOwner owner,
    std::string_view variableId)
{
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& declaration) {
                return sameSoundVariableIdentity(
                    declaration,
                    owner,
                    variableId);
            });
        if (found == candidate.soundVariableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Unknown project Sound variable: "
                    + std::string{variableId});
        }
        candidate.soundVariableLibrary.variables.erase(found);
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Sound-variable removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Sound-variable removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::captureProjectSoundVariableInitialSelection(
    std::string_view variableId,
    std::string_view definitionId,
    std::uint64_t expectedDocumentAuthorityGeneration)
{
    if (documentAuthorityGeneration()
        != expectedDocumentAuthorityGeneration)
    {
        return failure(
            EditError::StaleProjectVariableCapture,
            "Project Sound-variable capture authority changed");
    }
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find_if(
            candidate.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner.kind
                        == ProjectVariableOwnerKind::Project;
            });
        if (found == candidate.soundVariableLibrary.variables.end())
        {
            return failure(
                EditError::UnknownProjectVariable,
                "Unknown project-owned Sound variable: "
                    + std::string{variableId});
        }
        const auto selected = std::ranges::find(
            found->candidateDefinitionIds,
            definitionId);
        if (selected == found->candidateDefinitionIds.end())
        {
            return failure(
                EditError::InvalidProjectVariable,
                "Captured Sound is not a candidate of the variable");
        }
        const auto ordinal = static_cast<std::size_t>(
            std::distance(found->candidateDefinitionIds.begin(), selected));
        if (found->initialSelectedOrdinal == ordinal)
            return success();
        found->initialSelectedOrdinal = static_cast<std::uint16_t>(ordinal);
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the Sound-variable capture edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Sound-variable capture exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectGenerator(
    ProjectGeneratorProgram program)
{
    try
    {
        ProjectGeneratorLibrary incoming;
        incoming.programs.push_back(std::move(program));
        const auto incomingValidation =
            validateProjectGeneratorLibrary(incoming);
        if (!incomingValidation.ok())
        {
            ProjectDocument invalid;
            invalid.generatorLibrary = std::move(incoming);
            return generatorValidationFailure(invalid);
        }
        program = std::move(incoming.programs.front());
        auto candidate = document_;
        const auto found = std::ranges::find(
            candidate.generatorLibrary.programs,
            program.generatorId,
            &ProjectGeneratorProgram::generatorId);
        if (found == candidate.generatorLibrary.programs.end())
            candidate.generatorLibrary.programs.push_back(std::move(program));
        else
        {
            if (*found == program)
                return success();
            *found = std::move(program);
        }
        const auto validation = generatorValidationFailure(candidate);
        if (!validation.ok())
            return validation;
        const auto targets = generatorTargetValidationFailure(candidate, registry_);
        if (!targets.ok())
            return targets;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generator declaration edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generator declaration edit exceeded supported capacity");
    }
}

EditResult ProjectEditor::replaceProjectGenerator(
    std::string_view expectedGeneratorId,
    std::uint64_t expectedDocumentAuthorityGeneration,
    ProjectGeneratorProgram program)
{
    try
    {
        if (!talas::generative::isValidContextFreeIdentifier(
                expectedGeneratorId)
            || expectedDocumentAuthorityGeneration == 0
            || expectedDocumentAuthorityGeneration
                != documentAuthorityGeneration())
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Generator authority is invalid");
        }
        ProjectGeneratorLibrary incoming;
        incoming.programs.push_back(std::move(program));
        const auto incomingValidation =
            validateProjectGeneratorLibrary(incoming);
        if (!incomingValidation.ok())
        {
            ProjectDocument invalid;
            invalid.generatorLibrary = std::move(incoming);
            return generatorValidationFailure(invalid);
        }
        program = std::move(incoming.programs.front());
        const auto current = std::ranges::find(
            document_.generatorLibrary.programs,
            expectedGeneratorId,
            &ProjectGeneratorProgram::generatorId);
        if (current == document_.generatorLibrary.programs.end()
            || expectedDocumentAuthorityGeneration
                != documentAuthorityGeneration())
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Generator authority changed before commit");
        }
        auto candidate = document_;
        const auto found = std::ranges::find(
            candidate.generatorLibrary.programs,
            expectedGeneratorId,
            &ProjectGeneratorProgram::generatorId);
        if (found == candidate.generatorLibrary.programs.end())
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Generator authority changed before commit");
        }
        if (*found == program)
            return success();
        *found = std::move(program);
        const auto validation = generatorValidationFailure(candidate);
        if (!validation.ok())
            return validation;
        const auto targets = generatorTargetValidationFailure(candidate, registry_);
        if (!targets.ok())
            return targets;
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generator replacement edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generator replacement exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectGenerator(
    std::string_view generatorId)
{
    try
    {
        if (!talas::generative::isValidContextFreeIdentifier(generatorId))
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Generator authority is invalid");
        }
        const auto current = std::ranges::find(
            document_.generatorLibrary.programs,
            generatorId,
            &ProjectGeneratorProgram::generatorId);
        if (current == document_.generatorLibrary.programs.end())
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Unknown project generator");
        }
        auto candidate = document_;
        const auto found = std::ranges::find(
            candidate.generatorLibrary.programs,
            generatorId,
            &ProjectGeneratorProgram::generatorId);
        if (found == candidate.generatorLibrary.programs.end())
        {
            return failure(
                EditError::UnknownProjectGenerator,
                "Unknown project generator");
        }
        candidate.generatorLibrary.programs.erase(found);
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generator removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generator removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::applyGeneratedTimelineEvents(
    std::vector<ProjectGeneratorTimelineEvent> events,
    const ProjectGeneratorEditAuthority& authority)
{
    try
    {
        const auto authorityMatches = [&]() {
            return authority.documentGeneration != 0
                && authority.documentGeneration
                    == documentAuthorityGeneration()
                && !authority.canonicalDocumentBytes.empty()
                && serializeProjectDocument(document_)
                    == authority.canonicalDocumentBytes;
        };
        if (!authorityMatches())
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Timeline candidate no longer matches the exact project authority");
        }
        if (events.size() > maximumProjectGeneratorTimelineEvents)
        {
            return failure(
                EditError::InvalidProjectGenerator,
                "Generated Timeline candidate exceeds the fixed event bound");
        }
        if (events.empty())
            return success();

        auto candidate = document_;
        candidate.timeline.eventValues.reserve(
            candidate.timeline.eventValues.size() + events.size());
        for (auto& event : events)
        {
            const auto seconds = projectGeneratorTimelineSeconds(
                event.timeMicroseconds);
            if (!seconds.has_value())
            {
                return failure(
                    EditError::InvalidProjectGenerator,
                    "Generated Timeline event exceeds the fixed time horizon");
            }
            candidate.timeline.eventValues.push_back({
                *seconds,
                std::move(event.eventName),
                event.domain,
                event.value,
                event.interpolation,
            });
        }
        sortTimeline(candidate.timeline);
        const auto timeline = validateTimeline(candidate.timeline);
        if (!timeline.ok())
            return timeline;
        if (!authorityMatches())
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Timeline authority changed before commit");
        }
        return applyDocumentEditInternal(
            std::move(candidate), 0, true, true);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generated Timeline edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generated Timeline edit exceeded supported capacity");
    }
    catch (const std::invalid_argument& error)
    {
        return failure(EditError::InvalidProjectGenerator, error.what());
    }
}

EditResult ProjectEditor::applyGeneratedScoreEdits(
    ProjectScoreEditBatch edits,
    const ProjectGeneratorEditAuthority& authority)
{
    try
    {
        const auto authorityMatches = [&]() {
            return authority.documentGeneration != 0
                && authority.documentGeneration
                    == documentAuthorityGeneration()
                && !authority.canonicalDocumentBytes.empty()
                && serializeProjectDocument(document_)
                    == authority.canonicalDocumentBytes;
        };
        if (!authorityMatches())
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Score candidate no longer matches the exact project authority");
        }
        const auto definitionIds = scoreSoundDefinitionIds(document_);
        auto edited = projectScoreWithEdits(
            document_.timeline.score,
            std::move(edits),
            definitionIds);
        if (!edited.ok())
        {
            return failure(
                EditError::InvalidScore,
                edited.error.has_value()
                    ? edited.error->message
                    : "Generated Score edit failed");
        }
        if (*edited.candidate == document_.timeline.score)
            return success();
        auto candidate = document_;
        candidate.timeline.score = std::move(*edited.candidate);
        const auto targets = generatorTargetValidationFailure(candidate, registry_);
        if (!targets.ok())
            return targets;
        if (!authorityMatches())
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Score authority changed before commit");
        }
        return applyDocumentEditInternal(
            std::move(candidate), 0, true, true);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generated Score edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generated Score edit exceeded supported capacity");
    }
    catch (const std::invalid_argument& error)
    {
        return failure(EditError::InvalidProjectGenerator, error.what());
    }
}

EditResult ProjectEditor::applyGeneratedSoundDefinition(
    ProjectGeneratorSoundResult planned,
    const ProjectGeneratorEditAuthority& authority)
{
    try
    {
        const auto authorityMatches = [&]() {
            return authority.documentGeneration != 0
                && authority.documentGeneration
                    == documentAuthorityGeneration()
                && !authority.canonicalDocumentBytes.empty()
                && serializeProjectDocument(document_)
                    == authority.canonicalDocumentBytes;
        };
        if (!planned.ok())
        {
            return failure(
                EditError::InvalidProjectGenerator,
                "Generated Sound candidate is incomplete or invalid");
        }
        if (!authorityMatches())
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Sound candidate no longer matches the exact project authority");
        }
        const auto currentCatalog =
            projectGeneratorSoundCatalogAuthority(
                *planned.definition,
                registry_);
        if (!currentCatalog.has_value())
        {
            return failure(
                EditError::InvalidProjectGenerator,
                "Generated Sound candidate violates the restricted chain authority");
        }
        if (*currentCatalog
            != *planned.catalogAuthorityFingerprint)
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Sound candidate no longer matches the exact catalog authority");
        }
        if (findSoundDefinition(
                document_,
                planned.definition->definitionId) != nullptr)
        {
            return failure(
                EditError::DuplicateSoundDefinition,
                "Generated Sound definition identity already exists");
        }

        auto candidate = document_;
        candidate.soundDefinitions.reserve(
            candidate.soundDefinitions.size() + 1U);
        candidate.soundDefinitions.push_back(*planned.definition);
        const auto generatorTargets =
            generatorTargetValidationFailure(candidate, registry_);
        if (!generatorTargets.ok())
            return generatorTargets;
        const auto validation = projectValidationFailure(
            candidate,
            registry_);
        if (!validation.ok())
            return validation;
        const auto terminalCatalog =
            projectGeneratorSoundCatalogAuthority(
                *planned.definition,
                registry_);
        if (!authorityMatches()
            || !terminalCatalog.has_value()
            || *terminalCatalog
                != *planned.catalogAuthorityFingerprint)
        {
            return failure(
                EditError::StaleProjectGeneratorOutput,
                "Generated Sound authority changed before commit");
        }
        return applyDocumentEditInternal(
            std::move(candidate), 0, true, true);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the generated Sound edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Generated Sound edit exceeded supported capacity");
    }
    catch (const std::invalid_argument& error)
    {
        return failure(EditError::InvalidProjectGenerator, error.what());
    }
}

EditResult ProjectEditor::upsertExpressionDefinition(
    talas::expr::ExpressionDefinition definition)
{
    if (!talas::expr::supportsExpressionDefinitions(
            document_.expressionLanguage))
    {
        return failure(
            EditError::InvalidExpressionDefinition,
            "Expression definitions require PureV4 or SessionV1");
    }
    if (definition.name.size()
            > talas::expr::ExpressionLimits::maximumDefinitionNameBytes
        || definition.source.size()
            > talas::expr::ExpressionLimits::maximumDefinitionSourceBytes
        || definition.parameters.size()
            > talas::expr::ExpressionLimits::maximumDefinitionParameters
        || std::ranges::any_of(
            definition.parameters,
            [](const auto& parameter) {
                return parameter.name.size()
                    > talas::expr::ExpressionLimits::maximumDefinitionNameBytes;
            }))
    {
        return failure(
            EditError::InvalidExpressionDefinition,
            "Expression definition exceeds the fixed input bounds");
    }
    try
    {
        auto candidate = document_;
        auto found = std::ranges::find(
            candidate.expressionDefinitions.definitions,
            definition.name,
            &talas::expr::ExpressionDefinition::name);
        if (found == candidate.expressionDefinitions.definitions.end())
        {
            candidate.expressionDefinitions.definitions.push_back(
                std::move(definition));
        }
        else
        {
            *found = std::move(definition);
        }
        std::ranges::sort(
            candidate.expressionDefinitions.definitions,
            {},
            &talas::expr::ExpressionDefinition::name);
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            candidate.expressionLanguage);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::InvalidExpressionDefinition,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::InvalidExpressionDefinition,
                "Expression definition diagnostics exceed the fixed limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the expression definition candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Expression definition candidate exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeExpressionDefinition(
    std::string_view definitionName)
{
    if (!talas::expr::supportsExpressionDefinitions(
            document_.expressionLanguage))
    {
        return failure(
            EditError::InvalidExpressionDefinition,
            "Expression definitions require PureV4 or SessionV1");
    }
    if (definitionName.size()
        > talas::expr::ExpressionLimits::maximumDefinitionNameBytes)
    {
        return failure(
            EditError::UnknownExpressionDefinition,
            "Unknown expression definition");
    }
    try
    {
        auto candidate = document_;
        const auto found = std::ranges::find(
            candidate.expressionDefinitions.definitions,
            definitionName,
            &talas::expr::ExpressionDefinition::name);
        if (found == candidate.expressionDefinitions.definitions.end())
        {
            return failure(
                EditError::UnknownExpressionDefinition,
                "Unknown expression definition");
        }
        candidate.expressionDefinitions.definitions.erase(found);
        const auto audit = auditProjectExpressions(
            candidate,
            registry_,
            candidate.expressionLanguage);
        if (!audit.ok())
        {
            if (!audit.diagnostics.empty())
            {
                const auto& diagnostic = audit.diagnostics.front();
                return failure(
                    EditError::ExpressionDefinitionInUse,
                    diagnostic.path + " bytes "
                        + std::to_string(diagnostic.range.offset) + "-"
                        + std::to_string(
                            diagnostic.range.offset
                            + diagnostic.range.length)
                        + ": " + diagnostic.message);
            }
            return failure(
                EditError::ExpressionDefinitionInUse,
                "Expression definition removal exceeds the diagnostic limit");
        }
        return applyDocumentEditInternal(std::move(candidate), 0, false);
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the expression definition removal candidate");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Expression definition removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::addNode(
    ProjectNode node,
    CanvasPosition position)
{
    if (node.instanceId.empty())
        return failure(EditError::InvalidNodeId, "Node instance id cannot be empty");
    if (findNode(document_, node.instanceId) != nullptr)
        return failure(EditError::DuplicateNodeId, "Duplicate node instance id: " + node.instanceId);

    if (!node.typeId.empty() && node.soundDefinitionId.has_value())
    {
        return failure(
            EditError::InvalidGraph,
            "Node must reference exactly one built-in type or Sound definition");
    }
    if (node.soundDefinitionId.has_value()
        && findSoundDefinition(document_, *node.soundDefinitionId) == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + *node.soundDefinitionId);
    }
    const auto descriptor =
        describeProjectNode(document_, node, registry_);
    if (!descriptor.has_value())
    {
        return failure(
            EditError::UnknownNodeType,
            "Unknown node type: " + node.typeId);
    }
    if (const auto preservation =
            freezeAuthoredExternalComponent(node, *descriptor);
        preservation.has_value())
    {
        return *preservation;
    }

    const auto parameterResult =
        validateNodeParameters(node, *descriptor);
    if (!parameterResult.ok())
        return parameterResult;

    const auto nodeId = node.instanceId;
    ProjectDocument candidate = document_;
    candidate.graph.nodes.push_back(std::move(node));
    reconcileProjectPresentation(candidate);
    findProjectNodePresentation(candidate, std::nullopt, nodeId)->position =
        position;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::addSoundInstance(
    std::string instanceId,
    std::string soundDefinitionId,
    CanvasPosition position)
{
    ProjectNode node;
    node.instanceId = std::move(instanceId);
    node.soundDefinitionId = std::move(soundDefinitionId);
    return addNode(std::move(node), position);
}

EditResult ProjectEditor::addNodeToSoundDefinition(
    std::string_view definitionId,
    ProjectNode node,
    CanvasPosition position)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (node.instanceId.empty())
        return failure(EditError::InvalidNodeId, "Node instance id cannot be empty");
    if (findNode(definition->graph, node.instanceId) != nullptr)
        return failure(EditError::DuplicateNodeId, "Duplicate node instance id: " + node.instanceId);
    if (!node.typeId.empty() && node.soundDefinitionId.has_value())
    {
        return failure(
            EditError::InvalidGraph,
            "Node must reference exactly one built-in type or Sound definition");
    }
    if (node.soundDefinitionId.has_value()
        && findSoundDefinition(document_, *node.soundDefinitionId) == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + *node.soundDefinitionId);
    }
    const auto descriptor = describeProjectNode(document_, node, registry_);
    if (!descriptor.has_value())
    {
        return failure(
            EditError::UnknownNodeType,
            "Unknown node type: " + node.typeId);
    }
    if (const auto preservation =
            freezeAuthoredExternalComponent(node, *descriptor);
        preservation.has_value())
    {
        return *preservation;
    }
    const auto parameterResult = validateNodeParameters(node, *descriptor);
    if (!parameterResult.ok())
        return parameterResult;

    const auto nodeId = node.instanceId;
    ProjectDocument candidate = document_;
    findSoundDefinition(candidate, definitionId)
        ->graph.nodes.push_back(std::move(node));
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    reconcileProjectPresentation(candidate);
    findProjectNodePresentation(candidate, definitionId, nodeId)->position =
        position;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::addSoundInstanceToSoundDefinition(
    std::string_view parentDefinitionId,
    std::string instanceId,
    std::string soundDefinitionId,
    CanvasPosition position)
{
    ProjectNode node;
    node.instanceId = std::move(instanceId);
    node.soundDefinitionId = std::move(soundDefinitionId);
    return addNodeToSoundDefinition(
        parentDefinitionId,
        std::move(node),
        position);
}

EditResult ProjectEditor::duplicateNodesInSoundDefinition(
    std::string_view definitionId,
    std::span<const std::string> nodeIds,
    std::vector<std::string>& duplicatedNodeIds,
    CanvasPosition offset)
{
    ProjectDocument candidate = document_;
    const auto result = duplicateGraphSelection(
        candidate,
        definitionId,
        nodeIds,
        offset,
        duplicatedNodeIds,
        registry_);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::removeNodesFromSoundDefinition(
    std::string_view definitionId,
    std::span<const std::string> nodeIds)
{
    ProjectDocument candidate = document_;
    const auto result = removeGraphSelection(
        candidate, definitionId, nodeIds);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::moveNodesInSoundDefinition(
    std::string_view definitionId,
    std::span<const std::string> nodeIds,
    CanvasPosition delta,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Graph interaction transaction is no longer authoritative");
    }
    if (delta.x == 0 && delta.y == 0)
        return success();
    ProjectDocument candidate = document_;
    const auto result = moveGraphSelection(
        candidate, definitionId, nodeIds, delta);
    if (!result.ok())
        return result;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::removeNodeFromSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (findNode(definition->graph, nodeId) == nullptr)
    {
        return failure(
            EditError::UnknownNodeId,
            "Unknown node instance: " + std::string(nodeId));
    }

    ProjectDocument candidate = document_;
    auto* edited = findSoundDefinition(candidate, definitionId);
    auto& graph = edited->graph;
    std::erase_if(graph.nodes, [&](const auto& node) {
        return node.instanceId == nodeId;
    });
    std::erase_if(graph.connections, [&](const auto& connection) {
        return connection.fromNode == nodeId || connection.toNode == nodeId;
    });
    std::erase_if(edited->inputs, [&](const ProjectSoundPort& port) {
        return port.replacesSound && port.nodeId == nodeId;
    });
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::moveNodeInSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId,
    CanvasPosition position,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Graph interaction transaction is no longer authoritative");
    }
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (findNode(definition->graph, nodeId) == nullptr)
    {
        return failure(
            EditError::UnknownNodeId,
            "Unknown node instance: " + std::string(nodeId));
    }
    const auto* presented = findProjectNodePresentation(
        document_,
        definitionId,
        nodeId);
    if (presented != nullptr && presented->position == position)
        return success();
    ProjectDocument candidate = document_;
    reconcileProjectPresentation(candidate);
    findProjectNodePresentation(candidate, definitionId, nodeId)->position =
        position;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::connectInSoundDefinition(
    std::string_view definitionId,
    talas::core::Connection connection)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* fromNode = findNode(definition->graph, connection.fromNode);
    if (fromNode == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown source node: " + connection.fromNode);
    const auto* toNode = findNode(definition->graph, connection.toNode);
    if (toNode == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown target node: " + connection.toNode);
    if (const auto unavailable =
            unresolvedComponentEditFailure(*fromNode, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    if (const auto unavailable =
            unresolvedComponentEditFailure(*toNode, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    const auto fromDescriptor = describeProjectNode(document_, *fromNode, registry_);
    const auto toDescriptor = describeProjectNode(document_, *toNode, registry_);
    if (!fromDescriptor.has_value() || !toDescriptor.has_value())
        return failure(EditError::UnknownNodeType, "Connection references a node with an unknown type");
    const auto* output = findPort(fromDescriptor->outputs, connection.fromPort);
    if (output == nullptr)
        return failure(EditError::UnknownPort, "Unknown output port: " + connection.fromPort);
    const auto* input = findPort(toDescriptor->inputs, connection.toPort);
    if (input == nullptr)
        return failure(EditError::UnknownPort, "Unknown input port: " + connection.toPort);
    if (!talas::core::portDescriptorsCompatible(*output, *input))
        return failure(
            EditError::IncompatiblePortTypes,
            output->type == input->type
                ? "Connection spectral formats do not match"
                : "Connection port types do not match");
    if (!input->acceptsMany
        && std::ranges::any_of(
            definition->graph.connections,
            [&](const auto& existing) {
                return existing.toNode == connection.toNode
                    && existing.toPort == connection.toPort;
            }))
    {
        return failure(EditError::DuplicateInputConnection, "Input port already has a connection");
    }
    // A port that takes many Inputs still takes each connection once.
    if (std::ranges::find(definition->graph.connections, connection)
        != definition->graph.connections.end())
    {
        return failure(EditError::DuplicateInputConnection, "The connection already exists");
    }

    ProjectDocument candidate = document_;
    findSoundDefinition(candidate, definitionId)
        ->graph.connections.push_back(std::move(connection));
    if (containsCycle(candidate, registry_))
        return failure(EditError::InvalidGraph, "Connection would create a direct graph cycle");
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::disconnectInSoundDefinition(
    std::string_view definitionId,
    std::string_view toNode,
    std::string_view toPort)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* node = findNode(definition->graph, toNode);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown target node: " + std::string(toNode));
    const auto descriptor =
        describeProjectNodeForEditing(document_, *node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Target node has an unknown type: " + node->typeId);
    const auto* input = findPort(descriptor->inputs, toPort);
    if (input == nullptr)
        return failure(EditError::UnknownPort, "Unknown input port: " + std::string(toPort));
    if (std::ranges::none_of(
            definition->graph.connections,
            [&](const auto& connection) {
                return connection.toNode == toNode
                    && connection.toPort == toPort;
            }))
    {
        return success();
    }

    ProjectDocument candidate = document_;
    removeInputConnections(
        findSoundDefinition(candidate, definitionId)->graph.connections,
        *input,
        toNode);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::disconnectPreservedComponentInSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* node = findNode(definition->graph, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (!node->preservedComponent.has_value())
        return failure(EditError::InvalidSelection, "Node has no preserved component capsule");
    if (std::ranges::none_of(
            definition->graph.connections,
            [&](const auto& connection) {
                return connection.fromNode == nodeId
                    || connection.toNode == nodeId;
            }))
    {
        return success();
    }

    ProjectDocument candidate = document_;
    auto& connections = findSoundDefinition(candidate, definitionId)
                            ->graph.connections;
    std::erase_if(connections, [&](const auto& connection) {
        return connection.fromNode == nodeId
            || connection.toNode == nodeId;
    });
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setParameterInSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId,
    ProjectParameterExpression parameter)
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* node = findNode(definition->graph, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (const auto unavailable =
            unresolvedComponentEditFailure(*node, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    const auto descriptor = describeProjectNode(document_, *node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Node has an unknown type: " + node->typeId);
    const auto* descriptorParameter = findParameter(*descriptor, parameter.parameterId);
    if (descriptorParameter == nullptr)
        return failure(EditError::UnknownParameter, "Unknown parameter: " + parameter.parameterId);
    const auto valueResult = validateParameterValue(parameter, *descriptorParameter);
    if (!valueResult.ok())
        return valueResult;

    const auto expressionResult = validateAuthoredParameterExpression(
        document_,
        *node,
        parameter,
        *descriptorParameter,
        document_.expressionLanguage);
    if (!expressionResult.ok())
        return expressionResult;
    if (scriptCodeStringField(*node, parameter.parameterId) && parameter.text.has_value())
    {
        if (auto refused = codeStringFailure("Script", *parameter.text); refused.has_value())
            return *refused;
    }
    if (node->soundDefinitionId.has_value())
    {
        if (const auto* inner = findSoundDefinition(document_, *node->soundDefinitionId); inner != nullptr)
        {
            const auto exposed = std::ranges::find(inner->exposedParameters, parameter.parameterId,
                &ProjectSoundExposedParameter::parameterId);
            if (exposed != inner->exposedParameters.end())
            {
                if (auto refused = classFieldKindFailure(document_, *exposed, parameter); refused.has_value())
                    return *refused;
            }
        }
    }

    ProjectDocument candidate = document_;
    auto* candidateNode = findNode(
        findSoundDefinition(candidate, definitionId)->graph,
        nodeId);
    const auto parameterCount = static_cast<std::size_t>(std::ranges::count_if(
        candidateNode->parameters,
        [&](const auto& existing) {
            return existing.parameterId == parameter.parameterId;
        }));
    if (parameterCount > 1)
        return failure(EditError::DuplicateParameter, "Node contains duplicate parameter: " + parameter.parameterId);
    if (parameterCount == 1)
    {
        *std::ranges::find_if(
            candidateNode->parameters,
            [&](const auto& existing) {
                return existing.parameterId == parameter.parameterId;
            }) = std::move(parameter);
    }
    else
    {
        candidateNode->parameters.push_back(std::move(parameter));
    }
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::setResourceParameterAssetInSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId,
    std::string_view parameterId,
    ProjectAsset asset)
{
    const auto assetValidation = validateAsset(asset);
    if (!assetValidation.ok())
        return assetValidation;
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* node = findNode(definition->graph, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (const auto unavailable =
            unresolvedComponentEditFailure(*node, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    const auto descriptor = describeProjectNode(document_, *node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Node has an unknown built-in or Sound type");
    const auto* parameterDescriptor = findParameter(*descriptor, parameterId);
    if (parameterDescriptor == nullptr)
        return failure(EditError::UnknownParameter, "Unknown parameter: " + std::string(parameterId));
    if (parameterDescriptor->kind != talas::core::ParameterKind::Resource)
        return failure(EditError::InvalidParameterValue, "Project assets can only bind to Resource parameters");

    ProjectDocument candidate = document_;
    const auto candidateAsset = std::ranges::find_if(
        candidate.assets,
        [&](const auto& value) {
            return value.id == asset.id;
        });
    if (candidateAsset == candidate.assets.end())
        candidate.assets.push_back(asset);
    else if (*candidateAsset != asset)
    {
        return failure(
            EditError::InvalidAsset,
            "Existing asset manifests must use explicit relocation or replacement");
    }

    auto* candidateNode = findNode(
        findSoundDefinition(candidate, definitionId)->graph,
        nodeId);
    const auto parameterCount = static_cast<std::size_t>(std::ranges::count_if(
        candidateNode->parameters,
        [&](const auto& parameter) {
            return parameter.parameterId == parameterId;
        }));
    if (parameterCount > 1)
        return failure(EditError::DuplicateParameter, "Node contains duplicate parameter: " + std::string(parameterId));
    ProjectParameterExpression parameter;
    parameter.parameterId = std::string(parameterId);
    parameter.resource = asset.id;
    if (parameterCount == 1)
    {
        *std::ranges::find_if(
            candidateNode->parameters,
            [&](const auto& value) {
                return value.parameterId == parameterId;
            }) = std::move(parameter);
    }
    else
    {
        candidateNode->parameters.push_back(std::move(parameter));
    }
    const auto assetsValidation = validateAssets(candidate.assets);
    if (!assetsValidation.ok())
        return assetsValidation;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::repairPreservedComponentInSoundDefinition(
    std::string_view definitionId,
    std::string_view nodeId) const
{
    const auto* definition = findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto* node = findNode(definition->graph, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (!node->preservedComponent.has_value())
        return failure(EditError::InvalidSelection, "Node has no preserved component capsule");
    const auto availability = classifyProjectComponentAvailability(
        node->typeId,
        &*node->preservedComponent,
        registry_);
    return availability.availability
            == ProjectComponentAvailability::Resolved
        ? success(availability.message)
        : failure(EditError::UnresolvedComponent, availability.message);
}

EditResult ProjectEditor::removeNode(std::string_view nodeId)
{
    if (findNode(document_, nodeId) == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));

    ProjectDocument candidate = document_;
    std::erase_if(candidate.graph.nodes, [&](const ProjectNode& node) {
        return node.instanceId == nodeId;
    });
    std::erase_if(candidate.graph.connections, [&](const talas::core::Connection& connection) {
        return connection.fromNode == nodeId || connection.toNode == nodeId;
    });
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

std::vector<std::string> ProjectEditor::inputsReplacingNode(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId) const
{
    std::vector<std::string> inputs;
    const auto* definition = definitionId.has_value() ? findSoundDefinition(document_, *definitionId) : nullptr;
    const auto* graph = !definitionId.has_value() ? &document_.graph : definition == nullptr ? nullptr : &definition->graph;
    if (graph == nullptr)
        return inputs;
    for (const auto& connection : graph->connections)
    {
        if (connection.toNode == nodeId && std::ranges::find(inputs, connection.fromNode) == inputs.end())
            inputs.push_back(connection.fromNode);
    }
    return inputs;
}

EditResult ProjectEditor::replaceNodeWithItsInput(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::optional<std::string_view> inputNodeId)
{
    ProjectDocument candidate = document_;
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
    }
    if (findNode(*graph, nodeId) == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    const auto input = std::ranges::find_if(graph->connections, [&](const talas::core::Connection& connection) {
        return connection.toNode == nodeId && (!inputNodeId.has_value() || connection.fromNode == *inputNodeId);
    });
    if (inputNodeId.has_value() && input == graph->connections.end())
    {
        return failure(EditError::UnknownNodeId,
            std::string{*inputNodeId} + " is not an input of " + std::string{nodeId});
    }
    std::vector<talas::core::Connection> fed;
    for (const auto& connection : graph->connections)
    {
        if (connection.fromNode == nodeId)
            fed.push_back(connection);
    }
    if (input == graph->connections.end() && !fed.empty())
    {
        return failure(EditError::InvalidGraph,
            std::string{nodeId} + " has no input to take its place in the Sounds it feeds");
    }
    if (input != graph->connections.end())
    {
        const auto source = *input;
        for (const auto& connection : fed)
        {
            const talas::core::Connection spliced{source.fromNode, source.fromPort, connection.toNode, connection.toPort};
            if (std::ranges::find(graph->connections, spliced) == graph->connections.end())
                graph->connections.push_back(spliced);
        }
    }
    const std::array removed{std::string{nodeId}};
    const auto result = removeGraphSelection(candidate, definitionId, removed);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::duplicateNodes(
    std::span<const std::string> nodeIds,
    std::vector<std::string>& duplicatedNodeIds,
    CanvasPosition offset)
{
    ProjectDocument candidate = document_;
    const auto result = duplicateGraphSelection(
        candidate,
        std::nullopt,
        nodeIds,
        offset,
        duplicatedNodeIds,
        registry_);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

std::vector<std::string> ProjectEditor::inputsTakingConnection(
    std::optional<std::string_view> definitionId,
    const ProjectNode& node,
    const talas::core::Connection& connection) const
{
    std::vector<std::string> inputs;
    const auto* definition = definitionId.has_value() ? findSoundDefinition(document_, *definitionId) : nullptr;
    const auto* graph = !definitionId.has_value() ? &document_.graph : definition == nullptr ? nullptr : &definition->graph;
    if (graph == nullptr || std::ranges::find(graph->connections, connection) == graph->connections.end())
        return inputs;
    const auto* fromNode = findNode(*graph, connection.fromNode);
    if (fromNode == nullptr)
        return inputs;
    const auto fromDescriptor = describeProjectNode(document_, *fromNode, registry_);
    const auto nodeDescriptor = describeProjectNode(document_, node, registry_);
    if (!fromDescriptor.has_value() || !nodeDescriptor.has_value())
        return inputs;
    const auto* source = findPort(fromDescriptor->outputs, connection.fromPort);
    if (source == nullptr)
        return inputs;
    // A Sound with a replaceable input takes the line there.
    if (const auto* dropped = node.soundDefinitionId.has_value() ? findSoundDefinition(document_, *node.soundDefinitionId) : nullptr)
    {
        if (const auto* replaceable = replaceableSoundInput(*dropped);
            replaceable != nullptr && replaceable->replacesSound && replaceable->type == source->type)
        {
            inputs.push_back(replaceable->portId);
            return inputs;
        }
    }
    // Its Sound inputs of the signal's own kind, a stereo input's right
    // channel (rightIn, input0Right), which comes down the same
    // line, aside; a Sound with none takes the line where insertion would,
    // a field of another kind.
    const auto rightChannel = [](std::string_view id) {
        return id.ends_with("Right")
            || (id.starts_with("right") && id.size() > 5 && std::isupper(static_cast<unsigned char>(id[5])) != 0);
    };
    for (const auto& input : nodeDescriptor->inputs)
    {
        if (!input.compilerPopulated && input.type == source->type && !rightChannel(input.id)
            && talas::core::portDescriptorsCompatible(*source, input))
            inputs.push_back(input.id);
    }
    if (inputs.empty())
    {
        if (const auto* input = firstCompatibleInput(*nodeDescriptor, *source))
            inputs.push_back(input->id);
    }
    return inputs;
}

EditResult ProjectEditor::insertNodeOnConnection(
    std::optional<std::string_view> definitionId,
    ProjectNode node,
    CanvasPosition position,
    const talas::core::Connection& connection,
    std::optional<std::string_view> inputPortId)
{
    ProjectDocument candidate = document_;
    const auto result = insertGraphNodeOnConnection(
        candidate, definitionId, std::move(node), position, connection, registry_, inputPortId);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::mixNodeOnConnection(
    std::optional<std::string_view> definitionId,
    ProjectNode mixer,
    ProjectNode node,
    CanvasPosition position,
    const talas::core::Connection& connection)
{
    ProjectDocument candidate = document_;
    const auto mixerId = mixer.instanceId;
    const auto nodeId = node.instanceId;
    if (mixerId == nodeId)
        return failure(EditError::DuplicateNodeId, "The mixer and the dropped Sound need names of their own");
    if (const auto inserted = insertGraphNodeOnConnection(
            candidate, definitionId, std::move(mixer), position, connection, registry_, std::string_view{"inputs"});
        !inserted.ok())
        return inserted;
    auto* graph = graphForEditScope(candidate, definitionId);
    if (const auto valid = validateNodeForGraph(candidate, *graph, node, registry_); !valid.ok())
        return valid;
    const auto descriptor = describeProjectNode(candidate, node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "The dropped Sound's type is unknown");
    const auto output = std::ranges::find(descriptor->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
    if (output == descriptor->outputs.end())
        return failure(EditError::IncompatiblePortTypes, "The dropped Sound has no audio output to mix");
    graph->connections.push_back({nodeId, output->id, mixerId, "inputs"});
    graph->nodes.push_back(std::move(node));
    reconcileProjectPresentation(candidate);
    if (auto* presented = findProjectNodePresentation(candidate, definitionId, nodeId))
        presented->position = {position.x - 160, position.y + 90};
    if (containsCycle(candidate, registry_))
        return failure(EditError::InvalidGraph, "Mixing would create a direct graph cycle");
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::replaceNode(
    std::optional<std::string_view> definitionId,
    std::string_view replacedNodeId,
    ProjectNode replacement,
    bool withItsInputs)
{
    // A SoundCollectionVariable goes into an Inputs
    // field; it is not dropped onto a Sound's icon.
    if (replacement.typeId == "talas.soundCollectionVariable")
        return failure(EditError::InvalidGraph, "A SoundCollectionVariable goes into an Inputs field, not onto a Sound");
    ProjectDocument candidate = document_;
    if (withItsInputs)
    {
        // The Sounds upstream of the replaced one whose every line leads
        // into it or into another such Sound.
        const auto* graph = graphForEditScope(candidate, definitionId);
        if (graph == nullptr)
        {
            return failure(
                EditError::UnknownSoundDefinition,
                "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
        }
        if (findNode(*graph, replacedNodeId) == nullptr)
            return failure(EditError::UnknownNodeId, "Unknown node to replace: " + std::string{replacedNodeId});
        std::unordered_set<std::string> gone{std::string{replacedNodeId}};
        std::vector<std::string> inputs;
        for (auto grew = true; grew;)
        {
            grew = false;
            for (const auto& connection : graph->connections)
            {
                if (!gone.contains(connection.toNode) || gone.contains(connection.fromNode))
                    continue;
                const auto feedsOnlyThem = std::ranges::all_of(graph->connections, [&](const talas::core::Connection& line) {
                    return line.fromNode != connection.fromNode || gone.contains(line.toNode);
                });
                if (feedsOnlyThem)
                {
                    gone.insert(connection.fromNode);
                    inputs.push_back(connection.fromNode);
                    grew = true;
                }
            }
        }
        if (!inputs.empty())
        {
            if (const auto removed = removeGraphSelection(candidate, definitionId, inputs); !removed.ok())
                return removed;
        }
        // A Sound that also feeds others stays, without its line here.
        auto* edited = graphForEditScope(candidate, definitionId);
        std::erase_if(edited->connections, [&](const talas::core::Connection& connection) {
            return connection.toNode == replacedNodeId;
        });
    }
    const auto replacementId = replacement.instanceId;
    const auto result = replaceGraphNode(
        candidate, definitionId, replacedNodeId, std::move(replacement), registry_);
    if (!result.ok())
        return result;
    // Replace all leaves the replacement with its default
    // input, as a prototype comes with one: a Noise in each required audio
    // input left free.
    if (withItsInputs)
    {
        auto* graph = graphForEditScope(candidate, definitionId);
        const auto* placed = graph == nullptr ? nullptr : findNode(*graph, replacementId);
        const auto* type = placed == nullptr ? nullptr : registry_.findNodeType(placed->typeId);
        const auto inputs = type == nullptr ? std::span<const talas::core::PortDescriptor>{}
                                            : std::span<const talas::core::PortDescriptor>{type->inputs};
        for (const auto& input : inputs)
        {
            if (!input.required || input.compilerPopulated || input.type != talas::core::PortType::Audio
                || std::ranges::any_of(graph->connections, [&](const talas::core::Connection& line) {
                       return line.toNode == replacementId && line.toPort == input.id;
                   }))
                continue;
            std::string noiseId = "noise";
            for (int suffix = 2; findNode(*graph, noiseId) != nullptr; ++suffix)
                noiseId = "noise" + std::to_string(suffix);
            graph->nodes.push_back({noiseId, "talas.noise", {}});
            graph->connections.push_back({noiseId, "audioOut", replacementId, input.id});
        }
        reconcileProjectPresentation(candidate);
    }
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

std::vector<std::string> ProjectEditor::inputsOf(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::string_view portId) const
{
    std::vector<std::string> sources;
    const ProjectGraph* graph = &document_.graph;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document_.soundDefinitions, *definitionId, &ProjectSoundDefinition::definitionId);
        if (definition == document_.soundDefinitions.end())
            return sources;
        graph = &definition->graph;
    }
    for (const auto& connection : graph->connections)
    {
        if (connection.toNode == nodeId && connection.toPort == portId)
            sources.push_back(connection.fromNode);
    }
    return sources;
}

EditResult ProjectEditor::setInputs(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::string_view portId,
    std::vector<std::string> sources)
{
    ProjectDocument candidate = document_;
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
    }
    if (findNode(*graph, nodeId) == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown Sound: " + std::string{nodeId});
    std::vector<talas::core::Connection> previous;
    std::vector<std::size_t> slots;
    for (std::size_t index = 0; index < graph->connections.size(); ++index)
    {
        if (graph->connections[index].toNode == nodeId && graph->connections[index].toPort == portId)
        {
            previous.push_back(graph->connections[index]);
            slots.push_back(index);
        }
    }
    std::vector<talas::core::Connection> next;
    for (const auto& source : sources)
    {
        const auto* sourceNode = findNode(*graph, source);
        if (sourceNode == nullptr)
            return failure(EditError::UnknownNodeId, "No Sound named " + source + " in this structure");
        // A Sound already there keeps its output; another takes its first.
        const auto kept = std::ranges::find(previous, source, &talas::core::Connection::fromNode);
        std::string fromPort;
        if (kept != previous.end())
        {
            fromPort = kept->fromPort;
            previous.erase(kept);
        }
        else
        {
            const auto descriptor = describeProjectNode(candidate, *sourceNode, registry_);
            if (!descriptor.has_value() || descriptor->outputs.empty())
                return failure(EditError::InvalidGraph, source + " has no output");
            fromPort = descriptor->outputs.front().id;
        }
        next.push_back({source, fromPort, std::string{nodeId}, std::string{portId}});
    }
    // The input's lines, in the new order, where its first one was.
    const auto at = slots.empty() ? graph->connections.size() : slots.front();
    for (auto slot = slots.rbegin(); slot != slots.rend(); ++slot)
        graph->connections.erase(graph->connections.begin() + static_cast<std::ptrdiff_t>(*slot));
    graph->connections.insert(graph->connections.begin() + static_cast<std::ptrdiff_t>(std::min(at, graph->connections.size())),
        next.begin(), next.end());
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::addInputSound(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::string_view portId,
    ProjectNode node,
    std::optional<std::size_t> replacing)
{
    ProjectDocument candidate = document_;
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
    }
    if (findNode(*graph, nodeId) == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown Sound: " + std::string{nodeId});
    if (const auto valid = validateNodeForGraph(candidate, *graph, node, registry_); !valid.ok())
        return valid;
    const auto descriptor = describeProjectNode(candidate, node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "The dropped Sound's type is unknown");
    const auto output = std::ranges::find(descriptor->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
    if (output == descriptor->outputs.end())
        return failure(EditError::IncompatiblePortTypes, "The dropped Sound has no audio output to feed the input");
    const auto inputs = std::ranges::count_if(graph->connections, [&](const talas::core::Connection& connection) {
        return connection.toNode == nodeId && connection.toPort == portId;
    });
    const auto addedId = node.instanceId;
    std::optional<std::string> replacedId;
    std::optional<CanvasPosition> replacedAt;
    if (replacing.has_value())
    {
        // The new Sound takes the place of one already there.
        if (*replacing >= static_cast<std::size_t>(inputs))
            return failure(EditError::InvalidSelection, "There is no Sound in " + std::string{portId} + " to replace");
        auto place = graph->connections.begin();
        for (std::size_t seen = 0;; ++place)
        {
            if (place->toNode == nodeId && place->toPort == portId && seen++ == *replacing)
                break;
        }
        replacedId = place->fromNode;
        if (const auto* presented = findProjectNodePresentation(candidate, definitionId, *replacedId))
            replacedAt = presented->position;
        place->fromNode = addedId;
        place->fromPort = output->id;
    }
    else
    {
        graph->connections.push_back({addedId, output->id, std::string{nodeId}, std::string{portId}});
    }
    graph->nodes.push_back(std::move(node));
    if (replacedId.has_value()
        && std::ranges::none_of(graph->connections, [&](const talas::core::Connection& line) { return line.fromNode == *replacedId; }))
    {
        // The Sound replaced, and the Sounds that fed only it, go.
        std::unordered_set<std::string> gone{*replacedId};
        std::vector<std::string> removed{*replacedId};
        for (auto grew = true; grew;)
        {
            grew = false;
            for (const auto& connection : graph->connections)
            {
                if (!gone.contains(connection.toNode) || gone.contains(connection.fromNode))
                    continue;
                if (std::ranges::all_of(graph->connections, [&](const talas::core::Connection& line) {
                        return line.fromNode != connection.fromNode || gone.contains(line.toNode);
                    }))
                {
                    gone.insert(connection.fromNode);
                    removed.push_back(connection.fromNode);
                    grew = true;
                }
            }
        }
        if (const auto result = removeGraphSelection(candidate, definitionId, removed); !result.ok())
            return result;
        graph = graphForEditScope(candidate, definitionId);
    }
    const auto fed = findProjectNodePresentation(candidate, definitionId, nodeId);
    const auto at = fed == nullptr ? CanvasPosition{} : fed->position;
    reconcileProjectPresentation(candidate);
    if (auto* presented = findProjectNodePresentation(candidate, definitionId, addedId))
        presented->position = replacedAt.value_or(CanvasPosition{at.x - 200, at.y + static_cast<std::int32_t>(inputs) * 90});
    if (containsCycle(candidate, registry_))
        return failure(EditError::InvalidGraph, "The input would create a direct graph cycle");
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::shareNodeInInput(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::string_view portId,
    std::string_view sharedNodeId)
{
    const auto* graph = graphForEditScope(document_, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
    }
    const auto* node = findNode(*graph, nodeId);
    if (node == nullptr || findNode(*graph, sharedNodeId) == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown Sound to share or paste into");
    const auto descriptor = describeProjectNode(document_, *node, registry_);
    const auto unknownPort = failure(EditError::UnknownPort, std::string{nodeId} + " has no input " + std::string{portId});
    if (!descriptor.has_value())
        return unknownPort;
    const auto port = std::ranges::find(descriptor->inputs, portId, &talas::core::PortDescriptor::id);
    if (port == descriptor->inputs.end())
        return unknownPort;
    auto inputs = inputsOf(definitionId, nodeId, portId);
    if (port->acceptsMany)
    {
        inputs.emplace_back(sharedNodeId);
        return setInputs(definitionId, nodeId, portId, std::move(inputs));
    }
    if (inputs.size() == 1 && inputs.front() == sharedNodeId)
        return failure(EditError::InvalidSelection, std::string{sharedNodeId} + " is already in " + std::string{portId});
    // The field's Sound used only here takes the shared one's place outright.
    if (inputs.size() == 1
        && std::ranges::all_of(graph->connections, [&](const talas::core::Connection& line) {
               return line.fromNode != inputs.front() || (line.toNode == nodeId && line.toPort == portId);
           }))
        return shareNodeInPlaceOf(definitionId, inputs.front(), sharedNodeId);
    return setInputs(definitionId, nodeId, portId, {std::string{sharedNodeId}});
}

EditResult ProjectEditor::shareNodeInPlaceOf(
    std::optional<std::string_view> definitionId,
    std::string_view replacedNodeId,
    std::string_view sharedNodeId)
{
    if (replacedNodeId == sharedNodeId)
        return failure(EditError::InvalidSelection, "A Sound cannot take its own place");
    ProjectDocument candidate = document_;
    auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string{definitionId.value_or(std::string_view{})});
    }
    const auto* replaced = findNode(*graph, replacedNodeId);
    const auto* shared = findNode(*graph, sharedNodeId);
    if (replaced == nullptr || shared == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown Sound to share or replace");
    const auto sharedDescriptor = describeProjectNode(candidate, *shared, registry_);
    if (!sharedDescriptor.has_value() || sharedDescriptor->outputs.empty())
        return failure(EditError::InvalidGraph, "The shared Sound has no output");
    // The Sounds that fed only the replaced one go with it.
    std::unordered_set<std::string> gone{std::string{replacedNodeId}};
    std::vector<std::string> removed{std::string{replacedNodeId}};
    for (auto grew = true; grew;)
    {
        grew = false;
        for (const auto& connection : graph->connections)
        {
            if (!gone.contains(connection.toNode) || gone.contains(connection.fromNode)
                || connection.fromNode == sharedNodeId)
                continue;
            const auto feedsOnlyThem = std::ranges::all_of(graph->connections, [&](const talas::core::Connection& line) {
                return line.fromNode != connection.fromNode || gone.contains(line.toNode);
            });
            if (feedsOnlyThem)
            {
                gone.insert(connection.fromNode);
                removed.push_back(connection.fromNode);
                grew = true;
            }
        }
    }
    for (auto& connection : graph->connections)
    {
        if (connection.fromNode != replacedNodeId)
            continue;
        connection.fromNode = std::string{sharedNodeId};
        if (std::ranges::none_of(sharedDescriptor->outputs, [&](const auto& port) { return port.id == connection.fromPort; }))
            connection.fromPort = sharedDescriptor->outputs.front().id;
    }
    if (const auto result = removeGraphSelection(candidate, definitionId, removed); !result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::revertNodeParameters(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::vector<ProjectParameterExpression> parameters)
{
    ProjectDocument candidate = document_;
    auto* graph = &candidate.graph;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find(
            candidate.soundDefinitions, *definitionId, &ProjectSoundDefinition::definitionId);
        if (definition == candidate.soundDefinitions.end())
            return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string(*definitionId));
        graph = &definition->graph;
    }
    const auto node = std::ranges::find(graph->nodes, nodeId, &ProjectNode::instanceId);
    if (node == graph->nodes.end())
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (node->parameters == parameters)
        return success("Nothing to revert");
    node->parameters = std::move(parameters);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::renameNode(
    std::optional<std::string_view> definitionId,
    std::string_view nodeId,
    std::string newNodeId)
{
    ProjectDocument candidate = document_;
    const auto result = renameGraphNode(candidate, definitionId, nodeId, newNodeId);
    if (!result.ok() || newNodeId == nodeId)
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::setReplaceableInput(
    std::string_view definitionId,
    std::string_view nodeId)
{
    ProjectDocument candidate = document_;
    auto* definition = findSoundDefinition(candidate, definitionId);
    if (definition == nullptr)
        return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string(definitionId));
    const auto* node = findNode(definition->graph, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    const auto descriptor = node->soundDefinitionId.has_value() || node->typeId == "talas.variable"
            || node->typeId == "talas.soundCollectionVariable"
        ? std::nullopt
        : describeProjectNode(candidate, *node, registry_);
    std::string outputPortId;
    if (descriptor.has_value())
    {
        if (const auto output = std::ranges::find(descriptor->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
            output != descriptor->outputs.end())
        {
            outputPortId = output->id;
        }
    }
    if (outputPortId.empty()
        || std::ranges::any_of(definition->outputs, [&](const ProjectSoundPort& port) { return port.nodeId == nodeId; }))
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "The replaceable input is one of the Sound's own Sounds with an Audio output, other than its output");
    }
    const auto previous = std::ranges::find_if(definition->inputs, &ProjectSoundPort::replacesSound);
    std::string portId = previous == definition->inputs.end() ? std::string{} : previous->portId;
    if (previous != definition->inputs.end())
    {
        if (previous->nodeId == nodeId)
            return success("That Sound is already the replaceable input");
        definition->inputs.erase(previous);
    }
    if (portId.empty())
    {
        portId = "replaceableInput";
        for (int suffix = 2; std::ranges::find(definition->inputs, portId, &ProjectSoundPort::portId) != definition->inputs.end(); ++suffix)
            portId = "replaceableInput" + std::to_string(suffix);
    }
    ProjectSoundPort port;
    port.portId = std::move(portId);
    port.type = talas::core::PortType::Audio;
    port.nodeId = std::string{nodeId};
    port.nodePortId = std::move(outputPortId);
    port.replacesSound = true;
    definition->inputs.insert(definition->inputs.begin(), std::move(port));
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::arrangeNodes(
    std::optional<std::string_view> definitionId,
    std::span<const ProjectNodePresentation> positions)
{
    ProjectDocument candidate = document_;
    reconcileProjectPresentation(candidate);
    const auto* graph = graphForEditScope(candidate, definitionId);
    if (graph == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + std::string{definitionId.value_or(std::string_view{})});
    }
    auto moved = false;
    for (const auto& placed : positions)
    {
        auto* presented = findNode(*graph, placed.nodeId) == nullptr
            ? nullptr
            : findProjectNodePresentation(candidate, definitionId, placed.nodeId);
        if (presented == nullptr)
            return failure(EditError::UnknownNodeId, "Unknown node instance: " + placed.nodeId);
        moved = moved || presented->position != placed.position;
        presented->position = placed.position;
    }
    if (!moved)
        return success();
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::pasteGraphFragment(
    std::optional<std::string_view> definitionId,
    const ProjectGraphFragment& fragment,
    CanvasPosition origin,
    std::vector<std::string>& pastedNodeIds)
{
    ProjectDocument candidate = document_;
    std::vector<std::string> pasted;
    const auto result = pasteGraphFragmentInto(
        candidate, definitionId, fragment, origin, pasted, registry_);
    pastedNodeIds.clear();
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    pastedNodeIds = std::move(pasted);
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::removeNodes(
    std::span<const std::string> nodeIds)
{
    ProjectDocument candidate = document_;
    const auto result = removeGraphSelection(
        candidate, std::nullopt, nodeIds);
    if (!result.ok())
        return result;
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::moveNodes(
    std::span<const std::string> nodeIds,
    CanvasPosition delta,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Graph interaction transaction is no longer authoritative");
    }
    if (delta.x == 0 && delta.y == 0)
        return success();
    ProjectDocument candidate = document_;
    const auto result = moveGraphSelection(
        candidate, std::nullopt, nodeIds, delta);
    if (!result.ok())
        return result;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::moveNode(
    std::string_view nodeId,
    CanvasPosition position,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Graph interaction transaction is no longer authoritative");
    }
    const auto* existingNode = findNode(document_, nodeId);
    if (existingNode == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    const auto* presented = findProjectNodePresentation(
        document_,
        std::nullopt,
        nodeId);
    if (presented != nullptr && presented->position == position)
        return success();

    ProjectDocument candidate = document_;
    reconcileProjectPresentation(candidate);
    findProjectNodePresentation(candidate, std::nullopt, nodeId)->position =
        position;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::connect(talas::core::Connection connection)
{
    const auto* fromNode = findNode(document_, connection.fromNode);
    if (fromNode == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown source node: " + connection.fromNode);
    const auto* toNode = findNode(document_, connection.toNode);
    if (toNode == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown target node: " + connection.toNode);
    if (const auto unavailable =
            unresolvedComponentEditFailure(*fromNode, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    if (const auto unavailable =
            unresolvedComponentEditFailure(*toNode, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }

    const auto fromDescriptor =
        describeProjectNode(document_, *fromNode, registry_);
    const auto toDescriptor =
        describeProjectNode(document_, *toNode, registry_);
    if (!fromDescriptor.has_value() || !toDescriptor.has_value())
        return failure(EditError::UnknownNodeType, "Connection references a node with an unknown type");

    const auto* output =
        findPort(fromDescriptor->outputs, connection.fromPort);
    if (output == nullptr)
        return failure(EditError::UnknownPort, "Unknown output port: " + connection.fromPort);
    const auto* input =
        findPort(toDescriptor->inputs, connection.toPort);
    if (input == nullptr)
        return failure(EditError::UnknownPort, "Unknown input port: " + connection.toPort);
    if (!talas::core::portDescriptorsCompatible(*output, *input))
        return failure(
            EditError::IncompatiblePortTypes,
            output->type == input->type
                ? "Connection spectral formats do not match"
                : "Connection port types do not match");

    const bool destinationConnected = std::ranges::any_of(document_.graph.connections,
                                                           [&](const talas::core::Connection& existing) {
        return existing.toNode == connection.toNode && existing.toPort == connection.toPort;
    });
    if (destinationConnected && !input->acceptsMany)
        return failure(EditError::DuplicateInputConnection, "Input port already has a connection");
    if (std::ranges::find(document_.graph.connections, connection)
        != document_.graph.connections.end())
    {
        return failure(EditError::DuplicateInputConnection, "The connection already exists");
    }

    ProjectDocument candidate = document_;
    candidate.graph.connections.push_back(std::move(connection));
    if (containsCycle(candidate, registry_))
        return failure(EditError::InvalidGraph, "Connection would create a direct graph cycle");

    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;

    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::disconnect(std::string_view toNode, std::string_view toPort)
{
    const auto* node = findNode(document_, toNode);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown target node: " + std::string(toNode));

    const auto descriptor =
        describeProjectNodeForEditing(document_, *node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Target node has an unknown type: " + node->typeId);
    const auto* input = findPort(descriptor->inputs, toPort);
    if (input == nullptr)
        return failure(EditError::UnknownPort, "Unknown input port: " + std::string(toPort));

    const bool hasConnection = std::ranges::any_of(document_.graph.connections, [&](const talas::core::Connection& connection) {
        return connection.toNode == toNode && connection.toPort == toPort;
    });
    if (!hasConnection)
        return success();

    ProjectDocument candidate = document_;
    removeInputConnections(candidate.graph.connections, *input, toNode);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::disconnectPreservedComponent(
    std::string_view nodeId)
{
    const auto* node = findNode(document_, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (!node->preservedComponent.has_value())
        return failure(EditError::InvalidSelection, "Node has no preserved component capsule");
    if (std::ranges::none_of(
            document_.graph.connections,
            [&](const auto& connection) {
                return connection.fromNode == nodeId
                    || connection.toNode == nodeId;
            }))
    {
        return success();
    }

    ProjectDocument candidate = document_;
    std::erase_if(candidate.graph.connections, [&](const auto& connection) {
        return connection.fromNode == nodeId
            || connection.toNode == nodeId;
    });
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setParameter(std::string_view nodeId, ProjectParameterExpression parameter)
{
    const auto* node = findNode(document_, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (const auto unavailable =
            unresolvedComponentEditFailure(*node, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }

    const auto descriptor =
        describeProjectNode(document_, *node, registry_);
    if (!descriptor.has_value())
        return failure(EditError::UnknownNodeType, "Node has an unknown type: " + node->typeId);
    const auto* descriptorParameter =
        findParameter(*descriptor, parameter.parameterId);
    if (descriptorParameter == nullptr)
        return failure(EditError::UnknownParameter, "Unknown parameter: " + parameter.parameterId);

    const auto valueResult = validateParameterValue(parameter, *descriptorParameter);
    if (!valueResult.ok())
        return valueResult;

    // A Frequency, Duration or Time field
    // takes a number with its units, 440 hz or 2 s; a number entered bare,
    // other than 0, is refused. Projects keep the bare numbers they hold.
    if (parameter.expression.has_value()
        && (descriptorParameter->domain == talas::expr::ValueDomain::Hertz
            || descriptorParameter->domain == talas::expr::ValueDomain::Seconds
            || descriptorParameter->domain == talas::expr::ValueDomain::Samples))
    {
        const auto parsed = talas::expr::parse(
            *parameter.expression, document_.expressionLanguage, document_.expressionDefinitions);
        if (parsed.ok() && parsed.expression->eventReferences().empty()
            && parsed.expression->variableReferences().empty() && parsed.expression->identifiers().empty())
        {
            // A number with units converts between domains (440 hz is 69
            // nn, 2 s many samples); a bare one is the same in each.
            talas::expr::EvaluationContext context;
            context.sampleRate = 48000.0;
            const auto scalar = talas::expr::evaluate(*parsed.expression, context, {talas::expr::ValueDomain::Scalar});
            const auto unconverted = [&](talas::expr::ValueDomain domain) {
                const auto in = talas::expr::evaluate(*parsed.expression, context, {domain});
                return !in.ok() || in.value.value == scalar.value.value;
            };
            if (scalar.ok() && scalar.value.value != 0.0 && unconverted(talas::expr::ValueDomain::Hertz)
                && unconverted(talas::expr::ValueDomain::NoteNumber) && unconverted(talas::expr::ValueDomain::Seconds)
                && unconverted(talas::expr::ValueDomain::Samples))
            {
                const auto frequency = descriptorParameter->domain == talas::expr::ValueDomain::Hertz;
                return failure(EditError::InvalidParameterValue,
                    parameter.parameterId
                        + (frequency ? " is a frequency: give its units, as " + *parameter.expression + " hz"
                                     : " is a duration: give its units, as " + *parameter.expression + " s"));
            }
        }
    }

    const auto expressionResult = validateAuthoredParameterExpression(
        document_,
        *node,
        parameter,
        *descriptorParameter,
        document_.expressionLanguage);
    if (!expressionResult.ok())
        return expressionResult;
    if (scriptCodeStringField(*node, parameter.parameterId) && parameter.text.has_value())
    {
        if (auto refused = codeStringFailure("Script", *parameter.text); refused.has_value())
            return *refused;
    }
    // A Sound instance override may be stored under an alias of its exposed
    // parameter; it is the same override, not a second one.
    std::vector<std::string_view> overrideIds{parameter.parameterId};
    if (node->soundDefinitionId.has_value())
    {
        if (const auto* definition =
                findSoundDefinition(document_, *node->soundDefinitionId);
            definition != nullptr)
        {
            const auto exposed = std::ranges::find(
                definition->exposedParameters,
                parameter.parameterId,
                &ProjectSoundExposedParameter::parameterId);
            if (exposed != definition->exposedParameters.end())
            {
                overrideIds.insert(
                    overrideIds.end(),
                    exposed->aliases.begin(),
                    exposed->aliases.end());
                // The Class Editor's range: a constant value
                // outside it is refused; an Event Value plays as it comes.
                if ((exposed->minimum.has_value() || exposed->maximum.has_value())
                    && parameter.expression.has_value())
                {
                    const auto parsed = talas::expr::parse(
                        *parameter.expression, document_.expressionLanguage, document_.expressionDefinitions);
                    if (parsed.ok() && parsed.expression->eventReferences().empty()
                        && parsed.expression->variableReferences().empty()
                        && parsed.expression->identifiers().empty())
                    {
                        const auto evaluated = talas::expr::evaluate(*parsed.expression, {}, {exposed->domain});
                        if (evaluated.ok()
                            && ((exposed->minimum.has_value() && evaluated.value.value < *exposed->minimum)
                                || (exposed->maximum.has_value() && evaluated.value.value > *exposed->maximum)))
                        {
                            return failure(
                                EditError::InvalidParameterValue,
                                "The value lies outside " + exposed->displayName + "'s range");
                        }
                    }
                }
                if (auto refused = classFieldKindFailure(document_, *exposed, parameter); refused.has_value())
                    return *refused;
            }
        }
    }
    const auto sameOverride = [&](const ProjectParameterExpression& existing) {
        return std::ranges::find(overrideIds, existing.parameterId)
            != overrideIds.end();
    };

    ProjectDocument candidate = document_;
    auto* candidateNode = findNode(candidate, nodeId);
    const auto parameterCount = static_cast<std::size_t>(
        std::ranges::count_if(candidateNode->parameters, sameOverride));
    if (parameterCount > 1)
        return failure(EditError::DuplicateParameter, "Node contains duplicate parameter: " + parameter.parameterId);

    if (parameterCount == 1)
    {
        const auto found = std::ranges::find_if(
            candidateNode->parameters, sameOverride);
        *found = std::move(parameter);
    }
    else
    {
        candidateNode->parameters.push_back(std::move(parameter));
    }

    if (node->typeId == "talas.parameterTransformer"
        && descriptorParameter->id == "transformation")
    {
        const auto structural =
            prepareAuthoredProjectParameterTransformers(
                candidate,
                registry_);
        if (!structural.ok())
        {
            if (!structural.diagnostics.empty())
            {
                return failure(
                    EditError::InvalidParameterValue,
                    structural.diagnostics.front().message);
            }
            return failure(
                structural.terminalFailure
                        == ProjectParameterTransformationDiagnosticCode::AllocationFailed
                    ? EditError::AllocationFailed
                    : EditError::InvalidParameterValue,
                "Parameter transformation structural validation exceeded fixed limits");
        }
    }

    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::repairPreservedComponent(
    std::string_view nodeId) const
{
    const auto* node = findNode(document_, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (!node->preservedComponent.has_value())
        return failure(EditError::InvalidSelection, "Node has no preserved component capsule");
    const auto availability = classifyProjectComponentAvailability(
        node->typeId,
        &*node->preservedComponent,
        registry_);
    return availability.availability
            == ProjectComponentAvailability::Resolved
        ? success(availability.message)
        : failure(EditError::UnresolvedComponent, availability.message);
}

EditResult ProjectEditor::addVirtualControl(
    VirtualControl control,
    CanvasPosition position)
{
    if (control.midiMapping.has_value()
        && control.midiMapping->sourceMode
            != MidiControlSourceMode::ExactInput)
    {
        return failure(
            EditError::InvalidVirtualControl,
            "New MIDI mappings must select an exact input; re-learn migrated legacy mappings");
    }
    const auto eventName = control.eventName;
    const auto domain = control.domain;
    ProjectDocument candidate = document_;
    (void)wakeDormantVirtualControl(candidate.dormantVirtualControls, control);
    candidate.virtualControls.push_back(std::move(control));
    const auto validation = validateVirtualControls(candidate.virtualControls);
    if (!validation.ok())
        return validation;

    reconcileProjectPresentation(candidate);
    findProjectPerformanceControlPresentation(
        candidate,
        eventName,
        domain)->position = position;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::moveVirtualControl(
    std::string_view eventName,
    CanvasPosition position)
{
    const auto control = std::ranges::find_if(
        document_.virtualControls,
        [&](const auto& candidate) {
            return candidate.eventName == eventName;
        });
    if (control == document_.virtualControls.end())
    {
        return failure(
            EditError::UnknownVirtualControl,
            "Unknown Event Value: " + std::string(eventName));
    }
    const auto* presented = findProjectPerformanceControlPresentation(
        document_,
        control->eventName,
        control->domain);
    if (presented != nullptr && presented->position == position)
        return success();

    ProjectDocument candidate = document_;
    reconcileProjectPresentation(candidate);
    findProjectPerformanceControlPresentation(
        candidate,
        control->eventName,
        control->domain)->position = position;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlVisible(
    std::string_view eventName,
    bool visible)
{
    const auto control = std::ranges::find_if(
        document_.virtualControls,
        [&](const auto& candidate) {
            return candidate.eventName == eventName;
        });
    if (control == document_.virtualControls.end())
    {
        return failure(
            EditError::UnknownVirtualControl,
            "Unknown Event Value: " + std::string(eventName));
    }
    const auto* presented = findProjectPerformanceControlPresentation(
        document_,
        control->eventName,
        control->domain);
    if (presented != nullptr && presented->visible == visible)
        return success();

    ProjectDocument candidate = document_;
    reconcileProjectPresentation(candidate);
    findProjectPerformanceControlPresentation(
        candidate,
        control->eventName,
        control->domain)->visible = visible;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addPerformanceControl(
    SnapshotTargetIdentity target,
    ProjectPerformanceControlKind kind,
    std::string displayName,
    CanvasPosition position)
{
    if (!isKnownPerformanceControlKind(kind)
        || displayName.empty()
        || displayName.size() > maximumPerformanceDisplayNameBytes
        || !isProjectPerformanceTargetAvailable(document_, target))
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance control requires an available target, known kind, "
            "and bounded non-empty display name");
    }
    if (findProjectPerformanceControlPresentation(document_, target) != nullptr
        || document_.presentation.performanceControls.size()
            >= maximumPerformanceControlCount)
    {
        return failure(EditError::DuplicatePerformancePresentation,
            "Performance target is already presented or the panel is full");
    }

    ProjectDocument candidate = document_;
    ProjectPerformanceControlPresentation item;
    item.target = std::move(target);
    item.kind = kind;
    item.displayName = std::move(displayName);
    item.position = position;
    candidate.presentation.performanceControls.push_back(std::move(item));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removePerformanceControl(std::string_view controlId)
{
    const auto* item = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (item == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    if (item->target.kind == SnapshotTargetKind::EventValue)
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Event Value Performance controls are hidden or removed with "
            "their Event Value authority");
    }
    ProjectDocument candidate = document_;
    std::erase_if(candidate.presentation.performanceControls,
        [&](const auto& control) {
            return control.controlId == controlId;
        });
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::movePerformanceControl(
    std::string_view controlId,
    CanvasPosition position,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Performance layout interaction transaction is no longer authoritative");
    }
    const auto* current = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (current == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    if (current->position == position)
        return success();
    ProjectDocument candidate = document_;
    findProjectPerformanceControlPresentationById(candidate, controlId)
        ->position = position;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::setPerformanceControlVisible(
    std::string_view controlId,
    bool visible)
{
    const auto* current = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (current == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    if (current->visible == visible)
        return success();
    ProjectDocument candidate = document_;
    findProjectPerformanceControlPresentationById(candidate, controlId)
        ->visible = visible;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setPerformanceControlKind(
    std::string_view controlId,
    ProjectPerformanceControlKind kind)
{
    const auto* current = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (current == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    if (!isKnownPerformanceControlKind(kind))
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Unknown Performance control kind");
    }
    if (current->kind == kind)
        return success();
    ProjectDocument candidate = document_;
    auto* item = findProjectPerformanceControlPresentationById(
        candidate, controlId);
    item->kind = kind;
    if (kind != ProjectPerformanceControlKind::Macro)
        item->modulationSlots.clear();
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setPerformanceControlDisplayName(
    std::string_view controlId,
    std::string displayName)
{
    const auto* current = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (current == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    if (displayName.empty()
        || displayName.size() > maximumPerformanceDisplayNameBytes)
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance display name must be bounded and non-empty");
    }
    if (current->displayName == displayName)
        return success();
    ProjectDocument candidate = document_;
    findProjectPerformanceControlPresentationById(candidate, controlId)
        ->displayName = std::move(displayName);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setPerformanceControlModulationSlots(
    std::string_view controlId,
    std::vector<ProjectPerformanceModulationSlot> slots)
{
    const auto* current = findProjectPerformanceControlPresentationById(
        document_, controlId);
    if (current == nullptr)
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance control: " + std::string(controlId));
    }
    const auto validation = validatePerformanceSlots(
        document_, current->target, current->kind, slots);
    if (!validation.ok())
        return validation;
    std::size_t totalSlots = slots.size();
    for (const auto& item : document_.presentation.performanceControls)
    {
        if (item.controlId != controlId)
            totalSlots += item.modulationSlots.size();
    }
    if (totalSlots > maximumPerformanceModulationSlotCount)
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance panel exceeds the total modulation-slot limit");
    }
    if (current->modulationSlots == slots)
        return success();
    ProjectDocument candidate = document_;
    findProjectPerformanceControlPresentationById(candidate, controlId)
        ->modulationSlots = std::move(slots);
    if (projectPerformancePresentationHasCycle(candidate))
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance macro routing cannot contain a cycle");
    }
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addPerformanceMeter(
    ProjectPerformanceMeterPresentation meter)
{
    if (!isValidProjectPerformanceIdentifier(meter.meterId)
        || meter.outputChannel == 0)
    {
        return failure(EditError::InvalidPerformancePresentation,
            "Performance meter requires a bounded identifier and positive "
            "master output channel");
    }
    if (document_.presentation.performanceMeters.size()
            >= maximumPerformanceMeterCount
        || std::ranges::any_of(
            document_.presentation.performanceMeters,
            [&](const auto& current) {
                return current.meterId == meter.meterId;
            }))
    {
        return failure(EditError::DuplicatePerformancePresentation,
            "Performance meter already exists or the panel is full");
    }
    ProjectDocument candidate = document_;
    candidate.presentation.performanceMeters.push_back(std::move(meter));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removePerformanceMeter(std::string_view meterId)
{
    if (!std::ranges::any_of(document_.presentation.performanceMeters,
            [&](const auto& meter) {
                return meter.meterId == meterId;
            }))
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance meter: " + std::string(meterId));
    }
    ProjectDocument candidate = document_;
    std::erase_if(candidate.presentation.performanceMeters,
        [&](const auto& meter) {
            return meter.meterId == meterId;
        });
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::movePerformanceMeter(
    std::string_view meterId,
    CanvasPosition position,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Performance layout interaction transaction is no longer authoritative");
    }
    const auto current = std::ranges::find(
        document_.presentation.performanceMeters,
        meterId,
        &ProjectPerformanceMeterPresentation::meterId);
    if (current == document_.presentation.performanceMeters.end())
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance meter: " + std::string(meterId));
    }
    if (current->position == position)
        return success();
    ProjectDocument candidate = document_;
    std::ranges::find(candidate.presentation.performanceMeters,
        meterId,
        &ProjectPerformanceMeterPresentation::meterId)->position = position;
    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::setPerformanceMeterVisible(
    std::string_view meterId,
    bool visible)
{
    const auto current = std::ranges::find(
        document_.presentation.performanceMeters,
        meterId,
        &ProjectPerformanceMeterPresentation::meterId);
    if (current == document_.presentation.performanceMeters.end())
    {
        return failure(EditError::UnknownPerformancePresentation,
            "Unknown Performance meter: " + std::string(meterId));
    }
    if (current->visible == visible)
        return success();
    ProjectDocument candidate = document_;
    std::ranges::find(candidate.presentation.performanceMeters,
        meterId,
        &ProjectPerformanceMeterPresentation::meterId)->visible = visible;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlValue(std::string_view eventName, double value)
{
    PreparedVirtualControlValueEdit prepared;
    const auto result = prepareVirtualControlValue(
        eventName, value, prepared);
    if (!result.ok())
        return result;
    commitVirtualControlValue(std::move(prepared));
    return success();
}

EditResult ProjectEditor::prepareVirtualControlValue(
    std::string_view eventName,
    double value,
    PreparedVirtualControlValueEdit& prepared)
{
    const auto found = std::ranges::find_if(document_.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
    if (found == document_.virtualControls.end())
        return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + std::string(eventName));
    if (!std::isfinite(value) || value < found->minimum || value > found->maximum)
        return failure(EditError::InvalidVirtualControl, "Event Value must be finite and within its persisted range");
    prepared = {};
    if (found->value == value)
        return success();

    // Event Value edits join an unowned or value transaction, and isolate a
    // gesture that records only its owner's edits.
    const auto isolatesGesture = transactionOwner_ != 0 && !valueTransaction_;
    if (isolatesGesture
        && undoHistory_.size() < historyLimit
        && undoHistory_.capacity() < undoHistory_.size() + 1)
    {
        try
        {
            undoHistory_.reserve(undoHistory_.size() + 1);
        }
        catch (const std::bad_alloc&)
        {
            return failure(
                EditError::AllocationFailed,
                "Could not isolate the active graph gesture in project history");
        }
        catch (const std::length_error&)
        {
            return failure(
                EditError::AllocationFailed,
                "Graph gesture history exceeds supported capacity");
        }
    }
    if (isolatesGesture)
        closeHistoryTransaction();

    prepared.document_ = document_;
    const auto candidateControl = std::ranges::find_if(
        prepared.document_.virtualControls,
        [&](const VirtualControl& control) {
            return control.eventName == eventName;
        });
    candidateControl->value = value;
    prepared.eventName_ = std::string{eventName};
    prepared.value_ = value;
    reconcileProjectPresentation(prepared.document_);
    if (!transactionBefore_.has_value()
        && undoHistory_.size() < historyLimit
        && undoHistory_.capacity() < undoHistory_.size() + 1)
    {
        undoHistory_.reserve(undoHistory_.size() + 1);
    }
    prepared.changesDocument_ = true;
    return success();
}

void ProjectEditor::commitVirtualControlValue(
    PreparedVirtualControlValueEdit prepared) noexcept
{
    static_assert(std::is_nothrow_move_constructible_v<ProjectDocument>);
    static_assert(std::is_nothrow_move_assignable_v<ProjectDocument>);
    if (!prepared.changesDocument_)
        return;
    if (transactionBefore_.has_value())
    {
        advanceDocumentAuthority();
        if (transactionOwner_ != 0 && !valueTransaction_)
        {
            const auto mergePreparedValues = [&](auto& controls) {
                const auto control = std::ranges::find(
                    controls,
                    prepared.eventName_,
                    &VirtualControl::eventName);
                if (control != controls.end())
                {
                    control->value = prepared.value_;
                }
            };
            mergePreparedValues(transactionBefore_->virtualControls);
            mergePreparedValues(document_.virtualControls);
            transactionChanged_ = true;
            return;
        }
        document_ = std::move(prepared.document_);
        transactionChanged_ = true;
        return;
    }

    if (undoHistory_.size() == historyLimit)
        undoHistory_.erase(undoHistory_.begin());
    undoHistory_.push_back(std::move(document_));
    pushUndoWindow(historyWindow_);
    advanceDocumentAuthority();
    document_ = std::move(prepared.document_);
    clearRedoHistory();
}

EditResult ProjectEditor::setVirtualControlMidiMapping(std::string_view eventName,
                                                        std::optional<MidiControlMapping> mapping)
{
    const auto found = std::ranges::find_if(document_.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
    if (found == document_.virtualControls.end())
        return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + std::string(eventName));

    if (mapping.has_value())
    {
        return learnVirtualControlMidiMapping(
            eventName,
            *mapping,
            MidiMappingConflictPolicy::Reject);
    }

    ProjectDocument candidate = document_;
    const auto candidateControl = std::ranges::find_if(candidate.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
    if (!candidateControl->midiMapping.has_value())
        return success();
    candidateControl->midiMapping.reset();

    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setGlobalMap(std::string text)
{
    // The Default map's own text is the Default map.
    if (text == defaultGlobalMapText())
        text.clear();
    if (!text.empty())
    {
        const auto parsed = parseProjectGlobalMap(text);
        if (!parsed.ok())
        {
            return failure(
                EditError::InvalidVirtualControl,
                "Global map line " + std::to_string(parsed.line) + ": " + parsed.error);
        }
    }
    if (document_.globalMap == text)
        return success();
    ProjectDocument candidate = document_;
    candidate.globalMap = std::move(text);
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::setFieldReadingSettings(talas::expr::RoundingRule rounding,
    std::optional<double> fieldBeatTempoBpm, talas::expr::UnitlessSOSPitch unitlessSOSPitch)
{
    if (fieldBeatTempoBpm.has_value()
        && !(std::isfinite(*fieldBeatTempoBpm) && *fieldBeatTempoBpm >= talas::expr::minimumFieldBeatTempoBpm
            && *fieldBeatTempoBpm <= talas::expr::maximumFieldBeatTempoBpm))
    {
        return failure(EditError::InvalidParameterValue, "A beat's tempo for fields is from 1 to 1000");
    }
    auto& definitions = document_.expressionDefinitions;
    if (definitions.rounding == rounding && definitions.fieldBeatTempoBpm == fieldBeatTempoBpm
        && definitions.unitlessSOSPitch == unitlessSOSPitch)
    {
        return success();
    }
    ProjectDocument candidate = document_;
    candidate.expressionDefinitions.rounding = rounding;
    candidate.expressionDefinitions.fieldBeatTempoBpm = fieldBeatTempoBpm;
    candidate.expressionDefinitions.unitlessSOSPitch = unitlessSOSPitch;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlWidget(
    std::string_view eventName,
    VirtualControlWidget widget)
{
    ProjectDocument candidate = document_;
    const auto control = std::ranges::find(
        candidate.virtualControls, eventName, &VirtualControl::eventName);
    if (control == candidate.virtualControls.end())
        return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + std::string(eventName));
    if (!isValidVirtualControlWidget(widget, control->minimum, control->maximum))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "A widget's label and tab have no control characters, its grid lies from 0 to its range, and a log taper needs a minimum above 0");
    }
    if (control->widget == widget)
        return success();
    control->widget = std::move(widget);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlLayoutPages(std::vector<VirtualControlLayoutPage> pages)
{
    if (pages.size() > 256
        || std::ranges::any_of(pages, [](const VirtualControlLayoutPage& page) {
               return page.name.empty() || page.name.size() > maximumVirtualControlLabelBytes;
           }))
        return failure(EditError::InvalidVirtualControl, "A layout needs a name of at most 128 bytes");
    if (document_.virtualControlLayoutPages == pages)
        return success();
    ProjectDocument candidate = document_;
    candidate.virtualControlLayoutPages = std::move(pages);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlLayoutSettings(
    std::vector<VirtualControlLayoutSettings> settings, std::optional<std::vector<VirtualControlLayoutPage>> pages)
{
    if (settings.size() > maximumVirtualControlLayoutSettings
        || !std::ranges::all_of(settings, [](const auto& layout) { return isValidVirtualControlLayoutSettings(layout); }))
        return failure(EditError::InvalidVirtualControl,
            "A layout's own widgets need a name of at most 128 bytes and, for each Event Value once, a range and a widget that suits it");
    for (std::size_t index = 0; index < settings.size(); ++index)
    {
        const auto& layout = settings[index];
        if (std::ranges::any_of(settings.begin(), settings.begin() + static_cast<long>(index),
                [&](const VirtualControlLayoutSettings& earlier) { return earlier.layout == layout.layout; }))
            return failure(EditError::InvalidVirtualControl, "Two layouts keep their own widgets under one name: " + layout.layout);
        if (!layout.presetBankId.empty()
            && std::ranges::none_of(document_.snapshotLibrary.banks,
                [&](const ProjectSnapshotBank& bank) { return bank.bankId == layout.presetBankId; }))
            return failure(EditError::UnknownSnapshotBank, "Unknown snapshot bank: " + layout.presetBankId);
    }
    ProjectDocument candidate = document_;
    if (pages.has_value())
    {
        if (pages->size() > 256 || std::ranges::any_of(*pages, [](const VirtualControlLayoutPage& page) {
                return page.name.empty() || page.name.size() > maximumVirtualControlLabelBytes;
            }))
            return failure(EditError::InvalidVirtualControl, "A layout needs a name of at most 128 bytes");
        candidate.virtualControlLayoutPages = std::move(*pages);
    }
    candidate.virtualControlLayoutSettings = std::move(settings);
    if (candidate.virtualControlLayoutSettings == document_.virtualControlLayoutSettings
        && candidate.virtualControlLayoutPages == document_.virtualControlLayoutPages)
        return success();
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::pasteVirtualControlWidgets(std::vector<VirtualControl> pasted, std::string_view layoutPage)
{
    if (pasted.empty())
        return failure(EditError::InvalidVirtualControl, "There are no widgets to paste");
    ProjectDocument candidate = document_;
    const auto page = layoutPage.empty()
        ? candidate.virtualControlLayoutPages.end()
        : std::ranges::find(candidate.virtualControlLayoutPages, layoutPage, &VirtualControlLayoutPage::name);
    auto placed = false;
    for (const auto& widget : pasted)
    {
        const auto control = std::ranges::find(candidate.virtualControls, widget.eventName, &VirtualControl::eventName);
        if (control != candidate.virtualControls.end())
        {
            if (!takeVirtualControlWidget(*control, widget))
                return failure(EditError::InvalidVirtualControl, "The widget pasted for " + widget.eventName + " does not suit its range");
            placed = placed || control->widget.bounds.has_value();
            if (page != candidate.virtualControlLayoutPages.end() && std::ranges::find(page->eventNames, widget.eventName) == page->eventNames.end())
                page->eventNames.push_back(widget.eventName);
            continue;
        }
        // Only the widget sleeps: its type, place, size and range.
        VirtualControl dormant;
        dormant.eventName = widget.eventName;
        dormant.domain = widget.domain;
        dormant.kind = widget.kind;
        dormant.value = widget.value;
        if (!takeVirtualControlWidget(dormant, widget))
            return failure(EditError::InvalidVirtualControl, "The widget pasted for " + widget.eventName + " does not suit its range");
        const auto existing = std::ranges::find(candidate.dormantVirtualControls, widget.eventName, &VirtualControl::eventName);
        if (existing != candidate.dormantVirtualControls.end())
            *existing = std::move(dormant);
        else
            candidate.dormantVirtualControls.push_back(std::move(dormant));
    }
    if (const auto dormant = validateDormantVirtualControls(candidate); !dormant.ok())
        return dormant;
    if (placed && candidate.virtualControlLayout == VirtualControlLayout::Automatic)
        candidate.virtualControlLayout = VirtualControlLayout::Fixed;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlDecorations(std::vector<VirtualControlDecoration> decorations)
{
    if (decorations.size() > maximumVirtualControlDecorations
        || !std::ranges::all_of(decorations, [](const auto& decoration) { return isValidVirtualControlDecoration(decoration); }))
    {
        return failure(EditError::InvalidVirtualControl,
            "A surface holds at most 64 texts and boxes, each on the surface and of some size");
    }
    if (decorations == document_.virtualControlDecorations)
        return success();
    ProjectDocument candidate = document_;
    candidate.virtualControlDecorations = std::move(decorations);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::arrangeVirtualControls(
    VirtualControlLayout layout,
    const std::vector<std::pair<std::string, std::optional<VirtualControlBounds>>>& places)
{
    ProjectDocument candidate = document_;
    bool changed = candidate.virtualControlLayout != layout;
    for (const auto& [eventName, bounds] : places)
    {
        const auto control = std::ranges::find(candidate.virtualControls, eventName, &VirtualControl::eventName);
        if (control == candidate.virtualControls.end())
            return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + eventName);
        auto widget = control->widget;
        widget.bounds = bounds;
        if (!isValidVirtualControlWidget(widget, control->minimum, control->maximum))
            return failure(EditError::InvalidVirtualControl, "A widget's place must lie on the surface and have a size");
        changed = changed || widget != control->widget;
        control->widget = std::move(widget);
    }
    candidate.virtualControlLayout = layout;
    if (!changed)
        return success();
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlRange(
    std::string_view eventName,
    double minimum,
    double maximum)
{
    ProjectDocument candidate = document_;
    const auto control = std::ranges::find(
        candidate.virtualControls, eventName, &VirtualControl::eventName);
    if (control == candidate.virtualControls.end())
        return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + std::string(eventName));
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !(minimum < maximum)
        || !isValidVirtualControlWidget(control->widget, minimum, maximum))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "A control's minimum lies below its maximum, within its widget's grid and taper");
    }
    if (control->minimum == minimum && control->maximum == maximum)
        return success();
    control->minimum = minimum;
    control->maximum = maximum;
    control->value = std::clamp(control->value, minimum, maximum);
    const auto validation = validateVirtualControls(candidate.virtualControls);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlOscMapping(
    std::string_view eventName,
    std::optional<OscControlMapping> mapping)
{
    const auto found = std::ranges::find_if(
        document_.virtualControls,
        [&](const VirtualControl& control) {
            return control.eventName == eventName;
        });
    if (found == document_.virtualControls.end())
    {
        return failure(
            EditError::UnknownVirtualControl,
            "Unknown Event Value: " + std::string(eventName));
    }

    ProjectDocument candidate = document_;
    const auto candidateControl = std::ranges::find_if(
        candidate.virtualControls,
        [&](const VirtualControl& control) {
            return control.eventName == eventName;
        });
    if (candidateControl->oscMapping == mapping)
        return success();
    candidateControl->oscMapping = std::move(mapping);
    const auto validation = validateVirtualControls(candidate.virtualControls);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setVirtualControlOscEgressMapping(
    std::string_view eventName,
    std::optional<OscEgressMapping> mapping)
{
    const auto found = std::ranges::find_if(
        document_.virtualControls,
        [&](const VirtualControl& control) {
            return control.eventName == eventName;
        });
    if (found == document_.virtualControls.end())
    {
        return failure(
            EditError::UnknownVirtualControl,
            "Unknown Event Value: " + std::string(eventName));
    }

    ProjectDocument candidate = document_;
    const auto candidateControl = std::ranges::find_if(
        candidate.virtualControls,
        [&](const VirtualControl& control) {
            return control.eventName == eventName;
        });
    if (candidateControl->oscEgressMapping == mapping)
        return success();
    candidateControl->oscEgressMapping = std::move(mapping);
    const auto validation = validateVirtualControls(candidate.virtualControls);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::learnVirtualControlMidiMapping(
    std::string_view eventName,
    MidiControlMapping mapping,
    MidiMappingConflictPolicy conflictPolicy)
{
    if (mapping.sourceMode != MidiControlSourceMode::ExactInput)
    {
        return failure(
            EditError::InvalidVirtualControl,
            "New MIDI mappings must select an exact input; re-learn migrated legacy mappings");
    }
    if (conflictPolicy != MidiMappingConflictPolicy::Reject
        && conflictPolicy
            != MidiMappingConflictPolicy::ReplaceExisting)
    {
        return failure(
            EditError::DuplicateMidiMapping,
            "Unknown MIDI mapping conflict policy");
    }
    const auto plan = planVirtualControlMidiLearn(
        document_, eventName, mapping);
    switch (plan.disposition)
    {
    case MidiLearnDisposition::InvalidSource:
        return failure(
            EditError::InvalidVirtualControl,
            "MIDI mapping must use channel 1...16, controller 0...127, and a valid exact input identifier");
    case MidiLearnDisposition::UnknownTarget:
        return failure(
            EditError::UnknownVirtualControl,
            "Unknown Event Value MIDI target: " + std::string(eventName));
    case MidiLearnDisposition::AlreadyMapped:
        return success(
            "MIDI channel " + std::to_string(mapping.channel)
            + " CC " + std::to_string(mapping.controller)
            + " is already mapped to " + plan.requestedEventName);
    case MidiLearnDisposition::Conflict:
        if (conflictPolicy == MidiMappingConflictPolicy::Reject)
        {
            return failure(
                EditError::DuplicateMidiMapping,
                "MIDI channel " + std::to_string(mapping.channel)
                + " CC " + std::to_string(mapping.controller)
                + " is mapped to " + plan.conflictingEventName
                + "; mapping for " + plan.requestedEventName
                + " is unchanged");
        }
        break;
    case MidiLearnDisposition::Ready:
        break;
    }

    ProjectDocument candidate = document_;
    if (plan.disposition == MidiLearnDisposition::Conflict)
    {
        const auto conflicting = std::ranges::find_if(
            candidate.virtualControls,
            [&](const auto& control) {
                return control.eventName == plan.conflictingEventName;
            });
        if (conflicting == candidate.virtualControls.end()
            || !conflicting->midiMapping.has_value())
        {
            return failure(
                EditError::DuplicateMidiMapping,
                "MIDI mapping conflict changed before replacement");
        }
        conflicting->midiMapping.reset();
    }

    const auto requested = std::ranges::find_if(
        candidate.virtualControls,
        [&](const auto& control) {
            return control.eventName == plan.requestedEventName;
        });
    if (requested == candidate.virtualControls.end())
    {
        return failure(
            EditError::UnknownVirtualControl,
            "Event Value MIDI target changed before replacement");
    }
    requested->midiMapping = mapping;
    const auto validation = validateVirtualControls(candidate.virtualControls);
    if (!validation.ok())
        return validation;

    commit(std::move(candidate));
    return success(
        "MIDI channel " + std::to_string(mapping.channel)
        + " CC " + std::to_string(mapping.controller)
        + " mapped to " + plan.requestedEventName);
}

EditResult ProjectEditor::removeVirtualControl(std::string_view eventName)
{
    const auto found = std::ranges::find_if(document_.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
    if (found == document_.virtualControls.end())
        return failure(EditError::UnknownVirtualControl, "Unknown Event Value: " + std::string(eventName));
    if (projectReferencesEventValue(document_, registry_, eventName))
    {
        return failure(
            EditError::InvalidVirtualControl,
            "Cannot remove an Event Value control while the graph references it");
    }

    ProjectDocument candidate = document_;
    std::erase_if(candidate.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setResourceParameterAsset(std::string_view nodeId,
                                                     std::string_view parameterId,
                                                     ProjectAsset asset)
{
    const auto assetValidation = validateAsset(asset);
    if (!assetValidation.ok())
        return assetValidation;

    const auto* node = findNode(document_, nodeId);
    if (node == nullptr)
        return failure(EditError::UnknownNodeId, "Unknown node instance: " + std::string(nodeId));
    if (const auto unavailable =
            unresolvedComponentEditFailure(*node, registry_);
        unavailable.has_value())
    {
        return *unavailable;
    }
    const auto descriptor =
        describeProjectNode(document_, *node, registry_);
    if (!descriptor.has_value())
    {
        return failure(
            EditError::UnknownNodeType,
            "Node has an unknown built-in or Sound type");
    }
    const auto* parameterDescriptor =
        findParameter(*descriptor, parameterId);
    if (parameterDescriptor == nullptr)
        return failure(EditError::UnknownParameter, "Unknown parameter: " + std::string(parameterId));
    if (parameterDescriptor->kind != talas::core::ParameterKind::Resource)
        return failure(EditError::InvalidParameterValue, "Project assets can only bind to Resource parameters");

    const auto existingAsset = std::ranges::find_if(document_.assets, [&](const ProjectAsset& candidate) {
        return candidate.id == asset.id;
    });
    const auto existingParameter = std::ranges::find_if(node->parameters,
                                                        [&](const ProjectParameterExpression& parameter) {
        return parameter.parameterId == parameterId;
    });
    if (existingAsset != document_.assets.end()
        && *existingAsset == asset
        && existingParameter != node->parameters.end()
        && existingParameter->resource == std::optional<std::string>{asset.id})
    {
        return success();
    }

    ProjectDocument candidate = document_;
    const auto candidateAsset = std::ranges::find_if(candidate.assets, [&](const ProjectAsset& value) {
        return value.id == asset.id;
    });
    if (candidateAsset == candidate.assets.end())
        candidate.assets.push_back(asset);
    else if (*candidateAsset != asset)
    {
        return failure(
            EditError::InvalidAsset,
            "Existing asset manifests must use explicit relocation or replacement");
    }

    auto* candidateNode = findNode(candidate, nodeId);
    const auto parameterCount = static_cast<std::size_t>(std::ranges::count_if(
        candidateNode->parameters,
        [&](const ProjectParameterExpression& parameter) {
            return parameter.parameterId == parameterId;
        }));
    if (parameterCount > 1)
        return failure(EditError::DuplicateParameter, "Node contains duplicate parameter: " + std::string(parameterId));

    ProjectParameterExpression parameter;
    parameter.parameterId = std::string(parameterId);
    parameter.resource = asset.id;
    if (parameterCount == 1)
    {
        const auto found = std::ranges::find_if(candidateNode->parameters,
                                                [&](const ProjectParameterExpression& value) {
            return value.parameterId == parameterId;
        });
        *found = std::move(parameter);
    }
    else
    {
        candidateNode->parameters.push_back(std::move(parameter));
    }

    const auto assetsValidation = validateAssets(candidate.assets);
    if (!assetsValidation.ok())
        return assetsValidation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addAsset(ProjectAsset asset)
{
    const auto assetValidation = validateAsset(asset);
    if (!assetValidation.ok())
        return assetValidation;
    const auto existing = std::ranges::find(document_.assets, asset.id, &ProjectAsset::id);
    if (existing != document_.assets.end())
    {
        return *existing == asset
            ? success()
            : failure(EditError::InvalidAsset, "Existing asset manifests must use explicit relocation or replacement");
    }
    ProjectDocument candidate = document_;
    candidate.assets.push_back(std::move(asset));
    const auto assetsValidation = validateAssets(candidate.assets);
    if (!assetsValidation.ok())
        return assetsValidation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::relocateProjectAsset(
    ProjectAsset relocatedAsset)
{
    const auto existing = std::ranges::find(
        document_.assets,
        relocatedAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: " + relocatedAsset.id);
    }
    return applyProjectAssetResolutions({{
        ProjectAssetResolutionMode::Relocation,
        existing->generation,
        existing->contentHash,
        std::move(relocatedAsset),
    }});
}

EditResult ProjectEditor::acceptProjectAssetInspection(
    ProjectAsset inspectedAsset)
{
    const auto existing = std::ranges::find(
        document_.assets,
        inspectedAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: " + inspectedAsset.id);
    }
    if (!existing->contentHash.empty())
    {
        return inspectedAsset == *existing
            ? success()
            : failure(
                EditError::InvalidAsset,
                "Verified assets require explicit relocation or replacement");
    }
    if (inspectedAsset.path != existing->path)
    {
        return failure(
            EditError::InvalidAsset,
            "Single-asset inspection cannot change the asset path");
    }
    return applyProjectAssetResolutions({{
        ProjectAssetResolutionMode::InitialInspection,
        existing->generation,
        existing->contentHash,
        std::move(inspectedAsset),
    }});
}

EditResult ProjectEditor::acceptProjectAssetReplacement(
    ProjectAsset replacementAsset)
{
    const auto existing = std::ranges::find(
        document_.assets,
        replacementAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: " + replacementAsset.id);
    }
    return applyProjectAssetResolutions({{
        ProjectAssetResolutionMode::Replacement,
        existing->generation,
        existing->contentHash,
        std::move(replacementAsset),
    }});
}

EditResult ProjectEditor::acceptProjectAssetDisposition(
    ProjectAsset dispositionAsset)
{
    const auto existing = std::ranges::find(
        document_.assets,
        dispositionAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: " + dispositionAsset.id);
    }
    return acceptProjectAssetDisposition(
        std::move(dispositionAsset),
        *existing);
}

EditResult ProjectEditor::acceptProjectAssetDisposition(
    ProjectAsset dispositionAsset,
    const ProjectAsset& expectedAsset)
{
    if (dispositionAsset.id != expectedAsset.id)
    {
        return failure(
            EditError::InvalidAsset,
            "Disposition candidate and expected asset identities differ");
    }
    const auto existing = std::ranges::find(
        document_.assets,
        dispositionAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: " + dispositionAsset.id);
    }
    if (*existing != expectedAsset)
    {
        return failure(
            EditError::StaleAssetDisposition,
            "Project asset changed after disposition preparation began");
    }
    const auto validation = validateAsset(dispositionAsset);
    if (!validation.ok())
        return validation;

    auto expected = *existing;
    expected.path = dispositionAsset.path;
    expected.disposition = dispositionAsset.disposition;
    if (dispositionAsset != expected)
    {
        return failure(
            EditError::InvalidAsset,
            "Asset disposition may change only path and disposition");
    }
    if (dispositionAsset == *existing)
        return success();

    auto candidate = document_;
    const auto candidateAsset = std::ranges::find(
        candidate.assets,
        dispositionAsset.id,
        &ProjectAsset::id);
    *candidateAsset = std::move(dispositionAsset);
    const auto projectValidation =
        projectValidationFailure(candidate, registry_);
    if (!projectValidation.ok())
        return projectValidation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::applyProjectAssetResolutions(
    std::vector<ProjectAssetResolution> resolutions)
{
    if (resolutions.empty())
        return success();

    std::unordered_set<std::string> assetIds;
    auto candidateDocument = document_;
    auto changed = false;
    for (auto& resolution : resolutions)
    {
        const auto& assetId = resolution.asset.id;
        if (!assetIds.insert(assetId).second)
        {
            return failure(
                EditError::DuplicateAsset,
                "Duplicate asset resolution: " + assetId);
        }

        const auto existing = std::ranges::find(
            document_.assets,
            assetId,
            &ProjectAsset::id);
        if (existing == document_.assets.end())
        {
            return failure(
                EditError::InvalidAsset,
                "Unknown project asset: " + assetId);
        }
        if (existing->generation != resolution.expectedGeneration
            || existing->contentHash
                != resolution.expectedContentHash)
        {
            return failure(
                EditError::InvalidAsset,
                "Asset resolution is stale: " + assetId);
        }

        const auto validation = validateAsset(resolution.asset);
        if (!validation.ok())
            return validation;

        auto applied = resolution.asset;
        switch (resolution.mode)
        {
        case ProjectAssetResolutionMode::Relocation:
        {
            auto expected = *existing;
            expected.path = resolution.asset.path;
            if (existing->contentHash.empty()
                || resolution.asset.contentHash.empty())
            {
                return failure(
                    EditError::InvalidAsset,
                    "Same-content relocation requires a verified SHA-256 hash");
            }
            if (resolution.asset != expected)
            {
                return failure(
                    EditError::InvalidAsset,
                    "Relocation may change only the path of the same verified generation");
            }
            break;
        }
        case ProjectAssetResolutionMode::InitialInspection:
        {
            if (!existing->contentHash.empty()
                || resolution.asset.contentHash.empty())
            {
                return failure(
                    EditError::InvalidAsset,
                    "Initial inspection requires one unverified asset and a verified candidate");
            }
            auto expected = *existing;
            expected.path = resolution.asset.path;
            expected.contentHash = resolution.asset.contentHash;
            if (existing->kind == ProjectAssetKind::Audio
                || existing->kind
                    == ProjectAssetKind::PartialSpectrum)
            {
                if ((existing->kind == ProjectAssetKind::Audio
                        && resolution.asset.channelCount == 0U)
                    || (existing->kind
                            == ProjectAssetKind::PartialSpectrum
                        && resolution.asset.channelCount != 0U)
                    || resolution.asset.sampleRate <= 0.0
                    || resolution.asset.frameCount == 0U
                    || resolution.asset.encoding.empty())
                {
                    return failure(
                        EditError::InvalidAsset,
                        "Initial media inspection requires a complete kind-specific shape");
                }
                if (expected.channelCount == 0U)
                    expected.channelCount = resolution.asset.channelCount;
                if (expected.sampleRate == 0.0)
                    expected.sampleRate = resolution.asset.sampleRate;
                if (expected.frameCount == 0U)
                    expected.frameCount = resolution.asset.frameCount;
                if (expected.encoding.empty())
                    expected.encoding = resolution.asset.encoding;
            }
            if (resolution.asset != expected)
            {
                return failure(
                    EditError::InvalidAsset,
                    "Initial inspection may change path and fill only unknown hash/media metadata");
            }
            break;
        }
        case ProjectAssetResolutionMode::Replacement:
            if (existing->contentHash.empty()
                || resolution.asset.contentHash.empty())
            {
                return failure(
                    EditError::InvalidAsset,
                    "Replacement requires an accepted and a candidate SHA-256 hash");
            }
            if (resolution.asset.kind != existing->kind
                || resolution.asset.disposition
                    != existing->disposition)
            {
                return failure(
                    EditError::InvalidAsset,
                    "Replacement cannot change asset kind or disposition");
            }
            if (resolution.asset.contentHash
                == existing->contentHash)
            {
                return failure(
                    EditError::InvalidAsset,
                    "Identical content should use same-generation relocation");
            }
            if (existing->generation
                >= maximumProjectAssetJsonInteger)
            {
                return failure(
                    EditError::InvalidAsset,
                    "Project asset generation cannot be incremented safely");
            }
            applied.generation = existing->generation + 1U;
            break;
        }

        auto candidate = std::ranges::find(
            candidateDocument.assets,
            assetId,
            &ProjectAsset::id);
        if (*candidate != applied)
        {
            *candidate = std::move(applied);
            changed = true;
        }
    }

    if (!changed)
        return success();
    const auto assetsValidation =
        validateAssets(candidateDocument.assets);
    if (!assetsValidation.ok())
        return assetsValidation;
    const auto projectValidation =
        projectValidationFailure(candidateDocument, registry_);
    if (!projectValidation.ok())
        return projectValidation;

    commit(std::move(candidateDocument));
    return success();
}

EditResult ProjectEditor::reconcileProjectAssetRuntimeIdentity(
    ProjectAsset inspectedAsset,
    std::uint64_t acceptedGeneration,
    std::string_view acceptedContentHash)
{
    const auto validation = validateAsset(inspectedAsset);
    if (!validation.ok())
        return validation;
    if (acceptedGeneration == 0
        || acceptedGeneration
            > maximumProjectAssetJsonInteger
        || !isCanonicalProjectAssetContentHash(
            acceptedContentHash))
    {
        return failure(
            EditError::InvalidAsset,
            "Runtime resource identity is invalid");
    }
    const auto existing = std::ranges::find(
        document_.assets,
        inspectedAsset.id,
        &ProjectAsset::id);
    if (existing == document_.assets.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown project asset: "
                + inspectedAsset.id);
    }
    if (inspectedAsset.kind != existing->kind)
    {
        return failure(
            EditError::InvalidAsset,
            "Runtime identity reconciliation cannot change the asset media kind");
    }
    if (inspectedAsset.contentHash.empty())
    {
        return failure(
            EditError::InvalidAsset,
            "Runtime identity reconciliation requires inspected bytes");
    }
    const auto generationFloor = std::max(
        acceptedGeneration,
        existing->generation);
    const auto existingIsAccepted =
        existing->generation == acceptedGeneration
        && existing->contentHash
            == acceptedContentHash;
    if (inspectedAsset.contentHash
            == acceptedContentHash
        && existing->contentHash
            == acceptedContentHash)
    {
        inspectedAsset.generation =
            acceptedGeneration;
    }
    else if (inspectedAsset.contentHash
            == existing->contentHash
        && ((existing->generation > acceptedGeneration
             && existing->contentHash
                 != acceptedContentHash)
            || existingIsAccepted))
    {
        inspectedAsset.generation =
            existing->generation;
    }
    else if (existing->generation <= acceptedGeneration
             && inspectedAsset.contentHash
                 == acceptedContentHash)
    {
        inspectedAsset.generation =
            acceptedGeneration;
    }
    else
    {
        if (generationFloor
            >= maximumProjectAssetJsonInteger)
        {
            return failure(
                EditError::InvalidAsset,
                "Project asset generation cannot be incremented safely");
        }
        inspectedAsset.generation =
            generationFloor + 1U;
    }
    if (inspectedAsset == *existing)
        return success();

    auto candidate = document_;
    *std::ranges::find(
        candidate.assets,
        inspectedAsset.id,
        &ProjectAsset::id) =
        std::move(inspectedAsset);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setDeviceSettings(ProjectDeviceSettings settings)
{
    const auto validation = validateDeviceSettings(settings);
    if (!validation.ok())
        return validation;
    if (document_.deviceSettings == settings)
        return success();

    ProjectDocument candidate = document_;
    candidate.deviceSettings = std::move(settings);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::replaceLiveSetLibrary(
    ProjectLiveSetLibrary library)
{
    try
    {
        if (document_.liveSetLibrary == library)
            return success();
        ProjectDocument candidate = document_;
        candidate.liveSetLibrary = std::move(library);
        const auto validation = validateProjectLiveSetLibrary(candidate);
        if (!validation.ok())
        {
            const auto message = validation.diagnostics.empty()
                ? std::string{"Live-set diagnostics exceed the fixed limit"}
                : validation.diagnostics.front().message;
            return failure(EditError::InvalidLiveSet, message);
        }
        return applyDocumentEdit(std::move(candidate));
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Could not allocate the live-set replacement edit");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Live-set replacement exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectLiveSet(ProjectLiveSet liveSet)
{
    try
    {
        auto library = document_.liveSetLibrary;
        const auto found = std::ranges::find(
            library.liveSets,
            liveSet.liveSetId,
            &ProjectLiveSet::liveSetId);
        if (found == library.liveSets.end())
            library.liveSets.push_back(std::move(liveSet));
        else
        {
            if (*found == liveSet)
                return success();
            *found = std::move(liveSet);
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set upsert edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set upsert exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectLiveSet(std::string_view liveSetId)
{
    try
    {
        if (!isValidProjectLiveSetIdentifier(liveSetId))
            return failure(EditError::InvalidLiveSet, "Invalid live-set identity");
        auto library = document_.liveSetLibrary;
        const auto found = std::ranges::find(
            library.liveSets, liveSetId, &ProjectLiveSet::liveSetId);
        if (found == library.liveSets.end())
            return failure(EditError::InvalidLiveSet, "Unknown live-set identity");
        library.liveSets.erase(found);
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::moveProjectLiveSet(
    std::string_view liveSetId,
    std::size_t destinationIndex)
{
    try
    {
        auto library = document_.liveSetLibrary;
        if (!moveScoreEntity(
                library.liveSets,
                liveSetId,
                destinationIndex,
                &ProjectLiveSet::liveSetId))
        {
            return failure(EditError::InvalidLiveSet,
                "Unknown live-set identity or destination");
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set move edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set move exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectLiveSetCell(
    std::string_view liveSetId,
    ProjectLiveSetCell cell)
{
    try
    {
        auto library = document_.liveSetLibrary;
        const auto liveSet = std::ranges::find(
            library.liveSets, liveSetId, &ProjectLiveSet::liveSetId);
        if (liveSet == library.liveSets.end())
            return failure(EditError::InvalidLiveSet, "Unknown live-set identity");
        const auto found = std::ranges::find(
            liveSet->cells, cell.cellId, &ProjectLiveSetCell::cellId);
        if (found == liveSet->cells.end())
            liveSet->cells.push_back(std::move(cell));
        else
        {
            if (*found == cell)
                return success();
            *found = std::move(cell);
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set cell upsert edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set cell upsert exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectLiveSetCell(
    std::string_view liveSetId,
    std::string_view cellId)
{
    try
    {
        auto library = document_.liveSetLibrary;
        const auto liveSet = std::ranges::find(
            library.liveSets, liveSetId, &ProjectLiveSet::liveSetId);
        if (liveSet == library.liveSets.end())
            return failure(EditError::InvalidLiveSet, "Unknown live-set identity");
        const auto found = std::ranges::find(
            liveSet->cells, cellId, &ProjectLiveSetCell::cellId);
        if (found == liveSet->cells.end())
            return failure(EditError::InvalidLiveSet, "Unknown live-set cell identity");
        liveSet->cells.erase(found);
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set cell removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set cell removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::moveProjectLiveSetCell(
    std::string_view liveSetId,
    std::string_view cellId,
    std::size_t destinationIndex)
{
    try
    {
        auto library = document_.liveSetLibrary;
        const auto liveSet = std::ranges::find(
            library.liveSets, liveSetId, &ProjectLiveSet::liveSetId);
        if (liveSet == library.liveSets.end()
            || !moveScoreEntity(
                liveSet->cells,
                cellId,
                destinationIndex,
                &ProjectLiveSetCell::cellId))
        {
            return failure(EditError::InvalidLiveSet,
                "Unknown live-set cell identity or destination");
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-set cell move edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-set cell move exceeded supported capacity");
    }
}

EditResult ProjectEditor::upsertProjectLiveTool(ProjectLiveTool tool)
{
    try
    {
        auto library = document_.liveSetLibrary;
        const auto found = std::ranges::find(
            library.tools, tool.toolId, &ProjectLiveTool::toolId);
        if (found == library.tools.end())
            library.tools.push_back(std::move(tool));
        else
        {
            if (*found == tool)
                return success();
            *found = std::move(tool);
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-tool upsert edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-tool upsert exceeded supported capacity");
    }
}

EditResult ProjectEditor::removeProjectLiveTool(std::string_view toolId)
{
    try
    {
        if (!isValidProjectLiveSetIdentifier(toolId))
            return failure(EditError::InvalidLiveSet, "Invalid live-tool identity");
        auto library = document_.liveSetLibrary;
        const auto found = std::ranges::find(
            library.tools, toolId, &ProjectLiveTool::toolId);
        if (found == library.tools.end())
            return failure(EditError::InvalidLiveSet, "Unknown live-tool identity");
        library.tools.erase(found);
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-tool removal edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-tool removal exceeded supported capacity");
    }
}

EditResult ProjectEditor::moveProjectLiveTool(
    std::string_view toolId,
    std::size_t destinationIndex)
{
    try
    {
        auto library = document_.liveSetLibrary;
        if (!moveScoreEntity(
                library.tools,
                toolId,
                destinationIndex,
                &ProjectLiveTool::toolId))
        {
            return failure(EditError::InvalidLiveSet,
                "Unknown live-tool identity or destination");
        }
        return replaceLiveSetLibrary(std::move(library));
    }
    catch (const std::bad_alloc&)
    {
        return failure(EditError::AllocationFailed,
            "Could not allocate the live-tool move edit");
    }
    catch (const std::length_error&)
    {
        return failure(EditError::AllocationFailed,
            "Live-tool move exceeded supported capacity");
    }
}

EditResult ProjectEditor::setTimelineLoop(
    TimelineLoopRegion loop,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Timeline interaction transaction is no longer authoritative");
    }
    if (document_.timeline.loop == loop)
        return success();

    ProjectDocument candidate = document_;
    candidate.timeline.loop = loop;
    const auto validation = validateTimeline(candidate.timeline);
    if (!validation.ok())
        return validation;

    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::setTimelineMarkerProgramChannel(std::uint8_t channel)
{
    if (channel > 16)
        return failure(EditError::InvalidTimeline, "The Timeline's program change channel must be 1 to 16, or 0 for none");
    if (document_.timeline.markerProgramChannel == channel)
        return success();
    ProjectDocument candidate = document_;
    candidate.timeline.markerProgramChannel = channel;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setMusicalTime(ProjectMusicalTime musicalTime)
{
    if (const auto error = projectMusicalTimeValidationError(musicalTime))
        return failure(EditError::InvalidTimeline, *error);
    if (document_.timeline.musicalTime == musicalTime)
        return success();
    ProjectDocument candidate = document_;
    candidate.timeline.musicalTime = std::move(musicalTime);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addTempoEvent(MusicalTempoEvent event)
{
    auto candidate = document_.timeline.musicalTime;
    candidate.tempo.push_back(event);
    std::ranges::sort(candidate.tempo, {}, &MusicalTempoEvent::tick);
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::updateTempoEvent(
    std::size_t index,
    MusicalTempoEvent event)
{
    if (index >= document_.timeline.musicalTime.tempo.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown tempo event");
    auto candidate = document_.timeline.musicalTime;
    candidate.tempo[index] = event;
    std::ranges::sort(candidate.tempo, {}, &MusicalTempoEvent::tick);
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::removeTempoEvent(std::size_t index)
{
    if (index >= document_.timeline.musicalTime.tempo.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown tempo event");
    if (document_.timeline.musicalTime.tempo[index].tick == 0)
    {
        return failure(
            EditError::InvalidTimeline,
            "The tempo event at tick zero is required");
    }
    auto candidate = document_.timeline.musicalTime;
    candidate.tempo.erase(
        candidate.tempo.begin() + static_cast<std::ptrdiff_t>(index));
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::addMeterEvent(MusicalMeterEvent event)
{
    auto candidate = document_.timeline.musicalTime;
    candidate.meter.push_back(event);
    std::ranges::sort(candidate.meter, {}, &MusicalMeterEvent::tick);
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::updateMeterEvent(
    std::size_t index,
    MusicalMeterEvent event)
{
    if (index >= document_.timeline.musicalTime.meter.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown meter event");
    auto candidate = document_.timeline.musicalTime;
    candidate.meter[index] = event;
    std::ranges::sort(candidate.meter, {}, &MusicalMeterEvent::tick);
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::removeMeterEvent(std::size_t index)
{
    if (index >= document_.timeline.musicalTime.meter.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown meter event");
    if (document_.timeline.musicalTime.meter[index].tick == 0)
    {
        return failure(
            EditError::InvalidTimeline,
            "The meter event at tick zero is required");
    }
    auto candidate = document_.timeline.musicalTime;
    candidate.meter.erase(
        candidate.meter.begin() + static_cast<std::ptrdiff_t>(index));
    return setMusicalTime(std::move(candidate));
}

EditResult ProjectEditor::upsertPartialTrackEditSet(
    ProjectPartialTrackEditSet edits,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Partial-track interaction transaction is no longer authoritative");
    }
    if (const auto invalid = validateProjectPartialTrackEditSet(edits))
        return failure(EditError::InvalidAsset, invalid->message);
    const auto sourceAsset = std::ranges::find(
        document_.assets, edits.assetId, &ProjectAsset::id);
    if (sourceAsset == document_.assets.end()
        || !partialTrackEditSetMatchesAsset(edits, *sourceAsset))
    {
        return failure(
            EditError::InvalidAsset,
            "Partial-track edit set does not match its exact source asset");
    }
    const auto conflictingAsset = std::ranges::find_if(
        document_.partialTrackEdits,
        [&](const ProjectPartialTrackEditSet& existing) {
            return existing.assetId == edits.assetId
                && existing.editSetId != edits.editSetId;
        });
    if (conflictingAsset != document_.partialTrackEdits.end())
    {
        return failure(
            EditError::DuplicateAsset,
            "Only one partial-track edit set may target an asset");
    }

    const auto existing = std::ranges::find(
        document_.partialTrackEdits,
        edits.editSetId,
        &ProjectPartialTrackEditSet::editSetId);
    if (existing != document_.partialTrackEdits.end()
        && existing->assetId != edits.assetId)
    {
        return failure(
            EditError::DuplicateAsset,
            "Partial-track edit-set identity already belongs to another asset");
    }
    if (existing != document_.partialTrackEdits.end()
        && *existing == edits)
    {
        return success();
    }
    if (existing == document_.partialTrackEdits.end()
        && document_.partialTrackEdits.size()
            >= maximumProjectPartialTrackEditSets)
    {
        return failure(
            EditError::InvalidAsset,
            "Partial-track edit-set count exceeds the fixed limit");
    }

    auto candidate = document_;
    const auto candidateExisting = std::ranges::find(
        candidate.partialTrackEdits,
        edits.editSetId,
        &ProjectPartialTrackEditSet::editSetId);
    if (candidateExisting == candidate.partialTrackEdits.end())
        candidate.partialTrackEdits.push_back(std::move(edits));
    else
        *candidateExisting = std::move(edits);
    std::ranges::sort(
        candidate.partialTrackEdits,
        {},
        &ProjectPartialTrackEditSet::editSetId);
    return applyDocumentEdit(std::move(candidate), transactionToken);
}

EditResult ProjectEditor::applyPartialTrackEditBatch(
    const PartialTrackAsset& source,
    ProjectPartialTrackEditBatch batch,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Partial-track interaction transaction is no longer authoritative");
    }
    const auto asset = std::ranges::find(
        document_.assets, batch.assetId, &ProjectAsset::id);
    if (asset == document_.assets.end()
        || asset->kind != ProjectAssetKind::PartialSpectrum
        || asset->generation != batch.assetGeneration
        || asset->contentHash != batch.sourceContentHash
        || asset->encoding != partialTrackAssetEncoding
        || asset->sampleRate != source.sampleRate
        || asset->frameCount != source.sourceFrameCount
        || validatePartialTrackAsset(source).has_value())
    {
        return failure(
            EditError::InvalidAsset,
            "Partial-track edit command does not match its exact immutable source");
    }
    const auto existing = std::ranges::find(
        document_.partialTrackEdits,
        batch.assetId,
        &ProjectPartialTrackEditSet::assetId);
    const auto applied = talas::project::applyProjectPartialTrackEditBatch(
        source,
        existing == document_.partialTrackEdits.end()
            ? nullptr
            : &*existing,
        batch);
    if (!applied.ok())
    {
        return failure(
            EditError::InvalidAsset,
            applied.error.has_value()
                ? applied.error->message
                : "Partial-track edit command failed");
    }
    return upsertPartialTrackEditSet(
        std::move(applied.edits), transactionToken);
}

EditResult ProjectEditor::removePartialTrackEditSet(
    std::string_view editSetId)
{
    if (!isValidProjectPartialTrackEditIdentifier(editSetId))
    {
        return failure(
            EditError::InvalidAsset,
            "Invalid partial-track edit-set identity");
    }
    const auto existing = std::ranges::find(
        document_.partialTrackEdits,
        editSetId,
        &ProjectPartialTrackEditSet::editSetId);
    if (existing == document_.partialTrackEdits.end())
    {
        return failure(
            EditError::InvalidAsset,
            "Unknown partial-track edit set");
    }
    auto candidate = document_;
    const auto candidateExisting = std::ranges::find(
        candidate.partialTrackEdits,
        editSetId,
        &ProjectPartialTrackEditSet::editSetId);
    candidate.partialTrackEdits.erase(candidateExisting);
    return applyDocumentEdit(std::move(candidate));
}

EditResult ProjectEditor::rebaseAssetPathsForProjectMove(
    const std::optional<std::filesystem::path>& sourceProjectFilePath,
    const std::filesystem::path& targetProjectFilePath)
{
    if (targetProjectFilePath.empty())
        return failure(EditError::InvalidAsset, "Target project path cannot be empty");

    static_cast<void>(
        rebaseProjectAssetPaths(document_, sourceProjectFilePath, targetProjectFilePath));
    for (auto& historyDocument : undoHistory_)
        static_cast<void>(
            rebaseProjectAssetPaths(historyDocument, sourceProjectFilePath, targetProjectFilePath));
    for (auto& historyDocument : redoHistory_)
        static_cast<void>(
            rebaseProjectAssetPaths(historyDocument, sourceProjectFilePath, targetProjectFilePath));
    if (transactionBefore_.has_value())
        static_cast<void>(
            rebaseProjectAssetPaths(*transactionBefore_, sourceProjectFilePath, targetProjectFilePath));
    return success();
}

EditResult ProjectEditor::addTimelineEventValue(TimelineEventValue event)
{
    ProjectDocument candidate = document_;
    candidate.timeline.eventValues.push_back(std::move(event));
    sortTimeline(candidate.timeline);
    const auto validation = validateTimeline(candidate.timeline);
    if (!validation.ok())
        return validation;
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;

    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::updateTimelineEventValue(
    std::size_t index,
    TimelineEventValue event,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Timeline interaction transaction is no longer authoritative");
    }
    if (index >= document_.timeline.eventValues.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown Timeline Event Value");
    if (timelineEventsEqual(document_.timeline.eventValues[index], event))
        return success();

    ProjectDocument candidate = document_;
    candidate.timeline.eventValues[index] = std::move(event);
    sortTimeline(candidate.timeline);
    const auto validation = validateTimeline(candidate.timeline);
    if (!validation.ok())
        return validation;
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;

    commit(std::move(candidate), transactionToken);
    return success();
}

EditResult ProjectEditor::removeTimelineEventValue(std::size_t index)
{
    if (index >= document_.timeline.eventValues.size())
        return failure(EditError::UnknownTimelineEvent, "Unknown Timeline Event Value");

    ProjectDocument candidate = document_;
    candidate.timeline.eventValues.erase(
        candidate.timeline.eventValues.begin() + static_cast<std::ptrdiff_t>(index));
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::applyScoreEdits(
    ProjectScoreEditBatch edits,
    HistoryTransactionToken transactionToken)
{
    if (transactionToken != 0
        && (!transactionBefore_.has_value()
            || transactionOwner_ != transactionToken))
    {
        return failure(
            EditError::InvalidSelection,
            "Score interaction transaction is no longer authoritative");
    }
    const auto definitionIds = scoreSoundDefinitionIds(document_);
    auto edited = projectScoreWithEdits(document_.timeline.score, std::move(edits), definitionIds);
    if (!edited.ok())
    {
        return failure(EditError::InvalidScore,
                       edited.error.has_value() ? edited.error->message : "Score edit failed");
    }
    if (*edited.candidate == document_.timeline.score)
        return success();
    auto candidate = document_;
    candidate.timeline.score = std::move(*edited.candidate);
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    return applyDocumentEdit(std::move(candidate), transactionToken);
}

EditResult ProjectEditor::addScoreTrack(ScoreTrack track)
{
    if (std::ranges::find(document_.timeline.score.tracks, track.trackId, &ScoreTrack::trackId) !=
        document_.timeline.score.tracks.end())
    {
        return failure(EditError::InvalidScore, "Duplicate Score track identity");
    }
    ProjectScoreEditBatch edits;
    edits.tracks.push_back(std::move(track));
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::updateScoreTrack(ScoreTrack track)
{
    if (std::ranges::find(document_.timeline.score.tracks, track.trackId, &ScoreTrack::trackId) ==
        document_.timeline.score.tracks.end())
    {
        return failure(EditError::InvalidScore, "Unknown Score track identity");
    }
    ProjectScoreEditBatch edits;
    edits.tracks.push_back(std::move(track));
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::removeScoreTrack(std::string_view trackId)
{
    if (!isValidScoreIdentifier(trackId))
        return failure(EditError::InvalidScore, "Invalid Score track identity");
    ProjectScoreEditBatch edits;
    edits.removeTrackIds.emplace_back(trackId);
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::moveScoreTrack(std::string_view trackId, std::size_t destinationIndex)
{
    if (!isValidScoreIdentifier(trackId))
        return failure(EditError::InvalidScore, "Invalid Score track identity");
    auto candidate = document_;
    if (!moveScoreEntity(
            candidate.timeline.score.tracks, trackId, destinationIndex, &ScoreTrack::trackId))
    {
        return failure(EditError::InvalidScore, "Unknown Score track or destination");
    }
    if (const auto validation = validateScore(candidate); !validation.ok())
        return validation;
    if (candidate.timeline.score == document_.timeline.score)
        return success();
    return applyDocumentEdit(std::move(candidate));
}

EditResult ProjectEditor::addScoreClip(ScoreClip clip)
{
    if (std::ranges::find(document_.timeline.score.clips, clip.clipId, &ScoreClip::clipId) !=
        document_.timeline.score.clips.end())
    {
        return failure(EditError::InvalidScore, "Duplicate Score clip identity");
    }
    ProjectScoreEditBatch edits;
    edits.clips.push_back(std::move(clip));
    return applyScoreEdits(std::move(edits));
}

// A wrapper Sound of an effect on a clip's
// Sound, its Input node "input" an instance of the Sound it affects.
[[nodiscard]] static const ProjectSoundDefinition* clipEffectWrapper(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto* definition = findSoundDefinition(document, definitionId);
    if (definition == nullptr || definition->graph.nodes.size() != 2)
        return nullptr;
    const auto input = std::ranges::find(definition->graph.nodes, std::string_view{"input"}, &ProjectNode::instanceId);
    const auto effect = std::ranges::find(definition->graph.nodes, std::string_view{"effect"}, &ProjectNode::instanceId);
    return input != definition->graph.nodes.end() && effect != definition->graph.nodes.end()
            && input->soundDefinitionId.has_value() && !effect->typeId.empty()
        ? definition
        : nullptr;
}

EditResult ProjectEditor::applyEffectToScoreClip(
    std::string_view clipId,
    std::string effectTypeId)
{
    if (registry_.findNodeType(effectTypeId) == nullptr)
        return failure(EditError::UnknownNodeType, "Unknown node type: " + effectTypeId);
    ProjectNode effect;
    effect.instanceId = "effect";
    effect.typeId = std::move(effectTypeId);
    return applyEffectNodeToScoreClip(clipId, std::move(effect));
}

EditResult ProjectEditor::applySoundEffectToScoreClip(
    std::string_view clipId,
    std::string_view effectSoundDefinitionId)
{
    if (findSoundDefinition(document_, effectSoundDefinitionId) == nullptr)
        return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string{effectSoundDefinitionId});
    ProjectNode effect;
    effect.instanceId = "effect";
    effect.soundDefinitionId = std::string{effectSoundDefinitionId};
    return applyEffectNodeToScoreClip(clipId, std::move(effect));
}

EditResult ProjectEditor::applyEffectNodeToScoreClip(std::string_view clipId, ProjectNode effect)
{
    auto candidate = document_;
    const auto clip = std::ranges::find(candidate.timeline.score.clips, clipId, &ScoreClip::clipId);
    if (clip == candidate.timeline.score.clips.end())
        return failure(EditError::InvalidScore, "Unknown Score clip identity");
    if (clip->sourceInstanceId.has_value())
    {
        return failure(
            EditError::InvalidScore,
            "An effect goes on a clip that plays a Sound definition, not one bound to a Sound in the graph");
    }
    const auto* inner = findSoundDefinition(candidate, clip->soundDefinitionId);
    const auto effectDescribed = describeProjectNode(candidate, effect, registry_);
    const auto* effectDescriptor = effectDescribed.has_value() ? &*effectDescribed : nullptr;
    if (inner == nullptr || inner->outputs.empty())
        return failure(EditError::UnknownSoundDefinition, "The clip's Sound has no output for an effect");
    if (effectDescriptor == nullptr)
        return failure(EditError::UnknownNodeType, "Unknown effect: " + effect.typeId);
    const auto* effectSound = effect.soundDefinitionId.has_value() ? findSoundDefinition(candidate, *effect.soundDefinitionId) : nullptr;
    ProjectNode instance;
    instance.instanceId = "input";
    instance.soundDefinitionId = inner->definitionId;
    const auto instanceDescriptor = describeProjectNode(candidate, instance, registry_);
    if (!instanceDescriptor.has_value() || instanceDescriptor->outputs.empty())
        return failure(EditError::UnknownSoundDefinition, "The clip's Sound has no output for an effect");
    const auto& source = instanceDescriptor->outputs.front();
    const auto* effectInput = firstCompatibleInput(*effectDescriptor, source);
    if (effectInput == nullptr || effectDescriptor->outputs.empty())
    {
        return failure(
            EditError::IncompatiblePortTypes,
            "an effect on a clip is a Sound with an Input for the clip's Sound");
    }
    ProjectSoundDefinition wrapper;
    std::string base = effectSound != nullptr ? effectSound->definitionId
        : effect.typeId.starts_with("talas.") ? effect.typeId.substr(3) : effect.typeId;
    std::ranges::replace(base, '.', '_');
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        wrapper.definitionId = base + "Of" + inner->definitionId + (ordinal == 1 ? std::string{} : std::to_string(ordinal));
        if (findSoundDefinition(candidate, wrapper.definitionId) == nullptr)
            break;
    }
    // "The name of the bar changes to Compressor(Freq/Time Scaling (Vocoder))".
    wrapper.displayName = (effectSound != nullptr && !effectSound->displayName.empty() ? effectSound->displayName
                                                                                     : effectDescriptor->displayName)
        + "(" + inner->displayName + ")";
    const auto effectOutput = effectDescriptor->outputs.front().id;
    wrapper.graph.nodes = {instance, std::move(effect)};
    wrapper.graph.connections = {{"input", source.id, "effect", effectInput->id}};
    wrapper.outputs.push_back({"audioOut", talas::core::PortType::Audio, "effect", effectOutput});
    clip->soundDefinitionId = wrapper.definitionId;
    clip->renderArtifact.reset();
    candidate.soundDefinitions.push_back(std::move(wrapper));
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    const auto applied = applyDocumentEdit(std::move(candidate), 0);
    return applied.ok() ? eventValueSyncOutcome(sync) : applied;
}

EditResult ProjectEditor::removeLastScoreClipEffect(std::string_view clipId)
{
    auto candidate = document_;
    const auto clip = std::ranges::find(candidate.timeline.score.clips, clipId, &ScoreClip::clipId);
    if (clip == candidate.timeline.score.clips.end())
        return failure(EditError::InvalidScore, "Unknown Score clip identity");
    const auto* wrapper = clipEffectWrapper(candidate, clip->soundDefinitionId);
    if (wrapper == nullptr)
        return failure(EditError::InvalidScore, "the clip's Sound has no effect to remove");
    const auto input = std::ranges::find(wrapper->graph.nodes, std::string_view{"input"}, &ProjectNode::instanceId);
    clip->soundDefinitionId = *input->soundDefinitionId;
    clip->renderArtifact.reset();
    return applyDocumentEdit(std::move(candidate), 0);
}

EditResult ProjectEditor::replaceScoreClipInput(
    std::string_view clipId,
    std::string_view soundDefinitionId)
{
    auto candidate = document_;
    const auto clip = std::ranges::find(candidate.timeline.score.clips, clipId, &ScoreClip::clipId);
    if (clip == candidate.timeline.score.clips.end())
        return failure(EditError::InvalidScore, "Unknown Score clip identity");
    if (findSoundDefinition(candidate, soundDefinitionId) == nullptr)
        return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string{soundDefinitionId});
    if (clipEffectWrapper(candidate, clip->soundDefinitionId) == nullptr)
        return failure(EditError::InvalidScore, "the clip's Sound has no Input to replace");
    if (clip->soundDefinitionId == soundDefinitionId)
        return failure(EditError::InvalidScore, "A Sound cannot be its own Input");
    // The wrapper may be shared, so the clip takes a copy with the new Input.
    auto wrapper = *findSoundDefinition(candidate, clip->soundDefinitionId);
    const auto base = wrapper.definitionId;
    for (std::size_t ordinal = 2;; ++ordinal)
    {
        wrapper.definitionId = base + "_" + std::to_string(ordinal);
        if (findSoundDefinition(candidate, wrapper.definitionId) == nullptr)
            break;
    }
    auto& input = *std::ranges::find(wrapper.graph.nodes, std::string_view{"input"}, &ProjectNode::instanceId);
    input.soundDefinitionId = std::string{soundDefinitionId};
    // The new Input's first output feeds the effect where the old one did.
    const auto inputDescriptor = describeProjectNode(candidate, input, registry_);
    if (!inputDescriptor.has_value() || inputDescriptor->outputs.empty())
        return failure(EditError::UnknownSoundDefinition, "The new Input has no output");
    for (auto& connection : wrapper.graph.connections)
    {
        if (connection.fromNode == "input")
            connection.fromPort = inputDescriptor->outputs.front().id;
    }
    const auto* replaced = findSoundDefinition(candidate, soundDefinitionId);
    const auto open = wrapper.displayName.find('(');
    wrapper.displayName = (open == std::string::npos ? wrapper.displayName : wrapper.displayName.substr(0, open))
        + "(" + replaced->displayName + ")";
    clip->soundDefinitionId = wrapper.definitionId;
    clip->renderArtifact.reset();
    candidate.soundDefinitions.push_back(std::move(wrapper));
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    return applyDocumentEdit(std::move(candidate), 0);
}

EditResult ProjectEditor::updateScoreClip(
    ScoreClip clip,
    HistoryTransactionToken transactionToken)
{
    if (std::ranges::find(document_.timeline.score.clips, clip.clipId, &ScoreClip::clipId) ==
        document_.timeline.score.clips.end())
    {
        return failure(EditError::InvalidScore, "Unknown Score clip identity");
    }
    ProjectScoreEditBatch edits;
    edits.clips.push_back(std::move(clip));
    return applyScoreEdits(std::move(edits), transactionToken);
}

EditResult ProjectEditor::publishScoreClipRenderArtifact(
    ScoreClipRenderArtifactPublication publication)
{
    try
    {
        if (publication.expectedDocumentAuthorityGeneration == 0
            || publication.expectedDocumentAuthorityGeneration
                != documentAuthorityGeneration()
            || publication.expectedCanonicalDocumentBytes.empty()
            || serializeProjectDocument(document_)
                != publication.expectedCanonicalDocumentBytes)
        {
            return failure(
                EditError::InvalidScore,
                "Score clip artifact publication authority is stale");
        }
        auto candidate = document_;
        const auto clip = std::ranges::find(
            candidate.timeline.score.clips,
            publication.clipId,
            &ScoreClip::clipId);
        if (clip == candidate.timeline.score.clips.end())
        {
            return failure(
                EditError::InvalidScore,
                "Unknown Score clip artifact identity");
        }
        std::string fingerprintError;
        const auto currentFingerprint = projectScoreClipSourceFingerprint(
            document_,
            registry_,
            publication.clipId,
            publication.artifact.sampleRate,
            fingerprintError);
        if (currentFingerprint.empty()
            || currentFingerprint
                != publication.artifact.sourceFingerprint)
        {
            return failure(
                EditError::InvalidScore,
                fingerprintError.empty()
                    ? "Score clip artifact dependency authority is stale"
                    : std::move(fingerprintError));
        }
        for (auto& asset : publication.assets)
        {
            const auto existing = std::ranges::find(
                candidate.assets, asset.id, &ProjectAsset::id);
            if (existing != candidate.assets.end())
            {
                if (*existing != asset)
                {
                    return failure(
                        EditError::InvalidAsset,
                        "Score clip artifact asset identity already exists");
                }
                continue;
            }
            candidate.assets.push_back(std::move(asset));
        }
        clip->renderArtifact = std::move(publication.artifact);
        const auto validation = projectValidationFailure(candidate, registry_);
        if (!validation.ok())
            return validation;
        return applyDocumentEdit(std::move(candidate));
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Score clip artifact publication allocation failed");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Score clip artifact publication exceeded supported limits");
    }
    catch (const std::exception& exception)
    {
        return failure(
            EditError::InvalidScore,
            std::string{"Score clip artifact publication failed: "}
                + exception.what());
    }
}

EditResult ProjectEditor::clearScoreClipRenderArtifact(
    std::string_view clipId)
{
    try
    {
        auto candidate = document_;
        const auto clip = std::ranges::find(
            candidate.timeline.score.clips,
            clipId,
            &ScoreClip::clipId);
        if (clip == candidate.timeline.score.clips.end())
        {
            return failure(
                EditError::InvalidScore,
                "Unknown Score clip artifact identity");
        }
        if (!clip->renderArtifact.has_value())
            return success();
        clip->renderArtifact.reset();
        const auto validation = projectValidationFailure(candidate, registry_);
        if (!validation.ok())
            return validation;
        return applyDocumentEdit(std::move(candidate));
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            EditError::AllocationFailed,
            "Score clip artifact removal allocation failed");
    }
    catch (const std::length_error&)
    {
        return failure(
            EditError::AllocationFailed,
            "Score clip artifact removal exceeded supported limits");
    }
}

EditResult ProjectEditor::removeScoreClip(std::string_view clipId)
{
    if (!isValidScoreIdentifier(clipId))
        return failure(EditError::InvalidScore, "Invalid Score clip identity");
    ProjectScoreEditBatch edits;
    edits.removeClipIds.emplace_back(clipId);
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::moveScoreClip(std::string_view clipId, std::size_t destinationIndex)
{
    if (!isValidScoreIdentifier(clipId))
        return failure(EditError::InvalidScore, "Invalid Score clip identity");
    auto candidate = document_;
    if (!moveScoreEntity(
            candidate.timeline.score.clips, clipId, destinationIndex, &ScoreClip::clipId))
    {
        return failure(EditError::InvalidScore, "Unknown Score clip or destination");
    }
    if (const auto validation = validateScore(candidate); !validation.ok())
        return validation;
    if (candidate.timeline.score == document_.timeline.score)
        return success();
    return applyDocumentEdit(std::move(candidate));
}

EditResult ProjectEditor::addScoreAutomationLane(ScoreAutomationLane lane)
{
    if (std::ranges::find(
            document_.timeline.score.automation, lane.laneId, &ScoreAutomationLane::laneId) !=
        document_.timeline.score.automation.end())
    {
        return failure(EditError::InvalidScore, "Duplicate Score automation identity");
    }
    ProjectScoreEditBatch edits;
    edits.automation.push_back(std::move(lane));
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::updateScoreAutomationLane(
    ScoreAutomationLane lane,
    HistoryTransactionToken transactionToken)
{
    if (std::ranges::find(
            document_.timeline.score.automation, lane.laneId, &ScoreAutomationLane::laneId) ==
        document_.timeline.score.automation.end())
    {
        return failure(EditError::InvalidScore, "Unknown Score automation identity");
    }
    ProjectScoreEditBatch edits;
    edits.automation.push_back(std::move(lane));
    return applyScoreEdits(std::move(edits), transactionToken);
}

EditResult ProjectEditor::removeScoreAutomationLane(std::string_view laneId)
{
    if (!isValidScoreIdentifier(laneId))
        return failure(EditError::InvalidScore, "Invalid Score automation identity");
    ProjectScoreEditBatch edits;
    edits.removeAutomationLaneIds.emplace_back(laneId);
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::moveScoreAutomationLane(std::string_view laneId,
                                                  std::size_t destinationIndex)
{
    if (!isValidScoreIdentifier(laneId))
        return failure(EditError::InvalidScore, "Invalid Score automation identity");
    auto candidate = document_;
    if (!moveScoreEntity(candidate.timeline.score.automation,
                         laneId,
                         destinationIndex,
                         &ScoreAutomationLane::laneId))
    {
        return failure(EditError::InvalidScore, "Unknown Score automation lane or destination");
    }
    if (const auto validation = validateScore(candidate); !validation.ok())
        return validation;
    if (candidate.timeline.score == document_.timeline.score)
        return success();
    return applyDocumentEdit(std::move(candidate));
}

EditResult ProjectEditor::addScoreMarker(ScoreMarker marker)
{
    if (std::ranges::find(document_.timeline.score.markers,
                          marker.markerId,
                          &ScoreMarker::markerId) != document_.timeline.score.markers.end())
    {
        return failure(EditError::InvalidScore, "Duplicate Score marker identity");
    }
    ProjectScoreEditBatch edits;
    edits.markers.push_back(std::move(marker));
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::updateScoreMarker(
    ScoreMarker marker,
    HistoryTransactionToken transactionToken)
{
    if (std::ranges::find(document_.timeline.score.markers,
                          marker.markerId,
                          &ScoreMarker::markerId) == document_.timeline.score.markers.end())
    {
        return failure(EditError::InvalidScore, "Unknown Score marker identity");
    }
    ProjectScoreEditBatch edits;
    edits.markers.push_back(std::move(marker));
    return applyScoreEdits(std::move(edits), transactionToken);
}

EditResult ProjectEditor::removeScoreMarker(std::string_view markerId)
{
    if (!isValidScoreIdentifier(markerId))
        return failure(EditError::InvalidScore, "Invalid Score marker identity");
    ProjectScoreEditBatch edits;
    edits.removeMarkerIds.emplace_back(markerId);
    return applyScoreEdits(std::move(edits));
}

EditResult ProjectEditor::moveScoreMarker(std::string_view markerId, std::size_t destinationIndex)
{
    if (!isValidScoreIdentifier(markerId))
        return failure(EditError::InvalidScore, "Invalid Score marker identity");
    auto candidate = document_;
    if (!moveScoreEntity(
            candidate.timeline.score.markers, markerId, destinationIndex, &ScoreMarker::markerId))
    {
        return failure(EditError::InvalidScore, "Unknown Score marker or destination");
    }
    if (const auto validation = validateScore(candidate); !validation.ok())
        return validation;
    if (candidate.timeline.score == document_.timeline.score)
        return success();
    return applyDocumentEdit(std::move(candidate));
}

EditResult ProjectEditor::addSampleMap(ProjectSampleMap map)
{
    if (findProjectSampleMap(document_, map.mapId) != nullptr)
    {
        return failure(
            EditError::DuplicateSampleMap,
            "Duplicate sample-map identity: " + map.mapId);
    }

    auto candidate = document_;
    candidate.sampleMapLibrary.maps.push_back(std::move(map));
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::renameSampleMap(
    std::string_view mapId,
    std::string displayName)
{
    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    if (map->displayName == displayName)
        return success();

    map->displayName = std::move(displayName);
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSampleMap(std::string_view mapId)
{
    auto candidate = document_;
    auto& maps = candidate.sampleMapLibrary.maps;
    const auto found = std::ranges::find(
        maps,
        mapId,
        &ProjectSampleMap::mapId);
    if (found == maps.end())
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    maps.erase(found);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::moveSampleMap(
    std::string_view mapId,
    std::size_t destinationIndex)
{
    auto candidate = document_;
    auto& maps = candidate.sampleMapLibrary.maps;
    const auto found = std::ranges::find(
        maps,
        mapId,
        &ProjectSampleMap::mapId);
    if (found == maps.end())
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    if (destinationIndex >= maps.size())
    {
        return failure(
            EditError::InvalidSampleMap,
            "Sample-map destination index is out of range");
    }

    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(maps.begin(), found));
    if (sourceIndex == destinationIndex)
        return success();

    auto moved = std::move(*found);
    maps.erase(found);
    maps.insert(
        maps.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(moved));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addSampleRegion(
    std::string_view mapId,
    ProjectSampleRegion region)
{
    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    if (findProjectSampleRegion(*map, region.regionId) != nullptr)
    {
        return failure(
            EditError::DuplicateSampleRegion,
            "Duplicate sample-region identity: " + region.regionId);
    }

    map->regions.push_back(std::move(region));
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::updateSampleRegion(
    std::string_view mapId,
    std::string_view regionId,
    ProjectSampleRegion region)
{
    if (region.regionId != regionId)
    {
        return failure(
            EditError::InvalidSampleMap,
            "Sample-region identity cannot be changed by an update");
    }

    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    auto* current = findProjectSampleRegion(*map, regionId);
    if (current == nullptr)
    {
        return failure(
            EditError::UnknownSampleRegion,
            "Unknown sample region: " + std::string(regionId));
    }
    if (*current == region)
        return success();

    *current = std::move(region);
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::updateSampleRegions(
    std::string_view mapId,
    std::vector<ProjectSampleRegion> regions)
{
    if (regions.empty())
        return success();

    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }

    std::unordered_set<std::string> updatedIds;
    bool changed = false;
    for (auto& region : regions)
    {
        if (!updatedIds.insert(region.regionId).second)
        {
            return failure(
                EditError::DuplicateSampleRegion,
                "Duplicate sample-region update identity: "
                    + region.regionId);
        }
        auto* current = findProjectSampleRegion(*map, region.regionId);
        if (current == nullptr)
        {
            return failure(
                EditError::UnknownSampleRegion,
                "Unknown sample region: " + region.regionId);
        }
        if (*current != region)
        {
            *current = std::move(region);
            changed = true;
        }
    }
    if (!changed)
        return success();

    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSampleRegion(
    std::string_view mapId,
    std::string_view regionId)
{
    return removeSampleRegions(
        mapId,
        {std::string(regionId)});
}

EditResult ProjectEditor::removeSampleRegions(
    std::string_view mapId,
    std::vector<std::string> regionIds)
{
    if (regionIds.empty())
        return success();

    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }

    std::unordered_set<std::string> removedIds;
    for (const auto& regionId : regionIds)
    {
        if (!removedIds.insert(regionId).second)
        {
            return failure(
                EditError::DuplicateSampleRegion,
                "Duplicate sample-region removal identity: " + regionId);
        }
        if (findProjectSampleRegion(*map, regionId) == nullptr)
        {
            return failure(
                EditError::UnknownSampleRegion,
                "Unknown sample region: " + regionId);
        }
    }

    std::erase_if(
        map->regions,
        [&](const auto& region) {
            return removedIds.contains(region.regionId);
        });
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::moveSampleRegion(
    std::string_view mapId,
    std::string_view regionId,
    std::size_t destinationIndex)
{
    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    auto& regions = map->regions;
    const auto found = std::ranges::find(
        regions,
        regionId,
        &ProjectSampleRegion::regionId);
    if (found == regions.end())
    {
        return failure(
            EditError::UnknownSampleRegion,
            "Unknown sample region: " + std::string(regionId));
    }
    if (destinationIndex >= regions.size())
    {
        return failure(
            EditError::InvalidSampleMap,
            "Sample-region destination index is out of range");
    }

    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(regions.begin(), found));
    if (sourceIndex == destinationIndex)
        return success();

    auto moved = std::move(*found);
    regions.erase(found);
    regions.insert(
        regions.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(moved));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::autoMapSampleRegions(
    std::string_view mapId,
    ProjectSampleMapAutoMapRequest request)
{
    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }

    auto mapped = makeAutoMappedSampleRegions(request);
    if (!mapped.ok())
    {
        return failure(
            EditError::InvalidSampleMap,
            mapped.error->path + ": " + mapped.error->message);
    }
    for (const auto& region : mapped.regions)
    {
        if (findProjectSampleRegion(*map, region.regionId) != nullptr)
        {
            return failure(
                EditError::DuplicateSampleRegion,
                "Auto-map region identity already exists: "
                    + region.regionId);
        }
    }

    map->regions.insert(
        map->regions.end(),
        std::make_move_iterator(mapped.regions.begin()),
        std::make_move_iterator(mapped.regions.end()));
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::mergeSampleMapImport(
    std::string_view mapId,
    ProjectSampleMapImportBatch batch)
{
    if (batch.regions.empty())
    {
        return failure(
            EditError::InvalidSampleMap,
            "Sample-map merge requires at least one candidate region");
    }
    const auto identityValidation =
        validateImportRegionIdentities(batch.regions);
    if (!identityValidation.ok())
        return identityValidation;

    auto candidate = document_;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    for (const auto& region : batch.regions)
    {
        if (findProjectSampleRegion(*map, region.regionId) != nullptr)
        {
            return failure(
                EditError::DuplicateSampleRegion,
                "Sample-map merge region identity already exists: "
                    + region.regionId);
        }
    }

    const auto assetValidation =
        normalizeSampleMapImportBatch(candidate, batch);
    if (!assetValidation.ok())
        return assetValidation;
    map = findProjectSampleMap(candidate, mapId);
    map->regions.insert(
        map->regions.end(),
        std::make_move_iterator(batch.regions.begin()),
        std::make_move_iterator(batch.regions.end()));
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

std::optional<ProjectSampleMapReplaceConfirmation>
ProjectEditor::sampleMapReplaceConfirmation(
    std::string_view mapId) const
{
    const auto* map = findProjectSampleMap(document_, mapId);
    if (map == nullptr)
        return std::nullopt;
    return ProjectSampleMapReplaceConfirmation{
        std::string(mapId),
        *map,
    };
}

EditResult ProjectEditor::replaceSampleMapImport(
    std::string_view mapId,
    ProjectSampleMapImportBatch batch,
    const ProjectSampleMapReplaceConfirmation& confirmation)
{
    const auto* current = findProjectSampleMap(document_, mapId);
    if (current == nullptr)
    {
        return failure(
            EditError::UnknownSampleMap,
            "Unknown sample map: " + std::string(mapId));
    }
    if (confirmation.mapId_ != mapId
        || confirmation.expectedMap_ != *current)
    {
        return failure(
            EditError::StaleSampleMapConfirmation,
            "Sample-map replace confirmation is stale");
    }
    if (batch.regions.empty())
    {
        return failure(
            EditError::InvalidSampleMap,
            "Sample-map replace requires at least one candidate region");
    }
    const auto identityValidation =
        validateImportRegionIdentities(batch.regions);
    if (!identityValidation.ok())
        return identityValidation;

    auto candidate = document_;
    const auto assetValidation =
        normalizeSampleMapImportBatch(candidate, batch);
    if (!assetValidation.ok())
        return assetValidation;
    auto* map = findProjectSampleMap(candidate, mapId);
    if (map->regions == batch.regions
        && candidate.assets == document_.assets)
    {
        return success();
    }
    map->regions = std::move(batch.regions);
    const auto validation = validateSampleMapCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::addSoundDefinition(
    ProjectSoundDefinition definition)
{
    if (definition.definitionId.empty()
        || definition.displayName.empty())
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "Sound definition id and name cannot be empty");
    }
    if (findSoundDefinition(document_, definition.definitionId) != nullptr)
    {
        return failure(
            EditError::DuplicateSoundDefinition,
            "Duplicate Sound definition id: "
                + definition.definitionId);
    }

    ProjectDocument candidate = document_;
    candidate.soundDefinitions.push_back(std::move(definition));
    const auto validation =
        projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync =
        synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::duplicateSoundDefinition(
    std::string_view definitionId,
    std::string newDefinitionId,
    std::string displayName)
{
    const auto* source = findSoundDefinition(document_, definitionId);
    if (source == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (newDefinitionId.empty() || displayName.empty())
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "Sound definition id and name cannot be empty");
    }
    if (findSoundDefinition(document_, newDefinitionId) != nullptr)
    {
        return failure(
            EditError::DuplicateSoundDefinition,
            "Duplicate Sound definition id: " + newDefinitionId);
    }
    ProjectDocument candidate = document_;
    auto copy = *source;
    copy.definitionId = newDefinitionId;
    copy.displayName = std::move(displayName);
    candidate.soundDefinitions.push_back(std::move(copy));
    // The duplicate sits beside the original, in its collection.
    for (auto& collection : candidate.soundCollections)
    {
        if (std::ranges::find(collection.soundIds, definitionId) != collection.soundIds.end())
            collection.soundIds.push_back(newDefinitionId);
    }
    if (const auto* layout = findProjectStructureLayout(document_, definitionId))
    {
        auto copied = *layout;
        copied.soundDefinitionId = std::move(newDefinitionId);
        candidate.presentation.structureLayouts.push_back(std::move(copied));
    }
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::replaceSoundDefinition(
    std::string_view definitionId,
    ProjectSoundDefinition definition)
{
    const auto* existing =
        findSoundDefinition(document_, definitionId);
    if (existing == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (definition.definitionId != definitionId)
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "Replacing a Sound definition cannot change its stable id");
    }

    ProjectDocument candidate = document_;
    *findSoundDefinition(candidate, definitionId) =
        std::move(definition);
    const auto validation =
        projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync =
        synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::renameSoundDefinition(
    std::string_view definitionId,
    std::string displayName)
{
    const auto* definition =
        findSoundDefinition(document_, definitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    if (displayName.empty())
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "Sound definition name cannot be empty");
    }
    if (definition->displayName == displayName)
        return success();

    ProjectDocument candidate = document_;
    findSoundDefinition(candidate, definitionId)->displayName =
        std::move(displayName);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSoundDefinition(
    std::string_view definitionId)
{
    if (findSoundDefinition(document_, definitionId) == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: " + std::string(definitionId));
    }
    const auto graphReferences = [&](const ProjectGraph& graph) {
        return std::ranges::any_of(
            graph.nodes,
            [&](const auto& node) {
                return node.soundDefinitionId
                    == std::optional<std::string>{
                        std::string(definitionId)};
            });
    };
    if (graphReferences(document_.graph)
        || std::ranges::any_of(
            document_.soundDefinitions,
            [&](const auto& definition) {
                return definition.definitionId != definitionId
                    && graphReferences(definition.graph);
            })
        || std::ranges::any_of(
            document_.timeline.score.clips,
            [&](const auto& clip) {
                return clip.soundDefinitionId == definitionId;
            })
        || std::ranges::any_of(
            document_.variableLibrary.variables,
            [&](const ProjectVariableDeclaration& variable) {
                return (variable.owner.kind
                            == ProjectVariableOwnerKind::SoundDefinition
                        || variable.owner.kind
                            == ProjectVariableOwnerKind::MidiVoice)
                    && variable.owner.definitionId == definitionId;
            })
        || std::ranges::any_of(
            document_.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& variable) {
                return ((variable.owner.kind
                             == ProjectVariableOwnerKind::SoundDefinition
                         || variable.owner.kind
                             == ProjectVariableOwnerKind::MidiVoice)
                            && variable.owner.definitionId == definitionId)
                    || std::ranges::find(
                           variable.candidateDefinitionIds,
                           definitionId)
                        != variable.candidateDefinitionIds.end();
            }))
    {
        return failure(
            EditError::SoundDefinitionInUse,
            "Cannot remove a referenced Sound definition");
    }

    ProjectDocument candidate = document_;
    std::erase_if(
        candidate.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    // A removed Sound leaves its collection.
    for (auto& collection : candidate.soundCollections)
        std::erase(collection.soundIds, std::string{definitionId});
    const auto generatorTargets = generatorTargetValidationFailure(candidate, registry_);
    if (!generatorTargets.ok())
        return generatorTargets;
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return success();
}

namespace
{
// A collection's id from its name: its letters and digits, and a number
// when another collection or Sound has it.
[[nodiscard]] std::string freeCollectionId(const ProjectDocument& document, std::string_view name)
{
    std::string base;
    for (const auto character : name)
    {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_')
            base.push_back(character);
    }
    if (base.empty() || (base.front() >= '0' && base.front() <= '9'))
        base.insert(0, "collection");
    const auto taken = [&](const std::string& candidate) {
        return std::ranges::any_of(document.soundCollections,
                   [&](const ProjectSoundCollection& collection) { return collection.collectionId == candidate; })
            || std::ranges::any_of(document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return definition.definitionId == candidate; });
    };
    auto candidate = base;
    for (std::size_t suffix = 2; taken(candidate); ++suffix)
        candidate = base + std::to_string(suffix);
    return candidate;
}

[[nodiscard]] ProjectSoundCollection* findSoundCollection(ProjectDocument& document, std::string_view collectionId)
{
    const auto found = std::ranges::find(document.soundCollections, collectionId, &ProjectSoundCollection::collectionId);
    return found == document.soundCollections.end() ? nullptr : &*found;
}

// The collection holding a Sound or collection, if any.
[[nodiscard]] ProjectSoundCollection* holderOf(ProjectDocument& document, std::string_view id, bool sound)
{
    const auto found = std::ranges::find_if(document.soundCollections, [&](const ProjectSoundCollection& collection) {
        const auto& members = sound ? collection.soundIds : collection.collectionIds;
        return std::ranges::find(members, id) != members.end();
    });
    return found == document.soundCollections.end() ? nullptr : &*found;
}
}

EditResult ProjectEditor::collectSounds(
    std::optional<std::string_view> parentCollectionId,
    std::vector<std::string> soundIds,
    std::vector<std::string> collectionIds,
    std::string name,
    std::string& collectionId)
{
    if (name.empty() || (soundIds.empty() && collectionIds.empty()))
        return failure(EditError::InvalidSelection, "Collect: select Sounds and give the collection a name");
    ProjectDocument candidate = document_;
    if (parentCollectionId.has_value() && findSoundCollection(candidate, *parentCollectionId) == nullptr)
        return failure(EditError::InvalidSelection, "Unknown Sound collection: " + std::string{*parentCollectionId});
    ProjectSoundCollection collection;
    collection.collectionId = freeCollectionId(candidate, name);
    collection.name = std::move(name);
    // The selected Sounds and collections move into the new one.
    for (const auto& sound : soundIds)
    {
        if (auto* holder = holderOf(candidate, sound, true))
            std::erase(holder->soundIds, sound);
        collection.soundIds.push_back(sound);
    }
    for (const auto& nested : collectionIds)
    {
        if (parentCollectionId.has_value() && nested == *parentCollectionId)
            return failure(EditError::InvalidSelection, "A collection cannot hold the collection it is in");
        if (auto* holder = holderOf(candidate, nested, false))
            std::erase(holder->collectionIds, nested);
        collection.collectionIds.push_back(nested);
    }
    collectionId = collection.collectionId;
    candidate.soundCollections.push_back(std::move(collection));
    if (parentCollectionId.has_value())
        findSoundCollection(candidate, *parentCollectionId)->collectionIds.push_back(collectionId);
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::renameSoundCollection(std::string_view collectionId, std::string name)
{
    ProjectDocument candidate = document_;
    auto* collection = findSoundCollection(candidate, collectionId);
    if (collection == nullptr || name.empty())
        return failure(EditError::InvalidSelection, "A Sound collection needs a name");
    collection->name = std::move(name);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSoundCollection(std::string_view collectionId)
{
    ProjectDocument candidate = document_;
    const auto* collection = findSoundCollection(candidate, collectionId);
    if (collection == nullptr)
        return failure(EditError::InvalidSelection, "Unknown Sound collection: " + std::string{collectionId});
    const auto removed = *collection;
    // Its Sounds and collections go where it was.
    if (auto* holder = holderOf(candidate, collectionId, false))
    {
        std::erase(holder->collectionIds, std::string{collectionId});
        holder->soundIds.insert(holder->soundIds.end(), removed.soundIds.begin(), removed.soundIds.end());
        holder->collectionIds.insert(holder->collectionIds.end(), removed.collectionIds.begin(), removed.collectionIds.end());
    }
    std::erase_if(candidate.soundCollections, [&](const ProjectSoundCollection& other) {
        return other.collectionId == collectionId;
    });
    const auto validation = projectValidationFailure(candidate, registry_, true);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::duplicateSoundCollection(std::string_view collectionId, std::string& duplicateId)
{
    ProjectDocument candidate = document_;
    if (findSoundCollection(candidate, collectionId) == nullptr)
        return failure(EditError::InvalidSelection, "Unknown Sound collection: " + std::string{collectionId});
    const auto numbered = [](std::string_view base, const auto& taken) {
        for (std::size_t ordinal = 1;; ++ordinal)
        {
            auto name = std::string{base} + std::to_string(ordinal);
            if (!taken(name))
                return name;
        }
    };
    // A duplicate of a collection holds a duplicate
    // of each of its Sounds, and of its collections in turn.
    const std::function<std::string(std::string_view)> duplicate = [&](std::string_view id) -> std::string {
        const auto original = *findSoundCollection(candidate, id);
        ProjectSoundCollection copy;
        copy.name = numbered(original.name + " ", [&](const std::string& name) {
            return std::ranges::any_of(candidate.soundCollections, [&](const ProjectSoundCollection& other) {
                return other.name == name;
            });
        });
        copy.collectionId = freeCollectionId(candidate, copy.name);
        for (const auto& sound : original.soundIds)
        {
            const auto source = std::ranges::find(candidate.soundDefinitions, sound, &ProjectSoundDefinition::definitionId);
            if (source == candidate.soundDefinitions.end())
                continue;
            auto copied = *source;
            copied.definitionId = numbered(sound, [&](const std::string& name) {
                return std::ranges::any_of(candidate.soundDefinitions,
                           [&](const ProjectSoundDefinition& definition) { return definition.definitionId == name; })
                    || std::ranges::any_of(candidate.soundCollections,
                        [&](const ProjectSoundCollection& other) { return other.collectionId == name; });
            });
            const auto base = (source->displayName.empty() ? sound : source->displayName) + " ";
            copied.displayName = numbered(base, [&](const std::string& name) {
                return std::ranges::any_of(candidate.soundDefinitions,
                    [&](const ProjectSoundDefinition& definition) { return definition.displayName == name; });
            });
            if (const auto* layout = findProjectStructureLayout(candidate, sound))
            {
                auto copiedLayout = *layout;
                copiedLayout.soundDefinitionId = copied.definitionId;
                candidate.presentation.structureLayouts.push_back(std::move(copiedLayout));
            }
            copy.soundIds.push_back(copied.definitionId);
            candidate.soundDefinitions.push_back(std::move(copied));
        }
        for (const auto& nested : original.collectionIds)
            copy.collectionIds.push_back(duplicate(nested));
        const auto copyId = copy.collectionId;
        candidate.soundCollections.push_back(std::move(copy));
        return copyId;
    };
    duplicateId = duplicate(collectionId);
    // The duplicate sits beside the original.
    if (auto* holder = holderOf(candidate, collectionId, false))
        holder->collectionIds.push_back(duplicateId);
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::extractNodesToSound(
    std::vector<std::string> nodeIds,
    std::string definitionId,
    std::string displayName,
    std::string instanceId)
{
    if (nodeIds.empty())
    {
        return failure(
            EditError::InvalidSelection,
            "Select at least one node to extract a Sound");
    }
    if (definitionId.empty() || displayName.empty()
        || instanceId.empty())
    {
        return failure(
            EditError::InvalidSoundDefinition,
            "Extracted Sound ids and name cannot be empty");
    }
    if (findSoundDefinition(document_, definitionId) != nullptr)
    {
        return failure(
            EditError::DuplicateSoundDefinition,
            "Duplicate Sound definition id: " + definitionId);
    }
    if (findNode(document_, instanceId) != nullptr)
    {
        return failure(
            EditError::DuplicateNodeId,
            "Duplicate node instance id: " + instanceId);
    }

    std::unordered_set<std::string> selected;
    for (auto& nodeId : nodeIds)
    {
        if (!selected.insert(nodeId).second)
        {
            return failure(
                EditError::InvalidSelection,
                "Sound extraction selection contains duplicate node ids");
        }
        const auto* selectedNode =
            findNode(document_, nodeId);
        if (selectedNode == nullptr)
        {
            return failure(
                EditError::UnknownNodeId,
                "Unknown selected node: " + nodeId);
        }
        // A DiskRecorder whose output goes on is within the Sound, not its
        // output.
        if (!selectedNode->soundDefinitionId.has_value()
            && isTerminalNodeType(selectedNode->typeId)
            && !(selectedNode->typeId == "talas.diskRecorder"
                && std::ranges::any_of(document_.graph.connections, [&](const talas::core::Connection& connection) {
                       return connection.fromNode == selectedNode->instanceId && connection.fromPort == "audioOut";
                   })))
        {
            return failure(
                EditError::InvalidSelection,
                "Terminal root nodes cannot be extracted into a Sound");
        }
    }

    ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = std::move(displayName);
    CanvasPosition instancePosition{
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max(),
    };
    std::vector<ProjectNodePresentation> extractedPresentation;
    for (const auto& node : document_.graph.nodes)
    {
        if (!selected.contains(node.instanceId))
            continue;
        const auto* presented = findProjectNodePresentation(
            document_,
            std::nullopt,
            node.instanceId);
        if (presented == nullptr)
        {
            return failure(
                EditError::InvalidSelection,
                "Selected node is missing portable presentation state");
        }
        definition.graph.nodes.push_back(node);
        instancePosition.x =
            std::min(instancePosition.x, presented->position.x);
        instancePosition.y =
            std::min(instancePosition.y, presented->position.y);
    }
    exposeExampleVariables(definition, document_, registry_);
    for (const auto& internalNode : definition.graph.nodes)
    {
        const auto* presented = findProjectNodePresentation(
            document_,
            std::nullopt,
            internalNode.instanceId);
        const auto normalized = translatedPosition(
            presented->position,
            -static_cast<std::int64_t>(instancePosition.x),
            -static_cast<std::int64_t>(instancePosition.y));
        if (!normalized.has_value())
        {
            return failure(
                EditError::InvalidSelection,
                "Extracted Sound node positions exceed the supported canvas range");
        }
        extractedPresentation.push_back({
            internalNode.instanceId,
            *normalized,
        });
    }

    std::vector<talas::core::Connection> replacementConnections;
    std::unordered_map<std::string, std::string> outputBoundaries;
    const auto endpointKey = [](std::string_view nodeId,
                                std::string_view portId) {
        return std::string(nodeId) + "\x1f" + std::string(portId);
    };
    for (const auto& graphConnection : document_.graph.connections)
    {
        const auto fromSelected =
            selected.contains(graphConnection.fromNode);
        const auto toSelected =
            selected.contains(graphConnection.toNode);
        if (fromSelected && toSelected)
        {
            definition.graph.connections.push_back(graphConnection);
            continue;
        }
        if (!fromSelected && toSelected)
        {
            const auto* target =
                findNode(document_, graphConnection.toNode);
            const auto descriptor =
                describeProjectNode(document_, *target, registry_);
            const auto* port = descriptor.has_value()
                ? findPort(
                    descriptor->inputs,
                    graphConnection.toPort)
                : nullptr;
            if (port == nullptr)
            {
                return failure(
                    EditError::InvalidSelection,
                    "Cannot derive an extracted Sound input boundary");
            }
            const auto boundaryId =
                "input" + std::to_string(
                    definition.inputs.size() + 1);
            definition.inputs.push_back({
                boundaryId,
                port->type,
                graphConnection.toNode,
                graphConnection.toPort,
            });
            replacementConnections.push_back({
                graphConnection.fromNode,
                graphConnection.fromPort,
                instanceId,
                boundaryId,
            });
            continue;
        }
        if (fromSelected && !toSelected)
        {
            const auto key = endpointKey(
                graphConnection.fromNode,
                graphConnection.fromPort);
            auto boundary = outputBoundaries.find(key);
            if (boundary == outputBoundaries.end())
            {
                const auto* source =
                    findNode(document_, graphConnection.fromNode);
                const auto descriptor =
                    describeProjectNode(document_, *source, registry_);
                const auto* port = descriptor.has_value()
                    ? findPort(
                        descriptor->outputs,
                        graphConnection.fromPort)
                    : nullptr;
                if (port == nullptr)
                {
                    return failure(
                        EditError::InvalidSelection,
                        "Cannot derive an extracted Sound output boundary");
                }
                const auto boundaryId =
                    "output" + std::to_string(
                        definition.outputs.size() + 1);
                definition.outputs.push_back({
                    boundaryId,
                    port->type,
                    graphConnection.fromNode,
                    graphConnection.fromPort,
                });
                boundary = outputBoundaries
                               .emplace(key, boundaryId)
                               .first;
            }
            replacementConnections.push_back({
                instanceId,
                boundary->second,
                graphConnection.toNode,
                graphConnection.toPort,
            });
            continue;
        }
        replacementConnections.push_back(graphConnection);
    }

    ProjectDocument candidate = document_;
    std::erase_if(
        candidate.graph.nodes,
        [&](const auto& node) {
            return selected.contains(node.instanceId);
        });
    candidate.graph.connections =
        std::move(replacementConnections);
    candidate.soundDefinitions.push_back(
        std::move(definition));
    ProjectNode instance;
    instance.instanceId = std::move(instanceId);
    instance.soundDefinitionId =
        candidate.soundDefinitions.back().definitionId;
    const auto createdDefinitionId = *instance.soundDefinitionId;
    const auto createdInstanceId = instance.instanceId;
    candidate.graph.nodes.push_back(std::move(instance));
    reconcileProjectPresentation(candidate);
    findProjectNodePresentation(
        candidate,
        std::nullopt,
        createdInstanceId)->position = instancePosition;
    for (const auto& presented : extractedPresentation)
    {
        findProjectNodePresentation(
            candidate,
            createdDefinitionId,
            presented.nodeId)->position = presented.position;
    }

    const auto validation =
        projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync =
        synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::inlineSoundInstance(
    std::string_view instanceId)
{
    const auto* instance = findNode(document_, instanceId);
    if (instance == nullptr)
    {
        return failure(
            EditError::UnknownNodeId,
            "Unknown node instance: " + std::string(instanceId));
    }
    if (!instance->soundDefinitionId.has_value())
    {
        return failure(
            EditError::InvalidSelection,
            "Selected node is not a Sound instance");
    }
    const auto* definition = findSoundDefinition(
        document_,
        *instance->soundDefinitionId);
    if (definition == nullptr)
    {
        return failure(
            EditError::UnknownSoundDefinition,
            "Unknown Sound definition: "
                + *instance->soundDefinitionId);
    }
    const auto* instancePresentation = findProjectNodePresentation(
        document_,
        std::nullopt,
        instanceId);
    if (instancePresentation == nullptr)
    {
        return failure(
            EditError::InvalidSelection,
            "Sound instance is missing portable presentation state");
    }
    if (const auto dependency = inlineScopeDependency(document_, *definition))
        return failure(EditError::InvalidSelection, *dependency);

    std::unordered_set<std::string> usedIds;
    for (const auto& node : document_.graph.nodes)
    {
        if (node.instanceId != instanceId)
            usedIds.insert(node.instanceId);
    }
    std::unordered_map<std::string, std::string> inlinedIds;
    for (const auto& internalNode : definition->graph.nodes)
    {
        const auto base =
            std::string(instanceId) + "_" + internalNode.instanceId;
        auto candidateId = base;
        std::size_t suffix = 0;
        while (!usedIds.insert(candidateId).second)
            candidateId = base + "_" + std::to_string(++suffix);
        inlinedIds.emplace(
            internalNode.instanceId,
            std::move(candidateId));
    }

    std::vector<ProjectNodePresentation> inlinedPresentation;
    inlinedPresentation.reserve(definition->graph.nodes.size());
    for (const auto& internalNode : definition->graph.nodes)
    {
        const auto* presented = findProjectNodePresentation(
            document_,
            definition->definitionId,
            internalNode.instanceId);
        if (presented == nullptr)
        {
            return failure(
                EditError::InvalidSoundDefinition,
                "Sound definition node is missing portable presentation state");
        }
        const auto translated = translatedPosition(
            presented->position,
            instancePresentation->position.x,
            instancePresentation->position.y);
        if (!translated.has_value())
        {
            return failure(
                EditError::InvalidSelection,
                "Inlined Sound node positions exceed the supported canvas range");
        }
        inlinedPresentation.push_back({
            inlinedIds.at(internalNode.instanceId),
            *translated,
        });
    }

    ProjectDocument candidate = document_;
    std::erase_if(
        candidate.graph.nodes,
        [&](const auto& node) {
            return node.instanceId == instanceId;
        });
    std::erase_if(
        candidate.graph.connections,
        [&](const auto& graphConnection) {
            return graphConnection.fromNode == instanceId
                || graphConnection.toNode == instanceId;
        });
    for (const auto& internalNode : definition->graph.nodes)
    {
        auto clone = internalNode;
        applySoundInterfaceBindingsForInline(
            *definition,
            *instance,
            clone);
        clone.instanceId = inlinedIds.at(internalNode.instanceId);
        candidate.graph.nodes.push_back(std::move(clone));
    }
    for (const auto& graphConnection :
         definition->graph.connections)
    {
        auto clone = graphConnection;
        clone.fromNode = inlinedIds.at(graphConnection.fromNode);
        clone.toNode = inlinedIds.at(graphConnection.toNode);
        candidate.graph.connections.push_back(std::move(clone));
    }

    // A SoundCollectionVariable an instance gave Sounds is replaced by them,
    // each where the collection was: its own connections
    // out, as the definition made them.
    std::unordered_set<std::string> givenCollections;
    std::vector<talas::core::Connection> collectionOutputs;
    std::unordered_set<std::string> replacedSounds;
    for (const auto& graphConnection :
         document_.graph.connections)
    {
        if (graphConnection.toNode == instanceId)
        {
            const auto boundary = std::ranges::find_if(
                definition->inputs,
                [&](const auto& input) {
                    return input.portId
                        == graphConnection.toPort;
                });
            if (boundary == definition->inputs.end())
            {
                return failure(
                    EditError::InvalidSoundDefinition,
                    "Sound instance input boundary is missing");
            }
            if (boundary->replacesSound)
            {
                // The connected Sound takes the replaceable input's place
                // wherever it fed.
                const auto& replacedId = inlinedIds.at(boundary->nodeId);
                for (auto& internal : candidate.graph.connections)
                {
                    if (internal.fromNode == replacedId && internal.fromPort == boundary->nodePortId)
                    {
                        internal.fromNode = graphConnection.fromNode;
                        internal.fromPort = graphConnection.fromPort;
                    }
                }
                replacedSounds.insert(replacedId);
                continue;
            }
            // A boundary bound to a Variable gives it the connected Sound,
            // which it passes on as an Annotation (ADR-0006 amendment of
            // 2026-10-13).
            const auto inlined = std::ranges::find(candidate.graph.nodes,
                inlinedIds.at(boundary->nodeId), &ProjectNode::instanceId);
            const auto bindsCollection = inlined != candidate.graph.nodes.end()
                && inlined->typeId == "talas.soundCollectionVariable"
                && !inlined->soundDefinitionId.has_value();
            if (bindsCollection)
            {
                const auto collectionId = inlined->instanceId;
                if (givenCollections.insert(collectionId).second)
                {
                    for (const auto& internal : candidate.graph.connections)
                    {
                        if (internal.fromNode == collectionId)
                            collectionOutputs.push_back(internal);
                    }
                }
                for (const auto& output : collectionOutputs)
                {
                    if (output.fromNode == collectionId)
                    {
                        candidate.graph.connections.push_back({
                            graphConnection.fromNode,
                            graphConnection.fromPort,
                            output.toNode,
                            output.toPort,
                        });
                    }
                }
                continue;
            }
            const auto bindsVariable = inlined != candidate.graph.nodes.end()
                && inlined->typeId == "talas.variable"
                && !inlined->soundDefinitionId.has_value();
            if (bindsVariable)
            {
                inlined->typeId = "talas.annotation";
                inlined->parameters.clear();
            }
            candidate.graph.connections.push_back({
                graphConnection.fromNode,
                graphConnection.fromPort,
                inlinedIds.at(boundary->nodeId),
                bindsVariable ? std::string{"audioIn"} : boundary->nodePortId,
            });
        }
        if (graphConnection.fromNode == instanceId)
        {
            const auto boundary = std::ranges::find_if(
                definition->outputs,
                [&](const auto& output) {
                    return output.portId
                        == graphConnection.fromPort;
                });
            if (boundary == definition->outputs.end())
            {
                return failure(
                    EditError::InvalidSoundDefinition,
                    "Sound instance output boundary is missing");
            }
            candidate.graph.connections.push_back({
                inlinedIds.at(boundary->nodeId),
                boundary->nodePortId,
                graphConnection.toNode,
                graphConnection.toPort,
            });
        }
    }

    // A replaced Sound goes, with the Sounds of the instance that fed only it.
    for (auto grew = !replacedSounds.empty(); grew;)
    {
        grew = false;
        for (const auto& line : candidate.graph.connections)
        {
            const auto inlined = std::ranges::any_of(inlinedIds, [&](const auto& entry) {
                return entry.second == line.fromNode;
            });
            if (!inlined || !replacedSounds.contains(line.toNode) || replacedSounds.contains(line.fromNode))
                continue;
            const auto feedsOnlyThem = std::ranges::all_of(candidate.graph.connections, [&](const talas::core::Connection& other) {
                return other.fromNode != line.fromNode || replacedSounds.contains(other.toNode);
            });
            if (feedsOnlyThem)
            {
                replacedSounds.insert(line.fromNode);
                grew = true;
            }
        }
    }
    std::erase_if(candidate.graph.nodes, [&](const auto& node) {
        return givenCollections.contains(node.instanceId) || replacedSounds.contains(node.instanceId);
    });
    std::erase_if(candidate.graph.connections, [&](const auto& graphConnection) {
        return givenCollections.contains(graphConnection.fromNode)
            || givenCollections.contains(graphConnection.toNode)
            || replacedSounds.contains(graphConnection.fromNode)
            || replacedSounds.contains(graphConnection.toNode);
    });
    reconcileProjectPresentation(candidate);
    for (const auto& presented : inlinedPresentation)
    {
        if (givenCollections.contains(presented.nodeId) || replacedSounds.contains(presented.nodeId))
            continue;
        findProjectNodePresentation(
            candidate,
            std::nullopt,
            presented.nodeId)->position = presented.position;
    }

    const auto validation =
        projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync =
        synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

namespace
{
// A Sound with the Sound instances named expanded, one level:
// inlined as a project's own are, in a scratch copy of the
// project holding only its signal flow, the Sound's boundaries and
// parameter bindings that named an instance following that instance's own.
struct ExpandedSound
{
    EditResult result;
    ProjectSoundDefinition definition;
    std::vector<ProjectNodePresentation> layout;
};

[[nodiscard]] ExpandedSound expandedSound(const ProjectDocument& document, const talas::core::NodeRegistry& registry,
    const ProjectSoundDefinition& source, const std::vector<std::string>& instances)
{
    ExpandedSound expanded{{}, source, {}};
    ProjectDocument scratch = document;
    scratch.graph = source.graph;
    scratch.virtualControls.clear();
    scratch.virtualControlLayoutPages.clear();
    scratch.virtualControlLayoutSettings.clear();
    scratch.virtualControlDecorations.clear();
    scratch.dormantVirtualControls.clear();
    scratch.timeline = {};
    scratch.snapshotLibrary = {};
    scratch.liveSetLibrary = {};
    scratch.presentation.performanceControls.clear();
    scratch.presentation.performanceMeters.clear();
    if (auto* root = findProjectStructureLayout(scratch))
    {
        const auto* layout = findProjectStructureLayout(document, source.definitionId);
        root->nodes = layout == nullptr ? std::vector<ProjectNodePresentation>{} : layout->nodes;
    }
    const auto failed = [&](EditResult result) {
        expanded.result = std::move(result);
        return expanded;
    };
    std::optional<ProjectEditor> editor;
    try
    {
        editor.emplace(std::move(scratch), registry);
    }
    catch (const std::invalid_argument& error)
    {
        return failed(failure(EditError::InvalidSoundDefinition, error.what()));
    }
    for (const auto& instanceId : instances)
    {
        const auto& before = editor->document();
        const auto* instance = findNode(before, instanceId);
        if (instance == nullptr || !instance->soundDefinitionId.has_value())
            return failed(failure(EditError::InvalidSelection, "Selected node is not a Sound instance"));
        const auto* found = findSoundDefinition(before, *instance->soundDefinitionId);
        if (found == nullptr)
            return failed(failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + *instance->soundDefinitionId));
        const auto inner = *found;
        // The ids inlining gives the instance's Sounds.
        std::unordered_set<std::string> usedIds;
        for (const auto& node : before.graph.nodes)
        {
            if (node.instanceId != instanceId)
                usedIds.insert(node.instanceId);
        }
        std::unordered_map<std::string, std::string> inlinedIds;
        for (const auto& internalNode : inner.graph.nodes)
        {
            const auto base = instanceId + "_" + internalNode.instanceId;
            auto candidateId = base;
            std::size_t suffix = 0;
            while (!usedIds.insert(candidateId).second)
                candidateId = base + "_" + std::to_string(++suffix);
            inlinedIds.emplace(internalNode.instanceId, std::move(candidateId));
        }
        if (auto inlined = editor->inlineSoundInstance(instanceId); !inlined.ok())
            return failed(std::move(inlined));
        const auto follow = [&](ProjectSoundPort& port, const std::vector<ProjectSoundPort>& innerPorts) {
            if (port.nodeId != instanceId)
                return true;
            const auto at = std::ranges::find(innerPorts, port.nodePortId, &ProjectSoundPort::portId);
            if (at == innerPorts.end() || at->replacesSound)
                return false;
            port.nodeId = inlinedIds.at(at->nodeId);
            port.nodePortId = at->nodePortId;
            return true;
        };
        for (auto& output : expanded.definition.outputs)
        {
            if (!follow(output, inner.outputs))
                return failed(failure(EditError::InvalidSoundDefinition, "Sound instance output boundary is missing"));
        }
        for (auto& input : expanded.definition.inputs)
        {
            if (!follow(input, inner.inputs))
                return failed(failure(EditError::InvalidSelection, "The Sound's input feeds the instance's replaceable input"));
        }
        for (auto& parameter : expanded.definition.exposedParameters)
        {
            std::vector<ProjectSoundParameterBinding> bindings;
            for (const auto& binding : parameter.bindings)
            {
                if (binding.target != ProjectSoundParameterTarget::NodeParameter || binding.nodeId != instanceId)
                {
                    bindings.push_back(binding);
                    continue;
                }
                const auto innerParameter =
                    std::ranges::find(inner.exposedParameters, binding.parameterId, &ProjectSoundExposedParameter::parameterId);
                if (innerParameter == inner.exposedParameters.end())
                    continue;
                for (auto innerBinding : innerParameter->bindings)
                {
                    if (innerBinding.target == ProjectSoundParameterTarget::NodeParameter)
                        innerBinding.nodeId = inlinedIds.at(innerBinding.nodeId);
                    bindings.push_back(std::move(innerBinding));
                }
            }
            parameter.bindings = std::move(bindings);
        }
    }
    expanded.definition.graph = editor->document().graph;
    if (const auto* layout = findProjectStructureLayout(editor->document()))
        expanded.layout = layout->nodes;
    return expanded;
}

// The Event Values a Sound binds its own parameters to: a field reading one
// reads the Sound's parameter, not the global map's name.
[[nodiscard]] std::vector<std::string> soundOwnEventValues(const ProjectSoundDefinition& definition)
{
    std::vector<std::string> names;
    for (const auto& parameter : definition.exposedParameters)
    {
        for (const auto& binding : parameter.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::EventValue && !binding.eventName.empty())
                names.push_back(binding.eventName);
        }
    }
    return names;
}

// Each Virtual Control the expansion's Event Sources
// added starts at the value of its source that gives the Event Value's own
// control its value, so the expanded Sound plays at first as it did.
void carryEventSourceValues(const ProjectDocument& before, ProjectDocument& after,
    const std::vector<GlobalMapEventSourceRewrite>& rewrites)
{
    for (const auto& rewrite : rewrites)
    {
        const auto original = std::ranges::find(before.virtualControls, rewrite.eventName, &VirtualControl::eventName);
        if (original == before.virtualControls.end() || original->domain != talas::expr::ValueDomain::Scalar
            || std::ranges::find(before.virtualControls, rewrite.sourceEventName, &VirtualControl::eventName)
                != before.virtualControls.end())
            continue;
        const auto added = std::ranges::find(after.virtualControls, rewrite.sourceEventName, &VirtualControl::eventName);
        if (added == after.virtualControls.end() || added->domain != talas::expr::ValueDomain::Scalar)
            continue;
        if (const auto value = globalMapSourceValue(rewrite.entry, original->value))
            added->value = std::clamp(*value, added->minimum, added->maximum);
    }
}

// A signal flow with the Sound instance expanded as Action > Expand does
//: inlined by a scratch editor, then the
// fields of the Sounds it gave writing the global map's Event Sources.
struct ExpandedInstance
{
    EditResult result;
    ProjectDocument document;
    std::vector<GlobalMapEventSourceRewrite> rewrites;
};

[[nodiscard]] ExpandedInstance expandedInstance(const ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::optional<std::string_view> definitionId, std::string_view instanceId)
{
    ExpandedInstance expanded;
    std::optional<ProjectEditor> scratch;
    try
    {
        scratch.emplace(document, registry);
    }
    catch (const std::invalid_argument& error)
    {
        expanded.result = failure(EditError::InvalidSoundDefinition, error.what());
        return expanded;
    }
    expanded.result = definitionId.has_value() ? scratch->inlineSoundInstance(*definitionId, instanceId)
                                               : scratch->inlineSoundInstance(instanceId);
    if (!expanded.result.ok())
        return expanded;
    expanded.document = scratch->document();
    auto* graph = &expanded.document.graph;
    const auto* before = &document.graph;
    std::vector<std::string> kept;
    if (definitionId.has_value())
    {
        auto* definition = findSoundDefinition(expanded.document, *definitionId);
        const auto* original = findSoundDefinition(document, *definitionId);
        if (definition == nullptr || original == nullptr)
            return expanded;
        graph = &definition->graph;
        before = &original->graph;
        kept = soundOwnEventValues(*definition);
    }
    std::vector<std::string> added;
    for (const auto& node : graph->nodes)
    {
        if (std::ranges::find(before->nodes, node.instanceId, &ProjectNode::instanceId) == before->nodes.end())
            added.push_back(node.instanceId);
    }
    expanded.rewrites = writeGlobalMapEventSources(document, *graph, kept, &added);
    return expanded;
}
}

EditResult ProjectEditor::expandSoundInstance(std::string_view instanceId)
{
    auto expanded = expandedInstance(document_, registry_, std::nullopt, instanceId);
    if (!expanded.result.ok())
        return expanded.result;
    if (!expanded.rewrites.empty())
    {
        const auto validation = projectValidationFailure(expanded.document, registry_);
        if (!validation.ok())
            return validation;
        const auto sync = synchronizeProjectEventValueControls(expanded.document, registry_);
        carryEventSourceValues(document_, expanded.document, expanded.rewrites);
        expanded.result = eventValueSyncOutcome(sync);
    }
    commit(std::move(expanded.document));
    return expanded.result;
}

EditResult ProjectEditor::expandSoundInstance(std::string_view definitionId, std::string_view instanceId)
{
    auto expanded = expandedInstance(document_, registry_, definitionId, instanceId);
    if (!expanded.result.ok())
        return expanded.result;
    if (!expanded.rewrites.empty())
    {
        const auto validation = projectValidationFailure(expanded.document, registry_);
        if (!validation.ok())
            return validation;
        const auto sync = synchronizeProjectEventValueControls(expanded.document, registry_);
        carryEventSourceValues(document_, expanded.document, expanded.rewrites);
        expanded.result = eventValueSyncOutcome(sync);
    }
    commit(std::move(expanded.document));
    return expanded.result;
}

EditResult ProjectEditor::expandSoundDefinition(
    std::string_view definitionId,
    std::string newDefinitionId,
    std::string displayName)
{
    const auto* source = findSoundDefinition(document_, definitionId);
    if (source == nullptr)
        return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string(definitionId));
    if (newDefinitionId.empty() || displayName.empty())
        return failure(EditError::InvalidSoundDefinition, "Sound definition id and name cannot be empty");
    if (findSoundDefinition(document_, newDefinitionId) != nullptr)
        return failure(EditError::DuplicateSoundDefinition, "Duplicate Sound definition id: " + newDefinitionId);
    std::vector<std::string> instances;
    for (const auto& node : source->graph.nodes)
    {
        if (node.soundDefinitionId.has_value())
            instances.push_back(node.instanceId);
    }
    if (instances.empty())
        return failure(EditError::InvalidSelection, "The Sound holds no Sound of a class to expand");
    auto expanded = expandedSound(document_, registry_, *source, instances);
    if (!expanded.result.ok())
        return expanded.result;
    // The expansion's fields show the global map's Event
    // Sources where the Sound's had its Event Values.
    const auto rewrites = writeGlobalMapEventSources(
        document_, expanded.definition.graph, soundOwnEventValues(expanded.definition));
    ProjectDocument candidate = document_;
    expanded.definition.definitionId = newDefinitionId;
    expanded.definition.displayName = std::move(displayName);
    candidate.soundDefinitions.push_back(std::move(expanded.definition));
    // The expansion sits beside the original, in its collection.
    for (auto& collection : candidate.soundCollections)
    {
        if (std::ranges::find(collection.soundIds, definitionId) != collection.soundIds.end())
            collection.soundIds.push_back(newDefinitionId);
    }
    candidate.presentation.structureLayouts.push_back({std::move(newDefinitionId), std::move(expanded.layout)});
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    carryEventSourceValues(document_, candidate, rewrites);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::inlineSoundInstance(
    std::string_view definitionId,
    std::string_view instanceId)
{
    const auto* source = findSoundDefinition(document_, definitionId);
    if (source == nullptr)
        return failure(EditError::UnknownSoundDefinition, "Unknown Sound definition: " + std::string(definitionId));
    auto expanded = expandedSound(document_, registry_, *source, {std::string{instanceId}});
    if (!expanded.result.ok())
        return expanded.result;
    ProjectDocument candidate = document_;
    *findSoundDefinition(candidate, definitionId) = std::move(expanded.definition);
    if (auto* layout = findProjectStructureLayout(candidate, definitionId))
        layout->nodes = std::move(expanded.layout);
    else
        candidate.presentation.structureLayouts.push_back({std::string{definitionId}, std::move(expanded.layout)});
    reconcileProjectPresentation(candidate);
    const auto validation = projectValidationFailure(candidate, registry_);
    if (!validation.ok())
        return validation;
    const auto sync = synchronizeProjectEventValueControls(candidate, registry_);
    commit(std::move(candidate));
    return eventValueSyncOutcome(sync);
}

EditResult ProjectEditor::addSnapshotBank(
    ProjectSnapshotBank bank)
{
    if (std::ranges::any_of(
            document_.snapshotLibrary.banks,
            [&](const auto& candidate) {
                return candidate.bankId == bank.bankId;
            }))
    {
        return failure(
            EditError::DuplicateSnapshot,
            "Duplicate snapshot bank identity: " + bank.bankId);
    }
    auto candidate = document_;
    candidate.snapshotLibrary.banks.push_back(std::move(bank));
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::renameSnapshotBank(
    std::string_view bankId,
    std::string displayName)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    bank->displayName = std::move(displayName);
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::cloneSnapshotBank(
    std::string_view sourceBankId,
    std::string newBankId,
    std::string displayName)
{
    if (std::ranges::any_of(
            document_.snapshotLibrary.banks,
            [&](const auto& bank) {
                return bank.bankId == newBankId;
            }))
    {
        return failure(
            EditError::DuplicateSnapshot,
            "Duplicate snapshot bank identity: " + newBankId);
    }
    const auto source = std::ranges::find_if(
        document_.snapshotLibrary.banks,
        [&](const auto& bank) {
            return bank.bankId == sourceBankId;
        });
    if (source == document_.snapshotLibrary.banks.end())
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(sourceBankId));
    }
    auto clone = *source;
    clone.bankId = std::move(newBankId);
    clone.displayName = std::move(displayName);
    auto candidate = document_;
    candidate.snapshotLibrary.banks.push_back(std::move(clone));
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSnapshotBank(
    std::string_view bankId)
{
    auto candidate = document_;
    const auto found = std::ranges::find_if(
        candidate.snapshotLibrary.banks,
        [&](const auto& bank) {
            return bank.bankId == bankId;
        });
    if (found == candidate.snapshotLibrary.banks.end())
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    candidate.snapshotLibrary.banks.erase(found);
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::moveSnapshotBank(
    std::string_view bankId,
    std::size_t destinationIndex)
{
    auto candidate = document_;
    auto& banks = candidate.snapshotLibrary.banks;
    if (destinationIndex >= banks.size())
    {
        return failure(
            EditError::InvalidSnapshot,
            "Snapshot bank destination index is out of range");
    }
    const auto found = std::ranges::find_if(
        banks,
        [&](const auto& bank) {
            return bank.bankId == bankId;
        });
    if (found == banks.end())
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    auto moved = std::move(*found);
    banks.erase(found);
    banks.insert(
        banks.begin() + static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(moved));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setSnapshotBankSmoothing(
    std::string_view bankId,
    double smoothingSeconds)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    bank->smoothingSeconds = smoothingSeconds;
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::storeSnapshot(
    std::string_view bankId,
    ProjectSnapshot snapshot)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    if (findSnapshot(*bank, snapshot.snapshotId) != nullptr)
    {
        return failure(
            EditError::DuplicateSnapshot,
            "Duplicate snapshot identity: " + snapshot.snapshotId);
    }
    bank->snapshots.push_back(std::move(snapshot));
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::initializeSnapshot(
    std::string_view bankId,
    std::string snapshotId,
    std::string displayName)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    if (findSnapshot(*bank, snapshotId) != nullptr)
    {
        return failure(
            EditError::DuplicateSnapshot,
            "Duplicate snapshot identity: " + snapshotId);
    }

    const auto resolution = resolveSnapshotScopeTargets(
        candidate,
        registry_,
        bank->scope);
    if (!resolution.ok())
    {
        const auto& diagnostic = resolution.diagnostics.front();
        return failure(
            EditError::InvalidSnapshot,
            diagnostic.path + ": " + diagnostic.message);
    }
    const auto initialized = initializeSnapshotScopeValues(
        resolution.targets);
    if (!initialized.ok())
    {
        const auto& diagnostic = initialized.diagnostics.front();
        return failure(
            EditError::InvalidSnapshot,
            diagnostic.path + ": " + diagnostic.message);
    }

    ProjectSnapshot snapshot;
    snapshot.snapshotId = std::move(snapshotId);
    snapshot.displayName = std::move(displayName);
    snapshot.values = initialized.values;
    bank->snapshots.push_back(std::move(snapshot));
    for (const auto& target : resolution.targets)
    {
        if (!target.initialRandomRange.has_value())
            continue;
        const auto policy = std::ranges::find_if(
            bank->parameterPolicies,
            [&](const auto& candidatePolicy) {
                return candidatePolicy.target == target.target;
            });
        if (policy == bank->parameterPolicies.end())
        {
            bank->parameterPolicies.push_back({
                target.target,
                false,
                target.initialRandomRange,
            });
        }
    }

    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::renameSnapshot(
    std::string_view bankId,
    std::string_view snapshotId,
    std::string displayName)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    auto* snapshot = findSnapshot(*bank, snapshotId);
    if (snapshot == nullptr)
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot: " + std::string(snapshotId));
    }
    snapshot->displayName = std::move(displayName);
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::cloneSnapshot(
    std::string_view bankId,
    std::string_view sourceSnapshotId,
    std::string newSnapshotId,
    std::string displayName)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    if (findSnapshot(*bank, newSnapshotId) != nullptr)
    {
        return failure(
            EditError::DuplicateSnapshot,
            "Duplicate snapshot identity: " + newSnapshotId);
    }
    const auto* source = findSnapshot(*bank, sourceSnapshotId);
    if (source == nullptr)
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot: " + std::string(sourceSnapshotId));
    }
    auto clone = *source;
    clone.snapshotId = std::move(newSnapshotId);
    clone.displayName = std::move(displayName);
    bank->snapshots.push_back(std::move(clone));
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSnapshot(
    std::string_view bankId,
    std::string_view snapshotId)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    const auto found = std::ranges::find_if(
        bank->snapshots,
        [&](const auto& snapshot) {
            return snapshot.snapshotId == snapshotId;
        });
    if (found == bank->snapshots.end())
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot: " + std::string(snapshotId));
    }
    bank->snapshots.erase(found);
    const auto liveSets = liveSetValidationFailure(candidate);
    if (!liveSets.ok())
        return liveSets;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::moveSnapshot(
    std::string_view bankId,
    std::string_view snapshotId,
    std::size_t destinationIndex)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    if (destinationIndex >= bank->snapshots.size())
    {
        return failure(
            EditError::InvalidSnapshot,
            "Snapshot destination index is out of range");
    }
    const auto found = std::ranges::find_if(
        bank->snapshots,
        [&](const auto& snapshot) {
            return snapshot.snapshotId == snapshotId;
        });
    if (found == bank->snapshots.end())
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot: " + std::string(snapshotId));
    }
    auto moved = std::move(*found);
    bank->snapshots.erase(found);
    bank->snapshots.insert(
        bank->snapshots.begin()
            + static_cast<std::ptrdiff_t>(destinationIndex),
        std::move(moved));
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::setSnapshotParameterPolicy(
    std::string_view bankId,
    SnapshotParameterPolicy policy)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    const auto found = std::ranges::find_if(
        bank->parameterPolicies,
        [&](const auto& candidatePolicy) {
            return candidatePolicy.target == policy.target;
        });
    if (found == bank->parameterPolicies.end())
        bank->parameterPolicies.push_back(std::move(policy));
    else
        *found = std::move(policy);
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::removeSnapshotParameterPolicy(
    std::string_view bankId,
    const SnapshotTargetIdentity& target)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    const auto found = std::ranges::find_if(
        bank->parameterPolicies,
        [&](const auto& policy) {
            return policy.target == target;
        });
    if (found == bank->parameterPolicies.end())
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot parameter policy");
    }
    bank->parameterPolicies.erase(found);
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::captureSnapshotValues(
    std::string_view bankId,
    std::string_view snapshotId,
    std::vector<SnapshotValueEntry> values)
{
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    auto* snapshot = findSnapshot(*bank, snapshotId);
    if (snapshot == nullptr)
    {
        return failure(
            EditError::UnknownSnapshot,
            "Unknown snapshot: " + std::string(snapshotId));
    }
    snapshot->values = std::move(values);
    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

EditResult ProjectEditor::migrateSnapshotTarget(
    std::string_view bankId,
    const SnapshotTargetIdentity& staleTarget,
    SnapshotTargetIdentity replacementTarget)
{
    if (staleTarget == replacementTarget)
    {
        return failure(
            EditError::InvalidSnapshot,
            "Snapshot target migration requires a different replacement identity");
    }
    auto candidate = document_;
    auto* bank = findSnapshotBank(candidate, bankId);
    if (bank == nullptr)
    {
        return failure(
            EditError::UnknownSnapshotBank,
            "Unknown snapshot bank: " + std::string(bankId));
    }
    const auto resolution = resolveSnapshotScopeTargets(
        candidate,
        registry_,
        bank->scope);
    if (!resolution.ok())
    {
        const auto& diagnostic = resolution.diagnostics.front();
        return failure(
            EditError::InvalidSnapshot,
            diagnostic.path + ": " + diagnostic.message);
    }
    const auto replacement = std::ranges::find_if(
        resolution.targets,
        [&](const auto& target) {
            return target.target == replacementTarget;
        });
    if (replacement == resolution.targets.end())
    {
        return failure(
            EditError::InvalidSnapshot,
            "Replacement snapshot target does not resolve exactly in the bank scope");
    }

    auto replaced = false;
    const auto stalePolicy = std::ranges::find_if(
        bank->parameterPolicies,
        [&](const auto& policy) {
            return policy.target == staleTarget;
        });
    if (stalePolicy != bank->parameterPolicies.end())
    {
        const auto replacementPolicy = std::ranges::find_if(
            bank->parameterPolicies,
            [&](const auto& policy) {
                return policy.target == replacementTarget;
            });
        if (replacementPolicy != bank->parameterPolicies.end())
        {
            return failure(
                EditError::DuplicateSnapshot,
                "Replacement snapshot policy target already exists");
        }
        if (stalePolicy->range.has_value()
            && replacement->valueKind != SnapshotValueKind::Number)
        {
            return failure(
                EditError::InvalidSnapshot,
                "A numeric range cannot migrate to a non-numeric snapshot target");
        }
        stalePolicy->target = replacementTarget;
        replaced = true;
    }

    for (auto& snapshot : bank->snapshots)
    {
        const auto staleValue = std::ranges::find_if(
            snapshot.values,
            [&](const auto& entry) {
                return entry.target == staleTarget;
            });
        if (staleValue == snapshot.values.end())
            continue;
        if (std::ranges::any_of(
                snapshot.values,
                [&](const auto& entry) {
                    return entry.target == replacementTarget;
                }))
        {
            return failure(
                EditError::DuplicateSnapshot,
                "Replacement snapshot value target already exists in snapshot "
                    + snapshot.snapshotId);
        }
        auto migratedValue = *staleValue;
        migratedValue.target = replacementTarget;
        const auto compatible = captureSnapshotScopeValues(
            std::vector<SnapshotScopeTarget>{*replacement},
            std::vector<SnapshotValueEntry>{migratedValue});
        if (!compatible.ok())
        {
            return failure(
                EditError::InvalidSnapshot,
                compatible.diagnostics.front().message);
        }
        *staleValue = std::move(migratedValue);
        replaced = true;
    }
    if (!replaced)
    {
        return failure(
            EditError::InvalidSnapshot,
            "Stale snapshot target is not present in the selected bank");
    }

    const auto validation = validateSnapshotCandidate(candidate);
    if (!validation.ok())
        return validation;
    commit(std::move(candidate));
    return success();
}

void ProjectEditor::beginHistoryTransaction()
{
    closeHistoryTransaction();
    transactionBefore_ = document_;
    transactionWindow_ = historyWindow_;
    transactionChanged_ = false;
    transactionOwner_ = 0;
    valueTransaction_ = false;
}

void ProjectEditor::endHistoryTransaction()
{
    if (transactionOwner_ == 0)
        closeHistoryTransaction();
}

void ProjectEditor::cancelHistoryTransaction()
{
    if (transactionOwner_ == 0)
        rollbackHistoryTransaction();
}

ProjectEditor::HistoryTransactionToken
ProjectEditor::beginOwnedHistoryTransaction()
{
    return openOwnedHistoryTransaction(false);
}

ProjectEditor::HistoryTransactionToken
ProjectEditor::beginValueHistoryTransaction()
{
    return openOwnedHistoryTransaction(true);
}

void ProjectEditor::endOwnedHistoryTransaction(
    HistoryTransactionToken token)
{
    if (token == 0 || transactionOwner_ != token)
        return;
    closeHistoryTransaction();
}

void ProjectEditor::cancelOwnedHistoryTransaction(
    HistoryTransactionToken token)
{
    if (token == 0 || transactionOwner_ != token)
        return;
    rollbackHistoryTransaction();
}

bool ProjectEditor::ownsHistoryTransaction(
    HistoryTransactionToken token) const noexcept
{
    return token != 0
        && transactionBefore_.has_value()
        && transactionOwner_ == token;
}

bool ProjectEditor::hasValueHistoryTransaction() const noexcept
{
    return transactionBefore_.has_value() && valueTransaction_;
}

ProjectEditor::HistoryTransactionToken
ProjectEditor::openOwnedHistoryTransaction(bool valueEdits)
{
    closeHistoryTransaction();
    auto token = nextTransactionToken_++;
    if (token == 0)
        token = nextTransactionToken_++;
    transactionBefore_ = document_;
    transactionWindow_ = historyWindow_;
    transactionChanged_ = false;
    transactionOwner_ = token;
    valueTransaction_ = valueEdits;
    return token;
}

void ProjectEditor::closeHistoryTransaction()
{
    if (!transactionBefore_.has_value())
        return;

    if (transactionChanged_)
    {
        pushUndo(std::move(*transactionBefore_), transactionWindow_);
        clearRedoHistory();
    }
    transactionBefore_.reset();
    transactionChanged_ = false;
    transactionOwner_ = 0;
    valueTransaction_ = false;
}

void ProjectEditor::rollbackHistoryTransaction()
{
    if (!transactionBefore_.has_value())
        return;
    advanceDocumentAuthority();
    document_ = std::move(*transactionBefore_);
    transactionBefore_.reset();
    transactionChanged_ = false;
    transactionOwner_ = 0;
    valueTransaction_ = false;
}

bool ProjectEditor::canUndo() const
{
    return transactionChanged_ || !undoHistory_.empty();
}

bool ProjectEditor::canRedo() const
{
    return !transactionChanged_ && !redoHistory_.empty();
}

EditResult ProjectEditor::undo()
{
    closeHistoryTransaction();
    if (undoHistory_.empty())
        return failure(EditError::NoUndoAvailable, "No project edit is available to undo");

    const auto window = newestWindow(undoWindows_, undoHistory_.size());
    if (redoHistory_.size() == historyLimit)
        redoHistory_.erase(redoHistory_.begin());
    redoHistory_.push_back(std::move(document_));
    if (redoWindows_.size() >= redoHistory_.size() && !redoWindows_.empty())
        redoWindows_.erase(redoWindows_.begin());
    if (redoWindows_.size() < redoWindows_.capacity())
        redoWindows_.push_back(window);
    advanceDocumentAuthority();
    document_ = std::move(undoHistory_.back());
    undoHistory_.pop_back();
    if (undoWindows_.size() > undoHistory_.size())
        undoWindows_.pop_back();
    return success();
}

EditResult ProjectEditor::redo()
{
    closeHistoryTransaction();
    if (redoHistory_.empty())
        return failure(EditError::NoRedoAvailable, "No project edit is available to redo");

    const auto window = newestWindow(redoWindows_, redoHistory_.size());
    pushUndo(std::move(document_), window);
    advanceDocumentAuthority();
    document_ = std::move(redoHistory_.back());
    redoHistory_.pop_back();
    if (redoWindows_.size() > redoHistory_.size())
        redoWindows_.pop_back();
    return success();
}

void ProjectEditor::setHistoryWindow(HistoryWindow window) noexcept
{
    historyWindow_ = window;
}

ProjectEditor::HistoryWindow ProjectEditor::historyWindow() const noexcept
{
    return historyWindow_;
}

// An edit made in no window in particular (0) is undone in any window.
bool ProjectEditor::canUndoIn(HistoryWindow window) const noexcept
{
    const auto newest = transactionChanged_
        ? transactionWindow_
        : newestWindow(undoWindows_, undoHistory_.size());
    return (transactionChanged_ || !undoHistory_.empty()) && (newest == window || newest == 0);
}

bool ProjectEditor::canRedoIn(HistoryWindow window) const noexcept
{
    const auto newest = newestWindow(redoWindows_, redoHistory_.size());
    return canRedo() && (newest == window || newest == 0);
}

EditResult ProjectEditor::undoIn(HistoryWindow window)
{
    if (canUndo() && !canUndoIn(window))
        return failure(EditError::NoUndoAvailable,
            "The newest edit was made in another window: bring that window to the front to undo it");
    return undo();
}

EditResult ProjectEditor::redoIn(HistoryWindow window)
{
    if (canRedo() && !canRedoIn(window))
        return failure(EditError::NoRedoAvailable,
            "The edit to redo was made in another window: bring that window to the front to redo it");
    return redo();
}

std::uint64_t ProjectEditor::documentReplacements() const noexcept
{
    return documentReplacements_;
}

void ProjectEditor::commit(
    ProjectDocument candidate,
    HistoryTransactionToken transactionToken)
{
    reconcileProjectPresentation(candidate);
    if (transactionBefore_.has_value()
        && transactionOwner_ != 0
        && (valueTransaction_ || transactionToken != transactionOwner_))
    {
        closeHistoryTransaction();
    }
    if (transactionBefore_.has_value())
    {
        advanceDocumentAuthority();
        document_ = std::move(candidate);
        transactionChanged_ = true;
        return;
    }

    pushUndo(std::move(document_));
    advanceDocumentAuthority();
    document_ = std::move(candidate);
    clearRedoHistory();
}

void ProjectEditor::pushUndo(ProjectDocument snapshot)
{
    pushUndo(std::move(snapshot), historyWindow_);
}

void ProjectEditor::pushUndo(ProjectDocument snapshot, HistoryWindow window)
{
    if (undoHistory_.size() == historyLimit)
        undoHistory_.erase(undoHistory_.begin());
    undoHistory_.push_back(std::move(snapshot));
    pushUndoWindow(window);
}

void ProjectEditor::pushUndoWindow(HistoryWindow window) noexcept
{
    // undoHistory_ has just taken its newest document, dropping its oldest
    // at the limit; the tags follow it.
    while (!undoWindows_.empty() && undoWindows_.size() >= undoHistory_.size())
        undoWindows_.erase(undoWindows_.begin());
    if (undoWindows_.size() < undoWindows_.capacity())
        undoWindows_.push_back(window);
}

void ProjectEditor::clearRedoHistory() noexcept
{
    redoHistory_.clear();
    redoWindows_.clear();
}

talas::core::ValidationResult ProjectEditor::validateForRender() const
{
    return validateProjectDocument(document_, registry_);
}
}
