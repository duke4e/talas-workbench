#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace talas::runtime::component_bundle
{
inline constexpr std::size_t maximumComponentResolutionBundles = 128;
inline constexpr std::size_t maximumComponentResolutionEdges = 4096;

enum class ComponentBundleResolutionIssueCode : std::uint8_t
{
    SnapshotInvalid,
    BundleBudgetExceeded,
    EdgeBudgetExceeded,
    DuplicateBundleVersion,
    RequiredDependencyMissing,
    OptionalDependencyMissing,
    IncompatibleDependency,
    AmbiguousProvider,
    DependencyCycle,
};

struct ComponentBundleResolutionIssue
{
    ComponentBundleResolutionIssueCode code =
        ComponentBundleResolutionIssueCode::SnapshotInvalid;
    std::string bundleId;
    std::string dependencyBundleId;

    bool operator==(const ComponentBundleResolutionIssue&) const = default;
};

struct ResolvedComponentBundle
{
    std::string bundleId;
    std::string semanticVersion;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};

    bool operator==(const ResolvedComponentBundle&) const = default;
};

struct ComponentBundleResolutionResult
{
    std::vector<ResolvedComponentBundle> order;
    std::vector<ComponentBundleResolutionIssue> errors;
    std::vector<ComponentBundleResolutionIssue> warnings;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

[[nodiscard]] ComponentBundleResolutionResult resolveComponentBundleDependencies(
    const std::vector<ComponentBundleSnapshot>& snapshots);

enum class ComponentBundleMigrationRouteCode : std::uint8_t
{
    AlreadyCurrent,
    RouteAvailable,
    InvalidSchemaVersion,
    SourceNewerThanCurrent,
    EdgeBudgetExceeded,
    DuplicateSource,
    TargetBeyondCurrent,
    Cycle,
    NonForwardEdge,
    MissingRoute,
};

struct ComponentBundleMigrationRouteResult
{
    ComponentBundleMigrationRouteCode code =
        ComponentBundleMigrationRouteCode::InvalidSchemaVersion;
    std::vector<ComponentBundleMigrationEdge> route;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ComponentBundleMigrationRouteCode::AlreadyCurrent
            || code == ComponentBundleMigrationRouteCode::RouteAvailable;
    }
};

[[nodiscard]] ComponentBundleMigrationRouteResult
validateDeclaredComponentBundleMigrationRoute(
    const ComponentBundleStateModel& state,
    std::uint32_t sourceSchemaVersion);
}
