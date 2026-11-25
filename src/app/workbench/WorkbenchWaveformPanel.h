#pragma once

#include "talas/app/SampleMapInteraction.h"
#include "talas/audio/WaveformOverview.h"
#include "talas/project/ProjectSampleMaps.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::app::workbench
{
enum class SampleMapAuditionPresentation
{
    Idle,
    Preparing,
    Active,
    Stopping,
    Failed,
};

struct WaveformPanelState
{
    std::shared_ptr<const talas::audio::WaveformOverview> overview;
    std::optional<talas::project::ProjectSampleRegion> region;
    talas::app::WaveformViewport viewport;
    std::uint64_t cursorFrame = 0;
    bool loading = false;
    std::string errorText;
    SampleMapAuditionPresentation audition =
        SampleMapAuditionPresentation::Idle;
};

struct WaveformPanelCallbacks
{
    std::function<void(double)> zoom;
    std::function<void(std::uint64_t)> setCursor;
    std::function<void(bool, talas::app::WaveformLoopMarker, std::int64_t)>
        moveLoopMarker;
    std::function<void()> audition;
    std::function<void()> stopAudition;
};

class WaveformPanel : public juce::Component
{
public:
    ~WaveformPanel() override = default;

    virtual void setState(const WaveformPanelState& state) = 0;
    [[nodiscard]] virtual bool hasReadySurfaceForSmoke() const = 0;
    [[nodiscard]] virtual bool hasHonestLoadingStateForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<WaveformPanel> makeWaveformPanel(
    WaveformPanelCallbacks callbacks);
}
