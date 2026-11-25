#include "talas/project/ProjectSnapshots.h"

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>
#include <ranges>
#include <string_view>

namespace talas::project
{
namespace
{
struct GraphLayer
{
    const ProjectGraph* graph = nullptr;
    const ProjectSoundDefinition* definition = nullptr;
    const ProjectNode* instance = nullptr;
};

struct ResolvedSnapshotTarget
{
    talas::core::ParameterKind kind = talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    const std::vector<std::string>* choices = nullptr;
};

[[nodiscard]] const ProjectSoundDefinition* findDefinition(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end()
        ? nullptr
        : &*found;
}

[[nodiscard]] const ProjectNode* findNode(
    const ProjectGraph& graph,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        graph.nodes,
        [&](const auto& node) {
            return node.instanceId == nodeId;
        });
    return found == graph.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<GraphLayer> resolveGraphLayer(
    const ProjectDocument& document,
    const std::vector<std::string>& instancePath)
{
    GraphLayer layer{&document.graph, nullptr, nullptr};
    for (const auto& instanceId : instancePath)
    {
        const auto* instance = findNode(*layer.graph, instanceId);
        if (instance == nullptr || !instance->soundDefinitionId.has_value())
            return std::nullopt;
        const auto* definition = findDefinition(
            document,
            *instance->soundDefinitionId);
        if (definition == nullptr)
            return std::nullopt;
        layer = {&definition->graph, definition, instance};
    }
    return layer;
}

[[nodiscard]] std::string domainName(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
        return "scalar";
    case talas::expr::ValueDomain::Hertz:
        return "hertz";
    case talas::expr::ValueDomain::Seconds:
        return "seconds";
    case talas::expr::ValueDomain::NoteNumber:
        return "noteNumber";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    }
    return "unknown";
}

void appendIdentitySegment(
    std::string& output,
    char kind,
    std::string_view value)
{
    if (!output.empty())
        output.push_back('/');
    output.push_back(kind);
    output += std::to_string(value.size());
    output.push_back(':');
    output += value;
}

struct ParsedIdentitySegment
{
    char kind = '\0';
    std::string_view value;
};

[[nodiscard]] bool isAsciiIdentifierStart(char value) noexcept
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool isSnapshotIdentifier(
    std::string_view value) noexcept
{
    return !value.empty()
        && value.size() <= maximumSnapshotIdentifierBytes
        && isAsciiIdentifierStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [](const auto character) {
                return isAsciiIdentifierStart(character)
                    || (character >= '0' && character <= '9')
                    || character == '-';
            });
}

[[nodiscard]] bool isEventValueIdentifier(
    std::string_view value) noexcept
{
    return !value.empty()
        && value.size() <= maximumSnapshotIdentifierBytes
        && isAsciiIdentifierStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [](const auto character) {
                return isAsciiIdentifierStart(character)
                    || (character >= '0' && character <= '9');
            });
}

[[nodiscard]] std::optional<ParsedIdentitySegment>
consumeIdentitySegment(
    std::string_view identity,
    std::size_t& offset) noexcept
{
    if (offset != 0)
    {
        if (offset >= identity.size() || identity[offset] != '/')
            return std::nullopt;
        ++offset;
    }
    if (offset >= identity.size())
        return std::nullopt;
    const auto kind = identity[offset++];
    const auto lengthStart = offset;
    while (offset < identity.size()
           && identity[offset] >= '0'
           && identity[offset] <= '9')
    {
        ++offset;
    }
    if (lengthStart == offset
        || offset >= identity.size()
        || identity[offset] != ':'
        || (identity[lengthStart] == '0'
            && offset - lengthStart != 1))
    {
        return std::nullopt;
    }
    std::size_t length = 0;
    const auto [end, error] = std::from_chars(
        identity.data() + lengthStart,
        identity.data() + offset,
        length);
    if (error != std::errc{}
        || end != identity.data() + offset)
    {
        return std::nullopt;
    }
    ++offset;
    if (length > identity.size() - offset)
        return std::nullopt;
    const auto value = identity.substr(offset, length);
    offset += length;
    return ParsedIdentitySegment{kind, value};
}

class DiagnosticCollector
{
public:
    void append(
        SnapshotDiagnosticCode code,
        std::string path,
        const SnapshotTargetIdentity* target,
        std::string message,
        SnapshotDiagnosticSeverity severity =
            SnapshotDiagnosticSeverity::Warning)
    {
        if (diagnostics_.size() < maximumSnapshotDiagnosticCount)
        {
            diagnostics_.push_back({
                code,
                severity,
                std::move(path),
                target == nullptr
                    ? std::string{}
                    : snapshotTargetIdentityString(*target),
                std::move(message),
            });
            return;
        }
        if (diagnostics_.size() == maximumSnapshotDiagnosticCount)
        {
            diagnostics_.push_back({
                SnapshotDiagnosticCode::CapacityExceeded,
                SnapshotDiagnosticSeverity::Warning,
                "$.snapshotLibrary",
                {},
                "Additional snapshot diagnostics were truncated",
            });
        }
    }

    [[nodiscard]] std::vector<SnapshotDiagnostic> take()
    {
        return std::move(diagnostics_);
    }

private:
    std::vector<SnapshotDiagnostic> diagnostics_;
};

void appendBoundedOperationDiagnostic(
    std::vector<SnapshotDiagnostic>& diagnostics,
    SnapshotDiagnostic diagnostic)
{
    if (diagnostics.size() < maximumSnapshotDiagnosticCount)
    {
        diagnostics.push_back(std::move(diagnostic));
        return;
    }
    if (diagnostics.size() == maximumSnapshotDiagnosticCount)
    {
        diagnostics.push_back({
            SnapshotDiagnosticCode::CapacityExceeded,
            SnapshotDiagnosticSeverity::Warning,
            "$.snapshotOperation",
            {},
            "Additional snapshot operation diagnostics were truncated",
        });
    }
}

