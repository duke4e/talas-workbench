#pragma once

#include "talas/app/VcsEditorWindow.h"
#include "talas/project/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::app::workbench
{
// What the VCS Editor's controls do to the surface it edits.
struct VcsEditorActions
{
    // The Selected Value: the selected widget's name.
    std::function<juce::String()> selectedValue;
    // The selected widget's options: type, label, number, range, taper, place.
    std::function<void()> editSelected;
    // The editor's menus, shown at the button pressed.
    std::function<void(juce::Component&)> arrange;
    std::function<void(juce::Component&)> options;
    std::function<void(juce::Component&)> layout;
    // The padlock, which locks the surface and closes the editor.
    std::function<void()> lock;
    // Where the editor was put, to open there again.
    std::function<void(const talas::app::VcsRect&)> moved;
};

// The VCS Editor: a floating window, reminiscent of
// a calculator, that unlocking the surface opens and locking it closes; it
// can be dragged anywhere on the screen, kept on top of the Workbench.
class VcsEditorWindow final : public juce::DocumentWindow
{
public:
    explicit VcsEditorWindow(VcsEditorActions actions);
    ~VcsEditorWindow() override;

    // The Selected Value shown again, after the selection changed.
    void refresh();
    void closeButtonPressed() override;
    void moved() override;

private:
    class Content;
    VcsEditorActions actions_;
    Content* content_ = nullptr;
};

// The editor opened for `surface` beside it (or where it was put last),
// only while the surface is on the screen; none otherwise.
[[nodiscard]] std::unique_ptr<VcsEditorWindow> openVcsEditorWindow(
    juce::Component& surface, VcsEditorActions actions, const std::optional<talas::app::VcsRect>& last);

// The options of one widget in a dialog: Apply
// hands them back to `apply`, `status` reports a refused colour, and
// `closed` runs once the dialog is gone, whichever button closed it.
using VcsWidgetApply = std::function<void(
    const std::string& eventName, talas::project::VirtualControlWidget widget, double minimum, double maximum)>;
void showVcsWidgetDialog(juce::Component& owner, const talas::project::VirtualControl& control, VcsWidgetApply apply,
    std::function<void(const juce::String&)> status, std::function<void()> closed);
}
