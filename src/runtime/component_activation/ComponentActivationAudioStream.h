#pragma once

#include "talas/runtime/ComponentActivationAudioDataPlane.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace talas::runtime::component_activation
{
inline constexpr std::uint64_t componentActivationAudioStreamSetSlotMaskV1 =
    0x3U;

struct ComponentActivationAudioStreamPrepareResultV1;

enum class ComponentActivationAudioStreamDiagnosticV1 : std::uint8_t
{
    Ready,
    GrantInvalid,
    SourceInvalid,
    AllocationFailed,
    WorkerUnavailable,
    PrewarmFailed,
};

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
enum class ComponentActivationAudioStreamWorkerModeV1 : std::uint8_t
{
    Background,
    ManualTest,
};
#endif

class ComponentActivationAudioStreamV1
{
public:
    ~ComponentActivationAudioStreamV1();

    ComponentActivationAudioStreamV1(
        const ComponentActivationAudioStreamV1&) = delete;
    ComponentActivationAudioStreamV1& operator=(
        const ComponentActivationAudioStreamV1&) = delete;

    [[nodiscard]] const TalasKernelAbiImmutableAudioStreamViewV3& view()
        const noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 read(
        const TalasKernelAbiImmutableAudioStreamReadV3& request,
        TalasKernelAbiImmutableAudioStreamReadResultV3& result) noexcept;
    [[nodiscard]] bool bindResourceSetSlot(std::size_t slot) noexcept;

    // Exclusive control-thread operations: quiesce read(), epoch replacement,
    // slot binding and other lifecycle calls. The background decoder may run;
    // capture/restore synchronize it internally. One instance-owned baseline is
    // consumed by restore/commit or discarded by shutdown. Capture failure
    // preserves processing state; restore/commit allocate no memory. Terminal
    // failure/stop is never restored. Health may be inspected concurrently.
    [[nodiscard]] bool beginOfflineCheckpoint(bool deferHealth = false) noexcept;
    [[nodiscard]] bool restoreOfflineCheckpoint() noexcept;
    [[nodiscard]] bool commitOfflineCheckpoint() noexcept;

    // Control-thread lifecycle boundary. Replaces the callback-visible epoch,
    // drains stale responses, and synchronously prepares the new initial
    // window. Never call from a component process callback.
    [[nodiscard]] bool replaceEpochAndPrewarm(
        double initialSourceFrame,
        std::chrono::milliseconds timeout) noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ComponentActivationAudioStreamHealthV1 health()
        const noexcept;
    // Coordinator inspection of provisional/terminal state even when ordinary
    // health publication is deferred. No processing state is changed.
    [[nodiscard]] ComponentActivationAudioStreamHealthV1 processingHealth() const noexcept;

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    [[nodiscard]] std::size_t pumpForTest(
        std::size_t maximumBlocks = 1024U) noexcept;
    void setHealthCountersForTest(std::uint64_t value) noexcept;
    void setDecodeFailuresForTest(std::uint64_t value) noexcept;
    void failWorkerForTest() noexcept;
#endif

private:
    class Impl;
    explicit ComponentActivationAudioStreamV1(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;

    [[nodiscard]] static ComponentActivationAudioStreamPrepareResultV1
    prepareInternal(
        std::span<const std::uint8_t>,
        const ComponentActivationAudioStreamGrantV1&,
        const TalasKernelAbiImmutableAudioStreamPrepareV3&,
        std::chrono::milliseconds,
        bool) noexcept;

    friend struct ComponentActivationAudioStreamPrepareResultV1;
    friend ComponentActivationAudioStreamPrepareResultV1
    prepareComponentActivationAudioStreamV1(
        std::span<const std::uint8_t>,
        const ComponentActivationAudioStreamGrantV1&,
        const TalasKernelAbiImmutableAudioStreamPrepareV3&,
        std::chrono::milliseconds) noexcept;
#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    friend ComponentActivationAudioStreamPrepareResultV1
    prepareComponentActivationAudioStreamForTestV1(
        std::span<const std::uint8_t>,
        const ComponentActivationAudioStreamGrantV1&,
        const TalasKernelAbiImmutableAudioStreamPrepareV3&,
        ComponentActivationAudioStreamWorkerModeV1) noexcept;
#endif
};

struct ComponentActivationAudioStreamPrepareResultV1
{
    std::unique_ptr<ComponentActivationAudioStreamV1> stream;
    ComponentActivationAudioStreamDiagnosticV1 diagnostic =
        ComponentActivationAudioStreamDiagnosticV1::GrantInvalid;

    [[nodiscard]] bool ok() const noexcept
    {
        return stream != nullptr
            && diagnostic == ComponentActivationAudioStreamDiagnosticV1::Ready;
    }
};

[[nodiscard]] ComponentActivationAudioStreamPrepareResultV1
prepareComponentActivationAudioStreamV1(
    std::span<const std::uint8_t> sourceBytes,
    const ComponentActivationAudioStreamGrantV1& grant,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    std::chrono::milliseconds prewarmTimeout =
        std::chrono::milliseconds(250)) noexcept;

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
[[nodiscard]] ComponentActivationAudioStreamPrepareResultV1
prepareComponentActivationAudioStreamForTestV1(
    std::span<const std::uint8_t> sourceBytes,
    const ComponentActivationAudioStreamGrantV1& grant,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    ComponentActivationAudioStreamWorkerModeV1 workerMode) noexcept;
#endif
}
