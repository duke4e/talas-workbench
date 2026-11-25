#include "talas/app/GraphLineContour.h"

#include <algorithm>
#include <cmath>

namespace talas::app
{
std::vector<GraphLinePoint> graphLineCorners(GraphLinePoint start, GraphLinePoint end, GraphLineContour contour)
{
    switch (contour)
    {
    case GraphLineContour::Curved:
    {
        const auto controlDistance = std::max(42.0F, std::abs(end.x - start.x) * 0.45F);
        return {start, {start.x + controlDistance, start.y}, {end.x - controlDistance, end.y}, end};
    }
    case GraphLineContour::Radial:
        return {start, end};
    case GraphLineContour::RightAngle:
    {
        if (start.y == end.y)
            return {start, end};
        const auto middle = (start.x + end.x) * 0.5F;
        return {start, {middle, start.y}, {middle, end.y}, end};
    }
    }
    return {start, end};
}

GraphLinePoint graphLinePoint(GraphLinePoint start, GraphLinePoint end, GraphLineContour contour, float t)
{
    t = std::clamp(t, 0.0F, 1.0F);
    const auto corners = graphLineCorners(start, end, contour);
    if (contour == GraphLineContour::Curved)
    {
        const auto u = 1.0F - t;
        const auto weight = [&](float a, float b, float c, float d) {
            return a * (u * u * u) + b * (3.0F * u * u * t) + c * (3.0F * u * t * t) + d * (t * t * t);
        };
        return {weight(corners[0].x, corners[1].x, corners[2].x, corners[3].x),
            weight(corners[0].y, corners[1].y, corners[2].y, corners[3].y)};
    }
    std::vector<float> lengths;
    auto total = 0.0F;
    for (std::size_t index = 1; index < corners.size(); ++index)
    {
        lengths.push_back(std::hypot(corners[index].x - corners[index - 1].x, corners[index].y - corners[index - 1].y));
        total += lengths.back();
    }
    if (total <= 0.0F)
        return start;
    auto remaining = t * total;
    for (std::size_t index = 0; index < lengths.size(); ++index)
    {
        if (remaining <= lengths[index] || index + 1 == lengths.size())
        {
            const auto fraction = lengths[index] <= 0.0F ? 0.0F : std::min(1.0F, remaining / lengths[index]);
            const auto& from = corners[index];
            const auto& to = corners[index + 1];
            return {from.x + (to.x - from.x) * fraction, from.y + (to.y - from.y) * fraction};
        }
        remaining -= lengths[index];
    }
    return end;
}

std::string_view graphLineContourName(GraphLineContour contour) noexcept
{
    switch (contour)
    {
    case GraphLineContour::Curved: return "Curved";
    case GraphLineContour::Radial: return "Radial";
    case GraphLineContour::RightAngle: return "Right angle";
    }
    return {};
}
}
