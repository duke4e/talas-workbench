#pragma once

#include "talas/core/NodeRegistry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
inline constexpr std::uint32_t projectPreservedComponentSchemaVersion = 1;
inline constexpr std::size_t maximumProjectPreservedComponentInputs = 128;
inline constexpr std::size_t maximumProjectPreservedComponentOutputs = 128;
inline constexpr std::size_t maximumProjectPreservedComponentParameters = 512;
inline constexpr std::size_t maximumProjectPreservedComponentStateBytes =
    16U * 1024U * 1024U;
inline constexpr std::size_t maximumProjectPreservedComponents = 4096;
inline constexpr std::size_t maximumProjectPreservedComponentTotalStateBytes =
    64U * 1024U * 1024U;
inline constexpr std::size_t maximumProjectPreservedComponentTextBytes = 4096;

struct ProjectPreservedPort
{
    std::string portId;
    talas::core::PortType type = talas::core::PortType::Audio;
    bool required = false;
    talas::core::PortSignalContract signal;

    bool operator==(const ProjectPreservedPort& other) const;
};

struct ProjectPreservedParameter
{
    std::string parameterId;
    talas::core::ParameterKind kind = talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;

    bool operator==(const ProjectPreservedParameter&) const = default;
};

struct ProjectPreservedComponent
{
    std::uint32_t schemaVersion = projectPreservedComponentSchemaVersion;
    std::string typeVersion;
    std::uint32_t stateSchemaVersion = 1;
    std::vector<std::uint8_t> state;
    std::string displayName;
    std::string bundleId;
    std::vector<ProjectPreservedPort> inputs;
    std::vector<ProjectPreservedPort> outputs;
    std::vector<ProjectPreservedParameter> parameters;
    std::string recoveryHint;

    bool operator==(const ProjectPreservedComponent&) const = default;
};

struct ProjectComponentStateDecodeResult
{
    enum class ErrorCode
    {
        None,
        InvalidBase64,
        SizeLimitExceeded,
    };

    std::vector<std::uint8_t> bytes;
    std::string error;
    ErrorCode errorCode = ErrorCode::None;

    [[nodiscard]] bool ok() const
    {
        return error.empty();
    }
};

struct ProjectPreservedComponentValidationError
{
    std::string path;
    std::string message;
};

enum class ProjectComponentAvailability
{
    Native,
    LegacyUnknown,
    Missing,
    IncompatibleVersion,
    IncompatibleStateSchema,
    IncompatibleInterface,
    Resolved
};

struct ProjectComponentAvailabilityResult
{
    ProjectComponentAvailability availability =
        ProjectComponentAvailability::LegacyUnknown;
    std::string message;

    [[nodiscard]] bool executable() const
    {
        return availability == ProjectComponentAvailability::Native
            || availability == ProjectComponentAvailability::Resolved;
    }
};

[[nodiscard]] std::string encodeProjectComponentState(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] ProjectComponentStateDecodeResult decodeProjectComponentState(
    std::string_view encoded);
[[nodiscard]] ProjectComponentStateDecodeResult decodeProjectComponentState(
    std::string_view encoded,
    std::size_t maximumDecodedBytes);
[[nodiscard]] std::optional<ProjectPreservedComponentValidationError>
validateProjectPreservedComponent(
    const ProjectPreservedComponent& component);
[[nodiscard]] talas::core::NodeTypeDescriptor
makeProjectPreservedComponentDescriptor(
    std::string typeId,
    const ProjectPreservedComponent& component);
[[nodiscard]] std::optional<ProjectPreservedComponent>
makeProjectPreservedComponent(
    const talas::core::NodeTypeDescriptor& descriptor);
[[nodiscard]] ProjectComponentAvailabilityResult
classifyProjectComponentAvailability(
    std::string_view typeId,
    const ProjectPreservedComponent* component,
    const talas::core::NodeRegistry& registry);
}
