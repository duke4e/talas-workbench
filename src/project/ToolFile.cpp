#include "talas/project/ToolFile.h"

#include "JsonValue.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <utility>

namespace talas::project::tool
{
namespace
{
constexpr std::array<std::pair<std::string_view, Kind>, 7> kindNames{{
    {"object", Kind::Object},
    {"number", Kind::Number},
    {"string", Kind::String},
    {"random", Kind::RandomNumbers},
    {"textFile", Kind::TextFile},
    {"compiledSoundGrid", Kind::CompiledSoundGrid},
    {"view", Kind::View},
}};

constexpr std::array<std::pair<std::string_view, FileAccess>, 5> fileAccessNames{{
    {"readOnly", FileAccess::ReadOnly},
    {"writeOnly", FileAccess::WriteOnly},
    {"append", FileAccess::Append},
    {"readAppend", FileAccess::ReadAppend},
    {"readWrite", FileAccess::ReadWrite},
}};

constexpr std::array<std::pair<std::string_view, EventAccess>, 4> eventAccessNames{{
    {"none", EventAccess::None},
    {"readOnly", EventAccess::ReadOnly},
    {"writeOnly", EventAccess::WriteOnly},
    {"readWrite", EventAccess::ReadWrite},
}};

// How a step runs the responses whose conditions
// held; a file without it takes the default rule, every one.
constexpr std::array<std::pair<std::string_view, StepRule>, 2> stepRuleNames{{
    {"everyTrueResponse", StepRule::EveryTrueResponse},
    {"firstTrueResponse", StepRule::FirstTrueResponse},
}};

constexpr std::array<std::pair<std::string_view, WidgetKind>, 12> widgetNames{{
    {"none", WidgetKind::None},
    {"readOnlyText", WidgetKind::ReadOnlyText},
    {"text", WidgetKind::Text},
    {"meter", WidgetKind::Meter},
    {"fader", WidgetKind::Fader},
    {"faderNoNumber", WidgetKind::FaderNoNumber},
    {"radioButtons", WidgetKind::RadioButtons},
    {"button", WidgetKind::Button},
    {"triggerButton", WidgetKind::TriggerButton},
    {"toggleButton", WidgetKind::ToggleButton},
    {"stickButton", WidgetKind::StickButton},
    {"view", WidgetKind::View},
}};

template <typename Enum, std::size_t Count>
[[nodiscard]] std::string_view nameOf(const std::array<std::pair<std::string_view, Enum>, Count>& names, Enum value)
{
    const auto found = std::ranges::find(names, value, &std::pair<std::string_view, Enum>::second);
    return found == names.end() ? names.front().first : found->first;
}

template <typename Enum, std::size_t Count>
[[nodiscard]] std::optional<Enum> valueOf(const std::array<std::pair<std::string_view, Enum>, Count>& names, std::string_view name)
{
    const auto found = std::ranges::find(names, name, &std::pair<std::string_view, Enum>::first);
    return found == names.end() ? std::nullopt : std::optional<Enum>{found->second};
}

void quoted(std::string& output, std::string_view text)
{
    output.push_back('"');
    for (const auto character : text)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                constexpr std::string_view digits = "0123456789abcdef";
                output += "\\u00";
                output.push_back(digits[(static_cast<unsigned char>(character) >> 4) & 0xf]);
                output.push_back(digits[static_cast<unsigned char>(character) & 0xf]);
            }
            else
                output.push_back(character);
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string number(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}

void writeVariables(std::string& output, const std::vector<Variable>& variables, std::string_view indent)
{
    output += "[";
    for (std::size_t index = 0; index < variables.size(); ++index)
    {
        const auto& variable = variables[index];
        const auto& widget = variable.widget;
        output += index == 0 ? "\n" : ",\n";
        output += indent;
        output += "{\"name\": ";
        quoted(output, variable.name);
        output += ", \"kind\": ";
        quoted(output, nameOf(kindNames, variable.kind));
        output += ", \"initially\": ";
        quoted(output, variable.initially);
        output += ", \"fileAccess\": ";
        quoted(output, nameOf(fileAccessNames, variable.fileAccess));
        output += std::string{", \"stepWhenChanged\": "} + (variable.stepWhenChanged ? "true" : "false");
        output += ", \"eventValue\": ";
        quoted(output, variable.eventValue);
        output += ", \"access\": ";
        quoted(output, nameOf(eventAccessNames, variable.access));
        for (const auto& [key, value] : {std::pair{"minimum", variable.minimum}, std::pair{"maximum", variable.maximum},
                 std::pair{"grid", variable.grid}})
        {
            if (value.has_value())
                output += std::string{", \""} + key + "\": " + number(*value);
        }
        output += ", \"widget\": {\"kind\": ";
        quoted(output, nameOf(widgetNames, widget.kind));
        output += std::string{", \"labelled\": "} + (widget.labelled ? "true" : "false");
        output += ", \"bounds\": [" + number(widget.left) + ", " + number(widget.top) + ", " + number(widget.width) + ", "
            + number(widget.height) + "]";
        if (widget.digits.has_value())
            output += ", \"digits\": " + std::to_string(*widget.digits);
        output += std::string{", \"logTaper\": "} + (widget.logTaper ? "true" : "false");
        output += ", \"columns\": " + std::to_string(widget.columns);
        output += ", \"strings\": [";
        for (std::size_t at = 0; at < widget.strings.size(); ++at)
        {
            if (at > 0)
                output += ", ";
            quoted(output, widget.strings[at]);
        }
        output += "], \"downValue\": ";
        quoted(output, widget.downValue);
        output += ", \"upValue\": ";
        quoted(output, widget.upValue);
        output += "}}";
    }
    output += variables.empty() ? "]" : "\n" + std::string{indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0)} + "]";
}

[[nodiscard]] const std::string* stringMember(const json::Value& object, std::string_view key)
{
    const auto* member = json::findObjectMember(object, key);
    return member != nullptr && member->isString() ? &member->string() : nullptr;
}

[[nodiscard]] std::optional<Variable> readVariable(const json::Value& item, std::string& error)
{
    const auto fail = [&](std::string message) -> std::optional<Variable> {
        error = std::move(message);
        return std::nullopt;
    };
    if (!item.isObject())
        return fail("Expected each variable as an object");
    Variable variable;
    const auto* name = stringMember(item, "name");
    const auto* kind = stringMember(item, "kind");
    const auto* initially = stringMember(item, "initially");
    const auto* fileAccess = stringMember(item, "fileAccess");
    const auto* eventValue = stringMember(item, "eventValue");
    const auto* access = stringMember(item, "access");
    const auto* step = json::findObjectMember(item, "stepWhenChanged");
    if (name == nullptr || name->empty() || kind == nullptr || initially == nullptr || fileAccess == nullptr
        || eventValue == nullptr || access == nullptr || step == nullptr || !step->isBoolean())
        return fail("Expected a variable's name, kind, Initially, file access, step, Event Value and access");
    const auto parsedKind = valueOf(kindNames, *kind);
    const auto parsedFile = valueOf(fileAccessNames, *fileAccess);
    const auto parsedAccess = valueOf(eventAccessNames, *access);
    if (!parsedKind || !parsedFile || !parsedAccess)
        return fail("Unknown kind or access for the variable " + *name);
    variable.name = *name;
    variable.kind = *parsedKind;
    variable.initially = *initially;
    variable.fileAccess = *parsedFile;
    variable.stepWhenChanged = step->boolean();
    variable.eventValue = *eventValue;
    variable.access = *parsedAccess;
    for (const auto& [key, destination] : {std::pair{"minimum", &variable.minimum}, std::pair{"maximum", &variable.maximum},
             std::pair{"grid", &variable.grid}})
    {
        if (const auto* value = json::findObjectMember(item, key); value != nullptr)
        {
            if (!value->isNumber() || !std::isfinite(value->number()))
                return fail(std::string{"Expected a number for "} + key + " of " + *name);
            *destination = value->number();
        }
    }
    const auto* widget = json::findObjectMember(item, "widget");
    if (widget == nullptr || !widget->isObject())
        return fail("Expected the widget of " + *name);
    const auto* widgetKind = stringMember(*widget, "kind");
    const auto parsedWidget = widgetKind == nullptr ? std::nullopt : valueOf(widgetNames, *widgetKind);
    const auto* labelled = json::findObjectMember(*widget, "labelled");
    const auto* bounds = json::findObjectMember(*widget, "bounds");
    const auto* logTaper = json::findObjectMember(*widget, "logTaper");
    const auto* columns = json::findObjectMember(*widget, "columns");
    const auto* strings = json::findObjectMember(*widget, "strings");
    const auto* downValue = stringMember(*widget, "downValue");
    const auto* upValue = stringMember(*widget, "upValue");
    const auto numbers = bounds != nullptr && bounds->isArray() && bounds->array().size() == 4
        && std::ranges::all_of(bounds->array(), [](const json::Value& value) { return value.isNumber() && std::isfinite(value.number()); });
    if (!parsedWidget || labelled == nullptr || !labelled->isBoolean() || !numbers || logTaper == nullptr || !logTaper->isBoolean()
        || columns == nullptr || !columns->isNumber() || columns->number() < 1 || strings == nullptr || !strings->isArray()
        || downValue == nullptr || upValue == nullptr)
        return fail("Expected the widget's kind, label, rectangle, taper, columns, strings and button values for " + *name);
    variable.widget.kind = *parsedWidget;
    variable.widget.labelled = labelled->boolean();
    variable.widget.left = bounds->array()[0].number();
    variable.widget.top = bounds->array()[1].number();
    variable.widget.width = bounds->array()[2].number();
    variable.widget.height = bounds->array()[3].number();
    if (const auto* digits = json::findObjectMember(*widget, "digits"); digits != nullptr)
    {
        if (!digits->isNumber() || digits->number() < 0 || digits->number() > 12)
            return fail("Expected 0 to 12 digits for " + *name);
        variable.widget.digits = static_cast<int>(digits->number());
    }
    variable.widget.logTaper = logTaper->boolean();
    variable.widget.columns = static_cast<int>(columns->number());
    for (const auto& value : strings->array())
    {
        if (!value.isString())
            return fail("Expected the radio buttons' strings for " + *name);
        variable.widget.strings.push_back(value.string());
    }
    variable.widget.downValue = *downValue;
    variable.widget.upValue = *upValue;
    return variable;
}

[[nodiscard]] bool readVariables(const json::Value* array, std::vector<Variable>& variables, std::string& error)
{
    if (array == nullptr || !array->isArray())
    {
        error = "Expected an array of variables";
        return false;
    }
    for (const auto& item : array->array())
    {
        auto variable = readVariable(item, error);
        if (!variable.has_value())
            return false;
        if (std::ranges::any_of(variables, [&](const Variable& other) { return other.name == variable->name; }))
        {
            error = "Two variables are named " + variable->name;
            return false;
        }
        variables.push_back(std::move(*variable));
    }
    return true;
}
}

