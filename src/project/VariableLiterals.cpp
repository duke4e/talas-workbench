#include "talas/project/VariableLiterals.h"

#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>

namespace talas::project
{
namespace
{
[[nodiscard]] bool isSpace(char value) noexcept
{
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] bool isLetter(char value) noexcept
{
    return std::isalpha(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] bool isIdentifierCharacter(char value) noexcept
{
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// A Smalltalk number: digits, perhaps negative, a fraction and an exponent;
// a leading zero before the point is required.
[[nodiscard]] std::optional<double> numberLiteralValue(std::string_view text) noexcept
{
    if (text.empty())
        return std::nullopt;
    const auto digitsAt = text.front() == '-' ? std::size_t{1} : std::size_t{0};
    if (digitsAt >= text.size() || std::isdigit(static_cast<unsigned char>(text[digitsAt])) == 0)
        return std::nullopt;
    auto value = 0.0;
    const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// The characters of a quoted string starting at `text`'s first character, a
// doubled quote standing for one; nothing unless it ends `text`.
[[nodiscard]] std::optional<std::string> quotedText(std::string_view text)
{
    if (text.size() < 2 || text.front() != '\'')
        return std::nullopt;
    std::string characters;
    for (std::size_t at = 1; at < text.size(); ++at)
    {
        if (text[at] != '\'')
        {
            characters.push_back(text[at]);
            continue;
        }
        if (at + 1 < text.size() && text[at + 1] == '\'')
        {
            characters.push_back('\'');
            ++at;
            continue;
        }
        if (at + 1 != text.size())
            return std::nullopt;
        return characters;
    }
    return std::nullopt;
}

// The numbers of an array's inside, separated by spaces (and by periods in
// a brace array).
[[nodiscard]] std::optional<std::vector<double>> arrayNumbers(std::string_view inside, bool periods)
{
    std::vector<double> elements;
    std::size_t at = 0;
    while (true)
    {
        while (at < inside.size() && (isSpace(inside[at]) || (periods && inside[at] == '.')))
            ++at;
        if (at >= inside.size())
            break;
        auto end = at;
        while (end < inside.size() && !isSpace(inside[end]))
            ++end;
        auto token = inside.substr(at, end - at);
        if (periods && token.size() > 1 && token.back() == '.')
            token.remove_suffix(1);
        const auto number = numberLiteralValue(token);
        if (!number.has_value())
            return std::nullopt;
        elements.push_back(*number);
        at = end;
    }
    return elements;
}

[[nodiscard]] std::size_t utf8Length(unsigned char lead) noexcept
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 0;
}

[[nodiscard]] std::string numberText(double value)
{
    if (value == 0.0)
        value = 0.0;
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}

[[nodiscard]] std::string elementsText(const std::vector<double>& elements)
{
    std::string text;
    for (const auto element : elements)
    {
        if (!text.empty())
            text += ' ';
        text += numberText(element);
    }
    return text;
}

// The name of a field that is wholly one ?variable.
[[nodiscard]] std::optional<std::string> wholeVariable(std::string_view text)
{
    text = trimmed(text);
    if (text.size() < 2 || text.front() != '?' || !isLetter(text[1]))
        return std::nullopt;
    if (!std::ranges::all_of(text.substr(1), isIdentifierCharacter))
        return std::nullopt;
    return std::string{text.substr(1)};
}

// An array field's elements as written: each its place in the text and
// whether it is a ?variable standing alone.
struct ArrayToken
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::optional<std::string> variable;
};

[[nodiscard]] std::optional<std::vector<ArrayToken>> arrayTokens(std::string_view text)
{
    std::vector<ArrayToken> tokens;
    std::size_t at = 0;
    auto afterNumber = false;
    while (true)
    {
        while (at < text.size() && isSpace(text[at]))
            ++at;
        if (at >= text.size())
            break;
        if (text[at] == '{')
        {
            const auto close = text.find('}', at + 1);
            if (close == std::string_view::npos)
                return std::nullopt;
            tokens.push_back({at, close + 1, std::nullopt});
            at = close + 1;
            afterNumber = false;
            continue;
        }
        auto end = at;
        while (end < text.size() && !isSpace(text[end]))
            ++end;
        const auto token = text.substr(at, end - at);
        if (const auto variable = wholeVariable(token))
        {
            tokens.push_back({at, end, *variable});
            afterNumber = false;
        }
        else if (numberLiteralValue(token).has_value())
        {
            tokens.push_back({at, end, std::nullopt});
            afterNumber = true;
        }
        else if (afterNumber && std::ranges::all_of(token, isLetter))
        {
            // A unit word belongs to the number before it (60 nn, 0.5 s).
            tokens.back().end = end;
            afterNumber = false;
        }
        else
        {
            return std::nullopt;
        }
        at = end;
    }
    if (tokens.empty())
        return std::nullopt;
    return tokens;
}

// A class's string field takes its text as typed, never a variable.
[[nodiscard]] bool takesTextAsTyped(const ProjectDocument& document, const ProjectNode& node, std::string_view parameterId)
{
    if (!node.soundDefinitionId.has_value())
        return false;
    const auto definition = std::ranges::find(
        document.soundDefinitions, *node.soundDefinitionId, &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return false;
    return std::ranges::any_of(definition->exposedParameters, [&](const ProjectSoundExposedParameter& exposed) {
        return exposed.fieldKind == ProjectSoundFieldKind::String
            && (exposed.parameterId == parameterId || std::ranges::find(exposed.aliases, parameterId) != exposed.aliases.end());
    });
}

// Each ?name in `text`, outside quotes, in order.
template <typename Visit>
void forEachVariable(std::string_view text, bool skipQuotes, Visit visit)
{
    for (std::size_t at = 0; at < text.size(); ++at)
    {
        if (skipQuotes && (text[at] == '\'' || text[at] == '"'))
        {
            const auto closing = text.find(text[at], at + 1);
            if (closing == std::string_view::npos)
                return;
            at = closing;
            continue;
        }
        if (text[at] != '?' || at + 1 >= text.size() || !isLetter(text[at + 1])
            || (at > 0 && isIdentifierCharacter(text[at - 1])))
            continue;
        auto end = at + 1;
        while (end < text.size() && isIdentifierCharacter(text[end]))
            ++end;
        visit(at, end, text.substr(at + 1, end - at - 1));
        at = end - 1;
    }
}

// `text` with each ?name the replacement gives a value for replaced by it.
template <typename Replacement>
[[nodiscard]] std::string withEachVariableReplaced(std::string_view text, bool skipQuotes, Replacement replacement)
{
    std::string written;
    std::size_t copied = 0;
    forEachVariable(text, skipQuotes, [&](std::size_t start, std::size_t end, std::string_view name) {
        if (const auto value = replacement(name))
        {
            written.append(text.substr(copied, start - copied));
            written += *value;
            copied = end;
        }
    });
    written.append(text.substr(copied));
    return written;
}
} // namespace

VariableLiteral numberLiteral(double value)
{
    return {VariableLiteralKind::Number, value, {}, {}};
}

std::optional<VariableLiteral> parseVariableLiteral(std::string_view typed)
{
    const auto text = trimmed(typed);
    if (text.empty())
        return std::nullopt;
    if (text.front() == '\'')
    {
        if (auto characters = quotedText(text))
            return VariableLiteral{VariableLiteralKind::String, 0.0, std::move(*characters), {}};
        return std::nullopt;
    }
    if (text.front() == '$')
    {
        const auto rest = text.substr(1);
        if (rest.empty() || utf8Length(static_cast<unsigned char>(rest.front())) != rest.size())
            return std::nullopt;
        return VariableLiteral{VariableLiteralKind::Character, 0.0, std::string{rest}, {}};
    }
    if (text.starts_with("#("))
    {
        if (text.back() != ')')
            return std::nullopt;
        auto elements = arrayNumbers(text.substr(2, text.size() - 3), false);
        if (!elements.has_value())
            return std::nullopt;
        return VariableLiteral{VariableLiteralKind::Array, 0.0, {}, std::move(*elements)};
    }
    if (text.front() == '{')
    {
        if (text.back() != '}')
            return std::nullopt;
        auto elements = arrayNumbers(text.substr(1, text.size() - 2), true);
        if (!elements.has_value())
            return std::nullopt;
        return VariableLiteral{VariableLiteralKind::Array, 0.0, {}, std::move(*elements)};
    }
    if (text.front() == '#')
    {
        const auto name = text.substr(1);
        if (name.starts_with('\''))
        {
            if (auto characters = quotedText(name); characters.has_value() && !characters->empty())
                return VariableLiteral{VariableLiteralKind::Symbol, 0.0, std::move(*characters), {}};
            return std::nullopt;
        }
        if (name.empty() || !(isLetter(name.front()) || name.front() == '_')
            || !std::ranges::all_of(name, [](char value) { return isIdentifierCharacter(value) || value == ':'; }))
            return std::nullopt;
        return VariableLiteral{VariableLiteralKind::Symbol, 0.0, std::string{name}, {}};
    }
    if (const auto number = numberLiteralValue(text))
        return numberLiteral(*number);
    return std::nullopt;
}

std::string variableLiteralText(const VariableLiteral& literal)
{
    switch (literal.kind)
    {
    case VariableLiteralKind::Number:
        return numberText(literal.number);
    case VariableLiteralKind::String:
    {
        std::string text{"'"};
        for (const auto character : literal.text)
        {
            text.push_back(character);
            if (character == '\'')
                text.push_back('\'');
        }
        return text + "'";
    }
    case VariableLiteralKind::Symbol:
    {
        const auto plain = !literal.text.empty() && (isLetter(literal.text.front()) || literal.text.front() == '_')
            && std::ranges::all_of(literal.text, [](char value) { return isIdentifierCharacter(value) || value == ':'; });
        if (plain)
            return "#" + literal.text;
        return "#" + variableLiteralText({VariableLiteralKind::String, 0.0, literal.text, {}});
    }
    case VariableLiteralKind::Character:
        return "$" + literal.text;
    case VariableLiteralKind::Array:
        return "#(" + elementsText(literal.elements) + ")";
    }
    return {};
}

std::string variableLiteralFieldText(const VariableLiteral& literal)
{
    switch (literal.kind)
    {
    case VariableLiteralKind::Number:
        return numberText(literal.number);
    case VariableLiteralKind::String:
    case VariableLiteralKind::Symbol:
    case VariableLiteralKind::Character:
        return literal.text;
    case VariableLiteralKind::Array:
        return elementsText(literal.elements);
    }
    return {};
}

bool isArrayFieldText(std::string_view text)
{
    return arrayTokens(text).has_value();
}

std::vector<std::string> fieldVariableReferences(std::string_view text)
{
    if (auto whole = wholeVariable(text))
        return {std::move(*whole)};
    std::vector<std::string> names;
    if (!isArrayFieldText(text))
        return names;
    forEachVariable(text, false, [&](std::size_t, std::size_t, std::string_view name) {
        if (std::ranges::find(names, name) == names.end())
            names.emplace_back(name);
    });
    return names;
}

ProjectDocument withVariableLiterals(ProjectDocument document, const VariableLiteralEnvironment& environment)
{
    if (environment.empty())
        return document;
    const auto declarations = document.variableLibrary.variables;
    const auto literalOf = [&](std::string_view name) -> const VariableLiteral* {
        if (std::ranges::any_of(declarations, [&](const ProjectVariableDeclaration& declaration) {
                return declaration.variableId == name;
            }))
            return nullptr;
        const auto given = std::ranges::find_if(environment, [&](const auto& entry) { return entry.first == name; });
        return given == environment.end() ? nullptr : &given->second;
    };
    const auto apply = [&](ProjectGraph& graph) {
        for (auto& node : graph.nodes)
        {
            for (auto& parameter : node.parameters)
            {
                // An expression reads an array as the literal Array it is,
                // as `of:` reads one; its numbers come from
                // the project's variables.
                if (parameter.expression.has_value())
                {
                    parameter.expression = withEachVariableReplaced(*parameter.expression, false,
                        [&](std::string_view name) -> std::optional<std::string> {
                            const auto* literal = literalOf(name);
                            if (literal == nullptr || literal->kind != VariableLiteralKind::Array)
                                return std::nullopt;
                            return variableLiteralText(*literal);
                        });
                }
                // A Script's code reads the literal as Smalltalk writes it,
                // outside its strings and comments.
                if (node.typeId == "talas.script" && parameter.parameterId == "script")
                {
                    if (parameter.text.has_value())
                    {
                        parameter.text = withEachVariableReplaced(*parameter.text, true,
                            [&](std::string_view name) -> std::optional<std::string> {
                                const auto* literal = literalOf(name);
                                if (literal == nullptr || literal->kind == VariableLiteralKind::Number)
                                    return std::nullopt;
                                return variableLiteralText(*literal);
                            });
                    }
                    continue;
                }
                // A file field names the file the string, symbol or
                // character gives.
                if (parameter.resource.has_value())
                {
                    if (const auto name = wholeVariable(*parameter.resource))
                    {
                        const auto* literal = literalOf(*name);
                        if (literal != nullptr && literal->kind != VariableLiteralKind::Number
                            && literal->kind != VariableLiteralKind::Array)
                            parameter.resource = literal->text;
                    }
                }
                if (!parameter.text.has_value() || takesTextAsTyped(document, node, parameter.parameterId))
                    continue;
                if (const auto name = wholeVariable(*parameter.text))
                {
                    if (const auto* literal = literalOf(*name))
                        parameter.text = variableLiteralFieldText(*literal);
                    continue;
                }
                // In an array field a variable standing alone stands for the
                // whole array; {?name} is an array of that one variable.
                const auto tokens = arrayTokens(*parameter.text);
                if (!tokens.has_value())
                    continue;
                std::string written;
                std::size_t copied = 0;
                for (const auto& token : *tokens)
                {
                    if (!token.variable.has_value())
                        continue;
                    const auto* literal = literalOf(*token.variable);
                    if (literal == nullptr || literal->kind != VariableLiteralKind::Array)
                        continue;
                    written.append(*parameter.text, copied, token.start - copied);
                    written += elementsText(literal->elements);
                    copied = token.end;
                }
                if (copied > 0)
                {
                    written.append(*parameter.text, copied);
                    parameter.text = std::move(written);
                }
            }
        }
    };
    apply(document.graph);
    for (auto& definition : document.soundDefinitions)
        apply(definition.graph);
    return document;
}

ProjectDocument withVariableEnvironment(ProjectDocument document, const VariableLiteralEnvironment& environment)
{
    document = withVariableLiterals(std::move(document), environment);
    for (const auto& [name, literal] : environment)
    {
        if (literal.kind != VariableLiteralKind::Number)
            continue;
        if (std::ranges::any_of(document.variableLibrary.variables,
                [&](const ProjectVariableDeclaration& declaration) { return declaration.variableId == name; }))
            continue;
        document.variableLibrary.variables.push_back(
            {name, name, talas::expr::ValueDomain::Scalar, literal.number, {}, false});
    }
    return document;
}
} // namespace talas::project
