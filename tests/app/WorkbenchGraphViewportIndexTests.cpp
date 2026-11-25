#include "talas/app/WorkbenchGraphViewportIndex.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using Rect = talas::app::WorkbenchGraphViewportRect;
    std::vector<Rect> nodes;
    nodes.reserve(1000);
    for (std::size_t index = 0; index < 1000; ++index)
    {
        nodes.push_back({
            static_cast<std::int64_t>((index % 40) * 224),
            static_cast<std::int64_t>((index / 40) * 128),
            178,
            96,
        });
    }
    std::vector<Rect> connections;
    connections.reserve(2000);
    for (std::size_t index = 0; index < 2000; ++index)
    {
        const auto source = index % 999;
        const auto sourceColumn = source % 40;
        const auto sourceRow = source / 40;
        connections.push_back({
            static_cast<std::int64_t>(sourceColumn * 224 + 178),
            static_cast<std::int64_t>(sourceRow * 128 + 48),
            47,
            33,
        });
    }
    talas::app::WorkbenchGraphViewportIndex nodeIndex;
    talas::app::WorkbenchGraphViewportIndex connectionIndex;
    nodeIndex.rebuild(nodes);
    connectionIndex.rebuild(connections);
    std::vector<std::size_t> visible;
    visible.reserve(128);
    nodeIndex.query({0, 0, 960, 640}, visible);
    require(nodeIndex.itemCount() == 1000,
        "index lost 1,000-node fixture items");
    require(connectionIndex.itemCount() == 2000,
        "index lost 2,000-connection fixture items");
    require(nodeIndex.membershipCount()
                <= 1000 * talas::app::WorkbenchGraphViewportIndex::maximumMembershipsPerItem
            && connectionIndex.membershipCount()
                <= 2000 * talas::app::WorkbenchGraphViewportIndex::maximumMembershipsPerItem,
        "indices exceeded the fixed membership envelope");
    require(!visible.empty() && visible.size() < 100,
        "viewport query did not cull the 1,000-node fixture");
    require(nodeIndex.lastCandidateCount() < 160,
        "viewport query examined an effectively total node set");
    require(std::ranges::is_sorted(visible),
        "viewport query did not retain document order");
    require(std::ranges::find(visible, 0) != visible.end(),
        "viewport query omitted an intersecting origin node");

    const std::array<Rect, 1> longConnection{{
        {-12000, 300, 24000, 4},
    }};
    connectionIndex.rebuild(longConnection);
    connectionIndex.query({-100, 250, 200, 200}, visible);
    require(visible == std::vector<std::size_t>{0},
        "coarse index omitted a long visible connection");
    require(connectionIndex.membershipCount()
                <= talas::app::WorkbenchGraphViewportIndex::maximumMembershipsPerItem,
        "long connection exceeded the fixed hierarchical membership envelope");
    require(connectionIndex.lastCandidateCount() == 1,
        "long-connection query examined an unexpected candidate set");
    connectionIndex.query(
        {std::numeric_limits<std::int32_t>::min(),
         std::numeric_limits<std::int32_t>::min(),
         static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()),
         static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())},
        visible);
    require(visible == std::vector<std::size_t>{0},
        "large viewport query bypassed the hierarchical index");
    connectionIndex.rebuild(connections);

    nodeIndex.query({8200, 2800, 400, 300}, visible);
    require(!visible.empty() && visible.back() < 1000,
        "far viewport query did not return bounded exact indices");
    require(nodeIndex.lastCandidateCount() < 80,
        "far viewport query examined an effectively total node set");
    nodeIndex.query({-1000, -1000, 100, 100}, visible);
    require(visible.empty(), "negative empty viewport produced false hits");
    const std::array<Rect, 1> invalidItem{{{0, 0, 0, 96}}};
    nodeIndex.rebuild(invalidItem);
    nodeIndex.query({0, 0, 100, 100}, visible);
    require(visible.empty() && nodeIndex.membershipCount() == 0,
        "all-invalid index did not remain safely empty");
    std::cout << "Workbench graph viewport index tests passed\n";
}
