#pragma once

#include "talas/app/SoundIcons.h"
#include "talas/project/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// A Sound's icon, as the signal flow and the
// Sound file window draw it.
namespace talas::app::workbench
{
// The icon's outline in the area: its left edge the kind of input the Sound
// takes, its right edge the kind of output it gives, a pointed top a side
// effect.
[[nodiscard]] juce::Path soundIconOutline(juce::Rectangle<float> area, const talas::app::SoundIconShape& shape);
// The inner icon's elements in a row from the right of the area, each in a
// cell as high as the area.
void drawSoundIconGlyphs(juce::Graphics& graphics, juce::Rectangle<float> area,
    const std::vector<talas::app::IconGlyph>& glyphs);
// A class's drawn icon, large or small, its dots
// in the colour, as large as fits the area and centred in it; false when
// that icon has no dots.
bool drawSoundIconPicture(juce::Graphics& graphics, juce::Rectangle<float> area,
    const talas::project::ProjectSoundIcon& icon, bool large, juce::Colour colour);
}
