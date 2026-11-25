#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
using GraphNodeSet = std::set<std::string, std::less<>>;

// A Sound whose branch is hidden shows an ellipsis
// in place of everything to its left. The hidden Sounds are those that
// reach no Sound without inputs to the right (no outgoing line) except
// through a hidden branch; a Sound that also feeds a shown Sound stays.
// A SoundCollectionVariable is not, strictly
// speaking, a Sound, so one in another Sound's Inputs is not drawn in the
// signal flow (it shows in the Inputs field); one feeding nothing is.
[[nodiscard]] GraphNodeSet hiddenGraphNodes(
    const talas::project::ProjectGraph& graph,
    const GraphNodeSet& collapsed);

// Whether a Sound has any input line, so a branch to hide or show.
[[nodiscard]] bool graphNodeHasInputs(
    const talas::project::ProjectGraph& graph,
    std::string_view nodeId);

// A source, a Sound without inputs, replacing
// another source asks to confirm the replacement first.
[[nodiscard]] bool replacementAsksToConfirm(bool replacedHasInputs, bool replacementTakesInputs) noexcept;

// A click on a Sound's icon selects it alone;
// with Shift it joins the selection, or leaves it if it was already there.
enum class GraphClickSelection : std::uint8_t
{
    SelectOnly,
    Add,
    Remove,
};
[[nodiscard]] GraphClickSelection graphClickSelection(bool shift, bool alreadySelected) noexcept;
[[nodiscard]] std::string replacementConfirmation(std::string_view replaced, std::string_view replacement);

// A Sound whose inputs are hidden cannot be deleted
// until the ellipsis shows them.
[[nodiscard]] bool graphNodeDeletable(
    const talas::project::ProjectGraph& graph,
    const GraphNodeSet& collapsed,
    std::string_view nodeId);

// Clicking the ellipsis shows the inputs again and clicking the dot hides
// them; a double-click (or Control-click) opens the whole branch to the
// left at once: the node and every Sound upstream of it lose their hidden
// branches.
[[nodiscard]] GraphNodeSet toggledGraphBranch(
    const talas::project::ProjectGraph& graph,
    GraphNodeSet collapsed,
    std::string_view nodeId);
[[nodiscard]] GraphNodeSet fullyOpenedGraphBranch(
    const talas::project::ProjectGraph& graph,
    GraphNodeSet collapsed,
    std::string_view nodeId);

// The Sounds upstream of a node, the node itself not included.
[[nodiscard]] GraphNodeSet graphBranchOf(
    const talas::project::ProjectGraph& graph,
    std::string_view nodeId);

// A drag with Shift held:
// the Sound and all of its inputs move together.
[[nodiscard]] GraphNodeSet graphNodesShiftDragged(
    const talas::project::ProjectGraph& graph,
    std::string_view nodeId);

// Several Sounds can share an input, a
// line drawn from each of them to it; and a thick line labelled with a
// number joins a Sound to an input it uses more than once. For each of the
// graph's connections, how many of them join the same output of one Sound
// to the same other Sound (1 for a line of its own).
[[nodiscard]] std::vector<std::size_t> graphConnectionUseCounts(
    const talas::project::ProjectGraph& graph);
// The Sounds a Sound is an input of.
[[nodiscard]] GraphNodeSet graphNodesFedBy(
    const talas::project::ProjectGraph& graph,
    std::string_view nodeId);

// A Sound dragged from the prototypes is a copy, a
// number appended to its name: the base and the first number from 1 that
// names no Sound yet.
[[nodiscard]] std::string numberedCopyName(
    std::string_view base,
    const std::function<bool(std::string_view)>& taken);

struct GraphCleanUpSpacing
{
    std::int32_t columnWidth = 230;
    std::int32_t rowHeight = 110;
};

// Edit > Clean up: the signal flow laid out from
// right to left, each Sound one column left of the furthest Sound it
// feeds (the Sounds feeding nothing in the rightmost column), and each
// column's Sounds in the order of the inputs they feed, the rightmost
// column in its current top-to-bottom order. The layout keeps the
// graph's current top-left corner. The positions of every node of the
// graph that has one, in the graph's node order.
[[nodiscard]] std::vector<talas::project::ProjectNodePresentation>
cleanedUpGraphLayout(
    const talas::project::ProjectGraph& graph,
    const std::vector<talas::project::ProjectNodePresentation>& current,
    GraphCleanUpSpacing spacing = {});
}
