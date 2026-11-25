#include "talas/project/ProjectMissingNodes.h"

#include "SemanticVersion.h"

#include <limits>
#include <unordered_set>

namespace talas::project
{
namespace
{
constexpr std::string_view base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int base64Value(char value)
{
    const auto found = base64Alphabet.find(value);
    return found == std::string_view::npos
        ? -1
        : static_cast<int>(found);
}

[[nodiscard]] bool validBoundedText(
    std::string_view value,
    bool allowEmpty)
{
    return (allowEmpty || !value.empty())
        && value.size() <= maximumProjectPreservedComponentTextBytes;
}

template <typename Value, typename Projection>
[[nodiscard]] bool hasUniqueNonEmptyBoundedIds(
    const std::vector<Value>& values,
    Projection projection)
{
    std::unordered_set<std::string_view> identifiers;
    for (const auto& value : values)
    {
        const std::string_view identifier = projection(value);
        if (!validBoundedText(identifier, false)
            || !identifiers.insert(identifier).second)
        {
            return false;
        }
    }
    return true;
}
}

bool ProjectPreservedPort::operator==(
    const ProjectPreservedPort& other) const
{
    return portId == other.portId
        && type == other.type
        && required == other.required
        && signal.rate == other.signal.rate
        && signal.audioLayout == other.signal.audioLayout
        && signal.edge == other.signal.edge
        && signal.spectralFormat == other.signal.spectralFormat
        && signal.timelineBearing == other.signal.timelineBearing;
}

std::string encodeProjectComponentState(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return {};
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U)
    {
        const auto remaining = bytes.size() - offset;
        const std::uint32_t first = bytes[offset];
        const std::uint32_t second = remaining > 1 ? bytes[offset + 1] : 0;
        const std::uint32_t third = remaining > 2 ? bytes[offset + 2] : 0;
        const auto packed = (first << 16U) | (second << 8U) | third;
        encoded.push_back(base64Alphabet[(packed >> 18U) & 0x3fU]);
        encoded.push_back(base64Alphabet[(packed >> 12U) & 0x3fU]);
        encoded.push_back(
            remaining > 1
                ? base64Alphabet[(packed >> 6U) & 0x3fU]
                : '=');
        encoded.push_back(
            remaining > 2 ? base64Alphabet[packed & 0x3fU] : '=');
    }
    return encoded;
}

ProjectComponentStateDecodeResult decodeProjectComponentState(
    std::string_view encoded)
{
    return decodeProjectComponentState(
        encoded,
        maximumProjectPreservedComponentStateBytes);
}

