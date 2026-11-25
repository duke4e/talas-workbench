#pragma once

#include "talas/runtime/ComponentActivationAudioDataPlane.h"
#include "talas/runtime/IsolatedComponentRuntime.h"
#include "runtime/component_activation/ComponentActivationAuthorization.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace talas::runtime::component_activation
{
enum class ComponentActivationMacOSEndpointV1 : std::uint8_t
{
    Production,
    FixtureMalformed,
    FixtureHang,
    FixtureSessionMismatch,
    FixtureHeartbeatHang,
    FixtureMissing,
    FixtureTerminationFailure,
    FixtureProcessIdentityMismatch,
};

enum class ComponentActivationMacOSProfileV1 : std::uint8_t
{
    StatelessAudio,
    ImmutableResource,
    BoundedAudioStream,
    MultipleImmutableResources,
    MultipleImmutableOnlyResources,
};

enum class ComponentActivationMacOSHostStateV1 : std::uint8_t
{
    Prepared,
    Sealing,
    Ready,
    Loading,
    Canary,
    Resetting,
    PreparedActivation,
    TearingDown,
    Quarantined,
    Closed,
    Checkpointing,
};

enum class ComponentActivationMacOSHostDiagnosticV1 : std::uint8_t
{
    Ready,
    InvalidConfiguration,
    SharedStorageUnavailable,
    ControlQueueUnavailable,
    LaunchRejected,
    ProtocolRejected,
    DeadlineExpired,
    RuntimeEvidenceUnavailable,
    AuthorizationRejected,
    ActivationRejected,
    Revoked,
    ProcessTerminationFailed,
    Closed,
};

enum class ComponentActivationMacOSHostOperationV1 : std::uint8_t
{
    Scheduled,
    InvalidState,
    Rejected,
};

class ComponentActivationMacOSHost;

struct ComponentActivationMacOSHostPreparationV1
{
    std::unique_ptr<ComponentActivationMacOSHost> host;
    ComponentActivationMacOSHostDiagnosticV1 diagnostic =
        ComponentActivationMacOSHostDiagnosticV1::InvalidConfiguration;

    [[nodiscard]] bool ok() const noexcept;
};

class ComponentActivationMacOSHost
{
public:
    ~ComponentActivationMacOSHost();

    ComponentActivationMacOSHost(const ComponentActivationMacOSHost&) = delete;
    ComponentActivationMacOSHost& operator=(
        const ComponentActivationMacOSHost&) = delete;

    [[nodiscard]] static ComponentActivationMacOSHostPreparationV1 prepare(
        ComponentActivationMacOSEndpointV1 endpoint =
            ComponentActivationMacOSEndpointV1::Production,
        ComponentActivationMacOSProfileV1 profile =
            ComponentActivationMacOSProfileV1::StatelessAudio) noexcept;

    [[nodiscard]] ComponentActivationMacOSHostOperationV1 start() noexcept;
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 activate(
        ComponentActivationLeaseV1 lease,
        const component_bundle::ComponentBundleSnapshot& snapshot,
        std::span<const ComponentActivationStaticParameterV1>
            staticParameters) noexcept;
    // Exclusive control operations. Exclude process/stage calls from scheduling
    // until checkpointStatus() is no longer Pending. Health inspection may run
    // concurrently and retains a complete public snapshot while exposing live
    // failure. Prepared binding transfer separately queries complete participant
    // support through authenticated control.
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 queryOfflineCheckpoint() noexcept;
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 beginOfflineCheckpoint() noexcept;
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 restoreOfflineCheckpoint() noexcept;
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 commitOfflineCheckpoint() noexcept;
    [[nodiscard]] ComponentActivationCheckpointStatusV1 checkpointStatus() const noexcept;
    void shutdown() noexcept;

    [[nodiscard]] ComponentActivationMacOSHostStateV1 state() const noexcept;
    [[nodiscard]] ComponentActivationMacOSHostDiagnosticV1 diagnostic()
        const noexcept;
    [[nodiscard]] ComponentActivationSealedSessionV1 sealedSession()
        const noexcept;
    [[nodiscard]] ComponentActivationRuntimeEvidenceV1 runtimeEvidence()
        const noexcept;
    [[nodiscard]] std::uint64_t heartbeatCount() const noexcept;
    [[nodiscard]] std::int32_t peerProcessIdentifier() const noexcept;
    [[nodiscard]] bool sealedStorageContainsNoComponentBytes() const noexcept;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    // Test-only control-thread observation; production callbacks remain
    // non-waiting and fail closed on a missing N-to-N+1 response.
    [[nodiscard]] bool publishedAudioResponseReadyForTesting() const noexcept;
    // Test-only peer fault injection used to exercise helper finalization
    // while the decoded-Audio worker is still active.
    void cancelConnectionForTesting() noexcept;
    // Model an authorized peer sending a replay or wrong held checkpoint ID.
    void corruptNextCheckpointRequestForTesting(std::uint32_t mode) noexcept;
    // Send a real Ready heartbeat on the same queue turn that accepts activation.
    void overlapNextActivationHeartbeatForTesting(bool holdReply = false) noexcept;
    [[nodiscard]] bool activationHeartbeatHeldForTesting() const noexcept;
    void releaseActivationHeartbeatForTesting() noexcept;
    [[nodiscard]] std::uint64_t activationMessageCountForTesting() const noexcept;
    [[nodiscard]] std::optional<IsolatedComponentAudioStreamHealthSetSnapshot>
    audioStreamHealthSetForTesting() noexcept;
    void holdNextCheckpointReplyForTesting() noexcept;
    [[nodiscard]] bool checkpointReplyHeldForTesting() const noexcept;
    void releaseCheckpointReplyForTesting() noexcept;
    void holdNextResourceSetHealthReadForTesting() noexcept;
    [[nodiscard]] bool resourceSetHealthReadHeldForTesting() const noexcept;
    void releaseResourceSetHealthReadForTesting() noexcept;
#endif
    [[nodiscard]] ComponentActivationAudioOperationV1 processAudio(
        std::span<const float* const> inputChannels,
        std::span<float* const> outputChannels,
        std::uint32_t frameCount,
        ComponentActivationAudioTransportV1 transport) noexcept;
    [[nodiscard]] std::shared_ptr<PreparedIsolatedComponentBinding>
    takePreparedIsolatedComponentBinding(
        std::string nodeId,
        const component_bundle::ComponentBundleSnapshot& snapshot,
        std::span<const ComponentActivationStaticParameterV1>
            staticParameters) && noexcept;

private:
    [[nodiscard]] ComponentActivationMacOSHostOperationV1 requestCheckpoint(
        ComponentActivationCheckpointOperationV1 operation) noexcept;
    struct Impl;
    struct PreparedBinding;
    explicit ComponentActivationMacOSHost(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> impl_;
};
}
