#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::dsp
{
// A FrequencyScale: grains of the Input,
// each two periods of the frequency its FreqTracker gives around a pitch
// epoch, shaped by Window and laid down a period apart divided by the
// scale, so that they overlap to scale up and leave time between them to
// scale down; the duration is unchanged. The Input is delayed by Delay so
// that it lines up with the tracker's estimate.
class FrequencyScaleGranulator
{
public:
    // The lowest frequency a granulator follows, in hz: a tracker below it,
    // or one without an estimate, gives grains of its period.
    static constexpr double lowestFrequency = 20.0;
    // The scales a granulator plays.
    static constexpr double smallestScale = 1.0 / 64.0;
    static constexpr double largestScale = 64.0;

    struct Configuration
    {
        double sampleRate = 0.0;
        // The Delay, in frames, by which the Input is late.
        std::size_t delayFrames = 0;
        // MaxScale: the largest scale, which bounds the grains in flight.
        double maximumScale = 1.0;
        // The grain envelope, read across its two periods; Hann when empty.
        std::vector<double> window;
    };

    enum class Status : std::uint8_t
    {
        Ok,
        InvalidConfiguration,
        NonFiniteInput,
    };

    [[nodiscard]] static bool valid(const Configuration& configuration) noexcept;
    [[nodiscard]] static bool supportedScale(double scale) noexcept;

    // Allocates for `configuration`; nothing renders until it succeeds.
    [[nodiscard]] bool prepare(Configuration configuration);
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    void reset() noexcept;
    // Takes another granulator's state without allocating; false when the
    // two were prepared differently.
    [[nodiscard]] bool copyStateFrom(const FrequencyScaleGranulator& other) noexcept;

    // Renders frames: `input`, the tracker's `frequency` in hz and the
    // `scale`, each per frame, into `output`. Scales are held within
    // smallestScale and MaxScale. No allocation.
    [[nodiscard]] Status process(
        std::span<const double> input,
        std::span<const double> frequency,
        std::span<const double> scale,
        std::span<double> output) noexcept;

private:
    struct Grain
    {
        bool active = false;
        // The input frame of the grain's first sample, its length in
        // frames, how far it has played and its gain.
        double start = 0.0;
        double length = 0.0;
        double position = 0.0;
        double gain = 1.0;
    };

    [[nodiscard]] double windowAt(double phase) const noexcept;
    [[nodiscard]] double history(double frame) const noexcept;
    void startGrain(double offset, double scale) noexcept;

    Configuration configuration_;
    bool prepared_ = false;
    double longestPeriod_ = 0.0;
    std::vector<double> history_;
    std::vector<Grain> grains_;
    std::uint64_t frame_ = 0;
    // The latest pitch epoch, as an input frame, and its period.
    bool hasEpoch_ = false;
    double epoch_ = 0.0;
    double epochPeriod_ = 0.0;
    double phase_ = 0.0;
    double nextGrain_ = 0.0;
};
}
