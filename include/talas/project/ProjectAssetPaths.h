#pragma once

#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <optional>
#include <string>

namespace talas::project
{
[[nodiscard]] std::filesystem::path resolveProjectAssetPath(
    const ProjectAsset& asset,
    const std::optional<std::filesystem::path>& projectFilePath);

[[nodiscard]] std::string projectAssetStoredPath(
    const std::filesystem::path& resolvedAssetPath,
    const std::optional<std::filesystem::path>& projectFilePath);

[[nodiscard]] bool makeProjectAssetPathsAbsolute(
    ProjectDocument& document,
    const std::filesystem::path& sourceProjectFilePath);

[[nodiscard]] bool rebaseProjectAssetPaths(
    ProjectDocument& document,
    const std::optional<std::filesystem::path>& sourceProjectFilePath,
    const std::filesystem::path& targetProjectFilePath);
}
