#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/app/WorkbenchDeviceMapping.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testMinimumWindowUsesCompactPolicyWithoutCollapsingGraph()
{
    const auto policy = talas::app::workbenchLayoutPolicyForSize(
        talas::app::workbenchMinimumWidth,
        talas::app::workbenchMinimumHeight);
    require(
        policy.widthClass == talas::app::WorkbenchWidthClass::Compact,
        "minimum window should select the compact policy");
    require(
        !policy.showEditCommandsInToolbar,
        "compact toolbar should move edit commands into menus and overflow");
    require(
        policy.useSummonableSideWorkspaces,
        "compact windows should protect the graph with summonable side workspaces");

    constexpr int outerInsetsAndGutters = 20 + 8 + 8;
    const auto graphWidth = talas::app::workbenchMinimumWidth
        - outerInsetsAndGutters
        - policy.libraryWidth
        - policy.inspectorWidth;
    require(
        graphWidth >= 440,
        "minimum window should retain a usable graph canvas");

    constexpr int contentHeight = talas::app::workbenchMinimumHeight - 46 - 20;
    const auto inspectorHeight = contentHeight
        - policy.analysisHeight
        - policy.timelineHeight
        - 16;
    const auto graphHeight = contentHeight
        - policy.controlSurfaceHeight
        - policy.diagnosticsHeight
        - 16;
    require(
        inspectorHeight >= 140,
        "minimum window should retain a usable inspector");
    require(
        graphHeight >= 320,
        "minimum window should retain a usable graph height");
}

void testGraphFitTransformKeepsThePatchInsideTheViewport()
{
    constexpr int viewportWidth = 924;
    constexpr int viewportHeight = 450;
    constexpr int contentX = 110;
    constexpr int contentY = 170;
    constexpr int contentWidth = 728;
    constexpr int contentHeight = 168;
    constexpr int padding = 32;
    const auto transform = talas::app::workbenchGraphFitTransform(
        viewportWidth,
        viewportHeight,
        contentX,
        contentY,
        contentWidth,
        contentHeight,
        padding);

    require(
        transform.scale > 0.0F && transform.scale <= 1.0F,
        "fit transform should preserve readable, non-inverted graph geometry");
    const auto left = contentX * transform.scale + transform.offsetX;
    const auto right = (contentX + contentWidth) * transform.scale
        + transform.offsetX;
    const auto top = contentY * transform.scale + transform.offsetY;
    const auto bottom = (contentY + contentHeight) * transform.scale
        + transform.offsetY;
    require(
        left >= padding - 0.5F
            && right <= viewportWidth - padding + 0.5F
            && top >= padding - 0.5F
            && bottom <= viewportHeight - padding + 0.5F,
        "fit transform should keep every content edge inside the padded viewport");
    require(
        std::abs((left + right) * 0.5F - viewportWidth * 0.5F) < 0.6F
            && std::abs((top + bottom) * 0.5F - viewportHeight * 0.5F) < 0.6F,
        "fit transform should centre the patch rather than pinning it to an edge");
}

void testGraphFitTransformHandlesEmptyContent()
{
    const auto transform = talas::app::workbenchGraphFitTransform(
        640,
        360,
        0,
        0,
        0,
        0);
    require(
        transform.scale == 1.0F
            && transform.offsetX == 0.0F
            && transform.offsetY == 0.0F,
        "empty graphs should keep the neutral view transform");
}

void testGraphFitTransformContainsVeryLargePatches()
{
    constexpr int viewportWidth = 640;
    constexpr int viewportHeight = 360;
    constexpr int padding = 32;
    constexpr int contentWidth = 10000;
    constexpr int contentHeight = 6000;
    const auto transform = talas::app::workbenchGraphFitTransform(
        viewportWidth,
        viewportHeight,
        -1200,
        800,
        contentWidth,
        contentHeight,
        padding);
    const auto fittedWidth = contentWidth * transform.scale;
    const auto fittedHeight = contentHeight * transform.scale;
    require(
        fittedWidth <= viewportWidth - padding * 2 + 0.5F
            && fittedHeight <= viewportHeight - padding * 2 + 0.5F,
        "zoom-to-fit should contain even patches larger than four viewports");
}

