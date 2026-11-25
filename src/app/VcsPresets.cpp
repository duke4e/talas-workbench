#include "talas/app/VcsPresets.h"

#include "talas/app/VcsLayout.h"

#include <algorithm>
#include <array>

namespace talas::app
{
namespace
{
[[nodiscard]] std::uint64_t splitmix(std::uint64_t& state) noexcept
{
    auto value = (state += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
}

VcsPresetBankResult vcsPresetBank(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::vector<talas::project::SnapshotValueEntry>& current)
{
    VcsPresetBankResult result;
    const talas::project::SnapshotScopeIdentity scope;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(document, registry, scope);
    if (!resolved.ok())
    {
        result.diagnostic = resolved.diagnostics.empty() ? "The project's values cannot be kept as presets"
                                                         : resolved.diagnostics.front().message;
        return result;
    }
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = std::string{vcsPresetBankId};
    bank.displayName = "VCS Presets";
    bank.scope = scope;
    for (const auto& target : resolved.targets)
    {
        const auto onSurface = target.target.kind == talas::project::SnapshotTargetKind::EventValue;
        if (!onSurface || target.initialRandomRange.has_value())
            bank.parameterPolicies.push_back({target.target, !onSurface, target.initialRandomRange});
    }
    const auto defaults = talas::project::initializeSnapshotScopeValues(resolved.targets);
    bank.snapshots.push_back({std::string{vcsDefaultPresetId}, "Default", defaults.ok() ? defaults.values : current});
    result.bank = std::move(bank);
    return result;
}

const talas::project::ProjectSnapshotBank* findVcsPresetBank(const talas::project::ProjectDocument& document)
{
    const auto& banks = document.snapshotLibrary.banks;
    const auto found = std::ranges::find(banks, vcsPresetBankId, &talas::project::ProjectSnapshotBank::bankId);
    return found == banks.end() ? nullptr : &*found;
}

std::vector<std::string> vcsPresetLabels(const talas::project::ProjectSnapshotBank& bank)
{
    std::vector<std::string> labels;
    for (std::size_t index = 0; index < bank.snapshots.size(); ++index)
        labels.push_back(bank.snapshots[index].displayName + " [" + std::to_string(index + 1) + "]");
    return labels;
}

std::optional<std::string> vcsPresetForProgram(const talas::project::ProjectSnapshotBank& bank, unsigned program)
{
    if (program == 0 || program > bank.snapshots.size())
        return std::nullopt;
    return bank.snapshots[program - 1].snapshotId;
}

std::optional<std::string> vcsAdjacentPreset(const talas::project::ProjectSnapshotBank& bank, std::string_view current, int step)
{
    if (bank.snapshots.empty())
        return std::nullopt;
    const auto found = std::ranges::find(bank.snapshots, current, &talas::project::ProjectSnapshot::snapshotId);
    const auto index = found == bank.snapshots.end() ? 0 : static_cast<long>(found - bank.snapshots.begin()) + step;
    const auto clamped = std::clamp<long>(index, 0, static_cast<long>(bank.snapshots.size()) - 1);
    return bank.snapshots[static_cast<std::size_t>(clamped)].snapshotId;
}

std::string randomVcsPresetName(std::uint64_t seed)
{
    static constexpr std::array<const char*, 24> qualities{"Glassy", "Restless", "Velvet", "Brittle", "Molten", "Hollow",
        "Shimmering", "Feral", "Dusky", "Granular", "Sunken", "Electric", "Tidal", "Fractured", "Luminous", "Rusty",
        "Drifting", "Ominous", "Liquid", "Prickly", "Ghostly", "Buzzing", "Frozen", "Tangled"};
    static constexpr std::array<const char*, 24> things{"Swarm", "Cathedral", "Engine", "Lagoon", "Whisper", "Orbit",
        "Bellows", "Comet", "Thicket", "Furnace", "Lantern", "Reef", "Machine", "Chorus", "Canyon", "Hive", "Pendulum",
        "Storm", "Mirror", "Choir", "Tundra", "Gears", "Fountain", "Nebula"};
    auto state = seed;
    const auto quality = qualities[splitmix(state) % qualities.size()];
    const auto thing = things[splitmix(state) % things.size()];
    return std::string{quality} + " " + thing;
}

std::string uniqueVcsPresetId(const talas::project::ProjectSnapshotBank& bank)
{
    for (std::size_t ordinal = 1; ordinal <= talas::project::maximumSnapshotsPerBank; ++ordinal)
    {
        const auto candidate = "preset" + std::to_string(ordinal);
        if (std::ranges::find(bank.snapshots, candidate, &talas::project::ProjectSnapshot::snapshotId) == bank.snapshots.end())
            return candidate;
    }
    return {};
}

talas::project::SnapshotTargetIdentity vcsControlTarget(const talas::project::VirtualControl& control)
{
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::EventValue;
    target.eventName = control.eventName;
    target.domain = control.domain;
    return target;
}

bool vcsControlFrozen(const talas::project::ProjectSnapshotBank& bank, const talas::project::VirtualControl& control)
{
    const auto target = vcsControlTarget(control);
    const auto policy = std::ranges::find(bank.parameterPolicies, target, &talas::project::SnapshotParameterPolicy::target);
    return policy != bank.parameterPolicies.end() && policy->locked;
}

talas::project::SnapshotParameterPolicy vcsFreezePolicy(
    const talas::project::ProjectSnapshotBank& bank, const talas::project::VirtualControl& control, bool frozen)
{
    const auto target = vcsControlTarget(control);
    const auto policy = std::ranges::find(bank.parameterPolicies, target, &talas::project::SnapshotParameterPolicy::target);
    talas::project::SnapshotParameterPolicy result{target, frozen,
        policy != bank.parameterPolicies.end() ? policy->range
                                               : std::optional<talas::project::SnapshotParameterRange>{{control.minimum, control.maximum}}};
    return result;
}

std::vector<std::pair<std::string, double>> vcsControlValues(std::span<const talas::project::SnapshotValueEntry> values)
{
    std::vector<std::pair<std::string, double>> controls;
    for (const auto& entry : values)
    {
        if (entry.target.kind == talas::project::SnapshotTargetKind::EventValue
            && entry.value.kind == talas::project::SnapshotValueKind::Number)
            controls.emplace_back(entry.target.eventName, entry.value.number);
    }
    return controls;
}

std::vector<std::string> vcsPresetsBesidesDefault(const talas::project::ProjectSnapshotBank& bank)
{
    std::vector<std::string> removed;
    for (const auto& snapshot : bank.snapshots)
    {
        if (snapshot.snapshotId != vcsDefaultPresetId)
            removed.push_back(snapshot.snapshotId);
    }
    return removed;
}

std::string vcsLayoutPresetBankId(const talas::project::ProjectDocument& document, std::string_view layout)
{
    const auto* settings = findVcsLayoutSettings(document, layout);
    if (settings != nullptr && !settings->presetBankId.empty()
        && std::ranges::find(document.snapshotLibrary.banks, settings->presetBankId, &talas::project::ProjectSnapshotBank::bankId)
            != document.snapshotLibrary.banks.end())
        return settings->presetBankId;
    return std::string{vcsPresetBankId};
}

const talas::project::ProjectSnapshotBank* findVcsLayoutPresetBank(const talas::project::ProjectDocument& document, std::string_view layout)
{
    const auto& banks = document.snapshotLibrary.banks;
    const auto found = std::ranges::find(banks, vcsLayoutPresetBankId(document, layout), &talas::project::ProjectSnapshotBank::bankId);
    return found == banks.end() ? nullptr : &*found;
}

VcsPresetBankResult vcsLayoutPresetBank(const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry,
    const std::vector<talas::project::SnapshotValueEntry>& current, std::string_view layout,
    const std::vector<std::string>& eventNames)
{
    auto result = vcsPresetBank(document, registry, current);
    if (!result.ok())
        return result;
    auto& bank = *result.bank;
    // The presets the layout showed until now come along.
    if (const auto* shared = findVcsPresetBank(document); shared != nullptr && !shared->snapshots.empty())
        bank.snapshots = shared->snapshots;
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = std::string{vcsPresetBankId} + "-layout" + std::to_string(ordinal);
        if (std::ranges::find(document.snapshotLibrary.banks, candidate, &talas::project::ProjectSnapshotBank::bankId)
            == document.snapshotLibrary.banks.end())
        {
            bank.bankId = std::move(candidate);
            break;
        }
    }
    bank.displayName = std::string{layout};
    const talas::project::SnapshotScopeIdentity scope;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(document, registry, scope);
    const auto controls = vcsLayoutControls(document, layout);
    bank.parameterPolicies.clear();
    for (const auto& target : resolved.targets)
    {
        const auto& identity = target.target;
        const auto control = std::ranges::find(controls, identity.eventName, &talas::project::VirtualControl::eventName);
        const auto own = identity.kind == talas::project::SnapshotTargetKind::EventValue && control != controls.end()
            && std::ranges::find(eventNames, identity.eventName) != eventNames.end();
        if (!own)
        {
            bank.parameterPolicies.push_back({identity, true, target.initialRandomRange});
            continue;
        }
        const auto* shared = findVcsPresetBank(document);
        bank.parameterPolicies.push_back({identity, shared != nullptr && vcsControlFrozen(*shared, *control),
            talas::project::SnapshotParameterRange{control->minimum, control->maximum}});
    }
    return result;
}
}
