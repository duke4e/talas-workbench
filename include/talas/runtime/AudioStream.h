#pragma once

#include "talas/core/ResourceSnapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::runtime
{
using AudioStreamCancellationCheck = std::function<bool()>;

struct AudioStreamPrepareResult;

enum class AudioStreamWorkerMode : std::uint8_t
{
    Background,
    ManualTest,
};

// Which of a sample file's channels a stream reads: both averaged, as a
// mono Sound hears them, or the left or the right alone (a one-channel
// file's only channel for either).
enum class AudioStreamChannel : std::uint8_t
{
    Mix,
    Left,
    Right,
};

struct AudioStreamConfiguration
{
    std::size_t blockFrames = 256;
    std::size_t requestCapacity = 4;
    std::size_t responseCapacity = 8;
    std::size_t decodedCacheBlocks = 16;
    std::size_t lowWatermark = 2;
    std::size_t highWatermark = 6;
    AudioStreamWorkerMode workerMode =
        AudioStreamWorkerMode::Background;
    AudioStreamChannel channel = AudioStreamChannel::Mix;
};

enum class AudioStreamWorkerState : std::uint8_t
{
    Waiting,
    Filling,
    Ready,
    EndOfStream,
    Failed,
};

struct AudioStreamHealthSnapshot
{
    std::uint64_t underrunEvents = 0;
    std::uint64_t missingFrames = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t requestRejections = 0;
    std::uint64_t responseQueueFull = 0;
    std::uint64_t staleResponseBlocks = 0;
    std::uint64_t decodedBlocks = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t endOfStreamFrames = 0;
    std::size_t bufferedBlocks = 0;
    std::size_t bufferCapacity = 0;
    std::size_t lowWatermark = 0;
    std::size_t highWatermark = 0;
    std::size_t pendingRequests = 0;
    AudioStreamWorkerState workerState =
        AudioStreamWorkerState::Waiting;
};

// Control-thread aggregate authority for immutable prepared read windows.
// Every retained consumer block is charged exactly once before publication.
struct AudioStreamPreparedWindowBudget
{
    std::size_t maximumBlocks = 0;
    std::size_t maximumBytes = 0;
    std::size_t usedBlocks = 0;
    std::size_t usedBytes = 0;
};

[[nodiscard]] std::string_view audioStreamWorkerStateName(
    AudioStreamWorkerState state) noexcept;

enum class AudioStreamReadStatus : std::uint8_t
{
    Available,
    Missing,
    EndOfStream,
    InvalidIncrement,
};

struct AudioStreamReadResult
{
    AudioStreamReadStatus status =
        AudioStreamReadStatus::Missing;
    double sample = 0.0;
};

struct DecodedAudioData
{
    double sampleRate = 0.0;
    std::vector<double> monoSamples;
};

struct AudioStreamFullLoadResult
{
    std::shared_ptr<const DecodedAudioData> data;
    std::string error;
    bool allocationFailed = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return data != nullptr && error.empty();
    }
};

// Computes rateScale * sourceSampleRate / destinationSampleRate without an
// avoidable intermediate overflow. Returns no value unless the final source
// increment is positive and representable as a finite double.
[[nodiscard]] std::optional<double>
audioStreamSourceFrameIncrement(
    double rateScale,
    double sourceSampleRate,
    double destinationSampleRate) noexcept;

class DecodedBlockCacheOwner;

class AudioStreamConsumer
{
public:
    ~AudioStreamConsumer();

    AudioStreamConsumer(const AudioStreamConsumer&) = delete;
    AudioStreamConsumer& operator=(const AudioStreamConsumer&) = delete;
    AudioStreamConsumer(AudioStreamConsumer&&) = delete;
    AudioStreamConsumer& operator=(AudioStreamConsumer&&) = delete;

    // Audio-thread operation. A successful seek advances this consumer's
    // generation and publishes one bounded refill request without waiting.
    [[nodiscard]] bool seek(double sourceFrame) noexcept;
    // Audio-thread operation. The signed non-zero increment selects the
    // prepared refill direction while preserving the same generation rule.
    [[nodiscard]] bool seek(
        double sourceFrame,
        double sourceFrameIncrement) noexcept;
    // Audio-thread operation. Restarts only at the exact control-thread
    // prepared restart identity and serves its retained window immediately.
    // The internal refill epoch advances to reject in-flight stale responses,
    // but this is not published as an ordinary seek generation.
    [[nodiscard]] bool restartPrepared(
        double sourceFrame,
        double sourceFrameIncrement) noexcept;
    // Audio-thread operation. Repositions only to an exact control-thread
    // prepared cursor/direction identity. This replaces the internal refill
    // epoch and stale queue without publishing an ordinary seek generation.
    [[nodiscard]] bool relocatePrepared(
        double sourceFrame,
        double sourceFrameIncrement) noexcept;
    // Audio-thread operation. Reads prepared mono data only, advances the
    // source cursor for Available and Missing results, and never blocks.
    [[nodiscard]] AudioStreamReadResult readNext(
        double sourceFrameIncrement) noexcept;
    // Audio-thread operation for prepared loop/reverse consumers. Interpolates
    // between floor(sourceFrame) and the explicit whole-frame right endpoint,
    // then publishes nextSourceFrame as this consumer's logical cursor. This
    // permits exact loop-boundary interpolation without decode, I/O, or wait.
    [[nodiscard]] AudioStreamReadResult readInterpolated(
        double sourceFrame,
        double interpolationRightSourceFrame,
        double nextSourceFrame,
        double sourceFrameIncrement) noexcept;

