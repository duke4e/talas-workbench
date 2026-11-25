#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Find and Replace in a text field: a search starts
// after the current selection and selects the next occurrence; Replace
// takes the next occurrence, Replace all every one. Offsets are bytes.
namespace talas::app
{
struct TextMatch
{
    std::size_t start = 0;
    std::size_t end = 0;

    bool operator==(const TextMatch&) const = default;
};

// The next occurrence of `needle` at or after `from`; nothing when there is
// none or the needle is empty.
[[nodiscard]] std::optional<TextMatch> findNextInText(std::string_view text, std::string_view needle, std::size_t from);

struct TextReplacement
{
    std::string text;
    std::size_t count = 0;
    // The replacement text where the one occurrence was replaced, to select.
    std::optional<TextMatch> replaced;
};

// The text with the next occurrence at or after `from`, or with all of them,
// replaced; a count of 0 when the needle is not found.
[[nodiscard]] TextReplacement replaceInText(
    std::string_view text, std::string_view needle, std::string_view replacement, std::size_t from, bool all);
}
