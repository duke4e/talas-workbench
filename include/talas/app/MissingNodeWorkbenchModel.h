#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectMissingNodes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class MissingNodePortDirection
{
    Input,
    Output,
};

struct MissingNodePortRow
{
    std::string portId;
    MissingNodePortDirection direction =
        MissingNodePortDirection::Input;
    talas::core::PortType type = talas::core::PortType::Audio;
    std::string typeLabel;
    bool required = false;
    bool connected = false;
    std::string accessibleDescription;
};

struct MissingNodeWorkbenchProjection
{
    talas::project::ProjectComponentAvailability availability =
        talas::project::ProjectComponentAvailability::Missing;
    std::string qualifiedNodeId;
    std::string nodeId;
    std::string typeId;
    std::string displayName;
    std::string statusLabel;
    std::string typeVersionText;
    std::string stateText;
    std::string bundleText;
    std::string recoveryText;
    std::string diagnosticText;
    std::string accessibleName;
    std::string accessibleDescription;
    std::vector<MissingNodePortRow> ports;
    bool canRetry = false;
    bool canDisconnect = false;
    bool canDelete = true;
    bool parameterEditingEnabled = false;
};

enum class MissingNodeLayoutMode
{
    Compact,
    Regular,
};

enum class MissingNodeFocusTarget
{
    Retry,
    Disconnect,
    Delete,
    Diagnostic,
};

[[nodiscard]] std::optional<MissingNodeWorkbenchProjection>
missingNodeWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] MissingNodeLayoutMode missingNodeLayoutMode(
    int availableWidth) noexcept;

[[nodiscard]] std::vector<MissingNodeFocusTarget>
missingNodeKeyboardOrder(
    const MissingNodeWorkbenchProjection& projection);
}
