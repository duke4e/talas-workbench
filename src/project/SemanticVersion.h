#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace talas::project
{
enum class SemanticVersionOrder : std::int8_t
{
    Invalid = -2,
    Less = -1,
    Equivalent = 0,
    Greater = 1,
};

[[nodiscard]] bool isCanonicalSemanticVersion(
    std::string_view version,
    std::size_t maximumBytes) noexcept;

[[nodiscard]] SemanticVersionOrder compareCanonicalSemanticVersionPrecedence(
    std::string_view left,
    std::string_view right,
    std::size_t maximumBytes) noexcept;
}
