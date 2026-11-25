#pragma once

#include "talas/project/ProjectSampleMaps.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
struct SampleMapViewport
{
    double noteStart = 0.0;
    double noteEndExclusive = 128.0;
    double velocityStart = 1.0;
    double velocityEndExclusive = 128.0;

    bool operator==(const SampleMapViewport&) const = default;
};

struct SampleMapCanvas
{
    double width = 0.0;
    double height = 0.0;

    bool operator==(const SampleMapCanvas&) const = default;
};

struct SampleMapMidiPoint
{
    std::uint8_t note = 0;
    std::uint8_t velocity = 1;

    bool operator==(const SampleMapMidiPoint&) const = default;
};

struct SampleMapRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double right() const noexcept
    {
        return x + width;
    }

    [[nodiscard]] double bottom() const noexcept
    {
        return y + height;
    }

    bool operator==(const SampleMapRect&) const = default;
};

struct SampleMapRegionHit
{
    bool inside = false;
    bool lowNoteEdge = false;
    bool highNoteEdge = false;
    bool lowVelocityEdge = false;
    bool highVelocityEdge = false;

    [[nodiscard]] bool anyEdge() const noexcept
    {
        return lowNoteEdge || highNoteEdge
            || lowVelocityEdge || highVelocityEdge;
    }

    bool operator==(const SampleMapRegionHit&) const = default;
};

enum class SampleMapResizeEdge
{
    LowNote,
    HighNote,
    LowVelocity,
    HighVelocity,
};

enum class SampleMapSelectionMode
{
    Replace,
    Add,
    Toggle,
};

struct SampleMapMoveResult
{
    std::vector<talas::project::ProjectSampleRegion> regions;
    int appliedNoteDelta = 0;
    int appliedVelocityDelta = 0;
};

[[nodiscard]] double sampleMapNoteBoundaryToPixel(
    double noteBoundary,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept;

[[nodiscard]] double sampleMapVelocityBoundaryToPixel(
    double velocityBoundary,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept;

[[nodiscard]] std::optional<SampleMapMidiPoint> sampleMapMidiAtPoint(
    double x,
    double y,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept;

[[nodiscard]] SampleMapRect sampleMapRegionRect(
    const talas::project::ProjectSampleRegion& region,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas) noexcept;

[[nodiscard]] SampleMapRegionHit hitTestSampleMapRegion(
    const SampleMapRect& bounds,
    double x,
    double y,
    double edgeTolerance) noexcept;

[[nodiscard]] talas::project::ProjectSampleRegion resizeSampleMapRegion(
    const talas::project::ProjectSampleRegion& region,
    SampleMapResizeEdge edge,
    int delta) noexcept;

[[nodiscard]] SampleMapMoveResult moveSampleMapRegions(
    std::span<const talas::project::ProjectSampleRegion> regions,
    std::span<const std::string> selectedRegionIds,
    int requestedNoteDelta,
    int requestedVelocityDelta);

[[nodiscard]] std::vector<std::string> updateSampleMapSelection(
    std::span<const std::string> currentSelection,
    std::string regionId,
    SampleMapSelectionMode mode);

[[nodiscard]] std::vector<std::string> reconcileSampleMapSelection(
    const talas::project::ProjectSampleMap& map,
    std::span<const std::string> selectedRegionIds);

struct WaveformViewport
{
    double startFrame = 0.0;
    double endFrameExclusive = 0.0;

    bool operator==(const WaveformViewport&) const = default;
};

enum class WaveformLoopMarker
{
    Start,
    EndExclusive,
};

struct WaveformLoopProjection
{
    double startPixel = 0.0;
    double endPixel = 0.0;
    bool hasReturnLeg = false;

    bool operator==(const WaveformLoopProjection&) const = default;
};

[[nodiscard]] double waveformFrameToPixel(
    double frame,
    const WaveformViewport& viewport,
    double width) noexcept;

[[nodiscard]] std::uint64_t waveformPixelToFrame(
    double pixel,
    const WaveformViewport& viewport,
    double width) noexcept;

[[nodiscard]] WaveformViewport zoomWaveformViewport(
    const WaveformViewport& viewport,
    double anchorPixel,
    double zoomFactor,
    double width,
    std::uint64_t frameCount) noexcept;

[[nodiscard]] std::uint64_t clampWaveformFrame(
    double frame,
    std::uint64_t frameCount) noexcept;

[[nodiscard]] talas::project::ProjectSampleLoop moveWaveformLoopMarker(
    const talas::project::ProjectSampleLoop& loop,
    WaveformLoopMarker marker,
    std::int64_t delta,
    std::uint64_t frameCount) noexcept;

[[nodiscard]] WaveformLoopProjection projectWaveformLoop(
    const talas::project::ProjectSampleLoop& loop,
    const WaveformViewport& viewport,
    double width) noexcept;
}
