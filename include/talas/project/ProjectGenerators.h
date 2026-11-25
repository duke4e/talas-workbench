#pragma once

#include "talas/generative/ContextFreeGenerator.h"
#include "talas/project/ProjectGeneratorOutputs.h"

#include <cstddef>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr int projectGeneratorLibraryVersion = 2;

struct ProjectGeneratorProgram
{
    std::string generatorId;
    std::string displayName;
    talas::generative::ContextFreeGeneratorSource source;
    ProjectGeneratorOutputProfile output;

    bool operator==(const ProjectGeneratorProgram&) const = default;
};

struct ProjectGeneratorLibrary
{
    int version = projectGeneratorLibraryVersion;
    std::vector<ProjectGeneratorProgram> programs;

    bool operator==(const ProjectGeneratorLibrary&) const = default;
};

enum class ProjectGeneratorValidationCode
{
    InvalidVersion,
    TooManyPrograms,
    TooMuchSource,
    InvalidIdentifier,
    DuplicateProgram,
    InvalidDisplayName,
    InvalidProgram,
    InvalidOutputProfile,
    DiagnosticLimitExceeded
};

struct ProjectGeneratorDiagnostic
{
    ProjectGeneratorValidationCode code =
        ProjectGeneratorValidationCode::InvalidProgram;
    std::string path;
    talas::generative::ContextFreeSourceRange sourceRange;
    std::string message;
};

struct ProjectGeneratorValidationResult
{
    std::vector<ProjectGeneratorDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] ProjectGeneratorValidationResult
validateProjectGeneratorLibrary(const ProjectGeneratorLibrary& library);
}