std::string writeToolFile(const Definition& tool)
{
    std::string output = "{\n  \"format\": \"talas.tool\",\n  \"version\": 1,\n";
    // Written only when it is not the rule older files take.
    if (tool.stepRule != StepRule::EveryTrueResponse)
    {
        output += "  \"stepRule\": ";
        quoted(output, nameOf(stepRuleNames, tool.stepRule));
        output += ",\n";
    }
    output += "  \"variables\": ";
    writeVariables(output, tool.variables, "    ");
    output += ",\n  \"states\": [";
    for (std::size_t index = 0; index < tool.states.size(); ++index)
    {
        const auto& state = tool.states[index];
        output += index == 0 ? "\n" : ",\n";
        output += "    {\"name\": ";
        quoted(output, state.name);
        output += ", \"variables\": ";
        writeVariables(output, state.variables, "        ");
        output += ", \"responses\": [";
        for (std::size_t at = 0; at < state.responses.size(); ++at)
        {
            output += at == 0 ? "\n" : ",\n";
            output += "        {\"condition\": ";
            quoted(output, state.responses[at].condition);
            output += ", \"response\": ";
            quoted(output, state.responses[at].response);
            output += "}";
        }
        output += state.responses.empty() ? "]}" : "\n      ]}";
    }
    output += tool.states.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return output;
}

