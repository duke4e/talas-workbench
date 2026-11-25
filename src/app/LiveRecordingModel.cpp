#include "talas/app/LiveRecordingModel.h"

namespace talas::app
{
// While recording to disk the onscreen faders do not
// take the mouse; a MIDI fader box and the arrow keys' presets still work.
bool liveRecordingTakesValueFrom(PerformanceActivitySource source, bool recordingToDisk) noexcept
{
    return !recordingToDisk || source != PerformanceActivitySource::UserInterface;
}

std::string_view liveRecordingMouseRefusal() noexcept
{
    return "While recording to disk the faders do not take the mouse: use a MIDI fader box, or the arrow keys "
           "to change presets";
}

std::string_view timelineRecordingModeLabel(TimelineRecordingMode mode) noexcept
{
    switch (mode)
    {
    case TimelineRecordingMode::Live: return "Play it live (keys and MIDI faders are heard)";
    case TimelineRecordingMode::Offline: return "Render it offline (no live input)";
    }
    return {};
}
}
