#pragma once

#include "talas/project/ProjectAssetInspection.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::project
{
enum class ProjectAssetResolutionMode
{
    Relocation,
    InitialInspection,
    Replacement,
};

struct ProjectAssetResolution
{
    ProjectAssetResolutionMode mode =
        ProjectAssetResolutionMode::Relocation;
    std::uint64_t expectedGeneration = 1;
    std::string expectedContentHash;
    ProjectAsset asset;

    bool operator==(const ProjectAssetResolution&) const = default;
};

struct ProjectAssetCandidateResult
{
    std::optional<ProjectAssetResolution> resolution;
    std::optional<ProjectAssetInspectionError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return resolution.has_value() && !error.has_value();
    }
};

[[nodiscard]] ProjectAssetCandidateResult classifyProjectAssetCandidate(
    const ProjectAsset& manifest,
    const std::filesystem::path& candidatePath,
    const std::optional<std::filesystem::path>& projectFilePath = std::nullopt,
    ProjectAssetInspectionCancellationCheck shouldCancel = {});

inline constexpr std::size_t maximumProjectAssetSearchVisitedEntries = 65536;
inline constexpr std::size_t maximumProjectAssetSearchHashedFiles = 4096;
inline constexpr std::size_t maximumProjectAssetResolverDiagnostics = 256;

enum class ProjectAssetSearchStatus
{
    Completed,
    Cancelled,
    CapacityExceeded,
    InvalidRoot,
};

enum class ProjectAssetResolverDiagnosticCode
{
    Inaccessible,
    NotRegularFile,
    CapacityExceeded,
};

struct ProjectAssetResolverDiagnostic
{
    ProjectAssetResolverDiagnosticCode code =
        ProjectAssetResolverDiagnosticCode::Inaccessible;
    std::string path;
    std::string message;
};

struct ProjectAssetSearchLimits
{
    std::size_t maximumVisitedEntries =
        maximumProjectAssetSearchVisitedEntries;
    std::size_t maximumHashedFiles =
        maximumProjectAssetSearchHashedFiles;
    std::size_t maximumDiagnostics =
        maximumProjectAssetResolverDiagnostics;
};

struct ProjectAssetSearchProgress
{
    std::size_t visitedEntries = 0;
    std::size_t hashedFiles = 0;
    std::string currentRelativePath;
};

using ProjectAssetSearchProgressCallback =
    std::function<void(const ProjectAssetSearchProgress&)>;

struct ProjectAssetSearchMatch
{
    std::string assetId;
    std::vector<ProjectAssetResolution> candidates;
};

struct ProjectAssetSearchResult
{
    ProjectAssetSearchStatus status =
        ProjectAssetSearchStatus::Completed;
    std::size_t visitedEntries = 0;
    std::size_t hashedFiles = 0;
    std::vector<ProjectAssetSearchMatch> matches;
    std::vector<ProjectAssetResolverDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == ProjectAssetSearchStatus::Completed;
    }
};

[[nodiscard]] ProjectAssetSearchResult searchProjectAssetDirectory(
    std::span<const ProjectAsset> manifests,
    const std::filesystem::path& searchRoot,
    const std::optional<std::filesystem::path>& projectFilePath = std::nullopt,
    ProjectAssetSearchLimits limits = {},
    ProjectAssetInspectionCancellationCheck shouldCancel = {},
    ProjectAssetSearchProgressCallback onProgress = {});
}
