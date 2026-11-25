#include "app/workbench/WorkbenchLiveTimelineRecording.h"

#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/LiveRecordingModel.h"

#include <chrono>
#include <thread>

namespace talas::app::workbench
{
LiveTimelineRecordingSlot::Use::Use(LiveTimelineRecordingSlot& slot) noexcept
    : slot_(slot)
{
    slot_.users_.fetch_add(1, std::memory_order_seq_cst);
    recording_ = slot_.recording_.load(std::memory_order_seq_cst);
}

LiveTimelineRecordingSlot::Use::~Use()
{
    slot_.users_.fetch_sub(1, std::memory_order_seq_cst);
}

void LiveTimelineRecordingSlot::attach(talas::audio::LiveTimelineRecording* recording) noexcept
{
    recording_.store(recording, std::memory_order_seq_cst);
}

void LiveTimelineRecordingSlot::detach() noexcept
{
    recording_.store(nullptr, std::memory_order_seq_cst);
    while (users_.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

bool LiveTimelineRecordingSlot::attached() const noexcept
{
    return recording_.load(std::memory_order_acquire) != nullptr;
}

LiveRecordingMouseShield::LiveRecordingMouseShield(juce::Component& surface, std::function<void()> onRefusedClick)
    : surface_(surface), onRefusedClick_(std::move(onRefusedClick))
{
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
    setTitle("Recording to disk");
    setDescription(juceString(talas::app::liveRecordingMouseRefusal()));
    surface_.addAndMakeVisible(*this);
    surface_.addComponentListener(this);
    setBounds(surface_.getLocalBounds());
    toFront(false);
}

LiveRecordingMouseShield::~LiveRecordingMouseShield()
{
    surface_.removeComponentListener(this);
    surface_.removeChildComponent(this);
}

void LiveRecordingMouseShield::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colours::red.withAlpha(0.6F));
    graphics.drawRect(getLocalBounds(), 2);
    graphics.setFont(juce::FontOptions(13.0F));
    graphics.drawText("Recording to disk", getLocalBounds().reduced(8).removeFromTop(18),
        juce::Justification::topRight);
}

void LiveRecordingMouseShield::mouseDown(const juce::MouseEvent&)
{
    surface_.grabKeyboardFocus();
    if (onRefusedClick_)
        onRefusedClick_();
}

void LiveRecordingMouseShield::componentMovedOrResized(juce::Component&, bool, bool)
{
    setBounds(surface_.getLocalBounds());
    toFront(false);
}

// Record to disk... of the Timeline plays it from its
// start, as Play does, and records what it sends to the Master's channels as
// they are heard, until the Timeline's last Sound ends: a WaitUntil holds it
// until its key down, and MIDI faders and presets are heard. The job runs
// off the message thread and waits for the end, showing the progress; a
// stop keeps what was recorded so far.
std::function<talas::audio::TimelineRecordingResult()> WorkbenchComponent::startLiveTimelineRecording(
    talas::audio::TimelineRecordingOptions options)
{
    const auto refused = [](std::string error) {
        return std::function<talas::audio::TimelineRecordingResult()>([error = std::move(error)] {
            talas::audio::TimelineRecordingResult result;
            result.error = error;
            return result;
        });
    };
    if (liveTimelineRecordingSlot_->attached())
        return refused("A recording to disk is already playing");
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || device->getCurrentSampleRate() <= 0.0 || device->getCurrentBufferSizeSamples() <= 0)
        return refused("Recording the Timeline live needs an audio device");
    options.projectFilePath = projectPath();
    options.sampleRate = device->getCurrentSampleRate();
    options.maxBlockFrames = static_cast<std::size_t>(device->getCurrentBufferSizeSamples());
    std::string error;
    std::shared_ptr<talas::audio::LiveTimelineRecording> recording{
        talas::audio::LiveTimelineRecording::start(projectBridge_.editor().document(), options, error)};
    if (!recording)
        return refused(error);
    restartCurrentProject();
    if (!currentDocumentStaged_)
    {
        (void) recording->cancel();
        return refused("The Timeline could not be compiled to record it as it plays");
    }
    // It records from the stage just compiled, from the Timeline's start.
    recording->startsWithStage(projectBridge_.latestPreparedStageId());
    liveTimelineRecordingSlot_->attach(recording.get());
    liveRecordingShield_ = std::make_unique<LiveRecordingMouseShield>(*virtualControlSurface_, [this] {
        setStatus(juceString(talas::app::liveRecordingMouseRefusal()));
    });
    setStatus("Recording the Timeline to disk as it plays: start it with a MIDI key down if it waits for one");

    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    return [slot = liveTimelineRecordingSlot_, recording, stop = std::move(options.shouldCancel),
               progress = std::move(options.progress), safeThis] {
        while (!recording->reachedEnd() && !(stop && stop()))
        {
            if (progress)
                progress(recording->progress());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        slot->detach();
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->liveRecordingShield_.reset();
        });
        auto result = recording->finish();
        if (result.ok() && progress)
            progress(1.0);
        return result;
    };
}

bool WorkbenchComponent::liveTimelineRecordingActive() const noexcept
{
    return liveTimelineRecordingSlot_->attached();
}
}
