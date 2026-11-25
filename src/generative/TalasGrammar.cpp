#include "talas/generative/TalasGrammar.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>

namespace talas::generative::talas_grammar
{
namespace
{
// An expression this large stops rewriting, whatever MaxSize allows.
constexpr std::size_t maximumExpressionNodes = 100000;

class Parser
{
public:
    explicit Parser(std::string_view text)
        : text_(text)
    {
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

    void skipSpace() noexcept
    {
        while (position_ < text_.size()
            && std::isspace(static_cast<unsigned char>(text_[position_])))
        {
            ++position_;
        }
    }

    [[nodiscard]] bool atEnd() noexcept
    {
        skipSpace();
        return position_ >= text_.size();
    }

    [[nodiscard]] char peek() noexcept
    {
        skipSpace();
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    [[nodiscard]] bool accept(char expected) noexcept
    {
        if (peek() != expected)
            return false;
        ++position_;
        return true;
    }

    [[nodiscard]] bool acceptArrow() noexcept
    {
        skipSpace();
        if (text_.substr(position_, 2) != "->")
            return false;
        position_ += 2;
        return true;
    }

    // A variable not followed by a letter, as in `A ->`.
    [[nodiscard]] bool atRuleStart() noexcept
    {
        skipSpace();
        if (position_ >= text_.size()
            || !std::isupper(static_cast<unsigned char>(text_[position_])))
        {
            return false;
        }
        auto next = position_ + 1;
        while (next < text_.size() && std::isspace(static_cast<unsigned char>(text_[next])))
            ++next;
        return text_.substr(next, 2) == "->";
    }

    [[nodiscard]] std::optional<char> variable() noexcept
    {
        const auto letter = peek();
        if (!std::isupper(static_cast<unsigned char>(letter)))
            return std::nullopt;
        if (position_ + 1 < text_.size()
            && std::isalnum(static_cast<unsigned char>(text_[position_ + 1])))
        {
            return std::nullopt;
        }
        ++position_;
        return letter;
    }

    [[nodiscard]] std::optional<std::size_t> terminal() noexcept
    {
        if (peek() != 's')
            return std::nullopt;
        auto end = position_ + 1;
        while (end < text_.size() && std::isdigit(static_cast<unsigned char>(text_[end])))
            ++end;
        std::size_t number = 0;
        const auto [last, error] = std::from_chars(text_.data() + position_ + 1, text_.data() + end, number);
        if (error != std::errc{} || last != text_.data() + end || number == 0)
            return std::nullopt;
        position_ = end;
        return number;
    }

    [[nodiscard]] std::optional<double> bracketedWeight() noexcept
    {
        if (!accept('['))
            return std::nullopt;
        skipSpace();
        const auto start = position_;
        while (position_ < text_.size() && text_[position_] != ']')
            ++position_;
        if (position_ >= text_.size())
            return std::nullopt;
        auto number = text_.substr(start, position_ - start);
        while (!number.empty() && std::isspace(static_cast<unsigned char>(number.back())))
            number.remove_suffix(1);
        ++position_;
        double weight = 0.0;
        const auto [last, error] = std::from_chars(number.data(), number.data() + number.size(), weight);
        if (number.empty() || error != std::errc{} || last != number.data() + number.size()
            || !std::isfinite(weight) || weight < 0.0)
        {
            return std::nullopt;
        }
        return weight;
    }

    // element := variable | terminal | '(' element ((',' | '+') element)* ')'
    [[nodiscard]] std::optional<Expression> element(std::string& error, std::size_t depth)
    {
        if (depth > maximumNesting)
        {
            error = "ContextFreeGrammar expressions nest too deeply";
            return std::nullopt;
        }
        if (const auto letter = variable(); letter.has_value())
            return Expression{Expression::Kind::Variable, 0, *letter, {}};
        if (const auto number = terminal(); number.has_value())
            return Expression{Expression::Kind::Terminal, *number, 0, {}};
        if (!accept('('))
        {
            error = "ContextFreeGrammar expected a Sound s1 to sn, a variable A to Z or a parenthesized expression at "
                + std::to_string(position());
            return std::nullopt;
        }
        auto current = element(error, depth + 1);
        if (!current.has_value())
            return std::nullopt;
        // Parts joined here, not by a parenthesized part, grow one Sound.
        bool joinedHere = false;
        while (true)
        {
            const auto separator = peek();
            if (separator == ')')
            {
                ++position_;
                return current;
            }
            if (separator != ',' && separator != '+')
            {
                error = "ContextFreeGrammar expected `,`, `+` or `)` at " + std::to_string(position());
                return std::nullopt;
            }
            ++position_;
            const auto kind = separator == ',' ? Expression::Kind::Concatenation : Expression::Kind::Mix;
            auto next = element(error, depth + 1);
            if (!next.has_value())
                return std::nullopt;
            if (joinedHere && current->kind == kind)
            {
                current->parts.push_back(std::move(*next));
            }
            else
            {
                Expression joined{kind, 0, 0, {}};
                joined.parts.push_back(std::move(*current));
                joined.parts.push_back(std::move(*next));
                current = std::move(joined);
                joinedHere = true;
            }
        }
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
};

std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    auto mixed = (state += 0x9e3779b97f4a7c15ULL);
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    return mixed ^ (mixed >> 31U);
}

Expression* leftmostVariable(Expression& expression) noexcept
{
    if (expression.kind == Expression::Kind::Variable)
        return &expression;
    for (auto& part : expression.parts)
    {
        if (auto* found = leftmostVariable(part))
            return found;
    }
    return nullptr;
}

std::size_t countNodes(const Expression& expression) noexcept
{
    std::size_t count = 1;
    for (const auto& part : expression.parts)
        count += countNodes(part);
    return count;
}

void silenceVariables(Expression& expression) noexcept
{
    if (expression.kind == Expression::Kind::Variable)
    {
        expression = Expression{};
        return;
    }
    for (auto& part : expression.parts)
        silenceVariables(part);
}
}

GrammarResult parseRules(std::string_view text)
{
    GrammarResult result;
    if (text.size() > maximumSourceBytes)
    {
        result.error = "ContextFreeGrammar RewriteRules are too long";
        return result;
    }
    Parser parser(text);
    while (!parser.atEnd())
    {
        const auto letter = parser.variable();
        if (!letter.has_value() || !parser.acceptArrow())
        {
            result.error = "ContextFreeGrammar rules should read `A -> (expression) [weight] | ... .` at "
                + std::to_string(parser.position());
            return result;
        }
        if (std::ranges::any_of(result.grammar.rules, [&](const Rule& rule) { return rule.variable == *letter; }))
        {
            result.error = std::string{"ContextFreeGrammar has two rules for "} + *letter;
            return result;
        }
        Rule rule{*letter, {}};
        while (true)
        {
            auto expression = parser.element(result.error, 0);
            if (!expression.has_value())
                return result;
            const auto weight = parser.bracketedWeight();
            if (!weight.has_value())
            {
                result.error = std::string{"ContextFreeGrammar options of "} + *letter
                    + " need a non-negative weight in brackets, as in [2]";
                return result;
            }
            rule.options.push_back({std::move(*expression), *weight});
            if (parser.accept('|'))
                continue;
            if (!parser.accept('.') && !parser.atEnd() && !parser.atRuleStart())
            {
                result.error = "ContextFreeGrammar expected `|` or `.` at " + std::to_string(parser.position());
                return result;
            }
            break;
        }
        double total = 0.0;
        for (const auto& option : rule.options)
            total += option.weight;
        if (!(total > 0.0) || !std::isfinite(total))
        {
            result.error = std::string{"ContextFreeGrammar options of "} + *letter + " need a positive total weight";
            return result;
        }
        result.grammar.rules.push_back(std::move(rule));
    }
    return result;
}

ExpressionResult parseExpression(std::string_view text)
{
    ExpressionResult result;
    if (text.size() > maximumSourceBytes)
    {
        result.error = "ContextFreeGrammar StartExpression is too long";
        return result;
    }
    Parser parser(text);
    if (parser.atEnd())
        return result;
    auto expression = parser.element(result.error, 0);
    if (!expression.has_value())
        return result;
    if (!parser.atEnd())
    {
        result.error = "ContextFreeGrammar StartExpression continues after its expression at "
            + std::to_string(parser.position());
        return result;
    }
    result.expression = std::move(*expression);
    return result;
}

Derivation derive(
    const Grammar& grammar,
    const Expression& start,
    std::uint64_t seed,
    std::size_t maxRewrites,
    std::size_t maxSize)
{
    Derivation result{start, 0, {}};
    auto state = seed;
    while (result.rewrites < maxRewrites
        && countTerminals(result.expression) < maxSize
        && countNodes(result.expression) < maximumExpressionNodes)
    {
        auto* variable = leftmostVariable(result.expression);
        if (variable == nullptr)
            break;
        const auto rule = std::ranges::find(grammar.rules, variable->variable, &Rule::variable);
        if (rule == grammar.rules.end())
        {
            result.error = std::string{"ContextFreeGrammar has no rule for "} + variable->variable;
            return result;
        }
        double total = 0.0;
        for (const auto& option : rule->options)
            total += option.weight;
        const auto target = static_cast<double>(nextRandom(state) >> 11U) * 0x1.0p-53 * total;
        auto chosen = rule->options.size() - 1;
        double cumulative = 0.0;
        for (std::size_t index = 0; index < rule->options.size(); ++index)
        {
            cumulative += rule->options[index].weight;
            if (target < cumulative)
            {
                chosen = index;
                break;
            }
        }
        *variable = rule->options[chosen].expression;
        ++result.rewrites;
    }
    // A variable that has no rule fails even when rewriting stops before it.
    for (auto* variable = leftmostVariable(result.expression); variable != nullptr;
         variable = leftmostVariable(result.expression))
    {
        if (std::ranges::find(grammar.rules, variable->variable, &Rule::variable) == grammar.rules.end())
        {
            result.error = std::string{"ContextFreeGrammar has no rule for "} + variable->variable;
            return result;
        }
        *variable = Expression{};
    }
    silenceVariables(result.expression);
    return result;
}

std::size_t countTerminals(const Expression& expression) noexcept
{
    if (expression.kind == Expression::Kind::Terminal)
        return 1;
    std::size_t count = 0;
    for (const auto& part : expression.parts)
        count += countTerminals(part);
    return count;
}

std::size_t highestTerminal(const Expression& expression) noexcept
{
    auto highest = expression.kind == Expression::Kind::Terminal ? expression.terminal : std::size_t{0};
    for (const auto& part : expression.parts)
        highest = std::max(highest, highestTerminal(part));
    return highest;
}
}
