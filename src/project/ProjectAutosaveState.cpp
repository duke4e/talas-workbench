#include "talas/project/ProjectAutosaveState.h"

#include <algorithm>
#include <cmath>

namespace talas::project
{
namespace
{
[[nodiscard]] double finiteNonNegativeOr(
    double value,
    double fallback) noexcept
{
    return std::isfinite(value) && value >= 0.0 ? value : fallback;
}
}

ProjectAutosaveState::ProjectAutosaveState(
    double debounceMilliseconds,
    double initialRetryMilliseconds,
    double maximumRetryMilliseconds)
    : debounceMilliseconds_(
        finiteNonNegativeOr(debounceMilliseconds, 2000.0))
    , initialRetryMilliseconds_(
        finiteNonNegativeOr(initialRetryMilliseconds, 5000.0))
    , maximumRetryMilliseconds_(std::max(
        initialRetryMilliseconds_,
        finiteNonNegativeOr(maximumRetryMilliseconds, 30000.0)))
    , currentRetryMilliseconds_(initialRetryMilliseconds_)
{
}

void ProjectAutosaveState::documentChanged(
    bool dirty,
    double nowMilliseconds) noexcept
{
    if (!dirty)
    {
        clear();
        return;
    }
    if (!std::isfinite(nowMilliseconds))
        return;
    pending_ = true;
    dueMilliseconds_ = nowMilliseconds + debounceMilliseconds_;
    currentRetryMilliseconds_ = initialRetryMilliseconds_;
}

bool ProjectAutosaveState::shouldStartWrite(
    bool dirty,
    bool writerActive,
    double nowMilliseconds) const noexcept
{
    return pending_ && dirty && !writerActive
        && std::isfinite(nowMilliseconds)
        && nowMilliseconds >= dueMilliseconds_;
}

void ProjectAutosaveState::writeStarted() noexcept
{
    pending_ = false;
}

void ProjectAutosaveState::writeFinished(
    bool succeeded,
    bool dirty,
    double nowMilliseconds) noexcept
{
    if (succeeded)
    {
        hasWriteFailure_ = false;
        currentRetryMilliseconds_ = initialRetryMilliseconds_;
        return;
    }
    if (!dirty)
    {
        clear();
        return;
    }

    hasWriteFailure_ = true;
    if (!pending_ && std::isfinite(nowMilliseconds))
    {
        pending_ = true;
        dueMilliseconds_ = nowMilliseconds + currentRetryMilliseconds_;
    }
    currentRetryMilliseconds_ = std::min(
        maximumRetryMilliseconds_,
        currentRetryMilliseconds_ * 2.0);
}

void ProjectAutosaveState::clear() noexcept
{
    currentRetryMilliseconds_ = initialRetryMilliseconds_;
    dueMilliseconds_ = 0.0;
    pending_ = false;
    hasWriteFailure_ = false;
}

bool ProjectAutosaveState::pending() const noexcept
{
    return pending_;
}

bool ProjectAutosaveState::hasWriteFailure() const noexcept
{
    return hasWriteFailure_;
}
}
