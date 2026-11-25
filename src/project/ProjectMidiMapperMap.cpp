#include "talas/project/ProjectMidiMapperMap.h"

#include "talas/core/FromChars.h"

#include <charconv>
#include <cmath>

namespace talas::project
{
namespace
{
[[nodiscard]] bool isAsciiWhitespace(char value) noexcept
{
    return value == ' '
        || value == '\t'
        || value == '\n'
        || value == '\r'
        || value == '\f'
        || value == '\v';
}

[[nodiscard]] std::string_view trimAsciiWhitespace(
    std::string_view text) noexcept
{
    while (!text.empty() && isAsciiWhitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isAsciiWhitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool isMapIdentifier(std::string_view value) noexcept
{
    if (value.empty())
        return false;
    for (const char character : value)
    {
        if ((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_')
        {
            continue;
        }
        return false;
    }
    return true;
}

struct PitchBendComposition
{
    std::string_view keySource;
    double semitones = 1.0;
    bool hideBend = false;
};

[[nodiscard]] std::optional<PitchBendComposition>
parsePitchBendComposition(std::string_view source) noexcept
{
    if (!source.ends_with(" nn"))
        return std::nullopt;
    source = trimAsciiWhitespace(source.substr(0, source.size() - 3));
    if (!source.starts_with('(') || !source.ends_with(')'))
        return std::nullopt;
    source = trimAsciiWhitespace(source.substr(1, source.size() - 2));
    constexpr std::string_view keyToken = "`MIDIKeyNumber";
    if (!source.starts_with(keyToken))
        return std::nullopt;
    const auto keySource = source.substr(0, keyToken.size());
    source = trimAsciiWhitespace(source.substr(keyToken.size()));
    if (!source.starts_with('+'))
        return std::nullopt;
    source = trimAsciiWhitespace(source.substr(1));
    if (!source.starts_with('(') || !source.ends_with(')'))
        return std::nullopt;
    source = trimAsciiWhitespace(source.substr(1, source.size() - 2));
    constexpr std::string_view bendToken = "`MIDIPitchBend";
    if (source.starts_with(bendToken))
    {
        const auto option =
            trimAsciiWhitespace(source.substr(bendToken.size()));
        if (option.empty())
            return PitchBendComposition{keySource};
        if (option == "displayAs: #nothing" || option == "displayAs: nothing")
            return PitchBendComposition{keySource, 1.0, true};
        return std::nullopt;
    }
    const auto star = source.find('*');
    if (star == std::string_view::npos)
        return std::nullopt;
    double semitones = 0.0;
    if (!parseProjectMidiMapperMapBound(
            trimAsciiWhitespace(source.substr(0, star)), semitones)
        || !(semitones > 0.0) || semitones > 128.0
        || trimAsciiWhitespace(source.substr(star + 1)) != bendToken)
    {
        return std::nullopt;
    }
    return PitchBendComposition{keySource, semitones};
}
}

bool parseProjectMidiMapperMapBound(
    std::string_view text, double& value) noexcept
{
    if (text.empty())
        return false;
    const auto parsed = talas::fromChars(
        text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size()
        && std::isfinite(value);
}

std::optional<double> projectMidiMapperTaperValue(
    double normalized, double minimum, double maximum,
    MidiMapperMapTaper taper) noexcept
{
    if (!std::isfinite(normalized) || normalized < 0.0
        || normalized > 1.0 || !std::isfinite(minimum)
        || !std::isfinite(maximum) || !(maximum > minimum))
        return std::nullopt;
    if (taper == MidiMapperMapTaper::Linear)
        return minimum + normalized * (maximum - minimum);
    if (taper != MidiMapperMapTaper::Log || minimum < 0.0)
        return std::nullopt;
    constexpr double ratio = 1000.0;
    if (minimum == 0.0)
        return maximum
            * (std::expm1(std::log(ratio) * normalized) / (ratio - 1.0));
    return std::exp(std::log(minimum)
        + normalized * (std::log(maximum) - std::log(minimum)));
}

std::optional<double> projectMidiMapperTaperInverse(
    double value, double minimum, double maximum,
    MidiMapperMapTaper taper) noexcept
{
    if (!std::isfinite(value) || !std::isfinite(minimum)
        || !std::isfinite(maximum) || !(maximum > minimum)
        || value < minimum || value > maximum)
        return std::nullopt;
    if (taper == MidiMapperMapTaper::Linear)
        return (value - minimum) / (maximum - minimum);
    if (taper != MidiMapperMapTaper::Log || minimum < 0.0)
        return std::nullopt;
    constexpr double ratio = 1000.0;
    if (minimum == 0.0)
        return std::log1p((ratio - 1.0) * (value / maximum))
            / std::log(ratio);
    return (std::log(value) - std::log(minimum))
        / (std::log(maximum) - std::log(minimum));
}

MidiMapperMapParseResult parseProjectMidiMapperMap(
    std::string_view text) noexcept
{
    if (text.size() > maximumMidiMapperMapBytes)
    {
        return {
            MidiMapperMapMode::Passthrough,
            MidiMapperMapParseError::TooLong,
        };
    }
    for (const auto value : text)
    {
        if (static_cast<unsigned char>(value) > 0x7fU)
        {
            return {
                MidiMapperMapMode::Passthrough,
                MidiMapperMapParseError::NonAscii,
            };
        }
    }

    text = trimAsciiWhitespace(text);
    if (text.empty())
        return {};
    if (text == "gate")
        return {MidiMapperMapMode::Gate, {}};
    if (text == "velocity")
        return {MidiMapperMapMode::Velocity, {}};

    MidiMapperMapParseResult result;
    while (!text.empty())
    {
        const auto lineEnd = text.find('\n');
        const auto line = trimAsciiWhitespace(text.substr(0, lineEnd));
        if (lineEnd == std::string_view::npos)
            text = {};
        else
            text.remove_prefix(lineEnd + 1);
        if (line.empty())
            continue;
        if (!line.starts_with('!'))
        {
            result.error = MidiMapperMapParseError::UnknownMode;
            return result;
        }
        const auto separator = line.find(" is:");
        if (separator == std::string_view::npos)
        {
            result.error = MidiMapperMapParseError::UnknownMode;
            return result;
        }
        auto source = trimAsciiWhitespace(line.substr(separator + 4));
        if (source.ends_with('.'))
            source = trimAsciiWhitespace(source.substr(0, source.size() - 1));
        std::optional<double> pitchBendSemitones;
        bool hidePitchBend = false;
        if (source.ends_with(" nn"))
        {
            const auto composition = parsePitchBendComposition(source);
            if (!composition)
            {
                result.error = MidiMapperMapParseError::UnknownMode;
                return result;
            }
            source = composition->keySource;
            pitchBendSemitones = composition->semitones;
            hidePitchBend = composition->hideBend;
        }
        const bool ranged = !pitchBendSemitones && source.starts_with('(');
        if (ranged)
        {
            if (!source.ends_with(')'))
            {
                result.error = MidiMapperMapParseError::UnknownMode;
                return result;
            }
            source = trimAsciiWhitespace(source.substr(1, source.size() - 2));
        }
        if (!source.starts_with('`'))
        {
            result.error = MidiMapperMapParseError::UnknownMode;
            return result;
        }
        source.remove_prefix(1);
        std::string_view minimumText;
        std::string_view maximumText;
        std::string_view gridText;
        std::optional<std::uint8_t> channel;
        auto displayAs = MidiMapperMapDisplayAs::Default;
        auto taper = MidiMapperMapTaper::Linear;
        bool taperSeen = false;
        if (ranged)
        {
            const auto sourceEnd = source.find_first_of(" \t\r\n\f\v");
            if (sourceEnd == std::string_view::npos)
            {
                result.error = MidiMapperMapParseError::UnknownMode;
                return result;
            }
            auto options = trimAsciiWhitespace(source.substr(sourceEnd));
            source = source.substr(0, sourceEnd);
            // Any source takes a range and a display, as `Glide min: -0.01
            // max: 0.01 and `Bypass type: #gate; only a MIDI controller takes
            // a channel.
            const auto controller = source.starts_with("MIDIController");
            while (!options.empty())
            {
                const auto separator = options.find(';');
                auto group = trimAsciiWhitespace(
                    options.substr(0, separator));
                if (group.starts_with("channel:") && !channel && controller)
                {
                    group = trimAsciiWhitespace(group.substr(8));
                    unsigned parsedChannel = 0;
                    const auto parsed = std::from_chars(
                        group.data(), group.data() + group.size(),
                        parsedChannel);
                    if (parsed.ec != std::errc{}
                        || parsed.ptr != group.data() + group.size()
                        || parsedChannel < 1 || parsedChannel > 16)
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                    channel = static_cast<std::uint8_t>(parsedChannel);
                }
                else if (group.starts_with("min:")
                    && minimumText.empty())
                {
                    group = trimAsciiWhitespace(group.substr(4));
                    const auto minimumEnd = group.find_first_of(
                        " \t\r\n\f\v");
                    if (minimumEnd == std::string_view::npos)
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                    minimumText = group.substr(0, minimumEnd);
                    group = trimAsciiWhitespace(group.substr(minimumEnd));
                    if (!group.starts_with("max:"))
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                    group = trimAsciiWhitespace(group.substr(4));
                    const auto maximumEnd = group.find_first_of(
                        " \t\r\n\f\v");
                    maximumText = group.substr(0, maximumEnd);
                    if (maximumEnd != std::string_view::npos)
                    {
                        group = trimAsciiWhitespace(group.substr(maximumEnd));
                        if (!group.starts_with("grid:") || !gridText.empty())
                        {
                            result.error = MidiMapperMapParseError::UnknownMode;
                            return result;
                        }
                        gridText = trimAsciiWhitespace(group.substr(5));
                        if (gridText.empty())
                        {
                            result.error = MidiMapperMapParseError::UnknownMode;
                            return result;
                        }
                    }
                    double minimum = 0.0;
                    double maximum = 0.0;
                    if (!parseProjectMidiMapperMapBound(
                            minimumText, minimum)
                        || !parseProjectMidiMapperMapBound(
                            maximumText, maximum)
                        || !std::isfinite(maximum - minimum))
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                }
                else if (group.starts_with("grid:") && gridText.empty())
                {
                    gridText = trimAsciiWhitespace(group.substr(5));
                    if (gridText.empty())
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                }
                else if (group.starts_with("taper:") && !taperSeen)
                {
                    group = trimAsciiWhitespace(group.substr(6));
                    if (group == "#linear")
                        taper = MidiMapperMapTaper::Linear;
                    else if (group == "#log")
                        taper = MidiMapperMapTaper::Log;
                    else
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                    taperSeen = true;
                }
                // type: is the older spelling of displayAs:, as
                // in `Bypass type: #gate.
                else if ((group.starts_with("displayAs:") || group.starts_with("type:"))
                    && displayAs == MidiMapperMapDisplayAs::Default)
                {
                    group = trimAsciiWhitespace(group.substr(group.find(':') + 1));
                    // The widget is written with its # or without:
                    // displayAs: toggle, displayAs: #rotary.
                    if (group.starts_with('#'))
                        group.remove_prefix(1);
                    if (group == "fader")
                        displayAs = MidiMapperMapDisplayAs::Fader;
                    else if (group == "smallFader")
                        displayAs = MidiMapperMapDisplayAs::SmallFader;
                    else if (group == "toggle")
                        displayAs = MidiMapperMapDisplayAs::Toggle;
                    else if (group == "gate")
                        displayAs = MidiMapperMapDisplayAs::Gate;
                    else if (group == "nothing")
                        displayAs = MidiMapperMapDisplayAs::Nothing;
                    else if (group == "rotary")
                        displayAs = MidiMapperMapDisplayAs::Rotary;
                    else if (group == "pot")
                        displayAs = MidiMapperMapDisplayAs::Pot;
                    else
                    {
                        result.error = MidiMapperMapParseError::UnknownMode;
                        return result;
                    }
                }
                else
                {
                    result.error = MidiMapperMapParseError::UnknownMode;
                    return result;
                }
                if (separator == std::string_view::npos)
                    break;
                options = trimAsciiWhitespace(options.substr(separator + 1));
                if (options.empty())
                {
                    result.error = MidiMapperMapParseError::UnknownMode;
                    return result;
                }
            }
            if (!gridText.empty())
            {
                double minimum = 0.0;
                double maximum = 0.0;
                double step = 0.0;
                if (minimumText.empty()
                    || !parseProjectMidiMapperMapBound(minimumText, minimum)
                    || !parseProjectMidiMapperMapBound(maximumText, maximum)
                    || !parseProjectMidiMapperMapBound(gridText, step)
                    || !(maximum > minimum) || !(step > 0.0))
                {
                    result.error = MidiMapperMapParseError::UnknownMode;
                    return result;
                }
                const auto steps = (maximum - minimum) / step;
                if (!std::isfinite(steps) || steps < 1.0
                    || std::abs(steps - std::round(steps))
                        > 1.0e-9 * steps)
                {
                    result.error = MidiMapperMapParseError::UnknownMode;
                    return result;
                }
            }
            if (taper == MidiMapperMapTaper::Log
                && !minimumText.empty())
            {
                double minimum = 0.0;
                double maximum = 0.0;
                if (!parseProjectMidiMapperMapBound(minimumText, minimum)
                    || !parseProjectMidiMapperMapBound(maximumText, maximum)
                    || minimum < 0.0 || !(maximum > minimum))
                {
                    result.error = MidiMapperMapParseError::UnknownMode;
                    return result;
                }
            }
        }
        const auto target = line.substr(1, separator - 1);
        if (!isMapIdentifier(target) || !isMapIdentifier(source))
        {
            result.error = MidiMapperMapParseError::UnknownMode;
            return result;
        }
        for (std::size_t index = 0;
             index < result.localAssociationCount; ++index)
        {
            if (result.localAssociations[index].localEventValue == target)
            {
                result.error = MidiMapperMapParseError::DuplicateTarget;
                return result;
            }
        }
        if (result.localAssociationCount
            >= maximumMidiMapperMapAssociations)
        {
            result.error = MidiMapperMapParseError::TooManyAssociations;
            return result;
        }
        result.localAssociations[result.localAssociationCount++] = {
            target, source, minimumText, maximumText, gridText, channel,
            displayAs, taper, pitchBendSemitones, hidePitchBend};
    }
    return result;
}
}
