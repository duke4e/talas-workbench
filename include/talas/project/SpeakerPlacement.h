#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Speaker Placement: where the speakers stand
// round the listener, one an output channel in their order, each a radius
// and an angle in degrees: hard left 0, straight ahead 90, hard right 180,
// a negative angle as far behind as the positive one is in front; a
// subwoofer stands nowhere in the circle. The Placement preset menu's
// predefined setups fill them in.
namespace talas::project
{
struct SpeakerPosition
{
    double radius = 1.0;
    double angle = 0.0;
    bool subwoofer = false;

    bool operator==(const SpeakerPosition&) const = default;
};

inline constexpr std::size_t maximumSpeakers = 8;

struct SpeakerPlacementPreset
{
    std::string_view name;
    std::vector<SpeakerPosition> speakers;
};

[[nodiscard]] const std::vector<SpeakerPlacementPreset>& speakerPlacementPresets();
// The predefined setup a placement is, if it is one.
[[nodiscard]] std::optional<std::string_view> speakerPlacementPresetName(const std::vector<SpeakerPosition>& speakers);
// One speaker a line, its radius and angle, or "sub" for a subwoofer; and
// read back, 1 to 8 speakers of a radius of 0 or more and an angle from
// -360 to 360, else nothing.
[[nodiscard]] std::string speakerPlacementText(const std::vector<SpeakerPosition>& speakers);
[[nodiscard]] std::optional<std::vector<SpeakerPosition>> parseSpeakerPlacement(std::string_view text);
}
