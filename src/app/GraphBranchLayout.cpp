#include "talas/app/GraphBranchLayout.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

namespace talas::app
{
namespace
{
[[nodiscard]] bool hasOutgoing(const talas::project::ProjectGraph& graph, std::string_view nodeId)
{
    return std::ranges::any_of(graph.connections, [&](const auto& connection) { return connection.fromNode == nodeId; });
}
}

GraphNodeSet hiddenGraphNodes(const talas::project::ProjectGraph& graph, const GraphNodeSet& collapsed)
{
    GraphNodeSet shown;
    std::vector<std::string> pending;
    for (const auto& node : graph.nodes)
    {
        if (!hasOutgoing(graph, node.instanceId) && shown.insert(node.instanceId).second)
            pending.push_back(node.instanceId);
    }
    const auto collectionVariable = [&](std::string_view nodeId) {
        return std::ranges::any_of(graph.nodes, [&](const auto& node) {
            return node.instanceId == nodeId && node.typeId == "talas.soundCollectionVariable";
        });
    };
    while (!pending.empty())
    {
        const auto current = std::move(pending.back());
        pending.pop_back();
        if (collapsed.contains(current))
            continue;
        for (const auto& connection : graph.connections)
        {
            if (connection.toNode == current && !collectionVariable(connection.fromNode)
                && shown.insert(connection.fromNode).second)
            {
                pending.push_back(connection.fromNode);
            }
        }
    }
    GraphNodeSet hidden;
    for (const auto& node : graph.nodes)
    {
        if (!shown.contains(node.instanceId))
            hidden.insert(node.instanceId);
    }
    return hidden;
}

GraphClickSelection graphClickSelection(bool shift, bool alreadySelected) noexcept
{
    if (!shift)
        return GraphClickSelection::SelectOnly;
    return alreadySelected ? GraphClickSelection::Remove : GraphClickSelection::Add;
}

bool replacementAsksToConfirm(bool replacedHasInputs, bool replacementTakesInputs) noexcept
{
    return !replacedHasInputs && !replacementTakesInputs;
}

std::string replacementConfirmation(std::string_view replaced, std::string_view replacement)
{
    return "Replace " + std::string{replaced} + " with " + std::string{replacement} + "?";
}

bool graphNodeDeletable(const talas::project::ProjectGraph& graph, const GraphNodeSet& collapsed, std::string_view nodeId)
{
    return !collapsed.contains(nodeId) || !graphNodeHasInputs(graph, nodeId);
}

bool graphNodeHasInputs(const talas::project::ProjectGraph& graph, std::string_view nodeId)
{
    return std::ranges::any_of(graph.connections, [&](const auto& connection) { return connection.toNode == nodeId; });
}

GraphNodeSet graphBranchOf(const talas::project::ProjectGraph& graph, std::string_view nodeId)
{
    GraphNodeSet branch;
    std::vector<std::string> pending{std::string{nodeId}};
    while (!pending.empty())
    {
        const auto current = std::move(pending.back());
        pending.pop_back();
        for (const auto& connection : graph.connections)
        {
            if (connection.toNode == current && connection.fromNode != nodeId
                && branch.insert(connection.fromNode).second)
                pending.push_back(connection.fromNode);
        }
    }
    return branch;
}

GraphNodeSet toggledGraphBranch(const talas::project::ProjectGraph& graph, GraphNodeSet collapsed, std::string_view nodeId)
{
    if (const auto found = collapsed.find(nodeId); found != collapsed.end())
        collapsed.erase(found);
    else if (graphNodeHasInputs(graph, nodeId))
        collapsed.emplace(nodeId);
    return collapsed;
}

GraphNodeSet fullyOpenedGraphBranch(const talas::project::ProjectGraph& graph, GraphNodeSet collapsed, std::string_view nodeId)
{
    if (const auto found = collapsed.find(nodeId); found != collapsed.end())
        collapsed.erase(found);
    for (const auto& upstream : graphBranchOf(graph, nodeId))
        collapsed.erase(upstream);
    return collapsed;
}

std::vector<talas::project::ProjectNodePresentation> cleanedUpGraphLayout(const talas::project::ProjectGraph& graph,
    const std::vector<talas::project::ProjectNodePresentation>& current, GraphCleanUpSpacing spacing)
{
    std::map<std::string, talas::project::CanvasPosition, std::less<>> positions;
    for (const auto& presented : current)
        positions.emplace(presented.nodeId, presented.position);
    std::vector<std::string> nodes;
    for (const auto& node : graph.nodes)
    {
        if (positions.contains(node.instanceId))
            nodes.push_back(node.instanceId);
    }
    if (nodes.empty())
        return {};
    auto left = std::numeric_limits<std::int32_t>::max();
    auto top = std::numeric_limits<std::int32_t>::max();
    for (const auto& id : nodes)
    {
        left = std::min(left, positions.at(id).x);
        top = std::min(top, positions.at(id).y);
    }
    // Each Sound's column: the longest run of lines from it to a Sound
    // that feeds nothing, bounded by the node count so a loop ends.
    std::map<std::string, std::size_t, std::less<>> column;
    for (const auto& id : nodes)
        column.emplace(id, 0);
    for (std::size_t pass = 0; pass < nodes.size(); ++pass)
    {
        auto changed = false;
        for (const auto& connection : graph.connections)
        {
            const auto from = column.find(connection.fromNode);
            const auto to = column.find(connection.toNode);
            if (from == column.end() || to == column.end() || connection.fromNode == connection.toNode)
                continue;
            if (from->second < to->second + 1 && to->second + 1 < nodes.size())
            {
                from->second = to->second + 1;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    std::size_t columns = 0;
    for (const auto& [id, at] : column)
        columns = std::max(columns, at + 1);
    // Rows: the rightmost column by its current height, each next column in
    // the order of the rows and input lines its Sounds feed.
    std::map<std::string, double, std::less<>> rank;
    std::vector<std::vector<std::string>> byColumn(columns);
    for (const auto& id : nodes)
        byColumn[column.at(id)].push_back(id);
    std::ranges::stable_sort(byColumn[0], [&](const auto& a, const auto& b) {
        return positions.at(a).y < positions.at(b).y;
    });
    for (std::size_t row = 0; row < byColumn[0].size(); ++row)
        rank[byColumn[0][row]] = static_cast<double>(row);
    for (std::size_t at = 1; at < columns; ++at)
    {
        std::map<std::string, double, std::less<>> keys;
        for (const auto& id : byColumn[at])
        {
            auto best = std::numeric_limits<double>::infinity();
            for (std::size_t line = 0; line < graph.connections.size(); ++line)
            {
                const auto& connection = graph.connections[line];
                if (connection.fromNode != id)
                    continue;
                const auto fed = rank.find(connection.toNode);
                if (fed != rank.end())
                    best = std::min(best, fed->second * 1.0e6 + static_cast<double>(line));
            }
            keys[id] = best;
        }
        std::ranges::stable_sort(byColumn[at], [&](const auto& a, const auto& b) { return keys.at(a) < keys.at(b); });
        for (std::size_t row = 0; row < byColumn[at].size(); ++row)
            rank[byColumn[at][row]] = static_cast<double>(row);
    }
    std::map<std::string, talas::project::CanvasPosition, std::less<>> placed;
    for (std::size_t at = 0; at < columns; ++at)
    {
        for (std::size_t row = 0; row < byColumn[at].size(); ++row)
        {
            placed[byColumn[at][row]] = {
                left + static_cast<std::int32_t>(columns - 1 - at) * spacing.columnWidth,
                top + static_cast<std::int32_t>(row) * spacing.rowHeight,
            };
        }
    }
    std::vector<talas::project::ProjectNodePresentation> layout;
    layout.reserve(nodes.size());
    for (const auto& id : nodes)
        layout.push_back({id, placed.at(id)});
    return layout;
}

GraphNodeSet graphNodesShiftDragged(const talas::project::ProjectGraph& graph, std::string_view nodeId)
{
    auto dragged = graphBranchOf(graph, nodeId);
    dragged.insert(std::string{nodeId});
    return dragged;
}

std::vector<std::size_t> graphConnectionUseCounts(const talas::project::ProjectGraph& graph)
{
    std::map<std::tuple<std::string_view, std::string_view, std::string_view>, std::size_t> uses;
    for (const auto& connection : graph.connections)
        ++uses[{connection.fromNode, connection.fromPort, connection.toNode}];
    std::vector<std::size_t> counts;
    counts.reserve(graph.connections.size());
    for (const auto& connection : graph.connections)
        counts.push_back(uses[{connection.fromNode, connection.fromPort, connection.toNode}]);
    return counts;
}

GraphNodeSet graphNodesFedBy(const talas::project::ProjectGraph& graph, std::string_view nodeId)
{
    GraphNodeSet fed;
    for (const auto& connection : graph.connections)
        if (connection.fromNode == nodeId)
            fed.insert(connection.toNode);
    return fed;
}

std::string numberedCopyName(std::string_view base, const std::function<bool(std::string_view)>& taken)
{
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = std::string{base} + std::to_string(ordinal);
        if (!taken(candidate))
            return candidate;
    }
}
}
