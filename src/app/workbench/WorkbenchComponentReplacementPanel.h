#pragma once

#include "talas/app/WorkbenchSurfaceState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace talas::app::workbench
{
enum class ComponentReplacementPresentationState
{
    Hidden,
    Inspecting,
    AwaitingOwnerApproval,
    AwaitingConfirmation,
    Probing,
    Activating,
    Staging,
    Succeeded,
    Failed,
    Cancelled,
};

struct ComponentReplacementPresentation
{
    ComponentReplacementPresentationState state =
        ComponentReplacementPresentationState::Hidden;
    juce::String headline;
    juce::String status;
    juce::String details;
    juce::String confirmLabel;
    bool canChoose = false;
    bool canConfirm = false;
    bool canCancel = false;

    [[nodiscard]] bool visible() const noexcept
    {
        return state != ComponentReplacementPresentationState::Hidden;
    }
};

struct ComponentReplacementPanelCallbacks
{
    std::function<void()> choose;
    std::function<void()> confirm;
    std::function<void()> cancel;
};

class ComponentReplacementPanel : public juce::Component,
                                  public talas::app::WorkbenchSurfaceStateSource
{
public:
    ~ComponentReplacementPanel() override = default;

    virtual void setPresentation(
        const ComponentReplacementPresentation& presentation) = 0;
    [[nodiscard]] virtual bool showsAccessiblePreviewForSmoke(
        const juce::String& expectedText,
        bool confirmationExpected) const = 0;
};

[[nodiscard]] std::unique_ptr<ComponentReplacementPanel>
makeComponentReplacementPanel(
    ComponentReplacementPanelCallbacks callbacks);
}
