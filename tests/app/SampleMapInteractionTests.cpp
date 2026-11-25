#include "talas/app/SampleMapInteraction.h"

#include <cmath>
#include <optional>
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

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
    }
}

talas::project::ProjectSampleRegion region(
    std::string id,
    std::uint8_t lowNote,
    std::uint8_t rootNote,
    std::uint8_t highNote,
    std::uint8_t lowVelocity,
    std::uint8_t highVelocity)
{
    talas::project::ProjectSampleRegion result;
    result.regionId = std::move(id);
    result.assetId = "asset";
    result.lowNote = lowNote;
    result.rootNote = rootNote;
    result.highNote = highNote;
    result.lowVelocity = lowVelocity;
    result.highVelocity = highVelocity;
    return result;
}

void testMapTransformsUseInclusiveMidiCellsAndInvertedVelocity()
{
    const talas::app::SampleMapViewport viewport;
    const talas::app::SampleMapCanvas canvas{1280.0, 1270.0};

    requireNear(
        talas::app::sampleMapNoteBoundaryToPixel(0.0, viewport, canvas),
        0.0,
        1.0e-9,
        "note zero boundary");
    requireNear(
        talas::app::sampleMapNoteBoundaryToPixel(128.0, viewport, canvas),
        1280.0,
        1.0e-9,
        "note 128 exclusive boundary");
    requireNear(
        talas::app::sampleMapVelocityBoundaryToPixel(128.0, viewport, canvas),
        0.0,
        1.0e-9,
        "velocity 127 upper boundary");
    requireNear(
        talas::app::sampleMapVelocityBoundaryToPixel(1.0, viewport, canvas),
        1270.0,
        1.0e-9,
        "velocity one lower boundary");

    const auto midi = talas::app::sampleMapMidiAtPoint(
        605.0,
        275.0,
        viewport,
        canvas);
    require(
        midi.has_value() && midi->note == 60 && midi->velocity == 100,
        "pixel hit should resolve the exact inclusive note and velocity cell");
    require(
        !talas::app::sampleMapMidiAtPoint(
             -0.1,
             275.0,
             viewport,
             canvas)
             .has_value(),
        "points outside the canvas should not invent a MIDI position");

    const auto bounds = talas::app::sampleMapRegionRect(
        region("wide", 60, 64, 71, 65, 100),
        viewport,
        canvas);
    requireNear(bounds.x, 600.0, 1.0e-9, "region left boundary");
    requireNear(bounds.right(), 720.0, 1.0e-9, "region right boundary");
    requireNear(bounds.y, 270.0, 1.0e-9, "region high-velocity boundary");
    requireNear(bounds.bottom(), 630.0, 1.0e-9, "region low-velocity boundary");
}

void testBoundaryHitTestingReportsIndependentResizeAxes()
{
    const talas::app::SampleMapRect bounds{100.0, 200.0, 300.0, 160.0};
    const auto corner = talas::app::hitTestSampleMapRegion(
        bounds,
        101.0,
        202.0,
        4.0);
    require(
        corner.inside && corner.lowNoteEdge && corner.highVelocityEdge
            && !corner.highNoteEdge && !corner.lowVelocityEdge,
        "top-left hit should expose both exact resize boundaries");

    const auto body = talas::app::hitTestSampleMapRegion(
        bounds,
        250.0,
        280.0,
        4.0);
    require(
        body.inside && !body.anyEdge(),
        "region interior should remain an unambiguous move target");

    require(
        !talas::app::hitTestSampleMapRegion(
             bounds,
             20.0,
             20.0,
             4.0)
             .inside,
        "distant points should miss the region");
}

