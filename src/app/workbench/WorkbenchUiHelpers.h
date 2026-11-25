#pragma once

#include "talas/runtime/MtcSmpteProtocol.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string_view>

namespace talas::app::workbench
{
inline constexpr std::string_view prototypeDragPrefix = "talas.prototype:";
// A project Sound definition dragged from the Sound browser.
inline constexpr std::string_view soundDragPrefix = "talas.sound:";

[[nodiscard]] juce::String juceString(std::string_view value);
[[nodiscard]] juce::String workbenchSmpteTimeCodeText(
    const talas::runtime::SmpteTimeCode& timeCode);
[[nodiscard]] juce::String workbenchSmpteRateText(
    talas::runtime::SmpteRate rate);
[[nodiscard]] bool workbenchTransportSyncHasIssue(
    const juce::String& diagnostic);
[[nodiscard]] int workbenchScaledUiPixels(int basePixels) noexcept;

// An error in a text field: the field
// flashes, back in its own colour at the end.
void flashTextEditor(juce::TextEditor& editor);

void describeControl(
    juce::Component& component,
    const juce::String& title,
    const juce::String& help = {});
}
