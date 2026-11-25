#pragma once

#include "talas/audio/LiveTimelineRecording.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>

// Recording a Timeline to disk as it plays live.
namespace talas::app::workbench
{
// The live recording the audio callback hands each block to. A control
// thread attaches it and detaches it; once detach() returns, no audio
// callback uses it any more.
class LiveTimelineRecordingSlot
{
public:
    // Audio thread: the recording attached for the length of one callback.
    class Use
    {
    public:
        explicit Use(LiveTimelineRecordingSlot& slot) noexcept;
        ~Use();
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;

        [[nodiscard]] talas::audio::LiveTimelineRecording* get() const noexcept { return recording_; }
        explicit operator bool() const noexcept { return recording_ != nullptr; }
        talas::audio::LiveTimelineRecording* operator->() const noexcept { return recording_; }

    private:
        LiveTimelineRecordingSlot& slot_;
        talas::audio::LiveTimelineRecording* recording_ = nullptr;
    };

    void attach(talas::audio::LiveTimelineRecording* recording) noexcept;
    void detach() noexcept;
    [[nodiscard]] bool attached() const noexcept;

private:
    std::atomic<talas::audio::LiveTimelineRecording*> recording_ = nullptr;
    std::atomic<int> users_ = 0;
};

// Over the Virtual Control Surface while a recording to disk plays: the
// faders do not take the mouse. A click says so and gives the surface the
// keyboard, whose up and down arrows still choose presets; MIDI still moves
// the faders underneath.
class LiveRecordingMouseShield : public juce::Component, private juce::ComponentListener
{
public:
    LiveRecordingMouseShield(juce::Component& surface, std::function<void()> onRefusedClick);
    ~LiveRecordingMouseShield() override;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    void componentMovedOrResized(juce::Component& component, bool wasMoved, bool wasResized) override;

    juce::Component& surface_;
    std::function<void()> onRefusedClick_;
};
}
