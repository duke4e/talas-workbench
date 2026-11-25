#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/ParameterFieldEditors.h"
#include "talas/app/PasteHotModel.h"
#include "talas/app/TextSearch.h"

// The Edit menu's operations on the focused text field and on the selected
// Sound: Find and
// Replace, Paste hot, Paste special and Revert; and the Action menu's Set
// replaceable input.
namespace talas::app::workbench
{
void WorkbenchComponent::setReplaceableInputOf(const std::optional<std::string>& definitionId, std::string_view nodeId)
{
    if (!definitionId.has_value())
    {
        setStatus("Set replaceable input: open a Sound's editor and select one of its Sounds");
        return;
    }
    const auto result = projectBridge_.editor().setReplaceableInput(*definitionId, nodeId);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    setStatus(juceString(nodeId) + " is now the replaceable input");
}

// As long as a field holds an illegal value, the
// parameters of another Sound cannot be edited; fix it, or Revert, first.
void WorkbenchComponent::selectNodeFromCanvas(
    std::optional<std::string> definitionId, std::string nodeId, bool extendSelection)
{
    const auto sameSound = definitionId == activeGraphContext_ && nodeId == selectedNodeId_;
    // The value typed into the field is tested as a different Sound is
    // clicked, and taken when legal.
    if (!nodeId.empty()
        && talas::app::typedFieldValueTested(sameSound ? talas::app::FieldTestMoment::SameSoundClicked
                                                    : talas::app::FieldTestMoment::DifferentSoundClicked))
        (void) inspectorPanel_->testTypedValue();
    if (!talas::app::parametersMayMoveTo(inspectorPanel_->holdsIllegalValue(), sameSound))
    {
        juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
        setStatus("Fix the illegal value, or Revert, before editing another Sound");
        if (definitionId == primaryGraphContext_)
            graphCanvas_->setSelectedNodeIds(selectedNodeIds_, selectedNodeId_);
        if (definitionId == secondaryGraphContext_)
            secondaryGraphCanvas_->setSelectedNodeIds(selectedNodeIds_, selectedNodeId_);
        return;
    }
    selectNode(std::move(definitionId), std::move(nodeId), extendSelection);
}

void WorkbenchComponent::pasteSpecialGraphNode()
{
    if (inspectorPanel_ != nullptr && inspectorPanel_->pasteSpecialIntoFocusedInput())
        return;
    const auto parsed = talas::project::parseProjectGraphFragment(graphClipboardText().toStdString());
    const auto* graph = graphForContext(activeGraphContext_);
    if (!parsed.fragment.has_value() || parsed.fragment->nodes.size() != 1 || graph == nullptr
        || selectedNodeIds_.size() != 1)
    {
        setStatus("Paste special: copy one Sound, then select the Sound it should replace");
        return;
    }
    const auto shared = parsed.fragment->nodes.front().instanceId;
    if (std::ranges::none_of(graph->nodes, [&](const auto& node) { return node.instanceId == shared; }))
    {
        setStatus("Paste special shares a Sound of this structure; " + juceString(shared) + " is not in it");
        return;
    }
    const auto replaced = selectedNodeIds_.front();
    const auto result = projectBridge_.editor().shareNodeInPlaceOf(optionalStringView(activeGraphContext_), replaced, shared);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    selectNode(activeGraphContext_, shared);
    setStatus(juceString(shared) + " now also takes " + juceString(replaced) + "'s place");
}

// Paste special into an input field: the Sound copied, of
// this structure, shares the field.
bool WorkbenchComponent::pasteSpecialIntoInput(const std::string& nodeId, const std::string& portId)
{
    const auto parsed = talas::project::parseProjectGraphFragment(graphClipboardText().toStdString());
    const auto* graph = graphForContext(activeGraphContext_);
    if (!parsed.fragment.has_value() || parsed.fragment->nodes.size() != 1 || graph == nullptr)
        return false;
    const auto shared = parsed.fragment->nodes.front().instanceId;
    if (std::ranges::none_of(graph->nodes, [&](const auto& node) { return node.instanceId == shared; }))
    {
        setStatus("Paste special shares a Sound of this structure; " + juceString(shared) + " is not in it");
        return false;
    }
    const auto result = projectBridge_.editor().shareNodeInInput(optionalStringView(activeGraphContext_), nodeId, portId, shared);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return false;
    }
    documentChanged();
    setStatus(juceString(shared) + " now also feeds " + juceString(nodeId) + "'s " + juceString(portId));
    return true;
}

bool WorkbenchComponent::pasteHotInto(juce::TextEditor& editor, const juce::String& typed, int listIndex)
{
    const auto entries = talas::app::pasteHotEntries(projectBridge_.editor().document());
    const auto chosen = typed.isNotEmpty() ? talas::app::pasteHotMatch(entries, typed.toStdString())
        : listIndex >= 0 && static_cast<std::size_t>(listIndex) < entries.size()
        ? std::optional<std::size_t>{static_cast<std::size_t>(listIndex)}
        : std::nullopt;
    if (!chosen.has_value() || editor.isReadOnly())
    {
        juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
        setStatus("Paste hot: nothing in the list starts with \"" + typed + "\"");
        return false;
    }
    editor.insertTextAtCaret(juceString(entries[*chosen].text));
    setStatus("Pasted " + juceString(entries[*chosen].text));
    return true;
}

