#include "talas/app/VariableValuePrompt.h"

#include "talas/app/ParameterFieldEditors.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectVariables.h"

#include <algorithm>
#include <cctype>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] const talas::project::VariableLiteral* givenValue(
    const talas::project::VariableLiteralEnvironment& environment, std::string_view name)
{
    const auto found = std::ranges::find_if(environment, [&](const auto& entry) { return entry.first == name; });
    return found == environment.end() ? nullptr : &found->second;
}
} // namespace

VariableDialogAnswer variableDialogAnswer(std::string_view name, std::string_view typed, VariableDialogPress press)
{
    if (press == VariableDialogPress::Cancel)
        return {};
    if (auto value = talas::project::parseVariableLiteral(typed))
        return {std::move(*value), {}};
    return {std::nullopt,
        "?" + std::string{name} + " needs a number, a string ('text'), a symbol (#name), a character ($c) or an array (#(1 2 3))"};
}

talas::project::VariableLiteralEnvironment withVariableValuesGiven(
    talas::project::VariableLiteralEnvironment environment, const talas::project::VariableLiteralEnvironment& given)
{
    for (const auto& [name, value] : given)
    {
        const auto found = std::ranges::find_if(environment, [&](const auto& entry) { return entry.first == name; });
        if (found == environment.end())
            environment.emplace_back(name, value);
        else
            found->second = value;
    }
    return environment;
}

std::vector<std::string> variableEnvironmentLines(const talas::project::VariableLiteralEnvironment& environment)
{
    std::vector<std::string> lines;
    for (const auto& [name, value] : environment)
        lines.push_back("?" + name + " -> " + talas::project::variableLiteralText(value));
    return lines;
}

bool isArrayParameter(const talas::core::ParameterDescriptor& parameter)
{
    if (parameter.kind != talas::core::ParameterKind::Text)
        return false;
    const auto prototype = trimmed(parameter.defaultText);
    return parseArrayField(prototype).has_value()
        || (prototype.starts_with("#(") && talas::project::parseVariableLiteral(prototype).has_value());
}

std::optional<std::string> arrayParameterVariableToAsk(const talas::core::ParameterDescriptor& parameter, std::string_view typed)
{
    if (!isArrayParameter(parameter))
        return std::nullopt;
    const auto text = trimmed(typed);
    auto names = talas::project::fieldVariableReferences(text);
    if (names.size() != 1 || text != "?" + names.front())
        return std::nullopt;
    return std::move(names.front());
}

ArrayVariableQuestion arrayVariableQuestion(std::string_view name)
{
    const auto variable = "?" + std::string{name};
    return {"Is " + variable + " one element of an array, or the whole array?",
        "An array holding " + variable, variable + " is the whole array"};
}

std::string arrayParameterVariableText(std::string_view name, ArrayVariableMeaning meaning)
{
    const auto variable = "?" + std::string{name};
    return meaning == ArrayVariableMeaning::ArrayOfTheVariable ? "{" + variable + "}" : variable;
}

VariableEnvironmentUse variableEnvironmentUse(TalasMenuOperation operation) noexcept
{
    switch (operation)
    {
    // The operations that compile, play, record or make a class of the
    // Sound.
    case TalasMenuOperation::CompileLoadStart:
    case TalasMenuOperation::PlayMarked:
    case TalasMenuOperation::PlayAll:
    case TalasMenuOperation::CompileLoad:
    case TalasMenuOperation::CompileToDisk:
    case TalasMenuOperation::RecordToDisk:
    case TalasMenuOperation::NewClassFromExample:
    case TalasMenuOperation::FullWaveform:
    case TalasMenuOperation::Oscilloscope:
    case TalasMenuOperation::SpectrumAnalyzer:
    // Get Info gives each variable the environment's value.
    case TalasMenuOperation::GetInfo:
        return VariableEnvironmentUse::Within;
    case TalasMenuOperation::Environment:
        return VariableEnvironmentUse::Shows;
    case TalasMenuOperation::ResetEnvironment:
        return VariableEnvironmentUse::Resets;
    default:
        return VariableEnvironmentUse::Outside;
    }
}

std::vector<std::string> variablesToAskBefore(TalasMenuOperation operation, const talas::project::ProjectDocument& document,
    const talas::project::VariableLiteralEnvironment& environment)
{
    // Get Info shows what the environment has without asking.
    if (variableEnvironmentUse(operation) != VariableEnvironmentUse::Within || operation == TalasMenuOperation::GetInfo)
        return {};
    auto names = talas::project::undeclaredProjectVariables(document);
    std::erase_if(names, [&](const std::string& name) { return givenValue(environment, name) != nullptr; });
    return names;
}

std::vector<std::string> variableInformationLines(
    const std::vector<std::string>& variables, const talas::project::VariableLiteralEnvironment& environment)
{
    std::vector<std::string> lines;
    for (const auto& name : variables)
    {
        const auto* value = givenValue(environment, name);
        lines.push_back("?" + name
            + (value == nullptr ? std::string{": asked for when played"} : " = " + talas::project::variableLiteralText(*value)));
    }
    return lines;
}
} // namespace talas::app
