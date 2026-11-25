#include "talas/generative/TalasGrammar.h"

#include <stdexcept>
#include <string>

namespace
{
namespace kg = talas::generative::talas_grammar;
using Kind = kg::Expression::Kind;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// The Concatenation's Inputs in order, as text: s1 s2 ... with `+` groups in
// brackets.
std::string describe(const kg::Expression& expression)
{
    switch (expression.kind)
    {
    case Kind::Terminal:
        return "s" + std::to_string(expression.terminal);
    case Kind::Variable:
        return std::string{expression.variable};
    case Kind::Silence:
        return "-";
    case Kind::Concatenation:
    case Kind::Mix:
    {
        std::string text = expression.kind == Kind::Concatenation ? "(" : "[";
        for (std::size_t index = 0; index < expression.parts.size(); ++index)
        {
            if (index > 0)
                text += expression.kind == Kind::Concatenation ? " , " : " + ";
            text += describe(expression.parts[index]);
        }
        return text + (expression.kind == Kind::Concatenation ? ")" : "]");
    }
    }
    return {};
}

bool isPalindrome(const kg::Expression& expression)
{
    std::string terminals;
    const auto collect = [&](const auto& self, const kg::Expression& part) -> void {
        if (part.kind == Kind::Terminal)
            terminals += static_cast<char>('0' + part.terminal);
        for (const auto& child : part.parts)
            self(self, child);
    };
    collect(collect, expression);
    return std::string(terminals.rbegin(), terminals.rend()) == terminals && !terminals.empty();
}

// The palindrome example always generates palindromes.
void testPalindromeRuleGeneratesPalindromes()
{
    const auto rules = kg::parseRules(
        "A -> ((s1 , A) , (s1)) [2] |\n"
        "((s2 , A) , (s2)) [2] |\n"
        "((s3 , A) , (s3)) [2] |\n"
        "(s1) [1] | (s2) [1] | (s3) [1].");
    require(rules.ok(), "the page's palindrome rule should parse: " + rules.error);
    require(rules.grammar.rules.size() == 1 && rules.grammar.rules.front().options.size() == 6,
        "the palindrome rule should have six options");
    const auto start = kg::parseExpression("(A)");
    require(start.ok(), "(A) should parse");
    for (std::uint64_t seed = 1; seed <= 40; ++seed)
    {
        const auto derived = kg::derive(rules.grammar, start.expression, seed, 1000, 1000);
        require(derived.ok() && isPalindrome(derived.expression),
            "seed " + std::to_string(seed) + " should derive a palindrome, not " + describe(derived.expression));
    }
    const auto first = kg::derive(rules.grammar, start.expression, 34897, 1000, 1000);
    const auto again = kg::derive(rules.grammar, start.expression, 34897, 1000, 1000);
    require(first.expression == again.expression, "a Seed should give repeatable results");
}

void testExpressionsParse()
{
    const auto mixed = kg::parseExpression("(A + (s1 , B))");
    require(mixed.ok() && describe(mixed.expression) == "[A + (s1 , B)]", "(A + (s1 , B)) should parse");
    const auto deep = kg::parseExpression("((E , Z) + ((T , U) , (s3 , (X , s5))))");
    require(deep.ok() && describe(deep.expression) == "[(E , Z) + ((T , U) , (s3 , (X , s5)))]",
        "the page's last StartExpression should parse");
    const auto chain = kg::parseExpression("(s1 , s2 , s3)");
    require(chain.ok() && chain.expression.parts.size() == 3, "a chain of commas should make one Concatenation");
    require(kg::highestTerminal(deep.expression) == 5 && kg::countTerminals(deep.expression) == 2,
        "the expression should name s3 and s5");
    for (const auto* invalid : {"(s1 , )", "(s0)", "(AB)", "(s1 s2)", "(s1", "(s1) junk"})
        require(!kg::parseExpression(invalid).ok(), std::string{"`"} + invalid + "` should be refused");
    require(!kg::parseRules("A -> (s1)").ok(), "an option without a weight should be refused");
    require(!kg::parseRules("A -> (s1) [1]. A -> (s2) [1].").ok(), "two rules for one variable should be refused");
    require(!kg::parseRules("A -> (s1) [0].").ok(), "a rule needs a positive total weight");
    require(kg::parseRules("A -> (B) [1]. B -> (s1 + s2) [1.5]").ok(), "the last rule's period may be left out");
}

void testLimitsSilenceUnrewrittenVariables()
{
    const auto rules = kg::parseRules("A -> (s1 , A) [1].");
    const auto start = kg::parseExpression("(A)");
    const auto bounded = kg::derive(rules.grammar, start.expression, 7, 3, 1000);
    require(bounded.ok() && bounded.rewrites == 3 && kg::countTerminals(bounded.expression) == 3,
        "MaxRewrites should bound the rewriting: " + describe(bounded.expression));
    const auto sized = kg::derive(rules.grammar, start.expression, 7, 1000, 5);
    require(sized.ok() && kg::countTerminals(sized.expression) == 5, "MaxSize should bound the Sounds");
    const auto none = kg::derive(rules.grammar, start.expression, 7, 0, 1000);
    require(none.ok() && kg::countTerminals(none.expression) == 0 && none.expression.kind == Kind::Silence,
        "with no rewrites the variable should become silence");
    require(!kg::derive(rules.grammar, kg::parseExpression("(B)").expression, 7, 10, 10).ok(),
        "a variable without a rule should be refused");
}

void testWeightsChooseOptions()
{
    const auto rules = kg::parseRules("A -> (s1) [3] | (s2) [1] | (s3) [0].");
    const auto start = kg::parseExpression("(A)");
    int ones = 0;
    int threes = 0;
    for (std::uint64_t seed = 0; seed < 4000; ++seed)
    {
        const auto derived = kg::derive(rules.grammar, start.expression, seed, 10, 10);
        ones += derived.expression.terminal == 1;
        threes += derived.expression.terminal == 3;
    }
    require(ones > 2800 && ones < 3200 && threes == 0,
        "options should be chosen by their weights: " + std::to_string(ones) + " of 4000 chose s1");
}
}

int main()
{
    testPalindromeRuleGeneratesPalindromes();
    testExpressionsParse();
    testLimitsSilenceUnrewrittenVariables();
    testWeightsChooseOptions();
    return 0;
}
