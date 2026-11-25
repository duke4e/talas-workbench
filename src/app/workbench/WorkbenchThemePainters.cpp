#include "app/workbench/WorkbenchTheme.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// The theme's drawn parts: knobs, pads, transport glyphs, the Talas brand
// and status pills, and the contrast arithmetic they read their text by.
namespace talas::app::workbench
{
namespace
{
[[nodiscard]] float linearChannel(float channel) noexcept
{
    return channel <= 0.03928F ? channel / 12.92F : std::pow((channel + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float relativeLuminance(juce::Colour colour) noexcept
{
    return 0.2126F * linearChannel(colour.getFloatRed()) + 0.7152F * linearChannel(colour.getFloatGreen())
        + 0.0722F * linearChannel(colour.getFloatBlue());
}

juce::PathStrokeType roundStroke(float width)
{
    return juce::PathStrokeType{width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded};
}

// A gradient left to right across the area at its vertical centre, so an
// arc or a word reads violet to aqua as it goes.
[[nodiscard]] juce::ColourGradient horizontalAccent(juce::Rectangle<float> area, float alpha)
{
    return WorkbenchStyle::accentGradient(
        juce::Rectangle<float>{area.getX(), area.getCentreY(), area.getWidth(), 0.0F}, alpha);
}

// The x-height of the wordmark at a brand height, and its width in
// x-heights (from the t's crossbar to the end of the s, strokes included).
constexpr float wordmarkXHeight = 0.36F;
constexpr float wordmarkWidthInXHeights = 5.5F;

// "talas" in lowercase monoline strokes on a baseline at y = 0, one unit
// to the x-height: a t, a single-storey a, an l, an a and an s.
[[nodiscard]] juce::Path wordmarkStrokes()
{
    juce::Path path;
    // t
    path.startNewSubPath(0.22F, -1.32F);
    path.lineTo(0.22F, -0.3F);
    path.quadraticTo(0.22F, 0.0F, 0.5F, 0.0F);
    path.lineTo(0.6F, 0.0F);
    path.startNewSubPath(0.0F, -1.0F);
    path.lineTo(0.56F, -1.0F);
    const auto a = [&path](float x) {
        path.addEllipse(x, -1.0F, 1.0F, 1.0F);
        path.startNewSubPath(x + 1.0F, -1.0F);
        path.lineTo(x + 1.0F, 0.0F);
    };
    a(1.08F);
    // l
    path.startNewSubPath(2.56F, -1.5F);
    path.lineTo(2.56F, 0.0F);
    a(3.04F);
    // s
    const auto x = 4.52F;
    path.startNewSubPath(x + 0.74F, -0.84F);
    path.cubicTo(x + 0.66F, -0.95F, x + 0.52F, -1.0F, x + 0.38F, -1.0F);
    path.cubicTo(x + 0.18F, -1.0F, x + 0.04F, -0.9F, x + 0.04F, -0.74F);
    path.cubicTo(x + 0.04F, -0.58F, x + 0.2F, -0.53F, x + 0.39F, -0.5F);
    path.cubicTo(x + 0.6F, -0.47F, x + 0.78F, -0.42F, x + 0.78F, -0.24F);
    path.cubicTo(x + 0.78F, -0.07F, x + 0.6F, 0.0F, x + 0.39F, 0.0F);
    path.cubicTo(x + 0.18F, 0.0F, x + 0.05F, -0.08F, x + 0.0F, -0.17F);
    return path;
}
}

float WorkbenchStyle::contrastRatio(juce::Colour first, juce::Colour second)
{
    const auto a = relativeLuminance(first);
    const auto b = relativeLuminance(second);
    return (std::max(a, b) + 0.05F) / (std::min(a, b) + 0.05F);
}

juce::Colour WorkbenchStyle::readableTextOn(juce::Colour background)
{
    const auto opaque = background.withAlpha(1.0F);
    const auto frost = contrastRatio(workbenchTheme.primaryText, opaque);
    if (frost >= 4.5F)
        return workbenchTheme.primaryText;
    return contrastRatio(workbenchTheme.accentText, opaque) > frost ? workbenchTheme.accentText
                                                                     : workbenchTheme.primaryText;
}

juce::Colour WorkbenchStyle::readableToneOf(juce::Colour colour, juce::Colour background)
{
    const auto opaque = background.withAlpha(1.0F);
    for (int step = 0; step <= 20; ++step)
    {
        const auto tone = colour.withAlpha(1.0F).interpolatedWith(juce::Colours::white, static_cast<float>(step) * 0.05F);
        // A little over 4.5:1, so rounding never lets it fall short.
        if (contrastRatio(tone, opaque) >= 4.6F)
            return tone;
    }
    return juce::Colours::white;
}

void WorkbenchStyle::paintKnob(juce::Graphics& graphics, juce::Rectangle<float> area, float proportion,
    float startAngle, float endAngle, bool enabled)
{
    const auto size = std::min(area.getWidth(), area.getHeight());
    if (size < 10.0F)
        return;
    const auto enhanced = enhancedContrast();
    const auto alpha = enabled ? 1.0F : 0.45F;
    const auto centre = area.getCentre();
    // Room for the arc's halo around the ring.
    const auto outer = size * 0.5F - std::max(1.5F, size * 0.045F);
    const auto track = std::max(3.0F, outer * 0.15F);
    const auto arcRadius = outer - track * 0.5F;
    const auto value = std::clamp(proportion, 0.0F, 1.0F);
    const auto angle = startAngle + value * (endAngle - startAngle);
    const auto knob = juce::Rectangle<float>{outer * 2.0F, outer * 2.0F}.withCentre(centre);
    const auto arc = [&](float from, float to) {
        juce::Path path;
        path.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0F, from, to, true);
        return path;
    };

    // The track, sunk into the surface.
    const auto trackPath = arc(startAngle, endAngle);
    graphics.setColour((enhanced ? workbenchTheme.enhancedOutline : juce::Colours::black.withAlpha(0.45F))
                           .withMultipliedAlpha(alpha));
    graphics.strokePath(trackPath, roundStroke(track + (enhanced ? 2.0F : 1.5F)));
    graphics.setColour(workbenchTheme.well.interpolatedWith(workbenchTheme.surface, 0.3F).withMultipliedAlpha(alpha));
    graphics.strokePath(trackPath, roundStroke(track));

    // The value's arc in the gradient, a halo beneath it.
    if (value > 0.0F)
    {
        const auto valuePath = arc(startAngle, angle);
        if (!enhanced && enabled)
        {
            graphics.setGradientFill(horizontalAccent(knob, 0.12F));
            graphics.strokePath(valuePath, roundStroke(track * 3.4F));
            graphics.setGradientFill(horizontalAccent(knob, 0.24F));
            graphics.strokePath(valuePath, roundStroke(track * 2.0F));
        }
        graphics.setGradientFill(horizontalAccent(knob, alpha));
        graphics.strokePath(valuePath, roundStroke(track));
    }

    // The cap: raised glass with a sheen over its upper half.
    const auto capRadius = arcRadius - track * 1.5F;
    if (capRadius > 3.0F)
    {
        const auto cap = juce::Rectangle<float>{capRadius * 2.0F, capRadius * 2.0F}.withCentre(centre);
        if (!enhanced)
        {
            graphics.setColour(juce::Colours::black.withAlpha(0.22F * alpha));
            graphics.fillEllipse(cap.translated(0.0F, capRadius * 0.16F).expanded(capRadius * 0.08F));
            graphics.setColour(juce::Colours::black.withAlpha(0.3F * alpha));
            graphics.fillEllipse(cap.translated(0.0F, capRadius * 0.08F));
        }
        graphics.setGradientFill(juce::ColourGradient{juce::Colour{0xff323a57}.withMultipliedAlpha(alpha), centre.x,
            cap.getY(), juce::Colour{0xff151a29}.withMultipliedAlpha(alpha), centre.x, cap.getBottom(), false});
        graphics.fillEllipse(cap);
        if (!enhanced)
        {
            juce::Graphics::ScopedSaveState clipped(graphics);
            juce::Path capShape;
            capShape.addEllipse(cap);
            graphics.reduceClipRegion(capShape);
            graphics.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(0.16F * alpha), centre.x,
                cap.getY(), juce::Colours::white.withAlpha(0.0F), centre.x, cap.getY() + capRadius * 1.1F, false});
            graphics.fillEllipse(cap.reduced(capRadius * 0.1F, 0.0F).withHeight(capRadius * 1.15F));
        }
        graphics.setColour((enhanced ? workbenchTheme.enhancedOutline : juce::Colours::white.withAlpha(0.13F))
                               .withMultipliedAlpha(alpha));
        graphics.drawEllipse(cap.reduced(0.5F), 1.0F);
        juce::Path pointer;
        pointer.startNewSubPath(centre.getPointOnCircumference(capRadius * 0.28F, angle));
        pointer.lineTo(centre.getPointOnCircumference(capRadius * 0.76F, angle));
        graphics.setColour(workbenchTheme.primaryText.withMultipliedAlpha(alpha));
        graphics.strokePath(pointer, roundStroke(std::max(2.0F, capRadius * 0.15F)));
    }

