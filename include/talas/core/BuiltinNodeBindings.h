#pragma once

#include <string_view>

namespace talas::core
{
[[nodiscard]] bool hasBuiltinOfflineLegacyBinding(
    std::string_view typeId) noexcept;
[[nodiscard]] bool hasBuiltinPreparedRealtimeLegacyBinding(
    std::string_view typeId) noexcept;
[[nodiscard]] bool hasBuiltinOfflineKernelBinding(
    std::string_view typeId) noexcept;
[[nodiscard]] bool hasBuiltinPreparedRealtimeKernelBinding(
    std::string_view typeId) noexcept;
}
