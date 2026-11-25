#pragma once

#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace talas::project
{
inline constexpr const char projectPackageExtension[] = ".talaspkg";
inline constexpr const char projectPackageManifestFileName[] =
    "Manifest.json";
inline constexpr const char projectPackageDocumentFileName[] =
    "Project.talas.json";
inline constexpr const char projectPackageAssetsDirectoryName[] =
    "Assets";
inline constexpr std::string_view projectPackageManifestContents =
    "{\n"
    "  \"format\": \"talas.project-package\",\n"
    "  \"version\": 1\n"
    "}\n";

struct ProjectPackagePathResult
{
    std::filesystem::path projectFilePath;
    std::filesystem::path displayPath;
    std::optional<std::filesystem::path> packageDirectory;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return error.empty();
    }
};

struct ProjectPackageValidationResult
{
    std::string assetId;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const
    {
        return error.empty();
    }
};

[[nodiscard]] ProjectPackagePathResult resolveProjectPackagePath(
    const std::filesystem::path& inputPath);

[[nodiscard]] std::filesystem::path projectPackageDisplayPath(
    const std::filesystem::path& projectFilePath);

[[nodiscard]] ProjectPackageValidationResult
validateProjectPackageAssets(
    const ProjectDocument& document,
    const ProjectPackagePathResult& resolvedPackage,
    std::function<bool()> shouldCancel = {});
}
