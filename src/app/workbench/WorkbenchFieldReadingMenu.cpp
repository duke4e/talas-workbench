#include "app/workbench/WorkbenchComponentInternal.h"

#include <cmath>

namespace talas::app::workbench
{
namespace
{
enum FieldReadingItem
{
    roundAsTheManual,
    beatAtSixty,
    linearSOSPitch,
    fieldReadingItemCount
};
}

// How the project's fields read rounded and truncated, a beat and a number
// given to inSOSPitch (format 156), each ticked where the project takes that
// item's reading.
void WorkbenchComponent::appendFieldReadingMenu(juce::PopupMenu& menu)
{
    const auto& definitions = projectBridge_.editor().document().expressionDefinitions;
    juce::PopupMenu reading;
    reading.addItem(fieldReadingMenuItemBase + roundAsTheManual,
        "Rounded Half Up and Truncated Down", true,
        definitions.rounding == talas::expr::RoundingRule::ManualTable);
    // A tempo the project file gives other than 60 shows as it is, ticked,
    // and choosing it goes back to the Timeline's tempo.
    const auto tempo = definitions.fieldBeatTempoBpm.value_or(talas::expr::newProjectFieldBeatTempoBpm);
    reading.addItem(fieldReadingMenuItemBase + beatAtSixty,
        "Beat at Quarter Note = " + juce::String{tempo, tempo == std::floor(tempo) ? 0 : 2}, true,
        definitions.fieldBeatTempoBpm.has_value());
    reading.addItem(fieldReadingMenuItemBase + linearSOSPitch, "inSOSPitch Reads Plain Numbers Linearly", true,
        definitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Linear);
    menu.addSubMenu("Field Reading", reading);
}

bool WorkbenchComponent::selectFieldReading(int menuItemId)
{
    const auto item = menuItemId - fieldReadingMenuItemBase;
    if (item < 0 || item >= fieldReadingItemCount)
        return false;
    auto definitions = projectBridge_.editor().document().expressionDefinitions;
    if (item == roundAsTheManual)
    {
        definitions.rounding = definitions.rounding == talas::expr::RoundingRule::ManualTable
            ? talas::expr::RoundingRule::Smalltalk
            : talas::expr::RoundingRule::ManualTable;
    }
    else if (item == beatAtSixty)
    {
        definitions.fieldBeatTempoBpm = definitions.fieldBeatTempoBpm.has_value()
            ? std::nullopt
            : std::optional<double>{talas::expr::newProjectFieldBeatTempoBpm};
    }
    else
    {
        definitions.unitlessSOSPitch = definitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Linear
            ? talas::expr::UnitlessSOSPitch::Hertz
            : talas::expr::UnitlessSOSPitch::Linear;
    }
    const auto set = projectBridge_.editor().setFieldReadingSettings(
        definitions.rounding, definitions.fieldBeatTempoBpm, definitions.unitlessSOSPitch);
    if (!set.ok())
    {
        setStatus("The fields' reading could not be changed: " + juceString(set.message));
        return true;
    }
    documentChanged();
    setStatus(item == roundAsTheManual
            ? juce::String{definitions.rounding == talas::expr::RoundingRule::ManualTable
                    ? "Fields round half up and truncate down: -2.5 rounded is -2, -2.5 truncated -3"
                    : "Fields round as Smalltalk does: -2.5 rounded is -3, -2.5 truncated -2"}
        : item == beatAtSixty
            ? juce::String{definitions.fieldBeatTempoBpm.has_value()
                    ? "A beat in a field lasts a quarter note at 60, the Timeline keeping its own tempo"
                    : "A beat in a field follows the Timeline's tempo"}
            : juce::String{definitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Linear
                    ? "inSOSPitch reads a plain number linearly: 11025.0 inSOSPitch is 0.5"
                    : "inSOSPitch reads a plain number as hertz: 11025.0 inSOSPitch is 14/15"});
    return true;
}
}
