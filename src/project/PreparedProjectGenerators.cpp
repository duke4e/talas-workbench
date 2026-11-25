#include "talas/project/PreparedProjectGenerators.h"

#include <ranges>
#include <utility>

namespace talas::project
{
PreparedProjectGeneratorsResult prepareProjectGenerators(
    const ProjectGeneratorLibrary& library,
    std::stop_token stopToken)
{
    PreparedProjectGeneratorsResult result;
    const auto validation = validateProjectGeneratorLibrary(library);
    if (!validation.ok())
    {
        result.diagnostics = validation.diagnostics;
        result.truncated = validation.truncated;
        return result;
    }

    PreparedProjectGeneratorSet candidate;
    candidate.programs.reserve(library.programs.size());
    for (std::size_t index = 0; index < library.programs.size(); ++index)
    {
        if (stopToken.stop_requested())
        {
            result.cancelled = true;
            return result;
        }
        const auto& source = library.programs[index];
        auto prepared = talas::generative::prepareContextFreeGenerator(
            source.source);
        if (!prepared.ok())
        {
            const auto path = "$.generatorLibrary.programs["
                + std::to_string(index) + "].source";
            for (const auto& diagnostic : prepared.diagnostics)
            {
                if (result.diagnostics.size()
                    >= talas::generative::maximumContextFreeDiagnostics)
                {
                    result.truncated = true;
                    return result;
                }
                result.diagnostics.push_back({
                    ProjectGeneratorValidationCode::InvalidProgram,
                    path,
                    diagnostic.range,
                    diagnostic.message,
                });
            }
            if (prepared.truncated)
                result.truncated = true;
            return result;
        }
        auto preparedOutput = prepareProjectGeneratorOutputProfile(
            source.output,
            source.source.terminals,
            prepared.program);
        if (!preparedOutput.ok())
        {
            result.diagnostics.push_back({
                ProjectGeneratorValidationCode::InvalidOutputProfile,
                "$.generatorLibrary.programs["
                    + std::to_string(index) + "].output",
                {},
                preparedOutput.error->message,
            });
            return result;
        }
        candidate.programs.push_back({
            source.generatorId,
            prepared.program.canonicalChecksum,
            std::move(prepared.program),
            std::move(preparedOutput.prepared),
        });
    }
    result.prepared = std::move(candidate);
    return result;
}

const PreparedProjectGenerator* findPreparedProjectGenerator(
    const PreparedProjectGeneratorSet& prepared,
    std::string_view generatorId,
    std::uint64_t canonicalChecksum) noexcept
{
    const auto found = std::ranges::find_if(
        prepared.programs,
        [&](const PreparedProjectGenerator& candidate) {
            return candidate.generatorId == generatorId
                && candidate.canonicalChecksum == canonicalChecksum;
        });
    return found == prepared.programs.end() ? nullptr : &*found;
}
}
