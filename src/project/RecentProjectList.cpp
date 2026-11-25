#include "talas/project/RecentProjectList.h"

#include "JsonValue.h"
#include "TemporaryFileSafety.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::string_view stateFormat = "talas.recent-projects";
constexpr double stateVersion = 1.0;

[[nodiscard]] std::optional<std::filesystem::path> normalizedAbsolutePath(
    const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    std::error_code error;
    const auto absolute = path.is_absolute()
        ? path
        : std::filesystem::absolute(path, error);
    if (error || absolute.empty())
        return {};
    return absolute.lexically_normal();
}

[[nodiscard]] RecentProjectListError makeError(
    RecentProjectListErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    return {code, std::move(message), pathToUtf8(path)};
}

[[nodiscard]] std::filesystem::path siblingWithSuffix(
    const std::filesystem::path& path,
    std::string_view suffix)
{
    auto sibling = path;
    sibling += suffix;
    return sibling;
}

[[nodiscard]] std::filesystem::file_status fileSymlinkStatus(
    const std::filesystem::path& path,
    std::error_code& error)
{
    auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        error.clear();
        status = std::filesystem::file_status{
            std::filesystem::file_type::not_found};
    }
    return status;
}

void removeFileSilently(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = fileSymlinkStatus(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found)
        return;
    if (std::filesystem::is_regular_file(status)
        || status.type() == std::filesystem::file_type::symlink)
    {
        std::filesystem::remove(path, error);
    }
}

[[nodiscard]] std::string serializeState(const RecentProjectList& list)
{
    std::string output;
    output.reserve(128 + list.entries().size() * 80);
    output += "{\n  \"format\": ";
    json::appendEscapedString(output, stateFormat);
    output += ",\n  \"version\": 1,\n  \"projects\": [";
    for (std::size_t index = 0; index < list.entries().size(); ++index)
    {
        output += index == 0 ? "\n    " : ",\n    ";
        json::appendEscapedString(
            output,
            pathToUtf8(list.entries()[index]));
    }
    if (!list.entries().empty())
        output += "\n  ";
    output += "]\n}\n";
    return output;
}

[[nodiscard]] RecentProjectListLoadResult parseState(
    std::string_view text,
    const std::filesystem::path& path,
    std::size_t capacity,
    bool usedBackup)
{
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    auto parsed = json::parse(text, limits);
    if (!parsed.ok() || !parsed.value.isObject())
    {
        const auto message = parsed.errors.empty()
            ? "Recent-project state root must be an object"
            : "Invalid recent-project JSON: "
                + parsed.errors.front().message;
        return {{}, makeError(
                        RecentProjectListErrorCode::InvalidState,
                        message,
                        path),
                usedBackup};
    }

    const auto* format = json::findObjectMember(parsed.value, "format");
    const auto* version = json::findObjectMember(parsed.value, "version");
    const auto* projects = json::findObjectMember(parsed.value, "projects");
    if (format == nullptr || !format->isString()
        || format->string() != stateFormat)
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::UnsupportedState,
                        "Unsupported recent-project state format",
                        path),
                usedBackup};
    }
    if (version == nullptr || !version->isNumber()
        || !std::isfinite(version->number())
        || version->number() != stateVersion)
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::UnsupportedState,
                        "Unsupported recent-project state version",
                        path),
                usedBackup};
    }
    if (projects == nullptr || !projects->isArray())
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::InvalidState,
                        "Recent-project projects must be an array",
                        path),
                usedBackup};
    }

    std::vector<std::filesystem::path> persisted;
    persisted.reserve(projects->array().size());
    for (const auto& value : projects->array())
    {
        if (!value.isString() || value.string().empty())
        {
            return {{}, makeError(
                            RecentProjectListErrorCode::InvalidState,
                            "Recent-project entries must be non-empty strings",
                            path),
                    usedBackup};
        }
        std::filesystem::path entry;
        try
        {
            entry = pathFromUtf8(value.string());
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return {{}, makeError(
                            RecentProjectListErrorCode::InvalidState,
                            "Recent-project entry is not valid UTF-8",
                            path),
                    usedBackup};
        }
        if (!entry.is_absolute())
        {
            return {{}, makeError(
                            RecentProjectListErrorCode::InvalidState,
                            "Recent-project entries must be absolute paths",
                            path),
                    usedBackup};
        }
        persisted.push_back(entry);
    }

    RecentProjectList list(capacity);
    for (auto entry = persisted.rbegin(); entry != persisted.rend(); ++entry)
        static_cast<void>(list.note(*entry));
    return {std::move(list), {}, usedBackup};
}

[[nodiscard]] RecentProjectListLoadResult loadStateAt(
    const std::filesystem::path& path,
    std::size_t capacity,
    bool usedBackup)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::ReadFailed,
                        "Unable to open recent-project state",
                        path),
                usedBackup};
    }
    const std::string text(std::istreambuf_iterator<char>{input}, {});
    if (!input.good() && !input.eof())
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::ReadFailed,
                        "Unable to read recent-project state",
                        path),
                usedBackup};
    }
    return parseState(text, path, capacity, usedBackup);
}

[[nodiscard]] std::optional<RecentProjectListError> inspectReadableFile(
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = fileSymlinkStatus(path, error);
    if (error)
    {
        return makeError(
            RecentProjectListErrorCode::ReadFailed,
            "Unable to inspect recent-project state: " + error.message(),
            path);
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return {};
    if (!std::filesystem::is_regular_file(status))
    {
        return makeError(
            RecentProjectListErrorCode::ReadFailed,
            "Recent-project state is not a regular file",
            path);
    }
    return {};
}
}

