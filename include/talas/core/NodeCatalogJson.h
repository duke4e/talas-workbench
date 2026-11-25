#pragma once

#include "talas/core/NodeRegistry.h"

#include <string>

namespace talas::core
{
[[nodiscard]] std::string serializeNodeCatalog(const NodeRegistry& registry);
}
