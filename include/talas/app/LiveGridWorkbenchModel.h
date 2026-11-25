#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class LiveGridCellState : std::uint8_t
{
    Ready,
    Preparing,
    Active,
    Queued,
    Stale,
    Missing,
    Failed,
    Unavailable,
};

struct LiveGridSourceContext
{
    talas::runtime::RealtimeLiveSetSourceKind kind =
        talas::runtime::RealtimeLiveSetSourceKind::Workbench;
    std::string sourceIdentifier;
    std::string oscAddress;
    std::int32_t midiChannel = 1;
    std::int32_t midiProgram = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t routeGeneration = 0;
    bool sourceAvailable = true;
};

struct LiveGridProjectionEnvironment
{
    std::span<const std::string> connectedMidiSourceIdentifiers;
    bool oscListening = false;
    std::string_view queuedLiveSetId;
    std::string_view queuedCellId;
    std::uint64_t queuedRequestGeneration = 0;
    std::string_view projectedLiveSetId;
};

struct LiveGridCellAction
{
    std::string liveSetId;
    std::string cellId;
    std::string displayName;
    std::string componentId;
    std::string targetSummary;
    std::string quantizationSummary;
    std::string mappingSummary;
    std::string reason;
    std::uint32_t row = 0;
    std::uint32_t column = 0;
    LiveGridCellState state = LiveGridCellState::Unavailable;
    bool canActivate = false;
    bool sourceMapped = false;
    bool midiMapped = false;
    bool midiSourceAvailable = false;
    bool oscMapped = false;
    bool oscSourceAvailable = false;
    bool workbenchMapped = false;
};

struct LiveGridDispatchResult
{
    LiveGridCellAction action;
    talas::runtime::LiveSetActivationResult activation;
};

[[nodiscard]] std::string_view liveGridCellStateLabel(
    LiveGridCellState state) noexcept;

[[nodiscard]] std::vector<LiveGridCellAction> liveGridProjection(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment = {});

[[nodiscard]] LiveGridCellAction liveGridCellAction(
    const talas::runtime::ProjectRuntimeBridge& bridge,
    std::string_view liveSetId,
    std::string_view cellId,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment = {});

[[nodiscard]] LiveGridDispatchResult dispatchLiveGridCell(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::string_view liveSetId,
    std::string_view cellId,
    const LiveGridSourceContext& source,
    const LiveGridProjectionEnvironment& environment = {});
}
