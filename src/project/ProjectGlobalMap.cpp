#include "talas/project/ProjectGlobalMap.h"

#include "talas/expr/Expression.h"
#include "talas/project/MidiDefaultMap.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string twoDigits(unsigned value)
{
    return (value < 10 ? "0" : "") + std::to_string(value);
}

// A number as Val reads it back exactly: the shortest digits that
// give the double, a negative one written as 0 minus it.
[[nodiscard]] std::optional<std::string> literal(double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::abs(value));
    if (failed != std::errc{})
        return std::nullopt;
    std::string digits(buffer.data(), end);
    return value < 0.0 ? "(0 - " + digits + ")" : digits;
}

// (x + c), written (x - |c|) for a negative c; x for 0.
[[nodiscard]] std::optional<std::string> plus(const std::string& x, double c)
{
    if (c == 0.0)
        return x;
    const auto amount = literal(std::abs(c));
    if (!amount.has_value())
        return std::nullopt;
    return "(" + x + (c < 0.0 ? " - " : " + ") + *amount + ")";
}

// (x * c); x for 1.
[[nodiscard]] std::optional<std::string> times(const std::string& x, double c)
{
    if (c == 1.0)
        return x;
    const auto factor = literal(c);
    if (!factor.has_value())
        return std::nullopt;
    return "(" + x + " * " + *factor + ")";
}
}

