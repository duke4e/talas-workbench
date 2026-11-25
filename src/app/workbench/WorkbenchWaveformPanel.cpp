#include "app/workbench/WorkbenchWaveformPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
class WaveformCanvas final : public juce::Component
{
public:
    explicit WaveformCanvas(WaveformPanelCallbacks& callbacks)
        : callbacks_(callbacks)
    {
        setWantsKeyboardFocus(true);
        describeControl(
            *this,
            "Selected sample waveform",
            "Click to set the source-frame cursor. Arrow keys move it by one "
            "frame; Shift plus arrows move the sustain-loop end marker.");
    }

    void setState(const WaveformPanelState& state)
    {
        state_ = state;
        setDescription(
            state_.overview != nullptr
                ? juce::String(static_cast<juce::int64>(
                      state_.overview->frameCount))
                    + " source frames. Cursor at frame "
                    + juce::String(static_cast<juce::int64>(
                        state_.cursorFrame))
                : juce::String{
                      state_.loading
                          ? "Waveform overview is loading."
                          : "No waveform overview is available."});
        repaint();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        if (code != juce::KeyPress::leftKey
            && code != juce::KeyPress::rightKey)
        {
            return false;
        }
        const auto delta = code == juce::KeyPress::leftKey ? -1 : 1;
        if (key.getModifiers().isShiftDown())
        {
            if (callbacks_.moveLoopMarker)
            {
                callbacks_.moveLoopMarker(
                    false,
                    talas::app::WaveformLoopMarker::EndExclusive,
                    delta);
            }
        }
        else if (callbacks_.setCursor && state_.overview != nullptr)
        {
            const auto next = static_cast<double>(state_.cursorFrame)
                + static_cast<double>(delta);
            callbacks_.setCursor(talas::app::clampWaveformFrame(
                next,
                state_.overview->frameCount));
        }
        return true;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        if (state_.overview == nullptr || !callbacks_.setCursor)
            return;
        dragMarker_.reset();
        dragStartFrame_ = talas::app::waveformPixelToFrame(
            event.position.x,
            state_.viewport,
            getWidth());
        if (state_.region.has_value())
        {
            const auto detect = [&](
                                    bool release,
                                    const std::optional<talas::project::ProjectSampleLoop>& loop) {
                if (!loop.has_value())
                    return false;
                const auto projected = talas::app::projectWaveformLoop(
                    *loop,
                    state_.viewport,
                    getWidth());
                if (std::abs(event.position.x
                             - static_cast<float>(projected.startPixel))
                    <= 6.0F)
                {
                    dragMarker_ = std::pair{
                        release,
                        talas::app::WaveformLoopMarker::Start,
                    };
                    return true;
                }
                if (std::abs(event.position.x
                             - static_cast<float>(projected.endPixel))
                    <= 6.0F)
                {
                    dragMarker_ = std::pair{
                        release,
                        talas::app::WaveformLoopMarker::EndExclusive,
                    };
                    return true;
                }
                return false;
            };
            if (detect(false, state_.region->sustainLoop)
                || detect(true, state_.region->releaseLoop))
            {
                return;
            }
        }
        callbacks_.setCursor(talas::app::waveformPixelToFrame(
            event.position.x,
            state_.viewport,
            getWidth()));
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!dragMarker_.has_value()
            || state_.overview == nullptr
            || !callbacks_.moveLoopMarker)
        {
            return;
        }
        const auto endFrame = talas::app::waveformPixelToFrame(
            event.position.x,
            state_.viewport,
            getWidth());
        const auto delta = static_cast<std::int64_t>(endFrame)
            - static_cast<std::int64_t>(dragStartFrame_);
        if (delta != 0)
        {
            callbacks_.moveLoopMarker(
                dragMarker_->first,
                dragMarker_->second,
                delta);
        }
        dragMarker_.reset();
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintWell(graphics, getLocalBounds().toFloat(), 6.0F);
        if (state_.overview != nullptr)
            WorkbenchStyle::paintDisplayGrid(graphics, getLocalBounds().toFloat().reduced(1.0F), 16, 4);
        if (state_.overview == nullptr)
        {
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::labelStrong());
            graphics.drawFittedText(
                state_.loading ? "Building waveform overview..."
                               : (state_.errorText.empty()
                                      ? "Select one sample region"
                                      : juceString(state_.errorText)),
                getLocalBounds().reduced(20),
                juce::Justification::centred,
                2);
            return;
        }

        const auto& overview = *state_.overview;
        const auto centreY = static_cast<float>(getHeight()) * 0.5F;
        const auto amplitude = std::max(1.0F, centreY - 7.0F);
        // The envelope as one filled shape, lit aqua to violet, its edge glowing.
        std::vector<juce::Point<float>> tops;
        std::vector<juce::Point<float>> bottoms;
        for (const auto& bucket : overview.buckets)
        {
            if (bucket.channels.empty())
                continue;
            const auto bucketCentre =
                (static_cast<double>(bucket.firstFrame)
                 + static_cast<double>(bucket.endFrameExclusive))
                * 0.5;
            if (bucketCentre < state_.viewport.startFrame
                || bucketCentre >= state_.viewport.endFrameExclusive)
            {
                continue;
            }
            auto minimum = 1.0;
            auto maximum = -1.0;
            for (const auto& channel : bucket.channels)
            {
                minimum = std::min(minimum, channel.minimum);
                maximum = std::max(maximum, channel.maximum);
            }
            const auto x = static_cast<float>(
                talas::app::waveformFrameToPixel(
                    bucketCentre,
                    state_.viewport,
                    getWidth()));
            const auto top = centreY
                - static_cast<float>(maximum) * amplitude;
            const auto bottom = centreY
                - static_cast<float>(minimum) * amplitude;
            tops.push_back({x, std::min(top, bottom)});
            bottoms.push_back({x, std::max(top, bottom)});
        }
        if (!tops.empty())
        {
            juce::Path envelope;
            juce::Path edge;
            envelope.startNewSubPath(tops.front());
            edge.startNewSubPath(tops.front());
            for (std::size_t index = 1; index < tops.size(); ++index)
            {
                envelope.lineTo(tops[index]);
                edge.lineTo(tops[index]);
            }
            for (auto index = bottoms.size(); index-- > 0;)
                envelope.lineTo(bottoms[index]);
            envelope.closeSubPath();
            edge.startNewSubPath(bottoms.front());
            for (std::size_t index = 1; index < bottoms.size(); ++index)
                edge.lineTo(bottoms[index]);
            juce::ColourGradient fill{workbenchTheme.accentStart.withAlpha(0.55F), 0.0F, centreY - amplitude,
                workbenchTheme.accentStart.withAlpha(0.55F), 0.0F, centreY + amplitude, false};
            fill.addColour(0.5, workbenchTheme.audio.withAlpha(0.8F));
            graphics.setGradientFill(fill);
            graphics.fillPath(envelope);
            WorkbenchStyle::strokeWithGlow(graphics, edge, workbenchTheme.audio.brighter(0.2F), 1.0F, 4.0F);
            graphics.setColour(juce::Colours::white.withAlpha(0.12F));
            graphics.drawHorizontalLine(juce::roundToInt(centreY), 0.0F, static_cast<float>(getWidth()));
        }

        if (state_.region.has_value())
        {
            drawLoop(graphics, state_.region->sustainLoop, workbenchTheme.control);
            drawLoop(graphics, state_.region->releaseLoop, workbenchTheme.event);
        }
        const auto cursorX = static_cast<float>(
            talas::app::waveformFrameToPixel(
                static_cast<double>(state_.cursorFrame),
                state_.viewport,
                getWidth()));
        graphics.setColour(workbenchTheme.focus);
        graphics.drawVerticalLine(
            static_cast<int>(std::lround(cursorX)),
            0.0F,
            static_cast<float>(getHeight()));
    }

