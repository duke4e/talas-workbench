#pragma once

#include <cmath>

namespace talas::runtime::checked_time_stopper
{

enum class Error
{
    None,
    AudioInNonFinite,
    ResumeNonFinite,
    StateInvalid,
    OutputNonFinite,
};

struct State
{
    double heldSample = 0.0;
    bool hasHeldSample = false;
};

struct Transition
{
    double nextHeldSample = 0.0;
    bool nextHasHeldSample = false;
    double output = 0.0;
};

[[nodiscard]] inline Error advance(
    State state,
    double audioIn,
    double resume,
    bool resumeOnZero,
    Transition& transition) noexcept
{
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(resume))
        return Error::ResumeNonFinite;
    if (!std::isfinite(state.heldSample)
        || (!state.hasHeldSample && state.heldSample != 0.0))
    {
        return Error::StateInvalid;
    }

    const auto shouldResume = resumeOnZero
        ? resume == 0.0
        : resume != 0.0;
    if (shouldResume || !state.hasHeldSample)
    {
        state.heldSample = audioIn;
        state.hasHeldSample = true;
    }
    if (!std::isfinite(state.heldSample))
        return Error::OutputNonFinite;

    transition = {
        .nextHeldSample = state.heldSample,
        .nextHasHeldSample = state.hasHeldSample,
        .output = state.heldSample,
    };
    return Error::None;
}

} // namespace talas::runtime::checked_time_stopper
