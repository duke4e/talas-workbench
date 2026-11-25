#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// DelayWithFeedback: the scaled Input
// enters a delay line with some of the delayed signal fed back. Comb plays
// the delayed signal, so the Input is heard only after the first delay;
// Allpass adds some of the direct signal for a flat long-term response. The
// delay is Delay times DelayScale, read from the nearest frame or
// interpolated linearly. Mix, an extension, blends the scaled Input
// with the delayed signal. SmoothDelayChanges glides the delay toward a new
// value, and delays naming one Wavetable share a delay line
// (ADR-0025).
namespace talas::runtime::checked_delay_with_feedback
{
enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    DelayNonFiniteOrNegative,
    DelayScaleNonFiniteOrOutOfRange,
    DelayFramesNonFiniteOrUnrepresentable,
    HistoryCapacityExceeded,
    StateInvalid,
    AudioInNonFinite,
    ScaleNonFinite,
    FeedbackNonFiniteOrOutOfRange,
    MixNonFinite,
    HistorySampleNonFinite,
    InterpolationNonFinite,
    FeedbackProductNonFinite,
    FeedbackSumNonFinite,
    DryTermNonFinite,
    WetTermNonFinite,
    OutputNonFinite,
};

enum class Type
{
    Comb,
    Allpass,
};

enum class Interpolation
{
    None,
    Linear,
};

struct Config
{
    double sampleRate = 0.0;
    Type type = Type::Comb;
    Interpolation interpolation = Interpolation::None;
};

struct Controls
{
    double audioIn = 0.0;
    double scale = 1.0;
    double delaySeconds = 0.0;
    double delayScale = 1.0;
    double feedback = 0.0;
    double mix = 0.5;
};

// ADR-0025: SmoothDelayChanges moves the delay toward a new value by at
// most half a frame per frame, our choice, so the delayed signal's pitch
// bends by at most half instead of the delay jumping.
inline constexpr double glideFramesPerFrame = 0.5;

// The delay a gliding delay has reached.
struct Glide
{
    bool enabled = false;
    bool started = false;
    double frames = 0.0;
};

// The delay a gliding delay reaches this frame on its way to Target frames.
[[nodiscard]] inline double glidedFrames(const Glide& glide, double target) noexcept
{
    return glide.started
        ? glide.frames + std::clamp(target - glide.frames,
            -glideFramesPerFrame, glideFramesPerFrame)
        : target;
}

// The delay of Frames frames and the history it reads: the nearest frame
// without interpolation, the frame beyond it with.
[[nodiscard]] inline Error geometryForFrames(
    const Config& config,
    double frames,
    double& delayFrames,
    std::size_t& historyFrames) noexcept
{
    const auto needed = config.interpolation == Interpolation::Linear
        ? std::ceil(frames)
        : std::round(frames);
    const auto exclusiveLimit = std::ldexp(
        1.0, std::numeric_limits<std::size_t>::digits);
    if (!std::isfinite(frames) || !std::isfinite(needed)
        || needed < 0.0 || needed >= exclusiveLimit)
    {
        return Error::DelayFramesNonFiniteOrUnrepresentable;
    }
    delayFrames = config.interpolation == Interpolation::Linear
        ? frames
        : needed;
    historyFrames = static_cast<std::size_t>(needed);
    return Error::None;
}

// The delay in frames and the history it reads.
[[nodiscard]] inline Error delayGeometry(
    const Config& config,
    double delaySeconds,
    double delayScale,
    double& delayFrames,
    std::size_t& historyFrames) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(delaySeconds) || delaySeconds < 0.0)
        return Error::DelayNonFiniteOrNegative;
    if (!std::isfinite(delayScale) || delayScale < 0.0 || delayScale > 1.0)
        return Error::DelayScaleNonFiniteOrOutOfRange;
    return geometryForFrames(
        config, delaySeconds * delayScale * config.sampleRate,
        delayFrames, historyFrames);
}

