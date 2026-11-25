#include "talas/project/WorkspaceSessionFile.h"

#include "TemporaryFileSafety.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;
[[nodiscard]] WorkspaceSessionFileError makeError(
    WorkspaceSessionFileErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    return {
        code,
        std::move(message),
        pathToUtf8(path),
    };
}

[[nodiscard]] std::filesystem::path siblingWithSuffix(
    const std::filesystem::path& path,
    std::string_view suffix)
{
    auto sibling = path;
    sibling += suffix;
    return sibling;
}

void removeFileSilently(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = detail::symlinkStatus(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found)
        return;
    if (std::filesystem::is_regular_file(status)
        || status.type() == std::filesystem::file_type::symlink)
    {
        std::filesystem::remove(path, error);
    }
}

[[nodiscard]] std::uint64_t stablePathHash(std::string_view path)
{
    auto hash = fnvOffsetBasis;
    for (const auto byte : path)
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnvPrime;
    }
    return hash;
}

[[nodiscard]] std::string hexadecimal(std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex
           << std::nouppercase
           << std::setfill('0')
           << std::setw(16)
           << value;
    return output.str();
}
}

std::optional<std::filesystem::path> workspaceSessionFilePathForProject(
    const std::filesystem::path& localStateRoot,
    const std::filesystem::path& projectPath)
{
    if (localStateRoot.empty() || projectPath.empty())
        return {};

    std::error_code error;
    const auto absolute = projectPath.is_absolute()
        ? projectPath
        : std::filesystem::absolute(projectPath, error);
    if (error || absolute.empty())
        return {};

    const auto normalized = absolute.lexically_normal();
    const auto identity = pathToUtf8(normalized);
    if (identity.empty())
        return {};

    return localStateRoot
        / ("project-" + hexadecimal(stablePathHash(identity))
           + ".talas-workspace-session.json");
}

static WorkspaceSessionFileLoadResult loadWorkspaceSessionAt(
    const std::filesystem::path& path)
{
    WorkspaceSessionFileLoadResult result;
    if (path.empty())
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "WorkspaceSession path cannot be empty",
            path);
        return result;
    }

    std::error_code error;
    const auto status = detail::symlinkStatus(path, error);
    if (error)
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "Unable to inspect WorkspaceSession: " + error.message(),
            path);
        return result;
    }
    if (status.type() == std::filesystem::file_type::not_found)
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::NotFound,
            "WorkspaceSession does not exist",
            path);
        return result;
    }
    if (!std::filesystem::is_regular_file(status))
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "WorkspaceSession path is not a regular file",
            path);
        return result;
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error)
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "Unable to inspect WorkspaceSession size: " + error.message(),
            path);
        return result;
    }
    if (size > maximumWorkspaceSessionFileBytes)
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::InvalidSession,
            "WorkspaceSession exceeds the supported local-state size",
            path);
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "Unable to open WorkspaceSession",
            path);
        return result;
    }
    const std::string source(
        std::istreambuf_iterator<char>{input},
        {});
    if (!input.good() && !input.eof())
    {
        result.error = makeError(
            WorkspaceSessionFileErrorCode::ReadFailed,
            "Unable to read WorkspaceSession",
            path);
        return result;
    }

    auto parsed = parseWorkspaceSessionState(source);
    if (!parsed.ok())
    {
        result.diagnostics = std::move(parsed.errors);
        std::string message = "Invalid WorkspaceSession";
        if (!result.diagnostics.empty())
        {
            message += " at " + result.diagnostics.front().path
                + ": " + result.diagnostics.front().message;
        }
        result.error = makeError(
            WorkspaceSessionFileErrorCode::InvalidSession,
            std::move(message),
            path);
        return result;
    }

    result.state = std::move(parsed.state);
    return result;
}

WorkspaceSessionFileLoadResult loadWorkspaceSessionFile(
    const std::filesystem::path& path)
{
    auto primary = loadWorkspaceSessionAt(path);
    if (!primary.missing() || path.empty())
        return primary;

    const auto backup = siblingWithSuffix(path, ".bak");
    auto recovered = loadWorkspaceSessionAt(backup);
    return recovered.missing()
        ? primary
        : recovered;
}

