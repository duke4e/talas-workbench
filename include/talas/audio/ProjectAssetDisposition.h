#pragma once

#include "talas/audio/ProjectPackager.h"
#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
enum class ProjectAssetDispositionErrorCode
{
    InvalidInput,
    InspectionFailed,
    Cancelled,
    CopyFailed,
    CleanupFailed,
};

struct ProjectAssetDispositionError
{
    ProjectAssetDispositionErrorCode code =
        ProjectAssetDispositionErrorCode::InspectionFailed;
    std::string message;
    std::string path;
};

struct PreparedProjectAssetDisposition
{
    talas::project::ProjectAsset asset;
    std::vector<std::filesystem::path> createdFiles;
    std::vector<ProjectPackageImportResult> cleanupImports;
};

struct ProjectAssetDispositionOptions
{
    std::string assetId;
    talas::project::ProjectAssetDisposition targetDisposition =
        talas::project::ProjectAssetDisposition::Referenced;
    std::optional<std::filesystem::path> selectedPath;
    std::optional<std::filesystem::path> projectFilePath;
    std::function<bool()> shouldCancel;
};

struct ProjectAssetDispositionResult
{
    PreparedProjectAssetDisposition candidate;
    std::optional<ProjectAssetDispositionError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }

    [[nodiscard]] bool cancelled() const
    {
        return error.has_value()
            && error->code
                == ProjectAssetDispositionErrorCode::Cancelled;
    }
};

[[nodiscard]] ProjectAssetDispositionResult
prepareProjectAssetDisposition(
    const talas::project::ProjectDocument& document,
    const ProjectAssetDispositionOptions& options);

[[nodiscard]] std::string
discardPreparedProjectAssetDispositionFiles(
    PreparedProjectAssetDisposition& candidate);
}
