#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <string_view>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Arranging the Virtual Control Surface: Clean
// up in columns or Fit to the window, widgets moved and resized in Edit
// mode, selected one by one or by a box, squeezed together, their places
// typed into the editor's fields, and the surface larger than its window.
namespace talas::app
{
using VcsRect = talas::project::VirtualControlBounds;

enum class VcsCorner
{
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// Clean up: the widgets in order in the given number of columns,
// each in a cell of the given size.
[[nodiscard]] std::vector<VcsRect> vcsCleanUp(std::size_t count, unsigned columns, int cellWidth, int cellHeight, int gap = 8);

// Each type of control on a row of its own, in
// this order: annotations, displays, faders, gates, small faders, toggles.
enum class VcsTypeRow
{
    Annotation,
    Display,
    Fader,
    Gate,
    SmallFader,
    Toggle,
};

[[nodiscard]] VcsTypeRow vcsTypeRow(const talas::project::VirtualControl& control) noexcept;

// Clean up by type: the
// widgets kept in the order given within their type, each type starting a
// row of its own and wrapping at the columns; a cell for each widget given.
[[nodiscard]] std::vector<VcsRect> vcsCleanUpByType(
    const std::vector<VcsTypeRow>& types, unsigned columns, int cellWidth, int cellHeight, int gap = 8);

// The area the widgets cover, from the surface's corner.
[[nodiscard]] VcsRect vcsExtent(const std::vector<VcsRect>& rects);

// Fit: the arrangement scaled into a window of the size given.
[[nodiscard]] std::vector<VcsRect> vcsFit(const std::vector<VcsRect>& rects, int width, int height);

// Dragging the selected widgets, never past the surface's top or
// left edge.
[[nodiscard]] std::vector<VcsRect> vcsMoved(
    const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, int dx, int dy);

// Dragging a black square at a corner: the widget resized, never
// smaller than the minimum.
[[nodiscard]] VcsRect vcsResized(VcsRect rect, VcsCorner corner, int dx, int dy, int minimum = 16);
// Several selected widgets resized at once: the
// drag on one widget's corner resizes every selected one alike.
[[nodiscard]] std::vector<VcsRect> vcsResizedTogether(
    std::vector<VcsRect> rects, const std::set<std::size_t>& selection, VcsCorner corner, int dx, int dy, int minimum = 16);

// The widgets a box drawn from an empty area touches.
[[nodiscard]] std::set<std::size_t> vcsBoxSelection(const std::vector<VcsRect>& rects, VcsRect box);

// Shift+Click adds a widget to the selection or takes it out.
[[nodiscard]] std::set<std::size_t> vcsShiftClicked(std::set<std::size_t> selection, std::size_t widget);

// Arrange > Squeeze Vertical or Horizontal: the selected widgets
// moved together in their order, the space between them squeezed out.
[[nodiscard]] std::vector<VcsRect> vcsSqueezed(
    const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, bool vertical, int gap = 2);

// An upright fader's
// numeric field is at its top, above the fader, where a click selects its
// contents to type over; a horizontal fader's is at its right; a widget
// hiding its number has none.
enum class VcsNumberPlacement
{
    None,
    Above,
    Right,
};
[[nodiscard]] VcsNumberPlacement vcsFaderNumberPlacement(bool upright, bool showNumber) noexcept;

// The VCS Editor's Selected Value: the selected widget's name, or
// More than one.
[[nodiscard]] std::string vcsSelectedValueText(const std::vector<std::string>& names);

// The Left / Top / Rt / Bot and Left / Top / Wd / Ht fields.
[[nodiscard]] std::optional<VcsRect> vcsRectFromEdges(int left, int top, int right, int bottom);
[[nodiscard]] std::optional<VcsRect> vcsRectFromSize(int left, int top, int width, int height);

// A window smaller than the surface cuts some of it off, and shows the box
// that moves the view over it.
[[nodiscard]] bool vcsSurfaceCutOff(const VcsRect& extent, int windowWidth, int windowHeight) noexcept;
// Clean up on a surface no larger than its window fits the
// cleaned-up widgets to the window, as Fit does on a larger surface, so
// they grow and shrink with it; on a larger surface they keep their size.
[[nodiscard]] bool vcsCleanUpFitsWindow(const VcsRect& surface, int windowWidth, int windowHeight) noexcept;
// The box: a gray box of the whole surface's shape, `size` pixels
// on its longer side, and in it the blue box of the part the window shows,
// both from the box's corner; nothing while the window shows it all.
// Dragging the blue box by (dx, dy) of the box's pixels moves the view over
// the surface by as much of it, never past its edges.
struct VcsNavigator
{
    VcsRect surface;
    VcsRect view;
    double scale = 1.0;

