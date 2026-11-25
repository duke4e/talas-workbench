#include "talas/app/PasteHotModel.h"

#include "talas/project/ProjectGlobalMap.h"

#include <algorithm>
#include <cctype>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}
}

std::vector<PasteHotEntry> pasteHotEntries(const talas::project::ProjectDocument& document)
{
    std::vector<PasteHotEntry> entries{
        {"Sampling rate", "SignalProcessor sampleRate"},
        {"Half the sampling rate", "SignalProcessor sampleRate * 0.5"},
        {"MIDI key pitch", "!KeyPitch"},
        {"MIDI key velocity", "!KeyVelocity"},
        {"MIDI key down", "!KeyDown"},
        {"Fader with a smoothed change", "(!Fader smoothed)"},
        {"A ramp repeating each second", "(1 repeatingRamp: 1 s)"},
        // A frequency's inverse is the duration of one cycle,
        // and a duration's the frequency repeating it.
        {"One cycle of a frequency, as a duration", "(440 hz inverse)"},
        {"Once each duration, as a frequency", "(2 s inverse)"},
    };
    std::vector<std::string> names;
    for (const auto& entry : talas::project::projectGlobalMapEntries(document))
        names.push_back("!" + entry.eventName);
    std::ranges::sort(names, {}, [](const std::string& name) { return lowered(name); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (auto& name : names)
        entries.push_back({name, name});
    return entries;
}

std::optional<std::size_t> pasteHotMatch(std::span<const PasteHotEntry> entries, std::string_view typed)
{
    const auto wanted = lowered(typed);
    if (wanted.empty())
        return std::nullopt;
    const auto bare = [](std::string text) { return text.starts_with('!') ? text.substr(1) : text; };
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        if (lowered(entries[index].label).starts_with(wanted) || bare(lowered(entries[index].text)).starts_with(bare(wanted)))
            return index;
    }
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        if (lowered(entries[index].label).find(wanted) != std::string::npos
            || lowered(entries[index].text).find(wanted) != std::string::npos)
            return index;
    }
    return std::nullopt;
}
}
