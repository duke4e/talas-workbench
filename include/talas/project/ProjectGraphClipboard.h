#pragma once

#include "talas/project/ProjectDocument.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
// Sounds copied from a graph with the connections among them (Copy and Paste).
// A connection to a Sound outside the selection is left behind, as Paste gives
// a copy of the selected Sounds. Positions keep the Sounds' arrangement; paste
// moves the whole group.
struct ProjectGraphFragment
{
    std::vector<ProjectNode> nodes;
    std::vector<CanvasPosition> positions;
    std::vector<talas::core::Connection> connections;

    bool operator==(const ProjectGraphFragment&) const = default;
};

struct ProjectGraphFragmentResult
{
    std::optional<ProjectGraphFragment> fragment;
    std::string message;
};

// Copies the selected nodes of the root graph (definitionId empty) or of a
// Sound definition's graph. A preserved component that cannot be resolved
// is not copied, since its state belongs to the bundle that made it.
[[nodiscard]] ProjectGraphFragmentResult copyProjectGraphFragment(
    const ProjectDocument& document,
    std::optional<std::string_view> definitionId,
    std::span<const std::string> nodeIds);

// The clipboard text: a small JSON object marked "talasGraphFragment".
[[nodiscard]] std::string serializeProjectGraphFragment(
    const ProjectGraphFragment& fragment);
[[nodiscard]] ProjectGraphFragmentResult parseProjectGraphFragment(
    std::string_view text);
}