    // The tip riding the arc, where the value is.
    const auto tip = centre.getPointOnCircumference(arcRadius, angle);
    if (enabled && !enhanced)
    {
        graphics.setColour(accentAt(value).withAlpha(0.4F));
        graphics.fillEllipse(juce::Rectangle<float>{track * 2.1F, track * 2.1F}.withCentre(tip));
    }
    graphics.setColour(workbenchTheme.primaryText.withMultipliedAlpha(alpha));
    graphics.fillEllipse(juce::Rectangle<float>{track * 1.1F, track * 1.1F}.withCentre(tip));
}

void WorkbenchStyle::paintPad(juce::Graphics& graphics, juce::Rectangle<float> area, bool on, bool enabled,
    const juce::String& text)
{
    if (area.isEmpty())
        return;
    const auto enhanced = enhancedContrast();
    const auto alpha = enabled ? 1.0F : 0.45F;
    const auto radius = std::min(8.0F, area.getHeight() * 0.22F);
    if (on)
    {
        graphics.setGradientFill(accentGradient(area, alpha));
        graphics.fillRoundedRectangle(area, radius);
        if (!enhanced)
        {
            juce::Graphics::ScopedSaveState clipped(graphics);
            juce::Path shape;
            shape.addRoundedRectangle(area, radius);
            graphics.reduceClipRegion(shape);
            graphics.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(0.3F * alpha), area.getX(),
                area.getY(), juce::Colours::white.withAlpha(0.0F), area.getX(), area.getCentreY(), false});
            graphics.fillRect(area.withHeight(area.getHeight() * 0.5F));
        }
        graphics.setColour((enhanced ? workbenchTheme.enhancedOutline : juce::Colours::white.withAlpha(0.35F))
                               .withMultipliedAlpha(alpha));
        graphics.drawRoundedRectangle(area.reduced(0.5F), radius, enhanced ? 1.5F : 1.0F);
    }
    else
    {
        paintWell(graphics, area, radius);
        if (enhanced)
        {
            graphics.setColour(workbenchTheme.enhancedOutline.withMultipliedAlpha(alpha));
            graphics.drawRoundedRectangle(area.reduced(0.75F), radius, 1.5F);
        }
    }
    // A lamp at the left, lit with the pad, and the state written out.
    const auto lampSize = std::clamp(area.getHeight() * 0.16F, 5.0F, 9.0F);
    auto content = area.reduced(std::max(8.0F, area.getHeight() * 0.18F), 0.0F);
    const auto lamp = juce::Rectangle<float>{lampSize, lampSize}.withCentre(
        {content.getX() + lampSize * 0.5F, area.getCentreY()});
    if (on)
    {
        if (!enhanced)
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.35F * alpha));
            graphics.fillEllipse(lamp.expanded(lampSize * 0.35F));
        }
        graphics.setColour(juce::Colours::white.withAlpha(alpha));
        graphics.fillEllipse(lamp);
        graphics.setColour(workbenchTheme.accentText.withAlpha(0.6F * alpha));
        graphics.drawEllipse(lamp, 1.0F);
    }
    else
    {
        graphics.setColour(juce::Colours::black.withAlpha(0.5F * alpha));
        graphics.fillEllipse(lamp);
        graphics.setColour(workbenchTheme.interactiveOutline.withMultipliedAlpha(alpha));
        graphics.drawEllipse(lamp, 1.0F);
    }
    content.removeFromLeft(lampSize + 6.0F);
    graphics.setColour((on ? workbenchTheme.accentText : workbenchTheme.primaryText).withMultipliedAlpha(alpha));
    graphics.setFont(WorkbenchType::bodyStrong());
    graphics.drawFittedText(text, content.toNearestInt(), juce::Justification::centredLeft, 1);
}

