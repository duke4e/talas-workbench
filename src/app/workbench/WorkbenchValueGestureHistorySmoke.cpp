#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runValueGestureHistorySmoke()
{
    auto& editor = projectBridge_.editor();
    const auto previousDocument = editor.document();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored = editor.replaceDocument(previousDocument);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "value gesture history smoke could not restore the prior project";
        if (error.empty())
            return std::nullopt;
        return error;
    };

    talas::project::ProjectDocument document;
    for (const auto* eventName : {"GestureLevel", "OtherLevel"})
    {
        talas::project::VirtualControl control;
        control.eventName = eventName;
        document.virtualControls.push_back(std::move(control));
    }
    if (!editor.replaceDocument(document).ok())
        return finish("value gesture history smoke project was rejected");
    documentChanged();
    if (!virtualControlSurface_->startValueGestureForSmoke("GestureLevel", 0.5))
        return finish("value gesture history smoke could not start a slider gesture");
    // A rebuild deletes the row mid-gesture, so its drag end never arrives.
    virtualControlSurface_->refresh();
    const auto otherValue = editor.setVirtualControlValue("OtherLevel", 0.25);
    talas::project::VirtualControl added;
    added.eventName = "AddedLevel";
    const auto structural = editor.addVirtualControl(added);
    if (!otherValue.ok() || !structural.ok())
        return finish("value gesture history smoke could not edit after the rebuild");

    const auto valueOf = [&](std::string_view eventName) {
        const auto& controls = editor.document().virtualControls;
        const auto found = std::ranges::find(
            controls, eventName, &talas::project::VirtualControl::eventName);
        return found == controls.end() ? -1.0 : found->value;
    };
    const auto structuralUndone = editor.undo().ok()
        && editor.document().virtualControls.size() == 2
        && valueOf("OtherLevel") == 0.25
        && valueOf("GestureLevel") == 0.5;
    const auto otherUndone = structuralUndone
        && editor.undo().ok()
        && valueOf("OtherLevel") == 0.0
        && valueOf("GestureLevel") == 0.5;
    if (!otherUndone)
        return finish("A value gesture left open by a rebuild recorded later edits");
    return finish();
}
}
