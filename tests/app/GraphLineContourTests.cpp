#include "talas/app/GraphLineContour.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Radial and right angle lines.
namespace
{
using talas::app::GraphLineContour;
using talas::app::GraphLinePoint;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(GraphLinePoint left, GraphLinePoint right)
{
    return std::abs(left.x - right.x) < 1.0e-3F && std::abs(left.y - right.y) < 1.0e-3F;
}

// From an input Sound's output at (100, 40) to the Sound's input at
// (300, 140), and from another input below it.
void testRadialAndRightAngleLines()
{
    const GraphLinePoint output{100.0F, 40.0F};
    const GraphLinePoint input{300.0F, 140.0F};
    require(talas::app::graphLineCorners(output, input, GraphLineContour::Radial)
            == std::vector<GraphLinePoint>{output, input},
        "a radial line goes straight from the input Sound to the Sound");
    require(near(talas::app::graphLinePoint(output, input, GraphLineContour::Radial, 0.25F), {150.0F, 65.0F}),
        "a quarter of the way along a radial line lies on the straight line");
    const GraphLinePoint lower{100.0F, 240.0F};
    const auto upper = talas::app::graphLineCorners(output, input, GraphLineContour::Radial);
    const auto other = talas::app::graphLineCorners(lower, input, GraphLineContour::Radial);
    require(upper.back() == other.back() && upper.front().y < input.y && other.front().y > input.y,
        "radial lines fan out from the Sound's input to its inputs' outputs");

    const auto corners = talas::app::graphLineCorners(output, input, GraphLineContour::RightAngle);
    require(corners == std::vector<GraphLinePoint>{output, {200.0F, 40.0F}, {200.0F, 140.0F}, input},
        "a right angle line is straight lines meeting at right angles");
    for (std::size_t index = 1; index < corners.size(); ++index)
        require(corners[index].x == corners[index - 1].x || corners[index].y == corners[index - 1].y,
            "each piece of a right angle line is horizontal or vertical");
    require(near(talas::app::graphLinePoint(output, input, GraphLineContour::RightAngle, 0.25F), {175.0F, 40.0F})
            && near(talas::app::graphLinePoint(output, input, GraphLineContour::RightAngle, 0.5F), {200.0F, 90.0F})
            && near(talas::app::graphLinePoint(output, input, GraphLineContour::RightAngle, 1.0F), input),
        "the length along a right angle line goes along its corners");
    require(talas::app::graphLineCorners(output, {300.0F, 40.0F}, GraphLineContour::RightAngle).size() == 2,
        "level ends need no corner");
    require(near(talas::app::graphLinePoint(output, input, GraphLineContour::Curved, 0.0F), output)
            && near(talas::app::graphLinePoint(output, input, GraphLineContour::Curved, 1.0F), input)
            && talas::app::graphLineCorners(output, input, GraphLineContour::Curved).size() == 4,
        "a curved line is a Bézier from the output to the input");
    require(talas::app::graphLineContourName(GraphLineContour::RightAngle) == "Right angle"
            && talas::app::graphLineContourName(GraphLineContour::Radial) == "Radial",
        "the contours are named Right angle and Radial");
}
}

int main()
{
    try
    {
        testRadialAndRightAngleLines();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
