#pragma once

#include "talas/app/WorkbenchAccessibilityPolicy.h"
#include "talas/project/WorkspaceSessionState.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace talas::app
{
enum class WorkbenchWidthClass
{
    Compact,
    Regular,
    Expanded
};

enum class WorkbenchWorkspaceMode
{
    Patch,
    Automation,
    Analysis,
    Performance,
    Samples,
};

struct WorkbenchWorkspaceVisibility
{
    bool library = true;
    bool inspector = true;
    bool timeline = false;
    bool analysis = false;
    bool virtualControls = true;
    bool performance = false;
    bool samples = false;
};

struct WorkbenchLayoutPolicy
{
    WorkbenchWidthClass widthClass = WorkbenchWidthClass::Regular;
    int libraryWidth = 220;
    int inspectorWidth = 330;
    int soundDefinitionsHeight = 238;
    int timelineHeight = 294;
    int analysisHeight = 194;
    int diagnosticsHeight = 116;
    int controlSurfaceHeight = 138;
    bool showEditCommandsInToolbar = true;
    bool showWorkspaceCommandsInToolbar = true;
    bool useSummonableSideWorkspaces = false;
};

struct WorkbenchGraphViewTransform
{
    float scale = 1.0F;
    float offsetX = 0.0F;
    float offsetY = 0.0F;
};

enum class WorkbenchStructurePaneOrientation
{
    LeftRight,
    TopBottom,
};

struct WorkbenchStructureLayoutProjection
{
    WorkbenchStructurePaneOrientation orientation =
        WorkbenchStructurePaneOrientation::TopBottom;
    bool primaryPaneFirst = true;
};

struct WorkbenchGraphViewProjection
{
    bool showPorts = true;
    bool showWires = true;
    bool allowPortInteraction = true;
};

inline constexpr int workbenchMinimumWidth = 960;
inline constexpr int workbenchMinimumHeight = 640;
inline constexpr int workbenchDefaultWidth = 1180;
inline constexpr int workbenchDefaultHeight = 700;
inline constexpr std::uint32_t workbenchSessionWriteDebounceMilliseconds = 250;

class WorkbenchSessionWriteCoalescer
{
public:
    void noteChange(std::uint32_t nowMilliseconds) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool isDue(std::uint32_t nowMilliseconds) const noexcept;
    [[nodiscard]] bool consumeIfDue(
        std::uint32_t nowMilliseconds) noexcept;

private:
    std::uint32_t dueMilliseconds_ = 0;
    bool pending_ = false;
};

[[nodiscard]] WorkbenchLayoutPolicy workbenchLayoutPolicyForSize(
    int width,
    int height) noexcept;

[[nodiscard]] std::optional<WorkbenchLayoutPolicy> workbenchLayoutPolicyForSize(
    int width,
    int height,
    const WorkbenchAccessibilityProjection& accessibility) noexcept;

[[nodiscard]] WorkbenchGraphViewTransform workbenchGraphFitTransform(
    int viewportWidth,
    int viewportHeight,
    int contentX,
    int contentY,
    int contentWidth,
    int contentHeight,
    int padding = 32) noexcept;

[[nodiscard]] float workbenchGraphZoomScale(
    float currentScale,
    float factor,
    float fitScale) noexcept;

[[nodiscard]] std::int32_t workbenchNudgedCanvasCoordinate(
    std::int32_t current,
    int delta) noexcept;

[[nodiscard]] double workbenchPaneSplitRatio(
    int pointerPosition,
    int areaOrigin,
    int areaExtent) noexcept;

// The scroll bar at the bottom of the signal flow:
// in the flow's own units, the span its Sounds and the view cover together
// and the part the view shows; none while every Sound is in view.
struct WorkbenchGraphScroll
{
    double start = 0.0;
    double end = 0.0;
    double viewStart = 0.0;
    double viewEnd = 0.0;

    bool operator==(const WorkbenchGraphScroll&) const = default;
};

[[nodiscard]] std::optional<WorkbenchGraphScroll> workbenchGraphHorizontalScroll(
    double contentLeft, double contentRight, double viewLeft, double viewRight) noexcept;

// The line between the signal flow and the parameters: dragged, the parameters
// reach from the pointer to the area's right edge, the signal flow and the
// parameters each kept at least `minimum` wide; taking over the whole area,
// the parameters have all of it.
[[nodiscard]] int workbenchParametersWidth(int pointerX, int areaLeft, int areaRight, int minimum) noexcept;

// Each parameter field has the parameter's name
// above it. The rows the parameters stack for the field open, top to
// bottom: the parameter's name (the chooser naming it, from which the
// other fields are reached), its help (comment and range), an input field's
// Sounds as icons, then the field itself; with no parameter open, neither name
// nor field.
enum class ParameterFieldRow : std::uint8_t
{
    Name,
    Help,
    InputSounds,
    Field,
};
[[nodiscard]] std::vector<ParameterFieldRow> parameterFieldRows(bool parameterOpen, bool help, bool inputSounds);
// The Sound editor's
// two parts, the signal flow and the parameters, in an area `width` by
// `height`: the parameters `besideWidth` wide at the right, or, below,
// the lower part of the area (at least `minimum` of each where there is
// room), the signal flow above them.
struct WorkbenchEditorArea
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }
};
struct WorkbenchEditorParts
{
    WorkbenchEditorArea signalFlow;
    WorkbenchEditorArea parameters;
};
[[nodiscard]] WorkbenchEditorParts workbenchEditorParts(
    int width, int height, bool parametersBelow, int besideWidth, int gap, int minimum) noexcept;
