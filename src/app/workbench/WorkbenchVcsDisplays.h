#pragma once

#include "talas/app/VcsDisplays.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace talas::app::workbench
{
// The surface's oscilloscope displays: a tile for each
// OscilloscopeDisplay of the project, side by side, labelled with its
// Sound's name, its trace the Input's latest frames read from the staged
// project's analysis capture, and the buttons along its bottom zooming the
// trace in or out in time and amplitude.
class VcsDisplayStrip final : public juce::Component, private juce::Timer
{
public:
    explicit VcsDisplayStrip(talas::runtime::ProjectRuntimeBridge& bridge);
    ~VcsDisplayStrip() override;

    // The project's displays; the strip shows while there are any.
    void setDisplays(std::vector<talas::app::VcsDisplay> displays);
    [[nodiscard]] const std::vector<talas::app::VcsDisplay>& displays() const noexcept { return displays_; }
    [[nodiscard]] static int preferredHeight();

    void resized() override;
    void visibilityChanged() override;

private:
    class Tile;

    void timerCallback() override;

    talas::runtime::ProjectRuntimeBridge& bridge_;
    std::vector<talas::app::VcsDisplay> displays_;
    juce::OwnedArray<Tile> tiles_;
    std::vector<double> scratch_;
};
}
