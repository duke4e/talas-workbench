#include "app/workbench/WorkbenchUiHelpers.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/app/ParameterFieldEditors.h"

#include <algorithm>
#include <cmath>

namespace talas::app::workbench
{
void flashTextEditor(juce::TextEditor& editor)
{
    const auto own = editor.findColour(juce::TextEditor::backgroundColourId);
    const auto flashes = talas::app::illegalFieldSignal().flashMilliseconds;
    juce::Component::SafePointer<juce::TextEditor> target(&editor);
    for (std::size_t index = 0; index < flashes.size(); ++index)
    {
        juce::Timer::callAfterDelay(std::max(1, flashes[index]), [target, own, flashed = index % 2 == 0] {
            if (target == nullptr)
                return;
            target->setColour(juce::TextEditor::backgroundColourId, flashed ? errorColour.withAlpha(0.55F) : own);
            target->repaint();
        });
    }
}

juce::String juceString(std::string_view value)
{
    return juce::String::fromUTF8(
        value.data(),
        static_cast<int>(value.size()));
}

juce::String workbenchSmpteTimeCodeText(
    const talas::runtime::SmpteTimeCode& timeCode)
{
    return juce::String::formatted(
        "%02u:%02u:%02u:%02u",
        static_cast<unsigned>(timeCode.hour),
        static_cast<unsigned>(timeCode.minute),
        static_cast<unsigned>(timeCode.second),
        static_cast<unsigned>(timeCode.frame));
}

juce::String workbenchSmpteRateText(talas::runtime::SmpteRate rate)
{
    switch (rate)
    {
    case talas::runtime::SmpteRate::Fps24: return "24 fps";
    case talas::runtime::SmpteRate::Fps25: return "25 fps";
    case talas::runtime::SmpteRate::Fps2997Drop: return "29.97 DF";
    case talas::runtime::SmpteRate::Fps30: return "30 fps";
    }
    return "unknown rate";
}

bool workbenchTransportSyncHasIssue(
    const juce::String& diagnostic)
{
    return diagnostic.containsIgnoreCase("unavailable")
        || diagnostic.containsIgnoreCase("timed out")
        || diagnostic.containsIgnoreCase("waiting")
        || diagnostic.containsIgnoreCase("stale")
        || diagnostic.containsIgnoreCase("rejected")
        || diagnostic.containsIgnoreCase("evidence unavailable");
}

int workbenchScaledUiPixels(int basePixels) noexcept
{
    if (basePixels <= 0)
        return 0;
    return std::max(
        basePixels,
        static_cast<int>(std::lround(
            static_cast<float>(basePixels)
            * WorkbenchType::interfaceScale())));
}

void describeControl(
    juce::Component& component,
    const juce::String& title,
    const juce::String& help)
{
    component.setTitle(title);
    if (help.isNotEmpty())
    {
        component.setDescription(help);
        component.setHelpText(help);
    }
}
}
