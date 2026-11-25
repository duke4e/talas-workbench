#include "app/workbench/WorkbenchAnalysisTools.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/PartialTrackSpectrumEditing.h"
#include "talas/app/SpectralAnalysisSteps.h"
#include "talas/app/VirtualControlWidgetModel.h"
#include "talas/audio/PartialTrackAudioAnalysis.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/audio/QuasiHarmonicAnalysis.h"
#include "talas/audio/WavReader.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

// Spectral Analysis, step by step: select the
// sample, heard looped with a silence after it; choose the analysis
// frequency, the filter shape and the level, and audition the
// resynthesis against the original; create the spectrum file; then, for a
// harmonic spectrum, raise the unpitched fader until only the consonants
// are left, trace the fundamental, and create the quasi-harmonic spectrum
// file at the pitch given.
namespace talas::app::workbench
{
namespace
{
using talas::audio::FundamentalPoint;
using Step = talas::app::SpectralAnalysisStep;
using Audition = talas::app::SpectralAnalysisAudition;

[[nodiscard]] juce::String pathText(const std::filesystem::path& path)
{
    return juceString(talas::project::pathToUtf8(path));
}

[[nodiscard]] juce::String noteText(double hz)
{
    return juce::String(talas::audio::noteNumberOfFrequency(hz), 2) + " nn (" + juce::String(hz, 1) + " Hz)";
}

// The spectrum with time from left to right and frequency from the
// bottom to the top, amplitude as colour, the brightest the loudest, and
// the trace's white points over it. Click a point to select it and drag
// it; Backspace or Delete deletes it; Shift+Click adds one; Ctrl or Command
// and a box zooms in, Return or Enter zooms out; L draws
// the tracks as frames connected by lines or not.
// Shift held as the drag begins fine-tunes the
// fader, the value moving a tenth as far as the pointer from where it
// began, as the surface's faders do.
class FineTuningSlider final : public juce::Slider
{
public:
    void mouseDown(const juce::MouseEvent& event) override
    {
        fineTuning_ = event.mods.isShiftDown();
        startValue_ = getValue();
        if (!fineTuning_)
            juce::Slider::mouseDown(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!fineTuning_)
        {
            juce::Slider::mouseDrag(event);
            return;
        }
        const auto length = static_cast<double>(std::max(1, getWidth()));
        setValue(talas::app::fineTunedFaderValue(getMinimum(), getMaximum(), startValue_,
                     static_cast<double>(event.getDistanceFromDragStartX()) / length),
            juce::sendNotificationAsync);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!fineTuning_)
            juce::Slider::mouseUp(event);
        fineTuning_ = false;
    }

private:
    bool fineTuning_ = false;
    double startValue_ = 0.0;
};

class TraceView final : public juce::Component
{
public:
    std::function<void(std::vector<FundamentalPoint>)> onTraceChanged;

    void setSpectrum(const talas::project::PartialTrackAsset* spectrum, double highestHz)
    {
        spectrum_ = spectrum;
        fullHighHz_ = highestHz;
        zoomOut();
    }

    void setTrace(std::vector<FundamentalPoint> trace)
    {
        trace_ = std::move(trace);
        if (selected_ && *selected_ >= trace_.size())
            selected_.reset();
        repaint();
    }

    void zoomOut()
    {
        lowSeconds_ = 0.0;
        highSeconds_ = spectrum_ != nullptr && spectrum_->sampleRate > 0.0
            ? std::max(0.001, static_cast<double>(spectrum_->sourceFrameCount) / spectrum_->sampleRate)
            : 1.0;
        lowHz_ = 0.0;
        highHz_ = fullHighHz_ > 0.0 ? fullHighHz_ : 2000.0;
        image_ = {};
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colours::black);
        if (spectrum_ != nullptr)
        {
            if (!image_.isValid() || image_.getWidth() != getWidth() || image_.getHeight() != getHeight())
                renderSpectrum();
            graphics.drawImageAt(image_, 0, 0);
        }
        // The white line; clicked, the display selected, its points
        // show as squares.
        const auto handles = talas::app::spectrumTraceHandles(hasKeyboardFocus(true)) == talas::app::SpectrumTraceHandles::Squares;
        graphics.setColour(juce::Colours::white);
        for (std::size_t index = 0; index < trace_.size(); ++index)
        {
            const auto at = position(trace_[index]);
            if (index > 0)
                graphics.drawLine(juce::Line<float>{position(trace_[index - 1]), at}, 1.0F);
            const auto half = selected_ == index ? 4.5F : 3.0F;
            if (handles)
                graphics.fillRect(at.x - half, at.y - half, half * 2.0F, half * 2.0F);
        }
        if (zoomBox_)
        {
            graphics.setColour(juce::Colours::yellow);
            graphics.drawRect(*zoomBox_, 1);
        }
    }

    void resized() override { image_ = {}; }
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

