#pragma once

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
enum class ProjectAssetWatchFileState
{
    Missing,
    RegularFile,
    Other,
    Inaccessible,
};

struct ProjectAssetWatchEntry
{
    std::string assetId;
    std::uint64_t generation = 1;
    std::string contentHash;
    std::filesystem::path resolvedPath;
    std::filesystem::path canonicalTarget;
    ProjectAssetWatchFileState state =
        ProjectAssetWatchFileState::Missing;
    std::uintmax_t byteCount = 0;
    std::optional<std::filesystem::file_time_type>
        lastWriteTime;
    std::string error;
};

struct ProjectAssetWatchSnapshot
{
    std::vector<ProjectAssetWatchEntry> entries;
    std::optional<ProjectAssetReferenceResolutionFailure>
        referenceFailure;
};

struct ProjectAssetWatchChange
{
    std::string assetId;
    std::filesystem::path resolvedPath;
    std::string message;
};

[[nodiscard]] ProjectAssetWatchSnapshot
captureProjectAssetWatchSnapshot(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>&
        projectFilePath);

[[nodiscard]] std::optional<ProjectAssetWatchChange>
findProjectAssetWatchChange(
    const ProjectAssetWatchSnapshot& expected,
    const ProjectAssetWatchSnapshot& current);
}