    bool operator==(const VcsNavigator&) const = default;
};

[[nodiscard]] std::optional<VcsNavigator> vcsNavigator(
    const VcsRect& extent, int viewLeft, int viewTop, int windowWidth, int windowHeight, int size) noexcept;
[[nodiscard]] std::pair<int, int> vcsNavigatorDragged(const VcsNavigator& navigator, const VcsRect& extent, int viewLeft,
    int viewTop, int windowWidth, int windowHeight, int dx, int dy) noexcept;

// The hand: with the Control key down, a drag
// pushes the surface the way the pointer moves, the view going the other
// way, never past the surface's edges.
[[nodiscard]] std::pair<int, int> vcsHandDragged(const VcsRect& extent, int viewLeft, int viewTop, int windowWidth,
    int windowHeight, int dx, int dy) noexcept;

// A multilevel surface's layouts: the top
// layout with every control, then a layout for each Sound with controls of
// its own, its dots telling how far upstream of the output it lies, then
// the layouts made in the editor.
struct VcsLayoutEntry
{
    std::string name;
    std::size_t depth = 0;
    std::vector<std::string> eventNames;
    // A layout made in the editor, kept with the project.
    bool made = false;

    bool operator==(const VcsLayoutEntry&) const = default;
};

[[nodiscard]] std::vector<VcsLayoutEntry> vcsLayouts(const talas::project::ProjectDocument& document);
// The surface is global and changes whenever a
// different Sound is loaded. With a Sound of the project loaded alone, as
// Ctrl+Space and the Info menu load one (`loaded`, the structure with that
// Sound, the node `sound`, feeding the output), the layouts are that
// Sound's: the top one, named for it, holds only the controls of the Sounds
// from the output upstream, a layout for each of them follows, and then the
// layouts made in the editor, which are the project's (`document`'s). A
// Sound that is no longer the project's leaves the project's layouts.
[[nodiscard]] std::vector<VcsLayoutEntry> vcsLoadedSoundLayouts(
    const talas::project::ProjectDocument& document, const talas::project::ProjectDocument& loaded, std::string_view sound);
// The name the list shows: dots for each step upstream, then the name.
[[nodiscard]] std::string vcsLayoutLabel(const VcsLayoutEntry& layout);
// Shift with the up or down arrow: the layout above or below, staying at
// the ends.
[[nodiscard]] std::size_t vcsAdjacentLayout(std::size_t count, std::size_t current, bool up) noexcept;
// Duplicate: a layout of the editor's own with the layout's
// controls, named for it, to rename and change.
[[nodiscard]] talas::project::VirtualControlLayoutPage vcsDuplicatedLayout(const VcsLayoutEntry& layout);
// Shift+Click in the list of layouts extends the layouts chosen
// to merge: the one shown and those Shift+Clicked, one clicked again taken
// out.
[[nodiscard]] std::set<std::size_t> vcsLayoutsShiftClicked(
    std::set<std::size_t> chosen, std::size_t shown, std::size_t clicked);
// Merge into new layout: every control of the layouts chosen, once.
[[nodiscard]] talas::project::VirtualControlLayoutPage vcsMergedLayout(std::string name, const std::vector<VcsLayoutEntry>& layouts);
// Remove selected widgets: the layout without them.
[[nodiscard]] talas::project::VirtualControlLayoutPage vcsLayoutWithout(
    talas::project::VirtualControlLayoutPage layout, const std::set<std::string>& removed);
// A layout is its list of presets with the
// types, places and ranges of its widgets. Every layout but the project's
// top one may keep its own (format 155); a layout keeping none takes the
// surface's shared ones, as every layout of an older project does. A layout
// is named here as the list names it; an empty name is the top layout.
[[nodiscard]] const talas::project::VirtualControlLayoutSettings* findVcsLayoutSettings(
    const talas::project::ProjectDocument& document, std::string_view layout);
// The controls as the layout shows them: each the layout keeps a widget for
// takes its kind, widget and range (as far as it lies within the Event
// Value's own), its value shown within that range; the rest as shared.
[[nodiscard]] std::vector<talas::project::VirtualControl> vcsLayoutControls(
    const talas::project::ProjectDocument& document, std::string_view layout);
// The layout's arrangement: its own, or the surface's.
[[nodiscard]] talas::project::VirtualControlLayout vcsLayoutArrangement(
    const talas::project::ProjectDocument& document, std::string_view layout);
// The layout arranged in the VCS Editor: each widget given at its place on
// that layout alone, the layout following the arrangement given; a widget
// the layout does not keep yet starts as the shared one.
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsPlaced(
    const talas::project::ProjectDocument& document, std::string_view layout, talas::project::VirtualControlLayout arrangement,
    const std::vector<std::pair<std::string, std::optional<talas::project::VirtualControlBounds>>>& places);
// The VCS Editor's options for a widget on the layout, kept as its own:
// nothing when the Event Value has no control, the range does not lie
// within the Event Value's own, or the widget does not suit the range.
[[nodiscard]] std::optional<std::vector<talas::project::VirtualControlLayoutSettings>> vcsLayoutSettingsWidgetSet(
    const talas::project::ProjectDocument& document, std::string_view layout, std::string_view eventName,
    const talas::project::VirtualControlWidget& widget, double minimum, double maximum);
// The layout's presets kept in the bank named (format 155).
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsWithPresets(
    const talas::project::ProjectDocument& document, std::string_view layout, std::string bankId);
// A layout made in the editor renamed or removed takes what it keeps of its
// own along; a duplicate starts with a copy of its widgets and arrangement.
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsRenamed(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view from, const std::string& to);
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsWithout(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view layout);
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsCopied(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view from, const std::string& to);
// The VCS Editor's right-most menu, Options...: every option that
// adds, hides or removes widgets, in its order: Add Widget (a text or a
// box), Edit Text..., the selected widgets hidden from the layout (kept on
// the surface and in the other layouts), and the selected texts and boxes
// removed.
[[nodiscard]] const std::vector<std::string_view>& vcsOptionsMenuItems();

// Arrange > Align and Distribute: the selected widgets lined up on
// an edge or their centres, or spread evenly between the first and last.
enum class VcsAlignment
{
    Left,
    Right,
    Top,
    Bottom,
    HorizontalCentres,
    VerticalCentres,
};

[[nodiscard]] std::vector<VcsRect> vcsAligned(
    const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, VcsAlignment alignment);
[[nodiscard]] std::vector<VcsRect> vcsDistributed(
    const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, bool horizontal);

// The surface's texts and boxes among its widgets: each named so that no Event
// Value is, for selecting, moving and resizing them as widgets are.
[[nodiscard]] std::string vcsDecorationName(std::size_t index);
[[nodiscard]] std::optional<std::size_t> vcsDecorationIndex(std::string_view name) noexcept;
// Options > Add Widget > Text or Box: one beside the widgets at the top of
// the surface; text pasted into the surface fills all of it at first.
[[nodiscard]] talas::project::VirtualControlDecoration vcsAddedDecoration(
    talas::project::VirtualControlDecorationKind kind, const VcsRect& widgets, std::string text = {});
[[nodiscard]] talas::project::VirtualControlDecoration vcsPastedText(std::string text, const VcsRect& surface);
// The words typed into a selected text's Widget Label field become
// what it shows; none for a box, or an index not on the surface.
[[nodiscard]] std::optional<std::vector<talas::project::VirtualControlDecoration>> vcsDecorationRelabelled(
    std::vector<talas::project::VirtualControlDecoration> decorations, std::size_t index, std::string text);

// Locking the surface again asks whether to
// save the changes made to it since it was unlocked. The surface as its
// editor changes it: each control's widget kind, range and look and place,
// the arrangement, the layouts made in the editor, the texts and boxes, and
// the widgets kept dormant; not the controls' values or their MIDI.
struct VcsWidgetState
{
    std::string eventName;
    talas::project::VirtualControlKind kind = talas::project::VirtualControlKind::Fader;
    double minimum = 0.0;
    double maximum = 1.0;
    talas::project::VirtualControlWidget widget;

