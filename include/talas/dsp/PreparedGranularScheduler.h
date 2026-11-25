#pragma once

#include "talas/dsp/PreparedGranularLimits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace talas::dsp
{
struct PreparedGranularGrainStart
{
    std::int64_t index = 0;
    double startFrame = 0.0;

    bool operator==(const PreparedGranularGrainStart&) const = default;
};

struct PreparedGranularDensitySchedulerState
{
    double sampleRate = 0.0;
    std::size_t capacity = 0;
    double densityPhase = 0.0;
    std::int64_t nextGrainIndex = 0;
    std::array<PreparedGranularGrainStart,
        maximumPreparedGranularActiveGrains> starts{};
    std::size_t oldestIndex = 0;
    std::size_t size = 0;

    bool operator==(
        const PreparedGranularDensitySchedulerState&) const = default;
};

struct PreparedGranularDensityMixView
{
    std::array<PreparedGranularGrainStart,
        maximumPreparedGranularActiveGrains> starts{};
    std::size_t size = 0;

    bool operator==(const PreparedGranularDensityMixView&) const = default;
};

enum class PreparedGranularSchedulerStatus
{
    Ok,
    InvalidInput,
    GrainIndexOverflow,
};

class PreparedGranularDensityScheduler
{
public:
    PreparedGranularDensityScheduler(
        double sampleRate,
        std::size_t retainedCapacity);

    [[nodiscard]] PreparedGranularSchedulerStatus scheduleFrame(
        double density,
        double absoluteFrame) noexcept;

    [[nodiscard]] PreparedGranularSchedulerStatus scheduleFrame(
        double density,
        double absoluteFrame,
        std::size_t retainedLimit) noexcept;

    [[nodiscard]] PreparedGranularSchedulerStatus scheduleFrameWithMixView(
        double density,
        double absoluteFrame,
        std::size_t retainedLimit,
        std::size_t mixLimit,
        bool includeAllCrossingsForMix,
        double inclusiveFrameTolerance,
        PreparedGranularDensityMixView& mixView) noexcept;

    [[nodiscard]] std::optional<PreparedGranularGrainStart> recentStart(
        std::size_t newestOffset) const noexcept;

    [[nodiscard]] PreparedGranularDensitySchedulerState captureState()
        const noexcept;

    [[nodiscard]] PreparedGranularSchedulerStatus restoreState(
        const PreparedGranularDensitySchedulerState& state) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] double densityPhase() const noexcept { return densityPhase_; }
    [[nodiscard]] std::int64_t nextGrainIndex() const noexcept
    {
        return nextGrainIndex_;
    }

private:
    [[nodiscard]] PreparedGranularSchedulerStatus scheduleFrameImpl(
        double density,
        double absoluteFrame,
        std::size_t retainedLimit,
        std::size_t mixLimit,
        bool includeAllCrossingsForMix,
        double inclusiveFrameTolerance,
        PreparedGranularDensityMixView* mixView) noexcept;

    void append(PreparedGranularGrainStart start) noexcept;

    double sampleRate_ = 0.0;
    std::size_t capacity_ = 0;
    double densityPhase_ = 0.0;
    std::int64_t nextGrainIndex_ = 0;
    std::array<PreparedGranularGrainStart,
        maximumPreparedGranularActiveGrains> starts_{};
    std::size_t oldestIndex_ = 0;
    std::size_t size_ = 0;
};

[[nodiscard]] double deterministicSignedGrainRandom(
    double seed,
    std::int64_t grainIndex,
    std::uint64_t stream) noexcept;
}
