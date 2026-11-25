#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/ScoreClipMaterialization.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace talas::runtime
{
struct ScoreClipArtifactPublicationOptions
{
    std::string clipId;
    talas::project::ScoreClipRenderMode mode =
        talas::project::ScoreClipRenderMode::Cached;
    std::filesystem::path destinationDirectory;
    std::string storedPathPrefix;
    std::string assetIdPrefix;
    std::function<bool()> shouldCancel;
    std::function<void(double)> reportProgress;
    std::function<bool()> admitPublication;
    std::size_t maximumAggregateSamples =
        maximumPreparedScoreClipAudioSamples;
    std::size_t maximumAggregateRenderSamples =
        maximumPreparedScoreClipAudioSamples;
};

struct ScoreClipArtifactPublicationResult
{
    talas::project::ScoreClipRenderArtifact artifact;
    std::vector<talas::project::ProjectAsset> assets;
    std::filesystem::path publishedDirectory;
    std::string error;
    bool cancelled = false;
    bool invalidated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty() && !cancelled && !invalidated
            && !publishedDirectory.empty();
    }
};

// Renders one exact Score clip off the callback, writes and re-inspects every
// mono WAV, in the Timeline's cache word size (16-bit WAVs as pcm16-wav, 24-bit
// as sample files), inside a private sibling directory, then adopts the complete
// directory with no-replace semantics only after terminal authority admission.
[[nodiscard]] ScoreClipArtifactPublicationResult publishScoreClipArtifact(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipArtifactPublicationOptions options);
}
