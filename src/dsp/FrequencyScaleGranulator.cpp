#include "talas/dsp/FrequencyScaleGranulator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

namespace talas::dsp
{
namespace
{
// The shortest period a granulator follows, in frames.
constexpr double shortestPeriod = 2.0;
// The longest Delay, in frames.
constexpr std::size_t longestDelay = std::size_t{1} << 22;
}

bool FrequencyScaleGranulator::valid(const Configuration& configuration) noexcept
{
    return std::isfinite(configuration.sampleRate) && configuration.sampleRate >= 1.0
        && configuration.sampleRate <= 1'536'000.0
        && configuration.delayFrames <= longestDelay
        && supportedScale(configuration.maximumScale)
        && configuration.window.size() != 1
        && std::ranges::all_of(configuration.window, [](double value) { return std::isfinite(value); });
}

bool FrequencyScaleGranulator::supportedScale(double scale) noexcept
{
    return std::isfinite(scale) && scale >= smallestScale && scale <= largestScale;
}

bool FrequencyScaleGranulator::prepare(Configuration configuration)
{
    prepared_ = false;
    if (!valid(configuration))
        return false;
    configuration_ = std::move(configuration);
    longestPeriod_ = std::max(shortestPeriod, configuration_.sampleRate / lowestFrequency);
    // The Delay, and the two longest periods a grain reaches back past it.
    const auto reach = configuration_.delayFrames
        + 2 * static_cast<std::size_t>(std::ceil(longestPeriod_)) + 8;
    history_.assign(std::bit_ceil(reach), 0.0);
    // Grains of two periods laid a period over the scale apart: at most
    // twice the scale of them sound at once, and one more starts.
    grains_.assign(static_cast<std::size_t>(std::ceil(2.0 * configuration_.maximumScale)) + 2, Grain{});
    prepared_ = true;
    reset();
    return true;
}

void FrequencyScaleGranulator::reset() noexcept
{
    std::ranges::fill(history_, 0.0);
    std::ranges::fill(grains_, Grain{});
    frame_ = 0;
    hasEpoch_ = false;
    epoch_ = 0.0;
    epochPeriod_ = 0.0;
    phase_ = 0.0;
    nextGrain_ = 0.0;
}

bool FrequencyScaleGranulator::copyStateFrom(const FrequencyScaleGranulator& other) noexcept
{
    if (prepared_ != other.prepared_ || history_.size() != other.history_.size()
        || grains_.size() != other.grains_.size()
        || configuration_.window.size() != other.configuration_.window.size()
        || configuration_.sampleRate != other.configuration_.sampleRate
        || configuration_.delayFrames != other.configuration_.delayFrames
        || configuration_.maximumScale != other.configuration_.maximumScale)
    {
        return false;
    }
    std::ranges::copy(other.configuration_.window, configuration_.window.begin());
    std::ranges::copy(other.history_, history_.begin());
    std::ranges::copy(other.grains_, grains_.begin());
    longestPeriod_ = other.longestPeriod_;
    frame_ = other.frame_;
    hasEpoch_ = other.hasEpoch_;
    epoch_ = other.epoch_;
    epochPeriod_ = other.epochPeriod_;
    phase_ = other.phase_;
    nextGrain_ = other.nextGrain_;
    return true;
}

double FrequencyScaleGranulator::windowAt(double phase) const noexcept
{
    const auto& window = configuration_.window;
    if (window.empty())
        return 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * phase);
    const auto position = std::clamp(phase, 0.0, 1.0) * static_cast<double>(window.size() - 1);
    const auto left = static_cast<std::size_t>(position);
    const auto right = std::min(left + 1, window.size() - 1);
    const auto amount = position - static_cast<double>(left);
    return window[left] + (window[right] - window[left]) * amount;
}

double FrequencyScaleGranulator::history(double frame) const noexcept
{
    if (frame < 0.0)
        return 0.0;
    const auto mask = history_.size() - 1;
    const auto whole = static_cast<std::uint64_t>(frame);
    const auto amount = frame - static_cast<double>(whole);
    const auto left = history_[static_cast<std::size_t>(whole) & mask];
    const auto right = history_[static_cast<std::size_t>(whole + 1) & mask];
    return left + (right - left) * amount;
}

void FrequencyScaleGranulator::startGrain(double offset, double scale) noexcept
{
    auto slot = std::ranges::find_if(grains_, [](const Grain& grain) { return !grain.active; });
    if (slot == grains_.end())
    {
        // Past MaxScale: the oldest grain gives way.
        slot = std::ranges::max_element(grains_, {}, &Grain::position);
    }
    // Overlapping grains share the level one a period apart would have.
    *slot = Grain{true, epoch_ - epochPeriod_, 2.0 * epochPeriod_, offset, std::min(1.0, 1.0 / scale)};
}

FrequencyScaleGranulator::Status FrequencyScaleGranulator::process(
    std::span<const double> input,
    std::span<const double> frequency,
    std::span<const double> scale,
    std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size() || frequency.size() != output.size()
        || scale.size() != output.size())
    {
        return Status::InvalidConfiguration;
    }
    const auto sampleRate = configuration_.sampleRate;
    const auto delay = static_cast<double>(configuration_.delayFrames);
    const auto mask = history_.size() - 1;
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        if (!std::isfinite(input[index]))
            return Status::NonFiniteInput;
        history_[static_cast<std::size_t>(frame_) & mask] = input[index];
        // The Input as it was Delay frames ago, at the frequency the
        // tracker gives now.
        const auto now = static_cast<double>(frame_) - delay;
        ++frame_;
        const auto tracked = frequency[index];
        const auto period = std::isfinite(tracked) && tracked > 0.0
            ? std::clamp(sampleRate / tracked, shortestPeriod, longestPeriod_)
            : longestPeriod_;
        phase_ += 1.0 / period;
        if (phase_ >= 1.0)
        {
            phase_ -= std::floor(phase_);
            // The epoch fell the phase's excess of a period ago.
            hasEpoch_ = true;
            epoch_ = now - phase_ * period;
            epochPeriod_ = period;
        }
        auto factor = scale[index];
        if (!std::isfinite(factor) || factor < smallestScale)
            factor = smallestScale;
        factor = std::min(factor, configuration_.maximumScale);
        if (hasEpoch_ && now >= nextGrain_)
        {
            // Grains start a period over the scale apart, a grain late by a
            // fraction of a frame starting that far in.
            const auto spacing = epochPeriod_ / factor;
            const auto late = now - nextGrain_;
            const auto offset = late < 1.0 ? late : 0.0;
            startGrain(offset, factor);
            nextGrain_ = late < 1.0 ? nextGrain_ + spacing : now + spacing;
        }
        auto sum = 0.0;
        for (auto& grain : grains_)
        {
            if (!grain.active)
                continue;
            sum += grain.gain * windowAt(grain.position / grain.length) * history(grain.start + grain.position);
            grain.position += 1.0;
            if (grain.position >= grain.length)
                grain.active = false;
        }
        output[index] = now < 0.0 ? 0.0 : sum;
    }
    return Status::Ok;
}
}
