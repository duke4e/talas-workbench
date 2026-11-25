#include "talas/project/ProjectPartialTrackEdits.h"

#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] ProjectPartialTrackEditError error(
    ProjectPartialTrackEditErrorCode code,
    std::string message,
    std::size_t index = 0)
{
    return {code, index, std::move(message)};
}

[[nodiscard]] bool boundedAssetId(std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > maximumProjectPartialTrackAssetIdBytes)
    {
        return false;
    }
    return std::ranges::none_of(value, [](unsigned char character) {
        return character == 0 || character < 0x20U;
    });
}

[[nodiscard]] bool validInterpolation(
    PartialTrackPointInterpolation interpolation) noexcept
{
    switch (interpolation)
    {
    case PartialTrackPointInterpolation::Step:
    case PartialTrackPointInterpolation::Linear:
        return true;
    }
    return false;
}

[[nodiscard]] bool validPointForSampleRate(
    const PartialTrackPoint& point,
    double sampleRate) noexcept
{
    return point.trackId != 0
        && std::isfinite(point.amplitude)
        && point.amplitude >= 0.0
        && !(point.amplitude == 0.0 && std::signbit(point.amplitude))
        && std::isfinite(point.frequencyHz)
        && point.frequencyHz > 0.0
        && point.frequencyHz <= sampleRate * 0.5
        && std::isfinite(point.bandwidthHz)
        && point.bandwidthHz >= 0.0
        && !(point.bandwidthHz == 0.0 && std::signbit(point.bandwidthHz))
        && point.bandwidthHz <= sampleRate * 0.5;
}

struct DerivedPoint
{
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    PartialTrackPoint point;
    PartialTrackPointInterpolation interpolation =
        PartialTrackPointInterpolation::Linear;
    MaterializedPartialTrackPointIdentity identity;
};

[[nodiscard]] auto derivedKey(const DerivedPoint& point) noexcept
{
    return std::tuple{
        point.segmentId,
        point.sourceFrame,
        point.point.trackId};
}

[[nodiscard]] bool identityLess(
    const MaterializedPartialTrackPointIdentity& left,
    const MaterializedPartialTrackPointIdentity& right) noexcept
{
    if (left.origin != right.origin)
        return left.origin < right.origin;
    if (left.origin == MaterializedPartialTrackPointOrigin::Source)
        return left.source < right.source;
    return left.insertedEditId < right.insertedEditId;
}

[[nodiscard]] bool validMutationKind(
    ProjectPartialTrackPointMutationKind kind) noexcept
{
    switch (kind)
    {
    case ProjectPartialTrackPointMutationKind::Update:
    case ProjectPartialTrackPointMutationKind::Delete:
    case ProjectPartialTrackPointMutationKind::Insert:
        return true;
    }
    return false;
}

[[nodiscard]] bool validMutationIdentity(
    const MaterializedPartialTrackPointIdentity& identity) noexcept
{
    if (identity.origin == MaterializedPartialTrackPointOrigin::Source)
    {
        return identity.source.segmentId != 0
            && identity.source.trackId != 0
            && identity.source.sourceFrame
                <= maximumPartialTrackSourceFrames
            && identity.insertedEditId.empty();
    }
    return identity.source == PartialTrackSourcePointIdentity{}
        && isValidProjectPartialTrackEditIdentifier(
            identity.insertedEditId);
}
}

bool isValidProjectPartialTrackEditIdentifier(
    std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > maximumProjectPartialTrackEditIdBytes)
    {
        return false;
    }
    const auto alpha = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || character == '_';
    };
    const auto tail = [&](unsigned char character) {
        return alpha(character)
            || (character >= '0' && character <= '9')
            || character == '-';
    };
    return alpha(static_cast<unsigned char>(value.front()))
        && std::ranges::all_of(
            value.substr(1),
            [&](char character) {
                return tail(static_cast<unsigned char>(character));
            });
}