ToolFileResult readToolFile(std::string_view text)
{
    ToolFileResult result;
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    const auto parsed = json::parse(text, limits);
    if (!parsed.ok() || !parsed.value.isObject())
    {
        result.error = parsed.errors.empty() ? "A Tool file is a JSON object" : "Invalid Tool file: " + parsed.errors.front().message;
        return result;
    }
    const auto* format = stringMember(parsed.value, "format");
    const auto* version = json::findObjectMember(parsed.value, "version");
    if (format == nullptr || *format != "talas.tool" || version == nullptr || !version->isNumber() || version->number() != 1)
    {
        result.error = "Not a Talas Tool file of version 1";
        return result;
    }
    Definition tool;
    if (const auto* rule = json::findObjectMember(parsed.value, "stepRule"); rule != nullptr)
    {
        const auto parsedRule = rule->isString() ? valueOf(stepRuleNames, rule->string()) : std::nullopt;
        if (!parsedRule.has_value())
        {
            result.error = "Expected the Tool's step rule, everyTrueResponse or firstTrueResponse";
            return result;
        }
        tool.stepRule = *parsedRule;
    }
    if (!readVariables(json::findObjectMember(parsed.value, "variables"), tool.variables, result.error))
        return result;
    const auto* states = json::findObjectMember(parsed.value, "states");
    if (states == nullptr || !states->isArray())
    {
        result.error = "Expected the Tool's states";
        return result;
    }
    for (const auto& item : states->array())
    {
        State state;
        const auto* name = item.isObject() ? stringMember(item, "name") : nullptr;
        const auto* responses = item.isObject() ? json::findObjectMember(item, "responses") : nullptr;
        if (name == nullptr || name->empty() || responses == nullptr || !responses->isArray())
        {
            result.error = "Expected each state's name, variables and responses";
            return result;
        }
        state.name = *name;
        if (!readVariables(json::findObjectMember(item, "variables"), state.variables, result.error))
            return result;
        for (const auto& response : responses->array())
        {
            const auto* condition = response.isObject() ? stringMember(response, "condition") : nullptr;
            const auto* code = response.isObject() ? stringMember(response, "response") : nullptr;
            if (condition == nullptr || code == nullptr)
            {
                result.error = "Expected each triggered response's trigger and response";
                return result;
            }
            state.responses.push_back({*condition, *code});
        }
        if (std::ranges::any_of(tool.states, [&](const State& other) { return other.name == state.name; }))
        {
            result.error = "Two states are named " + state.name;
            return result;
        }
        tool.states.push_back(std::move(state));
    }
    result.tool = std::move(tool);
    return result;
}

Definition newTool()
{
    Definition tool;
    tool.states.push_back({"InitialState", {}, {}});
    return tool;
}
}