// One frame of a delay. A delay of its own owns History, a ring whose
// oldest frame it overwrites at WriteIndex and then advances. A delay that
// shares a line (ADR-0025) adds its write to the line's frame at
// WriteIndex, which the caller keeps; the line is one frame longer than its
// longest delay, so no delay reads the frame being written.
[[nodiscard]] inline Error advance(
    std::span<double> history,
    std::size_t& writeIndex,
    const Config& config,
    const Controls& controls,
    double& output,
    Glide* glide = nullptr,
    bool sharedLine = false) noexcept
{
    if (history.empty() ? writeIndex != 0 || sharedLine
                        : writeIndex >= history.size())
        return Error::StateInvalid;
    auto delayFrames = 0.0;
    auto historyFrames = std::size_t{0};
    if (const auto error = delayGeometry(
            config, controls.delaySeconds, controls.delayScale,
            delayFrames, historyFrames);
        error != Error::None)
    {
        return error;
    }
    auto glideFrames = 0.0;
    if (glide != nullptr && glide->enabled)
    {
        glideFrames = glidedFrames(*glide,
            controls.delaySeconds * controls.delayScale * config.sampleRate);
        if (const auto error = geometryForFrames(
                config, glideFrames, delayFrames, historyFrames);
            error != Error::None)
        {
            return error;
        }
    }
    if (historyFrames > history.size() - (sharedLine ? 1U : 0U))
        return Error::HistoryCapacityExceeded;
    if (!std::isfinite(controls.audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(controls.scale))
        return Error::ScaleNonFinite;
    if (!std::isfinite(controls.feedback) || std::abs(controls.feedback) > 1.0)
        return Error::FeedbackNonFiniteOrOutOfRange;
    if (!std::isfinite(controls.mix))
        return Error::MixNonFinite;

    const auto input = controls.audioIn * controls.scale;
    if (!std::isfinite(input))
        return Error::AudioInNonFinite;

    const auto framesAgo = [&](std::size_t frames) {
        return history[(writeIndex + history.size() - frames) % history.size()];
    };
    // Without a delay the scaled Input passes straight through.
    auto delayed = input;
    auto delayWrite = input;
    auto wet = input;
    if (historyFrames > 0)
    {
        const auto newerFrames = static_cast<std::size_t>(std::floor(delayFrames));
        const auto fraction = delayFrames - std::floor(delayFrames);
        const auto newer = newerFrames == 0 ? input : framesAgo(newerFrames);
        delayed = newer;
        if (fraction > 0.0)
        {
            const auto older = framesAgo(newerFrames + 1);
            delayed = newer + (older - newer) * fraction;
            if (!std::isfinite(older) || !std::isfinite(delayed))
                return Error::InterpolationNonFinite;
        }
        if (!std::isfinite(delayed))
            return Error::HistorySampleNonFinite;
        const auto feedbackProduct = delayed * controls.feedback;
        if (!std::isfinite(feedbackProduct))
            return Error::FeedbackProductNonFinite;
        delayWrite = input + feedbackProduct;
        if (!std::isfinite(delayWrite))
            return Error::FeedbackSumNonFinite;
        // Schroeder's allpass: the delayed signal less the fed-back sum.
        wet = config.type == Type::Allpass
            ? delayed - controls.feedback * delayWrite
            : delayed;
        if (!std::isfinite(wet))
            return Error::WetTermNonFinite;
    }

    const auto dryTerm = input * (1.0 - controls.mix);
    if (!std::isfinite(dryTerm))
        return Error::DryTermNonFinite;
    const auto wetTerm = wet * controls.mix;
    if (!std::isfinite(wetTerm))
        return Error::WetTermNonFinite;
    const auto candidateOutput = dryTerm + wetTerm;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    if (sharedLine)
    {
        const auto sum = history[writeIndex] + delayWrite;
        if (!std::isfinite(sum))
            return Error::FeedbackSumNonFinite;
        history[writeIndex] = sum;
    }
    else if (!history.empty())
    {
        history[writeIndex] = delayWrite;
        writeIndex = (writeIndex + 1U) % history.size();
    }
    if (glide != nullptr && glide->enabled)
    {
        glide->frames = glideFrames;
        glide->started = true;
    }
    output = candidateOutput;
    return Error::None;
}

// ADR-0025: a delay line that DelayWithFeedbacks naming one Wavetable
// share. The line's frame for sample Frame is Frame modulo its length.
// Before a delay reads or writes at a sample, the frames since the line was
// last touched are emptied, so a frame at which no delay ran holds silence;
// a line not yet started, or kept over a restart, empties only its current
// frame. Returns the current frame's index.
[[nodiscard]] inline std::size_t touchSharedLine(
    std::span<double> line,
    bool& started,
    std::uint64_t& touchedFrame,
    std::uint64_t frame) noexcept
{
    const auto length = static_cast<std::uint64_t>(line.size());
    const auto index = static_cast<std::size_t>(frame % length);
    if (!started)
    {
        line[index] = 0.0;
        started = true;
        touchedFrame = frame;
        return index;
    }
    if (frame <= touchedFrame)
        return index;
    const auto emptied = std::min(frame - touchedFrame, length);
    for (std::uint64_t back = 0; back < emptied; ++back)
        line[static_cast<std::size_t>((frame - back) % length)] = 0.0;
    touchedFrame = frame;
    return index;
}

inline void reset(
    std::span<double> history,
    std::size_t& writeIndex) noexcept
{
    std::ranges::fill(history, 0.0);
    writeIndex = 0;
}
}