// The fly-by help comes in a yellow box, its text
// dark on it.
void testFlyByHelpIsAYellowBox()
{
    const auto colours = talas::app::flyByHelpColours();
    const auto channel = [](std::uint32_t argb, int shift) { return static_cast<int>(argb >> shift & 0xffu); };
    require(channel(colours.box, 16) > 0xe0 && channel(colours.box, 8) > 0xd0 && channel(colours.box, 0) < 0xc0
            && channel(colours.text, 16) < 0x40 && channel(colours.text, 8) < 0x40 && channel(colours.text, 0) < 0x40
            && (colours.box >> 24) == 0xffu,
        "the fly-by help should be dark text in a yellow box");
}

// Selected Sounds are highlighted in blue, a row as a
// Sound in the signal flow.
void testSelectedSoundsAreHighlightedInBlue()
{
    const auto colours = talas::app::selectionHighlightColours();
    const auto channel = [](std::uint32_t argb, int shift) { return static_cast<int>(argb >> shift & 0xffu); };
    for (const auto colour : {colours.row, colours.soundInSignalFlow})
    {
        require(channel(colour, 0) > channel(colour, 8) && channel(colour, 0) > channel(colour, 16) && (colour >> 24) == 0xffu,
            "a selection should be highlighted in blue");
    }
    // With the preferences asking for a gray selection,
    // a Sound selected in the signal flow is highlighted in gray.
    const auto gray = talas::app::selectionHighlightColours(true);
    for (const auto colour : {gray.row, gray.soundInSignalFlow})
    {
        require(channel(colour, 0) == channel(colour, 8) && channel(colour, 8) == channel(colour, 16) && (colour >> 24) == 0xffu
                && colour != colours.soundInSignalFlow,
            "a selected Sound should be highlighted in gray when the preferences ask for gray");
    }
}

void testNarrowRegularWindowMovesEditCommandsIntoMenus()
{
    const auto policy = talas::app::workbenchLayoutPolicyForSize(
        1100,
        talas::app::workbenchDefaultHeight);
    require(
        policy.widthClass == talas::app::WorkbenchWidthClass::Regular
            && !policy.showEditCommandsInToolbar,
        "narrow regular windows should reserve readable toolbar status space");
}

void testManualZoomPreservesVeryLargePatchFitFloor()
{
    require(
        std::abs(talas::app::workbenchGraphZoomScale(0.05F, 0.8F, 0.05F)
                 - 0.05F)
            < 0.0001F,
        "zooming out from a sub-25-percent fit should not jump up and clip the patch");
    require(
        std::abs(talas::app::workbenchGraphZoomScale(0.05F, 1.2F, 0.05F)
                 - 0.06F)
            < 0.0001F,
        "zooming in from a sub-25-percent fit should remain continuous");
    require(
        talas::app::workbenchGraphZoomScale(0.01F, 1.05F, 0.01F)
            > 0.01F,
        "very small valid fit scales should still accept incremental zoom-in steps");
}

void testKeyboardCanvasNudgeSaturatesAtCoordinateBounds()
{
    require(
        talas::app::workbenchNudgedCanvasCoordinate(40, 8) == 48,
        "ordinary keyboard nudge should retain its exact delta");
    require(
        talas::app::workbenchNudgedCanvasCoordinate(
            std::numeric_limits<std::int32_t>::max(),
            8)
            == std::numeric_limits<std::int32_t>::max(),
        "keyboard nudge should saturate instead of overflowing above int32");
    require(
        talas::app::workbenchNudgedCanvasCoordinate(0, -8) == 0,
        "keyboard nudge should retain the canvas non-negative floor");
}

void testWorkspacePaneSplitRatioIsBoundedAndOverflowSafe()
{
    require(
        std::abs(
            talas::app::workbenchPaneSplitRatio(500, 100, 800)
            - 0.5) < 0.0001,
        "pane divider should project its pointer position into the local split area");
    require(
        talas::app::workbenchPaneSplitRatio(-1000, 100, 800)
                == 0.2
            && talas::app::workbenchPaneSplitRatio(5000, 100, 800)
                == 0.8,
        "pane divider should preserve usable graph space at both extremes");
    require(
        talas::app::workbenchPaneSplitRatio(
            std::numeric_limits<int>::max(),
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max())
                == 0.8
            && talas::app::workbenchPaneSplitRatio(0, 0, 0)
                == 0.5,
        "pane divider arithmetic should remain deterministic for overflow-prone or empty bounds");
}