[[nodiscard]] std::optional<SnapshotValueKind> snapshotValueKind(
    talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression:
        return SnapshotValueKind::Number;
    case talas::core::ParameterKind::Choice:
        return SnapshotValueKind::Choice;
    case talas::core::ParameterKind::Boolean:
        return SnapshotValueKind::Boolean;
    case talas::core::ParameterKind::Resource:
    case talas::core::ParameterKind::Text:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SnapshotValue> evaluateNumericDefault(
    std::string_view expression,
    talas::expr::ValueDomain domain,
    const talas::expr::EvaluationContext& context,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    if (expression.empty())
        return std::nullopt;
    const auto parsed = talas::expr::parse(expression, language, definitions);
    if (!parsed.ok())
        return std::nullopt;
    const auto evaluated = talas::expr::evaluate(
        *parsed.expression,
        context,
        talas::expr::EvaluationOptions{domain});
    if (!evaluated.ok() || !std::isfinite(evaluated.value.value))
        return std::nullopt;
    SnapshotValue value;
    value.kind = SnapshotValueKind::Number;
    value.domain = domain;
    value.number = evaluated.value.value;
    return value;
}

[[nodiscard]] SnapshotScopeTarget makeScopeTarget(
    SnapshotTargetIdentity identity,
    const talas::core::ParameterDescriptor& descriptor,
    const talas::expr::EvaluationContext& context,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    SnapshotScopeTarget target;
    target.target = std::move(identity);
    target.domain = descriptor.domain;
    target.choices = descriptor.choices;
    target.valueKind = *snapshotValueKind(descriptor.kind);
    switch (descriptor.kind)
    {
    case talas::core::ParameterKind::Expression:
        target.defaultValue = evaluateNumericDefault(
            descriptor.defaultExpression,
            descriptor.domain,
            context,
            language,
            definitions);
        if (!target.defaultValue.has_value())
        {
            target.defaultUnavailableReason =
                "Numeric descriptor default cannot be evaluated to a finite value";
        }
        break;
    case talas::core::ParameterKind::Choice:
    {
        SnapshotValue value;
        value.kind = SnapshotValueKind::Choice;
        value.choice = descriptor.defaultChoice;
        target.defaultValue = std::move(value);
        break;
    }
    case talas::core::ParameterKind::Boolean:
    {
        if (descriptor.defaultBoolean.has_value())
        {
            SnapshotValue value;
            value.kind = SnapshotValueKind::Boolean;
            value.boolean = *descriptor.defaultBoolean;
            target.defaultValue = value;
        }
        else
        {
            target.defaultUnavailableReason =
                "Boolean descriptor default is unavailable";
        }
        break;
    }
    case talas::core::ParameterKind::Resource:
    case talas::core::ParameterKind::Text:
        break;
    }
    return target;
}

[[nodiscard]] SnapshotScopeTarget makeScopeTarget(
    SnapshotTargetIdentity identity,
    const ProjectSoundExposedParameter& exposed,
    const talas::expr::EvaluationContext& context,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    SnapshotScopeTarget target;
    target.target = std::move(identity);
    target.domain = exposed.domain;
    target.choices = exposed.choices;
    target.valueKind = *snapshotValueKind(exposed.kind);
    switch (exposed.kind)
    {
    case talas::core::ParameterKind::Expression:
        if (exposed.defaultValue.expression.has_value())
        {
            target.defaultValue = evaluateNumericDefault(
                *exposed.defaultValue.expression,
                exposed.domain,
                context,
                language,
                definitions);
        }
        if (!target.defaultValue.has_value())
        {
            target.defaultUnavailableReason =
                "Numeric Sound interface default cannot be evaluated to a finite value";
        }
        break;
    case talas::core::ParameterKind::Choice:
        if (exposed.defaultValue.choice.has_value())
        {
            SnapshotValue value;
            value.kind = SnapshotValueKind::Choice;
            value.choice = *exposed.defaultValue.choice;
            target.defaultValue = std::move(value);
        }
        else
        {
            target.defaultUnavailableReason =
                "Choice Sound interface default is unavailable";
        }
        break;
    case talas::core::ParameterKind::Boolean:
        if (exposed.defaultValue.boolean.has_value())
        {
            SnapshotValue value;
            value.kind = SnapshotValueKind::Boolean;
            value.boolean = *exposed.defaultValue.boolean;
            target.defaultValue = value;
        }
        else
        {
            target.defaultUnavailableReason =
                "Boolean Sound interface default is unavailable";
        }
        break;
    case talas::core::ParameterKind::Resource:
    case talas::core::ParameterKind::Text:
        break;
    }
    return target;
}

void appendVariableScopeTargets(
    const ProjectDocument& document,
    ProjectVariableOwnerKind ownerKind,
    std::string_view definitionId,
    const std::vector<std::string>& instancePath,
    std::vector<SnapshotScopeTarget>& targets)
{
    for (const auto& declaration : document.variableLibrary.variables)
    {
        if (!declaration.snapshotEligible
            || declaration.owner.kind != ownerKind
            || (ownerKind == ProjectVariableOwnerKind::SoundDefinition
                && declaration.owner.definitionId != definitionId))
        {
            continue;
        }

        SnapshotScopeTarget target;
        target.target.kind = SnapshotTargetKind::Variable;
        target.target.variableOwnerKind = ownerKind
                == ProjectVariableOwnerKind::WorkspaceSession
            ? SnapshotVariableOwnerKind::WorkspaceSession
            : SnapshotVariableOwnerKind::Project;
        target.target.instancePath = instancePath;
        target.target.definitionId = ownerKind
                == ProjectVariableOwnerKind::SoundDefinition
            ? std::string{definitionId}
            : std::string{};
        target.target.variableId = declaration.variableId;
        target.valueKind = SnapshotValueKind::Number;
        target.domain = declaration.domain;

        SnapshotValue value;
        value.kind = SnapshotValueKind::Number;
        value.domain = declaration.domain;
        value.number = declaration.initialValue;
        target.defaultValue = value;
        targets.push_back(std::move(target));
    }
}

void appendSoundVariableScopeTargets(
    const ProjectDocument& document,
    ProjectVariableOwnerKind ownerKind,
    std::string_view definitionId,
    const std::vector<std::string>& instancePath,
    std::vector<SnapshotScopeTarget>& targets)
{
    for (const auto& declaration : document.soundVariableLibrary.variables)
    {
        if (!declaration.snapshotEligible
            || declaration.owner.kind != ownerKind
            || (ownerKind == ProjectVariableOwnerKind::SoundDefinition
                && declaration.owner.definitionId != definitionId)
            || declaration.candidateDefinitionIds.empty()
            || declaration.initialSelectedOrdinal
                >= declaration.candidateDefinitionIds.size())
        {
            continue;
        }

        SnapshotScopeTarget target;
        target.target.kind = SnapshotTargetKind::SoundVariable;
        target.target.variableOwnerKind = ownerKind
                == ProjectVariableOwnerKind::WorkspaceSession
            ? SnapshotVariableOwnerKind::WorkspaceSession
            : SnapshotVariableOwnerKind::Project;
        target.target.instancePath = instancePath;
        target.target.definitionId = ownerKind
                == ProjectVariableOwnerKind::SoundDefinition
            ? std::string{definitionId}
            : std::string{};
        target.target.variableId = declaration.variableId;
        target.valueKind = SnapshotValueKind::Choice;
        target.choices = declaration.candidateDefinitionIds;

        SnapshotValue value;
        value.kind = SnapshotValueKind::Choice;
        value.choice = declaration.candidateDefinitionIds[
            declaration.initialSelectedOrdinal];
        target.defaultValue = std::move(value);
        targets.push_back(std::move(target));
    }
}

void appendNodeScopeTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const ProjectNode& node,
    const std::vector<std::string>& containingInstancePath,
    std::optional<std::string> containingDefinitionId,
    const talas::expr::EvaluationContext& context,
    std::vector<SnapshotScopeTarget>& targets,
    DiagnosticCollector& diagnostics)
{
    if (!node.soundDefinitionId.has_value())
    {
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::PreparedTargetUnavailable,
                "$.snapshotScope",
                nullptr,
                "Snapshot scope node type is unavailable: " + node.typeId,
                SnapshotDiagnosticSeverity::Error);
            return;
        }
        for (const auto& parameter : descriptor->parameters)
        {
            if (!snapshotValueKind(parameter.kind).has_value())
                continue;
            SnapshotTargetIdentity identity;
            identity.kind = SnapshotTargetKind::NodeParameter;
            identity.instancePath = containingInstancePath;
            identity.containingDefinitionId = containingDefinitionId;
            identity.nodeId = node.instanceId;
            identity.parameterId = parameter.id;
            targets.push_back(makeScopeTarget(
                std::move(identity),
                parameter,
                context,
                document.expressionLanguage,
                document.expressionDefinitions));
        }
        return;
    }

    const auto* definition = findDefinition(
        document,
        *node.soundDefinitionId);
    if (definition == nullptr)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::MissingDefinition,
            "$.snapshotScope",
            nullptr,
            "Snapshot scope Sound definition is unavailable: "
                + *node.soundDefinitionId,
            SnapshotDiagnosticSeverity::Error);
        return;
    }
    auto instancePath = containingInstancePath;
    instancePath.push_back(node.instanceId);
    for (const auto& exposed : definition->exposedParameters)
    {
        if (!snapshotValueKind(exposed.kind).has_value())
            continue;
        SnapshotTargetIdentity identity;
        identity.kind = SnapshotTargetKind::SoundParameter;
        identity.instancePath = instancePath;
        identity.definitionId = definition->definitionId;
        identity.parameterId = exposed.parameterId;
        targets.push_back(makeScopeTarget(
            std::move(identity),
            exposed,
            context,
            document.expressionLanguage,
            document.expressionDefinitions));
    }
}

