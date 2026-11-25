#pragma once

#include "talas/runtime/Runtime.h"
#include "talas/runtime/PreparedScore.h"

namespace talas::runtime
{
[[nodiscard]] std::optional<talas::project::ProjectDocument>
isolatedScoreClipSourceDocument(
    const talas::project::ProjectDocument& document,
    const PreparedScoreClip& clip,
    const PreparedScoreTrack& track,
    std::string& error);

// Control-thread preparation only. Compile one repeat's exact source graph and
// source-time automation without rendering, consuming input, or prerolling DSP.
// The caller owns clip activation, content-offset advancement and fades.
[[nodiscard]] CompileResult prepareScoreClipSourceIteration(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    std::size_t clipIndex,
    std::uint32_t iterationOrdinal,
    std::vector<std::size_t>* preparedAnalysisTargets = nullptr);
}
