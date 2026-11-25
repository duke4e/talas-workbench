#pragma once

#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectMissingNodes.h"
#include "talas/runtime/AudioStream.h"
#include "talas/runtime/ComponentActivationProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace talas::runtime
{
inline constexpr std::size_t maximumIsolatedComponentAudioChannels = 2;
inline constexpr std::size_t maximumIsolatedComponentBlockFrames = 512;

enum class IsolatedComponentProcessResult : std::uint8_t
{
    FirstBlockSilence,
    Rendered,
    Quarantined,
};

enum class IsolatedComponentQuarantineReason : std::uint8_t
{
    Revoked,
    InvalidCallbackProfile,
    InvalidTransport,
    NonFiniteInput,
    NonFiniteOutput,
    ProcessRejected,
    SequenceExhausted,
};

struct IsolatedComponentTransport
{
    std::uint64_t generation = 0;
    std::int64_t sourceFrame = 0;
    bool reset = false;
};

struct IsolatedComponentImmutableResourceContract
{
    std::string parameterId;
    std::string resourceId;
    talas::core::ResourceSnapshotKind kind =
        talas::core::ResourceSnapshotKind::Table;
    std::uint64_t generation = 0;
    std::string sourceContentHash;
    std::array<std::uint8_t, componentTrustDigestBytes>
        mediatedPayloadDigest{};
    std::uint64_t elementCount = 0;
    std::uint64_t byteCount = 0;

    bool operator==(
        const IsolatedComponentImmutableResourceContract&) const = default;
};

struct IsolatedComponentAudioStreamContract
{
    std::string parameterId;
    std::string resourceId;
    std::uint64_t generation = 0;
    std::string sourceContentHash;
    std::uint64_t sourceByteCount = 0;
    std::uint64_t sourceFrameCount = 0;
    double maximumSourceFrameIncrement = 0.0;
    std::uint32_t sourceSampleRate = 0;
    std::uint32_t streamProfileVersion = 0;
    std::uint32_t sourceEncoding = 0;
    std::uint32_t maximumReadFrames = 0;
    std::uint32_t decodeBlockFrames = 0;
    std::uint16_t sourceChannelCount = 0;
    std::uint16_t maximumTouchedSourceBlocks = 0;
    std::uint16_t requestCapacity = 0;
    std::uint16_t responseCapacity = 0;
    std::uint16_t workerCacheCapacity = 0;
    std::uint16_t lowWatermarkBlocks = 0;
    std::uint16_t highWatermarkBlocks = 0;

    bool operator==(
        const IsolatedComponentAudioStreamContract&) const = default;
};

using IsolatedComponentResourceContract = std::variant<
    IsolatedComponentImmutableResourceContract,
    IsolatedComponentAudioStreamContract>;

struct IsolatedComponentResourceSetContract
{
    // The protocol grant and ordered member records are present for the
    // aggregate API-v4 profile. Legacy single-resource profiles are
    // normalized into one member without these aggregate records.
    std::optional<
        component_activation::ComponentActivationResourceSetGrantV1>
        grant;
    std::vector<
        component_activation::ComponentActivationResourceSetMemberV1>
        memberRecords;
    std::vector<IsolatedComponentResourceContract> members;
};

struct IsolatedComponentRuntimeContract
{
    std::string nodeId;
    std::string typeId;
    std::string typeVersion;
    std::string bundleId;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes>
        signerIdentityDigest{};
    ComponentExecutionPermissions requestedPermissions = 0;
    std::uint32_t policyVersion = 0;
    std::uint32_t stateSchemaVersion = 0;
    std::vector<talas::project::ProjectPreservedPort> inputs;
    std::vector<talas::project::ProjectPreservedPort> outputs;
    std::vector<talas::project::ProjectPreservedParameter> parameters;
    std::vector<component_activation::ComponentActivationStaticParameterV1>
        staticParameters;
    std::optional<IsolatedComponentResourceSetContract> resources;
    std::uint32_t sampleRate = 0;
    std::uint32_t blockFrames = 0;
    std::uint64_t activationGeneration = 0;
};

struct IsolatedComponentAudioStreamHealthSnapshot
{
    std::uint64_t seekGeneration = 0;
    AudioStreamHealthSnapshot health;
};

struct IsolatedComponentAudioStreamHealthSetSnapshot
{
    std::array<
        IsolatedComponentAudioStreamHealthSnapshot,
        component_activation::maximumComponentActivationResourceSetAudioStreamsV1>
        streams{};
    std::size_t streamCount = 0;
};

// Exclusive offline/control-thread participant. Destruction restores the
// captured processing boundary unless committed. Restore and commit must not
// allocate or throw. The original binding must outlive the checkpoint; no
// concurrent process caller is permitted. Participant-owned workers and public
// processing observations must be synchronized/deferred by the implementation.
// Authorization, revocation and quarantine are never part of processing undo.
class IsolatedComponentOfflineCheckpoint
{
public:
    virtual ~IsolatedComponentOfflineCheckpoint() = default;
    // False is terminal: the caller cannot undo peers that already committed.
    // A successful return still requires live binding publication authority.
    [[nodiscard]] virtual bool commit() noexcept = 0;
};

// Private prepared-graph seam. Product instances are authored only by the
// activation backend; tests may substitute a process-contained transport fake.
class PreparedIsolatedComponentBinding
{
public:
    virtual ~PreparedIsolatedComponentBinding() = default;

    [[nodiscard]] virtual const IsolatedComponentRuntimeContract&
    contract() const noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
    // Immutable preparation capability, independent of current active/revoked
    // state. Existing bindings retain their execution profile until their real
    // kernel, pipeline and resource participants implement this contract.
    [[nodiscard]] virtual bool supportsOfflineCheckpoint() const noexcept
    {
        return false;
    }
    // May allocate on the exclusive offline thread. A supporting binding must
    // capture every processing participant or return null/throw before any
    // processing occurs. Nested capture must reject without replacing baseline.
    [[nodiscard]] virtual std::unique_ptr<IsolatedComponentOfflineCheckpoint>
    beginOfflineCheckpoint()
    {
        return {};
    }
    // Control-thread, allocation-free projection of the authenticated shared
    // health plane. Stateless and static-resource bindings return no value.
    [[nodiscard]] virtual std::optional<
        IsolatedComponentAudioStreamHealthSetSnapshot> audioStreamHealthSet()
        noexcept
    {
        return std::nullopt;
    }
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    // Test-only control-thread observation for deterministic integration
    // sequencing. Production render code must never poll or wait on this seam.
    [[nodiscard]] virtual bool publishedAudioResponseReadyForTesting()
        const noexcept
    {
        return false;
    }
#endif
    // Control-thread admission. A successful begin linearizes publication
    // against owner/probe/runtime revocation until endStage() is called.
    [[nodiscard]] virtual bool tryBeginStage() noexcept = 0;
    virtual void endStage() noexcept = 0;
    [[nodiscard]] virtual IsolatedComponentProcessResult process(
        std::span<const float* const> inputChannels,
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        IsolatedComponentTransport transport) noexcept = 0;
    virtual void quarantine(
        IsolatedComponentQuarantineReason reason) noexcept = 0;
};
}
