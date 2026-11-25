#pragma once

#include "talas/project/PreparedProjectVariables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace talas::runtime::detail
{
// The prepared scope table is shared by project variables and Event Values.
// Resolve the closest binding first so a child declaration shadows its parent.
template <typename Bindings>
[[nodiscard]] auto findNearestEventValue(
    Bindings& bindings,
    const talas::project::PreparedProjectVariableTable& scopes,
    std::uint32_t scopeId,
    std::string_view name) noexcept
{
    const auto maximumDepth = std::max<std::size_t>(
        1, scopes.scopes.size());
    for (std::size_t depth = 0; depth <= maximumDepth; ++depth)
    {
        const auto found = std::ranges::find_if(
            bindings,
            [&](const auto& binding) {
                return binding.scopeId == scopeId
                    && binding.name == name;
            });
        if (found != bindings.end() || scopeId == 0)
            return found;

        const auto scope = std::ranges::find(
            scopes.scopes, scopeId,
            &talas::project::PreparedProjectVariableScope::scopeOrdinal);
        if (scope == scopes.scopes.end()
            || scope->parentScopeOrdinal >= scopeId)
            scopeId = 0;
        else
            scopeId = scope->parentScopeOrdinal;
    }
    return bindings.end();
}
}
