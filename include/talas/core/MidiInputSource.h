#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace talas::core
{
enum class MidiInputSource : std::uint8_t
{
    Live,
    File,
    Script,
};

// Source labels share one interpretation in project resource selection and
// runtime preparation. Historical blank fields select live MIDI.
[[nodiscard]] inline std::optional<MidiInputSource> parseMidiInputSource(std::string_view text) noexcept
{
    const auto whitespace = [](char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n'
            || value == '\f' || value == '\v';
    };
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    if (text.size() > 32) return std::nullopt;
    const auto equals = [text](std::string_view expected) {
        if (text.size() != expected.size()) return false;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const auto value = text[index];
            const auto lower = value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
            if (lower != expected[index]) return false;
        }
        return true;
    };
    if (text.empty() || equals("live midi")) return MidiInputSource::Live;
    if (equals("midi file")) return MidiInputSource::File;
    if (equals("script")) return MidiInputSource::Script;
    return std::nullopt;
}
}
