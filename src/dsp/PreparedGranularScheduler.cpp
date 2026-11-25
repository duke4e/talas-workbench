#include "talas/dsp/PreparedGranularScheduler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
}

PreparedGranularDensityScheduler::PreparedGranularDensityScheduler(
    double sampleRate,
    std::size_t retainedCapacity)
    : sampleRate_(sampleRate)
    , capacity_(retainedCapacity)
{
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0.0
        || capacity_ == 0
        || capacity_ > maximumPreparedGranularActiveGrains)
    {
        throw std::invalid_argument(
            "Invalid prepared granular scheduler configuration");
    }
}

PreparedGranularSchedulerStatus
PreparedGranularDensityScheduler::scheduleFrame(
    double density,
    double absoluteFrame) noexcept
{
    return scheduleFrame(density, absoluteFrame, capacity_);
}

PreparedGranularSchedulerStatus
PreparedGranularDensityScheduler::scheduleFrame(
    double density,
    double absoluteFrame,
    std::size_t retainedLimit) noexcept
{
    return scheduleFrameImpl(
        density,
        absoluteFrame,
        retainedLimit,
        0,
        false,
        0.0,
        nullptr);
}

PreparedGranularSchedulerStatus
PreparedGranularDensityScheduler::scheduleFrameWithMixView(
    double density,
    double absoluteFrame,
    std::size_t retainedLimit,
    std::size_t mixLimit,
    bool includeAllCrossingsForMix,
    double inclusiveFrameTolerance,
    PreparedGranularDensityMixView& mixView) noexcept
{
    mixView = {};
    return scheduleFrameImpl(
        density,
        absoluteFrame,
        retainedLimit,
        mixLimit,
        includeAllCrossingsForMix,
        inclusiveFrameTolerance,
        &mixView);
}

PreparedGranularSchedulerStatus
PreparedGranularDensityScheduler::scheduleFrameImpl(
    double density,
    double absoluteFrame,
    std::size_t retainedLimit,
    std::size_t mixLimit,
    bool includeAllCrossingsForMix,
    double inclusiveFrameTolerance,
    PreparedGranularDensityMixView* mixView) noexcept
{
    if (!std::isfinite(density) || density < 0.0
        || !std::isfinite(absoluteFrame)
        || retainedLimit == 0 || retainedLimit > capacity_)
    {
        return PreparedGranularSchedulerStatus::InvalidInput;
    }
    if (mixView != nullptr
        && (mixLimit == 0 || mixLimit > capacity_
            || !std::isfinite(inclusiveFrameTolerance)
            || inclusiveFrameTolerance < 0.0))
    {
        return PreparedGranularSchedulerStatus::InvalidInput;
    }
    if (density == 0.0)
        return PreparedGranularSchedulerStatus::Ok;

    const auto increment = density / sampleRate_;
    const auto phaseEnd = densityPhase_ + increment;
    if (!std::isfinite(increment) || increment <= 0.0
        || !std::isfinite(phaseEnd))
    {
        return PreparedGranularSchedulerStatus::InvalidInput;
    }

    const auto crossings = std::floor(phaseEnd);
    const auto appendImmediate = nextGrainIndex_ == 0 && size_ == 0;
    const auto candidateNextGrainIndex = nextGrainIndex_
        + static_cast<std::int64_t>(appendImmediate);
    std::array<PreparedGranularGrainStart,
        maximumPreparedGranularActiveGrains> candidateStarts{};
    std::size_t candidateStartCount = 0;
    std::int64_t crossingCount = 0;
    std::int64_t skippedCrossings = 0;
    if (crossings > 0.0)
    {
        constexpr auto indexLimitExclusive = 0x1p63;
        if (crossings >= indexLimitExclusive
            || static_cast<double>(candidateNextGrainIndex) + crossings
                >= indexLimitExclusive)
        {
            return PreparedGranularSchedulerStatus::GrainIndexOverflow;
        }

        crossingCount = static_cast<std::int64_t>(crossings);
        const auto retainedCrossings = static_cast<std::int64_t>(
            std::min<std::uint64_t>(
                static_cast<std::uint64_t>(crossingCount),
                static_cast<std::uint64_t>(retainedLimit)));
        skippedCrossings = crossingCount - retainedCrossings;
        for (std::int64_t crossing = skippedCrossings + 1;
             crossing <= crossingCount; ++crossing)
        {
            const auto eventFraction =
                (static_cast<double>(crossing) - densityPhase_) / increment;
            const auto startFrame = absoluteFrame + eventFraction;
            if (!std::isfinite(startFrame))
                return PreparedGranularSchedulerStatus::InvalidInput;
            candidateStarts[candidateStartCount] = {
                candidateNextGrainIndex + skippedCrossings
                    + static_cast<std::int64_t>(candidateStartCount),
                startFrame,
            };
            ++candidateStartCount;
        }
    }

    auto candidateDensityPhase = phaseEnd - crossings;
    if (candidateDensityPhase < 0.0
        && candidateDensityPhase > -1.0e-12)
    {
        candidateDensityPhase = 0.0;
    }
    if (!std::isfinite(candidateDensityPhase)
        || candidateDensityPhase < 0.0
        || candidateDensityPhase >= 1.0)
    {
        return PreparedGranularSchedulerStatus::InvalidInput;
    }

    auto candidateMixView = PreparedGranularDensityMixView{};
    if (mixView != nullptr)
    {
        const auto inclusiveFrame = absoluteFrame
            + inclusiveFrameTolerance;
        if (!std::isfinite(inclusiveFrame))
            return PreparedGranularSchedulerStatus::InvalidInput;

        const auto firstMixCrossing = includeAllCrossingsForMix
            ? std::int64_t{1} : skippedCrossings + 1;
        if (crossingCount >= firstMixCrossing)
        {
            const auto startForCrossing = [&](std::int64_t crossing) {
                const auto eventFraction =
                    (static_cast<double>(crossing) - densityPhase_)
                    / increment;
                return absoluteFrame + eventFraction;
            };
            auto firstFutureCrossing = firstMixCrossing;
            auto end = crossingCount + 1;
            while (firstFutureCrossing < end)
            {
                const auto middle = firstFutureCrossing
                    + (end - firstFutureCrossing) / 2;
                const auto startFrame = startForCrossing(middle);
                if (!std::isfinite(startFrame))
                    return PreparedGranularSchedulerStatus::InvalidInput;
                if (startFrame <= inclusiveFrame)
                    firstFutureCrossing = middle + 1;
                else
                    end = middle;
            }

            for (auto crossing = firstFutureCrossing - 1;
                 crossing >= firstMixCrossing
                    && candidateMixView.size < mixLimit;
                 --crossing)
            {
                const auto startFrame = startForCrossing(crossing);
                if (!std::isfinite(startFrame))
                    return PreparedGranularSchedulerStatus::InvalidInput;
                candidateMixView.starts[candidateMixView.size] = {
                    candidateNextGrainIndex + crossing - 1,
                    startFrame,
                };
                ++candidateMixView.size;
            }
        }
        if (appendImmediate && candidateMixView.size < mixLimit)
        {
            candidateMixView.starts[candidateMixView.size] = {
                nextGrainIndex_, absoluteFrame};
            ++candidateMixView.size;
        }
    }

    if (appendImmediate)
        append({nextGrainIndex_++, absoluteFrame});
    nextGrainIndex_ += skippedCrossings;
    for (std::size_t index = 0; index < candidateStartCount; ++index)
    {
        append(candidateStarts[index]);
        ++nextGrainIndex_;
    }
    while (size_ > retainedLimit)
    {
        oldestIndex_ = (oldestIndex_ + 1) % capacity_;
        --size_;
    }
    densityPhase_ = candidateDensityPhase;
    if (mixView != nullptr)
        *mixView = candidateMixView;
    return PreparedGranularSchedulerStatus::Ok;
}