void testStructureLayoutProjectionSeparatesAdaptiveAndExplicitAuthority()
{
    using Orientation =
        talas::project::WorkspaceStructureSplitOrientation;
    using Effective = talas::app::WorkbenchStructurePaneOrientation;

    const auto adaptiveNarrow =
        talas::app::workbenchStructureLayoutProjection(
            Orientation::Adaptive,
            true,
            839,
            500);
    const auto adaptiveWide =
        talas::app::workbenchStructureLayoutProjection(
            Orientation::Adaptive,
            false,
            840,
            260);
    const auto explicitLeftRight =
        talas::app::workbenchStructureLayoutProjection(
            Orientation::LeftRight,
            false,
            320,
            200);
    const auto explicitTopBottom =
        talas::app::workbenchStructureLayoutProjection(
            Orientation::TopBottom,
            true,
            2000,
            1200);

    require(
        adaptiveNarrow.orientation == Effective::TopBottom
            && adaptiveNarrow.primaryPaneFirst
            && adaptiveWide.orientation == Effective::LeftRight
            && !adaptiveWide.primaryPaneFirst,
        "adaptive Structure layout should use one exact responsive threshold without changing pane order");
    require(
        explicitLeftRight.orientation == Effective::LeftRight
            && !explicitLeftRight.primaryPaneFirst
            && explicitTopBottom.orientation == Effective::TopBottom
            && explicitTopBottom.primaryPaneFirst,
        "explicit Structure layout should remain stable across window sizes and preserve pane order");
}

void testGraphViewProjectionClosesEveryPresentationMode()
{
    const auto compact = talas::app::workbenchGraphViewProjection(
        talas::project::WorkspaceGraphViewMode::Compact);
    const auto ports = talas::app::workbenchGraphViewProjection(
        talas::project::WorkspaceGraphViewMode::Ports);
    const auto portsAndWires =
        talas::app::workbenchGraphViewProjection(
            talas::project::WorkspaceGraphViewMode::PortsAndWires);

    require(
        !compact.showPorts && !compact.showWires
            && !compact.allowPortInteraction,
        "Compact graph mode should expose node cards without hidden port authority");
    require(
        ports.showPorts && !ports.showWires
            && ports.allowPortInteraction,
        "Ports graph mode should retain typed connection interaction while hiding wires");
    require(
        portsAndWires.showPorts && portsAndWires.showWires
            && portsAndWires.allowPortInteraction,
        "Ports-and-Wires graph mode should retain the complete graph presentation");
}

void testWorkspaceSessionWritesUseTrailingDebounceAndExplicitFlush()
{
    talas::app::WorkbenchSessionWriteCoalescer coalescer;
    require(!coalescer.pending(), "session writer should start idle");

    coalescer.noteChange(1000);
    require(
        coalescer.pending()
            && !coalescer.isDue(1249)
            && coalescer.isDue(1250),
        "one session mutation should become due only after the debounce window");

    coalescer.noteChange(1240);
    require(
        !coalescer.isDue(1250)
            && !coalescer.isDue(1489)
            && coalescer.isDue(1490),
        "continuous wheel or divider events should coalesce behind the last mutation");

    require(
        coalescer.consumeIfDue(1490)
            && !coalescer.consumeIfDue(1491),
        "one due write attempt should be consumed even when its filesystem operation fails");
    coalescer.noteChange(1500);
    require(
        coalescer.consumeIfDue(1750),
        "a new session mutation should permit one new persistence attempt after a failure");
    coalescer.clear();
    require(
        !coalescer.pending() && !coalescer.isDue(2000),
        "an explicit lifecycle flush should clear pending persistence");
}

// The Tape Recorder's Filename button at the upper
// left, the selected file's name in the box immediately below it.
void testTheTapeRecordersFileNameIsBelowItsButton()
{
    const auto top = talas::app::tapeRecorderTop(600, 28, 4);
    require(top.filename.x == 0 && top.filename.y == 0 && top.file.x == top.filename.x
            && top.file.y == top.filename.bottom() + 4 && top.file.width > top.filename.width,
        "the selected file's name should be in the box immediately below the Filename button");
    require(top.format.y == 0 && top.bits.y == 0 && top.format.x > top.filename.right() && top.bits.x > top.format.right()
            && top.bits.right() == 600,
        "the format and bits menus should be at the right of the button's row");
}

