#include "talas/audio/OscilloscopeAnalysis.h"

#include <algorithm>
#include <cmath>

namespace talas::audio
{
bool OscilloscopeSweepState::shouldUpdate(
    std::uint64_t capturedEndFrame,
    double sampleRate,
    double sweepFrequencyHz) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(sweepFrequencyHz) || sweepFrequencyHz <= 0.0)
    {
        return false;
    }
    if (!hasUpdate_ || capturedEndFrame < lastUpdateEndFrame_)
    {
        lastUpdateEndFrame_ = capturedEndFrame;
        hasUpdate_ = true;
        return true;
    }
    const auto framesPerSweep = sampleRate / sweepFrequencyHz;
    if (!std::isfinite(framesPerSweep) || framesPerSweep <= 0.0)
        return false;
    const auto elapsed = capturedEndFrame - lastUpdateEndFrame_;
    if (static_cast<double>(elapsed) < framesPerSweep)
        return false;
    lastUpdateEndFrame_ = capturedEndFrame;
    return true;
}

void OscilloscopeSweepState::reset() noexcept
{
    lastUpdateEndFrame_ = 0;
    hasUpdate_ = false;
}

std::optional<OscilloscopeWindow> powerOfTwoLockedOscilloscopeWindow(
    std::uint64_t absoluteFirstFrame, std::size_t available, std::size_t frameCount, std::size_t lock) noexcept
{
    if (frameCount == 0 || lock == 0 || (frameCount & (frameCount - 1)) != 0 || available < frameCount)
        return std::nullopt;
    const auto lastStart = absoluteFirstFrame + (available - frameCount);
    const auto start = lastStart - lastStart % lock;
    if (start < absoluteFirstFrame)
        return std::nullopt;
    return OscilloscopeWindow{static_cast<std::size_t>(start - absoluteFirstFrame), frameCount, false};
}

OscilloscopeWindow selectOscilloscopeWindow(
    std::span<const double> samples,
    std::size_t requestedFrameCount,
    OscilloscopeTrigger trigger,
    double threshold)
{
    return selectOscilloscopeWindow(
        samples, samples, requestedFrameCount, trigger, threshold);
}

OscilloscopeWindow selectOscilloscopeWindow(
    std::span<const double> samples,
    std::span<const double> triggerSamples,
    std::size_t requestedFrameCount,
    OscilloscopeTrigger trigger,
    double threshold)
{
    OscilloscopeWindow result;
    result.frameCount =
        std::min(samples.size(), requestedFrameCount);
    result.firstFrame = samples.size() - result.frameCount;
    if (trigger == OscilloscopeTrigger::Free
        || result.frameCount < 2
        || samples.size() < requestedFrameCount
        || triggerSamples.size() != samples.size())
    {
        return result;
    }

    const auto preTriggerFrames = result.frameCount / 4;
    const auto postTriggerFrames =
        result.frameCount - preTriggerFrames;
    const auto firstCandidate =
        std::max<std::size_t>(preTriggerFrames, 1);
    const auto lastCandidate =
        samples.size() - postTriggerFrames;
    if (lastCandidate < firstCandidate)
        return result;

    const auto safeThreshold =
        std::isfinite(threshold)
        ? std::clamp(threshold, -1.0, 1.0)
        : 0.0;
    for (auto current = lastCandidate;; --current)
    {
        const auto previousSample = triggerSamples[current - 1];
        const auto currentSample = triggerSamples[current];
        const auto rising =
            previousSample < safeThreshold
            && currentSample >= safeThreshold;
        const auto falling =
            previousSample > safeThreshold
            && currentSample <= safeThreshold;
        if (std::isfinite(previousSample)
            && std::isfinite(currentSample)
            && ((trigger == OscilloscopeTrigger::Rising && rising)
                || (trigger == OscilloscopeTrigger::Falling && falling)))
        {
            result.firstFrame = current - preTriggerFrames;
            result.triggered = true;
            return result;
        }
        if (current == firstCandidate)
            break;
    }
    return result;
}

double normalizedOscilloscopeSample(double sample)
{
    return std::isfinite(sample)
        ? std::clamp(sample, -1.0, 1.0)
        : 0.0;
}
}
