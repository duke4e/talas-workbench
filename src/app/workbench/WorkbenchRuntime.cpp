#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchAudioMidiSettings.h"
#include "app/workbench/WorkbenchAnalysisDiagnostics.h"
#include "app/workbench/WorkbenchAuthorityGeneration.h"
#include "app/workbench/WorkbenchOscDiagnostics.h"
#include "app/workbench/WorkbenchRealtimeMetricsPresentation.h"
#include "talas/app/AnalysisWorkbenchModel.h"
#include "talas/app/LiveGridWorkbenchModel.h"
#include "talas/app/LiveRecordingModel.h"
#include "talas/app/MemoryWarnings.h"

#include <limits>
#include <ranges>
#include <string>

namespace talas::app::workbench
{
namespace
{
void addSaturatedSingleWriter(
    std::atomic<std::uint64_t>& value,
    std::uint64_t increment = 1) noexcept
{
    const auto current = value.load(std::memory_order_relaxed);
    value.store(
        talas::runtime::realtimeMetricAddSaturated(current, increment),
        std::memory_order_relaxed);
}

void publishMaximumSingleWriter(
    std::atomic<std::uint64_t>& value,
    std::uint64_t candidate) noexcept
{
    const auto current = value.load(std::memory_order_relaxed);
    if (candidate > current)
        value.store(candidate, std::memory_order_relaxed);
}

juce::String realtimeMetricText(std::uint64_t value)
{
    return juce::String(std::to_string(value));
}

std::int64_t controllerRecordingTimestampNanoseconds(
    double seconds) noexcept
{
    constexpr auto maximumSeconds =
        static_cast<double>(std::numeric_limits<std::int64_t>::max())
        / 1'000'000'000.0;
    if (!std::isfinite(seconds) || seconds <= 0.0
        || seconds >= maximumSeconds)
    {
        return 0;
    }
    const auto nanoseconds = seconds * 1'000'000'000.0;
    return nanoseconds > 0.0
        ? static_cast<std::int64_t>(std::llround(nanoseconds))
        : 0;
}

juce::String midiClockTempoText(
    std::uint32_t microsecondsPerQuarter)
{
    if (microsecondsPerQuarter == 0)
        return {};
    const auto bpm = 60'000'000.0
        / static_cast<double>(microsecondsPerQuarter);
    return juce::String(bpm, 2) + " BPM";
}

void appendWorkspaceAnalysisTargets(
    talas::runtime::RenderSettings& settings,
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisSessionState& session)
{
    const auto projection =
        talas::app::buildAnalysisWorkbenchProjection(
            document,
            registry,
            session);
    for (const auto& trace : projection.traces)
    {
        if (!trace.available())
            continue;
        const auto payloadKind = [&]()
            -> std::optional<talas::runtime::RealtimeAnalysisPayloadKind> {
            switch (trace.trace.kind)
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
            return std::nullopt;
        }();
        if (!payloadKind.has_value())
            continue;
        const auto portId = trace.trace.portId.value_or(
            std::string{
                talas::runtime::realtimeAnalysisTypedEventEndpoint});
        const auto duplicate = std::ranges::any_of(
            settings.realtimeAnalysisTargets,
            [&](const auto& target) {
                return !target.scoreClipId && !target.scoreIterationOrdinal
                    && target.instancePath == trace.trace.instancePath
                    && target.containingDefinitionId
                        == trace.trace.containingDefinitionId
                    && target.nodeId == trace.trace.nodeId
                    && target.portId == portId
                    && target.payloadKind == *payloadKind;
            });
        if (duplicate)
            continue;
        settings.realtimeAnalysisTargets.push_back({
            trace.trace.instancePath,
            trace.trace.containingDefinitionId,
            trace.trace.nodeId,
            portId,
            trace.trace.kind
                        == talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum
                    || trace.trace.kind
                        == talas::project::WorkspaceAnalysisTraceKind::ComplexFrame
                ? talas::runtime::RealtimeAnalysisKind::Spectrum
                : talas::runtime::RealtimeAnalysisKind::Oscilloscope,
            *payloadKind,
            1,
            trace.trace.traceId,
        });
    }
}

}

void WorkbenchComponent::scheduleHardwareInitialisationAfterFirstPaint()
{
    if (!hardwareInitialisationRequested_
        || hardwareInitialisationStarted_)
    {
        return;
    }

    hardwareInitialisationStarted_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::Timer::callAfterDelay(1, [safeThis] {
        if (safeThis == nullptr)
            return;

        const auto error = safeThis->deviceManager_.initialise(
            1,
            2,
            nullptr,
            true);
        safeThis->deviceManager_.addAudioCallback(safeThis);
        safeThis->rebuildMidiInputSourceRegistry();
        safeThis->deviceManager_.addChangeListener(safeThis);
        safeThis->hardwareInitialised_ = true;
        safeThis->startTimerHz(30);
        if (error.isNotEmpty())
            safeThis->setStatus("Audio device unavailable: " + error);
        else if (auto* device = safeThis->deviceManager_.getCurrentAudioDevice())
            safeThis->setStatus("Ready: " + device->getName());
        else
            safeThis->setStatus("Audio device unavailable");
        safeThis->commandManager_.commandStatusChanged();
    });
}

void WorkbenchComponent::globalFocusChanged(juce::Component* focused)
{
    // Ctrl+Y evaluates in the focused text field.
    if (evaluateKeysEditor_ != nullptr && evaluateKeysEditor_.getComponent() != focused)
        evaluateKeysEditor_->removeKeyListener(&evaluateKeys_);
    if (auto* editor = dynamic_cast<juce::TextEditor*>(focused);
        editor != nullptr && editor != evaluateKeysEditor_.getComponent())
    {
        editor->addKeyListener(&evaluateKeys_);
        evaluateKeysEditor_ = editor;
    }
    else if (evaluateKeysEditor_.getComponent() != focused)
        evaluateKeysEditor_ = nullptr;
    if (const auto identity = workbenchFocusIdentityForComponent(focused))
        focusCoordinator_.capture(identity);
    else if (focused == nullptr && getPeer() != nullptr
             && getPeer()->isFocused())
        scheduleWorkbenchFocusRestoration();
    refreshContextHelp(focused);
    noteFrontWindow(focused); // Undo's window.
    repaint();
}

void WorkbenchComponent::refreshContextHelp(juce::Component* focused)
{
    if (contextHelpPanel_ == nullptr
        || !accessibilityProjection_.contextHelpVisible())
    {
        return;
    }
    const auto identity = focused != nullptr
        ? workbenchFocusIdentityForComponent(focused)
        : currentWorkbenchFocusIdentity();
    if (!identity.has_value())
    {
        contextHelpPanel_->clearContent();
        return;
    }
    if (focused == nullptr)
        focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused == nullptr)
    {
        contextHelpPanel_->clearContent();
        return;
    }

    const std::optional<talas::app::WorkbenchSurfaceState>*
        authoritativeSurfaceState = nullptr;
    for (auto* owner = focused; owner != nullptr;
         owner = owner->getParentComponent())
    {
        if (auto* source = dynamic_cast<
                talas::app::WorkbenchSurfaceStateSource*>(owner))
        {
            authoritativeSurfaceState =
                &source->workbenchSurfaceStateForContextHelp();
            break;
        }
        if (owner == this)
            break;
    }
    if (authoritativeSurfaceState != nullptr
        && !authoritativeSurfaceState->has_value())
    {
        contextHelpPanel_->clearContent();
        return;
    }

    const auto boundedText = [](juce::String value,
                                std::size_t maximumBytes,
                                const char* fallback) {
        value = value.replaceCharacters("\n\r\t", "   ").trim();
        if (value.isEmpty())
            value = fallback;
        while (value.toStdString().size() > maximumBytes
               && value.length() > 1)
        {
            value = value.substring(0, value.length() - 1);
        }
        return value;
    };
    const auto commandForIdentity = [&]() -> std::optional<juce::CommandID> {
        const auto& item = identity->itemId;
        if (item == "toolbar.play") return commandPlay;
        if (item == "toolbar.pause") return commandPause;
        if (item == "toolbar.stop") return commandStop;
        if (item == "toolbar.undo") return commandUndo;
        if (item == "toolbar.redo") return commandRedo;
        if (item == "toolbar.delete") return commandDeleteSelectedNode;
        if (item == "toolbar.save") return commandSaveProject;
        if (item == "toolbar.workspace.patch") return commandWorkspacePatch;
        if (item == "toolbar.workspace.automation") return commandWorkspaceAutomation;
        if (item == "toolbar.workspace.analysis") return commandWorkspaceAnalysis;
        if (item == "toolbar.workspace.performance") return commandWorkspacePerformance;
        if (item == "toolbar.workspace.samples") return commandWorkspaceSamples;
        return std::nullopt;
    }();

    const auto role = [region = identity->region] {
        switch (region)
        {
        case WorkbenchFocusRegion::Toolbar: return juce::String{"Command"};
        case WorkbenchFocusRegion::Library: return juce::String{"Library item"};
        case WorkbenchFocusRegion::PrimaryGraph: return juce::String{"Graph control"};
        case WorkbenchFocusRegion::SecondaryGraph: return juce::String{"Secondary graph control"};
        case WorkbenchFocusRegion::RightWorkspace: return juce::String{"Workspace inspector"};
        case WorkbenchFocusRegion::LowerWorkspace: return juce::String{"Workspace control"};
        case WorkbenchFocusRegion::StatusAndHelp: return juce::String{"Status"};
        }
        return juce::String{"Workbench control"};
    }();
    const auto keyboardAction = [&]() {
        if (dynamic_cast<juce::Button*>(focused) != nullptr)
            return juce::String{"Press Return or Space to activate"};
        if (dynamic_cast<juce::Slider*>(focused) != nullptr)
            return juce::String{"Use arrow keys to adjust"};
        if (dynamic_cast<juce::ComboBox*>(focused) != nullptr)
            return juce::String{"Press Return, then use arrow keys"};
        if (dynamic_cast<juce::ListBox*>(focused) != nullptr)
            return juce::String{"Use arrow keys; press Return to open"};
        return juce::String{"Use Tab to move; press Return for the primary action"};
    }();
    auto title = focused->getTitle();
    auto support = focused->getDescription();
    auto next = focused->getHelpText();
    auto currentValue = juce::String{
        focused->isEnabled() ? "Available" : "Unavailable"};
    auto kind = focused->isEnabled()
        ? talas::app::WorkbenchSurfaceStateKind::Content
        : talas::app::WorkbenchSurfaceStateKind::Unavailable;
    std::optional<talas::app::WorkbenchSurfaceActionEvidence> action;

    if (commandForIdentity.has_value())
    {
        juce::ApplicationCommandInfo command{*commandForIdentity};
        getCommandInfo(*commandForIdentity, command);
        title = command.shortName;
        support = command.description;
        const auto enabled =
            (command.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
        const auto ticked =
            (command.flags & juce::ApplicationCommandInfo::isTicked) != 0;
        currentValue = enabled
            ? ticked ? "On and available" : "Available"
            : "Unavailable in the current Workbench state";
        kind = enabled
            ? talas::app::WorkbenchSurfaceStateKind::Content
            : talas::app::WorkbenchSurfaceStateKind::Unavailable;
        action = talas::app::WorkbenchSurfaceActionEvidence{
            "command." + std::to_string(*commandForIdentity),
            command.shortName.toStdString(),
            enabled,
        };
        next = enabled
            ? "Activate " + command.shortName
            : "Resolve the unavailable state before activating this command";
    }
    else if (auto* slider = dynamic_cast<juce::Slider*>(focused))
    {
        currentValue = slider->getTextFromValue(slider->getValue());
        action = talas::app::WorkbenchSurfaceActionEvidence{
            "control.adjust", "Adjust value", slider->isEnabled()};
    }
    else if (auto* combo = dynamic_cast<juce::ComboBox*>(focused))
    {
        currentValue = combo->getText().isNotEmpty()
            ? combo->getText()
            : juce::String{"No selection"};
        action = talas::app::WorkbenchSurfaceActionEvidence{
            "control.choose", "Choose value", combo->isEnabled()};
    }
    else if (auto* button = dynamic_cast<juce::Button*>(focused))
    {
        currentValue = button->getToggleState()
            ? "On"
            : button->isEnabled() ? "Available" : "Unavailable";
        action = talas::app::WorkbenchSurfaceActionEvidence{
            "control.activate", "Activate", button->isEnabled()};
    }
    else if (auto* label = dynamic_cast<juce::Label*>(focused))
    {
        currentValue = label->getText();
    }

    const auto stateEvidence = (title + " " + support + " " + currentValue)
                                   .toLowerCase();
    if (!commandForIdentity.has_value()
        && authoritativeSurfaceState == nullptr)
    {
        if (stateEvidence.contains("loading")
            || stateEvidence.contains("inspecting")
            || stateEvidence.contains("searching")
            || stateEvidence.contains("exporting")
            || stateEvidence.contains("packaging")
            || stateEvidence.contains("probing")
            || stateEvidence.contains("activating")
            || stateEvidence.contains("staging"))
        {
            kind = talas::app::WorkbenchSurfaceStateKind::Loading;
        }
        else if (stateEvidence.contains("stale")
                 || stateEvidence.contains("changed on disk"))
        {
            kind = talas::app::WorkbenchSurfaceStateKind::Stale;
        }
        else if (stateEvidence.contains("unavailable")
                 || stateEvidence.contains("not prepared"))
        {
            kind = talas::app::WorkbenchSurfaceStateKind::Unavailable;
        }
        else if (stateEvidence.contains("failed")
                 || stateEvidence.contains("error")
                 || stateEvidence.contains("invalid")
                 || stateEvidence.contains("rejected"))
        {
            kind = talas::app::WorkbenchSurfaceStateKind::Error;
        }
        else if (stateEvidence.contains("completed")
                 || stateEvidence.contains("succeeded")
                 || stateEvidence.contains("saved")
                 || stateEvidence.contains("recovered"))
        {
            kind = talas::app::WorkbenchSurfaceStateKind::Success;
        }
    }

    auto state = std::optional<talas::app::WorkbenchSurfaceState>{};
    if (authoritativeSurfaceState != nullptr)
    {
        state = **authoritativeSurfaceState;
        title = juceString(state->title());
        support = juceString(state->explanation());
        currentValue = juceString(
            talas::app::workbenchSurfaceStateName(state->kind()));
        if (state->action().has_value())
        {
            next = state->action()->enabled()
                ? "Activate " + juceString(state->action()->label())
                : juceString(state->action()->label()) + " is unavailable";
        }
        else
        {
            next = keyboardAction;
        }
    }

    title = boundedText(title, 160, "Workbench control");
    support = boundedText(
        support, 640,
        "This control exposes no additional support or limitation.");
    currentValue = boundedText(currentValue, 320, "State unavailable");
    next = boundedText(
        next.isNotEmpty() ? next : keyboardAction,
        320,
        "Use the primary keyboard action");
    if (!state.has_value())
    {
        const auto retainsContent =
            kind == talas::app::WorkbenchSurfaceStateKind::Content
            || kind == talas::app::WorkbenchSurfaceStateKind::Stale
            || kind == talas::app::WorkbenchSurfaceStateKind::Error;
        state = talas::app::workbenchSurfaceState({
            kind,
            title.toStdString(),
            support.toStdString(),
            action,
            retainsContent,
        });
    }
    if (!state.has_value())
    {
        contextHelpPanel_->clearContent();
        return;
    }
    contextHelpPanel_->setContent({
        boundedText(juceString(identity->itemId), 512, "focus.unknown"),
        title,
        role,
        currentValue,
        keyboardAction,
        support,
        next,
        state,
    });
}

void WorkbenchComponent::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);
    const auto callbackStartTicks =
        juce::Time::getHighResolutionTicks();
    const auto outputFrames = static_cast<std::size_t>(juce::jmax(0, numSamples));
    for (int channel = 0; channel < numOutputChannels; ++channel)
    {
        if (outputChannelData != nullptr && outputChannelData[channel] != nullptr)
            std::fill_n(outputChannelData[channel], outputFrames, 0.0F);
    }
    std::array<const float*, 8> physicalInputChannels{};
    for (std::size_t physicalChannel = 0; physicalChannel < physicalInputChannels.size(); ++physicalChannel)
    {
        const auto compactIndex = physicalInputChannelMap_[physicalChannel];
        if (compactIndex >= 0 && compactIndex < numInputChannels && inputChannelData != nullptr)
            physicalInputChannels[physicalChannel] = inputChannelData[compactIndex];
    }
    for (std::size_t channel = 0; channel < inputMeterPeaks_.size(); ++channel)
    {
        float peak = 0.0F;
        if (const auto* samples = physicalInputChannels[channel])
        {
            for (std::size_t frame = 0; frame < outputFrames; ++frame)
                peak = std::max(peak, std::abs(samples[frame]));
        }
        inputMeterPeaks_[channel].store(peak, std::memory_order_relaxed);
    }
    std::array<float*, 8> projectOutputChannels{};
    for (std::size_t ordinal = 0; ordinal < projectOutputChannels.size(); ++ordinal)
    {
        const auto compactIndex = projectOutputChannelMap_[ordinal].load(std::memory_order_relaxed);
        if (compactIndex >= 0 && compactIndex < numOutputChannels
            && outputChannelData != nullptr)
        {
            projectOutputChannels[ordinal] = outputChannelData[compactIndex];
        }
    }
    const auto callbackTimeSeconds =
        juce::Time::getMillisecondCounterHiRes() * 0.001;
    // A Sound the Sound Browser plays takes the outputs, as the Sound
    // Library's audition does.
    const auto browserRendered = renderSoundBrowserAudition(callbackTimeSeconds, projectOutputChannels.data(),
        projectOutputChannels.size(), outputFrames, {physicalInputChannels.data(), physicalInputChannels.size()});
    const auto soundLibraryAuditionRendered = browserRendered.has_value() ? browserRendered : renderSoundLibraryAudition(
        callbackTimeSeconds,
        projectOutputChannels.data(),
        projectOutputChannels.size(),
        outputFrames,
        {physicalInputChannels.data(), physicalInputChannels.size()});
    const auto analysisAuditionRendered =
        soundLibraryAuditionRendered.has_value()
        ? std::optional<bool>{}
        : renderPartialTrackPreview(
            projectOutputChannels.data(),
            projectOutputChannels.size(),
            outputFrames);
    const auto partialTrackPreviewRendered = analysisAuditionRendered;
    const auto analysisWorkspaceAuditionRendered =
        soundLibraryAuditionRendered.has_value()
            || partialTrackPreviewRendered.has_value()
        ? std::optional<bool>{}
        : renderAnalysisAudition(
            physicalInputChannels,
            callbackTimeSeconds,
            projectOutputChannels.data(),
            projectOutputChannels.size(),
            outputFrames);
    const auto auditionRendered = soundLibraryAuditionRendered.has_value()
        ? soundLibraryAuditionRendered
        : partialTrackPreviewRendered.has_value()
            ? partialTrackPreviewRendered
            : analysisWorkspaceAuditionRendered;
    // A Timeline recorded live renders the
    // Master's channels the device lacks into the recording's own buffers,
    // and the recording hears each block as it plays.
    LiveTimelineRecordingSlot::Use liveRecording{*liveTimelineRecordingSlot_};
    auto renderChannels = projectOutputChannels;
    if (liveRecording && !auditionRendered.has_value())
        liveRecording->lendChannels(std::span<float*>{renderChannels.data(), renderChannels.size()}, outputFrames);
    const auto rendered = auditionRendered.has_value()
        ? *auditionRendered
        : projectBridge_.renderOrdered(
            {
                physicalInputChannels.data(),
                physicalInputChannels.size(),
            },
            callbackTimeSeconds,
            renderChannels.data(),
            renderChannels.size(),
            outputFrames);
    if (liveRecording && !auditionRendered.has_value())
    {
        if (rendered)
            liveRecording->publishFromAudio(renderChannels.data(), renderChannels.size(), outputFrames,
                projectBridge_.scorePlaybackFrame(), projectBridge_.activeStageId());
        else
            liveRecording->rejectRenderFailureFromAudio();
    }
    const auto activeOutputCount = soundLibraryAuditionRendered.has_value()
        ? std::min(
            browserRendered.has_value() ? soundBrowserAuditionOutputs() : soundLibraryAuditionOutputChannels_.load(
                std::memory_order_acquire),
            projectOutputChannels.size())
        : analysisAuditionRendered.has_value()
        ? std::min<std::size_t>(2, projectOutputChannels.size())
        : analysisWorkspaceAuditionRendered.has_value()
        ? std::size_t{1}
        : std::min(
              projectBridge_.activeOutputChannelCount(),
              projectOutputChannels.size());
    publishAnalysisOutputTakeFromAudio(
        projectOutputChannels.data(), outputFrames, rendered);
    // The Sample Editor's Play and the Sound
    // Browser's samples.
    mixSampleAudition(outputChannelData, numOutputChannels, outputFrames);
    // The Tape Recorder hears and records the first two inputs and plays
    // its recording back.
    tapeRecorder_.process({physicalInputChannels.data(), 2}, {projectOutputChannels.data(), 2}, outputFrames);
    outputMeterStageId_.store(0, std::memory_order_release);
    for (std::size_t ordinal = 0; ordinal < outputMeterPeaks_.size(); ++ordinal)
    {
        if (ordinal >= activeOutputCount)
        {
            outputMeterPeaks_[ordinal].store(0.0F, std::memory_order_relaxed);
            continue;
        }
        const auto* samples = projectOutputChannels[ordinal];
        if (samples == nullptr)
        {
            outputMeterPeaks_[ordinal].store(-1.0F, std::memory_order_relaxed);
            continue;
        }
        float peak = 0.0F;
        if (rendered)
        {
            for (std::size_t frame = 0; frame < outputFrames; ++frame)
                peak = std::max(peak, std::abs(samples[frame]));
        }
        outputMeterPeaks_[ordinal].store(peak, std::memory_order_relaxed);
    }
    outputMeterStageId_.store(
        auditionRendered.has_value()
            ? 0
            : projectBridge_.activeStageId(),
        std::memory_order_release);
    // The System Output Level Pad, after the meters.
    if (const auto gain = systemOutputGain_.load(std::memory_order_relaxed); gain != 1.0F && outputChannelData != nullptr)
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::multiply(outputChannelData[channel], gain, numSamples);

    const auto elapsedTicks = std::max<juce::int64>(
        0,
        juce::Time::getHighResolutionTicks() - callbackStartTicks);
    const auto ticksPerSecond =
        deviceTimingTicksPerSecond_.load(std::memory_order_relaxed);
    if (ticksPerSecond != 0)
    {
        const auto elapsedNanoseconds = static_cast<std::uint64_t>(
            std::min(
                static_cast<double>(
                    std::numeric_limits<std::uint64_t>::max()),
                static_cast<double>(elapsedTicks) * 1'000'000'000.0
                    / static_cast<double>(ticksPerSecond)));
        publishDeviceCallbackDuration(elapsedNanoseconds);
    }
}

void WorkbenchComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    analysisOutputTakeDeviceEpoch_.fetch_add(
        1, std::memory_order_acq_rel);
    requestAnalysisOutputTakeStopFromDevice();
    juce::Component::SafePointer<WorkbenchComponent> outputTakeSafeThis(this);
    juce::MessageManager::callAsync([outputTakeSafeThis] {
        if (outputTakeSafeThis != nullptr)
        {
            outputTakeSafeThis->stopAnalysisOutputTakeForLifecycle();
            outputTakeSafeThis->refreshAnalysisWorkspace();
        }
    });
    analysisAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    analysisAuditionCallback_.store(nullptr, std::memory_order_release);
    partialTrackPreviewLifecycle_.store(0, std::memory_order_seq_cst);
    partialTrackPreviewCallback_.store(nullptr, std::memory_order_release);
    projectBridge_.resetRealtimeControlTiming();
    physicalInputChannelMap_.fill(-1);
    deviceMappingAttached_.store(false, std::memory_order_release);
    for (auto& mapping : projectOutputChannelMap_)
        mapping.store(-1, std::memory_order_relaxed);
    for (auto& physical : projectOutputPhysicalChannels_)
        physical.store(-1, std::memory_order_relaxed);
    if (device == nullptr || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0)
    {
        deviceTimingTicksPerSecond_.store(0, std::memory_order_release);
        devicePeriodNanoseconds_.store(0, std::memory_order_release);
        requestDeviceMetricsReset();
        deviceGeneration_.fetch_add(1, std::memory_order_release);
        scheduleClearAfterAudioStopped();
        publishDeviceStatus("Audio device has no valid sample rate or block size");
        return;
    }

    const auto timingTicksPerSecond =
        juce::Time::getHighResolutionTicksPerSecond();
    deviceTimingTicksPerSecond_.store(
        timingTicksPerSecond > 0
            ? static_cast<std::uint64_t>(timingTicksPerSecond)
            : 0,
        std::memory_order_release);
    devicePeriodNanoseconds_.store(
        static_cast<std::uint64_t>(std::llround(
            static_cast<double>(device->getCurrentBufferSizeSamples())
            / device->getCurrentSampleRate() * 1'000'000'000.0)),
        std::memory_order_release);
    requestDeviceMetricsReset();

    const auto activeInputs = device->getActiveInputChannels();
    int compactIndex = 0;
    for (int physicalChannel = 0; physicalChannel <= activeInputs.getHighestBit(); ++physicalChannel)
    {
        if (!activeInputs[physicalChannel])
            continue;
        if (physicalChannel < static_cast<int>(physicalInputChannelMap_.size()))
            physicalInputChannelMap_[static_cast<std::size_t>(physicalChannel)] = compactIndex;
        ++compactIndex;
    }

    const auto activeOutputs = device->getActiveOutputChannels();
    std::array<std::uint8_t, talas::app::workbenchMaximumDeviceChannels>
        activePhysicalOutputs{};
    for (std::size_t physicalChannel = 0;
         physicalChannel < activePhysicalOutputs.size();
         ++physicalChannel)
    {
        activePhysicalOutputs[physicalChannel] = activeOutputs[
            static_cast<int>(physicalChannel)]
            ? 1
            : 0;
    }
    const auto& deviceSettings = projectBridge_.editor().document().deviceSettings;
    const auto savedOutputOrder = deviceSettings.configured
        ? std::span<const std::int32_t>{deviceSettings.outputChannels}
        : std::span<const std::int32_t>{};
    const auto routes = talas::app::workbenchOutputChannelRoutes(
        activePhysicalOutputs,
        savedOutputOrder);
    for (std::size_t ordinal = 0; ordinal < routes.size(); ++ordinal)
    {
        projectOutputChannelMap_[ordinal].store(
            routes[ordinal].compactDeviceIndex,
            std::memory_order_relaxed);
        projectOutputPhysicalChannels_[ordinal].store(
            routes[ordinal].oneBasedPhysicalChannel,
            std::memory_order_relaxed);
    }
    deviceMappingAttached_.store(true, std::memory_order_release);

    const auto generation = deviceGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    talas::runtime::RenderSettings settings;
    settings.sampleRate = device->getCurrentSampleRate();
    settings.maxBlockFrames = static_cast<std::size_t>(device->getCurrentBufferSizeSamples());
    const auto deviceName = juce::String(device->getName());
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, settings = std::move(settings), deviceName, generation]() mutable {
        if (safeThis != nullptr
            && safeThis->deviceGeneration_.load(std::memory_order_acquire) == generation)
        {
            safeThis->stopSoundLibraryAudition();
            safeThis->stopAnalysisAudition();
            safeThis->stopPartialTrackPreview();
            safeThis->synchronizeWorkspaceSessionVariableValues();
            safeThis->synchronizeWorkspaceSessionSoundVariableValues();
            // A device/profile replacement starts a new callback authority.
            // Reapply only the last callback-acknowledged workspace values;
            // compatible restaging within one device session transfers cells.
            safeThis->projectBridge_.resetVariableValueHistory();
            safeThis->scoreClipArtifactStateCache_.clear();
            safeThis->workspaceVariableRestorePending_ =
                !safeThis->workspaceSession_.variableValues.empty();
            safeThis->workspaceSoundVariableRestorePending_ =
                !safeThis->workspaceSession_.soundVariableValues.empty();
            safeThis->stageProject(std::move(settings), deviceName);
            safeThis->rebuildTransportSyncAuthority();
        }
    });
}

void WorkbenchComponent::audioDeviceStopped()
{
    // CoreAudio can report a lost device on its notification thread. The
    // body writes single-writer publications owned by the message thread, so
    // a stop from elsewhere runs there, unless a newer device started first.
    if (!juce::MessageManager::existsAndIsCurrentThread())
    {
        const auto generation =
            deviceGeneration_.load(std::memory_order_acquire);
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, generation] {
            if (safeThis != nullptr
                && safeThis->deviceGeneration_.load(std::memory_order_acquire)
                    == generation)
            {
                safeThis->audioDeviceStopped();
            }
        });
        return;
    }
    for (const auto source : {
             talas::runtime::RealtimeLiveSetSourceKind::Workbench,
             talas::runtime::RealtimeLiveSetSourceKind::MidiProgram,
             talas::runtime::RealtimeLiveSetSourceKind::Osc,
             talas::runtime::RealtimeLiveSetSourceKind::Tool})
    {
        projectBridge_.revokeLiveSetSourceAuthority(source);
    }
    for (const auto source : {
             talas::runtime::RealtimeLiveToolSourceKind::Workbench,
             talas::runtime::RealtimeLiveToolSourceKind::MidiProgram,
             talas::runtime::RealtimeLiveToolSourceKind::Osc})
    {
        projectBridge_.revokeLiveToolSourceAuthority(source);
    }
    outputMeterStageId_.store(0, std::memory_order_release);
    analysisOutputTakeDeviceEpoch_.fetch_add(
        1, std::memory_order_acq_rel);
    requestAnalysisOutputTakeStopFromDevice();
    soundLibraryAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    soundLibraryAuditionCallback_.store(nullptr, std::memory_order_release);
    analysisAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    analysisAuditionCallback_.store(nullptr, std::memory_order_release);
    partialTrackPreviewLifecycle_.store(0, std::memory_order_seq_cst);
    partialTrackPreviewCallback_.store(nullptr, std::memory_order_release);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis != nullptr)
        {
            safeThis->stopSoundLibraryAudition();
            safeThis->stopAnalysisAudition();
            safeThis->stopPartialTrackPreview();
            safeThis->stopAnalysisOutputTakeForLifecycle();
            safeThis->refreshAnalysisWorkspace();
        }
    });
    projectBridge_.resetRealtimeControlTiming();
    if (midiClockAuthorityGeneration_.exchange(
            0, std::memory_order_acq_rel) != 0
        || projectBridge_.midiClockTelemetry().externalAuthority)
    {
        projectBridge_.revokeMidiClockAuthorityForSourceLoss();
    }
    if (mtcAuthorityGeneration_.exchange(
            0, std::memory_order_acq_rel) != 0
        || projectBridge_.mtcTelemetry().externalAuthority)
    {
        projectBridge_.revokeMtcAuthorityForSourceLoss();
    }
    deviceTimingTicksPerSecond_.store(0, std::memory_order_release);
    devicePeriodNanoseconds_.store(0, std::memory_order_release);
    requestDeviceMetricsReset();
    deviceMappingAttached_.store(false, std::memory_order_release);
    for (auto& mapping : projectOutputChannelMap_)
        mapping.store(-1, std::memory_order_relaxed);
    for (auto& physical : projectOutputPhysicalChannels_)
        physical.store(-1, std::memory_order_relaxed);
    deviceGeneration_.fetch_add(1, std::memory_order_release);
    scheduleClearAfterAudioStopped();
}

void WorkbenchComponent::requestDeviceMetricsReset() noexcept
{
    auto generation = deviceMetricsResetGeneration_.load(
        std::memory_order_relaxed);
    while (generation != std::numeric_limits<std::uint64_t>::max()
           && !deviceMetricsResetGeneration_.compare_exchange_weak(
               generation,
               generation + 1,
               std::memory_order_release,
               std::memory_order_relaxed))
    {
    }
}

