#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Virtual Control Surface Editor's widget options: the widget type, the
// numeric field, the label, the on and off colours, Min / Max / Grid and
// Taper, and the tabs a surface's widgets sit on.
namespace talas::app
{
// A fader position from 0 to 1 as the control's value: linear or
// logarithmic between its minimum and maximum (a log taper needs a
// minimum above 0, else it is linear), then on its grid counted from the
// minimum (a grid of 0 is continuous), within the range.
[[nodiscard]] double virtualControlValueAt(const talas::project::VirtualControl& control, double position);
[[nodiscard]] double virtualControlPositionOf(const talas::project::VirtualControl& control, double value);
// A value on the control's grid within its range.
[[nodiscard]] double quantizedVirtualControlValue(const talas::project::VirtualControl& control, double value);
// The order the surface shows its controls in: alphabetically by their names, case aside,
// as indices into the controls.
[[nodiscard]] std::vector<std::size_t> vcsAlphabeticalOrder(const std::vector<talas::project::VirtualControl>& controls);

// The texts of the Annotations anywhere in the signal flow: shown in a row of
// their own at the top of the virtual control surface, in the order the flow
// and the Sounds in it are reached, empty ones left out, each once.
[[nodiscard]] std::vector<std::string> vcsAnnotationTexts(const talas::project::ProjectDocument& document);

// Shift held while dragging a fader:
// fine-tuning, the value moving a tenth as far as the pointer, from where
// the drag began; `dragFraction` is the pointer's travel over the fader's
// length, up or right positive.
inline constexpr double vcsFineTuningRatio = 0.1;
[[nodiscard]] double vcsFineTunedValue(
    const talas::project::VirtualControl& control, double startValue, double dragFraction);
// The same for any fader from minimum to maximum, as the Spectral Analysis
// tool's Unpitched fader, within its range.
[[nodiscard]] double fineTunedFaderValue(double minimum, double maximum, double startValue, double dragFraction) noexcept;

// The numeric entry box, a small fader, with the Command or Control key held
// as the mouse is pressed:
// vertical movements as if of an invisible fader, `dragFraction` of its
// range up from where the drag began over `vcsInvisibleFaderPixels`, on the
// widget's grid and within its range.
inline constexpr int vcsInvisibleFaderPixels = 200;
[[nodiscard]] double vcsInvisibleFaderValue(
    const talas::project::VirtualControl& control, double startValue, double dragFraction);

// Tab and Shift+Tab on the surface: of its numbered widgets, the next or previous one's number to
// type, round from the last to the first; the first or the last when none
// is being typed.
[[nodiscard]] std::optional<std::size_t> vcsTabbedNumber(
    std::size_t count, std::optional<std::size_t> current, bool forward) noexcept;

// What a button sends as it is pressed or released:
// a gate its maximum while held and its minimum once released; a toggle
// switches to the other end on each press and stays there after release.
// Nothing for a fader, or for a toggle's release.
[[nodiscard]] std::optional<double> virtualControlButtonValue(const talas::project::VirtualControl& control, bool pressed);

// The label a widget shows: its own, else the Event Value's name.
[[nodiscard]] std::string virtualControlLabel(const talas::project::VirtualControl& control);
// A long name abbreviated to fit above its fader:
// its lower-case vowels dropped from the end, its first letter kept, then
// cut to at most `characters`.
[[nodiscard]] std::string vcsAbbreviatedName(std::string_view name, std::size_t characters);

// The surface's tabs in the order their first widgets come, the main tab
// (widgets of no tab) first when there is one.
[[nodiscard]] std::vector<std::string> virtualControlTabs(const std::vector<talas::project::VirtualControl>& controls);

// The widget's options as text fields write them, and back: style names
// (fader, horizontalFader, smallFader, potentiometer, buttonFill), taper
// (linear, log)
// and colours as #RRGGBB.
[[nodiscard]] std::string_view virtualControlWidgetStyleName(talas::project::VirtualControlWidgetStyle style);
[[nodiscard]] std::optional<talas::project::VirtualControlWidgetStyle> parseVirtualControlWidgetStyle(std::string_view name);
[[nodiscard]] std::string virtualControlColourText(std::optional<std::uint32_t> colour);
[[nodiscard]] std::optional<std::optional<std::uint32_t>> parseVirtualControlColour(std::string_view text);
}