void WorkbenchComponent::pasteHotIntoFocusedField()
{
    juce::Component::SafePointer<juce::TextEditor> target{
        dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())};
    if (target == nullptr || target->isReadOnly())
    {
        setStatus("Click in a parameter field to paste into it");
        return;
    }
    juce::StringArray labels;
    for (const auto& entry : talas::app::pasteHotEntries(projectBridge_.editor().document()))
        labels.add(juceString(entry.label == entry.text ? entry.label : entry.label + "  —  " + entry.text));
    auto* window = new juce::AlertWindow("Paste hot", "Choose, or type part of a name and press Enter",
        juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("typed", {}, "Name");
    window->addComboBox("list", labels, "Hot values");
    window->addButton("Paste", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window, target](int result) {
            const auto typed = window->getTextEditorContents("typed").trim();
            const auto* list = window->getComboBoxComponent("list");
            const auto index = list == nullptr ? -1 : list->getSelectedItemIndex();
            if (safeThis == nullptr || target == nullptr || result != 1)
                return;
            (void) safeThis->pasteHotInto(*target, typed, index);
        }),
        true);
}

void WorkbenchComponent::revertSelectedSound()
{
    if (!revertSnapshot_.has_value() || revertSnapshot_->nodeId != selectedNodeId_
        || revertSnapshot_->definitionId != activeGraphContext_)
    {
        setStatus("Select a Sound to revert its parameters");
        return;
    }
    const auto result = projectBridge_.editor().revertNodeParameters(
        optionalStringView(revertSnapshot_->definitionId), revertSnapshot_->nodeId, revertSnapshot_->parameters);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    setStatus("Reverted " + juceString(revertSnapshot_->nodeId) + " to its parameters when it was opened");
}

bool WorkbenchComponent::findInEditor(juce::TextEditor& editor, const juce::String& needle)
{
    const auto text = editor.getText();
    const auto utf8 = text.toStdString();
    const auto from = static_cast<std::size_t>(text.substring(0, editor.getHighlightedRegion().getEnd()).getNumBytesAsUTF8());
    const auto found = talas::app::findNextInText(utf8, needle.toStdString(), from);
    if (!found.has_value())
    {
        juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
        setStatus("\"" + needle + "\" not found after the selection");
        return false;
    }
    const auto characters = [&](std::size_t bytes) {
        return juce::String::fromUTF8(utf8.data(), static_cast<int>(bytes)).length();
    };
    editor.setHighlightedRegion({characters(found->start), characters(found->end)});
    setStatus("Found \"" + needle + "\"");
    return true;
}

bool WorkbenchComponent::replaceInEditor(
    juce::TextEditor& editor, const juce::String& needle, const juce::String& replacement, bool all)
{
    const auto text = editor.getText();
    const auto utf8 = text.toStdString();
    const auto from = static_cast<std::size_t>(text.substring(0, editor.getHighlightedRegion().getStart()).getNumBytesAsUTF8());
    const auto replaced = talas::app::replaceInText(utf8, needle.toStdString(), replacement.toStdString(), from, all);
    if (replaced.count == 0)
    {
        juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
        setStatus("\"" + needle + "\" not found to replace");
        return false;
    }
    editor.setText(juceString(replaced.text), true);
    if (replaced.replaced.has_value())
    {
        const auto characters = [&](std::size_t bytes) {
            return juce::String::fromUTF8(replaced.text.data(), static_cast<int>(bytes)).length();
        };
        editor.setHighlightedRegion({characters(replaced.replaced->start), characters(replaced.replaced->end)});
    }
    setStatus("Replaced " + juce::String(static_cast<int>(replaced.count)) + " of \"" + needle + "\"");
    return true;
}

void WorkbenchComponent::findInFocusedField(bool again)
{
    juce::Component::SafePointer<juce::TextEditor> target{
        dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())};
    if (target == nullptr)
    {
        setStatus("Click in a text field to search it");
        return;
    }
    if (again && lastFindText_.isNotEmpty())
    {
        (void) findInEditor(*target, lastFindText_);
        return;
    }
    auto* window = new juce::AlertWindow("Find", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("find", lastFindText_, "Find");
    window->addButton("Find", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window, target](int result) {
            const auto needle = window->getTextEditorContents("find");
            if (safeThis == nullptr || target == nullptr || result != 1 || needle.isEmpty())
                return;
            safeThis->lastFindText_ = needle;
            (void) safeThis->findInEditor(*target, needle);
        }),
        true);
}

void WorkbenchComponent::replaceInFocusedField(bool again)
{
    juce::Component::SafePointer<juce::TextEditor> target{
        dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())};
    if (target == nullptr || target->isReadOnly())
    {
        setStatus("Click in an editable text field to replace in it");
        return;
    }
    if (again && lastFindText_.isNotEmpty())
    {
        (void) replaceInEditor(*target, lastFindText_, lastReplaceText_, false);
        return;
    }
    auto* window = new juce::AlertWindow("Replace", {}, juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor("find", lastFindText_, "Replace");
    window->addTextEditor("replace", lastReplaceText_, "With");
    window->addButton("Replace", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Replace all", 2);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window, target](int result) {
            const auto needle = window->getTextEditorContents("find");
            const auto replacement = window->getTextEditorContents("replace");
            if (safeThis == nullptr || target == nullptr || result == 0 || needle.isEmpty())
                return;
            safeThis->lastFindText_ = needle;
            safeThis->lastReplaceText_ = replacement;
            (void) safeThis->replaceInEditor(*target, needle, replacement, result == 2);
        }),
        true);
}
}
