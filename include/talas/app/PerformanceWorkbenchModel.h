#pragma once

#include "talas/project/ProjectDocument.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class PerformanceActivitySource
{
    None,
    UserInterface,
    Midi,
    Osc,
    Snapshot,
    Macro
};

struct PerformanceTargetActivity
{
    talas::project::SnapshotTargetIdentity target;
    PerformanceActivitySource source = PerformanceActivitySource::None;

    bool operator==(const PerformanceTargetActivity&) const = default;
};

struct PerformanceProjectionContext
{
    std::vector<std::string> directTargetIdentities;
    std::vector<std::string> unavailableMidiSourceIdentifiers;
    std::vector<PerformanceTargetActivity> recentActivity;
    std::optional<std::string> activeSnapshotBankId;
};

struct PerformanceControlViewModel
{
    std::string controlId;
    talas::project::SnapshotTargetIdentity target;
    talas::project::ProjectPerformanceControlKind kind =
        talas::project::ProjectPerformanceControlKind::Fader;
    std::string displayName;
    talas::project::CanvasPosition position;
    bool visible = true;
    bool directEligible = false;
    bool locked = false;
    bool midiMapped = false;
    bool midiLegacy = false;
    bool midiSourceUnavailable = false;
    bool oscInputMapped = false;
    bool oscFeedbackMapped = false;
    PerformanceActivitySource recentActivity = PerformanceActivitySource::None;
    std::vector<talas::project::ProjectPerformanceModulationSlot> modulationSlots;

    bool operator==(const PerformanceControlViewModel&) const = default;
};

struct PerformancePanelProjection
{
    std::vector<PerformanceControlViewModel> controls;
    std::vector<talas::project::ProjectPerformanceMeterPresentation> meters;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty();
    }
};

struct PerformanceTargetValue
{
    talas::project::SnapshotTargetIdentity target;
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 1.0;
    bool directEligible = false;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;

    bool operator==(const PerformanceTargetValue&) const = default;
};

struct PerformanceMacroInput
{
    std::string controlId;
    double value = 0.0;

    bool operator==(const PerformanceMacroInput&) const = default;
};

struct PerformanceMacroReductionResult
{
    std::vector<talas::project::SnapshotValueEntry> values;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty();
    }
};

[[nodiscard]] PerformancePanelProjection buildPerformancePanelProjection(
    const talas::project::ProjectDocument& document,
    const PerformanceProjectionContext& context);

[[nodiscard]] PerformanceMacroReductionResult reducePerformanceMacros(
    const talas::project::ProjectDocument& document,
    std::span<const PerformanceMacroInput> inputs,
    std::span<const PerformanceTargetValue> targetValues,
    std::optional<std::string_view> activeSnapshotBankId = std::nullopt);
}