std::optional<ProjectPartialTrackEditError>
validateProjectPartialTrackEditSet(
    const ProjectPartialTrackEditSet& edits)
{
    if (!isValidProjectPartialTrackEditIdentifier(edits.editSetId)
        || !boundedAssetId(edits.assetId))
    {
        return error(
            ProjectPartialTrackEditErrorCode::InvalidIdentity,
            "Partial-track edit-set and asset identities are invalid");
    }
    if (edits.assetGeneration == 0
        || edits.assetGeneration > maximumProjectAssetJsonInteger
        || !isCanonicalProjectAssetContentHash(
            edits.sourceContentHash))
    {
        return error(
            ProjectPartialTrackEditErrorCode::InvalidSourceAuthority,
            "Partial-track edit set requires an exact positive asset generation and SHA-256 hash");
    }
    if (edits.pointOverrides.size()
            > maximumProjectPartialTrackPointOverrides
        || edits.insertedPoints.size()
            > maximumProjectPartialTrackInsertedPoints
        || edits.trackStates.size()
            > maximumProjectPartialTrackStates)
    {
        return error(
            ProjectPartialTrackEditErrorCode::LimitExceeded,
            "Partial-track edit collections exceed their fixed limits");
    }

    PartialTrackSourcePointIdentity previousSource;
    for (std::size_t index = 0;
         index < edits.pointOverrides.size();
         ++index)
    {
        const auto& point = edits.pointOverrides[index];
        if (point.source.segmentId == 0
            || point.source.trackId == 0
            || point.source.sourceFrame > maximumPartialTrackSourceFrames
            || point.outputFrame > maximumPartialTrackSourceFrames
            || (point.excluded && point.output != PartialTrackPoint{})
            || (!point.excluded && point.output.trackId == 0)
            || (!point.excluded
                && (!std::isfinite(point.output.amplitude)
                    || point.output.amplitude < 0.0
                    || (point.output.amplitude == 0.0
                        && std::signbit(point.output.amplitude))
                    || !std::isfinite(point.output.frequencyHz)
                    || point.output.frequencyHz <= 0.0
                    || !std::isfinite(point.output.bandwidthHz)
                    || point.output.bandwidthHz < 0.0
                    || (point.output.bandwidthHz == 0.0
                        && std::signbit(point.output.bandwidthHz))))
            || !validInterpolation(point.interpolation))
        {
            return error(
                ProjectPartialTrackEditErrorCode::InvalidIdentity,
                "Partial-track point override identity is invalid",
                index);
        }
        if (index != 0 && point.source <= previousSource)
        {
            return error(
                ProjectPartialTrackEditErrorCode::InvalidOrder,
                "Partial-track point overrides must be strictly source-identity ordered",
                index);
        }
        previousSource = point.source;
    }

    std::unordered_set<std::string> insertedIds;
    try
    {
        insertedIds.reserve(edits.insertedPoints.size());
        for (std::size_t index = 0;
             index < edits.insertedPoints.size();
             ++index)
        {
            const auto& point = edits.insertedPoints[index];
            if (!isValidProjectPartialTrackEditIdentifier(point.editId)
                || point.segmentId == 0
                || point.sourceFrame > maximumPartialTrackSourceFrames
                || point.point.trackId == 0
                || !std::isfinite(point.point.amplitude)
                || point.point.amplitude < 0.0
                || (point.point.amplitude == 0.0
                    && std::signbit(point.point.amplitude))
                || !std::isfinite(point.point.frequencyHz)
                || point.point.frequencyHz <= 0.0
                || !std::isfinite(point.point.bandwidthHz)
                || point.point.bandwidthHz < 0.0
                || (point.point.bandwidthHz == 0.0
                    && std::signbit(point.point.bandwidthHz))
                || !validInterpolation(point.interpolation))
            {
                return error(
                    ProjectPartialTrackEditErrorCode::InvalidIdentity,
                    "Inserted partial-track point identity is invalid",
                    index);
            }
            if (index != 0
                && point.editId
                    <= edits.insertedPoints[index - 1].editId)
            {
                return error(
                    ProjectPartialTrackEditErrorCode::InvalidOrder,
                    "Inserted partial-track points must be strictly edit-identity ordered",
                    index);
            }
            if (!insertedIds.insert(point.editId).second)
            {
                return error(
                    ProjectPartialTrackEditErrorCode::DuplicateIdentity,
                    "Inserted partial-track edit identity is duplicated",
                    index);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        return error(
            ProjectPartialTrackEditErrorCode::AllocationFailed,
            "Partial-track edit validation allocation failed");
    }

    std::uint64_t previousTrackId = 0;
    for (std::size_t index = 0;
         index < edits.trackStates.size();
         ++index)
    {
        const auto& state = edits.trackStates[index];
        if (state.trackId == 0
            || (index != 0 && state.trackId <= previousTrackId)
            || (state.muted && state.solo))
        {
            return error(
                ProjectPartialTrackEditErrorCode::InvalidOrder,
                "Partial-track states must be unique/ascending and cannot be both mute and solo",
                index);
        }
        previousTrackId = state.trackId;
    }
    return std::nullopt;
}

bool partialTrackEditSetMatchesAsset(
    const ProjectPartialTrackEditSet& edits,
    const ProjectAsset& asset) noexcept
{
    return asset.kind == ProjectAssetKind::PartialSpectrum
        && edits.assetId == asset.id
        && edits.assetGeneration == asset.generation
        && edits.sourceContentHash == asset.contentHash
        && asset.channelCount == 0
        && asset.encoding == partialTrackAssetEncoding;
}

MaterializedPartialTrackEditsResult materializeProjectPartialTrackEdits(
    const PartialTrackAsset& source,
    const ProjectPartialTrackEditSet& edits)
{
    MaterializedPartialTrackEditsResult result;
    if (const auto sourceError = validatePartialTrackAsset(source))
    {
        result.error = error(
            ProjectPartialTrackEditErrorCode::InvalidPoint,
            "Partial-track edit source is invalid: "
                + sourceError->message);
        return result;
    }
    result.error = validateProjectPartialTrackEditSet(edits);
    if (result.error.has_value())
        return result;

    try
    {
        std::size_t sourcePointCount = 0;
        for (const auto& frame : source.frames)
            sourcePointCount += frame.points.size();
        if (edits.insertedPoints.size()
            > maximumPartialTrackAssetPoints - sourcePointCount)
        {
            result.error = error(
                ProjectPartialTrackEditErrorCode::LimitExceeded,
                "Edited partial-track point count exceeds the asset limit");
            return result;
        }
        std::vector<DerivedPoint> points;
        points.reserve(sourcePointCount + edits.insertedPoints.size());
        std::size_t overrideIndex = 0;
        for (const auto& frame : source.frames)
        {
            for (const auto& sourcePoint : frame.points)
            {
                const PartialTrackSourcePointIdentity identity{
                    frame.segmentId,
                    frame.sourceFrame,
                    sourcePoint.trackId};
                const auto* replacement =
                    overrideIndex < edits.pointOverrides.size()
                        ? &edits.pointOverrides[overrideIndex]
                        : nullptr;
                if (replacement != nullptr
                    && replacement->source < identity)
                {
                    result.error = error(
                        ProjectPartialTrackEditErrorCode::MissingSourcePoint,
                        "Partial-track point override references a missing source point",
                        overrideIndex);
                    return result;
                }
                if (replacement != nullptr
                    && replacement->source == identity)
                {
                    if (!replacement->excluded)
                    {
                        points.push_back({
                            frame.segmentId,
                            replacement->outputFrame,
                            replacement->output,
                            replacement->interpolation,
                            {
                                MaterializedPartialTrackPointOrigin::Source,
                                identity,
                                {},
                            },
                        });
                    }
                    ++overrideIndex;
                }
                else
                {
                    points.push_back({
                        frame.segmentId,
                        frame.sourceFrame,
                        sourcePoint,
                        PartialTrackPointInterpolation::Linear,
                        {
                            MaterializedPartialTrackPointOrigin::Source,
                            identity,
                            {},
                        },
                    });
                }
            }
        }
        if (overrideIndex != edits.pointOverrides.size())
        {
            result.error = error(
                ProjectPartialTrackEditErrorCode::MissingSourcePoint,
                "Partial-track point override references a missing source point",
                overrideIndex);
            return result;
        }
        for (const auto& inserted : edits.insertedPoints)
        {
            points.push_back({
                inserted.segmentId,
                inserted.sourceFrame,
                inserted.point,
                inserted.interpolation,
                {
                    MaterializedPartialTrackPointOrigin::Inserted,
                    {},
                    inserted.editId,
                },
            });
        }
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            const auto& point = points[index];
            if (point.sourceFrame >= source.sourceFrameCount
                || !validPointForSampleRate(
                    point.point,
                    source.sampleRate))
            {
                result.error = error(
                    ProjectPartialTrackEditErrorCode::InvalidPoint,
                    "Materialized partial-track point is outside the source extent or numeric domain",
                    index);
                return result;
            }
        }
        std::ranges::sort(
            points,
            {},
            &derivedKey);
        for (std::size_t index = 1; index < points.size(); ++index)
        {
            if (derivedKey(points[index - 1]) == derivedKey(points[index]))
            {
                result.error = error(
                    ProjectPartialTrackEditErrorCode::DuplicateIdentity,
                    "Edited partial-track points collide at one frame/track identity",
                    index);
                return result;
            }
        }

        std::vector<std::pair<std::uint64_t, std::uint64_t>> frameKeys;
        frameKeys.reserve(source.frames.size() + points.size());
        for (const auto& frame : source.frames)
            frameKeys.emplace_back(frame.segmentId, frame.sourceFrame);
        for (const auto& point : points)
            frameKeys.emplace_back(point.segmentId, point.sourceFrame);
        std::ranges::sort(frameKeys);
        frameKeys.erase(
            std::unique(frameKeys.begin(), frameKeys.end()),
            frameKeys.end());

        result.materialized.asset.sampleRate = source.sampleRate;
        result.materialized.asset.sourceFrameCount =
            source.sourceFrameCount;
        result.materialized.asset.frames.reserve(frameKeys.size());
        result.materialized.pointInterpolations.reserve(points.size());
        result.materialized.pointIdentities.reserve(points.size());
        auto point = points.begin();
        for (const auto& [segmentId, sourceFrame] : frameKeys)
        {
            PartialTrackFrame frame;
            frame.segmentId = segmentId;
            frame.sourceFrame = sourceFrame;
            while (point != points.end()
                   && point->segmentId == segmentId
                   && point->sourceFrame == sourceFrame)
            {
                frame.points.push_back(point->point);
                result.materialized.pointInterpolations.push_back(
                    point->interpolation);
                result.materialized.pointIdentities.push_back(
                    point->identity);
                ++point;
            }
            result.materialized.asset.frames.push_back(std::move(frame));
        }
        if (const auto materializedError = validatePartialTrackAsset(
                result.materialized.asset))
        {
            result.error = error(
                ProjectPartialTrackEditErrorCode::InvalidPoint,
                "Materialized partial-track asset is invalid: "
                    + materializedError->message);
            result.materialized = {};
            return result;
        }

        std::unordered_set<std::uint64_t> trackIds;
        trackIds.reserve(std::min(
            maximumPartialTrackAssetTracks,
            points.size()));
        for (const auto& derived : points)
            trackIds.insert(derived.point.trackId);
        for (std::size_t index = 0;
             index < edits.trackStates.size();
             ++index)
        {
            if (!trackIds.contains(edits.trackStates[index].trackId))
            {
                result.error = error(
                    ProjectPartialTrackEditErrorCode::MissingTrack,
                    "Partial-track mute/solo state references a missing materialized track",
                    index);
                result.materialized = {};
                return result;
            }
        }
        result.materialized.trackStates = edits.trackStates;
    }
    catch (const std::bad_alloc&)
    {
        result.materialized = {};
        result.error = error(
            ProjectPartialTrackEditErrorCode::AllocationFailed,
            "Partial-track materialization allocation failed");
    }
    return result;
}

ProjectPartialTrackEditBatchResult applyProjectPartialTrackEditBatch(
    const PartialTrackAsset& source,
    const ProjectPartialTrackEditSet* current,
    const ProjectPartialTrackEditBatch& batch)
{
    ProjectPartialTrackEditBatchResult result;
    const auto fail = [&](ProjectPartialTrackEditErrorCode code,
                          std::string message,
                          std::size_t index = 0) {
        result.edits = {};
        result.error = error(code, std::move(message), index);
        return std::move(result);
    };
    if (!isValidProjectPartialTrackEditIdentifier(batch.editSetId)
        || !boundedAssetId(batch.assetId)
        || batch.assetGeneration == 0
        || batch.assetGeneration > maximumProjectAssetJsonInteger
        || !isCanonicalProjectAssetContentHash(batch.sourceContentHash))
    {
        return fail(
            ProjectPartialTrackEditErrorCode::InvalidSourceAuthority,
            "Partial-track edit batch authority is invalid");
    }
    if (batch.pointMutations.empty()
        && batch.trackStateMutations.empty())
    {
        return fail(
            ProjectPartialTrackEditErrorCode::InvalidOrder,
            "Partial-track edit batch is empty");
    }
    if (batch.pointMutations.size()
            > maximumProjectPartialTrackBatchPointMutations
        || batch.trackStateMutations.size()
            > maximumProjectPartialTrackStates)
    {
        return fail(
            ProjectPartialTrackEditErrorCode::LimitExceeded,
            "Partial-track edit batch exceeds its fixed mutation limits");
    }

    try
    {
        if (current != nullptr)
        {
            if (current->editSetId != batch.editSetId
                || current->assetId != batch.assetId
                || current->assetGeneration != batch.assetGeneration
                || current->sourceContentHash != batch.sourceContentHash)
            {
                return fail(
                    ProjectPartialTrackEditErrorCode::InvalidSourceAuthority,
                    "Partial-track edit batch does not match the current edit authority");
            }
            result.edits = *current;
        }
        else
        {
            result.edits.editSetId = batch.editSetId;
            result.edits.assetId = batch.assetId;
            result.edits.assetGeneration = batch.assetGeneration;
            result.edits.sourceContentHash = batch.sourceContentHash;
        }

        const auto materialized = materializeProjectPartialTrackEdits(
            source, result.edits);
        if (!materialized.ok())
        {
            result.error = materialized.error;
            result.edits = {};
            return result;
        }

        struct CurrentPoint
        {
            MaterializedPartialTrackPointIdentity identity;
        };
        std::vector<CurrentPoint> currentPoints;
        currentPoints.reserve(
            materialized.materialized.pointIdentities.size());
        for (const auto& identity
             : materialized.materialized.pointIdentities)
        {
            currentPoints.push_back({identity});
        }
        std::ranges::sort(
            currentPoints,
            [](const auto& left, const auto& right) {
                return identityLess(left.identity, right.identity);
            });
        const auto currentPoint = [&](const auto& identity) {
            const auto found = std::lower_bound(
                currentPoints.begin(), currentPoints.end(), identity,
                [](const auto& point, const auto& value) {
                    return identityLess(point.identity, value);
                });
            return found != currentPoints.end()
                && found->identity == identity;
        };

        std::vector<MaterializedPartialTrackPointIdentity> targets;
        targets.reserve(batch.pointMutations.size());
        for (std::size_t index = 0;
             index < batch.pointMutations.size(); ++index)
        {
            const auto& mutation = batch.pointMutations[index];
            if (!validMutationKind(mutation.kind)
                || !validMutationIdentity(mutation.target))
            {
                return fail(
                    ProjectPartialTrackEditErrorCode::InvalidIdentity,
                    "Partial-track point mutation kind is invalid",
                    index);
            }
            targets.push_back(mutation.target);
        }
        std::ranges::sort(targets, identityLess);
        if (std::ranges::adjacent_find(targets) != targets.end())
        {
            return fail(
                ProjectPartialTrackEditErrorCode::DuplicateIdentity,
                "Partial-track edit batch mutates one point more than once");
        }

        for (std::size_t index = 0;
             index < batch.pointMutations.size(); ++index)
        {
            const auto& mutation = batch.pointMutations[index];
            const auto exists = currentPoint(mutation.target);
            if (mutation.kind
                    == ProjectPartialTrackPointMutationKind::Insert)
            {
                if (mutation.target.origin
                        != MaterializedPartialTrackPointOrigin::Inserted
                    || exists)
                {
                    return fail(
                        ProjectPartialTrackEditErrorCode::DuplicateIdentity,
                        "Inserted partial-track point identity already exists",
                        index);
                }
                result.edits.insertedPoints.push_back({
                    mutation.target.insertedEditId,
                    mutation.segmentId,
                    mutation.outputFrame,
                    mutation.output,
                    mutation.interpolation,
                });
                continue;
            }
            if (!exists)
            {
                return fail(
                    ProjectPartialTrackEditErrorCode::MissingSourcePoint,
                    "Partial-track edit target is stale or missing",
                    index);
            }
            if (mutation.target.origin
                == MaterializedPartialTrackPointOrigin::Source)
            {
                const auto found = std::ranges::lower_bound(
                    result.edits.pointOverrides,
                    mutation.target.source,
                    {},
                    &ProjectPartialTrackPointOverride::source);
                ProjectPartialTrackPointOverride replacement;
                replacement.source = mutation.target.source;
                replacement.interpolation = mutation.interpolation;
                replacement.excluded = mutation.kind
                    == ProjectPartialTrackPointMutationKind::Delete;
                if (!replacement.excluded)
                {
                    replacement.outputFrame = mutation.outputFrame;
                    replacement.output = mutation.output;
                }
                if (found == result.edits.pointOverrides.end()
                    || found->source != mutation.target.source)
                {
                    result.edits.pointOverrides.insert(
                        found, std::move(replacement));
                }
                else
                {
                    *found = std::move(replacement);
                }
                continue;
            }
            const auto found = std::ranges::lower_bound(
                result.edits.insertedPoints,
                mutation.target.insertedEditId,
                {},
                &ProjectPartialTrackInsertedPoint::editId);
            if (found == result.edits.insertedPoints.end()
                || found->editId != mutation.target.insertedEditId)
            {
                return fail(
                    ProjectPartialTrackEditErrorCode::MissingSourcePoint,
                    "Inserted partial-track edit target is stale or missing",
                    index);
            }
            if (mutation.kind
                == ProjectPartialTrackPointMutationKind::Delete)
            {
                result.edits.insertedPoints.erase(found);
            }
            else
            {
                found->segmentId = mutation.segmentId;
                found->sourceFrame = mutation.outputFrame;
                found->point = mutation.output;
                found->interpolation = mutation.interpolation;
            }
        }

        std::vector<std::uint64_t> stateTargets;
        stateTargets.reserve(batch.trackStateMutations.size());
        for (const auto& mutation : batch.trackStateMutations)
            stateTargets.push_back(mutation.trackId);
        std::ranges::sort(stateTargets);
        if ((!stateTargets.empty() && stateTargets.front() == 0)
            || std::ranges::adjacent_find(stateTargets)
                != stateTargets.end())
        {
            return fail(
                ProjectPartialTrackEditErrorCode::DuplicateIdentity,
                "Partial-track state mutation identities are invalid or duplicated");
        }
        for (const auto& mutation : batch.trackStateMutations)
        {
            const auto found = std::ranges::lower_bound(
                result.edits.trackStates,
                mutation.trackId,
                {},
                &ProjectPartialTrackState::trackId);
            if (!mutation.muted && !mutation.solo)
            {
                if (found != result.edits.trackStates.end()
                    && found->trackId == mutation.trackId)
                {
                    result.edits.trackStates.erase(found);
                }
                continue;
            }
            ProjectPartialTrackState replacement{
                mutation.trackId, mutation.muted, mutation.solo};
            if (found == result.edits.trackStates.end()
                || found->trackId != mutation.trackId)
            {
                result.edits.trackStates.insert(
                    found, std::move(replacement));
            }
            else
            {
                *found = std::move(replacement);
            }
        }

        std::ranges::sort(
            result.edits.pointOverrides,
            {},
            &ProjectPartialTrackPointOverride::source);
        std::ranges::sort(
            result.edits.insertedPoints,
            {},
            &ProjectPartialTrackInsertedPoint::editId);
        std::ranges::sort(
            result.edits.trackStates,
            {},
            &ProjectPartialTrackState::trackId);
        if (const auto validation = validateProjectPartialTrackEditSet(
                result.edits))
        {
            result.error = validation;
            result.edits = {};
            return result;
        }
        const auto finalMaterialized = materializeProjectPartialTrackEdits(
            source, result.edits);
        if (!finalMaterialized.ok())
        {
            result.error = finalMaterialized.error;
            result.edits = {};
            return result;
        }
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return fail(
            ProjectPartialTrackEditErrorCode::AllocationFailed,
            "Partial-track edit batch allocation failed");
    }
    catch (const std::length_error&)
    {
        return fail(
            ProjectPartialTrackEditErrorCode::LimitExceeded,
            "Partial-track edit batch exceeds supported container capacity");
    }
}
}
