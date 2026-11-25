#pragma once

#include "talas/project/ProjectDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::app
{
inline constexpr std::size_t workbenchMaximumDeviceChannels = 8;

using WorkbenchOutputChannelMap =
    std::array<int, workbenchMaximumDeviceChannels>;

struct WorkbenchOutputChannelRoute
{
    int compactDeviceIndex = -1;
    int oneBasedPhysicalChannel = -1;

    bool operator==(const WorkbenchOutputChannelRoute&) const = default;
};

using WorkbenchOutputChannelRoutes =
    std::array<
        WorkbenchOutputChannelRoute,
        workbenchMaximumDeviceChannels>;

[[nodiscard]] WorkbenchOutputChannelRoutes
workbenchOutputChannelRoutes(
    std::span<const std::uint8_t> activePhysicalChannels,
    std::span<const std::int32_t> savedOneBasedPhysicalOrder) noexcept;

[[nodiscard]] WorkbenchOutputChannelMap workbenchOutputChannelMap(
    std::span<const std::uint8_t> activePhysicalChannels,
    std::span<const std::int32_t> savedOneBasedPhysicalOrder) noexcept;

// Undo and Redo reopen audio and MIDI devices only when the history step
// changed the project's device settings. Reopening restarts every MIDI
// input, which stops external MIDI Clock/MTC sync and cancels MIDI learn.
[[nodiscard]] bool workbenchHistoryStepReopensDevices(
    const talas::project::ProjectDeviceSettings& before,
    const talas::project::ProjectDeviceSettings& after);

enum class WorkbenchDeviceNotificationAction
{
    Ignore,
    RecordInProject,
    KeepProjectPreference,
};

// What a device-manager notification does to the project. Only a change
// made in Audio & MIDI Settings becomes project history; a host fallback,
// such as after the saved device is unplugged, keeps the saved preference.
[[nodiscard]] WorkbenchDeviceNotificationAction
workbenchDeviceNotificationAction(
    bool settingsDialogOpen,
    const talas::project::ProjectDeviceSettings& project,
    const talas::project::ProjectDeviceSettings& host);
}
