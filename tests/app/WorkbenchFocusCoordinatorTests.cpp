#include "app/workbench/WorkbenchFocusCoordinator.h"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using Candidate = talas::app::workbench::WorkbenchFocusCandidate;
using Identity = talas::app::workbench::WorkbenchFocusIdentity;
using Region = talas::app::workbench::WorkbenchFocusRegion;

Candidate candidate(
    Region region,
    std::string item,
    std::uint32_t order,
    bool visible = true,
    bool enabled = true)
{
    return {{region, std::move(item)}, order, visible, enabled};
}

void testVisibleOrderFollowsTaskTopologyAndLocalOrder()
{
    const std::vector candidates{
        candidate(Region::LowerWorkspace, "diagnostics", 2),
        candidate(Region::Toolbar, "play", 2),
        candidate(Region::Toolbar, "save", 1),
        candidate(Region::Library, "prototype-search", 1, false),
        candidate(Region::PrimaryGraph, "graph", 1),
        candidate(Region::RightWorkspace, "inspector", 1, true, false),
    };
    const auto order =
        talas::app::workbench::workbenchVisibleFocusOrder(candidates);
    require(order.has_value()
            && *order == std::vector<Identity>{
                {Region::Toolbar, "save"},
                {Region::Toolbar, "play"},
                {Region::PrimaryGraph, "graph"},
                {Region::LowerWorkspace, "diagnostics"},
            },
        "visible focus order should be deterministic and omit hidden or disabled targets");
}

void testExactIdentityRestoresThenFallsBackWithinItsRegion()
{
    talas::app::workbench::WorkbenchFocusCoordinator coordinator;
    coordinator.capture(Identity{Region::RightWorkspace, "asset:voice"});
    auto candidates = std::vector{
        candidate(Region::PrimaryGraph, "graph", 1),
        candidate(Region::RightWorkspace, "asset:voice", 2),
        candidate(Region::RightWorkspace, "asset:list", 1),
    };
    require(coordinator.restorationTarget(candidates)
                == std::optional<Identity>{
                    {Region::RightWorkspace, "asset:voice"}},
        "an exact still-visible dynamic identity should restore");

    candidates[1].visible = false;
    require(coordinator.restorationTarget(candidates)
                == std::optional<Identity>{
                    {Region::RightWorkspace, "asset:list"}},
        "a removed dynamic identity should fall back within its visible region");
}

void testHiddenRegionPrefersTheActiveGraphThenNearestVisibleRegion()
{
    talas::app::workbench::WorkbenchFocusCoordinator coordinator;
    coordinator.capture(Identity{Region::Library, "sound-row:voice"});
    const std::vector withGraph{
        candidate(Region::Toolbar, "play", 1),
        candidate(Region::PrimaryGraph, "graph", 1),
        candidate(Region::RightWorkspace, "inspector", 1),
    };
    require(coordinator.restorationTarget(withGraph)
                == std::optional<Identity>{{Region::PrimaryGraph, "graph"}},
        "a hidden region should prefer the active graph fallback");

    const std::vector withoutGraph{
        candidate(Region::Toolbar, "play", 1),
        candidate(Region::RightWorkspace, "inspector", 1),
    };
    require(coordinator.restorationTarget(withoutGraph)
                == std::optional<Identity>{{Region::Toolbar, "play"}},
        "without a graph, fallback should choose the nearest visible task region");
}

void testMalformedOrDuplicateAuthorityFailsClosed()
{
    talas::app::workbench::WorkbenchFocusCoordinator coordinator;
    coordinator.capture(Identity{Region::PrimaryGraph, "graph"});
    const std::vector duplicate{
        candidate(Region::PrimaryGraph, "graph", 1),
        candidate(Region::PrimaryGraph, "graph", 2),
    };
    require(!coordinator.restorationTarget(duplicate).has_value(),
        "duplicate focus authority should not pick an ambiguous target");

    coordinator.capture(Identity{
        static_cast<Region>(255),
        "invented",
    });
    require(!coordinator.captured().has_value(),
        "unknown captured regions should fail closed");

    coordinator.capture(Identity{
        Region::PrimaryGraph,
        std::string(4097, 'x'),
    });
    require(!coordinator.captured().has_value(),
        "oversized captured identities should fail closed");

    std::vector<Candidate> oversizedSet;
    for (std::uint32_t index = 0; index < 65; ++index)
    {
        oversizedSet.push_back(candidate(
            Region::PrimaryGraph,
            "item-" + std::to_string(index),
            index));
    }
    require(!talas::app::workbench::workbenchVisibleFocusOrder(oversizedSet)
                 .has_value(),
        "oversized candidate sets should fail closed before allocation work");
}

void testDynamicIdentityIsBoundedStableAndFieldFramed()
{
    const std::array firstFields{std::string_view{"a:b"}, std::string_view{"c"}};
    const std::array secondFields{std::string_view{"a"}, std::string_view{"b:c"}};
    const auto first = talas::app::workbench::workbenchStableFocusItemId(
        "graph.item", firstFields);
    require(!first.empty() && first.size() < 64,
        "dynamic focus identities should have a fixed bounded projection");
    require(first == talas::app::workbench::workbenchStableFocusItemId(
                         "graph.item", firstFields),
        "dynamic focus identities should be stable");
    require(first != talas::app::workbench::workbenchStableFocusItemId(
                         "graph.item", secondFields),
        "length-framed fields should not alias delimiter placement");

    const std::string huge(100000, 'x');
    const std::array hugeField{std::string_view{huge}};
    require(talas::app::workbench::workbenchStableFocusItemId(
                "graph.item", hugeField).size()
            < 64,
        "legal large project identifiers should not expand the component identity");
}
}

int main()
{
    testVisibleOrderFollowsTaskTopologyAndLocalOrder();
    testExactIdentityRestoresThenFallsBackWithinItsRegion();
    testHiddenRegionPrefersTheActiveGraphThenNearestVisibleRegion();
    testMalformedOrDuplicateAuthorityFailsClosed();
    testDynamicIdentityIsBoundedStableAndFieldFramed();
    return 0;
}
