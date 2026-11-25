#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace talas::runtime::checked_mixer_sum
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    SumNonFinite,
};

// Adds one of Mixer's inputs into `sum`; the first input starts it. On
// failure `sum` holds partial scratch and must not be published.
[[nodiscard]] inline Error accumulate(
    std::span<const double> input,
    bool first,
    std::span<double> sum) noexcept
{
    if (input.size() != sum.size())
        return Error::SizeMismatch;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (!std::isfinite(input[frame]))
            return Error::InputNonFinite;
        const auto candidate = first ? input[frame] : sum[frame] + input[frame];
        if (!std::isfinite(candidate))
            return Error::SumNonFinite;
        sum[frame] = candidate;
    }
    return Error::None;
}

// The bindings Mixer's right output adds, by index into `bindings`: each
// inputs binding, or in its place the rightInputs binding from the same
// source (the right channel of a stereo Input), then any rightInputs binding
// whose source has no inputs binding. Empty when nothing is on rightInputs,
// so the right output hears the same sum as the left.
[[nodiscard]] inline std::vector<std::size_t> rightChannelBindings(
    std::span<const InputBinding> bindings)
{
    std::vector<std::size_t> result;
    std::vector<bool> paired(bindings.size(), false);
    const auto hasRightInputs = std::ranges::any_of(
        bindings, [](const InputBinding& binding) {
            return binding.inputPortId == "rightInputs";
        });
    if (!hasRightInputs)
        return result;
    for (std::size_t index = 0; index < bindings.size(); ++index)
    {
        if (bindings[index].inputPortId != "inputs")
            continue;
        auto chosen = index;
        for (std::size_t right = 0; right < bindings.size(); ++right)
        {
            if (!paired[right]
                && bindings[right].inputPortId == "rightInputs"
                && bindings[right].sourceNodeId == bindings[index].sourceNodeId
                && bindings[right].sourceScopeId == bindings[index].sourceScopeId)
            {
                paired[right] = true;
                chosen = right;
                break;
            }
        }
        result.push_back(chosen);
    }
    for (std::size_t right = 0; right < bindings.size(); ++right)
    {
        if (!paired[right] && bindings[right].inputPortId == "rightInputs")
            result.push_back(right);
    }
    return result;
}
}
