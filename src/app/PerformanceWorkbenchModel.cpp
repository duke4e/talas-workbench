#include "talas/app/PerformanceWorkbenchModel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace talas::app
{
namespace
{
using talas::project::ProjectPerformanceControlKind;
using talas::project::SnapshotTargetIdentity;
using talas::project::SnapshotTargetKind;

const talas::project::VirtualControl* eventControl(
    const talas::project::ProjectDocument& document,
    const SnapshotTargetIdentity& target)
{
    if (target.kind != SnapshotTargetKind::EventValue)
        return nullptr;
    const auto found = std::ranges::find_if(
        document.virtualControls,
        [&](const auto& control) {
            return control.eventName == target.eventName
                && control.domain == target.domain;
        });
    return found == document.virtualControls.end() ? nullptr : &*found;
}

const talas::project::ProjectSnapshotBank* activeBank(
    const talas::project::ProjectDocument& document,
    std::optional<std::string_view> bankId)
{
    if (!bankId.has_value())
        return nullptr;
    const auto found = std::ranges::find(
        document.snapshotLibrary.banks,
        *bankId,
        &talas::project::ProjectSnapshotBank::bankId);
    return found == document.snapshotLibrary.banks.end() ? nullptr : &*found;
}

bool targetLocked(
    const talas::project::ProjectSnapshotBank* bank,
    const SnapshotTargetIdentity& target)
{
    if (bank == nullptr)
        return false;
    const auto policy = std::ranges::find(
        bank->parameterPolicies,
        target,
        &talas::project::SnapshotParameterPolicy::target);
    return policy != bank->parameterPolicies.end() && policy->locked;
}

const PerformanceTargetValue* targetValue(
    std::span<const PerformanceTargetValue> values,
    const SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        values,
        target,
        &PerformanceTargetValue::target);
    return found == values.end() ? nullptr : &*found;
}

std::optional<double> normalizedValue(
    double value,
    double minimum,
    double maximum)
{
    if (!std::isfinite(value) || !std::isfinite(minimum)
        || !std::isfinite(maximum) || !(minimum < maximum)
        || value < minimum || value > maximum)
    {
        return std::nullopt;
    }
    const auto middle = std::midpoint(minimum, maximum);
    if (value >= middle)
    {
        const auto upper = maximum - middle;
        return upper > 0.0 ? (value - middle) / upper : 0.0;
    }
    const auto lower = middle - minimum;
    return lower > 0.0 ? -(middle - value) / lower : 0.0;
}

bool hasMacroCycle(const talas::project::ProjectDocument& document)
{
    return talas::project::projectPerformancePresentationHasCycle(document);
}
}

PerformancePanelProjection buildPerformancePanelProjection(
    const talas::project::ProjectDocument& document,
    const PerformanceProjectionContext& context)
{
    PerformancePanelProjection result;
    const auto* bank = activeBank(document, context.activeSnapshotBankId);
    std::unordered_set<std::string> direct(
        context.directTargetIdentities.begin(),
        context.directTargetIdentities.end());
    std::unordered_set<std::string> unavailableMidi(
        context.unavailableMidiSourceIdentifiers.begin(),
        context.unavailableMidiSourceIdentifiers.end());
    std::unordered_map<std::string, PerformanceActivitySource> activity;
    for (const auto& item : context.recentActivity)
    {
        activity[talas::project::snapshotTargetIdentityString(item.target)] =
            item.source;
    }

    result.controls.reserve(document.presentation.performanceControls.size());
    for (const auto& item : document.presentation.performanceControls)
    {
        PerformanceControlViewModel view;
        view.controlId = item.controlId;
        view.target = item.target;
        view.kind = item.kind;
        view.displayName = item.displayName;
        view.position = item.position;
        view.visible = item.visible;
        view.modulationSlots = item.modulationSlots;
        const auto identity =
            talas::project::snapshotTargetIdentityString(item.target);
        view.directEligible = direct.contains(identity);
        view.locked = targetLocked(bank, item.target);
        if (const auto* control = eventControl(document, item.target))
        {
            view.midiMapped = control->midiMapping.has_value();
            view.midiLegacy = control->midiMapping.has_value()
                && control->midiMapping->sourceMode
                    == talas::project::MidiControlSourceMode::LegacyAnyInput;
            view.midiSourceUnavailable = control->midiMapping.has_value()
                && control->midiMapping->sourceMode
                    == talas::project::MidiControlSourceMode::ExactInput
                && unavailableMidi.contains(
                    control->midiMapping->sourceIdentifier);
            view.oscInputMapped = control->oscMapping.has_value();
            view.oscFeedbackMapped = control->oscEgressMapping.has_value();
        }
        if (const auto found = activity.find(identity); found != activity.end())
            view.recentActivity = found->second;
        result.controls.push_back(std::move(view));
    }
    result.meters = document.presentation.performanceMeters;
    if (hasMacroCycle(document))
        result.diagnostics.push_back("Performance macro routing contains a cycle");
    return result;
}

