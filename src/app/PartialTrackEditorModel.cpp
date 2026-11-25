#include "talas/app/PartialTrackEditorModel.h"

#include "talas/app/WorkbenchGraphViewportIndex.h"
#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace talas::app
{
namespace
{
constexpr double frequencyCoordinateScale = 1000.0;
constexpr std::size_t maximumEditorPixels = 1U << 20U;

[[nodiscard]] std::int64_t frequencyCoordinate(double value) noexcept
{
    return static_cast<std::int64_t>(std::llround(
        value * frequencyCoordinateScale));
}

[[nodiscard]] bool validFilter(
    const PartialTrackEditorFilter& filter) noexcept
{
    return std::isfinite(filter.minimumAmplitude)
        && filter.minimumAmplitude >= 0.0
        && filter.trackIds.size()
            <= talas::project::maximumPartialTrackAssetTracks
        && std::ranges::is_sorted(filter.trackIds)
        && std::adjacent_find(
               filter.trackIds.begin(), filter.trackIds.end())
            == filter.trackIds.end()
        && std::ranges::none_of(
            filter.trackIds,
            [](std::uint64_t value) { return value == 0; });
}

[[nodiscard]] std::string authorityText(
    const talas::project::ProjectAsset& asset)
{
    return asset.id + " · generation "
        + std::to_string(asset.generation) + " · SHA-256 "
        + asset.contentHash.substr(0, 12);
}

[[nodiscard]] bool identityLess(
    const talas::project::MaterializedPartialTrackPointIdentity& left,
    const talas::project::MaterializedPartialTrackPointIdentity& right)
    noexcept
{
    if (left.origin != right.origin)
        return left.origin < right.origin;
    if (left.origin
        == talas::project::MaterializedPartialTrackPointOrigin::Source)
    {
        return left.source < right.source;
    }
    return left.insertedEditId < right.insertedEditId;
}
}

struct PartialTrackEditorModel::PreparedPoint
{
    talas::project::MaterializedPartialTrackPointIdentity identity;
    std::uint64_t trackId = 0;
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    double amplitude = 0.0;
    double frequencyHz = 0.0;
    double bandwidthHz = 0.0;
    talas::project::PartialTrackPointInterpolation interpolation =
        talas::project::PartialTrackPointInterpolation::Linear;
};

struct PartialTrackEditorModel::PreparedSegment
{
    std::size_t startPointIndex = 0;
    std::size_t endPointIndex = 0;
};

class PartialTrackEditorModel::Index
{
public:
    void rebuild(
        std::span<const PreparedPoint> points,
        std::span<const PreparedSegment> segments)
    {
        pointCount_ = points.size();
        rects_.clear();
        rects_.reserve(points.size() + segments.size());
        for (const auto& point : points)
        {
            rects_.push_back({
                static_cast<std::int64_t>(point.sourceFrame),
                frequencyCoordinate(point.frequencyHz),
                1,
                1,
            });
        }
        for (const auto& segment : segments)
        {
            const auto& start = points[segment.startPointIndex];
            const auto& end = points[segment.endPointIndex];
            const auto minFrame = std::min(
                start.sourceFrame, end.sourceFrame);
            const auto maxFrame = std::max(
                start.sourceFrame, end.sourceFrame);
            const auto startFrequency =
                frequencyCoordinate(start.frequencyHz);
            const auto endFrequency =
                frequencyCoordinate(end.frequencyHz);
            const auto minFrequency = std::min(
                startFrequency, endFrequency);
            const auto maxFrequency = std::max(
                startFrequency, endFrequency);
            rects_.push_back({
                static_cast<std::int64_t>(minFrame),
                minFrequency,
                static_cast<std::int64_t>(maxFrame - minFrame + 1U),
                maxFrequency - minFrequency + 1,
            });
        }
        index_.rebuild(rects_);
    }

    void query(
        const PartialTrackEditorViewport& viewport,
        std::vector<std::size_t>& result)
    {
        const auto minimumFrequency = frequencyCoordinate(
            viewport.minimumFrequencyHz);
        const auto maximumFrequency = frequencyCoordinate(
            viewport.maximumFrequencyHz);
        index_.query({
            static_cast<std::int64_t>(viewport.startFrame),
            minimumFrequency,
            static_cast<std::int64_t>(
                viewport.endFrame - viewport.startFrame),
            maximumFrequency - minimumFrequency + 1,
        }, result);
    }

    [[nodiscard]] std::size_t pointCount() const noexcept
    {
        return pointCount_;
    }

    [[nodiscard]] std::size_t lastCandidateCount() const noexcept
    {
        return index_.lastCandidateCount();
    }

private:
    WorkbenchGraphViewportIndex index_;
    std::vector<WorkbenchGraphViewportRect> rects_;
    std::size_t pointCount_ = 0;
};

PartialTrackEditorModel::PartialTrackEditorModel() = default;
PartialTrackEditorModel::~PartialTrackEditorModel() = default;
PartialTrackEditorModel::PartialTrackEditorModel(
    PartialTrackEditorModel&&) noexcept = default;
PartialTrackEditorModel& PartialTrackEditorModel::operator=(
    PartialTrackEditorModel&&) noexcept = default;

void PartialTrackEditorModel::setLoading()
{
    state_ = PartialTrackEditorState::Loading;
    message_ = "Loading partial tracks";
    manifest_ = {};
    materialized_ = {};
    points_.clear();
    segments_.clear();
    trackIds_.clear();
    trackPointCounts_.clear();
    identityOrder_.clear();
    candidateIndices_.clear();
    selection_.reset();
    index_.reset();
    lastCandidateCount_ = 0;
    maximumRenderedSegmentTrackCount_ = 0;
}

void PartialTrackEditorModel::setError(std::string message)
{
    setLoading();
    state_ = PartialTrackEditorState::Error;
    message_ = std::move(message);
}

bool PartialTrackEditorModel::setSource(
    const PartialTrackEditorSource& source)
{
    auto fail = [&](std::string message) {
        state_ = PartialTrackEditorState::Error;
        message_ = std::move(message);
        return false;
    };
    if (source.source == nullptr
        || talas::project::validateProjectAssetManifest(source.manifest)
        || source.manifest.kind
            != talas::project::ProjectAssetKind::PartialSpectrum
        || source.manifest.generation == 0
        || source.manifest.sampleRate != source.source->sampleRate
        || source.manifest.frameCount != source.source->sourceFrameCount
        || source.manifest.encoding
            != talas::project::partialTrackAssetEncoding
        || !talas::project::isCanonicalProjectAssetContentHash(
            source.manifest.contentHash))
    {
        return fail("Partial-track source authority is invalid");
    }
    try
    {
        talas::project::ProjectPartialTrackEditSet defaultEdits;
        const talas::project::ProjectPartialTrackEditSet* edits = nullptr;
        if (source.edits.has_value())
        {
            if (!talas::project::partialTrackEditSetMatchesAsset(
                    *source.edits, source.manifest))
            {
                return fail("Partial-track edit authority is stale");
            }
            edits = &*source.edits;
        }
        else
        {
            defaultEdits.editSetId = "partial_view";
            defaultEdits.assetId = source.manifest.id;
            defaultEdits.assetGeneration = source.manifest.generation;
            defaultEdits.sourceContentHash = source.manifest.contentHash;
            edits = &defaultEdits;
        }
        auto materialized = talas::project::materializeProjectPartialTrackEdits(
            *source.source, *edits);
        if (!materialized.ok())
        {
            return fail(materialized.error.has_value()
                ? materialized.error->message
                : "Partial-track edit materialization failed");
        }

        if (materialized.materialized.pointIdentities.size()
                != materialized.materialized.pointInterpolations.size())
        {
            return fail("Partial-track materialization identity counts disagree");
        }
        std::vector<PreparedPoint> points;
        points.reserve(
            materialized.materialized.pointIdentities.size());
        std::size_t flatIndex = 0;
        for (const auto& frame : materialized.materialized.asset.frames)
        {
            for (const auto& point : frame.points)
            {
                points.push_back({
                    materialized.materialized.pointIdentities[flatIndex],
                    point.trackId,
                    frame.segmentId,
                    frame.sourceFrame,
                    point.amplitude,
                    point.frequencyHz,
                    point.bandwidthHz,
                    materialized.materialized
                        .pointInterpolations[flatIndex],
                });
                ++flatIndex;
            }
        }
        std::vector<PreparedSegment> segments;
        segments.reserve(points.size());
        std::unordered_map<std::uint64_t, std::size_t> lastByTrack;
        std::unordered_map<std::uint64_t, std::size_t> countByTrack;
        const auto hasSolo = std::ranges::any_of(
            materialized.materialized.trackStates,
            &talas::project::ProjectPartialTrackState::solo);
        std::unordered_set<std::uint64_t> mutedTrackIds;
        std::unordered_set<std::uint64_t> soloTrackIds;
        mutedTrackIds.reserve(
            materialized.materialized.trackStates.size());
        soloTrackIds.reserve(
            materialized.materialized.trackStates.size());
        for (const auto& state : materialized.materialized.trackStates)
        {
            if (state.muted)
                mutedTrackIds.insert(state.trackId);
            if (state.solo && !state.muted)
                soloTrackIds.insert(state.trackId);
        }
        const auto isRenderedTrack = [&](std::uint64_t trackId) {
            return !mutedTrackIds.contains(trackId)
                && (!hasSolo || soloTrackIds.contains(trackId));
        };
        std::unordered_set<std::uint64_t> renderedTrackIds;
        renderedTrackIds.reserve(std::min(
            points.size(),
            talas::project::maximumPartialTrackAssetTracks));
        lastByTrack.reserve(std::min(
            points.size(),
            talas::project::maximumPartialTrackAssetTracks));
        countByTrack.reserve(std::min(
            points.size(),
            talas::project::maximumPartialTrackAssetTracks));
        std::uint64_t activeSegment = 0;
        std::size_t maximumRenderedSegmentTrackCount = 0;
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            if (points[index].segmentId != activeSegment)
            {
                lastByTrack.clear();
                renderedTrackIds.clear();
                activeSegment = points[index].segmentId;
            }
            const auto found = lastByTrack.find(points[index].trackId);
            if (found != lastByTrack.end())
                segments.push_back({found->second, index});
            lastByTrack[points[index].trackId] = index;
            if (isRenderedTrack(points[index].trackId))
            {
                renderedTrackIds.insert(points[index].trackId);
                maximumRenderedSegmentTrackCount = std::max(
                    maximumRenderedSegmentTrackCount,
                    renderedTrackIds.size());
            }
            ++countByTrack[points[index].trackId];
        }
        std::vector<std::uint64_t> trackIds;
        trackIds.reserve(countByTrack.size());
        for (const auto& [trackId, unused] : countByTrack)
        {
            (void) unused;
            trackIds.push_back(trackId);
        }
        std::ranges::sort(trackIds);
        std::vector<std::size_t> trackPointCounts;
        trackPointCounts.reserve(trackIds.size());
        for (const auto trackId : trackIds)
            trackPointCounts.push_back(countByTrack.at(trackId));
        std::vector<std::size_t> identityOrder(points.size());
        std::iota(identityOrder.begin(), identityOrder.end(), 0U);
        std::ranges::sort(
            identityOrder,
            [&](std::size_t left, std::size_t right) {
                return identityLess(
                    points[left].identity,
                    points[right].identity);
            });
        auto index = std::make_unique<Index>();
        index->rebuild(points, segments);
        std::vector<std::size_t> candidates;
        candidates.reserve(std::min<std::size_t>(
            points.size() + segments.size(),
            maximumPartialTrackEditorVisibleRows * 16U));

        manifest_ = source.manifest;
        materialized_ = std::move(materialized.materialized);
        points_ = std::move(points);
        segments_ = std::move(segments);
        trackIds_ = std::move(trackIds);
        trackPointCounts_ = std::move(trackPointCounts);
        identityOrder_ = std::move(identityOrder);
        candidateIndices_ = std::move(candidates);
        maximumRenderedSegmentTrackCount_ =
            maximumRenderedSegmentTrackCount;
        index_ = std::move(index);
        state_ = points_.empty()
            ? PartialTrackEditorState::Empty
            : PartialTrackEditorState::Ready;
        message_ = points_.empty()
            ? "No partial tracks"
            : "Partial tracks ready";
        if (selection_.has_value())
            (void) setSelection(*selection_);
        return true;
    }
    catch (...)
    {
        return fail("Partial-track editor preparation allocation failed");
    }
}

