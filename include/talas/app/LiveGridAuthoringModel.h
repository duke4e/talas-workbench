#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// Authoring a Live Set as a Compiled Sound Grid: 128 boxes, each a MIDI
// program number, a Sound (or another target) put into a box, and one program
// channel for the grid. Each function returns the edited value for
// ProjectEditor's upsert, which validates it.

// The program a box answers to: rows of 16 boxes, 0 at the top left.
[[nodiscard]] int liveGridProgramNumber(
    std::uint32_t row,
    std::uint32_t column) noexcept;

[[nodiscard]] const talas::project::ProjectLiveSetCell* liveGridCellAt(
    const talas::project::ProjectLiveSet& liveSet,
    std::uint32_t row,
    std::uint32_t column) noexcept;

// A new, empty Live Set with an id no other Live Set has.
[[nodiscard]] talas::project::ProjectLiveSet makeLiveGridSet(
    const talas::project::ProjectLiveSetLibrary& library,
    std::string_view displayName);

struct LiveGridTargetChoice
{
    std::string label;
    talas::project::ProjectLiveSetCellTarget target;

    bool operator==(const LiveGridTargetChoice&) const = default;
};

// What a box can hold: each Sound definition, each snapshot, each score
// region (a marker with an end), a track's mute and unmute, and Stop.
[[nodiscard]] std::vector<LiveGridTargetChoice> liveGridTargetChoices(
    const talas::project::ProjectDocument& document);

[[nodiscard]] std::string liveGridTargetLabel(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectLiveSetCellTarget& target);

// The grid's program channel: the channel of its boxes' MIDI program
// mappings, empty when they have none.
[[nodiscard]] std::optional<int> liveGridProgramChannel(
    const talas::project::ProjectLiveSet& liveSet) noexcept;

// Puts a target into a box. A box that held a cell keeps its id,
// quantization and any name given to it; a new one is named after its
// target. Every box can be clicked in the Workbench and, when the grid
// has a program channel, answers to its program on that channel from the
// grid's MIDI input.
[[nodiscard]] talas::project::ProjectLiveSetCell assignLiveGridCell(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectLiveSet& liveSet,
    std::uint32_t row,
    std::uint32_t column,
    const LiveGridTargetChoice& choice);

// Gives every box the program mapping of the channel from that MIDI input
// (a project MIDI input identifier), or removes them.
[[nodiscard]] talas::project::ProjectLiveSet withLiveGridProgramChannel(
    talas::project::ProjectLiveSet liveSet,
    std::optional<int> channel,
    std::string_view midiSourceIdentifier);

// A new Live Tool that steps through cells: it starts in "start", and each
// step added is a state reached by a manual trigger that activates a cell.
[[nodiscard]] talas::project::ProjectLiveTool makeLiveGridTool(
    const talas::project::ProjectLiveSetLibrary& library,
    std::string_view displayName);

[[nodiscard]] talas::project::ProjectLiveTool withLiveGridToolStep(
    talas::project::ProjectLiveTool tool,
    std::string_view liveSetId,
    const talas::project::ProjectLiveSetCell& cell);
}