private:
    void drawLoop(
        juce::Graphics& graphics,
        const std::optional<talas::project::ProjectSampleLoop>& loop,
        juce::Colour colour)
    {
        if (!loop.has_value())
            return;
        const auto projection = talas::app::projectWaveformLoop(
            *loop,
            state_.viewport,
            getWidth());
        const auto start = static_cast<float>(projection.startPixel);
        const auto end = static_cast<float>(projection.endPixel);
        graphics.setColour(colour.withAlpha(0.14F));
        graphics.fillRect(
            juce::Rectangle<float>{
                start,
                1.0F,
                std::max(1.0F, end - start),
                static_cast<float>(getHeight() - 2)});
        graphics.setColour(colour);
        graphics.drawVerticalLine(
            static_cast<int>(std::lround(start)),
            0.0F,
            static_cast<float>(getHeight()));
        graphics.drawVerticalLine(
            static_cast<int>(std::lround(end)),
            0.0F,
            static_cast<float>(getHeight()));
        if (projection.hasReturnLeg)
        {
            graphics.drawArrow(
                juce::Line<float>{end - 3.0F, 8.0F, start + 3.0F, 8.0F},
                1.0F,
                6.0F,
                5.0F);
        }
    }

    WaveformPanelCallbacks& callbacks_;
    WaveformPanelState state_;
    std::optional<std::pair<bool, talas::app::WaveformLoopMarker>>
        dragMarker_;
    std::uint64_t dragStartFrame_ = 0;
};

