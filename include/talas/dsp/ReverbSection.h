#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::dsp
{
enum class ReverbSectionType
{
    Comb,
    Allpass,
};

enum class ReverbSectionInterpolation
{
    Linear,
    Nearest,
};

enum class ReverbSectionFailure
{
    None,
    InvalidSampleRate,
    InvalidInput,
    InvalidScale,
    InvalidDelay,
    InvalidDelayScale,
    DelayProduct,
    DelayFrameConversion,
    DelayCapacity,
    InvalidWriteIndex,
    InvalidDecayTime,
    Feedback,
    ScaledInput,
    DelayRead,
    Interpolation,
    DelayWrite,
    Output,
};

struct ReverbSectionSampleConfiguration
{
    double sampleRate = 0.0;
    double input = 0.0;
    double scale = 1.0;
    double decayTime = 1.0;
    double delaySeconds = 0.0;
    double delayScale = 1.0;
    ReverbSectionType type = ReverbSectionType::Comb;
    ReverbSectionInterpolation interpolation =
        ReverbSectionInterpolation::Linear;
};

struct ReverbSectionDelayGeometry
{
    double actualDelaySeconds = 0.0;
    double delayFrames = 0.0;
    std::size_t capacity = 0;

    bool operator==(const ReverbSectionDelayGeometry&) const = default;
};

struct ReverbSectionTransition
{
    double output = 0.0;
    double delayWrite = 0.0;
    std::size_t nextWriteIndex = 0;
    bool writesDelay = false;

    bool operator==(const ReverbSectionTransition&) const = default;
};

[[nodiscard]] inline bool checkedReverbSectionDelayGeometry(
    double sampleRate,
    double delaySeconds,
    double delayScale,
    ReverbSectionInterpolation interpolation,
    ReverbSectionDelayGeometry& geometry,
    ReverbSectionFailure* failure = nullptr) noexcept
{
    const auto reject = [&](ReverbSectionFailure reason) {
        if (failure != nullptr)
            *failure = reason;
        return false;
    };
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return reject(ReverbSectionFailure::InvalidSampleRate);
    if (!std::isfinite(delaySeconds) || delaySeconds < 0.0)
        return reject(ReverbSectionFailure::InvalidDelay);
    if (!std::isfinite(delayScale)
        || delayScale < 0.0 || delayScale > 1.0)
    {
        return reject(ReverbSectionFailure::InvalidDelayScale);
    }

    const auto actualDelaySeconds = delaySeconds * delayScale;
    const auto delayFrames = actualDelaySeconds * sampleRate;
    if (!std::isfinite(actualDelaySeconds)
        || actualDelaySeconds < 0.0)
    {
        return reject(ReverbSectionFailure::DelayProduct);
    }
    if (!std::isfinite(delayFrames) || delayFrames < 0.0)
    {
        return reject(ReverbSectionFailure::DelayFrameConversion);
    }

    const auto capacityFrames =
        interpolation == ReverbSectionInterpolation::Linear
        ? std::ceil(delayFrames)
        : std::round(delayFrames);
    const auto maximumCapacityExclusive = std::ldexp(
        1.0L, std::numeric_limits<std::size_t>::digits);
    if (!std::isfinite(capacityFrames) || capacityFrames < 0.0
        || static_cast<long double>(capacityFrames)
            >= maximumCapacityExclusive
        || (interpolation == ReverbSectionInterpolation::Nearest
            && delayFrames > 0.0 && capacityFrames == 0.0))
    {
        return reject(ReverbSectionFailure::DelayCapacity);
    }

    geometry = {
        actualDelaySeconds,
        delayFrames,
        static_cast<std::size_t>(capacityFrames),
    };
    if (failure != nullptr)
        *failure = ReverbSectionFailure::None;
    return true;
}

[[nodiscard]] inline bool checkedReverbSectionFeedback(
    double actualDelaySeconds,
    double decayTime,
    double& feedback,
    ReverbSectionFailure* failure = nullptr) noexcept
{
    const auto reject = [&](ReverbSectionFailure reason) {
        if (failure != nullptr)
            *failure = reason;
        return false;
    };
    if (!std::isfinite(actualDelaySeconds) || actualDelaySeconds < 0.0)
        return reject(ReverbSectionFailure::InvalidDelay);
    if (!std::isfinite(decayTime) || decayTime <= 0.0)
    {
        return reject(ReverbSectionFailure::InvalidDecayTime);
    }
    if (actualDelaySeconds == 0.0)
    {
        feedback = 0.0;
        if (failure != nullptr)
            *failure = ReverbSectionFailure::None;
        return true;
    }

    const auto exponent = actualDelaySeconds / decayTime;
    const auto candidate = std::pow(0.001, exponent);
    if (!std::isfinite(exponent)
        || !std::isfinite(candidate)
        || candidate < 0.0 || candidate >= 1.0)
    {
        return reject(ReverbSectionFailure::Feedback);
    }
    feedback = candidate;
    if (failure != nullptr)
        *failure = ReverbSectionFailure::None;
    return true;
}

[[nodiscard]] inline bool checkedReverbSectionTransition(
    const ReverbSectionSampleConfiguration& configuration,
    std::span<const double> delayLine,
    std::size_t writeIndex,
    ReverbSectionTransition& transition,
    ReverbSectionFailure* failure = nullptr) noexcept
{
    const auto reject = [&](ReverbSectionFailure reason) {
        if (failure != nullptr)
            *failure = reason;
        return false;
    };
    if (!std::isfinite(configuration.input))
        return reject(ReverbSectionFailure::InvalidInput);
    if (!std::isfinite(configuration.scale))
        return reject(ReverbSectionFailure::InvalidScale);
    if (delayLine.empty() ? writeIndex != 0
                          : writeIndex >= delayLine.size())
    {
        return reject(ReverbSectionFailure::InvalidWriteIndex);
    }

    ReverbSectionDelayGeometry geometry;
    if (!checkedReverbSectionDelayGeometry(
            configuration.sampleRate,
            configuration.delaySeconds,
            configuration.delayScale,
            configuration.interpolation,
            geometry,
            failure))
    {
        return false;
    }
    if (geometry.capacity > delayLine.size())
        return reject(ReverbSectionFailure::DelayCapacity);

    auto feedback = 0.0;
    if (!checkedReverbSectionFeedback(
            geometry.actualDelaySeconds,
            configuration.decayTime,
            feedback,
            failure))
    {
        return false;
    }

    const auto x = configuration.input * configuration.scale;
    if (!std::isfinite(x))
        return reject(ReverbSectionFailure::ScaledInput);

    ReverbSectionTransition candidate;
    candidate.nextWriteIndex = writeIndex;
    if (geometry.delayFrames <= 0.0 || delayLine.empty())
    {
        candidate.output = x;
        candidate.delayWrite = x;
        candidate.writesDelay = !delayLine.empty();
        if (!delayLine.empty())
            candidate.nextWriteIndex = (writeIndex + 1) % delayLine.size();
        transition = candidate;
        if (failure != nullptr)
            *failure = ReverbSectionFailure::None;
        return true;
    }

    const auto readWhole = [&](std::size_t framesAgo, double& value) {
        if (framesAgo == 0)
        {
            value = x;
            return true;
        }
        if (framesAgo > delayLine.size())
            return false;
        const auto readIndex =
            (writeIndex + delayLine.size() - framesAgo) % delayLine.size();
        value = delayLine[readIndex];
        return std::isfinite(value);
    };

    auto delayed = 0.0;
    if (configuration.interpolation == ReverbSectionInterpolation::Nearest)
    {
        const auto framesAgo = static_cast<std::size_t>(
            std::round(geometry.delayFrames));
        if (!readWhole(framesAgo, delayed))
            return reject(ReverbSectionFailure::DelayRead);
    }
    else
    {
        const auto baseDelay = std::floor(geometry.delayFrames);
        if (!std::isfinite(baseDelay) || baseDelay < 0.0)
            return reject(ReverbSectionFailure::Interpolation);
        const auto baseFramesAgo = static_cast<std::size_t>(baseDelay);
        const auto fraction = geometry.delayFrames - baseDelay;
        auto newer = 0.0;
        if (!std::isfinite(fraction))
            return reject(ReverbSectionFailure::Interpolation);
        if (!readWhole(baseFramesAgo, newer))
            return reject(ReverbSectionFailure::DelayRead);
        delayed = newer;
        if (fraction > 1.0e-12)
        {
            auto older = 0.0;
            if (!readWhole(baseFramesAgo + 1, older))
                return reject(ReverbSectionFailure::DelayRead);
            const auto difference = older - newer;
            const auto interpolationProduct = difference * fraction;
            delayed = newer + interpolationProduct;
            if (!std::isfinite(difference)
                || !std::isfinite(interpolationProduct)
                || !std::isfinite(delayed))
            {
                return reject(ReverbSectionFailure::Interpolation);
            }
        }
    }
    if (!std::isfinite(delayed))
        return reject(ReverbSectionFailure::DelayRead);

    const auto feedbackProduct = delayed * feedback;
    const auto delayWrite = x + feedbackProduct;
    if (!std::isfinite(feedbackProduct) || !std::isfinite(delayWrite))
        return reject(ReverbSectionFailure::DelayWrite);

    auto output = delayed;
    if (configuration.type == ReverbSectionType::Allpass)
    {
        // Schroeder's allpass subtracts the fed-back delay input, not the
        // bare Input; only then is the long-term response flat.
        const auto directFeedback = delayWrite * feedback;
        output = delayed - directFeedback;
        if (!std::isfinite(directFeedback) || !std::isfinite(output))
            return reject(ReverbSectionFailure::Output);
    }

    candidate.output = output;
    candidate.delayWrite = delayWrite;
    candidate.nextWriteIndex = (writeIndex + 1) % delayLine.size();
    candidate.writesDelay = true;
    transition = candidate;
    if (failure != nullptr)
        *failure = ReverbSectionFailure::None;
    return true;
}
}
