#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace talas::audio
{
enum class OscilloscopeTrigger
{
    Free,
    Rising,
    Falling,
};

struct OscilloscopeWindow
{
    std::size_t firstFrame = 0;
    std::size_t frameCount = 0;
    bool triggered = false;
};

class OscilloscopeSingleShotState
{
public:
    [[nodiscard]] bool acceptsDisplay(bool singleShot) const noexcept
    {
        return !singleShot || !latched_;
    }

    void observe(bool singleShot, bool triggered) noexcept
    {
        if (singleShot && triggered)
            latched_ = true;
    }

    void rearm() noexcept
    {
        latched_ = false;
    }

    [[nodiscard]] bool latched() const noexcept
    {
        return latched_;
    }

private:
    bool latched_ = false;
};

class OscilloscopeSweepState
{
public:
    [[nodiscard]] bool shouldUpdate(
        std::uint64_t capturedEndFrame,
        double sampleRate,
        double sweepFrequencyHz) noexcept;
    void reset() noexcept;

private:
    std::uint64_t lastUpdateEndFrame_ = 0;
    bool hasUpdate_ = false;
};

[[nodiscard]] OscilloscopeWindow selectOscilloscopeWindow(
    std::span<const double> samples,
    std::size_t requestedFrameCount,
    OscilloscopeTrigger trigger,
    double threshold = 0.0);

[[nodiscard]] OscilloscopeWindow selectOscilloscopeWindow(
    std::span<const double> samples,
    std::span<const double> triggerSamples,
    std::size_t requestedFrameCount,
    OscilloscopeTrigger trigger,
    double threshold = 0.0);

[[nodiscard]] double normalizedOscilloscopeSample(double sample);

// Info > Oscilloscope: a power-of-two number of
// samples from an absolute frame that is a multiple of `lock`, so that a
// signal whose period is 1, 2, 4 ... 512 samples stands still; the newest
// such window of `available` samples beginning at absoluteFirstFrame, or
// none when they hold none.
[[nodiscard]] std::optional<OscilloscopeWindow> powerOfTwoLockedOscilloscopeWindow(
    std::uint64_t absoluteFirstFrame, std::size_t available, std::size_t frameCount, std::size_t lock = 512) noexcept;
}
