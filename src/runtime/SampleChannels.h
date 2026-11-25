#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace talas::runtime::sample_channels
{
// The Sound types whose stereo samples play their left channel on audioOut
// and their right channel on rightOut (format 95).
[[nodiscard]] inline bool playsStereoFiles(std::string_view typeId) noexcept
{
    return typeId == "talas.sample" || typeId == "talas.multisample"
        || typeId == "talas.keyMappedMultisample" || typeId == "talas.diskPlayer"
        || typeId == "talas.multifileDiskPlayer";
}

// Whether a step plays a stereo file's channels apart: projects before
// format 95 name `channels` `legacyMono`.
[[nodiscard]] inline bool stereo(const CompileStep& step) noexcept
{
    if (!playsStereoFiles(step.typeId))
        return false;
    const auto found = std::ranges::find(step.parameters,
        std::string_view{"channels"}, &ParameterValue::id);
    return found == step.parameters.end()
        || found->choice != std::optional<std::string>{"legacyMono"};
}

// A table's right channel, or its only one.
[[nodiscard]] inline std::span<const double> right(const TableResource& table) noexcept
{
    return table.rightSamples.size() == table.samples.size() && !table.rightSamples.empty()
        ? std::span<const double>{table.rightSamples}
        : std::span<const double>{table.samples};
}
}
