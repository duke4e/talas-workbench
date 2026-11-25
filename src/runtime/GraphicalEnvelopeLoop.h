#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// A GraphicalEnvelope's loop, whose Envelope
// field marks the breakpoints its loop runs between: the word loopStart or
// loopEnd after a breakpoint's value, as in
//   {0 s} {0} {0.1 s} {1} loopStart {0.5 s} {0.6} loopEnd {1 s} {0}
// While Gate stays on the segments between the marks repeat, each repeat
// going on from where the last left the envelope, as a
// MultisegmentEnvelope's loop does. When Gate falls the envelope jumps to
// the loopEnd breakpoint (the left arrow) and plays on from there; a
// loopStart alone (a right arrow) is a release point the envelope jumps to
// when Gate falls. A loopEnd alone is illegal.
namespace talas::runtime::graphical_envelope_loop
{
struct Marks
{
    // The text without its marks, for the envelope's own parser.
    std::string text;
    bool looped = false;
    // A loopStart without a loopEnd: the release point at start.
    bool releasePoint = false;
    // The breakpoints the loop starts and ends at, counted from 0.
    std::size_t start = 0;
    std::size_t end = 0;
};

[[nodiscard]] inline bool isWordCharacter(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9');
}

// The marks of an Envelope text, or nothing when a mark repeats, comes
// before a breakpoint's value, or is a loopEnd without a loopStart before
// it.
[[nodiscard]] inline std::optional<Marks> stripLoopMarks(std::string_view text)
{
    Marks marks;
    std::size_t expressions = 0;
    std::optional<std::size_t> start;
    std::optional<std::size_t> end;
    std::size_t offset = 0;
    while (offset < text.size())
    {
        const auto character = text[offset];
        if (character == '{')
        {
            const auto close = text.find('}', offset + 1);
            if (close == std::string_view::npos)
            {
                marks.text.append(text.substr(offset));
                break;
            }
            marks.text.append(text.substr(offset, close + 1 - offset));
            ++expressions;
            offset = close + 1;
            continue;
        }
        if (isWordCharacter(character))
        {
            auto wordEnd = offset;
            while (wordEnd < text.size() && isWordCharacter(text[wordEnd]))
                ++wordEnd;
            const auto word = text.substr(offset, wordEnd - offset);
            if (word == "loopStart" || word == "loopEnd")
            {
                auto& slot = word == "loopStart" ? start : end;
                if (slot.has_value() || expressions < 2 || expressions % 2 != 0)
                    return std::nullopt;
                slot = expressions / 2 - 1;
                marks.text.push_back(' ');
            }
            else
            {
                // Anything else is the envelope parser's to refuse.
                marks.text.append(word);
            }
            offset = wordEnd;
            continue;
        }
        marks.text.push_back(character);
        ++offset;
    }
    if (end.has_value() && !start.has_value())
        return std::nullopt;
    if (start.has_value() && !end.has_value())
    {
        marks.releasePoint = true;
        marks.start = *start;
        return marks;
    }
    if (start.has_value())
    {
        if (*start >= *end)
            return std::nullopt;
        marks.looped = true;
        marks.start = *start;
        marks.end = *end;
    }
    return marks;
}
}