void testWorkspaceModesRevealOnlyTheRelevantSecondaryWorkflow()
{
    const auto patch = talas::app::workbenchWorkspaceVisibility(
        talas::app::WorkbenchWorkspaceMode::Patch);
    require(
        patch.library && patch.inspector
            && !patch.timeline && !patch.analysis
            && patch.virtualControls,
        "Patch mode should pair the graph with discovery, inspection, and live controls");

    const auto automation = talas::app::workbenchWorkspaceVisibility(
        talas::app::WorkbenchWorkspaceMode::Automation);
    require(
        !automation.library && !automation.inspector
            && automation.timeline && !automation.analysis
            && automation.virtualControls,
        "Automation mode should replace unrelated side panels with Timeline");

    const auto analysis = talas::app::workbenchWorkspaceVisibility(
        talas::app::WorkbenchWorkspaceMode::Analysis);
    require(
        !analysis.library && !analysis.inspector
            && !analysis.timeline && analysis.analysis
            && !analysis.virtualControls,
        "Analysis mode should prioritise measurement and return control space to the graph");

    const auto performance = talas::app::workbenchWorkspaceVisibility(
        talas::app::WorkbenchWorkspaceMode::Performance);
    require(
        performance.library && !performance.inspector
            && performance.performance
            && !performance.timeline && !performance.analysis
            && !performance.virtualControls,
        "Performance mode should retain reusable discovery while the central canvas replaces the legacy Event Values strip");

    const auto samples = talas::app::workbenchWorkspaceVisibility(
        talas::app::WorkbenchWorkspaceMode::Samples);
    require(
        samples.samples
            && !samples.library && !samples.inspector
            && !samples.timeline && !samples.analysis
            && !samples.virtualControls && !samples.performance,
        "Samples mode should reserve the task surface for map and waveform authoring panels");

    // Playing a Sound with Event Values brings the
    // surface to the front, unless the preferences say otherwise.
    using Mode = talas::app::WorkbenchWorkspaceMode;
    require(talas::app::workspaceShowingControlsOnPlay(Mode::Performance, true, true) == Mode::Patch
            && talas::app::workspaceShowingControlsOnPlay(Mode::Analysis, true, true) == Mode::Patch
            && talas::app::workbenchWorkspaceVisibility(Mode::Patch).virtualControls
            && !talas::app::workspaceShowingControlsOnPlay(Mode::Patch, true, true)
            && !talas::app::workspaceShowingControlsOnPlay(Mode::Performance, false, true)
            && !talas::app::workspaceShowingControlsOnPlay(Mode::Performance, true, false),
        "playing a Sound with Event Values should bring the virtual control surface to the front");

    // With Edit > View...'s Parameters
    // below the signal flow, the signal flow takes the upper part and the
    // parameters the lower; beside it, the parameters are at the right.
    const auto below = talas::app::workbenchEditorParts(900, 600, true, 320, 8, 160);
    require(below.signalFlow.y == 0 && below.signalFlow.bottom() + 8 == below.parameters.y && below.parameters.bottom() == 600
            && below.signalFlow.width == 900 && below.parameters.width == 900 && below.signalFlow.height > below.parameters.height,
        "the signal flow should be the top section and the parameters the lower one");
    const auto beside = talas::app::workbenchEditorParts(900, 600, false, 320, 8, 160);
    require(beside.parameters.x == 580 && beside.parameters.width == 320 && beside.signalFlow.right() + 8 == beside.parameters.x
            && beside.parameters.height == 600,
        "beside the signal flow, the parameters should take the right of the area");
}

// With Edit > View...'s
// Prototypes across the top, the prototype strip is the window across the
// top of the screen, the Sound definitions and the editors below it; else
// the prototypes are a column at the left above the Sound definitions.
void testThePrototypeStripRunsAcrossTheTop()
{
    const auto strip = talas::app::workbenchLibraryParts(1200, 700, true, 280, 180, 168, 8);
    require(strip.prototypes.x == 0 && strip.prototypes.y == 0 && strip.prototypes.width == 1200
            && strip.prototypes.height == 168,
        "the prototype strip should be the window across the top of the screen");
    require(strip.soundDefinitions.y == 176 && strip.soundDefinitions.bottom() == 700 && strip.soundDefinitions.width == 280
            && strip.rest.x == 288 && strip.rest.y == 176 && strip.rest.right() == 1200 && strip.rest.bottom() == 700,
        "below the strip, the Sound definitions at the left and the editors beside them");
    const auto column = talas::app::workbenchLibraryParts(1200, 700, false, 280, 180, 168, 8);
    require(column.prototypes.x == 0 && column.prototypes.y == 0 && column.prototypes.width == 280
            && column.prototypes.bottom() + 8 == column.soundDefinitions.y && column.soundDefinitions.bottom() == 700
            && column.soundDefinitions.height == 180 && column.rest.x == 288 && column.rest.y == 0
            && column.rest.height == 700 && column.rest.right() == 1200,
        "at the left, the prototypes should be the column's upper part above the Sound definitions");
    const auto tiny = talas::app::workbenchLibraryParts(100, 50, true, 280, 180, 168, 8);
    require(tiny.prototypes.height == 50 && tiny.soundDefinitions.height == 0 && tiny.rest.width == 0
            && tiny.rest.height == 0,
        "a strip taller than the area should take it all, leaving nothing below");
}