[[nodiscard]] bool validateScope(
    const ProjectDocument& document,
    const SnapshotScopeIdentity& scope,
    const std::string& path,
    DiagnosticCollector& diagnostics)
{
    if (scope.kind == SnapshotScopeKind::Project)
    {
        if (!scope.instancePath.empty()
            || scope.containingDefinitionId.has_value()
            || !scope.definitionId.empty()
            || !scope.nodeId.empty())
        {
            diagnostics.append(
                SnapshotDiagnosticCode::ScopeMismatch,
                path,
                nullptr,
                "Project snapshot scope contains non-project identity fields");
            return false;
        }
        return true;
    }
    if (scope.kind == SnapshotScopeKind::Definition
        && (scope.instancePath.empty()
            || scope.containingDefinitionId.has_value()
            || scope.definitionId.empty()
            || !scope.nodeId.empty()))
    {
        diagnostics.append(
            SnapshotDiagnosticCode::ScopeMismatch,
            path,
            nullptr,
            "Definition snapshot scope has an invalid identity shape");
        return false;
    }
    if (scope.kind == SnapshotScopeKind::Node
        && (!scope.definitionId.empty() || scope.nodeId.empty()))
    {
        diagnostics.append(
            SnapshotDiagnosticCode::ScopeMismatch,
            path,
            nullptr,
            "Node snapshot scope has an invalid identity shape");
        return false;
    }
    const auto layer = resolveGraphLayer(document, scope.instancePath);
    if (!layer.has_value())
    {
        diagnostics.append(
            scope.kind == SnapshotScopeKind::Definition
                ? SnapshotDiagnosticCode::MissingSoundInstance
                : SnapshotDiagnosticCode::MissingNode,
            path,
            nullptr,
            "Snapshot scope instance path no longer resolves");
        return false;
    }
    if (scope.kind == SnapshotScopeKind::Definition)
    {
        if (layer->definition == nullptr
            || layer->definition->definitionId != scope.definitionId)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingDefinition,
                path,
                nullptr,
                "Snapshot definition scope no longer resolves exactly");
            return false;
        }
        return true;
    }
    const auto expectedDefinitionId = layer->definition == nullptr
        ? std::optional<std::string_view>{}
        : std::optional<std::string_view>{
              layer->definition->definitionId};
    if ((!scope.containingDefinitionId.has_value()
         && expectedDefinitionId.has_value())
        || (scope.containingDefinitionId.has_value()
            && (!expectedDefinitionId.has_value()
                || *scope.containingDefinitionId
                    != *expectedDefinitionId)))
    {
        diagnostics.append(
            SnapshotDiagnosticCode::ScopeMismatch,
            path,
            nullptr,
            "Snapshot node scope containing definition changed");
        return false;
    }
    if (findNode(*layer->graph, scope.nodeId) == nullptr)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::MissingNode,
            path,
            nullptr,
            "Snapshot node scope no longer resolves");
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<ResolvedSnapshotTarget> resolveTarget(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SnapshotTargetIdentity& target,
    const std::string& path,
    DiagnosticCollector& diagnostics)
{
    if (target.kind == SnapshotTargetKind::EventValue)
    {
        const auto exact = std::ranges::find_if(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName == target.eventName
                    && control.domain == target.domain;
            });
        if (exact != document.virtualControls.end())
        {
            return ResolvedSnapshotTarget{
                talas::core::ParameterKind::Expression,
                target.domain,
                nullptr,
            };
        }
        const auto sameName = std::ranges::find_if(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName == target.eventName;
            });
        diagnostics.append(
            sameName == document.virtualControls.end()
                ? SnapshotDiagnosticCode::MissingParameter
                : SnapshotDiagnosticCode::DomainChanged,
            path,
            &target,
            sameName == document.virtualControls.end()
                ? "Snapshot Event Value no longer exists"
                : "Snapshot Event Value domain changed");
        return std::nullopt;
    }

    if (target.kind == SnapshotTargetKind::SoundVariable)
    {
        const ProjectSoundVariableDeclaration* declaration = nullptr;
        if (target.instancePath.empty())
        {
            const auto expectedOwner = target.variableOwnerKind
                    == SnapshotVariableOwnerKind::WorkspaceSession
                ? ProjectVariableOwnerKind::WorkspaceSession
                : ProjectVariableOwnerKind::Project;
            const auto found = std::ranges::find_if(
                document.soundVariableLibrary.variables,
                [&](const ProjectSoundVariableDeclaration& candidate) {
                    return candidate.variableId == target.variableId
                        && candidate.owner.kind == expectedOwner;
                });
            declaration = found
                    == document.soundVariableLibrary.variables.end()
                ? nullptr
                : &*found;
            if (!target.definitionId.empty())
                declaration = nullptr;
        }
        else if (target.variableOwnerKind
                 == SnapshotVariableOwnerKind::Project)
        {
            if (const auto layer = resolveGraphLayer(
                    document,
                    target.instancePath);
                layer.has_value() && layer->definition != nullptr
                && layer->definition->definitionId == target.definitionId)
            {
                const auto found = std::ranges::find_if(
                    document.soundVariableLibrary.variables,
                    [&](const ProjectSoundVariableDeclaration& candidate) {
                        return candidate.variableId == target.variableId
                            && candidate.owner.kind
                                == ProjectVariableOwnerKind::SoundDefinition
                            && candidate.owner.definitionId
                                == target.definitionId;
                    });
                declaration = found
                        == document.soundVariableLibrary.variables.end()
                    ? nullptr
                    : &*found;
            }
        }
        if (declaration == nullptr)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingParameter,
                path,
                &target,
                "Snapshot Sound variable no longer resolves in its exact scope");
            return std::nullopt;
        }
        if (!declaration->snapshotEligible)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::IneligibleParameter,
                path,
                &target,
                "Snapshot Sound variable is not snapshot eligible");
            return std::nullopt;
        }
        return ResolvedSnapshotTarget{
            talas::core::ParameterKind::Choice,
            talas::expr::ValueDomain::Scalar,
            &declaration->candidateDefinitionIds,
        };
    }

    if (target.kind == SnapshotTargetKind::Variable)
    {
        const ProjectVariableDeclaration* declaration = nullptr;
        if (target.instancePath.empty())
        {
            const auto expectedOwner = target.variableOwnerKind
                    == SnapshotVariableOwnerKind::WorkspaceSession
                ? ProjectVariableOwnerKind::WorkspaceSession
                : ProjectVariableOwnerKind::Project;
            const auto found = std::ranges::find_if(
                document.variableLibrary.variables,
                [&](const ProjectVariableDeclaration& candidate) {
                    return candidate.variableId == target.variableId
                        && candidate.owner.kind == expectedOwner;
                });
            declaration = found != document.variableLibrary.variables.end()
                ? &*found
                : nullptr;
            if (!target.definitionId.empty())
                declaration = nullptr;
        }
        else if (target.variableOwnerKind
                 == SnapshotVariableOwnerKind::Project)
        {
            if (const auto layer = resolveGraphLayer(
                    document,
                    target.instancePath);
                layer.has_value() && layer->definition != nullptr
                && layer->definition->definitionId == target.definitionId)
            {
                const auto found = std::ranges::find_if(
                    document.variableLibrary.variables,
                    [&](const ProjectVariableDeclaration& candidate) {
                        return candidate.variableId == target.variableId
                            && candidate.owner.kind
                                == ProjectVariableOwnerKind::SoundDefinition
                            && candidate.owner.definitionId
                                == target.definitionId;
                    });
                declaration = found
                        == document.variableLibrary.variables.end()
                    ? nullptr
                    : &*found;
            }
        }
        if (declaration == nullptr)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingParameter,
                path,
                &target,
                "Snapshot variable no longer resolves in its exact scope");
            return std::nullopt;
        }
        if (!declaration->snapshotEligible)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::IneligibleParameter,
                path,
                &target,
                "Snapshot variable is not snapshot eligible");
            return std::nullopt;
        }
        return ResolvedSnapshotTarget{
            talas::core::ParameterKind::Expression,
            declaration->domain,
            nullptr,
        };
    }

    if (target.kind == SnapshotTargetKind::NodeParameter)
    {
        const auto layer = resolveGraphLayer(
            document,
            target.instancePath);
        if (!layer.has_value())
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingNode,
                path,
                &target,
                "Snapshot node instance path no longer resolves");
            return std::nullopt;
        }
        const auto expectedDefinitionId = layer->definition == nullptr
            ? std::optional<std::string_view>{}
            : std::optional<std::string_view>{
                  layer->definition->definitionId};
        if ((!target.containingDefinitionId.has_value()
             && expectedDefinitionId.has_value())
            || (target.containingDefinitionId.has_value()
                && (!expectedDefinitionId.has_value()
                    || *target.containingDefinitionId
                        != *expectedDefinitionId)))
        {
            diagnostics.append(
                SnapshotDiagnosticCode::ScopeMismatch,
                path,
                &target,
                "Snapshot node-parameter containing definition changed");
            return std::nullopt;
        }
        const auto* node = findNode(*layer->graph, target.nodeId);
        if (node == nullptr)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingNode,
                path,
                &target,
                "Snapshot node no longer exists");
            return std::nullopt;
        }
        if (node->soundDefinitionId.has_value())
        {
            diagnostics.append(
                SnapshotDiagnosticCode::IneligibleParameter,
                path,
                &target,
                "Sound instances use exposed soundParameter targets");
            return std::nullopt;
        }
        const auto* descriptor = registry.findNodeType(node->typeId);
        if (descriptor == nullptr)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::PreparedTargetUnavailable,
                path,
                &target,
                "Snapshot node type is unavailable");
            return std::nullopt;
        }
        const auto parameter = std::ranges::find_if(
            descriptor->parameters,
            [&](const auto& candidate) {
                return candidate.id == target.parameterId;
            });
        if (parameter == descriptor->parameters.end())
        {
            diagnostics.append(
                SnapshotDiagnosticCode::MissingParameter,
                path,
                &target,
                "Snapshot node parameter no longer exists");
            return std::nullopt;
        }
        if (parameter->kind == talas::core::ParameterKind::Resource
            || parameter->kind == talas::core::ParameterKind::Text)
        {
            diagnostics.append(
                SnapshotDiagnosticCode::IneligibleParameter,
                path,
                &target,
                "Resource and text parameters are not snapshot eligible");
            return std::nullopt;
        }
        return ResolvedSnapshotTarget{
            parameter->kind,
            parameter->domain,
            &parameter->choices,
        };
    }

    const auto layer = resolveGraphLayer(document, target.instancePath);
    if (!layer.has_value() || layer->instance == nullptr)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::MissingSoundInstance,
            path,
            &target,
            "Snapshot Sound instance path no longer resolves");
        return std::nullopt;
    }
    if (layer->definition == nullptr
        || layer->definition->definitionId != target.definitionId)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::MissingDefinition,
            path,
            &target,
            "Snapshot Sound definition identity changed");
        return std::nullopt;
    }
    const auto exact = std::ranges::find_if(
        layer->definition->exposedParameters,
        [&](const auto& parameter) {
            return parameter.parameterId == target.parameterId;
        });
    if (exact == layer->definition->exposedParameters.end())
    {
        const auto alias = std::ranges::find_if(
            layer->definition->exposedParameters,
            [&](const auto& parameter) {
                return std::ranges::find(
                           parameter.aliases,
                           target.parameterId)
                    != parameter.aliases.end();
            });
        diagnostics.append(
            alias == layer->definition->exposedParameters.end()
                ? SnapshotDiagnosticCode::MissingParameter
                : SnapshotDiagnosticCode::AliasAvailable,
            path,
            &target,
            alias == layer->definition->exposedParameters.end()
                ? "Snapshot Sound parameter no longer exists"
                : "A compatibility alias is available for explicit migration to "
                    + alias->parameterId);
        return std::nullopt;
    }
    if (exact->kind == talas::core::ParameterKind::Resource
        || exact->kind == talas::core::ParameterKind::Text)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::IneligibleParameter,
            path,
            &target,
            "Resource and text parameters are not snapshot eligible");
        return std::nullopt;
    }
    return ResolvedSnapshotTarget{
        exact->kind,
        exact->domain,
        &exact->choices,
    };
}

