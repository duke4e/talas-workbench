#include "app/workbench/WorkbenchAnalysisPanel.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/audio/AnalyzerDisplayMapping.h"
#include "talas/audio/OscilloscopeAnalysis.h"
#include "talas/audio/SpectrumAnalysis.h"
#include "talas/runtime/RealtimeMetrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
inline constexpr int analysisPanelHeaderHeight = 22;
inline constexpr int analysisPanelControlRowsHeight = 206;
inline constexpr int analysisPanelCaptureStatusHeight = 18;
inline constexpr int analysisPanelCaptureStatusGap = 2;
inline constexpr int analysisPanelEvidenceRowHeight = 20;

[[nodiscard]] juce::String compactAnalysisFrequency(double frequency)
{
    auto text = juce::String(frequency, 9);
    while (text.containsChar('.') && text.endsWithChar('0'))
        text = text.dropLastCharacters(1);
    if (text.endsWithChar('.'))
        text = text.dropLastCharacters(1);
    return text;
}

class AnalysisPanelImpl final : public AnalysisPanel, private juce::Timer
{
public:
    AnalysisPanelImpl()
        : freezeButton_("Freeze")
        , pinButton_("Pin")
        , removeButton_("Unpin")
        , moveUpButton_("Up")
        , moveDownButton_("Down")
        , visibilityButton_("Show")
        , soloButton_("Solo")
        , auditionStartButton_("Start")
        , auditionStopButton_("Stop")
        , auditionCommitButton_("Commit")
        , outputTakeRecordButton_("Record Take...")
        , outputTakeStopButton_("Stop")
        , outputTakeCancelButton_("Cancel")
        , peakHoldButton_("Hold")
        , resetButton_("Reset")
        , rearmButton_("Rearm")
    {
        setWantsKeyboardFocus(true);
        setFocusContainerType(
            juce::Component::FocusContainerType::none);
        describeControl(
            *this,
            "Analysis",
            "Oscilloscope and spectrum analysis for the selected display node. Focus the plot and use Left or Right to inspect measurements, Shift for fine steps, Home or End for endpoints, and Escape to clear the cursor.");
        addAndMakeVisible(freezeButton_);
        addAndMakeVisible(traceBox_);
        addChildComponent(sourceCaptureBox_);
        addChildComponent(sourceCaptureLabel_);
        sourceCaptureLabel_.setText("Source", juce::dontSendNotification);
        sourceCaptureLabel_.setFont(WorkbenchType::data());
        sourceCaptureLabel_.setColour(juce::Label::textColourId, textColour);
        describeControl(sourceCaptureBox_, "Analysis source",
            "Choose the project graph or one exact Score clip and repeat to inspect.");
        sourceCaptureBox_.onChange = [this] { applySelectedCapture(); };
        addAndMakeVisible(pinButton_);
        addAndMakeVisible(removeButton_);
        addAndMakeVisible(moveUpButton_);
        addAndMakeVisible(moveDownButton_);
        addAndMakeVisible(visibilityButton_);
        addAndMakeVisible(soloButton_);
        addAndMakeVisible(traceScaleLabel_);
        addAndMakeVisible(traceScaleSlider_);
        addAndMakeVisible(historyBox_);
        addAndMakeVisible(refreshRateBox_);
        addAndMakeVisible(auditionSourceBox_);
        addAndMakeVisible(auditionInputBox_);
        addAndMakeVisible(auditionAssetBox_);
        addAndMakeVisible(auditionFrequencyBox_);
        addAndMakeVisible(auditionStartButton_);
        addAndMakeVisible(auditionStopButton_);
        addAndMakeVisible(auditionCommitButton_);
        addAndMakeVisible(auditionStatus_);
        addAndMakeVisible(outputTakeRecordButton_);
        addAndMakeVisible(outputTakeStopButton_);
        addAndMakeVisible(outputTakeCancelButton_);
        addAndMakeVisible(outputTakeStatus_);
        addAndMakeVisible(frameCountBox_);
        addAndMakeVisible(zoomBox_);
        addAndMakeVisible(windowBox_);
        addAndMakeVisible(averageBox_);
        addAndMakeVisible(peakHoldButton_);
        addAndMakeVisible(resetButton_);
        addAndMakeVisible(triggerModeBox_);
        addAndMakeVisible(triggerLevelSlider_);
        addAndMakeVisible(sweepFrequencySlider_);
        addAndMakeVisible(rearmButton_);
        addAndMakeVisible(frequencyScaleBox_);
        addAndMakeVisible(spectrumPanSlider_);
        addAndMakeVisible(preparedStatus_);
        addAndMakeVisible(monitorFocus_);
        addAndMakeVisible(cursorReadout_);
        preparedStatus_.setInterceptsMouseClicks(false, false);
        preparedStatus_.setColour(
            juce::Label::textColourId,
            dataColour);
        preparedStatus_.setFont(WorkbenchType::micro());
        preparedStatus_.setJustificationType(
            juce::Justification::centredLeft);
        describeControl(
            preparedStatus_,
            "Prepared spectral contract",
            "The exact prepared spectral frame format and timing for the selected node, or the current graph inventory.");
        monitorFocus_.setInterceptsMouseClicks(false, false);
        monitorFocus_.setWantsKeyboardFocus(true);
        monitorFocus_.setColour(
            juce::Label::textColourId,
            selectionColour);
        monitorFocus_.setFont(WorkbenchType::micro());
        monitorFocus_.setJustificationType(
            juce::Justification::centredLeft);
        describeControl(
            monitorFocus_,
            "Focused diagnostic monitor record",
            "The exact Event, MIDI, OSC ingress, or OSC feedback record selected from Diagnostics.");
        cursorReadout_.setInterceptsMouseClicks(false, false);
        cursorReadout_.setColour(
            juce::Label::textColourId,
            textColour);
        cursorReadout_.setFont(WorkbenchType::data());
        cursorReadout_.setJustificationType(
            juce::Justification::centredRight);
        describeControl(
            cursorReadout_,
            "Analyzer cursor measurement",
            "Current time and amplitude, or frequency and level, at the analyzer cursor.");

        auditionSourceBox_.addItem("Upstream", 1);
        auditionSourceBox_.addItem("Silence", 2);
        auditionSourceBox_.addItem("Impulse", 3);
        auditionSourceBox_.addItem("Sine", 4);
        auditionSourceBox_.addItem("Frozen asset", 5);
        auditionSourceBox_.setSelectedId(1, juce::dontSendNotification);
        auditionFrequencyBox_.setEditableText(true);
        auditionFrequencyBox_.addItem("220", 1);
        auditionFrequencyBox_.addItem("440", 2);
        auditionFrequencyBox_.addItem("880", 3);
        auditionFrequencyBox_.setText("440", juce::dontSendNotification);
        auditionStatus_.setInterceptsMouseClicks(false, false);
        auditionStatus_.setColour(juce::Label::textColourId, dataColour);
        auditionStatus_.setFont(WorkbenchType::micro());
        auditionStatus_.setJustificationType(
            juce::Justification::centredLeft);
        describeControl(
            auditionSourceBox_,
            "Analysis audition source",
            "Choose the retained upstream graph or a temporary Silence, Impulse, Sine, or frozen Audio asset source.");
        describeControl(
            auditionInputBox_,
            "Analysis audition destination input",
            "Choose the exact Audio input replaced only inside the temporary audition graph.");
        describeControl(
            auditionAssetBox_,
            "Frozen Analysis asset",
            "Choose one exact project Audio asset to decode and freeze before audition starts.");
        describeControl(
            auditionFrequencyBox_,
            "Analysis Sine frequency",
            "Finite Sine frequency from 1 to 20000 hertz.");
        describeControl(
            auditionStartButton_,
            "Start Analysis audition",
            "Prepare the exact selected trace off-thread and switch only the temporary audio callback target.");
        describeControl(
            auditionStopButton_,
            "Stop Analysis audition",
            "Return immediately to the retained project graph without changing project history.");
        describeControl(
            auditionCommitButton_,
            "Commit Analysis test source",
            "Commit a portable root-graph Silence or Sine source as one undoable project edit.");
        describeControl(
            auditionStatus_,
            "Analysis audition status",
            "Exact source, destination, lifecycle, or cancellation reason for the temporary audition.");
        outputTakeStatus_.setInterceptsMouseClicks(false, false);
        outputTakeStatus_.setColour(
            juce::Label::textColourId,
            dataColour);
        outputTakeStatus_.setFont(WorkbenchType::micro());
        outputTakeStatus_.setJustificationType(
            juce::Justification::centredLeft);
        describeControl(
            outputTakeRecordButton_,
            "Record output take",
            "Choose one PCM16 WAV destination and record the exact prepared project output through a bounded callback queue.");
        describeControl(
            outputTakeStopButton_,
            "Stop and save output take",
            "Stop callback admission, drain the bounded queue, close the WAV writer, and publish the completed take.");
        describeControl(
            outputTakeCancelButton_,
            "Cancel output take",
            "Stop recording and remove only the private temporary take without publishing a destination.");
        describeControl(
            outputTakeStatus_,
            "Output take status",
            "Exact destination, sample rate, channel count, elapsed frames, drops, incomplete state, or failure reason.");
        outputTakeRecordButton_.onClick = [this] {
            if (callbacks_.chooseOutputTake)
                callbacks_.chooseOutputTake();
        };
        outputTakeStopButton_.onClick = [this] {
            if (callbacks_.stopOutputTake)
                callbacks_.stopOutputTake();
        };
        outputTakeCancelButton_.onClick = [this] {
            if (callbacks_.cancelOutputTake)
                callbacks_.cancelOutputTake();
        };
        auditionSourceBox_.onChange = [this] {
            updateAuditionControlState();
            resized();
        };
        auditionFrequencyBox_.onChange = [this] {
            updateAuditionControlState();
        };
        auditionStartButton_.onClick = [this] {
            if (!callbacks_.startAudition)
                return;
            const auto index = traceBox_.getSelectedItemIndex();
            if (index < 0
                || static_cast<std::size_t>(index)
                    >= projection_.traces.size()
                || projection_.traces[static_cast<std::size_t>(index)]
                       .trace.kind
                    != talas::project::WorkspaceAnalysisTraceKind::Audio)
            {
                return;
            }
            talas::project::WorkspaceAnalysisAuditionState request;
            request.traceId = projection_.traces[
                static_cast<std::size_t>(index)].trace.traceId;
            switch (auditionSourceBox_.getSelectedId())
            {
            case 2:
                request.source = talas::project::WorkspaceAnalysisTestSourceKind::Silence;
                break;
            case 3:
                request.source = talas::project::WorkspaceAnalysisTestSourceKind::Impulse;
                break;
            case 4:
                request.source = talas::project::WorkspaceAnalysisTestSourceKind::Sine;
                break;
            case 5:
                request.source = talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset;
                break;
            default:
                request.source = talas::project::WorkspaceAnalysisTestSourceKind::None;
                break;
            }
            if (request.source
                != talas::project::WorkspaceAnalysisTestSourceKind::None)
            {
                const auto inputIndex =
                    auditionInputBox_.getSelectedItemIndex();
                if (inputIndex < 0
                    || static_cast<std::size_t>(inputIndex)
                        >= auditionState_.destinationInputs.size())
                {
                    auditionStatus_.setText(
                        "Select an exact Audio destination input",
                        juce::dontSendNotification);
                    return;
                }
                request.destinationInputPortId =
                    auditionState_.destinationInputs[
                        static_cast<std::size_t>(inputIndex)].id;
            }
            const auto frequency =
                auditionFrequencyBox_.getText().getDoubleValue();
            if (request.source
                    == talas::project::WorkspaceAnalysisTestSourceKind::Sine
                && (!std::isfinite(frequency)
                    || frequency < 1.0
                    || frequency > 20000.0))
            {
                auditionStatus_.setText(
                    "Enter a Sine frequency from 1 to 20000 Hz",
                    juce::dontSendNotification);
                auditionStatus_.setTitle(
                    "Analysis audition status: Enter a Sine frequency from 1 to 20000 Hz");
                return;
            }
            request.sineFrequencyHz = frequency;
            if (request.source
                == talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset)
            {
                const auto assetIndex =
                    auditionAssetBox_.getSelectedItemIndex();
                if (assetIndex < 0
                    || static_cast<std::size_t>(assetIndex)
                        >= auditionState_.frozenAssets.size())
                {
                    auditionStatus_.setText(
                        "Select one exact frozen Audio asset",
                        juce::dontSendNotification);
                    return;
                }
                request.frozenAssetId = auditionState_.frozenAssets[
                    static_cast<std::size_t>(assetIndex)].id;
            }
            callbacks_.startAudition(std::move(request));
        };
        auditionStopButton_.onClick = [this] {
            if (callbacks_.stopAudition)
                callbacks_.stopAudition();
        };
        auditionCommitButton_.onClick = [this] {
            if (callbacks_.commitAudition)
                callbacks_.commitAudition();
        };

        frameCountBox_.addItem("64", 1);
        frameCountBox_.addItem("128", 2);
        frameCountBox_.addItem("256", 3);
        frameCountBox_.setSelectedId(3, juce::dontSendNotification);
        frameCountBox_.setTooltip("Analysis length in samples");
        describeControl(
            frameCountBox_,
            "Analysis length",
            "Number of captured samples used for analysis.");

        zoomBox_.addItem("1x", 1);
        zoomBox_.addItem("2x", 2);
        zoomBox_.addItem("4x", 3);
        zoomBox_.setSelectedId(1, juce::dontSendNotification);
        zoomBox_.setTooltip(
            "Scope amplitude or spectrum frequency zoom");
        describeControl(
            zoomBox_,
            "Analysis zoom",
            "Scope amplitude or spectrum frequency zoom.");

        triggerModeBox_.addItem("Sweep", 1);
        triggerModeBox_.addItem("Rise", 2);
        triggerModeBox_.addItem("Fall", 3);
        triggerModeBox_.addItem(juceString("1× Rise"), 4);
        triggerModeBox_.addItem(juceString("1× Fall"), 5);
        triggerModeBox_.setSelectedId(
            1,
            juce::dontSendNotification);
        triggerModeBox_.setTooltip(
            "Oscilloscope trigger mode");
        describeControl(
            triggerModeBox_,
            "Trigger mode",
            "Oscilloscope trigger mode.");

        triggerLevelSlider_.setSliderStyle(
            juce::Slider::LinearHorizontal);
        triggerLevelSlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            42,
            20);
        triggerLevelSlider_.setRange(-1.0, 1.0, 0.01);
        triggerLevelSlider_.setValue(
            0.0,
            juce::dontSendNotification);
        triggerLevelSlider_.setNumDecimalPlacesToDisplay(2);
        triggerLevelSlider_.setTooltip(
            "Oscilloscope trigger threshold");
        describeControl(
            triggerLevelSlider_,
            "Trigger level",
            "Oscilloscope trigger threshold from minus one to one.");

