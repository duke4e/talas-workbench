#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace talas::cli
{
// ADR-0029: talas_cli export-plugin. Returns 0 on success, 1 when the export
// fails and 2 for a usage error.
[[nodiscard]] int runExportPluginCommand(
    std::span<const std::string_view> args,
    std::ostream& output,
    std::ostream& error);
}
