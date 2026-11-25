#include "talas/app/PerformanceWorkbenchModel.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::SnapshotTargetIdentity eventTarget(std::string name)
{
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::EventValue;
    target.eventName = std::move(name);
    return target;
}

talas::project::ProjectDocument performanceDocument()
{
    talas::project::ProjectDocument document;
    talas::project::VirtualControl first;
    first.eventName = "MacroA";
    first.minimum = -1.0;
    first.maximum = 1.0;
    first.midiMapping = talas::project::MidiControlMapping{
        1,
        20,
        talas::project::MidiControlSourceMode::ExactInput,
        "controller-a",
    };
    document.virtualControls.push_back(first);
    talas::project::VirtualControl second;
    second.eventName = "MacroB";
    document.virtualControls.push_back(second);
    talas::project::VirtualControl destination;
    destination.eventName = "Gain";
    destination.minimum = 0.0;
    destination.maximum = 10.0;
    destination.value = 5.0;
    destination.oscMapping = talas::project::OscControlMapping{"/gain"};
    destination.oscEgressMapping = talas::project::OscEgressMapping{"/gain/out"};
    document.virtualControls.push_back(destination);
    talas::project::reconcileProjectPresentation(document);

    auto* macroA = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    auto* macroB = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroB", talas::expr::ValueDomain::Scalar);
    const auto* gain = talas::project::findProjectPerformanceControlPresentation(
        document, "Gain", talas::expr::ValueDomain::Scalar);
    macroA->kind = talas::project::ProjectPerformanceControlKind::Macro;
    macroA->modulationSlots = {{gain->target, 1.0}};
    macroB->kind = talas::project::ProjectPerformanceControlKind::Macro;
    macroB->modulationSlots = {{gain->target, 0.5}};
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {640, 96}, true});
    return document;
}

void testProjectionCombinesMappingEligibilityLockAndActivity()
{
    auto document = performanceDocument();
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "live";
    bank.displayName = "Live";
    bank.parameterPolicies.push_back({eventTarget("Gain"), true, std::nullopt});
    document.snapshotLibrary.banks.push_back(bank);

    talas::app::PerformanceProjectionContext context;
    for (const auto& control : document.presentation.performanceControls)
    {
        context.directTargetIdentities.push_back(
            talas::project::snapshotTargetIdentityString(control.target));
    }
    context.unavailableMidiSourceIdentifiers.push_back("controller-a");
    context.recentActivity.push_back(
        {eventTarget("Gain"), talas::app::PerformanceActivitySource::Osc});
    context.activeSnapshotBankId = "live";

    const auto projection = talas::app::buildPerformancePanelProjection(
        document, context);
    const auto macro = std::ranges::find_if(
        projection.controls,
        [](const auto& control) {
            return control.target.eventName == "MacroA";
        });
    const auto gain = std::ranges::find_if(
        projection.controls,
        [](const auto& control) {
            return control.target.eventName == "Gain";
        });
    require(projection.ok() && projection.controls.size() == 3
            && projection.meters.size() == 1
            && macro != projection.controls.end()
            && macro->directEligible && macro->midiMapped
            && !macro->midiLegacy && macro->midiSourceUnavailable
            && gain != projection.controls.end()
            && gain->directEligible && gain->locked
            && gain->oscInputMapped && gain->oscFeedbackMapped
            && gain->recentActivity == talas::app::PerformanceActivitySource::Osc,
        "Performance projection should combine portable identity with exact "
        "mapping, availability, lock, activity, and meter truth");
}