        sweepFrequencySlider_.setSliderStyle(
            juce::Slider::LinearHorizontal);
        sweepFrequencySlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, 55, 20);
        sweepFrequencySlider_.setRange(0.1, 30.0, 0.1);
        sweepFrequencySlider_.setValue(30.0, juce::dontSendNotification);
        sweepFrequencySlider_.setNumDecimalPlacesToDisplay(1);
        sweepFrequencySlider_.setTextValueSuffix(" Hz");
        describeControl(
            sweepFrequencySlider_,
            "Sweep frequency",
            "Maximum rate for a new oscilloscope trace in Sweep mode.");

        frequencyScaleBox_.addItem("Linear", 1);
        frequencyScaleBox_.addItem("Log", 2);
        frequencyScaleBox_.setSelectedId(
            1,
            juce::dontSendNotification);
        frequencyScaleBox_.setTooltip(
            "Spectrum frequency-axis scale");
        describeControl(
            frequencyScaleBox_,
            "Frequency scale",
            "Spectrum frequency-axis scale.");

        spectrumPanSlider_.setSliderStyle(
            juce::Slider::LinearHorizontal);
        spectrumPanSlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            38,
            20);
        spectrumPanSlider_.setRange(0.0, 100.0, 1.0);
        spectrumPanSlider_.setValue(
            0.0,
            juce::dontSendNotification);
        spectrumPanSlider_.setNumDecimalPlacesToDisplay(0);
        spectrumPanSlider_.setTextValueSuffix("%");
        spectrumPanSlider_.setTooltip(
            "Pan through the zoomed spectrum range");
        describeControl(
            spectrumPanSlider_,
            "Spectrum pan",
            "Pan through the zoomed spectrum range.");

        windowBox_.addItem("Rect", 1);
        windowBox_.addItem("Hann", 2);
        windowBox_.addItem("Black", 3);
        // LinearEnvelope, Hamming and Long Kaiser.
        windowBox_.addItem("Linear", 4);
        windowBox_.addItem("Hamm", 5);
        windowBox_.addItem("Kaiser", 6);
        windowBox_.setSelectedId(2, juce::dontSendNotification);
        windowBox_.setTooltip("Spectrum window");
        describeControl(
            windowBox_,
            "Spectrum window",
            "Window function used for spectrum analysis.");

        averageBox_.addItem("1x", 1);
        averageBox_.addItem("2x", 2);
        averageBox_.addItem("4x", 3);
        averageBox_.addItem("8x", 4);
        averageBox_.setSelectedId(1, juce::dontSendNotification);
        averageBox_.setTooltip("Spectrum averaging depth");
        describeControl(
            averageBox_,
            "Spectrum averaging",
            "Number of spectrum frames to average.");

        freezeButton_.setTooltip(
            "Freeze the display while capture continues draining");
        moveUpButton_.setTooltip("Move the selected trace one lane up");
        moveDownButton_.setTooltip("Move the selected trace one lane down");
        visibilityButton_.setTooltip(
            "Show or hide the selected trace while capture continues draining");
        soloButton_.setTooltip(
            "Temporarily focus only the selected trace");
        traceScaleSlider_.setSliderStyle(
            juce::Slider::LinearHorizontal);
        traceScaleSlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            40,
            20);
        traceScaleSlider_.setRange(0.01, 64.0, 0.01);
        traceScaleSlider_.setSkewFactorFromMidPoint(1.0);
        traceScaleSlider_.setValue(1.0, juce::dontSendNotification);
        traceScaleSlider_.setNumDecimalPlacesToDisplay(2);
        traceScaleSlider_.setTextValueSuffix("x");
        traceScaleSlider_.setTooltip(
            "Vertical scale for the selected trace");
        traceScaleLabel_.setText(
            "Scale",
            juce::dontSendNotification);
        traceScaleLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        traceScaleLabel_.setFont(WorkbenchType::micro());
        traceScaleLabel_.setJustificationType(
            juce::Justification::centredLeft);
        traceScaleLabel_.setInterceptsMouseClicks(false, false);
        for (const auto frames : {60, 120, 240, 600})
        {
            historyBox_.addItem(
                juce::String(frames) + " fr",
                frames);
        }
        historyBox_.setSelectedId(120, juce::dontSendNotification);
        historyBox_.setTooltip(
            "Sequential Audio and Control display history in scalar frames");
        for (const auto rate : {5, 15, 30, 60})
        {
            refreshRateBox_.addItem(
                juce::String(rate) + " Hz",
                rate);
        }
        refreshRateBox_.setSelectedId(30, juce::dontSendNotification);
        refreshRateBox_.setTooltip(
            "Analysis message-thread refresh rate");
        peakHoldButton_.setTooltip(
            "Hold the maximum value of each spectrum bin");
        resetButton_.setTooltip(
            "Reset spectrum averaging and peak hold");
        rearmButton_.setTooltip(
            "Rearm the selected single-shot trigger");
        describeControl(
            freezeButton_,
            "Freeze analysis",
            "Freeze the display while capture continues draining.");
        describeControl(
            traceBox_,
            "Pinned analysis traces",
            "Select one exact project, instance, node, and port trace while every admitted trace continues draining.");
        describeControl(
            pinButton_,
            "Pin selected target",
            "Pin the selected graph node's exact Audio output into the local Analysis workspace.");
        describeControl(
            removeButton_,
            "Unpin selected trace",
            "Remove the selected local Analysis trace without changing project history.");
        describeControl(
            moveUpButton_,
            "Move trace up",
            "Move the selected local Analysis trace one lane earlier.");
        describeControl(
            moveDownButton_,
            "Move trace down",
            "Move the selected local Analysis trace one lane later.");
        describeControl(
            visibilityButton_,
            "Trace visibility",
            "Show or hide the selected trace while its prepared capture continues draining.");
        describeControl(
            soloButton_,
            "Solo selected trace",
            "Temporarily display only the selected trace without changing workspace persistence.");
        describeControl(
            traceScaleSlider_,
            "Trace vertical scale",
            "Set the selected trace vertical scale from 0.01 to 64 times.");
        describeControl(
            traceScaleLabel_,
            "Trace scale label",
            "The following control changes the selected trace vertical scale.");
        describeControl(
            historyBox_,
            "Analysis history window",
            "Choose how many sequential Audio or Control scalar frames remain visible.");
        describeControl(
            refreshRateBox_,
            "Analysis refresh rate",
            "Choose the bounded message-thread Analysis refresh rate from 5 to 60 hertz.");
        describeControl(
            peakHoldButton_,
            "Peak hold",
            "Hold the maximum value of each spectrum bin.");
        describeControl(
            resetButton_,
            "Reset analysis",
            "Reset spectrum averaging and peak hold.");
        describeControl(
            rearmButton_,
            "Rearm trigger",
            "Rearm the selected single-shot trigger.");

        freezeButton_.onClick = [this] {
            frozen_ = freezeButton_.getToggleState();
            if (callbacks_.setFrozen)
                callbacks_.setFrozen(frozen_);
            updateControlState();
            repaint();
        };
        traceBox_.onChange = [this] {
            const auto index = traceBox_.getSelectedItemIndex();
            if (index < 0
                || static_cast<std::size_t>(index)
                    >= projection_.traces.size())
            {
                return;
            }
            if (callbacks_.selectTrace)
            {
                const auto& trace = projection_.traces[
                    static_cast<std::size_t>(index)];
                traceBox_.setTooltip(
                    juceString("Exact analysis target · ")
                    + juceString(trace.targetIdentity));
                callbacks_.selectTrace(
                    trace.trace.traceId);
            }
        };
        pinButton_.onClick = [this] {
            if (callbacks_.pinSelectedTarget)
                callbacks_.pinSelectedTarget();
        };
        removeButton_.onClick = [this] {
            const auto index = traceBox_.getSelectedItemIndex();
            if (index < 0
                || static_cast<std::size_t>(index)
                    >= projection_.traces.size()
                || !callbacks_.removeTrace)
            {
                return;
            }
            callbacks_.removeTrace(
                projection_.traces[static_cast<std::size_t>(index)]
                    .trace.traceId);
        };
        moveUpButton_.onClick = [this] {
            moveSelectedTrace(-1);
        };
        moveDownButton_.onClick = [this] {
            moveSelectedTrace(1);
        };
        visibilityButton_.onClick = [this] {
            const auto* trace = selectedTraceView();
            if (trace != nullptr && callbacks_.setTraceVisible)
            {
                callbacks_.setTraceVisible(
                    trace->trace.traceId,
                    visibilityButton_.getToggleState());
            }
        };
        soloButton_.onClick = [this] {
            const auto* trace = selectedTraceView();
            if (trace == nullptr || !soloButton_.getToggleState())
                soloTraceId_.reset();
            else
                soloTraceId_ = trace->trace.traceId;
            updateControlState();
            repaint();
        };
        traceScaleSlider_.onValueChange = [this] {
            const auto* trace = selectedTraceView();
            if (trace != nullptr && callbacks_.setTraceScale)
            {
                callbacks_.setTraceScale(
                    trace->trace.traceId,
                    traceScaleSlider_.getValue());
            }
        };
        historyBox_.onChange = [this] {
            if (callbacks_.setHistoryFrames
                && historyBox_.getSelectedId() > 0)
            {
                callbacks_.setHistoryFrames(
                    static_cast<std::uint32_t>(
                        historyBox_.getSelectedId()));
            }
        };
        refreshRateBox_.onChange = [this] {
            if (callbacks_.setRefreshRateHz
                && refreshRateBox_.getSelectedId() > 0)
            {
                callbacks_.setRefreshRateHz(
                    static_cast<std::uint32_t>(
                        refreshRateBox_.getSelectedId()));
            }
        };
        frameCountBox_.onChange = [this] {
            singleShotState_.rearm();
            resetSpectrumState();
            repaint();
        };
        zoomBox_.onChange = [this] {
            updateControlState();
            refreshCursorReadout();
            repaint();
        };
        triggerModeBox_.onChange = [this] {
            singleShotState_.rearm();
            sweepState_.reset();
            updateControlState();
            refreshCursorReadout();
            repaint();
        };
        triggerLevelSlider_.onValueChange = [this] {
            triggerLevelEdited_ = true;
            singleShotState_.rearm();
            updateControlState();
            refreshCursorReadout();
            repaint();
        };
        sweepFrequencySlider_.onValueChange = [this] {
            sweepState_.reset();
            refreshCursorReadout();
            repaint();
        };
        rearmButton_.onClick = [this] {
            singleShotState_.rearm();
            updateControlState();
            repaint();
        };
        frequencyScaleBox_.onChange = [this] {
            refreshCursorReadout();
            repaint();
        };
        startTimerHz(static_cast<int>(refreshRateHz_));
        spectrumPanSlider_.onValueChange = [this] {
            refreshCursorReadout();
            repaint();
        };
        windowBox_.onChange = [this] {
            resetSpectrumState();
            repaint();
        };
        averageBox_.onChange = [this] {
            resetSpectrumState();
            repaint();
        };
        peakHoldButton_.onClick = [this] {
            spectrumPeaks_.clear();
            if (peakHoldButton_.getToggleState()
                && !currentSpectrumMagnitudes_.empty())
            {
                talas::audio::updateSpectrumPeakHold(
                    currentSpectrumMagnitudes_,
                    spectrumPeaks_);
            }
            updateControlState();
            repaint();
        };
        resetButton_.onClick = [this] {
            resetSpectrumState();
            repaint();
        };
        updateControlState();
        refreshCursorReadout();
        setPreparedStatus(
            juceString("Prepared spectral contract · no graph"));
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    ~AnalysisPanelImpl() override
    {
        stopTimer();
        if (cursorMouseListenerRoot_ != nullptr)
            cursorMouseListenerRoot_->removeMouseListener(this);
    }

    void parentHierarchyChanged() override
    {
        auto* nextRoot = getTopLevelComponent();
        if (nextRoot == this)
            nextRoot = nullptr;
        if (nextRoot == cursorMouseListenerRoot_)
            return;
        if (cursorMouseListenerRoot_ != nullptr)
            cursorMouseListenerRoot_->removeMouseListener(this);
        cursorMouseListenerRoot_ = nextRoot;
        if (cursorMouseListenerRoot_ != nullptr)
        {
            cursorMouseListenerRoot_->addMouseListener(
                this,
                true);
        }
    }

    void setSelectedNode(
        std::string nodeId,
        std::string_view typeId) override
    {
        selectionNodeId_ = nodeId;
        selectionTypeId_ = std::string{typeId};
        if (!infoDisplay_.has_value())
            showNode(std::move(nodeId), typeId);
    }

    void showInfoDisplay(std::string nodeId, std::string_view typeId) override
    {
        // The Info menu's analyzer takes the
        // preferences' length and window, the panel's own kept for after.
        if (!infoDisplay_.has_value())
            ownSpectrumSettings_ = spectrumSettings();
        if (typeId == std::string_view{"talas.spectrumAnalyzerDisplay"})
            applySpectrumSettings(talas::app::spectrumDisplaySettings(true, infoSpectrumSettings_, ownSpectrumSettings_));
        infoDisplay_ = nodeId;
        showNode(std::move(nodeId), typeId);
    }

    void clearInfoDisplay() override
    {
        if (!infoDisplay_.has_value())
            return;
        infoDisplay_.reset();
        applySpectrumSettings(talas::app::spectrumDisplaySettings(false, infoSpectrumSettings_, ownSpectrumSettings_));
        showNode(selectionNodeId_, selectionTypeId_);
    }

    void setInfoSpectrumSettings(talas::app::SpectrumAnalyzerSettings settings) override
    {
        infoSpectrumSettings_ = settings;
    }

    [[nodiscard]] talas::app::SpectrumAnalyzerSettings spectrumSettings() const
    {
        // The window box's ids follow the preferences' order of windows.
        const auto& windows = talas::app::spectrumAnalyzerWindows();
        const auto index = static_cast<std::size_t>(std::clamp(windowBox_.getSelectedId(), 1, static_cast<int>(windows.size())) - 1);
        return {static_cast<unsigned>(selectedFrameCount()), windows[index]};
    }

    void applySpectrumSettings(const talas::app::SpectrumAnalyzerSettings& settings)
    {
        frameCountBox_.setSelectedId(settings.length <= 64 ? 1 : settings.length <= 128 ? 2 : 3, juce::dontSendNotification);
        const auto& windows = talas::app::spectrumAnalyzerWindows();
        const auto found = std::ranges::find(windows, settings.window);
        windowBox_.setSelectedId(found == windows.end() ? 2 : static_cast<int>(found - windows.begin()) + 1,
            juce::dontSendNotification);
        resetSpectrumState();
    }

    [[nodiscard]] std::optional<std::string> infoDisplayForSmoke() const override
    {
        return infoDisplay_.has_value() && *infoDisplay_ == selectedNodeId_ && mode_ != Mode::None ? infoDisplay_
                                                                                                   : std::nullopt;
    }

    void showNode(
        std::string nodeId,
        std::string_view typeId)
    {
        Mode nextMode = Mode::None;
        if (typeId == "talas.oscilloscopeDisplay")
            nextMode = Mode::Oscilloscope;
        else if (typeId == "talas.spectrumAnalyzerDisplay")
            nextMode = Mode::Spectrum;
        if (selectedNodeId_ == nodeId && mode_ == nextMode)
            return;
        selectedNodeId_ = std::move(nodeId);
        mode_ = nextMode;
        clearDisplay();
        frozen_ = false;
        freezeButton_.setToggleState(
            false,
            juce::dontSendNotification);
        peakHoldButton_.setToggleState(
            false,
            juce::dontSendNotification);
        triggerModeBox_.setSelectedId(
            1,
            juce::dontSendNotification);
        applyPreparedOscilloscopeFields();
        updateControlState();
        refreshCursorReadout();
        repaint();
    }

    void setWorkspaceProjection(
        talas::app::AnalysisWorkbenchProjection projection,
        bool frozen,
        std::uint32_t refreshRateHz,
        std::uint32_t historyFrames) override
    {
        projection_ = std::move(projection);
        frozen_ = frozen;
        const auto nextRefreshRate = std::clamp(
            refreshRateHz,
            talas::project::minimumWorkspaceAnalysisRefreshRateHz,
            talas::project::maximumWorkspaceAnalysisRefreshRateHz);
        if (refreshRateHz_ != nextRefreshRate)
        {
            refreshRateHz_ = nextRefreshRate;
            startTimerHz(static_cast<int>(refreshRateHz_));
        }
        historyFrames_ = std::clamp(
            historyFrames,
            talas::project::minimumWorkspaceAnalysisHistoryFrames,
            talas::project::maximumWorkspaceAnalysisHistoryFrames);
        refreshRateBox_.setSelectedId(
            static_cast<int>(refreshRateHz_),
            juce::dontSendNotification);
        if (refreshRateBox_.getSelectedId() == 0)
        {
            refreshRateBox_.setText(
                juce::String(refreshRateHz_) + " Hz",
                juce::dontSendNotification);
        }
        historyBox_.setSelectedId(
            static_cast<int>(historyFrames_),
            juce::dontSendNotification);
        if (historyBox_.getSelectedId() == 0)
        {
            historyBox_.setText(
                juce::String(historyFrames_) + " fr",
                juce::dontSendNotification);
        }
        freezeButton_.setToggleState(
            frozen_,
            juce::dontSendNotification);
        traceBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0;
             index < projection_.traces.size();
             ++index)
        {
            const auto& trace = projection_.traces[index];
            auto label = juceString(trace.trace.nodeId);
            if (trace.trace.portId.has_value())
                label += juceString(" · ")
                    + juceString(*trace.trace.portId);
            if (!trace.available())
                label += juceString(" · unavailable");
            else if (!trace.trace.visible)
                label += juceString(" · hidden");
            traceBox_.addItem(
                label,
                static_cast<int>(index + 1));
        }
        if (projection_.selectedTraceIndex.has_value()
            && *projection_.selectedTraceIndex
                < projection_.traces.size())
        {
            traceBox_.setSelectedItemIndex(
                static_cast<int>(*projection_.selectedTraceIndex),
                juce::dontSendNotification);
        }
        else if (!projection_.traces.empty())
        {
            traceBox_.setSelectedItemIndex(
                0,
                juce::dontSendNotification);
        }
        if (const auto selected = traceBox_.getSelectedItemIndex();
            selected >= 0
            && static_cast<std::size_t>(selected)
                < projection_.traces.size())
        {
            traceBox_.setTooltip(
                juceString("Exact analysis target · ")
                + juceString(
                    projection_.traces[
                        static_cast<std::size_t>(selected)]
                        .targetIdentity));
        }
        else
        {
            traceBox_.setTooltip(
                "No pinned analysis trace is selected");
        }
        if (soloTraceId_.has_value()
            && !std::ranges::any_of(
                projection_.traces,
                [&](const auto& trace) {
                    return trace.trace.traceId == *soloTraceId_;
                }))
        {
            soloTraceId_.reset();
        }
        rebuildTraceLanes();
        updateSelectedTraceView();
        updateSelectedTraceControls();
        updateControlState();
        refreshCursorReadout();
        resized();
        repaint();
    }

    void setCallbacks(
        AnalysisPanelCallbacks callbacks) override
    {
        callbacks_ = std::move(callbacks);
    }

    void setCapture(
        std::shared_ptr<talas::runtime::RealtimeAnalysisCapture> capture,
        double sampleRate,
        std::shared_ptr<const talas::runtime::PreparedScoreSourceAnalysisSet> scoreSources = {}) override
    {
        if (rootCapture_ == capture && scoreSources_ == scoreSources && sampleRate_ == sampleRate)
            return;
        std::optional<std::pair<std::string, std::uint32_t>> selected;
        const auto previous = sourceCaptureBox_.getSelectedId() - 2;
        if (scoreSources_ && previous >= 0 && static_cast<std::size_t>(previous) < scoreSources_->size())
            selected = {(*scoreSources_)[previous].clipId, (*scoreSources_)[previous].iterationOrdinal};
        rootCapture_ = std::move(capture);
        scoreSources_ = std::move(scoreSources);
        sourceSampleRate_ = sampleRate;
        sourceCaptureBox_.clear(juce::dontSendNotification);
        if (rootCapture_) sourceCaptureBox_.addItem("Project graph", 1);
        int selectedId = rootCapture_ ? 1 : 0;
        if (scoreSources_)
            for (std::size_t index = 0; index < scoreSources_->size(); ++index)
            {
                const auto& source = (*scoreSources_)[index];
                const auto id = static_cast<int>(index) + 2;
                sourceCaptureBox_.addItem(juceString(source.clipId) + " / Repeat "
                    + juce::String(source.iterationOrdinal + 1), id);
                if (selectedId == 0 || (selected && selected->first == source.clipId
                    && selected->second == source.iterationOrdinal)) selectedId = id;
            }
        sourceCaptureBox_.setSelectedId(selectedId, juce::dontSendNotification);
        const bool visible = scoreSources_ && !scoreSources_->empty();
        sourceCaptureBox_.setVisible(visible); sourceCaptureLabel_.setVisible(visible);
        applySelectedCapture();
        resized();
    }

    void applySelectedCapture()
    {
        auto capture = rootCapture_;
        const auto selected = sourceCaptureBox_.getSelectedId() - 2;
        if (scoreSources_ && selected >= 0 && static_cast<std::size_t>(selected) < scoreSources_->size())
            capture = (*scoreSources_)[selected].capture;
        sourceCaptureBox_.setTooltip("Analysis source: " + sourceCaptureBox_.getText());
        applyCapture(std::move(capture), sourceSampleRate_);
    }

    void applyCapture(std::shared_ptr<talas::runtime::RealtimeAnalysisCapture> capture, double sampleRate)
    {
        if (capture_ == capture && sampleRate_ == sampleRate)
            return;
        capture_ = std::move(capture);
        sampleRate_ = sampleRate;
        clearDisplay();
        applyPreparedOscilloscopeFields();
        captureScratch_.clear();
        slowSweepScratch_.clear();
        separateTriggerScratch_.clear();
        visibleSamples_.clear();
        visibleSeparateTriggerSamples_.clear();
        traceLanes_.clear();
        if (capture_ != nullptr && capture_->ready())
        {
            captureScratch_.resize(capture_->maxPayloadValues());
            slowSweepScratch_.resize(capture_->maxPayloadValues());
            separateTriggerScratch_.resize(capture_->maxPayloadValues());
            visibleSamples_.resize(capture_->maxPayloadValues());
            visibleSeparateTriggerSamples_.resize(capture_->maxPayloadValues());
        }
        rebuildTraceLanes();
        frozen_ = false;
        freezeButton_.setToggleState(
            false,
            juce::dontSendNotification);
        updateControlState();
        refreshCursorReadout();
        repaint();
    }

    [[nodiscard]] bool selectScoreSourceForSmoke(std::string_view clipId, std::uint32_t iteration) override
    {
        if (scoreSources_)
            for (std::size_t index = 0; index < scoreSources_->size(); ++index)
                if ((*scoreSources_)[index].clipId == clipId && (*scoreSources_)[index].iterationOrdinal == iteration)
                {
                    sourceCaptureBox_.setSelectedId(static_cast<int>(index) + 2, juce::sendNotificationSync);
                    return capture_ == (*scoreSources_)[index].capture;
                }
        return false;
    }

    [[nodiscard]] bool sourceControlsUsableForSmoke() const override
    {
        return sourceCaptureBox_.isVisible() && sourceCaptureLabel_.isVisible()
            && sourceCaptureBox_.getWidth() >= 80 && sourceCaptureBox_.getHeight() >= 24
            && getLocalBounds().contains(sourceCaptureBox_.getBounds())
            && sourceCaptureBox_.getTitle().isNotEmpty() && sourceCaptureBox_.getSelectedId() >= 1;
    }

    void clearCapture() override
    {
        setCapture({}, 0.0);
    }

    void setPlaying(bool playing) override
    {
        playing_ = playing;
        repaint();
    }

    void setPreparedStatus(juce::String status) override
    {
        if (preparedStatus_.getText() == status)
            return;
        preparedStatus_.setText(status, juce::dontSendNotification);
        preparedStatus_.setTitle(
            "Prepared spectral contract: " + status);
    }

    void setAuditionState(
        AnalysisAuditionPanelState state) override
    {
        auditionState_ = std::move(state);
        auditionInputBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0;
             index < auditionState_.destinationInputs.size();
             ++index)
        {
            auditionInputBox_.addItem(
                juceString(auditionState_.destinationInputs[index].label),
                static_cast<int>(index + 1));
        }
        auditionAssetBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0;
             index < auditionState_.frozenAssets.size();
             ++index)
        {
            auditionAssetBox_.addItem(
                juceString(auditionState_.frozenAssets[index].label),
                static_cast<int>(index + 1));
        }
        if (auditionState_.request.has_value())
        {
            const auto& request = *auditionState_.request;
            auditionSourceBox_.setSelectedId(
                1 + static_cast<int>(request.source),
                juce::dontSendNotification);
            auditionFrequencyBox_.setText(
                compactAnalysisFrequency(request.sineFrequencyHz),
                juce::dontSendNotification);
            if (request.destinationInputPortId.has_value())
            {
                const auto input = std::ranges::find(
                    auditionState_.destinationInputs,
                    *request.destinationInputPortId,
                    &AnalysisAuditionPanelOption::id);
                if (input != auditionState_.destinationInputs.end())
                {
                    auditionInputBox_.setSelectedItemIndex(
                        static_cast<int>(std::distance(
                            auditionState_.destinationInputs.begin(), input)),
                        juce::dontSendNotification);
                }
            }
            if (request.frozenAssetId.has_value())
            {
                const auto asset = std::ranges::find(
                    auditionState_.frozenAssets,
                    *request.frozenAssetId,
                    &AnalysisAuditionPanelOption::id);
                if (asset != auditionState_.frozenAssets.end())
                {
                    auditionAssetBox_.setSelectedItemIndex(
                        static_cast<int>(std::distance(
                            auditionState_.frozenAssets.begin(), asset)),
                        juce::dontSendNotification);
                }
            }
        }
        if (auditionInputBox_.getSelectedId() == 0
            && !auditionState_.destinationInputs.empty())
        {
            auditionInputBox_.setSelectedItemIndex(
                0, juce::dontSendNotification);
        }
        if (auditionAssetBox_.getSelectedId() == 0
            && !auditionState_.frozenAssets.empty())
        {
            auditionAssetBox_.setSelectedItemIndex(
                0, juce::dontSendNotification);
        }
        auditionStatus_.setText(
            auditionState_.status.empty()
                ? (auditionState_.transportExternallyControlled
                      ? juce::String{
                            "Audition unavailable while external transport controls playback"}
                      : juce::String{
                            "Select one Audio trace to audition"})
                : juceString(auditionState_.status),
            juce::dontSendNotification);
        auditionStatus_.setTitle(
            "Analysis audition status: " + auditionStatus_.getText());
        auditionStatus_.setTooltip(auditionStatus_.getText());
        auditionStatus_.setColour(
            juce::Label::textColourId,
            auditionState_.active ? audioColour : dataColour);
        updateAuditionControlState();
        resized();
    }

    void setOutputTakeState(
        AnalysisOutputTakePanelState state) override
    {
        outputTakeState_ = std::move(state);
        const auto recorderAction = outputTakeState_.diskRecorder
            ? juce::String{"Record Disk Recorder..."}
            : juce::String{"Record Output Take"};
        outputTakeRecordButton_.setButtonText(recorderAction);
        outputTakeRecordButton_.setTitle(recorderAction);
        outputTakeRecordButton_.setTooltip(
            outputTakeState_.diskRecorder
                ? juce::String{
                    "Choose one explicit local WAV destination for the exact prepared mono Disk Recorder preview. The project filename is only a suggestion."}
                : juce::String{
                    "Choose one PCM16 WAV destination and record the exact prepared project output through a bounded callback queue."});
        outputTakeStatus_.setText(
            outputTakeState_.status.empty()
                ? juce::String{"Output take idle"}
                : juceString(outputTakeState_.status),
            juce::dontSendNotification);
        outputTakeStatus_.setTitle(
            "Output take status: " + outputTakeStatus_.getText());
        outputTakeStatus_.setTooltip(outputTakeStatus_.getText());
        outputTakeStatus_.setColour(
            juce::Label::textColourId,
            outputTakeState_.incomplete
                ? errorColour
                : (outputTakeState_.recording
                    ? outputColour
                    : dataColour));
        updateOutputTakeControlState();
        resized();
    }

    [[nodiscard]] bool preparedStatusContainsForSmoke(
        const juce::String& text) const override
    {
        return preparedStatus_.getText().contains(text)
            && preparedStatus_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool traceLayoutControlsUsableForSmoke() const override
    {
        return traceLanes_.size() >= 2
            && moveUpButton_.isVisible()
            && moveDownButton_.isVisible()
            && visibilityButton_.isVisible()
            && soloButton_.isVisible()
            && traceScaleLabel_.isVisible()
            && traceScaleSlider_.isVisible()
            && historyBox_.isVisible()
            && refreshRateBox_.isVisible()
            && !moveUpButton_.getBounds().isEmpty()
            && !moveDownButton_.getBounds().isEmpty()
            && !visibilityButton_.getBounds().isEmpty()
            && !soloButton_.getBounds().isEmpty()
            && !traceScaleLabel_.getBounds().isEmpty()
            && !traceScaleSlider_.getBounds().isEmpty()
            && !historyBox_.getBounds().isEmpty()
            && !refreshRateBox_.getBounds().isEmpty()
            && moveUpButton_.getTitle().isNotEmpty()
            && visibilityButton_.getTitle().isNotEmpty()
            && soloButton_.getTitle().isNotEmpty()
            && traceScaleSlider_.getTitle().isNotEmpty()
            && historyBox_.getTitle().isNotEmpty()
            && refreshRateBox_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool auditionControlsUsableForSmoke() const override
    {
        const auto panelBounds = getLocalBounds();
        const auto visibleAndContained = [&](const juce::Component& control) {
            return !control.isVisible()
                || (!control.getBounds().isEmpty()
                    && panelBounds.contains(control.getBounds()));
        };
        return visibleAndContained(auditionSourceBox_)
            && visibleAndContained(auditionInputBox_)
            && visibleAndContained(auditionAssetBox_)
            && visibleAndContained(auditionFrequencyBox_)
            && visibleAndContained(auditionStartButton_)
            && visibleAndContained(auditionStopButton_)
            && visibleAndContained(auditionCommitButton_)
            && visibleAndContained(auditionStatus_)
            && !auditionSourceBox_.getBounds().isEmpty()
            && !auditionStartButton_.getBounds().isEmpty()
            && !auditionStopButton_.getBounds().isEmpty()
            && !auditionCommitButton_.getBounds().isEmpty()
            && auditionStatus_.getText().isNotEmpty()
            && auditionStatus_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool outputTakeControlsUsableForSmoke() const override
    {
        const auto panelBounds = getLocalBounds();
        const auto usable = [&](const juce::Component& control) {
            return control.isVisible()
                && !control.getBounds().isEmpty()
                && panelBounds.contains(control.getBounds())
                && control.getTitle().isNotEmpty();
        };
        return usable(outputTakeRecordButton_)
            && usable(outputTakeStopButton_)
            && usable(outputTakeCancelButton_)
            && usable(outputTakeStatus_)
            && outputTakeStatus_.getText().isNotEmpty();
    }

    [[nodiscard]] bool outputTakeActionContainsForSmoke(
        const juce::String& text) const override
    {
        return outputTakeRecordButton_.getButtonText().contains(text)
            && outputTakeRecordButton_.getTitle().contains(text);
    }

    [[nodiscard]] bool traceKeyboardCursorUsableForSmoke() override
    {
        poll();
        if (!getWantsKeyboardFocus()
            || traceLanes_.empty()
            || visibleFrameCount_ == 0)
        {
            return false;
        }
        pinnedCursorX_.reset();
        hoverCursorX_.reset();
        hoverLevel_.reset();
        const auto movedToFirst = keyPressed(
            juce::KeyPress{juce::KeyPress::homeKey});
        const auto firstReadout = cursorReadout_.getText();
        const auto movedRight = keyPressed(
            juce::KeyPress{juce::KeyPress::rightKey});
        const auto movedFine = keyPressed(juce::KeyPress{
            juce::KeyPress::rightKey,
            juce::ModifierKeys::shiftModifier,
            0});
        const auto movedLeft = keyPressed(
            juce::KeyPress{juce::KeyPress::leftKey});
        const auto movedToLast = keyPressed(
            juce::KeyPress{juce::KeyPress::endKey});
        const auto lastReadout = cursorReadout_.getText();
        const auto cleared = keyPressed(
            juce::KeyPress{juce::KeyPress::escapeKey});
        return movedToFirst
            && movedRight
            && movedFine
            && movedLeft
            && movedToLast
            && cleared
            && firstReadout.isNotEmpty()
            && lastReadout.isNotEmpty()
            && !pinnedCursorX_.has_value()
            && cursorReadout_.getText().isEmpty();
    }

    [[nodiscard]] bool traceStatusContainsForSmoke(
        const juce::String& text) const override
    {
        return statusText().contains(text);
    }

    [[nodiscard]] std::optional<std::uint64_t>
    selectedTraceSequenceForSmoke() const noexcept override
    {
        const auto selected = selectedTraceLaneIndex();
        if (!selected.has_value())
            return std::nullopt;
        const auto& lane = traceLanes_[*selected];
        return lane.header.has_value()
            ? std::optional<std::uint64_t>{lane.header->sequence}
            : std::nullopt;
    }

    [[nodiscard]] talas::app::AnalysisMonitorSourceInput
    eventMonitorSnapshot() const override
    {
        talas::app::AnalysisMonitorSourceInput result;
        result.source = talas::app::AnalysisDiagnosticSource::EventMonitor;
        result.nextSequence = nextEventMonitorSequence_;
        result.records = eventMonitorRecords_;
        const auto eventLane = std::ranges::find_if(
            traceLanes_,
            [](const auto& lane) {
                return lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Event;
            });
        if (eventLane == traceLanes_.end())
        {
            result.evidence = talas::app::AnalysisEvidenceState::Unavailable;
            return result;
        }
        result.stageId = eventLane->observedStageId;
        result.bindToActiveStage = result.stageId != 0;
        result.evidence = eventLane->runtimeState
                == talas::app::AnalysisTraceRuntimeState::RejectedEvidence
            ? talas::app::AnalysisEvidenceState::RejectedEvidence
            : capture_ != nullptr && capture_->ready()
                ? talas::app::AnalysisEvidenceState::Available
                : talas::app::AnalysisEvidenceState::Unavailable;
        for (const auto& lane : traceLanes_)
        {
            if (lane.view.trace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Event)
            {
                result.droppedRecords =
                    talas::runtime::realtimeMetricAddSaturated(
                        result.droppedRecords,
                        lane.droppedBlocks);
            }
        }
        return result;
    }

    bool focusDiagnosticMonitor(
        const talas::app::AnalysisDiagnosticIdentity& identity,
        juce::String summary) override
    {
        const auto supported = identity.kind
                == talas::app::AnalysisNavigationKind::EventMonitor
            || identity.kind
                == talas::app::AnalysisNavigationKind::MidiMonitor
            || identity.kind
                == talas::app::AnalysisNavigationKind::OscIngressMonitor
            || identity.kind
                == talas::app::AnalysisNavigationKind::OscEgressMonitor;
        if (!supported)
            return false;
        focusedMonitorIdentity_ = identity;
        const auto kind = std::to_string(static_cast<int>(identity.kind));
        const auto authority = std::to_string(identity.authorityGeneration);
        const auto sequence = std::to_string(identity.monitorSequence);
        const auto stage = std::to_string(identity.stageId);
        const std::array focusFields{
            std::string_view{kind}, std::string_view{stage},
            std::string_view{authority}, std::string_view{sequence},
            std::string_view{identity.nodeId},
            std::string_view{identity.structuralSourcePath},
            std::string_view{identity.parameterId},
            std::string_view{identity.assetId},
            std::string_view{identity.traceId},
            std::string_view{identity.sourceId}};
        monitorFocus_.setComponentID(juceString(
            workbenchStableFocusItemId(
                "analysis.monitor", focusFields)));
        auto exact = std::move(summary);
        if (identity.monitorSequence != 0)
        {
            exact += juceString(" · sequence ")
                + juce::String(std::to_string(
                    identity.monitorSequence));
        }
        if (identity.stageId != 0)
        {
            exact += juceString(" · stage ")
                + juce::String(std::to_string(identity.stageId));
        }
        if (!identity.sourceId.empty())
            exact += juceString(" · ") + juceString(identity.sourceId);
        monitorFocus_.setText(exact, juce::dontSendNotification);
        monitorFocus_.setTitle(exact);
        monitorFocus_.setTooltip(exact);
        if (identity.kind
                == talas::app::AnalysisNavigationKind::EventMonitor
            && !identity.traceId.empty()
            && callbacks_.selectTrace)
        {
            callbacks_.selectTrace(identity.traceId);
        }
        resized();
        if (isShowing())
            monitorFocus_.grabKeyboardFocus();
        repaint();
        return true;
    }

    [[nodiscard]] bool monitorFocusContainsForSmoke(
        const juce::String& text) const override
    {
        return monitorFocus_.getText().contains(text);
    }

    void timerCallback() override
    {
        poll();
    }

    void poll() override
    {
        if (!playing_ || capture_ == nullptr)
        {
            return;
        }
        if (!traceLanes_.empty())
        {
            pollTraceLanes();
            return;
        }
        if (mode_ == Mode::None
            || selectedNodeId_.empty() || captureScratch_.empty())
        {
            return;
        }
        const auto probeIndex = capture_->preferredProbeIndex(selectedNodeId_);
        if (!probeIndex.has_value())
            return;
        const auto* spec = capture_->probeSpec(*probeIndex);
        if (spec == nullptr
            || (mode_ == Mode::Oscilloscope
                && spec->kind != talas::runtime::RealtimeAnalysisKind::Oscilloscope)
            || (mode_ == Mode::Spectrum
                && spec->kind != talas::runtime::RealtimeAnalysisKind::Spectrum))
        {
            return;
        }
        const auto copied = capture_->copyLatestRecord(
            *probeIndex,
            captureScratch_);
        if (!copied.has_value())
            return;
        droppedBlockCount_ = capture_->droppedBlockCount(*probeIndex);
        std::optional<talas::runtime::RealtimeAnalysisRecordHeader>
            separateTriggerRecord;
        std::optional<talas::runtime::RealtimeAnalysisRecordHeader>
            slowSweepRecord;
        const talas::runtime::RealtimeAnalysisProbeSpec* slowSweepSpec = nullptr;
        bool hasDirectTriggerProbe = false;
        bool hasSeparateTriggerProbe = false;
        bool triggerIsRight = false;
        if (mode_ == Mode::Oscilloscope)
        {
            const auto slowSweepProbe = capture_->probeIndex(
                selectedNodeId_, "@slow-sweep", spec->scopeId,
                talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock);
            if (slowSweepProbe.has_value())
            {
                slowSweepSpec = capture_->probeSpec(*slowSweepProbe);
                slowSweepRecord = capture_->copyLatestRecord(
                    *slowSweepProbe, slowSweepScratch_);
            }
            const auto directProbe = capture_->probeIndex(
                selectedNodeId_, "sweepOrTrigOut", spec->scopeId,
                talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock);
            hasDirectTriggerProbe = directProbe.has_value();
            const auto triggerProbe = capture_->preferredTriggerProbeIndex(
                *probeIndex);
            hasSeparateTriggerProbe = triggerProbe.has_value();
            triggerIsRight = !hasDirectTriggerProbe
                && triggerProbe.has_value();
            if (triggerProbe.has_value())
                separateTriggerRecord = capture_->copyLatestRecord(
                    *triggerProbe, separateTriggerScratch_);
            if (hasDirectTriggerProbe != defaultTriggerIsDirect_)
            {
                defaultTriggerIsDirect_ = hasDirectTriggerProbe;
                if (!triggerLevelEdited_)
                {
                    triggerLevelSlider_.setValue(
                        hasDirectTriggerProbe ? 0.5 : 0.0,
                        juce::dontSendNotification);
                }
            }
        }
        if (frozen_
            || !singleShotState_.acceptsDisplay(
                isSingleShotTrigger()))
        {
            repaint();
            return;
        }
        if (mode_ == Mode::Oscilloscope)
        {
            const auto slowSweepActive = slowSweepSpec != nullptr
                && selectedScopeTrigger()
                    == talas::audio::OscilloscopeTrigger::Free;
            if (slowSweepActive && !slowSweepRecord.has_value())
                return;
            const auto rawEnd = copied->absoluteSamplePosition
                + copied->valueCount;
            const auto slowStride = slowSweepActive
                ? slowSweepSpec->sampleStrideFrames : 1;
            const auto allowedSlowEnd = rawEnd / slowStride
                + (rawEnd % slowStride != 0);
            if (slowSweepActive
                && (slowSweepRecord->stageId != copied->stageId
                    || slowSweepRecord->bindingGeneration
                        != copied->bindingGeneration
                    || slowSweepRecord->absoluteSamplePosition
                            + slowSweepRecord->valueCount
                        > allowedSlowEnd))
                return;
            if (sweepStageId_ != copied->stageId
                || sweepBindingGeneration_
                    != copied->bindingGeneration)
            {
                sweepState_.reset();
                sweepStageId_ = copied->stageId;
                sweepBindingGeneration_ = copied->bindingGeneration;
            }
            // Info > Oscilloscope keeps to the captures that
            // hold a window starting at a multiple of 512 frames.
            if (!slowSweepActive && locksPowerOfTwoPeriods() && copied->valueCount >= selectedFrameCount()
                && !talas::audio::powerOfTwoLockedOscilloscopeWindow(
                    copied->absoluteSamplePosition, copied->valueCount, selectedFrameCount()))
                return;
            if (!slowSweepActive && selectedScopeTrigger()
                    == talas::audio::OscilloscopeTrigger::Free
                && !sweepState_.shouldUpdate(
                    copied->absoluteSamplePosition
                        + copied->valueCount,
                    sampleRate_,
                    sweepFrequencySlider_.getValue()))
            {
                return;
            }
        }
        const auto slowSweepActive = mode_ == Mode::Oscilloscope
            && slowSweepSpec != nullptr
            && selectedScopeTrigger()
                == talas::audio::OscilloscopeTrigger::Free;
        const auto& displayRecord = slowSweepActive
            ? *slowSweepRecord : *copied;
        separateTriggerConnected_ = hasSeparateTriggerProbe;
        separateTriggerIsRight_ = triggerIsRight;
        separateTriggerActive_ = separateTriggerRecord.has_value()
            && separateTriggerRecord->stageId == displayRecord.stageId
            && separateTriggerRecord->bindingGeneration
                == displayRecord.bindingGeneration
            && separateTriggerRecord->absoluteSamplePosition
                == displayRecord.absoluteSamplePosition
            && separateTriggerRecord->valueCount == displayRecord.valueCount;
        if (separateTriggerActive_)
            std::copy_n(separateTriggerScratch_.begin(),
                displayRecord.valueCount,
                visibleSeparateTriggerSamples_.begin());
        visibleSampleStrideFrames_ = slowSweepActive
            ? slowSweepSpec->sampleStrideFrames : 1;
        visibleInitialSweepFrequencyHz_ = slowSweepActive
            ? slowSweepSpec->initialSweepFrequencyHz : 30.0;
        visibleFrameCount_ = displayRecord.valueCount;
        visibleAbsoluteFirstFrame_ = displayRecord.absoluteSamplePosition;
        std::copy_n(
            slowSweepActive
                ? slowSweepScratch_.begin() : captureScratch_.begin(),
            visibleFrameCount_,
            visibleSamples_.begin());
        if (mode_ == Mode::Spectrum)
            rebuildSpectrum();
        else
            singleShotState_.observe(
                isSingleShotTrigger(),
                oscilloscopeWindow().triggered);
        updateControlState();
        refreshCursorReadout();
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);

        auto bounds = getLocalBounds().reduced(10);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::panelTitle());
        graphics.drawText(
            !traceLanes_.empty()
                ? juceString("Analysis · ")
                    + juce::String(
                        static_cast<int>(traceLanes_.size()))
                    + (traceLanes_.size() == 1
                        ? " pinned trace"
                        : " pinned traces")
                : (mode_ == Mode::None
                    ? juce::String{"Analysis"}
                    : juceString("Analysis · ")
                        + juceString(selectedNodeId_)),
            bounds.removeFromTop(analysisPanelHeaderHeight),
            juce::Justification::centredLeft);
        bounds.removeFromTop(controlRowsHeight());
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        const auto status = statusText();
        graphics.drawText(
            status,
            bounds.removeFromTop(analysisPanelCaptureStatusHeight),
            juce::Justification::centredLeft);
        bounds.removeFromTop(analysisPanelCaptureStatusGap);

        const auto plot = analysisPlotBounds();
        graphics.setColour(backgroundColour);
        graphics.fillRoundedRectangle(plot, 4.0F);
        graphics.setColour(panelBorderColour);
        graphics.drawRoundedRectangle(plot, 4.0F, 1.0F);
        if (!traceLanes_.empty())
        {
            drawTraceLanes(graphics, plot);
            return;
        }
        if (mode_ == Mode::None || visibleFrameCount_ == 0)
            return;
        if (mode_ == Mode::Oscilloscope)
            drawOscilloscope(graphics, plot);
        else
            drawSpectrum(graphics, plot);
        drawCursor(graphics, plot);
    }

    void resized() override
    {
        hoverCursorX_.reset();
        auto bounds = getLocalBounds().reduced(10);
        bounds.removeFromTop(analysisPanelHeaderHeight);
        if (sourceCaptureBox_.isVisible())
        {
            auto sourceRow = bounds.removeFromTop(24);
            sourceCaptureLabel_.setBounds(sourceRow.removeFromLeft(48));
            sourceCaptureBox_.setBounds(sourceRow);
            bounds.removeFromTop(4);
        }
        auto traceControls = bounds.removeFromTop(24);
        traceBox_.setBounds(traceControls.removeFromLeft(
            std::max(80, traceControls.getWidth() - 116)));
        traceControls.removeFromLeft(4);
        pinButton_.setBounds(traceControls.removeFromLeft(52));
        traceControls.removeFromLeft(4);
        removeButton_.setBounds(traceControls);
        bounds.removeFromTop(4);
        auto primaryControls = bounds.removeFromTop(24);
        freezeButton_.setBounds(primaryControls.removeFromLeft(62));
        primaryControls.removeFromLeft(4);
        if (!traceLanes_.empty())
        {
            moveUpButton_.setBounds(primaryControls.removeFromLeft(32));
            primaryControls.removeFromLeft(4);
            moveDownButton_.setBounds(primaryControls.removeFromLeft(42));
            primaryControls.removeFromLeft(4);
            visibilityButton_.setBounds(primaryControls.removeFromLeft(48));
            primaryControls.removeFromLeft(4);
            soloButton_.setBounds(primaryControls);
        }
        else
        {
            frameCountBox_.setBounds(primaryControls.removeFromLeft(50));
            primaryControls.removeFromLeft(4);
            zoomBox_.setBounds(primaryControls.removeFromLeft(50));
            primaryControls.removeFromLeft(4);
            const auto modeControl =
                primaryControls.removeFromLeft(76);
            resetButton_.setBounds(modeControl);
            triggerModeBox_.setBounds(modeControl);
        }

        bounds.removeFromTop(2);
        const auto secondaryRow = bounds.removeFromTop(24);
        if (!traceLanes_.empty())
        {
            auto scaleControls = secondaryRow;
            traceScaleLabel_.setBounds(
                scaleControls.removeFromLeft(40));
            scaleControls.removeFromLeft(4);
            traceScaleSlider_.setBounds(
                scaleControls.removeFromLeft(
                    std::max(70, scaleControls.getWidth() - 132)));
            scaleControls.removeFromLeft(4);
            historyBox_.setBounds(
                scaleControls.removeFromLeft(64));
            scaleControls.removeFromLeft(4);
            refreshRateBox_.setBounds(scaleControls);
        }
        auto spectrumControls = secondaryRow;
        windowBox_.setBounds(spectrumControls.removeFromLeft(58));
        spectrumControls.removeFromLeft(4);
        averageBox_.setBounds(spectrumControls.removeFromLeft(44));
        spectrumControls.removeFromLeft(4);
        peakHoldButton_.setBounds(
            spectrumControls.removeFromLeft(50));
        spectrumControls.removeFromLeft(4);
        frequencyScaleBox_.setBounds(
            spectrumControls.removeFromLeft(54));
        spectrumControls.removeFromLeft(4);
        spectrumPanSlider_.setBounds(spectrumControls);

        auto scopeControls = secondaryRow;
        triggerLevelSlider_.setBounds(
            scopeControls.removeFromLeft(132));
        scopeControls.removeFromLeft(4);
        rearmButton_.setBounds(
            scopeControls.removeFromLeft(64));
        scopeControls.removeFromLeft(4);
        sweepFrequencySlider_.setBounds(scopeControls);
        bounds.removeFromTop(4);
        auto auditionSettingsRow = bounds.removeFromTop(24);
        auditionSourceBox_.setBounds(
            auditionSettingsRow.removeFromLeft(
                std::min(92, auditionSettingsRow.getWidth())));
        auditionSettingsRow.removeFromLeft(
            std::min(4, auditionSettingsRow.getWidth()));
        auditionInputBox_.setBounds({});
        auditionFrequencyBox_.setBounds({});
        auditionAssetBox_.setBounds({});
        if (auditionInputBox_.isVisible())
        {
            if (auditionFrequencyBox_.isVisible())
            {
                const auto frequencyWidth = std::min(
                    64, auditionSettingsRow.getWidth());
                auto destination = auditionSettingsRow;
                destination.removeFromRight(
                    std::min(4 + frequencyWidth, destination.getWidth()));
                auditionInputBox_.setBounds(destination);
                auditionFrequencyBox_.setBounds(
                    auditionSettingsRow.removeFromRight(frequencyWidth));
            }
            else if (auditionAssetBox_.isVisible())
            {
                const auto available = auditionSettingsRow.getWidth();
                const auto destinationWidth = std::max(
                    0, (available - 4) / 2);
                auditionInputBox_.setBounds(
                    auditionSettingsRow.removeFromLeft(destinationWidth));
                auditionSettingsRow.removeFromLeft(
                    std::min(4, auditionSettingsRow.getWidth()));
                auditionAssetBox_.setBounds(auditionSettingsRow);
            }
            else
            {
                auditionInputBox_.setBounds(auditionSettingsRow);
            }
        }
        bounds.removeFromTop(4);
        auto auditionActionsRow = bounds.removeFromTop(24);
        const auto actionWidth = std::max(
            0, (auditionActionsRow.getWidth() - 8) / 3);
        auditionStartButton_.setBounds(
            auditionActionsRow.removeFromLeft(actionWidth));
        auditionActionsRow.removeFromLeft(
            std::min(4, auditionActionsRow.getWidth()));
        auditionStopButton_.setBounds(
            auditionActionsRow.removeFromLeft(actionWidth));
        auditionActionsRow.removeFromLeft(
            std::min(4, auditionActionsRow.getWidth()));
        auditionCommitButton_.setBounds(auditionActionsRow);
        bounds.removeFromTop(2);
        auditionStatus_.setBounds(bounds.removeFromTop(20));
        bounds.removeFromTop(4);
        auto outputTakeActionsRow = bounds.removeFromTop(24);
        const auto recordWidth = std::max(
            0, (outputTakeActionsRow.getWidth() - 8) / 2);
        outputTakeRecordButton_.setBounds(
            outputTakeActionsRow.removeFromLeft(recordWidth));
        outputTakeActionsRow.removeFromLeft(
            std::min(4, outputTakeActionsRow.getWidth()));
        const auto terminalWidth = std::max(
            0, (outputTakeActionsRow.getWidth() - 4) / 2);
        outputTakeStopButton_.setBounds(
            outputTakeActionsRow.removeFromLeft(terminalWidth));
        outputTakeActionsRow.removeFromLeft(
            std::min(4, outputTakeActionsRow.getWidth()));
        outputTakeCancelButton_.setBounds(outputTakeActionsRow);
        bounds.removeFromTop(2);
        outputTakeStatus_.setBounds(bounds.removeFromTop(20));
        auto preparedBounds = getLocalBounds().reduced(10);
        preparedBounds.removeFromTop(analysisPanelHeaderHeight);
        preparedBounds.removeFromTop(controlRowsHeight());
        preparedBounds.removeFromTop(analysisPanelCaptureStatusHeight);
        preparedBounds.removeFromTop(analysisPanelCaptureStatusGap);
        preparedStatus_.setBounds(
            preparedBounds.removeFromTop(analysisPanelEvidenceRowHeight));
        monitorFocus_.setBounds(
            preparedBounds.removeFromTop(analysisPanelEvidenceRowHeight));
        cursorReadout_.setBounds(
            analysisPlotBounds()
                .toNearestInt()
                .removeFromTop(18)
                .reduced(4, 0));
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        const auto localEvent = event.getEventRelativeTo(this);
        const auto plot = analysisPlotBounds();
        if (displayMode() != Mode::None
            && visibleFrameCount_ > 0
            && plot.contains(localEvent.position))
        {
            if (!pinnedCursorX_.has_value())
            {
                hoverCursorX_ =
                    normalizedCursorX(localEvent.position.x);
            }
            // The amplitude at the cursor's height too.
            hoverLevel_ = displayMode() == Mode::Oscilloscope
                ? std::optional<double>{talas::audio::oscilloscopeAmplitudeAtHeight(
                      static_cast<double>(plot.getCentreY() - localEvent.position.y),
                      static_cast<double>(plot.getHeight()) * 0.46,
                      selectedZoom())}
                : std::nullopt;
            setMouseCursor(
                juce::MouseCursor::CrosshairCursor);
        }
        else
        {
            hoverCursorX_.reset();
            hoverLevel_.reset();
            setMouseCursor(
                juce::MouseCursor::NormalCursor);
        }
        refreshCursorReadout();
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hoverCursorX_.reset();
        hoverLevel_.reset();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        refreshCursorReadout();
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto localEvent = event.getEventRelativeTo(this);
        const auto plot = analysisPlotBounds();
        if (!traceLanes_.empty()
            && plot.contains(localEvent.position))
        {
            const auto displayedCount = displayedTraceCount();
            if (displayedCount != 0)
            {
                const auto laneHeight = plot.getHeight()
                    / static_cast<float>(displayedCount);
                const auto requestedIndex = std::min<std::size_t>(
                    static_cast<std::size_t>(std::max(
                        0.0F,
                        (localEvent.position.y - plot.getY())
                            / laneHeight)),
                    displayedCount - 1);
                std::size_t displayedIndex = 0;
                for (const auto& lane : traceLanes_)
                {
                    if (!displaysTrace(lane))
                        continue;
                    if (displayedIndex++ == requestedIndex)
                    {
                        if (callbacks_.selectTrace)
                        {
                            callbacks_.selectTrace(
                                lane.view.trace.traceId);
                        }
                        break;
                    }
                }
            }
        }
        if (displayMode() != Mode::None
            && visibleFrameCount_ > 0
            && plot.contains(localEvent.position))
        {
            pinnedCursorX_ =
                normalizedCursorX(localEvent.position.x);
            hoverCursorX_.reset();
            draggingCursor_ = true;
            grabKeyboardFocus();
            setMouseCursor(
                juce::MouseCursor::CrosshairCursor);
        }
        else
        {
            pinnedCursorX_.reset();
            draggingCursor_ = false;
        }
        refreshCursorReadout();
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!draggingCursor_)
            return;
        const auto localEvent = event.getEventRelativeTo(this);
        pinnedCursorX_ =
            normalizedCursorX(localEvent.position.x);
        refreshCursorReadout();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        draggingCursor_ = false;
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (displayMode() == Mode::None || visibleFrameCount_ == 0)
            return false;

        std::optional<talas::audio::AnalyzerCursorMove> move;
        if (key.getKeyCode() == juce::KeyPress::leftKey)
            move = talas::audio::AnalyzerCursorMove::Previous;
        else if (key.getKeyCode() == juce::KeyPress::rightKey)
            move = talas::audio::AnalyzerCursorMove::Next;
        else if (key.getKeyCode() == juce::KeyPress::homeKey)
            move = talas::audio::AnalyzerCursorMove::First;
        else if (key.getKeyCode() == juce::KeyPress::endKey)
            move = talas::audio::AnalyzerCursorMove::Last;
        else if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            pinnedCursorX_.reset();
            hoverCursorX_.reset();
            refreshCursorReadout(true);
            repaint();
            return true;
        }
        else if (key.getKeyCode() == juce::KeyPress::returnKey)
        {
            if (pinnedCursorX_.has_value())
                pinnedCursorX_.reset();
            else
                pinnedCursorX_ = hoverCursorX_.value_or(0.5);
            refreshCursorReadout(true);
            repaint();
            return true;
        }
        if (!move.has_value())
            return false;

        const auto current = pinnedCursorX_.value_or(
            hoverCursorX_.value_or(0.5));
        pinnedCursorX_ = talas::audio::moveNormalizedAnalyzerCursor(
            current,
            *move,
            key.getModifiers().isShiftDown());
        hoverCursorX_.reset();
        refreshCursorReadout(true);
        repaint();
        return true;
    }