void WorkbenchComponent::publishDeviceCallbackDuration(
    std::uint64_t elapsedNanoseconds) noexcept
{
    const auto oddSequence =
        deviceMetricsPublicationSequence_.fetch_add(
            1, std::memory_order_acq_rel)
        + 1;
    const auto callbacksBeforePublication =
        deviceCallbackCount_.load(std::memory_order_relaxed);
    const auto overrunsBeforePublication =
        deviceDeadlineOverrunCount_.load(
            std::memory_order_relaxed);
    const auto requestedMetricsGeneration =
        deviceMetricsResetGeneration_.load(std::memory_order_acquire);
    const auto adoptsReset = requestedMetricsGeneration
        != adoptedDeviceMetricsResetGeneration_;
    if (adoptsReset)
    {
        adoptedDevicePeriodNanoseconds_ =
            devicePeriodNanoseconds_.load(std::memory_order_relaxed);
    }

    addSaturatedSingleWriter(deviceCallbackCount_);
    publishMaximumSingleWriter(
        deviceMaximumCallbackNanoseconds_,
        elapsedNanoseconds);
    deviceLatestCallbackNanoseconds_.store(
        elapsedNanoseconds,
        std::memory_order_relaxed);
    const auto period = adoptedDevicePeriodNanoseconds_;
    if (period != 0 && elapsedNanoseconds > period)
        addSaturatedSingleWriter(deviceDeadlineOverrunCount_);
    if (adoptsReset)
    {
        deviceBaselineCallbackCount_.store(
            callbacksBeforePublication,
            std::memory_order_relaxed);
        deviceBaselineDeadlineOverrunCount_.store(
            overrunsBeforePublication,
            std::memory_order_relaxed);
        adoptedDeviceMetricsResetGeneration_ =
            requestedMetricsGeneration;
        publishedDeviceMetricsGeneration_.store(
            requestedMetricsGeneration,
            std::memory_order_relaxed);
        publishedDevicePeriodNanoseconds_.store(
            period,
            std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
    deviceMetricsPublicationSequence_.store(
        oddSequence + 1,
        std::memory_order_release);
}

bool WorkbenchComponent::midiSourceCapacityFixtureIsValidForSmoke()
{
    const auto message = talas::project::MidiControllerMessage{
        2,
        74,
        64,
        1,
        midiInputSourceGeneration_.load(std::memory_order_acquire),
    };
    midiInputSourceCapacityExceeded_ = true;
    rebuildMidiRuntimeRoutes();
    const auto rejected = !midiRuntimeRoutes_.map(message).has_value()
        && midiRuntimeRouteDiagnostic_.contains("more than 16 inputs");

    midiInputSourceCapacityExceeded_ = false;
    rebuildMidiRuntimeRoutes();
    return rejected && midiRuntimeRoutes_.map(message).has_value()
        && midiRuntimeRouteDiagnostic_.isEmpty()
        && midiClockAuthorityFixtureIsValidForSmoke();
}

bool WorkbenchComponent::midiClockAuthorityFixtureIsValidForSmoke()
{
    if (hardwareEnabled_ || !midiInputSources_.empty())
        return false;

    MidiInputSourceRecord source;
    source.identifier = "smoke-clock-input";
    source.displayName = "Smoke Clock Input";
    source.slot = 1;
    source.generation = midiInputSourceGeneration_.load(
        std::memory_order_acquire);
    const auto sourceGeneration = source.generation;
    midiInputSources_.push_back(std::move(source));

    const auto originalSettings =
        projectBridge_.editor().document().deviceSettings;
    auto settings = originalSettings;
    settings.configured = true;
    settings.sampleRate = 48'000.0;
    settings.bufferSize = 256;
    settings.outputChannels = {1, 2};
    settings.midiInputDeviceIdentifiers.push_back(
        "smoke-clock-input");
    settings.transportSync.sourceMode =
        talas::project::TransportSyncSourceMode::MidiClockExactInput;
    settings.transportSync.sourceIdentifier = "smoke-clock-input";
    if (!projectBridge_.editor().setDeviceSettings(settings).ok())
    {
        midiInputSources_.clear();
        return false;
    }
    rebuildTransportSyncAuthority();

    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    auto start = juce::MidiMessage::midiStart();
    start.setTimeStamp(now);
    handleIncomingMidiMessageFromSource(
        1, sourceGeneration, start);
    std::array<float, 1> left{};
    std::array<float, 1> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    const auto started = projectBridge_.render(
        {}, now + 0.01, channels.data(), channels.size(), left.size());
    const auto running = projectBridge_.midiClockTelemetry().running;

    auto stop = juce::MidiMessage::midiStop();
    stop.setTimeStamp(now + 0.1);
    const auto stopsBefore = projectBridge_.midiClockTelemetry()
                                 .acceptedStops;
    handleIncomingMidiMessageFromSource(
        1, sourceGeneration, stop);
    (void)projectBridge_.render(
        {}, now + 0.11, channels.data(), channels.size(), left.size());
    const auto afterStop = projectBridge_.midiClockTelemetry();
    const auto stopProcessed = afterStop.acceptedStops > stopsBefore;
    const auto held = !afterStop.running;

    const auto restored = projectBridge_.editor()
                              .setDeviceSettings(originalSettings)
                              .ok();
    midiClockAccessibleDetail_ =
        "stale smoke exact source identifier";
    midiInputSources_.clear();
    rebuildTransportSyncAuthority();
    (void)projectBridge_.render(
        {}, now + 0.2, channels.data(), channels.size(), left.size());
    const auto valid =
        started && running && stopProcessed && held && restored
        && midiClockAccessibleDetail_.isEmpty();
    if (!valid)
    {
        midiClockDiagnostic_ =
            "Clock smoke start=" + juce::String(started ? 1 : 0)
            + " run=" + juce::String(running ? 1 : 0)
            + " stop=" + juce::String(stopProcessed ? 1 : 0)
            + " held=" + juce::String(held ? 1 : 0)
            + " restore=" + juce::String(restored ? 1 : 0);
    }
    return valid;
}

void WorkbenchComponent::MidiInputSourceCallback::handleIncomingMidiMessage(
    juce::MidiInput*,
    const juce::MidiMessage& message)
{
    if (owner != nullptr)
        owner->handleIncomingMidiMessageFromSource(slot, generation, message);
}

void WorkbenchComponent::handleIncomingMidiMessageFromSource(
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration,
    const juce::MidiMessage& message)
{
    if (sourceGeneration
        != midiInputSourceGeneration_.load(std::memory_order_acquire))
    {
        return;
    }
    // Escape-learn takes the keys and controllers to the message thread
    //; they still play.
    if (midiLearnArmed_.load(std::memory_order_acquire)
        && (message.isNoteOnOrOff() || message.isController()))
    {
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<WorkbenchComponent>(this), message] {
            if (safeThis != nullptr)
                safeThis->learnFromMidi(message);
        });
    }
    noteMidiMessageForWindow(message);
    const auto* rawData = message.getRawData();
    const auto rawSize = message.getRawDataSize();
    const auto mtcAuthority = mtcAuthorityGeneration_.load(
        std::memory_order_acquire);
    if (mtcAuthority != 0 && rawData != nullptr && rawSize > 0
        && (rawData[0] == 0xF1 || rawData[0] == 0xF0))
    {
        (void)projectBridge_.enqueueMtc(
            std::span<const std::uint8_t>{
                rawData, static_cast<std::size_t>(rawSize)},
            sourceSlot,
            sourceGeneration,
            mtcAuthority,
            juce::Time::getMillisecondCounterHiRes() * 0.001);
        return;
    }
    std::optional<talas::runtime::RealtimeControlCommandKind> clockKind;
    if (message.isMidiClock())
        clockKind = talas::runtime::RealtimeControlCommandKind::MidiClockTick;
    else if (message.isMidiStart())
        clockKind = talas::runtime::RealtimeControlCommandKind::MidiClockStart;
    else if (message.isMidiContinue())
        clockKind = talas::runtime::RealtimeControlCommandKind::MidiClockContinue;
    else if (message.isMidiStop())
        clockKind = talas::runtime::RealtimeControlCommandKind::MidiClockStop;
    else if (message.isSongPositionPointer())
        clockKind = talas::runtime::RealtimeControlCommandKind::MidiSongPosition;
    if (clockKind.has_value())
    {
        // Any input's timing clock is the Sound's
        // `MIDITimingClock and !TimingClock, whatever the transport follows.
        if (*clockKind != talas::runtime::RealtimeControlCommandKind::MidiSongPosition)
            (void)projectBridge_.enqueueMidiTimingClock(
                *clockKind,
                message.getTimeStamp() > 0.0
                    ? message.getTimeStamp()
                    : juce::Time::getMillisecondCounterHiRes() * 0.001);
        const auto authorityGeneration =
            midiClockAuthorityGeneration_.load(
                std::memory_order_acquire);
        if (authorityGeneration != 0)
        {
            (void)projectBridge_.enqueueMidiClock(
                *clockKind,
                sourceSlot,
                sourceGeneration,
                authorityGeneration,
                juce::Time::getMillisecondCounterHiRes() * 0.001,
                message.isSongPositionPointer()
                    ? static_cast<std::uint16_t>(
                          message.getSongPositionPointerMidiBeat())
                    : 0);
        }
        return;
    }
    if (message.isProgramChange())
    {
        talas::runtime::RealtimeControlCommand command;
        command.kind =
            talas::runtime::RealtimeControlCommandKind::MidiProgramChange;
        command.eventId = static_cast<std::uint32_t>(
            std::clamp(message.getProgramChangeNumber(), 0, 127) + 1);
        command.midiChannel = static_cast<std::uint8_t>(
            std::clamp(message.getChannel(), 1, 16));
        command.arrivalTimeSeconds = message.getTimeStamp() > 0.0
            ? message.getTimeStamp()
            : juce::Time::getMillisecondCounterHiRes() * 0.001;
        command.generation =
            midiUiStateToken_.load(std::memory_order_acquire);
        command.stageId =
            uiDocumentGeneration_.load(std::memory_order_acquire);
        command.midiSourceSlot = sourceSlot;
        command.midiSourceGeneration = sourceGeneration;
        (void)projectBridge_.enqueueMidiProgramChange(
            command.midiChannel,
            static_cast<int>(command.eventId - 1),
            command.arrivalTimeSeconds);
        if (!midiUiIngress_.tryPush(command)
            && sourceSlot != 0
            && sourceSlot <= midiUiDroppedBySource_.size())
        {
            addSaturatedSingleWriter(
                midiUiDroppedBySource_[sourceSlot - 1]);
        }
        return;
    }
    if (message.isNoteOn())
    {
        const auto generation = sampleMidiSelectionGeneration_.load(
            std::memory_order_acquire);
        const auto channel = std::clamp(message.getChannel(), 1, 16);
        const auto note = static_cast<std::uint8_t>(
            std::clamp(message.getNoteNumber(), 0, 127));
        const auto velocity = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(message.getVelocity()), 1, 127));
        const auto arrivalTimeSeconds = message.getTimeStamp();
        if (!sampleMidiRoutingEnabled_.load(std::memory_order_acquire))
        {
            (void)projectBridge_.enqueueMidiNoteOn(channel, note, velocity, arrivalTimeSeconds);
            forwardMidiNoteToAudition(true, channel, note, velocity, arrivalTimeSeconds);
            return;
        }
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        juce::MessageManager::callAsync(
            [safeThis,
             generation,
             channel,
             note,
             velocity,
             arrivalTimeSeconds] {
                if (safeThis != nullptr
                    && generation
                        == safeThis->sampleMidiSelectionGeneration_.load(
                            std::memory_order_acquire)
                    && safeThis->sampleMidiRoutingEnabled_.load(
                        std::memory_order_acquire))
                {
                    safeThis->handleSampleMapMidiNoteOn(
                        channel,
                        note,
                        velocity,
                        arrivalTimeSeconds);
                }
            });
        return;
    }
    if (message.isNoteOff())
    {
        const auto channel = std::clamp(message.getChannel(), 1, 16);
        const auto note = static_cast<std::uint8_t>(
            std::clamp(message.getNoteNumber(), 0, 127));
        const auto arrivalTimeSeconds = message.getTimeStamp();
        if (sampleMidiRoutingEnabled_.load(std::memory_order_acquire))
        {
            const auto generation = sampleMidiSelectionGeneration_.load(
                std::memory_order_acquire);
            juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
            juce::MessageManager::callAsync(
                [safeThis,
                 generation,
                 channel,
                 note,
                 arrivalTimeSeconds] {
                    if (safeThis != nullptr
                        && generation
                            == safeThis->sampleMidiSelectionGeneration_.load(
                                std::memory_order_acquire)
                        && safeThis->sampleMidiRoutingEnabled_.load(
                            std::memory_order_acquire))
                    {
                        safeThis->handleSampleMapMidiNoteOff(
                            channel,
                            note,
                            arrivalTimeSeconds);
                    }
                });
            return;
        }
        (void)projectBridge_.enqueueMidiNoteOff(channel, note, arrivalTimeSeconds);
        forwardMidiNoteToAudition(false, channel, note, 0, arrivalTimeSeconds);
        return;
    }
    if (message.isChannelPressure())
    {
        (void)projectBridge_.enqueueMidiChannelPressure(
            message.getChannel(), message.getChannelPressureValue(),
            message.getTimeStamp() > 0.0
                ? message.getTimeStamp()
                : juce::Time::getMillisecondCounterHiRes() * 0.001);
        return;
    }
    if (message.isPitchWheel())
    {
        scrubSpectraForPitchBend(message.getPitchWheelValue());
        (void)projectBridge_.enqueueMidiPitchBend(
            message.getChannel(), message.getPitchWheelValue(),
            message.getTimeStamp() > 0.0
                ? message.getTimeStamp()
                : juce::Time::getMillisecondCounterHiRes() * 0.001);
        return;
    }
    if (!message.isController())
        return;

    const talas::project::MidiControllerMessage controllerMessage{
        message.getChannel(),
        message.getControllerNumber(),
        message.getControllerValue(),
        sourceSlot,
        sourceGeneration,
    };
    const auto controllerArrivalTimeSeconds = message.getTimeStamp() > 0.0
        ? message.getTimeStamp()
        : juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto builtInAccepted = projectBridge_.enqueueMidiControlChange(
        controllerMessage.channel,
        controllerMessage.controller,
        controllerMessage.value,
        controllerArrivalTimeSeconds,
        sourceGeneration);
    const auto midiUiStateToken =
        midiUiStateToken_.load(std::memory_order_acquire);

    talas::runtime::RealtimeControlCommand uiCommand;
    uiCommand.eventId = static_cast<std::uint32_t>(
        (controllerMessage.channel - 1) * 128 + controllerMessage.controller + 1);
    uiCommand.value = static_cast<double>(controllerMessage.value);
    uiCommand.arrivalTimeSeconds = controllerArrivalTimeSeconds;
    uiCommand.generation = midiUiStateToken;
    uiCommand.stageId = uiDocumentGeneration_.load(std::memory_order_acquire);
    uiCommand.midiSourceSlot = sourceSlot;
    uiCommand.midiSourceGeneration = sourceGeneration;
    uiCommand.builtInMidiControllerAccepted = builtInAccepted;
    if (!midiUiIngress_.tryPush(uiCommand)
        && sourceSlot != 0
        && sourceSlot <= midiUiDroppedBySource_.size())
    {
        addSaturatedSingleWriter(
            midiUiDroppedBySource_[sourceSlot - 1]);
    }
}

std::uint64_t
WorkbenchComponent::invalidateMidiInputSourceRegistryState() noexcept
{
    projectBridge_.revokeLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::MidiProgram);
    projectBridge_.revokeLiveToolSourceAuthority(
        talas::runtime::RealtimeLiveToolSourceKind::MidiProgram);
    const auto generation = advanceWorkbenchAuthorityGeneration(
        midiInputSourceGeneration_);
    if (virtualControlSurface_ != nullptr)
        virtualControlSurface_->cancelMidiLearn();
    return generation;
}

void WorkbenchComponent::rebuildMidiInputSourceRegistry()
{
    const auto generation = invalidateMidiInputSourceRegistryState();
    if (midiClockAuthorityGeneration_.exchange(
            0, std::memory_order_acq_rel) != 0
        || projectBridge_.midiClockTelemetry().externalAuthority)
    {
        projectBridge_.revokeMidiClockAuthorityForSourceLoss();
    }
    if (mtcAuthorityGeneration_.exchange(
            0, std::memory_order_acq_rel) != 0
        || projectBridge_.mtcTelemetry().externalAuthority)
    {
        projectBridge_.revokeMtcAuthorityForSourceLoss();
    }

    for (auto& source : midiInputSources_)
    {
        if (source.callback != nullptr)
        {
            deviceManager_.removeMidiInputDeviceCallback(
                source.identifier,
                source.callback.get());
        }
    }
    midiInputSources_.clear();
    projectBridge_.resetRealtimeControlTiming();
    midiInputSourceRegistryFailed_ = false;

    try
    {
        auto available = juce::MidiInput::getAvailableDevices();
        std::ranges::sort(available, {}, [](const auto& device) {
            return device.identifier;
        });
        const auto enabledCount = std::ranges::count_if(
            available,
            [&](const auto& device) {
                return deviceManager_.isMidiInputDeviceEnabled(
                    device.identifier);
            });
        midiInputSourceCapacityExceeded_ = enabledCount > 16;
        if (midiInputSourceCapacityExceeded_)
        {
            rebuildMidiRuntimeRoutes(false);
            rebuildTransportSyncAuthority();
            if (virtualControlSurface_ != nullptr)
                virtualControlSurface_->refresh();
            return;
        }
        std::vector<MidiInputSourceRecord> candidate;
        candidate.reserve(static_cast<std::size_t>(enabledCount));
        for (const auto& device : available)
        {
            if (!deviceManager_.isMidiInputDeviceEnabled(
                    device.identifier))
            {
                continue;
            }
            MidiInputSourceRecord source;
            source.identifier = device.identifier;
            source.displayName = device.name;
            source.slot = static_cast<std::uint8_t>(
                candidate.size() + 1);
            source.generation = generation;
            source.callback =
                std::make_unique<MidiInputSourceCallback>();
            source.callback->owner = this;
            source.callback->slot = source.slot;
            source.callback->generation = generation;
            candidate.push_back(std::move(source));
        }

        std::size_t registeredCount = 0;
        try
        {
            while (registeredCount < candidate.size())
            {
                const auto index = registeredCount++;
                const auto& source = candidate[index];
                deviceManager_.addMidiInputDeviceCallback(
                    source.identifier,
                    source.callback.get());
            }
        }
        catch (...)
        {
            for (std::size_t index = 0;
                 index < registeredCount;
                 ++index)
            {
                deviceManager_.removeMidiInputDeviceCallback(
                    candidate[index].identifier,
                    candidate[index].callback.get());
            }
            throw;
        }
        midiInputSources_.swap(candidate);
    }
    catch (...)
    {
        midiInputSources_.clear();
        midiInputSourceCapacityExceeded_ = false;
        midiInputSourceRegistryFailed_ = true;
    }
    rebuildMidiRuntimeRoutes(false);
    rebuildTransportSyncAuthority();
    if (virtualControlSurface_ != nullptr)
        virtualControlSurface_->refresh();
}

bool WorkbenchComponent::usesExternalMidiClock() const noexcept
{
    return projectBridge_.editor().document().deviceSettings
               .transportSync.sourceMode
        == talas::project::TransportSyncSourceMode::MidiClockExactInput;
}

bool WorkbenchComponent::usesExternalMtc() const noexcept
{
    return projectBridge_.editor().document().deviceSettings
               .transportSync.sourceMode
        == talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput;
}

void WorkbenchComponent::rebuildTransportSyncAuthority()
{
    const auto& sync = projectBridge_.editor().document()
                           .deviceSettings.transportSync;
    if (sync.sourceMode
            != talas::project::TransportSyncSourceMode::Internal)
    {
        stopSoundLibraryAudition();
        stopAnalysisAudition();
    }
    const auto clockWasPublished = midiClockAuthorityGeneration_.exchange(
        0, std::memory_order_acq_rel);
    const auto mtcWasPublished = mtcAuthorityGeneration_.exchange(
        0, std::memory_order_acq_rel);
    if (clockWasPublished != 0
        || projectBridge_.midiClockTelemetry().externalAuthority)
    {
        projectBridge_.configureMidiClockAuthority(0);
    }
    if (mtcWasPublished != 0
        || projectBridge_.mtcTelemetry().externalAuthority)
    {
        projectBridge_.configureMtcAuthority(0);
    }
    if (sync.sourceMode == talas::project::TransportSyncSourceMode::Internal)
    {
        midiClockDiagnostic_.clear();
        midiClockAccessibleDetail_.clear();
        latestMidiClockTelemetry_ = projectBridge_.midiClockTelemetry();
        latestMtcTelemetry_ = projectBridge_.mtcTelemetry();
        commandManager_.commandStatusChanged();
        refreshPersistentStatus();
        return;
    }

    const auto source = std::ranges::find_if(
        midiInputSources_,
        [&](const auto& candidate) {
            return candidate.identifier.toStdString()
                == sync.sourceIdentifier;
        });
    const auto mtcSelected = sync.sourceMode
        == talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput;
    const auto authorityName = mtcSelected
        ? juce::String{"MTC"}
        : juce::String{"MIDI Clock"};
    if (midiInputSourceCapacityExceeded_
        || midiInputSourceRegistryFailed_
        || source == midiInputSources_.end())
    {
        midiClockDiagnostic_ = midiInputSourceCapacityExceeded_
            ? authorityName + " unavailable: more than 16 enabled inputs"
            : (midiInputSourceRegistryFailed_
                   ? authorityName + " unavailable: input registry failed"
                   : authorityName + " waiting for its saved input");
        midiClockAccessibleDetail_ =
            "External " + authorityName + " exact source identifier "
            + juceString(sync.sourceIdentifier) + ". "
            + midiClockDiagnostic_
            + (mtcSelected
                   ? ". SMPTE position follows MTC; audio is not time-stretched."
                   : ". Musical phase follows the external clock; audio is not time-stretched.");
        latestMidiClockTelemetry_ = projectBridge_.midiClockTelemetry();
        latestMtcTelemetry_ = projectBridge_.mtcTelemetry();
        commandManager_.commandStatusChanged();
        refreshPersistentStatus();
        return;
    }

    if (mtcSelected)
    {
        auto generation = nextMtcAuthorityGeneration_++;
        if (generation == 0)
        {
            generation = 1;
            nextMtcAuthorityGeneration_ = 2;
        }
        projectBridge_.configureMtcAuthority(
            generation, source->slot, source->generation);
        mtcAuthorityGeneration_.store(generation, std::memory_order_release);
        midiClockDiagnostic_ = juceString("MTC ready · ") + source->displayName;
    }
    else
    {
        auto generation = nextMidiClockAuthorityGeneration_++;
        if (generation == 0)
        {
            generation = 1;
            nextMidiClockAuthorityGeneration_ = 2;
        }
        projectBridge_.configureMidiClockAuthority(
            generation, source->slot, source->generation);
        midiClockAuthorityGeneration_.store(
            generation, std::memory_order_release);
        midiClockDiagnostic_ = juceString("MIDI Clock ready · ") + source->displayName;
    }
    midiClockAccessibleDetail_ =
        "External " + authorityName + " exact source identifier "
        + juceString(sync.sourceIdentifier) + ". "
        + midiClockDiagnostic_
        + (mtcSelected
               ? ". SMPTE position follows MTC; audio is not time-stretched."
               : ". Musical phase follows the external clock; audio is not time-stretched.");
    latestMidiClockTelemetry_ = projectBridge_.midiClockTelemetry();
    latestMtcTelemetry_ = projectBridge_.mtcTelemetry();
    commandManager_.commandStatusChanged();
    refreshPersistentStatus();
}

void WorkbenchComponent::setTransportSyncSource(
    talas::project::ProjectTransportSyncSettings sync)
{
    auto settings = projectBridge_.editor().document().deviceSettings;
    const auto external = sync.sourceMode
        != talas::project::TransportSyncSourceMode::Internal;
    if (external)
    {
        if (sync.sourceIdentifier.empty())
        {
            setStatus("Select an exact MIDI input for external transport sync");
            return;
        }
        if (!settings.configured)
        {
            auto* device = deviceManager_.getCurrentAudioDevice();
            if (device == nullptr
                || device->getCurrentSampleRate() <= 0.0
                || device->getCurrentBufferSizeSamples() <= 0)
            {
                setStatus(
                    "Select an available audio device before external transport sync");
                return;
            }
            const auto setup = deviceManager_.getAudioDeviceSetup();
            settings.configured = true;
            settings.audioInputDeviceName =
                setup.inputDeviceName.toStdString();
            settings.audioOutputDeviceName =
                setup.outputDeviceName.toStdString();
            settings.sampleRate = device->getCurrentSampleRate();
            settings.bufferSize = static_cast<std::int32_t>(
                device->getCurrentBufferSizeSamples());
            for (int channel = 0; channel < 8; ++channel)
            {
                if (setup.inputChannels[channel])
                    settings.inputChannels.push_back(channel + 1);
                if (setup.outputChannels[channel])
                    settings.outputChannels.push_back(channel + 1);
            }
        }
        settings.transportSync = sync;
        if (std::ranges::find(
                settings.midiInputDeviceIdentifiers,
                sync.sourceIdentifier)
            == settings.midiInputDeviceIdentifiers.end())
        {
            settings.midiInputDeviceIdentifiers.push_back(
                sync.sourceIdentifier);
        }
    }
    else
    {
        settings.transportSync.sourceMode =
            talas::project::TransportSyncSourceMode::Internal;
        settings.transportSync.sourceIdentifier.clear();
    }

    const auto result = projectBridge_.editor().setDeviceSettings(
        std::move(settings));
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged(false);
    rebuildTransportSyncAuthority();
    switch (sync.sourceMode)
    {
    case talas::project::TransportSyncSourceMode::MidiClockExactInput:
        setStatus("External MIDI Clock selected");
        break;
    case talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput:
        setStatus("External MTC selected");
        break;
    case talas::project::TransportSyncSourceMode::Internal:
    default:
        setStatus("Internal transport selected");
        break;
    }
}

