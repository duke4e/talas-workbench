#include "talas/app/WorkbenchDeviceMapping.h"

#include <algorithm>

namespace talas::app
{
WorkbenchOutputChannelRoutes workbenchOutputChannelRoutes(
    std::span<const std::uint8_t> activePhysicalChannels,
    std::span<const std::int32_t> savedOneBasedPhysicalOrder) noexcept
{
    WorkbenchOutputChannelMap physicalToCompact{};
    physicalToCompact.fill(-1);
    std::size_t activeCount = 0;
    for (std::size_t physical = 0;
         physical < std::min(activePhysicalChannels.size(), physicalToCompact.size());
         ++physical)
    {
        if (activePhysicalChannels[physical] != 0)
            physicalToCompact[physical] = static_cast<int>(activeCount++);
    }

    WorkbenchOutputChannelRoutes result{};
    if (!savedOneBasedPhysicalOrder.empty())
    {
        std::array<std::uint8_t, workbenchMaximumDeviceChannels> seen{};
        const auto boundedSavedCount = std::min(
            savedOneBasedPhysicalOrder.size(),
            result.size());
        for (std::size_t ordinal = 0; ordinal < boundedSavedCount; ++ordinal)
        {
            const auto physical = savedOneBasedPhysicalOrder[ordinal] - 1;
            if (physical < 0
                || physical >= static_cast<std::int32_t>(physicalToCompact.size()))
            {
                continue;
            }
            const auto index = static_cast<std::size_t>(physical);
            result[ordinal].oneBasedPhysicalChannel =
                static_cast<int>(index + 1);
            if (seen[index] != 0)
                continue;
            seen[index] = 1;
            result[ordinal].compactDeviceIndex =
                physicalToCompact[index];
        }
        return result;
    }

    std::size_t ordinal = 0;
    for (std::size_t physical = 0;
         physical < physicalToCompact.size();
         ++physical)
    {
        const auto compact = physicalToCompact[physical];
        if (compact >= 0)
        {
            result[ordinal++] = {
                compact,
                static_cast<int>(physical + 1),
            };
        }
    }
    return result;
}

WorkbenchOutputChannelMap workbenchOutputChannelMap(
    std::span<const std::uint8_t> activePhysicalChannels,
    std::span<const std::int32_t> savedOneBasedPhysicalOrder) noexcept
{
    const auto routes = workbenchOutputChannelRoutes(
        activePhysicalChannels,
        savedOneBasedPhysicalOrder);
    WorkbenchOutputChannelMap result{};
    for (std::size_t ordinal = 0;
         ordinal < result.size();
         ++ordinal)
    {
        result[ordinal] = routes[ordinal].compactDeviceIndex;
    }
    return result;
}

bool workbenchHistoryStepReopensDevices(
    const talas::project::ProjectDeviceSettings& before,
    const talas::project::ProjectDeviceSettings& after)
{
    return before != after;
}

WorkbenchDeviceNotificationAction workbenchDeviceNotificationAction(
    bool settingsDialogOpen,
    const talas::project::ProjectDeviceSettings& project,
    const talas::project::ProjectDeviceSettings& host)
{
    if (project == host)
        return WorkbenchDeviceNotificationAction::Ignore;
    return settingsDialogOpen
        ? WorkbenchDeviceNotificationAction::RecordInProject
        : WorkbenchDeviceNotificationAction::KeepProjectPreference;
}
}