void WorkbenchStyle::paintGlyph(juce::Graphics& graphics, juce::Rectangle<float> area, Glyph glyph, juce::Colour colour)
{
    const auto size = std::min(area.getWidth(), area.getHeight());
    if (size <= 1.0F)
        return;
    const auto centre = area.getCentre();
    juce::Path path;
    switch (glyph)
    {
    case Glyph::Play:
    {
        const auto width = size * 0.88F;
        path.addTriangle(centre.x - width * 0.42F, centre.y - size * 0.5F, centre.x - width * 0.42F,
            centre.y + size * 0.5F, centre.x + width * 0.58F, centre.y);
        path = path.createPathWithRoundedCorners(size * 0.14F);
        break;
    }
    case Glyph::Pause:
    {
        const auto bar = size * 0.3F;
        const auto gap = size * 0.22F;
        path.addRoundedRectangle(centre.x - gap * 0.5F - bar, centre.y - size * 0.46F, bar, size * 0.92F, size * 0.08F);
        path.addRoundedRectangle(centre.x + gap * 0.5F, centre.y - size * 0.46F, bar, size * 0.92F, size * 0.08F);
        break;
    }
    case Glyph::Stop:
        path.addRoundedRectangle(juce::Rectangle<float>{size * 0.84F, size * 0.84F}.withCentre(centre), size * 0.13F);
        break;
    case Glyph::Record:
        path.addEllipse(juce::Rectangle<float>{size * 0.9F, size * 0.9F}.withCentre(centre));
        break;
    }
    graphics.setColour(colour);
    graphics.fillPath(path);
}

