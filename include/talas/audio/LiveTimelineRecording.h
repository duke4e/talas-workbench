#pragma once

#include "talas/audio/RealtimeOutputTake.h"
#include "talas/audio/TimelineRecording.h"
#include "talas/project/ProjectDocument.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Record to disk of a Timeline as it plays live. The
// Timeline plays in realtime and what it sends to the Master's chosen
// channels is recorded as it is heard, from the block in which it starts
// until its composition cursor reaches the end of its last Sound. A
// WaitUntil within the Timeline that waits for a MIDI key down holds that
// cursor, so the Timeline (and what follows it in the recording) starts at
// the key, and MIDI controllers and presets that change Event Values while
// it records are heard in the files. The files are those of
// recordTimelineToDisk, which renders the same mix offline without any
// live input.
namespace talas::audio
{
class LiveTimelineRecording
{
public:
    // The options' destination, channels, interleaving, format and bits
    // name the files as recordTimelineToDisk's do; its sample rate and
    // block size are the audio device's. A recording takes no progress or
    // cancel callbacks: progress() and cancel() stand for them.
    [[nodiscard]] static std::unique_ptr<LiveTimelineRecording> start(
        const talas::project::ProjectDocument& document,
        TimelineRecordingOptions options,
        std::string& error);

    ~LiveTimelineRecording();
    LiveTimelineRecording(const LiveTimelineRecording&) = delete;
    LiveTimelineRecording& operator=(const LiveTimelineRecording&) = delete;

    // The Master's output channels, and the end of the Timeline's last
    // Sound on its composition clock, in frames.
    [[nodiscard]] std::size_t masterChannelCount() const noexcept;
    [[nodiscard]] std::uint64_t endFrame() const noexcept;

    // Audio thread, before the Timeline renders: the Master's channels the
    // audio device has no output for render into the recording's own
    // buffers (zeroed), so a Timeline records more channels than the
    // hardware has.
    void lendChannels(std::span<float*> channels, std::size_t frameCount) noexcept;

    // Control thread, before the audio thread sees the recording: it starts
    // only with the render stage given, the one just compiled to play the
    // Timeline, and not with a stage still playing before it.
    void startsWithStage(std::uint64_t stageId) noexcept;

    // Audio thread, after the Timeline rendered a block: the Master's
    // channels, its composition cursor after the block (none without a
    // prepared Score) and the render stage. The recording starts with the
    // first block in which the Timeline starts from its beginning, binds
    // to that block's stage, and keeps the frames up to the end of the
    // Timeline; a block of another stage, or one that failed to render,
    // leaves the recording incomplete.
    void publishFromAudio(
        float* const* channels,
        std::size_t channelCount,
        std::size_t frameCount,
        std::optional<std::uint64_t> scoreFrameAfter,
        std::uint64_t stageId) noexcept;
    void rejectRenderFailureFromAudio() noexcept;

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool reachedEnd() const noexcept;
    // For the thermometer: how far the Timeline's composition cursor has
    // come, 1 at its end.
    [[nodiscard]] double progress() const noexcept;
    // Frames recorded, and the fewest of them any file has written so far.
    [[nodiscard]] std::uint64_t recordedFrames() const noexcept;
    [[nodiscard]] std::uint64_t writtenFrames() const;

    // Control thread, once the audio thread no longer publishes: the files
    // with what was recorded (up to the end, or as far as it went when
    // stopped early), saved in the format chosen.
    [[nodiscard]] TimelineRecordingResult finish();
    // Control thread: no files are left.
    [[nodiscard]] TimelineRecordingResult cancel();

private:
    struct File
    {
        std::vector<std::size_t> channels;
        std::filesystem::path destination;
        std::optional<std::filesystem::path> staging;
        std::unique_ptr<RealtimeOutputTake> take;
    };

    LiveTimelineRecording() = default;

    TimelineRecordingOptions options_;
    std::uint64_t endFrame_ = 0;
    std::size_t masterChannels_ = 0;
    std::vector<File> files_;
    std::vector<std::vector<float>> scratch_;
    std::vector<float> silence_;
    std::atomic<bool> started_ = false;
    std::atomic<bool> reachedEnd_ = false;
    std::atomic<bool> failed_ = false;
    std::atomic<std::uint64_t> stageId_ = 0;
    std::atomic<std::uint64_t> awaitedStageId_ = 0;
    std::atomic<std::uint64_t> scoreFrame_ = 0;
    std::atomic<std::uint64_t> recordedFrames_ = 0;
    bool finished_ = false;
};
}