void testResizeAndMultiRegionMoveClampWithoutChangingRelativeLayout()
{
    const auto first = region("first", 2, 5, 10, 3, 20);
    const auto second = region("second", 100, 110, 127, 100, 122);

    require(
        talas::app::resizeSampleMapRegion(
            first,
            talas::app::SampleMapResizeEdge::LowNote,
            20)
            .lowNote == 5,
        "low-note resize should stop at the root note");
    require(
        talas::app::resizeSampleMapRegion(
            first,
            talas::app::SampleMapResizeEdge::HighNote,
            -20)
            .highNote == 5,
        "high-note resize should stop at the root note");
    require(
        talas::app::resizeSampleMapRegion(
            first,
            talas::app::SampleMapResizeEdge::HighVelocity,
            -100)
            .highVelocity == 3,
        "velocity resize should preserve an inclusive non-empty range");

    const std::vector source{first, second};
    const std::vector<std::string> selection{"first", "second"};
    const auto moved = talas::app::moveSampleMapRegions(
        source,
        selection,
        -10,
        20);
    require(
        moved.appliedNoteDelta == -2
            && moved.appliedVelocityDelta == 5,
        "multi-drag should derive one shared delta from every selected boundary");
    require(
        moved.regions[0].lowNote == 0
            && moved.regions[0].rootNote == 3
            && moved.regions[0].highVelocity == 25
            && moved.regions[1].lowNote == 98
            && moved.regions[1].highNote == 125
            && moved.regions[1].highVelocity == 127,
        "multi-drag should preserve each region span, root offset, and relative layout");
}

void testSelectionModesRemainStableAndReconcileStaleIds()
{
    std::vector<std::string> selection;
    selection = talas::app::updateSampleMapSelection(
        selection,
        "a",
        talas::app::SampleMapSelectionMode::Replace);
    selection = talas::app::updateSampleMapSelection(
        selection,
        "b",
        talas::app::SampleMapSelectionMode::Add);
    selection = talas::app::updateSampleMapSelection(
        selection,
        "a",
        talas::app::SampleMapSelectionMode::Toggle);
    require(
        selection == std::vector<std::string>{"b"},
        "replace, additive, and toggle selection should preserve stable primary order");

    const talas::project::ProjectSampleMap map{
        "map",
        "Map",
        {
            region("a", 0, 0, 10, 1, 127),
            region("b", 11, 11, 20, 1, 127),
        },
    };
    const std::vector<std::string> staleSelection{
        "stale",
        "b",
        "b",
        "a",
    };
    require(
        talas::app::reconcileSampleMapSelection(map, staleSelection)
            == std::vector<std::string>({"b", "a"}),
        "selection reconciliation should remove stale and duplicate ids without changing primary order");
}

void testWaveformMappingZoomAndMarkersClampToExactFrames()
{
    const talas::app::WaveformViewport viewport{100.0, 300.0};
    requireNear(
        talas::app::waveformFrameToPixel(200.0, viewport, 1000.0),
        500.0,
        1.0e-9,
        "frame to pixel mapping");
    require(
        talas::app::waveformPixelToFrame(250.0, viewport, 1000.0)
            == 150,
        "pixel to frame mapping should return an exact nearest source frame");

    const auto zoomed = talas::app::zoomWaveformViewport(
        viewport,
        250.0,
        2.0,
        1000.0,
        1000);
    requireNear(zoomed.startFrame, 125.0, 1.0e-9, "anchored zoom start");
    requireNear(zoomed.endFrameExclusive, 225.0, 1.0e-9, "anchored zoom end");
    require(
        talas::app::clampWaveformFrame(-20.0, 1000) == 0
            && talas::app::clampWaveformFrame(2000.0, 1000) == 1000,
        "cursor frames should clamp to the complete immutable generation");

    const talas::project::ProjectSampleLoop loop{
        talas::project::ProjectSampleLoopMode::Alternate,
        10,
        20,
    };
    require(
        talas::app::moveWaveformLoopMarker(
            loop,
            talas::app::WaveformLoopMarker::Start,
            50,
            100)
            .startFrame == 19,
        "loop start should stop one frame before its exclusive end");
    require(
        talas::app::moveWaveformLoopMarker(
            loop,
            talas::app::WaveformLoopMarker::EndExclusive,
            -50,
            100)
            .endFrameExclusive == 11,
        "loop end should stop one frame after its start");

    const auto projection = talas::app::projectWaveformLoop(
        loop,
        {0.0, 100.0},
        1000.0);
    require(
        projection.startPixel == 100.0
            && projection.endPixel == 200.0
            && projection.hasReturnLeg,
        "alternate loop projection should expose both forward and return traversal legs");
}
}

int main()
{
    testMapTransformsUseInclusiveMidiCellsAndInvertedVelocity();
    testBoundaryHitTestingReportsIndependentResizeAxes();
    testResizeAndMultiRegionMoveClampWithoutChangingRelativeLayout();
    testSelectionModesRemainStableAndReconcileStaleIds();
    testWaveformMappingZoomAndMarkersClampToExactFrames();
    return 0;
}