void validateValue(
    const SnapshotTargetIdentity& identity,
    const SnapshotValue& value,
    const ResolvedSnapshotTarget& target,
    const std::string& path,
    DiagnosticCollector& diagnostics)
{
    const auto expectedKind = target.kind
            == talas::core::ParameterKind::Expression
        ? SnapshotValueKind::Number
        : target.kind == talas::core::ParameterKind::Choice
        ? SnapshotValueKind::Choice
        : SnapshotValueKind::Boolean;
    if (value.kind != expectedKind)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::KindChanged,
            path,
            &identity,
            "Snapshot value kind no longer matches its parameter");
        return;
    }
    if (value.kind == SnapshotValueKind::Number
        && value.domain != target.domain)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::DomainChanged,
            path,
            &identity,
            "Snapshot numeric domain changed from "
                + domainName(value.domain) + " to "
                + domainName(target.domain));
        return;
    }
    if (value.kind == SnapshotValueKind::Choice
        && (target.choices == nullptr
            || std::ranges::find(*target.choices, value.choice)
                == target.choices->end()))
    {
        diagnostics.append(
            SnapshotDiagnosticCode::InvalidChoice,
            path,
            &identity,
            "Snapshot choice is no longer available");
    }
}
}

std::string qualifiedSnapshotSoundParameterIdentity(
    const std::vector<std::string>& instancePath,
    std::string_view definitionId,
    std::string_view parameterId)
{
    std::string result;
    for (const auto& instanceId : instancePath)
        appendIdentitySegment(result, 'i', instanceId);
    appendIdentitySegment(result, 'd', definitionId);
    appendIdentitySegment(result, 'p', parameterId);
    return result;
}

std::string snapshotTargetIdentityString(
    const SnapshotTargetIdentity& target)
{
    std::string result;
    switch (target.kind)
    {
    case SnapshotTargetKind::EventValue:
        appendIdentitySegment(result, 'e', target.eventName);
        appendIdentitySegment(result, 'u', domainName(target.domain));
        break;
    case SnapshotTargetKind::NodeParameter:
        for (const auto& instanceId : target.instancePath)
            appendIdentitySegment(result, 'i', instanceId);
        appendIdentitySegment(
            result,
            'd',
            target.containingDefinitionId.value_or(""));
        appendIdentitySegment(result, 'n', target.nodeId);
        appendIdentitySegment(result, 'p', target.parameterId);
        break;
    case SnapshotTargetKind::SoundParameter:
        result = qualifiedSnapshotSoundParameterIdentity(
            target.instancePath,
            target.definitionId,
            target.parameterId);
        break;
    case SnapshotTargetKind::Variable:
        if (target.variableOwnerKind
            == SnapshotVariableOwnerKind::WorkspaceSession)
        {
            appendIdentitySegment(result, 'w', {});
        }
        for (const auto& instanceId : target.instancePath)
            appendIdentitySegment(result, 'i', instanceId);
        if (!target.instancePath.empty())
            appendIdentitySegment(result, 'd', target.definitionId);
        appendIdentitySegment(result, 'v', target.variableId);
        break;
    case SnapshotTargetKind::SoundVariable:
        if (target.variableOwnerKind
            == SnapshotVariableOwnerKind::WorkspaceSession)
        {
            appendIdentitySegment(result, 'w', {});
        }
        for (const auto& instanceId : target.instancePath)
            appendIdentitySegment(result, 'i', instanceId);
        if (!target.instancePath.empty())
            appendIdentitySegment(result, 'd', target.definitionId);
        appendIdentitySegment(result, 'c', target.variableId);
        break;
    }
    return result;
}

bool isCanonicalSnapshotTargetIdentityString(
    std::string_view identity) noexcept
{
    std::size_t offset = 0;
    auto segment = consumeIdentitySegment(identity, offset);
    if (!segment.has_value())
        return false;

    if (segment->kind == 'e')
    {
        if (!isEventValueIdentifier(segment->value))
            return false;
        segment = consumeIdentitySegment(identity, offset);
        if (!segment.has_value() || segment->kind != 'u')
            return false;
        const auto domain = segment->value;
        return offset == identity.size()
            && (domain == "scalar" || domain == "hertz"
                || domain == "seconds" || domain == "noteNumber"
                || domain == "samples");
    }
    if (segment->kind == 'v' || segment->kind == 'c')
    {
        return isValidProjectVariableIdentifier(segment->value)
            && offset == identity.size();
    }
    if (segment->kind == 'w')
    {
        if (!segment->value.empty())
            return false;
        segment = consumeIdentitySegment(identity, offset);
        return segment.has_value()
            && (segment->kind == 'v' || segment->kind == 'c')
            && isValidProjectVariableIdentifier(segment->value)
            && offset == identity.size();
    }

    std::size_t instanceCount = 0;
    while (segment->kind == 'i')
    {
        if (!isSnapshotIdentifier(segment->value))
            return false;
        ++instanceCount;
        segment = consumeIdentitySegment(identity, offset);
        if (!segment.has_value())
            return false;
    }
    if (segment->kind != 'd')
        return false;
    const auto definitionId = segment->value;
    segment = consumeIdentitySegment(identity, offset);
    if (!segment.has_value())
        return false;

    if (segment->kind == 'n')
    {
        if ((instanceCount == 0) != definitionId.empty()
            || (!definitionId.empty()
                && !isSnapshotIdentifier(definitionId))
            || !isSnapshotIdentifier(segment->value))
            return false;
        segment = consumeIdentitySegment(identity, offset);
        return segment.has_value() && segment->kind == 'p'
            && isSnapshotIdentifier(segment->value)
            && offset == identity.size();
    }
    if (segment->kind == 'v' || segment->kind == 'c')
    {
        return instanceCount != 0
            && isSnapshotIdentifier(definitionId)
            && isValidProjectVariableIdentifier(segment->value)
            && offset == identity.size();
    }
    return segment->kind == 'p' && instanceCount != 0
        && isSnapshotIdentifier(definitionId)
        && isSnapshotIdentifier(segment->value)
        && offset == identity.size();
}

