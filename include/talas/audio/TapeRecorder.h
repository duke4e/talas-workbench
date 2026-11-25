#pragma once

#include "talas/audio/RealtimeOutputTake.h"
#include "talas/runtime/RealtimeOutputRecorder.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The Tape Recorder tool: a tape recorder's control panel recording the audio
// presented to the inputs. Record puts it into monitor mode, the pause button
// pressed and the input heard; play, or the pause button again, starts
// recording; pause pauses and resumes the recording, stop ends it; play then
// plays the recording back. Max(L) and Max(R) show the largest input levels
// seen since Reset, the two faders attenuate the inputs, and the Left and
// Right buttons choose which inputs are recorded.
namespace talas::audio
{
enum class TapeRecorderState
{
    Stopped,
    // Record pressed: the input monitored, the pause button down.
    Monitoring,
    Recording,
    RecordingPaused,
    Playing,
    PlayingPaused,
};

class TapeRecorderTransport
{
public:
    [[nodiscard]] TapeRecorderState state() const noexcept { return state_; }
    void record() noexcept;
    void pause() noexcept;
    // `hasRecording` lets play play the last recording back when stopped.
    void play(bool hasRecording) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool pauseDown() const noexcept;
    [[nodiscard]] bool recordDown() const noexcept;
    // The input is heard while monitoring and recording.
    [[nodiscard]] bool monitors() const noexcept;
    [[nodiscard]] bool records() const noexcept { return state_ == TapeRecorderState::Recording; }
    [[nodiscard]] bool plays() const noexcept { return state_ == TapeRecorderState::Playing; }

private:
    TapeRecorderState state_ = TapeRecorderState::Stopped;
};

// The audio side: the inputs through the faders, heard and measured while
// monitoring, sent to a take while recording, and a recording played back.
// The control-thread setters publish atomically; replacing the take or the
// clip waits for a callback in progress to finish.
// The most an Input fader boosts its input.
inline constexpr float tapeRecorderMaximumInputGain = 16.0F;

class TapeRecorderEngine
{
public:
    // The stage ID a take for the Tape Recorder starts with.
    static constexpr std::uint64_t takeStageId = 1;
    // Longer callbacks are recorded in blocks of this many frames at most,
    // as a take's recorder admits.
    explicit TapeRecorderEngine(
        std::size_t maximumBlockFrames = talas::runtime::maximumRealtimeOutputRecorderBlockFrames);

    // The largest block a take is given, the take's maxBlockFrames.
    [[nodiscard]] std::size_t maximumBlockFrames() const noexcept { return maximumBlockFrames_; }

    // An Input fader boosts a level too low, from
    // unity up to tapeRecorderMaximumInputGain; it cannot attenuate.
    void setInputLevel(std::size_t channel, float level) noexcept;
    [[nodiscard]] float inputLevel(std::size_t channel) const noexcept;
    void setChannelEnabled(std::size_t channel, bool enabled) noexcept;
    [[nodiscard]] bool channelEnabled(std::size_t channel) const noexcept;
    [[nodiscard]] std::size_t enabledChannelCount() const noexcept;
    void setMonitoring(bool monitoring) noexcept;
    [[nodiscard]] bool monitoring() const noexcept;
    // The take the inputs go to, nullptr for none; the caller owns it and
    // keeps it alive until replaced.
    void setRecordingTake(RealtimeOutputTake* take) noexcept;
    void setRecordingPaused(bool paused) noexcept;
    // The recording to play back, one vector per channel.
    void setPlaybackClip(std::shared_ptr<const std::vector<std::vector<float>>> clip) noexcept;
    void setPlaying(bool playing) noexcept;
    [[nodiscard]] bool playing() const noexcept;
    void rewind() noexcept;

    // The input's level in the last callback, after its fader.
    [[nodiscard]] float currentLevel(std::size_t channel) const noexcept;
    [[nodiscard]] float maxLevel(std::size_t channel) const noexcept;
    void resetMaxLevels() noexcept;
    [[nodiscard]] std::uint64_t recordedFrames() const noexcept;
    [[nodiscard]] std::uint64_t playedFrames() const noexcept;

    // Audio thread: adds to the outputs.
    void process(std::span<const float* const> inputs, std::span<float* const> outputs, std::size_t frames) noexcept;

private:
    void quiesce() const noexcept;

    std::size_t maximumBlockFrames_;
    std::array<std::atomic<float>, 2> levels_{};
    std::array<std::atomic<bool>, 2> enabled_{};
    std::array<std::atomic<float>, 2> current_{};
    std::array<std::atomic<float>, 2> maxima_{};
    std::atomic<bool> monitoring_ = false;
    std::atomic<bool> recordingPaused_ = false;
    std::atomic<RealtimeOutputTake*> take_ = nullptr;
    std::atomic<bool> playing_ = false;
    std::atomic<std::uint64_t> recorded_ = 0;
    std::atomic<std::uint64_t> played_ = 0;
    std::atomic<int> inFlight_ = 0;
    std::shared_ptr<const std::vector<std::vector<float>>> clip_;
    std::atomic<const std::vector<std::vector<float>>*> clipView_ = nullptr;
    std::array<std::vector<float>, 2> scratch_;
};
}
