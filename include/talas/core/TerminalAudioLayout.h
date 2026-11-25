#pragma once

#include <array>
#include <span>
#include <string_view>

namespace talas::core
{
struct TerminalAudioInput
{
    std::string_view portId;
    bool optional = false;
    // An optional channel with a fallback carries the fallback's input when
    // its own port is unconnected, so a mono Sound fills a stereo pair.
    std::string_view fallbackPortId{};
};

// Ordered output channels of explicit audio terminals. A stereo pair takes
// its right channel from its Right port, or from its left input when that is
// unconnected. Other optional channels exist
// only when connected; required inputs are checked by graph validation.
[[nodiscard]] inline std::span<const TerminalAudioInput>
terminalAudioInputs(std::string_view typeId) noexcept
{
    static constexpr std::array<TerminalAudioInput, 2> audio{{
        {"audioIn"}, {"rightIn", true}}};
    static constexpr std::array<TerminalAudioInput, 1> recorder{{{"audioIn"}}};
    static constexpr std::array<TerminalAudioInput, 4> output4{{
        {"out1"}, {"out2"}, {"out3"}, {"out4"}}};
    static constexpr std::array<TerminalAudioInput, 6> output6{{
        {"out1"}, {"out2"}, {"out3"}, {"out4"},
        {"out5"}, {"out6"}}};
    static constexpr std::array<TerminalAudioInput, 8> output8{{
        {"out1"}, {"out2"}, {"out3"}, {"out4"},
        {"out5"}, {"out6"}, {"out7"}, {"out8"}}};
    static constexpr std::array<TerminalAudioInput, 4> stereo4{{
        {"out12"}, {"out12Right", true, "out12"},
        {"out34"}, {"out34Right", true, "out34"}}};
    static constexpr std::array<TerminalAudioInput, 8> stereo8{{
        {"out12"}, {"out12Right", true, "out12"},
        {"out34"}, {"out34Right", true, "out34"},
        {"out56"}, {"out56Right", true, "out56"},
        {"out78"}, {"out78Right", true, "out78"}}};
    if (typeId == "talas.audioOutput") return audio;
    if (typeId == "talas.diskRecorder") return recorder;
    if (typeId == "talas.output4") return output4;
    if (typeId == "talas.output6") return output6;
    if (typeId == "talas.output8") return output8;
    if (typeId == "talas.stereoInOutput4") return stereo4;
    if (typeId == "talas.stereoInOutput8") return stereo8;
    return {};
}

// A stereo Disk Recorder's channels: its Input and its Right input, or its
// Input again when Right is unconnected, as a mono Sound is identical on
// both channels.
[[nodiscard]] inline std::span<const TerminalAudioInput>
stereoDiskRecorderAudioInputs() noexcept
{
    static constexpr std::array<TerminalAudioInput, 2> recorder{{
        {"audioIn"}, {"rightIn", true, "audioIn"}}};
    return recorder;
}
}
