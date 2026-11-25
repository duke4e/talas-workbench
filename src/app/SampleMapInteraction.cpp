#include "talas/app/SampleMapInteraction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace talas::app
{
namespace
{
[[nodiscard]] bool validMapGeometry(
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept
{
    return std::isfinite(viewport.noteStart)
        && std::isfinite(viewport.noteEndExclusive)
        && std::isfinite(viewport.velocityStart)
        && std::isfinite(viewport.velocityEndExclusive)
        && viewport.noteEndExclusive > viewport.noteStart
        && viewport.velocityEndExclusive > viewport.velocityStart
        && std::isfinite(canvas.width)
        && std::isfinite(canvas.height)
        && canvas.width > 0.0
        && canvas.height > 0.0;
}

[[nodiscard]] bool selected(
    std::span<const std::string> selectedRegionIds,
    std::string_view regionId)
{
    return std::ranges::find(selectedRegionIds, regionId)
        != selectedRegionIds.end();
}

[[nodiscard]] std::uint64_t shiftedFrame(
    std::uint64_t frame,
    std::int64_t delta) noexcept
{
    if (delta >= 0)
    {
        const auto amount = static_cast<std::uint64_t>(delta);
        if (amount > std::numeric_limits<std::uint64_t>::max() - frame)
            return std::numeric_limits<std::uint64_t>::max();
        return frame + amount;
    }
    const auto amount = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
    return amount > frame ? 0 : frame - amount;
}
}

double sampleMapNoteBoundaryToPixel(
    double noteBoundary,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept
{
    if (!validMapGeometry(viewport, canvas) || !std::isfinite(noteBoundary))
        return 0.0;
    return (noteBoundary - viewport.noteStart)
        / (viewport.noteEndExclusive - viewport.noteStart)
        * canvas.width;
}

double sampleMapVelocityBoundaryToPixel(
    double velocityBoundary,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept
{
    if (!validMapGeometry(viewport, canvas)
        || !std::isfinite(velocityBoundary))
    {
        return 0.0;
    }
    return (viewport.velocityEndExclusive - velocityBoundary)
        / (viewport.velocityEndExclusive - viewport.velocityStart)
        * canvas.height;
}

std::optional<SampleMapMidiPoint> sampleMapMidiAtPoint(
    double x,
    double y,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept
{
    if (!validMapGeometry(viewport, canvas)
        || !std::isfinite(x) || !std::isfinite(y)
        || x < 0.0 || y < 0.0
        || x >= canvas.width || y >= canvas.height)
    {
        return std::nullopt;
    }

    const auto noteValue = viewport.noteStart
        + x / canvas.width
            * (viewport.noteEndExclusive - viewport.noteStart);
    const auto velocityBoundary = viewport.velocityEndExclusive
        - y / canvas.height
            * (viewport.velocityEndExclusive - viewport.velocityStart);
    const auto note = std::clamp(
        static_cast<int>(std::floor(noteValue)),
        0,
        127);
    const auto velocity = std::clamp(
        static_cast<int>(std::ceil(velocityBoundary) - 1.0),
        1,
        127);
    return SampleMapMidiPoint{
        static_cast<std::uint8_t>(note),
        static_cast<std::uint8_t>(velocity),
    };
}

SampleMapRect sampleMapRegionRect(
    const talas::project::ProjectSampleRegion& region,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept
{
    if (!validMapGeometry(viewport, canvas))
        return {};
    const auto rawLeft = sampleMapNoteBoundaryToPixel(
        region.lowNote,
        viewport,
        canvas);
    const auto rawRight = sampleMapNoteBoundaryToPixel(
        static_cast<double>(region.highNote) + 1.0,
        viewport,
        canvas);
    const auto rawTop = sampleMapVelocityBoundaryToPixel(
        static_cast<double>(region.highVelocity) + 1.0,
        viewport,
        canvas);
    const auto rawBottom = sampleMapVelocityBoundaryToPixel(
        region.lowVelocity,
        viewport,
        canvas);
    const auto left = std::clamp(rawLeft, 0.0, canvas.width);
    const auto right = std::clamp(rawRight, 0.0, canvas.width);
    const auto top = std::clamp(rawTop, 0.0, canvas.height);
    const auto bottom = std::clamp(rawBottom, 0.0, canvas.height);
    return {
        std::min(left, right),
        std::min(top, bottom),
        std::abs(right - left),
        std::abs(bottom - top),
    };
}

SampleMapRegionHit hitTestSampleMapRegion(
    const SampleMapRect& bounds,
    double x,
    double y,
    double edgeTolerance) noexcept
{
    SampleMapRegionHit result;
    if (!std::isfinite(x) || !std::isfinite(y)
        || !std::isfinite(edgeTolerance)
        || bounds.width < 0.0 || bounds.height < 0.0)
    {
        return result;
    }
    const auto tolerance = std::max(0.0, edgeTolerance);
    if (x < bounds.x - tolerance || x > bounds.right() + tolerance
        || y < bounds.y - tolerance || y > bounds.bottom() + tolerance)
    {
        return result;
    }
    result.inside = true;
    result.lowNoteEdge = std::abs(x - bounds.x) <= tolerance;
    result.highNoteEdge = std::abs(x - bounds.right()) <= tolerance;
    result.highVelocityEdge = std::abs(y - bounds.y) <= tolerance;
    result.lowVelocityEdge = std::abs(y - bounds.bottom()) <= tolerance;
    return result;
}

talas::project::ProjectSampleRegion resizeSampleMapRegion(
    const talas::project::ProjectSampleRegion& source,
    SampleMapResizeEdge edge,
    int delta) noexcept
{
    auto region = source;
    const auto shifted = [&](std::uint8_t value) {
        return static_cast<long long>(value)
            + static_cast<long long>(delta);
    };
    switch (edge)
    {
    case SampleMapResizeEdge::LowNote:
        region.lowNote = static_cast<std::uint8_t>(std::clamp<long long>(
            shifted(source.lowNote),
            0,
            std::min<long long>(source.rootNote, source.highNote)));
        break;
    case SampleMapResizeEdge::HighNote:
        region.highNote = static_cast<std::uint8_t>(std::clamp<long long>(
            shifted(source.highNote),
            std::max<long long>(source.rootNote, source.lowNote),
            127));
        break;
    case SampleMapResizeEdge::LowVelocity:
        region.lowVelocity = static_cast<std::uint8_t>(std::clamp<long long>(
            shifted(source.lowVelocity),
            1,
            source.highVelocity));
        break;
    case SampleMapResizeEdge::HighVelocity:
        region.highVelocity = static_cast<std::uint8_t>(std::clamp<long long>(
            shifted(source.highVelocity),
            source.lowVelocity,
            127));
        break;
    }
    return region;
}

SampleMapMoveResult moveSampleMapRegions(
    std::span<const talas::project::ProjectSampleRegion> regions,
    std::span<const std::string> selectedRegionIds,
    int requestedNoteDelta,
    int requestedVelocityDelta)
{
    SampleMapMoveResult result;
    result.regions.assign(regions.begin(), regions.end());

    int minimumNoteDelta = -127;
    int maximumNoteDelta = 127;
    int minimumVelocityDelta = -126;
    int maximumVelocityDelta = 126;
    bool hasSelection = false;
    for (const auto& region : regions)
    {
        if (!selected(selectedRegionIds, region.regionId))
            continue;
        hasSelection = true;
        minimumNoteDelta = std::max(
            minimumNoteDelta,
            -static_cast<int>(region.lowNote));
        maximumNoteDelta = std::min(
            maximumNoteDelta,
            127 - static_cast<int>(region.highNote));
        minimumVelocityDelta = std::max(
            minimumVelocityDelta,
            1 - static_cast<int>(region.lowVelocity));
        maximumVelocityDelta = std::min(
            maximumVelocityDelta,
            127 - static_cast<int>(region.highVelocity));
    }
    if (!hasSelection)
        return result;

    result.appliedNoteDelta = std::clamp(
        requestedNoteDelta,
        minimumNoteDelta,
        maximumNoteDelta);
    result.appliedVelocityDelta = std::clamp(
        requestedVelocityDelta,
        minimumVelocityDelta,
        maximumVelocityDelta);
    for (auto& region : result.regions)
    {
        if (!selected(selectedRegionIds, region.regionId))
            continue;
        region.lowNote = static_cast<std::uint8_t>(
            static_cast<int>(region.lowNote) + result.appliedNoteDelta);
        region.rootNote = static_cast<std::uint8_t>(
            static_cast<int>(region.rootNote) + result.appliedNoteDelta);
        region.highNote = static_cast<std::uint8_t>(
            static_cast<int>(region.highNote) + result.appliedNoteDelta);
        region.lowVelocity = static_cast<std::uint8_t>(
            static_cast<int>(region.lowVelocity)
            + result.appliedVelocityDelta);
        region.highVelocity = static_cast<std::uint8_t>(
            static_cast<int>(region.highVelocity)
            + result.appliedVelocityDelta);
    }
    return result;
}

std::vector<std::string> updateSampleMapSelection(
    std::span<const std::string> currentSelection,
    std::string regionId,
    SampleMapSelectionMode mode)
{
    std::vector<std::string> result(
        currentSelection.begin(),
        currentSelection.end());
    if (regionId.empty())
        return mode == SampleMapSelectionMode::Replace
            ? std::vector<std::string>{}
            : result;

    const auto existing = std::ranges::find(result, regionId);
    switch (mode)
    {
    case SampleMapSelectionMode::Replace:
        return {std::move(regionId)};
    case SampleMapSelectionMode::Add:
        if (existing == result.end())
            result.push_back(std::move(regionId));
        break;
    case SampleMapSelectionMode::Toggle:
        if (existing == result.end())
            result.push_back(std::move(regionId));
        else
            result.erase(existing);
        break;
    }
    return result;
}

std::vector<std::string> reconcileSampleMapSelection(
    const talas::project::ProjectSampleMap& map,
    std::span<const std::string> selectedRegionIds)
{
    std::vector<std::string> result;
    result.reserve(selectedRegionIds.size());
    std::unordered_set<std::string> seen;
    for (const auto& regionId : selectedRegionIds)
    {
        if (!seen.insert(regionId).second)
            continue;
        if (talas::project::findProjectSampleRegion(map, regionId) != nullptr)
            result.push_back(regionId);
    }
    return result;
}

double waveformFrameToPixel(
    double frame,
    const WaveformViewport& viewport,
    double width) noexcept
{
    if (!std::isfinite(frame)
        || !std::isfinite(viewport.startFrame)
        || !std::isfinite(viewport.endFrameExclusive)
        || !std::isfinite(width)
        || width <= 0.0
        || viewport.endFrameExclusive <= viewport.startFrame)
    {
        return 0.0;
    }
    return (frame - viewport.startFrame)
        / (viewport.endFrameExclusive - viewport.startFrame)
        * width;
}

std::uint64_t waveformPixelToFrame(
    double pixel,
    const WaveformViewport& viewport,
    double width) noexcept
{
    if (!std::isfinite(pixel)
        || !std::isfinite(viewport.startFrame)
        || !std::isfinite(viewport.endFrameExclusive)
        || !std::isfinite(width)
        || width <= 0.0
        || viewport.endFrameExclusive <= viewport.startFrame)
    {
        return 0;
    }
    const auto ratio = std::clamp(pixel / width, 0.0, 1.0);
    const auto frame = viewport.startFrame
        + ratio * (viewport.endFrameExclusive - viewport.startFrame);
    const auto maximum = viewport.endFrameExclusive <= 0.0
        ? std::uint64_t{0}
        : static_cast<std::uint64_t>(
              std::floor(viewport.endFrameExclusive));
    return clampWaveformFrame(frame, maximum);
}

WaveformViewport zoomWaveformViewport(
    const WaveformViewport& source,
    double anchorPixel,
    double zoomFactor,
    double width,
    std::uint64_t frameCount) noexcept
{
    if (frameCount == 0)
        return {};
    const auto fullEnd = static_cast<double>(frameCount);
    const auto start = std::clamp(source.startFrame, 0.0, fullEnd);
    const auto end = std::clamp(
        source.endFrameExclusive,
        start,
        fullEnd);
    if (!std::isfinite(zoomFactor) || zoomFactor <= 0.0
        || !std::isfinite(width) || width <= 0.0
        || end <= start)
    {
        return {start, end};
    }

    const auto anchorRatio = std::clamp(anchorPixel / width, 0.0, 1.0);
    const auto anchorFrame = start + anchorRatio * (end - start);
    const auto visibleFrames = std::clamp(
        (end - start) / zoomFactor,
        1.0,
        fullEnd);
    auto nextStart = anchorFrame - anchorRatio * visibleFrames;
    auto nextEnd = nextStart + visibleFrames;
    if (nextStart < 0.0)
    {
        nextEnd -= nextStart;
        nextStart = 0.0;
    }
    if (nextEnd > fullEnd)
    {
        nextStart -= nextEnd - fullEnd;
        nextEnd = fullEnd;
    }
    return {std::max(0.0, nextStart), nextEnd};
}

std::uint64_t clampWaveformFrame(
    double frame,
    std::uint64_t frameCount) noexcept
{
    if (!std::isfinite(frame) || frame <= 0.0)
        return 0;
    const auto maximum = static_cast<long double>(frameCount);
    const auto value = std::min(
        static_cast<long double>(frame),
        maximum);
    return static_cast<std::uint64_t>(std::floor(value + 0.5L));
}

talas::project::ProjectSampleLoop moveWaveformLoopMarker(
    const talas::project::ProjectSampleLoop& source,
    WaveformLoopMarker marker,
    std::int64_t delta,
    std::uint64_t frameCount) noexcept
{
    auto loop = source;
    if (frameCount == 0)
    {
        loop.startFrame = 0;
        loop.endFrameExclusive = 0;
        return loop;
    }
    loop.endFrameExclusive = std::clamp<std::uint64_t>(
        loop.endFrameExclusive,
        1,
        frameCount);
    loop.startFrame = std::min(
        loop.startFrame,
        loop.endFrameExclusive - 1);
    if (marker == WaveformLoopMarker::Start)
    {
        loop.startFrame = std::min(
            shiftedFrame(loop.startFrame, delta),
            loop.endFrameExclusive - 1);
    }
    else
    {
        loop.endFrameExclusive = std::clamp(
            shiftedFrame(loop.endFrameExclusive, delta),
            loop.startFrame + 1,
            frameCount);
    }
    return loop;
}

WaveformLoopProjection projectWaveformLoop(
    const talas::project::ProjectSampleLoop& loop,
    const WaveformViewport& viewport,
    double width) noexcept
{
    return {
        waveformFrameToPixel(
            static_cast<double>(loop.startFrame),
            viewport,
            width),
        waveformFrameToPixel(
            static_cast<double>(loop.endFrameExclusive),
            viewport,
            width),
        loop.mode == talas::project::ProjectSampleLoopMode::Alternate,
    };
}
}
