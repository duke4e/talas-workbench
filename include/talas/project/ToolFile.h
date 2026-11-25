#pragma once

#include "talas/project/Tool.h"

#include <optional>
#include <string>
#include <string_view>

// A Tool's file: its global variables, its
// states with their local variables and triggered responses, and each
// variable's widget, as the Tool editor saves them and File > Open reads
// them back.
namespace talas::project::tool
{
inline constexpr std::string_view toolFileExtension = ".talastool";

[[nodiscard]] std::string writeToolFile(const Definition& tool);

struct ToolFileResult
{
    std::optional<Definition> tool;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return tool.has_value();
    }
};

[[nodiscard]] ToolFileResult readToolFile(std::string_view text);

// A new Tool: an InitialState with no variables or responses.
[[nodiscard]] Definition newTool();
}