    // Control or Command held, the cursor is a magnifying
    // glass for drawing the zoom box.
    void modifierKeysChanged(const juce::ModifierKeys& modifiers) override
    {
        setMouseCursor(talas::app::spectrumViewCursor(modifiers.isCtrlDown() || modifiers.isCommandDown())
                    == talas::app::SpectrumViewCursor::Magnifier
                ? workbenchMagnifierMouseCursor()
                : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        dragStart_ = event.getPosition();
        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            zoomBox_ = juce::Rectangle<int>{dragStart_, dragStart_};
            return;
        }
        if (event.mods.isShiftDown())
        {
            if (auto added = talas::audio::traceWithPoint(trace_, point(event.position)))
            {
                const auto seconds = point(event.position).seconds;
                change(std::move(*added));
                selected_ = indexAt(seconds);
            }
            return;
        }
        selected_.reset();
        for (std::size_t index = 0; index < trace_.size(); ++index)
        {
            if (position(trace_[index]).getDistanceFrom(event.position) <= 6.0F)
                selected_ = index;
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (zoomBox_)
        {
            zoomBox_ = juce::Rectangle<int>::leftTopRightBottom(std::min(dragStart_.x, event.x),
                std::min(dragStart_.y, event.y), std::max(dragStart_.x, event.x), std::max(dragStart_.y, event.y));
            repaint();
            return;
        }
        if (!selected_)
            return;
        const auto moved = point(event.position);
        if (auto edited = talas::audio::traceWithPointMoved(trace_, *selected_, moved))
        {
            change(std::move(*edited));
            selected_ = indexAt(moved.seconds);
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!zoomBox_)
            return;
        const auto box = *zoomBox_;
        zoomBox_.reset();
        const auto width = static_cast<double>(std::max(1, getWidth()));
        const auto height = static_cast<double>(std::max(1, getHeight()));
        if (const auto window = box.getWidth() > 4 && box.getHeight() > 4
                ? talas::app::spectrumViewZoomedTo({lowSeconds_, highSeconds_, lowHz_, highHz_}, box.getX() / width,
                      box.getY() / height, box.getRight() / width, box.getBottom() / height)
                : std::nullopt)
        {
            lowSeconds_ = window->lowTime;
            highSeconds_ = window->highTime;
            lowHz_ = window->lowHz;
            highHz_ = window->highHz;
            image_ = {};
        }
        repaint();
    }

    // Return, lowercase L and Delete.
    bool keyPressed(const juce::KeyPress& key) override
    {
        using talas::app::TraceViewKey;
        const auto pressed = key == juce::KeyPress::returnKey ? TraceViewKey::Return
            : !key.getModifiers().isAnyModifierKeyDown() && (key.getTextCharacter() == 'l' || key.getKeyCode() == 'l' || key.getKeyCode() == 'L')
            ? TraceViewKey::LowercaseL
            : key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey ? TraceViewKey::Delete
                                                                                     : TraceViewKey::Other;
        switch (talas::app::traceViewAction(pressed, selected_.has_value()))
        {
        case talas::app::TraceViewAction::ShowWhole:
            zoomOut();
            return true;
        case talas::app::TraceViewAction::ToggleConnectedFrames:
            connected_ = !connected_;
            image_ = {};
            repaint();
            return true;
        case talas::app::TraceViewAction::DeletePoint:
            if (auto edited = talas::audio::traceWithoutPoint(trace_, *selected_))
            {
                selected_.reset();
                change(std::move(*edited));
            }
            return true;
        case talas::app::TraceViewAction::None:
            break;
        }
        return false;
    }

private:
    [[nodiscard]] juce::Point<float> position(const FundamentalPoint& at) const
    {
        const auto width = static_cast<double>(std::max(1, getWidth()));
        const auto height = static_cast<double>(std::max(1, getHeight()));
        const auto x = (at.seconds - lowSeconds_) / std::max(1.0e-9, highSeconds_ - lowSeconds_) * width;
        const auto y = height - (at.hz - lowHz_) / std::max(1.0e-9, highHz_ - lowHz_) * height;
        return {static_cast<float>(x), static_cast<float>(y)};
    }

    [[nodiscard]] FundamentalPoint point(juce::Point<float> at) const
    {
        const auto width = static_cast<double>(std::max(1, getWidth()));
        const auto height = static_cast<double>(std::max(1, getHeight()));
        return {std::max(0.0, lowSeconds_ + at.x / width * (highSeconds_ - lowSeconds_)),
            std::max(1.0, lowHz_ + (height - at.y) / height * (highHz_ - lowHz_))};
    }

    [[nodiscard]] std::optional<std::size_t> indexAt(double seconds) const
    {
        for (std::size_t index = 0; index < trace_.size(); ++index)
        {
            if (trace_[index].seconds == seconds)
                return index;
        }
        return std::nullopt;
    }

    void change(std::vector<FundamentalPoint> trace)
    {
        trace_ = trace;
        repaint();
        if (onTraceChanged)
            onTraceChanged(std::move(trace));
    }

    [[nodiscard]] static const talas::project::PartialTrackPoint* trackPoint(
        const talas::project::PartialTrackFrame& frame, std::uint64_t trackId)
    {
        const auto found = std::ranges::lower_bound(frame.points, trackId, {}, &talas::project::PartialTrackPoint::trackId);
        return found != frame.points.end() && found->trackId == trackId ? &*found : nullptr;
    }

    void renderSpectrum()
    {
        image_ = juce::Image{juce::Image::RGB, std::max(1, getWidth()), std::max(1, getHeight()), true};
        juce::Graphics graphics{image_};
        double loudest = 0.0;
        for (const auto& frame : spectrum_->frames)
        {
            for (const auto& point : frame.points)
                loudest = std::max(loudest, point.amplitude);
        }
        if (!(loudest > 0.0))
            return;
        const auto& frames = spectrum_->frames;
        const auto colourOf = [loudest](double amplitude) {
            // -60 dB to the loudest: dark blue through green to yellow, as
            // the spectrum editor draws it.
            return juce::Colour{talas::app::partialTrackAmplitudeColour(amplitude / loudest, false)};
        };
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            const auto seconds = static_cast<double>(frames[index].sourceFrame) / spectrum_->sampleRate;
            const auto next = index + 1 < frames.size()
                ? static_cast<double>(frames[index + 1].sourceFrame) / spectrum_->sampleRate
                : seconds + 0.01;
            if (next < lowSeconds_ || seconds > highSeconds_)
                continue;
            const auto left = position({seconds, lowHz_}).x;
            const auto right = std::max(left + 1.0F, position({next, lowHz_}).x);
            for (const auto& point : frames[index].points)
            {
                if (point.frequencyHz < lowHz_ || point.frequencyHz > highHz_ || !(point.amplitude > 0.0))
                    continue;
                graphics.setColour(colourOf(point.amplitude));
                const auto y = position({seconds, point.frequencyHz}).y;
                const auto* following = connected_ && index + 1 < frames.size()
                    ? trackPoint(frames[index + 1], point.trackId)
                    : nullptr;
                if (following != nullptr)
                    graphics.drawLine(left, y, right, position({next, following->frequencyHz}).y, 1.5F);
                else
                    graphics.fillRect(juce::Rectangle<float>{left, y - 1.0F, right - left, 2.0F});
            }
        }
    }

    const talas::project::PartialTrackAsset* spectrum_ = nullptr;
    bool connected_ = false;
    std::vector<FundamentalPoint> trace_;
    std::optional<std::size_t> selected_;
    std::optional<juce::Rectangle<int>> zoomBox_;
    juce::Point<int> dragStart_;
    double fullHighHz_ = 0.0;
    double lowSeconds_ = 0.0;
    double highSeconds_ = 1.0;
    double lowHz_ = 0.0;
    double highHz_ = 2000.0;
    juce::Image image_;
};