bool snapshotTargetBelongsToScope(
    const SnapshotScopeIdentity& scope,
    const SnapshotTargetIdentity& target)
{
    switch (scope.kind)
    {
    case SnapshotScopeKind::Project:
        if (target.kind == SnapshotTargetKind::Variable
            || target.kind == SnapshotTargetKind::SoundVariable)
            return target.instancePath.empty();
        if (target.kind == SnapshotTargetKind::EventValue)
            return true;
        if (target.kind == SnapshotTargetKind::NodeParameter)
        {
            return target.instancePath.empty()
                && !target.containingDefinitionId.has_value();
        }
        return target.instancePath.size() == 1;
    case SnapshotScopeKind::Definition:
        if (target.kind == SnapshotTargetKind::Variable
            || target.kind == SnapshotTargetKind::SoundVariable)
        {
            return target.instancePath == scope.instancePath
                && target.definitionId == scope.definitionId;
        }
        if (target.kind == SnapshotTargetKind::EventValue)
            return false;
        if (target.kind == SnapshotTargetKind::NodeParameter)
        {
            return target.instancePath == scope.instancePath
                && target.containingDefinitionId
                    == std::optional<std::string>{scope.definitionId};
        }
        return target.instancePath.size()
                == scope.instancePath.size() + 1
            && std::ranges::equal(
                scope.instancePath.begin(),
                scope.instancePath.end(),
                target.instancePath.begin(),
                target.instancePath.begin()
                    + static_cast<std::ptrdiff_t>(
                        scope.instancePath.size()));
    case SnapshotScopeKind::Node:
        if (target.kind == SnapshotTargetKind::Variable
            || target.kind == SnapshotTargetKind::SoundVariable)
        {
            return target.instancePath.size()
                    == scope.instancePath.size() + 1
                && std::ranges::equal(
                    scope.instancePath.begin(),
                    scope.instancePath.end(),
                    target.instancePath.begin(),
                    target.instancePath.end() - 1)
                && target.instancePath.back() == scope.nodeId;
        }
        if (target.kind == SnapshotTargetKind::EventValue)
            return false;
        if (target.kind == SnapshotTargetKind::NodeParameter)
        {
            return target.instancePath == scope.instancePath
                && target.containingDefinitionId
                    == scope.containingDefinitionId
                && target.nodeId == scope.nodeId;
        }
        if (target.instancePath.size()
            != scope.instancePath.size() + 1)
        {
            return false;
        }
        return std::ranges::equal(
                   scope.instancePath.begin(),
                   scope.instancePath.end(),
                   target.instancePath.begin(),
                   target.instancePath.end() - 1)
            && target.instancePath.back() == scope.nodeId;
    }
    return false;
}

static SnapshotScopeResolutionResult resolveSnapshotScopeTargetsImpl(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SnapshotScopeIdentity& scope,
    talas::expr::EvaluationContext evaluationContext,
    bool enforceSnapshotCapacity)
{
    SnapshotScopeResolutionResult result;
    DiagnosticCollector diagnostics;
    if (!validateScope(
            document,
            scope,
            "$.snapshotScope",
            diagnostics))
    {
        result.diagnostics = diagnostics.take();
        return result;
    }

    const auto appendGraph = [&](
                                 const ProjectGraph& graph,
                                 const std::vector<std::string>& instancePath,
                                 std::optional<std::string> definitionId) {
        for (const auto& node : graph.nodes)
        {
            appendNodeScopeTargets(
                document,
                registry,
                node,
                instancePath,
                definitionId,
                evaluationContext,
                result.targets,
                diagnostics);
        }
    };

    switch (scope.kind)
    {
    case SnapshotScopeKind::Project:
        appendVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::Project,
            {},
            {},
            result.targets);
        appendVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::WorkspaceSession,
            {},
            {},
            result.targets);
        appendSoundVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::Project,
            {},
            {},
            result.targets);
        appendSoundVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::WorkspaceSession,
            {},
            {},
            result.targets);
        appendGraph(document.graph, {}, std::nullopt);
        for (const auto& control : document.virtualControls)
        {
            SnapshotScopeTarget target;
            target.target.kind = SnapshotTargetKind::EventValue;
            target.target.eventName = control.eventName;
            target.target.domain = control.domain;
            target.valueKind = SnapshotValueKind::Number;
            target.domain = control.domain;
            SnapshotValue value;
            value.kind = SnapshotValueKind::Number;
            value.domain = control.domain;
            value.number = control.value;
            target.defaultValue = value;
            target.initialRandomRange = SnapshotParameterRange{
                control.minimum,
                control.maximum,
            };
            result.targets.push_back(std::move(target));
        }
        break;
    case SnapshotScopeKind::Definition:
    {
        const auto layer = resolveGraphLayer(
            document,
            scope.instancePath);
        appendVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::SoundDefinition,
            scope.definitionId,
            scope.instancePath,
            result.targets);
        appendSoundVariableScopeTargets(
            document,
            ProjectVariableOwnerKind::SoundDefinition,
            scope.definitionId,
            scope.instancePath,
            result.targets);
        appendGraph(
            *layer->graph,
            scope.instancePath,
            scope.definitionId);
        break;
    }
    case SnapshotScopeKind::Node:
    {
        const auto layer = resolveGraphLayer(
            document,
            scope.instancePath);
        const auto* node = findNode(*layer->graph, scope.nodeId);
        appendNodeScopeTargets(
            document,
            registry,
            *node,
            scope.instancePath,
            scope.containingDefinitionId,
            evaluationContext,
            result.targets,
            diagnostics);
        if (node->soundDefinitionId.has_value())
        {
            auto instancePath = scope.instancePath;
            instancePath.push_back(node->instanceId);
            appendVariableScopeTargets(
                document,
                ProjectVariableOwnerKind::SoundDefinition,
                *node->soundDefinitionId,
                instancePath,
                result.targets);
            appendSoundVariableScopeTargets(
                document,
                ProjectVariableOwnerKind::SoundDefinition,
                *node->soundDefinitionId,
                instancePath,
                result.targets);
        }
        break;
    }
    }

    if (enforceSnapshotCapacity
        && result.targets.size()
            > maximumSnapshotValuesPerSnapshot)
    {
        diagnostics.append(
            SnapshotDiagnosticCode::CapacityExceeded,
            "$.snapshotScope",
            nullptr,
            "Snapshot scope exceeds the maximum target count",
            SnapshotDiagnosticSeverity::Error);
    }
    std::ranges::sort(
        result.targets,
        {},
        [](const auto& target) {
            return snapshotTargetIdentityString(target.target);
        });
    for (std::size_t index = 0; index < result.targets.size(); ++index)
    {
        const auto& target = result.targets[index].target;
        if (!snapshotTargetBelongsToScope(scope, target))
        {
            diagnostics.append(
                SnapshotDiagnosticCode::ScopeMismatch,
                "$.snapshotScope.targets["
                    + std::to_string(index) + "]",
                &target,
                "Resolved target does not belong to the requested scope",
                SnapshotDiagnosticSeverity::Error);
        }
        if (index != 0
            && snapshotTargetIdentityString(
                   result.targets[index - 1].target)
                == snapshotTargetIdentityString(target))
        {
            diagnostics.append(
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                "$.snapshotScope.targets["
                    + std::to_string(index) + "]",
                &target,
                "Snapshot scope resolves a duplicate target identity",
                SnapshotDiagnosticSeverity::Error);
        }
    }
    result.diagnostics = diagnostics.take();
    if (!result.diagnostics.empty())
        result.targets.clear();
    return result;
}

