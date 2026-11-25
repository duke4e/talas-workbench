#pragma once

#include "talas/project/ProjectDocument.h"

#include <optional>

namespace talas::runtime
{
// Expand on a Sound holding a Script: the Sound as it plays, each Script in its signal flow
// replaced by the Sound it makes, a Mixer of copies of its Inputs started
// at the times and set to the values the Script gave them. Nothing when the
// Sound holds no Script or a Script of it does not run.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> soundWithScriptsExpanded(
    const talas::project::ProjectDocument& document, const talas::project::ProjectSoundDefinition& sound,
    double sampleRate = 48000.0);
}
