#include "talas/runtime/RealtimeOutputRecorder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::runtime
{
RealtimeOutputRecorder::RealtimeOutputRecorder(
    std::size_t channelCount,
    std::size_t maxBlockFrames,
    std::size_t queueCapacity)
    : channelCount_(channelCount)
    , maxBlockFrames_(maxBlockFrames)
    , queueCapacity_(queueCapacity)
{
    if (channelCount == 0
        || channelCount > maximumRealtimeOutputRecorderChannels
        || maxBlockFrames == 0
        || maxBlockFrames > maximumRealtimeOutputRecorderBlockFrames
        || queueCapacity < 2
        || queueCapacity > maximumRealtimeOutputRecorderQueueCapacity)
    {
        return;
    }
    if (channelCount
            > std::numeric_limits<std::size_t>::max() / maxBlockFrames
        || channelCount * maxBlockFrames
            > std::numeric_limits<std::size_t>::max() / queueCapacity)
    {
        return;
    }
    samples_.resize(
        channelCount * maxBlockFrames * queueCapacity,
        0.0F);
    blocks_.resize(queueCapacity);
    ready_ = true;
}

bool RealtimeOutputRecorder::ready() const noexcept
{
    return ready_;
}

bool RealtimeOutputRecorder::startAccepting() noexcept
{
    if (!ready_ || accepting_.load(std::memory_order_seq_cst)
        || activeProducerOperations_.load(std::memory_order_seq_cst) != 0
        || writeIndex_.load(std::memory_order_acquire)
            != readIndex_.load(std::memory_order_acquire))
    {
        return false;
    }
    accepting_.store(true, std::memory_order_seq_cst);
    return true;
}

void RealtimeOutputRecorder::stopAccepting() noexcept
{
    accepting_.store(false, std::memory_order_seq_cst);
}

bool RealtimeOutputRecorder::accepting() const noexcept
{
    return accepting_.load(std::memory_order_acquire);
}

bool RealtimeOutputRecorder::producerQuiescent() const noexcept
{
    return activeProducerOperations_.load(std::memory_order_seq_cst) == 0;
}

bool RealtimeOutputRecorder::beginProducerOperation() noexcept
{
    if (!accepting_.load(std::memory_order_seq_cst))
        return false;
    activeProducerOperations_.fetch_add(1, std::memory_order_seq_cst);
    if (!accepting_.load(std::memory_order_seq_cst))
    {
        activeProducerOperations_.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

void RealtimeOutputRecorder::endProducerOperation() noexcept
{
    activeProducerOperations_.fetch_sub(1, std::memory_order_seq_cst);
}

void RealtimeOutputRecorder::addSaturated(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t amount) noexcept
{
    const auto current = counter.load(std::memory_order_relaxed);
    counter.store(
        amount > std::numeric_limits<std::uint64_t>::max() - current
            ? std::numeric_limits<std::uint64_t>::max()
            : current + amount,
        std::memory_order_relaxed);
}

bool RealtimeOutputRecorder::tryPublish(
    float* const* channels,
    std::size_t channelCount,
    std::size_t frameCount) noexcept
{
    if (!beginProducerOperation())
        return false;
    struct EndOperation
    {
        RealtimeOutputRecorder* recorder;
        ~EndOperation() { recorder->endProducerOperation(); }
    } end{this};

    if (channels == nullptr || channelCount != channelCount_
        || frameCount == 0 || frameCount > maxBlockFrames_)
    {
        addSaturated(rejectedBlocks_);
        return false;
    }
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        if (channels[channel] == nullptr)
        {
            addSaturated(rejectedBlocks_);
            return false;
        }
    }

    const auto write = writeIndex_.load(std::memory_order_relaxed);
    const auto read = readIndex_.load(std::memory_order_acquire);
    if (write < read
        || write == std::numeric_limits<std::uint64_t>::max()
        || write - read >= queueCapacity_)
    {
        addSaturated(droppedBlocks_);
        addSaturated(droppedFrames_, frameCount);
        return false;
    }

    const auto slot = static_cast<std::size_t>(write % queueCapacity_);
    const auto slotOffset = slot * channelCount_ * maxBlockFrames_;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        for (std::size_t channel = 0; channel < channelCount_; ++channel)
        {
            const auto sample = channels[channel][frame];
            if (!std::isfinite(sample))
            {
                addSaturated(rejectedBlocks_);
                return false;
            }
            samples_[slotOffset + frame * channelCount_ + channel] = sample;
        }
    }
    blocks_[slot] = {
        write + 1,
        static_cast<std::uint32_t>(channelCount_),
        static_cast<std::uint32_t>(frameCount),
    };
    writeIndex_.store(write + 1, std::memory_order_release);
    addSaturated(acceptedBlocks_);
    addSaturated(acceptedFrames_, frameCount);
    return true;
}

RealtimeOutputRecorderConsumeStatus RealtimeOutputRecorder::tryConsume(
    std::span<float> interleavedDestination,
    RealtimeOutputRecorderBlock& block) noexcept
{
    if (!ready_)
        return RealtimeOutputRecorderConsumeStatus::NotReady;
    const auto read = readIndex_.load(std::memory_order_relaxed);
    const auto write = writeIndex_.load(std::memory_order_acquire);
    if (read == write)
        return RealtimeOutputRecorderConsumeStatus::Empty;

    const auto slot = static_cast<std::size_t>(read % queueCapacity_);
    const auto candidate = blocks_[slot];
    const auto sampleCount = static_cast<std::size_t>(candidate.frameCount)
        * candidate.channelCount;
    if (candidate.sequence != read + 1
        || candidate.channelCount != channelCount_
        || candidate.frameCount == 0
        || candidate.frameCount > maxBlockFrames_)
    {
        return RealtimeOutputRecorderConsumeStatus::NotReady;
    }
    if (interleavedDestination.size() < sampleCount)
        return RealtimeOutputRecorderConsumeStatus::DestinationTooSmall;

    const auto slotOffset = slot * channelCount_ * maxBlockFrames_;
    std::copy_n(
        samples_.data() + static_cast<std::ptrdiff_t>(slotOffset),
        sampleCount,
        interleavedDestination.data());
    block = candidate;
    readIndex_.store(read + 1, std::memory_order_release);
    return RealtimeOutputRecorderConsumeStatus::Accepted;
}

RealtimeOutputRecorderSnapshot RealtimeOutputRecorder::snapshot() const noexcept
{
    const auto read = readIndex_.load(std::memory_order_acquire);
    const auto write = writeIndex_.load(std::memory_order_acquire);
    return {
        ready_,
        accepting(),
        producerQuiescent(),
        static_cast<std::uint32_t>(channelCount_),
        static_cast<std::uint32_t>(maxBlockFrames_),
        static_cast<std::uint32_t>(queueCapacity_),
        acceptedBlocks_.load(std::memory_order_relaxed),
        acceptedFrames_.load(std::memory_order_relaxed),
        droppedBlocks_.load(std::memory_order_relaxed),
        droppedFrames_.load(std::memory_order_relaxed),
        rejectedBlocks_.load(std::memory_order_relaxed),
        write >= read ? write - read : 0,
    };
}

bool RealtimeOutputRecorder::discardPending() noexcept
{
    if (accepting() || !producerQuiescent())
        return false;
    const auto write = writeIndex_.load(std::memory_order_acquire);
    readIndex_.store(write, std::memory_order_release);
    return true;
}
}
