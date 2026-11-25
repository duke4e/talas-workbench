#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_wait_until
{
enum class Error
{
    None,
    StateInvalid,
    AudioInNonFinite,
    ResumeNonFinite,
    HistoryFull,
    HistorySampleNonFinite,
};

struct Transition
{
    double output = 0.0;
    bool writesHistory = false;
    std::size_t writeIndex = 0;
    double historyWrite = 0.0;
    std::size_t nextReadIndex = 0;
    std::size_t nextWriteIndex = 0;
    std::size_t nextCount = 0;
    bool nextResumed = false;
};

[[nodiscard]] inline Error advance(
    std::span<const double> history,
    std::size_t readIndex,
    std::size_t writeIndex,
    std::size_t count,
    bool resumed,
    double audioIn,
    double resume,
    bool resumeOnZero,
    Transition& destination) noexcept
{
    if (history.empty() || readIndex >= history.size()
        || writeIndex >= history.size() || count > history.size())
    {
        return Error::StateInvalid;
    }
    const auto distanceToWrap = history.size() - readIndex;
    const auto expectedWriteIndex = count >= distanceToWrap
        ? count - distanceToWrap
        : readIndex + count;
    if (writeIndex != expectedWriteIndex)
        return Error::StateInvalid;
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(resume))
        return Error::ResumeNonFinite;

    destination = {
        .output = 0.0,
        .writesHistory = false,
        .writeIndex = writeIndex,
        .historyWrite = audioIn,
        .nextReadIndex = readIndex,
        .nextWriteIndex = writeIndex,
        .nextCount = count,
        .nextResumed = resumed
            || (resumeOnZero ? resume == 0.0 : resume != 0.0),
    };

    if (!destination.nextResumed)
    {
        if (count == history.size())
            return Error::HistoryFull;
        destination.writesHistory = true;
        destination.nextWriteIndex = (writeIndex + 1U) % history.size();
        destination.nextCount = count + 1U;
        return Error::None;
    }

    if (count == 0)
    {
        destination.output = audioIn;
        return Error::None;
    }

    const auto historySample = history[readIndex];
    if (!std::isfinite(historySample))
        return Error::HistorySampleNonFinite;
    destination.output = historySample;
    destination.writesHistory = true;
    destination.nextReadIndex = (readIndex + 1U) % history.size();
    destination.nextWriteIndex = (writeIndex + 1U) % history.size();
    return Error::None;
}

inline void reset(
    std::span<double> history,
    std::size_t& readIndex,
    std::size_t& writeIndex,
    std::size_t& count,
    bool& resumed) noexcept
{
    for (auto& sample : history)
        sample = 0.0;
    readIndex = 0;
    writeIndex = 0;
    count = 0;
    resumed = false;
}
}