juce::String WorkbenchComponent::setOscIngressSettings(
    talas::project::ProjectOscIngressSettings oscSettings)
{
    if (!oscSettings.enabled)
        oscSettings.port = talas::project::defaultOscIngressPort;
    return setOscLoopbackProfileSettings(
        oscSettings,
        projectBridge_.editor().document().deviceSettings.oscEgress);
}

juce::String WorkbenchComponent::setOscEgressSettings(
    talas::project::ProjectOscEgressSettings oscSettings)
{
    if (!oscSettings.enabled)
        oscSettings.port = talas::project::defaultOscEgressPort;
    return setOscLoopbackProfileSettings(
        projectBridge_.editor().document().deviceSettings.oscIngress,
        oscSettings);
}

juce::String WorkbenchComponent::setOscLoopbackProfileSettings(
    talas::project::ProjectOscIngressSettings ingress,
    talas::project::ProjectOscEgressSettings egress)
{
    if (!talas::project::isValidOscEndpointPair(ingress, egress))
    {
        const juce::String message =
            "Invalid OSC loopback profile; enabled ports must differ";
        setStatus(message);
        return message;
    }
    auto settings = projectBridge_.editor().document().deviceSettings;
    if (settings.oscIngress != ingress || settings.oscEgress != egress)
    {
        suspendOscLoopbackProfile();
        settings.oscIngress = ingress;
        settings.oscEgress = egress;
        const auto result = projectBridge_.editor().setDeviceSettings(
            std::move(settings));
        if (!result.ok())
        {
            rebuildOscIngress();
            const auto message = juceString(result.message);
            setStatus(message);
            return message;
        }
        documentChanged(false);
    }
    else
    {
        rebuildOscIngress();
    }

    latestOscLoopbackTelemetry_ = oscLoopbackProfile_.telemetry();
    if (!ingress.enabled && !egress.enabled)
    {
        setStatus("OSC loopback profile disabled");
        return "OSC loopback profile disabled";
    }
    if (latestOscLoopbackTelemetry_.state
            == WorkbenchOscLoopbackProfileState::Ready
        && latestOscLoopbackTelemetry_.activeIngress == ingress
        && latestOscLoopbackTelemetry_.activeEgress == egress)
    {
        const auto message = juceString("OSC loopback profile ready · generation ")
            + juce::String(static_cast<juce::int64>(
                latestOscLoopbackTelemetry_.profileGeneration));
        setStatus(message);
        return message;
    }
    const auto failure = oscIngressDiagnostic_.isNotEmpty()
        ? oscIngressDiagnostic_
        : oscEgressDiagnostic_.isNotEmpty()
            ? oscEgressDiagnostic_
            : juce::String{"OSC loopback profile could not start"};
    setStatus(failure);
    return failure;
}

std::string_view WorkbenchComponent::midiSourceIdentifier(
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration) const noexcept
{
    if (sourceSlot == 0
        || sourceGeneration
            != midiInputSourceGeneration_.load(std::memory_order_acquire))
    {
        return {};
    }
    const auto found = std::ranges::find_if(
        midiInputSources_,
        [&](const auto& source) {
            return source.slot == sourceSlot
                && source.generation == sourceGeneration;
        });
    if (found == midiInputSources_.end())
    {
        if (sourceSlot == 1 && !midiSmokeSourceIdentifier_.empty())
            return midiSmokeSourceIdentifier_;
        return {};
    }
    return {
        found->identifier.toRawUTF8(),
        static_cast<std::size_t>(found->identifier.getNumBytesAsUTF8())};
}

std::optional<std::string>
WorkbenchComponent::midiInputSourceDisplayName(
    std::string_view identifier) const
{
    if (identifier == midiSmokeSourceIdentifier_)
        return "Smoke MIDI Input";
    const auto source = std::ranges::find_if(
        midiInputSources_,
        [&](const auto& candidate) {
            return candidate.identifier.toStdString() == identifier;
        });
    return source == midiInputSources_.end()
        ? std::nullopt
        : std::optional<std::string>{source->displayName.toStdString()};
}

[[nodiscard]] juce::String WorkbenchComponent::outputMappingStatus(std::size_t requiredChannels) const
{
    const auto boundedRequired = std::min(requiredChannels, projectOutputChannelMap_.size());
    std::size_t mapped = 0;
    for (std::size_t channel = 0; channel < boundedRequired; ++channel)
        mapped += projectOutputChannelMap_[channel].load(std::memory_order_relaxed) >= 0 ? 1U : 0U;
    auto status = juceString(" · ")
        + juce::String(static_cast<int>(requiredChannels))
        + " output";
    if (requiredChannels != 1)
        status += "s";
    if (mapped < requiredChannels)
    {
        status += " (" + juce::String(static_cast<int>(mapped))
            + " mapped; " + juce::String(static_cast<int>(requiredChannels - mapped))
            + " discarded)";
    }
    return status;
}

void WorkbenchComponent::refreshPreparedGraphPresentation()
{
    ++preparedPresentationRefreshCountForSmoke_;
    preparedPresentationDocumentStaged_ =
        currentDocumentStaged_;
    const auto& document = projectBridge_.editor().document();
    auto inspectorContract = preparedInspectorContract(
        document,
        activeGraphContext_,
        selectedNodeId_,
        latestPreparedGraphStatus_);
    if (latestPreparedGraphStatus_.prepared != nullptr
        && !currentDocumentStaged_)
    {
        inspectorContract =
            (latestPreparedGraphStatus_.activationRejected
                ? "Score change has not been applied. Previous audio continues.\n"
                : "Active prepared graph is from the last compiled project state.\n")
            + inspectorContract;
    }
    inspectorPanel_->setPreparedContract(
        std::move(inspectorContract));
    analysisPanel_->setPreparedStatus(
        preparedAnalysisSummary(
            document,
            activeGraphContext_,
            selectedNodeId_,
            latestPreparedGraphStatus_));
    refreshRuntimeDiagnostics();

    refreshPreparedProjectStatusPresentation();
}

void WorkbenchComponent::refreshPreparedGraphLivePresentation()
{
    const auto& document = projectBridge_.editor().document();
    if (preparedSelectionTargetsStream(
            document,
            activeGraphContext_,
            selectedNodeId_,
            latestPreparedGraphStatus_))
    {
        auto inspectorContract = preparedInspectorContract(
            document,
            activeGraphContext_,
            selectedNodeId_,
            latestPreparedGraphStatus_);
        if (latestPreparedGraphStatus_.prepared != nullptr
            && !currentDocumentStaged_)
        {
            inspectorContract =
                (latestPreparedGraphStatus_.activationRejected
                    ? "Score change has not been applied. Previous audio continues.\n"
                    : "Active prepared graph is from the last compiled project state.\n")
                + inspectorContract;
        }
        inspectorPanel_->setPreparedContract(
            std::move(inspectorContract));
    }
    refreshRuntimeDiagnostics();
    refreshPreparedProjectStatusPresentation();
}

void WorkbenchComponent::refreshRuntimeDiagnostics()
{
    oscLoopbackProfile_.refreshTelemetry();
    latestOscLoopbackTelemetry_ = oscLoopbackProfile_.telemetry();
    latestOscIngressTelemetry_ = latestOscLoopbackTelemetry_.ingress;
    latestOscEgressTelemetry_ = latestOscLoopbackTelemetry_.egress;
    auto diagnostics = preparedRuntimeDiagnostics(
        latestPreparedGraphStatus_,
        latestRealtimeMetrics_);
    const auto records = oscIngress_.recentRecords();
    const auto egressRecords = oscEgress_.recentRecords();
    std::uint64_t midiDroppedRecords = 0;
    for (const auto& dropped : midiUiDroppedBySource_)
    {
        midiDroppedRecords = talas::runtime::realtimeMetricAddSaturated(
            midiDroppedRecords,
            dropped.load(std::memory_order_acquire));
    }
    const auto snapshot = makeWorkbenchAnalysisDiagnosticsSnapshot(
        latestPreparedGraphStatus_,
        latestRealtimeMetrics_,
        projectBridge_.preparedNodeTiming(),
        presentedDeviceCallbackNanoseconds_,
        assetRecoveryCoordinator_.state(),
        analysisPanel_->eventMonitorSnapshot(),
        std::span<const talas::app::AnalysisMonitorRecordInput>{
            midiMonitorRecords_.data(),
            midiMonitorRecordCount_},
        nextMidiMonitorSequence_,
        midiDroppedRecords,
        latestOscIngressTelemetry_,
        records,
        oscIngress_.routeGeneration(),
        latestOscEgressTelemetry_,
        egressRecords,
        oscEgress_.routeGeneration());
    appendWorkbenchAnalysisDiagnostics(
        diagnostics,
        talas::app::buildAnalysisDiagnosticsProjection(snapshot));
    diagnosticsPanel_->setRuntimeDiagnostics(std::move(diagnostics));
}

void WorkbenchComponent::refreshPreparedProjectStatusPresentation()
{
    const auto nextHeadline =
        latestPreparedGraphStatus_.prepared == nullptr
        ? juce::String{}
        : preparedStatusHeadline(
              latestPreparedGraphStatus_);
    const auto nextAccessible =
        latestPreparedGraphStatus_.prepared == nullptr
        ? juce::String{}
        : preparedAccessibleSummary(
              latestPreparedGraphStatus_);
    const auto projectStatusChanged =
        nextHeadline != preparedProjectStatusHeadline_
        || nextAccessible != preparedAccessibleStatus_;
    preparedProjectStatusHeadline_ = nextHeadline;
    preparedAccessibleStatus_ = nextAccessible;
    if (projectStatusChanged)
        refreshPersistentStatus();
}

void WorkbenchComponent::refreshPreparedGraphStatus()
{
    auto nextMetrics = projectBridge_.realtimeMetrics();
    if (nextMetrics.epoch != latestRealtimeMetrics_.epoch)
        requestDeviceMetricsReset();
    const auto requestedDeviceGeneration =
        deviceMetricsResetGeneration_.load(std::memory_order_acquire);
    auto nextDeviceCallbackNanoseconds =
        presentedDeviceCallbackNanoseconds_;
    for (std::size_t attempt = 0; attempt < 2; ++attempt)
    {
        const auto sequence =
            deviceMetricsPublicationSequence_.load(
                std::memory_order_acquire);
        if ((sequence & 1U) != 0U)
            continue;
        const auto publishedGeneration =
            publishedDeviceMetricsGeneration_.load(
                std::memory_order_relaxed);
        const auto callbackCount =
            deviceCallbackCount_.load(std::memory_order_relaxed);
        const auto callbackBaseline =
            deviceBaselineCallbackCount_.load(
                std::memory_order_relaxed);
        const auto overrunCount =
            deviceDeadlineOverrunCount_.load(
                std::memory_order_relaxed);
        const auto overrunBaseline =
            deviceBaselineDeadlineOverrunCount_.load(
                std::memory_order_relaxed);
        const auto maximumNanoseconds =
            deviceMaximumCallbackNanoseconds_.load(
                std::memory_order_relaxed);
        const auto latestNanoseconds =
            deviceLatestCallbackNanoseconds_.load(
                std::memory_order_relaxed);
        const auto periodNanoseconds =
            publishedDevicePeriodNanoseconds_.load(
                std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto confirmedSequence =
            deviceMetricsPublicationSequence_.load(
                std::memory_order_relaxed);
        if (sequence != confirmedSequence
            || (confirmedSequence & 1U) != 0U)
        {
            continue;
        }
        if (publishedGeneration == requestedDeviceGeneration
            && periodNanoseconds != 0)
        {
            nextMetrics.deviceDeadlineAvailable = 1;
            nextMetrics.deviceCallbacks =
                talas::runtime::realtimeMetricDelta(
                    callbackCount, callbackBaseline);
            nextMetrics.deviceDeadlineOverruns =
                talas::runtime::realtimeMetricDelta(
                    overrunCount, overrunBaseline);
            nextMetrics.deviceMaximumCallbackNanoseconds =
                maximumNanoseconds;
            nextMetrics.devicePeriodNanoseconds =
                periodNanoseconds;
            nextDeviceCallbackNanoseconds = latestNanoseconds;
        }
        break;
    }
    const auto realtimeMetricsChanged =
        !sameRealtimeMetricsPresentation(
            latestRealtimeMetrics_, nextMetrics)
        || nextDeviceCallbackNanoseconds
            != presentedDeviceCallbackNanoseconds_;
    latestRealtimeMetrics_ = nextMetrics;
    presentedDeviceCallbackNanoseconds_ =
        nextDeviceCallbackNanoseconds;

    talas::runtime::PreparedGraphStatusSnapshot nextStatus;
    if (deviceMappingAttached_.load(std::memory_order_acquire))
    {
        std::array<
            talas::runtime::PreparedGraphStatusSnapshot::OutputDeviceRoute,
            talas::app::workbenchMaximumDeviceChannels>
            routes{};
        for (std::size_t ordinal = 0;
             ordinal < routes.size();
             ++ordinal)
        {
            routes[ordinal].oneBasedPhysicalChannel =
                projectOutputPhysicalChannels_[ordinal].load(
                    std::memory_order_relaxed);
            routes[ordinal].available =
                projectOutputChannelMap_[ordinal].load(
                    std::memory_order_relaxed)
                >= 0;
        }
        nextStatus = projectBridge_.preparedGraphStatus(routes);
    }
    else
    {
        nextStatus = projectBridge_.preparedGraphStatus();
    }
    if (nextStatus.activationRejected && currentDocumentStaged_)
    {
        currentDocumentStaged_ = false;
        analysisPanel_->setCapture({}, 0.0);
        virtualControlSurface_->refresh();
        setStatus("Score change not applied: playback advanced. Previous audio continues; press Retry to apply the change.");
    }
    const auto rejectionChanged = latestPreparedGraphStatus_.activationRejected
        != nextStatus.activationRejected;
    const auto staticStatusChanged =
        !samePreparedStaticStatus(
            latestPreparedGraphStatus_,
            nextStatus);
    const auto documentStageChanged =
        preparedPresentationDocumentStaged_
        != currentDocumentStaged_;
    const auto streamHealthChanged =
        !sameRealtimeStreamHealth(
            latestPreparedGraphStatus_.streamHealth,
            nextStatus.streamHealth);
    latestPreparedGraphStatus_ = std::move(nextStatus);
    playButton_.setButtonText(latestPreparedGraphStatus_.activationRejected ? "Retry" : "Play");
    if (rejectionChanged)
        commandManager_.commandStatusChanged();
    if (staticStatusChanged || documentStageChanged)
        refreshPreparedGraphPresentation();
    else if (streamHealthChanged || realtimeMetricsChanged)
        refreshPreparedGraphLivePresentation();
}

void WorkbenchComponent::refreshOutputMeterLabel()
{
    const auto soundLibraryActive = soundLibraryAuditionActive_ != nullptr;
    auto channelCount = std::min(
        soundLibraryActive
            ? soundLibraryAuditionOutputChannels_.load(std::memory_order_acquire)
            : projectBridge_.activeOutputChannelCount(),
        outputMeterPeaks_.size());
    if (!soundLibraryActive && channelCount == 0
        && latestPreparedGraphStatus_.prepared != nullptr
        && !latestPreparedGraphStatus_.activationRejected
        && !latestPreparedGraphStatus_.awaitingActivation)
    {
        channelCount = std::min(
            latestPreparedGraphStatus_.prepared->outputs.size(),
            outputMeterPeaks_.size());
    }
    if (channelCount == 0)
    {
        outputMeterLabel_.setText(
            "Out inactive",
            juce::dontSendNotification);
        outputMeterLabel_.setTitle(
            "Output meters and prepared device routes: no active output");
        return;
    }
    juce::String text{"Out"};
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto peak = outputMeterPeaks_[channel].load(std::memory_order_relaxed);
        text += " " + juce::String(static_cast<int>(channel + 1));
        if (latestPreparedGraphStatus_.deviceMappingAttached
            && channel
                < latestPreparedGraphStatus_
                      .outputDeviceRoutes.size())
        {
            const auto& route = latestPreparedGraphStatus_
                .outputDeviceRoutes[channel];
            if (route.oneBasedPhysicalChannel > 0)
            {
                text += "@D"
                    + juce::String(
                        route.oneBasedPhysicalChannel);
                if (!route.available)
                    text += "-off";
            }
            else
                text += "@none";
        }
        text += ":";
        if (peak < 0.0F)
            text += "X";
        else if (peak <= 1.0e-6F)
            text += "-inf";
        else
            text += juce::String(20.0 * std::log10(static_cast<double>(peak)), 1);
    }
    const auto& streamHealth =
        latestPreparedGraphStatus_.streamHealth;
    if (streamHealth.consumerCount > 0)
    {
        text += juceString(" · Stream")
            + juce::String(static_cast<int>(
                streamHealth.consumerCount));
        bool hasIssue = false;
        if (streamHealth.totals.decodeFailures > 0)
        {
            hasIssue = true;
            text += " E:"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.decodeFailures));
        }
        if (streamHealth.totals.underrunEvents > 0)
        {
            hasIssue = true;
            text += " U:"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.underrunEvents))
                + "/"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.missingFrames));
        }
        if (streamHealth.totals.requestRejections > 0
            || streamHealth.totals.responseQueueFull > 0)
        {
            hasIssue = true;
            text += " Q:"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.requestRejections))
                + "/"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.responseQueueFull));
        }
        if (streamHealth.totals.staleResponseBlocks > 0)
        {
            hasIssue = true;
            text += " St:"
                + juce::String(static_cast<juce::int64>(
                    streamHealth.totals.staleResponseBlocks));
        }
        if (!hasIssue)
            text += " ok";
    }
    outputMeterLabel_.setText(text, juce::dontSendNotification);
    outputMeterLabel_.setTitle(
        "Output meters and prepared device routes: " + text);
}