RecentProjectList::RecentProjectList(std::size_t capacity)
    : capacity_(capacity)
{
    entries_.reserve(capacity_);
}

const std::vector<std::filesystem::path>& RecentProjectList::entries() const noexcept
{
    return entries_;
}

std::size_t RecentProjectList::capacity() const noexcept
{
    return capacity_;
}

bool RecentProjectList::note(const std::filesystem::path& projectPath)
{
    const auto normalized = normalizedAbsolutePath(projectPath);
    if (!normalized.has_value() || capacity_ == 0)
        return false;
    const auto existing = std::ranges::find(entries_, *normalized);
    if (existing != entries_.end() && existing == entries_.begin())
        return false;
    if (existing != entries_.end())
        entries_.erase(existing);
    entries_.insert(entries_.begin(), *normalized);
    if (entries_.size() > capacity_)
        entries_.resize(capacity_);
    return true;
}

bool RecentProjectList::remove(const std::filesystem::path& projectPath)
{
    const auto normalized = normalizedAbsolutePath(projectPath);
    if (!normalized.has_value())
        return false;
    const auto existing = std::ranges::find(entries_, *normalized);
    if (existing == entries_.end())
        return false;
    entries_.erase(existing);
    return true;
}

RecentProjectListSaveResult saveRecentProjectList(
    const std::filesystem::path& path,
    const RecentProjectList& list)
{
    if (path.empty())
    {
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            "Recent-project state path cannot be empty",
            path)};
    }

    std::error_code error;
    const auto targetStatus = fileSymlinkStatus(path, error);
    if (error)
    {
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            "Unable to inspect recent-project destination: " + error.message(),
            path)};
    }
    const auto targetExists =
        targetStatus.type() != std::filesystem::file_type::not_found;
    if (targetExists && !std::filesystem::is_regular_file(targetStatus))
    {
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            "Recent-project destination is not a regular file",
            path)};
    }

    const auto parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return {makeError(
                RecentProjectListErrorCode::WriteFailed,
                "Unable to create recent-project directory: " + error.message(),
                path)};
        }
    }

    const auto temporary = siblingWithSuffix(path, ".tmp");
    const auto backup = siblingWithSuffix(path, ".bak");
    if (auto removalError = detail::removeTemporaryArtifact(
            temporary,
            "temporary recent-project state");
        removalError.has_value())
    {
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            std::move(*removalError),
            temporary)};
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return {makeError(
                RecentProjectListErrorCode::WriteFailed,
                "Unable to create temporary recent-project state",
                temporary)};
        }
        output << serializeState(list);
        output.flush();
        if (!output.good())
        {
            output.close();
            removeFileSilently(temporary);
            return {makeError(
                RecentProjectListErrorCode::WriteFailed,
                "Unable to write temporary recent-project state",
                temporary)};
        }
    }

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
            RecentProjectListErrorCode::WriteFailed,
            "Unable to publish recent-project state: " + error.message(),
            path)};
    }

    error.clear();
    const auto backupStatus = fileSymlinkStatus(backup, error);
    if (error
        || backupStatus.type() != std::filesystem::file_type::not_found)
    {
        removeFileSilently(temporary);
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            error
                ? "Unable to inspect recent-project backup: " + error.message()
                : "Recent-project backup already exists",
            backup)};
    }

    std::filesystem::rename(path, backup, error);
    if (error)
    {
        removeFileSilently(temporary);
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            "Unable to preserve prior recent-project state: " + error.message(),
            path)};
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::error_code restoreError;
        std::filesystem::rename(backup, path, restoreError);
        removeFileSilently(temporary);
        return {makeError(
            RecentProjectListErrorCode::WriteFailed,
            "Unable to replace recent-project state: " + error.message(),
            path)};
    }
    removeFileSilently(backup);
    return {};
}

RecentProjectListLoadResult loadRecentProjectList(
    const std::filesystem::path& path,
    std::size_t capacity)
{
    if (path.empty())
    {
        return {{}, makeError(
                        RecentProjectListErrorCode::NotFound,
                        "Recent-project state path is empty",
                        path),
                false};
    }

    if (auto inspection = inspectReadableFile(path); inspection.has_value())
        return {{}, std::move(inspection), false};
    std::error_code error;
    const auto primaryStatus = fileSymlinkStatus(path, error);
    if (primaryStatus.type() != std::filesystem::file_type::not_found)
    {
        auto primary = loadStateAt(path, capacity, false);
        if (primary.ok())
            return primary;
        if (primary.error.has_value()
            && primary.error->code
                == RecentProjectListErrorCode::UnsupportedState)
        {
            return primary;
        }

        const auto backup = siblingWithSuffix(path, ".bak");
        if (!inspectReadableFile(backup).has_value())
        {
            const auto backupStatus = fileSymlinkStatus(backup, error);
            if (backupStatus.type()
                != std::filesystem::file_type::not_found)
            {
                auto recovered = loadStateAt(backup, capacity, true);
                if (recovered.ok())
                    return recovered;
            }
        }
        return primary;
    }

    const auto backup = siblingWithSuffix(path, ".bak");
    if (auto inspection = inspectReadableFile(backup); inspection.has_value())
        return {{}, std::move(inspection), true};
    const auto backupStatus = fileSymlinkStatus(backup, error);
    if (backupStatus.type() != std::filesystem::file_type::not_found)
        return loadStateAt(backup, capacity, true);

    return {{}, makeError(
                    RecentProjectListErrorCode::NotFound,
                    "No recent-project state exists",
                    path),
            false};
}
}
