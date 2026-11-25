#include "talas/app/SampleMapWorkbenchModel.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string assetLabel(std::string_view pathOrId)
{
    const auto separator = pathOrId.find_last_of("/\\");
    const auto label = separator == std::string_view::npos
        ? pathOrId
        : pathOrId.substr(separator + 1);
    return label.empty() ? std::string(pathOrId) : std::string(label);
}

[[nodiscard]] std::string loopDescription(
    std::string_view role,
    const std::optional<talas::project::ProjectSampleLoop>& loop)
{
    if (!loop.has_value())
        return "no " + std::string(role) + " loop";
    return std::string(role) + " "
        + (loop->mode == talas::project::ProjectSampleLoopMode::Alternate
               ? "alternate"
               : "forward")
        + " loop";
}
}

SampleMapViewport sampleMapViewportFromSession(
    const talas::project::WorkspaceSamplesSessionState& session) noexcept
{
    return {
        session.mapViewport.noteStart,
        session.mapViewport.noteStart + session.mapViewport.noteSpan,
        session.mapViewport.velocityStart,
        session.mapViewport.velocityStart
            + session.mapViewport.velocitySpan,
    };
}

WaveformViewport waveformViewportFromSession(
    const talas::project::WorkspaceSamplesSessionState& session,
    std::uint64_t frameCount) noexcept
{
    const auto frames = static_cast<double>(frameCount);
    const auto start = session.waveformViewport.normalizedStart * frames;
    return {
        start,
        start + session.waveformViewport.normalizedSpan * frames,
    };
}

std::string sampleMapRegionAccessibleDescription(
    const talas::project::ProjectSampleRegion& region,
    std::string label)
{
    if (label.empty())
        label = region.assetId;
    return std::move(label)
        + "; notes " + std::to_string(region.lowNote)
        + "-" + std::to_string(region.highNote)
        + "; velocities " + std::to_string(region.lowVelocity)
        + "-" + std::to_string(region.highVelocity)
        + "; root " + std::to_string(region.rootNote)
        + "; " + (region.reverse ? "reverse" : "forward")
        + "; " + loopDescription("sustain", region.sustainLoop)
        + "; " + loopDescription("release", region.releaseLoop);
}

SampleMapWorkbenchProjection projectSampleMapWorkbench(
    const talas::project::ProjectDocument& document,
    const talas::project::WorkspaceSamplesSessionState& session,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas)
{
    SampleMapWorkbenchProjection result;
    const talas::project::ProjectSampleMap* activeMap = nullptr;
    if (session.activeMapId.has_value())
    {
        activeMap = talas::project::findProjectSampleMap(
            document,
            *session.activeMapId);
    }
    result.maps.reserve(document.sampleMapLibrary.maps.size());
    for (const auto& map : document.sampleMapLibrary.maps)
    {
        const auto active = activeMap != nullptr
            && map.mapId == activeMap->mapId;
        result.maps.push_back({
            map.mapId,
            map.displayName,
            map.regions.size(),
            active,
        });
    }
    if (activeMap == nullptr)
        return result;
    result.activeMapId = activeMap->mapId;

    const auto selection = reconcileSampleMapSelection(
        *activeMap,
        session.selectedRegionIds);
    const auto primary = selection.empty()
        ? std::string_view{}
        : std::string_view{selection.front()};
    result.regions.reserve(activeMap->regions.size());
    for (std::size_t index = 0; index < activeMap->regions.size(); ++index)
    {
        const auto& region = activeMap->regions[index];
        const auto asset = std::ranges::find(
            document.assets,
            region.assetId,
            &talas::project::ProjectAsset::id);
        const auto label = asset == document.assets.end()
            ? region.assetId
            : assetLabel(asset->path.empty() ? asset->id : asset->path);
        const auto isSelected = std::ranges::find(
            selection,
            region.regionId) != selection.end();
        result.regions.push_back({
            region.regionId,
            region.assetId,
            index,
            sampleMapRegionRect(region, viewport, canvas),
            isSelected,
            isSelected && region.regionId == primary,
            sampleMapRegionAccessibleDescription(region, label),
        });
    }
    return result;
}

std::vector<std::string> sampleMapRegionIdsAtMidi(
    const talas::project::ProjectSampleMap& map,
    std::uint8_t note,
    std::uint8_t velocity)
{
    std::vector<std::string> result;
    if (note > 127 || velocity == 0 || velocity > 127)
        return result;
    for (const auto& region : map.regions)
    {
        if (note >= region.lowNote && note <= region.highNote
            && velocity >= region.lowVelocity
            && velocity <= region.highVelocity)
        {
            result.push_back(region.regionId);
        }
    }
    return result;
}

SampleMapAutoMapPreview makeSampleMapAutoMapPreview(
    const talas::project::ProjectSampleMapAutoMapRequest& request,
    const SampleMapViewport& viewport,
    const SampleMapCanvas& canvas)
{
    SampleMapAutoMapPreview result;
    auto mapped = talas::project::makeAutoMappedSampleRegions(request);
    if (!mapped.ok())
    {
        result.error = std::move(mapped.error);
        return result;
    }
    result.regions.reserve(mapped.regions.size());
    for (auto& region : mapped.regions)
    {
        const auto bounds = sampleMapRegionRect(region, viewport, canvas);
        result.regions.push_back({std::move(region), bounds});
    }
    return result;
}
}
