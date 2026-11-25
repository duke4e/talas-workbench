#include "talas/project/ProjectAssetPaths.h"

#include "talas/project/ProjectPathEncoding.h"

namespace talas::project
{
namespace
{
[[nodiscard]] std::filesystem::path normalizedAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}
}

std::filesystem::path resolveProjectAssetPath(
    const ProjectAsset& asset,
    const std::optional<std::filesystem::path>& projectFilePath)
{
    std::filesystem::path stored;
    try
    {
        stored = pathFromUtf8(asset.path);
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return {};
    }
    if (stored.empty())
        return {};
    if (stored.is_absolute())
        return stored.lexically_normal();
    if (!projectFilePath.has_value())
        return stored.lexically_normal();
    return (normalizedAbsolutePath(projectFilePath->parent_path()) / stored).lexically_normal();
}

std::string projectAssetStoredPath(
    const std::filesystem::path& resolvedAssetPath,
    const std::optional<std::filesystem::path>& projectFilePath)
{
    auto normalized = normalizedAbsolutePath(resolvedAssetPath);
    std::error_code canonicalAssetError;
    const auto canonicalAsset = std::filesystem::weakly_canonical(
        normalized,
        canonicalAssetError);
    if (!canonicalAssetError)
        normalized = canonicalAsset;
    if (!projectFilePath.has_value() || projectFilePath->empty())
        return pathToUtf8(normalized);

    auto projectDirectory =
        normalizedAbsolutePath(*projectFilePath).parent_path();
    std::error_code canonicalError;
    const auto canonicalProjectDirectory =
        std::filesystem::weakly_canonical(
            projectDirectory,
            canonicalError);
    if (!canonicalError)
        projectDirectory = canonicalProjectDirectory;
    const auto relative = normalized.lexically_relative(projectDirectory);
    return pathToUtf8(relative.empty() ? normalized : relative);
}

bool makeProjectAssetPathsAbsolute(
    ProjectDocument& document,
    const std::filesystem::path& sourceProjectFilePath)
{
    if (sourceProjectFilePath.empty())
        return false;

    auto changed = false;
    for (auto& asset : document.assets)
    {
        const auto resolved = resolveProjectAssetPath(
            asset,
            sourceProjectFilePath);
        if (resolved.empty() || resolved.is_relative())
            continue;
        const auto stored = pathToUtf8(resolved);
        if (asset.path == stored)
            continue;
        asset.path = stored;
        changed = true;
    }
    return changed;
}

bool rebaseProjectAssetPaths(
    ProjectDocument& document,
    const std::optional<std::filesystem::path>& sourceProjectFilePath,
    const std::filesystem::path& targetProjectFilePath)
{
    if (targetProjectFilePath.empty())
        return false;

    const auto targetDirectory = normalizedAbsolutePath(targetProjectFilePath).parent_path();
    auto changed = false;
    for (auto& asset : document.assets)
    {
        const auto resolved = resolveProjectAssetPath(asset, sourceProjectFilePath);
        if (resolved.empty() || resolved.is_relative())
            continue;

        const auto relative = resolved.lexically_relative(targetDirectory);
        const auto rebased = relative.empty() ? resolved : relative;
        const auto stored = pathToUtf8(rebased);
        if (asset.path == stored)
            continue;
        asset.path = stored;
        changed = true;
    }
    return changed;
}
}