void WorkbenchComponent::timerCallback()
{
    variablePresentationTick_ = static_cast<std::uint8_t>(
        (variablePresentationTick_ + 1U) % 6U);
    if (variablePresentationTick_ == 0)
    {
        synchronizeWorkspaceSessionVariableValues();
        synchronizeWorkspaceSessionSoundVariableValues();
        inspectorPanel_->refreshVariableValues();
    }
    finishSoundLibraryAuditionIfReady();
    finishAnalysisAuditionIfReady();
    finishPartialTrackPreviewIfReady();
    finishPartialTrackDerivativeIfReady();
    refreshPartialTrackOperationState();
    finishComponentReplacementIfReady();
    retryPendingSampleCleanup();
    finishSampleMapImportIfReady();
    finishSampleAssetDispositionIfReady();
    finishSampleWaveformIfReady();
    finishSampleMapAuditionIfReady();
    finishSampleMapAuditionRestoreIfReady();
    if (sampleImportPresentation_
        == SampleMapImportPresentation::Running)
    {
        refreshSampleMapWorkspace();
    }
    if (workspaceSessionWriteCoalescer_.consumeIfDue(
            juce::Time::getMillisecondCounter()))
    {
        const auto warning = persistCurrentWorkspaceSession();
        if (warning.isNotEmpty())
            setStatus(warning);
    }
    finishRecoveryWriteIfReady();
    startRecoveryWriteIfDue();
    finishAssetRecoveryWorkerIfReady();
    pollAssetRecoveryProgress();
    processPendingMidiControllers();
    processPendingOscPerformanceControls();
    oscLoopbackProfile_.refreshTelemetry();
    const auto nextOscProfileTelemetry =
        oscLoopbackProfile_.telemetry();
    const auto oscLivePresentationChanged =
        nextOscProfileTelemetry.ingress
                != latestOscLoopbackTelemetry_.ingress
        || nextOscProfileTelemetry.egress
                != latestOscLoopbackTelemetry_.egress;
    latestOscLoopbackTelemetry_ = nextOscProfileTelemetry;
    latestOscIngressTelemetry_ = nextOscProfileTelemetry.ingress;
    latestOscEgressTelemetry_ = nextOscProfileTelemetry.egress;
    if (oscLivePresentationChanged)
    {
        refreshRuntimeDiagnostics();
        refreshPersistentStatus();
    }
    const auto midiClockTelemetry =
        projectBridge_.midiClockTelemetry();
    const auto mtcTelemetry = projectBridge_.mtcTelemetry();
    const auto musicalClockSnapshot =
        projectBridge_.musicalTransportSnapshot();
    if (usesExternalMtc())
    {
        const auto previousDiagnostic = midiClockDiagnostic_;
        const auto& sync = projectBridge_.editor().document()
                               .deviceSettings.transportSync;
        const auto exactSourceIdentifier =
            juceString(sync.sourceIdentifier);
        const auto source = std::ranges::find_if(
            midiInputSources_,
            [&](const auto& candidate) {
                return candidate.identifier == exactSourceIdentifier;
            });
        const auto sourceName = source != midiInputSources_.end()
            ? source->displayName
            : juce::String{"saved input"};
        const auto coherentMtc = mtcTelemetry.externalAuthority
            && musicalClockSnapshot.publicationValid
            && musicalClockSnapshot.mtcAuthority
            && musicalClockSnapshot.mtcAuthorityGeneration
                == mtcTelemetry.authorityGeneration
            && musicalClockSnapshot.mtc.identity.authorityGeneration
                == mtcTelemetry.authorityGeneration
            && musicalClockSnapshot.mtc.identity.stageId
                == musicalClockSnapshot.mtcStageId;
        const auto mtcRunning = coherentMtc && mtcTelemetry.running;
        const auto playingChanged = transportPlaying_ != mtcRunning;
        transportPlaying_ = mtcRunning;
        if (playingChanged)
        {
            analysisPanel_->setPlaying(transportPlaying_);
            setStatus(transportPlaying_
                ? "External MTC acquired transport"
                : "External MTC held transport");
            commandManager_.commandStatusChanged();
        }

        if (!mtcTelemetry.externalAuthority)
        {
            midiClockDiagnostic_ =
                "MTC waiting for its saved input";
        }
        else if (!coherentMtc)
        {
            midiClockDiagnostic_ =
                juceString("MTC evidence unavailable · transport held");
        }
        else if (mtcTelemetry.follower.discontinuityReason
                     == talas::runtime::MtcDiscontinuityReason::EvidenceRejected
                 || mtcTelemetry.follower.invalidMessages
                     > latestMtcTelemetry_.follower.invalidMessages
                 || mtcTelemetry.follower.invalidLabelsOrRates
                     > latestMtcTelemetry_.follower.invalidLabelsOrRates
                 || mtcTelemetry.follower.unsupportedDirections
                     > latestMtcTelemetry_.follower.unsupportedDirections)
        {
            midiClockDiagnostic_ =
                juceString("MTC message rejected · check timecode rate and source direction");
            setStatus(midiClockDiagnostic_);
        }
        else if (mtcTelemetry.follower.discontinuityReason
                     == talas::runtime::MtcDiscontinuityReason::ArithmeticOverflow
                 || mtcTelemetry.mappingFailures
                     > latestMtcTelemetry_.mappingFailures
                 || mtcTelemetry.follower.arithmeticOverflows
                     > latestMtcTelemetry_.follower.arithmeticOverflows)
        {
            midiClockDiagnostic_ =
                juceString("MTC position rejected · project sample mapping is unavailable");
            setStatus(midiClockDiagnostic_);
        }
        else
        {
            const auto position = mtcTelemetry.follower.hasTimeCode
                ? workbenchSmpteTimeCodeText(mtcTelemetry.follower.timeCode)
                : juce::String{};
            const auto rate = mtcTelemetry.follower.hasTimeCode
                ? workbenchSmpteRateText(mtcTelemetry.follower.timeCode.rate)
                : juce::String{};
            switch (mtcTelemetry.follower.state)
            {
            case talas::runtime::MtcTransportLockState::Locked:
                midiClockDiagnostic_ = "MTC locked"
                    + (position.isNotEmpty() ? juceString(" · ") + position : juce::String{})
                    + (rate.isNotEmpty() ? juceString(" · ") + rate : juce::String{})
                    + (mtcRunning ? juceString(" · running") : juceString(" · transport held"));
                break;
            case talas::runtime::MtcTransportLockState::Located:
                midiClockDiagnostic_ = "MTC located"
                    + (position.isNotEmpty() ? juceString(" · ") + position : juce::String{})
                    + (rate.isNotEmpty() ? juceString(" · ") + rate : juce::String{})
                    + juceString(" · transport held");
                break;
            case talas::runtime::MtcTransportLockState::Acquiring:
                midiClockDiagnostic_ = juceString("MTC acquiring · next quarter-frame piece ")
                    + juce::String(
                        static_cast<int>(
                            mtcTelemetry.follower.nextExpectedPiece) + 1)
                    + " of 8";
                break;
            case talas::runtime::MtcTransportLockState::Stale:
                midiClockDiagnostic_ =
                    juceString("MTC stale · transport held · awaiting a complete fresh set");
                break;
            case talas::runtime::MtcTransportLockState::Waiting:
            default:
                midiClockDiagnostic_ =
                    juceString("MTC ready · awaiting a complete timecode set");
                break;
            }
        }
        midiClockDiagnostic_ += juceString(" · ") + sourceName
            + juceString(" — ") + exactSourceIdentifier;
        midiClockAccessibleDetail_ =
            "External MTC exact source identifier "
            + exactSourceIdentifier + ". "
            + midiClockDiagnostic_;
        if (coherentMtc && mtcTelemetry.follower.hasTimeCode)
        {
            midiClockAccessibleDetail_ += ". SMPTE position "
                + workbenchSmpteTimeCodeText(mtcTelemetry.follower.timeCode)
                + " at "
                + workbenchSmpteRateText(mtcTelemetry.follower.timeCode.rate)
                + ". Project frame "
                + realtimeMetricText(mtcTelemetry.mappedProjectFrame);
        }
        midiClockAccessibleDetail_ +=
            ". SMPTE position follows MTC; audio is not time-stretched.";
        if (playingChanged
            || midiClockDiagnostic_ != previousDiagnostic)
        {
            refreshPersistentStatus();
        }
    }
    else if (usesExternalMidiClock())
    {
        const auto previousDiagnostic = midiClockDiagnostic_;
        const auto coherentExternalState =
            musicalClockSnapshot.externalAuthority
            && musicalClockSnapshot.externalAuthorityGeneration
                == midiClockTelemetry.authorityGeneration;
        const auto clockRunning = coherentExternalState
            ? musicalClockSnapshot.transport.playing
            : midiClockTelemetry.running;
        const auto clockLockState = coherentExternalState
            ? musicalClockSnapshot.externalLockState
            : midiClockTelemetry.lockState;
        const auto clockQuarterMicroseconds = coherentExternalState
            ? musicalClockSnapshot.externalMicrosecondsPerQuarter
            : midiClockTelemetry.estimatedMicrosecondsPerQuarter;
        const auto clockPhaseErrorNanoseconds = coherentExternalState
            ? musicalClockSnapshot.externalPhaseErrorNanoseconds
            : midiClockTelemetry.phaseErrorNanoseconds;
        const auto clockJitterNanoseconds = coherentExternalState
            ? musicalClockSnapshot.externalJitterNanoseconds
            : midiClockTelemetry.jitterNanoseconds;
        const auto clockAcquisitionIntervals = coherentExternalState
            ? musicalClockSnapshot.externalAcquisitionIntervals
            : midiClockTelemetry.acquisitionIntervals;
        const auto& clockSettings = projectBridge_.editor().document()
                                        .deviceSettings.transportSync;
        const auto exactSourceIdentifier =
            juceString(clockSettings.sourceIdentifier);
        const auto source = std::ranges::find_if(
            midiInputSources_,
            [&](const auto& candidate) {
                return candidate.identifier == exactSourceIdentifier;
            });
        const auto sourceName = source != midiInputSources_.end()
            ? source->displayName
            : juce::String{"saved input"};
        const auto playingChanged = transportPlaying_
            != clockRunning;
        transportPlaying_ = clockRunning;
        if (playingChanged)
        {
            analysisPanel_->setPlaying(transportPlaying_);
            setStatus(transportPlaying_
                ? "External MIDI Clock started transport"
                : "External MIDI Clock stopped transport");
            commandManager_.commandStatusChanged();
        }
        if (!midiClockTelemetry.externalAuthority)
        {
            midiClockDiagnostic_ =
                "MIDI Clock waiting for its saved input";
        }
        else if (midiClockTelemetry.signalStale)
        {
            midiClockDiagnostic_ =
                juceString("MIDI Clock signal timed out · transport held");
            setStatus(midiClockDiagnostic_);
        }
        else if (midiClockTelemetry.songPositionConversionFailures
                 > latestMidiClockTelemetry_
                       .songPositionConversionFailures)
        {
            midiClockDiagnostic_ =
                juceString("MIDI Song Position rejected · musical map or sample rate is invalid");
            setStatus(midiClockDiagnostic_);
        }
        else if (midiClockTelemetry.acceptedSongPositions
                 > latestMidiClockTelemetry_.acceptedSongPositions)
        {
            midiClockDiagnostic_ =
                "MIDI Song Position "
                + juce::String(midiClockTelemetry.lastSongPosition)
                + juceString(" · frame ")
                + juce::String(
                    static_cast<juce::int64>(
                        std::min<std::uint64_t>(
                            midiClockTelemetry.lastRelocationFrame,
                            static_cast<std::uint64_t>(
                                std::numeric_limits<juce::int64>::max()))));
            setStatus(midiClockDiagnostic_);
        }
        else if (clockRunning)
        {
            const auto tempo = midiClockTempoText(clockQuarterMicroseconds);
            switch (clockLockState)
            {
            case talas::runtime::MidiClockLockState::Locked:
                midiClockDiagnostic_ = "MIDI Clock locked"
                    + (tempo.isNotEmpty() ? juceString(" · ") + tempo : juce::String{})
                    + juceString(" · running");
                break;
            case talas::runtime::MidiClockLockState::Holdover:
                midiClockDiagnostic_ = "MIDI Clock holdover"
                    + (tempo.isNotEmpty() ? juceString(" · ") + tempo : juce::String{})
                    + juceString(" · running temporarily");
                break;
            case talas::runtime::MidiClockLockState::Acquiring:
                midiClockDiagnostic_ = juceString("MIDI Clock acquiring · ")
                    + juce::String(clockAcquisitionIntervals)
                    + juceString("/3 intervals · transport running");
                break;
            case talas::runtime::MidiClockLockState::Stale:
                midiClockDiagnostic_ =
                    juceString("MIDI Clock stale · transport held");
                break;
            case talas::runtime::MidiClockLockState::Waiting:
            default:
                midiClockDiagnostic_ =
                    juceString("MIDI Clock waiting for pulse lock · transport running");
                break;
            }
        }
        else
        {
            const auto tempo = midiClockTempoText(clockQuarterMicroseconds);
            switch (clockLockState)
            {
            case talas::runtime::MidiClockLockState::Locked:
                midiClockDiagnostic_ = "MIDI Clock locked"
                    + (tempo.isNotEmpty() ? juceString(" · ") + tempo : juce::String{})
                    + juceString(" · awaiting Start or Continue");
                break;
            case talas::runtime::MidiClockLockState::Holdover:
                midiClockDiagnostic_ = "MIDI Clock holdover"
                    + (tempo.isNotEmpty() ? juceString(" · ") + tempo : juce::String{})
                    + juceString(" · transport held");
                break;
            case talas::runtime::MidiClockLockState::Acquiring:
                midiClockDiagnostic_ = juceString("MIDI Clock acquiring · ")
                    + juce::String(clockAcquisitionIntervals)
                    + "/3 intervals";
                break;
            case talas::runtime::MidiClockLockState::Stale:
                midiClockDiagnostic_ =
                    juceString("MIDI Clock stale · awaiting fresh lock and Start or Continue");
                break;
            case talas::runtime::MidiClockLockState::Waiting:
            default:
                midiClockDiagnostic_ =
                    juceString("MIDI Clock ready · awaiting pulses and Start or Continue");
                break;
            }
        }
        midiClockDiagnostic_ += juceString(" · ") + sourceName;
        if (clockLockState == talas::runtime::MidiClockLockState::Locked
            || clockLockState == talas::runtime::MidiClockLockState::Holdover)
        {
            midiClockDiagnostic_ += juceString(" · phase ")
                + juce::String(static_cast<juce::int64>(
                    clockPhaseErrorNanoseconds))
                + juceString(" ns · jitter ")
                + realtimeMetricText(clockJitterNanoseconds)
                + " ns";
        }
        midiClockAccessibleDetail_ =
            "External MIDI Clock exact source identifier "
            + exactSourceIdentifier + ". "
            + midiClockDiagnostic_
            + ". Musical phase follows the external clock; audio is not time-stretched.";
        if (musicalClockSnapshot.musicalPosition.has_value())
        {
            midiClockAccessibleDetail_ += " Timeline position bar "
                + realtimeMetricText(
                    musicalClockSnapshot.musicalPosition->bar)
                + ", beat "
                + juce::String(
                    musicalClockSnapshot.musicalPosition->beat)
                + ", tick "
                + juce::String(
                    musicalClockSnapshot.musicalPosition->tick)
                + ".";
        }
        if (playingChanged
            || midiClockDiagnostic_ != previousDiagnostic)
        {
            refreshPersistentStatus();
        }
    }
    else
    {
        midiClockAccessibleDetail_.clear();
    }
    latestMidiClockTelemetry_ = midiClockTelemetry;
    latestMtcTelemetry_ = mtcTelemetry;
    virtualControlSurface_->finishMidiTransactionIfIdle();
    if (currentDocumentStaged_)
    {
        const auto analysisSampleRate =
            projectBridge_.latestAnalysisCaptureSampleRate();
        if (analysisSampleRate > 0.0)
        {
            analysisPanel_->setCapture(
                projectBridge_.latestAnalysisCapture(),
                analysisSampleRate,
                projectBridge_.latestScoreSourceAnalysisCaptures());
        }
    }
    performancePanel_->refreshRuntimeState();
    performanceCanvas_->refreshRuntimeState();
    refreshPreparedGraphStatus();
    refreshOutputMeterLabel();
    refreshTransportLabel();
    timelinePanel_->setTransportSnapshot(
        musicalClockSnapshot);
    timelinePanel_->setExternalTransportAccessibilityDetail(
        midiClockAccessibleDetail_);
    pollProjectFileChanges();
    pollProjectAssetChanges();
    finishScoreClipArtifactPublicationIfReady();
    finishDiskCacheCaptureIfReady();
    if (diskCacheCaptureRunning_)
    {
        refreshDiskCacheCaptureStatus();
    }
    else if (scoreClipArtifactRunning_)
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        if (operation.phase
            == talas::app::WorkbenchOperationPhase::Invalidated)
        {
            setStatus("Cancelling clip artifact operation: project authority changed");
        }
        else if (operation.phase
                 == talas::app::WorkbenchOperationPhase::Cancelling)
        {
            setStatus("Cancelling clip artifact operation...");
        }
        else
        {
            setStatus(
                (scoreClipArtifactActiveMode_
                        == talas::project::ScoreClipRenderMode::Frozen
                    ? "Freezing Score clip... "
                    : "Rendering Score clip cache... ")
                + juce::String(
                    static_cast<int>(operation.progressPermille / 10U))
                + "%");
        }
        timelinePanel_->repaint();
    }
    else if (exportRunning_)
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        if (operation.phase
            == talas::app::WorkbenchOperationPhase::Invalidated)
        {
            setStatus("Cancelling audio export: project authority changed");
        }
        else if (operation.phase
                 == talas::app::WorkbenchOperationPhase::Cancelling)
        {
            setStatus("Cancelling audio export...");
        }
        else
        {
            setStatus(
                "Exporting audio... "
                + juce::String(
                    static_cast<int>(operation.progressPermille / 10U))
                + "%");
        }
    }
    else if (packageRunning_)
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        if (operation.phase
            == talas::app::WorkbenchOperationPhase::Invalidated)
        {
            setStatus("Cancelling project package: project authority changed");
        }
        else if (operation.phase
                 == talas::app::WorkbenchOperationPhase::Cancelling)
        {
            setStatus("Cancelling project package...");
        }
        else
        {
            setStatus(
                "Packaging project... "
                + juce::String(
                    static_cast<int>(operation.progressPermille / 10U))
                + "%");
        }
    }
    else if (
        pendingProjectFileStatus_.isNotEmpty())
    {
        setStatus(pendingProjectFileStatus_);
        pendingProjectFileStatus_.clear();
    }
    else if (
        pendingAssetWatchStatus_.isNotEmpty())
    {
        setStatus(pendingAssetWatchStatus_);
        pendingAssetWatchStatus_.clear();
    }
    const auto collected = projectBridge_.collectRetired();
    juce::ignoreUnused(collected);
    const auto latestStageGeneration =
        projectBridge_.latestStageGeneration();
    if (projectBridge_.editor().document().deviceSettings.oscIngress.enabled
        && latestStageGeneration != oscRouteStageGeneration_)
    {
        rebuildOscIngress();
    }
    else if (projectBridge_.editor().document().deviceSettings.oscEgress.enabled
             && latestStageGeneration
                 != oscEgressRouteStageGeneration_)
    {
        rebuildOscEgress();
    }
    refreshContextHelp(
        juce::Component::getCurrentlyFocusedComponent());
}

