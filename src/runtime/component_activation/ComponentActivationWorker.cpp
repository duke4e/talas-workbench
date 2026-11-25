#include "runtime/component_activation/ComponentActivationWorker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace talas::runtime::component_activation
{
namespace
{
void realtimePollPause() noexcept
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
}

ComponentActivationWorkerV1::ComponentActivationWorkerV1(
    ComponentActivationAudioWorkerPlaneV1 plane,
    std::optional<ComponentActivationAudioStreamHealthProducerV1>
        healthProducerV1,
    std::optional<ComponentActivationAudioStreamHealthProducerV2>
        healthProducerV2,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationLoadedInstanceV1& instance) noexcept
    : plane_(std::move(plane)),
      healthProducerV1_(std::move(healthProducerV1)),
      healthProducerV2_(std::move(healthProducerV2)),
      instance_(&instance),
      sampleRate_(authorization.sampleRate),
      blockFrames_(authorization.blockFrames),
      inputChannelCount_(authorization.inputChannelCount),
      outputChannelCount_(authorization.outputChannelCount)
{
    for (std::size_t channel = 0; channel < inputChannelCount_; ++channel)
    {
        auto& input = inputs_[channel];
        input.structSize = sizeof(input);
        input.kind = TALAS_KERNEL_ABI_SIGNAL_PER_FRAME;
        input.samples = inputScratch_.data()
            + channel * maximumComponentActivationAudioFramesV1;
        input.sampleCount = blockFrames_;
    }
    for (std::size_t channel = 0; channel < outputChannelCount_; ++channel)
    {
        auto& output = outputs_[channel];
        output.structSize = sizeof(output);
        output.samples = outputScratch_.data()
            + channel * maximumComponentActivationAudioFramesV1;
        output.sampleCount = blockFrames_;
    }
}

ComponentActivationWorkerV1::~ComponentActivationWorkerV1()
{
    stop();
    discardOfflineCheckpoint();
}

std::unique_ptr<ComponentActivationWorkerV1>
ComponentActivationWorkerV1::prepare(
    std::span<std::byte> sharedPlane,
    const ComponentActivationAuthorizationV1& authorization,
    ComponentActivationLoadedInstanceV1& instance,
    ComponentActivationWorkerDiagnosticV1& diagnostic,
    std::span<std::byte> healthPlane) noexcept
{
    diagnostic = ComponentActivationWorkerDiagnosticV1::InvalidConfiguration;
    if (!instance.active())
        return nullptr;
    if (!instance.replaceAudioStreamEpochAndPrewarm(0.0))
    {
        diagnostic =
            ComponentActivationWorkerDiagnosticV1::StreamPrewarmRejected;
        return nullptr;
    }
    auto plane = ComponentActivationAudioWorkerPlaneV1::attach(
        sharedPlane, authorization);
    if (!plane.has_value())
    {
        diagnostic = ComponentActivationWorkerDiagnosticV1::PlaneRejected;
        return nullptr;
    }
    std::optional<ComponentActivationAudioStreamHealthProducerV1>
        healthProducerV1;
    std::optional<ComponentActivationAudioStreamHealthProducerV2>
        healthProducerV2;
    const auto streamHealth = instance.audioStreamHealth();
    const auto streamHealthSet = instance.audioStreamHealthSet();
    if (streamHealth.has_value() && streamHealthSet.has_value())
    {
        diagnostic = ComponentActivationWorkerDiagnosticV1::PlaneRejected;
        return nullptr;
    }
    if (streamHealthSet.has_value())
    {
        if (streamHealthSet->slotCount == 0U
            || streamHealthSet->slotCount
                != streamHealthSet->binding.slotCount)
        {
            diagnostic = ComponentActivationWorkerDiagnosticV1::PlaneRejected;
            return nullptr;
        }
        healthProducerV2 =
            ComponentActivationAudioStreamHealthProducerV2::attach(
                healthPlane, authorization, streamHealthSet->binding);
        if (!healthProducerV2.has_value())
        {
            diagnostic = ComponentActivationWorkerDiagnosticV1::PlaneRejected;
            return nullptr;
        }
        for (std::size_t slot = 0;
             slot < streamHealthSet->slotCount;
             ++slot)
        {
            if (streamHealthSet->slots[slot].binding
                    != streamHealthSet->binding.slots[slot]
                || !healthProducerV2->publish(
                    slot, streamHealthSet->slots[slot].health))
            {
                diagnostic =
                    ComponentActivationWorkerDiagnosticV1::PlaneRejected;
                return nullptr;
            }
        }
    }
    else if (streamHealth.has_value())
    {
        healthProducerV1 =
            ComponentActivationAudioStreamHealthProducerV1::attach(
                healthPlane, authorization);
        if (!healthProducerV1.has_value()
            || !healthProducerV1->publish(*streamHealth))
        {
            diagnostic = ComponentActivationWorkerDiagnosticV1::PlaneRejected;
            return nullptr;
        }
    }
    try
    {
        auto worker = std::unique_ptr<ComponentActivationWorkerV1>(
            new ComponentActivationWorkerV1(
                std::move(*plane),
                std::move(healthProducerV1),
                std::move(healthProducerV2),
                authorization,
                instance));
        diagnostic = ComponentActivationWorkerDiagnosticV1::Ready;
        return worker;
    }
    catch (const std::bad_alloc&)
    {
        diagnostic = ComponentActivationWorkerDiagnosticV1::ThreadUnavailable;
        return nullptr;
    }
}

void ComponentActivationWorkerV1::discardOfflineCheckpoint() noexcept
{
    if (!checkpointActive_) return;
    checkpointActive_ = false;
    if (instance_ != nullptr
        && instance_->commitOfflineCheckpoint() != TALAS_KERNEL_ABI_STATUS_OK)
        plane_.quarantine(ComponentActivationAudioQuarantineReasonV1::ExplicitControl);
}

TalasKernelAbiStatusV1 ComponentActivationWorkerV1::beginOfflineCheckpoint(
    std::uint64_t completedSequence) noexcept
{
    stop();
    if (plane_.isQuarantined() || instance_ == nullptr || !instance_->active())
    {
        discardOfflineCheckpoint();
        plane_.quarantine(ComponentActivationAudioQuarantineReasonV1::ExplicitControl);
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    if (checkpointActive_) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!plane_.isQuiescentAtSequence(completedSequence))
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    const auto status = instance_->beginOfflineCheckpoint();
    if (status == TALAS_KERNEL_ABI_STATUS_OK)
    {
        checkpointTransport_ = previousTransport_;
        checkpointActive_ = true;
        if (!publishResourceHealth())
        {
            discardOfflineCheckpoint();
            return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
        }
    }
    else if (!instance_->active() || status == TALAS_KERNEL_ABI_STATUS_FATAL
        || status == TALAS_KERNEL_ABI_STATUS_QUARANTINED)
        plane_.quarantine(ComponentActivationAudioQuarantineReasonV1::ExplicitControl);
    return status;
}

TalasKernelAbiStatusV1 ComponentActivationWorkerV1::finishOfflineCheckpoint(
    std::uint64_t completedSequence, bool restore) noexcept
{
    stop();
    if (plane_.isQuarantined() || instance_ == nullptr || !instance_->active())
    {
        discardOfflineCheckpoint();
        plane_.quarantine(ComponentActivationAudioQuarantineReasonV1::ExplicitControl);
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    if (!checkpointActive_ || !plane_.isQuiescentAtSequence(completedSequence))
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    checkpointActive_ = false;
    const auto status = restore ? instance_->restoreOfflineCheckpoint()
        : instance_->commitOfflineCheckpoint();
    if (status == TALAS_KERNEL_ABI_STATUS_OK)
    {
        if (restore) previousTransport_ = checkpointTransport_;
        if (!publishResourceHealth()) return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    }
    else
        plane_.quarantine(ComponentActivationAudioQuarantineReasonV1::ExplicitControl);
    return status;
}

TalasKernelAbiStatusV1 ComponentActivationWorkerV1::restoreOfflineCheckpoint(
    std::uint64_t completedSequence) noexcept
{
    return finishOfflineCheckpoint(completedSequence, true);
}

TalasKernelAbiStatusV1 ComponentActivationWorkerV1::commitOfflineCheckpoint(
    std::uint64_t completedSequence) noexcept
{
    return finishOfflineCheckpoint(completedSequence, false);
}

bool ComponentActivationWorkerV1::start() noexcept
{
    if (thread_.joinable() || instance_ == nullptr || !instance_->active()
        || plane_.isQuarantined())
        return false;
    stopRequested_.store(false, std::memory_order_release);
    try
    {
        thread_ = std::thread([this] { run(); });
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(100);
        while (!running_.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        if (running_.load(std::memory_order_acquire))
            return true;
        stop();
        return false;
    }
    catch (...)
    {
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
}

void ComponentActivationWorkerV1::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

bool ComponentActivationWorkerV1::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

TalasKernelAbiTransportV1 ComponentActivationWorkerV1::transportFor(
    const ComponentActivationAudioTransportV1& transport) const noexcept
{
    TalasKernelAbiTransportV1 value{};
    value.structSize = sizeof(value);
    value.playing = 1;
    value.epoch = transport.generation;
    value.absoluteFrame = static_cast<std::uint64_t>(transport.sourceFrame);
    value.playheadFrame = value.absoluteFrame;
    value.discontinuitySequence = transport.generation;
    value.sampleRate = sampleRate_;
    return value;
}

bool ComponentActivationWorkerV1::publishResourceHealth() noexcept
{
    const auto streamHealth = instance_->audioStreamHealth();
    const auto streamHealthSet = instance_->audioStreamHealthSet();
    const auto streamHealthImpossible = streamHealth.has_value()
        && (streamHealth->decodeFailures != 0U
            || streamHealth->workerState
                == ComponentActivationAudioStreamWorkerStateV1::Failed
            || streamHealth->workerState
                == ComponentActivationAudioStreamWorkerStateV1::Stopped);
    auto streamHealthSetInvalid = streamHealthSet.has_value()
        != healthProducerV2_.has_value();
    if (streamHealthSet.has_value() && !streamHealthSetInvalid)
    {
        streamHealthSetInvalid = streamHealthSet->slotCount == 0U
            || streamHealthSet->slotCount
                != streamHealthSet->binding.slotCount;
        for (std::size_t slot = 0;
             !streamHealthSetInvalid && slot < streamHealthSet->slotCount;
             ++slot)
        {
            const auto& observed = streamHealthSet->slots[slot];
            streamHealthSetInvalid = observed.binding
                    != streamHealthSet->binding.slots[slot]
                || observed.health.decodeFailures != 0U
                || observed.health.workerState
                    == ComponentActivationAudioStreamWorkerStateV1::Failed
                || observed.health.workerState
                    == ComponentActivationAudioStreamWorkerStateV1::Stopped
                || !healthProducerV2_->publish(slot, observed.health);
        }
    }
    if (streamHealthImpossible || streamHealthSetInvalid
        || (streamHealth.has_value() && streamHealthSet.has_value())
        || streamHealth.has_value() != healthProducerV1_.has_value()
        || (streamHealth.has_value()
            && !healthProducerV1_->publish(*streamHealth)))
    {
        plane_.quarantine(
            ComponentActivationAudioQuarantineReasonV1::StreamHealthInvalid);
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

bool ComponentActivationWorkerV1::processOne() noexcept
{
    const auto acquired = plane_.acquire(block_);
    if (acquired == ComponentActivationAudioOperationV1::Missing)
        return false;
    if (acquired != ComponentActivationAudioOperationV1::Acquired)
    {
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
    if (block_.transport.sourceFrame < 0)
    {
        plane_.quarantine(
            ComponentActivationAudioQuarantineReasonV1::
                RequestBindingMismatch);
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
    const auto resetRequired = previousTransport_.generation == 0
        || previousTransport_.generation != block_.transport.generation
        || (block_.transport.flags & 1U) != 0;
    const auto processingStart = std::chrono::steady_clock::now();
    if (resetRequired)
    {
        TalasKernelAbiResetV1 reset{};
        reset.structSize = sizeof(reset);
        reset.reason = previousTransport_.generation == 0
            ? TALAS_KERNEL_ABI_RESET_GRAPH_ACTIVATION
            : TALAS_KERNEL_ABI_RESET_TRANSPORT;
        reset.transport = transportFor(block_.transport);
        if (instance_->reset(reset) != TALAS_KERNEL_ABI_STATUS_OK)
        {
            plane_.quarantine(
                ComponentActivationAudioQuarantineReasonV1::
                    WorkerResetRejected);
            stopRequested_.store(true, std::memory_order_release);
            return false;
        }
    }
    for (std::size_t channel = 0; channel < inputChannelCount_; ++channel)
    {
        for (std::size_t frame = 0; frame < blockFrames_; ++frame)
        {
            const auto index =
                channel * maximumComponentActivationAudioFramesV1 + frame;
            inputScratch_[index] = block_.inputSamples[index];
        }
    }
    std::fill(outputScratch_.begin(), outputScratch_.end(), 0.0);
    TalasKernelAbiProcessV1 process{};
    process.structSize = sizeof(process);
    process.frameCount = blockFrames_;
    process.inputs = inputs_.data();
    process.inputCount = inputChannelCount_;
    process.outputs = outputs_.data();
    process.outputCount = outputChannelCount_;
    process.transport = transportFor(block_.transport);
    if (instance_->process(process, block_.sequence)
        != TALAS_KERNEL_ABI_STATUS_OK)
    {
        plane_.quarantine(
            ComponentActivationAudioQuarantineReasonV1::
                WorkerProcessRejected);
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
    if (!publishResourceHealth()) return false;
    const auto processingDuration = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - processingStart)
                                        .count();
    block_.processingNanoseconds = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, processingDuration));
    for (std::size_t channel = 0; channel < outputChannelCount_; ++channel)
    {
        for (std::size_t frame = 0; frame < blockFrames_; ++frame)
        {
            const auto index =
                channel * maximumComponentActivationAudioFramesV1 + frame;
            const auto value = outputScratch_[index];
            if (!std::isfinite(value)
                || value > std::numeric_limits<float>::max()
                || value < -std::numeric_limits<float>::max())
            {
                plane_.quarantine(
                    ComponentActivationAudioQuarantineReasonV1::
                        WorkerOutputNonFinite);
                stopRequested_.store(true, std::memory_order_release);
                return false;
            }
            block_.outputSamples[index] = static_cast<float>(value);
        }
    }
    if (plane_.publish(block_)
        != ComponentActivationAudioOperationV1::Published)
    {
        stopRequested_.store(true, std::memory_order_release);
        return false;
    }
    previousTransport_ = block_.transport;
    return true;
}

void ComponentActivationWorkerV1::run() noexcept
{
#if defined(__APPLE__)
    static_cast<void>(pthread_set_qos_class_self_np(
        QOS_CLASS_USER_INTERACTIVE, 0));
#endif
    running_.store(true, std::memory_order_release);
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        if (!processOne()
            && !stopRequested_.load(std::memory_order_acquire))
        {
            realtimePollPause();
        }
    }
    running_.store(false, std::memory_order_release);
}
}
