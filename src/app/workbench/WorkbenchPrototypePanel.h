#pragma once

#include "talas/core/NodeRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
class PrototypePanel : public juce::Component
{
public:
    using AddCallback = std::function<void(
        const talas::core::NodeTypeDescriptor&)>;

    ~PrototypePanel() override = default;

    virtual void focusSearch() = 0;
    // Ctrl+Space plays the selected prototype.
    using PlayCallback = std::function<void(const talas::core::NodeTypeDescriptor&)>;
    virtual void setPlayer(PlayCallback onPlay) = 0;
    // A double-click opens an editor on a copy of the prototype; without an
    // opener it is added as Return adds it.
    using OpenCallback = std::function<void(const talas::core::NodeTypeDescriptor&)>;
    virtual void setOpener(OpenCallback onOpen) = 0;
    virtual bool doubleClickForSmoke(std::string_view typeId) = 0;
    // Ctrl+C copies the prototype selected.
    using CopyCallback = std::function<void(const talas::core::NodeTypeDescriptor&)>;
    virtual void setCopier(CopyCallback onCopy) = 0;
    virtual bool keyForSmoke(const juce::KeyPress& key) = 0;
    // The prototype selected, while the prototypes have the keyboard focus
    // (the Info menu takes it).
    [[nodiscard]] virtual std::optional<std::string> focusedPrototype() const = 0;
    virtual bool selectForSmoke(std::string_view typeId) = 0;
    // Zoom In and Out while the prototypes have the keyboard focus: their
    // entries larger or smaller; whether they had it.
    virtual bool zoomIfFocused(bool in) = 0;
    [[nodiscard]] virtual int rowHeightForSmoke() const = 0;
    [[nodiscard]] virtual bool searchFocusedForSmoke() const = 0;

    [[nodiscard]] virtual bool descriptorDiscoveryReadyForSmoke(
        std::string_view typeId,
        const juce::String& expectedDetail) = 0;
};

[[nodiscard]] std::unique_ptr<PrototypePanel> makePrototypePanel(
    const talas::core::NodeRegistry& registry,
    PrototypePanel::AddCallback onAdd);
}
