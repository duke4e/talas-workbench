#include "talas/project/ProjectSampleMaps.h"

#include "talas/project/ProjectDocument.h"

#include <array>
#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>

namespace talas::project
{
namespace
{
[[nodiscard]] bool isIdentifier(std::string_view value)
{
    const auto isStart = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    };
    const auto isContinue = [&](char character) {
        return isStart(character)
            || (character >= '0' && character <= '9');
    };
    return !value.empty()
        && value.size() <= maximumProjectSampleMapIdentifierBytes
        && isStart(value.front())
        && std::ranges::all_of(value.substr(1), isContinue);
}

void addDiagnostic(
    std::vector<ProjectSampleMapDiagnostic>& diagnostics,
    std::string path,
    std::string message)
{
    diagnostics.push_back({std::move(path), std::move(message)});
}

[[nodiscard]] const ProjectAsset* findAsset(
    const ProjectDocument& document,
    std::string_view assetId)
{
    const auto found = std::ranges::find(
        document.assets,
        assetId,
        &ProjectAsset::id);
    return found == document.assets.end() ? nullptr : &*found;
}

void validateLoop(
    std::vector<ProjectSampleMapDiagnostic>& diagnostics,
    const ProjectSampleLoop& loop,
    std::string_view path,
    const ProjectAsset* asset)
{
    switch (loop.mode)
    {
    case ProjectSampleLoopMode::Forward:
    case ProjectSampleLoopMode::Alternate:
        break;
    default:
        addDiagnostic(
            diagnostics,
            std::string(path) + ".mode",
            "Expected forward or alternate loop mode");
        break;
    }
    if (loop.startFrame >= loop.endFrameExclusive)
    {
        addDiagnostic(
            diagnostics,
            std::string(path) + ".endFrameExclusive",
            "Loop end frame must be greater than its start frame");
    }
    if (asset != nullptr)
    {
        if (asset->frameCount == 0)
        {
            addDiagnostic(
                diagnostics,
                std::string(path),
                "Loop markers require inspected audio frame count");
        }
        else if (loop.endFrameExclusive > asset->frameCount)
        {
            addDiagnostic(
                diagnostics,
                std::string(path) + ".endFrameExclusive",
                "Loop end frame exceeds the referenced audio asset");
        }
    }
}

[[nodiscard]] std::string asciiCaseFold(std::string_view value)
{
    std::string folded;
    folded.reserve(value.size());
    for (const auto character : value)
    {
        folded.push_back(
            character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character);
    }
    return folded;
}
}

const ProjectSampleMap* findProjectSampleMap(
    const ProjectDocument& document,
    std::string_view mapId)
{
    const auto found = std::ranges::find(
        document.sampleMapLibrary.maps,
        mapId,
        &ProjectSampleMap::mapId);
    return found == document.sampleMapLibrary.maps.end()
        ? nullptr
        : &*found;
}

ProjectSampleMap* findProjectSampleMap(
    ProjectDocument& document,
    std::string_view mapId)
{
    const auto found = std::ranges::find(
        document.sampleMapLibrary.maps,
        mapId,
        &ProjectSampleMap::mapId);
    return found == document.sampleMapLibrary.maps.end()
        ? nullptr
        : &*found;
}

const ProjectSampleRegion* findProjectSampleRegion(
    const ProjectSampleMap& map,
    std::string_view regionId)
{
    const auto found = std::ranges::find(
        map.regions,
        regionId,
        &ProjectSampleRegion::regionId);
    return found == map.regions.end() ? nullptr : &*found;
}

ProjectSampleRegion* findProjectSampleRegion(
    ProjectSampleMap& map,
    std::string_view regionId)
{
    const auto found = std::ranges::find(
        map.regions,
        regionId,
        &ProjectSampleRegion::regionId);
    return found == map.regions.end() ? nullptr : &*found;
}