std::size_t
PartialTrackEditorModel::maximumRenderedSegmentTrackCount() const noexcept
{
    return maximumRenderedSegmentTrackCount_;
}

bool PartialTrackEditorModel::setFilter(
    PartialTrackEditorFilter filter)
{
    if (!validFilter(filter))
        return false;
    filter_ = std::move(filter);
    return true;
}

bool PartialTrackEditorModel::setSelection(
    PartialTrackEditorSelection selection)
{
    bool exists = false;
    if (selection.kind == PartialTrackEditorItemKind::Point)
    {
        const auto found = std::lower_bound(
            identityOrder_.begin(),
            identityOrder_.end(),
            selection.point,
            [&](std::size_t index,
                const talas::project::MaterializedPartialTrackPointIdentity& value) {
                return identityLess(points_[index].identity, value);
            });
        exists = found != identityOrder_.end()
            && points_[*found].identity == selection.point;
        if (exists)
            selection.trackId = points_[*found].trackId;
    }
    else
    {
        exists = selection.trackId != 0
            && std::ranges::binary_search(trackIds_, selection.trackId);
    }
    if (!exists)
    {
        selection_.reset();
        return false;
    }
    selection_ = std::move(selection);
    return true;
}

void PartialTrackEditorModel::clearSelection() noexcept
{
    selection_.reset();
}

