#include "talas/audio/ProjectAssetChangeTracker.h"

#include "talas/audio/ProjectAssetLoader.h"

#include "talas/project/ProjectAssetPaths.h"

#include <algorithm>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace talas::audio
{
namespace
{
[[nodiscard]] bool isMissing(
    const std::filesystem::file_status& status)
{
    return status.type()
        == std::filesystem::file_type::not_found;
}

[[nodiscard]] ProjectAssetWatchEntry captureEntry(
    const talas::project::ProjectAsset& asset,
    const std::optional<std::filesystem::path>&
        projectFilePath)
{
    ProjectAssetWatchEntry entry;
    entry.assetId = asset.id;
    entry.generation = asset.generation;
    entry.contentHash = asset.contentHash;
    entry.resolvedPath =
        talas::project::resolveProjectAssetPath(
            asset,
            projectFilePath);
    if (entry.resolvedPath.empty())
    {
        entry.state =
            ProjectAssetWatchFileState::Inaccessible;
        entry.error = "Asset path cannot be resolved";
        return entry;
    }

    std::error_code error;
    const auto status =
        std::filesystem::status(
            entry.resolvedPath,
            error);
    if (error
        == std::errc::no_such_file_or_directory
        || (!error && isMissing(status)))
    {
        entry.state =
            ProjectAssetWatchFileState::Missing;
        return entry;
    }
    if (error)
    {
        entry.state =
            ProjectAssetWatchFileState::Inaccessible;
        entry.error = error.message();
        return entry;
    }
    if (!std::filesystem::is_regular_file(status))
    {
        entry.state =
            ProjectAssetWatchFileState::Other;
        return entry;
    }

    entry.canonicalTarget =
        std::filesystem::canonical(
            entry.resolvedPath,
            error);
    if (error)
    {
        entry.state =
            ProjectAssetWatchFileState::Inaccessible;
        entry.error = error.message();
        return entry;
    }
    entry.byteCount =
        std::filesystem::file_size(
            entry.canonicalTarget,
            error);
    if (error)
    {
        entry.state =
            ProjectAssetWatchFileState::Inaccessible;
        entry.error = error.message();
        return entry;
    }
    entry.lastWriteTime =
        std::filesystem::last_write_time(
            entry.canonicalTarget,
            error);
    if (error)
    {
        entry.state =
            ProjectAssetWatchFileState::Inaccessible;
        entry.lastWriteTime.reset();
        entry.error = error.message();
        return entry;
    }
    entry.state =
        ProjectAssetWatchFileState::RegularFile;
    return entry;
}

[[nodiscard]] bool entriesEqual(
    const ProjectAssetWatchEntry& expected,
    const ProjectAssetWatchEntry& current)
{
    return expected.assetId == current.assetId
        && expected.generation == current.generation
        && expected.contentHash == current.contentHash
        && expected.resolvedPath
            == current.resolvedPath
        && expected.canonicalTarget
            == current.canonicalTarget
        && expected.state == current.state
        && expected.byteCount == current.byteCount
        && expected.lastWriteTime
            == current.lastWriteTime
        && expected.error == current.error;
}

[[nodiscard]] ProjectAssetWatchChange changeFor(
    const ProjectAssetWatchEntry& entry,
    std::string message)
{
    return {
        entry.assetId,
        entry.resolvedPath,
        std::move(message),
    };
}
}

ProjectAssetWatchSnapshot
captureProjectAssetWatchSnapshot(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>&
        projectFilePath)
{
    ProjectAssetWatchSnapshot snapshot;
    std::set<std::string> referencedAssetIds;
    auto resolvedReferences =
        reachableProjectAssetReferences(document, registry);
    if (!resolvedReferences.ok())
    {
        snapshot.referenceFailure =
            std::move(resolvedReferences.failure);
        return snapshot;
    }
    for (const auto& reference : resolvedReferences.references)
        referencedAssetIds.insert(reference.assetId);

    snapshot.entries.reserve(
        std::min(
            document.assets.size(),
            referencedAssetIds.size()));
    for (const auto& asset : document.assets)
    {
        if (referencedAssetIds.contains(asset.id))
        {
            snapshot.entries.push_back(
                captureEntry(
                    asset,
                    projectFilePath));
        }
    }
    return snapshot;
}

std::optional<ProjectAssetWatchChange>
findProjectAssetWatchChange(
    const ProjectAssetWatchSnapshot& expected,
    const ProjectAssetWatchSnapshot& current)
{
    if (current.referenceFailure.has_value())
    {
        return ProjectAssetWatchChange{
            {},
            {},
            current.referenceFailure->message,
        };
    }
    if (expected.referenceFailure.has_value())
    {
        return ProjectAssetWatchChange{
            {},
            {},
            "Asset reference resolution recovered",
        };
    }
    const auto commonCount =
        std::min(
            expected.entries.size(),
            current.entries.size());
    for (std::size_t index = 0;
         index < commonCount;
         ++index)
    {
        const auto& before = expected.entries[index];
        const auto& after = current.entries[index];
        if (entriesEqual(before, after))
            continue;
        if (before.assetId != after.assetId
            || before.resolvedPath
                != after.resolvedPath)
        {
            return changeFor(
                after,
                "Asset binding changed");
        }
        if (before.generation != after.generation
            || before.contentHash != after.contentHash)
        {
            return changeFor(
                after,
                "Accepted asset generation changed");
        }
        switch (after.state)
        {
        case ProjectAssetWatchFileState::Missing:
            return changeFor(
                after,
                "Asset is missing");
        case ProjectAssetWatchFileState::Other:
            return changeFor(
                after,
                "Asset is no longer a regular file");
        case ProjectAssetWatchFileState::Inaccessible:
            return changeFor(
                after,
                after.error.empty()
                    ? "Asset cannot be inspected"
                    : "Asset cannot be inspected: "
                        + after.error);
        case ProjectAssetWatchFileState::RegularFile:
            return changeFor(
                after,
                "Asset changed on disk");
        }
    }

    if (expected.entries.size()
        == current.entries.size())
    {
        return {};
    }
    if (commonCount < current.entries.size())
    {
        return changeFor(
            current.entries[commonCount],
            "Referenced asset was added");
    }
    return changeFor(
        expected.entries[commonCount],
        "Referenced asset was removed");
}
}