ProjectSampleMapAutoMapResult makeAutoMappedSampleRegions(
    const ProjectSampleMapAutoMapRequest& request)
{
    ProjectSampleMapAutoMapResult result;
    const auto fail = [&](std::string path, std::string message) {
        result.error = ProjectSampleMapDiagnostic{
            std::move(path),
            std::move(message),
        };
        return result;
    };

    if (request.candidates.empty())
    {
        return fail(
            "$.autoMap.candidates",
            "Auto-map requires at least one candidate");
    }
    if (request.lowNote > 127
        || request.highNote > 127
        || request.lowNote > request.highNote
        || request.anchorRootNote < request.lowNote
        || request.anchorRootNote > request.highNote)
    {
        return fail(
            "$.autoMap.noteRange",
            "Auto-map requires low <= anchor <= high within MIDI notes 0 through 127");
    }
    const auto availableRoots =
        static_cast<std::size_t>(request.highNote)
        - static_cast<std::size_t>(request.anchorRootNote)
        + 1U;
    if (request.candidates.size() > availableRoots)
    {
        return fail(
            "$.autoMap.candidates",
            "Auto-map candidates do not fit the chosen root-note range");
    }

    struct SortableCandidate
    {
        ProjectSampleMapAutoMapCandidate candidate;
        std::string caseFoldedBasename;
    };
    std::vector<SortableCandidate> sorted;
    sorted.reserve(request.candidates.size());
    std::unordered_set<std::string> regionIds;
    for (std::size_t index = 0;
         index < request.candidates.size();
         ++index)
    {
        const auto& candidate = request.candidates[index];
        const auto path = "$.autoMap.candidates["
            + std::to_string(index) + "]";
        if (!isIdentifier(candidate.regionId))
        {
            return fail(
                path + ".regionId",
                "Auto-map region id must be a bounded identifier");
        }
        if (!regionIds.insert(candidate.regionId).second)
        {
            return fail(
                path + ".regionId",
                "Auto-map region identities must be unique");
        }
        if (candidate.assetId.empty())
        {
            return fail(
                path + ".assetId",
                "Auto-map candidate requires an asset identity");
        }
        if (candidate.sourceBasename.empty()
            || candidate.normalizedSourcePath.empty())
        {
            return fail(
                path,
                "Auto-map candidate requires deterministic source sort keys");
        }
        sorted.push_back({
            candidate,
            asciiCaseFold(candidate.sourceBasename),
        });
    }
    std::ranges::sort(
        sorted,
        [](const auto& left, const auto& right) {
            if (left.caseFoldedBasename
                != right.caseFoldedBasename)
            {
                return left.caseFoldedBasename
                    < right.caseFoldedBasename;
            }
            if (left.candidate.normalizedSourcePath
                != right.candidate.normalizedSourcePath)
            {
                return left.candidate.normalizedSourcePath
                    < right.candidate.normalizedSourcePath;
            }
            if (left.candidate.regionId
                != right.candidate.regionId)
            {
                return left.candidate.regionId
                    < right.candidate.regionId;
            }
            return left.candidate.assetId
                < right.candidate.assetId;
        });

    result.regions.reserve(sorted.size());
    for (std::size_t index = 0; index < sorted.size(); ++index)
    {
        const auto root = static_cast<std::uint8_t>(
            static_cast<std::size_t>(request.anchorRootNote) + index);
        const auto low = index == 0
            ? request.lowNote
            : static_cast<std::uint8_t>(
                  (static_cast<unsigned>(root - 1U)
                   + static_cast<unsigned>(root))
                      / 2U
                  + 1U);
        const auto high = index + 1U == sorted.size()
            ? request.highNote
            : static_cast<std::uint8_t>(
                  (static_cast<unsigned>(root)
                   + static_cast<unsigned>(root + 1U))
                  / 2U);

        ProjectSampleRegion region;
        region.regionId = sorted[index].candidate.regionId;
        region.assetId = sorted[index].candidate.assetId;
        region.rootNote = root;
        region.lowNote = low;
        region.highNote = high;
        region.lowVelocity = 1;
        region.highVelocity = 127;
        result.regions.push_back(std::move(region));
    }
    return result;
}

