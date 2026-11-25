#include "talas/app/SnapshotWorkbenchModel.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>

namespace talas::app
{
namespace
{
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
        return "note number";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    }
    return "unknown";
}

[[nodiscard]] std::string formatNumber(double value)
{
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    return converted.ec == std::errc{}
        ? std::string(buffer.data(), converted.ptr)
        : std::string{"invalid"};
}

[[nodiscard]] std::string formatValue(
    const talas::project::SnapshotValue& value)
{
    switch (value.kind)
    {
    case talas::project::SnapshotValueKind::Number:
    {
        auto result = formatNumber(value.number);
        if (value.domain != talas::expr::ValueDomain::Scalar)
            result += " " + domainName(value.domain);
        return result;
    }
    case talas::project::SnapshotValueKind::Choice:
        return value.choice;
    case talas::project::SnapshotValueKind::Boolean:
        return value.boolean ? "On" : "Off";
    }
    return {};
}

[[nodiscard]] std::string scopeBreadcrumb(
    const talas::project::SnapshotScopeIdentity& scope)
{
    std::string path;
    for (const auto& instance : scope.instancePath)
    {
        if (!path.empty())
            path += " / ";
        path += instance;
    }
    switch (scope.kind)
    {
    case talas::project::SnapshotScopeKind::Project:
        return "Project / Root";
    case talas::project::SnapshotScopeKind::Definition:
        return "Definition / "
            + (path.empty() ? scope.definitionId
                            : path + " / " + scope.definitionId);
    case talas::project::SnapshotScopeKind::Node:
        return "Node / "
            + (path.empty() ? scope.nodeId
                            : path + " / " + scope.nodeId);
    }
    return "Project / Root";
}

[[nodiscard]] std::string targetDisplayName(
    const talas::project::SnapshotTargetIdentity& target)
{
    switch (target.kind)
    {
    case talas::project::SnapshotTargetKind::EventValue:
        return target.eventName;
    case talas::project::SnapshotTargetKind::NodeParameter:
        return target.nodeId + " / " + target.parameterId;
    case talas::project::SnapshotTargetKind::SoundParameter:
        return (target.instancePath.empty()
                    ? target.definitionId
                    : target.instancePath.back())
            + " / " + target.parameterId;
    case talas::project::SnapshotTargetKind::Variable:
        return target.instancePath.empty()
            ? "@" + target.variableId
            : target.instancePath.back() + " / @"
                + target.variableId;
    case talas::project::SnapshotTargetKind::SoundVariable:
        return target.instancePath.empty()
            ? "Sound @" + target.variableId
            : target.instancePath.back() + " / Sound @"
                + target.variableId;
    }
    return {};
}

[[nodiscard]] std::string kindAndDomain(
    talas::project::SnapshotValueKind kind,
    talas::expr::ValueDomain domain)
{
    switch (kind)
    {
    case talas::project::SnapshotValueKind::Number:
        return "Number · " + domainName(domain);
    case talas::project::SnapshotValueKind::Choice:
        return "Choice";
    case talas::project::SnapshotValueKind::Boolean:
        return "Boolean";
    }
    return {};
}

[[nodiscard]] const talas::project::SnapshotDiagnostic*
diagnosticForTarget(
    std::span<const talas::project::SnapshotDiagnostic> diagnostics,
    const talas::project::SnapshotTargetIdentity& target)
{
    const auto printable =
        talas::project::snapshotTargetIdentityString(target);
    const auto found = std::ranges::find(
        diagnostics,
        printable,
        &talas::project::SnapshotDiagnostic::targetIdentity);
    return found == diagnostics.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] const talas::project::SnapshotValue* currentValue(
    const talas::runtime::SnapshotPerformanceState& state,
    const talas::project::SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        state.currentValues,
        target,
        &talas::project::SnapshotValueEntry::target);
    return found == state.currentValues.end()
        ? nullptr
        : std::addressof(found->value);
}

[[nodiscard]] const talas::project::ProjectSoundDefinition*
findDefinition(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] const talas::project::ProjectGraph* graphAtInstancePath(
    const talas::project::ProjectDocument& document,
    const std::vector<std::string>& instancePath)
{
    const auto* graph = std::addressof(document.graph);
    for (const auto& instanceId : instancePath)
    {
        const auto node = std::ranges::find(
            graph->nodes,
            instanceId,
            &talas::project::ProjectNode::instanceId);
        if (node == graph->nodes.end()
            || !node->soundDefinitionId.has_value())
        {
            return nullptr;
        }
        const auto* definition = findDefinition(
            document,
            *node->soundDefinitionId);
        if (definition == nullptr)
            return nullptr;
        graph = std::addressof(definition->graph);
    }
    return graph;
}
}

