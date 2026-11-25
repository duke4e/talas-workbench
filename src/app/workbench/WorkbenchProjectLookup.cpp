#include "app/workbench/WorkbenchProjectLookup.h"

#include <ranges>

namespace talas::app::workbench
{
const talas::project::ProjectNode* findNode(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        document.graph.nodes,
        [&](const talas::project::ProjectNode& candidate) {
            return candidate.instanceId == nodeId;
        });
    return found == document.graph.nodes.end() ? nullptr : &*found;
}

const talas::project::ProjectParameterExpression* findParameter(
    const talas::project::ProjectNode& node,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        node.parameters,
        [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == parameterId;
        });
    return found == node.parameters.end() ? nullptr : &*found;
}

const talas::core::ParameterDescriptor* findDescriptorParameter(
    const talas::core::NodeTypeDescriptor& descriptor,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        descriptor.parameters,
        [&](const talas::core::ParameterDescriptor& parameter) {
            return parameter.id == parameterId;
        });
    return found == descriptor.parameters.end() ? nullptr : &*found;
}
}