class SpectralAnalysisWizard final
    : public AnalysisToolPanel
    , private juce::Timer
{
public:
    SpectralAnalysisWizard(StatusCallback onStatus, CompletedCallback onCompleted, AuditionCallback onAudition)
        : onStatus_(std::move(onStatus)), onCompleted_(std::move(onCompleted)), onAudition_(std::move(onAudition))
    {
        setOpaque(true);
        heading_.setFont(WorkbenchType::panelTitle());
        addAndMakeVisible(heading_);
        progress_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(progress_);
        back_.setButtonText("Back");
        back_.onClick = [this] { (void) back(); };
        next_.setButtonText("Next");
        next_.onClick = [this] { (void) next(); };
        cancel_.setButtonText("Cancel");
        cancel_.onClick = [this] { cancelRequested_.store(true); };
        stop_.setButtonText("Stop");
        stop_.setTooltip("Stop the audition");
        stop_.onClick = [this] { stopAudition(); };
        for (auto* button : {&back_, &next_, &stop_})
            addAndMakeVisible(*button);
        addChildComponent(cancel_);

        // Select.
        select_.setButtonText("Select...");
        select_.onClick = [this] { chooseSample(); };
        sampleLabel_.setText("No sample selected", juce::dontSendNotification);
        silenceLabel_.setText("Silence after it (s)", juce::dontSendNotification);
        silence_.setText("1", juce::dontSendNotification);
        silence_.setInputRestrictions(8, "0123456789.");
        silence_.onReturnKey = [this] { play(); };
        silence_.onFocusLost = [this] { play(); };
        addPage(Step::Select, {&select_, &sampleLabel_, &silenceLabel_, &silence_});

        // Parameters.
        lowestLabel_.setText("Analysis frequency", juce::dontSendNotification);
        for (int setting = 1; setting <= talas::app::spectralAnalysisLowestFrequencySettings; ++setting)
            lowest_.addItem(juceString(talas::app::spectralAnalysisLowestFrequencyLabel(setting)), setting);
        lowest_.setSelectedId(1, juce::dontSendNotification);
        lowest_.onChange = [this] { invalidateAnalysis(); };
        responseLabel_.setText("Filter", juce::dontSendNotification);
        const auto responses = talas::app::spectralAnalysisResponseLabels();
        for (std::size_t index = 0; index < responses.size(); ++index)
            response_.addItem(juceString(responses[index]), static_cast<int>(index) + 1);
        response_.setSelectedId(1, juce::dontSendNotification);
        response_.onChange = [this] { invalidateAnalysis(); };
        describeControl(lowest_, "Analysis frequency",
            "The lowest filter's fundamental: low enough for the sample's fundamental, not so low it takes the noise below");
        describeControl(response_, "Filter", "Better frequency resolution (BestFreq) or better timing (BestTime)");
        audition_.setButtonText("Audition");
        audition_.onClick = [this] { (void) analyze(true); };
        resynthesis_.setButtonText("Resynthesis");
        original_.setButtonText("Original Sample");
        resynthesis_.setRadioGroupId(1);
        original_.setRadioGroupId(1);
        resynthesis_.onClick = [this] { choose(Audition::Resynthesis); };
        original_.onClick = [this] { choose(Audition::OriginalSample); };
        level_.setRange(0.05, 2.0, 0.01);
        level_.setValue(1.0, juce::dontSendNotification);
        level_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 22);
        level_.onDragEnd = [this] { play(); };
        describeControl(level_, "Level", "The resynthesis's level, to keep it from clipping");
        oscillators_.setColour(juce::Label::textColourId, secondaryTextColour);
        addPage(Step::Parameters, {&lowestLabel_, &lowest_, &responseLabel_, &response_, &audition_, &resynthesis_,
            &original_, &level_, &oscillators_});

        // Save.
        createSpectrum_.setButtonText("Create Spectrum File...");
        createSpectrum_.onClick = [this] { chooseSpectrumFile(); };
        quasiHarmonic_.setButtonText("Set Quasi-Harmonic Parameters");
        quasiHarmonic_.onClick = [this] { (void) next(); };
        saveHint_.setText("A spectrum file serves time and pitch changes of any sound and morphs of noisy ones; "
                          "go on for a harmonic spectrum to morph pitched sounds.",
            juce::dontSendNotification);
        saveHint_.setColour(juce::Label::textColourId, secondaryTextColour);
        addPage(Step::Save, {&createSpectrum_, &quasiHarmonic_, &saveHint_});

        // Unpitched.
        unpitchedLabel_.setText("Unpitched", juce::dontSendNotification);
        unpitched_.setRange(0.0, 1.0, 0.001);
        unpitched_.setValue(0.0, juce::dontSendNotification);
        unpitched_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 22);
        // Shift fine-tunes it, as FineTuningSlider does.
        unpitched_.onValueChange = [this] { play(); };
        describeControl(unpitched_, "Unpitched",
            "Raise it until the vowels are gone and only consonants, breaths and clicks are left; Shift fine-tunes");
        unpitchedHint_.setText("Raise the fader until the singable vowels are gone, leaving the consonants, breaths "
                               "and mouth clicks; hold Shift to fine-tune.",
            juce::dontSendNotification);
        unpitchedHint_.setColour(juce::Label::textColourId, secondaryTextColour);
        addPage(Step::Unpitched, {&unpitchedLabel_, &unpitched_, &unpitchedHint_});

        // Trace.
        traceOriginal_.setButtonText("Original Sample");
        fundamentalOnly_.setButtonText("Fundamental Only");
        allHarmonics_.setButtonText("All Harmonics");
        for (auto* radio : {&traceOriginal_, &fundamentalOnly_, &allHarmonics_})
            radio->setRadioGroupId(2);
        traceOriginal_.onClick = [this] { choose(Audition::OriginalSample); };
        fundamentalOnly_.onClick = [this] { choose(Audition::FundamentalOnly); };
        allHarmonics_.onClick = [this] { choose(Audition::AllHarmonics); };
        autoLower_.setButtonText("Auto trace lower");
        autoLower_.onClick = [this] { (void) traceAction(TraceAction::AutoTraceLower); };
        autoHigher_.setButtonText("Auto trace higher");
        autoHigher_.onClick = [this] { (void) traceAction(TraceAction::AutoTraceHigher); };
        handTrace_.setButtonText("Hand trace");
        handTrace_.onClick = [this] { (void) traceAction(TraceAction::HandTrace); };
        harmonicLabel_.setText("Traced Harmonic", juce::dontSendNotification);
        harmonic_.setText("1", juce::dontSendNotification);
        harmonic_.setInputRestrictions(3, "0123456789");
        harmonic_.onReturnKey = [this] { (void) setTracedHarmonic(harmonic_.getText().getIntValue()); };
        traceLevelLabel_.setText("Level", juce::dontSendNotification);
        traceLevel_.setRange(0.05, 2.0, 0.01);
        traceLevel_.setValue(1.0, juce::dontSendNotification);
        traceLevel_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 22);
        describeControl(traceLevel_, "Level", "The analysis's amplitude, lowered if the resynthesis clips");
        traceLevel_.onDragEnd = [this] {
            level_.setValue(traceLevel_.getValue(), juce::dontSendNotification);
            harmonicsStale_ = true;
            play();
        };
        level_.onValueChange = [this] { traceLevel_.setValue(level_.getValue(), juce::dontSendNotification); };
        zoomOut_.setButtonText("Zoom out");
        zoomOut_.onClick = [this] { traceView_.zoomOut(); };
        traceView_.onTraceChanged = [this](std::vector<FundamentalPoint> trace) {
            trace_ = std::move(trace);
            harmonicsStale_ = true;
            play();
        };
        traceView_.setWantsKeyboardFocus(true);
        addPage(Step::Trace, {&traceView_, &traceOriginal_, &fundamentalOnly_, &allHarmonics_, &autoLower_,
            &autoHigher_, &handTrace_, &harmonicLabel_, &harmonic_, &traceLevelLabel_, &traceLevel_, &zoomOut_});

        // Harmonic.
        createHarmonic_.setButtonText("Create Quasi-Harmonic Spectrum File...");
        createHarmonic_.onClick = [this] { askPitch(); };
        harmonicHint_.setColour(juce::Label::textColourId, secondaryTextColour);
        // The buttons back to each step along the way.
        newSample_.setButtonText("New sample");
        newSample_.onClick = [this] { (void) goBackTo(Step::Select); };
        adjustRange_.setButtonText("Adjust range & response");
        adjustRange_.onClick = [this] { (void) goBackTo(Step::Parameters); };
        adjustUnpitched_.setButtonText("Adjust pitched/unpitched");
        adjustUnpitched_.onClick = [this] { (void) goBackTo(Step::Unpitched); };
        adjustFundamental_.setButtonText("Adjust fundamental");
        adjustFundamental_.onClick = [this] { (void) goBackTo(Step::Trace); };
        addPage(Step::Harmonic, {&createHarmonic_, &harmonicHint_, &newSample_, &adjustRange_, &adjustUnpitched_,
            &adjustFundamental_});

        showStep();
        setSize(640, 420);
        startTimerHz(10);
    }

    ~SpectralAnalysisWizard() override
    {
        stopTimer();
        cancelAnalysis();
        stopAudition();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        heading_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(8);
        auto footer = bounds.removeFromBottom(28);
        next_.setBounds(footer.removeFromRight(80));
        footer.removeFromRight(6);
        back_.setBounds(footer.removeFromRight(80));
        footer.removeFromRight(6);
        cancel_.setBounds(footer.removeFromRight(80));
        stop_.setBounds(footer.removeFromLeft(64));
        footer.removeFromLeft(8);
        progress_.setBounds(footer);
        bounds.removeFromBottom(8);
        const auto row = [&bounds](int height = 28) {
            auto line = bounds.removeFromTop(height);
            bounds.removeFromTop(6);
            return line;
        };
        {
            auto line = row();
            select_.setBounds(line.removeFromLeft(110));
            line.removeFromLeft(8);
            sampleLabel_.setBounds(line);
            line = row();
            silenceLabel_.setBounds(line.removeFromLeft(150));
            silence_.setBounds(line.removeFromLeft(70));
        }
        bounds = getLocalBounds().reduced(12).withTrimmedTop(32).withTrimmedBottom(36);
        {
            auto line = row();
            lowestLabel_.setBounds(line.removeFromLeft(140));
            lowest_.setBounds(line.removeFromLeft(220));
            line = row();
            responseLabel_.setBounds(line.removeFromLeft(140));
            response_.setBounds(line.removeFromLeft(220));
            line = row();
            audition_.setBounds(line.removeFromLeft(110));
            line = row();
            resynthesis_.setBounds(line.removeFromLeft(130));
            level_.setBounds(line.removeFromLeft(260));
            line = row(20);
            oscillators_.setBounds(line.withTrimmedLeft(20));
            line = row();
            original_.setBounds(line.removeFromLeft(150));
        }
        bounds = getLocalBounds().reduced(12).withTrimmedTop(32).withTrimmedBottom(36);
        {
            auto line = row();
            createSpectrum_.setBounds(line.removeFromLeft(200));
            line = row(44);
            saveHint_.setBounds(line);
            line = row();
            quasiHarmonic_.setBounds(line.removeFromLeft(240));
        }
        bounds = getLocalBounds().reduced(12).withTrimmedTop(32).withTrimmedBottom(36);
        {
            auto line = row();
            unpitchedLabel_.setBounds(line.removeFromLeft(100));
            unpitched_.setBounds(line);
            line = row(44);
            unpitchedHint_.setBounds(line);
        }
        bounds = getLocalBounds().reduced(12).withTrimmedTop(32).withTrimmedBottom(36);
        {
            auto controls = bounds.removeFromLeft(150);
            bounds.removeFromLeft(8);
            for (auto* control : std::initializer_list<juce::Component*>{&traceOriginal_, &fundamentalOnly_,
                     &allHarmonics_, &autoLower_, &autoHigher_, &handTrace_, &harmonicLabel_, &harmonic_,
                     &traceLevelLabel_, &traceLevel_, &zoomOut_})
            {
                control->setBounds(controls.removeFromTop(26));
                controls.removeFromTop(4);
            }
            traceView_.setBounds(bounds);
        }
        bounds = getLocalBounds().reduced(12).withTrimmedTop(32).withTrimmedBottom(36);
        {
            auto line = row();
            createHarmonic_.setBounds(line.removeFromLeft(280));
            line = row(60);
            harmonicHint_.setBounds(line);
            for (auto* button : {&newSample_, &adjustRange_, &adjustUnpitched_, &adjustFundamental_})
                button->setBounds(row().removeFromLeft(220));
        }
    }

    void cancelAnalysis() override
    {
        cancelRequested_.store(true);
        if (worker_.joinable())
            worker_.join();
        running_ = false;
    }

    [[nodiscard]] bool setSampleForSmoke(const std::filesystem::path& sample) override
    {
        return loadSample(sample);
    }

    [[nodiscard]] bool analyzeNowForSmoke(const std::filesystem::path& output, bool) override
    {
        if (!analyze(false))
            return false;
        finishNow();
        return writeSpectrum(output);
    }

    void setSpectralSettingsForSmoke(int lowestSetting, int response, bool) override
    {
        lowest_.setSelectedId(lowestSetting, juce::dontSendNotification);
        response_.setSelectedId(response + 1, juce::dontSendNotification);
        invalidateAnalysis();
    }

    [[nodiscard]] Step spectralStepForSmoke() const override { return steps_.step(); }
    bool nextForSmoke() override
    {
        const auto moved = next();
        finishNow();
        return moved;
    }
    bool backForSmoke() override { return back(); }
    [[nodiscard]] bool auditionNowForSmoke() override
    {
        if (!analyze(true))
            return false;
        finishNow();
        return analysis_.has_value();
    }
    [[nodiscard]] std::size_t oscillatorsForSmoke() const override { return oscillatorCount_; }
    void setUnpitchedForSmoke(double fader) override { unpitched_.setValue(fader, juce::sendNotificationSync); }
    [[nodiscard]] std::vector<FundamentalPoint> traceForSmoke() const override { return trace_; }
    bool traceActionForSmoke(TraceAction action) override { return traceAction(action); }
    bool setTracedHarmonicForSmoke(int harmonic) override { return setTracedHarmonic(harmonic); }
    bool addTracePointForSmoke(FundamentalPoint point) override
    {
        auto added = talas::audio::traceWithPoint(trace_, point);
        if (!added)
            return false;
        setTrace(std::move(*added));
        return true;
    }
    bool deleteTracePointForSmoke(std::size_t index) override
    {
        auto deleted = talas::audio::traceWithoutPoint(trace_, index);
        if (!deleted)
            return false;
        setTrace(std::move(*deleted));
        return true;
    }
    bool setTraceAuditionForSmoke(Audition audition) override
    {
        if (!steps_.chooseAudition(audition))
            return false;
        showStep();
        play();
        return true;
    }
    [[nodiscard]] bool createSpectrumNowForSmoke(
        const std::filesystem::path& output, talas::app::StereoAnalysisChoice choice) override
    {
        if (!createSpectrum(output, choice))
            return false;
        finishNow();
        return true;
    }
    [[nodiscard]] bool createHarmonicNowForSmoke(const std::filesystem::path& output, double noteNumber) override
    {
        return writeHarmonic(output, 440.0 * std::pow(2.0, (noteNumber - 69.0) / 12.0));
    }