void WorkbenchComponent::publishMidiMonitor(
    const talas::runtime::RealtimeControlCommand& command,
    std::string outcome,
    talas::app::AnalysisLiveState liveState,
    std::uint64_t stageId,
    std::string sourceIdentifier)
{
    if (nextMidiMonitorSequence_
        == std::numeric_limits<std::uint64_t>::max())
    {
        return;
    }
    if (midiMonitorRecordCount_ == midiMonitorRecords_.size())
    {
        std::move(
            midiMonitorRecords_.begin() + 1,
            midiMonitorRecords_.end(),
            midiMonitorRecords_.begin());
        --midiMonitorRecordCount_;
    }
    const auto route = command.eventId == 0
        ? 0 : static_cast<int>(command.eventId - 1);
    auto& record = midiMonitorRecords_[midiMonitorRecordCount_++];
    record = {};
    record.sequence = nextMidiMonitorSequence_++;
    record.stageId = stageId;
    record.label = command.kind
            == talas::runtime::RealtimeControlCommandKind::MidiProgramChange
        ? "MIDI Program ch " + std::to_string(command.midiChannel)
            + " / " + std::to_string(route)
        : command.eventId == 0
            ? "MIDI CC"
            : "MIDI CC ch " + std::to_string(route / 128 + 1)
                + " / " + std::to_string(route % 128);
    record.detail = std::move(outcome)
        + " · value " + std::to_string(command.value)
        + " · source slot "
        + std::to_string(command.midiSourceSlot)
        + " · source generation "
        + std::to_string(command.midiSourceGeneration);
    if (!sourceIdentifier.empty())
        record.detail += " · source " + sourceIdentifier;
    record.liveState = liveState;
    record.identity.kind =
        talas::app::AnalysisNavigationKind::MidiMonitor;
    record.identity.stageId = stageId;
    record.identity.authorityGeneration = command.generation;
    record.identity.monitorSequence = record.sequence;
    record.identity.sourceId = sourceIdentifier.empty()
        ? "slot " + std::to_string(command.midiSourceSlot)
            + " @ " + std::to_string(command.midiSourceGeneration)
        : std::move(sourceIdentifier);
}

void WorkbenchComponent::processPendingOscPerformanceControls()
{
    talas::runtime::RealtimeControlCommand command;
    const auto popBudget = oscPerformanceIngress_.capacity();
    for (std::size_t popped = 0;
         popped < popBudget && oscPerformanceIngress_.tryPop(command);
         ++popped)
    {
        if (command.stageGeneration == 0
            || command.stageGeneration != projectBridge_.latestStageGeneration()
            || command.generation != oscIngress_.routeGeneration())
        {
            continue;
        }
        const auto control = std::ranges::find_if(
            stagedVirtualControls_,
            [&](const auto& candidate) {
                return projectBridge_.realtimeEventValueIdForLatestStage(
                           candidate.eventName,
                           candidate.domain)
                    == std::optional<std::uint32_t>{command.eventId};
            });
        if (control == stagedVirtualControls_.end())
            continue;
        const auto rejected = dispatchPerformanceEventValue(
            control->eventName,
            command.value,
            talas::app::PerformanceActivitySource::Osc,
            {
                talas::project::ControllerRecordingOrigin::Osc,
                command.recordingSourceTimestampNanoseconds,
                command.recordingRouteGeneration,
                command.recordingSourceGeneration,
            });
        if (rejected.has_value())
        {
            setStatus(*rejected);
            continue;
        }
        publishOscEgressValue(control->eventName, command.value);
        if (const auto* presentation =
                talas::project::findProjectPerformanceControlPresentation(
                    projectBridge_.editor().document(),
                    control->eventName,
                    control->domain))
        {
            performanceCanvas_->noteActivity(
                presentation->target,
                talas::app::PerformanceActivitySource::Osc);
        }
    }
}

std::optional<juce::String>
WorkbenchComponent::dispatchPerformanceEventValue(
    std::string_view eventName,
    double value,
    talas::app::PerformanceActivitySource source,
    talas::runtime::ControllerRecordingIngressEvidence recording)
{
    // While the Timeline records to disk the faders
    // do not take the mouse; MIDI faders and presets still change values.
    if (!talas::app::liveRecordingTakesValueFrom(source, liveTimelineRecordingActive()))
        return juceString(talas::app::liveRecordingMouseRefusal());
    processSoundLibraryControllerRoute();
    const auto& document = projectBridge_.editor().document();
    const auto control = std::ranges::find(
        document.virtualControls,
        eventName,
        &talas::project::VirtualControl::eventName);
    if (control == document.virtualControls.end())
        return juce::String{"Event Value no longer exists"};

    const auto* presentation =
        talas::project::findProjectPerformanceControlPresentation(
            document, eventName, control->domain);
    const auto target = presentation == nullptr
        ? std::optional<talas::project::SnapshotTargetIdentity>{}
        : std::optional<talas::project::SnapshotTargetIdentity>{
            presentation->target};
    if (currentDocumentStaged_
        && presentation != nullptr
        && projectBridge_.snapshotPerformanceDocumentCurrent())
    {
        const auto result = performanceDispatcher_.dispatch(
            presentation->controlId,
            value,
            projectBridge_.latestStageGeneration());
        if (!result.ok())
        {
            return result.message.empty()
                ? juce::String{"Performance update was rejected"}
                : juceString(result.message);
        }
        if (recording.sourceTimestampNanoseconds <= 0
            && source == talas::app::PerformanceActivitySource::UserInterface)
        {
            recording.origin = talas::project::ControllerRecordingOrigin::Ui;
            recording.sourceTimestampNanoseconds =
                controllerRecordingTimestampNanoseconds(
                    juce::Time::getMillisecondCounterHiRes() * 0.001);
            recording.routeGeneration = std::max(
                uiDocumentGeneration_.load(std::memory_order_acquire),
                projectBridge_.latestStageGeneration());
            recording.sourceGeneration = 0;
        }
        for (const auto& applied : result.appliedValues)
        {
            performanceCanvas_->noteActivity(
                applied.target,
                applied.target == presentation->target
                    ? source
                    : talas::app::PerformanceActivitySource::Macro);
            if (applied.target.kind
                    != talas::project::SnapshotTargetKind::EventValue
                || applied.value.kind
                    != talas::project::SnapshotValueKind::Number)
            {
                continue;
            }
            const auto eventId =
                projectBridge_.realtimeEventValueIdForLatestStage(
                    applied.target.eventName,
                    applied.value.domain);
            if (!eventId.has_value())
                continue;
            const auto admitted =
                projectBridge_.enqueueControllerRecordingObservationAtTimeForStage(
                    *eventId,
                    applied.value.number,
                    applied.value.domain,
                    recording.sourceTimestampNanoseconds,
                    projectBridge_.latestStageGeneration(),
                    recording);
            if (!admitted && projectBridge_.controllerRecordingActive())
            {
                projectBridge_.stopControllerRecording(
                    talas::project::ControllerRecordingTerminal::CapacityExceeded);
            }
        }
        return std::nullopt;
    }

    const auto edited = projectBridge_.editor().setVirtualControlValue(
        eventName, value);
    if (!edited.ok())
        return juceString(edited.message);
    if (target.has_value())
        performanceCanvas_->noteActivity(*target, source);
    return std::nullopt;
}

void WorkbenchComponent::finishPerformanceValueGesture()
{
    if (currentDocumentStaged_
        && projectBridge_.snapshotPerformanceDocumentCurrent())
    {
        snapshotAuthoringChanged();
        return;
    }
    documentChanged(false);
}

std::optional<std::string>
WorkbenchComponent::runMidiIngressGenerationWorkflowForSmoke(
    std::string_view eventName)
{
    midiSmokeSourceIdentifier_ = "smoke-midi-input";
    const auto findControl = [&]()
        -> const talas::project::VirtualControl* {
        const auto& controls =
            projectBridge_.editor().document().virtualControls;
        const auto found = std::ranges::find_if(
            controls,
            [&](const auto& control) {
                return control.eventName == eventName;
            });
        return found == controls.end() ? nullptr : &*found;
    };
    const auto queueUiMessage = [&](int channel, int controller, int value) {
        talas::runtime::RealtimeControlCommand command;
        command.eventId = static_cast<std::uint32_t>(
            (channel - 1) * 128 + controller + 1);
        command.value = static_cast<double>(value);
        command.stageId =
            uiDocumentGeneration_.load(std::memory_order_acquire);
        command.generation =
            midiUiStateToken_.load(std::memory_order_acquire);
        command.midiSourceSlot = 1;
        command.midiSourceGeneration =
            midiInputSourceGeneration_.load(std::memory_order_acquire);
        return midiUiIngress_.tryPush(command);
    };
    const auto remainsUnmapped = [&] {
        const auto* control = findControl();
        return control != nullptr
            && !control->midiMapping.has_value();
    };

    if (!remainsUnmapped() || !queueUiMessage(2, 75, 127)
        || !virtualControlSurface_->beginMidiLearnForSmoke(eventName))
    {
        return "pre-learn MIDI generation fixture could not be prepared";
    }
    processPendingMidiControllers();
    if (!remainsUnmapped())
        return "a MIDI message queued before learn rebound the new target";
    virtualControlSurface_->cancelMidiLearn();

    if (!virtualControlSurface_->beginMidiLearnForSmoke(eventName)
        || !queueUiMessage(2, 76, 127))
    {
        return "learn-session MIDI generation fixture could not be prepared";
    }
    virtualControlSurface_->cancelMidiLearn();
    if (!virtualControlSurface_->beginMidiLearnForSmoke(eventName))
        return "replacement MIDI learn session could not start";
    processPendingMidiControllers();
    if (!remainsUnmapped())
        return "a MIDI message from an older learn session rebound the target";
    virtualControlSurface_->cancelMidiLearn();

    if (!virtualControlSurface_->beginMidiLearnForSmoke(eventName)
        || !queueUiMessage(2, 77, 127))
    {
        return "source-generation MIDI fixture could not be prepared";
    }
    (void)invalidateMidiInputSourceRegistryState();
    processPendingMidiControllers();
    if (!remainsUnmapped())
        return "a MIDI message from a disconnected source generation rebound the target";
    if (!queueUiMessage(2, 77, 127))
        return "replacement source generation could not queue a fresh message";
    processPendingMidiControllers();
    if (!remainsUnmapped())
        return "a new source generation completed a stale MIDI learn session";

    const auto firstMapping =
        projectBridge_.editor().learnVirtualControlMidiMapping(
            eventName,
            {2,
             74,
             talas::project::MidiControlSourceMode::ExactInput,
             midiSmokeSourceIdentifier_});
    if (!firstMapping.ok())
        return "route-generation MIDI fixture could not map its first source";
    documentChanged(false);
    const auto* before = findControl();
    if (before == nullptr || !queueUiMessage(2, 74, 127))
        return "route-generation MIDI fixture could not queue its old source";
    const auto previousValue = before->value;
    const auto remapped =
        projectBridge_.editor().learnVirtualControlMidiMapping(
            eventName,
            {3,
             21,
             talas::project::MidiControlSourceMode::ExactInput,
             midiSmokeSourceIdentifier_});
    if (!remapped.ok())
        return "route-generation MIDI fixture could not remap its source";
    documentChanged(false);
    processPendingMidiControllers();
    const auto* after = findControl();
    if (after == nullptr || after->value != previousValue
        || !after->midiMapping.has_value()
        || after->midiMapping->channel != 3
        || after->midiMapping->controller != 21)
    {
        return "a MIDI message from the prior route generation changed the remapped target";
    }
    midiSmokeSourceIdentifier_.clear();
    virtualControlSurface_->refresh();
    const auto unavailableMidi = virtualControlSurface_
        ->midiMappingPresentationForSmoke(eventName);
    if (!unavailableMidi.has_value()
        || unavailableMidi->find("Input unavailable") == std::string::npos
        || unavailableMidi->find("smoke-midi-input") == std::string::npos)
    {
        return "disconnected exact MIDI mapping did not expose its unavailable input and identifier: "
            + unavailableMidi.value_or("<missing>");
    }
    midiSmokeSourceIdentifier_ = "smoke-midi-input";
    virtualControlSurface_->refresh();
    const auto cleared =
        projectBridge_.editor().setVirtualControlMidiMapping(
            eventName, std::nullopt);
    if (!cleared.ok())
        return "route-generation MIDI fixture could not restore an unmapped target";
    documentChanged(false);
    virtualControlSurface_->refresh();
    return std::nullopt;
}

void WorkbenchComponent::playCurrentProject()
{
    const auto retryRejectedStage = latestPreparedGraphStatus_.activationRejected;
    stopPartialTrackPreview();
    const auto hasSoundLibraryAuditionWork = auditionActive_
        || soundLibraryAuditionPending_.has_value()
        || soundLibraryAuditionThread_.joinable()
        || soundLibraryAuditionActive_ != nullptr
        || soundLibraryAuditionRetiring_ != nullptr;
    if (hasSoundLibraryAuditionWork)
    {
        stopSoundLibraryAudition();
    }
    if (sampleMapAuditionActive_
        || sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing)
    {
        stopSampleMapAudition();
        if (sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Stopping)
        {
            return;
        }
    }
    setAuditionPresentation(std::nullopt);
    if (currentDocumentStaged_)
    {
        setPlaying(true);
        return;
    }
    if (askForVariableValues())
        return;
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0)
    {
        setStatus("Audio device has no valid sample rate or block size");
        return;
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = device->getCurrentSampleRate();
    settings.maxBlockFrames = static_cast<std::size_t>(device->getCurrentBufferSizeSamples());
    if (!stageProject(std::move(settings), device->getName()))
        return;
    // The virtual control surface opens as a Sound with Event Values plays,
    // unless the preferences say otherwise.
    if (const auto shown = talas::app::workspaceShowingControlsOnPlay(
            workspaceMode_, preferences_.openVirtualControlsOnPlay, !projectBridge_.editor().document().virtualControls.empty()))
        activateWorkspaceMode(*shown);
    if (!retryRejectedStage)
        setPlaying(true);
}

bool WorkbenchComponent::compileAndLoadCurrentProject(std::optional<talas::runtime::RenderSettings> profile)
{
    if (transportPlaying_)
        pauseCurrentProject();
    if (!currentDocumentStaged_)
    {
        if (askForVariableValues())
            return false;
        auto* device = deviceManager_.getCurrentAudioDevice();
        if (!profile.has_value())
        {
            if (device == nullptr || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0)
            {
                setStatus("Audio device has no valid sample rate or block size");
                return false;
            }
            profile = talas::runtime::RenderSettings{
                device->getCurrentSampleRate(), static_cast<std::size_t>(device->getCurrentBufferSizeSamples())};
        }
        if (!stageProject(std::move(*profile), device == nullptr ? juce::String{"Compile & Load"} : device->getName()))
            return false;
    }
    setStatus("Compiled and loaded · Restart starts it");
    return true;
}