PerformanceMacroReductionResult reducePerformanceMacros(
    const talas::project::ProjectDocument& document,
    std::span<const PerformanceMacroInput> inputs,
    std::span<const PerformanceTargetValue> targetValues,
    std::optional<std::string_view> activeSnapshotBankId)
{
    PerformanceMacroReductionResult result;
    if (inputs.empty())
        return result;
    if (inputs.size() > talas::project::maximumPerformanceControlCount
        || targetValues.size()
            > talas::project::maximumSnapshotValuesPerSnapshot)
    {
        result.diagnostics.push_back(
            "Performance macro input exceeds the prepared value limit");
        return result;
    }
    if (hasMacroCycle(document))
    {
        result.diagnostics.push_back("Performance macro routing contains a cycle");
        return result;
    }

    const auto* bank = activeBank(document, activeSnapshotBankId);
    std::unordered_set<std::string> targetValueIds;
    for (const auto& target : targetValues)
    {
        if (!targetValueIds.insert(
                talas::project::snapshotTargetIdentityString(target.target))
                 .second)
        {
            result.diagnostics.push_back(
                "Performance target values must have unique identities");
            return result;
        }
    }
    std::map<std::string, double> destinationOffsets;
    std::map<std::string, SnapshotTargetIdentity> targets;
    std::map<std::string, double> directSources;
    std::unordered_set<std::string> inputIds;
    for (const auto& input : inputs)
    {
        if (!inputIds.insert(input.controlId).second || !std::isfinite(input.value))
        {
            result.diagnostics.push_back(
                "Performance macro inputs must be unique and finite");
            return result;
        }
        const auto* control =
            talas::project::findProjectPerformanceControlPresentationById(
                document, input.controlId);
        if (control == nullptr
            || control->kind != ProjectPerformanceControlKind::Macro)
        {
            result.diagnostics.push_back(
                "Performance macro input does not identify a macro control");
            return result;
        }
        const auto* sourceState = targetValue(targetValues, control->target);
        const auto normalized = sourceState == nullptr
            ? std::optional<double>{}
            : normalizedValue(input.value,
                sourceState->minimum,
                sourceState->maximum);
        if (sourceState == nullptr || !sourceState->directEligible
            || !normalized.has_value())
        {
            result.diagnostics.push_back(
                "Performance macro source is unavailable or outside its range");
            return result;
        }
        const auto sourceIdentity =
            talas::project::snapshotTargetIdentityString(control->target);
        directSources[sourceIdentity] = input.value;
        targets[sourceIdentity] = control->target;
        for (const auto& slot : control->modulationSlots)
        {
            const auto destinationIdentity =
                talas::project::snapshotTargetIdentityString(slot.target);
            destinationOffsets[destinationIdentity] += *normalized * slot.depth;
            targets[destinationIdentity] = slot.target;
        }
    }

    for (const auto& [identity, target] : targets)
    {
        const auto source = directSources.find(identity);
        const auto offset = destinationOffsets.find(identity);
        const auto hasSource = source != directSources.end();
        const auto hasOffset = offset != destinationOffsets.end();
        const auto locked = targetLocked(bank, target);
        if (locked && !hasSource)
            continue;
        const auto* state = targetValue(targetValues, target);
        if (state == nullptr || !state->directEligible
            || !normalizedValue(hasSource ? source->second : state->value,
                    state->minimum,
                    state->maximum)
                    .has_value())
        {
            result.values.clear();
            result.diagnostics.push_back(
                "Performance macro destination is unavailable or lacks a range");
            return result;
        }
        talas::project::SnapshotValue snapshot;
        snapshot.kind = talas::project::SnapshotValueKind::Number;
        snapshot.domain = state->domain;
        const auto base = hasSource ? source->second : state->value;
        if (hasOffset && !locked)
        {
            const auto baseNormalized = *normalizedValue(
                base, state->minimum, state->maximum);
            snapshot.number = std::lerp(state->minimum,
                state->maximum,
                (std::clamp(baseNormalized + offset->second, -1.0, 1.0)
                        + 1.0)
                    * 0.5);
        }
        else
        {
            snapshot.number = base;
        }
        result.values.push_back({target, snapshot});
    }
    if (result.values.size()
        > talas::project::maximumSnapshotValuesPerSnapshot)
    {
        result.values.clear();
        result.diagnostics.push_back(
            "Performance macro transaction exceeds the prepared value limit");
    }
    return result;
}
}