GlobalMapParseResult parseProjectGlobalMap(std::string_view text)
{
    GlobalMapParseResult result;
    if (text.size() > maximumGlobalMapBytes)
    {
        result.error = "A global map holds at most 16384 bytes";
        return result;
    }
    std::size_t lineNumber = 0;
    while (!text.empty())
    {
        ++lineNumber;
        const auto end = text.find('\n');
        const auto line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        // Blank lines and "comments" in double quotes between associations.
        if (line.empty() || (line.front() == '"' && line.back() == '"' && line.size() > 1))
            continue;
        const auto fail = [&](std::string message) {
            result.entries.clear();
            result.error = std::move(message);
            result.line = lineNumber;
            return result;
        };
        const auto parsed = parseProjectMidiMapperMap(line);
        if (!parsed.ok() || parsed.mode != MidiMapperMapMode::Passthrough || parsed.localAssociationCount != 1)
            return fail("Expected !Name is: `MIDIControllerNN or `MIDIPitchBend, with options in parentheses");
        const auto& association = parsed.localAssociations[0];
        if (association.pitchBendSemitones.has_value())
            return fail("The global map takes MIDI controllers and the pitch wheel, not key numbers");
        GlobalMapEntry entry;
        entry.eventName = std::string{association.localEventValue};
        if (entry.eventName.starts_with('!'))
            entry.eventName.erase(0, 1);
        const auto source = association.localEventSource;
        constexpr std::string_view controllerPrefix = "MIDIController";
        if (source == "MIDIPitchBend")
        {
            entry.pitchBend = true;
            entry.minimum = -1.0;
            entry.maximum = 1.0;
            if (association.hasRange() || association.hasGrid() || association.channel.has_value())
                return fail("The pitch wheel takes no channel, range or grid in the global map");
        }
        else if (source.starts_with(controllerPrefix))
        {
            const auto digits = source.substr(controllerPrefix.size());
            unsigned controller = 0;
            const auto [end, failed] = std::from_chars(digits.data(), digits.data() + digits.size(), controller);
            if (digits.empty() || failed != std::errc{} || end != digits.data() + digits.size() || controller > 127)
                return fail("A MIDI controller's number is 0 to 127");
            entry.controller = static_cast<std::uint8_t>(controller);
        }
        else if (!source.starts_with("MIDI") && !source.empty()
            && ((source.front() >= 'a' && source.front() <= 'z') || (source.front() >= 'A' && source.front() <= 'Z'))
            && std::ranges::all_of(source, [](char character) {
                   return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                       || (character >= '0' && character <= '9');
               }))
        {
            // Any legal variable name is another Event Source, which only
            // Talas sets.
            entry.source = std::string{source};
        }
        else
        {
            return fail("The global map takes `MIDIControllerNN, `MIDIPitchBend or another source named as a variable");
        }
        entry.channel = association.channel;
        if (association.hasRange())
        {
            if (!parseProjectMidiMapperMapBound(association.minimumText, entry.minimum)
                || !parseProjectMidiMapperMapBound(association.maximumText, entry.maximum)
                || !(entry.maximum > entry.minimum))
                return fail("min: lies below max:");
        }
        if (association.hasGrid()
            && (!parseProjectMidiMapperMapBound(association.gridText, entry.grid) || entry.grid < 0.0))
            return fail("grid: is a step that is not negative");
        entry.taper = association.taper;
        if (entry.taper == MidiMapperMapTaper::Log && entry.minimum < 0.0)
            return fail("A log taper needs a minimum of 0 or more");
        entry.displayAs = association.displayAs;
        if (std::ranges::any_of(result.entries, [&](const auto& other) { return other.eventName == entry.eventName; }))
            return fail("!" + entry.eventName + " is already in the map");
        if (result.entries.size() >= maximumGlobalMapEntries)
            return fail("A global map holds at most 128 associations");
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::string defaultGlobalMapText()
{
    std::string text;
    for (const auto& entry : defaultMidiMapControllers)
    {
        text += "!" + std::string{entry.eventName} + " is: ";
        const auto source = "`MIDIController" + twoDigits(entry.controller);
        if (entry.kind == VirtualControlKind::Gate)
            text += "(" + source + " displayAs: #gate)";
        else if (entry.kind == VirtualControlKind::Toggle)
            text += "(" + source + " displayAs: #toggle)";
        else
            text += source;
        text += ".\n";
    }
    text += "!" + std::string{defaultMidiMapPitchBendName} + " is: `MIDIPitchBend.\n";
    return text;
}

std::vector<GlobalMapEntry> projectGlobalMapEntries(const ProjectDocument& document)
{
    auto parsed = parseProjectGlobalMap(document.globalMap.empty() ? defaultGlobalMapText() : document.globalMap);
    if (!parsed.ok())
        return {};
    std::erase_if(parsed.entries, [&](const GlobalMapEntry& entry) {
        return projectMapsEventValue(document, entry.eventName);
    });
    return parsed.entries;
}

std::optional<double> globalMapValue(const GlobalMapEntry& entry, double normalized) noexcept
{
    if (!std::isfinite(normalized))
        return std::nullopt;
    if (entry.pitchBend)
        return std::clamp(normalized, -1.0, 1.0);
    auto value = projectMidiMapperTaperValue(std::clamp(normalized, 0.0, 1.0), entry.minimum, entry.maximum, entry.taper);
    if (!value.has_value())
        return std::nullopt;
    if (entry.grid > 0.0)
    {
        *value = entry.minimum + std::round((*value - entry.minimum) / entry.grid) * entry.grid;
        *value = std::clamp(*value, entry.minimum, entry.maximum);
    }
    return value;
}

VirtualControlKind globalMapControlKind(const GlobalMapEntry& entry) noexcept
{
    switch (entry.displayAs)
    {
    case MidiMapperMapDisplayAs::Gate:
        return VirtualControlKind::Gate;
    case MidiMapperMapDisplayAs::Toggle:
        return VirtualControlKind::Toggle;
    default:
        return VirtualControlKind::Fader;
    }
}

std::optional<std::string> globalMapEventSource(const GlobalMapEntry& entry)
{
    if (entry.pitchBend)
        return std::string{"`MIDIPitchBend"};
    if (!entry.source.empty())
        return "`" + entry.source;
    if (entry.channel.has_value())
        return std::nullopt;
    return "`MIDIController" + twoDigits(entry.controller);
}

std::optional<std::string> globalMapSourceExpression(const GlobalMapEntry& entry)
{
    const auto source = globalMapEventSource(entry);
    if (!source.has_value())
        return std::nullopt;
    // The wheel's value from -1 to 1 is its Event Value's.
    if (entry.pitchBend)
        return source;
    if (!std::isfinite(entry.minimum) || !std::isfinite(entry.maximum) || !(entry.maximum > entry.minimum))
        return std::nullopt;
    // The taper, as projectMidiMapperTaperValue gives it.
    std::optional<std::string> value;
    if (entry.taper == MidiMapperMapTaper::Linear)
    {
        const auto scaled = times(*source, entry.maximum - entry.minimum);
        value = scaled.has_value() ? plus(*scaled, entry.minimum) : std::nullopt;
    }
    else if (entry.taper == MidiMapperMapTaper::Log && entry.minimum == 0.0)
    {
        // maximum * (e^(ln 1000 * n) - 1) / 999.
        const auto exponent = times(*source, std::log(1000.0));
        const auto factor = literal(entry.maximum);
        if (exponent.has_value() && factor.has_value())
            value = "(" + *factor + " * (((" + *exponent + " exp) - 1) / 999))";
    }
    else if (entry.taper == MidiMapperMapTaper::Log && entry.minimum > 0.0)
    {
        // e^(ln minimum + n * (ln maximum - ln minimum)).
        const auto scaled = times(*source, std::log(entry.maximum) - std::log(entry.minimum));
        const auto shifted = scaled.has_value() ? plus(*scaled, std::log(entry.minimum)) : std::nullopt;
        if (shifted.has_value())
            value = "(" + *shifted + " exp)";
    }
    if (!value.has_value() || !(entry.grid > 0.0))
        return value;
    // On the grid from the minimum, kept below the maximum (globalMapValue).
    const auto offset = plus(*value, -entry.minimum);
    const auto grid = literal(entry.grid);
    const auto top = literal(entry.maximum);
    if (!offset.has_value() || !grid.has_value() || !top.has_value())
        return std::nullopt;
    const auto stepped = plus("((" + *offset + " / " + *grid + ") rounded * " + *grid + ")", entry.minimum);
    if (!stepped.has_value())
        return std::nullopt;
    return "(" + *stepped + " min: " + *top + ")";
}

std::optional<double> globalMapSourceValue(const GlobalMapEntry& entry, double value) noexcept
{
    if (!std::isfinite(value))
        return std::nullopt;
    if (entry.pitchBend)
        return std::clamp(value, -1.0, 1.0);
    if (!(entry.maximum > entry.minimum))
        return std::nullopt;
    return projectMidiMapperTaperInverse(
        std::clamp(value, entry.minimum, entry.maximum), entry.minimum, entry.maximum, entry.taper);
}

std::vector<GlobalMapEventSourceRewrite> writeGlobalMapEventSources(const ProjectDocument& document,
    ProjectGraph& graph, std::span<const std::string> kept, const std::vector<std::string>* nodeIds)
{
    constexpr auto val = talas::expr::ExpressionLanguageVersion::ValV1;
    std::vector<GlobalMapEventSourceRewrite> rewrites;
    if (document.expressionLanguage != val)
        return rewrites;
    const auto entries = projectGlobalMapEntries(document);
    if (entries.empty())
        return rewrites;
    // A MIDIMapper's own names stay its own.
    std::vector<std::string> mapped;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId != "talas.midiMapper")
            continue;
        const auto map = std::ranges::find(node.parameters, std::string_view{"map"}, &ProjectParameterExpression::parameterId);
        if (map == node.parameters.end() || !map->text.has_value())
            continue;
        const auto parsed = parseProjectMidiMapperMap(*map->text);
        for (std::size_t index = 0; parsed.ok() && index < parsed.localAssociationCount; ++index)
        {
            std::string name{parsed.localAssociations[index].localEventValue};
            if (name.starts_with('!'))
                name.erase(0, 1);
            mapped.push_back(std::move(name));
        }
    }
    // The rewrite of a name the map holds and a field may lose.
    const auto rewriteOf = [&](const std::string& name) -> std::optional<GlobalMapEventSourceRewrite> {
        if (std::ranges::find(kept, name) != kept.end() || std::ranges::find(mapped, name) != mapped.end())
            return std::nullopt;
        const auto entry = std::ranges::find(entries, name, &GlobalMapEntry::eventName);
        if (entry == entries.end())
            return std::nullopt;
        const auto control = std::ranges::find(document.virtualControls, name, &VirtualControl::eventName);
        if (control != document.virtualControls.end() && control->domain != talas::expr::ValueDomain::Scalar)
            return std::nullopt;
        const auto source = globalMapEventSource(*entry);
        const auto sourceName = source.has_value() ? talas::expr::eventSourceEventName(source->substr(1)) : std::nullopt;
        if (!sourceName.has_value() || !globalMapSourceExpression(*entry).has_value())
            return std::nullopt;
        return GlobalMapEventSourceRewrite{name, *sourceName, *entry};
    };
    for (auto& node : graph.nodes)
    {
        if (nodeIds != nullptr && std::ranges::find(*nodeIds, node.instanceId) == nodeIds->end())
            continue;
        for (auto& parameter : node.parameters)
        {
            if (!parameter.expression.has_value())
                continue;
            const auto names = talas::expr::scanEventReferenceNames(*parameter.expression, val);
            if (!names.has_value())
                continue;
            std::vector<GlobalMapEventSourceRewrite> found;
            std::vector<std::string> expressions;
            for (const auto& name : *names)
            {
                if (auto rewrite = rewriteOf(name))
                {
                    expressions.push_back(*globalMapSourceExpression(rewrite->entry));
                    found.push_back(std::move(*rewrite));
                }
            }
            if (found.empty())
                continue;
            std::vector<talas::expr::EventReferenceReplacement> replacements;
            for (std::size_t index = 0; index < found.size(); ++index)
                replacements.push_back({found[index].eventName, expressions[index]});
            auto written = talas::expr::replaceEventReferences(*parameter.expression, val, replacements);
            // A field the sources would not fit or parse in keeps its names.
            if (!written.has_value() || !talas::expr::parse(*written, val, document.expressionDefinitions).ok())
                continue;
            parameter.expression = std::move(*written);
            for (auto& rewrite : found)
            {
                if (std::ranges::find(rewrites, rewrite.eventName, &GlobalMapEventSourceRewrite::eventName)
                    == rewrites.end())
                    rewrites.push_back(std::move(rewrite));
            }
        }
    }
    return rewrites;
}