    bool operator==(const VcsWidgetState&) const = default;
};

struct VcsSurface
{
    std::vector<VcsWidgetState> widgets;
    talas::project::VirtualControlLayout layout = talas::project::VirtualControlLayout::Automatic;
    std::vector<talas::project::VirtualControlLayoutPage> pages;
    std::vector<talas::project::VirtualControlDecoration> decorations;
    std::vector<talas::project::VirtualControl> dormant;
    // What the layouts keep of their own.
    std::vector<talas::project::VirtualControlLayoutSettings> settings;
};

[[nodiscard]] VcsSurface vcsSurface(const talas::project::ProjectDocument& document);
[[nodiscard]] bool vcsSurfaceChanged(const VcsSurface& unlocked, const talas::project::ProjectDocument& document);
// The changes discarded: the surface as it was unlocked, each control still
// in the project taking its widget back in its place, its value kept within
// the range it takes back.
[[nodiscard]] talas::project::ProjectDocument vcsSurfaceRestored(
    talas::project::ProjectDocument document, const VcsSurface& unlocked);
// Choosing another layout of the unlocked surface,
// changed since it was unlocked, first asks whether to save the changes to
// the layout shown; the locked surface, or an unchanged one, switches at
// once.
[[nodiscard]] bool vcsLayoutSwitchAsksToSave(
    const std::optional<VcsSurface>& unlocked, const talas::project::ProjectDocument& document);
}
