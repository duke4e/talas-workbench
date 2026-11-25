#include "talas/plugin/PluginParameters.h"

#include "project/JsonValue.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/VirtualControlMidi.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace talas::plugin
{
namespace
{
namespace json = talas::project::json;

// The units a control's label may end with, as "(unit)", that a host shows
// after the value rather than in the name.
constexpr std::array knownUnits = {
    std::string_view{"Hz"}, std::string_view{"kHz"}, std::string_view{"s"}, std::string_view{"ms"},
    std::string_view{"dB"}, std::string_view{"BPM"}, std::string_view{"cents"}, std::string_view{"note"},
    std::string_view{"nn"}, std::string_view{"frames"}, std::string_view{"partials"},
    std::string_view{"grains/s"}, std::string_view{"%"}, std::string_view{"st"},
    std::string_view{"semitones"}, std::string_view{"samples"}, std::string_view{"x"},
};

[[nodiscard]] std::string_view domainUnit(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Hertz:
        return "Hz";
    case talas::expr::ValueDomain::Seconds:
        return "s";
    case talas::expr::ValueDomain::NoteNumber:
        return "nn";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    case talas::expr::ValueDomain::Scalar:
        break;
    }
    return {};
}

[[nodiscard]] PluginParameterKind parameterKind(talas::project::VirtualControlKind kind) noexcept
{
    switch (kind)
    {
    case talas::project::VirtualControlKind::Toggle:
        return PluginParameterKind::Toggle;
    case talas::project::VirtualControlKind::Gate:
        return PluginParameterKind::Momentary;
    case talas::project::VirtualControlKind::Fader:
        break;
    }
    return PluginParameterKind::Continuous;
}

// Whether an instrument's host sets this Event Value through MIDI rather
// than as a parameter: the built-in controller names (!cc07, !bt64, !sw65)
// and the MIDI Event Sources (!MIDIPitchBend and the rest).
[[nodiscard]] bool isMidiDrivenEventName(std::string_view name) noexcept
{
    return talas::project::parseBuiltInMidiControllerName(name).has_value() || name.starts_with("MIDI");
}

void splitLabel(std::string_view label, std::string& displayName, std::string& unit)
{
    displayName = std::string{label};
    if (!label.ends_with(')'))
        return;
    const auto open = label.rfind(" (");
    if (open == std::string_view::npos || open == 0)
        return;
    const auto inside = label.substr(open + 2, label.size() - open - 3);
    if (std::ranges::find(knownUnits, inside) == knownUnits.end())
        return;
    displayName = std::string{label.substr(0, open)};
    unit = std::string{inside};
}

[[nodiscard]] std::optional<PluginParameterSpec> specFor(
    const talas::project::VirtualControl& control,
    bool declared,
    std::vector<std::string>& warnings)
{
    PluginParameterSpec spec;
    spec.eventName = control.eventName;
    spec.domain = control.domain;
    spec.kind = parameterKind(control.kind);
    spec.declared = declared;
    spec.minimum = control.minimum;
    spec.maximum = control.maximum;
    if (!std::isfinite(spec.minimum) || !std::isfinite(spec.maximum) || !(spec.maximum > spec.minimum))
    {
        if (spec.kind == PluginParameterKind::Continuous)
        {
            warnings.push_back(
                "!" + control.eventName + " has no usable range (" + std::to_string(control.minimum) + " to "
                + std::to_string(control.maximum) + "); it stays at its default");
            return std::nullopt;
        }
        spec.minimum = 0.0;
        spec.maximum = 1.0;
    }
    spec.defaultValue = std::isfinite(control.value) ? control.value : spec.minimum;
    spec.logarithmic = spec.kind == PluginParameterKind::Continuous
        && control.widget.taper == talas::project::VirtualControlTaper::Log && spec.minimum > 0.0;
    spec.step = spec.kind == PluginParameterKind::Continuous && std::isfinite(control.widget.grid)
            && control.widget.grid > 0.0 && control.widget.grid < spec.maximum - spec.minimum
        ? control.widget.grid
        : 0.0;
    spec.defaultValue = clampParameterValue(spec, spec.defaultValue);
    splitLabel(control.widget.label.empty() ? std::string_view{control.eventName}
                                            : std::string_view{control.widget.label},
        spec.displayName, spec.unit);
    if (spec.unit.empty())
        spec.unit = std::string{domainUnit(spec.domain)};
    return spec;
}

void forEachNode(
    const talas::project::ProjectDocument& document,
    const auto& visit)
{
    for (const auto& node : document.graph.nodes)
        visit(node);
    for (const auto& definition : document.soundDefinitions)
    {
        for (const auto& node : definition.graph.nodes)
            visit(node);
    }
}

[[nodiscard]] const talas::project::ProjectParameterExpression* findParameter(
    const talas::project::ProjectNode& node,
    std::string_view id)
{
    const auto found = std::ranges::find(node.parameters, id, &talas::project::ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

void appendNumber(std::string& out, double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    char buffer[64] = {};
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, status == std::errc{} ? end : buffer);
}
}

PluginParameterMapping mapProjectParameters(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    PluginKind kind)
{
    PluginParameterMapping mapping;
    std::unordered_set<std::string> seen;
    const auto add = [&](const talas::project::VirtualControl& control, bool declared) {
        if (control.eventName.empty() || seen.contains(control.eventName))
            return;
        if (kind == PluginKind::Instrument && isMidiDrivenEventName(control.eventName))
            return;
        seen.insert(control.eventName);
        if (mapping.parameters.size() >= maximumPluginParameters)
        {
            mapping.warnings.push_back(
                "!" + control.eventName + " is not a parameter: a plugin shows at most "
                + std::to_string(maximumPluginParameters));
            return;
        }
        if (auto spec = specFor(control, declared, mapping.warnings))
            mapping.parameters.push_back(std::move(*spec));
    };
    for (const auto& control : document.virtualControls)
        add(control, true);
    const auto discovered = talas::project::discoverProjectEventValueControls(document, registry, true);
    for (const auto& diagnostic : discovered.diagnostics)
        mapping.warnings.push_back(diagnostic.message + (diagnostic.eventName.empty() ? "" : " (!" + diagnostic.eventName + ")"));
    for (const auto& control : discovered.missingControls)
        add(control, false);
    return mapping;
}

double clampParameterValue(const PluginParameterSpec& spec, double value) noexcept
{
    if (!std::isfinite(value))
        value = spec.defaultValue;
    if (spec.kind != PluginParameterKind::Continuous)
        return value >= (spec.minimum + spec.maximum) * 0.5 ? spec.maximum : spec.minimum;
    value = std::clamp(value, spec.minimum, spec.maximum);
    if (spec.step > 0.0)
    {
        value = spec.minimum + std::round((value - spec.minimum) / spec.step) * spec.step;
        value = std::clamp(value, spec.minimum, spec.maximum);
    }
    return value;
}

double parameterValueFromNormalized(const PluginParameterSpec& spec, double normalized) noexcept
{
    if (!std::isfinite(normalized))
        normalized = 0.0;
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (spec.kind != PluginParameterKind::Continuous)
        return normalized >= 0.5 ? spec.maximum : spec.minimum;
    const auto value = spec.logarithmic
        ? spec.minimum * std::pow(spec.maximum / spec.minimum, normalized)
        : spec.minimum + normalized * (spec.maximum - spec.minimum);
    return clampParameterValue(spec, value);
}

double parameterValueToNormalized(const PluginParameterSpec& spec, double value) noexcept
{
    value = clampParameterValue(spec, value);
    if (spec.kind != PluginParameterKind::Continuous)
        return value >= spec.maximum ? 1.0 : 0.0;
    const auto normalized = spec.logarithmic
        ? std::log(value / spec.minimum) / std::log(spec.maximum / spec.minimum)
        : (value - spec.minimum) / (spec.maximum - spec.minimum);
    return std::clamp(std::isfinite(normalized) ? normalized : 0.0, 0.0, 1.0);
}

std::array<bool, 8> projectAudioInputChannels(const talas::project::ProjectDocument& document)
{
    std::array<bool, 8> channels{};
    forEachNode(document, [&](const talas::project::ProjectNode& node) {
        if (node.typeId != "talas.audioInput")
            return;
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
        {
            const auto* parameter = findParameter(node, "channel" + std::to_string(channel + 1));
            // Channel 1 is on unless the node turns it off; the rest off unless on.
            const auto selected = parameter != nullptr && parameter->boolean.has_value()
                ? *parameter->boolean
                : channel == 0;
            channels[channel] = channels[channel] || selected;
        }
    });
    return channels;
}

bool projectPlaysLiveMidi(const talas::project::ProjectDocument& document)
{
    bool live = false;
    forEachNode(document, [&](const talas::project::ProjectNode& node) {
        if (node.typeId != "talas.midiVoice" && node.typeId != "talas.midiMapper")
            return;
        const auto* source = findParameter(node, "source");
        const auto text = source != nullptr && source->text.has_value() ? std::string_view{*source->text}
                                                                        : std::string_view{};
        live = live || text.empty() || text == "Live MIDI";
    });
    return live;
}

std::string serializePluginState(const PluginState& state)
{
    std::string text = "{\"format\":";
    json::appendEscapedString(text, pluginStateFormat);
    text += ",\"version\":" + std::to_string(pluginStateFormatVersion) + ",\"plugin\":";
    json::appendEscapedString(text, state.pluginName);
    text += ",\"projectHash\":";
    json::appendEscapedString(text, state.projectHash);
    text += ",\"parameters\":{";
    bool first = true;
    for (const auto& [name, value] : state.parameters)
    {
        if (!first)
            text += ',';
        first = false;
        json::appendEscapedString(text, name);
        text += ':';
        appendNumber(text, value);
    }
    text += "}}";
    return text;
}

std::optional<PluginState> parsePluginState(std::string_view text, std::string* error)
{
    json::ParseLimits limits;
    limits.maximumInputBytes = 1024 * 1024;
    limits.maximumNestingDepth = 4;
    limits.requireValidUtf8 = true;
    const auto parsed = json::parse(text, limits);
    const auto fail = [&](std::string message) -> std::optional<PluginState> {
        if (error != nullptr)
            *error = std::move(message);
        return std::nullopt;
    };
    if (!parsed.ok() || !parsed.value.isObject())
        return fail("The plugin state is not a JSON object");
    const auto* format = json::findObjectMember(parsed.value, "format");
    if (format == nullptr || !format->isString() || format->string() != pluginStateFormat)
        return fail("The plugin state is not a Talas plugin state");
    const auto* version = json::findObjectMember(parsed.value, "version");
    if (version == nullptr || !version->isNumber() || version->number() != pluginStateFormatVersion)
        return fail("The plugin state has an unsupported version");
    PluginState state;
    if (const auto* plugin = json::findObjectMember(parsed.value, "plugin"); plugin != nullptr && plugin->isString())
        state.pluginName = plugin->string();
    if (const auto* hash = json::findObjectMember(parsed.value, "projectHash"); hash != nullptr && hash->isString())
        state.projectHash = hash->string();
    const auto* parameters = json::findObjectMember(parsed.value, "parameters");
    if (parameters == nullptr || !parameters->isObject())
        return fail("The plugin state has no parameters object");
    for (const auto& [name, value] : parameters->object())
    {
        if (value.isNumber() && std::isfinite(value.number()))
            state.parameters.emplace_back(name, value.number());
    }
    return state;
}
}