void WorkbenchComponent::restartCurrentProject()
{
    if (!currentDocumentStaged_)
    {
        playCurrentProject();
        return;
    }
    (void) projectBridge_.requestTransportRelocation(0);
    if (transportPlaying_)
        dspStatusClock_.start(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    setPlaying(true);
}

void WorkbenchComponent::refreshAnalysisWorkspace()
{
    finishAnalysisOutputTakeIfReady();
    auto projection = talas::app::buildAnalysisWorkbenchProjection(
        projectBridge_.editor().document(),
        registry_,
        workspaceSession_.analysis);
    analysisPanel_->setWorkspaceProjection(
        projection,
        workspaceSession_.analysis.frozen,
        workspaceSession_.analysis.refreshRateHz,
        workspaceSession_.analysis.historyFrames);
    AnalysisAuditionPanelState auditionState;
    auditionState.request = workspaceSession_.analysis.audition;
    auditionState.preparing =
        !analysisAuditionWorkerFinished_.load(std::memory_order_acquire);
    auditionState.active = analysisAuditionActive_ != nullptr;
    auditionState.canCommit = auditionState.active
        && analysisAuditionActive_->preparation
            .portableCommitDocument.has_value();
    auditionState.transportExternallyControlled =
        usesExternalMidiClock() || usesExternalMtc();
    auditionState.status = analysisAuditionStatus_.toStdString();
    const auto selected = projection.selectedTraceIndex.has_value()
            && *projection.selectedTraceIndex < projection.traces.size()
        ? &projection.traces[*projection.selectedTraceIndex].trace
        : nullptr;
    if (selected != nullptr)
    {
        const talas::project::ProjectGraph* graph =
            &projectBridge_.editor().document().graph;
        for (const auto& instanceId : selected->instancePath)
        {
            const auto instance = std::ranges::find(
                graph->nodes,
                instanceId,
                &talas::project::ProjectNode::instanceId);
            if (instance == graph->nodes.end()
                || !instance->soundDefinitionId.has_value())
            {
                graph = nullptr;
                break;
            }
            const auto definition = std::ranges::find(
                projectBridge_.editor().document().soundDefinitions,
                *instance->soundDefinitionId,
                &talas::project::ProjectSoundDefinition::definitionId);
            if (definition
                == projectBridge_.editor().document().soundDefinitions.end())
            {
                graph = nullptr;
                break;
            }
            graph = &definition->graph;
        }
        if (graph != nullptr)
        {
            const auto node = std::ranges::find(
                graph->nodes,
                selected->nodeId,
                &talas::project::ProjectNode::instanceId);
            const auto* descriptor = node == graph->nodes.end()
                ? nullptr
                : registry_.findNodeType(node->typeId);
            if (descriptor != nullptr)
            {
                for (const auto& input : descriptor->inputs)
                {
                    if (input.type == talas::core::PortType::Audio)
                    {
                        auditionState.destinationInputs.push_back({
                            input.id,
                            descriptor->displayName + " · "
                                + selected->nodeId + "." + input.id,
                        });
                    }
                }
            }
        }
    }
    for (const auto& asset : projectBridge_.editor().document().assets)
    {
        if (asset.kind == talas::project::ProjectAssetKind::Audio)
            auditionState.frozenAssets.push_back({asset.id, asset.id});
    }
    analysisPanel_->setAuditionState(std::move(auditionState));
    AnalysisOutputTakePanelState outputTakeState;
    outputTakeState.status = analysisOutputTakeStatus_.toStdString();
    outputTakeState.deviceReady = currentDocumentStaged_
        && deviceMappingAttached_.load(std::memory_order_acquire)
        && deviceManager_.getCurrentAudioDevice() != nullptr;
    const auto outputTakeStageStatus =
        projectBridge_.preparedGraphStatus();
    applyDiskRecorderOutputTakeState(
        outputTakeState, outputTakeStageStatus);
    if (analysisOutputTake_ != nullptr)
    {
        const auto snapshot = analysisOutputTake_->snapshot();
        outputTakeState.destinationName = talas::project::pathToUtf8(
            snapshot.destination.filename());
        outputTakeState.sampleRate = snapshot.sampleRate;
        outputTakeState.writtenFrames = snapshot.writtenFrames;
        outputTakeState.droppedBlocks = snapshot.droppedBlocks;
        outputTakeState.rejectedBlocks = snapshot.rejectedBlocks;
        outputTakeState.channelCount = snapshot.channelCount;
        outputTakeState.recording = snapshot.state
            == talas::audio::RealtimeOutputTakeState::Recording;
        outputTakeState.stopping = snapshot.state
            == talas::audio::RealtimeOutputTakeState::Stopping;
        outputTakeState.incomplete = snapshot.incomplete;
    }
    else if (analysisOutputTakeLastSnapshot_.has_value())
    {
        const auto& snapshot = *analysisOutputTakeLastSnapshot_;
        outputTakeState.destinationName = talas::project::pathToUtf8(
            snapshot.destination.filename());
        outputTakeState.sampleRate = snapshot.sampleRate;
        outputTakeState.writtenFrames = snapshot.writtenFrames;
        outputTakeState.droppedBlocks = snapshot.droppedBlocks;
        outputTakeState.rejectedBlocks = snapshot.rejectedBlocks;
        outputTakeState.channelCount = snapshot.channelCount;
        outputTakeState.incomplete = snapshot.incomplete;
    }
    analysisPanel_->setOutputTakeState(std::move(outputTakeState));
    if (currentDocumentStaged_)
    {
        const auto analysisSampleRate =
            projectBridge_.latestAnalysisCaptureSampleRate();
        if (analysisSampleRate > 0.0)
        {
            analysisPanel_->setCapture(
                projectBridge_.latestAnalysisCapture(),
                analysisSampleRate,
                projectBridge_.latestScoreSourceAnalysisCaptures());
        }
    }
}

bool WorkbenchComponent::stageProject(talas::runtime::RenderSettings settings, const juce::String& deviceName)
{
    settings.variableEnvironment = variableEnvironment_;
    settings.variableLiteralEnvironment = variableLiteralEnvironment_;
    settings.soundVariableEnvironment = soundVariableEnvironment_;
    settings.speakerPlacement = preferences_.speakerPlacement;
    settings.playedScoreClipIds = timelinePanel_->marks().playedClips(projectBridge_.editor().document().timeline.score);
    stopSoundLibraryAudition();
    stopAnalysisAudition();
    stopPartialTrackPreview();
    // One output take belongs to one accepted render stage/profile. Revoke,
    // drain, and finalize it before any replacement graph can be prepared or
    // adopted, so a file can never mix samples or metadata across stages.
    stopAnalysisOutputTakeForLifecycle();
    const auto executionDiagnostics =
        talas::runtime::projectComponentExecutionDiagnostics(
            projectBridge_.editor().document(),
            registry_);
    if (!executionDiagnostics.empty())
    {
        publishCompileDiagnostics(executionDiagnostics);
        setStatus(
            "Compile failed: "
            + juceString(executionDiagnostics.front().message));
        return false;
    }
    if (!acceptPendingProjectAssetReplacement())
        return false;
    if (const auto conflict =
            firstProjectAssetIdentityConflict();
        conflict.has_value())
    {
        pendingAssetReplacementId_ =
            *conflict;
        setStatus(
            "Asset generation history changed: "
            + juceString(*conflict)
            + juceString(" · press Play again to inspect and reconcile it as the next generation"));
        return false;
    }
    pendingAssetWatchStatus_.clear();
    publishCompileDiagnostics({});
    if (const auto packageError =
            currentPackageStagingError();
        packageError.has_value())
    {
        currentDocumentStaged_ = false;
        assetWatchBaseline_ = {};
        setStatus(
            "Package validation failed: "
            + juceString(*packageError));
        return false;
    }
    const auto beforeLoad =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                projectBridge_.editor()
                    .document(),
                registry_,
                projectPath());
    bool acceptedInitialInspection = false;
    if (!loadProjectAssets(
            projectBridge_.editor().document(),
            settings,
            &acceptedInitialInspection))
    {
        currentDocumentStaged_ = false;
        assetWatchBaseline_ = {};
        return false;
    }
    if (const auto packageError =
            currentPackageStagingError();
        packageError.has_value())
    {
        currentDocumentStaged_ = false;
        assetWatchBaseline_ = {};
        setStatus(
            "Package changed while loading: "
            + juceString(*packageError));
        return false;
    }
    auto afterLoad =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                projectBridge_.editor()
                    .document(),
                registry_,
                projectPath());
    if (const auto changedAsset =
            firstChangedLoadedResourceSnapshot(
                settings.resourceSnapshots);
        changedAsset.has_value())
    {
        currentDocumentStaged_ = false;
        assetWatchBaseline_ = {};
        openAssetRecoveryForAsset(
            *changedAsset,
            "Asset changed after loading",
            true);
        return false;
    }
    if (!acceptedInitialInspection)
    {
        if (const auto change =
            talas::audio::
                findProjectAssetWatchChange(
                    beforeLoad,
                    afterLoad);
            change.has_value())
        {
            currentDocumentStaged_ = false;
            assetWatchBaseline_ = {};
            setStatus(
                "Asset changed while loading: "
                + juceString(change->assetId));
            return false;
        }
    }
    settings.enableRealtimeAnalysisCapture = true;
    appendWorkspaceAnalysisTargets(
        settings,
        projectBridge_.editor().document(),
        registry_,
        workspaceSession_.analysis);
    const auto analysisSampleRate = settings.sampleRate;
    const auto partialTrackEditorSnapshot =
        partialTrackEditorSnapshotFor(settings);
    stagedMidiRoutesValid_ = false;
    stagedVirtualControls_.clear();
    rebuildMidiRuntimeRoutes();
    rebuildOscIngress();
    // Info > Oscilloscope or Spectrum analyzer stages the selected Sound with its display, once.
    const auto infoDisplay = std::exchange(infoDisplayStage_, std::nullopt);
    const auto result = compileShowingWatch(infoDisplay ? &infoDisplay->document : nullptr, std::move(settings));
    if (result.ok())
    {
        infoDisplay && !infoDisplay->displayNodeId.empty()
            ? analysisPanel_->showInfoDisplay(infoDisplay->displayNodeId, infoDisplay->displayTypeId)
            : analysisPanel_->clearInfoDisplay();
        const auto restoredWorkspaceVariables =
            !workspaceVariableRestorePending_
            || applyWorkspaceSessionVariableValuesToCurrentStage();
        const auto restoredWorkspaceSoundVariables =
            !workspaceSoundVariableRestorePending_
            || applyWorkspaceSessionSoundVariableValuesToCurrentStage();
        currentDocumentStaged_ = true;
        loadedSoundInputLines_ = talas::app::loadedSoundInputLines(talas::app::loadedSoundInputs(projectBridge_.editor().document()));
        assetWatchBaseline_ =
            std::move(afterLoad);
        nextAssetWatchPollMilliseconds_ =
            juce::Time::
                getMillisecondCounterHiRes()
            + 1000.0;
        publishCompiledSound();
        analysisPanel_->setCapture(
            projectBridge_.latestAnalysisCapture(),
            analysisSampleRate,
            projectBridge_.latestScoreSourceAnalysisCaptures());
        refreshPartialTrackEditor(partialTrackEditorSnapshot);
        stagedVirtualControls_ = projectBridge_.editor().document().virtualControls;
        stagedMidiRoutesValid_ = true;
        // The surface changes with the Sound loaded.
        const auto alone = infoDisplay.has_value() && !infoDisplay->loadedSound.empty();
        virtualControlSurface_->setLoadedSound(alone ? std::optional{infoDisplay->document} : std::nullopt,
            alone ? infoDisplay->loadedSound : std::string{});
        virtualControlSurface_->refresh();
        rebuildMidiRuntimeRoutes();
        rebuildOscIngress();
        refreshPreparedGraphStatus();
        if (latestPreparedGraphStatus_.activationRejected)
            return false;
        setStatus(
            "Ready: " + deviceName
            + outputMappingStatus(result.outputChannelCount)
            + (restoredWorkspaceVariables
                    ? juce::String{}
                    : juceString(" · workspace variable restore pending"))
            + (restoredWorkspaceSoundVariables
                    ? juce::String{}
                    : juceString(" · workspace Sound State restore pending")));
        return true;
    }
    currentDocumentStaged_ = false;
    virtualControlSurface_->refresh();
    assetWatchBaseline_ = {};

    if (!result.diagnostics.empty())
    {
        publishCompileDiagnostics(result.diagnostics);
        setStatus("Compile failed: " + juceString(result.diagnostics.front().message));
    }
    else if (!result.message.empty())
    {
        setStatus("Realtime graph not staged: " + juceString(result.message));
    }
    else
    {
        setStatus("Realtime graph not staged");
    }
    return false;
}

[[nodiscard]] std::optional<std::string>
WorkbenchComponent::currentPackageStagingError() const
{
    if (!projectPath().has_value())
        return {};
    const auto resolvedProject =
        talas::project::resolveProjectPackagePath(
            *projectPath());
    if (!resolvedProject.ok())
        return resolvedProject.error;
    const auto packageValidation =
        talas::project::
            validateProjectPackageAssets(
                projectBridge_.editor()
                    .document(),
                resolvedProject);
    if (packageValidation.ok())
        return {};
    if (packageValidation.assetId.empty())
        return packageValidation.error;
    return "asset '" + packageValidation.assetId
        + "': " + packageValidation.error;
}

bool WorkbenchComponent::loadProjectAssets(
    const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings& settings,
    bool* acceptedInitialInspection)
{
    refreshActivityCursor(false, true); // An animated cursor while samples load.
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(document, registry_, projectPath());
    refreshActivityCursor();
    if (!loaded.ok())
    {
        const auto& failure = *loaded.failure;
        const auto message =
            talas::audio::describeProjectAssetLoadFailure(failure);
        const std::array diagnostics = {
            talas::runtime::Diagnostic{
                talas::runtime::DiagnosticCode::InvalidRenderSettings,
                failure.nodeId,
                {},
                failure.parameterId,
                message,
                failure.structuralSourcePath,
            },
        };
        publishCompileDiagnostics(diagnostics, &document);
        const auto isEditorDocument =
            &document
            == &projectBridge_.editor().document();
        const auto recoveryOpened = isEditorDocument
            && !failure.assetId.empty()
            && openAssetRecovery(failure);
        if (!recoveryOpened)
        {
            setStatus(
                loaded.wasCancelled
                    ? juce::String{"Asset loading cancelled"}
                    : juceString(message));
        }
        return false;
    }

    std::vector<talas::project::ProjectAsset>
        initialInspections;
    const auto& editorDocument =
        projectBridge_.editor().document();
    for (const auto& asset : document.assets)
    {
        if (!asset.contentHash.empty())
            continue;
        const auto editorAsset = std::ranges::find(
            editorDocument.assets,
            asset.id,
            &talas::project::ProjectAsset::id);
        if (editorAsset == editorDocument.assets.end()
            || *editorAsset != asset
            || !editorAsset->contentHash.empty())
        {
            continue;
        }
        const auto snapshot = std::ranges::find(
            loaded.snapshots,
            asset.id,
            [](const auto& value) -> const std::string& {
                return value->id;
            });
        if (snapshot == loaded.snapshots.end())
            continue;
        auto inspected = asset;
        inspected.contentHash =
            (*snapshot)->contentHash;
        if (asset.kind
            == talas::project::ProjectAssetKind::Audio)
        {
            inspected.channelCount =
                (*snapshot)->channelCount;
            inspected.sampleRate =
                (*snapshot)->sampleRate;
            inspected.frameCount =
                (*snapshot)->frameCount;
            inspected.encoding =
                (*snapshot)->encoding;
        }
        initialInspections.push_back(
            std::move(inspected));
    }
    for (auto& inspected : initialInspections)
    {
        const auto accepted =
            projectBridge_.editor()
                .acceptProjectAssetInspection(
                    std::move(inspected));
        if (!accepted.ok())
        {
            setStatus(
                "Asset inspection was not accepted: "
                + juceString(accepted.message));
            return false;
        }
    }
    if (!initialInspections.empty())
    {
        if (acceptedInitialInspection != nullptr)
            *acceptedInitialInspection = true;
        documentChanged();
    }
    settings.resourceSnapshots.insert(
        settings.resourceSnapshots.end(),
        std::make_move_iterator(loaded.snapshots.begin()),
        std::make_move_iterator(loaded.snapshots.end()));
    return true;
}

[[nodiscard]] const talas::project::ProjectDocument&
WorkbenchComponent::compileDiagnosticDocument() const
{
    return compileDiagnosticSourceDocument_.has_value()
        ? *compileDiagnosticSourceDocument_
        : projectBridge_.editor().document();
}

void WorkbenchComponent::publishCompileDiagnostics(
    std::span<const talas::runtime::Diagnostic> diagnostics,
    const talas::project::ProjectDocument* sourceDocument)
{
    latestCompileDiagnostics_.assign(diagnostics.begin(), diagnostics.end());
    const auto& editorDocument = projectBridge_.editor().document();
    if (!latestCompileDiagnostics_.empty()
        && sourceDocument != nullptr
        && sourceDocument != &editorDocument)
    {
        compileDiagnosticSourceDocument_ = *sourceDocument;
    }
    else
    {
        compileDiagnosticSourceDocument_.reset();
    }
    graphCanvas_->setCompileDiagnostics(
        latestCompileDiagnostics_,
        compileDiagnosticDocument());
    secondaryGraphCanvas_->setCompileDiagnostics(
        latestCompileDiagnostics_,
        compileDiagnosticDocument());
    diagnosticsPanel_->setCompileDiagnostics(
        latestCompileDiagnostics_);
    refreshPersistentStatus();
    // Sound Compiler Warnings: Notify immediately
    // puts the first in a dialog to close, else the diagnostics alone show.
    if (preferences_.notifyCompilerWarnings && !latestCompileDiagnostics_.empty() && isShowing())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Sound compiler",
            juceString(latestCompileDiagnostics_.front().message), {}, this);
}

void WorkbenchComponent::rebuildMidiRuntimeRoutes(
    bool invalidateRealtimeControlIngress)
{
    midiRouteAdmissionGate_.closeAndWait();
    struct AdmissionReopener
    {
        talas::project::RealtimeMidiRouteAdmissionGate& gate;
        ~AdmissionReopener() { gate.open(); }
    } admissionReopener{midiRouteAdmissionGate_};
    if (invalidateRealtimeControlIngress)
        projectBridge_.resetRealtimeControlTiming();
    if (midiInputSourceRegistryFailed_)
    {
        midiRuntimeRoutes_.clear();
        midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
        midiRuntimeRouteDiagnostic_ =
            "MIDI routes unavailable: input registry could not be rebuilt";
        refreshPersistentStatus();
        return;
    }
    if (midiInputSourceCapacityExceeded_)
    {
        midiRuntimeRoutes_.clear();
        midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
        midiRuntimeRouteDiagnostic_ =
            "MIDI routes unavailable: more than 16 inputs are enabled";
        refreshPersistentStatus();
        return;
    }
    if (!stagedMidiRoutesValid_)
    {
        midiRuntimeRoutes_.clear();
        midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
        midiRuntimeRouteDiagnostic_.clear();
        refreshPersistentStatus();
        return;
    }

    std::array<talas::project::PreparedMidiControlRoute, 16 * 128>
        preparedRoutes{};
    std::size_t preparedCount = 0;
    std::size_t unavailableExactSources = 0;
    for (const auto& control : stagedVirtualControls_)
    {
        if (!control.midiMapping.has_value())
            continue;
        if (preparedCount == preparedRoutes.size())
        {
            midiRuntimeRoutes_.clear();
            midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
            midiRuntimeRouteDiagnostic_ =
                "MIDI routes unavailable: route capacity exceeded";
            refreshPersistentStatus();
            return;
        }
        const auto eventId =
            projectBridge_.realtimeEventValueIdForLatestStage(
                control.eventName, control.domain);
        if (!eventId.has_value())
        {
            midiRuntimeRoutes_.clear();
            midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
            midiRuntimeRouteDiagnostic_ =
                "MIDI routes unavailable: "
                + juceString(control.eventName)
                + " is not in the staged graph";
            refreshPersistentStatus();
            return;
        }
        std::uint8_t sourceSlot = 0;
        if (control.midiMapping->sourceMode
            == talas::project::MidiControlSourceMode::ExactInput)
        {
            const auto source = std::ranges::find_if(
                midiInputSources_,
                [&](const auto& candidate) {
                    return candidate.identifier.toStdString()
                        == control.midiMapping->sourceIdentifier;
                });
            if (source == midiInputSources_.end())
            {
                if (control.midiMapping->sourceIdentifier
                    == midiSmokeSourceIdentifier_)
                {
                    sourceSlot = 1;
                }
                else
                {
                    ++unavailableExactSources;
                    continue;
                }
            }
            else
            {
                sourceSlot = source->slot;
            }
        }
        preparedRoutes[preparedCount++] = {
            *control.midiMapping,
            *eventId,
            control.kind,
            control.minimum,
            control.maximum,
            control.domain,
            sourceSlot,
            midiInputSourceGeneration_.load(std::memory_order_acquire),
        };
    }

    const auto replacement = midiRuntimeRoutes_.replace(
        std::span<const talas::project::PreparedMidiControlRoute>{
            preparedRoutes.data(), preparedCount});
    if (!replacement.ok())
    {
        midiRuntimeRoutes_.clear();
        midiRuntimeRouteDiagnostic_ =
            "MIDI routes unavailable: invalid or conflicting route set";
    }
    else
    {
        midiRuntimeRouteDiagnostic_ = unavailableExactSources == 0
            ? juce::String{}
            : juce::String(static_cast<int>(unavailableExactSources))
                + " exact MIDI route"
                + (unavailableExactSources == 1 ? " is" : "s are")
                + " unavailable until the saved input reconnects";
    }
    midiUiStateToken_.fetch_add(2, std::memory_order_acq_rel);
    refreshPersistentStatus();
}

bool WorkbenchComponent::isOscRouteAvailable(
    std::string_view eventName,
    std::string_view address) const
{
    const auto& document = projectBridge_.editor().document();
    const auto& settings = document.deviceSettings.oscIngress;
    const auto control = std::ranges::find(
        document.virtualControls,
        eventName,
        &talas::project::VirtualControl::eventName);
    const auto telemetry = oscIngress_.telemetry();
    if (!settings.enabled
        || control == document.virtualControls.end()
        || !oscRoutesAvailable_
        || telemetry.state != WorkbenchOscIngressState::Listening
        || telemetry.activePort
            != static_cast<std::uint16_t>(settings.port)
        || oscRouteStageGeneration_
            != projectBridge_.latestStageGeneration()
        || !projectBridge_.realtimeEventValueIdForLatestStage(
            eventName,
            control->domain)
                .has_value())
    {
        return false;
    }
    return control->oscMapping.has_value()
        && control->oscMapping->address == address;
}

bool WorkbenchComponent::isOscEgressRouteAvailable(
    std::string_view eventName,
    std::string_view address) const
{
    const auto& document = projectBridge_.editor().document();
    const auto& settings = document.deviceSettings.oscEgress;
    const auto control = std::ranges::find(
        document.virtualControls,
        eventName,
        &talas::project::VirtualControl::eventName);
    const auto telemetry = oscEgress_.telemetry();
    if (!settings.enabled
        || control == document.virtualControls.end()
        || !oscEgressRoutesAvailable_
        || telemetry.state != WorkbenchOscEgressState::Ready
        || telemetry.activePort
            != static_cast<std::uint16_t>(settings.port)
        || oscEgressRouteStageGeneration_
            != projectBridge_.latestStageGeneration()
        || !projectBridge_.realtimeEventValueIdForLatestStage(
            eventName,
            control->domain)
                .has_value())
    {
        return false;
    }
    return control->oscEgressMapping.has_value()
        && control->oscEgressMapping->address == address;
}