std::optional<std::string> EventValueLearning::key(bool down, bool oneKeyGivesPitch)
{
    if (down)
    {
        ++held_;
        most_ = std::max(most_, held_);
        return std::nullopt;
    }
    if (held_ == 0)
        return std::nullopt;
    if (--held_ > 0)
        return std::nullopt;
    const auto keys = std::exchange(most_, 0);
    if (keys == 1)
        return oneKeyGivesPitch ? "!Pitch" : "!KeyPitch";
    return keys == 2 ? "!KeyDown" : "!KeyVelocity";
}

std::string EventValueLearning::controller(
    const ProjectDocument& document, std::uint8_t controller, std::uint8_t channel)
{
    for (const auto& control : document.virtualControls)
    {
        if (control.midiMapping.has_value() && control.midiMapping->controller == controller
            && control.midiMapping->channel == channel)
            return "!" + control.eventName;
    }
    for (const auto& entry : projectGlobalMapEntries(document))
    {
        if (!entry.pitchBend && entry.source.empty() && entry.controller == controller
            && (!entry.channel.has_value() || *entry.channel == channel))
            return "!" + entry.eventName;
    }
    const auto number = std::to_string(controller);
    return "!cc" + std::string(number.size() < 2 ? 2 - number.size() : 0, '0') + number;
}
}
