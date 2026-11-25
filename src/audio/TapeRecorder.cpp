#include "talas/audio/TapeRecorder.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace talas::audio
{
void TapeRecorderTransport::record() noexcept
{
    if (state_ == TapeRecorderState::Stopped || state_ == TapeRecorderState::Playing
        || state_ == TapeRecorderState::PlayingPaused)
        state_ = TapeRecorderState::Monitoring;
}

void TapeRecorderTransport::pause() noexcept
{
    switch (state_)
    {
    case TapeRecorderState::Monitoring:
    case TapeRecorderState::RecordingPaused:
        state_ = TapeRecorderState::Recording;
        break;
    case TapeRecorderState::Recording:
        state_ = TapeRecorderState::RecordingPaused;
        break;
    case TapeRecorderState::Playing:
        state_ = TapeRecorderState::PlayingPaused;
        break;
    case TapeRecorderState::PlayingPaused:
        state_ = TapeRecorderState::Playing;
        break;
    case TapeRecorderState::Stopped:
        break;
    }
}

void TapeRecorderTransport::play(bool hasRecording) noexcept
{
    switch (state_)
    {
    case TapeRecorderState::Monitoring:
    case TapeRecorderState::RecordingPaused:
        state_ = TapeRecorderState::Recording;
        break;
    case TapeRecorderState::Stopped:
        if (hasRecording)
            state_ = TapeRecorderState::Playing;
        break;
    case TapeRecorderState::PlayingPaused:
        state_ = TapeRecorderState::Playing;
        break;
    case TapeRecorderState::Recording:
    case TapeRecorderState::Playing:
        break;
    }
}

void TapeRecorderTransport::stop() noexcept
{
    state_ = TapeRecorderState::Stopped;
}

bool TapeRecorderTransport::pauseDown() const noexcept
{
    return state_ == TapeRecorderState::Monitoring || state_ == TapeRecorderState::RecordingPaused
        || state_ == TapeRecorderState::PlayingPaused;
}

bool TapeRecorderTransport::recordDown() const noexcept
{
    return monitors();
}

bool TapeRecorderTransport::monitors() const noexcept
{
    return state_ == TapeRecorderState::Monitoring || state_ == TapeRecorderState::Recording
        || state_ == TapeRecorderState::RecordingPaused;
}

TapeRecorderEngine::TapeRecorderEngine(std::size_t maximumBlockFrames)
    : maximumBlockFrames_(std::max<std::size_t>(1, maximumBlockFrames))
{
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        levels_[channel].store(1.0F);
        enabled_[channel].store(true);
        current_[channel].store(0.0F);
        maxima_[channel].store(0.0F);
        scratch_[channel].assign(maximumBlockFrames_, 0.0F);
    }
}