WorkspaceSessionFileSaveResult saveWorkspaceSessionFile(
    const std::filesystem::path& path,
    const WorkspaceSessionState& state)
{
    if (path.empty())
    {
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "WorkspaceSession path cannot be empty",
            path)};
    }

    const auto serialized = serializeWorkspaceSessionState(state);
    if (serialized.size() > maximumWorkspaceSessionFileBytes)
    {
        return {makeError(
            WorkspaceSessionFileErrorCode::InvalidSession,
            "Refusing to persist WorkspaceSession larger than the supported local-state size",
            path)};
    }
    const auto validated = parseWorkspaceSessionState(serialized);
    if (!validated.ok())
    {
        const auto detail = validated.errors.empty()
            ? std::string{}
            : " at " + validated.errors.front().path
                + ": " + validated.errors.front().message;
        return {makeError(
            WorkspaceSessionFileErrorCode::InvalidSession,
            "Refusing to persist invalid WorkspaceSession" + detail,
            path)};
    }

    std::error_code error;
    const auto targetStatus = detail::symlinkStatus(path, error);
    if (error)
    {
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "Unable to inspect WorkspaceSession destination: "
                + error.message(),
            path)};
    }
    if (targetStatus.type() != std::filesystem::file_type::not_found
        && !std::filesystem::is_regular_file(targetStatus))
    {
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "WorkspaceSession destination is not a regular file",
            path)};
    }
    const auto targetExists = targetStatus.type()
        != std::filesystem::file_type::not_found;

    const auto parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return {makeError(
                WorkspaceSessionFileErrorCode::WriteFailed,
                "Unable to create WorkspaceSession directory: "
                    + error.message(),
                path)};
        }
    }

    const auto temporary = siblingWithSuffix(path, ".tmp");
    const auto backup = siblingWithSuffix(path, ".bak");
    if (const auto removalError = detail::removeTemporaryArtifact(
            temporary,
            "temporary WorkspaceSession");
        removalError.has_value())
    {
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            *removalError,
            temporary)};
    }

    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return {makeError(
                WorkspaceSessionFileErrorCode::WriteFailed,
                "Unable to create temporary WorkspaceSession",
                temporary)};
        }
        output << serialized;
        output.flush();
        if (!output.good())
        {
            output.close();
            removeFileSilently(temporary);
            return {makeError(
                WorkspaceSessionFileErrorCode::WriteFailed,
                "Unable to write temporary WorkspaceSession",
                temporary)};
        }
    }

    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (!error)
    {
        removeFileSilently(backup);
        return {};
    }
    if (!targetExists)
    {
        removeFileSilently(temporary);
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "Unable to replace WorkspaceSession: " + error.message(),
            path)};
    }

    error.clear();
    const auto backupStatus = detail::symlinkStatus(backup, error);
    if (error
        || backupStatus.type()
            != std::filesystem::file_type::not_found)
    {
        removeFileSilently(temporary);
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            error
                ? "Unable to inspect WorkspaceSession backup: "
                    + error.message()
                : "WorkspaceSession backup already exists",
            backup)};
    }

    std::filesystem::rename(path, backup, error);
    if (error)
    {
        removeFileSilently(temporary);
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "Unable to preserve prior WorkspaceSession: "
                + error.message(),
            path)};
    }

    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::string restoreFailure;
        std::error_code restoreError;
        std::filesystem::rename(backup, path, restoreError);
        if (restoreError)
        {
            restoreFailure =
                "; the preserved session remains at "
                + pathToUtf8(backup)
                + " because restoring it failed: "
                + restoreError.message();
        }
        removeFileSilently(temporary);
        return {makeError(
            WorkspaceSessionFileErrorCode::WriteFailed,
            "Unable to replace WorkspaceSession: "
                + error.message()
                + restoreFailure,
            path)};
    }

    removeFileSilently(backup);
    return {};
}
}
