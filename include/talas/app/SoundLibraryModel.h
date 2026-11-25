#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace talas::app
{
inline constexpr std::size_t maximumSoundLibraryEntries = 256;
inline constexpr std::size_t maximumSoundLibraryStringBytes = 512;
inline constexpr std::size_t maximumSoundLibraryTags = 16;
inline constexpr std::size_t maximumSoundLibraryAuditionInputs = 32;
inline constexpr std::size_t maximumSoundLibraryAuditionOutputs = 8;

enum class SoundLibrarySourceKind
{
    ProjectDefinition,
    Component,
    OriginalExample,
};

enum class SoundLibraryExpectedOutcome
{
    Renderable,
    ValidationOnly,
    PreservedUnavailable,
};

enum class SoundLibraryCompatibility
{
    Ready,
    ValidationOnly,
    MissingDependency,
    MissingAsset,
    UnsupportedRuntime,
    PreservedUnavailable,
    Malformed,
};

enum class SoundLibraryHealth
{
    Healthy,
    Warning,
    Unavailable,
    Invalid,
};

enum class SoundLibraryAction : std::uint32_t
{
    OpenExample = 1U << 0U,
    OpenHelp = 1U << 1U,
    ShowDefinition = 1U << 2U,
    UseInProject = 1U << 3U,
    Audition = 1U << 4U,
};

using SoundLibraryActionMask = std::uint32_t;

[[nodiscard]] constexpr SoundLibraryActionMask soundLibraryActionMask(
    SoundLibraryAction action) noexcept
{
    return static_cast<SoundLibraryActionMask>(action);
}

[[nodiscard]] constexpr bool soundLibraryHasAction(
    SoundLibraryActionMask mask,
    SoundLibraryAction action) noexcept
{
    return (mask & soundLibraryActionMask(action)) != 0;
}

struct SoundLibraryManifestEntry
{
    std::string id;
    std::string projectPath;
    std::string title;
    std::string summary;
    std::string category;
    std::vector<std::string> tags;
    std::string author;
    std::string origin;
    std::string helpAnchor;
    SoundLibraryExpectedOutcome expectedOutcome =
        SoundLibraryExpectedOutcome::ValidationOnly;
    std::optional<std::string> definitionId;
    SoundLibraryActionMask actions = 0;
    // Derived only by the checked loader from the exact example snapshot.
    // Parsed manifest JSON cannot author these runtime-facing identities.
    std::vector<talas::project::ProjectSoundPort> auditionInputs;
    std::vector<std::string> auditionAudioAssetIds;
    // Derived from the exact bounded project bytes by the checked loader. It
    // is never accepted from the manifest JSON itself.
    std::string verifiedProjectContentHash;

    bool operator==(const SoundLibraryManifestEntry&) const = default;
};

struct SoundLibraryManifest
{
    std::uint32_t schemaVersion = 0;
    std::string authority;
    std::vector<SoundLibraryManifestEntry> entries;
};

struct SoundLibraryManifestLoadResult
{
    std::optional<SoundLibraryManifest> manifest;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return manifest.has_value();
    }
};

struct SoundLibraryExampleSnapshot
{
    talas::project::ProjectDocument document;
    std::string contentHash;
};

struct SoundLibraryExampleSnapshotResult
{
    std::optional<SoundLibraryExampleSnapshot> snapshot;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return snapshot.has_value();
    }
};

[[nodiscard]] SoundLibraryManifestLoadResult parseSoundLibraryManifest(
    std::string_view text);

// root owns examples/ and docs/help/. All paths are resolved relative to this
// explicit root; the process working directory is never consulted.
[[nodiscard]] SoundLibraryManifestLoadResult loadSoundLibraryManifest(
    const std::filesystem::path& root,
    const talas::core::NodeRegistry& registry);

// Re-reads one bounded project snapshot and accepts it only when its bytes
// still match the digest derived by loadSoundLibraryManifest.
[[nodiscard]] SoundLibraryExampleSnapshotResult
loadSoundLibraryExampleSnapshot(
    const std::filesystem::path& root,
    const SoundLibraryManifestEntry& entry,
    const talas::core::NodeRegistry& registry);

struct SoundLibraryEntry
{
    std::string stableId;
    SoundLibrarySourceKind sourceKind =
        SoundLibrarySourceKind::ProjectDefinition;
    std::string sourceIdentity;
    std::string displayName;
    std::string summary;
    std::string category;
    std::vector<std::string> tags;
    std::string author;
    std::string origin;
    std::string searchIndex;
    SoundLibraryCompatibility compatibility =
        SoundLibraryCompatibility::Malformed;
    SoundLibraryHealth dependencyHealth = SoundLibraryHealth::Invalid;
    SoundLibraryHealth assetHealth = SoundLibraryHealth::Invalid;
    SoundLibraryActionMask actions = 0;
    std::string actionDiagnostic;
    std::optional<std::string> projectPath;
    std::optional<std::string> definitionId;
    std::optional<std::string> descriptorTypeId;
    std::optional<std::string> helpAnchor;
    std::optional<std::string> verifiedProjectContentHash;
    std::vector<talas::project::ProjectSoundPort> auditionInputs;
    std::vector<std::string> auditionAudioAssetIds;
};

struct SoundLibraryDefinitionAuditionProfile
{
    std::vector<talas::project::ProjectSoundPort> inputs;
    std::size_t outputChannelCount = 0;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostic.empty() && outputChannelCount > 0;
    }
};

[[nodiscard]] SoundLibraryDefinitionAuditionProfile
soundLibraryDefinitionAuditionProfile(
    const talas::project::ProjectSoundDefinition& definition);

[[nodiscard]] std::size_t soundLibraryProjectOutputChannelCount(
    const talas::project::ProjectDocument& document) noexcept;

[[nodiscard]] bool soundLibraryProjectHasBoundedTerminalEvents(
    const talas::project::ProjectDocument& document) noexcept;

[[nodiscard]] std::vector<SoundLibraryEntry> buildSoundLibraryEntries(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SoundLibraryManifest& manifest);

[[nodiscard]] bool soundLibraryEntryMatches(
    const SoundLibraryEntry& entry,
    std::string_view query);
}
