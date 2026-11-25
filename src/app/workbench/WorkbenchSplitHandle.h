#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace talas::app::workbench
{
class WorkspaceSplitHandle : public juce::Component
{
public:
    using ChangeCallback = std::function<void(juce::Point<int>)>;
    using ResetCallback = std::function<void()>;

    ~WorkspaceSplitHandle() override = default;

    virtual void setVerticalDivider(bool vertical) = 0;
};

[[nodiscard]] std::unique_ptr<WorkspaceSplitHandle>
makeWorkspaceSplitHandle(
    WorkspaceSplitHandle::ChangeCallback onChange,
    WorkspaceSplitHandle::ResetCallback onReset);
}
