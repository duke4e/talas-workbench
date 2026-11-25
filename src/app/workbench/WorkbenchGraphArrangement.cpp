#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/GraphBranchLayout.h"

namespace talas::app::workbench
{
// Select a Sound, press Enter, type a new name.
void WorkbenchComponent::promptRenameGraphNode(
    std::optional<std::string> definitionId,
    std::string nodeId)
{
    if (nodeForContext(definitionId, nodeId) == nullptr)
    {
        setStatus("Select a Sound to rename");
        return;
    }
    if (destructivePromptActive_ || fileChooserActive_ || soundDialog_ != nullptr)
    {
        setStatus("Finish the active dialog before renaming a Sound");
        return;
    }
    soundDialog_ = std::make_unique<juce::AlertWindow>(
        "Rename Sound",
        "A new name for " + juceString(nodeId),
        juce::MessageBoxIconType::NoIcon,
        this);
    soundDialog_->addTextEditor("nodeName", juceString(nodeId), "Name");
    soundDialog_->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    soundDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    soundDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis, definitionId = std::move(definitionId), nodeId = std::move(nodeId)](int result) {
                if (safeThis == nullptr || safeThis->soundDialog_ == nullptr)
                    return;
                auto name = safeThis->soundDialog_->getTextEditor("nodeName")->getText().toStdString();
                safeThis->soundDialog_.reset();
                safeThis->scheduleWorkbenchFocusRestoration();
                if (result == 1)
                    (void)safeThis->renameGraphNode(definitionId, nodeId, std::move(name));
            }),
        false);
}

bool WorkbenchComponent::renameGraphNode(
    const std::optional<std::string>& definitionId,
    const std::string& nodeId,
    std::string newNodeId)
{
    const auto scope = definitionId.has_value()
        ? std::optional<std::string_view>{*definitionId}
        : std::nullopt;
    const auto renamed = projectBridge_.editor().renameNode(scope, nodeId, newNodeId);
    if (!renamed.ok())
    {
        setStatus(juceString(renamed.message));
        return false;
    }
    documentChanged();
    selectNode(definitionId, newNodeId);
    setStatus("Renamed " + juceString(nodeId) + " to " + juceString(newNodeId));
    return true;
}

// Edit > Clean up: the virtual control
// surface when it is in front, locked or not, in the columns asked for;
// else the signal flow.
void WorkbenchComponent::cleanUpFrontWindow()
{
    if (!vcsFocusForSmoke_ && !virtualControlSurface_->hasKeyboardFocus(true))
    {
        (void) cleanUpGraphLayout(activeGraphContext_);
        return;
    }
    if (smokeCleanUpColumns_.has_value())
        (void) virtualControlSurface_->cleanUpForSmoke(*smokeCleanUpColumns_, false);
    else
        virtualControlSurface_->showCleanUpDialog();
}

// Edit > Clean up lays the signal flow out in
// columns from right to left.
bool WorkbenchComponent::cleanUpGraphLayout(
    const std::optional<std::string>& definitionId)
{
    const auto* graph = graphForContext(definitionId);
    if (graph == nullptr || graph->nodes.empty())
    {
        setStatus("No Sounds to clean up");
        return false;
    }
    const auto& document = projectBridge_.editor().document();
    std::vector<talas::project::ProjectNodePresentation> current;
    for (const auto& node : graph->nodes)
    {
        if (const auto* presented = talas::project::findProjectNodePresentation(
                document,
                definitionId.has_value() ? std::optional<std::string_view>{*definitionId} : std::nullopt,
                node.instanceId))
            current.push_back(*presented);
    }
    const auto layout = talas::app::cleanedUpGraphLayout(*graph, current);
    const auto arranged = projectBridge_.editor().arrangeNodes(
        definitionId.has_value() ? std::optional<std::string_view>{*definitionId} : std::nullopt,
        layout);
    if (!arranged.ok())
    {
        setStatus(juceString(arranged.message));
        return false;
    }
    documentChanged();
    graphCanvas_->refreshModel();
    setStatus("Cleaned up the signal flow");
    return true;
}
}
