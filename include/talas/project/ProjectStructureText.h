#pragma once

#include "talas/project/ProjectDocument.h"

#include <string>
#include <string_view>

namespace talas::project
{
// The Info menu's Structure as Text: a Sound's
// name followed by its inputs' names in parentheses, and below it each
// input the same way, indented a tab and starting with a dot more for each
// level between it and the Sound; a Sound with no
// inputs is named alone. The Sound is the
// node `soundId` of `graph`, or, when it is empty, the one the graph's
// output hears. A Sound reached again below itself, a feedback loop, is
// named without its inputs.
[[nodiscard]] std::string projectStructureAsText(
    const ProjectGraph& graph,
    std::string_view soundId = {});
// A Sound of the project or of a Sound file, wherever it is listed (the Sound
// Browser, a Sound file window): read from the Sound its first output is,
// without opening it.
[[nodiscard]] std::string soundStructureAsText(const ProjectSoundDefinition& sound);
}