float WorkbenchStyle::brandWidth(float height, bool withWordmark) noexcept
{
    const auto mark = height;
    if (!withWordmark)
        return mark;
    return mark + height * 0.3F + height * wordmarkXHeight * wordmarkWidthInXHeights;
}

void WorkbenchStyle::paintBrand(juce::Graphics& graphics, juce::Rectangle<float> area, bool withWordmark)
{
    const auto height = std::min(area.getHeight(), area.getWidth());
    if (height < 8.0F)
        return;
    const auto enhanced = enhancedContrast();
    // The mark: a glass roundel lit with the gradient, a wave cut across it.
    const auto roundel = juce::Rectangle<float>{area.getX(), area.getCentreY() - height * 0.5F, height, height}
                             .reduced(height * 0.04F);
    if (!enhanced)
    {
        graphics.setGradientFill(accentGradient(roundel.expanded(2.0F), 0.22F));
        graphics.fillEllipse(roundel.expanded(2.0F));
    }
    graphics.setGradientFill(accentGradient(roundel));
    graphics.fillEllipse(roundel);
    if (!enhanced)
    {
        juce::Graphics::ScopedSaveState clipped(graphics);
        juce::Path shape;
        shape.addEllipse(roundel);
        graphics.reduceClipRegion(shape);
        graphics.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(0.42F), roundel.getCentreX(),
            roundel.getY(), juce::Colours::white.withAlpha(0.0F), roundel.getCentreX(), roundel.getCentreY(), false});
        graphics.fillEllipse(roundel.reduced(roundel.getWidth() * 0.12F, 0.0F).withHeight(roundel.getHeight() * 0.55F));
    }
    juce::Path wave;
    const auto left = roundel.getX() + roundel.getWidth() * 0.17F;
    const auto span = roundel.getWidth() * 0.66F;
    const auto amplitude = roundel.getHeight() * 0.15F;
    constexpr int steps = 28;
    for (int step = 0; step <= steps; ++step)
    {
        const auto t = static_cast<float>(step) / static_cast<float>(steps);
        const auto point = juce::Point<float>{left + span * t,
            roundel.getCentreY() - amplitude * std::sin(t * 1.5F * 2.0F * std::numbers::pi_v<float>)};
        step == 0 ? wave.startNewSubPath(point) : wave.lineTo(point);
    }
    graphics.setColour(workbenchTheme.accentText);
    graphics.strokePath(wave, roundStroke(std::max(1.6F, height * 0.095F)));
    graphics.setColour(juce::Colours::white.withAlpha(enhanced ? 0.6F : 0.3F));
    graphics.drawEllipse(roundel.reduced(0.5F), 1.0F);
    if (!withWordmark)
        return;

    // The wordmark, stroked and filled with the gradient.
    const auto xHeight = height * wordmarkXHeight;
    const auto wordLeft = roundel.getRight() + height * 0.3F + xHeight * 0.1F;
    const auto baseline = area.getCentreY() + xHeight * 0.75F;
    auto strokes = wordmarkStrokes();
    strokes.applyTransform(juce::AffineTransform::scale(xHeight).translated(wordLeft, baseline));
    juce::Path word;
    roundStroke(std::max(1.5F, xHeight * 0.2F)).createStrokedPath(word, strokes);
    const auto wordBounds = word.getBounds();
    if (!enhanced)
    {
        graphics.setGradientFill(horizontalAccent(wordBounds, 0.18F));
        graphics.strokePath(word, roundStroke(2.0F));
    }
    graphics.setGradientFill(horizontalAccent(wordBounds, 1.0F));
    graphics.fillPath(word);
}

void WorkbenchStyle::paintPill(juce::Graphics& graphics, juce::Rectangle<float> area, juce::Colour colour,
    juce::Colour background, const juce::String& text, const juce::Font& font)
{
    if (area.isEmpty())
        return;
    const auto enhanced = enhancedContrast();
    const auto radius = area.getHeight() * 0.5F;
    const auto tint = colour.withAlpha(enhanced ? 0.3F : 0.24F);
    graphics.setColour(tint);
    graphics.fillRoundedRectangle(area, radius);
    graphics.setColour(enhanced ? readableToneOf(colour, background) : colour.withAlpha(0.7F));
    graphics.drawRoundedRectangle(area.reduced(0.5F), radius, 1.0F);
    graphics.setColour(readableToneOf(colour, background.withAlpha(1.0F).overlaidWith(tint)));
    graphics.setFont(font);
    graphics.drawText(text, area, juce::Justification::centred, false);
}
}
