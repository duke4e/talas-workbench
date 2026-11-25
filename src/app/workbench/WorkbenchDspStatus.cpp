#include "app/workbench/WorkbenchDspStatus.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace talas::app::workbench
{
DspStatusPanel::DspStatusPanel(
    std::function<DspStatusReading()> read, std::function<void()> recycle, std::function<void(double)> setSampleRate)
    : read_(std::move(read))
    , setSampleRate_(std::move(setSampleRate))
    , recycleMemory_(std::move(recycle))
    , recycledSeconds_(juce::Time::getMillisecondCounterHiRes() / 1000.0)
{
    // The Sample Rate menu.
    sampleRate_.setTooltip("Sample Rate: the rates the audio device offers among the seven supported");
    sampleRate_.onChange = [this] {
        const auto index = sampleRate_.getSelectedItemIndex();
        if (setSampleRate_ && index >= 0 && static_cast<std::size_t>(index) < sampleRateChoices_.size()
            && std::abs(sampleRateChoices_[static_cast<std::size_t>(index)] - reading_.sampleRate) >= 0.5)
            setSampleRate_(sampleRateChoices_[static_cast<std::size_t>(index)]);
    };
    addAndMakeVisible(sampleRate_);
    recycle_.setTooltip("Recycle memory; it is also recycled as this window opens, and when memory use turns heavy");
    recycle_.onClick = [this] {
        if (recycleMemory_)
            recycleMemory_();
        recycledSeconds_ = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        refresh();
    };
    addAndMakeVisible(recycle_);
    setSize(360, 356);
    setTitle("DSP status");
    setDescription("Time since the Sound started, MIDI time code, input and output levels and peaks; click to reset the peaks.");
    startTimerHz(15);
}

DspStatusPanel::~DspStatusPanel()
{
    stopTimer();
}

void DspStatusPanel::refresh()
{
    if (read_)
        reading_ = read_();
    refreshSampleRates();
    for (const auto level : reading_.input)
        inputPeak_.observe(level);
    for (const auto level : reading_.output)
        outputPeak_.observe(level);
    repaint();
}

void DspStatusPanel::resized()
{
    recycle_.setBounds(getWidth() - 10 - 72, 10 + 24 + 4, 72, 22);
    sampleRate_.setBounds(getWidth() - 10 - 72 - 6 - 96, 10 + 24 + 4, 96, 22);
}

void DspStatusPanel::refreshSampleRates()
{
    const auto choices = talas::app::dspStatusSampleRateChoices(reading_.deviceSampleRates);
    if (choices != sampleRateChoices_)
    {
        sampleRateChoices_ = choices;
        sampleRate_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < choices.size(); ++index)
            sampleRate_.addItem(juce::String(choices[index], 0) + " Hz", static_cast<int>(index) + 1);
    }
    const auto current = std::ranges::find_if(sampleRateChoices_, [&](double rate) { return std::abs(rate - reading_.sampleRate) < 0.5; });
    sampleRate_.setSelectedItemIndex(current == sampleRateChoices_.end() ? -1 : static_cast<int>(current - sampleRateChoices_.begin()),
        juce::dontSendNotification);
    sampleRate_.setEnabled(!sampleRateChoices_.empty());
}

juce::StringArray DspStatusPanel::sampleRateChoicesForSmoke() const
{
    juce::StringArray items;
    for (const auto rate : sampleRateChoices_)
        items.add(juce::String(rate, 0));
    return items;
}

void DspStatusPanel::resetPeaks()
{
    inputPeak_.reset();
    outputPeak_.reset();
    repaint();
}

std::string DspStatusPanel::elapsedText() const
{
    return talas::app::dspStatusElapsedText(reading_.elapsedSeconds);
}

std::string DspStatusPanel::timeCodeText() const
{
    if (!reading_.timeCode.has_value())
        return "--:--:--.--";
    const auto& code = *reading_.timeCode;
    return talas::app::dspStatusTimeCodeText(code[0], code[1], code[2], code[3]);
}

std::string DspStatusPanel::peakText(bool output) const
{
    return output ? outputPeak_.text() : inputPeak_.text();
}

void DspStatusPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(workbenchTheme.surface);
    auto bounds = getLocalBounds().reduced(10);
    auto top = bounds.removeFromTop(24);
    graphics.setColour(workbenchTheme.primaryText);
    graphics.setFont(WorkbenchType::data());
    graphics.drawText(juceString(elapsedText()), top, juce::Justification::centredLeft);
    graphics.drawText(juceString(timeCodeText()), top, juce::Justification::centredRight);
    // The memory in use, a bar coloured by how heavy the use is,
    // beside the Recycle button.
    auto memory = bounds.removeFromTop(30).withTrimmedTop(4).withTrimmedRight(80).withHeight(22);
    graphics.setColour(workbenchTheme.controlTile);
    graphics.fillRect(memory);
    const auto fraction = std::clamp(reading_.memoryFraction, 0.0, 1.0);
    const auto colour = [] (talas::app::MemoryUseColour use) {
        switch (use)
        {
        case talas::app::MemoryUseColour::Cyan: return juce::Colours::cyan;
        case talas::app::MemoryUseColour::Green: return juce::Colours::limegreen;
        case talas::app::MemoryUseColour::Yellow: return juce::Colours::yellow;
        case talas::app::MemoryUseColour::Red: break;
        }
        return juce::Colours::red;
    }(talas::app::memoryUseColour(fraction));
    graphics.setColour(colour);
    graphics.fillRect(memory.withWidth(juce::roundToInt(static_cast<double>(memory.getWidth()) * fraction)));
    graphics.setColour(workbenchTheme.primaryText);
    graphics.setFont(WorkbenchType::micro());
    graphics.drawText("Memory " + juce::String(juce::roundToInt(fraction * 100.0)) + " %", memory.reduced(4, 0),
        juce::Justification::centredLeft);
    // The processor's usage, changing as Sounds start and
    // end.
    auto processor = bounds.removeFromTop(26).withTrimmedTop(4).withTrimmedRight(80);
    const auto load = std::clamp(reading_.processorLoad, 0.0, 1.0);
    graphics.setColour(workbenchTheme.controlTile);
    graphics.fillRect(processor);
    graphics.setColour(load < 0.8 ? juce::Colours::limegreen : juce::Colours::red);
    graphics.fillRect(processor.withWidth(juce::roundToInt(static_cast<double>(processor.getWidth()) * load)));
    graphics.setColour(workbenchTheme.primaryText);
    graphics.drawText(juceString(talas::app::dspStatusProcessorText(load)), processor.reduced(4, 0), juce::Justification::centredLeft);
    bounds.removeFromTop(8);
    // The Sound last loaded's global map, A/D input, MIDI channels
    // and Event Values, a line each at the bottom.
    auto inputs = bounds.removeFromBottom(static_cast<int>(reading_.soundInputs.size()) * 16);
    graphics.setColour(workbenchTheme.secondaryText);
    graphics.setFont(WorkbenchType::micro());
    for (const auto& line : reading_.soundInputs)
        graphics.drawFittedText(juceString(line), inputs.removeFromTop(16), juce::Justification::centredLeft, 1);
    bounds.removeFromBottom(6);
    const auto labels = bounds.removeFromBottom(18);
    // Two meters a side, or eight, a peak
    // indicator after each side's.
    const auto meters = static_cast<int>(meterCount());
    const auto barWidth = bounds.getWidth() / (2 * meters + 3);
    const auto marks = talas::app::dspStatusMeterMarksDb();
    const auto barAt = [&](int slot) {
        return juce::Rectangle<int>{bounds.getX() + slot * barWidth, bounds.getY(), barWidth - 6, bounds.getHeight()};
    };
    const auto drawBar = [&](int slot, float level, const juce::String& name) {
        const auto bar = barAt(slot);
        graphics.setColour(workbenchTheme.controlTile);
        graphics.fillRect(bar);
        const auto fraction = static_cast<float>(talas::app::dspStatusMeterFraction(level));
        graphics.setColour(level >= 1.0F ? workbenchTheme.error : workbenchTheme.audio);
        graphics.fillRect(bar.withTop(bar.getBottom() - juce::roundToInt(fraction * static_cast<float>(bar.getHeight()))));
        graphics.setColour(workbenchTheme.outline);
        for (const auto decibels : marks)
        {
            const auto y = bar.getBottom()
                - juce::roundToInt(static_cast<float>((decibels - talas::app::dspStatusMeterFloorDb) / -talas::app::dspStatusMeterFloorDb)
                    * static_cast<float>(bar.getHeight()));
            graphics.drawHorizontalLine(y, static_cast<float>(bar.getRight()), static_cast<float>(bar.getRight() + 4));
        }
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(name, juce::Rectangle<int>{bar.getX(), labels.getY(), bar.getWidth(), labels.getHeight()},
            juce::Justification::centred);
    };
    const auto drawPeak = [&](int slot, const talas::app::DspPeakIndicator& peak) {
        const auto box = barAt(slot).withSizeKeepingCentre(barWidth, 22);
        graphics.setColour(peak.clipped() ? workbenchTheme.error : workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(juceString(peak.text()), box, juce::Justification::centred);
    };
    if (meters == 2)
    {
        drawBar(0, reading_.input[0], "In L");
        drawPeak(1, inputPeak_);
        drawBar(2, reading_.input[1], "In R");
        drawBar(4, reading_.output[0], "Out L");
        drawPeak(5, outputPeak_);
        drawBar(6, reading_.output[1], "Out R");
        return;
    }
    for (int channel = 0; channel < meters; ++channel)
    {
        drawBar(channel, reading_.input[static_cast<std::size_t>(channel)], "In " + juce::String(channel + 1));
        drawBar(meters + 2 + channel, reading_.output[static_cast<std::size_t>(channel)], "Out " + juce::String(channel + 1));
    }
    drawPeak(meters, inputPeak_);
    drawPeak(2 * meters + 2, outputPeak_);
}

void DspStatusPanel::mouseDown(const juce::MouseEvent&)
{
    resetPeaks();
}

void DspStatusPanel::timerCallback()
{
    if (!isShowing())
        return;
    // Held still during disk operations unless the display
    // updates then.
    if (!read_().animate)
        return;
    refresh();
    // Open, the window recycles by itself under heavy use.
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (recycleMemory_ && talas::app::shouldRecycleAutomatically(reading_.memoryFraction, now - recycledSeconds_))
    {
        recycledSeconds_ = now;
        recycleMemory_();
    }
}
}
