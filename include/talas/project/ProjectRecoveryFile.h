#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
enum class ProjectRecoveryErrorCode
{
    NotFound,
    ReadFailed,
    WriteFailed,
    InvalidRecovery,
    Cancelled,
};

struct ProjectRecoveryError
{
    ProjectRecoveryErrorCode code;
    std::string message;
    std::string path;
};

inline constexpr std::size_t maximumProjectRecoveryVersions = 20;
inline constexpr std::uint64_t maximumProjectRecoveryTimestamp =
    9007199254740991ULL;
inline constexpr std::uint64_t
    maximumProjectRecoveryCalendarDisplayTimestamp = 4102444799999ULL;

[[nodiscard]] constexpr bool
projectRecoveryTimestampIsCalendarDisplaySafe(
    std::uint64_t timestamp) noexcept
{
    return timestamp > 0
        && timestamp
            <= maximumProjectRecoveryCalendarDisplayTimestamp;
}

struct ProjectRecoveryVersion
{
    std::uint64_t createdAtUnixMilliseconds = 0;
    ProjectDocument recoveredDocument;
};

struct ProjectRecoveryRecord
{
    std::optional<std::filesystem::path> projectPath;
    std::optional<ProjectFileRevision> projectRevision;
    ProjectDocument cleanDocument;
    std::vector<ProjectRecoveryVersion> versions;
};

struct ProjectRecoverySaveResult
{
    std::optional<ProjectRecoveryError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }

    [[nodiscard]] bool cancelled() const
    {
        return error.has_value()
            && error->code == ProjectRecoveryErrorCode::Cancelled;
    }
};

struct ProjectRecoveryLoadResult
{
    std::optional<ProjectRecoveryRecord> record;
    std::optional<ProjectRecoveryError> error;
    bool usedBackup = false;

    [[nodiscard]] bool ok() const
    {
        return record.has_value() && !error.has_value();
    }

    [[nodiscard]] bool missing() const
    {
        return error.has_value()
            && error->code == ProjectRecoveryErrorCode::NotFound;
    }
};

struct ProjectRecoveryDiscardResult
{
    std::optional<ProjectRecoveryError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

using ProjectRecoveryCancellationCheck = std::function<bool()>;

[[nodiscard]] bool appendProjectRecoveryVersion(
    ProjectRecoveryRecord& record,
    ProjectDocument recoveredDocument,
    std::uint64_t createdAtUnixMilliseconds);
[[nodiscard]] std::optional<std::size_t>
projectRecoveryVersionIndexNewestFirst(
    const ProjectRecoveryRecord& record,
    std::size_t newestFirstPosition) noexcept;
[[nodiscard]] bool makeProjectRecoveryAssetPathsAbsolute(
    ProjectRecoveryRecord& record,
    const std::filesystem::path& sourceProjectPath);
[[nodiscard]] bool projectRecoverySourceMatchesBaseline(
    const ProjectRecoveryRecord& record,
    const ProjectDocument& sourceDocument,
    const ProjectFileRevision& sourceRevision);
[[nodiscard]] bool consumeSuccessfulProjectRecoveryWrite(
    const ProjectRecoverySaveResult& result,
    std::vector<ProjectRecoveryVersion>& publishedVersions,
    std::vector<ProjectRecoveryVersion>& currentVersions) noexcept;
[[nodiscard]] ProjectRecoverySaveResult saveProjectRecoveryFile(
    const std::filesystem::path& path,
    const ProjectRecoveryRecord& record,
    ProjectRecoveryCancellationCheck shouldCancel = {});
[[nodiscard]] ProjectRecoveryLoadResult loadProjectRecoveryFile(
    const std::filesystem::path& path);
[[nodiscard]] ProjectRecoveryDiscardResult discardProjectRecoveryFile(
    const std::filesystem::path& path);
}
