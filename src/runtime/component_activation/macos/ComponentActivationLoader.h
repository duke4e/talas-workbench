#pragma once

#include "talas/runtime/ComponentActivationProtocol.h"
#include "talas/runtime/KernelAbi.h"
#include "runtime/component_activation/ComponentActivationAudioStream.h"

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <span>

namespace talas::runtime::component_activation
{
struct ComponentActivationLoaderResultV1;

struct ComponentActivationPreparedResourceSetMemberV1
{
    const ComponentActivationResourceSetMemberV1* member = nullptr;
    const ComponentActivationImmutableResourceGrantV1* immutableGrant =
        nullptr;
    std::span<const std::uint8_t> immutableBytes;
    const ComponentActivationAudioStreamGrantV1* audioGrant = nullptr;
    std::span<const std::uint8_t> audioSourceBytes;
};

struct ComponentActivationPreparedResourceSetV1
{
    const ComponentActivationResourceSetGrantV1* grant = nullptr;
    std::span<const ComponentActivationPreparedResourceSetMemberV1> members;
};

struct ComponentActivationAudioStreamHealthSlotV1
{
    ComponentActivationAudioStreamHealthSlotBindingV2 binding;
    ComponentActivationAudioStreamHealthV1 health;
};

struct ComponentActivationAudioStreamHealthSetV1
{
    ComponentActivationAudioStreamHealthSetBindingV2 binding;
    std::array<
        ComponentActivationAudioStreamHealthSlotV1,
        maximumComponentActivationResourceSetAudioStreamsV1>
        slots{};
    std::uint16_t slotCount = 0;
};

enum class ComponentActivationLoaderDiagnosticV1 : std::uint8_t
{
    Ready,
    ProtocolRejected,
    ManifestRejected,
    EntryRejected,
    ResourceRejected,
    AudioStreamRejected,
    LoadRejected,
    AbiRejected,
    CreateRejected,
    PrepareRejected,
    HostViolation,
    AllocationFailed,
};

class ComponentActivationLoadedInstanceV1
{
public:
    ~ComponentActivationLoadedInstanceV1();

    ComponentActivationLoadedInstanceV1(
        const ComponentActivationLoadedInstanceV1&) = delete;
    ComponentActivationLoadedInstanceV1& operator=(
        const ComponentActivationLoadedInstanceV1&) = delete;

    [[nodiscard]] bool active() const noexcept;
    // Exclusive control-thread kernel-state primitive. Transport and mediated
    // resource consumers require separate checkpoints before binding opt-in.
    [[nodiscard]] bool supportsKernelOfflineCheckpoint() const noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 beginKernelOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 restoreKernelOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 commitKernelOfflineCheckpoint() noexcept;
    // Exclusive, coordinated kernel and mediated-resource checkpoint. Quiesce
    // processing, reset, replacement and lifecycle operations before control
    // calls. Health inspection may run concurrently; destruction still requires
    // readers to release the instance. Host/helper transport is not captured.
    [[nodiscard]] bool supportsOfflineCheckpoint() const noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 beginOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 restoreOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 commitOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 reset(
        const TalasKernelAbiResetV1& reset) noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 process(
        const TalasKernelAbiProcessV1& process) noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 process(
        const TalasKernelAbiProcessV1& process,
        std::uint64_t outerSequence) noexcept;
    [[nodiscard]] bool replaceAudioStreamEpochAndPrewarm(
        double initialSourceFrame) noexcept;
    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthV1>
    audioStreamHealth() const noexcept;
    [[nodiscard]] std::optional<ComponentActivationAudioStreamHealthSetV1>
    audioStreamHealthSet() const noexcept;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    void failNextCheckpointAllocationForTesting() noexcept;
    [[nodiscard]] std::array<
        TalasKernelAbiAudioStreamTokenV3,
        maximumComponentActivationResourceSetAudioStreamsV1>
    audioStreamTokensForTesting() const noexcept;
    void setAudioStreamHealthCountersForTest(
        std::uint64_t value,
        std::optional<std::size_t> resourceSetAudioSlot = std::nullopt)
        noexcept;
#endif
    [[nodiscard]] bool shutdown() noexcept;

private:
    struct Impl;
    explicit ComponentActivationLoadedInstanceV1(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;

    friend struct ComponentActivationLoaderResultV1;
    friend ComponentActivationLoaderResultV1 loadComponentActivationV1(
        const ComponentActivationAuthorizationV1&,
        const ComponentActivationSealedSessionV1&,
        std::span<const std::uint8_t>,
        std::span<const std::uint8_t>,
        std::span<const ComponentActivationStaticParameterV1>,
        int,
        const ComponentActivationImmutableResourceGrantV1*,
        std::span<const std::uint8_t>,
        const ComponentActivationAudioStreamGrantV1*,
        std::span<const std::uint8_t>,
        const ComponentActivationPreparedResourceSetV1*) noexcept;
};

struct ComponentActivationLoaderResultV1
{
    std::unique_ptr<ComponentActivationLoadedInstanceV1> instance;
    ComponentActivationLoaderDiagnosticV1 diagnostic =
        ComponentActivationLoaderDiagnosticV1::ProtocolRejected;

    [[nodiscard]] bool ok() const noexcept
    {
        return instance != nullptr
            && diagnostic == ComponentActivationLoaderDiagnosticV1::Ready;
    }
};

[[nodiscard]] ComponentActivationLoaderResultV1 loadComponentActivationV1(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    std::span<const std::uint8_t> canonicalManifest,
    std::span<const std::uint8_t> entryBytes,
    std::span<const ComponentActivationStaticParameterV1> staticParameters,
    int materializationFileDescriptor,
    const ComponentActivationImmutableResourceGrantV1* resourceGrant = nullptr,
    std::span<const std::uint8_t> resourceBytes = {},
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant = nullptr,
    std::span<const std::uint8_t> audioSourceBytes = {},
    const ComponentActivationPreparedResourceSetV1* resourceSet = nullptr)
    noexcept;
}
