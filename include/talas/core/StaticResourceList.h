#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::core
{
inline constexpr std::size_t maximumStaticResourceListTextBytes = 4'096;
inline constexpr std::size_t maximumStaticResourceListPositions = 256;

[[nodiscard]] std::optional<std::vector<std::string>>
parseBoundedStaticResourceList(std::string_view text);

// A bounded compile-time Smalltalk literal array selector for Resource fields.
// Ordinary resource identifiers bypass this parser.
[[nodiscard]] bool beginsStaticStringResourceSelection(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<std::string>
selectBoundedStaticStringResource(std::string_view text);

// A line's words, separated by whitespace, a word within single quotes
// holding spaces (a doubled quote within it one quote, as in Smalltalk): a
// DiskSplicer's segment. Nothing for a quote left
// open.
[[nodiscard]] std::optional<std::vector<std::string>>
splitQuotedWords(std::string_view line);
// A word as splitQuotedWords reads it back: within quotes when it holds
// whitespace or a quote, or is empty.
[[nodiscard]] std::string quotedWord(std::string_view word);
}
