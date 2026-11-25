#pragma once

#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "talas/runtime/ComponentIsolationControlSession.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <cstddef>
#include <memory>
#include <span>

namespace talas::runtime
{
enum class ComponentIsolationMacOSHostOperation : std::uint8_t
{
    Scheduled,
    InvalidState,
    Unavailable,
};

enum class ComponentIsolationMacOSProductEndpointV1 : std::uint8_t
{
    ProductionGain,
    FixtureMalformed,
    FixtureWrongVersion,
    FixtureWrongNonce,
    FixtureStaleGeneration,
    FixtureControlledFailure,
    FixtureCrash,
    FixtureHang,
    FixtureOversizedDiagnostic,
    FixtureEarlyExit,
    FixtureTeardownRace,
    FixtureSandboxProbe,
    FixtureHeartbeatHang,
    FixtureTeardownHang,
    FixtureHeartbeatLate,
    FixturePrewarmHang,
};

enum class ComponentIsolationMacOSHostPreparationDiagnostic : std::uint8_t
{
    Ready,
    InvalidConfiguration,
    SharedStorageUnavailable,
    ControlQueueUnavailable,
    AllocationFailed,
};

class ComponentIsolationMacOSHost;

struct ComponentIsolationMacOSHostPreparation
{
    std::unique_ptr<ComponentIsolationMacOSHost> host;
    ComponentIsolationMacOSHostPreparationDiagnostic diagnostic =
        ComponentIsolationMacOSHostPreparationDiagnostic::InvalidConfiguration;
};

class ComponentIsolationMacOSHost
{
public:
    ~ComponentIsolationMacOSHost();

    ComponentIsolationMacOSHost(ComponentIsolationMacOSHost&&) = delete;
    ComponentIsolationMacOSHost& operator=(ComponentIsolationMacOSHost&&) =
        delete;
    ComponentIsolationMacOSHost(const ComponentIsolationMacOSHost&) = delete;
    ComponentIsolationMacOSHost& operator=(
        const ComponentIsolationMacOSHost&) = delete;

    [[nodiscard]] static ComponentIsolationMacOSHostPreparation prepare(
        const ComponentIsolationControlSessionConfigurationV1& configuration,
        std::shared_ptr<ComponentIsolationAudioDataPlane> sharedAudio,
        ComponentIsolationMacOSProductEndpointV1 endpoint =
            ComponentIsolationMacOSProductEndpointV1::ProductionGain)
        noexcept;

    [[nodiscard]] ComponentIsolationMacOSHostOperation start() noexcept;
    [[nodiscard]] ComponentIsolationMacOSHostOperation requestHeartbeat()
        noexcept;
    [[nodiscard]] ComponentIsolationMacOSHostOperation requestTeardown()
        noexcept;
    // Control-owner notification; this schedules work and is not callback-safe.
    [[nodiscard]] ComponentIsolationMacOSHostOperation
    reportResponseDeadlineMiss() noexcept;

    [[nodiscard]] ComponentIsolationControlSessionState state() const noexcept;
    [[nodiscard]] ComponentIsolationControlSessionDiagnostic diagnostic()
        const noexcept;
    [[nodiscard]] std::size_t diagnosticBytes() const noexcept;
    [[nodiscard]] std::size_t copyDiagnostic(
        std::span<std::byte> destination) const noexcept;
    [[nodiscard]] std::uint32_t lifecycleTrace() const noexcept;
    [[nodiscard]] bool operationPending() const noexcept;
    [[nodiscard]] ComponentIsolationEvidenceV1 isolationEvidence()
        const noexcept;

private:
    struct Impl;

    explicit ComponentIsolationMacOSHost(std::shared_ptr<Impl> implementation)
        noexcept;

    std::shared_ptr<Impl> impl_;
};
}
