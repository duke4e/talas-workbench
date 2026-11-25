#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;

inline constexpr std::size_t maximumProjectSampleMaps = 64;
inline constexpr std::size_t maximumProjectSampleRegions = 1024;
inline constexpr std::size_t maximumPreparedSampleMapLayers = 32;
inline constexpr std::size_t maximumProjectSampleMapIdentifierBytes = 128;
inline constexpr std::size_t maximumProjectSampleMapNameBytes = 256;

enum class ProjectSampleLoopMode
{
    Forward,
    Alternate,
};

struct ProjectSampleLoop
{
    ProjectSampleLoopMode mode = ProjectSampleLoopMode::Forward;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrameExclusive = 0;

    bool operator==(const ProjectSampleLoop&) const = default;
};

struct ProjectSampleRegion
{
    std::string regionId;
    std::string assetId;
    std::uint8_t rootNote = 60;
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t lowVelocity = 1;
    std::uint8_t highVelocity = 127;
    bool reverse = false;
    std::optional<ProjectSampleLoop> sustainLoop;
    std::optional<ProjectSampleLoop> releaseLoop;

    bool operator==(const ProjectSampleRegion&) const = default;
};

struct ProjectSampleMap
{
    std::string mapId;
    std::string displayName;
    std::vector<ProjectSampleRegion> regions;

    bool operator==(const ProjectSampleMap&) const = default;
};

struct ProjectSampleMapLibraryState
{
    std::uint32_t schemaVersion = 1;
    std::vector<ProjectSampleMap> maps;

    bool operator==(const ProjectSampleMapLibraryState&) const = default;
};

struct ProjectSampleMapDiagnostic
{
    std::string path;
    std::string message;

    bool operator==(const ProjectSampleMapDiagnostic&) const = default;
};

struct ProjectSampleMapAutoMapCandidate
{
    std::string regionId;
    std::string assetId;
    std::string sourceBasename;
    std::string normalizedSourcePath;

    bool operator==(const ProjectSampleMapAutoMapCandidate&) const = default;
};

struct ProjectSampleMapAutoMapRequest
{
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t anchorRootNote = 60;
    std::vector<ProjectSampleMapAutoMapCandidate> candidates;
};

struct ProjectSampleMapAutoMapResult
{
    std::vector<ProjectSampleRegion> regions;
    std::optional<ProjectSampleMapDiagnostic> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

[[nodiscard]] const ProjectSampleMap* findProjectSampleMap(
    const ProjectDocument& document,
    std::string_view mapId);

[[nodiscard]] ProjectSampleMap* findProjectSampleMap(
    ProjectDocument& document,
    std::string_view mapId);

[[nodiscard]] const ProjectSampleRegion* findProjectSampleRegion(
    const ProjectSampleMap& map,
    std::string_view regionId);

[[nodiscard]] ProjectSampleRegion* findProjectSampleRegion(
    ProjectSampleMap& map,
    std::string_view regionId);

[[nodiscard]] ProjectSampleMapAutoMapResult
makeAutoMappedSampleRegions(
    const ProjectSampleMapAutoMapRequest& request);

[[nodiscard]] std::vector<ProjectSampleMapDiagnostic>
projectSampleMapDiagnostics(const ProjectDocument& document);
}
