#include "talas/app/LiveGridAuthoringModel.h"

#include <algorithm>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace talas::app
{
namespace
{
template <typename Items, typename Id>
[[nodiscard]] std::string uniqueIdentifier(
    const Items& items,
    Id id,
    std::string_view base)
{
    std::unordered_set<std::string> used;
    for (const auto& item : items)
        used.insert(item.*id);
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = std::string{base} + std::to_string(ordinal);
        if (!used.contains(candidate))
            return candidate;
    }
}

[[nodiscard]] std::string displayNameOr(
    std::string_view name,
    std::string_view fallback)
{
    return name.empty() ? std::string{fallback} : std::string{name};
}

[[nodiscard]] talas::project::ProjectLiveSetMidiProgramMapping programMapping(
    int channel,
    std::uint32_t row,
    std::uint32_t column,
    std::string_view source)
{
    return {channel, liveGridProgramNumber(row, column), std::string{source}};
}

[[nodiscard]] const talas::project::ProjectLiveSetMidiProgramMapping*
firstProgramMapping(const talas::project::ProjectLiveSet& liveSet) noexcept
{
    for (const auto& cell : liveSet.cells)
    {
        for (const auto& mapping : cell.mappings)
        {
            if (const auto* program =
                    std::get_if<talas::project::ProjectLiveSetMidiProgramMapping>(&mapping))
            {
                return program;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] bool isProgramMapping(
    const talas::project::ProjectLiveSetTriggerMapping& mapping) noexcept
{
    return std::holds_alternative<talas::project::ProjectLiveSetMidiProgramMapping>(
        mapping);
}
}

int liveGridProgramNumber(std::uint32_t row, std::uint32_t column) noexcept
{
    return static_cast<int>(
        row * talas::project::projectLiveSetColumns + column);
}

const talas::project::ProjectLiveSetCell* liveGridCellAt(
    const talas::project::ProjectLiveSet& liveSet,
    std::uint32_t row,
    std::uint32_t column) noexcept
{
    const auto found = std::ranges::find_if(liveSet.cells, [&](const auto& cell) {
        return cell.row == row && cell.column == column;
    });
    return found == liveSet.cells.end() ? nullptr : &*found;
}

talas::project::ProjectLiveSet makeLiveGridSet(
    const talas::project::ProjectLiveSetLibrary& library,
    std::string_view displayName)
{
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = uniqueIdentifier(
        library.liveSets, &talas::project::ProjectLiveSet::liveSetId, "grid");
    liveSet.displayName = displayNameOr(displayName, "Grid");
    return liveSet;
}

std::vector<LiveGridTargetChoice> liveGridTargetChoices(
    const talas::project::ProjectDocument& document)
{
    std::vector<LiveGridTargetChoice> choices;
    for (const auto& definition : document.soundDefinitions)
    {
        choices.push_back({
            "Sound: " + displayNameOr(definition.displayName, definition.definitionId),
            talas::project::ProjectLiveSetSoundTarget{definition.definitionId, std::nullopt},
        });
    }
    for (const auto& bank : document.snapshotLibrary.banks)
    {
        for (const auto& snapshot : bank.snapshots)
        {
            choices.push_back({
                "Snapshot: " + displayNameOr(bank.displayName, bank.bankId) + " / "
                    + displayNameOr(snapshot.displayName, snapshot.snapshotId),
                talas::project::ProjectLiveSetSnapshotTarget{bank.bankId, snapshot.snapshotId},
            });
        }
    }
    for (const auto& marker : document.timeline.score.markers)
    {
        if (!marker.end.has_value())
            continue;
        choices.push_back({
            "Score region: " + displayNameOr(marker.title, marker.markerId),
            talas::project::ProjectLiveSetScoreRegionTarget{marker.markerId},
        });
    }
    choices.push_back({"Mute track", talas::project::ProjectLiveSetTrackMuteTarget{true}});
    choices.push_back({"Unmute track", talas::project::ProjectLiveSetTrackMuteTarget{false}});
    choices.push_back({"Stop", talas::project::ProjectLiveSetStopTarget{}});
    return choices;
}

std::string liveGridTargetLabel(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectLiveSetCellTarget& target)
{
    for (const auto& choice : liveGridTargetChoices(document))
    {
        if (choice.target == target)
            return choice.label;
    }
    return std::visit(
        [](const auto& value) -> std::string {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Target, talas::project::ProjectLiveSetSoundTarget>)
                return "Sound: " + value.soundDefinitionId;
            else if constexpr (std::is_same_v<Target, talas::project::ProjectLiveSetSnapshotTarget>)
                return "Snapshot: " + value.bankId + " / " + value.snapshotId;
            else if constexpr (std::is_same_v<Target, talas::project::ProjectLiveSetScoreRegionTarget>)
                return "Score region: " + value.markerId;
            else if constexpr (std::is_same_v<Target, talas::project::ProjectLiveSetTrackMuteTarget>)
                return value.muted ? "Mute track" : "Unmute track";
            else
                return "Stop";
        },
        target);
}

std::optional<int> liveGridProgramChannel(
    const talas::project::ProjectLiveSet& liveSet) noexcept
{
    const auto* program = firstProgramMapping(liveSet);
    return program == nullptr ? std::nullopt : std::optional<int>{program->channel};
}

talas::project::ProjectLiveSetCell assignLiveGridCell(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectLiveSet& liveSet,
    std::uint32_t row,
    std::uint32_t column,
    const LiveGridTargetChoice& choice)
{
    talas::project::ProjectLiveSetCell cell;
    const auto* existing = liveGridCellAt(liveSet, row, column);
    if (existing != nullptr)
    {
        cell = *existing;
        // A name the box took from its old target follows the new one; a
        // name given to the box stays.
        if (cell.displayName.empty()
            || cell.displayName == liveGridTargetLabel(document, existing->target))
        {
            cell.displayName = choice.label;
        }
    }
    else
    {
        cell.cellId = "box" + std::to_string(liveGridProgramNumber(row, column));
        if (std::ranges::any_of(liveSet.cells, [&](const auto& other) {
                return other.cellId == cell.cellId;
            }))
        {
            cell.cellId = uniqueIdentifier(
                liveSet.cells,
                &talas::project::ProjectLiveSetCell::cellId,
                cell.cellId + "_");
        }
        cell.row = row;
        cell.column = column;
        cell.displayName = choice.label;
    }
    cell.target = choice.target;
    std::erase_if(cell.mappings, isProgramMapping);
    if (std::ranges::none_of(cell.mappings, [](const auto& mapping) {
            return std::holds_alternative<talas::project::ProjectLiveSetWorkbenchMapping>(mapping);
        }))
    {
        cell.mappings.insert(
            cell.mappings.begin(), talas::project::ProjectLiveSetWorkbenchMapping{});
    }
    if (const auto* program = firstProgramMapping(liveSet))
    {
        cell.mappings.push_back(programMapping(
            program->channel, row, column, program->sourceIdentifier));
    }
    return cell;
}

talas::project::ProjectLiveSet withLiveGridProgramChannel(
    talas::project::ProjectLiveSet liveSet,
    std::optional<int> channel,
    std::string_view midiSourceIdentifier)
{
    for (auto& cell : liveSet.cells)
    {
        std::erase_if(cell.mappings, isProgramMapping);
        if (channel.has_value())
        {
            cell.mappings.push_back(programMapping(
                *channel, cell.row, cell.column, midiSourceIdentifier));
        }
    }
    return liveSet;
}

talas::project::ProjectLiveTool makeLiveGridTool(
    const talas::project::ProjectLiveSetLibrary& library,
    std::string_view displayName)
{
    talas::project::ProjectLiveTool tool;
    tool.toolId = uniqueIdentifier(
        library.tools, &talas::project::ProjectLiveTool::toolId, "tool");
    tool.displayName = displayNameOr(displayName, "Tool");
    tool.initialStateId = "start";
    tool.states.push_back({"start", "Start"});
    return tool;
}

talas::project::ProjectLiveTool withLiveGridToolStep(
    talas::project::ProjectLiveTool tool,
    std::string_view liveSetId,
    const talas::project::ProjectLiveSetCell& cell)
{
    const auto from = tool.states.empty() ? std::string{"start"} : tool.states.back().stateId;
    if (tool.states.empty())
    {
        tool.initialStateId = "start";
        tool.states.push_back({"start", "Start"});
    }
    const auto step = uniqueIdentifier(
        tool.states, &talas::project::ProjectLiveToolState::stateId, "step");
    tool.states.push_back({step, displayNameOr(cell.displayName, cell.cellId)});
    talas::project::ProjectLiveToolTransition transition;
    transition.transitionId = uniqueIdentifier(
        tool.transitions, &talas::project::ProjectLiveToolTransition::transitionId, "next");
    transition.fromStateId = from;
    transition.toStateId = step;
    transition.trigger.kind = talas::project::ProjectLiveToolTriggerKind::Manual;
    talas::project::ProjectLiveToolCommand command;
    command.kind = talas::project::ProjectLiveToolCommandKind::ActivateCell;
    command.liveSetId = std::string{liveSetId};
    command.cellId = cell.cellId;
    transition.commands.push_back(std::move(command));
    tool.transitions.push_back(std::move(transition));
    return tool;
}
}
