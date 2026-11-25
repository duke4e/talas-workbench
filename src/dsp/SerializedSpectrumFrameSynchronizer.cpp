#include "talas/dsp/SerializedSpectrumFrameSynchronizer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] bool spansOverlap(
    std::span<double> left, std::span<double> right) noexcept
{
    if (left.empty() || right.empty()) return false;
    const std::less<const double*> before;
    return before(left.data(), right.data() + right.size())
        && before(right.data(), left.data() + left.size());
}
}

SerializedSpectrumSyncSnapshot::SerializedSpectrumSyncSnapshot(
    std::size_t frameLength)
    : frameLength_(frameLength),
      amplitudeHistory_(frameLength),
      frequencyHistory_(frameLength)
{
    if (frameLength == 0)
        throw std::invalid_argument(
            "Serialized spectrum snapshot requires a positive frame length");
}

PreparedSerializedSpectrumFrameSynchronizer::
    PreparedSerializedSpectrumFrameSynchronizer(
        std::size_t frameLength, std::size_t maximumBlockFrames)
    : frameLength_(frameLength),
      maximumBlockFrames_(maximumBlockFrames),
      amplitudeHistory_(frameLength),
      frequencyHistory_(frameLength),
      amplitudeScratch_(maximumBlockFrames),
      frequencyScratch_(maximumBlockFrames),
      componentIndexScratch_(maximumBlockFrames)
{
    if (frameLength == 0 || maximumBlockFrames == 0)
        throw std::invalid_argument(
            "Serialized spectrum synchronization requires positive frame and block lengths");
}

void PreparedSerializedSpectrumFrameSynchronizer::reset(
    std::uint64_t absoluteFrame) noexcept
{
    std::ranges::fill(amplitudeHistory_, 0.0);
    std::ranges::fill(frequencyHistory_, 0.0);
    phase_ = static_cast<std::size_t>(
        absoluteFrame % static_cast<std::uint64_t>(frameLength_));
    writeIndex_ = 0;
    historyCount_ = 0;
    delay_ = 0;
    startupSilence_ = 0;
    outputComponentIndex_ = phase_;
    previousSyncOutput_ = false;
    previousSyncInput_ = false;
}

SerializedSpectrumSyncSnapshot
PreparedSerializedSpectrumFrameSynchronizer::prepareSnapshot() const
{
    return SerializedSpectrumSyncSnapshot(frameLength_);
}

bool PreparedSerializedSpectrumFrameSynchronizer::capture(
    SerializedSpectrumSyncSnapshot& snapshot) const noexcept
{
    if (snapshot.frameLength_ != frameLength_
        || snapshot.amplitudeHistory_.size() != frameLength_
        || snapshot.frequencyHistory_.size() != frameLength_)
    {
        return false;
    }
    std::ranges::copy(amplitudeHistory_, snapshot.amplitudeHistory_.begin());
    std::ranges::copy(frequencyHistory_, snapshot.frequencyHistory_.begin());
    snapshot.phase_ = phase_;
    snapshot.writeIndex_ = writeIndex_;
    snapshot.historyCount_ = historyCount_;
    snapshot.delay_ = delay_;
    snapshot.startupSilence_ = startupSilence_;
    snapshot.outputComponentIndex_ = outputComponentIndex_;
    snapshot.previousSyncOutput_ = previousSyncOutput_;
    snapshot.previousSyncInput_ = previousSyncInput_;
    return true;
}

bool PreparedSerializedSpectrumFrameSynchronizer::restore(
    const SerializedSpectrumSyncSnapshot& snapshot) noexcept
{
    if (snapshot.frameLength_ != frameLength_
        || snapshot.amplitudeHistory_.size() != frameLength_
        || snapshot.frequencyHistory_.size() != frameLength_
        || snapshot.phase_ >= frameLength_
        || snapshot.writeIndex_ >= frameLength_
        || snapshot.historyCount_ > frameLength_
        || snapshot.delay_ >= frameLength_
        || snapshot.startupSilence_ >= frameLength_
        || snapshot.outputComponentIndex_ >= frameLength_)
    {
        return false;
    }
    std::ranges::copy(snapshot.amplitudeHistory_, amplitudeHistory_.begin());
    std::ranges::copy(snapshot.frequencyHistory_, frequencyHistory_.begin());
    phase_ = snapshot.phase_;
    writeIndex_ = snapshot.writeIndex_;
    historyCount_ = snapshot.historyCount_;
    delay_ = snapshot.delay_;
    startupSilence_ = snapshot.startupSilence_;
    outputComponentIndex_ = snapshot.outputComponentIndex_;
    previousSyncOutput_ = snapshot.previousSyncOutput_;
    previousSyncInput_ = snapshot.previousSyncInput_;
    return true;
}