PartialTrackEditorProjection PartialTrackEditorModel::project(
    const PartialTrackEditorViewport& viewport)
{
    PartialTrackEditorProjection result;
    result.state = state_;
    result.message = message_;
    if (state_ == PartialTrackEditorState::Loading
        || state_ == PartialTrackEditorState::Error)
    {
        return result;
    }
    result.sourceAuthorityText = authorityText(manifest_);
    result.qualityText =
        "Partial Tracks v1 · linear/step trajectories · deterministic bandwidth-noise synthesis on";
    result.latencyText = "Prepared additive playback · 0 frames latency";
    if (state_ == PartialTrackEditorState::Empty)
        return result;
    if (viewport.endFrame <= viewport.startFrame
        || viewport.endFrame
            > talas::project::maximumPartialTrackSourceFrames + 1U
        || !std::isfinite(viewport.minimumFrequencyHz)
        || !std::isfinite(viewport.maximumFrequencyHz)
        || viewport.minimumFrequencyHz <= 0.0
        || viewport.maximumFrequencyHz
            <= viewport.minimumFrequencyHz
        || viewport.maximumFrequencyHz
            > materialized_.asset.sampleRate * 0.5
        || viewport.pixelWidth == 0
        || viewport.pixelHeight == 0
        || viewport.pixelWidth > maximumEditorPixels
        || viewport.pixelHeight > maximumEditorPixels
        || viewport.visibleTrackRows == 0
        || viewport.visibleTrackRows
            > maximumPartialTrackEditorVisibleRows)
    {
        result.state = PartialTrackEditorState::Error;
        result.message = "Partial-track viewport is invalid";
        return result;
    }

    const auto timeTickCount = std::min<std::size_t>(
        maximumPartialTrackEditorRulerTicks,
        std::max<std::size_t>(2, viewport.pixelWidth / 120));
    result.timeTicks.reserve(timeTickCount);
    for (std::size_t tick = 0; tick < timeTickCount; ++tick)
    {
        const auto ratio = timeTickCount == 1
            ? 0.0
            : static_cast<double>(tick)
                / static_cast<double>(timeTickCount - 1);
        result.timeTicks.push_back({
            static_cast<double>(viewport.startFrame)
                + ratio * static_cast<double>(
                    viewport.endFrame - viewport.startFrame),
            ratio * static_cast<double>(viewport.pixelWidth),
        });
    }
    const auto frequencyTickCount = std::min<std::size_t>(
        maximumPartialTrackEditorRulerTicks,
        std::max<std::size_t>(2, viewport.pixelHeight / 80));
    result.frequencyTicks.reserve(frequencyTickCount);
    for (std::size_t tick = 0; tick < frequencyTickCount; ++tick)
    {
        const auto ratio = frequencyTickCount == 1
            ? 0.0
            : static_cast<double>(tick)
                / static_cast<double>(frequencyTickCount - 1);
        result.frequencyTicks.push_back({
            std::lerp(
                viewport.maximumFrequencyHz,
                viewport.minimumFrequencyHz,
                ratio),
            ratio * static_cast<double>(viewport.pixelHeight),
        });
    }

    const auto isTrackAllowed = [&](std::uint64_t trackId) {
        if (!filter_.trackIds.empty()
            && !std::ranges::binary_search(filter_.trackIds, trackId))
            return false;
        const auto state = std::ranges::find(
            materialized_.trackStates,
            trackId,
            &talas::project::ProjectPartialTrackState::trackId);
        const auto muted = state != materialized_.trackStates.end()
            && state->muted;
        const auto solo = state != materialized_.trackStates.end()
            && state->solo;
        return (filter_.includeMuted || !muted)
            && (!filter_.soloOnly || solo);
    };
    std::vector<std::uint64_t> filteredTracks;
    filteredTracks.reserve(trackIds_.size());
    std::ranges::copy_if(
        trackIds_,
        std::back_inserter(filteredTracks),
        isTrackAllowed);
    const auto rowStart = std::min(
        viewport.firstTrackRow, filteredTracks.size());
    const auto rowEnd = std::min(
        filteredTracks.size(),
        rowStart + std::min(
            viewport.visibleTrackRows,
            filteredTracks.size() - rowStart));
    if (rowStart < rowEnd)
    {
        result.rows.reserve(rowEnd - rowStart);
        for (std::size_t row = rowStart;
             row < rowEnd; ++row)
        {
            const auto trackId = filteredTracks[row];
            const auto state = std::ranges::find(
                materialized_.trackStates,
                trackId,
                &talas::project::ProjectPartialTrackState::trackId);
            result.rows.push_back({
                trackId,
                trackPointCounts_[static_cast<std::size_t>(
                    std::ranges::lower_bound(trackIds_, trackId)
                    - trackIds_.begin())],
                state != materialized_.trackStates.end() && state->muted,
                state != materialized_.trackStates.end() && state->solo,
                selection_.has_value()
                    && selection_->kind == PartialTrackEditorItemKind::Track
                    && selection_->trackId == trackId,
            });
        }
    }

    index_->query(viewport, candidateIndices_);
    lastCandidateCount_ = index_->lastCandidateCount();
    result.viewportCandidateCount = lastCandidateCount_;
    const auto xForFrame = [&](std::uint64_t frame) {
        return static_cast<double>(frame - viewport.startFrame)
            * static_cast<double>(viewport.pixelWidth)
            / static_cast<double>(
                viewport.endFrame - viewport.startFrame);
    };
    const auto yForFrequency = [&](double frequency) {
        return (viewport.maximumFrequencyHz - frequency)
            * static_cast<double>(viewport.pixelHeight)
            / (viewport.maximumFrequencyHz
                - viewport.minimumFrequencyHz);
    };
    const auto rowVisible = [&](std::uint64_t trackId) {
        return std::binary_search(
            filteredTracks.begin()
                + static_cast<std::ptrdiff_t>(rowStart),
            filteredTracks.begin()
                + static_cast<std::ptrdiff_t>(rowEnd),
            trackId);
    };
    result.points.reserve(candidateIndices_.size());
    result.segments.reserve(candidateIndices_.size());
    for (const auto candidate : candidateIndices_)
    {
        if (candidate < points_.size())
        {
            const auto& point = points_[candidate];
            if (!rowVisible(point.trackId)
                || point.amplitude < filter_.minimumAmplitude)
                continue;
            const auto selected = selection_.has_value()
                && selection_->kind == PartialTrackEditorItemKind::Point
                && selection_->trackId == point.trackId
                && selection_->point == point.identity;
            result.points.push_back({
                point.identity,
                point.trackId,
                point.segmentId,
                point.sourceFrame,
                point.amplitude,
                point.frequencyHz,
                point.bandwidthHz,
                point.interpolation,
                xForFrame(point.sourceFrame),
                yForFrequency(point.frequencyHz),
                selected,
            });
            continue;
        }
        const auto segmentIndex = candidate - points_.size();
        if (segmentIndex >= segments_.size())
            continue;
        const auto& segment = segments_[segmentIndex];
        const auto& start = points_[segment.startPointIndex];
        const auto& end = points_[segment.endPointIndex];
        if (!rowVisible(start.trackId)
            || std::max(start.amplitude, end.amplitude)
                < filter_.minimumAmplitude)
            continue;
        result.segments.push_back({
            start.identity,
            end.identity,
            start.trackId,
            start.interpolation,
            xForFrame(start.sourceFrame),
            yForFrequency(start.frequencyHz),
            xForFrame(end.sourceFrame),
            yForFrequency(end.frequencyHz),
        });
    }

    result.commandMask = partialTrackEditorCommandBit(
        PartialTrackEditorCommand::Select)
        | partialTrackEditorCommandBit(
            PartialTrackEditorCommand::InsertPoint);
    if (selection_.has_value())
    {
        result.commandMask |= partialTrackEditorCommandBit(
            PartialTrackEditorCommand::ClearSelection);
        if (selection_->kind == PartialTrackEditorItemKind::Point)
        {
            if (std::ranges::any_of(
                    result.points,
                    [](const auto& point) { return point.selected; }))
            {
                result.commandMask |= partialTrackEditorCommandBit(
                    PartialTrackEditorCommand::MovePoint)
                    | partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::DeletePoint)
                    | partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::SetStepInterpolation)
                    | partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::SetLinearInterpolation)
                    | partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::RetrackPoint);
            }
        }
        else
        {
            const auto selectedRow = std::ranges::find_if(
                result.rows,
                [](const auto& row) { return row.selected; });
            if (selectedRow != result.rows.end())
            {
                result.commandMask |= partialTrackEditorCommandBit(
                    PartialTrackEditorCommand::MuteTrack)
                    | partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::SoloTrack);
                const auto hasVisibleRegion = std::ranges::any_of(
                    result.points,
                    [&](const auto& point) {
                        return point.trackId == selectedRow->trackId;
                    });
                if (hasVisibleRegion)
                {
                    result.commandMask |= partialTrackEditorCommandBit(
                        PartialTrackEditorCommand::TransformVisibleRegion)
                        | partialTrackEditorCommandBit(
                            PartialTrackEditorCommand::SplitVisibleRegion);
                    if (selectedRow != result.rows.begin())
                    {
                        result.commandMask |= partialTrackEditorCommandBit(
                            PartialTrackEditorCommand::MergeVisibleRegion);
                    }
                }
            }
        }
    }
    result.selectionRetained = selection_.has_value();
    return result;
}

PartialTrackEditorState PartialTrackEditorModel::state() const noexcept
{
    return state_;
}

std::size_t PartialTrackEditorModel::indexedPointCount() const noexcept
{
    return points_.size();
}

std::size_t PartialTrackEditorModel::indexedSegmentCount() const noexcept
{
    return segments_.size();
}

std::size_t PartialTrackEditorModel::lastCandidateCount() const noexcept
{
    return lastCandidateCount_;
}

const talas::project::ProjectAsset* editedSpectrumAsset(
    const talas::project::ProjectDocument& document, const std::optional<std::string>& opened)
{
    const talas::project::ProjectAsset* first = nullptr;
    for (const auto& asset : document.assets)
    {
        if (asset.kind != talas::project::ProjectAssetKind::PartialSpectrum)
            continue;
        if (opened.has_value() && asset.id == *opened)
            return &asset;
        if (first == nullptr)
            first = &asset;
    }
    return first;
}
}
