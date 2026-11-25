#include "talas/app/PerformanceRuntimeDispatcher.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace talas::app
{
namespace
{
const talas::project::VirtualControl* eventControl(const talas::project::ProjectDocument& document,
                                                const talas::project::SnapshotTargetIdentity& target)
{
    if (target.kind != talas::project::SnapshotTargetKind::EventValue)
        return nullptr;
    const auto found = std::ranges::find_if(
        document.virtualControls, [&](const auto& control)
        { return control.eventName == target.eventName && control.domain == target.domain; });
    return found == document.virtualControls.end() ? nullptr : &*found;
}

const talas::project::ProjectSnapshotBank* activeBank(const talas::project::ProjectDocument& document,
                                                   std::string_view bankId)
{
    const auto found = std::ranges::find(document.snapshotLibrary.banks, bankId,
                                         &talas::project::ProjectSnapshotBank::bankId);
    return found == document.snapshotLibrary.banks.end() ? nullptr : &*found;
}

std::optional<talas::project::SnapshotParameterRange>
targetRange(const talas::project::ProjectDocument& document,
            const talas::project::ProjectSnapshotBank* bank,
            const talas::project::SnapshotTargetIdentity& target)
{
    if (const auto* control = eventControl(document, target))
    {
        return talas::project::SnapshotParameterRange{
            control->minimum,
            control->maximum,
        };
    }
    if (bank == nullptr)
        return std::nullopt;
    const auto policy = std::ranges::find(bank->parameterPolicies, target,
                                          &talas::project::SnapshotParameterPolicy::target);
    return policy == bank->parameterPolicies.end()
               ? std::optional<talas::project::SnapshotParameterRange>{}
               : policy->range;
}

PerformanceDispatchResult failure(PerformanceDispatchOutcome outcome, std::string message)
{
    PerformanceDispatchResult result;
    result.outcome = outcome;
    result.message = std::move(message);
    return result;
}
} // namespace

PerformanceRuntimeDispatcher::PerformanceRuntimeDispatcher(
    talas::runtime::ProjectRuntimeBridge& bridge) noexcept
    : bridge_(bridge)
{
}

PerformanceDispatchResult
PerformanceRuntimeDispatcher::dispatch(std::string_view controlId, double value,
                                       std::uint64_t expectedStageGeneration,
                                       double smoothingSeconds)
{
    try
    {
        if (!std::isfinite(value) || !std::isfinite(smoothingSeconds) || smoothingSeconds < 0.0)
        {
            return failure(PerformanceDispatchOutcome::InvalidValue,
                           "Performance value and smoothing must be finite and non-negative");
        }
        if (expectedStageGeneration == 0 ||
            expectedStageGeneration != bridge_.latestStageGeneration() ||
            !bridge_.snapshotPerformanceDocumentCurrent())
        {
            return failure(PerformanceDispatchOutcome::StaleAuthority,
                           "Performance control targets a stale document or prepared stage");
        }

        const auto& document = bridge_.editor().document();
        const auto* control =
            talas::project::findProjectPerformanceControlPresentationById(document, controlId);
        if (control == nullptr)
        {
            return failure(PerformanceDispatchOutcome::InvalidControl,
                           "Unknown Performance control");
        }
        if (!bridge_.snapshotDirectTargetPrepared(control->target))
        {
            return failure(PerformanceDispatchOutcome::TargetUnavailable,
                           "Performance target is not direct in the prepared stage");
        }

        const auto& performance = bridge_.performanceState();
        const auto* bank = activeBank(document, performance.activeBankId);
        const auto baselineValues = bridge_.performanceBaselineValues();
        std::vector<PerformanceTargetValue> targetValues;
        try
        {
            targetValues.reserve(document.presentation.performanceControls.size());
            for (const auto& presented : document.presentation.performanceControls)
            {
                const auto current = std::ranges::find(performance.currentValues, presented.target,
                                                       &talas::project::SnapshotValueEntry::target);
                const auto range = targetRange(document, bank, presented.target);
                PerformanceTargetValue state;
                state.target = presented.target;
                state.directEligible = bridge_.snapshotDirectTargetPrepared(presented.target);
                if (current != performance.currentValues.end() &&
                    current->value.kind == talas::project::SnapshotValueKind::Number)
                {
                    state.value = current->value.number;
                    state.domain = current->value.domain;
                }
                const auto baseline = std::ranges::find(
                    baselineValues,
                    presented.target,
                    &talas::project::SnapshotValueEntry::target);
                if (baseline != baselineValues.end()
                    && baseline->value.kind
                        == talas::project::SnapshotValueKind::Number)
                {
                    state.value = baseline->value.number;
                    state.domain = baseline->value.domain;
                }
                if (const auto* persisted = eventControl(document, presented.target))
                {
                    state.value = persisted->value;
                    state.domain = persisted->domain;
                }
                if (range.has_value())
                {
                    state.minimum = range->minimum;
                    state.maximum = range->maximum;
                }
                else
                {
                    state.minimum = 0.0;
                    state.maximum = 0.0;
                }
                targetValues.push_back(std::move(state));
            }
        }
        catch (const std::bad_alloc&)
        {
            return failure(PerformanceDispatchOutcome::RuntimeRejected,
                           "Performance dispatch allocation exceeded capacity");
        }
        catch (const std::length_error&)
        {
            return failure(PerformanceDispatchOutcome::RuntimeRejected,
                           "Performance dispatch exceeded capacity");
        }

        std::vector<talas::project::SnapshotValueEntry> values;
        if (control->kind == talas::project::ProjectPerformanceControlKind::Macro)
        {
            std::vector<PerformanceMacroInput> inputs;
            inputs.reserve(document.presentation.performanceControls.size());
            for (const auto& presented : document.presentation.performanceControls)
            {
                if (presented.kind !=
                    talas::project::ProjectPerformanceControlKind::Macro)
                {
                    continue;
                }
                const auto state = std::ranges::find(
                    targetValues,
                    presented.target,
                    &PerformanceTargetValue::target);
                if (state == targetValues.end())
                {
                    return failure(
                        PerformanceDispatchOutcome::TargetUnavailable,
                        "Performance macro source state is unavailable");
                }
                inputs.push_back({
                    presented.controlId,
                    presented.controlId == control->controlId
                        ? value
                        : state->value,
                });
            }
            auto reduced = reducePerformanceMacros(
                document,
                inputs,
                targetValues,
                performance.activeBankId.empty()
                    ? std::optional<std::string_view>{}
                    : std::optional<std::string_view>{performance.activeBankId});
            if (!reduced.ok())
            {
                return failure(PerformanceDispatchOutcome::InvalidValue,
                               reduced.diagnostics.front());
            }
            values = std::move(reduced.values);
            std::unordered_set<std::string> requiredTargets{
                talas::project::snapshotTargetIdentityString(control->target),
            };
            for (const auto& slot : control->modulationSlots)
            {
                requiredTargets.insert(
                    talas::project::snapshotTargetIdentityString(slot.target));
            }
            std::erase_if(values, [&](const auto& applied) {
                return !requiredTargets.contains(
                    talas::project::snapshotTargetIdentityString(
                        applied.target));
            });
        }
        else
        {
            const auto state =
                std::ranges::find(targetValues, control->target, &PerformanceTargetValue::target);
            if (state == targetValues.end() || !state->directEligible ||
                !std::isfinite(state->minimum) || !std::isfinite(state->maximum) ||
                !(state->minimum < state->maximum))
            {
                return failure(PerformanceDispatchOutcome::TargetUnavailable,
                               "Performance target lacks an exact finite direct range");
            }
            talas::project::SnapshotValue snapshot;
            snapshot.kind = talas::project::SnapshotValueKind::Number;
            snapshot.domain = state->domain;
            snapshot.number = std::clamp(value, state->minimum, state->maximum);
            values.push_back({control->target, snapshot});
        }

        talas::project::PreparedVirtualControlValueEdit authoringEdit;
        if (control->target.kind == talas::project::SnapshotTargetKind::EventValue)
        {
            const auto source = std::ranges::find(values, control->target,
                                                  &talas::project::SnapshotValueEntry::target);
            if (source == values.end())
            {
                return failure(PerformanceDispatchOutcome::AuthoringRejected,
                               "Performance transaction omitted its source value");
            }
            const auto prepared = bridge_.editor().prepareVirtualControlValue(
                control->target.eventName, source->value.number, authoringEdit);
            if (!prepared.ok())
            {
                return failure(PerformanceDispatchOutcome::AuthoringRejected, prepared.message);
            }
        }

        auto applied = control->target.kind == talas::project::SnapshotTargetKind::EventValue
                           ? bridge_.applyPerformanceDirectValues(values, smoothingSeconds,
                                                                  expectedStageGeneration,
                                                                  std::move(authoringEdit))
                           : bridge_.applyPerformanceDirectValues(values, smoothingSeconds,
                                                                  expectedStageGeneration);
        if (!applied.ok())
        {
            const auto stale = std::ranges::any_of(
                applied.diagnostics,
                [](const auto& diagnostic)
                {
                    return diagnostic.code ==
                           talas::project::SnapshotDiagnosticCode::StalePreparedGeneration;
                });
            return failure(stale ? PerformanceDispatchOutcome::StaleAuthority
                                 : PerformanceDispatchOutcome::RuntimeRejected,
                           applied.message.empty() ? "Performance direct transaction was rejected"
                                                   : applied.message);
        }

        PerformanceDispatchResult result;
        result.outcome = PerformanceDispatchOutcome::Accepted;
        result.appliedValues = std::move(values);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return failure(PerformanceDispatchOutcome::RuntimeRejected,
                       "Performance dispatch allocation exceeded capacity");
    }
    catch (const std::length_error&)
    {
        return failure(PerformanceDispatchOutcome::RuntimeRejected,
                       "Performance dispatch exceeded capacity");
    }
}
} // namespace talas::app
