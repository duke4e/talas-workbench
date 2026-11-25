#pragma once

#include "talas/core/NodeRegistry.h"

namespace talas::core
{
void registerBuiltinNodeTypes(NodeRegistry& registry);
[[nodiscard]] NodeRegistry makeBuiltinNodeRegistry();
}
