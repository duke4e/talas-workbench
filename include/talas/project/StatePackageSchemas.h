#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct PresetPackageState
{
    std::uint32_t schemaVersion = 1;

    bool operator==(const PresetPackageState&) const = default;
};

struct DistributionPackageState
{
    std::uint32_t schemaVersion = 1;

    bool operator==(const DistributionPackageState&) const = default;
};

inline constexpr std::uint32_t currentHostStateSchemaVersion = 2;
inline constexpr std::size_t maximumHostAutomationReferenceCount = 4096;
inline constexpr std::size_t maximumHostAutomationIdBytes = 256;
inline constexpr std::size_t maximumHostTargetIdentityBytes = 4096;
inline constexpr std::size_t maximumHostExternalProjectUriBytes = 4096;
inline constexpr std::size_t maximumHostEmbeddedProjectBytes =
    12 * 1024 * 1024;
inline constexpr std::size_t maximumHostStateBytes =
    16 * 1024 * 1024;

enum class HostAutomationDisposition
{
    Active,
    Tombstone,
};

struct HostAutomationReference
{
    std::string automationId;
    std::string targetIdentity;
    HostAutomationDisposition disposition =
        HostAutomationDisposition::Active;
    std::optional<std::string> replacementAutomationId;

    bool operator==(const HostAutomationReference&) const = default;
};

enum class HostProjectStorageMode
{
    Embedded,
    Linked,
};

struct HostProjectStorage
{
    HostProjectStorageMode mode = HostProjectStorageMode::Embedded;
    std::string projectPayload;
    std::string externalProjectUri;
    std::string expectedContentHash;

    bool operator==(const HostProjectStorage&) const = default;
};

struct HostState
{
    std::uint32_t schemaVersion = currentHostStateSchemaVersion;
    std::vector<HostAutomationReference> automationReferences;
    std::optional<HostProjectStorage> projectStorage;
    bool structuralDirty = false;

    bool operator==(const HostState&) const = default;
};

struct StateEnvelopeDiagnostic
{
    std::string message;
    std::string path;

    bool operator==(const StateEnvelopeDiagnostic&) const = default;
};

template <typename State>
struct StateEnvelopeLoadResult
{
    State state;
    std::vector<StateEnvelopeDiagnostic> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

using PresetPackageStateLoadResult =
    StateEnvelopeLoadResult<PresetPackageState>;
using DistributionPackageStateLoadResult =
    StateEnvelopeLoadResult<DistributionPackageState>;
using HostStateLoadResult = StateEnvelopeLoadResult<HostState>;

struct HostStateSerializationResult
{
    std::string source;
    std::vector<StateEnvelopeDiagnostic> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return errors.empty();
    }
};

enum class HostAutomationResolutionKind
{
    Resolved,
    Migrated,
    Tombstone,
    MissingReference,
    MissingTarget,
    InvalidState,
};

struct HostAutomationResolution
{
    HostAutomationResolutionKind kind =
        HostAutomationResolutionKind::MissingReference;
    std::string automationId;
    std::string resolvedAutomationId;
    std::string targetIdentity;
    std::vector<StateEnvelopeDiagnostic> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return kind == HostAutomationResolutionKind::Resolved
            || kind == HostAutomationResolutionKind::Migrated;
    }
};

[[nodiscard]] std::string serializePresetPackageState(
    const PresetPackageState& state);

[[nodiscard]] PresetPackageStateLoadResult
parsePresetPackageState(std::string_view source);

[[nodiscard]] std::string serializeDistributionPackageState(
    const DistributionPackageState& state);

[[nodiscard]] DistributionPackageStateLoadResult
parseDistributionPackageState(std::string_view source);

[[nodiscard]] HostStateSerializationResult serializeHostState(
    const HostState& state);

[[nodiscard]] HostStateLoadResult parseHostState(
    std::string_view source);

[[nodiscard]] HostStateLoadResult reconcileHostAutomationReferences(
    const HostState& state,
    std::span<const std::string> availableTargetIdentities);

[[nodiscard]] HostAutomationResolution resolveHostAutomationReference(
    const HostState& state,
    std::string_view automationId,
    std::span<const std::string> availableTargetIdentities);
}