void TapeRecorderEngine::quiesce() const noexcept
{
    while (inFlight_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void TapeRecorderEngine::setInputLevel(std::size_t channel, float level) noexcept
{
    if (channel < levels_.size() && std::isfinite(level))
        levels_[channel].store(std::clamp(level, 1.0F, tapeRecorderMaximumInputGain), std::memory_order_release);
}

float TapeRecorderEngine::inputLevel(std::size_t channel) const noexcept
{
    return channel < levels_.size() ? levels_[channel].load(std::memory_order_acquire) : 0.0F;
}

void TapeRecorderEngine::setChannelEnabled(std::size_t channel, bool enabled) noexcept
{
    if (channel < enabled_.size())
        enabled_[channel].store(enabled, std::memory_order_release);
}

bool TapeRecorderEngine::channelEnabled(std::size_t channel) const noexcept
{
    return channel < enabled_.size() && enabled_[channel].load(std::memory_order_acquire);
}

std::size_t TapeRecorderEngine::enabledChannelCount() const noexcept
{
    return (channelEnabled(0) ? 1U : 0U) + (channelEnabled(1) ? 1U : 0U);
}

void TapeRecorderEngine::setMonitoring(bool monitoring) noexcept
{
    monitoring_.store(monitoring, std::memory_order_release);
}

bool TapeRecorderEngine::monitoring() const noexcept
{
    return monitoring_.load(std::memory_order_acquire);
}

void TapeRecorderEngine::setRecordingTake(RealtimeOutputTake* take) noexcept
{
    if (take != nullptr)
        recorded_.store(0, std::memory_order_release);
    take_.store(take, std::memory_order_release);
    if (take == nullptr)
        quiesce();
}

void TapeRecorderEngine::setRecordingPaused(bool paused) noexcept
{
    recordingPaused_.store(paused, std::memory_order_release);
}

void TapeRecorderEngine::setPlaybackClip(std::shared_ptr<const std::vector<std::vector<float>>> clip) noexcept
{
    playing_.store(false, std::memory_order_release);
    clipView_.store(nullptr, std::memory_order_release);
    quiesce();
    clip_ = std::move(clip);
    played_.store(0, std::memory_order_release);
    clipView_.store(clip_.get(), std::memory_order_release);
}

void TapeRecorderEngine::setPlaying(bool playing) noexcept
{
    playing_.store(playing, std::memory_order_release);
}

bool TapeRecorderEngine::playing() const noexcept
{
    return playing_.load(std::memory_order_acquire);
}

void TapeRecorderEngine::rewind() noexcept
{
    played_.store(0, std::memory_order_release);
}

float TapeRecorderEngine::currentLevel(std::size_t channel) const noexcept
{
    return channel < current_.size() ? current_[channel].load(std::memory_order_acquire) : 0.0F;
}

float TapeRecorderEngine::maxLevel(std::size_t channel) const noexcept
{
    return channel < maxima_.size() ? maxima_[channel].load(std::memory_order_acquire) : 0.0F;
}

void TapeRecorderEngine::resetMaxLevels() noexcept
{
    for (auto& maximum : maxima_)
        maximum.store(0.0F, std::memory_order_release);
}

std::uint64_t TapeRecorderEngine::recordedFrames() const noexcept
{
    return recorded_.load(std::memory_order_acquire);
}

std::uint64_t TapeRecorderEngine::playedFrames() const noexcept
{
    return played_.load(std::memory_order_acquire);
}

void TapeRecorderEngine::process(
    std::span<const float* const> inputs, std::span<float* const> outputs, std::size_t frames) noexcept
{
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    const auto monitoring = monitoring_.load(std::memory_order_acquire);
    auto* const take = take_.load(std::memory_order_acquire);
    const auto recording = take != nullptr && !recordingPaused_.load(std::memory_order_acquire);
    const auto* const clip = playing_.load(std::memory_order_acquire) ? clipView_.load(std::memory_order_acquire) : nullptr;
    std::array<float, 2> current{};
    for (std::size_t offset = 0; offset < frames; offset += maximumBlockFrames_)
    {
        const auto count = std::min(maximumBlockFrames_, frames - offset);
        std::array<float*, 2> recorded{};
        std::size_t recordedChannels = 0;
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            if (!enabled_[channel].load(std::memory_order_acquire))
                continue;
            const auto level = levels_[channel].load(std::memory_order_acquire);
            const auto* input = channel < inputs.size() ? inputs[channel] : nullptr;
            auto* heard = monitoring && channel < outputs.size() ? outputs[channel] : nullptr;
            auto& scratch = scratch_[recordedChannels];
            float peak = 0.0F;
            for (std::size_t frame = 0; frame < count; ++frame)
            {
                const auto sample = input != nullptr ? input[offset + frame] * level : 0.0F;
                peak = std::max(peak, std::abs(sample));
                scratch[frame] = sample;
                if (heard != nullptr)
                    heard[offset + frame] += sample;
            }
            current[channel] = std::max(current[channel], peak);
            auto seen = maxima_[channel].load(std::memory_order_relaxed);
            while (peak > seen && !maxima_[channel].compare_exchange_weak(seen, peak, std::memory_order_acq_rel))
            {
            }
            recorded[recordedChannels++] = scratch.data();
        }
        if (recording && recordedChannels == take->channelCount())
        {
            (void) take->publishFromAudio(recorded.data(), recordedChannels, count, takeStageId, 1, 1);
            recorded_.fetch_add(count, std::memory_order_acq_rel);
        }
        if (clip != nullptr && !clip->empty())
        {
            const auto position = played_.load(std::memory_order_acquire);
            const auto length = clip->front().size();
            const auto playable = position < length ? std::min<std::size_t>(count, length - position) : 0;
            for (std::size_t channel = 0; channel < outputs.size() && channel < 2; ++channel)
            {
                if (outputs[channel] == nullptr)
                    continue;
                const auto& source = (*clip)[std::min(channel, clip->size() - 1)];
                for (std::size_t frame = 0; frame < playable && position + frame < source.size(); ++frame)
                    outputs[channel][offset + frame] += source[position + frame];
            }
            played_.store(position + playable, std::memory_order_release);
            if (position + playable >= length)
                playing_.store(false, std::memory_order_release);
        }
    }
    for (std::size_t channel = 0; channel < current.size(); ++channel)
        current_[channel].store(current[channel], std::memory_order_release);
    inFlight_.fetch_sub(1, std::memory_order_acq_rel);
}
}
