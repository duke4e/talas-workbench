#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace talas::cli
{
[[nodiscard]] int runCli(std::span<const std::string_view> args, std::ostream& output, std::ostream& error);
}
