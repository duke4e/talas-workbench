#include "app/workbench/WorkbenchSoundIconPainting.h"

#include "app/workbench/WorkbenchTheme.h"
#include "talas/app/SoundClassIcon.h"

#include <algorithm>

namespace talas::app::workbench
{
// The icon's outline, its left edge the
// kind of input the Sound takes, its right edge the kind of output it
// gives, and a pointed top a side effect.
juce::Path soundIconOutline(juce::Rectangle<float> area, const talas::app::SoundIconShape& shape)
{
    using talas::app::IconEdge;
    constexpr auto depth = 7.0F;
    constexpr auto small = 4.0F;
    const auto x0 = area.getX();
    const auto x1 = area.getRight();
    const auto top = area.getY() + (shape.pointedTop ? 6.0F : 0.0F);
    const auto y1 = area.getBottom();
    const auto cy = (top + y1) * 0.5F;
    const auto half = std::min(10.0F, (y1 - top) * 0.2F);
    const auto inset = [&](IconEdge edge) {
        return edge == IconEdge::Environment ? depth : edge == IconEdge::Constructor ? small : 0.0F;
    };
    juce::Path path;
    path.startNewSubPath(x0 + inset(shape.left), top);
    if (shape.pointedTop)
    {
        const auto cx = area.getCentreX();
        path.lineTo(cx - 10.0F, top);
        path.lineTo(cx, area.getY());
        path.lineTo(cx + 10.0F, top);
    }
    // The right edge, top to bottom: an output bulges out.
    switch (shape.right)
    {
    case IconEdge::Audio:
        path.lineTo(x1 - depth, top);
        path.quadraticTo(x1 + depth, cy, x1 - depth, y1);
        break;
    case IconEdge::Subaudio:
        path.lineTo(x1 - small, top);
        path.lineTo(x1 - small, cy - half);
        path.quadraticTo(x1 + small, cy, x1 - small, cy + half);
        path.lineTo(x1 - small, y1);
        break;
    case IconEdge::Transparent:
        path.lineTo(x1 - small, top);
        path.lineTo(x1 - small, cy - 2.0F * half);
        path.quadraticTo(x1 + small, cy - half, x1 - small, cy);
        path.quadraticTo(x1 + small, cy + half, x1 - small, cy + 2.0F * half);
        path.lineTo(x1 - small, y1);
        break;
    case IconEdge::Spectrum:
        path.lineTo(x1 - depth, top);
        path.lineTo(x1, cy);
        path.lineTo(x1 - depth, y1);
        break;
    case IconEdge::Variable:
        path.lineTo(x1 - small, top);
        path.lineTo(x1 - small, cy - 2.0F * half);
        path.lineTo(x1, cy - half);
        path.lineTo(x1 - small, cy);
        path.lineTo(x1, cy + half);
        path.lineTo(x1 - small, cy + 2.0F * half);
        path.lineTo(x1 - small, y1);
        break;
    default:
        path.lineTo(x1, top);
        path.lineTo(x1, y1);
        break;
    }
    // The left edge, bottom to top: an input dips in, an environment
    // or constructor points left.
    switch (shape.left)
    {
    case IconEdge::Audio:
        path.lineTo(x0, y1);
        path.quadraticTo(x0 + 2.0F * depth, cy, x0, top);
        break;
    case IconEdge::Subaudio:
    case IconEdge::Transparent:
        path.lineTo(x0, y1);
        path.lineTo(x0, cy + half);
        path.quadraticTo(x0 + 2.0F * small, cy, x0, cy - half);
        path.lineTo(x0, top);
        break;
    case IconEdge::Spectrum:
        path.lineTo(x0, y1);
        path.lineTo(x0 + depth, cy);
        path.lineTo(x0, top);
        break;
    case IconEdge::Environment:
        path.lineTo(x0 + depth, y1);
        path.lineTo(x0, cy);
        path.lineTo(x0 + depth, top);
        break;
    case IconEdge::Constructor:
        path.lineTo(x0 + small, y1);
        path.lineTo(x0 + small, cy + half);
        path.lineTo(x0, cy);
        path.lineTo(x0 + small, cy - half);
        path.lineTo(x0 + small, top);
        break;
    default:
        path.lineTo(x0, y1);
        path.lineTo(x0, top);
        break;
    }
    path.closeSubPath();
    return path;
}

// The inner icon's elements, each in a cell, coloured for amplitude,
// frequency or time.
void drawSoundIconGlyphs(juce::Graphics& graphics, juce::Rectangle<float> area,
    const std::vector<talas::app::IconGlyph>& glyphs)
{
    using talas::app::IconElement;
    const auto cell = area.getHeight();
    auto x = area.getRight() - cell * static_cast<float>(glyphs.size());
    for (const auto& glyph : glyphs)
    {
        const auto r = juce::Rectangle<float>{x, area.getY(), cell, cell}.reduced(1.5F);
        x += cell;
        graphics.setColour(glyph.colour == talas::app::IconColour::Amplitude ? juce::Colour{0xff3fb8a8}
                : glyph.colour == talas::app::IconColour::Frequency          ? juce::Colour{0xffe07a9a}
                : glyph.colour == talas::app::IconColour::Time               ? juce::Colour{0xff9b7ee0}
                                                                          : secondaryTextColour);
        const auto stroke = juce::PathStrokeType{1.1F};
        const auto l = r.getX();
        const auto t = r.getY();
        const auto w = r.getWidth();
        const auto h = r.getHeight();
        const auto cx = r.getCentreX();
        const auto cy = r.getCentreY();
        const auto arrowHead = [&](float tipX, float tipY, bool hollow) {
            juce::Path head;
            head.addTriangle(tipX, tipY, tipX - 3.5F, tipY - 2.5F, tipX - 3.5F, tipY + 2.5F);
            if (hollow)
                graphics.strokePath(head, stroke);
            else
                graphics.fillPath(head);
        };
        const auto dashed = [&](float fromX, float toX, float y, float gap) {
            for (auto dashX = fromX; dashX < toX; dashX += 2.0F * gap)
                graphics.drawLine(dashX, y, std::min(dashX + gap, toX), y, 1.1F);
        };
        juce::Path path;
        switch (glyph.element)
        {
        case IconElement::File:
            path.startNewSubPath(l + 2, t);
            path.lineTo(l + w - 4, t);
            path.lineTo(l + w - 1, t + 3);
            path.lineTo(l + w - 1, t + h);
            path.lineTo(l + 2, t + h);
            path.closeSubPath();
            graphics.strokePath(path, stroke);
            break;
        case IconElement::Disk:
            graphics.drawEllipse(r, 1.1F);
            graphics.fillEllipse(r.withSizeKeepingCentre(2.5F, 2.5F));
            break;
        case IconElement::Ram:
            graphics.drawRect(r.withHeight(h * 0.55F).withY(t + h * 0.15F), 1.1F);
            for (auto tooth = l + 1.5F; tooth < l + w; tooth += 2.5F)
                graphics.drawLine(tooth, t + h * 0.7F, tooth, t + h * 0.9F, 1.0F);
            break;
        case IconElement::LiveInput:
            graphics.drawRoundedRectangle(r.withSizeKeepingCentre(w * 0.4F, h * 0.6F).withY(t), 2.0F, 1.1F);
            graphics.drawLine(cx, t + h * 0.6F, cx, t + h, 1.1F);
            graphics.drawLine(cx - 3, t + h, cx + 3, t + h, 1.1F);
            break;
        case IconElement::Audio:
        case IconElement::Oscillator:
        {
            const auto wave = glyph.element == IconElement::Oscillator ? r.reduced(2.5F) : r;
            if (glyph.element == IconElement::Oscillator)
                graphics.drawEllipse(r, 1.1F);
            path.startNewSubPath(wave.getX(), wave.getCentreY());
            for (auto step = 1; step <= 12; ++step)
            {
                const auto fraction = static_cast<float>(step) / 12.0F;
                path.lineTo(wave.getX() + fraction * wave.getWidth(),
                    wave.getCentreY() - std::sin(fraction * juce::MathConstants<float>::twoPi) * wave.getHeight() * 0.4F);
            }
            graphics.strokePath(path, stroke);
            break;
        }
        case IconElement::Spectrum:
            graphics.drawLine(l, t + h, l + w, t + h, 1.1F);
            for (auto bar = 0; bar < 4; ++bar)
            {
                const auto barX = l + 1.5F + static_cast<float>(bar) * w / 4.0F;
                graphics.drawLine(barX, t + h, barX, t + h * (0.15F + 0.2F * static_cast<float>(bar)), 1.1F);
            }
            break;
        case IconElement::Cloud:
            graphics.drawEllipse(l, t + h * 0.35F, w * 0.5F, h * 0.5F, 1.0F);
            graphics.drawEllipse(l + w * 0.25F, t + h * 0.15F, w * 0.5F, h * 0.55F, 1.0F);
            graphics.drawEllipse(l + w * 0.5F, t + h * 0.35F, w * 0.5F, h * 0.5F, 1.0F);
            break;
        case IconElement::Input:
            graphics.drawLine(l, cy, l + w - 3, cy, 1.1F);
            arrowHead(l + w, cy, false);
            break;
        case IconElement::Midi:
            graphics.drawEllipse(r, 1.1F);
            for (auto pin = 0; pin < 5; ++pin)
            {
                const auto angle = juce::MathConstants<float>::pi * (0.15F + 0.175F * static_cast<float>(pin));
                graphics.fillEllipse(cx - std::cos(angle) * w * 0.28F - 0.8F, cy + std::sin(angle) * h * 0.28F - 1.8F, 1.6F, 1.6F);
            }
            break;
        case IconElement::Output:
            dashed(l, l + w - 4, cy, 1.5F);
            arrowHead(l + w, cy, true);
            break;
        case IconElement::Process:
            dashed(l, l + w - 4, cy, 2.5F);
            arrowHead(l + w, cy, true);
            break;
        case IconElement::Arithmetic:
        case IconElement::Frequency:
        case IconElement::Time:
        case IconElement::Amplitude:
        case IconElement::Text:
        case IconElement::More:
            graphics.setFont(WorkbenchType::micro());
            graphics.drawText(glyph.element == IconElement::Arithmetic ? juce::String::fromUTF8("\u00b1")
                    : glyph.element == IconElement::Frequency           ? juce::String{"f"}
                    : glyph.element == IconElement::Time                ? juce::String{"t"}
                    : glyph.element == IconElement::Amplitude           ? juce::String{"a"}
                    : glyph.element == IconElement::Text                ? juce::String{"abc"}
                                                                        : juce::String::fromUTF8("\u2026"),
                r.expanded(2.0F, 0.0F), juce::Justification::centred);
            break;
        case IconElement::Pitch:
            for (auto line = 0; line < 5; ++line)
                graphics.drawLine(l, t + 1 + static_cast<float>(line) * (h - 2) / 4.0F, l + w,
                    t + 1 + static_cast<float>(line) * (h - 2) / 4.0F, 0.7F);
            graphics.fillEllipse(cx - 2.0F, cy - 1.5F, 4.0F, 3.0F);
            break;
        case IconElement::ControlPoint:
            graphics.fillRect(juce::Rectangle<float>{cx - 1.0F, t, 2.0F, h});
            graphics.drawRect(juce::Rectangle<float>{cx - 2.5F, cy - 2.5F, 5.0F, 5.0F}, 1.0F);
            break;
        case IconElement::Several:
            graphics.drawRect(r.withTrimmedLeft(w * 0.35F).withTrimmedBottom(h * 0.35F), 1.0F);
            graphics.drawRect(r.withTrimmedRight(w * 0.35F).withTrimmedTop(h * 0.35F), 1.0F);
            break;
        case IconElement::Blocking:
            graphics.fillRect(r.reduced(2.0F));
            break;
        case IconElement::Enabling:
            graphics.drawRect(r.reduced(2.0F), 1.1F);
            break;
        case IconElement::Scale:
            graphics.drawLine(l + 3, cy, l + w - 3, cy, 1.1F);
            arrowHead(l + w, cy, false);
            path.addTriangle(l, cy, l + 3.5F, cy - 2.5F, l + 3.5F, cy + 2.5F);
            graphics.fillPath(path);
            break;
        case IconElement::Cycle:
            path.addCentredArc(cx, cy, w * 0.4F, h * 0.4F, 0.0F, 0.4F, juce::MathConstants<float>::twoPi - 0.2F, true);
            graphics.strokePath(path, stroke);
            arrowHead(cx + 1.5F, t + h * 0.1F, false);
            break;
        case IconElement::SideEffect:
            graphics.drawLine(cx, t + h, cx, t + 3, 1.1F);
            path.addTriangle(cx, t, cx - 2.5F, t + 3.5F, cx + 2.5F, t + 3.5F);
            graphics.fillPath(path);
            break;
        case IconElement::Action:
        case IconElement::Control:
        case IconElement::Write:
            // The hand, on a knob or holding a pencil.
            graphics.drawRoundedRectangle(juce::Rectangle<float>{l, t + h * 0.45F, w * 0.55F, h * 0.55F}, 1.5F, 1.0F);
            for (auto finger = 0; finger < 3; ++finger)
                graphics.drawLine(l + 1.5F + static_cast<float>(finger) * 2.0F, t + h * 0.45F,
                    l + 1.5F + static_cast<float>(finger) * 2.0F, t + h * 0.2F, 1.0F);
            if (glyph.element == IconElement::Control)
                graphics.drawEllipse(juce::Rectangle<float>{l + w * 0.55F, t, w * 0.45F, h * 0.45F}, 1.0F);
            if (glyph.element == IconElement::Write)
                graphics.drawLine(l + w * 0.5F, t + h * 0.6F, l + w, t, 1.6F);
            break;
        case IconElement::Read:
            path.startNewSubPath(l, cy);
            path.quadraticTo(cx, t, l + w, cy);
            path.quadraticTo(cx, t + h, l, cy);
            graphics.strokePath(path, stroke);
            graphics.fillEllipse(r.withSizeKeepingCentre(3.0F, 3.0F));
            break;
        case IconElement::Formants:
            path.startNewSubPath(l, cy);
            path.quadraticTo(cx, t + h * 0.1F, l + w, cy);
            path.quadraticTo(cx, t + h * 0.9F, l, cy);
            graphics.strokePath(path, stroke);
            graphics.drawLine(l + 1, cy, l + w - 1, cy, 0.8F);
            break;
        case IconElement::Bandpass:
            // Three inputs: the outer two blocked, the middle one let
            // through to the output.
            for (auto row = 0; row < 3; ++row)
            {
                const auto y = t + 1.5F + static_cast<float>(row) * (h - 3) / 2.0F;
                graphics.drawLine(l, y, l + w * 0.45F, y, 1.0F);
                if (row == 1)
                {
                    graphics.drawRect(juce::Rectangle<float>{l + w * 0.45F, y - 1.5F, 3.0F, 3.0F}, 0.8F);
                    dashed(l + w * 0.45F + 3.0F, l + w - 3.0F, y, 1.2F);
                    arrowHead(l + w, y, true);
                }
                else
                    graphics.fillRect(juce::Rectangle<float>{l + w * 0.45F, y - 1.5F, 3.0F, 3.0F});
            }
            break;
        }
    }
}

bool drawSoundIconPicture(juce::Graphics& graphics, juce::Rectangle<float> area,
    const talas::project::ProjectSoundIcon& icon, bool large, juce::Colour colour)
{
    if ((large ? icon.large.empty() : icon.small.empty()) || area.isEmpty())
        return false;
    const auto width = static_cast<float>(talas::app::soundIconWidth(large));
    const auto height = static_cast<float>(talas::app::soundIconHeight(large));
    const auto dot = std::min(area.getWidth() / width, area.getHeight() / height);
    const auto picture = area.withSizeKeepingCentre(dot * width, dot * height);
    graphics.setColour(colour);
    for (std::size_t y = 0; y < talas::app::soundIconHeight(large); ++y)
    {
        for (std::size_t x = 0; x < talas::app::soundIconWidth(large); ++x)
        {
            if (talas::app::soundIconDot(icon, large, {x, y}))
                graphics.fillRect(picture.getX() + static_cast<float>(x) * dot, picture.getY() + static_cast<float>(y) * dot, dot, dot);
        }
    }
    return true;
}
}