SnapshotWorkbenchCaptureContext snapshotWorkbenchCaptureContext(
    const talas::project::ProjectDocument& document,
    std::string_view selectedNodeId,
    std::optional<std::string_view> containingDefinitionId,
    std::string_view preferredRootInstanceId)
{
    SnapshotWorkbenchCaptureContext context;
    context.selectedNodeId = selectedNodeId;
    if (!containingDefinitionId.has_value())
        return context;

    context.containingDefinitionId =
        std::string(*containingDefinitionId);
    std::vector<std::vector<std::string>> matchingPaths;
    std::function<void(
        const talas::project::ProjectGraph&,
        const std::vector<std::string>&,
        std::size_t)> collect;
    collect = [&](const auto& graph,
                  const auto& parentPath,
                  std::size_t depth) {
        if (depth > 64)
            return;
        for (const auto& node : graph.nodes)
        {
            if (!node.soundDefinitionId.has_value())
                continue;
            auto path = parentPath;
            path.push_back(node.instanceId);
            if (*node.soundDefinitionId == *containingDefinitionId)
                matchingPaths.push_back(path);
            const auto* definition = findDefinition(
                document,
                *node.soundDefinitionId);
            if (definition != nullptr)
                collect(definition->graph, path, depth + 1);
        }
    };
    collect(document.graph, {}, 1);

    std::vector<const std::vector<std::string>*> preferredPaths;
    if (!preferredRootInstanceId.empty())
    {
        for (const auto& path : matchingPaths)
        {
            if (!path.empty()
                && path.front() == preferredRootInstanceId)
            {
                preferredPaths.push_back(std::addressof(path));
            }
        }
    }
    if (preferredPaths.size() == 1)
    {
        context.instancePath = *preferredPaths.front();
        return context;
    }
    if (preferredPaths.empty() && matchingPaths.size() == 1)
    {
        context.instancePath = matchingPaths.front();
        return context;
    }
    context.exact = false;
    return context;
}

std::vector<SnapshotWorkbenchCaptureScopeOption>
snapshotWorkbenchCaptureScopes(
    const talas::project::ProjectDocument& document,
    std::string_view selectedNodeId)
{
    return snapshotWorkbenchCaptureScopes(
        document,
        snapshotWorkbenchCaptureContext(
            document,
            selectedNodeId,
            std::nullopt,
            {}));
}

std::vector<SnapshotWorkbenchCaptureScopeOption>
snapshotWorkbenchCaptureScopes(
    const talas::project::ProjectDocument& document,
    const SnapshotWorkbenchCaptureContext& context)
{
    std::vector<SnapshotWorkbenchCaptureScopeOption> options;
    const auto* graph = context.exact
        ? graphAtInstancePath(document, context.instancePath)
        : nullptr;
    const talas::project::ProjectNode* selected = nullptr;
    if (graph != nullptr)
    {
        const auto found = std::ranges::find(
            graph->nodes,
            context.selectedNodeId,
            &talas::project::ProjectNode::instanceId);
        if (found != graph->nodes.end())
            selected = std::addressof(*found);
    }
    if (selected != nullptr)
    {
        talas::project::SnapshotScopeIdentity nodeScope;
        nodeScope.kind = talas::project::SnapshotScopeKind::Node;
        nodeScope.instancePath = context.instancePath;
        nodeScope.containingDefinitionId =
            context.containingDefinitionId;
        nodeScope.nodeId = selected->instanceId;
        options.push_back({
            nodeScope,
            "Selected Node · " + selected->instanceId,
            scopeBreadcrumb(nodeScope),
            "Capture selected node "
                + selected->instanceId
                + " at " + scopeBreadcrumb(nodeScope),
        });
        if (selected->soundDefinitionId.has_value())
        {
            talas::project::SnapshotScopeIdentity definitionScope;
            definitionScope.kind =
                talas::project::SnapshotScopeKind::Definition;
            definitionScope.instancePath = context.instancePath;
            definitionScope.instancePath.push_back(
                selected->instanceId);
            definitionScope.definitionId =
                *selected->soundDefinitionId;
            options.push_back({
                definitionScope,
                "Sound Definition · "
                    + *selected->soundDefinitionId,
                scopeBreadcrumb(definitionScope),
                "Capture definition "
                    + *selected->soundDefinitionId
                    + " through concrete instance path "
                    + scopeBreadcrumb(definitionScope),
            });
        }
    }
    if (context.exact
        && context.containingDefinitionId.has_value()
        && !context.instancePath.empty())
    {
        talas::project::SnapshotScopeIdentity definitionScope;
        definitionScope.kind =
            talas::project::SnapshotScopeKind::Definition;
        definitionScope.instancePath = context.instancePath;
        definitionScope.definitionId =
            *context.containingDefinitionId;
        options.push_back({
            definitionScope,
            "Current Definition · "
                + *context.containingDefinitionId,
            scopeBreadcrumb(definitionScope),
            "Capture the active definition "
                + *context.containingDefinitionId
                + " through concrete instance path "
                + scopeBreadcrumb(definitionScope),
        });
    }

    talas::project::SnapshotScopeIdentity projectScope;
    options.push_back({
        projectScope,
        "Whole Project",
        scopeBreadcrumb(projectScope),
        "Capture the whole project root and Virtual Controls",
    });
    return options;
}

