#pragma once

#include <string_view>
#include <vector>

// The contour of the lines between the Sounds in
// the signal flow diagram. Radial lines go straight from a Sound's input
// to its input Sound's output; right angle lines use only straight lines
// and right angles. Curved, the workbench's own, bends smoothly.
namespace talas::app
{
enum class GraphLineContour
{
    Curved,
    Radial,
    RightAngle,
};

struct GraphLinePoint
{
    float x = 0.0F;
    float y = 0.0F;

    bool operator==(const GraphLinePoint&) const = default;
};

// The line from an output at start to an input at end: for Curved the
// cubic Bézier's four control points, otherwise the corners of the
// polyline, start and end included.
[[nodiscard]] std::vector<GraphLinePoint> graphLineCorners(
    GraphLinePoint start,
    GraphLinePoint end,
    GraphLineContour contour);
// The point a fraction t (0 to 1) of the way along the line: the curve's
// parameter for Curved, the length along it otherwise.
[[nodiscard]] GraphLinePoint graphLinePoint(
    GraphLinePoint start,
    GraphLinePoint end,
    GraphLineContour contour,
    float t);
[[nodiscard]] std::string_view graphLineContourName(GraphLineContour contour) noexcept;
}
