#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectAsset;

inline constexpr std::size_t maximumProjectPartialTrackEditSets = 64;
inline constexpr std::size_t maximumProjectPartialTrackPointOverrides =
    1U << 18U;
inline constexpr std::size_t maximumProjectPartialTrackInsertedPoints =
    1U << 16U;
inline constexpr std::size_t maximumProjectPartialTrackStates =
    maximumPartialTrackAssetTracks;
inline constexpr std::size_t maximumProjectPartialTrackEditIdBytes = 128;
inline constexpr std::size_t maximumProjectPartialTrackAssetIdBytes = 512;
inline constexpr std::size_t maximumProjectPartialTrackBatchPointMutations =
    4096;

enum class PartialTrackPointInterpolation : std::uint8_t
{
    Step,
    Linear,
};

struct PartialTrackSourcePointIdentity
{
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    std::uint64_t trackId = 0;

    auto operator<=>(const PartialTrackSourcePointIdentity&) const = default;
};

struct ProjectPartialTrackPointOverride
{
    PartialTrackSourcePointIdentity source;
    std::uint64_t outputFrame = 0;
    PartialTrackPoint output;
    PartialTrackPointInterpolation interpolation =
        PartialTrackPointInterpolation::Linear;
    bool excluded = false;

    bool operator==(const ProjectPartialTrackPointOverride&) const = default;
};

struct ProjectPartialTrackInsertedPoint
{
    std::string editId;
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    PartialTrackPoint point;
    PartialTrackPointInterpolation interpolation =
        PartialTrackPointInterpolation::Linear;

    bool operator==(const ProjectPartialTrackInsertedPoint&) const = default;
};

struct ProjectPartialTrackState
{
    std::uint64_t trackId = 0;
    bool muted = false;
    bool solo = false;

    bool operator==(const ProjectPartialTrackState&) const = default;
};

enum class MaterializedPartialTrackPointOrigin : std::uint8_t
{
    Source,
    Inserted,
};

struct MaterializedPartialTrackPointIdentity
{
    MaterializedPartialTrackPointOrigin origin =
        MaterializedPartialTrackPointOrigin::Source;
    PartialTrackSourcePointIdentity source;
    std::string insertedEditId;

    bool operator==(
        const MaterializedPartialTrackPointIdentity&) const = default;
};

struct ProjectPartialTrackEditSet
{
    std::string editSetId;
    std::string assetId;
    std::uint64_t assetGeneration = 0;
    std::string sourceContentHash;
    std::vector<ProjectPartialTrackPointOverride> pointOverrides;
    std::vector<ProjectPartialTrackInsertedPoint> insertedPoints;
    std::vector<ProjectPartialTrackState> trackStates;

    bool operator==(const ProjectPartialTrackEditSet&) const = default;
};

enum class ProjectPartialTrackPointMutationKind : std::uint8_t
{
    Update,
    Delete,
    Insert,
};

struct ProjectPartialTrackPointMutation
{
    ProjectPartialTrackPointMutationKind kind =
        ProjectPartialTrackPointMutationKind::Update;
    MaterializedPartialTrackPointIdentity target;
    std::uint64_t segmentId = 0;
    std::uint64_t outputFrame = 0;
    PartialTrackPoint output;
    PartialTrackPointInterpolation interpolation =
        PartialTrackPointInterpolation::Linear;

    bool operator==(const ProjectPartialTrackPointMutation&) const = default;
};

struct ProjectPartialTrackStateMutation
{
    std::uint64_t trackId = 0;
    bool muted = false;
    bool solo = false;

    bool operator==(const ProjectPartialTrackStateMutation&) const = default;
};

struct ProjectPartialTrackEditBatch
{
    std::string editSetId;
    std::string assetId;
    std::uint64_t assetGeneration = 0;
    std::string sourceContentHash;
    std::vector<ProjectPartialTrackPointMutation> pointMutations;
    std::vector<ProjectPartialTrackStateMutation> trackStateMutations;
};

enum class ProjectPartialTrackEditErrorCode : std::uint8_t
{
    InvalidIdentity,
    InvalidSourceAuthority,
    LimitExceeded,
    InvalidOrder,
    DuplicateIdentity,
    MissingSourcePoint,
    MissingTrack,
    InvalidPoint,
    AllocationFailed,
};

struct ProjectPartialTrackEditError
{
    ProjectPartialTrackEditErrorCode code =
        ProjectPartialTrackEditErrorCode::InvalidIdentity;
    std::size_t index = 0;
    std::string message;
};

struct MaterializedPartialTrackEdits
{
    PartialTrackAsset asset;
    std::vector<ProjectPartialTrackState> trackStates;
    std::vector<PartialTrackPointInterpolation> pointInterpolations;
    std::vector<MaterializedPartialTrackPointIdentity> pointIdentities;

    bool operator==(const MaterializedPartialTrackEdits&) const = default;
};

struct MaterializedPartialTrackEditsResult
{
    MaterializedPartialTrackEdits materialized;
    std::optional<ProjectPartialTrackEditError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

struct ProjectPartialTrackEditBatchResult
{
    ProjectPartialTrackEditSet edits;
    std::optional<ProjectPartialTrackEditError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] bool isValidProjectPartialTrackEditIdentifier(
    std::string_view value) noexcept;

[[nodiscard]] std::optional<ProjectPartialTrackEditError>
validateProjectPartialTrackEditSet(
    const ProjectPartialTrackEditSet& edits);

[[nodiscard]] bool partialTrackEditSetMatchesAsset(
    const ProjectPartialTrackEditSet& edits,
    const ProjectAsset& asset) noexcept;

[[nodiscard]] MaterializedPartialTrackEditsResult
materializeProjectPartialTrackEdits(
    const PartialTrackAsset& source,
    const ProjectPartialTrackEditSet& edits);

[[nodiscard]] ProjectPartialTrackEditBatchResult
applyProjectPartialTrackEditBatch(
    const PartialTrackAsset& source,
    const ProjectPartialTrackEditSet* current,
    const ProjectPartialTrackEditBatch& batch);
}