ProjectComponentStateDecodeResult decodeProjectComponentState(
    std::string_view encoded,
    std::size_t maximumDecodedBytes)
{
    ProjectComponentStateDecodeResult result;
    if (encoded.empty())
        return result;
    if (encoded.size() % 4U != 0)
    {
        result.error = "Expected canonical padded base64";
        result.errorCode =
            ProjectComponentStateDecodeResult::ErrorCode::InvalidBase64;
        return result;
    }
    const auto quartetCount = encoded.size() / 4U;
    if (quartetCount
        > (maximumProjectPreservedComponentStateBytes + 2U) / 3U)
    {
        result.error = "Decoded component state exceeds the supported limit";
        result.errorCode =
            ProjectComponentStateDecodeResult::ErrorCode::SizeLimitExceeded;
        return result;
    }
    std::size_t decodedSize = quartetCount * 3U;
    for (std::size_t quartet = 0; quartet < quartetCount; ++quartet)
    {
        const auto offset = quartet * 4U;
        const auto finalQuartet = quartet + 1U == quartetCount;
        const auto first = base64Value(encoded[offset]);
        const auto second = base64Value(encoded[offset + 1U]);
        const auto thirdPadding = encoded[offset + 2U] == '=';
        const auto fourthPadding = encoded[offset + 3U] == '=';
        const auto third = thirdPadding
            ? 0
            : base64Value(encoded[offset + 2U]);
        const auto fourth = fourthPadding
            ? 0
            : base64Value(encoded[offset + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0
            || (!finalQuartet && (thirdPadding || fourthPadding))
            || (thirdPadding && !fourthPadding)
            || (thirdPadding && (second & 0x0f) != 0)
            || (fourthPadding && !thirdPadding && (third & 0x03) != 0))
        {
            result.bytes.clear();
            result.error = "Expected canonical padded base64";
            result.errorCode =
                ProjectComponentStateDecodeResult::ErrorCode::InvalidBase64;
            return result;
        }
        if (finalQuartet)
        {
            decodedSize -= static_cast<std::size_t>(thirdPadding)
                + static_cast<std::size_t>(fourthPadding);
        }
    }
    const auto effectiveLimit = std::min(
        maximumDecodedBytes,
        maximumProjectPreservedComponentStateBytes);
    if (decodedSize > effectiveLimit)
    {
        result.error = "Decoded component state exceeds the supported limit";
        result.errorCode =
            ProjectComponentStateDecodeResult::ErrorCode::SizeLimitExceeded;
        return result;
    }

    result.bytes.reserve(decodedSize);
    for (std::size_t quartet = 0; quartet < quartetCount; ++quartet)
    {
        const auto offset = quartet * 4U;
        const auto first = base64Value(encoded[offset]);
        const auto second = base64Value(encoded[offset + 1U]);
        const auto thirdPadding = encoded[offset + 2U] == '=';
        const auto fourthPadding = encoded[offset + 3U] == '=';
        const auto third = thirdPadding
            ? 0
            : base64Value(encoded[offset + 2U]);
        const auto fourth = fourthPadding
            ? 0
            : base64Value(encoded[offset + 3U]);
        const auto packed =
            (static_cast<std::uint32_t>(first) << 18U)
            | (static_cast<std::uint32_t>(second) << 12U)
            | (static_cast<std::uint32_t>(third) << 6U)
            | static_cast<std::uint32_t>(fourth);
        result.bytes.push_back(
            static_cast<std::uint8_t>((packed >> 16U) & 0xffU));
        if (!thirdPadding)
            result.bytes.push_back(
                static_cast<std::uint8_t>((packed >> 8U) & 0xffU));
        if (!fourthPadding)
            result.bytes.push_back(
                static_cast<std::uint8_t>(packed & 0xffU));
    }
    return result;
}

std::optional<ProjectPreservedComponentValidationError>
validateProjectPreservedComponent(
    const ProjectPreservedComponent& component)
{
    if (component.schemaVersion
        != projectPreservedComponentSchemaVersion)
    {
        return {{".schemaVersion", "Unsupported preserved-component schema version"}};
    }
    if (!isCanonicalSemanticVersion(
            component.typeVersion,
            maximumProjectPreservedComponentTextBytes))
        return {{".typeVersion", "Expected a semantic component type version"}};
    if (component.stateSchemaVersion == 0)
        return {{".stateSchemaVersion", "Component state schema version must be positive"}};
    if (component.state.size()
        > maximumProjectPreservedComponentStateBytes)
    {
        return {{".state", "Component state exceeds the supported limit"}};
    }
    if (!validBoundedText(component.displayName, false))
        return {{".displayName", "Component display name must be non-empty and bounded"}};
    if (!validBoundedText(component.bundleId, true))
        return {{".bundleId", "Component bundle id exceeds the supported limit"}};
    if (!validBoundedText(component.recoveryHint, true))
        return {{".recoveryHint", "Component recovery hint exceeds the supported limit"}};
    if (component.inputs.size()
            > maximumProjectPreservedComponentInputs
        || component.outputs.size()
            > maximumProjectPreservedComponentOutputs
        || component.parameters.size()
            > maximumProjectPreservedComponentParameters)
    {
        return {{"", "Frozen component interface exceeds the supported limit"}};
    }
    if (!hasUniqueNonEmptyBoundedIds(
            component.inputs,
            [](const auto& port) -> std::string_view {
                return port.portId;
            }))
    {
        return {{".inputs", "Frozen component input ids must be unique and non-empty"}};
    }
    if (!hasUniqueNonEmptyBoundedIds(
            component.outputs,
            [](const auto& port) -> std::string_view {
                return port.portId;
            }))
    {
        return {{".outputs", "Frozen component output ids must be unique and non-empty"}};
    }
    if (!hasUniqueNonEmptyBoundedIds(
            component.parameters,
            [](const auto& parameter) -> std::string_view {
                return parameter.parameterId;
            }))
    {
        return {{".parameters", "Frozen component parameter ids must be unique and non-empty"}};
    }
    for (const auto& parameter : component.parameters)
    {
        if (parameter.kind != talas::core::ParameterKind::Choice
            && !parameter.choices.empty())
        {
            return {{".parameters", "Only Choice parameters may preserve choices"}};
        }
        if (parameter.kind == talas::core::ParameterKind::Choice
            && parameter.choices.empty())
        {
            return {{".parameters", "Choice parameters require at least one frozen choice"}};
        }
        if (!hasUniqueNonEmptyBoundedIds(
                parameter.choices,
                [](const std::string& choice) -> std::string_view {
                    return choice;
                }))
        {
            return {{".parameters", "Frozen component choices must be unique and non-empty"}};
        }
    }
    return std::nullopt;
}

talas::core::NodeTypeDescriptor makeProjectPreservedComponentDescriptor(
    std::string typeId,
    const ProjectPreservedComponent& component)
{
    talas::core::NodeTypeDescriptor descriptor;
    descriptor.typeId = std::move(typeId);
    descriptor.displayName = component.displayName;
    descriptor.category = "Preserved components";
    descriptor.inputs.reserve(component.inputs.size());
    descriptor.outputs.reserve(component.outputs.size());
    descriptor.parameters.reserve(component.parameters.size());
    const auto appendPorts = [](
                                 const std::vector<ProjectPreservedPort>& source,
                                 std::vector<talas::core::PortDescriptor>& destination) {
        for (const auto& port : source)
        {
            destination.emplace_back(
                port.portId,
                port.type,
                port.required,
                port.signal);
        }
    };
    appendPorts(component.inputs, descriptor.inputs);
    appendPorts(component.outputs, descriptor.outputs);
    for (const auto& preserved : component.parameters)
    {
        talas::core::ParameterDescriptor parameter;
        parameter.id = preserved.parameterId;
        parameter.kind = preserved.kind;
        parameter.domain = preserved.domain;
        parameter.choices = preserved.choices;
        switch (preserved.kind)
        {
        case talas::core::ParameterKind::Expression:
            parameter.defaultExpression = "0";
            break;
        case talas::core::ParameterKind::Choice:
            if (!preserved.choices.empty())
                parameter.defaultChoice = preserved.choices.front();
            break;
        case talas::core::ParameterKind::Boolean:
            parameter.defaultBoolean = false;
            break;
        case talas::core::ParameterKind::Resource:
            parameter.defaultResource.clear();
            break;
        case talas::core::ParameterKind::Text:
            parameter.defaultText.clear();
            break;
        }
        descriptor.parameters.push_back(std::move(parameter));
    }
    descriptor.supportLevel = talas::core::NodeSupportLevel::Placeholder;
    descriptor.supportSummary =
        "Frozen non-executable interface for preserved project data.";
    descriptor.provenance.vendor = component.bundleId.empty()
        ? "Unknown component vendor"
        : component.bundleId;
    descriptor.provenance.origin =
        talas::core::NodeOrigin::PreservedComponent;
    descriptor.typeVersion = component.typeVersion;
    descriptor.stateSchemaVersion = component.stateSchemaVersion;
    return descriptor;
}

std::optional<ProjectPreservedComponent> makeProjectPreservedComponent(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    ProjectPreservedComponent component;
    component.typeVersion = descriptor.typeVersion;
    component.stateSchemaVersion = descriptor.stateSchemaVersion;
    component.displayName = descriptor.displayName;
    component.bundleId = descriptor.provenance.vendor;
    component.recoveryHint =
        "Install the matching component version to restore execution";
    component.inputs.reserve(descriptor.inputs.size());
    component.outputs.reserve(descriptor.outputs.size());
    component.parameters.reserve(descriptor.parameters.size());
    const auto appendPorts = [](
                                 const std::vector<talas::core::PortDescriptor>& source,
                                 std::vector<ProjectPreservedPort>& destination) {
        for (const auto& port : source)
        {
            destination.push_back({
                port.id,
                port.type,
                port.required,
                port.signal,
            });
        }
    };
    appendPorts(descriptor.inputs, component.inputs);
    appendPorts(descriptor.outputs, component.outputs);
    for (const auto& parameter : descriptor.parameters)
    {
        component.parameters.push_back({
            parameter.id,
            parameter.kind,
            parameter.domain,
            parameter.choices,
        });
    }
    if (validateProjectPreservedComponent(component).has_value())
        return std::nullopt;
    return component;
}

ProjectComponentAvailabilityResult
classifyProjectComponentAvailability(
    std::string_view typeId,
    const ProjectPreservedComponent* component,
    const talas::core::NodeRegistry& registry)
{
    const auto* descriptor = registry.findNodeType(typeId);
    if (component == nullptr)
    {
        if (descriptor != nullptr)
        {
            return {
                ProjectComponentAvailability::Native,
                "Registered node type is available: " + std::string(typeId),
            };
        }
        return {
            ProjectComponentAvailability::LegacyUnknown,
            "Unknown node type has no preservation capsule: "
                + std::string(typeId),
        };
    }
    if (const auto invalid =
            validateProjectPreservedComponent(*component);
        invalid.has_value())
    {
        return {
            ProjectComponentAvailability::IncompatibleInterface,
            "Invalid preserved component contract"
                + invalid->path + ": " + invalid->message,
        };
    }
    if (descriptor == nullptr)
    {
        return {
            ProjectComponentAvailability::Missing,
            "Missing component type: " + std::string(typeId)
                + " @ " + component->typeVersion,
        };
    }
    if (descriptor->typeVersion != component->typeVersion)
    {
        return {
            ProjectComponentAvailability::IncompatibleVersion,
            "Component type version mismatch for " + std::string(typeId)
                + ": project requires " + component->typeVersion
                + ", registry provides " + descriptor->typeVersion,
        };
    }
    if (descriptor->stateSchemaVersion
        != component->stateSchemaVersion)
    {
        return {
            ProjectComponentAvailability::IncompatibleStateSchema,
            "Component state schema mismatch for " + std::string(typeId)
                + ": project requires "
                + std::to_string(component->stateSchemaVersion)
                + ", registry provides "
                + std::to_string(descriptor->stateSchemaVersion),
        };
    }

    const auto portsMatch = [](
                                const std::vector<ProjectPreservedPort>& preserved,
                                const std::vector<talas::core::PortDescriptor>& actual) {
        if (preserved.size() != actual.size())
            return false;
        for (std::size_t index = 0; index < preserved.size(); ++index)
        {
            const auto& expected = preserved[index];
            const auto& candidate = actual[index];
            if (expected.portId != candidate.id
                || expected.type != candidate.type
                || expected.required != candidate.required
                || expected.signal.rate != candidate.signal.rate
                || expected.signal.audioLayout
                    != candidate.signal.audioLayout
                || expected.signal.edge != candidate.signal.edge
                || expected.signal.spectralFormat
                    != candidate.signal.spectralFormat
                || expected.signal.timelineBearing
                    != candidate.signal.timelineBearing)
            {
                return false;
            }
        }
        return true;
    };
    if (!portsMatch(component->inputs, descriptor->inputs))
    {
        return {
            ProjectComponentAvailability::IncompatibleInterface,
            "Preserved component input interface mismatch for "
                + std::string(typeId),
        };
    }
    if (!portsMatch(component->outputs, descriptor->outputs))
    {
        return {
            ProjectComponentAvailability::IncompatibleInterface,
            "Preserved component output interface mismatch for "
                + std::string(typeId),
        };
    }
    if (component->parameters.size() != descriptor->parameters.size())
    {
        return {
            ProjectComponentAvailability::IncompatibleInterface,
            "Preserved component parameter interface mismatch for "
                + std::string(typeId),
        };
    }
    for (std::size_t index = 0;
         index < component->parameters.size();
         ++index)
    {
        const auto& expected = component->parameters[index];
        const auto& candidate = descriptor->parameters[index];
        if (expected.parameterId != candidate.id
            || expected.kind != candidate.kind
            || expected.domain != candidate.domain
            || expected.choices != candidate.choices)
        {
            return {
                ProjectComponentAvailability::IncompatibleInterface,
                "Preserved component parameter interface mismatch for "
                    + std::string(typeId),
            };
        }
    }
    return {
        ProjectComponentAvailability::Resolved,
        "Preserved component exactly matches the registered descriptor: "
            + std::string(typeId) + " @ " + component->typeVersion,
    };
}
}
