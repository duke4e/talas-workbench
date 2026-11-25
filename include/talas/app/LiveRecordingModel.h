#pragma once

#include "talas/app/PerformanceWorkbenchModel.h"

#include <string_view>

// Recording a Timeline to disk as it plays live.
namespace talas::app
{
// While the recording plays, the surface's faders do not take the mouse,
// while an external MIDI fader box (and OSC), and the presets the arrow
// keys choose, still change the parameters; with no recording every source
// does.
[[nodiscard]] bool liveRecordingTakesValueFrom(PerformanceActivitySource source, bool recordingToDisk) noexcept;

// What the Workbench says when the mouse moves a fader while it records.
[[nodiscard]] std::string_view liveRecordingMouseRefusal() noexcept;

// How Record to disk... records a Timeline: played live (a WaitUntil waiting
// for its key down, MIDI faders heard), or rendered offline from start to end,
// which takes no live input; the dialog's choices in order, live first.
enum class TimelineRecordingMode
{
    Live,
    Offline,
};

[[nodiscard]] std::string_view timelineRecordingModeLabel(TimelineRecordingMode mode) noexcept;
}
