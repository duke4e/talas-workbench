#pragma once

#include "talas/app/TalasKeyEquivalents.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/VariableLiterals.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
}

// The dialog that asks a lifted Sound's variables for their values, and
// the environment that keeps them.
namespace talas::app
{
// How the dialog was answered: the Value button, Enter (the same), or
// Cancel.
enum class VariableDialogPress : std::uint8_t
{
    ValueButton,
    Enter,
    Cancel,
};

// A value typed and taken with Value or
// Enter is the literal it is, a number, string, symbol, character or array
// (`talas::project::parseVariableLiteral`); other text is refused with the
// reason, the dialog staying open. Cancel gives nothing and refuses
// nothing.
struct VariableDialogAnswer
{
    std::optional<talas::project::VariableLiteral> value;
    std::string refusal;

    bool operator==(const VariableDialogAnswer&) const = default;
};
[[nodiscard]] VariableDialogAnswer variableDialogAnswer(
    std::string_view name, std::string_view typed, VariableDialogPress press);

// The environment with the values given, each variable given again taking
// its new value in its old place.
[[nodiscard]] talas::project::VariableLiteralEnvironment withVariableValuesGiven(
    talas::project::VariableLiteralEnvironment environment, const talas::project::VariableLiteralEnvironment& given);

// The environment as Info > Environment shows it, one ?name -> literal
// line each.
[[nodiscard]] std::vector<std::string> variableEnvironmentLines(
    const talas::project::VariableLiteralEnvironment& environment);

// An array parameter is a text field whose prototype value
// is an array (numbers one after another, or a Smalltalk #( ) array).
[[nodiscard]] bool isArrayParameter(const talas::core::ParameterDescriptor& parameter);

// Setting an array parameter to a variable asks what the variable is: an
// array holding that one variable ({?name}), or the variable standing for
// the whole array (?name).
enum class ArrayVariableMeaning : std::uint8_t
{
    ArrayOfTheVariable,
    VariableIsTheArray,
};

// The variable to ask about when `typed`, entered into the parameter, is
// one ?variable alone and the parameter is an array parameter; nothing
// otherwise, the text then taken as typed.
[[nodiscard]] std::optional<std::string> arrayParameterVariableToAsk(
    const talas::core::ParameterDescriptor& parameter, std::string_view typed);

struct ArrayVariableQuestion
{
    std::string message;
    std::string arrayOfTheVariable;
    std::string variableIsTheArray;

    bool operator==(const ArrayVariableQuestion&) const = default;
};
[[nodiscard]] ArrayVariableQuestion arrayVariableQuestion(std::string_view name);

// The field's text for the answer.
[[nodiscard]] std::string arrayParameterVariableText(std::string_view name, ArrayVariableMeaning meaning);

// Most of the Action and Info menus' operations take place
// within the environment. Those that compile, play, record or make a class
// of the Sound ask for the values the environment lacks and use its values,
// Get Info shows each variable's value from it, Environment shows it and
// Reset environment clears it; the rest work on the Sound as written.
enum class VariableEnvironmentUse : std::uint8_t
{
    Outside,
    Within,
    Shows,
    Resets,
};
[[nodiscard]] VariableEnvironmentUse variableEnvironmentUse(TalasMenuOperation operation) noexcept;

// The variables an operation asks values for before it runs: for one
// within the environment, those the document's fields read that neither a
// declaration nor the environment gives; none for the others.
[[nodiscard]] std::vector<std::string> variablesToAskBefore(TalasMenuOperation operation,
    const talas::project::ProjectDocument& document, const talas::project::VariableLiteralEnvironment& environment);

// Get Info within the environment: each of the Sound's variables with the
// value the environment gives it, or that it is asked for when played.
[[nodiscard]] std::vector<std::string> variableInformationLines(
    const std::vector<std::string>& variables, const talas::project::VariableLiteralEnvironment& environment);
} // namespace talas::app
