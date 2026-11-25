#pragma once

#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
enum class ProjectFileErrorCode
{
    ReadFailed,
    WriteFailed,
    ExternalConflict,
    InvalidProject
};

struct ProjectFileError
{
    ProjectFileErrorCode code;
    std::string message;
    std::string path;
};

struct ProjectFileRevision
{
    bool exists = false;
    std::string contents;

    [[nodiscard]] bool operator==(
        const ProjectFileRevision&) const = default;
};

struct ProjectFileSaveResult
{
    std::optional<ProjectFileError> error;
    std::optional<ProjectFileRevision> revision;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

struct ProjectFileRevisionResult
{
    std::optional<ProjectFileRevision> revision;
    std::optional<ProjectFileError> error;

    [[nodiscard]] bool ok() const
    {
        return revision.has_value()
            && !error.has_value();
    }
};

struct ProjectFileLoadResult
{
    std::optional<ProjectDocument> document;
    std::vector<ProjectFileError> errors;
    std::optional<ProjectFileRevision> revision;

    [[nodiscard]] bool ok() const
    {
        return document.has_value() && errors.empty();
    }
};

[[nodiscard]] ProjectFileRevisionResult
captureProjectFileRevision(
    const std::filesystem::path& path);

[[nodiscard]] ProjectFileSaveResult saveProjectFile(
    const std::filesystem::path& path,
    const ProjectDocument& document,
    const std::optional<ProjectFileRevision>&
        expectedRevision = {});
[[nodiscard]] ProjectFileLoadResult loadProjectFile(const std::filesystem::path& path);
}