void testScaledLayoutProtectsTheGraphBeforeRetainingSideWorkspaces()
{
    for (const auto scale : {
             talas::project::WorkspaceInterfaceScale::Percent100,
             talas::project::WorkspaceInterfaceScale::Percent115,
             talas::project::WorkspaceInterfaceScale::Percent130,
             talas::project::WorkspaceInterfaceScale::Percent150,
         })
    {
        talas::project::WorkspaceAccessibilityPreferences preferences;
        preferences.interfaceScale = scale;
        const auto accessibility =
            talas::app::workbenchAccessibilityProjection(
                preferences, 1.0, false);
        require(accessibility.has_value(),
            "each closed interface scale should provide layout evidence");
        const auto policy = talas::app::workbenchLayoutPolicyForSize(
            talas::app::workbenchMinimumWidth,
            talas::app::workbenchMinimumHeight,
            *accessibility);
        require(policy.has_value()
                && policy->useSummonableSideWorkspaces
                && !policy->showEditCommandsInToolbar
                && !policy->showWorkspaceCommandsInToolbar,
            "minimum windows should move side and secondary actions out before scaled graph geometry collapses");
        require(policy->libraryWidth >= 180
                && policy->inspectorWidth >= 300
                && policy->controlSurfaceHeight >= 120,
            "scaling should preserve or enlarge established logical control geometry");
    }

    talas::project::WorkspaceAccessibilityPreferences preferences;
    const auto accessibility = talas::app::workbenchAccessibilityProjection(
        preferences, 1.0, false);
    require(accessibility.has_value()
            && !talas::app::workbenchLayoutPolicyForSize(
                talas::app::workbenchMinimumWidth - 1,
                talas::app::workbenchMinimumHeight,
                *accessibility).has_value()
            && !talas::app::workbenchLayoutPolicyForSize(
                talas::app::workbenchMinimumWidth,
                talas::app::workbenchMinimumHeight - 1,
                *accessibility).has_value(),
        "scaled layout projection should reject geometry below the supported Workbench floor");

    preferences.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent150;
    const auto expandedAccessibility =
        talas::app::workbenchAccessibilityProjection(preferences, 1.0, false);
    const auto expanded = expandedAccessibility.has_value()
        ? talas::app::workbenchLayoutPolicyForSize(
            1800, 900, *expandedAccessibility)
        : std::nullopt;
    require(expanded.has_value()
            && expanded->soundDefinitionsHeight >= 380,
        "a supported tall 150-percent layout should make the complete Sound Library detail and project-action surface reachable");
}

void testDefaultWindowPreservesEstablishedWorkbenchComposition()
{
    const auto policy = talas::app::workbenchLayoutPolicyForSize(
        talas::app::workbenchDefaultWidth,
        talas::app::workbenchDefaultHeight);
    require(
        policy.widthClass == talas::app::WorkbenchWidthClass::Regular,
        "default window should use the regular policy");
    require(
        policy.libraryWidth == 220 && policy.inspectorWidth == 330,
        "default side columns should preserve the documented design system");
    require(
        policy.timelineHeight == 294 && policy.analysisHeight == 194,
        "default right-column rhythm should remain stable");
    require(
        policy.showEditCommandsInToolbar,
        "regular toolbar should expose frequent edit commands");
    require(
        policy.showWorkspaceCommandsInToolbar,
        "regular toolbar should expose workspace shortcuts");
}

void testExpandedWindowUsesAdditionalSpaceDeliberately()
{
    const auto policy = talas::app::workbenchLayoutPolicyForSize(1600, 900);
    require(
        policy.widthClass == talas::app::WorkbenchWidthClass::Expanded,
        "wide window should select the expanded policy");
    require(
        policy.libraryWidth > 220 && policy.inspectorWidth > 330,
        "expanded policy should grow supporting work zones deliberately");
    require(
        policy.timelineHeight > 294 && policy.analysisHeight > 194,
        "expanded policy should give measurement surfaces more room");
}

