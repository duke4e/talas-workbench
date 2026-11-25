#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <string_view>

namespace talas::app::workbench
{
[[nodiscard]] const talas::project::ProjectNode* findNode(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId);

[[nodiscard]] const talas::project::ProjectParameterExpression* findParameter(
    const talas::project::ProjectNode& node,
    std::string_view parameterId);

[[nodiscard]] const talas::core::ParameterDescriptor* findDescriptorParameter(
    const talas::core::NodeTypeDescriptor& descriptor,
    std::string_view parameterId);
}