std::optional<PreparedGranularGrainStart>
PreparedGranularDensityScheduler::recentStart(
    std::size_t newestOffset) const noexcept
{
    if (newestOffset >= size_)
        return std::nullopt;
    const auto physicalIndex =
        (oldestIndex_ + size_ - 1 - newestOffset)
        % capacity_;
    return starts_[physicalIndex];
}

PreparedGranularDensitySchedulerState
PreparedGranularDensityScheduler::captureState() const noexcept
{
    return {
        sampleRate_,
        capacity_,
        densityPhase_,
        nextGrainIndex_,
        starts_,
        oldestIndex_,
        size_,
    };
}

PreparedGranularSchedulerStatus
PreparedGranularDensityScheduler::restoreState(
    const PreparedGranularDensitySchedulerState& state) noexcept
{
    if (state.sampleRate != sampleRate_
        || state.capacity != capacity_
        || !std::isfinite(state.densityPhase)
        || state.densityPhase < 0.0
        || state.densityPhase >= 1.0
        || state.nextGrainIndex < 0
        || state.oldestIndex >= capacity_
        || state.size > capacity_)
    {
        return PreparedGranularSchedulerStatus::InvalidInput;
    }

    std::int64_t previousIndex = -1;
    for (std::size_t offset = 0; offset < state.size; ++offset)
    {
        const auto physicalIndex =
            (state.oldestIndex + offset) % state.capacity;
        const auto& start = state.starts[physicalIndex];
        if (start.index < 0
            || start.index <= previousIndex
            || start.index >= state.nextGrainIndex
            || !std::isfinite(start.startFrame))
        {
            return PreparedGranularSchedulerStatus::InvalidInput;
        }
        previousIndex = start.index;
    }

    densityPhase_ = state.densityPhase;
    nextGrainIndex_ = state.nextGrainIndex;
    starts_ = state.starts;
    oldestIndex_ = state.oldestIndex;
    size_ = state.size;
    return PreparedGranularSchedulerStatus::Ok;
}

void PreparedGranularDensityScheduler::reset() noexcept
{
    densityPhase_ = 0.0;
    nextGrainIndex_ = 0;
    oldestIndex_ = 0;
    size_ = 0;
    starts_.fill({});
}

void PreparedGranularDensityScheduler::append(
    PreparedGranularGrainStart start) noexcept
{
    if (size_ < capacity_)
    {
        const auto index = (oldestIndex_ + size_)
            % capacity_;
        starts_[index] = start;
        ++size_;
        return;
    }
    starts_[oldestIndex_] = start;
    oldestIndex_ = (oldestIndex_ + 1)
        % capacity_;
}

double deterministicSignedGrainRandom(
    double seed,
    std::int64_t grainIndex,
    std::uint64_t stream) noexcept
{
    const auto clampedSeed = std::clamp(seed, 0.0, 1.0);
    const auto seedBits = static_cast<std::uint64_t>(std::llround(
        clampedSeed * 9007199254740991.0));
    auto value = seedBits
        ^ (static_cast<std::uint64_t>(grainIndex)
            * 0xd1b54a32d192ed03ULL);
    value ^= stream * 0x9fb21c651e98df25ULL;
    const auto randomBits = splitMix64(value);
    const auto unit = static_cast<double>(randomBits >> 11U)
        * (1.0 / 9007199254740992.0);
    return unit * 2.0 - 1.0;
}
}
