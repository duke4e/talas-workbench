#pragma once

#include "talas/app/WorkbenchSurfaceState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>

namespace talas::app::workbench
{
struct WorkbenchContextHelpContent
{
    juce::String stableIdentity;
    juce::String title;
    juce::String role;
    juce::String currentValue;
    juce::String primaryKeyboardAction;
    juce::String supportOrLimitation;
    juce::String exactNextAction;
    std::optional<talas::app::WorkbenchSurfaceState> surfaceState;

    [[nodiscard]] bool operator==(
        const WorkbenchContextHelpContent&) const = default;
};

class ContextHelpPanel : public juce::Component
{
public:
    ~ContextHelpPanel() override = default;

    virtual void setContent(WorkbenchContextHelpContent content) = 0;
    virtual void clearContent() = 0;
    [[nodiscard]] virtual bool contentContainsForSmoke(
        juce::StringRef stableIdentity,
        juce::StringRef requiredText) const = 0;
    [[nodiscard]] virtual std::string contentSummaryForSmoke() const = 0;
    [[nodiscard]] virtual std::uint64_t announcementCountForSmoke()
        const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ContextHelpPanel> makeContextHelpPanel();
}
