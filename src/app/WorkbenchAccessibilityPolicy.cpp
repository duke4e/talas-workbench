#include "talas/app/WorkbenchAccessibilityPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::app
{
float workbenchInterfaceScaleFactor(
    talas::project::WorkspaceInterfaceScale scale) noexcept
{
    switch (scale)
    {
    case talas::project::WorkspaceInterfaceScale::Percent100:
        return 1.0F;
    case talas::project::WorkspaceInterfaceScale::Percent115:
        return 1.15F;
    case talas::project::WorkspaceInterfaceScale::Percent130:
        return 1.30F;
    case talas::project::WorkspaceInterfaceScale::Percent150:
        return 1.50F;
    }
    return 0.0F;
}

int workbenchScaledLogicalPixels(
    int basePixels,
    talas::project::WorkspaceInterfaceScale interfaceScale) noexcept
{
    if (basePixels <= 0
        || !talas::project::isKnownWorkspaceInterfaceScale(interfaceScale))
        return 0;

    const auto percent = static_cast<std::int64_t>(interfaceScale);
    const auto scaled = static_cast<std::int64_t>(basePixels) * percent;
    if (scaled > static_cast<std::int64_t>(std::numeric_limits<int>::max())
            * 100
        - 50)
    {
        return std::numeric_limits<int>::max();
    }
    return std::max(basePixels, static_cast<int>((scaled + 50) / 100));
}

std::optional<WorkbenchAccessibilityProjection>
workbenchAccessibilityProjection(
    const talas::project::WorkspaceAccessibilityPreferences& preferences,
    double hostScale,
    bool hostReduceMotion) noexcept
{
    const auto interfaceScale = workbenchInterfaceScaleFactor(
        preferences.interfaceScale);
    if (interfaceScale == 0.0F || !std::isfinite(hostScale)
        || hostScale < 0.5 || hostScale > 8.0)
    {
        return std::nullopt;
    }

    WorkbenchAccessibilityProjection result;
    result.interfaceScalePreset_ = preferences.interfaceScale;
    result.interfaceScale_ = interfaceScale;
    result.hostScale_ = static_cast<float>(hostScale);
    result.effectivePhysicalScale_ = static_cast<float>(
        static_cast<double>(interfaceScale) * hostScale);
    result.enhancedContrast_ = preferences.enhancedContrast;
    result.reduceMotion_ = preferences.reduceMotion || hostReduceMotion;
    result.contextHelpVisible_ = preferences.contextHelpVisible;
    result.minimumAuthoredTextPixels_ = workbenchScaledLogicalPixels(
        10, preferences.interfaceScale);
    result.toolbarHeight_ = workbenchScaledLogicalPixels(
        46, preferences.interfaceScale);
    result.statusHeight_ = workbenchScaledLogicalPixels(
        20, preferences.interfaceScale);
    result.controlHeight_ = workbenchScaledLogicalPixels(
        32, preferences.interfaceScale);
    result.compactControlHeight_ = workbenchScaledLogicalPixels(
        24, preferences.interfaceScale);
    result.panelPadding_ = workbenchScaledLogicalPixels(
        8, preferences.interfaceScale);
    result.gutter_ = workbenchScaledLogicalPixels(
        8, preferences.interfaceScale);
    result.focusOutlinePixels_ = workbenchScaledLogicalPixels(
        2, preferences.interfaceScale);
    return result;
}
}
