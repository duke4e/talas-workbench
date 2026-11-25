#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::component_bundle
{
inline constexpr std::string_view componentBundleManifestFormat =
    "talas.component-bundle";
inline constexpr std::uint32_t componentBundleManifestVersion = 1;
inline constexpr std::size_t maximumComponentBundleManifestBytes =
    256U * 1024U;
inline constexpr std::size_t maximumComponentBundleManifestJsonDepth = 16;
inline constexpr std::size_t maximumComponentBundleManifestJsonValues = 16'384;
inline constexpr std::size_t maximumComponentBundleManifestStringBytes =
    512U * 1024U;
inline constexpr std::size_t maximumComponentBundleManifestContainerElements =
    4096;
inline constexpr std::size_t maximumComponentBundleStableIdBytes = 240;
inline constexpr std::size_t maximumComponentBundlePathBytes = 240;
inline constexpr std::size_t maximumComponentBundleFiles = 256;
inline constexpr std::uint64_t maximumComponentBundleFileBytes =
    64U * 1024U * 1024U;
inline constexpr std::uint64_t maximumComponentBundlePayloadBytes =
    128U * 1024U * 1024U;
inline constexpr std::size_t maximumComponentBundleNodes = 64;
inline constexpr std::size_t maximumComponentBundleInputsPerNode = 128;
inline constexpr std::size_t maximumComponentBundleOutputsPerNode = 128;
inline constexpr std::size_t maximumComponentBundleParametersPerNode = 512;
inline constexpr std::size_t maximumComponentBundleChoicesPerParameter = 256;
inline constexpr std::size_t maximumComponentBundleDependencies = 64;
inline constexpr std::size_t maximumComponentBundleMigrationEdges = 128;
inline constexpr std::size_t maximumComponentBundleNotices = 32;
inline constexpr std::size_t maximumComponentBundleProvenanceTextBytes = 4096;

enum class ComponentBundleFilePurpose : std::uint8_t
{
    Resource,
    DeclarativeUi,
    License,
    MigrationData,
};

struct ComponentBundlePayloadRecord
{
    std::string path;
    std::uint64_t byteCount = 0;
    std::array<std::uint8_t, componentTrustDigestBytes> sha256{};

    bool operator==(const ComponentBundlePayloadRecord&) const = default;
};

struct ComponentBundleFileRecord : ComponentBundlePayloadRecord
{
    ComponentBundleFilePurpose purpose =
        ComponentBundleFilePurpose::Resource;

    bool operator==(const ComponentBundleFileRecord&) const = default;
};

struct ComponentBundlePort
{
    std::string id;
    talas::core::PortType type = talas::core::PortType::Audio;
    bool required = false;
    talas::core::PortSignalContract signal;

    bool operator==(const ComponentBundlePort& other) const
    {
        return id == other.id
            && type == other.type
            && required == other.required
            && signal.rate == other.signal.rate
            && signal.audioLayout == other.signal.audioLayout
            && signal.edge == other.signal.edge
            && signal.spectralFormat == other.signal.spectralFormat
            && signal.timelineBearing == other.signal.timelineBearing;
    }
};

struct ComponentBundleParameter
{
    std::string id;
    talas::core::ParameterKind kind = talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;

    bool operator==(const ComponentBundleParameter&) const = default;
};

struct ComponentBundleNode
{
    std::string typeId;
    std::uint32_t stateSchemaVersion = 1;
    std::vector<ComponentBundlePort> inputs;
    std::vector<ComponentBundlePort> outputs;
    std::vector<ComponentBundleParameter> parameters;

    bool operator==(const ComponentBundleNode&) const = default;
};

struct ComponentBundleDependency
{
    std::string bundleId;
    std::string minimumVersion;
    std::string maximumExclusiveVersion;
    bool optional = false;

    bool operator==(const ComponentBundleDependency&) const = default;
};

struct ComponentBundleMigrationEdge
{
    std::uint32_t fromSchemaVersion = 0;
    std::uint32_t toSchemaVersion = 0;

    bool operator==(const ComponentBundleMigrationEdge&) const = default;
};

struct ComponentBundleStateModel
{
    std::uint32_t currentSchemaVersion = 1;
    std::vector<ComponentBundleMigrationEdge> migrations;

    bool operator==(const ComponentBundleStateModel&) const = default;
};

struct ComponentBundleProvenance
{
    std::string publisher;
    std::string licenseExpression;
    std::vector<std::string> notices;

    bool operator==(const ComponentBundleProvenance&) const = default;
};

struct ComponentBundleManifest
{
    std::string bundleId;
    std::string semanticVersion;
    std::uint32_t minimumEngineAbiVersion = 0;
    std::uint32_t maximumEngineAbiVersion = 0;
    std::uint32_t minimumEngineApiVersion = 0;
    std::uint32_t maximumEngineApiVersion = 0;
    ComponentExecutionRoles declaredRoles = 0;
    ComponentExecutionPermissions requestedPermissions = 0;
    ComponentBundlePayloadRecord entry;
    std::vector<ComponentBundleFileRecord> files;
    std::vector<ComponentBundleNode> nodes;
    std::vector<ComponentBundleDependency> dependencies;
    ComponentBundleStateModel state;
    ComponentBundleProvenance provenance;
    std::optional<std::string> declarativeUiPath;

    bool operator==(const ComponentBundleManifest&) const = default;
};

enum class ComponentBundleManifestErrorCode : std::uint8_t
{
    JsonInvalid,
    BudgetExceeded,
    UnknownField,
    MissingField,
    TypeMismatch,
    InvalidInteger,
    InvalidIdentity,
    InvalidSemanticVersion,
    InvalidEnum,
    InvalidPath,
    InvalidDigest,
    DuplicateIdentity,
    ContradictoryDeclaration,
};

struct ComponentBundleManifestError
{
    ComponentBundleManifestErrorCode code =
        ComponentBundleManifestErrorCode::JsonInvalid;
    std::string path;
    std::string summary;

    bool operator==(const ComponentBundleManifestError&) const = default;
};

struct ComponentBundleManifestParseResult
{
    std::optional<ComponentBundleManifest> manifest;
    std::vector<ComponentBundleManifestError> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return manifest.has_value() && errors.empty();
    }
};

struct ComponentBundleManifestWriteResult
{
    std::string canonicalJson;
    std::vector<ComponentBundleManifestError> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

[[nodiscard]] ComponentBundleManifestParseResult parseComponentBundleManifest(
    std::string_view source);
[[nodiscard]] ComponentBundleManifestWriteResult
writeCanonicalComponentBundleManifest(const ComponentBundleManifest& manifest);
}
