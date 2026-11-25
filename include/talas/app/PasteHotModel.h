#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Edit > Paste hot: a list of some
// commonly used expressions, then every Event Value name of the project's
// global map, to paste into a field; typing part of a name jumps to it.
namespace talas::app
{
struct PasteHotEntry
{
    std::string label;
    std::string text;

    bool operator==(const PasteHotEntry&) const = default;
};

[[nodiscard]] std::vector<PasteHotEntry> pasteHotEntries(const talas::project::ProjectDocument& document);

// The entry typing jumps to: the first whose label or text starts with what
// was typed, else the first holding it, ignoring case.
[[nodiscard]] std::optional<std::size_t> pasteHotMatch(std::span<const PasteHotEntry> entries, std::string_view typed);
}