class WaveformPanelImpl final : public WaveformPanel
{
public:
    explicit WaveformPanelImpl(WaveformPanelCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , zoomInButton_("Zoom In")
        , zoomOutButton_("Zoom Out")
        , auditionButton_("Audition")
        , canvas_(callbacks_)
    {
        describeControl(
            *this,
            "Waveform and loop editor",
            "Inspect immutable sample bytes and edit exact sustain and release loop frames.");
        title_.setText("Waveform", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setFont(WorkbenchType::panelTitle());
        readout_.setColour(juce::Label::textColourId, secondaryTextColour);
        readout_.setFont(WorkbenchType::data());
        readout_.setMinimumHorizontalScale(1.0F);
        addAndMakeVisible(title_);
        addAndMakeVisible(readout_);
        addAndMakeVisible(zoomInButton_);
        addAndMakeVisible(zoomOutButton_);
        addAndMakeVisible(auditionButton_);
        addAndMakeVisible(canvas_);
        describeControl(
            zoomInButton_,
            "Zoom waveform in",
            "Narrow the visible source-frame range around the cursor.");
        describeControl(
            zoomOutButton_,
            "Zoom waveform out",
            "Widen the visible source-frame range around the cursor.");
        describeControl(
            auditionButton_,
            "Audition selected sample map",
            "Prepare or stop an isolated sample-map graph without changing the project document.");
        zoomInButton_.onClick = [this] {
            if (callbacks_.zoom)
                callbacks_.zoom(2.0);
        };
        zoomOutButton_.onClick = [this] {
            if (callbacks_.zoom)
                callbacks_.zoom(0.5);
        };
        auditionButton_.onClick = [this] {
            if (state_.audition == SampleMapAuditionPresentation::Active)
            {
                if (callbacks_.stopAudition)
                    callbacks_.stopAudition();
            }
            else if (callbacks_.audition)
            {
                callbacks_.audition();
            }
        };
    }

    void setState(const WaveformPanelState& state) override
    {
        state_ = state;
        canvas_.setState(state_);
        auto readout = state_.overview != nullptr
            ? juce::String(static_cast<juce::int64>(state_.cursorFrame))
                + " / "
                + juce::String(static_cast<juce::int64>(state_.overview->frameCount))
                + juceString(" frames · ")
                + juce::String(static_cast<int>(state_.overview->channelCount))
                + juceString(" ch · ")
                + juce::String(static_cast<int>(state_.overview->sampleRate))
                + " Hz"
            : juceString(
                  state_.loading ? "Loading immutable generation..."
                                 : state_.errorText);
        readout_.setText(readout, juce::dontSendNotification);
        readout_.setTitle(readout);
        const auto canEdit = state_.overview != nullptr
            && state_.region.has_value();
        zoomInButton_.setEnabled(canEdit);
        zoomOutButton_.setEnabled(canEdit);
        auditionButton_.setEnabled(
            canEdit
            && state_.audition != SampleMapAuditionPresentation::Preparing
            && state_.audition != SampleMapAuditionPresentation::Stopping);
        auditionButton_.setButtonText(
            state_.audition == SampleMapAuditionPresentation::Active
                ? "Stop Audition"
                : (state_.audition == SampleMapAuditionPresentation::Preparing
                       ? "Preparing..."
                       : (state_.audition
                                  == SampleMapAuditionPresentation::Stopping
                              ? "Stopping..."
                              : "Audition")));
        repaint();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    [[nodiscard]] bool hasReadySurfaceForSmoke() const override
    {
        return !getBounds().isEmpty()
            && !canvas_.getBounds().isEmpty()
            && state_.overview != nullptr
            && state_.region.has_value()
            && zoomInButton_.isEnabled();
    }

    [[nodiscard]] bool hasHonestLoadingStateForSmoke() const override
    {
        return state_.loading
            && state_.overview == nullptr
            && readout_.getText().containsIgnoreCase("loading");
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto header = bounds.removeFromTop(28);
        title_.setBounds(header.removeFromLeft(82));
        zoomInButton_.setBounds(header.removeFromRight(72));
        header.removeFromRight(6);
        zoomOutButton_.setBounds(header.removeFromRight(78));
        header.removeFromRight(6);
        auditionButton_.setBounds(header.removeFromRight(104));
        header.removeFromRight(8);
        readout_.setBounds(header);
        bounds.removeFromTop(6);
        canvas_.setBounds(bounds);
    }

private:
    WaveformPanelCallbacks callbacks_;
    WaveformPanelState state_;
    juce::Label title_;
    juce::Label readout_;
    juce::TextButton zoomInButton_;
    juce::TextButton zoomOutButton_;
    juce::TextButton auditionButton_;
    WaveformCanvas canvas_;
};
}

std::unique_ptr<WaveformPanel> makeWaveformPanel(
    WaveformPanelCallbacks callbacks)
{
    return std::make_unique<WaveformPanelImpl>(std::move(callbacks));
}
}
