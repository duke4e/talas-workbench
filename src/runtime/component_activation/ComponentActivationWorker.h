#pragma once

#include "talas/runtime/ComponentActivationAudioDataPlane.h"
#include "runtime/component_activation/macos/ComponentActivationLoader.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace talas::runtime::component_activation
{
enum class ComponentActivationWorkerDiagnosticV1 : std::uint8_t
{
    Ready,
    InvalidConfiguration,
    PlaneRejected,
    StreamPrewarmRejected,
    ThreadUnavailable,
};

class ComponentActivationWorkerV1
{
public:
    ~ComponentActivationWorkerV1();

    ComponentActivationWorkerV1(const ComponentActivationWorkerV1&) = delete;
    ComponentActivationWorkerV1& operator=(
        const ComponentActivationWorkerV1&) = delete;

    [[nodiscard]] static std::unique_ptr<ComponentActivationWorkerV1> prepare(
        std::span<std::byte> sharedPlane,
        const ComponentActivationAuthorizationV1& authorization,
        ComponentActivationLoadedInstanceV1& instance,
        ComponentActivationWorkerDiagnosticV1& diagnostic,
        std::span<std::byte> healthPlane = {}) noexcept;

    // Exclusive control operations, with host submission excluded. Stop/join
    // the same worker and require the exact completed physical sequence with
    // no queued request. On success the worker remains stopped; start() resumes
    // provisional or committed processing without resetting prepared resources.
    // The instance must outlive this worker. Direct loaded checkpoint/lifecycle
    // operations cannot interleave with a worker-owned checkpoint. The caller
    // separately coordinates host pending output and parent health publication.
    // Successful control publishes exact resource health before returning. A
    // failed publication quarantines and consumes held ownership.
    [[nodiscard]] TalasKernelAbiStatusV1 beginOfflineCheckpoint(
        std::uint64_t completedSequence) noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 restoreOfflineCheckpoint(
        std::uint64_t completedSequence) noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 commitOfflineCheckpoint(
        std::uint64_t completedSequence) noexcept;

    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    ComponentActivationWorkerV1(
        ComponentActivationAudioWorkerPlaneV1 plane,
        std::optional<ComponentActivationAudioStreamHealthProducerV1>
            healthProducerV1,
        std::optional<ComponentActivationAudioStreamHealthProducerV2>
            healthProducerV2,
        const ComponentActivationAuthorizationV1& authorization,
        ComponentActivationLoadedInstanceV1& instance) noexcept;

    [[nodiscard]] TalasKernelAbiStatusV1 finishOfflineCheckpoint(
        std::uint64_t completedSequence, bool restore) noexcept;
    void discardOfflineCheckpoint() noexcept;
    void run() noexcept;
    [[nodiscard]] bool processOne() noexcept;
    [[nodiscard]] bool publishResourceHealth() noexcept;
    [[nodiscard]] TalasKernelAbiTransportV1 transportFor(
        const ComponentActivationAudioTransportV1& transport) const noexcept;

    ComponentActivationAudioWorkerPlaneV1 plane_;
    std::optional<ComponentActivationAudioStreamHealthProducerV1>
        healthProducerV1_;
    std::optional<ComponentActivationAudioStreamHealthProducerV2>
        healthProducerV2_;
    ComponentActivationLoadedInstanceV1* instance_ = nullptr;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t blockFrames_ = 0;
    std::uint16_t inputChannelCount_ = 0;
    std::uint16_t outputChannelCount_ = 0;
    std::array<double, maximumComponentActivationAudioSamplesV1> inputScratch_{};
    std::array<double, maximumComponentActivationAudioSamplesV1> outputScratch_{};
    std::array<TalasKernelAbiSignalV1, maximumComponentActivationAudioChannelsV1>
        inputs_{};
    std::array<TalasKernelAbiOutputV1, maximumComponentActivationAudioChannelsV1>
        outputs_{};
    ComponentActivationAudioWorkerBlockV1 block_;
    ComponentActivationAudioTransportV1 previousTransport_;
    ComponentActivationAudioTransportV1 checkpointTransport_;
    bool checkpointActive_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};
}