void testMacroReductionIsDeterministicBoundedAndLockAware()
{
    auto document = performanceDocument();
    const auto* macroA = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    const auto* macroB = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroB", talas::expr::ValueDomain::Scalar);
    const auto* gain = talas::project::findProjectPerformanceControlPresentation(
        document, "Gain", talas::expr::ValueDomain::Scalar);
    const std::vector<talas::app::PerformanceTargetValue> states{
        {macroA->target, 0.0, -1.0, 1.0, true},
        {macroB->target, 0.5, 0.0, 1.0, true},
        {gain->target, 5.0, 0.0, 10.0, true},
    };
    const std::vector<talas::app::PerformanceMacroInput> inputs{
        {macroA->controlId, 1.0},
        {macroB->controlId, 0.0},
    };
    const auto reduced = talas::app::reducePerformanceMacros(
        document, inputs, states);
    const auto gainValue = std::ranges::find_if(
        reduced.values,
        [](const auto& value) {
            return value.target.eventName == "Gain";
        });
    require(reduced.ok() && reduced.values.size() == 3
            && gainValue != reduced.values.end()
            && gainValue->value.number == 7.5,
        "macro reducer should sum normalized bipolar depth offsets, clamp, "
        "and emit source plus fan-out in one bounded result");

    auto reversed = inputs;
    std::ranges::reverse(reversed);
    require(talas::app::reducePerformanceMacros(document, reversed, states).values
            == reduced.values,
        "macro reduction should be deterministic independent of input order");

    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "locked";
    bank.displayName = "Locked";
    bank.parameterPolicies.push_back({gain->target, true, std::nullopt});
    document.snapshotLibrary.banks.push_back(bank);
    const auto locked = talas::app::reducePerformanceMacros(
        document, inputs, states, "locked");
    require(locked.ok() && locked.values.size() == 2
            && std::ranges::none_of(locked.values, [](const auto& value) {
                return value.target.eventName == "Gain";
            }),
        "active snapshot lock should exclude a macro destination without "
        "blocking the exact source transaction");

    auto chained = performanceDocument();
    auto* chainedA =
        talas::project::findProjectPerformanceControlPresentation(
            chained, "MacroA", talas::expr::ValueDomain::Scalar);
    const auto* chainedB =
        talas::project::findProjectPerformanceControlPresentation(
            chained, "MacroB", talas::expr::ValueDomain::Scalar);
    chainedA->modulationSlots = {{chainedB->target, 0.5}};
    const auto chainedResult = talas::app::reducePerformanceMacros(
        chained, inputs, states);
    const auto chainedBValue = std::ranges::find_if(
        chainedResult.values,
        [](const auto& value) {
            return value.target.eventName == "MacroB";
        });
    require(chainedResult.ok() && chainedResult.values.size() == 3
            && chainedBValue != chainedResult.values.end()
            && chainedBValue->value.number == 0.25,
        "an acyclic macro-source destination should reduce to one exact target "
        "entry rather than duplicate direct and modulation values");

    auto duplicateStates = states;
    duplicateStates.push_back(states.front());
    require(!talas::app::reducePerformanceMacros(
                document, inputs, duplicateStates)
                 .ok(),
        "macro reduction should reject ambiguous duplicate target state");
}

void testMacroCyclesFailClosed()
{
    auto document = performanceDocument();
    auto* macroA = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroA", talas::expr::ValueDomain::Scalar);
    auto* macroB = talas::project::findProjectPerformanceControlPresentation(
        document, "MacroB", talas::expr::ValueDomain::Scalar);
    macroA->modulationSlots = {{macroB->target, 1.0}};
    macroB->modulationSlots = {{macroA->target, 1.0}};
    const std::vector<talas::app::PerformanceTargetValue> states{
        {macroA->target, 0.0, -1.0, 1.0, true},
        {macroB->target, 0.5, 0.0, 1.0, true},
    };
    const std::vector<talas::app::PerformanceMacroInput> inputs{
        {macroA->controlId, 0.5},
    };
    require(!talas::app::buildPerformancePanelProjection(document, {}).ok()
            && !talas::app::reducePerformanceMacros(document, inputs, states).ok(),
        "cyclic Performance macros should fail closed in projection and "
        "runtime reduction");
}
}

int main()
{
    testProjectionCombinesMappingEligibilityLockAndActivity();
    testMacroReductionIsDeterministicBoundedAndLockAware();
    testMacroCyclesFailClosed();
    return 0;
}
