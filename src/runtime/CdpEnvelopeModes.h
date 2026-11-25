#pragma once

#include "talas/cdp/EnvelopeWarp.h"

#include <cmath>
#include <optional>
#include <string_view>

namespace talas::runtime::cdp_envelope
{
// CDP Envelope Warp's modes, CDP's envel warp modes 3, 4, 5, 7, 8, 9, 10,
// 11, 12 and 15.
[[nodiscard]] inline std::optional<talas::cdp::EnvelopeWarp::Mode> warpMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::EnvelopeWarp::Mode;
    return choice == "exaggerate" ? std::optional{Mode::Exaggerate}
        : choice == "attenuate"   ? std::optional{Mode::Attenuate}
        : choice == "lift"        ? std::optional{Mode::Lift}
        : choice == "flatten"     ? std::optional{Mode::Flatten}
        : choice == "gate"        ? std::optional{Mode::Gate}
        : choice == "invert"      ? std::optional{Mode::Invert}
        : choice == "limit"       ? std::optional{Mode::Limit}
        : choice == "corrugate"   ? std::optional{Mode::Corrugate}
        : choice == "expand"      ? std::optional{Mode::Expand}
        : choice == "ducked"      ? std::optional{Mode::Ducked}
                                  : std::optional<Mode>{};
}

// CDP Envelope Impose's modes: envel impose 1 and envel replace 1.
[[nodiscard]] inline std::optional<talas::cdp::EnvelopeImpose::Mode> imposeMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::EnvelopeImpose::Mode;
    return choice == "impose" ? std::optional{Mode::Impose}
        : choice == "replace" ? std::optional{Mode::Replace}
                              : std::optional<Mode>{};
}

// CDP Envelope Target's scales: envel replace 3 and 4.
[[nodiscard]] inline std::optional<talas::cdp::EnvelopeTarget::Mode> targetMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::EnvelopeTarget::Mode;
    return choice == "level"     ? std::optional{Mode::Level}
        : choice == "decibels"   ? std::optional{Mode::Decibels}
                                 : std::optional<Mode>{};
}

// Flatten and corrugate read ahead as far as their largest flatten, trough
// delay and peak separation; a live one takes at most its maximum field.
[[nodiscard]] inline bool readsAheadByMaximum(talas::cdp::EnvelopeWarp::Mode mode, bool second) noexcept
{
    using Mode = talas::cdp::EnvelopeWarp::Mode;
    return second ? mode == Mode::Corrugate : mode == Mode::Flatten || mode == Mode::Corrugate;
}

// Envel warp's whole fields (smoothing and the maxima): 0 to 32767.
[[nodiscard]] inline bool wholeField(const std::optional<double>& value) noexcept
{
    return value.has_value() && *value >= 0.0
        && *value <= static_cast<double>(talas::cdp::EnvelopeWarp::maximumCorrugate) && *value == std::round(*value);
}
}
