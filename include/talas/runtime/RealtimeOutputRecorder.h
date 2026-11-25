#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::runtime
{
inline constexpr std::size_t maximumRealtimeOutputRecorderChannels = 8;
inline constexpr std::size_t maximumRealtimeOutputRecorderBlockFrames = 4096;
inline constexpr std::size_t maximumRealtimeOutputRecorderQueueCapacity = 32;

struct RealtimeOutputRecorderBlock
{
    std::uint64_t sequence = 0;
    std::uint32_t channelCount = 0;
    std::uint32_t frameCount = 0;

    bool operator==(const RealtimeOutputRecorderBlock&) const = default;
};

enum class RealtimeOutputRecorderConsumeStatus : std::uint8_t
{
    Empty,
    Accepted,
    DestinationTooSmall,
    NotReady,
};

struct RealtimeOutputRecorderSnapshot
{
    bool ready = false;
    bool accepting = false;
    bool producerQuiescent = true;
    std::uint32_t channelCount = 0;
    std::uint32_t maxBlockFrames = 0;
    std::uint32_t queueCapacity = 0;
    std::uint64_t acceptedBlocks = 0;
    std::uint64_t acceptedFrames = 0;
    std::uint64_t droppedBlocks = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t rejectedBlocks = 0;
    std::uint64_t queuedBlocks = 0;

    [[nodiscard]] bool incomplete() const noexcept
    {
        return droppedBlocks != 0 || rejectedBlocks != 0;
    }
};

// One prepared audio-thread producer and one worker-thread consumer. All
// payload storage is allocated by the constructor. Admission and publication
// perform fixed work and never allocate, wait, lock, or retry.
class RealtimeOutputRecorder
{
public:
    RealtimeOutputRecorder(
        std::size_t channelCount,
        std::size_t maxBlockFrames,
        std::size_t queueCapacity);

    RealtimeOutputRecorder(const RealtimeOutputRecorder&) = delete;
    RealtimeOutputRecorder& operator=(const RealtimeOutputRecorder&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool startAccepting() noexcept;
    void stopAccepting() noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool producerQuiescent() const noexcept;

    [[nodiscard]] bool tryPublish(
        float* const* channels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;

    [[nodiscard]] RealtimeOutputRecorderConsumeStatus tryConsume(
        std::span<float> interleavedDestination,
        RealtimeOutputRecorderBlock& block) noexcept;

    [[nodiscard]] RealtimeOutputRecorderSnapshot snapshot() const noexcept;
    [[nodiscard]] bool discardPending() noexcept;

private:
    [[nodiscard]] bool beginProducerOperation() noexcept;
    void endProducerOperation() noexcept;
    static void addSaturated(
        std::atomic<std::uint64_t>& counter,
        std::uint64_t amount = 1) noexcept;

    bool ready_ = false;
    std::size_t channelCount_ = 0;
    std::size_t maxBlockFrames_ = 0;
    std::size_t queueCapacity_ = 0;
    std::vector<float> samples_;
    std::vector<RealtimeOutputRecorderBlock> blocks_;
    std::atomic<bool> accepting_ = false;
    std::atomic<std::uint32_t> activeProducerOperations_ = 0;
    std::atomic<std::uint64_t> writeIndex_ = 0;
    std::atomic<std::uint64_t> readIndex_ = 0;
    std::atomic<std::uint64_t> acceptedBlocks_ = 0;
    std::atomic<std::uint64_t> acceptedFrames_ = 0;
    std::atomic<std::uint64_t> droppedBlocks_ = 0;
    std::atomic<std::uint64_t> droppedFrames_ = 0;
    std::atomic<std::uint64_t> rejectedBlocks_ = 0;
};
}
