#include "talas/app/WorkbenchGraphViewportIndex.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>

namespace talas::app
{
namespace
{
std::int64_t safeEnd(std::int64_t start, std::int64_t length) noexcept
{
    if (length <= 0)
        return start;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    return start > maximum - (length - 1) ? maximum : start + length - 1;
}

bool intersects(
    const WorkbenchGraphViewportRect& left,
    const WorkbenchGraphViewportRect& right) noexcept
{
    if (left.width <= 0 || left.height <= 0
        || right.width <= 0 || right.height <= 0)
        return false;
    return left.x <= safeEnd(right.x, right.width)
        && right.x <= safeEnd(left.x, left.width)
        && left.y <= safeEnd(right.y, right.height)
        && right.y <= safeEnd(left.y, left.height);
}

std::int64_t boundedInclusiveSpan(
    std::int64_t minimum,
    std::int64_t maximum) noexcept
{
    if (maximum < minimum)
        return 0;
    constexpr auto sign = std::uint64_t{1} << 63U;
    const auto orderedMinimum = std::bit_cast<std::uint64_t>(minimum) ^ sign;
    const auto orderedMaximum = std::bit_cast<std::uint64_t>(maximum) ^ sign;
    const auto distance = orderedMaximum - orderedMinimum;
    const auto limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    return distance >= limit
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(distance + 1U);
}

WorkbenchGraphViewportRect enclosingRect(
    std::span<const WorkbenchGraphViewportRect> items,
    std::span<const std::size_t> order) noexcept
{
    if (order.empty())
        return {};
    auto minimumX = items[order.front()].x;
    auto minimumY = items[order.front()].y;
    auto maximumX = safeEnd(
        items[order.front()].x, items[order.front()].width);
    auto maximumY = safeEnd(
        items[order.front()].y, items[order.front()].height);
    for (const auto index : order.subspan(1))
    {
        const auto& item = items[index];
        minimumX = std::min(minimumX, item.x);
        minimumY = std::min(minimumY, item.y);
        maximumX = std::max(maximumX, safeEnd(item.x, item.width));
        maximumY = std::max(maximumY, safeEnd(item.y, item.height));
    }
    return {
        minimumX,
        minimumY,
        boundedInclusiveSpan(minimumX, maximumX),
        boundedInclusiveSpan(minimumY, maximumY),
    };
}
}

void WorkbenchGraphViewportIndex::rebuild(
    std::span<const WorkbenchGraphViewportRect> items)
{
    items_.assign(items.begin(), items.end());
    itemOrder_.clear();
    itemOrder_.reserve(items_.size());
    for (std::size_t index = 0; index < items_.size(); ++index)
    {
        const auto& item = items_[index];
        if (item.width > 0 && item.height > 0)
            itemOrder_.push_back(index);
    }
    membershipCount_ = itemOrder_.size();
    nodes_.clear();
    nodes_.reserve(itemOrder_.empty() ? 0U : itemOrder_.size() * 2U - 1U);
    if (!itemOrder_.empty())
        (void) buildNode(0, itemOrder_.size());
    queryStack_.clear();
    queryStack_.reserve(nodes_.size());
    lastCandidateCount_ = 0;
}

std::size_t WorkbenchGraphViewportIndex::buildNode(
    std::size_t begin,
    std::size_t end)
{
    const auto nodeIndex = nodes_.size();
    nodes_.push_back({});
    const auto bounds = enclosingRect(
        items_,
        std::span<const std::size_t>{itemOrder_}.subspan(begin, end - begin));
    if (end - begin <= maximumLeafItems)
    {
        nodes_[nodeIndex] = {bounds, begin, end, 0, 0, true};
        return nodeIndex;
    }

    const auto splitOnX = bounds.width >= bounds.height;
    const auto midpoint = [](const WorkbenchGraphViewportRect& item,
                              bool xAxis) noexcept {
        const auto start = xAxis ? item.x : item.y;
        const auto length = xAxis ? item.width : item.height;
        return std::midpoint(start, safeEnd(start, length));
    };
    std::stable_sort(
        itemOrder_.begin() + static_cast<std::ptrdiff_t>(begin),
        itemOrder_.begin() + static_cast<std::ptrdiff_t>(end),
        [&](std::size_t left, std::size_t right) {
            const auto primaryLeft = midpoint(items_[left], splitOnX);
            const auto primaryRight = midpoint(items_[right], splitOnX);
            if (primaryLeft != primaryRight)
                return primaryLeft < primaryRight;
            const auto secondaryLeft = midpoint(items_[left], !splitOnX);
            const auto secondaryRight = midpoint(items_[right], !splitOnX);
            return secondaryLeft != secondaryRight
                ? secondaryLeft < secondaryRight
                : left < right;
        });
    const auto middle = begin + (end - begin) / 2U;
    const auto left = buildNode(begin, middle);
    const auto right = buildNode(middle, end);
    nodes_[nodeIndex] = {bounds, begin, end, left, right, false};
    return nodeIndex;
}

void WorkbenchGraphViewportIndex::query(
    WorkbenchGraphViewportRect viewport,
    std::vector<std::size_t>& result)
{
    result.clear();
    lastCandidateCount_ = 0;
    if (viewport.width <= 0 || viewport.height <= 0 || nodes_.empty())
        return;
    queryStack_.clear();
    queryStack_.push_back(0);
    while (!queryStack_.empty())
    {
        const auto nodeIndex = queryStack_.back();
        queryStack_.pop_back();
        const auto& node = nodes_[nodeIndex];
        if (!intersects(node.bounds, viewport))
            continue;
        if (!node.leaf)
        {
            queryStack_.push_back(node.right);
            queryStack_.push_back(node.left);
            continue;
        }
        for (auto orderIndex = node.begin;
             orderIndex < node.end; ++orderIndex)
        {
            const auto itemIndex = itemOrder_[orderIndex];
            ++lastCandidateCount_;
            if (intersects(items_[itemIndex], viewport))
                result.push_back(itemIndex);
        }
    }
    std::ranges::sort(result);
}

std::size_t WorkbenchGraphViewportIndex::itemCount() const noexcept
{
    return items_.size();
}

std::size_t WorkbenchGraphViewportIndex::membershipCount() const noexcept
{
    return membershipCount_;
}

std::size_t WorkbenchGraphViewportIndex::lastCandidateCount() const noexcept
{
    return lastCandidateCount_;
}
}
