#pragma once

#include "talas/project/WorkspaceSessionState.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumWorkspaceSessionFileBytes =
    1024 * 1024;

enum class WorkspaceSessionFileErrorCode
{
    NotFound,
    ReadFailed,
    WriteFailed,
    InvalidSession,
};

struct WorkspaceSessionFileError
{
    WorkspaceSessionFileErrorCode code;
    std::string message;
    std::string path;
};

struct WorkspaceSessionFileLoadResult
{
    WorkspaceSessionState state;
    std::optional<WorkspaceSessionFileError> error;
    std::vector<WorkspaceSessionDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }

    [[nodiscard]] bool missing() const noexcept
    {
        return error.has_value()
            && error->code == WorkspaceSessionFileErrorCode::NotFound;
    }
};

struct WorkspaceSessionFileSaveResult
{
    std::optional<WorkspaceSessionFileError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] std::optional<std::filesystem::path>
workspaceSessionFilePathForProject(
    const std::filesystem::path& localStateRoot,
    const std::filesystem::path& projectPath);

[[nodiscard]] WorkspaceSessionFileLoadResult loadWorkspaceSessionFile(
    const std::filesystem::path& path);

[[nodiscard]] WorkspaceSessionFileSaveResult saveWorkspaceSessionFile(
    const std::filesystem::path& path,
    const WorkspaceSessionState& state);
}
