#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// Shift+Space plays the Timeline's marked Sounds only, compiling just them,
// and leaves it in that mode, so Space stops and starts them; Ctrl+Space
// compiles and plays every Sound again, or in the
// signal flow the selected Sound alone.
void WorkbenchComponent::playTimelineMarked(bool markedOnly)
{
    auto& marks = timelinePanel_->marks();
    if (markedOnly && marks.clipIds().empty())
    {
        setStatus("Mark Sounds with Option-click before playing the marked Sounds");
        return;
    }
    if (markedOnly)
        marks.playMarkedOnly();
    else
        marks.playAll();
    // Ctrl+Space rewinds as it plays every Sound again; Shift+Space goes
    // on from where the time cursor is.
    // In the signal flow, Ctrl+Space compiles, loads and starts the selected
    // Sound alone, as Oscilloscope stages it.
    auto alone = markedOnly || activeGraphContext_.has_value()
        ? std::nullopt
        : talas::app::documentForControlSpace(projectBridge_.editor().document(), registry_, selectedNodeIds_, signalFlowHasFocus());
    if (alone.has_value())
        infoDisplayStage_ = talas::app::InfoDisplayDocument{std::move(*alone), {}, {}, selectedNodeIds_.front()};
    if (!markedOnly)
        stopCurrentProject();
    else if (transportPlaying_)
        pauseCurrentProject();
    currentDocumentStaged_ = false;
    playCurrentProject();
    setStatus(markedOnly ? "Playing the marked Sounds only · Space stops and starts them · Ctrl+Space plays all"
            : alone.has_value() ? "Playing the selected Sound alone"
                                : "Playing every Sound of the Timeline");
}

bool WorkbenchComponent::signalFlowHasFocus() const
{
    return signalFlowFocusForSmoke_ || graphCanvas_->hasKeyboardFocus(true) || secondaryGraphCanvas_->hasKeyboardFocus(true);
}
}
