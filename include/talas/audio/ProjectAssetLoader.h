#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::audio
{
struct LoadedProjectAsset
{
    std::string id;
    talas::project::ProjectAssetKind kind =
        talas::project::ProjectAssetKind::Table;
    std::filesystem::path resolvedPath;
    std::vector<double> tableSamples;
    std::vector<double> tableRightSamples;
    std::uint32_t tableSourceSampleRate = 0;
    std::optional<talas::core::SampleLoop> tableForwardLoop;
    std::optional<talas::core::SamplePitch> tableSamplePitch;
    std::vector<std::uint8_t> binaryBytes;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
    std::vector<std::uint8_t> partialTrackBytes;
};

struct ProjectAssetLoadFailure
{
    std::string assetId;
    std::filesystem::path resolvedPath;
    std::string nodeId;
    std::string parameterId;
    std::string structuralSourcePath;
    std::string message;
    bool replacementMayBeAccepted = false;
};

[[nodiscard]] std::string describeProjectAssetLoadFailure(
    const ProjectAssetLoadFailure& failure);

struct ProjectResourceSnapshotsLoadResult
{
    std::vector<talas::core::ResourceSnapshotPtr> snapshots;
    std::optional<ProjectAssetLoadFailure> failure;
    bool wasCancelled = false;

    [[nodiscard]] bool ok() const
    {
        return !failure.has_value() && !wasCancelled;
    }
};

struct ProjectAssetsLoadResult
{
    std::vector<LoadedProjectAsset> assets;
    std::optional<ProjectAssetLoadFailure> failure;
    bool wasCancelled = false;

    [[nodiscard]] bool ok() const
    {
        return !failure.has_value() && !wasCancelled;
    }
};

struct ProjectAssetReference
{
    std::string assetId;
    std::string nodeId;
    std::string parameterId;
    std::string structuralSourcePath;
    // A KeyMappedMultisample's FirstSample, which brings every sample file
    // in its folder.
    bool bank = false;
};

struct ProjectAssetReferenceResolutionFailure
{
    std::string nodeId;
    std::string parameterId;
    std::string structuralSourcePath;
    std::string message;

    bool operator==(
        const ProjectAssetReferenceResolutionFailure&) const = default;
};

struct ProjectAssetReferencesResult
{
    std::vector<ProjectAssetReference> references;
    std::optional<ProjectAssetReferenceResolutionFailure> failure;

    [[nodiscard]] bool ok() const noexcept
    {
        return !failure.has_value();
    }
};

[[nodiscard]] ProjectAssetReferencesResult
reachableProjectAssetReferences(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

// Compatibility name for callers migrating to the reachable-asset authority.
[[nodiscard]] ProjectAssetReferencesResult
referencedProjectAssetParameters(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] std::filesystem::path resolveProjectAssetPath(
    const talas::project::ProjectAsset& asset,
    const std::optional<std::filesystem::path>& projectFilePath);

[[nodiscard]] ProjectResourceSnapshotsLoadResult
loadReferencedProjectResourceSnapshots(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::span<const std::string> excludedAssetIds = {},
    talas::project::ProjectAssetInspectionCancellationCheck
        shouldCancel = {});

// Compatibility entry point. New preparation paths should retain immutable
// snapshots through loadReferencedProjectResourceSnapshots instead.
[[nodiscard]] ProjectAssetsLoadResult loadReferencedProjectAssets(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::span<const std::string> excludedAssetIds = {},
    talas::project::ProjectAssetInspectionCancellationCheck
        shouldCancel = {});
}
