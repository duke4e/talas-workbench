#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// The active field: the text field with the keyboard focus, drawn with a border.
// The Edit menu's operations apply to it: Cut, Copy, Paste and Select All
// edit its text rather than the signal flow's Sounds, in a tool window's
// fields too, as the class editor's description and comments.
juce::TextEditor* WorkbenchComponent::activeTextField() const
{
    if (smokeActiveField_ != nullptr)
        return smokeActiveField_;
    auto* field = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
    if (field == nullptr)
        return nullptr;
    return talas::app::editMenuTarget(isParentOf(field), isInToolWindow(*field)) == talas::app::EditMenuTarget::TextField
        ? field
        : nullptr;
}

bool WorkbenchComponent::activeFieldCommandInfo(juce::ApplicationCommandInfo& info) const
{
    const auto* field = activeTextField();
    if (field == nullptr)
        return false;
    const auto selected = !field->getHighlightedRegion().isEmpty();
    switch (info.commandID)
    {
    case commandCutSelectedNodes:
        info.setActive(selected && !field->isReadOnly());
        return true;
    case commandCopySelectedNodes:
        info.setActive(selected);
        return true;
    case commandPasteGraphNodes:
        info.setActive(!field->isReadOnly());
        return true;
    case commandSelectAllGraphNodes:
        info.setActive(true);
        return true;
    default:
        return false;
    }
}

bool WorkbenchComponent::editActiveField(juce::CommandID command)
{
    auto* field = activeTextField();
    if (field == nullptr)
        return false;
    switch (command)
    {
    case commandCutSelectedNodes:
        field->cut();
        return true;
    case commandCopySelectedNodes:
        field->copy();
        return true;
    case commandPasteGraphNodes:
        field->paste();
        return true;
    case commandSelectAllGraphNodes:
        field->selectAll();
        return true;
    default:
        return false;
    }
}

juce::KeyPress WorkbenchComponent::transportKeyPress(talas::app::TalasTransportCommand command)
{
    const auto& key = talas::app::talasTransportKey(command);
    auto modifiers = 0;
    if (key.control)
        modifiers |= juce::ModifierKeys::ctrlModifier;
    if (key.commandKey)
        modifiers |= juce::ModifierKeys::commandModifier;
    if (key.shift)
        modifiers |= juce::ModifierKeys::shiftModifier;
    const auto code = key.key == talas::app::TalasTransportKeyName::Space ? static_cast<int>(' ')
        : key.key == talas::app::TalasTransportKeyName::RightArrow        ? juce::KeyPress::rightKey
                                                                       : static_cast<int>('k');
    return juce::KeyPress{code, juce::ModifierKeys{modifiers}, 0};
}
}
