#include "app/workbench/WorkbenchParameterFieldEditor.h"

#include "app/workbench/WorkbenchTheme.h"

#include <algorithm>
#include <cmath>

namespace talas::app::workbench
{
ParameterFieldEditor::ParameterFieldEditor(CommitCallback onCommit)
    : onCommit_(std::move(onCommit))
{
    setWantsKeyboardFocus(true);
    setTitle("Parameter field editor");
    for (auto* button : {&loopStartButton_, &loopEndButton_, &zoomInButton_, &zoomOutButton_, &addValueButton_,
             &removeValueButton_})
        addChildComponent(*button);
    loopStartButton_.onClick = [this] { (void)toggleSelectedLoopStart(); };
    loopEndButton_.onClick = [this] { (void)toggleSelectedLoopEnd(); };
    zoomInButton_.onClick = [this] { zoom(true); };
    zoomOutButton_.onClick = [this] { zoom(false); };
    addValueButton_.onClick = [this] { (void)appendArrayValue(); };
    removeValueButton_.onClick = [this] { (void)removeLastArrayValue(); };
    addValueButton_.setTooltip("Add a value at the end of the array");
    removeValueButton_.setTooltip("Remove the array's last value");
}

void ParameterFieldEditor::showField(const std::string& text, bool envelope, double rate)
{
    if (rate != rate_)
    {
        rate_ = rate;
        repaint();
    }
    // The field the editor itself just wrote keeps its selection and zoom.
    if (mode_ != Mode::None && text == shownText_ && envelope == shownEnvelope_)
    {
        setVisible(true);
        updateButtons();
        return;
    }
    const auto previous = mode_;
    shownText_ = text;
    shownEnvelope_ = envelope;
    mode_ = Mode::None;
    selected_.reset();
    if (envelope)
    {
        if (const auto field = talas::app::parseEnvelopeField(text))
        {
            envelope_ = *field;
            mode_ = Mode::Envelope;
            if (previous != Mode::Envelope)
                visibleSeconds_ = std::max(1.0, std::ceil(envelope_.points.back().seconds));
        }
    }
    else if (const auto elements = talas::app::parseArrayField(text))
    {
        array_ = *elements;
        arrayRange_ = talas::app::arrayFieldRange(array_);
        mode_ = Mode::Array;
    }
    setVisible(mode_ != Mode::None);
    updateButtons();
    resized();
    repaint();
}

int ParameterFieldEditor::preferredHeight() const noexcept
{
    return mode_ == Mode::None ? 0 : 150;
}

void ParameterFieldEditor::updateButtons()
{
    const auto envelope = mode_ == Mode::Envelope;
    const auto array = mode_ == Mode::Array;
    loopStartButton_.setVisible(envelope);
    loopEndButton_.setVisible(envelope);
    zoomInButton_.setVisible(envelope);
    zoomOutButton_.setVisible(envelope);
    addValueButton_.setVisible(array);
    removeValueButton_.setVisible(array);
    loopStartButton_.setEnabled(selected_.has_value());
    loopEndButton_.setEnabled(selected_.has_value());
    removeValueButton_.setEnabled(array_.size() > 1);
}

void ParameterFieldEditor::commit()
{
    changed_ = false;
    if (!onCommit_)
        return;
    shownText_ = mode_ == Mode::Envelope ? talas::app::envelopeFieldText(envelope_) : talas::app::arrayFieldText(array_);
    if (mode_ != Mode::None)
        onCommit_(shownText_);
}

bool ParameterFieldEditor::addEnvelopePoint(double seconds, double value)
{
    if (mode_ != Mode::Envelope || envelope_.points.size() >= 4096)
        return false;
    selected_ = talas::app::addEnvelopeBreakpoint(envelope_, seconds, value);
    updateButtons();
    repaint();
    commit();
    return true;
}

bool ParameterFieldEditor::selectEnvelopePoint(std::size_t index)
{
    if (mode_ != Mode::Envelope || index >= envelope_.points.size())
        return false;
    selected_ = index;
    updateButtons();
    repaint();
    return true;
}

bool ParameterFieldEditor::moveSelectedEnvelopePoint(double seconds, double value)
{
    if (mode_ != Mode::Envelope || !selected_.has_value())
        return false;
    talas::app::moveEnvelopeBreakpoint(envelope_, *selected_, seconds, value);
    changed_ = true;
    repaint();
    if (!dragging_)
        commit();
    return true;
}

bool ParameterFieldEditor::removeSelectedEnvelopePoint()
{
    if (mode_ != Mode::Envelope || !selected_.has_value()
        || !talas::app::removeEnvelopeBreakpoint(envelope_, *selected_))
        return false;
    selected_.reset();
    updateButtons();
    repaint();
    commit();
    return true;
}

bool ParameterFieldEditor::toggleSelectedLoopStart()
{
    if (mode_ != Mode::Envelope || !selected_.has_value()
        || !talas::app::toggleEnvelopeLoopStart(envelope_, *selected_))
        return false;
    repaint();
    commit();
    return true;
}

bool ParameterFieldEditor::toggleSelectedLoopEnd()
{
    if (mode_ != Mode::Envelope || !selected_.has_value()
        || !talas::app::toggleEnvelopeLoopEnd(envelope_, *selected_))
        return false;
    repaint();
    commit();
    return true;
}

void ParameterFieldEditor::zoom(bool in)
{
    visibleSeconds_ = std::clamp(in ? visibleSeconds_ / 2.0 : visibleSeconds_ * 2.0, 0.125, 3600.0);
    repaint();
}

bool ParameterFieldEditor::setArrayValue(std::size_t index, double value)
{
    if (mode_ != Mode::Array || index >= array_.size() || !std::isfinite(value))
        return false;
    array_[index].value = value;
    changed_ = true;
    repaint();
    if (!dragging_)
        commit();
    return true;
}

bool ParameterFieldEditor::appendArrayValue()
{
    if (mode_ != Mode::Array || array_.empty() || array_.size() >= 1024)
        return false;
    array_.push_back(array_.back());
    updateButtons();
    repaint();
    commit();
    return true;
}

bool ParameterFieldEditor::removeLastArrayValue()
{
    if (mode_ != Mode::Array || array_.size() <= 1)
        return false;
    array_.pop_back();
    updateButtons();
    repaint();
    commit();
    return true;
}

juce::Rectangle<float> ParameterFieldEditor::plotArea() const
{
    return getLocalBounds().toFloat().reduced(4.0F).withTrimmedBottom(30.0F);
}

juce::Point<float> ParameterFieldEditor::envelopePointPosition(const talas::app::EnvelopeBreakpoint& point) const
{
    const auto area = plotArea();
    return {area.getX() + static_cast<float>(point.seconds / visibleSeconds_) * area.getWidth(),
        area.getBottom() - static_cast<float>(point.value) * area.getHeight()};
}

std::pair<double, double> ParameterFieldEditor::envelopeValueAt(juce::Point<float> position) const
{
    const auto area = plotArea();
    return {static_cast<double>((position.x - area.getX()) / std::max(1.0F, area.getWidth())) * visibleSeconds_,
        static_cast<double>((area.getBottom() - position.y) / std::max(1.0F, area.getHeight()))};
}

std::optional<std::size_t> ParameterFieldEditor::arrayIndexAt(juce::Point<float> position) const
{
    const auto area = plotArea();
    if (array_.empty() || !area.contains(position))
        return std::nullopt;
    const auto width = area.getWidth() / static_cast<float>(array_.size());
    return std::min(array_.size() - 1, static_cast<std::size_t>((position.x - area.getX()) / width));
}

double ParameterFieldEditor::arrayValueAt(juce::Point<float> position) const
{
    const auto area = plotArea();
    const auto proportion = std::clamp((area.getBottom() - position.y) / std::max(1.0F, area.getHeight()), 0.0F, 1.0F);
    const auto value = arrayRange_.minimum + static_cast<double>(proportion) * (arrayRange_.maximum - arrayRange_.minimum);
    // Whole numbers stay whole, as note numbers are.
    const auto whole = std::ranges::all_of(array_, [](const auto& element) {
        return element.value == std::floor(element.value);
    });
    return whole ? std::round(value) : std::round(value * 1000.0) / 1000.0;
}

void ParameterFieldEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(workbenchTheme.controlTile);
    const auto area = plotArea();
    graphics.setColour(workbenchTheme.outline);
    graphics.drawRect(area);
    if (mode_ == Mode::Envelope)
    {
        // Thin lines a tenth of a second apart, heavy ones each second,
        // labelled with the time they mark as played at the Rate.
        graphics.setFont(WorkbenchType::micro());
        for (const auto& line : talas::app::envelopeGridLines(visibleSeconds_, rate_))
        {
            const auto x = area.getX() + static_cast<float>(line.seconds / visibleSeconds_) * area.getWidth();
            graphics.setColour(line.heavy ? workbenchTheme.interactiveOutline : workbenchTheme.graphGrid);
            graphics.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
            if (line.heavy)
                graphics.drawText(juce::String{line.playedSeconds, 2}.trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " s",
                    juce::Rectangle<float>{x + 2.0F, area.getY() + 1.0F, 40.0F, 12.0F}, juce::Justification::centredLeft);
        }
        juce::Path line;
        for (std::size_t index = 0; index < envelope_.points.size(); ++index)
        {
            const auto at = envelopePointPosition(envelope_.points[index]);
            if (index == 0)
                line.startNewSubPath(at);
            else
                line.lineTo(at);
        }
        graphics.setColour(controlColour);
        graphics.strokePath(line, juce::PathStrokeType{1.5F});
        for (std::size_t index = 0; index < envelope_.points.size(); ++index)
        {
            const auto box = juce::Rectangle<float>{8.0F, 8.0F}.withCentre(envelopePointPosition(envelope_.points[index]));
            graphics.setColour(textColour);
            if (selected_ == index)
                graphics.drawRect(box, 1.5F);
            else
                graphics.fillRect(box);
            if (envelope_.loopStart == index || envelope_.loopEnd == index)
            {
                // Two little red arrows mark the loop.
                graphics.setColour(juce::Colour{talas::app::envelopeLoopArrowColour()});
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(envelope_.loopStart == index ? juce::String::fromUTF8("▶") : juce::String::fromUTF8("◀"),
                    box.translated(0.0F, -12.0F).expanded(4.0F), juce::Justification::centred);
            }
        }
        if (selected_.has_value())
        {
            const auto& point = envelope_.points[*selected_];
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::micro());
            graphics.drawText(juce::String(point.seconds, 3) + " s, " + juce::String(point.value, 3),
                area.reduced(4.0F).removeFromTop(14.0F), juce::Justification::topRight);
        }
    }
    else if (mode_ == Mode::Array && !array_.empty())
    {
        const auto width = area.getWidth() / static_cast<float>(array_.size());
        const auto span = std::max(1e-12, arrayRange_.maximum - arrayRange_.minimum);
        const auto zero = area.getBottom()
            - static_cast<float>((0.0 - arrayRange_.minimum) / span) * area.getHeight();
        for (std::size_t index = 0; index < array_.size(); ++index)
        {
            const auto top = area.getBottom()
                - static_cast<float>((array_[index].value - arrayRange_.minimum) / span) * area.getHeight();
            const auto bar = juce::Rectangle<float>::leftTopRightBottom(area.getX() + width * static_cast<float>(index) + 1.0F,
                std::min(top, zero), area.getX() + width * static_cast<float>(index + 1) - 1.0F, std::max(top, zero));
            graphics.setColour(dataColour.withAlpha(0.8F));
            graphics.fillRect(bar);
        }
    }
}

