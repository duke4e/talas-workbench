#include "talas/project/SpeakerPlacement.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace talas::project
{
const std::vector<SpeakerPlacementPreset>& speakerPlacementPresets()
{
    // Each in its channels' order; a subwoofer at no angle.
    static const std::vector<SpeakerPlacementPreset> presets{
        {"Stereo", {{1.0, 0.0}, {1.0, 180.0}}},
        {"Quad", {{1.0, 45.0}, {1.0, 135.0}, {1.0, -45.0}, {1.0, -135.0}}},
        {"Quad + Ctr + Sub", {{1.0, 45.0}, {1.0, 135.0}, {1.0, -45.0}, {1.0, -135.0}, {1.0, 90.0}, {1.0, 0.0, true}}},
        {"5.1", {{1.0, 60.0}, {1.0, 120.0}, {1.0, 90.0}, {1.0, 0.0, true}, {1.0, -20.0}, {1.0, -160.0}}},
        {"Hexagon", {{1.0, 60.0}, {1.0, 120.0}, {1.0, 180.0}, {1.0, -120.0}, {1.0, -60.0}, {1.0, 0.0}}},
        {"Octagon", {{1.0, 67.5}, {1.0, 112.5}, {1.0, 157.5}, {1.0, -157.5}, {1.0, -112.5}, {1.0, -67.5}, {1.0, -22.5},
                        {1.0, 22.5}}},
    };
    return presets;
}

std::optional<std::string_view> speakerPlacementPresetName(const std::vector<SpeakerPosition>& speakers)
{
    for (const auto& preset : speakerPlacementPresets())
    {
        if (preset.speakers == speakers)
            return preset.name;
    }
    return std::nullopt;
}

std::string speakerPlacementText(const std::vector<SpeakerPosition>& speakers)
{
    std::string text;
    for (const auto& speaker : speakers)
    {
        if (!text.empty())
            text += '\n';
        if (speaker.subwoofer)
        {
            text += "sub";
            continue;
        }
        char line[64];
        std::snprintf(line, sizeof line, "%g %g", speaker.radius, speaker.angle);
        text += line;
    }
    return text;
}

std::optional<std::vector<SpeakerPosition>> parseSpeakerPlacement(std::string_view text)
{
    std::vector<SpeakerPosition> speakers;
    std::istringstream lines{std::string{text}};
    for (std::string line; std::getline(lines, line);)
    {
        std::istringstream words{line};
        std::string first;
        if (!(words >> first))
            continue;
        std::string rest;
        if (first == "sub")
        {
            if (words >> rest)
                return std::nullopt;
            speakers.push_back({1.0, 0.0, true});
            continue;
        }
        std::string second;
        if (!(words >> second) || (words >> rest))
            return std::nullopt;
        char* end = nullptr;
        const auto radius = std::strtod(first.c_str(), &end);
        if (end != first.c_str() + first.size())
            return std::nullopt;
        const auto angle = std::strtod(second.c_str(), &end);
        if (end != second.c_str() + second.size() || !std::isfinite(radius) || !std::isfinite(angle) || radius < 0.0
            || std::abs(angle) > 360.0)
            return std::nullopt;
        speakers.push_back({radius, angle});
    }
    if (speakers.empty() || speakers.size() > maximumSpeakers)
        return std::nullopt;
    return speakers;
}
}
