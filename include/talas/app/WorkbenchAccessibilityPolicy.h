#pragma once

#include "talas/project/WorkspaceSessionState.h"

#include <optional>

namespace talas::app
{
class WorkbenchAccessibilityProjection
{
public:
    [[nodiscard]] talas::project::WorkspaceInterfaceScale
    interfaceScalePreset() const noexcept
    {
        return interfaceScalePreset_;
    }

    [[nodiscard]] float interfaceScale() const noexcept
    {
        return interfaceScale_;
    }

    [[nodiscard]] float hostScale() const noexcept { return hostScale_; }
    [[nodiscard]] float effectivePhysicalScale() const noexcept
    {
        return effectivePhysicalScale_;
    }
    [[nodiscard]] bool enhancedContrast() const noexcept
    {
        return enhancedContrast_;
    }
    [[nodiscard]] bool reduceMotion() const noexcept { return reduceMotion_; }
    [[nodiscard]] bool contextHelpVisible() const noexcept
    {
        return contextHelpVisible_;
    }
    [[nodiscard]] int minimumAuthoredTextPixels() const noexcept
    {
        return minimumAuthoredTextPixels_;
    }
    [[nodiscard]] int toolbarHeight() const noexcept { return toolbarHeight_; }
    [[nodiscard]] int statusHeight() const noexcept { return statusHeight_; }
    [[nodiscard]] int controlHeight() const noexcept { return controlHeight_; }
    [[nodiscard]] int compactControlHeight() const noexcept
    {
        return compactControlHeight_;
    }
    [[nodiscard]] int panelPadding() const noexcept { return panelPadding_; }
    [[nodiscard]] int gutter() const noexcept { return gutter_; }
    [[nodiscard]] int focusOutlinePixels() const noexcept
    {
        return focusOutlinePixels_;
    }

    bool operator==(const WorkbenchAccessibilityProjection&) const = default;

private:
    friend std::optional<WorkbenchAccessibilityProjection>
    workbenchAccessibilityProjection(
        const talas::project::WorkspaceAccessibilityPreferences&,
        double,
        bool) noexcept;

    WorkbenchAccessibilityProjection() = default;

    talas::project::WorkspaceInterfaceScale interfaceScalePreset_ =
        talas::project::WorkspaceInterfaceScale::Percent100;
    float interfaceScale_ = 1.0F;
    float hostScale_ = 1.0F;
    float effectivePhysicalScale_ = 1.0F;
    bool enhancedContrast_ = false;
    bool reduceMotion_ = false;
    bool contextHelpVisible_ = false;
    int minimumAuthoredTextPixels_ = 10;
    int toolbarHeight_ = 46;
    int statusHeight_ = 20;
    int controlHeight_ = 32;
    int compactControlHeight_ = 24;
    int panelPadding_ = 8;
    int gutter_ = 8;
    int focusOutlinePixels_ = 2;
};

[[nodiscard]] float workbenchInterfaceScaleFactor(
    talas::project::WorkspaceInterfaceScale scale) noexcept;

[[nodiscard]] std::optional<WorkbenchAccessibilityProjection>
workbenchAccessibilityProjection(
    const talas::project::WorkspaceAccessibilityPreferences& preferences,
    double hostScale,
    bool hostReduceMotion) noexcept;

[[nodiscard]] int workbenchScaledLogicalPixels(
    int basePixels,
    talas::project::WorkspaceInterfaceScale interfaceScale) noexcept;
}
