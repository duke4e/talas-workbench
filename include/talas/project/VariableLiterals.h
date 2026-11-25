#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The values a lifted
// Sound's variables are given. A ?variable may hold more than a number: the
// dialog that asks for it takes a number, a string, a symbol, a character or
// an array typed as Smalltalk writes them, and each field reads the variable
// as it would read that literal typed into it.
namespace talas::project
{
struct ProjectDocument;

enum class VariableLiteralKind : std::uint8_t
{
    Number,
    String,
    Symbol,
    Character,
    Array,
};

struct VariableLiteral
{
    VariableLiteralKind kind = VariableLiteralKind::Number;
    double number = 0.0;
    // A string's characters, a symbol's name or the one character.
    std::string text;
    // An array's numbers.
    std::vector<double> elements;

    bool operator==(const VariableLiteral&) const = default;
};

[[nodiscard]] VariableLiteral numberLiteral(double value);

// What a value typed into the variable dialog is: a number (0.25, -3,
// 1e3), a string ('piano', a quote doubled inside it), a symbol (#Square21
// or #'two words'), a character ($a) or an array of numbers (#(1 0 1), or
// {1. 0. 1} as a brace array); nothing for any other text.
[[nodiscard]] std::optional<VariableLiteral> parseVariableLiteral(std::string_view typed);

// The literal as Smalltalk prints it, as the Environment shows it: 0.25,
// 'piano', #Square21, $a, #(1 0 1).
[[nodiscard]] std::string variableLiteralText(const VariableLiteral& literal);

// The literal as a field holding it reads, typed in place of the variable:
// the number, the string's characters, the symbol's name, the character,
// the array's numbers one after another.
[[nodiscard]] std::string variableLiteralFieldText(const VariableLiteral& literal);

// The values given to variables, by name, in the order they were given.
using VariableLiteralEnvironment = std::vector<std::pair<std::string, VariableLiteral>>;

// The document with its fields reading the environment's variables as they
// read the literals they hold: a text field that is wholly ?name the
// literal's field text (a string in a text field, an array in an array
// field); a file field wholly ?name the string, symbol or character naming
// the file; and in an array field (numbers, braced elements and variables
// one after another) each ?name standing alone the array's numbers in its
// place, the variable standing for the whole array, where {?name} is an
// array holding that one variable. Numbers in expression
// fields, a Script's code and the variables a declaration names are left to
// the project's variables, as before.
[[nodiscard]] ProjectDocument withVariableLiterals(
    ProjectDocument document, const VariableLiteralEnvironment& environment);

// The document taking place within the environment: its
// fields reading the environment's literals, and each number the
// environment gives a variable no declaration names declared as a project
// variable of that value, so that nothing is left to ask for.
[[nodiscard]] ProjectDocument withVariableEnvironment(
    ProjectDocument document, const VariableLiteralEnvironment& environment);

// Whether a text field reads as an array: numbers (with their units),
// braced elements and ?variables one after another, at least one of them.
[[nodiscard]] bool isArrayFieldText(std::string_view text);

// The variables a text or file field reads whole, or an array field reads
// as elements standing alone (?name, not {?name}), in the order they appear.
[[nodiscard]] std::vector<std::string> fieldVariableReferences(std::string_view text);
} // namespace talas::project