private:
    void addPage(Step step, std::initializer_list<juce::Component*> components)
    {
        for (auto* component : components)
        {
            addChildComponent(*component);
            pages_.push_back({step, component});
        }
    }

    void showStep()
    {
        const auto step = steps_.step();
        for (const auto& [owner, component] : pages_)
            component->setVisible(owner == step);
        heading_.setText("Spectral Analysis · " + juceString(talas::app::SpectralAnalysisSteps::title(step)),
            juce::dontSendNotification);
        back_.setEnabled(step != Step::Select);
        next_.setEnabled(steps_.canGoOn());
        const auto audition = steps_.audition();
        resynthesis_.setToggleState(audition == Audition::Resynthesis, juce::dontSendNotification);
        original_.setToggleState(audition == Audition::OriginalSample, juce::dontSendNotification);
        traceOriginal_.setToggleState(audition == Audition::OriginalSample, juce::dontSendNotification);
        fundamentalOnly_.setToggleState(audition == Audition::FundamentalOnly, juce::dontSendNotification);
        allHarmonics_.setToggleState(audition == Audition::AllHarmonics, juce::dontSendNotification);
        if (step == Step::Harmonic)
            describeHarmonic();
    }

    // On to the next step, the straight analysis run first where the step
    // needs it, and the step's audition played.
    bool next()
    {
        if (running_)
            return false;
        if (!steps_.canGoOn())
        {
            if (steps_.step() == Step::Select)
                onStatus_("Select a sample to analyze first");
            return false;
        }
        const auto from = steps_.step();
        if ((from == Step::Parameters || from == Step::Save) && !analysis_)
            return analyze(false, [this] { (void) next(); });
        (void) steps_.next();
        showStep();
        if (steps_.step() == Step::Trace)
        {
            traceView_.setSpectrum(analysis_ ? &*analysis_ : nullptr, highestHz() * std::max(1, tracedHarmonic_) * 2.0);
            retrace(talas::audio::FundamentalTraceBias::Guess);
        }
        play();
        return true;
    }

    bool back()
    {
        if (running_ || !steps_.back())
            return false;
        showStep();
        play();
        return true;
    }

    // The last page's buttons back to an earlier step.
    bool goBackTo(Step step)
    {
        if (running_ || !steps_.goBackTo(step))
            return false;
        showStep();
        play();
        return true;
    }

    void choose(Audition audition)
    {
        if (steps_.chooseAudition(audition))
            play();
        showStep();
    }

    void chooseSample()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Select a Sample to Analyze",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.aifc");
        juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void) safeThis->loadSample(talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
            });
    }

    bool loadSample(const std::filesystem::path& sample)
    {
        if (running_)
        {
            onStatus_("Wait for the running analysis or cancel it first");
            return false;
        }
        auto read = talas::audio::readSampleFile(sample);
        if (!read.ok() || read.channels.empty() || read.channels.front().empty())
        {
            const auto message = "The sample could not be read: "
                + juceString(read.ok() ? std::string{"it has no frames"} : read.error);
            sampleLabel_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return false;
        }
        sample_ = sample;
        sampleRate_ = static_cast<double>(read.sampleRate);
        channels_ = read.channels;
        // The steps before saving analyze a stereo
        // sample's left channel; the file saved may take the mix.
        mono_ = talas::app::spectralAnalysisAuditionedChannel(read.channels);
        sampleLabel_.setText(pathText(sample.filename()) + " · " + juce::String(durationSeconds(), 2) + " s, "
                + juce::String(read.sampleRate) + " Hz",
            juce::dontSendNotification);
        invalidateAnalysis();
        steps_.sampleSelected();
        showStep();
        play();
        return true;
    }

    [[nodiscard]] double durationSeconds() const
    {
        return sampleRate_ > 0.0 ? static_cast<double>(mono_.size()) / sampleRate_ : 0.0;
    }

    [[nodiscard]] double silenceSeconds() const
    {
        return std::clamp(silence_.getText().getDoubleValue(), 0.0, 60.0);
    }

    [[nodiscard]] double lowestHz() const
    {
        return talas::app::spectralAnalysisLowestFrequencyHz(lowest_.getSelectedId());
    }

    [[nodiscard]] double highestHz() const { return lowestHz() * 4.0; }

    [[nodiscard]] talas::audio::PartialTrackAudioAnalysisSettings analysisSettings() const
    {
        talas::audio::PartialTrackAudioAnalysisSettings settings;
        settings.lowestFrequencyHz = lowestHz();
        settings.responseBins = talas::app::spectralAnalysisResponseBins(response_.getSelectedId() - 1);
        settings.kind = talas::audio::PartialTrackAnalysisKind::Straight;
        return settings;
    }

    void invalidateAnalysis()
    {
        traceView_.setSpectrum(nullptr, 0.0);
        traceView_.setTrace({});
        analysis_.reset();
        resynthesisSamples_.clear();
        pitches_.clear();
        trace_.clear();
        harmonicsStale_ = true;
        oscillatorCount_ = 0;
        oscillators_.setText({}, juce::dontSendNotification);
    }

    // The straight analysis, run on a worker; Audition plays its
    // resynthesis when it is done.
    bool analyze(bool thenAudition, std::function<void()> then = {})
    {
        if (running_ || mono_.empty())
        {
            onStatus_(running_ ? "An analysis is already running" : "Select a sample to analyze first");
            return false;
        }
        if (analysis_)
        {
            if (thenAudition)
                choose(Audition::Resynthesis);
            if (then)
                then();
            return true;
        }
        const auto settings = analysisSettings();
        return startJob("Analyzing...",
            [this, settings, samples = mono_, rate = sampleRate_] {
                auto result = talas::audio::analyzeMonoAudioPartialTracks(
                    samples, rate, settings, {}, [this] { return cancelRequested_.load(); });
                if (!result.ok())
                {
                    jobError_ = result.error ? juceString(result.error->message) : juce::String{"The analysis failed"};
                    return;
                }
                auto rendered = talas::audio::renderPartialTrackAssetOffline(result.asset);
                jobAnalysis_ = std::move(result.asset);
                jobSamples_ = rendered.ok() ? std::move(rendered.samples) : std::vector<double>{};
            },
            [this, thenAudition, then = std::move(then), lowest = settings.lowestFrequencyHz] {
                analysis_ = std::move(jobAnalysis_);
                resynthesisSamples_ = std::move(jobSamples_);
                pitches_ = talas::audio::framePitches(*analysis_, lowest);
                oscillatorCount_ = talas::app::resynthesisOscillatorCount(*analysis_);
                // The number of oscillators the resynthesis uses.
                const auto warning = talas::app::resynthesisCapacityWarning(*analysis_);
                oscillators_.setText(juce::String(static_cast<int>(oscillatorCount_)) + " oscillators"
                        + (warning.has_value() ? " (" + juceString(*warning) + ")" : juce::String{}),
                    juce::dontSendNotification);
                if (thenAudition)
                    choose(Audition::Resynthesis);
                if (then)
                    then();
            });
    }

    bool startJob(const juce::String& message, std::function<void()> work, std::function<void()> then)
    {
        if (worker_.joinable())
            worker_.join();
        jobError_ = {};
        cancelRequested_.store(false);
        done_.store(false);
        running_ = true;
        then_ = std::move(then);
        cancel_.setVisible(true);
        progress_.setText(message, juce::dontSendNotification);
        try
        {
            worker_ = std::thread([this, work = std::move(work)] {
                work();
                done_.store(true, std::memory_order_release);
            });
        }
        catch (const std::system_error&)
        {
            running_ = false;
            cancel_.setVisible(false);
            onStatus_("The analysis could not start");
            return false;
        }
        return true;
    }

    void finishNow()
    {
        if (!running_)
            return;
        if (worker_.joinable())
            worker_.join();
        timerCallback();
    }

    void timerCallback() override
    {
        if (isShowing())
            wasShowing_ = true;
        else if (wasShowing_)
        {
            // The window closed: the audition stops with it.
            wasShowing_ = false;
            stopAudition();
        }
        if (!running_ || !done_.load(std::memory_order_acquire))
            return;
        if (worker_.joinable())
            worker_.join();
        running_ = false;
        cancel_.setVisible(false);
        auto then = std::move(then_);
        then_ = {};
        if (cancelRequested_.load() || jobError_.isNotEmpty())
        {
            const auto message = cancelRequested_.load() ? juce::String{"Analysis cancelled"} : jobError_;
            progress_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return;
        }
        progress_.setText({}, juce::dontSendNotification);
        if (then)
            then();
    }

    void audition(std::vector<double> samples)
    {
        if (onAudition_ && !samples.empty() && sampleRate_ > 0.0)
            onAudition_(talas::audio::sampleWithSilence(samples, sampleRate_, silenceSeconds()), sampleRate_);
    }

    void stopAudition()
    {
        if (onAudition_)
            onAudition_({}, sampleRate_);
    }

    [[nodiscard]] std::vector<double> leveled(std::vector<double> samples) const
    {
        const auto level = level_.getValue();
        for (auto& sample : samples)
            sample *= level;
        return samples;
    }

    // What the step plays: the sample looped with the silence
    // after it, the resynthesis at its level, what the unpitched fader
    // leaves, a sine at the traced fundamental, or all its harmonics.
    void play()
    {
        switch (steps_.audition())
        {
        case Audition::OriginalSample:
            audition(mono_);
            return;
        case Audition::Resynthesis:
            audition(resynthesisSamples_.empty() ? mono_ : leveled(resynthesisSamples_));
            return;
        case Audition::UnpitchedOnly:
            if (analysis_)
                audition(talas::audio::unpitchedOnly(mono_, *analysis_, pitches_, unpitched_.getValue()));
            return;
        case Audition::FundamentalOnly:
            audition(talas::audio::fundamentalSine(trace_, tracedHarmonic_, sampleRate_, mono_.size()));
            return;
        case Audition::AllHarmonics:
            if (!analysis_ || trace_.empty())
                return;
            if (harmonicsStale_)
            {
                auto rendered = talas::audio::renderPartialTrackAssetOffline(harmonicAsset());
                harmonicSamples_ = rendered.ok() ? std::move(rendered.samples) : std::vector<double>{};
                harmonicsStale_ = false;
            }
            audition(harmonicSamples_);
            return;
        }
    }

    void setTrace(std::vector<FundamentalPoint> trace)
    {
        trace_ = std::move(trace);
        harmonicsStale_ = true;
        traceView_.setTrace(trace_);
        play();
    }

    void retrace(talas::audio::FundamentalTraceBias bias)
    {
        if (!analysis_)
            return;
        setTrace(talas::audio::traceFundamental(
            *analysis_, pitches_, unpitched_.getValue(), lowestHz(), highestHz(), bias, tracedHarmonic_));
        if (trace_.empty())
            onStatus_("No frame is pitched: go back and raise the unpitched fader, or Hand trace the fundamental");
    }

    bool traceAction(TraceAction action)
    {
        if (!analysis_)
            return false;
        switch (action)
        {
        case TraceAction::AutoTraceLower:
            retrace(talas::audio::FundamentalTraceBias::Lower);
            break;
        case TraceAction::AutoTraceHigher:
            retrace(talas::audio::FundamentalTraceBias::Higher);
            break;
        case TraceAction::HandTrace:
            // Add the points with Shift+Click.
            setTrace({});
            onStatus_("Hand trace: Shift+Click to add the fundamental's points");
            break;
        }
        return true;
    }

    // Change Traced Harmonic and press Enter.
    bool setTracedHarmonic(int harmonic)
    {
        if (harmonic < 1 || harmonic > 64 || !analysis_)
        {
            harmonic_.setText(juce::String(tracedHarmonic_), juce::dontSendNotification);
            return false;
        }
        tracedHarmonic_ = harmonic;
        harmonic_.setText(juce::String(harmonic), juce::dontSendNotification);
        retrace(talas::audio::FundamentalTraceBias::Guess);
        return true;
    }

    [[nodiscard]] talas::project::PartialTrackAsset harmonicAsset() const
    {
        auto harmonic = talas::audio::harmonicSpectrum(*analysis_, trace_, tracedHarmonic_);
        const auto level = level_.getValue();
        for (auto& frame : harmonic.frames)
        {
            for (auto& point : frame.points)
                point.amplitude *= level;
        }
        return harmonic;
    }

    void describeHarmonic()
    {
        const auto summary = talas::audio::fundamentalSummary(trace_, tracedHarmonic_);
        // The starting, ending and average pitches as a hint.
        harmonicHint_.setText(summary
                ? "The fundamental starts at " + noteText(summary->startHz) + ", ends at " + noteText(summary->endHz)
                    + " and averages " + noteText(summary->averageHz)
                    + ". Give a tone its pitch; speech and sounds of several pitches take middle C, 60 nn."
                : juce::String{"There is no trace: go back and trace the fundamental first."},
            juce::dontSendNotification);
    }

    // The spectrum file, and for a stereo sample a mono mix of its channels,
    // one of them, or the left and right into separate files.
    void chooseSpectrumFile()
    {
        if (mono_.empty())
        {
            onStatus_("Select a sample to analyze first");
            return;
        }
        if (channels_.size() != 2)
        {
            chooseSpectrumDestination(talas::app::StereoAnalysisChoice::MonoMix);
            return;
        }
        choiceWindow_ = std::make_unique<juce::AlertWindow>("Stereo sample",
            "Analyze a mono mix of the two channels, one of them, or both into separate files?",
            juce::MessageBoxIconType::QuestionIcon, this);
        juce::StringArray labels;
        for (const auto& label : talas::app::stereoAnalysisChoiceLabels())
            labels.add(juceString(label));
        choiceWindow_->addComboBox("channels", labels);
        choiceWindow_->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        choiceWindow_->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
        choiceWindow_->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result) {
            if (safeThis == nullptr || result != 1)
                return;
            const auto index = safeThis->choiceWindow_->getComboBoxComponent("channels")->getSelectedItemIndex();
            safeThis->chooseSpectrumDestination(static_cast<talas::app::StereoAnalysisChoice>(std::clamp(index, 0, 3)));
        }));
    }

    void chooseSpectrumDestination(talas::app::StereoAnalysisChoice choice)
    {
        const auto initial = talas::app::analysisOutputPath(sample_, talas::app::spectralAnalysisFileSuffix(lowest_.getSelectedId()));
        // A .spc spectrum file or a .talaspartial one.
        chooser_ = std::make_unique<juce::FileChooser>("Save the Spectrum File", juce::File{pathText(initial)},
            juce::String{std::string{talas::project::spectrumFilePatterns}});
        juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, choice](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void) safeThis->createSpectrum(
                    talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()), choice);
            });
    }

    bool createSpectrum(const std::filesystem::path& output, talas::app::StereoAnalysisChoice choice)
    {
        if (channels_.size() != 2)
        {
            juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
            return analyze(false, [safeThis, output] {
                if (safeThis != nullptr)
                    (void) safeThis->writeSpectrum(output);
            });
        }
        if (running_)
            return false;
        auto selected = talas::app::stereoAnalysisSamples(channels_, choice);
        return startJob("Analyzing...",
            [this, settings = analysisSettings(), selected = std::move(selected), rate = sampleRate_] {
                for (const auto& samples : selected)
                {
                    auto result = talas::audio::analyzeMonoAudioPartialTracks(
                        samples, rate, settings, {}, [this] { return cancelRequested_.load(); });
                    if (!result.ok())
                    {
                        jobError_ = result.error ? juceString(result.error->message) : juce::String{"The analysis failed"};
                        return;
                    }
                    jobChannels_.push_back(std::move(result.asset));
                }
            },
            [this, files = talas::app::stereoAnalysisFiles(output, choice)] {
                auto assets = std::move(jobChannels_);
                jobChannels_.clear();
                for (std::size_t index = 0; index < files.size() && index < assets.size(); ++index)
                {
                    if (!publish(files[index], leveledAsset(std::move(assets[index]))))
                        return;
                }
                Completed completed;
                completed.kind = Kind::Spectral;
                completed.sample = sample_;
                completed.output = files.front();
                if (files.size() == 2)
                    completed.secondOutput = files[1];
                completed.durationSeconds = durationSeconds();
                completed.summary = files.size() == 2 ? juce::String{"Spectrum files of the left and right channels"}
                                                      : "Spectrum file of the " + pathText(files.front().filename());
                progress_.setText(completed.summary, juce::dontSendNotification);
                if (onCompleted_)
                    onCompleted_(std::move(completed));
            });
    }

    [[nodiscard]] talas::project::PartialTrackAsset leveledAsset(talas::project::PartialTrackAsset asset) const
    {
        const auto level = level_.getValue();
        for (auto& frame : asset.frames)
        {
            for (auto& point : frame.points)
                point.amplitude *= level;
        }
        return asset;
    }

    [[nodiscard]] bool publish(const std::filesystem::path& output, talas::project::PartialTrackAsset asset)
    {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        std::string error;
        if (!talas::audio::publishPartialTrackAssetNoReplace(output, asset, error))
        {
            onStatus_("The spectrum file could not be written: " + juceString(error));
            return false;
        }
        return true;
    }

    // The spectrum file, at the level chosen.
    bool writeSpectrum(const std::filesystem::path& output)
    {
        if (!analysis_)
            return false;
        auto asset = *analysis_;
        const auto level = level_.getValue();
        std::size_t points = 0;
        for (auto& frame : asset.frames)
        {
            points += frame.points.size();
            for (auto& point : frame.points)
                point.amplitude *= level;
        }
        if (!publish(output, std::move(asset)))
            return false;
        Completed completed;
        completed.kind = Kind::Spectral;
        completed.sample = sample_;
        completed.output = output;
        completed.durationSeconds = durationSeconds();
        completed.summary = "Spectrum file: " + juce::String(static_cast<juce::int64>(analysis_->frames.size()))
            + " frames, " + juce::String(static_cast<juce::int64>(points)) + " points";
        progress_.setText(completed.summary, juce::dontSendNotification);
        if (onCompleted_)
            onCompleted_(std::move(completed));
        return true;
    }

    // The pitch asked for, middle C offered.
    void askPitch()
    {
        if (!analysis_ || trace_.empty())
        {
            onStatus_("Trace the fundamental first");
            return;
        }
        describeHarmonic();
        pitchWindow_ = std::make_unique<juce::AlertWindow>(
            "Pitch of the analysis", harmonicHint_.getText(), juce::MessageBoxIconType::QuestionIcon, this);
        pitchWindow_->addTextEditor("pitch", "60", "Pitch (nn)");
        pitchWindow_->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        pitchWindow_->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
        pitchWindow_->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result) {
            if (safeThis == nullptr || result != 1)
                return;
            const auto noteNumber = safeThis->pitchWindow_->getTextEditorContents("pitch").getDoubleValue();
            safeThis->chooseHarmonicFile(440.0 * std::pow(2.0, (noteNumber - 69.0) / 12.0));
        }));
    }

    void chooseHarmonicFile(double pitchHz)
    {
        const auto initial = talas::app::analysisOutputPath(sample_, talas::app::harmonicAnalysisFileSuffix());
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save the Quasi-Harmonic Spectrum File", juce::File{pathText(initial)},
            juce::String{std::string{talas::project::spectrumFilePatterns}});
        juce::Component::SafePointer<SpectralAnalysisWizard> safeThis(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, pitchHz](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void) safeThis->writeHarmonic(
                    talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()), pitchHz);
            });
    }

    // The harmonic spectrum and its SumOfSines table.
    bool writeHarmonic(const std::filesystem::path& output, double pitchHz)
    {
        if (!analysis_ || trace_.empty() || !(pitchHz > 0.0) || !std::isfinite(pitchHz))
        {
            onStatus_("Trace the fundamental and give a pitch first");
            return false;
        }
        auto harmonic = harmonicAsset();
        const auto table = talas::audio::harmonicAmplitudeTable(harmonic);
        if (!publish(output, std::move(harmonic)))
            return false;
        const auto tablePath = talas::app::harmonicAnalysisTablePath(output);
        std::string error;
        if (!talas::audio::writeHarmonicAmplitudeTable(tablePath, table, error))
        {
            onStatus_("The harmonic table could not be written: " + juceString(error));
            return false;
        }
        Completed completed;
        completed.kind = Kind::Spectral;
        completed.sample = sample_;
        completed.output = output;
        completed.durationSeconds = durationSeconds();
        completed.harmonicTable = tablePath;
        completed.harmonicFrames = table.frames;
        completed.harmonicPartials = table.partials;
        completed.pitchHz = pitchHz;
        completed.summary = "Quasi-harmonic spectrum file: " + juce::String(static_cast<juce::int64>(table.frames))
            + " frames of " + juce::String(static_cast<juce::int64>(table.partials)) + " harmonics at "
            + noteText(pitchHz);
        progress_.setText(completed.summary, juce::dontSendNotification);
        if (onCompleted_)
            onCompleted_(std::move(completed));
        return true;
    }

    StatusCallback onStatus_;
    CompletedCallback onCompleted_;
    AuditionCallback onAudition_;
    talas::app::SpectralAnalysisSteps steps_;
    std::vector<std::pair<Step, juce::Component*>> pages_;
    std::filesystem::path sample_;
    std::vector<double> mono_;
    double sampleRate_ = 0.0;
    std::optional<talas::project::PartialTrackAsset> analysis_;
    std::vector<double> resynthesisSamples_;
    std::vector<talas::audio::FramePitch> pitches_;
    std::vector<FundamentalPoint> trace_;
    int tracedHarmonic_ = 1;
    std::vector<double> harmonicSamples_;
    bool harmonicsStale_ = true;
    std::size_t oscillatorCount_ = 0;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::AlertWindow> pitchWindow_;
    std::unique_ptr<juce::AlertWindow> choiceWindow_;
    std::vector<std::vector<double>> channels_;
    std::vector<talas::project::PartialTrackAsset> jobChannels_;
    std::thread worker_;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> done_{false};
    bool running_ = false;
    bool wasShowing_ = false;
    std::function<void()> then_;
    juce::String jobError_;
    std::optional<talas::project::PartialTrackAsset> jobAnalysis_;
    std::vector<double> jobSamples_;

    juce::Label heading_;
    juce::Label progress_;
    juce::TextButton back_;
    juce::TextButton next_;
    juce::TextButton cancel_;
    juce::TextButton stop_;
    juce::TextButton select_;
    juce::Label sampleLabel_;
    juce::Label silenceLabel_;
    juce::TextEditor silence_;
    juce::Label lowestLabel_;
    juce::ComboBox lowest_;
    juce::Label responseLabel_;
    juce::ComboBox response_;
    juce::TextButton audition_;
    juce::ToggleButton resynthesis_;
    juce::ToggleButton original_;
    juce::Slider level_;
    juce::Label oscillators_;
    juce::TextButton createSpectrum_;
    juce::TextButton quasiHarmonic_;
    juce::Label saveHint_;
    juce::Label unpitchedLabel_;
    FineTuningSlider unpitched_;
    juce::Label unpitchedHint_;
    TraceView traceView_;
    juce::ToggleButton traceOriginal_;
    juce::ToggleButton fundamentalOnly_;
    juce::ToggleButton allHarmonics_;
    juce::TextButton autoLower_;
    juce::TextButton autoHigher_;
    juce::TextButton handTrace_;
    juce::Label harmonicLabel_;
    juce::TextEditor harmonic_;
    juce::Label traceLevelLabel_;
    juce::Slider traceLevel_;
    juce::TextButton zoomOut_;
    juce::TextButton createHarmonic_;
    juce::Label harmonicHint_;
    juce::TextButton newSample_;
    juce::TextButton adjustRange_;
    juce::TextButton adjustUnpitched_;
    juce::TextButton adjustFundamental_;
};
}

std::unique_ptr<AnalysisToolPanel> makeSpectralAnalysisWizard(
    AnalysisToolPanel::StatusCallback onStatus,
    AnalysisToolPanel::CompletedCallback onCompleted,
    AnalysisToolPanel::AuditionCallback onAudition)
{
    return std::make_unique<SpectralAnalysisWizard>(std::move(onStatus), std::move(onCompleted), std::move(onAudition));
}
}
