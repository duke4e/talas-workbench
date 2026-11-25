#pragma once

#include "talas/app/SampleMapInteraction.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace talas::app
{
struct SampleMapLibraryEntry
{
    std::string mapId;
    std::string displayName;
    std::size_t regionCount = 0;
    bool active = false;
};

struct SampleMapRegionProjection
{
    std::string regionId;
    std::string assetId;
    std::size_t layerIndex = 0;
    SampleMapRect bounds;
    bool selected = false;
    bool primary = false;
    std::string accessibleDescription;
};

struct SampleMapWorkbenchProjection
{
    std::vector<SampleMapLibraryEntry> maps;
    std::optional<std::string> activeMapId;
    std::vector<SampleMapRegionProjection> regions;
};

struct SampleMapAutoMapPreviewRegion
{
    talas::project::ProjectSampleRegion region;
    SampleMapRect bounds;
};

struct SampleMapAutoMapPreview
{
    std::vector<SampleMapAutoMapPreviewRegion> regions;
    std::optional<talas::project::ProjectSampleMapDiagnostic> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] SampleMapViewport sampleMapViewportFromSession(
    const talas::project::WorkspaceSamplesSessionState& session) noexcept;

[[nodiscard]] WaveformViewport waveformViewportFromSession(
    const talas::project::WorkspaceSamplesSessionState& session,
    std::uint64_t frameCount) noexcept;

[[nodiscard]] std::string sampleMapRegionAccessibleDescription(
    const talas::project::ProjectSampleRegion& region,
    std::string assetLabel);

[[nodiscard]] SampleMapWorkbenchProjection projectSampleMapWorkbench(
    const talas::project::ProjectDocument& document,
    const talas::project::WorkspaceSamplesSessionState& session,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas);

[[nodiscard]] std::vector<std::string> sampleMapRegionIdsAtMidi(
    const talas::project::ProjectSampleMap& map,
    std::uint8_t note,
    std::uint8_t velocity);

[[nodiscard]] SampleMapAutoMapPreview makeSampleMapAutoMapPreview(
    const talas::project::ProjectSampleMapAutoMapRequest& request,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas);
}
