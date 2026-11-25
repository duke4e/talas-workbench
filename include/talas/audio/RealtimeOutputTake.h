#pragma once

#include "talas/audio/AudioFilePublication.h"
#include "talas/audio/WavWriter.h"
#include "talas/runtime/RealtimeOutputRecorder.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
#include <functional>
#endif
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace talas::audio
{
inline constexpr std::size_t defaultRealtimeOutputTakeQueueCapacity = 16;

enum class RealtimeOutputTakeState : std::uint8_t
{
    Recording,
    Stopping,
    Completed,
    Cancelled,
    Failed,
};

struct RealtimeOutputTakeOptions
{
    std::filesystem::path destination;
    ExistingAudioFilePolicy existingPolicy = ExistingAudioFilePolicy::Reject;
    double sampleRate = 0.0;
    std::size_t channelCount = 0;
    std::size_t maxBlockFrames = 0;
    std::size_t queueCapacity = defaultRealtimeOutputTakeQueueCapacity;
    std::uint64_t sourceStageId = 0;
    std::uint64_t sourceAuthorityGeneration = 1;
    std::uint64_t sourceDocumentAuthorityGeneration = 1;
    std::vector<std::filesystem::path> protectedFiles;
    std::vector<std::filesystem::path> protectedDirectories;
    // The take stops and publishes what it recorded on reaching this many
    // frames. It is always also bounded by the RIFF/WAVE size limit.
    std::optional<std::uint64_t> maximumFrames;
    // The take's word size: 16 bits, or a DiskRecorder's 8 or 24.
    WavWordSize wordSize;
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
    std::function<void()> afterTemporaryFileOpenForTesting;
    std::function<void()> beforeRecorderDrainForTesting;
    std::function<void()> beforePublicationAdmissionForTesting;
    std::function<void()> afterPublicationAdmissionForTesting;
#endif
};

struct RealtimeOutputTakeSnapshot
{
    RealtimeOutputTakeState state = RealtimeOutputTakeState::Failed;
    std::filesystem::path destination;
    double sampleRate = 0.0;
    std::size_t channelCount = 0;
    std::uint64_t sourceStageId = 0;
    std::uint64_t sourceAuthorityGeneration = 0;
    std::uint64_t sourceDocumentAuthorityGeneration = 0;
    std::uint64_t writtenFrames = 0;
    std::uint64_t droppedBlocks = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t rejectedBlocks = 0;
    bool incomplete = false;
    std::string diagnostic;

    [[nodiscard]] bool terminal() const noexcept
    {
        return state == RealtimeOutputTakeState::Completed
            || state == RealtimeOutputTakeState::Cancelled
            || state == RealtimeOutputTakeState::Failed;
    }
};

// Owns the worker, growing WAV writer, private staging authority, and one
// bounded recorder queue. All filesystem operations remain on the worker or
// calling control thread; publishFromAudio() only copies into prepared memory.
class RealtimeOutputTake
{
public:
    [[nodiscard]] static std::unique_ptr<RealtimeOutputTake> start(
        RealtimeOutputTakeOptions options,
        std::string& error);

    ~RealtimeOutputTake();

    RealtimeOutputTake(const RealtimeOutputTake&) = delete;
    RealtimeOutputTake& operator=(const RealtimeOutputTake&) = delete;

    [[nodiscard]] bool publishFromAudio(
        float* const* channels,
        std::size_t channelCount,
        std::size_t frameCount,
        std::uint64_t activeStageId,
        std::uint64_t activeAuthorityGeneration = 1,
        std::uint64_t activeDocumentAuthorityGeneration = 1) noexcept;

    // Bounded audio-thread rejection for a failed source render. This records
    // incomplete evidence and closes admission without filesystem work.
    void rejectRenderFailureFromAudio() noexcept;

    [[nodiscard]] std::size_t channelCount() const noexcept;

    void requestStop() noexcept;
    void requestCancel() noexcept;
    void requestAuthorityInvalidation() noexcept;
    void join();
    [[nodiscard]] RealtimeOutputTakeSnapshot snapshot() const;

private:
    RealtimeOutputTake(
        RealtimeOutputTakeOptions options,
        std::unique_ptr<AudioFilePublication> publication,
        std::unique_ptr<talas::runtime::RealtimeOutputRecorder> recorder);

    void rejectFromAudio(
        bool stageMismatch,
        bool authorityMismatch,
        bool renderFailure) noexcept;
    void run(std::stop_token stopToken) noexcept;
    void setTerminal(
        RealtimeOutputTakeState state,
        std::string diagnostic,
        std::uint64_t writtenFrames) noexcept;

    RealtimeOutputTakeOptions options_;
    std::unique_ptr<AudioFilePublication> publication_;
    std::unique_ptr<talas::runtime::RealtimeOutputRecorder> recorder_;
    std::atomic<std::uint8_t> request_ = 0;
    std::atomic<RealtimeOutputTakeState> state_ =
        RealtimeOutputTakeState::Recording;
    std::jthread worker_;
    mutable std::mutex resultMutex_;
    std::string diagnostic_;
    std::uint64_t writtenFrames_ = 0;
    std::atomic<std::uint64_t> liveWrittenFrames_ = 0;
    std::atomic<std::uint64_t> stageRejectedBlocks_ = 0;
    std::atomic<bool> stageBindingRejected_ = false;
    std::atomic<bool> authorityBindingRejected_ = false;
    std::atomic<bool> renderFailureRejected_ = false;
    mutable std::mutex publicationMutex_;
    bool publicationRevoked_ = false;
    bool publicationCommitted_ = false;
};
}
