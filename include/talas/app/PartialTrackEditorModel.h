#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectPartialTrackEdits.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app
{
inline constexpr std::size_t maximumPartialTrackEditorRulerTicks = 32;
inline constexpr std::size_t maximumPartialTrackEditorVisibleRows = 4096;

enum class PartialTrackEditorState : std::uint8_t
{
    Loading,
    Ready,
    Empty,
    Error,
};

enum class PartialTrackEditorItemKind : std::uint8_t
{
    Track,
    Point,
};

struct PartialTrackEditorSelection
{
    PartialTrackEditorItemKind kind = PartialTrackEditorItemKind::Track;
    std::uint64_t trackId = 0;
    talas::project::MaterializedPartialTrackPointIdentity point;

    bool operator==(const PartialTrackEditorSelection&) const = default;
};

struct PartialTrackEditorFilter
{
    std::vector<std::uint64_t> trackIds;
    double minimumAmplitude = 0.0;
    bool includeMuted = true;
    bool soloOnly = false;

    bool operator==(const PartialTrackEditorFilter&) const = default;
};

struct PartialTrackEditorViewport
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 1;
    double minimumFrequencyHz = 20.0;
    double maximumFrequencyHz = 20000.0;
    std::size_t pixelWidth = 1;
    std::size_t pixelHeight = 1;
    std::size_t firstTrackRow = 0;
    std::size_t visibleTrackRows = 64;
};

enum class PartialTrackEditorCommand : std::uint32_t
{
    None = 0,
    Select = 1U << 0U,
    MovePoint = 1U << 1U,
    DeletePoint = 1U << 2U,
    SetStepInterpolation = 1U << 3U,
    SetLinearInterpolation = 1U << 4U,
    MuteTrack = 1U << 5U,
    SoloTrack = 1U << 6U,
    ClearSelection = 1U << 7U,
    InsertPoint = 1U << 8U,
    RetrackPoint = 1U << 9U,
    TransformVisibleRegion = 1U << 10U,
    SplitVisibleRegion = 1U << 11U,
    MergeVisibleRegion = 1U << 12U,
};

[[nodiscard]] constexpr std::uint32_t partialTrackEditorCommandBit(
    PartialTrackEditorCommand command) noexcept
{
    return static_cast<std::uint32_t>(command);
}

struct PartialTrackEditorRulerTick
{
    double value = 0.0;
    double pixel = 0.0;
};

struct PartialTrackEditorTrackRow
{
    std::uint64_t trackId = 0;
    std::size_t pointCount = 0;
    bool muted = false;
    bool solo = false;
    bool selected = false;
};

struct PartialTrackEditorPointGeometry
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
    double x = 0.0;
    double y = 0.0;
    bool selected = false;
};

struct PartialTrackEditorSegmentGeometry
{
    talas::project::MaterializedPartialTrackPointIdentity startIdentity;
    talas::project::MaterializedPartialTrackPointIdentity endIdentity;
    std::uint64_t trackId = 0;
    talas::project::PartialTrackPointInterpolation interpolation =
        talas::project::PartialTrackPointInterpolation::Linear;
    double startX = 0.0;
    double startY = 0.0;
    double endX = 0.0;
    double endY = 0.0;
};

struct PartialTrackEditorProjection
{
    PartialTrackEditorState state = PartialTrackEditorState::Loading;
    std::string message;
    std::string sourceAuthorityText;
    std::string qualityText;
    std::string latencyText;
    std::vector<PartialTrackEditorRulerTick> timeTicks;
    std::vector<PartialTrackEditorRulerTick> frequencyTicks;
    std::vector<PartialTrackEditorTrackRow> rows;
    std::vector<PartialTrackEditorPointGeometry> points;
    std::vector<PartialTrackEditorSegmentGeometry> segments;
    std::uint32_t commandMask = 0;
    std::size_t viewportCandidateCount = 0;
    bool selectionRetained = false;
};

struct PartialTrackEditorSource
{
    talas::project::ProjectAsset manifest;
    std::shared_ptr<const talas::project::PartialTrackAsset> source;
    std::optional<talas::project::ProjectPartialTrackEditSet> edits;
};

class PartialTrackEditorModel final
{
public:
    PartialTrackEditorModel();
    ~PartialTrackEditorModel();
    PartialTrackEditorModel(PartialTrackEditorModel&&) noexcept;
    PartialTrackEditorModel& operator=(
        PartialTrackEditorModel&&) noexcept;
    PartialTrackEditorModel(const PartialTrackEditorModel&) = delete;
    PartialTrackEditorModel& operator=(
        const PartialTrackEditorModel&) = delete;

    void setLoading();
    void setError(std::string message);
    [[nodiscard]] bool setSource(const PartialTrackEditorSource& source);
    [[nodiscard]] bool setFilter(PartialTrackEditorFilter filter);
    [[nodiscard]] bool setSelection(
        PartialTrackEditorSelection selection);
    void clearSelection() noexcept;

    [[nodiscard]] PartialTrackEditorProjection project(
        const PartialTrackEditorViewport& viewport);

    [[nodiscard]] PartialTrackEditorState state() const noexcept;
    [[nodiscard]] std::size_t indexedPointCount() const noexcept;
    [[nodiscard]] std::size_t indexedSegmentCount() const noexcept;
    [[nodiscard]] std::size_t lastCandidateCount() const noexcept;
    [[nodiscard]] std::size_t maximumRenderedSegmentTrackCount() const noexcept;

private:
    struct PreparedPoint;
    struct PreparedSegment;

    PartialTrackEditorState state_ = PartialTrackEditorState::Loading;
    std::string message_ = "Loading partial tracks";
    talas::project::ProjectAsset manifest_;
    talas::project::MaterializedPartialTrackEdits materialized_;
    PartialTrackEditorFilter filter_;
    std::optional<PartialTrackEditorSelection> selection_;
    std::vector<PreparedPoint> points_;
    std::vector<PreparedSegment> segments_;
    std::vector<std::uint64_t> trackIds_;
    std::vector<std::size_t> trackPointCounts_;
    std::vector<std::size_t> identityOrder_;
    std::vector<std::size_t> candidateIndices_;
    std::size_t lastCandidateCount_ = 0;
    std::size_t maximumRenderedSegmentTrackCount_ = 0;

    class Index;
    std::unique_ptr<Index> index_;
};

// The spectrum the editor shows: the one a spectrum file opened made the project's,
// else the project's first; none in a project without one.
[[nodiscard]] const talas::project::ProjectAsset* editedSpectrumAsset(
    const talas::project::ProjectDocument& document, const std::optional<std::string>& opened);
}
