#pragma once

#include "talas/generative/ContextFreeGenerator.h"
#include "talas/project/ProjectGenerators.h"

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct PreparedProjectGenerator
{
    std::string generatorId;
    std::uint64_t canonicalChecksum = 0;
    talas::generative::PreparedContextFreeGenerator program;
    PreparedProjectGeneratorOutputProfile output;
};

struct PreparedProjectGeneratorSet
{
    std::vector<PreparedProjectGenerator> programs;
};

struct PreparedProjectGeneratorsResult
{
    PreparedProjectGeneratorSet prepared;
    std::vector<ProjectGeneratorDiagnostic> diagnostics;
    bool truncated = false;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated && !cancelled;
    }
};

[[nodiscard]] PreparedProjectGeneratorsResult prepareProjectGenerators(
    const ProjectGeneratorLibrary& library,
    std::stop_token stopToken = {});

[[nodiscard]] const PreparedProjectGenerator* findPreparedProjectGenerator(
    const PreparedProjectGeneratorSet& prepared,
    std::string_view generatorId,
    std::uint64_t canonicalChecksum) noexcept;
}
