#include "talas/audio/ProjectAssetDisposition.h"

#include "talas/audio/ProjectPackager.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"

#include <ranges>
#include <utility>

namespace talas::audio
{
namespace
{
[[nodiscard]] bool pathIsInside(
    const std::filesystem::path& path,
    const std::filesystem::path& directory)
{
    const auto relative = path.lexically_relative(directory);
    if (relative.empty())
        return path == directory;
    return *relative.begin() != "..";
}

[[nodiscard]] ProjectAssetDispositionResult failure(
    ProjectAssetDispositionErrorCode code,
    std::string message,
    const std::filesystem::path& path = {})
{
    ProjectAssetDispositionResult result;
    result.error = ProjectAssetDispositionError{
        code,
        std::move(message),
        talas::project::pathToUtf8(path),
    };
    return result;
}

[[nodiscard]] ProjectAssetDispositionResult rollbackFailure(
    PreparedProjectAssetDisposition& candidate,
    ProjectAssetDispositionErrorCode code,
    std::string message,
    const std::filesystem::path& path = {})
{
    const auto cleanup =
        discardPreparedProjectAssetDispositionFiles(candidate);
    if (!cleanup.empty())
    {
        ProjectAssetDispositionResult result;
        result.candidate = std::move(candidate);
        result.error = ProjectAssetDispositionError{
            ProjectAssetDispositionErrorCode::CleanupFailed,
            std::move(message) + "; cleanup failed: " + cleanup,
            talas::project::pathToUtf8(path),
        };
        return result;
    }
    candidate.asset = {};
    return failure(code, std::move(message), path);
}
}

ProjectAssetDispositionResult prepareProjectAssetDisposition(
    const talas::project::ProjectDocument& document,
    const ProjectAssetDispositionOptions& options)
{
    const auto existing = std::ranges::find(
        document.assets,
        options.assetId,
        &talas::project::ProjectAsset::id);
    if (existing == document.assets.end())
    {
        return failure(
            ProjectAssetDispositionErrorCode::InvalidInput,
            "Unknown project asset: " + options.assetId);
    }
    if (options.shouldCancel && options.shouldCancel())
    {
        return failure(
            ProjectAssetDispositionErrorCode::Cancelled,
            "Project asset disposition change cancelled");
    }

    std::optional<std::filesystem::path> effectiveProjectPath =
        options.projectFilePath;
    std::optional<std::filesystem::path> packageDirectory;
    if (options.projectFilePath.has_value())
    {
        const auto resolved = talas::project::resolveProjectPackagePath(
            *options.projectFilePath);
        if (resolved.ok())
        {
            effectiveProjectPath = resolved.projectFilePath;
            packageDirectory = resolved.packageDirectory;
        }
        else if (options.targetDisposition
                 == talas::project::ProjectAssetDisposition::Embedded)
        {
            return failure(
                ProjectAssetDispositionErrorCode::InvalidInput,
                resolved.error,
                *options.projectFilePath);
        }
    }

    std::filesystem::path sourcePath;
    if (options.targetDisposition
        == talas::project::ProjectAssetDisposition::Referenced)
    {
        if (!options.selectedPath.has_value()
            || options.selectedPath->empty())
        {
            return failure(
                ProjectAssetDispositionErrorCode::InvalidInput,
                "Referenced disposition requires an explicit durable path");
        }
        sourcePath = *options.selectedPath;
    }
    else if (options.targetDisposition
             == talas::project::ProjectAssetDisposition::Embedded)
    {
        if (!packageDirectory.has_value())
        {
            return failure(
                ProjectAssetDispositionErrorCode::InvalidInput,
                "Embedded disposition requires an open project package",
                options.projectFilePath.value_or(
                    std::filesystem::path{}));
        }
        sourcePath = options.selectedPath.value_or(
            talas::project::resolveProjectAssetPath(
                *existing,
                effectiveProjectPath));
    }
    else
    {
        return failure(
            ProjectAssetDispositionErrorCode::InvalidInput,
            "Unknown project asset disposition");
    }

    std::error_code canonicalError;
    const auto canonicalSource = std::filesystem::canonical(
        sourcePath,
        canonicalError);
    if (canonicalError)
    {
        return failure(
            ProjectAssetDispositionErrorCode::InspectionFailed,
            "Unable to resolve disposition source: "
                + canonicalError.message(),
            sourcePath);
    }
    if (options.targetDisposition
            == talas::project::ProjectAssetDisposition::Referenced
        && packageDirectory.has_value())
    {
        std::error_code packageError;
        const auto canonicalPackage = std::filesystem::canonical(
            *packageDirectory,
            packageError);
        if (packageError)
        {
            return failure(
                ProjectAssetDispositionErrorCode::InvalidInput,
                "Unable to resolve the project package for a durable reference: "
                    + packageError.message(),
                *packageDirectory);
        }
        if (pathIsInside(canonicalSource, canonicalPackage))
        {
            return failure(
                ProjectAssetDispositionErrorCode::InvalidInput,
                "Referenced disposition requires a durable path outside the project package",
                canonicalSource);
        }
    }
    const auto inspected = talas::project::inspectProjectAsset(
        *existing,
        canonicalSource,
        options.shouldCancel);
    if (!inspected.ok())
    {
        return failure(
            inspected.error->code
                    == talas::project::ProjectAssetInspectionErrorCode::Cancelled
                ? ProjectAssetDispositionErrorCode::Cancelled
                : ProjectAssetDispositionErrorCode::InspectionFailed,
            inspected.error->message,
            canonicalSource);
    }

    PreparedProjectAssetDisposition candidate;
    candidate.asset = inspected.asset;
    if (options.targetDisposition
        == talas::project::ProjectAssetDisposition::Referenced)
    {
        candidate.asset.path = talas::project::projectAssetStoredPath(
            canonicalSource,
            effectiveProjectPath);
        candidate.asset.disposition =
            talas::project::ProjectAssetDisposition::Referenced;
        return {std::move(candidate), std::nullopt};
    }

    auto imported = importProjectPackageAsset(
        *packageDirectory,
        canonicalSource,
        existing->id,
        options.shouldCancel);
    if (!imported.ok())
    {
        const auto importedError = imported.error;
        const auto importedErrorCode = imported.errorCode;
        const auto errorPath = imported.retainedStagingPath.empty()
            ? canonicalSource
            : imported.retainedStagingPath;
        if (!imported.retainedStagingPath.empty())
            candidate.cleanupImports.push_back(std::move(imported));
        return rollbackFailure(
            candidate,
            importedErrorCode
                    == ProjectPackageImportErrorCode::CleanupFailed
                ? ProjectAssetDispositionErrorCode::CleanupFailed
                : importedErrorCode
                          == ProjectPackageImportErrorCode::Cancelled
                    ? ProjectAssetDispositionErrorCode::Cancelled
                    : ProjectAssetDispositionErrorCode::CopyFailed,
            importedError,
            errorPath);
    }
    const auto copiedPath =
        *packageDirectory
        / talas::project::pathFromUtf8(imported.relativePath);
    if (imported.created)
        candidate.createdFiles.push_back(copiedPath);
    candidate.asset.path = imported.relativePath;
    candidate.asset.disposition =
        talas::project::ProjectAssetDisposition::Embedded;
    const auto verified = talas::project::inspectProjectAsset(
        candidate.asset,
        copiedPath,
        options.shouldCancel);
    if (!verified.ok())
    {
        return rollbackFailure(
            candidate,
            verified.error->code
                    == talas::project::ProjectAssetInspectionErrorCode::Cancelled
                ? ProjectAssetDispositionErrorCode::Cancelled
                : ProjectAssetDispositionErrorCode::CopyFailed,
            verified.error->message,
            copiedPath);
    }
    candidate.asset = verified.asset;
    return {std::move(candidate), std::nullopt};
}

std::string discardPreparedProjectAssetDispositionFiles(
    PreparedProjectAssetDisposition& candidate)
{
    std::vector<ProjectPackageImportResult> remainingImports;
    std::string firstError;
    for (auto& imported : candidate.cleanupImports)
    {
        const auto cleanup =
            discardProjectPackageImportArtifacts(imported);
        if (!cleanup.empty())
        {
            if (firstError.empty())
                firstError = cleanup;
            remainingImports.push_back(std::move(imported));
        }
    }
    candidate.cleanupImports = std::move(remainingImports);

    std::vector<std::filesystem::path> remaining;
    for (const auto& path : candidate.createdFiles)
    {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(
            path,
            statusError);
        if (statusError == std::errc::no_such_file_or_directory)
            continue;
        if (!statusError
            && status.type() == std::filesystem::file_type::not_found)
        {
            continue;
        }
        if (statusError
            || !std::filesystem::is_regular_file(status)
            || std::filesystem::is_symlink(status))
        {
            if (firstError.empty())
            {
                firstError = statusError
                    ? statusError.message()
                    : "candidate-created path is not a regular file";
            }
            remaining.push_back(path);
            continue;
        }
        std::error_code removeError;
        if (!std::filesystem::remove(path, removeError)
            || removeError)
        {
            if (firstError.empty())
            {
                firstError = removeError
                    ? removeError.message()
                    : "unable to remove candidate-created file";
            }
            remaining.push_back(path);
        }
    }
    candidate.createdFiles = std::move(remaining);
    return firstError;
}
}