private:
    enum class Mode
    {
        None,
        Oscilloscope,
        Spectrum,
    };

    struct TraceLane
    {
        talas::app::AnalysisTraceViewModel view;
        talas::app::AnalysisTraceRuntimeState runtimeState =
            talas::app::AnalysisTraceRuntimeState::TargetUnavailable;
        std::optional<std::size_t> probeIndex;
        std::optional<talas::runtime::RealtimeAnalysisRecordHeader> header;
        std::vector<double> scratch;
        std::vector<double> samples;
        std::vector<std::uint64_t> scratchSamplePositions;
        std::vector<std::uint64_t> samplePositions;
        std::size_t valueCount = 0;
        std::uint64_t droppedBlocks = 0;
        std::uint64_t rejectedRecords = 0;
        std::uint64_t projectionRejectedRecords = 0;
        std::uint64_t observedStageId = 0;
        std::uint64_t observedBindingGeneration = 0;
    };

    static void clearTraceEvidence(TraceLane& lane) noexcept
    {
        lane.header.reset();
        lane.valueCount = 0;
    }

    void refreshTraceRuntimeState(
        TraceLane& lane,
        bool acceptedRecord = false,
        bool rejectedEvidence = false)
    {
        const auto captureReady = capture_ != nullptr && capture_->ready();
        const auto bindingReady = captureReady
            && capture_->boundStageId() != 0
            && capture_->bindingGeneration() != 0;
        lane.runtimeState = talas::app::analysisTraceRuntimeState({
            lane.view.availability,
            captureReady,
            lane.probeIndex.has_value(),
            bindingReady,
            acceptedRecord,
            lane.header.has_value(),
            rejectedEvidence,
        });
    }

    [[nodiscard]] const talas::app::AnalysisTraceViewModel*
    selectedTraceView() const noexcept
    {
        const auto index = traceBox_.getSelectedItemIndex();
        return index >= 0
                && static_cast<std::size_t>(index)
                    < projection_.traces.size()
            ? &projection_.traces[static_cast<std::size_t>(index)]
            : nullptr;
    }

    void moveSelectedTrace(int direction)
    {
        const auto* trace = selectedTraceView();
        if (trace != nullptr && callbacks_.moveTrace)
            callbacks_.moveTrace(trace->trace.traceId, direction);
    }

    void updateSelectedTraceControls()
    {
        const auto* trace = selectedTraceView();
        const auto hasTrace = trace != nullptr;
        visibilityButton_.setToggleState(
            hasTrace && trace->trace.visible,
            juce::dontSendNotification);
        soloButton_.setToggleState(
            hasTrace && soloTraceId_
                == std::optional<std::string>{trace->trace.traceId},
            juce::dontSendNotification);
        traceScaleSlider_.setValue(
            hasTrace ? trace->trace.verticalScale : 1.0,
            juce::dontSendNotification);
    }

    [[nodiscard]] bool displaysTrace(const TraceLane& lane) const noexcept
    {
        return soloTraceId_.has_value()
            ? lane.view.trace.traceId == *soloTraceId_
            : lane.view.trace.visible;
    }

    [[nodiscard]] std::size_t displayedTraceCount() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            traceLanes_,
            [&](const auto& lane) { return displaysTrace(lane); }));
    }

    void rebuildTraceLanes()
    {
        std::vector<TraceLane> next;
        next.reserve(projection_.traces.size());
        for (const auto& trace : projection_.traces)
        {
            TraceLane lane;
            const auto existing = std::ranges::find_if(
                traceLanes_,
                [&](const auto& candidate) {
                    return candidate.view.trace.traceId
                        == trace.trace.traceId;
                });
            if (existing != traceLanes_.end())
                lane = std::move(*existing);
            lane.view = trace;
            if (capture_ != nullptr && capture_->ready())
            {
                if (lane.scratch.size()
                    != capture_->maxPayloadValues())
                {
                    lane.scratch.assign(
                        capture_->maxPayloadValues(),
                        0.0);
                    lane.samples.assign(
                        capture_->maxPayloadValues(),
                        0.0);
                    lane.scratchSamplePositions.assign(
                        capture_->maxPayloadValues(),
                        0);
                    lane.samplePositions.assign(
                        capture_->maxPayloadValues(),
                        0);
                    lane.header.reset();
                    lane.valueCount = 0;
                }
                if (trace.available())
                {
                    const auto nextProbeIndex =
                        capture_->probeIndexForClientId(
                            trace.trace.traceId);
                    if (lane.probeIndex != nextProbeIndex)
                    {
                        clearTraceEvidence(lane);
                    }
                    lane.probeIndex = nextProbeIndex;
                }
                else
                {
                    lane.probeIndex.reset();
                    clearTraceEvidence(lane);
                }
                const auto stageId = capture_->boundStageId();
                const auto bindingGeneration =
                    capture_->bindingGeneration();
                if (lane.observedStageId != stageId
                    || lane.observedBindingGeneration
                        != bindingGeneration)
                {
                    clearTraceEvidence(lane);
                    lane.observedStageId = stageId;
                    lane.observedBindingGeneration = bindingGeneration;
                }
            }
            else
            {
                lane.probeIndex.reset();
                clearTraceEvidence(lane);
                lane.observedStageId = 0;
                lane.observedBindingGeneration = 0;
            }
            refreshTraceRuntimeState(lane);
            next.push_back(std::move(lane));
        }
        traceLanes_ = std::move(next);
        trimSequentialHistory();
    }

    void trimSequentialHistory()
    {
        for (auto& lane : traceLanes_)
        {
            if ((lane.view.trace.kind
                    != talas::project::WorkspaceAnalysisTraceKind::Audio
                 && lane.view.trace.kind
                    != talas::project::WorkspaceAnalysisTraceKind::Control)
                || lane.valueCount <= historyFrames_)
            {
                continue;
            }
            const auto first = lane.valueCount - historyFrames_;
            std::copy_n(
                lane.samples.begin()
                    + static_cast<std::ptrdiff_t>(first),
                historyFrames_,
                lane.samples.begin());
            std::copy_n(
                lane.samplePositions.begin()
                    + static_cast<std::ptrdiff_t>(first),
                historyFrames_,
                lane.samplePositions.begin());
            lane.valueCount = historyFrames_;
        }
    }

    [[nodiscard]] bool appendSequentialHistory(
        TraceLane& lane,
        std::size_t valueCount,
        std::uint64_t absoluteSamplePosition)
    {
        if (valueCount > lane.scratchSamplePositions.size()
            || (valueCount != 0
                && valueCount - 1
                    > std::numeric_limits<std::uint64_t>::max()
                        - absoluteSamplePosition))
        {
            return false;
        }
        for (std::size_t frame = 0; frame < valueCount; ++frame)
        {
            lane.scratchSamplePositions[frame] =
                absoluteSamplePosition + frame;
        }
        const auto nextValueCount =
            talas::app::appendAnalysisSequentialHistory(
            lane.samples,
            lane.valueCount,
            std::span<const double>{
                lane.scratch.data(),
                valueCount},
            historyFrames_);
        const auto capacity = std::min<std::size_t>(
            historyFrames_,
            lane.samplePositions.size());
        const auto storedValueCount = std::min(
            lane.valueCount,
            lane.samplePositions.size());
        if (valueCount >= capacity)
        {
            std::copy_n(
                lane.scratchSamplePositions.begin()
                    + static_cast<std::ptrdiff_t>(valueCount - capacity),
                capacity,
                lane.samplePositions.begin());
        }
        else
        {
            const auto retained = std::min(
                storedValueCount,
                capacity - valueCount);
            if (retained != 0)
            {
                std::copy_n(
                    lane.samplePositions.begin()
                        + static_cast<std::ptrdiff_t>(
                            storedValueCount - retained),
                    retained,
                    lane.samplePositions.begin());
            }
            std::copy_n(
                lane.scratchSamplePositions.begin(),
                valueCount,
                lane.samplePositions.begin()
                    + static_cast<std::ptrdiff_t>(retained));
        }
        lane.valueCount = nextValueCount;
        return true;
    }

    [[nodiscard]] std::optional<std::size_t>
    selectedTraceLaneIndex() const noexcept
    {
        if (projection_.selectedTraceIndex.has_value()
            && *projection_.selectedTraceIndex < traceLanes_.size())
        {
            return *projection_.selectedTraceIndex;
        }
        return traceLanes_.empty()
            ? std::optional<std::size_t>{}
            : std::optional<std::size_t>{0};
    }

    void updateSelectedTraceView()
    {
        if (traceLanes_.empty())
            return;
        const auto selected = selectedTraceLaneIndex();
        if (!selected.has_value())
            return;
        const auto& lane = traceLanes_[*selected];
        visibleFrameCount_ = lane.valueCount;
        if (visibleSamples_.size() < lane.samples.size())
            visibleSamples_.resize(lane.samples.size());
        if (lane.valueCount != 0)
        {
            std::copy_n(
                lane.samples.begin(),
                lane.valueCount,
                visibleSamples_.begin());
        }
        droppedBlockCount_ = lane.droppedBlocks;
    }

    void pollTraceLanes()
    {
        bool changed = false;
        for (auto& lane : traceLanes_)
        {
            const auto priorRuntimeState = lane.runtimeState;
            const auto priorSequence = lane.header.has_value()
                ? lane.header->sequence : 0;
            const auto priorDropped = lane.droppedBlocks;
            const auto priorRejected = lane.rejectedRecords;
            if (!lane.view.available()
                || capture_ == nullptr
                || !capture_->ready()
                || !lane.probeIndex.has_value()
                || lane.scratch.empty())
            {
                refreshTraceRuntimeState(lane);
                changed = changed
                    || lane.runtimeState != priorRuntimeState;
                continue;
            }
            const auto stageId = capture_->boundStageId();
            const auto bindingGeneration =
                capture_->bindingGeneration();
            if (lane.observedStageId != stageId
                || lane.observedBindingGeneration != bindingGeneration)
            {
                clearTraceEvidence(lane);
                lane.observedStageId = stageId;
                lane.observedBindingGeneration = bindingGeneration;
                changed = true;
            }
            lane.droppedBlocks = capture_->droppedBlockCount(
                *lane.probeIndex);
            const auto copyResult = capture_->copyLatestRecordResult(
                *lane.probeIndex,
                lane.scratch);
            const auto captureRejectedAfter =
                capture_->rejectedRecordCount(*lane.probeIndex);
            const auto projectionRejected =
                copyResult.status
                        == talas::runtime::RealtimeAnalysisCopyStatus::DestinationTooSmall
                    || copyResult.status
                        == talas::runtime::RealtimeAnalysisCopyStatus::InvalidProbe;
            if (projectionRejected)
            {
                lane.projectionRejectedRecords =
                    talas::runtime::realtimeMetricAddSaturated(
                        lane.projectionRejectedRecords,
                        1);
            }
            lane.rejectedRecords =
                talas::runtime::realtimeMetricAddSaturated(
                    captureRejectedAfter,
                    lane.projectionRejectedRecords);
            const auto rejectedEvidence =
                copyResult.status
                    == talas::runtime::RealtimeAnalysisCopyStatus::RejectedEvidence
                || projectionRejected;
            if (copyResult.status
                != talas::runtime::RealtimeAnalysisCopyStatus::Accepted)
            {
                if (rejectedEvidence)
                    clearTraceEvidence(lane);
                refreshTraceRuntimeState(
                    lane,
                    false,
                    rejectedEvidence);
                changed = changed
                    || rejectedEvidence
                    || lane.runtimeState != priorRuntimeState
                    || lane.droppedBlocks != priorDropped
                    || lane.rejectedRecords != priorRejected;
                continue;
            }
            const auto& record = copyResult.header;
            const auto expectedPayload = [&] {
                switch (lane.view.trace.kind)
                {
                case talas::project::WorkspaceAnalysisTraceKind::Audio:
                    return talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock;
                case talas::project::WorkspaceAnalysisTraceKind::Control:
                    return talas::runtime::RealtimeAnalysisPayloadKind::ControlBlock;
                case talas::project::WorkspaceAnalysisTraceKind::Event:
                    return talas::runtime::RealtimeAnalysisPayloadKind::EventBatch;
                case talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum:
                    return talas::runtime::RealtimeAnalysisPayloadKind::PartialSpectrum;
                case talas::project::WorkspaceAnalysisTraceKind::ComplexFrame:
                    return talas::runtime::RealtimeAnalysisPayloadKind::ComplexFrame;
                }
                return talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock;
            }();
            if (record.payloadKind != expectedPayload
                || record.channelCount != 1
                || record.valueCount > lane.samples.size())
            {
                lane.projectionRejectedRecords =
                    talas::runtime::realtimeMetricAddSaturated(
                        lane.projectionRejectedRecords,
                        1);
                lane.rejectedRecords =
                    talas::runtime::realtimeMetricAddSaturated(
                        capture_->rejectedRecordCount(
                            *lane.probeIndex),
                        lane.projectionRejectedRecords);
                clearTraceEvidence(lane);
                refreshTraceRuntimeState(lane, false, true);
                changed = true;
                continue;
            }
            if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Event
                && record.sequence != priorSequence)
            {
                appendEventMonitor(lane, record);
            }
            if (frozen_)
            {
                refreshTraceRuntimeState(lane);
                changed = changed
                    || lane.runtimeState != priorRuntimeState
                    || lane.droppedBlocks != priorDropped
                    || lane.rejectedRecords != priorRejected;
                continue;
            }
            if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Audio
                || lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Control)
            {
                if (!appendSequentialHistory(
                    lane,
                    record.valueCount,
                    record.absoluteSamplePosition))
                {
                    lane.projectionRejectedRecords =
                        talas::runtime::realtimeMetricAddSaturated(
                            lane.projectionRejectedRecords,
                            1);
                    lane.rejectedRecords =
                        talas::runtime::realtimeMetricAddSaturated(
                            capture_->rejectedRecordCount(
                                *lane.probeIndex),
                            lane.projectionRejectedRecords);
                    clearTraceEvidence(lane);
                    refreshTraceRuntimeState(lane, false, true);
                    changed = true;
                    continue;
                }
            }
            else
            {
                lane.valueCount = record.valueCount;
                std::copy_n(
                    lane.scratch.begin(),
                    lane.valueCount,
                    lane.samples.begin());
            }
            lane.header = record;
            refreshTraceRuntimeState(lane, true, false);
            changed = true;
        }
        if (changed)
        {
            updateSelectedTraceView();
            updateControlState();
            refreshCursorReadout();
        }
        repaint();
    }

    void appendEventMonitor(
        const TraceLane& lane,
        const talas::runtime::RealtimeAnalysisRecordHeader& record)
    {
        if (record.valueCount % 3 != 0
            || nextEventMonitorSequence_
                == std::numeric_limits<std::uint64_t>::max())
        {
            return;
        }
        constexpr std::size_t capacity =
            talas::app::maximumAnalysisMonitorRecordsPerSource;
        for (std::size_t value = 0;
             value < record.valueCount;
             value += 3)
        {
            if (nextEventMonitorSequence_
                == std::numeric_limits<std::uint64_t>::max())
            {
                break;
            }
            if (eventMonitorRecords_.size() == capacity)
                eventMonitorRecords_.erase(eventMonitorRecords_.begin());
            talas::app::AnalysisMonitorRecordInput monitor;
            monitor.sequence = nextEventMonitorSequence_++;
            monitor.stageId = record.stageId;
            monitor.label = lane.view.trace.traceId;
            monitor.detail = "event "
                + std::to_string(static_cast<std::uint64_t>(
                    lane.scratch[value]))
                + " · value " + std::to_string(lane.scratch[value + 1])
                + " · callback frame "
                + std::to_string(static_cast<std::uint64_t>(
                    lane.scratch[value + 2]));
            monitor.identity.kind =
                talas::app::AnalysisNavigationKind::EventMonitor;
            monitor.identity.stageId = record.stageId;
            monitor.identity.authorityGeneration =
                record.bindingGeneration;
            monitor.identity.monitorSequence = monitor.sequence;
            monitor.identity.nodeId = lane.view.trace.nodeId;
            monitor.identity.traceId = lane.view.trace.traceId;
            eventMonitorRecords_.push_back(std::move(monitor));
        }
    }

    [[nodiscard]] static juce::String availabilityText(
        talas::app::AnalysisTraceAvailability availability)
    {
        switch (availability)
        {
        case talas::app::AnalysisTraceAvailability::Available:
            return "Waiting for prepared capture";
        case talas::app::AnalysisTraceAvailability::MissingInstance:
            return "Instance unavailable";
        case talas::app::AnalysisTraceAvailability::MissingDefinition:
            return "Sound definition unavailable";
        case talas::app::AnalysisTraceAvailability::MissingNode:
            return "Node unavailable";
        case talas::app::AnalysisTraceAvailability::UnsupportedNodeType:
            return "Node type has no prepared analysis contract";
        case talas::app::AnalysisTraceAvailability::MissingPort:
            return "Prepared port unavailable";
        case talas::app::AnalysisTraceAvailability::IncompatiblePortKind:
            return "Prepared port kind changed";
        }
        return "Unavailable";
    }

    [[nodiscard]] static juce::String runtimeStateText(
        const TraceLane& lane)
    {
        switch (lane.runtimeState)
        {
        case talas::app::AnalysisTraceRuntimeState::TargetUnavailable:
            return availabilityText(lane.view.availability);
        case talas::app::AnalysisTraceRuntimeState::CaptureUnavailable:
            return "Prepared capture unavailable";
        case talas::app::AnalysisTraceRuntimeState::PreparedProbeUnavailable:
            return "Exact prepared probe unavailable";
        case talas::app::AnalysisTraceRuntimeState::WaitingForCurrentStage:
            return "Waiting for current-stage capture";
        case talas::app::AnalysisTraceRuntimeState::Live:
            return "Live";
        case talas::app::AnalysisTraceRuntimeState::RetainedNoNewData:
            return juceString(
                "No new data · retained last accepted record");
        case talas::app::AnalysisTraceRuntimeState::RejectedEvidence:
            return "Capture evidence rejected";
        }
        return "Unavailable";
    }

    [[nodiscard]] static bool displaysAcceptedEvidence(
        const TraceLane& lane) noexcept
    {
        return lane.runtimeState
                == talas::app::AnalysisTraceRuntimeState::Live
            || lane.runtimeState
                == talas::app::AnalysisTraceRuntimeState::RetainedNoNewData;
    }

    [[nodiscard]] static juce::Colour traceColour(
        talas::project::WorkspaceAnalysisTraceKind kind,
        std::uint8_t slot)
    {
        const auto semantic = [&] {
            switch (kind)
            {
            case talas::project::WorkspaceAnalysisTraceKind::Audio:
                return audioColour;
            case talas::project::WorkspaceAnalysisTraceKind::Control:
                return controlColour;
            case talas::project::WorkspaceAnalysisTraceKind::Event:
                return eventColour;
            case talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum:
            case talas::project::WorkspaceAnalysisTraceKind::ComplexFrame:
                return dataColour;
            }
            return dataColour;
        }();
        return semantic.interpolatedWith(
            textColour,
            static_cast<float>(slot) * 0.025F);
    }

    void drawTraceLanes(
        juce::Graphics& graphics,
        juce::Rectangle<float> plot) const
    {
        if (traceLanes_.empty() || plot.isEmpty())
            return;
        const auto displayedCount = displayedTraceCount();
        if (displayedCount == 0)
        {
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::label());
            graphics.drawFittedText(
                juceString(
                    "All traces are hidden · enable Show on a selected trace"),
                plot.toNearestInt().reduced(8),
                juce::Justification::centred,
                2);
            return;
        }
        const auto selected = selectedTraceLaneIndex();
        const auto laneHeight = plot.getHeight()
            / static_cast<float>(displayedCount);
        const auto activeCursorX = pinnedCursorX_.has_value()
            ? pinnedCursorX_
            : hoverCursorX_;
        std::size_t displayedIndex = 0;
        for (std::size_t index = 0;
             index < traceLanes_.size();
             ++index)
        {
            const auto& lane = traceLanes_[index];
            if (!displaysTrace(lane))
                continue;
            auto laneBounds = juce::Rectangle<float>{
                plot.getX(),
                plot.getY()
                    + laneHeight * static_cast<float>(displayedIndex),
                plot.getWidth(),
                laneHeight};
            ++displayedIndex;
            if (selected == std::optional<std::size_t>{index})
            {
                graphics.setColour(selectionColour.withAlpha(0.10F));
                graphics.fillRect(laneBounds);
            }
            graphics.setColour(panelBorderColour.withAlpha(0.65F));
            graphics.drawHorizontalLine(
                static_cast<int>(std::round(laneBounds.getBottom())),
                laneBounds.getX(),
                laneBounds.getRight());
            auto labelBounds = laneBounds.removeFromLeft(
                std::min(108.0F, laneBounds.getWidth() * 0.34F));
            graphics.setColour(
                lane.view.available()
                    ? textColour
                    : errorColour);
            graphics.setFont(WorkbenchType::micro());
            graphics.drawFittedText(
                "[" + juceString(talas::app::analysisTraceKindCode(
                            lane.view.trace.kind))
                    + "] " + juceString(lane.view.trace.nodeId)
                    + (lane.view.trace.portId.has_value()
                        ? juceString(" · ")
                            + juceString(*lane.view.trace.portId)
                        : lane.view.trace.kind
                                == talas::project::WorkspaceAnalysisTraceKind::Event
                            ? juceString(" · typed events")
                            : juce::String{}),
                labelBounds.reduced(4, 1).toNearestInt(),
                juce::Justification::centredLeft,
                1);
            const auto wave = laneBounds.reduced(4.0F, 2.0F);
            if (!displaysAcceptedEvidence(lane)
                || !lane.header.has_value()
                || (lane.valueCount == 0
                    && lane.view.trace.kind
                        != talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum
                    && lane.view.trace.kind
                        != talas::project::WorkspaceAnalysisTraceKind::Event))
            {
                graphics.setColour(secondaryTextColour);
                graphics.drawFittedText(
                    runtimeStateText(lane),
                    wave.toNearestInt(),
                    juce::Justification::centred,
                    1);
                continue;
            }
            if (selected == std::optional<std::size_t>{index}
                && activeCursorX.has_value())
            {
                const auto x = wave.getX()
                    + wave.getWidth()
                        * static_cast<float>(*activeCursorX);
                graphics.setColour(selectionColour.withAlpha(0.8F));
                graphics.drawVerticalLine(
                    static_cast<int>(std::round(x)),
                    wave.getY(),
                    wave.getBottom());
            }
            if (lane.runtimeState
                == talas::app::AnalysisTraceRuntimeState::RetainedNoNewData)
            {
                graphics.setColour(secondaryTextColour);
                graphics.setFont(WorkbenchType::micro());
                auto retainedBadge = wave.withHeight(12.0F);
                retainedBadge = retainedBadge.removeFromRight(48.0F);
                graphics.drawFittedText(
                    "retained",
                    retainedBadge.toNearestInt(),
                    juce::Justification::centredRight,
                    1);
            }
            if (lane.view.trace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Event)
            {
                if (lane.valueCount == 0)
                {
                    graphics.setColour(secondaryTextColour);
                    graphics.drawFittedText(
                        "No events in newest batch",
                        wave.toNearestInt(),
                        juce::Justification::centred,
                        1);
                    continue;
                }
                if (lane.valueCount % 3 != 0)
                    continue;
                double maximumFrameOffset = 1.0;
                for (std::size_t value = 0;
                     value < lane.valueCount;
                     value += 3)
                {
                    maximumFrameOffset = std::max(
                        maximumFrameOffset,
                        lane.samples[value + 2]);
                }
                graphics.setColour(traceColour(
                    lane.view.trace.kind,
                    lane.view.trace.colourSlot));
                for (std::size_t value = 0;
                     value < lane.valueCount;
                     value += 3)
                {
                    const auto x = wave.getX()
                        + wave.getWidth() * static_cast<float>(std::clamp(
                            lane.samples[value + 2]
                                / maximumFrameOffset,
                            0.0,
                            1.0));
                    const auto normalizedValue = std::clamp(
                        0.5 + lane.samples[value + 1]
                            * lane.view.trace.verticalScale * 0.5,
                        0.0,
                        1.0);
                    const auto y = wave.getBottom()
                        - static_cast<float>(normalizedValue)
                            * wave.getHeight();
                    graphics.drawVerticalLine(
                        static_cast<int>(std::round(x)),
                        y,
                        wave.getBottom());
                    graphics.fillEllipse(
                        x - 2.0F,
                        y - 2.0F,
                        4.0F,
                        4.0F);
                }
                continue;
            }
            if (lane.view.trace.kind
                == talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum)
            {
                if (lane.valueCount == 0)
                {
                    graphics.setColour(secondaryTextColour);
                    graphics.drawFittedText(
                        "No partials in newest frame",
                        wave.toNearestInt(),
                        juce::Justification::centred,
                        1);
                    continue;
                }
                if (lane.valueCount % 3 != 0 || sampleRate_ <= 0.0)
                    continue;
                graphics.setColour(traceColour(
                    lane.view.trace.kind,
                    lane.view.trace.colourSlot));
                const auto nyquist = sampleRate_ * 0.5;
                for (std::size_t value = 0;
                     value < lane.valueCount;
                     value += 3)
                {
                    const auto x = wave.getX()
                        + wave.getWidth() * static_cast<float>(std::clamp(
                            lane.samples[value] / nyquist, 0.0, 1.0));
                    const auto magnitude = std::clamp(
                        std::abs(lane.samples[value + 1])
                            * lane.view.trace.verticalScale,
                        0.0,
                        1.0);
                    const auto y = wave.getBottom()
                        - static_cast<float>(magnitude) * wave.getHeight();
                    graphics.drawVerticalLine(
                        static_cast<int>(std::round(x)),
                        y,
                        wave.getBottom());
                    graphics.fillEllipse(x - 1.5F, y - 1.5F, 3.0F, 3.0F);
                }
                continue;
            }
            if (lane.view.trace.kind
                == talas::project::WorkspaceAnalysisTraceKind::ComplexFrame)
            {
                if (lane.valueCount % 2 != 0)
                    continue;
                const auto binCount = lane.valueCount / 2;
                double maximumMagnitude = 0.0;
                for (std::size_t bin = 0; bin < binCount; ++bin)
                {
                    maximumMagnitude = std::max(
                        maximumMagnitude,
                        std::hypot(
                            lane.samples[bin * 2],
                            lane.samples[bin * 2 + 1]));
                }
                if (!(maximumMagnitude > 0.0))
                    maximumMagnitude = 1.0;
                juce::Path spectralPath;
                for (std::size_t bin = 0; bin < binCount; ++bin)
                {
                    const auto x = wave.getX()
                        + wave.getWidth() * static_cast<float>(bin)
                            / static_cast<float>(std::max<std::size_t>(
                                1, binCount - 1));
                    const auto magnitude = std::clamp(
                        std::hypot(
                            lane.samples[bin * 2],
                            lane.samples[bin * 2 + 1])
                            / maximumMagnitude
                            * lane.view.trace.verticalScale,
                        0.0,
                        1.0);
                    const auto y = wave.getBottom()
                        - static_cast<float>(magnitude) * wave.getHeight();
                    if (bin == 0)
                        spectralPath.startNewSubPath(x, y);
                    else
                        spectralPath.lineTo(x, y);
                }
                graphics.setColour(traceColour(
                    lane.view.trace.kind,
                    lane.view.trace.colourSlot));
                graphics.strokePath(
                    spectralPath,
                    juce::PathStrokeType{1.25F});
                continue;
            }
            graphics.setColour(panelBorderColour.withAlpha(0.45F));
            graphics.drawHorizontalLine(
                static_cast<int>(std::round(wave.getCentreY())),
                wave.getX(),
                wave.getRight());
            if (lane.valueCount == 1)
            {
                const auto sample = std::clamp(
                    lane.samples.front()
                        * lane.view.trace.verticalScale,
                    -1.0,
                    1.0);
                const auto y = wave.getCentreY()
                    - static_cast<float>(sample)
                        * wave.getHeight() * 0.46F;
                graphics.setColour(traceColour(
                    lane.view.trace.kind,
                    lane.view.trace.colourSlot));
                if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Control)
                {
                    graphics.drawHorizontalLine(
                        static_cast<int>(std::round(y)),
                        wave.getX(),
                        wave.getRight());
                }
                else
                {
                    graphics.fillEllipse(
                        wave.getCentreX() - 2.0F,
                        y - 2.0F,
                        4.0F,
                        4.0F);
                }
                continue;
            }
            juce::Path path;
            for (std::size_t frame = 0;
                 frame < lane.valueCount;
                 ++frame)
            {
                const auto x = wave.getX()
                    + wave.getWidth() * static_cast<float>(frame)
                        / static_cast<float>(lane.valueCount - 1);
                const auto sample = std::clamp(
                    lane.samples[frame]
                        * lane.view.trace.verticalScale,
                    -1.0,
                    1.0);
                const auto y = wave.getCentreY()
                    - static_cast<float>(sample)
                        * wave.getHeight() * 0.46F;
                if (frame == 0)
                    path.startNewSubPath(x, y);
                else
                    path.lineTo(x, y);
            }
            graphics.setColour(traceColour(
                lane.view.trace.kind,
                lane.view.trace.colourSlot));
            graphics.strokePath(
                path,
                juce::PathStrokeType{
                    1.0F + 0.25F * static_cast<float>(
                        lane.view.trace.colourSlot % 3)});
        }
    }

    [[nodiscard]] Mode displayMode() const noexcept
    {
        return traceLanes_.empty()
            ? mode_
            : Mode::Oscilloscope;
    }

    juce::String statusText() const
    {
        if (!traceLanes_.empty())
        {
            std::size_t liveCount = 0;
            std::size_t retainedCount = 0;
            std::size_t waitingCount = 0;
            std::size_t unavailableCount = 0;
            std::size_t rejectedEvidenceCount = 0;
            std::size_t hiddenCount = 0;
            std::uint64_t dropped = 0;
            std::uint64_t rejected = 0;
            for (const auto& lane : traceLanes_)
            {
                if (!lane.view.trace.visible)
                    ++hiddenCount;
                switch (lane.runtimeState)
                {
                case talas::app::AnalysisTraceRuntimeState::Live:
                    ++liveCount;
                    break;
                case talas::app::AnalysisTraceRuntimeState::RetainedNoNewData:
                    ++retainedCount;
                    break;
                case talas::app::AnalysisTraceRuntimeState::WaitingForCurrentStage:
                    ++waitingCount;
                    break;
                case talas::app::AnalysisTraceRuntimeState::RejectedEvidence:
                    ++rejectedEvidenceCount;
                    break;
                case talas::app::AnalysisTraceRuntimeState::TargetUnavailable:
                case talas::app::AnalysisTraceRuntimeState::CaptureUnavailable:
                case talas::app::AnalysisTraceRuntimeState::PreparedProbeUnavailable:
                    ++unavailableCount;
                    break;
                }
                dropped = talas::runtime::realtimeMetricAddSaturated(
                    dropped,
                    lane.droppedBlocks);
                rejected = talas::runtime::realtimeMetricAddSaturated(
                    rejected,
                    lane.rejectedRecords);
            }
            auto status = frozen_
                ? juceString("Frozen · capture draining")
                : juce::String{"Analysis"};
            if (!playing_)
                status = juceString("Paused · ") + status;
            status += juceString(" · ")
                + juce::String(static_cast<int>(liveCount))
                + " live";
            if (retainedCount != 0)
            {
                status += juceString(" · retained ")
                    + juce::String(static_cast<int>(retainedCount));
            }
            if (waitingCount != 0)
            {
                status += juceString(" · waiting ")
                    + juce::String(static_cast<int>(waitingCount));
            }
            status += juceString(" · history ")
                + juce::String(historyFrames_)
                + juceString(" · ")
                + juce::String(refreshRateHz_) + " Hz";
            if (unavailableCount != 0)
            {
                status += juceString(" · unavailable ")
                    + juce::String(static_cast<int>(unavailableCount));
            }
            if (rejectedEvidenceCount != 0)
            {
                status += juceString(" · evidence rejected ")
                    + juce::String(
                        static_cast<int>(rejectedEvidenceCount));
            }
            if (hiddenCount != 0)
            {
                status += juceString(" · hidden ")
                    + juce::String(static_cast<int>(hiddenCount));
            }
            if (soloTraceId_.has_value())
                status += juceString(" · solo");
            if (dropped != 0)
                status += juceString(" · dropped ")
                    + juce::String(std::to_string(dropped));
            if (rejected != 0)
                status += juceString(" · rejected ")
                    + juce::String(std::to_string(rejected));
            return status;
        }
        if (mode_ == Mode::None)
            return "Select an Oscilloscope Display or Spectrum Analyzer Display node";
        if (!playing_ && visibleFrameCount_ == 0)
            return "Stopped";
        if (capture_ == nullptr)
            return "Compile and play the current graph to begin capture";
        if (visibleFrameCount_ == 0)
            return frozen_
                ? juceString(
                    "Frozen · waiting for the first complete audio block")
                : "Waiting for the first complete audio block";
        const auto scope = oscilloscopeWindow();
        const auto displayedFrames = mode_ == Mode::Spectrum
            ? analyzedFrameCount_
            : scope.frameCount;
        auto status = frozen_ ? juceString("Frozen · ") : juce::String{};
        if (!playing_)
            status += juceString("Paused · ");
        status += juce::String(
            static_cast<int>(displayedFrames))
            + " samples";
        if (mode_ == Mode::Spectrum)
        {
            status += juceString(" · ") + windowName();
            if (selectedFrequencyScale()
                == talas::audio::SpectrumFrequencyScale::Logarithmic)
            {
                status += juceString(" · log");
            }
            const auto depth = averagingDepth();
            if (depth > 1)
            {
                status += juceString(" · avg ")
                    + juce::String(static_cast<int>(depth))
                    + "x";
            }
            if (peakHoldButton_.getToggleState())
                status += juceString(" · peak");
        }
        else if (separateTriggerConnected_
            && selectedScopeTrigger()
                != talas::audio::OscilloscopeTrigger::Free)
        {
            if (isSingleShotTrigger())
                status += separateTriggerIsRight_
                    ? (singleShotState_.latched()
                        ? juceString(" · right single captured")
                        : juceString(" · right single armed"))
                    : (singleShotState_.latched()
                        ? juceString(" · direct single captured")
                        : juceString(" · direct single armed"));
            else
                status += separateTriggerIsRight_
                    ? (scope.triggered
                        ? juceString(" · right trigger locked")
                        : juceString(" · right trigger waiting"))
                    : (scope.triggered
                        ? juceString(" · direct trigger locked")
                        : juceString(" · direct trigger waiting"));
        }
        else if (selectedScopeTrigger()
                 != talas::audio::OscilloscopeTrigger::Free)
        {
            if (isSingleShotTrigger())
            {
                status += singleShotState_.latched()
                    ? juceString(" · single captured")
                    : juceString(" · single armed");
            }
            else
            {
                status += scope.triggered
                    ? juceString(" · trig locked")
                    : juceString(" · trig auto");
            }
            status += selectedScopeTrigger()
                    == talas::audio::OscilloscopeTrigger::Falling
                ? juceString(" ↓")
                : juceString(" ↑");
            status += " @ "
                + juce::String(triggerLevelSlider_.getValue(), 2);
        }
        else if (mode_ == Mode::Oscilloscope)
        {
            status += juceString(" · sweep ")
                + juce::String(
                    sweepFrequencySlider_.getValue(), 1)
                + " Hz";
            if (visibleSampleStrideFrames_ > 1)
                status += juceString(" · timebase ")
                    + juce::String(
                        visibleInitialSweepFrequencyHz_, 2)
                    + " Hz";
        }
        if (droppedBlockCount_ > 0)
            status += juceString(" · dropped ")
                + juce::String(static_cast<juce::int64>(droppedBlockCount_));
        if (selectedZoom() > 1.0)
        {
            status += juceString(" · zoom ")
                + juce::String(
                    static_cast<int>(selectedZoom()))
                + "x";
            if (mode_ == Mode::Spectrum)
            {
                status += juceString(" · pan ")
                    + juce::String(
                        static_cast<int>(
                            spectrumPanSlider_.getValue()))
                    + "%";
            }
        }
        if (pinnedCursorX_.has_value())
            status += juceString(" · cursor pinned");
        return status;
    }

    [[nodiscard]] std::size_t selectedFrameCount() const
    {
        switch (frameCountBox_.getSelectedId())
        {
        case 1: return 64;
        case 2: return 128;
        default: return 256;
        }
    }

    [[nodiscard]] talas::audio::SpectrumWindow selectedWindow() const
    {
        switch (windowBox_.getSelectedId())
        {
        case 1:
            return talas::audio::SpectrumWindow::Rectangular;
        case 3:
            return talas::audio::SpectrumWindow::Blackman;
        case 4:
            return talas::audio::SpectrumWindow::LinearEnvelope;
        case 5:
            return talas::audio::SpectrumWindow::Hamming;
        case 6:
            return talas::audio::SpectrumWindow::LongKaiser;
        default:
            return talas::audio::SpectrumWindow::Hann;
        }
    }

    [[nodiscard]] juce::String windowName() const
    {
        switch (selectedWindow())
        {
        case talas::audio::SpectrumWindow::Rectangular:
            return "Rect";
        case talas::audio::SpectrumWindow::Hann:
            return "Hann";
        case talas::audio::SpectrumWindow::Blackman:
            return "Blackman";
        case talas::audio::SpectrumWindow::LinearEnvelope:
            return "LinearEnvelope";
        case talas::audio::SpectrumWindow::Hamming:
            return "Hamming";
        case talas::audio::SpectrumWindow::LongKaiser:
            return "Long Kaiser";
        }
        return {};
    }

    [[nodiscard]] std::size_t averagingDepth() const
    {
        switch (averageBox_.getSelectedId())
        {
        case 2: return 2;
        case 3: return 4;
        case 4: return 8;
        default: return 1;
        }
    }

    [[nodiscard]] double selectedZoom() const
    {
        switch (zoomBox_.getSelectedId())
        {
        case 2: return 2.0;
        case 3: return 4.0;
        default: return 1.0;
        }
    }

    [[nodiscard]] talas::audio::OscilloscopeTrigger
    selectedScopeTrigger() const
    {
        switch (triggerModeBox_.getSelectedId())
        {
        case 2:
        case 4:
            return talas::audio::OscilloscopeTrigger::Rising;
        case 3:
        case 5:
            return talas::audio::OscilloscopeTrigger::Falling;
        default:
            return talas::audio::OscilloscopeTrigger::Free;
        }
    }

    [[nodiscard]] bool isSingleShotTrigger() const
    {
        return triggerModeBox_.getSelectedId() == 4
            || triggerModeBox_.getSelectedId() == 5;
    }

    [[nodiscard]] talas::audio::SpectrumFrequencyScale
    selectedFrequencyScale() const
    {
        return frequencyScaleBox_.getSelectedId() == 2
            ? talas::audio::SpectrumFrequencyScale::Logarithmic
            : talas::audio::SpectrumFrequencyScale::Linear;
    }

    [[nodiscard]] double selectedSpectrumPan() const
    {
        return spectrumPanSlider_.getValue() / 100.0;
    }

    [[nodiscard]] talas::audio::SpectrumView
    spectrumView() const
    {
        return talas::audio::selectSpectrumView(
            spectrumMagnitudes_.size(),
            selectedZoom(),
            selectedSpectrumPan(),
            selectedFrequencyScale());
    }

    [[nodiscard]] double normalizedCursorX(float x) const
    {
        const auto plot = analysisPlotBounds();
        if (plot.getWidth() <= 0.0F)
            return 0.0;
        return std::clamp(
            static_cast<double>(
                (x - plot.getX()) / plot.getWidth()),
            0.0,
            1.0);
    }

    [[nodiscard]] juce::String cursorReadoutText(
        double normalizedX) const
    {
        if (!traceLanes_.empty())
        {
            const auto selected = selectedTraceLaneIndex();
            if (!selected.has_value())
                return {};
            const auto& lane = traceLanes_[*selected];
            if ((lane.view.trace.kind
                        == talas::project::WorkspaceAnalysisTraceKind::Audio
                    || lane.view.trace.kind
                        == talas::project::WorkspaceAnalysisTraceKind::Control)
                && lane.valueCount != 0)
            {
                const auto frame = std::min<std::size_t>(
                    static_cast<std::size_t>(std::round(
                        std::clamp(normalizedX, 0.0, 1.0)
                        * static_cast<double>(lane.valueCount - 1))),
                    lane.valueCount - 1);
                if (frame >= lane.samplePositions.size())
                {
                    return juceString(
                        "Cursor unavailable · sample position missing");
                }
                const auto samplePosition = lane.samplePositions[frame];
                auto readout = juceString("frame ")
                    + juce::String(std::to_string(samplePosition));
                if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Audio)
                {
                    if (sampleRate_ > 0.0)
                    {
                        readout += juceString(" · ")
                            + juce::String(
                                static_cast<double>(samplePosition)
                                    * 1000.0 / sampleRate_,
                                3)
                            + " ms";
                    }
                    readout += juceString(" · amplitude ")
                        + juce::String(lane.samples[frame], 3);
                }
                else
                {
                    readout += juceString(" · value ")
                        + juce::String(lane.samples[frame], 3);
                }
                return readout;
            }
            if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::Event
                && lane.valueCount >= 3
                && lane.valueCount % 3 == 0)
            {
                const auto eventCount = lane.valueCount / 3;
                const auto eventIndex = std::min<std::size_t>(
                    static_cast<std::size_t>(std::round(
                        std::clamp(normalizedX, 0.0, 1.0)
                        * static_cast<double>(eventCount - 1))),
                    eventCount - 1);
                const auto value = eventIndex * 3;
                return "event "
                    + juce::String(lane.samples[value], 0)
                    + ", value "
                    + juce::String(lane.samples[value + 1], 3)
                    + ", frame "
                    + juce::String(lane.samples[value + 2], 0);
            }
            if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum
                && lane.valueCount >= 3
                && lane.valueCount % 3 == 0
                && sampleRate_ > 0.0)
            {
                const auto requestedFrequency = std::clamp(
                    normalizedX, 0.0, 1.0) * sampleRate_ * 0.5;
                std::size_t nearest = 0;
                auto distance = std::numeric_limits<double>::infinity();
                for (std::size_t value = 0;
                     value < lane.valueCount;
                     value += 3)
                {
                    const auto candidate = std::abs(
                        lane.samples[value] - requestedFrequency);
                    if (candidate < distance)
                    {
                        distance = candidate;
                        nearest = value;
                    }
                }
                return juce::String(lane.samples[nearest], 2)
                    + " Hz, amplitude "
                    + juce::String(lane.samples[nearest + 1], 3)
                    + ", bandwidth "
                    + juce::String(lane.samples[nearest + 2], 2)
                    + " Hz";
            }
            if (lane.view.trace.kind
                    == talas::project::WorkspaceAnalysisTraceKind::ComplexFrame
                && lane.valueCount >= 2
                && lane.valueCount % 2 == 0)
            {
                const auto binCount = lane.valueCount / 2;
                const auto bin = std::min<std::size_t>(
                    static_cast<std::size_t>(std::round(
                        std::clamp(normalizedX, 0.0, 1.0)
                        * static_cast<double>(binCount - 1))),
                    binCount - 1);
                const auto real = lane.samples[bin * 2];
                const auto imaginary = lane.samples[bin * 2 + 1];
                const auto frequency = sampleRate_ > 0.0
                    ? static_cast<double>(bin)
                        / static_cast<double>(std::max<std::size_t>(
                            1, binCount - 1))
                        * sampleRate_ * 0.5
                    : 0.0;
                return juce::String(frequency, 2)
                    + " Hz, magnitude "
                    + juce::String(std::hypot(real, imaginary), 3)
                    + ", phase "
                    + juce::String(std::atan2(imaginary, real), 3)
                    + " rad";
            }
        }
        if (displayMode() == Mode::Oscilloscope)
        {
            const auto readout = talas::audio::scopeCursorReadout(
                std::span<const double>{
                    visibleSamples_.data(),
                    visibleFrameCount_},
                oscilloscopeWindow(),
                scopeSampleRate(),
                scopeCursorPosition(normalizedX));
            if (!readout.has_value())
                return {};
            return juce::String(readout->timeSeconds * 1000.0, 3)
                + " ms, amplitude "
                + juce::String(readout->amplitude, 3);
        }
        if (displayMode() == Mode::Spectrum)
        {
            const auto readout = talas::audio::spectrumCursorReadout(
                spectrumMagnitudes_,
                analyzedFrameCount_,
                sampleRate_,
                spectrumView(),
                normalizedX);
            if (!readout.has_value())
                return {};
            return juce::String(readout->frequencyHz, 1)
                + " Hz, "
                + juce::String(readout->decibels, 1)
                + " dB";
        }
        return {};
    }

    void refreshCursorReadout(bool announce = false)
    {
        const auto activeCursorX = pinnedCursorX_.has_value()
            ? pinnedCursorX_
            : hoverCursorX_;
        auto nextText = activeCursorX.has_value()
            ? cursorReadoutText(*activeCursorX)
            : juce::String{};
        if (hoverLevel_.has_value() && hoverCursorX_.has_value())
            nextText += (nextText.isEmpty() ? juce::String{} : juceString(" · ")) + "cursor level "
                + juce::String(*hoverLevel_, 3);
        const auto changed = cursorReadout_.getText() != nextText;
        cursorReadout_.setText(
            nextText,
            juce::dontSendNotification);
        cursorReadout_.setTitle(
            nextText.isEmpty()
                ? juce::String{"Analyzer cursor measurement"}
                : "Analyzer cursor: " + nextText);
        if (changed)
        {
            if (auto* handler = cursorReadout_.getAccessibilityHandler())
            {
                handler->notifyAccessibilityEvent(
                    juce::AccessibilityEvent::titleChanged);
            }
        }
        if (announce)
        {
            juce::AccessibilityHandler::postAnnouncement(
                nextText.isEmpty()
                    ? juce::String{"Analyzer cursor cleared"}
                    : nextText,
                juce::AccessibilityHandler::AnnouncementPriority::medium);
        }
    }

    void applyPreparedOscilloscopeFields()
    {
        double frequency = 30.0;
        int triggerModeId = 1;
        if (mode_ == Mode::Oscilloscope && capture_ != nullptr)
        {
            const auto index = capture_->preferredProbeIndex(
                selectedNodeId_);
            const auto* spec = index.has_value()
                ? capture_->probeSpec(*index)
                : nullptr;
            if (spec != nullptr
                && spec->kind
                    == talas::runtime::RealtimeAnalysisKind::Oscilloscope)
            {
                frequency = spec->sweepFrequencyHz;
                if (spec->triggerModeEnabled)
                    triggerModeId = spec->triggerOnFallingEdge ? 3 : 2;
            }
        }
        triggerModeBox_.setSelectedId(
            triggerModeId, juce::dontSendNotification);
        if (!std::isfinite(frequency))
            frequency = 30.0;
        sweepFrequencySlider_.setValue(
            std::clamp(frequency, 0.1, 30.0),
            juce::dontSendNotification);
        sweepState_.reset();
    }

    void clearDisplay()
    {
        visibleFrameCount_ = 0;
        visibleSampleStrideFrames_ = 1;
        visibleInitialSweepFrequencyHz_ = 30.0;
        analyzedFrameCount_ = 0;
        currentSpectrumMagnitudes_.clear();
        spectrumMagnitudes_.clear();
        spectrumPeaks_.clear();
        droppedBlockCount_ = 0;
        separateTriggerConnected_ = false;
        separateTriggerActive_ = false;
        separateTriggerIsRight_ = false;
        defaultTriggerIsDirect_ = false;
        triggerLevelEdited_ = false;
        triggerLevelSlider_.setValue(0.0, juce::dontSendNotification);
        singleShotState_.rearm();
        sweepState_.reset();
        sweepStageId_ = 0;
        sweepBindingGeneration_ = 0;
        hoverCursorX_.reset();
        pinnedCursorX_.reset();
        draggingCursor_ = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void resetSpectrumState()
    {
        currentSpectrumMagnitudes_.clear();
        spectrumMagnitudes_.clear();
        spectrumPeaks_.clear();
        analyzedFrameCount_ = 0;
        if (displayMode() == Mode::Spectrum
            && visibleFrameCount_ > 0)
        {
            rebuildSpectrum();
        }
        updateControlState();
        refreshCursorReadout();
    }

    void updateControlState()
    {
        const auto workspaceTraces = !traceLanes_.empty();
        const auto hasMode = mode_ != Mode::None || workspaceTraces;
        const auto spectrum = mode_ == Mode::Spectrum
            && !workspaceTraces;
        const auto oscilloscope = mode_ == Mode::Oscilloscope
            && !workspaceTraces;
        updateSelectedTraceControls();
        const auto selectedIndex = traceBox_.getSelectedItemIndex();
        const auto hasSelectedTrace = selectedIndex >= 0
            && static_cast<std::size_t>(selectedIndex)
                < projection_.traces.size();
        traceBox_.setEnabled(!projection_.traces.empty());
        pinButton_.setEnabled(!selectedNodeId_.empty());
        removeButton_.setEnabled(hasSelectedTrace);
        moveUpButton_.setVisible(workspaceTraces);
        moveUpButton_.setEnabled(
            hasSelectedTrace && selectedIndex > 0);
        moveDownButton_.setVisible(workspaceTraces);
        moveDownButton_.setEnabled(
            hasSelectedTrace
            && static_cast<std::size_t>(selectedIndex + 1)
                < projection_.traces.size());
        visibilityButton_.setVisible(workspaceTraces);
        visibilityButton_.setEnabled(hasSelectedTrace);
        soloButton_.setVisible(workspaceTraces);
        soloButton_.setEnabled(hasSelectedTrace);
        traceScaleLabel_.setVisible(workspaceTraces);
        traceScaleSlider_.setVisible(workspaceTraces);
        traceScaleSlider_.setEnabled(hasSelectedTrace);
        historyBox_.setVisible(workspaceTraces);
        historyBox_.setEnabled(workspaceTraces);
        refreshRateBox_.setVisible(workspaceTraces);
        refreshRateBox_.setEnabled(workspaceTraces);
        frameCountBox_.setVisible(!workspaceTraces);
        frameCountBox_.setEnabled(hasMode);
        zoomBox_.setVisible(!workspaceTraces);
        zoomBox_.setEnabled(hasMode);
        freezeButton_.setEnabled(
            hasMode && visibleFrameCount_ > 0);
        windowBox_.setVisible(spectrum);
        windowBox_.setEnabled(spectrum);
        averageBox_.setVisible(spectrum);
        averageBox_.setEnabled(spectrum);
        peakHoldButton_.setVisible(spectrum);
        peakHoldButton_.setEnabled(spectrum);
        resetButton_.setVisible(spectrum);
        resetButton_.setEnabled(
            spectrum && visibleFrameCount_ > 0);
        frequencyScaleBox_.setVisible(spectrum);
        frequencyScaleBox_.setEnabled(spectrum);
        spectrumPanSlider_.setVisible(spectrum);
        spectrumPanSlider_.setEnabled(
            spectrum && selectedZoom() > 1.0);
        triggerModeBox_.setVisible(oscilloscope);
        triggerModeBox_.setEnabled(oscilloscope);
        triggerLevelSlider_.setVisible(oscilloscope);
        triggerLevelSlider_.setEnabled(
            oscilloscope
            && selectedScopeTrigger()
                != talas::audio::OscilloscopeTrigger::Free);
        sweepFrequencySlider_.setVisible(oscilloscope);
        sweepFrequencySlider_.setEnabled(
            oscilloscope && selectedScopeTrigger()
                == talas::audio::OscilloscopeTrigger::Free);
        rearmButton_.setVisible(
            oscilloscope && isSingleShotTrigger());
        rearmButton_.setEnabled(
            oscilloscope && isSingleShotTrigger()
            && visibleFrameCount_ > 1);
        updateAuditionControlState();
        updateOutputTakeControlState();
    }

    void updateOutputTakeControlState()
    {
        const auto active = outputTakeState_.recording
            || outputTakeState_.stopping;
        outputTakeRecordButton_.setEnabled(
            outputTakeState_.deviceReady && !active
                && (!outputTakeState_.diskRecorder
                    || outputTakeState_.diskRecorderWritable));
        outputTakeStopButton_.setEnabled(
            outputTakeState_.recording);
        outputTakeCancelButton_.setEnabled(active);
    }

    void updateAuditionControlState()
    {
        const auto selectedIndex = traceBox_.getSelectedItemIndex();
        const auto selectedTrace = selectedIndex >= 0
            && static_cast<std::size_t>(selectedIndex)
                < projection_.traces.size()
            && projection_.traces[static_cast<std::size_t>(selectedIndex)]
                   .trace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Audio;
        const auto sourceId = auditionSourceBox_.getSelectedId();
        const auto temporary = sourceId > 1;
        const auto sine = sourceId == 4;
        const auto frozen = sourceId == 5;
        const auto inputIndex = auditionInputBox_.getSelectedItemIndex();
        const auto validInput = inputIndex >= 0
            && static_cast<std::size_t>(inputIndex)
                < auditionState_.destinationInputs.size();
        const auto assetIndex = auditionAssetBox_.getSelectedItemIndex();
        const auto validAsset = assetIndex >= 0
            && static_cast<std::size_t>(assetIndex)
                < auditionState_.frozenAssets.size();
        const auto frequency =
            auditionFrequencyBox_.getText().getDoubleValue();
        const auto validFrequency = !sine
            || (std::isfinite(frequency)
                && frequency >= 1.0
                && frequency <= 20000.0);
        auditionSourceBox_.setEnabled(
            selectedTrace && !auditionState_.preparing
            && !auditionState_.active);
        auditionInputBox_.setVisible(temporary);
        auditionInputBox_.setEnabled(
            temporary && !auditionState_.preparing
            && !auditionState_.active
            && !auditionState_.destinationInputs.empty());
        auditionFrequencyBox_.setVisible(sine);
        auditionFrequencyBox_.setEnabled(
            sine && !auditionState_.preparing
            && !auditionState_.active);
        auditionAssetBox_.setVisible(frozen);
        auditionAssetBox_.setEnabled(
            frozen && !auditionState_.preparing
            && !auditionState_.active
            && !auditionState_.frozenAssets.empty());
        auditionStartButton_.setEnabled(
            selectedTrace && !auditionState_.preparing
            && !auditionState_.active
            && !auditionState_.transportExternallyControlled
            && (!temporary || validInput)
            && (!frozen || validAsset)
            && validFrequency);
        auditionStopButton_.setEnabled(
            auditionState_.preparing || auditionState_.active);
        auditionCommitButton_.setEnabled(
            auditionState_.active && auditionState_.canCommit);
    }

    [[nodiscard]] double scopeSampleRate() const noexcept
    {
        return sampleRate_ /
            static_cast<double>(visibleSampleStrideFrames_);
    }

    [[nodiscard]] std::size_t scopePlotDenominator(
        const talas::audio::OscilloscopeWindow& window) const noexcept
    {
        return visibleSampleStrideFrames_ > 1
                && window.frameCount < selectedFrameCount()
            ? selectedFrameCount() - 1
            : window.frameCount - 1;
    }

    [[nodiscard]] double scopeCursorPosition(double normalizedX) const
    {
        const auto window = oscilloscopeWindow();
        if (window.frameCount < 2)
            return normalizedX;
        return std::clamp(
            normalizedX
                * static_cast<double>(scopePlotDenominator(window))
                / static_cast<double>(window.frameCount - 1),
            0.0, 1.0);
    }

    [[nodiscard]] talas::audio::OscilloscopeWindow
    oscilloscopeWindow() const
    {
        if (separateTriggerConnected_ && !separateTriggerActive_)
            return talas::audio::selectOscilloscopeWindow(
                std::span<const double>{
                    visibleSamples_.data(),
                    visibleFrameCount_},
                selectedFrameCount(),
                talas::audio::OscilloscopeTrigger::Free);
        if (separateTriggerActive_
            && selectedScopeTrigger()
                != talas::audio::OscilloscopeTrigger::Free)
        {
            return talas::audio::selectOscilloscopeWindow(
                std::span<const double>{
                    visibleSamples_.data(),
                    visibleFrameCount_},
                std::span<const double>{
                    visibleSeparateTriggerSamples_.data(),
                    visibleFrameCount_},
                selectedFrameCount(),
                selectedScopeTrigger()
                        == talas::audio::OscilloscopeTrigger::Falling
                    ? talas::audio::OscilloscopeTrigger::Falling
                    : talas::audio::OscilloscopeTrigger::Rising,
                triggerLevelSlider_.getValue());
        }
        if (locksPowerOfTwoPeriods() && visibleSampleStrideFrames_ == 1)
        {
            if (const auto locked = talas::audio::powerOfTwoLockedOscilloscopeWindow(
                    visibleAbsoluteFirstFrame_, visibleFrameCount_, selectedFrameCount()))
                return *locked;
        }
        return talas::audio::selectOscilloscopeWindow(
            std::span<const double>{
                visibleSamples_.data(),
                visibleFrameCount_},
            selectedFrameCount(),
            selectedScopeTrigger(),
            triggerLevelSlider_.getValue());
    }

    // The Info menu's free-running oscilloscope locks a
    // period of a power of two samples, up to 512.
    [[nodiscard]] bool locksPowerOfTwoPeriods() const
    {
        return infoDisplay_.has_value() && *infoDisplay_ == selectedNodeId_ && mode_ == Mode::Oscilloscope
            && !separateTriggerConnected_ && selectedScopeTrigger() == talas::audio::OscilloscopeTrigger::Free;
    }

    [[nodiscard]] juce::Rectangle<float>
    analysisPlotBounds() const
    {
        auto bounds = getLocalBounds().reduced(10);
        bounds.removeFromTop(analysisPanelHeaderHeight);
        bounds.removeFromTop(controlRowsHeight());
        bounds.removeFromTop(analysisPanelCaptureStatusHeight);
        bounds.removeFromTop(analysisPanelCaptureStatusGap);
        bounds.removeFromTop(analysisPanelEvidenceRowHeight);
        bounds.removeFromTop(analysisPanelEvidenceRowHeight);
        return bounds.reduced(0, 4).toFloat();
    }

    void rebuildSpectrum()
    {
        const auto analysis =
            talas::audio::analyzeNormalizedSpectrum(
                std::span<const double>{
                    visibleSamples_.data(),
                    visibleFrameCount_},
                selectedFrameCount(),
                selectedWindow());
        analyzedFrameCount_ = analysis.frameCount;
        currentSpectrumMagnitudes_ = analysis.magnitudes;
        talas::audio::updateSpectrumDisplay(
            currentSpectrumMagnitudes_,
            averagingDepth(),
            peakHoldButton_.getToggleState(),
            spectrumMagnitudes_,
            spectrumPeaks_);
    }

    void drawOscilloscope(juce::Graphics& graphics, juce::Rectangle<float> plot) const
    {
        graphics.setColour(panelBorderColour.withAlpha(0.7F));
        graphics.drawHorizontalLine(
            static_cast<int>(plot.getCentreY()),
            plot.getX(),
            plot.getRight());
        if (!separateTriggerConnected_ && selectedScopeTrigger()
            != talas::audio::OscilloscopeTrigger::Free)
        {
            const auto triggerDisplayValue =
                talas::audio::zoomedOscilloscopeSample(
                    triggerLevelSlider_.getValue(),
                    selectedZoom());
            const auto triggerY = plot.getCentreY()
                - static_cast<float>(triggerDisplayValue)
                    * plot.getHeight() * 0.46F;
            graphics.setColour(eventColour.withAlpha(0.6F));
            const float dashes[]{4.0F, 3.0F};
            graphics.drawDashedLine(
                juce::Line<float>{
                    plot.getX(),
                    triggerY,
                    plot.getRight(),
                    triggerY},
                dashes,
                2,
                1.0F);
        }
        const auto window = oscilloscopeWindow();
        if (window.frameCount < 2)
            return;
        const auto plotDenominator = scopePlotDenominator(window);
        juce::Path path;
        for (std::size_t frame = 0; frame < window.frameCount; ++frame)
        {
            const auto x = plot.getX()
                + plot.getWidth() * static_cast<float>(frame)
                    / static_cast<float>(plotDenominator);
            const auto sample =
                talas::audio::zoomedOscilloscopeSample(
                    visibleSamples_[window.firstFrame + frame],
                    selectedZoom());
            const auto y = plot.getCentreY()
                - static_cast<float>(sample) * plot.getHeight() * 0.46F;
            if (frame == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        graphics.setColour(audioColour);
        graphics.strokePath(path, juce::PathStrokeType{1.5F});
    }

    void drawSpectrum(juce::Graphics& graphics, juce::Rectangle<float> plot) const
    {
        const auto view = spectrumView();
        if (view.firstBin > view.lastBin
            || view.lastBin >= spectrumMagnitudes_.size()
            || view.firstBin == view.lastBin)
            return;
        if (spectrumPeaks_.size() == spectrumMagnitudes_.size())
        {
            const auto peaks =
                spectrumPath(
                    spectrumPeaks_,
                    view,
                    plot);
            graphics.setColour(eventColour.withAlpha(0.8F));
            graphics.strokePath(
                peaks,
                juce::PathStrokeType{1.0F});
        }
        const auto path =
            spectrumPath(spectrumMagnitudes_, view, plot);
        graphics.setColour(controlColour);
        graphics.strokePath(path, juce::PathStrokeType{1.5F});
        if (sampleRate_ > 0.0)
        {
            graphics.setColour(secondaryTextColour.withAlpha(0.8F));
            graphics.setFont(WorkbenchType::micro());
            const auto lowerFrequency =
                static_cast<double>(view.firstBin)
                * sampleRate_
                / static_cast<double>(analyzedFrameCount_);
            graphics.drawText(
                juce::String(lowerFrequency, 0) + " Hz",
                plot.toNearestInt().removeFromBottom(14).removeFromLeft(42),
                juce::Justification::centredLeft);
            auto rightLabel = plot.toNearestInt().removeFromBottom(14).removeFromRight(72);
            const auto upperFrequency =
                static_cast<double>(view.lastBin)
                * sampleRate_
                / static_cast<double>(analyzedFrameCount_);
            graphics.drawText(
                juce::String(upperFrequency, 0) + " Hz",
                rightLabel,
                juce::Justification::centredRight);
        }
    }

    void drawCursor(
        juce::Graphics& graphics,
        juce::Rectangle<float> plot) const
    {
        const auto activeCursorX = pinnedCursorX_.has_value()
            ? pinnedCursorX_
            : hoverCursorX_;
        if (!activeCursorX.has_value()
            || plot.getWidth() <= 0.0F)
        {
            return;
        }
        const auto normalizedX = *activeCursorX;
        auto x = plot.getX();
        auto y = plot.getCentreY();
        if (displayMode() == Mode::Oscilloscope)
        {
            const auto window = oscilloscopeWindow();
            const auto readout =
                talas::audio::scopeCursorReadout(
                    std::span<const double>{
                        visibleSamples_.data(),
                        visibleFrameCount_},
                    window,
                    scopeSampleRate(),
                    scopeCursorPosition(normalizedX));
            if (!readout.has_value())
                return;
            if (window.frameCount > 1)
            {
                x += plot.getWidth()
                    * static_cast<float>(readout->sampleOffset)
                    / static_cast<float>(scopePlotDenominator(window));
            }
            const auto displaySample =
                talas::audio::zoomedOscilloscopeSample(
                    readout->amplitude,
                    selectedZoom());
            y -= static_cast<float>(displaySample)
                * plot.getHeight() * 0.46F;
        }
        else if (displayMode() == Mode::Spectrum)
        {
            const auto view = spectrumView();
            const auto readout =
                talas::audio::spectrumCursorReadout(
                    spectrumMagnitudes_,
                    analyzedFrameCount_,
                    sampleRate_,
                    view,
                    normalizedX);
            if (!readout.has_value())
                return;
            x += plot.getWidth()
                * static_cast<float>(
                    talas::audio::spectrumNormalizedX(
                        view,
                        readout->bin));
            y = plot.getBottom()
                - plot.getHeight()
                    * static_cast<float>(
                        readout->normalizedMagnitude);
        }
        else
        {
            return;
        }

        graphics.setColour(selectionColour.withAlpha(
            pinnedCursorX_.has_value() ? 0.85F : 0.55F));
        graphics.drawVerticalLine(
            static_cast<int>(std::round(x)),
            plot.getY(),
            plot.getBottom());
        graphics.fillEllipse(x - 2.5F, y - 2.5F, 5.0F, 5.0F);
    }

    static juce::Path spectrumPath(
        std::span<const double> magnitudes,
        const talas::audio::SpectrumView& view,
        juce::Rectangle<float> plot)
    {
        juce::Path path;
        if (view.firstBin > view.lastBin
            || view.lastBin >= magnitudes.size()
            || view.firstBin == view.lastBin)
            return path;
        for (std::size_t bin = view.firstBin;; ++bin)
        {
            const auto x = plot.getX()
                + plot.getWidth()
                    * static_cast<float>(
                        talas::audio::spectrumNormalizedX(
                            view,
                            bin));
            const auto y = plot.getBottom()
                - plot.getHeight()
                    * static_cast<float>(magnitudes[bin]);
            if (bin == view.firstBin)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
            if (bin == view.lastBin)
                break;
        }
        return path;
    }

    Mode mode_ = Mode::None;
    [[nodiscard]] int controlRowsHeight() const noexcept
    { return analysisPanelControlRowsHeight + (sourceCaptureBox_.isVisible() ? 28 : 0); }
    std::string selectedNodeId_;
    std::string selectionNodeId_;
    std::string selectionTypeId_;
    std::optional<std::string> infoDisplay_;
    talas::app::SpectrumAnalyzerSettings infoSpectrumSettings_;
    talas::app::SpectrumAnalyzerSettings ownSpectrumSettings_;
    talas::app::AnalysisWorkbenchProjection projection_;
    AnalysisPanelCallbacks callbacks_;
    AnalysisAuditionPanelState auditionState_;
    AnalysisOutputTakePanelState outputTakeState_;
    std::vector<TraceLane> traceLanes_;
    std::vector<talas::app::AnalysisMonitorRecordInput>
        eventMonitorRecords_;
    std::uint64_t nextEventMonitorSequence_ = 1;
    std::shared_ptr<talas::runtime::RealtimeAnalysisCapture> capture_;
    std::shared_ptr<talas::runtime::RealtimeAnalysisCapture> rootCapture_;
    std::shared_ptr<const talas::runtime::PreparedScoreSourceAnalysisSet> scoreSources_;
    double sourceSampleRate_ = 0.0;
    std::vector<double> captureScratch_;
    std::uint64_t visibleAbsoluteFirstFrame_ = 0;
    std::vector<double> slowSweepScratch_;
    std::vector<double> separateTriggerScratch_;
    std::vector<double> visibleSamples_;
    std::vector<double> visibleSeparateTriggerSamples_;
    std::vector<double> currentSpectrumMagnitudes_;
    std::vector<double> spectrumMagnitudes_;
    std::vector<double> spectrumPeaks_;
    std::size_t visibleFrameCount_ = 0;
    std::uint32_t visibleSampleStrideFrames_ = 1;
    double visibleInitialSweepFrequencyHz_ = 30.0;
    std::size_t analyzedFrameCount_ = 0;
    std::uint64_t droppedBlockCount_ = 0;
    double sampleRate_ = 0.0;
    std::uint32_t refreshRateHz_ = 30;
    std::uint32_t historyFrames_ = 120;
    bool playing_ = false;
    bool frozen_ = false;
    bool separateTriggerConnected_ = false;
    bool separateTriggerActive_ = false;
    bool separateTriggerIsRight_ = false;
    bool defaultTriggerIsDirect_ = false;
    bool triggerLevelEdited_ = false;
    talas::audio::OscilloscopeSweepState sweepState_;
    std::uint64_t sweepStageId_ = 0;
    std::uint64_t sweepBindingGeneration_ = 0;
    bool draggingCursor_ = false;
    talas::audio::OscilloscopeSingleShotState singleShotState_;
    juce::Component* cursorMouseListenerRoot_ = nullptr;
    std::optional<double> hoverCursorX_;
    std::optional<double> hoverLevel_;
    std::optional<double> pinnedCursorX_;
    std::optional<std::string> soloTraceId_;
    juce::ToggleButton freezeButton_;
    juce::ComboBox traceBox_;
    juce::ComboBox sourceCaptureBox_;
    juce::Label sourceCaptureLabel_;
    juce::TextButton pinButton_;
    juce::TextButton removeButton_;
    juce::TextButton moveUpButton_;
    juce::TextButton moveDownButton_;
    juce::ToggleButton visibilityButton_;
    juce::ToggleButton soloButton_;
    juce::Label traceScaleLabel_;
    juce::Slider traceScaleSlider_;
    juce::ComboBox historyBox_;
    juce::ComboBox refreshRateBox_;
    juce::ComboBox auditionSourceBox_;
    juce::ComboBox auditionInputBox_;
    juce::ComboBox auditionAssetBox_;
    juce::ComboBox auditionFrequencyBox_;
    juce::TextButton auditionStartButton_;
    juce::TextButton auditionStopButton_;
    juce::TextButton auditionCommitButton_;
    juce::Label auditionStatus_;
    juce::TextButton outputTakeRecordButton_;
    juce::TextButton outputTakeStopButton_;
    juce::TextButton outputTakeCancelButton_;
    juce::Label outputTakeStatus_;
    juce::ComboBox frameCountBox_;
    juce::ComboBox zoomBox_;
    juce::ComboBox triggerModeBox_;
    juce::Slider triggerLevelSlider_;
    juce::Slider sweepFrequencySlider_;
    juce::ComboBox windowBox_;
    juce::ComboBox averageBox_;
    juce::ToggleButton peakHoldButton_;
    juce::TextButton resetButton_;
    juce::TextButton rearmButton_;
    juce::ComboBox frequencyScaleBox_;
    juce::Slider spectrumPanSlider_;
    juce::Label preparedStatus_;
    juce::Label monitorFocus_;
    juce::Label cursorReadout_;
    talas::app::AnalysisDiagnosticIdentity focusedMonitorIdentity_;
};

}

std::unique_ptr<AnalysisPanel> makeAnalysisPanel()
{
    return std::make_unique<AnalysisPanelImpl>();
}
}
