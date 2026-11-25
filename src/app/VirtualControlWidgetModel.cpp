#include "talas/app/VirtualControlWidgetModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace talas::app
{
namespace
{
[[nodiscard]] bool logTaper(const talas::project::VirtualControl& control)
{
    return control.widget.taper == talas::project::VirtualControlTaper::Log && control.minimum > 0.0
        && control.maximum > control.minimum;
}
}

std::vector<std::size_t> vcsAlphabeticalOrder(const std::vector<talas::project::VirtualControl>& controls)
{
    const auto folded = [](std::string_view name) {
        std::string text{name};
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::vector<std::size_t> order(controls.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::ranges::stable_sort(order, [&](std::size_t left, std::size_t right) {
        return folded(controls[left].eventName) < folded(controls[right].eventName);
    });
    return order;
}

std::vector<std::string> vcsAnnotationTexts(const talas::project::ProjectDocument& document)
{
    std::vector<std::string> texts;
    std::vector<std::string> visited;
    const auto walk = [&](const auto& self, const talas::project::ProjectGraph& graph) -> void {
        for (const auto& node : graph.nodes)
        {
            if (node.typeId == "talas.annotation")
            {
                const auto text = std::ranges::find(node.parameters, std::string_view{"text"},
                    &talas::project::ProjectParameterExpression::parameterId);
                if (text != node.parameters.end() && text->text.has_value() && !text->text->empty()
                    && std::ranges::find(texts, *text->text) == texts.end())
                    texts.push_back(*text->text);
            }
            if (!node.soundDefinitionId.has_value() || std::ranges::find(visited, *node.soundDefinitionId) != visited.end())
                continue;
            visited.push_back(*node.soundDefinitionId);
            const auto definition = std::ranges::find(document.soundDefinitions, *node.soundDefinitionId,
                &talas::project::ProjectSoundDefinition::definitionId);
            if (definition != document.soundDefinitions.end())
                self(self, definition->graph);
        }
    };
    walk(walk, document.graph);
    return texts;
}

double fineTunedFaderValue(double minimum, double maximum, double startValue, double dragFraction) noexcept
{
    return std::clamp(startValue + dragFraction * (maximum - minimum) * vcsFineTuningRatio, std::min(minimum, maximum),
        std::max(minimum, maximum));
}

double vcsFineTunedValue(const talas::project::VirtualControl& control, double startValue, double dragFraction)
{
    return quantizedVirtualControlValue(
        control, startValue + dragFraction * (control.maximum - control.minimum) * vcsFineTuningRatio);
}

double vcsInvisibleFaderValue(const talas::project::VirtualControl& control, double startValue, double dragFraction)
{
    return quantizedVirtualControlValue(control, startValue + dragFraction * (control.maximum - control.minimum));
}

std::optional<std::size_t> vcsTabbedNumber(std::size_t count, std::optional<std::size_t> current, bool forward) noexcept
{
    if (count == 0)
        return std::nullopt;
    if (!current.has_value() || *current >= count)
        return forward ? 0 : count - 1;
    return forward ? (*current + 1) % count : (*current + count - 1) % count;
}

double quantizedVirtualControlValue(const talas::project::VirtualControl& control, double value)
{
    if (!std::isfinite(value))
        value = control.minimum;
    value = std::clamp(value, control.minimum, control.maximum);
    const auto grid = control.widget.grid;
    // A log fader's grid below 1 is a fraction of the fader's travel: its
    // position steps by it.
    if (logTaper(control) && grid > 0.0 && grid < 1.0 && control.maximum > control.minimum)
    {
        const auto position = std::round(std::log(value / control.minimum) / std::log(control.maximum / control.minimum) / grid) * grid;
        return std::clamp(control.minimum * std::pow(control.maximum / control.minimum, std::min(position, 1.0)), control.minimum,
            control.maximum);
    }
    if (grid > 0.0 && std::isfinite(grid))
    {
        value = control.minimum + std::round((value - control.minimum) / grid) * grid;
        if (value > control.maximum + 1e-12)
            value -= grid;
        value = std::clamp(value, control.minimum, control.maximum);
    }
    return value;
}

std::optional<double> virtualControlButtonValue(const talas::project::VirtualControl& control, bool pressed)
{
    if (control.kind == talas::project::VirtualControlKind::Gate)
        return pressed ? control.maximum : control.minimum;
    if (control.kind == talas::project::VirtualControlKind::Toggle && pressed)
        return control.value >= (control.minimum + control.maximum) * 0.5 ? control.minimum : control.maximum;
    return std::nullopt;
}

double virtualControlValueAt(const talas::project::VirtualControl& control, double position)
{
    position = std::clamp(std::isfinite(position) ? position : 0.0, 0.0, 1.0);
    const auto value = logTaper(control)
        ? control.minimum * std::pow(control.maximum / control.minimum, position)
        : control.minimum + (control.maximum - control.minimum) * position;
    return quantizedVirtualControlValue(control, value);
}

double virtualControlPositionOf(const talas::project::VirtualControl& control, double value)
{
    if (!(control.maximum > control.minimum))
        return 0.0;
    value = std::clamp(std::isfinite(value) ? value : control.minimum, control.minimum, control.maximum);
    if (logTaper(control))
        return std::log(value / control.minimum) / std::log(control.maximum / control.minimum);
    return (value - control.minimum) / (control.maximum - control.minimum);
}

std::string virtualControlLabel(const talas::project::VirtualControl& control)
{
    return control.widget.label.empty() ? control.eventName : control.widget.label;
}

std::string vcsAbbreviatedName(std::string_view name, std::size_t characters)
{
    std::string shown{name};
    for (auto at = shown.size(); shown.size() > characters && at > 1; --at)
    {
        if (std::string_view{"aeiou"}.find(shown[at - 1]) != std::string_view::npos)
            shown.erase(at - 1, 1);
    }
    if (shown.size() > characters)
        shown.resize(characters);
    return shown;
}

std::vector<std::string> virtualControlTabs(const std::vector<talas::project::VirtualControl>& controls)
{
    std::vector<std::string> tabs;
    for (const auto& control : controls)
    {
        if (std::ranges::find(tabs, control.widget.tab) == tabs.end())
            tabs.push_back(control.widget.tab);
    }
    // The main tab first.
    if (const auto main = std::ranges::find(tabs, std::string{}); main != tabs.end() && main != tabs.begin())
        std::rotate(tabs.begin(), main, main + 1);
    return tabs;
}

std::string_view virtualControlWidgetStyleName(talas::project::VirtualControlWidgetStyle style)
{
    using Style = talas::project::VirtualControlWidgetStyle;
    switch (style)
    {
    case Style::Default: return "default";
    case Style::Fader: return "fader";
    case Style::HorizontalFader: return "horizontalFader";
    case Style::SmallFader: return "smallFader";
    case Style::Potentiometer: return "potentiometer";
    case Style::ButtonFill: return "buttonFill";
    }
    return "default";
}

std::optional<talas::project::VirtualControlWidgetStyle> parseVirtualControlWidgetStyle(std::string_view name)
{
    using Style = talas::project::VirtualControlWidgetStyle;
    for (const auto style :
        {Style::Default, Style::Fader, Style::HorizontalFader, Style::SmallFader, Style::Potentiometer, Style::ButtonFill})
    {
        if (virtualControlWidgetStyleName(style) == name)
            return style;
    }
    return std::nullopt;
}

std::string virtualControlColourText(std::optional<std::uint32_t> colour)
{
    if (!colour.has_value())
        return {};
    static constexpr char digits[] = "0123456789abcdef";
    std::string text = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        text.push_back(digits[(*colour >> shift) & 0xFu]);
    return text;
}

std::optional<std::optional<std::uint32_t>> parseVirtualControlColour(std::string_view text)
{
    if (text.empty())
        return std::optional<std::uint32_t>{};
    if (text.front() == '#')
        text.remove_prefix(1);
    if (text.size() != 6)
        return std::nullopt;
    std::uint32_t colour = 0;
    for (const auto character : text)
    {
        const auto lower = static_cast<char>(character >= 'A' && character <= 'F' ? character - 'A' + 'a' : character);
        const auto digit = lower >= '0' && lower <= '9' ? lower - '0' : lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
        if (digit < 0)
            return std::nullopt;
        colour = colour << 4 | static_cast<std::uint32_t>(digit);
    }
    return std::optional<std::uint32_t>{colour};
}
}
