#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
inline constexpr std::uint32_t scoreClipMaterializationRendererVersion =
    talas::project::scoreClipRenderArtifactRendererVersion;
inline constexpr std::size_t maximumPreparedScoreClipAudioSamples =
    16U * 1024U * 1024U;

struct ScoreClipMaterializationOptions
{
    std::function<bool()> shouldCancel;
    // Cache/freeze publication must always render the current live source,
    // never copy a previously persisted artifact back into a new artifact.
    bool forceLiveSource = false;
    // Caches are recorded pre-fader, before the
    // clip's fades, its track's Replace overlap and its panning, which are
    // applied as the cache is played.
    bool preFader = false;
    std::size_t maximumAggregateSamples =
        maximumPreparedScoreClipAudioSamples;
    std::size_t maximumAggregateRenderSamples =
        maximumPreparedScoreClipAudioSamples;
};

struct ScoreClipMaterializationResult
{
    std::vector<PreparedScoreClipAudioPtr> clips;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty() && !cancelled;
    }
};

struct ScoreClipMaterializationSettingsResult
{
    std::optional<RenderSettings> settings;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return settings.has_value() && error.empty() && !cancelled;
    }
};

[[nodiscard]] ScoreClipMaterializationSettingsResult
prepareScoreClipMaterializationSettings(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::string_view clipId,
    double sampleRate,
    std::size_t maximumBlockFrames,
    std::function<bool()> shouldCancel = {});

[[nodiscard]] std::string scoreClipSourceFingerprint(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    std::string& error);

[[nodiscard]] ScoreClipMaterializationResult materializeProjectScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options = {});

// Prepares only tracks that opt into cross-Sound MemoryWriter storage. Their
// completed clip recordings are transferred in Timeline order during offline
// source materialization; the returned audio is immutable stage input.
[[nodiscard]] ScoreClipMaterializationResult
materializeProjectSharedMemoryWriterScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options = {});

// Prepares the clips on tracks taking a submix as
// their audio input, each rendered with the audio of the clips on the tracks
// feeding that submix, which a live source cannot take.
[[nodiscard]] ScoreClipMaterializationResult
materializeProjectInputTrackScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options = {});

[[nodiscard]] ScoreClipMaterializationResult materializeProjectScoreClip(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    std::string_view clipId,
    ScoreClipMaterializationOptions options = {});
}