void testHeightAdaptationDoesNotJumpAtClassBoundaries()
{
    for (const auto boundary : {680, 820})
    {
        const auto before = talas::app::workbenchLayoutPolicyForSize(
            1180,
            boundary - 1);
        const auto after = talas::app::workbenchLayoutPolicyForSize(
            1180,
            boundary);
        require(
            std::abs(before.timelineHeight - after.timelineHeight) <= 2
                && std::abs(before.analysisHeight - after.analysisHeight) <= 2
                && std::abs(before.diagnosticsHeight - after.diagnosticsHeight) <= 2
                && std::abs(before.controlSurfaceHeight - after.controlSurfaceHeight) <= 2,
            "resizing across a height class should not make panels jump");
    }
}

void testWidthClassesDoNotMakeSideColumnsJump()
{
    for (const auto boundary : {1100, 1500})
    {
        const auto before = talas::app::workbenchLayoutPolicyForSize(
            boundary - 1,
            700);
        const auto after = talas::app::workbenchLayoutPolicyForSize(
            boundary,
            700);
        require(
            std::abs(before.libraryWidth - after.libraryWidth) <= 2
                && std::abs(before.inspectorWidth - after.inspectorWidth) <= 2,
            "resizing across a width class should not make side columns jump");
    }
}

void testSavedOutputOrderMapsPhysicalChannelsToCompactCallbackRanks()
{
    const std::array<std::uint8_t, 8> active{0, 1, 0, 1, 0, 1, 0, 0};
    const std::array<std::int32_t, 3> savedOrder{6, 2, 4};
    const auto mapping = talas::app::workbenchOutputChannelMap(active, savedOrder);
    require(
        mapping[0] == 2 && mapping[1] == 0 && mapping[2] == 1,
        "saved project ordinals should map to ascending compact device ranks");
    require(mapping[3] == -1,
            "unrequested project output ordinals should remain unavailable");

    const auto routes = talas::app::workbenchOutputChannelRoutes(
        active,
        savedOrder);
    require(
        routes[0].compactDeviceIndex == 2
            && routes[0].oneBasedPhysicalChannel == 6
            && routes[1].compactDeviceIndex == 0
            && routes[1].oneBasedPhysicalChannel == 2
            && routes[2].compactDeviceIndex == 1
            && routes[2].oneBasedPhysicalChannel == 4,
        "presentation routes should retain the exact one-based physical destination alongside callback rank");
    require(
        routes[3].compactDeviceIndex == -1
            && routes[3].oneBasedPhysicalChannel == -1,
        "an unrequested project ordinal should remain explicitly unattached");
}

void testSavedOutputOrderPreservesOrdinalsWhenDeviceAvailabilityChanges()
{
    const std::array<std::uint8_t, 8> active{0, 1, 0, 1, 0, 1, 0, 0};
    const std::array<std::int32_t, 2> staleOrder{6, 2};
    auto mapping = talas::app::workbenchOutputChannelMap(active, staleOrder);
    require(
        mapping[0] == 2 && mapping[1] == 0 && mapping[2] == -1,
        "active device channels absent from a saved order must stay untargeted");

    const std::array<std::uint8_t, 8> missingDestination{0, 1, 0, 1, 0, 0, 0, 0};
    mapping = talas::app::workbenchOutputChannelMap(missingDestination, staleOrder);
    require(
        mapping[0] == -1 && mapping[1] == 0 && mapping[2] == -1,
        "a missing saved physical destination must discard only its project ordinal");
    const auto missingRoutes = talas::app::workbenchOutputChannelRoutes(
        missingDestination,
        staleOrder);
    require(
        missingRoutes[0].compactDeviceIndex == -1
            && missingRoutes[0].oneBasedPhysicalChannel == 6
            && missingRoutes[1].compactDeviceIndex == 0
            && missingRoutes[1].oneBasedPhysicalChannel == 2,
        "missing hardware should preserve the requested physical destination for honest unavailable status");

    const std::array<std::int32_t, 3> duplicateOrder{2, 2, 4};
    mapping = talas::app::workbenchOutputChannelMap(active, duplicateOrder);
    require(
        mapping[0] == 0 && mapping[1] == -1 && mapping[2] == 1,
        "a duplicate saved destination must discard the duplicate ordinal without rerouting it");

    mapping = talas::app::workbenchOutputChannelMap(active, {});
    require(
        mapping[0] == 0 && mapping[1] == 1 && mapping[2] == 2,
        "only an absent saved order should use the active ascending fallback");

    const std::array<std::uint8_t, 8> fourAvailable{
        1, 1, 1, 1, 0, 0, 0, 0,
    };
    const auto fourChannelFallback =
        talas::app::workbenchOutputChannelRoutes(fourAvailable, {});
    for (std::size_t ordinal = 0; ordinal < 4; ++ordinal)
    {
        require(
            fourChannelFallback[ordinal].compactDeviceIndex
                    == static_cast<std::int32_t>(ordinal)
                && fourChannelFallback[ordinal].oneBasedPhysicalChannel
                    == static_cast<std::int32_t>(ordinal + 1),
            "absent saved routing should map every Output4 ordinal to ascending hardware");
    }

    const std::array<std::uint8_t, 8> twoAvailable{
        1, 1, 0, 0, 0, 0, 0, 0,
    };
    const auto partialFallback =
        talas::app::workbenchOutputChannelRoutes(twoAvailable, {});
    require(
        partialFallback[0].compactDeviceIndex == 0
            && partialFallback[1].compactDeviceIndex == 1
            && partialFallback[2].compactDeviceIndex == -1
            && partialFallback[3].compactDeviceIndex == -1,
        "Output4 fallback should preserve available prefix ordinals and discard only missing hardware destinations");

    const std::array<std::uint8_t, 8> noneAvailable{};
    const auto unavailableFallback =
        talas::app::workbenchOutputChannelRoutes(noneAvailable, {});
    for (std::size_t ordinal = 0; ordinal < 4; ++ordinal)
    {
        require(
            unavailableFallback[ordinal].compactDeviceIndex == -1
                && unavailableFallback[ordinal].oneBasedPhysicalChannel == -1,
            "Output4 fallback without hardware should leave every logical ordinal unattached");
    }

    const std::array<std::uint8_t, 8> eightAvailable{
        1, 1, 1, 1, 1, 1, 1, 1,
    };
    const auto eightChannelFallback =
        talas::app::workbenchOutputChannelRoutes(eightAvailable, {});
    for (std::size_t ordinal = 0; ordinal < 8; ++ordinal)
    {
        require(
            eightChannelFallback[ordinal].compactDeviceIndex
                    == static_cast<std::int32_t>(ordinal)
                && eightChannelFallback[ordinal].oneBasedPhysicalChannel
                    == static_cast<std::int32_t>(ordinal + 1),
            "absent saved routing should preserve every Output8 ordinal in ascending hardware order");
    }
}

