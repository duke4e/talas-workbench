#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace talas::project
{
inline constexpr std::size_t maximumMidiMapperMapBytes = 512;
inline constexpr std::size_t maximumMidiMapperMapAssociations = 16;

enum class MidiMapperMapMode : std::uint8_t
{
    Passthrough,
    Gate,
    Velocity,
};

enum class MidiMapperMapParseError : std::uint8_t
{
    None,
    TooLong,
    TooManyAssociations,
    NonAscii,
    DuplicateTarget,
    UnknownMode,
};

enum class MidiMapperMapDisplayAs : std::uint8_t
{
    Default,
    Fader,
    SmallFader,
    Toggle,
    Gate,
    Nothing,
    // A knob, turned.
    Rotary,
    Pot,
};

enum class MidiMapperMapTaper : std::uint8_t
{
    Linear,
    Log,
};

struct MidiMapperMapAssociation
{
    std::string_view localEventValue;
    std::string_view localEventSource;
    std::string_view minimumText;
    std::string_view maximumText;
    std::string_view gridText;
    std::optional<std::uint8_t> channel;
    MidiMapperMapDisplayAs displayAs = MidiMapperMapDisplayAs::Default;
    MidiMapperMapTaper taper = MidiMapperMapTaper::Linear;
    std::optional<double> pitchBendSemitones;
    bool hidePitchBend = false;

    [[nodiscard]] constexpr bool hasRange() const noexcept
    {
        return !minimumText.empty();
    }

    [[nodiscard]] constexpr bool hasGrid() const noexcept
    {
        return !gridText.empty();
    }
};

struct MidiMapperMapParseResult
{
    MidiMapperMapMode mode = MidiMapperMapMode::Passthrough;
    MidiMapperMapParseError error = MidiMapperMapParseError::None;
    // Views refer to the input text. Audio passthrough and scoped Event Value
    // projections are prepared separately by the runtime.
    std::array<MidiMapperMapAssociation,
        maximumMidiMapperMapAssociations> localAssociations{};
    std::size_t localAssociationCount = 0;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return error == MidiMapperMapParseError::None;
    }
};

[[nodiscard]] MidiMapperMapParseResult
parseProjectMidiMapperMap(std::string_view text) noexcept;

[[nodiscard]] bool parseProjectMidiMapperMapBound(
    std::string_view text, double& value) noexcept;

[[nodiscard]] std::optional<double> projectMidiMapperTaperValue(
    double normalized, double minimum, double maximum,
    MidiMapperMapTaper taper) noexcept;
[[nodiscard]] std::optional<double> projectMidiMapperTaperInverse(
    double value, double minimum, double maximum,
    MidiMapperMapTaper taper) noexcept;
}
