#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/Runtime.h"

#include <string>

namespace talas::runtime
{
struct PreparedScoreGraphProjectionResult
{
    talas::project::ProjectDocument document;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Internal compile-time projection. Public compile entry points rebuild the
// prepared clip-audio table before reaching this boundary.
[[nodiscard]] PreparedScoreGraphProjectionResult
projectPreparedScoreGraph(
    const talas::project::ProjectDocument& document,
    const PreparedScoreSchedule& schedule,
    RenderSettings& settings, bool liveSources = false);
}