SnapshotScopeResolutionResult resolveSnapshotScopeTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SnapshotScopeIdentity& scope,
    talas::expr::EvaluationContext evaluationContext)
{
    return resolveSnapshotScopeTargetsImpl(
        document,
        registry,
        scope,
        evaluationContext,
        true);
}

SnapshotScopeResolutionResult resolveAllSnapshotTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::EvaluationContext evaluationContext)
{
    SnapshotScopeResolutionResult result;
    const auto project = resolveSnapshotScopeTargetsImpl(
        document,
        registry,
        {},
        evaluationContext,
        false);
    if (!project.ok())
        return project;
    result.targets = project.targets;
    if (result.targets.size()
        > maximumResolvedSnapshotTargetCount)
    {
        result.targets.clear();
        result.diagnostics.push_back({
            SnapshotDiagnosticCode::CapacityExceeded,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotTargets",
            {},
            "Snapshot target discovery exceeded its bounded capacity",
        });
        return result;
    }

    constexpr std::size_t maximumSoundDepth = 64;
    std::function<bool(
        const ProjectGraph&,
        const std::vector<std::string>&,
        std::size_t)> appendNestedTargets;
    appendNestedTargets = [&](const ProjectGraph& graph,
                              const std::vector<std::string>& parentPath,
                              std::size_t depth) {
        if (depth > maximumSoundDepth)
        {
            result.diagnostics.push_back({
                SnapshotDiagnosticCode::CapacityExceeded,
                SnapshotDiagnosticSeverity::Error,
                "$.snapshotTargets",
                {},
                "Snapshot target discovery exceeded the maximum Sound depth",
            });
            return false;
        }
        for (const auto& node : graph.nodes)
        {
            if (!node.soundDefinitionId.has_value())
                continue;
            const auto* definition = findDefinition(
                document,
                *node.soundDefinitionId);
            if (definition == nullptr)
            {
                result.diagnostics.push_back({
                    SnapshotDiagnosticCode::MissingDefinition,
                    SnapshotDiagnosticSeverity::Error,
                    "$.snapshotTargets",
                    {},
                    "Snapshot target discovery found a missing Sound definition",
                });
                return false;
            }
            auto instancePath = parentPath;
            instancePath.push_back(node.instanceId);
            SnapshotScopeIdentity scope;
            scope.kind = SnapshotScopeKind::Definition;
            scope.instancePath = instancePath;
            scope.definitionId = definition->definitionId;
            const auto resolved = resolveSnapshotScopeTargetsImpl(
                document,
                registry,
                scope,
                evaluationContext,
                false);
            if (!resolved.ok())
            {
                result.diagnostics.insert(
                    result.diagnostics.end(),
                    resolved.diagnostics.begin(),
                    resolved.diagnostics.end());
                return false;
            }
            if (result.targets.size()
                    > maximumResolvedSnapshotTargetCount
                || resolved.targets.size()
                    > maximumResolvedSnapshotTargetCount
                        - result.targets.size())
            {
                result.diagnostics.push_back({
                    SnapshotDiagnosticCode::CapacityExceeded,
                    SnapshotDiagnosticSeverity::Error,
                    "$.snapshotTargets",
                    {},
                    "Snapshot target discovery exceeded its bounded capacity",
                });
                return false;
            }
            result.targets.insert(
                result.targets.end(),
                resolved.targets.begin(),
                resolved.targets.end());
            if (!appendNestedTargets(
                    definition->graph,
                    instancePath,
                    depth + 1))
            {
                return false;
            }
        }
        return true;
    };
    if (!appendNestedTargets(document.graph, {}, 0))
    {
        result.targets.clear();
        return result;
    }

    std::ranges::sort(
        result.targets,
        {},
        [](const auto& target) {
            return snapshotTargetIdentityString(target.target);
        });
    for (std::size_t index = 1; index < result.targets.size(); ++index)
    {
        if (snapshotTargetIdentityString(
                result.targets[index - 1].target)
            != snapshotTargetIdentityString(
                result.targets[index].target))
        {
            continue;
        }
        result.diagnostics.push_back({
            SnapshotDiagnosticCode::DuplicateResolvedTarget,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotTargets[" + std::to_string(index) + "]",
            snapshotTargetIdentityString(result.targets[index].target),
            "Complete snapshot target discovery produced a duplicate identity",
        });
    }
    if (!result.diagnostics.empty())
        result.targets.clear();
    return result;
}

SnapshotOperationResult initializeSnapshotScopeValues(
    const std::vector<SnapshotScopeTarget>& targets)
{
    SnapshotOperationResult result;
    std::vector<const SnapshotScopeTarget*> ordered;
    ordered.reserve(targets.size());
    for (const auto& target : targets)
        ordered.push_back(&target);
    std::ranges::sort(
        ordered,
        {},
        [](const auto* target) {
            return snapshotTargetIdentityString(target->target);
        });
    const auto fail = [&](const SnapshotScopeTarget& target,
                          SnapshotDiagnosticCode code,
                          std::string message) {
        appendBoundedOperationDiagnostic(result.diagnostics, {
            code,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotInitialize",
            snapshotTargetIdentityString(target.target),
            std::move(message),
        });
    };
    if (ordered.size() > maximumSnapshotValuesPerSnapshot)
    {
        result.diagnostics.push_back({
            SnapshotDiagnosticCode::CapacityExceeded,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotInitialize",
            {},
            "Snapshot initialization exceeds the maximum target count",
        });
        return result;
    }
    result.values.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index)
    {
        const auto& target = *ordered[index];
        if (index != 0
            && ordered[index - 1]->target == target.target)
        {
            fail(
                target,
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                "Snapshot initialization contains a duplicate target");
            continue;
        }
        if (!target.defaultValue.has_value())
        {
            fail(
                target,
                SnapshotDiagnosticCode::PreparedTargetUnavailable,
                target.defaultUnavailableReason.empty()
                    ? "Snapshot target default is unavailable"
                    : target.defaultUnavailableReason);
            continue;
        }
        const auto& value = *target.defaultValue;
        if (value.kind != target.valueKind)
        {
            fail(
                target,
                SnapshotDiagnosticCode::KindChanged,
                "Snapshot target default kind does not match its descriptor");
            continue;
        }
        if (value.kind == SnapshotValueKind::Number
            && (value.domain != target.domain
                || !std::isfinite(value.number)))
        {
            fail(
                target,
                SnapshotDiagnosticCode::DomainChanged,
                "Snapshot numeric default domain or value is invalid");
            continue;
        }
        if (value.kind == SnapshotValueKind::Choice
            && std::ranges::find(target.choices, value.choice)
                == target.choices.end())
        {
            fail(
                target,
                SnapshotDiagnosticCode::InvalidChoice,
                "Snapshot choice default is unavailable");
            continue;
        }
        result.values.push_back({target.target, value});
    }
    if (!result.diagnostics.empty())
        result.values.clear();
    return result;
}