void testNativeConfirmationAcceptsOnlyTheFirstButton()
{
    // Native message boxes return the 0-based button index; the dialogs list
    // the action first and Cancel second.
    require(talas::app::nativeConfirmationAccepted(0),
            "the first native button (the action) must confirm");
    require(!talas::app::nativeConfirmationAccepted(1),
            "the second native button (Cancel) must cancel");
    require(!talas::app::nativeConfirmationAccepted(-1)
                && !talas::app::nativeConfirmationAccepted(2),
            "unexpected native dialog results must fail closed");
}
}

void testDeviceSettingsChangeOnlyThroughTheDialogOrDeviceHistory()
{
    talas::project::ProjectDeviceSettings saved;
    saved.configured = true;
    saved.audioOutputDeviceName = "Studio Interface";
    saved.sampleRate = 48000.0;
    saved.bufferSize = 256;
    saved.outputChannels = {1, 2};
    auto fallback = saved;
    fallback.audioOutputDeviceName = "Built-in Output";

    require(!talas::app::workbenchHistoryStepReopensDevices(saved, saved),
        "undoing an unrelated edit must not reopen audio and MIDI devices");
    require(talas::app::workbenchHistoryStepReopensDevices(saved, fallback),
        "undoing a device change should reopen the devices it names");

    using Action = talas::app::WorkbenchDeviceNotificationAction;
    require(talas::app::workbenchDeviceNotificationAction(false, saved, fallback)
                == Action::KeepProjectPreference,
        "a host fallback after hot-plug must not rewrite the saved device or add history");
    require(talas::app::workbenchDeviceNotificationAction(true, saved, fallback)
                == Action::RecordInProject,
        "a change made in Audio & MIDI Settings should become project history");
    require(talas::app::workbenchDeviceNotificationAction(true, saved, saved)
                    == Action::Ignore
            && talas::app::workbenchDeviceNotificationAction(false, saved, saved)
                == Action::Ignore,
        "an unchanged device state should do nothing");
}

