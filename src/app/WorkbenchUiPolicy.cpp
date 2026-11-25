#include "talas/app/WorkbenchUiPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace talas::app
{
void WorkbenchSessionWriteCoalescer::noteChange(
    std::uint32_t nowMilliseconds) noexcept
{
    dueMilliseconds_ = nowMilliseconds
        + workbenchSessionWriteDebounceMilliseconds;
    pending_ = true;
}

void WorkbenchSessionWriteCoalescer::clear() noexcept
{
    pending_ = false;
}

bool WorkbenchSessionWriteCoalescer::pending() const noexcept
{
    return pending_;
}

bool WorkbenchSessionWriteCoalescer::isDue(
    std::uint32_t nowMilliseconds) const noexcept
{
    return pending_
        && static_cast<std::int32_t>(
               nowMilliseconds - dueMilliseconds_)
            >= 0;
}

bool WorkbenchSessionWriteCoalescer::consumeIfDue(
    std::uint32_t nowMilliseconds) noexcept
{
    if (!isDue(nowMilliseconds))
        return false;
    clear();
    return true;
}

WorkbenchLayoutPolicy workbenchLayoutPolicyForSize(
    int width,
    int height) noexcept
{
    WorkbenchLayoutPolicy policy;

    if (width < 1100)
    {
        policy.widthClass = WorkbenchWidthClass::Compact;
        constexpr int compactRange =
            1100 - workbenchMinimumWidth;
        const auto progress = std::clamp(
            width - workbenchMinimumWidth,
            0,
            compactRange);
        const auto interpolate = [progress](int compact, int regular) {
            return compact
                + ((regular - compact) * progress
                   + compactRange / 2)
                    / compactRange;
        };
        policy.libraryWidth = interpolate(180, 220);
        policy.inspectorWidth = interpolate(300, 330);
        policy.showEditCommandsInToolbar = false;
        policy.useSummonableSideWorkspaces = true;
    }
    else if (width >= 1500)
    {
        policy.widthClass = WorkbenchWidthClass::Expanded;
        constexpr int expandedRange = 100;
        const auto progress = std::clamp(
            width - 1500,
            0,
            expandedRange);
        const auto interpolate = [progress](int regular, int expanded) {
            return regular
                + ((expanded - regular) * progress
                   + expandedRange / 2)
                    / expandedRange;
        };
        policy.libraryWidth = interpolate(220, 240);
        policy.inspectorWidth = interpolate(330, 360);
    }

    if (width < 1160)
        policy.showEditCommandsInToolbar = false;

    if (height < workbenchDefaultHeight)
    {
        constexpr int adaptationRange =
            workbenchDefaultHeight - workbenchMinimumHeight;
        const auto progress = std::clamp(
            height - workbenchMinimumHeight,
            0,
            adaptationRange);
        const auto interpolate = [progress](int compact, int regular) {
            return compact
                + ((regular - compact) * progress
                   + adaptationRange / 2)
                    / adaptationRange;
        };
        policy.soundDefinitionsHeight = interpolate(210, 238);
        policy.timelineHeight = interpolate(238, 294);
        policy.analysisHeight = interpolate(170, 194);
        policy.diagnosticsHeight = interpolate(96, 116);
        policy.controlSurfaceHeight = interpolate(120, 138);
    }
    else if (height >= 820)
    {
        constexpr int expandedRange = 80;
        const auto progress = std::clamp(
            height - 820,
            0,
            expandedRange);
        const auto interpolate = [progress](int regular, int expanded) {
            return regular
                + ((expanded - regular) * progress
                   + expandedRange / 2)
                    / expandedRange;
        };
        policy.soundDefinitionsHeight = interpolate(238, 260);
        policy.timelineHeight = interpolate(294, 320);
        policy.analysisHeight = interpolate(194, 216);
        policy.diagnosticsHeight = interpolate(116, 126);
        policy.controlSurfaceHeight = interpolate(138, 148);
    }

    return policy;
}

std::optional<WorkbenchLayoutPolicy> workbenchLayoutPolicyForSize(
    int width,
    int height,
    const WorkbenchAccessibilityProjection& accessibility) noexcept
{
    if (width < workbenchMinimumWidth || height < workbenchMinimumHeight)
        return std::nullopt;

    auto policy = workbenchLayoutPolicyForSize(width, height);
    const auto scale = accessibility.interfaceScale();

    const auto scaled = [&accessibility](int value) {
        return workbenchScaledLogicalPixels(
            value, accessibility.interfaceScalePreset());
    };
    policy.libraryWidth = scaled(policy.libraryWidth);
    policy.inspectorWidth = scaled(policy.inspectorWidth);
    policy.soundDefinitionsHeight = scaled(policy.soundDefinitionsHeight);
    policy.timelineHeight = scaled(policy.timelineHeight);
    policy.analysisHeight = scaled(policy.analysisHeight);
    policy.diagnosticsHeight = scaled(policy.diagnosticsHeight);
    policy.controlSurfaceHeight = scaled(policy.controlSurfaceHeight);

    if (scale > 1.0F)
    {
        policy.showEditCommandsInToolbar = false;
        if (width < 1500 || scale >= 1.3F)
            policy.useSummonableSideWorkspaces = true;
    }
    if (scale >= 1.3F || width < 1100)
        policy.showWorkspaceCommandsInToolbar = false;
    return policy;
}

WorkbenchGraphViewTransform workbenchGraphFitTransform(
    int viewportWidth,
    int viewportHeight,
    int contentX,
    int contentY,
    int contentWidth,
    int contentHeight,
    int padding) noexcept
{
    if (viewportWidth <= 0 || viewportHeight <= 0
        || contentWidth <= 0 || contentHeight <= 0)
    {
        return {};
    }

    const auto safePadding = std::max(0, padding);
    const auto availableWidth = std::max(
        1,
        viewportWidth - safePadding * 2);
    const auto availableHeight = std::max(
        1,
        viewportHeight - safePadding * 2);
    const auto horizontalScale = static_cast<float>(availableWidth)
        / static_cast<float>(contentWidth);
    const auto verticalScale = static_cast<float>(availableHeight)
        / static_cast<float>(contentHeight);
    const auto scale = std::min({
        horizontalScale,
        verticalScale,
        1.0F,
    });
    const auto fittedWidth = static_cast<float>(contentWidth) * scale;
    const auto fittedHeight = static_cast<float>(contentHeight) * scale;

    return {
        scale,
        (static_cast<float>(viewportWidth) - fittedWidth) * 0.5F
            - static_cast<float>(contentX) * scale,
        (static_cast<float>(viewportHeight) - fittedHeight) * 0.5F
            - static_cast<float>(contentY) * scale,
    };
}

float workbenchGraphZoomScale(
    float currentScale,
    float factor,
    float fitScale) noexcept
{
    const auto safeCurrent = std::isfinite(currentScale)
            && currentScale > 0.0F
        ? currentScale
        : 1.0F;
    const auto safeFactor = std::isfinite(factor)
            && factor > 0.0F
        ? factor
        : 1.0F;
    const auto safeFit = std::isfinite(fitScale)
            && fitScale > 0.0F
        ? fitScale
        : 0.25F;
    return std::clamp(
        safeCurrent * safeFactor,
        std::min(0.25F, safeFit),
        1.6F);
}

std::int32_t workbenchNudgedCanvasCoordinate(
    std::int32_t current,
    int delta) noexcept
{
    const auto translated = static_cast<std::int64_t>(current)
        + static_cast<std::int64_t>(delta);
    return static_cast<std::int32_t>(std::clamp(
        translated,
        std::int64_t{0},
        static_cast<std::int64_t>(
            std::numeric_limits<std::int32_t>::max())));
}

double workbenchPaneSplitRatio(
    int pointerPosition,
    int areaOrigin,
    int areaExtent) noexcept
{
    if (areaExtent <= 0)
        return 0.5;
    const auto offset = static_cast<std::int64_t>(pointerPosition)
        - static_cast<std::int64_t>(areaOrigin);
    const auto ratio = static_cast<double>(offset)
        / static_cast<double>(areaExtent);
    return std::clamp(ratio, 0.2, 0.8);
}

std::optional<WorkbenchGraphScroll> workbenchGraphHorizontalScroll(
    double contentLeft, double contentRight, double viewLeft, double viewRight) noexcept
{
    if (!(contentRight > contentLeft) || !(viewRight > viewLeft)
        || (contentLeft >= viewLeft && contentRight <= viewRight))
        return std::nullopt;
    return WorkbenchGraphScroll{std::min(contentLeft, viewLeft), std::max(contentRight, viewRight), viewLeft, viewRight};
}

int workbenchParametersWidth(int pointerX, int areaLeft, int areaRight, int minimum) noexcept
{
    const auto width = static_cast<std::int64_t>(areaRight) - pointerX;
    const auto widest = static_cast<std::int64_t>(areaRight) - areaLeft - minimum;
    return static_cast<int>(std::clamp<std::int64_t>(width, std::min<std::int64_t>(minimum, std::max<std::int64_t>(0, widest)),
        std::max<std::int64_t>(minimum, widest)));
}

std::vector<ParameterFieldRow> parameterFieldRows(bool parameterOpen, bool help, bool inputSounds)
{
    std::vector<ParameterFieldRow> rows;
    if (parameterOpen)
        rows.push_back(ParameterFieldRow::Name);
    if (help)
        rows.push_back(ParameterFieldRow::Help);
    if (inputSounds)
        rows.push_back(ParameterFieldRow::InputSounds);
    if (parameterOpen)
        rows.push_back(ParameterFieldRow::Field);
    return rows;
}

int workbenchParametersWidth(std::optional<int> dragged, int policyWidth, bool takeOver, int areaWidth, int minimum) noexcept
{
    if (takeOver)
        return std::max(0, areaWidth);
    const auto widest = std::max(0, areaWidth - minimum);
    return std::clamp(dragged.value_or(policyWidth), std::min(minimum, widest), std::max(minimum, widest));
}

WorkbenchStructureLayoutProjection workbenchStructureLayoutProjection(
    talas::project::WorkspaceStructureSplitOrientation orientation,
    bool primaryPaneFirst,
    int availableWidth,
    int availableHeight) noexcept
{
    auto effective = WorkbenchStructurePaneOrientation::TopBottom;
    switch (orientation)
    {
    case talas::project::WorkspaceStructureSplitOrientation::Adaptive:
        effective = availableWidth >= 840 && availableHeight >= 260
            ? WorkbenchStructurePaneOrientation::LeftRight
            : WorkbenchStructurePaneOrientation::TopBottom;
        break;
    case talas::project::WorkspaceStructureSplitOrientation::LeftRight:
        effective = WorkbenchStructurePaneOrientation::LeftRight;
        break;
    case talas::project::WorkspaceStructureSplitOrientation::TopBottom:
        effective = WorkbenchStructurePaneOrientation::TopBottom;
        break;
    }
    return {effective, primaryPaneFirst};
}

WorkbenchGraphViewProjection workbenchGraphViewProjection(
    talas::project::WorkspaceGraphViewMode mode) noexcept
{
    switch (mode)
    {
    case talas::project::WorkspaceGraphViewMode::Compact:
        return {false, false, false};
    case talas::project::WorkspaceGraphViewMode::Ports:
        return {true, false, true};
    case talas::project::WorkspaceGraphViewMode::PortsAndWires:
        return {true, true, true};
    }
    return {false, false, false};
}

WorkbenchEditorParts workbenchEditorParts(
    int width, int height, bool parametersBelow, int besideWidth, int gap, int minimum) noexcept
{
    width = std::max(0, width);
    height = std::max(0, height);
    gap = std::max(0, gap);
    WorkbenchEditorParts parts;
    if (!parametersBelow)
    {
        const auto beside = std::clamp(besideWidth, 0, width);
        parts.parameters = {width - beside, 0, beside, height};
        parts.signalFlow = {0, 0, std::max(0, width - beside - gap), height};
        return parts;
    }
    auto below = (height - gap) * 2 / 5;
    if (height - gap >= 2 * minimum)
        below = std::clamp(below, minimum, height - gap - minimum);
    below = std::clamp(below, 0, height);
    parts.parameters = {0, height - below, width, below};
    parts.signalFlow = {0, 0, width, std::max(0, height - below - gap)};
    return parts;
}

WorkbenchLibraryParts workbenchLibraryParts(int width, int height, bool stripAcrossTop,
    int libraryWidth, int soundDefinitionsHeight, int stripHeight, int gap) noexcept
{
    width = std::max(0, width);
    height = std::max(0, height);
    gap = std::max(0, gap);
    const auto column = std::clamp(libraryWidth, 0, width);
    const auto restX = std::min(width, column + gap);
    WorkbenchLibraryParts parts;
    if (stripAcrossTop)
    {
        const auto strip = std::clamp(stripHeight, 0, height);
        const auto belowY = std::min(height, strip + gap);
        const auto below = height - belowY;
        parts.prototypes = {0, 0, width, strip};
        parts.soundDefinitions = {0, belowY, column, below};
        parts.rest = {restX, belowY, width - restX, below};
        return parts;
    }
    const auto definitions = std::clamp(soundDefinitionsHeight, 0, height);
    parts.soundDefinitions = {0, height - definitions, column, definitions};
    parts.prototypes = {0, 0, column, std::max(0, height - definitions - gap)};
    parts.rest = {restX, 0, width - restX, height};
    return parts;
}

TapeRecorderTop tapeRecorderTop(int width, int rowHeight, int gap) noexcept
{
    width = std::max(0, width);
    rowHeight = std::max(0, rowHeight);
    gap = std::max(0, gap);
    TapeRecorderTop top;
    top.filename = {0, 0, std::min(84, width), rowHeight};
    const auto menus = std::clamp(width - top.filename.width - 8, 0, 166);
    top.bits = {width - menus / 2, 0, menus / 2, rowHeight};
    top.format = {width - menus, 0, std::max(0, menus / 2 - 6), rowHeight};
    top.file = {0, rowHeight + gap, width, rowHeight};
    return top;
}

std::optional<WorkbenchWorkspaceMode> workspaceShowingControlsOnPlay(
    WorkbenchWorkspaceMode mode, bool openOnPlay, bool hasControls) noexcept
{
    if (!openOnPlay || !hasControls
        || (mode != WorkbenchWorkspaceMode::Analysis && mode != WorkbenchWorkspaceMode::Performance))
        return std::nullopt;
    return WorkbenchWorkspaceMode::Patch;
}

WorkbenchWorkspaceVisibility workbenchWorkspaceVisibility(
    WorkbenchWorkspaceMode mode) noexcept
{
    switch (mode)
    {
    case WorkbenchWorkspaceMode::Patch:
        return {
            true,
            true,
            false,
            false,
            true,
            false,
            false,
        };
    case WorkbenchWorkspaceMode::Automation:
        return {
            false,
            false,
            true,
            false,
            true,
            false,
            false,
        };
    case WorkbenchWorkspaceMode::Analysis:
        return {
            false,
            false,
            false,
            true,
            false,
            false,
            false,
        };
    case WorkbenchWorkspaceMode::Performance:
        return {
            true,
            false,
            false,
            false,
            false,
            true,
            false,
        };
    case WorkbenchWorkspaceMode::Samples:
        return {
            false,
            false,
            false,
            false,
            false,
            false,
            true,
        };
    }
    return {};
}

FlyByHelpColours flyByHelpColours() noexcept
{
    return {0xfffff2a6u, 0xff1c1a10u, 0xff9a8740u};
}

SelectionHighlightColours selectionHighlightColours(bool gray) noexcept
{
    return gray ? SelectionHighlightColours{0xff4a4a4au, 0xffc8c8c8u} : SelectionHighlightColours{0xff33465au, 0xffd9e7ffu};
}
}