SnapshotOperationResult captureSnapshotScopeValues(
    const std::vector<SnapshotScopeTarget>& targets,
    const std::vector<SnapshotValueEntry>& currentValues)
{
    SnapshotOperationResult result;
    const auto fail = [&](
                          const SnapshotTargetIdentity* target,
                          SnapshotDiagnosticCode code,
                          std::string message) {
        appendBoundedOperationDiagnostic(result.diagnostics, {
            code,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotCapture",
            target == nullptr
                ? std::string{}
                : snapshotTargetIdentityString(*target),
            std::move(message),
        });
    };
    if (targets.size() > maximumSnapshotValuesPerSnapshot
        || currentValues.size() > maximumSnapshotValuesPerSnapshot)
    {
        fail(
            nullptr,
            SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot capture exceeds the maximum target count");
        return result;
    }

    std::vector<const SnapshotScopeTarget*> ordered;
    ordered.reserve(targets.size());
    for (const auto& target : targets)
        ordered.push_back(&target);
    std::ranges::sort(
        ordered,
        {},
        [](const auto* target) {
            return snapshotTargetIdentityString(target->target);
        });
    for (std::size_t index = 1; index < ordered.size(); ++index)
    {
        if (ordered[index - 1]->target == ordered[index]->target)
        {
            fail(
                &ordered[index]->target,
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                "Snapshot capture scope contains a duplicate target");
        }
    }
    for (std::size_t index = 0; index < currentValues.size(); ++index)
    {
        if (std::ranges::find_if(
                currentValues.begin(),
                currentValues.begin()
                    + static_cast<std::ptrdiff_t>(index),
                [&](const auto& candidate) {
                    return candidate.target
                        == currentValues[index].target;
                }) != currentValues.begin()
                    + static_cast<std::ptrdiff_t>(index))
        {
            fail(
                &currentValues[index].target,
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                "Current performance values contain a duplicate target");
        }
    }
    if (targets.size() != currentValues.size())
    {
        fail(
            nullptr,
            SnapshotDiagnosticCode::ScopeMismatch,
            "Current performance values do not match the complete snapshot scope");
    }

    result.values.reserve(ordered.size());
    for (const auto* target : ordered)
    {
        const auto current = std::ranges::find_if(
            currentValues,
            [&](const auto& candidate) {
                return candidate.target == target->target;
            });
        if (current == currentValues.end())
        {
            fail(
                &target->target,
                SnapshotDiagnosticCode::MissingParameter,
                "Current performance value is missing for snapshot capture");
            continue;
        }
        if (current->value.kind != target->valueKind)
        {
            fail(
                &target->target,
                SnapshotDiagnosticCode::KindChanged,
                "Current performance value kind changed");
            continue;
        }
        if (current->value.kind == SnapshotValueKind::Number
            && (current->value.domain != target->domain
                || !std::isfinite(current->value.number)))
        {
            fail(
                &target->target,
                SnapshotDiagnosticCode::DomainChanged,
                "Current numeric performance value domain or value is invalid");
            continue;
        }
        if (current->value.kind == SnapshotValueKind::Choice
            && std::ranges::find(
                   target->choices,
                   current->value.choice)
                == target->choices.end())
        {
            fail(
                &target->target,
                SnapshotDiagnosticCode::InvalidChoice,
                "Current performance choice is unavailable");
            continue;
        }
        result.values.push_back(*current);
    }
    if (!result.diagnostics.empty())
        result.values.clear();
    return result;
}

std::vector<SnapshotDiagnostic> snapshotLibraryDiagnostics(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    DiagnosticCollector diagnostics;
    for (std::size_t bankIndex = 0;
         bankIndex < document.snapshotLibrary.banks.size();
         ++bankIndex)
    {
        const auto& bank = document.snapshotLibrary.banks[bankIndex];
        const auto bankPath = "$.snapshotLibrary.banks["
            + std::to_string(bankIndex) + "]";
        static_cast<void>(validateScope(
            document,
            bank.scope,
            bankPath + ".scope",
            diagnostics));
        for (std::size_t policyIndex = 0;
             policyIndex < bank.parameterPolicies.size();
             ++policyIndex)
        {
            const auto& policy = bank.parameterPolicies[policyIndex];
            const auto path = bankPath + ".parameterPolicies["
                + std::to_string(policyIndex) + "].target";
            if (!snapshotTargetBelongsToScope(
                    bank.scope,
                    policy.target))
            {
                diagnostics.append(
                    SnapshotDiagnosticCode::ScopeMismatch,
                    path,
                    &policy.target,
                    "Snapshot policy target does not belong to its bank scope");
            }
            const auto target = resolveTarget(
                document,
                registry,
                policy.target,
                path,
                diagnostics);
            if (target.has_value()
                && policy.range.has_value()
                && target->kind != talas::core::ParameterKind::Expression)
            {
                diagnostics.append(
                    SnapshotDiagnosticCode::IneligibleParameter,
                    path,
                    &policy.target,
                    "Only numeric snapshot targets accept a random range");
            }
        }
        for (std::size_t snapshotIndex = 0;
             snapshotIndex < bank.snapshots.size();
             ++snapshotIndex)
        {
            const auto& snapshot = bank.snapshots[snapshotIndex];
            for (std::size_t valueIndex = 0;
                 valueIndex < snapshot.values.size();
                 ++valueIndex)
            {
                const auto& entry = snapshot.values[valueIndex];
                const auto path = bankPath + ".snapshots["
                    + std::to_string(snapshotIndex) + "].values["
                    + std::to_string(valueIndex) + "].target";
                if (!snapshotTargetBelongsToScope(
                        bank.scope,
                        entry.target))
                {
                    diagnostics.append(
                        SnapshotDiagnosticCode::ScopeMismatch,
                        path,
                        &entry.target,
                        "Snapshot value target does not belong to its bank scope");
                }
                const auto target = resolveTarget(
                    document,
                    registry,
                    entry.target,
                    path,
                    diagnostics);
                if (target.has_value())
                {
                    validateValue(
                        entry.target,
                        entry.value,
                        *target,
                        path,
                        diagnostics);
                }
            }
        }
    }
    return diagnostics.take();
}

namespace
{
// A target the project no longer has, rather than one changed: its Sound,
// parameter, variable or Event Value is gone.
[[nodiscard]] bool targetObsolete(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SnapshotTargetIdentity& target)
{
    DiagnosticCollector diagnostics;
    if (resolveTarget(document, registry, target, {}, diagnostics).has_value())
        return false;
    const auto found = diagnostics.take();
    return std::ranges::any_of(found, [](const SnapshotDiagnostic& diagnostic) {
        return diagnostic.code == SnapshotDiagnosticCode::MissingNode
            || diagnostic.code == SnapshotDiagnosticCode::MissingDefinition
            || diagnostic.code == SnapshotDiagnosticCode::MissingSoundInstance
            || diagnostic.code == SnapshotDiagnosticCode::MissingParameter;
    });
}

[[nodiscard]] bool surfaceHasControl(const ProjectDocument& document, std::string_view eventName)
{
    return std::ranges::any_of(document.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName;
    });
}
}

ObsoleteVirtualControlMaterial obsoleteVirtualControlMaterial(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    ObsoleteVirtualControlMaterial material;
    material.widgets = document.dormantVirtualControls.size();
    for (const auto& page : document.virtualControlLayoutPages)
    {
        material.layoutEntries += static_cast<std::size_t>(std::ranges::count_if(
            page.eventNames, [&](const std::string& name) { return !surfaceHasControl(document, name); }));
    }
    for (const auto& bank : document.snapshotLibrary.banks)
    {
        material.presetValues += static_cast<std::size_t>(std::ranges::count_if(bank.parameterPolicies,
            [&](const SnapshotParameterPolicy& policy) { return targetObsolete(document, registry, policy.target); }));
        for (const auto& snapshot : bank.snapshots)
        {
            material.presetValues += static_cast<std::size_t>(std::ranges::count_if(snapshot.values,
                [&](const SnapshotValueEntry& entry) { return targetObsolete(document, registry, entry.target); }));
        }
    }
    return material;
}

ObsoleteVirtualControlMaterial removeObsoleteVirtualControlMaterial(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    const auto material = obsoleteVirtualControlMaterial(document, registry);
    if (material.empty())
        return material;
    document.dormantVirtualControls.clear();
    std::erase_if(document.virtualControlLayoutPages, [&](VirtualControlLayoutPage& page) {
        const auto before = page.eventNames.size();
        std::erase_if(page.eventNames, [&](const std::string& name) { return !surfaceHasControl(document, name); });
        return before > 0 && page.eventNames.empty();
    });
    // What is obsolete is judged on the document as it was: the banks'
    // targets resolve against its Sounds and controls, which stay.
    for (auto& bank : document.snapshotLibrary.banks)
    {
        std::erase_if(bank.parameterPolicies,
            [&](const SnapshotParameterPolicy& policy) { return targetObsolete(document, registry, policy.target); });
        for (auto& snapshot : bank.snapshots)
        {
            std::erase_if(snapshot.values,
                [&](const SnapshotValueEntry& entry) { return targetObsolete(document, registry, entry.target); });
        }
    }
    return material;
}