    [[nodiscard]] double sourceFrameCursor() const noexcept;
    // Control-thread inspection; never call from the audio callback.
    [[nodiscard]] std::uint64_t seekGeneration() const noexcept;
    [[nodiscard]] AudioStreamHealthSnapshot health() const noexcept;

private:
    friend class RealtimeRenderSession;
    friend struct AudioStreamConsumerTestAccess;
    // Audio-thread parent transaction. Keep response queue ownership until
    // commit; cancellation restores only consumer-owned candidate state.
    [[nodiscard]] bool beginCallbackCheckpoint() noexcept;
    void commitCallbackCheckpoint() noexcept;
    void rollbackCallbackCheckpoint() noexcept;
    // Internal source reset to a prepared read window, or its saved EOF cursor.
    bool restorePreparedSourceBoundary(double sourceFrame, double increment) noexcept;
    bool canRestorePreparedSourceBoundary(double sourceFrame, double increment,
        std::uint64_t refillCount) const noexcept;

    // Exclusive offline/control-thread processing checkpoint. The consumer must
    // outlive it and must not be rendered concurrently. Worker access is guarded
    // only during capture/restore. Deferred inspection retains entry health and
    // generation until commit/restore; the processing cursor remains live.
    class OfflineCheckpoint
    {
    public:
        class State;
        explicit OfflineCheckpoint(std::unique_ptr<State> state) noexcept;
        ~OfflineCheckpoint();
        void commit() noexcept;
    private:
        std::unique_ptr<State> state_;
    };
    // deterministicReads decodes the immutable source outside the callback;
    // the resulting read authority exists only for this offline transaction.
    [[nodiscard]] std::unique_ptr<OfflineCheckpoint> beginOfflineCheckpoint(bool deferInspection = false, bool deterministicReads = false);
    class Impl;
    AudioStreamConsumer(
        std::shared_ptr<DecodedBlockCacheOwner> owner,
        std::unique_ptr<Impl> impl);

    std::shared_ptr<DecodedBlockCacheOwner> owner_;
    std::unique_ptr<Impl> impl_;

    friend class DecodedBlockCacheOwner;
};

class DecodedBlockCacheOwner
    : public std::enable_shared_from_this<DecodedBlockCacheOwner>
{
public:
    ~DecodedBlockCacheOwner();

    DecodedBlockCacheOwner(const DecodedBlockCacheOwner&) = delete;
    DecodedBlockCacheOwner& operator=(const DecodedBlockCacheOwner&) = delete;
    DecodedBlockCacheOwner(DecodedBlockCacheOwner&&) = delete;
    DecodedBlockCacheOwner& operator=(DecodedBlockCacheOwner&&) = delete;

    // Control-thread preparation. Consumer storage and both SPSC queues are
    // fully allocated before this call returns.
    [[nodiscard]] std::shared_ptr<AudioStreamConsumer>
    createConsumer(
        double initialSourceFrame,
        double initialSourceFrameIncrement = 1.0);

    // Control-thread only. Waits for the configured high watermark, clamped
    // to the remaining source blocks; never call
    // from an audio callback.
    [[nodiscard]] bool prewarm(
        const std::shared_ptr<AudioStreamConsumer>& consumer,
        std::chrono::milliseconds timeout);

    // Control-thread only. Retains an immutable window around sourceFrame that
    // covers both the high watermark and maximumOutputFrames at the supplied
    // increment. The exact cursor/direction is also bound for callback-visible
    // relocation. All storage and private-backing I/O complete before this
    // call returns; never call from an audio callback.
    [[nodiscard]] bool prepareReadWindow(
        const std::shared_ptr<AudioStreamConsumer>& consumer,
        double sourceFrame,
        double sourceFrameIncrement,
        std::size_t maximumOutputFrames,
        AudioStreamPreparedWindowBudget& budget);
    // Control-thread only. Prepares and binds the consumer's one exact
    // callback-visible restart identity.
    [[nodiscard]] bool prepareRestartWindow(
        const std::shared_ptr<AudioStreamConsumer>& consumer,
        double sourceFrame,
        double sourceFrameIncrement,
        std::size_t maximumOutputFrames,
        AudioStreamPreparedWindowBudget& budget);

    // Deterministic test seam; returns zero for a background-worker owner.
    [[nodiscard]] std::size_t pumpForTest(
        std::size_t maximumBlocks = 1024);
    [[nodiscard]] std::string_view resourceId() const noexcept;
    [[nodiscard]] AudioStreamChannel channel() const noexcept;
    // Non-realtime deterministic oracle used by offline rendering and stream
    // differential tests. Reads only the owner's private immutable backing.
    [[nodiscard]] AudioStreamFullLoadResult loadAllForOffline() const;

private:
    class Impl;
    explicit DecodedBlockCacheOwner(std::unique_ptr<Impl> impl);
    void unregisterConsumer(AudioStreamConsumer* consumer) noexcept;

    std::unique_ptr<Impl> impl_;

    friend class AudioStreamConsumer;
    friend struct AudioStreamPrepareResult;
    friend AudioStreamPrepareResult prepareAudioStream(
        talas::core::ResourceSnapshotPtr,
        AudioStreamConfiguration,
        AudioStreamCancellationCheck);
};

struct AudioStreamPrepareResult
{
    std::shared_ptr<DecodedBlockCacheOwner> owner;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return owner != nullptr && error.empty() && !cancelled;
    }
};

[[nodiscard]] AudioStreamPrepareResult prepareAudioStream(
    talas::core::ResourceSnapshotPtr snapshot,
    AudioStreamConfiguration configuration = {},
    AudioStreamCancellationCheck shouldCancel = {});
}
