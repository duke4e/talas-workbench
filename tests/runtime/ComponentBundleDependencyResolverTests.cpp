#include "runtime/component_bundle/ComponentBundleDependencyResolver.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace bundle = talas::runtime::component_bundle;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes> digest(
    std::string_view value)
{
    std::array<std::uint8_t, talas::runtime::componentTrustDigestBytes> output{};
    const auto nibble = [](char character) {
        return character <= '9' ? character - '0' : character - 'a' + 10;
    };
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        output[index] = static_cast<std::uint8_t>(
            (nibble(value[index * 2U]) << 4U)
            | nibble(value[index * 2U + 1U]));
    }
    return output;
}

bundle::ComponentBundleDependency dependency(
    std::string id,
    std::string minimum = "1.0.0",
    std::string maximumExclusive = "2.0.0",
    bool optional = false)
{
    return {
        std::move(id),
        std::move(minimum),
        std::move(maximumExclusive),
        optional};
}

bundle::ComponentBundleSnapshot snapshot(
    std::string bundleId,
    std::string version,
    std::vector<bundle::ComponentBundleDependency> dependencies = {},
    bundle::ComponentBundleStateModel state = {1, {}})
{
    bundle::ComponentBundleManifest manifest;
    manifest.bundleId = std::move(bundleId);
    manifest.semanticVersion = std::move(version);
    manifest.minimumEngineAbiVersion = 1;
    manifest.maximumEngineAbiVersion = 1;
    manifest.minimumEngineApiVersion = 1;
    manifest.maximumEngineApiVersion = 1;
    manifest.declaredRoles = talas::runtime::componentRole(
        talas::runtime::ComponentExecutionRole::RealtimeProcessor);
    manifest.requestedPermissions = talas::runtime::componentPermission(
        talas::runtime::ComponentExecutionPermission::RealtimeAudio);
    manifest.entry.path = "Payload/component.bin";
    manifest.entry.byteCount = 3;
    manifest.entry.sha256 = digest(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    manifest.nodes.push_back({
        manifest.bundleId + ".node",
        state.currentSchemaVersion,
        {},
        {},
        {}});
    manifest.dependencies = std::move(dependencies);
    manifest.state = std::move(state);
    manifest.provenance.publisher = "Example";
    manifest.provenance.licenseExpression = "Apache-2.0";

    const auto canonical =
        bundle::writeCanonicalComponentBundleManifest(manifest);
    require(canonical.ok(), "resolver fixture manifest should canonicalize");

    bundle::ComponentBundleSnapshot result;
    result.manifest = std::move(manifest);
    result.canonicalManifest = canonical.canonicalJson;
    result.payloads.push_back({
        bundle::ComponentBundleFilePurpose::Resource,
        "Payload/component.bin",
        digest(
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad"),
        {'a', 'b', 'c'},
        true});
    result.contentDigest =
        bundle::calculateComponentBundleSnapshotContentDigest(result);
    require(
        bundle::validateComponentBundleSnapshotIdentity(result),
        "resolver fixture should be a valid immutable snapshot");
    return result;
}

bool hasIssue(
    const std::vector<bundle::ComponentBundleResolutionIssue>& issues,
    bundle::ComponentBundleResolutionIssueCode code)
{
    return std::ranges::any_of(
        issues,
        [code](const auto& issue) { return issue.code == code; });
}

std::vector<std::string> resolvedIds(
    const bundle::ComponentBundleResolutionResult& result)
{
    std::vector<std::string> ids;
    for (const auto& item : result.order)
        ids.push_back(item.bundleId + "@" + item.semanticVersion);
    return ids;
}

void testResolutionIsStableAcrossInputPermutations()
{
    const auto foundation = snapshot("org.example.foundation", "1.2.0");
    const auto consumer = snapshot(
        "org.example.consumer",
        "1.0.0",
        {dependency("org.example.foundation")});
    const auto independent = snapshot("org.example.independent", "3.0.0");
    std::vector first{consumer, independent, foundation};
    std::vector second{foundation, consumer, independent};

    const auto firstResult = bundle::resolveComponentBundleDependencies(first);
    const auto secondResult = bundle::resolveComponentBundleDependencies(second);
    require(
        firstResult.ok() && secondResult.ok()
            && resolvedIds(firstResult) == resolvedIds(secondResult)
            && resolvedIds(firstResult)
                == std::vector<std::string>({
                    "org.example.foundation@1.2.0",
                    "org.example.consumer@1.0.0",
                    "org.example.independent@3.0.0"}),
        "topological resolution should use stable bundle/version tie breaking");
}

void testMissingIncompatibleAmbiguousAndDuplicateProvidersFailClosed()
{
    const auto requiredMissing = bundle::resolveComponentBundleDependencies(
        std::vector{snapshot(
            "org.example.consumer",
            "1.0.0",
            {dependency("org.example.missing")})});
    require(
        !requiredMissing.ok()
            && hasIssue(
                requiredMissing.errors,
                bundle::ComponentBundleResolutionIssueCode::RequiredDependencyMissing),
        "missing required dependencies should reject");

    const auto optionalMissing = bundle::resolveComponentBundleDependencies(
        std::vector{snapshot(
            "org.example.consumer",
            "1.0.0",
            {dependency("org.example.optional", "1.0.0", "2.0.0", true)})});
    require(
        optionalMissing.ok() && optionalMissing.order.size() == 1U
            && hasIssue(
                optionalMissing.warnings,
                bundle::ComponentBundleResolutionIssueCode::OptionalDependencyMissing),
        "optional absence should be reported without creating authority");

    const auto incompatible = bundle::resolveComponentBundleDependencies(
        std::vector{
            snapshot(
                "org.example.consumer",
                "1.0.0",
                {dependency("org.example.provider")}),
            snapshot("org.example.provider", "2.0.0")});
    require(
        !incompatible.ok()
            && hasIssue(
                incompatible.errors,
                bundle::ComponentBundleResolutionIssueCode::IncompatibleDependency),
        "exclusive maximum versions should reject incompatible providers");

    const auto ambiguous = bundle::resolveComponentBundleDependencies(
        std::vector{
            snapshot(
                "org.example.consumer",
                "1.0.0",
                {dependency("org.example.provider")}),
            snapshot("org.example.provider", "1.1.0"),
            snapshot("org.example.provider", "1.2.0")});
    require(
        !ambiguous.ok()
            && hasIssue(
                ambiguous.errors,
                bundle::ComponentBundleResolutionIssueCode::AmbiguousProvider),
        "multiple compatible providers should reject instead of auto-selecting");

    const auto provider = snapshot("org.example.provider", "1.1.0");
    const auto duplicate = bundle::resolveComponentBundleDependencies(
        std::vector{provider, provider});
    require(
        !duplicate.ok()
            && hasIssue(
                duplicate.errors,
                bundle::ComponentBundleResolutionIssueCode::DuplicateBundleVersion),
        "duplicate bundle/version identities should reject regardless of bytes");
}

void testCyclesInvalidSnapshotsAndSemanticBoundariesReject()
{
    const auto cycle = bundle::resolveComponentBundleDependencies(
        std::vector{
            snapshot(
                "org.example.a",
                "1.0.0",
                {dependency("org.example.b")}),
            snapshot(
                "org.example.b",
                "1.0.0",
                {dependency("org.example.a")})});
    require(
        !cycle.ok()
            && hasIssue(
                cycle.errors,
                bundle::ComponentBundleResolutionIssueCode::DependencyCycle),
        "dependency cycles should reject without a partial order");

    auto invalid = snapshot("org.example.invalid", "1.0.0");
    invalid.payloads.front().bytes.front() ^= 0xffU;
    const auto invalidResult = bundle::resolveComponentBundleDependencies(
        std::vector{invalid});
    require(
        !invalidResult.ok()
            && hasIssue(
                invalidResult.errors,
                bundle::ComponentBundleResolutionIssueCode::SnapshotInvalid),
        "resolver should revalidate immutable snapshot identity");

    const auto inclusiveMinimum = bundle::resolveComponentBundleDependencies(
        std::vector{
            snapshot(
                "org.example.consumer",
                "1.0.0",
                {dependency("org.example.provider", "1.0.0", "2.0.0")}),
            snapshot("org.example.provider", "1.0.0+build.7")});
    require(
        inclusiveMinimum.ok(),
        "minimum precedence should be inclusive and ignore build metadata");
}

void testBundleAndEdgeBudgetsRejectBeforeGraphGrowth()
{
    std::vector<bundle::ComponentBundleSnapshot> tooMany;
    for (std::size_t index = 0;
         index < bundle::maximumComponentResolutionBundles + 1U;
         ++index)
    {
        tooMany.push_back(snapshot(
            "org.example.bundle" + std::to_string(index),
            "1.0.0"));
    }
    const auto bundleBudget =
        bundle::resolveComponentBundleDependencies(tooMany);
    require(
        !bundleBudget.ok()
            && hasIssue(
                bundleBudget.errors,
                bundle::ComponentBundleResolutionIssueCode::BundleBudgetExceeded),
        "bundle count should reject before graph allocation");

    std::vector<bundle::ComponentBundleSnapshot> edgeHeavy;
    for (std::size_t index = 0; index < 64U; ++index)
    {
        std::vector<bundle::ComponentBundleDependency> providerDependency;
        if (index == 0U)
            providerDependency.push_back(dependency("org.example.provider1"));
        edgeHeavy.push_back(snapshot(
            "org.example.provider" + std::to_string(index),
            "1.0.0",
            std::move(providerDependency)));
    }
    std::vector<bundle::ComponentBundleDependency> allProviders;
    for (std::size_t index = 0; index < 64U; ++index)
    {
        allProviders.push_back(dependency(
            "org.example.provider" + std::to_string(index)));
    }
    for (std::size_t index = 0; index < 64U; ++index)
    {
        edgeHeavy.push_back(snapshot(
            "org.example.consumer" + std::to_string(index),
            "1.0.0",
            allProviders));
    }
    const auto edgeBudget =
        bundle::resolveComponentBundleDependencies(edgeHeavy);
    require(
        !edgeBudget.ok()
            && hasIssue(
                edgeBudget.errors,
                bundle::ComponentBundleResolutionIssueCode::EdgeBudgetExceeded),
        "dependency edges should reject before exceeding the graph budget");
}

void testDeclaredMigrationRoutesArePureBoundedAndDeterministic()
{
    const bundle::ComponentBundleStateModel state{
        4,
        {{2, 4}, {1, 2}}};
    const auto route =
        bundle::validateDeclaredComponentBundleMigrationRoute(state, 1);
    require(
        route.ok()
            && route.code
                == bundle::ComponentBundleMigrationRouteCode::RouteAvailable
            && route.route
                == std::vector<bundle::ComponentBundleMigrationEdge>({
                    {1, 2},
                    {2, 4}}),
        "migration metadata should yield one deterministic forward route");
    require(
        bundle::validateDeclaredComponentBundleMigrationRoute(state, 4).code
                == bundle::ComponentBundleMigrationRouteCode::AlreadyCurrent
            && bundle::validateDeclaredComponentBundleMigrationRoute(state, 3).code
                == bundle::ComponentBundleMigrationRouteCode::MissingRoute
            && bundle::validateDeclaredComponentBundleMigrationRoute(state, 5).code
                == bundle::ComponentBundleMigrationRouteCode::SourceNewerThanCurrent,
        "current, gap, and newer-state cases should remain distinct");

    require(
        bundle::validateDeclaredComponentBundleMigrationRoute(
            {3, {{1, 2}, {1, 3}}},
            1).code
                == bundle::ComponentBundleMigrationRouteCode::DuplicateSource
            && bundle::validateDeclaredComponentBundleMigrationRoute(
                {3, {{1, 2}, {2, 1}}},
                1).code
                == bundle::ComponentBundleMigrationRouteCode::Cycle
            && bundle::validateDeclaredComponentBundleMigrationRoute(
                {3, {{1, 1}}},
                1).code
                == bundle::ComponentBundleMigrationRouteCode::NonForwardEdge
            && bundle::validateDeclaredComponentBundleMigrationRoute(
                {3, {{1, 4}}},
                1).code
                == bundle::ComponentBundleMigrationRouteCode::TargetBeyondCurrent,
        "ambiguous, cyclic, non-forward, and out-of-range routes should reject");

    bundle::ComponentBundleStateModel overBudget;
    overBudget.currentSchemaVersion = static_cast<std::uint32_t>(
        bundle::maximumComponentBundleMigrationEdges + 2U);
    overBudget.migrations.resize(
        bundle::maximumComponentBundleMigrationEdges + 1U,
        {1, 2});
    require(
        bundle::validateDeclaredComponentBundleMigrationRoute(overBudget, 1).code
            == bundle::ComponentBundleMigrationRouteCode::EdgeBudgetExceeded,
        "migration metadata should reject before exceeding its edge budget");
}
}

int main()
{
    testResolutionIsStableAcrossInputPermutations();
    testMissingIncompatibleAmbiguousAndDuplicateProvidersFailClosed();
    testCyclesInvalidSnapshotsAndSemanticBoundariesReject();
    testBundleAndEdgeBudgetsRejectBeforeGraphGrowth();
    testDeclaredMigrationRoutesArePureBoundedAndDeterministic();
    return 0;
}
