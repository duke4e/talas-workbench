#include "talas/project/ProjectGenerators.h"

#include "JsonValue.h"

#include <limits>
#include <ranges>
#include <unordered_set>

namespace talas::project
{
namespace
{
void appendDiagnostic(
    ProjectGeneratorValidationResult& result,
    ProjectGeneratorValidationCode code,
    std::string path,
    talas::generative::ContextFreeSourceRange range,
    std::string message)
{
    if (result.truncated)
        return;
    if (result.diagnostics.size()
        >= talas::generative::maximumContextFreeDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back(
        {code, std::move(path), range, std::move(message)});
}

[[nodiscard]] bool containsControlCharacter(
    std::string_view value) noexcept
{
    return std::ranges::any_of(value, [](unsigned char character) {
        return character < 0x20U || character == 0x7fU;
    });
}
}

ProjectGeneratorValidationResult validateProjectGeneratorLibrary(
    const ProjectGeneratorLibrary& library)
{
    ProjectGeneratorValidationResult result;
    if (library.version != projectGeneratorLibraryVersion)
    {
        appendDiagnostic(
            result,
            ProjectGeneratorValidationCode::InvalidVersion,
            "$.generatorLibrary.version",
            {},
            "Project generator library version must be 1");
    }
    if (library.programs.size()
        > talas::generative::maximumContextFreePrograms)
    {
        appendDiagnostic(
            result,
            ProjectGeneratorValidationCode::TooManyPrograms,
            "$.generatorLibrary.programs",
            {},
            "Project generator count exceeds its fixed limit");
        return result;
    }

    std::size_t aggregateSourceBytes = 0;
    std::unordered_set<std::string_view> identities;
    identities.reserve(library.programs.size());
    for (std::size_t index = 0; index < library.programs.size(); ++index)
    {
        const auto& program = library.programs[index];
        const auto path = "$.generatorLibrary.programs["
            + std::to_string(index) + "]";
        if (!talas::generative::isValidContextFreeIdentifier(
                program.generatorId))
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::InvalidIdentifier,
                path + ".generatorId",
                {},
                "Generator ID must use the bounded ASCII identifier grammar");
        }
        else if (!identities.insert(program.generatorId).second)
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::DuplicateProgram,
                path + ".generatorId",
                {},
                "Generator IDs must be unique");
        }
        if (program.displayName.empty()
            || program.displayName.size()
                > talas::generative::maximumContextFreeDisplayNameBytes
            || !json::isValidUtf8(program.displayName)
            || containsControlCharacter(program.displayName))
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::InvalidDisplayName,
                path + ".displayName",
                {},
                "Generator display name must be bounded non-empty UTF-8 text");
        }
        std::size_t programSourceBytes = program.source.grammar.size();
        for (std::size_t terminalIndex = 0;
             terminalIndex < program.source.terminals.size();
            ++terminalIndex)
        {
            const auto separatorBytes = terminalIndex == 0U ? 0U : 1U;
            const auto maximum = std::numeric_limits<std::size_t>::max();
            if (programSourceBytes > maximum - separatorBytes
                || program.source.terminals[terminalIndex].size()
                    > maximum - programSourceBytes - separatorBytes)
            {
                programSourceBytes =
                    std::numeric_limits<std::size_t>::max();
                break;
            }
            programSourceBytes += separatorBytes
                + program.source.terminals[terminalIndex].size();
        }
        if (programSourceBytes
            > talas::generative::maximumContextFreeProjectSourceBytes
                - aggregateSourceBytes)
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::TooMuchSource,
                path + ".source",
                {},
                "Aggregate project generator source exceeds 1 MiB");
            return result;
        }
        aggregateSourceBytes += programSourceBytes;

        const auto parsed = talas::generative::parseContextFreeGenerator(
            program.source);
        for (const auto& diagnostic : parsed.diagnostics)
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::InvalidProgram,
                path + ".source",
                diagnostic.range,
                diagnostic.message);
        }
        if (parsed.truncated)
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::DiagnosticLimitExceeded,
                path + ".source",
                {},
                "Generator diagnostics exceed their fixed limit");
        }
        if (const auto output = projectGeneratorOutputValidationError(
                program.output,
                program.source.terminals))
        {
            appendDiagnostic(
                result,
                ProjectGeneratorValidationCode::InvalidOutputProfile,
                path + ".output.bindings["
                    + std::to_string(output->bindingIndex)
                    + "].actions["
                    + std::to_string(output->actionIndex) + "]",
                {},
                output->message);
        }
    }
    return result;
}
}
