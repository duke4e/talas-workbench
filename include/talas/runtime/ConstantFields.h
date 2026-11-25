#pragma once

#include <string_view>

namespace talas::runtime
{
// Whether a Sound's expression field is constant, set as the Sound starts
// and played at that value, or hot, following Event Values and time while
// it plays.
[[nodiscard]] bool isConstantField(std::string_view typeId, std::string_view parameterId) noexcept;
}