// A signal flow wider than its window has a scroll
// bar at the bottom covering the flow and the view; one in view has none.
void testTheSignalFlowScrollsWhenItDoesNotFit()
{
    using talas::app::WorkbenchGraphScroll;
    require(talas::app::workbenchGraphHorizontalScroll(0.0, 2000.0, 500.0, 900.0)
                == std::optional<WorkbenchGraphScroll>{WorkbenchGraphScroll{0.0, 2000.0, 500.0, 900.0}}
            && talas::app::workbenchGraphHorizontalScroll(100.0, 300.0, 200.0, 600.0)
                == std::optional<WorkbenchGraphScroll>{WorkbenchGraphScroll{100.0, 600.0, 200.0, 600.0}},
        "the scroll bar should span the flow and the view, the view its thumb");
    require(!talas::app::workbenchGraphHorizontalScroll(100.0, 300.0, 0.0, 800.0).has_value()
            && !talas::app::workbenchGraphHorizontalScroll(0.0, 0.0, 0.0, 800.0).has_value(),
        "a signal flow in view should need no scroll bar");
}

// Dragging the
// line gives the parameters more or less room, each side kept usable;
// taking over, the parameters have the whole area.
void testTheLineBetweenTheSignalFlowAndTheParameters()
{
    require(talas::app::workbenchParametersWidth(700, 100, 1100, 200) == 400,
        "the parameters should reach from the line to the right edge");
    require(talas::app::workbenchParametersWidth(150, 100, 1100, 200) == 800
            && talas::app::workbenchParametersWidth(1090, 100, 1100, 200) == 200,
        "neither the signal flow nor the parameters should be dragged away");
    require(talas::app::workbenchParametersWidth(std::nullopt, 330, false, 1000, 200) == 330
            && talas::app::workbenchParametersWidth(500, 330, false, 1000, 200) == 500
            && talas::app::workbenchParametersWidth(900, 330, false, 1000, 200) == 800
            && talas::app::workbenchParametersWidth(500, 330, true, 1000, 200) == 1000,
        "the parameters should keep the width dragged, within the area, or take it all over");
}

// Each parameter field has the parameter's name
// above it: the name first, the field last, its help and an input field's
// Sounds between; with no parameter open, neither.
void testEachFieldHasItsNameAboveIt()
{
    using Row = talas::app::ParameterFieldRow;
    require(talas::app::parameterFieldRows(true, false, false) == std::vector<Row>{Row::Name, Row::Field},
        "the parameter's name should be above its field");
    require(talas::app::parameterFieldRows(true, true, true) == std::vector<Row>{Row::Name, Row::Help, Row::InputSounds, Row::Field},
        "the name should stay on top, the help and an input field's Sounds between it and the field");
    require(talas::app::parameterFieldRows(false, true, false) == std::vector<Row>{Row::Help},
        "with no parameter open, there is neither a name nor a field");
}

int main()
{
    testEachFieldHasItsNameAboveIt();
    testNativeConfirmationAcceptsOnlyTheFirstButton();
    testFlyByHelpIsAYellowBox();
    testSelectedSoundsAreHighlightedInBlue();
    testTheLineBetweenTheSignalFlowAndTheParameters();
    testTheSignalFlowScrollsWhenItDoesNotFit();
    testMinimumWindowUsesCompactPolicyWithoutCollapsingGraph();
    testDefaultWindowPreservesEstablishedWorkbenchComposition();
    testExpandedWindowUsesAdditionalSpaceDeliberately();
    testHeightAdaptationDoesNotJumpAtClassBoundaries();
    testWidthClassesDoNotMakeSideColumnsJump();
    testSavedOutputOrderMapsPhysicalChannelsToCompactCallbackRanks();
    testSavedOutputOrderPreservesOrdinalsWhenDeviceAvailabilityChanges();
    testGraphFitTransformKeepsThePatchInsideTheViewport();
    testGraphFitTransformHandlesEmptyContent();
    testGraphFitTransformContainsVeryLargePatches();
    testNarrowRegularWindowMovesEditCommandsIntoMenus();
    testManualZoomPreservesVeryLargePatchFitFloor();
    testKeyboardCanvasNudgeSaturatesAtCoordinateBounds();
    testWorkspacePaneSplitRatioIsBoundedAndOverflowSafe();
    testStructureLayoutProjectionSeparatesAdaptiveAndExplicitAuthority();
    testGraphViewProjectionClosesEveryPresentationMode();
    testWorkspaceSessionWritesUseTrailingDebounceAndExplicitFlush();
    testWorkspaceModesRevealOnlyTheRelevantSecondaryWorkflow();
    testTheTapeRecordersFileNameIsBelowItsButton();
    testThePrototypeStripRunsAcrossTheTop();
    testScaledLayoutProtectsTheGraphBeforeRetainingSideWorkspaces();
    testDeviceSettingsChangeOnlyThroughTheDialogOrDeviceHistory();
    return 0;
}
