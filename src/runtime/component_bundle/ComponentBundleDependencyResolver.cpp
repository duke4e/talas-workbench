#include "runtime/component_bundle/ComponentBundleDependencyResolver.h"

#include "project/SemanticVersion.h"

#include <algorithm>
#include <map>
#include <set>

namespace talas::runtime::component_bundle
{
namespace
{
[[nodiscard]] bool versionLess(
    std::string_view left,
    std::string_view right) noexcept
{
    const auto order = talas::project::compareCanonicalSemanticVersionPrecedence(
        left,
        right,
        maximumComponentSemanticVersionBytes);
    if (order == talas::project::SemanticVersionOrder::Less)
        return true;
    if (order == talas::project::SemanticVersionOrder::Greater)
        return false;
    return left < right;
}

[[nodiscard]] bool snapshotLess(
    const ComponentBundleSnapshot* left,
    const ComponentBundleSnapshot* right) noexcept
{
    if (left->manifest.bundleId != right->manifest.bundleId)
        return left->manifest.bundleId < right->manifest.bundleId;
    if (left->manifest.semanticVersion != right->manifest.semanticVersion)
    {
        return versionLess(
            left->manifest.semanticVersion,
            right->manifest.semanticVersion);
    }
    return left->contentDigest < right->contentDigest;
}

[[nodiscard]] bool versionMatches(
    std::string_view version,
    const ComponentBundleDependency& dependency) noexcept
{
    const auto minimum =
        talas::project::compareCanonicalSemanticVersionPrecedence(
            version,
            dependency.minimumVersion,
            maximumComponentSemanticVersionBytes);
    const auto maximum =
        talas::project::compareCanonicalSemanticVersionPrecedence(
            version,
            dependency.maximumExclusiveVersion,
            maximumComponentSemanticVersionBytes);
    return minimum != talas::project::SemanticVersionOrder::Invalid
        && maximum != talas::project::SemanticVersionOrder::Invalid
        && minimum != talas::project::SemanticVersionOrder::Less
        && maximum == talas::project::SemanticVersionOrder::Less;
}

void addIssue(
    std::vector<ComponentBundleResolutionIssue>& issues,
    ComponentBundleResolutionIssueCode code,
    std::string_view bundleId = {},
    std::string_view dependencyBundleId = {})
{
    issues.push_back({
        code,
        std::string(bundleId),
        std::string(dependencyBundleId),
    });
}
}

ComponentBundleResolutionResult resolveComponentBundleDependencies(
    const std::vector<ComponentBundleSnapshot>& snapshots)
{
    ComponentBundleResolutionResult result;
    if (snapshots.size() > maximumComponentResolutionBundles)
    {
        addIssue(
            result.errors,
            ComponentBundleResolutionIssueCode::BundleBudgetExceeded);
        return result;
    }

    std::vector<const ComponentBundleSnapshot*> ordered;
    ordered.reserve(snapshots.size());
    for (const auto& snapshot : snapshots)
        ordered.push_back(&snapshot);
    std::ranges::sort(ordered, snapshotLess);

    for (const auto* snapshot : ordered)
    {
        if (!validateComponentBundleSnapshotIdentity(*snapshot))
        {
            addIssue(
                result.errors,
                ComponentBundleResolutionIssueCode::SnapshotInvalid,
                snapshot->manifest.bundleId);
            return result;
        }
    }
    for (std::size_t index = 1; index < ordered.size(); ++index)
    {
        if (ordered[index - 1U]->manifest.bundleId
                == ordered[index]->manifest.bundleId
            && ordered[index - 1U]->manifest.semanticVersion
                == ordered[index]->manifest.semanticVersion)
        {
            addIssue(
                result.errors,
                ComponentBundleResolutionIssueCode::DuplicateBundleVersion,
                ordered[index]->manifest.bundleId);
            return result;
        }
    }

    std::map<std::string, std::vector<std::size_t>, std::less<>> providers;
    for (std::size_t index = 0; index < ordered.size(); ++index)
        providers[ordered[index]->manifest.bundleId].push_back(index);

    std::vector<std::size_t> indegree(ordered.size());
    std::vector<std::vector<std::size_t>> dependents(ordered.size());
    std::size_t edgeCount = 0;
    for (std::size_t consumerIndex = 0;
         consumerIndex < ordered.size();
         ++consumerIndex)
    {
        const auto& manifest = ordered[consumerIndex]->manifest;
        std::vector<const ComponentBundleDependency*> dependencies;
        dependencies.reserve(manifest.dependencies.size());
        for (const auto& dependency : manifest.dependencies)
            dependencies.push_back(&dependency);
        std::ranges::sort(
            dependencies,
            [](const auto* left, const auto* right) {
                if (left->bundleId != right->bundleId)
                    return left->bundleId < right->bundleId;
                if (left->minimumVersion != right->minimumVersion)
                {
                    return versionLess(
                        left->minimumVersion,
                        right->minimumVersion);
                }
                return versionLess(
                    left->maximumExclusiveVersion,
                    right->maximumExclusiveVersion);
            });

        for (const auto* dependency : dependencies)
        {
            const auto providerGroup = providers.find(dependency->bundleId);
            std::vector<std::size_t> compatible;
            if (providerGroup != providers.end())
            {
                for (const auto providerIndex : providerGroup->second)
                {
                    if (versionMatches(
                            ordered[providerIndex]->manifest.semanticVersion,
                            *dependency))
                    {
                        compatible.push_back(providerIndex);
                    }
                }
            }

            if (compatible.empty())
            {
                if (dependency->optional)
                {
                    addIssue(
                        result.warnings,
                        ComponentBundleResolutionIssueCode::OptionalDependencyMissing,
                        manifest.bundleId,
                        dependency->bundleId);
                    continue;
                }
                addIssue(
                    result.errors,
                    providerGroup == providers.end()
                        ? ComponentBundleResolutionIssueCode::RequiredDependencyMissing
                        : ComponentBundleResolutionIssueCode::IncompatibleDependency,
                    manifest.bundleId,
                    dependency->bundleId);
                continue;
            }
            if (compatible.size() != 1U)
            {
                addIssue(
                    result.errors,
                    ComponentBundleResolutionIssueCode::AmbiguousProvider,
                    manifest.bundleId,
                    dependency->bundleId);
                continue;
            }
            if (edgeCount == maximumComponentResolutionEdges)
            {
                addIssue(
                    result.errors,
                    ComponentBundleResolutionIssueCode::EdgeBudgetExceeded,
                    manifest.bundleId,
                    dependency->bundleId);
                return result;
            }
            ++edgeCount;
            ++indegree[consumerIndex];
            dependents[compatible.front()].push_back(consumerIndex);
        }
    }
    if (!result.errors.empty())
        return result;

    for (auto& adjacency : dependents)
        std::ranges::sort(adjacency);
    std::set<std::size_t> ready;
    for (std::size_t index = 0; index < indegree.size(); ++index)
    {
        if (indegree[index] == 0U)
            ready.insert(index);
    }

    std::vector<std::size_t> resolvedIndices;
    resolvedIndices.reserve(ordered.size());
    while (!ready.empty())
    {
        const auto node = *ready.begin();
        ready.erase(ready.begin());
        resolvedIndices.push_back(node);
        for (const auto dependent : dependents[node])
        {
            --indegree[dependent];
            if (indegree[dependent] == 0U)
                ready.insert(dependent);
        }
    }
    if (resolvedIndices.size() != ordered.size())
    {
        const auto cycle = std::ranges::find_if(
            indegree,
            [](std::size_t value) { return value != 0U; });
        const auto index = static_cast<std::size_t>(
            std::distance(indegree.begin(), cycle));
        addIssue(
            result.errors,
            ComponentBundleResolutionIssueCode::DependencyCycle,
            ordered[index]->manifest.bundleId);
        return result;
    }

    result.order.reserve(ordered.size());
    for (const auto index : resolvedIndices)
    {
        const auto& snapshot = *ordered[index];
        result.order.push_back({
            snapshot.manifest.bundleId,
            snapshot.manifest.semanticVersion,
            snapshot.contentDigest,
        });
    }
    return result;
}

ComponentBundleMigrationRouteResult
validateDeclaredComponentBundleMigrationRoute(
    const ComponentBundleStateModel& state,
    std::uint32_t sourceSchemaVersion)
{
    if (sourceSchemaVersion == 0U || state.currentSchemaVersion == 0U)
    {
        return {
            ComponentBundleMigrationRouteCode::InvalidSchemaVersion,
            {}};
    }
    if (state.migrations.size() > maximumComponentBundleMigrationEdges)
    {
        return {
            ComponentBundleMigrationRouteCode::EdgeBudgetExceeded,
            {}};
    }
    if (sourceSchemaVersion > state.currentSchemaVersion)
    {
        return {
            ComponentBundleMigrationRouteCode::SourceNewerThanCurrent,
            {}};
    }

    auto edges = state.migrations;
    std::ranges::sort(edges, [](const auto& left, const auto& right) {
        if (left.fromSchemaVersion != right.fromSchemaVersion)
            return left.fromSchemaVersion < right.fromSchemaVersion;
        return left.toSchemaVersion < right.toSchemaVersion;
    });
    for (std::size_t index = 0; index < edges.size(); ++index)
    {
        const auto& edge = edges[index];
        if (edge.fromSchemaVersion == 0U || edge.toSchemaVersion == 0U)
        {
            return {
                ComponentBundleMigrationRouteCode::InvalidSchemaVersion,
                {}};
        }
        if (index > 0U
            && edges[index - 1U].fromSchemaVersion
                == edge.fromSchemaVersion)
        {
            return {
                ComponentBundleMigrationRouteCode::DuplicateSource,
                {}};
        }
        if (edge.toSchemaVersion > state.currentSchemaVersion)
        {
            return {
                ComponentBundleMigrationRouteCode::TargetBeyondCurrent,
                {}};
        }
        if (edge.toSchemaVersion == edge.fromSchemaVersion)
        {
            return {
                ComponentBundleMigrationRouteCode::NonForwardEdge,
                {}};
        }
    }

    std::map<std::uint32_t, std::uint32_t> transitions;
    for (const auto& edge : edges)
        transitions.emplace(edge.fromSchemaVersion, edge.toSchemaVersion);
    for (const auto& [start, target] : transitions)
    {
        (void)target;
        std::set<std::uint32_t> visited;
        auto cursor = start;
        while (true)
        {
            const auto transition = transitions.find(cursor);
            if (transition == transitions.end())
                break;
            if (!visited.insert(cursor).second)
            {
                return {
                    ComponentBundleMigrationRouteCode::Cycle,
                    {}};
            }
            cursor = transition->second;
        }
    }
    if (std::ranges::any_of(edges, [](const auto& edge) {
            return edge.toSchemaVersion < edge.fromSchemaVersion;
        }))
    {
        return {
            ComponentBundleMigrationRouteCode::NonForwardEdge,
            {}};
    }
    if (sourceSchemaVersion == state.currentSchemaVersion)
    {
        return {
            ComponentBundleMigrationRouteCode::AlreadyCurrent,
            {}};
    }

    ComponentBundleMigrationRouteResult result;
    result.code = ComponentBundleMigrationRouteCode::RouteAvailable;
    result.route.reserve(edges.size());
    auto cursor = sourceSchemaVersion;
    while (cursor != state.currentSchemaVersion)
    {
        const auto transition = transitions.find(cursor);
        if (transition == transitions.end())
        {
            result.code = ComponentBundleMigrationRouteCode::MissingRoute;
            result.route.clear();
            return result;
        }
        result.route.push_back({cursor, transition->second});
        cursor = transition->second;
    }
    return result;
}
}