void ParameterFieldEditor::resized()
{
    auto row = getLocalBounds().reduced(4).removeFromBottom(26);
    for (auto* button : {&loopStartButton_, &loopEndButton_, &zoomInButton_, &zoomOutButton_})
        button->setBounds(row.removeFromLeft(78).reduced(1, 0));
    row = getLocalBounds().reduced(4).removeFromBottom(26);
    addValueButton_.setBounds(row.removeFromLeft(30).reduced(1, 0));
    removeValueButton_.setBounds(row.removeFromLeft(30).reduced(1, 0));
}

void ParameterFieldEditor::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    const auto position = event.position;
    if (mode_ == Mode::Envelope)
    {
        const auto [seconds, value] = envelopeValueAt(position);
        if (event.mods.isShiftDown())
        {
            (void)addEnvelopePoint(seconds, value);
            return;
        }
        const auto area = plotArea();
        selected_ = talas::app::nearestEnvelopeBreakpoint(envelope_, seconds, value,
            6.0 / std::max(1.0, static_cast<double>(area.getWidth())) * visibleSeconds_,
            6.0 / std::max(1.0, static_cast<double>(area.getHeight())));
        dragging_ = selected_.has_value();
        updateButtons();
        repaint();
        return;
    }
    if (mode_ == Mode::Array)
    {
        if (const auto index = arrayIndexAt(position))
        {
            dragging_ = true;
            (void)setArrayValue(*index, arrayValueAt(position));
        }
    }
}

void ParameterFieldEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (!dragging_)
        return;
    if (mode_ == Mode::Envelope)
    {
        const auto [seconds, value] = envelopeValueAt(event.position);
        (void)moveSelectedEnvelopePoint(seconds, value);
    }
    else if (mode_ == Mode::Array)
    {
        if (const auto index = arrayIndexAt(event.position))
            (void)setArrayValue(*index, arrayValueAt(event.position));
    }
}

void ParameterFieldEditor::mouseUp(const juce::MouseEvent&)
{
    dragging_ = false;
    if (changed_)
        commit();
}

bool ParameterFieldEditor::keyPressed(const juce::KeyPress& key)
{
    if (mode_ == Mode::Envelope
        && (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey))
        return removeSelectedEnvelopePoint();
    return false;
}
}
