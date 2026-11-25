#include "app/workbench/WorkbenchVcsDisplays.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace talas::app::workbench
{
// One display: the Sound's name over its trace, and the zoom buttons along
// its bottom.
class VcsDisplayStrip::Tile final : public juce::Component
{
public:
    explicit Tile(talas::app::VcsDisplay display)
        : display_(std::move(display))
    {
        describeControl(*this, juceString(display_.label), "The waveform of the OscilloscopeDisplay's Input");
        static constexpr std::array buttons{talas::app::VcsScopeButton::TimeOut, talas::app::VcsScopeButton::TimeIn,
            talas::app::VcsScopeButton::AmplitudeOut, talas::app::VcsScopeButton::AmplitudeIn};
        static constexpr std::array tips{"Zoom out in time", "Zoom in in time", "Zoom out in amplitude", "Zoom in in amplitude"};
        for (std::size_t index = 0; index < buttons.size(); ++index)
        {
            auto* button = buttons_.add(new juce::TextButton(juceString(talas::app::vcsScopeButtonText(buttons[index]))));
            button->setTooltip(tips[index]);
            button->onClick = [this, which = buttons[index]] {
                zoom_ = talas::app::vcsScopeZoomed(zoom_, which);
                repaint();
            };
            addAndMakeVisible(*button);
        }
    }

    [[nodiscard]] const talas::app::VcsDisplay& display() const noexcept { return display_; }

    // A capture of the Input's latest frames, from the stage it belongs to.
    void take(const talas::runtime::RealtimeAnalysisRecordHeader& header, std::span<const double> frames)
    {
        if (header.stageId != stageId_ || header.bindingGeneration != generation_)
        {
            history_.clear();
            stageId_ = header.stageId;
            generation_ = header.bindingGeneration;
        }
        history_.add(header.absoluteSamplePosition, frames);
        repaint();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(2);
        auto row = bounds.removeFromBottom(workbenchScaledUiPixels(18));
        const auto width = row.getWidth() / std::max(1, buttons_.size());
        for (auto* button : buttons_)
            button->setBounds(row.removeFromLeft(width).reduced(1, 0));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(workbenchTheme.controlTile);
        graphics.setColour(workbenchTheme.outline);
        graphics.drawRect(getLocalBounds(), 1);
        auto bounds = getLocalBounds().reduced(4);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::labelStrong());
        graphics.drawText(juceString(display_.label), bounds.removeFromTop(workbenchScaledUiPixels(16)),
            juce::Justification::centredLeft, true);
        bounds.removeFromBottom(workbenchScaledUiPixels(20));
        if (bounds.getHeight() < 4 || bounds.getWidth() < 4)
            return;
        graphics.setColour(workbenchTheme.canvas);
        graphics.fillRect(bounds);
        const auto centre = static_cast<float>(bounds.getCentreY());
        graphics.setColour(workbenchTheme.graphGrid.brighter(0.4f));
        graphics.drawHorizontalLine(static_cast<int>(centre), static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        const auto half = static_cast<float>(bounds.getHeight()) * 0.5f;
        const auto heightOf = [&](double value) {
            const auto y = centre - static_cast<float>(value / zoom_.amplitude) * half;
            return std::clamp(y, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
        };
        const auto frames = history_.latest(zoom_.frames);
        const auto trace = talas::app::vcsScopeColumns(frames, zoom_.frames, static_cast<std::size_t>(bounds.getWidth()));
        graphics.setColour(audioColour);
        for (std::size_t column = 0; column < trace.size(); ++column)
        {
            if (!trace[column].present)
                continue;
            const auto top = heightOf(trace[column].high);
            const auto bottom = heightOf(trace[column].low);
            graphics.fillRect(static_cast<float>(bounds.getX()) + static_cast<float>(column), top, 1.0f,
                std::max(1.0f, bottom - top));
        }
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(juce::String(static_cast<int>(zoom_.frames)) + " samp", bounds.reduced(2),
            juce::Justification::bottomRight, false);
    }

private:
    talas::app::VcsDisplay display_;
    talas::app::VcsScopeZoom zoom_;
    talas::app::VcsScopeHistory history_;
    std::uint64_t stageId_ = 0;
    std::uint64_t generation_ = 0;
    juce::OwnedArray<juce::TextButton> buttons_;
};

VcsDisplayStrip::VcsDisplayStrip(talas::runtime::ProjectRuntimeBridge& bridge)
    : bridge_(bridge)
{
    describeControl(*this, "Displays", "The oscilloscope displays of the project, each named for its Sound.");
}

VcsDisplayStrip::~VcsDisplayStrip()
{
    stopTimer();
}

int VcsDisplayStrip::preferredHeight()
{
    return workbenchScaledUiPixels(132);
}

void VcsDisplayStrip::setDisplays(std::vector<talas::app::VcsDisplay> displays)
{
    if (displays == displays_)
        return;
    displays_ = std::move(displays);
    tiles_.clear();
    for (const auto& display : displays_)
        addAndMakeVisible(tiles_.add(new Tile(display)));
    setVisible(!displays_.empty());
    resized();
}

void VcsDisplayStrip::resized()
{
    if (tiles_.isEmpty())
        return;
    auto bounds = getLocalBounds();
    const auto gap = workbenchScaledUiPixels(4);
    const auto width = std::min(workbenchScaledUiPixels(240),
        (bounds.getWidth() - gap * (tiles_.size() - 1)) / tiles_.size());
    for (auto* tile : tiles_)
    {
        tile->setBounds(bounds.removeFromLeft(width));
        bounds.removeFromLeft(gap);
    }
}

void VcsDisplayStrip::visibilityChanged()
{
    if (isVisible() && !displays_.empty())
        startTimerHz(30);
    else
        stopTimer();
}

void VcsDisplayStrip::timerCallback()
{
    if (!isShowing())
        return;
    const auto capture = bridge_.latestAnalysisCapture();
    if (capture == nullptr || !capture->ready())
        return;
    scratch_.resize(std::max(scratch_.size(), capture->maxPayloadValues()));
    for (auto* tile : tiles_)
    {
        const auto probe = capture->preferredProbeIndex(tile->display().nodeId);
        const auto* spec = probe.has_value() ? capture->probeSpec(*probe) : nullptr;
        if (spec == nullptr || spec->kind != talas::runtime::RealtimeAnalysisKind::Oscilloscope)
            continue;
        const auto copied = capture->copyLatestRecord(*probe, scratch_);
        if (copied.has_value())
            tile->take(*copied, std::span<const double>{scratch_.data(), copied->valueCount});
    }
}
}
