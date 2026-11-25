#pragma once

#include "talas/core/NodeRegistry.h"

#include <string>
#include <vector>

namespace talas::core
{
struct NodeInstance
{
    std::string instanceId;
    std::string typeId;
};

struct Connection
{
    std::string fromNode;
    std::string fromPort;
    std::string toNode;
    std::string toPort;

    bool operator==(const Connection&) const = default;
};

enum class ValidationCode
{
    UnknownNodeType,
    InvalidNodeIdentity,
    UnknownSoundDefinition,
    UnknownNodeInstance,
    UnknownPort,
    MissingRequiredInput,
    PortTypeMismatch,
    DuplicateInputConnection,
    DuplicateNodeInstance,
    CycleDetected,
    InvalidSoundBoundary,
    RecursiveSoundDefinition,
    SoundExpansionLimitExceeded,
    InvalidSampleMap,
    InvalidProjectAsset,
    DuplicateProjectAsset,
    InvalidProjectVariable,
    InvalidParameterTransformation,
    InvalidLiveSet,
};

struct ValidationError
{
    ValidationCode code;
    std::string message;
    std::string nodeId;
};

struct ValidationResult
{
    std::vector<ValidationError> errors;

    [[nodiscard]] bool ok() const
    {
        return errors.empty();
    }
};

class Graph
{
public:
    void addNode(NodeInstance node);
    void connect(Connection connection);

    [[nodiscard]] const std::vector<NodeInstance>& nodes() const;
    [[nodiscard]] const std::vector<Connection>& connections() const;
    [[nodiscard]] ValidationResult validate(const NodeRegistry& registry) const;

private:
    std::vector<NodeInstance> nodes_;
    std::vector<Connection> connections_;
};
}
