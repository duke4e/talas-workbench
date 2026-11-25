#include "talas/project/ProjectPackage.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>

namespace talas::project
{
namespace
{
[[nodiscard]] std::string lowerExtension(
    const std::filesystem::path& path)
{
    auto extension = pathToUtf8(path.extension());
    std::ranges::transform(
        extension,
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension;
}

[[nodiscard]] bool isPackageSuffix(
    const std::filesystem::path& path)
{
    return lowerExtension(path) == projectPackageExtension;
}

[[nodiscard]] bool isMissing(
    const std::filesystem::file_status& status) noexcept
{
    return status.type() == std::filesystem::file_type::not_found;
}

[[nodiscard]] std::filesystem::file_status safeSymlinkStatus(
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

[[nodiscard]] bool isPathWithin(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    const auto normalizedChild = child.lexically_normal();
    const auto normalizedParent = parent.lexically_normal();
    auto childIterator = normalizedChild.begin();
    for (auto parentIterator = normalizedParent.begin();
         parentIterator != normalizedParent.end();
         ++parentIterator, ++childIterator)
    {
        if (childIterator == normalizedChild.end()
            || *childIterator != *parentIterator)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ProjectPackagePathResult invalidPackage(
    const std::filesystem::path& inputPath,
    std::string message)
{
    ProjectPackagePathResult result;
    result.displayPath = inputPath;
    result.error = std::move(message);
    return result;
}

[[nodiscard]] ProjectPackagePathResult resolvePackageDirectory(
    const std::filesystem::path& packageDirectory,
    const std::filesystem::path& inputPath)
{
    if (!isPackageSuffix(packageDirectory))
    {
        return invalidPackage(
            inputPath,
            "Project package directory must use the .talaspkg suffix");
    }

    std::error_code error;
    const auto packageStatus =
        safeSymlinkStatus(packageDirectory, error);
    if (error)
    {
        return invalidPackage(
            inputPath,
            "Unable to inspect project package: "
                + error.message());
    }
    if (!std::filesystem::is_directory(packageStatus))
    {
        return invalidPackage(
            inputPath,
            isMissing(packageStatus)
                ? "Project package does not exist"
                : "Project package path is not a directory");
    }

    const auto manifestPath =
        packageDirectory / projectPackageManifestFileName;
    const auto manifestStatus =
        safeSymlinkStatus(manifestPath, error);
    if (error || !std::filesystem::is_regular_file(manifestStatus))
    {
        return invalidPackage(
            inputPath,
            error
                ? "Unable to inspect project package manifest: "
                    + error.message()
                : "Project package manifest is missing or unsafe");
    }
    std::ifstream manifest(manifestPath, std::ios::binary);
    const std::string manifestText(
        std::istreambuf_iterator<char>{manifest},
        std::istreambuf_iterator<char>{});
    if (!manifest.good() && !manifest.eof())
    {
        return invalidPackage(
            inputPath,
            "Unable to read project package manifest");
    }
    if (manifestText != projectPackageManifestContents)
    {
        return invalidPackage(
            inputPath,
            "Unsupported or malformed project package manifest");
    }

    const auto projectPath =
        packageDirectory / projectPackageDocumentFileName;
    const auto projectStatus =
        safeSymlinkStatus(projectPath, error);
    if (error || !std::filesystem::is_regular_file(projectStatus))
    {
        return invalidPackage(
            inputPath,
            error
                ? "Unable to inspect packaged project file: "
                    + error.message()
                : "Packaged project file is missing or unsafe");
    }

    const auto assetsPath =
        packageDirectory / projectPackageAssetsDirectoryName;
    const auto assetsStatus =
        safeSymlinkStatus(assetsPath, error);
    if (error || !std::filesystem::is_directory(assetsStatus))
    {
        return invalidPackage(
            inputPath,
            error
                ? "Unable to inspect package Assets directory: "
                    + error.message()
                : "Package Assets directory is missing or unsafe");
    }

    return {
        projectPath,
        packageDirectory,
        packageDirectory,
        {},
    };
}
}

ProjectPackagePathResult resolveProjectPackagePath(
    const std::filesystem::path& inputPath)
{
    if (inputPath.empty())
        return invalidPackage(inputPath, "Project path cannot be empty");

    std::error_code error;
    const auto inputStatus = safeSymlinkStatus(inputPath, error);
    if (error)
    {
        return invalidPackage(
            inputPath,
            "Unable to inspect project path: " + error.message());
    }

    if (std::filesystem::is_directory(inputStatus)
        || isPackageSuffix(inputPath))
    {
        return resolvePackageDirectory(inputPath, inputPath);
    }

    if (inputPath.filename() == projectPackageDocumentFileName
        && isPackageSuffix(inputPath.parent_path()))
    {
        return resolvePackageDirectory(
            inputPath.parent_path(),
            inputPath);
    }

    return {
        inputPath,
        inputPath,
        {},
        {},
    };
}

std::filesystem::path projectPackageDisplayPath(
    const std::filesystem::path& projectFilePath)
{
    if (projectFilePath.filename()
            == projectPackageDocumentFileName
        && isPackageSuffix(
            projectFilePath.parent_path()))
    {
        return projectFilePath.parent_path();
    }
    return projectFilePath;
}

ProjectPackageValidationResult validateProjectPackageAssets(
    const ProjectDocument& document,
    const ProjectPackagePathResult& resolvedPackage,
    std::function<bool()> shouldCancel)
{
    if (!resolvedPackage.ok())
        return {{}, resolvedPackage.error};
    if (!resolvedPackage.packageDirectory.has_value())
        return {};

    const auto assetsDirectory =
        *resolvedPackage.packageDirectory
        / projectPackageAssetsDirectoryName;
    std::error_code error;
    const auto canonicalAssets =
        std::filesystem::canonical(assetsDirectory, error);
    if (error)
    {
        return {
            {},
            "Unable to resolve package Assets directory: "
                + error.message(),
        };
    }

    std::unordered_set<std::string> assetIds;
    for (const auto& asset : document.assets)
    {
        if (shouldCancel && shouldCancel())
        {
            return {
                asset.id,
                "Package asset verification cancelled",
                true,
            };
        }
        if (!assetIds.insert(asset.id).second)
        {
            return {
                asset.id,
                "Duplicate package asset id/generation",
            };
        }
        if (const auto invalid = validateProjectAssetManifest(asset);
            invalid.has_value())
        {
            return {
                asset.id,
                "Invalid package asset manifest: "
                    + invalid->message,
            };
        }
        std::filesystem::path storedPath;
        try
        {
            storedPath = pathFromUtf8(asset.path);
        }
        catch (const std::filesystem::filesystem_error& exception)
        {
            return {
                asset.id,
                "Invalid package asset path: "
                    + std::string(exception.what()),
            };
        }
        const auto normalized = storedPath.lexically_normal();
        if (storedPath.empty() || storedPath.is_absolute()
            || normalized.empty()
            || *normalized.begin()
                != std::filesystem::path{
                    projectPackageAssetsDirectoryName})
        {
            return {
                asset.id,
                "Package asset path must remain below Assets",
            };
        }
        if (std::ranges::any_of(
                normalized,
                [](const auto& component) {
                    return component == "..";
                }))
        {
            return {
                asset.id,
                "Package asset path cannot contain parent traversal",
            };
        }

        const auto resolvedPath =
            (resolvedPackage.projectFilePath.parent_path()
             / normalized)
                .lexically_normal();
        if (!isPathWithin(resolvedPath, assetsDirectory))
        {
            return {
                asset.id,
                "Package asset path escapes Assets",
            };
        }

        const auto canonicalAsset =
            std::filesystem::canonical(resolvedPath, error);
        if (error)
        {
            return {
                asset.id,
                "Unable to resolve packaged asset: "
                    + error.message(),
            };
        }
        if (!isPathWithin(canonicalAsset, canonicalAssets))
        {
            return {
                asset.id,
                "Packaged asset resolves outside Assets",
            };
        }
        const auto status =
            std::filesystem::status(canonicalAsset, error);
        if (error || !std::filesystem::is_regular_file(status))
        {
            return {
                asset.id,
                error
                    ? "Unable to inspect packaged asset: "
                        + error.message()
                    : "Packaged asset is not a regular file",
            };
        }
        if (asset.disposition
            == ProjectAssetDisposition::Embedded)
        {
            if (asset.contentHash.empty())
            {
                return {
                    asset.id,
                    "Embedded package asset requires a verified SHA-256 hash",
                };
            }
            const auto inspected = inspectProjectAsset(
                asset,
                canonicalAsset,
                shouldCancel);
            if (!inspected.ok())
            {
                return {
                    asset.id,
                    inspected.error->code
                            == ProjectAssetInspectionErrorCode::Cancelled
                        ? "Package asset verification cancelled"
                        : "Embedded package asset verification failed: "
                            + inspected.error->message,
                    inspected.error->code
                        == ProjectAssetInspectionErrorCode::Cancelled,
                };
            }
        }
    }
    return {};
}
}
