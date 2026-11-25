#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace talas::audio
{
enum class ProjectPackageErrorCode
{
    InvalidInput,
    AssetUnavailable,
    WriteFailed,
    Cancelled,
};

struct ProjectPackageError
{
    ProjectPackageErrorCode code =
        ProjectPackageErrorCode::WriteFailed;
    std::string message;
    std::string path;
};

struct ProjectPackageOptions
{
    std::filesystem::path outputPackagePath;
    std::optional<std::filesystem::path>
        sourceProjectFilePath;
    std::function<bool()> shouldCancel;
    std::function<void(double)> reportProgress;
    std::function<bool()> admitPublication;
};

struct ProjectPackageResult
{
    std::optional<ProjectPackageError> error;
    std::string warning;
    std::string message;
    std::size_t assetCount = 0;
    std::size_t copiedFileCount = 0;
    std::uintmax_t copiedByteCount = 0;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }

    [[nodiscard]] bool cancelled() const
    {
        return error.has_value()
            && error->code
                == ProjectPackageErrorCode::Cancelled;
    }
};

enum class ProjectPackageImportErrorCode
{
    Cancelled,
    CleanupFailed,
};

struct ProjectPackageImportResult
{
    std::string relativePath;
    std::string error;
    bool created = false;
    std::optional<ProjectPackageImportErrorCode> errorCode;
    std::filesystem::path retainedStagingPath;

    [[nodiscard]] bool ok() const
    {
        return error.empty() && !errorCode.has_value();
    }

    [[nodiscard]] bool cancelled() const
    {
        return errorCode
            == ProjectPackageImportErrorCode::Cancelled;
    }
};

[[nodiscard]] ProjectPackageResult saveProjectPackage(
    const talas::project::ProjectDocument& document,
    const ProjectPackageOptions& options);

[[nodiscard]] ProjectPackageImportResult
importProjectPackageAsset(
    const std::filesystem::path& packageDirectory,
    const std::filesystem::path& sourcePath,
    std::string_view preferredName,
    std::function<bool()> shouldCancel = {});

// Retries deletion of staging storage retained after an import cleanup
// failure. Ownership is cleared only after the exact retained path is gone.
[[nodiscard]] std::string discardProjectPackageImportArtifacts(
    ProjectPackageImportResult& result);
}
