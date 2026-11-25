#pragma once

#include "talas/app/SoundBrowserModel.h"
#include "talas/app/VcsLayout.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The surface of a Sound played in the Sound Browser. A Sound played there
// shows its Virtual Control Surface: a widget for each Event Value the browser
// plays it with, the one its Sound file keeps (a Sound file's Sounds share one
// surface) or a new one where the file keeps none, each at the value the
// browser plays it at. Played while the substitute input connection button is
// down, the surface takes no changes; with the button disengaged and the Sound
// played again, it lets you change it: its values by hand and by the dice, its
// frozen controls, and the places, types and ranges of its widgets, which Save
// keeps with the Sound in its Sound file on the disk.
namespace talas::app
{
struct SoundBrowserSurface
{
    std::string definitionId;
    // The Sound's name, the surface's title.
    std::string name;
    std::vector<talas::project::VirtualControl> widgets;
    talas::project::VirtualControlLayout arrangement = talas::project::VirtualControlLayout::Automatic;
    // Names clicked to freeze them against the dice; at first those
    // frozen in the Sound file's presets.
    std::set<std::string> frozen;
    // The substitute input connection button was down when the Sound was
    // played.
    bool playedThroughSubstituteInput = false;
    // Widgets placed, or given another type or range, since it was played:
    // an arrangement for Save to keep.
    bool rearranged = false;
};

struct SoundBrowserSurfaceResult
{
    std::optional<SoundBrowserSurface> surface;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return surface.has_value(); }
};

// The surface shown as the Sound `definitionId` of `soundFile` plays with
// the replacement input `input`, `audition` being the document the browser
// plays (soundBrowserAuditionDocument's).
[[nodiscard]] SoundBrowserSurfaceResult soundBrowserSurface(const talas::project::ProjectDocument& soundFile,
    std::string_view definitionId, const SoundBrowserReplacementInput& input,
    const talas::project::ProjectDocument& audition, const talas::core::NodeRegistry& registry);

// Whether the surface takes changes, which it does only when its
// Sound was played with the substitute input disengaged. Disengaging the
// button afterwards changes nothing until the Sound is played again.
[[nodiscard]] bool soundBrowserSurfaceTakesChanges(const SoundBrowserSurface& surface) noexcept;

// The line the surface shows above its widgets: why it takes no changes
// and what to do about it, or how to change it.
[[nodiscard]] std::string soundBrowserSurfaceNote(const SoundBrowserSurface& surface);

// Each change gives the surface changed, or nothing while the surface takes
// no changes, for a name it has no widget for, or for a change no widget
// can take.
//
// A fader moved, a value typed into its field and Enter pressed, or
// a button pressed: the value, kept within the widget's range.
[[nodiscard]] std::optional<SoundBrowserSurface> soundBrowserSurfaceValueSet(
    SoundBrowserSurface surface, std::string_view eventName, double value);
// The dice: each widget that is not frozen at a random value of its
// range, on its grid; a toggle on or off; a gate left alone.
[[nodiscard]] std::optional<SoundBrowserSurface> soundBrowserSurfaceDiceRolled(SoundBrowserSurface surface, std::uint64_t seed);
// A widget's name clicked: frozen, or active again.
[[nodiscard]] std::optional<SoundBrowserSurface> soundBrowserSurfaceFrozenToggled(
    SoundBrowserSurface surface, std::string_view eventName);
// Rearranging the widgets: each named at the place given, as the
// surface showed them when it was unlocked or as one was dragged; the
// surface then keeps every widget at its place.
[[nodiscard]] std::optional<SoundBrowserSurface> soundBrowserSurfacePlaced(
    SoundBrowserSurface surface, const std::vector<std::pair<std::string, VcsRect>>& places);
// A widget's type and range: a fader, gate or toggle from minimum
// to maximum, the minimum below the maximum; its value kept within.
[[nodiscard]] std::optional<SoundBrowserSurface> soundBrowserSurfaceWidgetSet(SoundBrowserSurface surface,
    std::string_view eventName, talas::project::VirtualControlKind kind, double minimum, double maximum);

// Saving the new arrangement along with the Sound on the disk: the
// Sound file keeping the surface's widgets, each in place of the one the
// file kept for its Event Value (that one's saved value, within the new
// range, and its MIDI and OSC mappings kept) and the others added, and the
// surface's arrangement. Nothing while the surface takes no changes.
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundFileWithBrowserSurface(
    talas::project::ProjectDocument soundFile, const SoundBrowserSurface& surface);
}