std::vector<ProjectSampleMapDiagnostic>
projectSampleMapDiagnostics(const ProjectDocument& document)
{
    std::vector<ProjectSampleMapDiagnostic> diagnostics;
    if (document.sampleMapLibrary.schemaVersion != 1)
    {
        addDiagnostic(
            diagnostics,
            "$.sampleMapLibrary.version",
            "Expected sample-map library version 1");
    }
    if (document.sampleMapLibrary.maps.size() > maximumProjectSampleMaps)
    {
        addDiagnostic(
            diagnostics,
            "$.sampleMapLibrary.maps",
            "Sample-map count exceeds the supported limit");
    }

    std::unordered_set<std::string> mapIds;
    std::size_t totalRegionCount = 0;
    for (std::size_t mapIndex = 0;
         mapIndex < document.sampleMapLibrary.maps.size();
         ++mapIndex)
    {
        const auto& map = document.sampleMapLibrary.maps[mapIndex];
        const auto mapPath = "$.sampleMapLibrary.maps["
            + std::to_string(mapIndex) + "]";
        if (!isIdentifier(map.mapId))
        {
            addDiagnostic(
                diagnostics,
                mapPath + ".id",
                "Sample-map id must be a bounded identifier");
        }
        else if (!mapIds.insert(map.mapId).second)
        {
            addDiagnostic(
                diagnostics,
                mapPath + ".id",
                "Duplicate sample-map identity");
        }
        if (map.displayName.empty()
            || map.displayName.size() > maximumProjectSampleMapNameBytes)
        {
            addDiagnostic(
                diagnostics,
                mapPath + ".name",
                "Sample-map name must be non-empty and bounded");
        }
        if (map.regions.size() > maximumProjectSampleRegions)
        {
            addDiagnostic(
                diagnostics,
                mapPath + ".regions",
                "Sample-map region count exceeds the supported limit");
        }
        totalRegionCount += map.regions.size();

        std::unordered_set<std::string> regionIds;
        std::array<std::uint8_t, 128 * 127> layerCounts{};
        for (std::size_t regionIndex = 0;
             regionIndex < map.regions.size();
             ++regionIndex)
        {
            const auto& region = map.regions[regionIndex];
            const auto regionPath = mapPath + ".regions["
                + std::to_string(regionIndex) + "]";
            if (!isIdentifier(region.regionId))
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".id",
                    "Sample-region id must be a bounded identifier");
            }
            else if (!regionIds.insert(region.regionId).second)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".id",
                    "Duplicate sample-region identity in map");
            }

            const auto* asset = findAsset(document, region.assetId);
            if (asset == nullptr)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".assetId",
                    "Sample region must reference an existing project asset");
            }
            else if (asset->kind != ProjectAssetKind::Audio)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".assetId",
                    "Sample region must reference an Audio asset");
                asset = nullptr;
            }

            if (region.lowNote > 127)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".lowNote",
                    "Low note must be from 0 through 127");
            }
            if (region.highNote > 127)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".highNote",
                    "High note must be from 0 through 127");
            }
            if (region.rootNote > 127)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".rootNote",
                    "Root note must be from 0 through 127");
            }
            if (region.lowNote > region.highNote)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".highNote",
                    "High note must not be below low note");
            }
            if (region.rootNote < region.lowNote
                || region.rootNote > region.highNote)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".rootNote",
                    "Root note must lie inside the region note range");
            }
            if (region.lowVelocity == 0
                || region.lowVelocity > 127)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".lowVelocity",
                    "Low velocity must be from 1 through 127");
            }
            if (region.highVelocity == 0
                || region.highVelocity > 127
                || region.highVelocity < region.lowVelocity)
            {
                addDiagnostic(
                    diagnostics,
                    regionPath + ".highVelocity",
                    "High velocity must be from low velocity through 127");
            }
            if (region.sustainLoop.has_value())
            {
                validateLoop(
                    diagnostics,
                    *region.sustainLoop,
                    regionPath + ".sustainLoop",
                    asset);
            }
            if (region.releaseLoop.has_value())
            {
                validateLoop(
                    diagnostics,
                    *region.releaseLoop,
                    regionPath + ".releaseLoop",
                    asset);
            }

            if (region.lowNote <= region.highNote
                && region.highNote <= 127
                && region.lowVelocity >= 1
                && region.lowVelocity <= region.highVelocity
                && region.highVelocity <= 127)
            {
                for (std::size_t note = region.lowNote;
                     note <= region.highNote;
                     ++note)
                {
                    for (std::size_t velocity = region.lowVelocity;
                         velocity <= region.highVelocity;
                         ++velocity)
                    {
                        auto& count = layerCounts[
                            note * 127 + velocity - 1];
                        if (count < 255)
                            ++count;
                    }
                }
            }
        }

        if (std::ranges::any_of(
                layerCounts,
                [](std::uint8_t count) {
                    return count > maximumPreparedSampleMapLayers;
                }))
        {
            addDiagnostic(
                diagnostics,
                mapPath + ".regions",
                "Sample-map overlap exceeds the prepared 32-layer limit");
        }
    }
    if (totalRegionCount > maximumProjectSampleRegions)
    {
        addDiagnostic(
            diagnostics,
            "$.sampleMapLibrary.maps",
            "Total sample-region count exceeds the supported limit");
    }
    return diagnostics;
}
}