void WorkbenchComponent::setPlaying(bool playing)
{
    // The DSP status time restarts as the Sound starts and holds once it
    // stops.
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (playing && !transportPlaying_)
        dspStatusClock_.start(now);
    else if (!playing)
        dspStatusClock_.stop(now);
    projectBridge_.setPlaying(playing);
    transportPlaying_ = playing;
    if (playing)
        transportStopped_ = false;
    analysisPanel_->setPlaying(playing);
    // Sample memory read before it is written warns as it plays.
    const auto& graph = projectBridge_.editor().document().graph;
    setStatus(playing ? "Playing realtime graph" + juce::String{talas::app::memoryWarningsStatus(graph)} : "Paused");
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::refreshTransportLabel()
{
    const auto totalMilliseconds = static_cast<std::uint64_t>(
        std::max(0.0, projectBridge_.transportTimeSeconds()) * 1000.0);
    const auto minutes = totalMilliseconds / 60000;
    const auto seconds = (totalMilliseconds / 1000) % 60;
    const auto milliseconds = totalMilliseconds % 1000;
    transportLabel_.setText(
        juce::String(static_cast<juce::int64>(minutes)).paddedLeft('0', 2)
            + ":" + juce::String(static_cast<juce::int64>(seconds)).paddedLeft('0', 2)
            + "." + juce::String(static_cast<juce::int64>(milliseconds)).paddedLeft('0', 3),
        juce::dontSendNotification);
}

void WorkbenchComponent::pauseCurrentProject()
{
    stopPartialTrackPreview();
    if (soundLibraryAuditionActive_ != nullptr)
    {
        soundLibraryAuditionLifecycle_.store(2, std::memory_order_seq_cst);
        soundLibraryAuditionActive_->bridge->setPlaying(false);
        soundLibraryAuditionActive_->previousTransportPlaying = false;
        transportPlaying_ = false;
        analysisPanel_->setPlaying(false);
        soundLibraryAuditionStatus_ = "Private audition paused";
        setStatus(soundLibraryAuditionStatus_);
        refreshPersistentStatus();
        commandManager_.commandStatusChanged();
        return;
    }
    const auto hasSoundLibraryAuditionWork = auditionActive_
        || soundLibraryAuditionPending_.has_value()
        || soundLibraryAuditionThread_.joinable()
        || soundLibraryAuditionRetiring_ != nullptr;
    if (hasSoundLibraryAuditionWork)
    {
        stopSoundLibraryAudition();
        setPlaying(false);
        setStatus("Private audition cancelled and project paused");
        return;
    }
    setPlaying(false);
}

void WorkbenchComponent::stopCurrentProject()
{
    stopPartialTrackPreview();
    const auto hasSoundLibraryAuditionWork = auditionActive_
        || soundLibraryAuditionPending_.has_value()
        || soundLibraryAuditionThread_.joinable()
        || soundLibraryAuditionActive_ != nullptr
        || soundLibraryAuditionRetiring_ != nullptr;
    if (hasSoundLibraryAuditionWork)
    {
        stopSoundLibraryAudition();
    }
    if (sampleMapAuditionActive_
        || sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing)
    {
        stopSampleMapAudition();
        return;
    }
    if (sampleMapAuditionPresentation_
        == SampleMapAuditionPresentation::Stopping)
    {
        finishSampleMapAuditionRestoreIfReady();
        return;
    }
    ++sampleMapAuditionGeneration_;
    stopSampleMapAuditionWorker();
    sampleMapAuditionActive_ = false;
    sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Idle;
    sampleMapAuditionPendingVoice_.reset();
    sampleMapAuditionActiveVoice_.reset();
    sampleMapAuditionRestoreSettings_.reset();
    sampleMapAuditionRestoreDeviceName_.clear();
    setAuditionPresentation(std::nullopt);
    projectBridge_.stopTransport();
    dspStatusClock_.stop(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    transportPlaying_ = false;
    transportStopped_ = true;
    analysisPanel_->setPlaying(false);
    currentDocumentStaged_ = false;
    setStatus("Stopped at start");
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::publishDeviceStatus(juce::String status)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, status = std::move(status)] {
        if (safeThis != nullptr)
            safeThis->setStatus(status);
    });
}

void WorkbenchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source != &deviceManager_ || applyingProjectDeviceSettings_)
        return;
    rebuildMidiInputSourceRegistry();
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || device->getCurrentSampleRate() <= 0.0
        || device->getCurrentBufferSizeSamples() <= 0)
    {
        return;
    }

    const auto setup = deviceManager_.getAudioDeviceSetup();
    talas::project::ProjectDeviceSettings settings;
    settings.configured = true;
    settings.audioInputDeviceName =
        setup.inputDeviceName.toStdString();
    settings.audioOutputDeviceName =
        setup.outputDeviceName.toStdString();
    settings.sampleRate = device->getCurrentSampleRate();
    settings.bufferSize =
        static_cast<std::int32_t>(device->getCurrentBufferSizeSamples());
    for (int channel = 0; channel < 8; ++channel)
    {
        if (setup.inputChannels[channel])
            settings.inputChannels.push_back(channel + 1);
        if (setup.outputChannels[channel])
            settings.outputChannels.push_back(channel + 1);
    }
    for (const auto& midi : juce::MidiInput::getAvailableDevices())
    {
        if (deviceManager_.isMidiInputDeviceEnabled(midi.identifier))
        {
            settings.midiInputDeviceIdentifiers.push_back(
                midi.identifier.toStdString());
        }
    }
    const auto& previousSync = projectBridge_.editor().document()
                                   .deviceSettings.transportSync;
    settings.transportSync = previousSync;
    settings.oscIngress = projectBridge_.editor().document()
                              .deviceSettings.oscIngress;
    if (previousSync.sourceMode
        != talas::project::TransportSyncSourceMode::Internal)
    {
        const auto available = juce::MidiInput::getAvailableDevices();
        const auto requested = juceString(
            previousSync.sourceIdentifier);
        const auto found = std::ranges::find_if(
            available,
            [&](const auto& midi) {
                return midi.identifier == requested;
            });
        if (found == available.end())
        {
            settings.midiInputDeviceIdentifiers.push_back(
                previousSync.sourceIdentifier);
        }
        else if (!deviceManager_.isMidiInputDeviceEnabled(
                     found->identifier))
        {
            settings.transportSync.sourceMode =
                talas::project::TransportSyncSourceMode::Internal;
            settings.transportSync.sourceIdentifier.clear();
        }
    }
    std::ranges::sort(settings.midiInputDeviceIdentifiers);
    const auto uniqueEnd =
        std::ranges::unique(settings.midiInputDeviceIdentifiers).begin();
    settings.midiInputDeviceIdentifiers.erase(
        uniqueEnd,
        settings.midiInputDeviceIdentifiers.end());
    switch (talas::app::workbenchDeviceNotificationAction(
        audioSettingsComponent_ != nullptr,
        projectBridge_.editor().document().deviceSettings,
        settings))
    {
    case talas::app::WorkbenchDeviceNotificationAction::Ignore:
        return;
    case talas::app::WorkbenchDeviceNotificationAction::KeepProjectPreference:
        rebuildTransportSyncAuthority();
        setStatus("Using available host devices; project Audio/MIDI settings unchanged");
        return;
    case talas::app::WorkbenchDeviceNotificationAction::RecordInProject:
        break;
    }

    const auto result =
        projectBridge_.editor().setDeviceSettings(std::move(settings));
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged(false);
    rebuildTransportSyncAuthority();
    setStatus("Project Audio/MIDI settings updated");
}

juce::String WorkbenchComponent::applyProjectDeviceSettings()
{
    if (!hardwareEnabled_)
        return {};
    const auto& settings =
        projectBridge_.editor().document().deviceSettings;
    if (!settings.configured)
        return {};

    applyingProjectDeviceSettings_ = true;
    const auto applicationGeneration =
        ++deviceSettingsApplicationGeneration_;
    const auto previousSetup = deviceManager_.getAudioDeviceSetup();
    auto setup = previousSetup;
    juce::String warning;
    if (auto* type = deviceManager_.getCurrentDeviceTypeObject())
    {
        const auto inputNames = type->getDeviceNames(true);
        const auto outputNames = type->getDeviceNames(false);
        const auto requestedInput =
            juceString(settings.audioInputDeviceName);
        const auto requestedOutput =
            juceString(settings.audioOutputDeviceName);
        if (requestedInput.isEmpty()
            || inputNames.contains(requestedInput))
        {
            setup.inputDeviceName = requestedInput;
        }
        else
        {
            warning = "Saved audio input is unavailable";
        }
        if (requestedOutput.isEmpty()
            || outputNames.contains(requestedOutput))
        {
            setup.outputDeviceName = requestedOutput;
        }
        else if (warning.isEmpty())
        {
            warning = "Saved audio output is unavailable";
        }
    }
    setup.sampleRate = settings.sampleRate;
    setup.bufferSize = settings.bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    for (const auto channel : settings.inputChannels)
        setup.inputChannels.setBit(channel - 1);
    for (const auto channel : settings.outputChannels)
        setup.outputChannels.setBit(channel - 1);
    const auto audioError =
        deviceManager_.setAudioDeviceSetup(setup, true);
    if (audioError.isNotEmpty())
    {
        const auto restoreError =
            deviceManager_.setAudioDeviceSetup(previousSetup, true);
        warning = "Saved audio settings unavailable: " + audioError;
        if (restoreError.isNotEmpty())
            warning += " (host audio restore also failed: " + restoreError + ")";
    }

    const auto availableMidi =
        juce::MidiInput::getAvailableDevices();
    std::vector<juce::String> previouslyEnabledMidi;
    for (const auto& device : availableMidi)
    {
        if (deviceManager_.isMidiInputDeviceEnabled(device.identifier))
            previouslyEnabledMidi.push_back(device.identifier);
    }
    const auto allMidiAvailable = std::ranges::all_of(
        settings.midiInputDeviceIdentifiers,
        [&](const auto& identifier) {
            const auto requested = juceString(identifier);
            return std::ranges::any_of(
                availableMidi,
                [&](const auto& device) {
                    return device.identifier == requested;
                });
        });
    if (allMidiAvailable)
    {
        for (const auto& device : availableMidi)
        {
            deviceManager_.setMidiInputDeviceEnabled(
                device.identifier,
                false);
        }
    }
    for (const auto& identifier :
         settings.midiInputDeviceIdentifiers)
    {
        const auto requested = juceString(identifier);
        const auto found = std::ranges::find_if(
            availableMidi,
            [&](const auto& device) {
                return device.identifier == requested;
            });
        if (allMidiAvailable && found != availableMidi.end())
        {
            deviceManager_.setMidiInputDeviceEnabled(
                found->identifier,
                true);
        }
        else if (!allMidiAvailable && warning.isEmpty())
        {
            warning = "A saved MIDI input is unavailable";
        }
    }
    const auto midiApplySucceeded = allMidiAvailable
        && std::ranges::all_of(
            settings.midiInputDeviceIdentifiers,
            [&](const auto& identifier) {
                return deviceManager_.isMidiInputDeviceEnabled(
                    juceString(identifier));
            });
    if (allMidiAvailable && !midiApplySucceeded)
    {
        for (const auto& device : availableMidi)
        {
            deviceManager_.setMidiInputDeviceEnabled(
                device.identifier,
                false);
        }
        for (const auto& identifier : previouslyEnabledMidi)
            deviceManager_.setMidiInputDeviceEnabled(identifier, true);
        if (warning.isEmpty())
            warning = "Saved MIDI inputs could not be opened";
        const auto midiRestoreSucceeded = std::ranges::all_of(
            availableMidi,
            [&](const auto& device) {
                const auto wasEnabled = std::ranges::find(
                    previouslyEnabledMidi,
                    device.identifier)
                    != previouslyEnabledMidi.end();
                return deviceManager_.isMidiInputDeviceEnabled(
                           device.identifier)
                    == wasEnabled;
            });
        if (!midiRestoreSucceeded)
            warning += " (host MIDI restore also failed)";
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync(
        [safeThis, applicationGeneration] {
            if (safeThis != nullptr
                && safeThis->deviceSettingsApplicationGeneration_
                    == applicationGeneration)
            {
                safeThis->applyingProjectDeviceSettings_ = false;
                safeThis->rebuildMidiInputSourceRegistry();
            }
        });
    if (warning.isNotEmpty())
        warning += "; using available host devices";
    return warning;
}

void WorkbenchComponent::scheduleClearAfterAudioStopped()
{
    // setAudioDeviceSetup stops and restarts the device synchronously, so by
    // the time this runs the callback may already render the same engine
    // again. Only the generation that stopped may clear it; a restart has
    // restaged through audioDeviceAboutToStart instead.
    const auto generation = deviceGeneration_.load(std::memory_order_acquire);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, generation] {
        if (safeThis != nullptr
            && safeThis->deviceGeneration_.load(std::memory_order_acquire) == generation)
        {
            safeThis->performancePanel_->synchronizeLiveToolSessionState();
            safeThis->noteWorkspaceSessionChanged();
            safeThis->projectBridge_.clearAfterAudioStopped();
            safeThis->setAuditionPresentation(std::nullopt);
            safeThis->transportPlaying_ = false;
            safeThis->analysisPanel_->setPlaying(false);
            safeThis->analysisPanel_->clearCapture();
            for (auto& peak : safeThis->outputMeterPeaks_)
                peak.store(0.0F, std::memory_order_relaxed);
            safeThis->outputMeterStageId_.store(
                0, std::memory_order_release);
            safeThis->currentDocumentStaged_ = false;
            safeThis->refreshPersistentStatus();
            safeThis->commandManager_
                .commandStatusChanged();
        }
    });
}

void WorkbenchComponent::showAudioSettings()
{
    std::vector<WorkbenchTransportSyncSourceOption> syncSources;
    syncSources.reserve(midiInputSources_.size() * 2);
    for (const auto& source : midiInputSources_)
    {
        syncSources.push_back({
            source.displayName,
            source.identifier.toStdString(),
            talas::project::TransportSyncSourceMode::MidiClockExactInput,
        });
        syncSources.push_back({
            source.displayName,
            source.identifier.toStdString(),
            talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput,
        });
    }
    const auto selectedSync = projectBridge_.editor().document()
                                  .deviceSettings.transportSync;
    const auto oscSettings = projectBridge_.editor().document()
                                 .deviceSettings.oscIngress;
    const auto oscEgressSettings = projectBridge_.editor().document()
                                       .deviceSettings.oscEgress;
    latestOscLoopbackTelemetry_ = oscLoopbackProfile_.telemetry();
    auto oscProfileState = juce::String{"OSC profile disabled"};
    switch (latestOscLoopbackTelemetry_.state)
    {
    case WorkbenchOscLoopbackProfileState::Ready:
        oscProfileState = juceString("Ready · generation ")
            + juce::String(static_cast<juce::int64>(
                latestOscLoopbackTelemetry_.profileGeneration));
        break;
    case WorkbenchOscLoopbackProfileState::RetainedAfterFailure:
        oscProfileState = "Retained after failed profile request";
        break;
    case WorkbenchOscLoopbackProfileState::RouteUnavailable:
        oscProfileState = "Profile routes unavailable for active stage";
        break;
    case WorkbenchOscLoopbackProfileState::Preparing:
        oscProfileState = "Preparing OSC loopback profile";
        break;
    case WorkbenchOscLoopbackProfileState::Failed:
        oscProfileState = "OSC loopback profile failed";
        break;
    case WorkbenchOscLoopbackProfileState::Disabled:
        break;
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    auto selector = createAudioMidiSettingsComponent(
        deviceManager_,
        std::move(syncSources),
        selectedSync,
        [safeThis](talas::project::ProjectTransportSyncSettings sync) {
            if (safeThis != nullptr)
                safeThis->setTransportSyncSource(std::move(sync));
        },
        oscSettings,
        oscEgressSettings,
        std::move(oscProfileState),
        [safeThis](talas::project::ProjectOscIngressSettings ingress,
                   talas::project::ProjectOscEgressSettings egress) {
            if (safeThis == nullptr)
                return juce::String{"Workbench closed"};
            return safeThis->setOscLoopbackProfileSettings(
                ingress, egress);
        });
    audioSettingsComponent_ = selector.get();
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle = "Audio & MIDI Settings";
    options.dialogBackgroundColour = panelColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void WorkbenchComponent::refreshPersistentStatus()
{
    const auto dirty = editBoundary_.isDirty(
        projectBridge_.editor().document());
    const auto projectState = dirty
        ? juce::String{"Unsaved"}
        : juce::String{"Saved"};
    const auto compileState = !latestCompileDiagnostics_.empty()
        ? juce::String(static_cast<int>(latestCompileDiagnostics_.size()))
            + (latestCompileDiagnostics_.size() == 1
                   ? " issue"
                   : " issues")
        : (currentDocumentStaged_
               ? juce::String{"Compiled"}
               : juce::String{"Compile pending"});
    const auto transportState = sampleMapAuditionActive_
        ? (transportPlaying_
               ? juce::String{"Previewing Samples"}
               : juce::String{"Samples preview paused"})
        : auditionActive_
        ? (transportPlaying_
               ? juce::String{"Previewing "}
                   + juceString(
                       auditionDefinitionId_.value_or("Sound"))
               : juceString("Preview paused · ")
                   + juceString(
                       auditionDefinitionId_.value_or("Sound")))
        : (transportPlaying_
               ? juce::String{"Playing"}
               : (currentDocumentStaged_
                      ? juce::String{"Paused"}
                      : juce::String{"Stopped"}));
    auto text = projectState + juceString(" · ") + compileState
        + juceString(" · ") + transportState;
    if (preparedProjectStatusHeadline_.isNotEmpty())
    {
        text += juceString(" · ")
            + preparedProjectStatusHeadline_;
    }
    if (midiRuntimeRouteDiagnostic_.isNotEmpty())
        text += juceString(" · ") + midiRuntimeRouteDiagnostic_;
    const auto oscIngressHasIssue = projectBridge_.editor().document()
        .deviceSettings.oscIngress.enabled && (!oscRoutesAvailable_
            || latestOscIngressTelemetry_.state
                != WorkbenchOscIngressState::Listening
            || latestOscIngressTelemetry_.failedPort != 0);
    if (oscIngressHasIssue && oscIngressDiagnostic_.isNotEmpty())
        text += juceString(" · ") + oscIngressDiagnostic_;
    const auto oscEgressHasIssue = projectBridge_.editor().document()
        .deviceSettings.oscEgress.enabled && (!oscEgressRoutesAvailable_
            || latestOscEgressTelemetry_.state
                != WorkbenchOscEgressState::Ready
            || latestOscEgressTelemetry_.failedPort != 0);
    if (oscEgressHasIssue && oscEgressDiagnostic_.isNotEmpty())
        text += juceString(" · ") + oscEgressDiagnostic_;
    if (const auto oscProfile = oscProfileStatusText();
        oscProfile.isNotEmpty())
    {
        text += juceString(" · ") + oscProfile;
    }
    if (midiClockDiagnostic_.isNotEmpty())
        text += juceString(" · ") + midiClockDiagnostic_;
    auto accessibleTitle = "Project state: " + text;
    if (midiClockAccessibleDetail_.isNotEmpty())
        accessibleTitle += ". " + midiClockAccessibleDetail_;
    if (preparedAccessibleStatus_.isNotEmpty())
        accessibleTitle += ". " + preparedAccessibleStatus_;
    const auto changed = projectStateLabel_.getText() != text
        || projectStateLabel_.getTitle() != accessibleTitle;
    const auto transportSyncHasIssue =
        workbenchTransportSyncHasIssue(midiClockDiagnostic_);
    projectStateLabel_.setText(text, juce::dontSendNotification);
    projectStateLabel_.setTitle(accessibleTitle);
    projectStateLabel_.setColour(
        juce::Label::textColourId,
        latestCompileDiagnostics_.empty()
                && midiRuntimeRouteDiagnostic_.isEmpty()
                && !oscIngressHasIssue
                && !oscEgressHasIssue
                && !transportSyncHasIssue
                && !preparedProjectStatusHeadline_.containsIgnoreCase(
                    "issue")
            ? secondaryTextColour
            : errorColour);
    if (changed)
    {
        if (auto* handler = projectStateLabel_.getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::titleChanged);
        }
    }
}

}