SerializedSpectrumSyncStatus
PreparedSerializedSpectrumFrameSynchronizer::process(
    std::span<const double> amplitudeIn,
    std::span<const double> frequencyIn,
    std::span<const double> syncOutput,
    std::span<const double> syncInput,
    std::span<double> amplitudeOut,
    std::span<double> frequencyOut,
    std::span<std::size_t> componentIndexOut) noexcept
{
    const auto frames = amplitudeIn.size();
    if (frequencyIn.size() != frames || syncOutput.size() != frames
        || syncInput.size() != frames || amplitudeOut.size() != frames
        || frequencyOut.size() != frames
        || componentIndexOut.size() != frames
        || spansOverlap(amplitudeOut, frequencyOut))
    {
        return SerializedSpectrumSyncStatus::InvalidBlock;
    }
    if (frames > maximumBlockFrames_)
        return SerializedSpectrumSyncStatus::CapacityExceeded;

    // Validate the entire callback before modifying history or output. This
    // also makes a failed block safe to retry with corrected inputs.
    auto phase = phase_;
    auto historyCount = historyCount_;
    auto previousOutput = previousSyncOutput_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto outputTrigger = syncOutput[frame] > 0.0;
        const auto inputTrigger = syncInput[frame] > 0.0;
        if (!std::isfinite(amplitudeIn[frame])
            || !std::isfinite(frequencyIn[frame])
            || !std::isfinite(syncOutput[frame])
            || !std::isfinite(syncInput[frame]))
        {
            return SerializedSpectrumSyncStatus::InvalidBlock;
        }
        if (outputTrigger && inputTrigger)
            return SerializedSpectrumSyncStatus::ConflictingTriggers;
        if (outputTrigger && !previousOutput && historyCount < phase)
            return SerializedSpectrumSyncStatus::MissingInputFrameStart;
        previousOutput = outputTrigger;
        phase = phase + 1 == frameLength_ ? 0 : phase + 1;
        if (historyCount < frameLength_) ++historyCount;
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto amplitude = amplitudeIn[frame];
        const auto frequency = frequencyIn[frame];
        const auto outputTrigger = syncOutput[frame] > 0.0;
        const auto inputTrigger = syncInput[frame] > 0.0;
        if (outputTrigger && !previousSyncOutput_)
        {
            delay_ = phase_;
            startupSilence_ = 0;
            outputComponentIndex_ = 0;
        }
        else if (inputTrigger && !previousSyncInput_)
        {
            delay_ = phase_ == 0 ? 0 : frameLength_ - phase_;
            startupSilence_ = delay_;
            outputComponentIndex_ = 0;
        }
        previousSyncOutput_ = outputTrigger;
        previousSyncInput_ = inputTrigger;

        auto outputAmplitude = amplitude;
        auto outputFrequency = frequency;
        if (startupSilence_ != 0)
        {
            outputAmplitude = 0.0;
            outputFrequency = 0.0;
            componentIndexScratch_[frame] = frameLength_;
            --startupSilence_;
        }
        else
        {
            if (delay_ != 0)
            {
                const auto readIndex = writeIndex_ >= delay_
                    ? writeIndex_ - delay_
                    : frameLength_ - (delay_ - writeIndex_);
                outputAmplitude = amplitudeHistory_[readIndex];
                outputFrequency = frequencyHistory_[readIndex];
            }
            componentIndexScratch_[frame] = outputComponentIndex_;
            outputComponentIndex_ = outputComponentIndex_ + 1 == frameLength_
                ? 0 : outputComponentIndex_ + 1;
        }
        amplitudeScratch_[frame] = outputAmplitude;
        frequencyScratch_[frame] = outputFrequency;
        amplitudeHistory_[writeIndex_] = amplitude;
        frequencyHistory_[writeIndex_] = frequency;
        writeIndex_ = writeIndex_ + 1 == frameLength_ ? 0 : writeIndex_ + 1;
        phase_ = phase_ + 1 == frameLength_ ? 0 : phase_ + 1;
        if (historyCount_ < frameLength_) ++historyCount_;
    }
    std::ranges::copy(amplitudeScratch_.begin(),
        amplitudeScratch_.begin() + static_cast<std::ptrdiff_t>(frames),
        amplitudeOut.begin());
    std::ranges::copy(frequencyScratch_.begin(),
        frequencyScratch_.begin() + static_cast<std::ptrdiff_t>(frames),
        frequencyOut.begin());
    std::ranges::copy(componentIndexScratch_.begin(),
        componentIndexScratch_.begin() + static_cast<std::ptrdiff_t>(frames),
        componentIndexOut.begin());
    return SerializedSpectrumSyncStatus::Ok;
}
}