// The prototype strip, the
// window across the top of the screen, `stripHeight` high, when the View
// asks for the strip across the top, the Sound definitions a column
// `libraryWidth` wide at the left below it; else the prototypes are that
// column's upper part above the Sound definitions, `soundDefinitionsHeight`
// high. The rest of an area `width` by `height` is the editors'.
struct WorkbenchLibraryParts
{
    WorkbenchEditorArea prototypes;
    WorkbenchEditorArea soundDefinitions;
    WorkbenchEditorArea rest;
};
[[nodiscard]] WorkbenchLibraryParts workbenchLibraryParts(int width, int height, bool stripAcrossTop,
    int libraryWidth, int soundDefinitionsHeight, int stripHeight, int gap) noexcept;
[[nodiscard]] int workbenchParametersWidth(
    std::optional<int> dragged, int policyWidth, bool takeOver, int areaWidth, int minimum) noexcept;
// The Tape Recorder's top: the Filename button at
// the upper left, the selected file's name in the box immediately below
// it, and the file's format and bits menus at the right of the button's
// row; for a top `width` wide, rows `rowHeight` high `gap` apart.
struct TapeRecorderTop
{
    WorkbenchEditorArea filename;
    WorkbenchEditorArea file;
    WorkbenchEditorArea format;
    WorkbenchEditorArea bits;
};
[[nodiscard]] TapeRecorderTop tapeRecorderTop(int width, int rowHeight, int gap) noexcept;

[[nodiscard]] WorkbenchStructureLayoutProjection
workbenchStructureLayoutProjection(
    talas::project::WorkspaceStructureSplitOrientation orientation,
    bool primaryPaneFirst,
    int availableWidth,
    int availableHeight) noexcept;

[[nodiscard]] WorkbenchGraphViewProjection
workbenchGraphViewProjection(
    talas::project::WorkspaceGraphViewMode mode) noexcept;

[[nodiscard]] WorkbenchWorkspaceVisibility workbenchWorkspaceVisibility(
    WorkbenchWorkspaceMode mode) noexcept;

// A Sound with Event Values played brings
// the virtual control surface to the front, unless the preferences say
// otherwise: the workspace to switch to from one that hides it, Analysis or
// Performance; nothing when the surface is already there.
[[nodiscard]] std::optional<WorkbenchWorkspaceMode> workspaceShowingControlsOnPlay(
    WorkbenchWorkspaceMode mode, bool openOnPlay, bool hasControls) noexcept;

// Native confirmation dialogs (juce::NativeMessageBox::showAsync) report the
// 0-based index of the pressed button. Destructive and irreversible actions
// list the action button first and Cancel second, so only index 0 confirms;
// every other result, including a dismissed dialog, cancels.
[[nodiscard]] constexpr bool nativeConfirmationAccepted(int buttonIndex) noexcept
{
    return buttonIndex == 0;
}

// Fly-by help: what a control or a
// parameter's name does, shown in a yellow box as the pointer rests over
// it; the box, its text and its edge as ARGB.
struct FlyByHelpColours
{
    std::uint32_t box = 0;
    std::uint32_t text = 0;
    std::uint32_t outline = 0;
};
[[nodiscard]] FlyByHelpColours flyByHelpColours() noexcept;

// Selected Sounds are highlighted in blue: a selected
// row (the prototypes, a Sound file window) and a Sound selected in the
// signal flow, as ARGB; in gray when the preferences ask for gray.
struct SelectionHighlightColours
{
    std::uint32_t row = 0;
    std::uint32_t soundInSignalFlow = 0;
};
[[nodiscard]] SelectionHighlightColours selectionHighlightColours(bool gray = false) noexcept;
}
