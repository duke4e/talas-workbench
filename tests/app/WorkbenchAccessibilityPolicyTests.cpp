#include "talas/app/WorkbenchAccessibilityPolicy.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testClosedScalePresetsComposeWithHostScaleExactlyOnce()
{
    using Scale = talas::project::WorkspaceInterfaceScale;
    constexpr std::array scales{
        Scale::Percent100,
        Scale::Percent115,
        Scale::Percent130,
        Scale::Percent150,
    };
    constexpr std::array expected{1.0F, 1.15F, 1.30F, 1.50F};
    for (std::size_t index = 0; index < scales.size(); ++index)
    {
        talas::project::WorkspaceAccessibilityPreferences preferences;
        preferences.interfaceScale = scales[index];
        const auto oneX = talas::app::workbenchAccessibilityProjection(
            preferences, 1.0, false);
        const auto twoX = talas::app::workbenchAccessibilityProjection(
            preferences, 2.0, false);
        require(oneX.has_value() && twoX.has_value(),
            "every closed interface scale should project");
        require(std::abs(oneX->interfaceScale() - expected[index]) < 0.0001F
                && std::abs(twoX->effectivePhysicalScale()
                        - expected[index] * 2.0F) < 0.0001F,
            "local and host scale should compose once without changing the local authority");
        require(oneX->minimumAuthoredTextPixels() >= 10
                && oneX->controlHeight() >= 32
                && oneX->focusOutlinePixels() >= 2,
            "larger presets must never shrink the established design floor");
    }
}

void testUnknownScaleAndInvalidHostEvidenceFailClosed()
{
    talas::project::WorkspaceAccessibilityPreferences preferences;
    preferences.interfaceScale =
        static_cast<talas::project::WorkspaceInterfaceScale>(125);
    require(!talas::app::workbenchAccessibilityProjection(
                preferences, 1.0, false).has_value(),
        "an unapproved scale must not be rounded into a plausible preset");

    preferences.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent100;
    require(!talas::app::workbenchAccessibilityProjection(
                preferences,
                std::numeric_limits<double>::quiet_NaN(), false).has_value()
            && !talas::app::workbenchAccessibilityProjection(
                preferences, 0.0, false).has_value()
            && !talas::app::workbenchAccessibilityProjection(
                preferences, 9.0, false).has_value(),
        "non-finite or unsupported host scale must fail before publication");
}

void testPreferencesProjectWithoutMutatingTheirMeaning()
{
    talas::project::WorkspaceAccessibilityPreferences preferences;
    preferences.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent130;
    preferences.enhancedContrast = true;
    preferences.reduceMotion = true;
    preferences.contextHelpVisible = true;
    const auto projection = talas::app::workbenchAccessibilityProjection(
        preferences, 2.0, false);
    require(projection.has_value()
            && projection->enhancedContrast()
            && projection->reduceMotion()
            && projection->contextHelpVisible()
            && projection->interfaceScale() == 1.3F
            && projection->hostScale() == 2.0F,
        "all local preferences should survive projection without reinterpretation");

    preferences.reduceMotion = false;
    const auto hostReduced = talas::app::workbenchAccessibilityProjection(
        preferences, 1.0, true);
    require(hostReduced.has_value() && hostReduced->reduceMotion(),
        "host reduce-motion evidence should conservatively suppress nonessential local motion");

    require(talas::app::workbenchScaledLogicalPixels(
                10, talas::project::WorkspaceInterfaceScale::Percent115) == 12,
        "closed scale arithmetic should round exact half pixels upward without float drift");
}
}

int main()
{
    testClosedScalePresetsComposeWithHostScaleExactlyOnce();
    testUnknownScaleAndInvalidHostEvidenceFailClosed();
    testPreferencesProjectWithoutMutatingTheirMeaning();
    return 0;
}
