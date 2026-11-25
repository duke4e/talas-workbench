#pragma once

#include "runtime/component_bundle/ComponentBundleInspection.h"
#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstdint>
#include <memory>

namespace talas::runtime::component_probe
{
enum class ComponentBundleProbeMacOSCoordinatorOperationV1 : std::uint8_t
{
    Scheduled,
    InvalidState,
    Unavailable,
};

enum class ComponentBundleProbeMacOSCoordinatorStateV1 : std::uint8_t
{
    Prepared,
    Running,
    Finished,
    Failed,
    Cancelled,
};

enum class ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1
    : std::uint8_t
{
    Ready,
    BackendUnavailable,
    SnapshotInvalid,
    EntryMissing,
    EntryInvalid,
    PolicyIneligible,
    GenerationUnavailable,
    RandomUnavailable,
    QueueUnavailable,
    AllocationFailed,
};

enum class ComponentBundleProbeMacOSCoordinatorDiagnosticV1 : std::uint8_t
{
    Prepared,
    Running,
    Ready,
    ProbeRejected,
    ControlledFatal,
    ProcessCrashed,
    DeadlineExpired,
    ProtocolRejected,
    LaunchDenied,
    PolicyChanged,
    Cancelled,
    InternalFailure,
};

enum class ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1
    : std::uint8_t
{
    Production,
    TestShort,
};

struct ComponentBundleProbeMacOSCoordinatorInputsV1
{
    component_bundle::ComponentBundleSnapshot snapshot;
    ComponentSignatureEvidence signature;
    ComponentOwnerAuthorizationEvidenceV1 ownerAuthorization;
};

class ComponentBundleProbeMacOSCoordinator;

struct ComponentBundleProbeMacOSCoordinatorPreparationV1
{
    std::unique_ptr<ComponentBundleProbeMacOSCoordinator> coordinator;
    ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1 diagnostic =
        ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
            SnapshotInvalid;
    ComponentTrustDecision trustDecision;
};

class ComponentBundleProbeMacOSCoordinator
{
public:
    ~ComponentBundleProbeMacOSCoordinator();

    ComponentBundleProbeMacOSCoordinator(
        const ComponentBundleProbeMacOSCoordinator&) = delete;
    ComponentBundleProbeMacOSCoordinator& operator=(
        const ComponentBundleProbeMacOSCoordinator&) = delete;
    ComponentBundleProbeMacOSCoordinator(
        ComponentBundleProbeMacOSCoordinator&&) = delete;
    ComponentBundleProbeMacOSCoordinator& operator=(
        ComponentBundleProbeMacOSCoordinator&&) = delete;

    [[nodiscard]] static ComponentBundleProbeMacOSCoordinatorPreparationV1
    prepare(
        ComponentBundleProbeMacOSCoordinatorInputsV1 inputs,
        ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1 deadlineProfile =
            ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::Production)
        noexcept;

    [[nodiscard]] ComponentBundleProbeMacOSCoordinatorOperationV1 start()
        noexcept;
    [[nodiscard]] ComponentBundleProbeMacOSCoordinatorOperationV1 cancel()
        noexcept;

    [[nodiscard]] ComponentBundleProbeMacOSCoordinatorStateV1 state()
        const noexcept;
    [[nodiscard]] ComponentBundleProbeMacOSCoordinatorDiagnosticV1 diagnostic()
        const noexcept;
    [[nodiscard]] ComponentBundleProbeResultDiagnosticV1 resultDiagnostic()
        const noexcept;
    [[nodiscard]] bool operationPending() const noexcept;
    [[nodiscard]] ComponentProbeEvidence evidence() const noexcept;
    [[nodiscard]] std::uint64_t artifactGeneration() const noexcept;
    [[nodiscard]] std::array<std::uint8_t, componentProbeNonceBytes>
    validationNonce() const noexcept;
    [[nodiscard]] std::int32_t processIdentifier() const noexcept;

private:
    struct Impl;

    explicit ComponentBundleProbeMacOSCoordinator(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> impl_;
};
}