SnapshotWorkbenchProjection snapshotWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::runtime::SnapshotPerformanceState& performanceState,
    std::string_view selectedInstanceId,
    std::string_view selectedBankId,
    std::string_view selectedSnapshotId,
    std::span<const talas::project::SnapshotDiagnostic> diagnostics,
    bool transitionActive,
    bool transitionCoalesced,
    bool performanceDocumentCurrent,
    const SnapshotWorkbenchCaptureContext* captureContext)
{
    SnapshotWorkbenchProjection projection;
    const auto inferredCaptureContext =
        snapshotWorkbenchCaptureContext(
            document,
            selectedInstanceId,
            std::nullopt,
            {});
    projection.captureScopes = snapshotWorkbenchCaptureScopes(
        document,
        captureContext == nullptr
            ? inferredCaptureContext
            : *captureContext);
    projection.captureScopeBreadcrumb =
        projection.captureScopes.front().scopeBreadcrumb;
    projection.emptyTitle = "No snapshot banks";
    projection.emptyDescription =
        "Create a bank to capture "
        + projection.captureScopeBreadcrumb
        + " as a complete typed snapshot scope.";
    projection.transitionActive = transitionActive;
    projection.transitionCoalesced = transitionCoalesced;
    projection.morphPosition = performanceState.morphPosition;
    projection.randomSeed = performanceState.lastRandomSeed;
    if (!performanceDocumentCurrent)
    {
        projection.status =
            "Stage the current project before storing snapshot values";
    }

    projection.banks.reserve(document.snapshotLibrary.banks.size());
    for (const auto& bank : document.snapshotLibrary.banks)
    {
        projection.banks.push_back({
            bank.bankId,
            bank.displayName,
            scopeBreadcrumb(bank.scope),
        });
    }
    if (projection.banks.empty())
        return projection;

    const auto requestedBank = std::ranges::find(
        document.snapshotLibrary.banks,
        selectedBankId,
        &talas::project::ProjectSnapshotBank::bankId);
    const auto activeBank = std::ranges::find(
        document.snapshotLibrary.banks,
        performanceState.activeBankId,
        &talas::project::ProjectSnapshotBank::bankId);
    const auto bank = requestedBank
            != document.snapshotLibrary.banks.end()
        ? requestedBank
        : activeBank != document.snapshotLibrary.banks.end()
        ? activeBank
        : document.snapshotLibrary.banks.begin();
    projection.selectedBankId = bank->bankId;
    projection.captureScopeBreadcrumb = scopeBreadcrumb(bank->scope);
    projection.smoothingSeconds = bank->smoothingSeconds;

    talas::expr::EvaluationContext evaluation;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        bank->scope,
        evaluation);
    projection.canStore = performanceDocumentCurrent
        && resolved.ok()
        && !resolved.targets.empty();
    if (!resolved.ok() && !resolved.diagnostics.empty())
        projection.status = resolved.diagnostics.front().message;

    const auto requestedSnapshot = std::ranges::find(
        bank->snapshots,
        selectedSnapshotId,
        &talas::project::ProjectSnapshot::snapshotId);
    const auto activeSnapshot = std::ranges::find(
        bank->snapshots,
        performanceState.activeSnapshotId,
        &talas::project::ProjectSnapshot::snapshotId);
    const auto selectedSnapshot = requestedSnapshot != bank->snapshots.end()
        ? requestedSnapshot
        : activeSnapshot != bank->snapshots.end()
        ? activeSnapshot
        : bank->snapshots.begin();
    if (selectedSnapshot != bank->snapshots.end())
        projection.selectedSnapshotId = selectedSnapshot->snapshotId;

    projection.snapshots.reserve(bank->snapshots.size());
    for (const auto& snapshot : bank->snapshots)
    {
        SnapshotWorkbenchSnapshotRow row;
        row.snapshotId = snapshot.snapshotId;
        row.displayName = snapshot.displayName;
        row.active = performanceState.activeBankId == bank->bankId
            && performanceState.activeSnapshotId == snapshot.snapshotId;
        row.morphLeft = performanceState.activeBankId == bank->bankId
            && performanceState.morphLeftSnapshotId == snapshot.snapshotId;
        row.morphRight = performanceState.activeBankId == bank->bankId
            && performanceState.morphRightSnapshotId == snapshot.snapshotId;
        if (row.active)
            row.stateLabel = "Active";
        if (row.morphLeft)
            row.stateLabel += (row.stateLabel.empty() ? "A" : " · A");
        if (row.morphRight)
            row.stateLabel += (row.stateLabel.empty() ? "B" : " · B");

        auto compatible = resolved.ok()
            ? talas::project::captureSnapshotScopeValues(
                resolved.targets,
                snapshot.values)
            : talas::project::SnapshotOperationResult{};
        if (resolved.ok() && !compatible.ok())
            row.diagnostic = compatible.diagnostics.front().message;
        for (const auto& value : snapshot.values)
        {
            if (const auto* diagnostic = diagnosticForTarget(
                    diagnostics,
                    value.target);
                diagnostic != nullptr)
            {
                row.diagnostic = diagnostic->message;
                break;
            }
        }
        row.recallEnabled = resolved.ok()
            && compatible.ok()
            && row.diagnostic.empty();
        row.accessibleDescription = row.displayName;
        if (!row.stateLabel.empty())
            row.accessibleDescription += ", " + row.stateLabel;
        row.accessibleDescription += row.recallEnabled
            ? ", ready to recall"
            : ", unavailable: " + row.diagnostic;
        projection.snapshots.push_back(std::move(row));
    }

    bool rangesComplete = resolved.ok();
    bool parametersCompatible = resolved.ok();
    projection.parameters.reserve(resolved.targets.size());
    for (const auto& target : resolved.targets)
    {
        SnapshotWorkbenchParameterRow row;
        row.target = target.target;
        row.displayName = targetDisplayName(target.target);
        row.identityPath =
            talas::project::snapshotTargetIdentityString(target.target);
        row.kindAndDomain = kindAndDomain(
            target.valueKind,
            target.domain);
        row.numeric = target.valueKind
            == talas::project::SnapshotValueKind::Number;
        const auto policy = std::ranges::find(
            bank->parameterPolicies,
            target.target,
            &talas::project::SnapshotParameterPolicy::target);
        row.locked = policy != bank->parameterPolicies.end()
            && policy->locked;
        const auto range = policy != bank->parameterPolicies.end()
                && policy->range.has_value()
            ? policy->range
            : target.initialRandomRange;
        row.rangeAvailable =
            !row.numeric || row.locked || range.has_value();
        if (range.has_value())
        {
            row.range = formatNumber(range->minimum)
                + " ... " + formatNumber(range->maximum);
        }
        if (const auto* live = currentValue(
                performanceState,
                target.target);
            live != nullptr)
        {
            row.currentValue = formatValue(*live);
        }
        else if (target.defaultValue.has_value())
        {
            row.currentValue = formatValue(*target.defaultValue);
        }
        else
        {
            row.currentValue = "Unavailable";
        }
        if (const auto* diagnostic = diagnosticForTarget(
                diagnostics,
                target.target);
            diagnostic != nullptr)
        {
            row.diagnostic = diagnostic->message;
            parametersCompatible = false;
        }
        if (!row.rangeAvailable)
        {
            if (!row.diagnostic.empty())
                row.diagnostic += " · ";
            row.diagnostic +=
                "Numeric range required for randomization";
        }
        rangesComplete = rangesComplete && row.rangeAvailable;
        row.accessibleDescription = row.displayName
            + ", " + row.kindAndDomain
            + ", current " + row.currentValue
            + (row.locked ? ", locked" : ", unlocked")
            + (row.range.empty() ? ", no random range"
                                 : ", range " + row.range);
        if (!row.diagnostic.empty())
            row.accessibleDescription += ", " + row.diagnostic;
        projection.parameters.push_back(std::move(row));
    }

    const auto selectedRow = std::ranges::find(
        projection.snapshots,
        projection.selectedSnapshotId,
        &SnapshotWorkbenchSnapshotRow::snapshotId);
    projection.canRecall = selectedRow != projection.snapshots.end()
        && selectedRow->recallEnabled;
    projection.canMorph = std::ranges::count_if(
        projection.snapshots,
        &SnapshotWorkbenchSnapshotRow::recallEnabled) >= 2;
    projection.canRandomize = parametersCompatible
        && rangesComplete
        && !projection.parameters.empty();
    projection.canRandomMerge = projection.canRandomize;

    if (transitionActive)
    {
        projection.status = transitionCoalesced
            ? "Transition active · newest complete candidate queued"
            : "Transition active";
    }
    else if (projection.status.empty() && !diagnostics.empty())
    {
        projection.status = diagnostics.front().message;
    }
    return projection;
}
}
