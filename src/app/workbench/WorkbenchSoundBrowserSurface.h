#pragma once

#include "talas/app/SoundBrowserSurface.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>

namespace talas::app::workbench
{
// What the surface of the Sound the Sound Browser plays asks of the
// Workbench: a widget's value sent to that Sound,
// the surface saved with the Sound in its Sound file, and a status line.
struct SoundBrowserSurfaceHost
{
    // False when the Sound no longer plays.
    std::function<bool(const talas::project::VirtualControl&)> sendValue;
    std::function<bool(const talas::app::SoundBrowserSurface&)> save;
    std::function<void(const juce::String&)> status;
};

// The surface in a window of its own: its widgets, the dice, the padlock
// that unlocks it to drag its widgets about, and Save; all of them still
// while it takes no changes.
class SoundBrowserSurfacePanel : public juce::Component
{
public:
    ~SoundBrowserSurfacePanel() override = default;

    // The surface of the Sound just played.
    virtual void show(talas::app::SoundBrowserSurface surface) = 0;
    [[nodiscard]] virtual const std::optional<talas::app::SoundBrowserSurface>& surface() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<SoundBrowserSurfacePanel> makeSoundBrowserSurfacePanel(SoundBrowserSurfaceHost host);
}
