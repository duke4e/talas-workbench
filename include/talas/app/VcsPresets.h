#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectSnapshots.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The presets of the Virtual Control Surface:
// a snapshot bank of the project's values whose first preset, Default, holds
// the values it starts with; each preset numbered by its MIDI program change;
// a random name suggested for a new one; and the dice, which leave a frozen
// control and everything the surface does not show where they are.
namespace talas::app
{
inline constexpr std::string_view vcsPresetBankId = "vcsPresets";
inline constexpr std::string_view vcsDefaultPresetId = "default";

struct VcsPresetBankResult
{
    std::optional<talas::project::ProjectSnapshotBank> bank;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return bank.has_value(); }
};

// The bank, made the first time a preset is taken: the virtual controls free
// to the dice within their ranges, every other value locked; Default holds
// the values the project starts with, or those given when it has none.
[[nodiscard]] VcsPresetBankResult vcsPresetBank(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::vector<talas::project::SnapshotValueEntry>& current);

[[nodiscard]] const talas::project::ProjectSnapshotBank* findVcsPresetBank(const talas::project::ProjectDocument& document);

// The list: each preset's name with its program change in brackets.
[[nodiscard]] std::vector<std::string> vcsPresetLabels(const talas::project::ProjectSnapshotBank& bank);
// The preset a MIDI program change (1 to 128) selects: the one it numbers.
[[nodiscard]] std::optional<std::string> vcsPresetForProgram(const talas::project::ProjectSnapshotBank& bank, unsigned program);
// The arrow keys: the preset above or below, staying at the ends.
[[nodiscard]] std::optional<std::string> vcsAdjacentPreset(
    const talas::project::ProjectSnapshotBank& bank, std::string_view current, int step);

// A name suggested for a new preset.
[[nodiscard]] std::string randomVcsPresetName(std::uint64_t seed);
[[nodiscard]] std::string uniqueVcsPresetId(const talas::project::ProjectSnapshotBank& bank);

// Frozen controls: locked against the dice.
[[nodiscard]] talas::project::SnapshotTargetIdentity vcsControlTarget(const talas::project::VirtualControl& control);
[[nodiscard]] bool vcsControlFrozen(const talas::project::ProjectSnapshotBank& bank, const talas::project::VirtualControl& control);
[[nodiscard]] talas::project::SnapshotParameterPolicy vcsFreezePolicy(
    const talas::project::ProjectSnapshotBank& bank, const talas::project::VirtualControl& control, bool frozen);

// The controls' values among a snapshot's, to show on the surface.
[[nodiscard]] std::vector<std::pair<std::string, double>> vcsControlValues(
    std::span<const talas::project::SnapshotValueEntry> values);

// Remove all but Default.
[[nodiscard]] std::vector<std::string> vcsPresetsBesidesDefault(const talas::project::ProjectSnapshotBank& bank);

// Each layout has its own list of presets. The
// bank a layout's presets are taken into and chosen from: the one its
// settings name (format 155), or the surface's (vcsPresetBankId) for the
// project's top layout (an empty name) and a layout keeping none, as every
// layout of an older project does.
[[nodiscard]] std::string vcsLayoutPresetBankId(const talas::project::ProjectDocument& document, std::string_view layout);
[[nodiscard]] const talas::project::ProjectSnapshotBank* findVcsLayoutPresetBank(
    const talas::project::ProjectDocument& document, std::string_view layout);
// The layout's own bank, made the first time it takes a preset or freezes a
// control, named for the layout: the presets it showed until then (the
// surface's, or a Default of the values the project starts with), the dice
// moving only the layout's own controls (`eventNames`) within the ranges it
// shows them in, a control frozen in the surface's presets staying frozen,
// and every other value locked, so that choosing a preset leaves it alone.
[[nodiscard]] VcsPresetBankResult vcsLayoutPresetBank(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const std::vector<talas::project::SnapshotValueEntry>& current,
    std::string_view layout, const std::vector<std::string>& eventNames);
}
