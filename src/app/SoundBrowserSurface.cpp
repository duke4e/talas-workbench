#include "talas/app/SoundBrowserSurface.h"

#include "talas/app/VcsPresets.h"
#include "talas/project/ProjectEventValueDiscovery.h"

#include <algorithm>
#include <cmath>
#include <random>

// A Sound played from the Sound Browser shows its
// surface, which takes changes only when the Sound was played with the
// substitute input disengaged.
namespace talas::app
{
namespace
{
[[nodiscard]] SoundBrowserSurfaceResult failure(std::string message)
{
    SoundBrowserSurfaceResult result;
    result.diagnostic = std::move(message);
    return result;
}

[[nodiscard]] talas::project::VirtualControl* widgetNamed(SoundBrowserSurface& surface, std::string_view eventName)
{
    const auto found = std::ranges::find(surface.widgets, eventName, &talas::project::VirtualControl::eventName);
    return found == surface.widgets.end() ? nullptr : &*found;
}

[[nodiscard]] double onGrid(const talas::project::VirtualControl& widget, double value)
{
    if (widget.widget.grid <= 0.0)
        return std::clamp(value, widget.minimum, widget.maximum);
    const auto steps = std::round((value - widget.minimum) / widget.widget.grid);
    return std::clamp(widget.minimum + steps * widget.widget.grid, widget.minimum, widget.maximum);
}
}

SoundBrowserSurfaceResult soundBrowserSurface(const talas::project::ProjectDocument& soundFile, std::string_view definitionId,
    const SoundBrowserReplacementInput& input, const talas::project::ProjectDocument& audition,
    const talas::core::NodeRegistry& registry)
{
    const auto definition = std::ranges::find(
        soundFile.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (definition == soundFile.soundDefinitions.end())
        return failure("The Sound is no longer in its Sound file");
    // The Event Values the browser plays the Sound with, each at the value
    // it starts at there (the audition keeps no controls of its own).
    const auto discovered = talas::project::discoverProjectEventValueControls(audition, registry);
    if (!discovered.ok())
        return failure(discovered.diagnostics.front().message);
    SoundBrowserSurface surface;
    surface.definitionId = definition->definitionId;
    surface.name = definition->displayName.empty() ? definition->definitionId : definition->displayName;
    surface.playedThroughSubstituteInput = input.connected;
    surface.arrangement = soundFile.virtualControlLayout;
    const auto* presets = findVcsPresetBank(soundFile);
    for (const auto& played : discovered.missingControls)
    {
        auto widget = played;
        const auto kept = std::ranges::find(soundFile.virtualControls, played.eventName, &talas::project::VirtualControl::eventName);
        if (kept != soundFile.virtualControls.end() && kept->domain == played.domain)
        {
            // The file's widget, its type, range, look and place, showing the
            // value the browser plays (at the range's end, should it lie
            // beyond, until the widget is moved).
            widget = *kept;
            widget.value = played.value;
            if (presets != nullptr && vcsControlFrozen(*presets, *kept))
                surface.frozen.insert(widget.eventName);
        }
        surface.widgets.push_back(std::move(widget));
    }
    SoundBrowserSurfaceResult result;
    result.surface = std::move(surface);
    return result;
}

bool soundBrowserSurfaceTakesChanges(const SoundBrowserSurface& surface) noexcept
{
    return !surface.playedThroughSubstituteInput;
}

std::string soundBrowserSurfaceNote(const SoundBrowserSurface& surface)
{
    if (!soundBrowserSurfaceTakesChanges(surface))
        return "Played through the substitute input, this surface takes no changes: "
               "disengage the connection button and play " + surface.name + " again to change it";
    if (surface.widgets.empty())
        return surface.name + " has no controls";
    return "Move the faders, roll the dice (r), click a name to freeze it; unlock to rearrange the widgets, then save "
           "them with the Sound";
}

std::optional<SoundBrowserSurface> soundBrowserSurfaceValueSet(
    SoundBrowserSurface surface, std::string_view eventName, double value)
{
    auto* widget = soundBrowserSurfaceTakesChanges(surface) && std::isfinite(value) ? widgetNamed(surface, eventName) : nullptr;
    if (widget == nullptr)
        return std::nullopt;
    widget->value = std::clamp(value, widget->minimum, widget->maximum);
    return surface;
}

std::optional<SoundBrowserSurface> soundBrowserSurfaceDiceRolled(SoundBrowserSurface surface, std::uint64_t seed)
{
    if (!soundBrowserSurfaceTakesChanges(surface))
        return std::nullopt;
    std::mt19937_64 random{seed};
    for (auto& widget : surface.widgets)
    {
        if (surface.frozen.contains(widget.eventName) || widget.kind == talas::project::VirtualControlKind::Gate)
            continue;
        if (widget.kind == talas::project::VirtualControlKind::Toggle)
        {
            widget.value = std::bernoulli_distribution{0.5}(random) ? widget.maximum : widget.minimum;
            continue;
        }
        widget.value = onGrid(widget, std::uniform_real_distribution<double>{widget.minimum, widget.maximum}(random));
    }
    return surface;
}

std::optional<SoundBrowserSurface> soundBrowserSurfaceFrozenToggled(SoundBrowserSurface surface, std::string_view eventName)
{
    if (!soundBrowserSurfaceTakesChanges(surface) || widgetNamed(surface, eventName) == nullptr)
        return std::nullopt;
    const std::string name{eventName};
    if (!surface.frozen.erase(name))
        surface.frozen.insert(name);
    return surface;
}

std::optional<SoundBrowserSurface> soundBrowserSurfacePlaced(
    SoundBrowserSurface surface, const std::vector<std::pair<std::string, VcsRect>>& places)
{
    if (!soundBrowserSurfaceTakesChanges(surface) || places.empty())
        return std::nullopt;
    for (const auto& [name, place] : places)
    {
        auto* widget = widgetNamed(surface, name);
        auto moved = widget == nullptr ? talas::project::VirtualControlWidget{} : widget->widget;
        moved.bounds = place;
        if (widget == nullptr || !talas::project::isValidVirtualControlWidget(moved, widget->minimum, widget->maximum))
            return std::nullopt;
        widget->widget = std::move(moved);
    }
    surface.arrangement = talas::project::VirtualControlLayout::Fixed;
    surface.rearranged = true;
    return surface;
}

std::optional<SoundBrowserSurface> soundBrowserSurfaceWidgetSet(SoundBrowserSurface surface, std::string_view eventName,
    talas::project::VirtualControlKind kind, double minimum, double maximum)
{
    auto* widget = soundBrowserSurfaceTakesChanges(surface) ? widgetNamed(surface, eventName) : nullptr;
    if (widget == nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) || !(minimum < maximum))
        return std::nullopt;
    // A grid wider than the new range, or a log taper reaching 0, would not
    // suit it; they are kept within it.
    auto look = widget->widget;
    look.grid = std::min(look.grid, maximum - minimum);
    if (minimum <= 0.0)
        look.taper = talas::project::VirtualControlTaper::Linear;
    if (!talas::project::isValidVirtualControlWidget(look, minimum, maximum))
        return std::nullopt;
    widget->kind = kind;
    widget->minimum = minimum;
    widget->maximum = maximum;
    widget->widget = std::move(look);
    widget->value = std::clamp(widget->value, minimum, maximum);
    surface.rearranged = true;
    return surface;
}

std::optional<talas::project::ProjectDocument> soundFileWithBrowserSurface(
    talas::project::ProjectDocument soundFile, const SoundBrowserSurface& surface)
{
    if (!soundBrowserSurfaceTakesChanges(surface))
        return std::nullopt;
    for (const auto& widget : surface.widgets)
    {
        auto kept = std::ranges::find(soundFile.virtualControls, widget.eventName, &talas::project::VirtualControl::eventName);
        if (kept == soundFile.virtualControls.end())
        {
            soundFile.virtualControls.push_back(widget);
            continue;
        }
        if (kept->domain != widget.domain)
        {
            *kept = widget;
            continue;
        }
        kept->kind = widget.kind;
        kept->minimum = widget.minimum;
        kept->maximum = widget.maximum;
        kept->value = std::clamp(kept->value, widget.minimum, widget.maximum);
        kept->widget = widget.widget;
    }
    if (surface.arrangement != talas::project::VirtualControlLayout::Automatic)
        soundFile.virtualControlLayout = surface.arrangement;
    talas::project::reconcileProjectPresentation(soundFile);
    return soundFile;
}
}
