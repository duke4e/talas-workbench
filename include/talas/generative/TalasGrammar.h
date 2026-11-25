#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talas::generative::talas_grammar
{
// The ContextFreeGrammar: fully
// parenthesized expressions of the Inputs s1 to sn and variables A to Z,
// separated by `,` for a Concatenation or `+` for a CenteringMixer, and
// production rules `A -> (expression) [weight] | ... .` that rewrite them.
inline constexpr std::size_t maximumSourceBytes = 64U * 1024U;
inline constexpr std::size_t maximumNesting = 256;

struct Expression
{
    enum class Kind
    {
        Terminal,
        Variable,
        Concatenation,
        Mix,
        Silence,
    };

    Kind kind = Kind::Silence;
    // One-based Input number for a Terminal, the letter for a Variable.
    std::size_t terminal = 0;
    char variable = 0;
    std::vector<Expression> parts;

    bool operator==(const Expression&) const = default;
};

struct Option
{
    Expression expression;
    double weight = 1.0;
};

struct Rule
{
    char variable = 0;
    std::vector<Option> options;
};

struct Grammar
{
    std::vector<Rule> rules;
};

struct GrammarResult
{
    Grammar grammar;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

struct ExpressionResult
{
    Expression expression;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

[[nodiscard]] GrammarResult parseRules(std::string_view text);
[[nodiscard]] ExpressionResult parseExpression(std::string_view text);

struct Derivation
{
    Expression expression;
    std::size_t rewrites = 0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Rewrites the leftmost variable, choosing each rule's option by its weight
// with a generator the seed starts, until no variable is left, maxRewrites
// rewrites have been made or the expression holds maxSize Inputs. Variables
// left then become silence.
[[nodiscard]] Derivation derive(
    const Grammar& grammar,
    const Expression& start,
    std::uint64_t seed,
    std::size_t maxRewrites,
    std::size_t maxSize);

[[nodiscard]] std::size_t countTerminals(const Expression& expression) noexcept;
[[nodiscard]] std::size_t highestTerminal(const Expression& expression) noexcept;
}