SnapshotOperationResult morphSnapshotValues(
    const ProjectSnapshot& left,
    const ProjectSnapshot& right,
    const std::vector<SnapshotValueEntry>& current,
    const std::vector<SnapshotParameterPolicy>& policies,
    double amount)
{
    SnapshotOperationResult result;
    const auto fail = [&](
                          SnapshotDiagnosticCode code,
                          const SnapshotTargetIdentity* target,
                          std::string message) {
        appendBoundedOperationDiagnostic(result.diagnostics, {
            code,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotMorph",
            target == nullptr
                ? std::string{}
                : snapshotTargetIdentityString(*target),
            std::move(message),
        });
    };
    if (!std::isfinite(amount))
    {
        fail(
            SnapshotDiagnosticCode::StageFailed,
            nullptr,
            "Snapshot morph amount must be finite");
        return result;
    }
    if (left.values.size() != right.values.size())
    {
        fail(
            SnapshotDiagnosticCode::ScopeMismatch,
            nullptr,
            "Snapshot morph endpoints require the same target set");
        return result;
    }

    std::vector<const SnapshotValueEntry*> orderedLeft;
    orderedLeft.reserve(left.values.size());
    for (const auto& value : left.values)
        orderedLeft.push_back(&value);
    std::ranges::sort(
        orderedLeft,
        {},
        [](const auto* value) {
            return snapshotTargetIdentityString(value->target);
        });
    for (std::size_t index = 1; index < orderedLeft.size(); ++index)
    {
        if (orderedLeft[index - 1]->target == orderedLeft[index]->target)
        {
            fail(
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                &orderedLeft[index]->target,
                "Snapshot morph endpoint contains a duplicate target");
            return result;
        }
    }

    const auto normalizedAmount = std::clamp(amount, 0.0, 1.0);
    result.values.reserve(orderedLeft.size());
    for (const auto* leftValue : orderedLeft)
    {
        const auto rightValue = std::ranges::find_if(
            right.values,
            [&](const auto& candidate) {
                return candidate.target == leftValue->target;
            });
        if (rightValue == right.values.end())
        {
            fail(
                SnapshotDiagnosticCode::ScopeMismatch,
                &leftValue->target,
                "Snapshot morph endpoints require the same target set");
            continue;
        }
        if (leftValue->value.kind != rightValue->value.kind
            || (leftValue->value.kind == SnapshotValueKind::Number
                && leftValue->value.domain != rightValue->value.domain))
        {
            fail(
                SnapshotDiagnosticCode::KindChanged,
                &leftValue->target,
                "Snapshot morph endpoint value kinds do not match");
            continue;
        }
        const auto policy = std::ranges::find_if(
            policies,
            [&](const auto& candidate) {
                return candidate.target == leftValue->target;
            });
        if (policy != policies.end() && policy->locked)
        {
            const auto held = std::ranges::find_if(
                current,
                [&](const auto& candidate) {
                    return candidate.target == leftValue->target;
                });
            if (held == current.end()
                || held->value.kind != leftValue->value.kind
                || (held->value.kind == SnapshotValueKind::Number
                    && held->value.domain != leftValue->value.domain))
            {
                fail(
                    SnapshotDiagnosticCode::MissingParameter,
                    &leftValue->target,
                    "Locked morph target is missing its gesture-start value");
                continue;
            }
            result.values.push_back(*held);
            continue;
        }

        auto value = leftValue->value;
        switch (value.kind)
        {
        case SnapshotValueKind::Number:
            value.number = std::lerp(
                leftValue->value.number,
                rightValue->value.number,
                normalizedAmount);
            break;
        case SnapshotValueKind::Choice:
            if (normalizedAmount >= 0.5)
                value.choice = rightValue->value.choice;
            break;
        case SnapshotValueKind::Boolean:
            if (normalizedAmount >= 0.5)
                value.boolean = rightValue->value.boolean;
            break;
        }
        result.values.push_back({leftValue->target, std::move(value)});
    }
    if (!result.diagnostics.empty())
        result.values.clear();
    return result;
}

SnapshotOperationResult randomizeSnapshotValues(
    const std::vector<SnapshotRandomizationTarget>& targets,
    std::uint64_t seed)
{
    SnapshotOperationResult result;
    std::vector<const SnapshotRandomizationTarget*> ordered;
    ordered.reserve(targets.size());
    for (const auto& target : targets)
        ordered.push_back(&target);
    std::ranges::sort(
        ordered,
        {},
        [](const auto* target) {
            return snapshotTargetIdentityString(target->current.target);
        });
    const auto fail = [&](
                          SnapshotDiagnosticCode code,
                          const SnapshotTargetIdentity& target,
                          std::string message) {
        appendBoundedOperationDiagnostic(result.diagnostics, {
            code,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotRandomize",
            snapshotTargetIdentityString(target),
            std::move(message),
        });
    };

    for (std::size_t index = 0; index < ordered.size(); ++index)
    {
        const auto& target = *ordered[index];
        if (index != 0
            && ordered[index - 1]->current.target
                == target.current.target)
        {
            fail(
                SnapshotDiagnosticCode::DuplicateResolvedTarget,
                target.current.target,
                "Randomization target set contains a duplicate identity");
            continue;
        }
        if (target.locked)
            continue;
        switch (target.current.value.kind)
        {
        case SnapshotValueKind::Number:
            if (!target.range.has_value())
            {
                fail(
                    SnapshotDiagnosticCode::MissingRandomRange,
                    target.current.target,
                    "Numeric randomization target requires an explicit range");
            }
            else if (!std::isfinite(target.range->minimum)
                     || !std::isfinite(target.range->maximum)
                     || target.range->minimum > target.range->maximum)
            {
                fail(
                    SnapshotDiagnosticCode::MissingRandomRange,
                    target.current.target,
                    "Numeric randomization range is invalid");
            }
            break;
        case SnapshotValueKind::Choice:
            if (target.choices.empty())
            {
                fail(
                    SnapshotDiagnosticCode::InvalidChoice,
                    target.current.target,
                    "Choice randomization target has no available choices");
            }
            break;
        case SnapshotValueKind::Boolean:
            break;
        }
    }
    if (!result.diagnostics.empty())
        return result;

    const auto next = [](std::uint64_t& state) {
        state += 0x9e3779b97f4a7c15ULL;
        auto value = state;
        value = (value ^ (value >> 30U))
            * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U))
            * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    };
    constexpr double inverseTwoToThe53 = 0x1.0p-53;
    auto state = seed;
    result.values.reserve(ordered.size());
    for (const auto* target : ordered)
    {
        const auto bits = next(state);
        const auto unit = static_cast<double>(bits >> 11U)
            * inverseTwoToThe53;
        auto value = target->current.value;
        if (!target->locked)
        {
            switch (value.kind)
            {
            case SnapshotValueKind::Number:
            {
                const auto span =
                    target->range->maximum - target->range->minimum;
                // Keep the frozen evaluation order; a span beyond the
                // double range would otherwise yield infinity.
                value.number = std::isfinite(span)
                    ? target->range->minimum + unit * span
                    : std::lerp(
                        target->range->minimum,
                        target->range->maximum,
                        unit);
                break;
            }
            case SnapshotValueKind::Choice:
            {
                const auto scaled = unit
                    * static_cast<double>(target->choices.size());
                const auto index = std::min(
                    static_cast<std::size_t>(scaled),
                    target->choices.size() - 1);
                value.choice = target->choices[index];
                break;
            }
            case SnapshotValueKind::Boolean:
                value.boolean = unit >= 0.5;
                break;
            }
        }
        result.values.push_back({target->current.target, std::move(value)});
    }
    return result;
}

SnapshotOperationResult randomMergeSnapshotValues(
    const std::vector<SnapshotRandomizationTarget>& targets,
    std::uint64_t seed,
    double amount)
{
    if (!std::isfinite(amount))
    {
        return {{}, {{
            SnapshotDiagnosticCode::StageFailed,
            SnapshotDiagnosticSeverity::Error,
            "$.snapshotRandomMerge",
            {},
            "Random merge amount must be finite",
        }}};
    }
    auto result = randomizeSnapshotValues(targets, seed);
    if (!result.ok())
        return result;
    const auto normalizedAmount = std::clamp(amount, 0.0, 1.0);
    for (auto& randomized : result.values)
    {
        const auto target = std::ranges::find_if(
            targets,
            [&](const auto& candidate) {
                return candidate.current.target == randomized.target;
            });
        if (target == targets.end() || target->locked)
            continue;
        switch (randomized.value.kind)
        {
        case SnapshotValueKind::Number:
            randomized.value.number = std::lerp(
                target->current.value.number,
                randomized.value.number,
                normalizedAmount);
            break;
        case SnapshotValueKind::Choice:
            if (normalizedAmount < 0.5)
                randomized.value.choice = target->current.value.choice;
            break;
        case SnapshotValueKind::Boolean:
            if (normalizedAmount < 0.5)
                randomized.value.boolean = target->current.value.boolean;
            break;
        }
    }
    return result;
}
}
