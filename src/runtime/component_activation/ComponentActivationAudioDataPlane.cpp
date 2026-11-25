#include "talas/runtime/ComponentActivationAudioDataPlane.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

namespace talas::runtime::component_activation
{
namespace
{
static_assert(std::is_trivially_copyable_v<
    ComponentActivationAudioBindingV1>);
static_assert(std::is_trivially_copyable_v<
    ComponentActivationAudioRecordV1>);
static_assert(std::atomic_ref<unsigned char>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint16_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<float>::is_always_lock_free);

template <typename Value>
[[nodiscard]] std::atomic_ref<Value> atomic(Value& value) noexcept
{
    return std::atomic_ref<Value>{value};
}

template <typename Value>
[[nodiscard]] Value loadAtomic(
    const Value& value,
    std::memory_order order) noexcept
{
    return std::atomic_ref<Value>{const_cast<Value&>(value)}.load(order);
}

template <typename Value>
void storeObject(Value& destination, const Value& source) noexcept
{
    auto* destinationBytes = reinterpret_cast<unsigned char*>(&destination);
    const auto* sourceBytes = reinterpret_cast<const unsigned char*>(&source);
    for (std::size_t index = 0; index < sizeof(Value); ++index)
    {
        atomic(destinationBytes[index]).store(
            sourceBytes[index], std::memory_order_relaxed);
    }
}

template <typename Value>
[[nodiscard]] Value loadObject(const Value& source) noexcept
{
    Value result{};
    auto* destinationBytes = reinterpret_cast<unsigned char*>(&result);
    const auto* sourceBytes = reinterpret_cast<const unsigned char*>(&source);
    for (std::size_t index = 0; index < sizeof(Value); ++index)
    {
        destinationBytes[index] = loadAtomic(
            sourceBytes[index], std::memory_order_relaxed);
    }
    return result;
}

[[nodiscard]] ComponentActivationSealedSessionV1 structuralSession(
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    ComponentActivationSealedSessionV1 session;
    session.header = componentActivationHeaderV1(
        ComponentActivationRecordKindV1::SealedSession,
        sizeof(session));
    session.profileVersion = componentActivationProfileVersionV1;
    // The audio plane is bound to an already validated authorization record.
    // Reconstruct its exact closed profile so both the v1 stateless and the
    // v2 immutable-table authorization digests remain admissible.
    session.supportedPermissions = authorization.requestedPermissions;
    session.safeguards = allComponentIsolationSafeguardsV1;
    session.runtimeSessionGeneration = authorization.runtimeSessionGeneration;
    session.sharedStorageIdentity = authorization.sharedStorageIdentity;
    session.sharedStorageBytes = 64U * 1024U;
    session.runtimeSessionNonce = authorization.runtimeSessionNonce;
    return session;
}

[[nodiscard]] bool validAuthorization(
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    return validateComponentActivationAuthorizationV1(
               authorization, structuralSession(authorization))
        == ComponentActivationProtocolDiagnosticV1::Valid;
}

[[nodiscard]] ComponentActivationAudioBindingV1 bindingFor(
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    ComponentActivationAudioBindingV1 binding;
    binding.authorizationDigest =
        calculateComponentActivationAuthorizationDigestV1(authorization);
    binding.contentDigest = authorization.contentDigest;
    binding.signerIdentityDigest = authorization.signerIdentityDigest;
    binding.requestedPermissions = authorization.requestedPermissions;
    binding.declaredRoles = authorization.declaredRoles;
    binding.policyVersion = authorization.policyVersion;
    binding.sampleRate = authorization.sampleRate;
    binding.blockFrames = authorization.blockFrames;
    binding.inputChannelCount = authorization.inputChannelCount;
    binding.outputChannelCount = authorization.outputChannelCount;
    binding.artifactGeneration = authorization.artifactGeneration;
    binding.activationGeneration = authorization.activationGeneration;
    binding.runtimeSessionGeneration =
        authorization.runtimeSessionGeneration;
    binding.sharedStorageIdentity = authorization.sharedStorageIdentity;
    binding.validationNonce = authorization.validationNonce;
    binding.runtimeSessionNonce = authorization.runtimeSessionNonce;
    binding.authorizationNonce = authorization.authorizationNonce;
    return binding;
}

[[nodiscard]] bool exactStorage(std::span<std::byte> storage) noexcept
{
    return storage.size() == componentActivationAudioPlaneBytesV1
        && storage.data() != nullptr
        && reinterpret_cast<std::uintptr_t>(storage.data())
            % alignof(ComponentActivationAudioSharedPlaneV1)
            == 0;
}

[[nodiscard]] bool pristine(std::span<const std::byte> storage) noexcept
{
    return std::ranges::all_of(storage, [](std::byte value) {
        return value == std::byte{};
    });
}

[[nodiscard]] ComponentActivationAudioSharedPlaneV1* planeFrom(
    std::span<std::byte> storage) noexcept
{
    return reinterpret_cast<ComponentActivationAudioSharedPlaneV1*>(
        storage.data());
}

[[nodiscard]] bool validHeader(
    ComponentActivationAudioSharedPlaneV1& plane,
    const ComponentActivationAudioBindingV1& expected) noexcept
{
    const auto publication = loadAtomic(
        plane.publication, std::memory_order_acquire);
    if (publication != 2)
        return false;
    const auto magic = loadAtomic(plane.magic, std::memory_order_relaxed);
    const auto protocol = loadAtomic(
        plane.protocolVersion, std::memory_order_relaxed);
    const auto headerBytes = loadAtomic(
        plane.headerBytes, std::memory_order_relaxed);
    const auto planeBytes = loadAtomic(
        plane.planeBytes, std::memory_order_relaxed);
    const auto profile = loadAtomic(
        plane.profileVersion, std::memory_order_relaxed);
    const auto binding = loadObject(plane.binding);
    const auto reserved = loadObject(plane.reserved0);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(plane.publication, std::memory_order_relaxed)
        != publication)
    {
        return false;
    }
    return magic == componentActivationAudioPlaneMagicV1
        && protocol == componentActivationAudioProtocolVersionV1
        && headerBytes
            == offsetof(ComponentActivationAudioSharedPlaneV1, requests)
        && planeBytes == sizeof(ComponentActivationAudioSharedPlaneV1)
        && profile == componentActivationProfileVersionV1
        && binding == expected
        && std::ranges::all_of(reserved, [](std::uint64_t value) {
               return value == 0;
           });
}

[[nodiscard]] constexpr std::uint64_t publicationFor(
    std::uint64_t sequence) noexcept
{
    return sequence * 2U;
}

[[nodiscard]] ComponentActivationAudioRecordV1 recordFor(
    ComponentActivationAudioRecordKindV1 kind,
    const ComponentActivationAudioBindingV1& binding,
    std::uint64_t sequence,
    ComponentActivationAudioTransportV1 transport,
    ComponentActivationAudioOutcomeV1 outcome) noexcept
{
    ComponentActivationAudioRecordV1 record;
    record.magic = componentActivationAudioRecordMagicV1;
    record.protocolVersion = componentActivationAudioProtocolVersionV1;
    record.kind = static_cast<std::uint16_t>(kind);
    record.recordBytes = sizeof(record);
    record.binding = binding;
    record.sequence = sequence;
    record.transport = transport;
    record.frameCount = binding.blockFrames;
    record.inputChannelCount = binding.inputChannelCount;
    record.outputChannelCount = binding.outputChannelCount;
    record.outcome = static_cast<std::uint32_t>(outcome);
    return record;
}

[[nodiscard]] bool validRecord(
    const ComponentActivationAudioRecordV1& record,
    ComponentActivationAudioRecordKindV1 kind,
    const ComponentActivationAudioBindingV1& binding,
    std::uint64_t sequence) noexcept
{
    const auto expectedOutcome = kind
            == ComponentActivationAudioRecordKindV1::Request
        ? ComponentActivationAudioOutcomeV1::None
        : ComponentActivationAudioOutcomeV1::Ready;
    return record.magic == componentActivationAudioRecordMagicV1
        && record.protocolVersion
            == componentActivationAudioProtocolVersionV1
        && record.kind == static_cast<std::uint16_t>(kind)
        && record.recordBytes == sizeof(record)
        && record.reserved0 == 0
        && record.binding == binding
        && record.sequence == sequence
        && record.transport.generation != 0
        && record.transport.reserved0 == 0
        && (record.transport.flags & ~std::uint32_t{1}) == 0
        && record.frameCount == binding.blockFrames
        && record.inputChannelCount == binding.inputChannelCount
        && record.outputChannelCount == binding.outputChannelCount
        && record.outcome == static_cast<std::uint32_t>(expectedOutcome)
        && (kind == ComponentActivationAudioRecordKindV1::Response
            || record.processingNanoseconds == 0)
        && std::ranges::all_of(record.reserved1, [](std::uint64_t value) {
               return value == 0;
           });
}

void latchQuarantine(
    ComponentActivationAudioSharedPlaneV1& plane,
    ComponentActivationAudioQuarantineReasonV1 reason) noexcept
{
    auto expected = static_cast<std::uint32_t>(
        ComponentActivationAudioQuarantineReasonV1::None);
    static_cast<void>(atomic(plane.quarantineReason).compare_exchange_strong(
        expected,
        static_cast<std::uint32_t>(reason),
        std::memory_order_acq_rel,
        std::memory_order_acquire));
    atomic(plane.quarantined).store(1, std::memory_order_release);
}

[[nodiscard]] bool quarantined(
    const ComponentActivationAudioSharedPlaneV1& plane) noexcept
{
    return loadAtomic(plane.quarantined, std::memory_order_acquire) != 0;
}

[[nodiscard]] bool knownQuarantineReason(
    ComponentActivationAudioQuarantineReasonV1 reason) noexcept
{
    switch (reason)
    {
        case ComponentActivationAudioQuarantineReasonV1::None:
        case ComponentActivationAudioQuarantineReasonV1::Revoked:
        case ComponentActivationAudioQuarantineReasonV1::InvalidCallbackProfile:
        case ComponentActivationAudioQuarantineReasonV1::InputNonFinite:
        case ComponentActivationAudioQuarantineReasonV1::ResponseMissing:
        case ComponentActivationAudioQuarantineReasonV1::ResponseSnapshotUnstable:
        case ComponentActivationAudioQuarantineReasonV1::ResponseBindingMismatch:
        case ComponentActivationAudioQuarantineReasonV1::ResponseNonFinite:
        case ComponentActivationAudioQuarantineReasonV1::RequestSnapshotUnstable:
        case ComponentActivationAudioQuarantineReasonV1::RequestBindingMismatch:
        case ComponentActivationAudioQuarantineReasonV1::WorkerOutputNonFinite:
        case ComponentActivationAudioQuarantineReasonV1::WorkerResetRejected:
        case ComponentActivationAudioQuarantineReasonV1::WorkerProcessRejected:
        case ComponentActivationAudioQuarantineReasonV1::StreamHealthInvalid:
        case ComponentActivationAudioQuarantineReasonV1::SequenceExhausted:
        case ComponentActivationAudioQuarantineReasonV1::ExplicitControl:
            return true;
    }
    return false;
}

void zeroOutputs(
    std::span<float* const> outputChannels,
    std::uint32_t frameCount) noexcept
{
    for (auto* channel : outputChannels)
    {
        if (channel != nullptr)
            std::fill_n(channel, frameCount, 0.0F);
    }
}

[[nodiscard]] bool validChannels(
    std::span<const float* const> inputs,
    std::span<float* const> outputs,
    const ComponentActivationAudioBindingV1& binding,
    std::uint32_t frameCount) noexcept
{
    return frameCount == binding.blockFrames
        && inputs.size() == binding.inputChannelCount
        && outputs.size() == binding.outputChannelCount
        && std::ranges::all_of(inputs, [](const float* value) {
               return value != nullptr;
           })
        && std::ranges::all_of(outputs, [](const float* value) {
               return value != nullptr;
           });
}

[[nodiscard]] bool finiteInputs(
    std::span<const float* const> inputs,
    std::uint32_t frameCount) noexcept
{
    for (const auto* channel : inputs)
    {
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            if (!std::isfinite(channel[frame]))
                return false;
        }
    }
    return true;
}

void writeRequest(
    ComponentActivationAudioSharedSlotV1& slot,
    const ComponentActivationAudioRecordV1& record,
    std::span<const float* const> inputChannels) noexcept
{
    const auto publication = publicationFor(record.sequence);
    atomic(slot.publication).store(
        publication | 1U, std::memory_order_release);
    storeObject(slot.record, record);
    for (std::size_t channel = 0; channel < inputChannels.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < record.frameCount; ++frame)
        {
            atomic(slot.samples[
                channel * maximumComponentActivationAudioFramesV1 + frame])
                .store(inputChannels[channel][frame], std::memory_order_relaxed);
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
    atomic(slot.publication).store(publication, std::memory_order_release);
}

void writeResponse(
    ComponentActivationAudioSharedSlotV1& slot,
    const ComponentActivationAudioRecordV1& record,
    const ComponentActivationAudioWorkerBlockV1& block) noexcept
{
    const auto publication = publicationFor(record.sequence);
    atomic(slot.publication).store(
        publication | 1U, std::memory_order_release);
    storeObject(slot.record, record);
    for (std::size_t channel = 0;
         channel < record.outputChannelCount;
         ++channel)
    {
        for (std::size_t frame = 0; frame < record.frameCount; ++frame)
        {
            const auto index =
                channel * maximumComponentActivationAudioFramesV1 + frame;
            atomic(slot.samples[index]).store(
                block.outputSamples[index], std::memory_order_relaxed);
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
    atomic(slot.publication).store(publication, std::memory_order_release);
}

[[nodiscard]] ComponentActivationAudioOperationV1 consumeResponse(
    ComponentActivationAudioSharedPlaneV1& plane,
    const ComponentActivationAudioBindingV1& binding,
    std::uint64_t sequence,
    const ComponentActivationAudioTransportV1& expectedTransport,
    std::span<float* const> outputChannels,
    std::uint32_t frameCount,
    std::uint64_t* processingNanoseconds) noexcept
{
    auto& slot = plane.responses[
        static_cast<std::size_t>((sequence - 1U)
            % componentActivationAudioSlotCountV1)];
    const auto expectedPublication = publicationFor(sequence);
    const auto publication = loadAtomic(
        slot.publication, std::memory_order_acquire);
    if (publication != expectedPublication)
    {
        latchQuarantine(
            plane,
            ComponentActivationAudioQuarantineReasonV1::ResponseMissing);
        return ComponentActivationAudioOperationV1::Missing;
    }
    const auto record = loadObject(slot.record);
    if (!validRecord(
            record,
            ComponentActivationAudioRecordKindV1::Response,
            binding,
            sequence)
        || record.transport != expectedTransport)
    {
        latchQuarantine(
            plane,
            ComponentActivationAudioQuarantineReasonV1::
                ResponseBindingMismatch);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    bool finite = true;
    for (std::size_t channel = 0; channel < outputChannels.size(); ++channel)
    {
        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            const auto value = loadAtomic(
                slot.samples[
                    channel * maximumComponentActivationAudioFramesV1
                    + frame],
                std::memory_order_relaxed);
            outputChannels[channel][frame] = value;
            finite = finite && std::isfinite(value);
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(slot.publication, std::memory_order_relaxed) != publication)
    {
        zeroOutputs(outputChannels, frameCount);
        latchQuarantine(
            plane,
            ComponentActivationAudioQuarantineReasonV1::
                ResponseSnapshotUnstable);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    if (!finite)
    {
        zeroOutputs(outputChannels, frameCount);
        latchQuarantine(
            plane,
            ComponentActivationAudioQuarantineReasonV1::ResponseNonFinite);
        return ComponentActivationAudioOperationV1::NonFinite;
    }
    if (processingNanoseconds != nullptr)
        *processingNanoseconds = record.processingNanoseconds;
    return ComponentActivationAudioOperationV1::Rendered;
}
}

ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioDataPlaneV1(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactStorage(storage))
        return ComponentActivationAudioDiagnosticV1::InvalidStorage;
    if (!validAuthorization(authorization))
        return ComponentActivationAudioDiagnosticV1::AuthorizationInvalid;
    if (!pristine(storage))
        return ComponentActivationAudioDiagnosticV1::StorageNotPristine;
    auto* plane = std::construct_at(planeFrom(storage));
    atomic(plane->publication).store(1, std::memory_order_relaxed);
    plane->magic = componentActivationAudioPlaneMagicV1;
    plane->protocolVersion = componentActivationAudioProtocolVersionV1;
    plane->headerBytes = offsetof(
        ComponentActivationAudioSharedPlaneV1, requests);
    plane->planeBytes = sizeof(ComponentActivationAudioSharedPlaneV1);
    plane->profileVersion = componentActivationProfileVersionV1;
    plane->binding = bindingFor(authorization);
    std::atomic_thread_fence(std::memory_order_release);
    atomic(plane->publication).store(2, std::memory_order_release);
    return ComponentActivationAudioDiagnosticV1::Ready;
}

ComponentActivationAudioHostV1::ComponentActivationAudioHostV1(
    ComponentActivationAudioSharedPlaneV1* plane,
    ComponentActivationAudioBindingV1 binding) noexcept
    : plane_(plane), binding_(std::move(binding))
{
}

ComponentActivationAudioHostV1::ComponentActivationAudioHostV1(
    ComponentActivationAudioHostV1&& other) noexcept
{
    *this = std::move(other);
}

ComponentActivationAudioHostV1& ComponentActivationAudioHostV1::operator=(
    ComponentActivationAudioHostV1&& other) noexcept
{
    if (this != &other)
    {
        plane_ = std::exchange(other.plane_, nullptr);
        binding_ = other.binding_;
        nextSequence_ = std::exchange(other.nextSequence_, 0);
        previousTransport_ = other.previousTransport_;
        hasPendingOutput_ = std::exchange(other.hasPendingOutput_, false);
        checkpoint_ = other.checkpoint_;
        restoredPending_ = other.restoredPending_;
        other.checkpoint_.reset();
        other.restoredPending_.reset();
    }
    return *this;
}

bool ComponentActivationAudioHostV1::checkpointAuthority(
    bool authorizationActive) noexcept
{
    if (plane_ != nullptr && !authorizationActive)
        latchQuarantine(*plane_, ComponentActivationAudioQuarantineReasonV1::Revoked);
    if (plane_ == nullptr || quarantined(*plane_))
    {
        checkpoint_.reset();
        restoredPending_.reset();
        return false;
    }
    return true;
}

ComponentActivationAudioCheckpointResultV1
ComponentActivationAudioHostV1::snapshotPendingOutput(PendingOutput& pending) noexcept
{
    using Result = ComponentActivationAudioCheckpointResultV1;
    pending = {};
    pending.hasPending = hasPendingOutput_;
    if (!hasPendingOutput_)
        return Result::Captured;
    if (restoredPending_)
    {
        pending = *restoredPending_;
        return Result::Captured;
    }
    pending.transport = previousTransport_;
    const auto sequence = nextSequence_ - 1U;
    const auto& slot = plane_->responses[static_cast<std::size_t>(
        (sequence - 1U) % componentActivationAudioSlotCountV1)];
    const auto publication = loadAtomic(slot.publication, std::memory_order_acquire);
    if (publication != publicationFor(sequence))
    {
        // Unlike a realtime callback, the control thread may wait for the
        // outstanding worker publication. Future identities are never valid.
        if (publication <= (publicationFor(sequence) | 1U))
            return Result::MissingResponse;
        latchQuarantine(*plane_, ComponentActivationAudioQuarantineReasonV1::ResponseBindingMismatch);
        return Result::Quarantined;
    }
    std::array<float*, maximumComponentActivationAudioChannelsV1> outputs{};
    for (std::size_t channel = 0; channel < binding_.outputChannelCount; ++channel)
        outputs[channel] = pending.samples.data()
            + channel * maximumComponentActivationAudioFramesV1;
    const auto result = consumeResponse(*plane_, binding_, sequence,
        previousTransport_, {outputs.data(), binding_.outputChannelCount},
        binding_.blockFrames, nullptr);
    return result == ComponentActivationAudioOperationV1::Rendered
        ? Result::Captured : Result::Quarantined;
}

ComponentActivationAudioCheckpointResultV1
ComponentActivationAudioHostV1::beginOfflineCheckpoint(bool authorizationActive) noexcept
{
    using Result = ComponentActivationAudioCheckpointResultV1;
    if (!checkpointAuthority(authorizationActive))
        return Result::Quarantined;
    if (checkpoint_ || nextSequence_ == 0)
        return Result::InvalidState;
    PendingOutput pending;
    const auto result = snapshotPendingOutput(pending);
    if (result == Result::Captured)
        checkpoint_ = pending;
    else if (result == Result::Quarantined)
        (void)checkpointAuthority(true);
    return result;
}

ComponentActivationAudioCheckpointResultV1
ComponentActivationAudioHostV1::restoreOfflineCheckpoint(bool authorizationActive) noexcept
{
    using Result = ComponentActivationAudioCheckpointResultV1;
    if (!checkpointAuthority(authorizationActive))
        return Result::Quarantined;
    if (!checkpoint_ || nextSequence_ == 0)
        return Result::InvalidState;
    PendingOutput current;
    const auto result = snapshotPendingOutput(current);
    if (result != Result::Captured)
    {
        if (result == Result::Quarantined)
            (void)checkpointAuthority(true);
        return result;
    }
    hasPendingOutput_ = checkpoint_->hasPending;
    if (hasPendingOutput_)
        restoredPending_ = *checkpoint_;
    else
        restoredPending_.reset();
    checkpoint_.reset();
    return Result::Restored;
}

ComponentActivationAudioCheckpointResultV1
ComponentActivationAudioHostV1::commitOfflineCheckpoint(bool authorizationActive) noexcept
{
    using Result = ComponentActivationAudioCheckpointResultV1;
    if (!checkpointAuthority(authorizationActive))
        return Result::Quarantined;
    if (!checkpoint_ || nextSequence_ == 0)
        return Result::InvalidState;
    checkpoint_.reset();
    return Result::Committed;
}

ComponentActivationAudioCheckpointResultV1
ComponentActivationAudioHostV1::validateOfflineCheckpointBoundary(bool authorizationActive) noexcept
{
    using Result = ComponentActivationAudioCheckpointResultV1;
    if (!checkpointAuthority(authorizationActive)) return Result::Quarantined;
    if (nextSequence_ == 0) return Result::InvalidState;
    PendingOutput pending;
    const auto result = snapshotPendingOutput(pending);
    if (result == Result::Quarantined) (void)checkpointAuthority(true);
    return result;
}

std::uint64_t ComponentActivationAudioHostV1::lastSubmittedSequence() const noexcept
{
    return plane_ != nullptr && nextSequence_ > 0 ? nextSequence_ - 1U : 0;
}

std::optional<ComponentActivationAudioHostV1>
ComponentActivationAudioHostV1::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactStorage(storage) || !validAuthorization(authorization))
        return std::nullopt;
    auto* plane = planeFrom(storage);
    auto binding = bindingFor(authorization);
    if (!validHeader(*plane, binding))
        return std::nullopt;
    return ComponentActivationAudioHostV1{plane, std::move(binding)};
}

ComponentActivationAudioOperationV1 ComponentActivationAudioHostV1::process(
    std::span<const float* const> inputChannels,
    std::span<float* const> outputChannels,
    std::uint32_t frameCount,
    ComponentActivationAudioTransportV1 transport,
    bool authorizationActive) noexcept
{
    if (plane_ == nullptr
        || !validChannels(
            inputChannels, outputChannels, binding_, frameCount))
    {
        zeroOutputs(
            outputChannels,
            std::min(frameCount, binding_.blockFrames));
        if (plane_ != nullptr)
        {
            latchQuarantine(
                *plane_,
                ComponentActivationAudioQuarantineReasonV1::
                    InvalidCallbackProfile);
        }
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }
    zeroOutputs(outputChannels, frameCount);
    if (quarantined(*plane_))
        return ComponentActivationAudioOperationV1::Quarantined;
    if (!authorizationActive)
    {
        latchQuarantine(
            *plane_, ComponentActivationAudioQuarantineReasonV1::Revoked);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    if (nextSequence_ == 0
        || nextSequence_ > std::numeric_limits<std::uint64_t>::max() / 2U)
    {
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::SequenceExhausted);
        return ComponentActivationAudioOperationV1::SequenceExhausted;
    }
    if (transport.generation == 0 || transport.reserved0 != 0
        || (transport.flags & ~std::uint32_t{1}) != 0)
    {
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::
                InvalidCallbackProfile);
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }

    const auto firstBlock = !hasPendingOutput_;
    if (restoredPending_)
    {
        for (std::size_t channel = 0; channel < outputChannels.size(); ++channel)
            std::copy_n(restoredPending_->samples.data()
                    + channel * maximumComponentActivationAudioFramesV1,
                frameCount, outputChannels[channel]);
    }
    else if (!firstBlock)
    {
        const auto responseSequence = nextSequence_ - 1U;
        const auto consumed = consumeResponse(
            *plane_,
            binding_,
            responseSequence,
            previousTransport_,
            outputChannels,
            frameCount,
            nullptr);
        if (consumed != ComponentActivationAudioOperationV1::Rendered)
            return consumed;
    }

    if (!finiteInputs(inputChannels, frameCount))
    {
        zeroOutputs(outputChannels, frameCount);
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::InputNonFinite);
        return ComponentActivationAudioOperationV1::NonFinite;
    }
    const auto request = recordFor(
        ComponentActivationAudioRecordKindV1::Request,
        binding_,
        nextSequence_,
        transport,
        ComponentActivationAudioOutcomeV1::None);
    auto& requestSlot = plane_->requests[
        static_cast<std::size_t>((nextSequence_ - 1U)
            % componentActivationAudioSlotCountV1)];
    writeRequest(requestSlot, request, inputChannels);
    previousTransport_ = transport;
    hasPendingOutput_ = true;
    restoredPending_.reset();
    ++nextSequence_;
    return firstBlock
        ? ComponentActivationAudioOperationV1::FirstBlockSilence
        : ComponentActivationAudioOperationV1::Rendered;
}

ComponentActivationAudioOperationV1
ComponentActivationAudioHostV1::consumeFinal(
    std::span<float* const> outputChannels,
    std::uint32_t frameCount,
    bool authorizationActive,
    std::uint64_t& processingNanoseconds) noexcept
{
    processingNanoseconds = 0;
    if (plane_ == nullptr || frameCount != binding_.blockFrames
        || outputChannels.size() != binding_.outputChannelCount
        || std::ranges::any_of(outputChannels, [](const float* value) {
               return value == nullptr;
           }))
    {
        zeroOutputs(
            outputChannels,
            std::min(frameCount, binding_.blockFrames));
        if (plane_ != nullptr)
        {
            latchQuarantine(
                *plane_,
                ComponentActivationAudioQuarantineReasonV1::
                    InvalidCallbackProfile);
        }
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }
    zeroOutputs(outputChannels, frameCount);
    if (quarantined(*plane_))
        return ComponentActivationAudioOperationV1::Quarantined;
    if (!authorizationActive)
    {
        latchQuarantine(
            *plane_, ComponentActivationAudioQuarantineReasonV1::Revoked);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    if (nextSequence_ != 2 || checkpoint_ || restoredPending_
        || !hasPendingOutput_)
    {
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::
                InvalidCallbackProfile);
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }
    const auto consumed = consumeResponse(
        *plane_,
        binding_,
        1,
        previousTransport_,
        outputChannels,
        frameCount,
        &processingNanoseconds);
    if (consumed == ComponentActivationAudioOperationV1::Rendered)
    {
        nextSequence_ = 0;
        hasPendingOutput_ = false;
    }
    return consumed;
}

void ComponentActivationAudioHostV1::quarantine(
    ComponentActivationAudioQuarantineReasonV1 reason) noexcept
{
    if (plane_ != nullptr)
        latchQuarantine(*plane_, reason);
}

bool ComponentActivationAudioHostV1::isQuarantined() const noexcept
{
    return plane_ == nullptr || quarantined(*plane_);
}

ComponentActivationAudioQuarantineReasonV1
ComponentActivationAudioHostV1::quarantineReason() const noexcept
{
    if (plane_ == nullptr)
        return ComponentActivationAudioQuarantineReasonV1::ExplicitControl;
    const auto reason = static_cast<ComponentActivationAudioQuarantineReasonV1>(
        loadAtomic(plane_->quarantineReason, std::memory_order_acquire));
    return knownQuarantineReason(reason)
        ? reason
        : ComponentActivationAudioQuarantineReasonV1::ExplicitControl;
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
bool ComponentActivationAudioHostV1::publishedResponseReady() const noexcept
{
    if (plane_ == nullptr || !hasPendingOutput_ || quarantined(*plane_))
        return false;
    if (restoredPending_)
        return true;
    const auto sequence = nextSequence_ - 1U;
    const auto& slot = plane_->responses[
        static_cast<std::size_t>((sequence - 1U)
            % componentActivationAudioSlotCountV1)];
    return loadAtomic(slot.publication, std::memory_order_acquire)
        == publicationFor(sequence);
}
#endif

ComponentActivationAudioWorkerPlaneV1::
    ComponentActivationAudioWorkerPlaneV1(
        ComponentActivationAudioSharedPlaneV1* plane,
        ComponentActivationAudioBindingV1 binding) noexcept
    : plane_(plane), binding_(std::move(binding))
{
}

std::optional<ComponentActivationAudioWorkerPlaneV1>
ComponentActivationAudioWorkerPlaneV1::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactStorage(storage) || !validAuthorization(authorization))
        return std::nullopt;
    auto* plane = planeFrom(storage);
    auto binding = bindingFor(authorization);
    if (!validHeader(*plane, binding))
        return std::nullopt;
    return ComponentActivationAudioWorkerPlaneV1{
        plane, std::move(binding)};
}

bool ComponentActivationAudioWorkerPlaneV1::isQuiescentAtSequence(
    std::uint64_t sequence) const noexcept
{
    if (plane_ == nullptr || quarantined(*plane_) || requestAcquired_
        || nextSequence_ == 0 || sequence != nextSequence_ - 1U)
        return false;
    const auto& next = plane_->requests[static_cast<std::size_t>(
        (nextSequence_ - 1U) % componentActivationAudioSlotCountV1)];
    const auto publication = loadAtomic(next.publication, std::memory_order_acquire);
    return (publication & 1U) == 0 && publication < publicationFor(nextSequence_);
}

ComponentActivationAudioOperationV1
ComponentActivationAudioWorkerPlaneV1::acquire(
    ComponentActivationAudioWorkerBlockV1& block) noexcept
{
    block = {};
    if (plane_ == nullptr || quarantined(*plane_))
        return ComponentActivationAudioOperationV1::Quarantined;
    if (requestAcquired_ || nextSequence_ == 0
        || nextSequence_ > std::numeric_limits<std::uint64_t>::max() / 2U)
    {
        if (nextSequence_ == 0
            || nextSequence_ > std::numeric_limits<std::uint64_t>::max() / 2U)
        {
            latchQuarantine(
                *plane_,
                ComponentActivationAudioQuarantineReasonV1::
                    SequenceExhausted);
            return ComponentActivationAudioOperationV1::SequenceExhausted;
        }
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }
    auto& slot = plane_->requests[
        static_cast<std::size_t>((nextSequence_ - 1U)
            % componentActivationAudioSlotCountV1)];
    const auto expectedPublication = publicationFor(nextSequence_);
    const auto publication = loadAtomic(
        slot.publication, std::memory_order_acquire);
    if (publication != expectedPublication)
        return ComponentActivationAudioOperationV1::Missing;
    const auto record = loadObject(slot.record);
    if (!validRecord(
            record,
            ComponentActivationAudioRecordKindV1::Request,
            binding_,
            nextSequence_))
    {
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::
                RequestBindingMismatch);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    block.sequence = record.sequence;
    block.transport = record.transport;
    block.frameCount = record.frameCount;
    block.inputChannelCount = record.inputChannelCount;
    block.outputChannelCount = record.outputChannelCount;
    acquiredTransport_ = record.transport;
    bool finite = true;
    for (std::size_t channel = 0;
         channel < block.inputChannelCount;
         ++channel)
    {
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
        {
            const auto index =
                channel * maximumComponentActivationAudioFramesV1 + frame;
            block.inputSamples[index] = loadAtomic(
                slot.samples[index], std::memory_order_relaxed);
            finite = finite && std::isfinite(block.inputSamples[index]);
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(slot.publication, std::memory_order_relaxed)
        != publication)
    {
        block = {};
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::
                RequestSnapshotUnstable);
        return ComponentActivationAudioOperationV1::Quarantined;
    }
    if (!finite)
    {
        block = {};
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::InputNonFinite);
        return ComponentActivationAudioOperationV1::NonFinite;
    }
    requestAcquired_ = true;
    return ComponentActivationAudioOperationV1::Acquired;
}

ComponentActivationAudioOperationV1
ComponentActivationAudioWorkerPlaneV1::publish(
    const ComponentActivationAudioWorkerBlockV1& block) noexcept
{
    if (plane_ == nullptr || quarantined(*plane_))
        return ComponentActivationAudioOperationV1::Quarantined;
    if (!requestAcquired_ || block.sequence != nextSequence_
        || block.frameCount != binding_.blockFrames
        || block.inputChannelCount != binding_.inputChannelCount
        || block.outputChannelCount != binding_.outputChannelCount
        || block.transport != acquiredTransport_)
    {
        latchQuarantine(
            *plane_,
            ComponentActivationAudioQuarantineReasonV1::
                RequestBindingMismatch);
        return ComponentActivationAudioOperationV1::InvalidArgument;
    }
    for (std::size_t channel = 0;
         channel < block.outputChannelCount;
         ++channel)
    {
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
        {
            const auto index =
                channel * maximumComponentActivationAudioFramesV1 + frame;
            if (!std::isfinite(block.outputSamples[index]))
            {
                latchQuarantine(
                    *plane_,
                    ComponentActivationAudioQuarantineReasonV1::
                        WorkerOutputNonFinite);
                return ComponentActivationAudioOperationV1::NonFinite;
            }
        }
    }
    auto response = recordFor(
        ComponentActivationAudioRecordKindV1::Response,
        binding_,
        nextSequence_,
        block.transport,
        ComponentActivationAudioOutcomeV1::Ready);
    response.processingNanoseconds = block.processingNanoseconds;
    auto& slot = plane_->responses[
        static_cast<std::size_t>((nextSequence_ - 1U)
            % componentActivationAudioSlotCountV1)];
    writeResponse(slot, response, block);
    requestAcquired_ = false;
    acquiredTransport_ = {};
    ++nextSequence_;
    return ComponentActivationAudioOperationV1::Published;
}

void ComponentActivationAudioWorkerPlaneV1::quarantine(
    ComponentActivationAudioQuarantineReasonV1 reason) noexcept
{
    if (plane_ != nullptr)
        latchQuarantine(*plane_, reason);
}

bool ComponentActivationAudioWorkerPlaneV1::isQuarantined() const noexcept
{
    return plane_ == nullptr || quarantined(*plane_);
}

namespace
{
[[nodiscard]] bool exactHealthStorage(
    std::span<std::byte> storage) noexcept
{
    return storage.size()
            == componentActivationAudioStreamHealthPlaneBytesV1
        && storage.data() != nullptr
        && reinterpret_cast<std::uintptr_t>(storage.data())
            % alignof(ComponentActivationAudioStreamHealthSharedPlaneV1)
            == 0;
}

[[nodiscard]] ComponentActivationAudioStreamHealthSharedPlaneV1*
healthPlaneFrom(std::span<std::byte> storage) noexcept
{
    return reinterpret_cast<
        ComponentActivationAudioStreamHealthSharedPlaneV1*>(storage.data());
}

[[nodiscard]] bool validHealthState(
    const ComponentActivationAudioStreamHealthV1& health) noexcept
{
    return health.seekGeneration != 0U
        && health.bufferedBlocks
            <= componentActivationAudioResponseCapacityV1
        && health.pendingRequests
            <= componentActivationAudioRequestCapacityV1
        && health.decodeFailures == 0U
        && health.workerState
            <= ComponentActivationAudioStreamWorkerStateV1::Stopped
        && health.workerState
            != ComponentActivationAudioStreamWorkerStateV1::Failed
        && health.workerState
            != ComponentActivationAudioStreamWorkerStateV1::Stopped
        && health.reserved0 == 0U
        && std::ranges::all_of(
            health.reserved1,
            [](std::uint64_t value) { return value == 0U; });
}

[[nodiscard]] bool monotonicHealth(
    const ComponentActivationAudioStreamHealthV1& previous,
    const ComponentActivationAudioStreamHealthV1& next) noexcept
{
    return next.underrunEvents >= previous.underrunEvents
        && next.missingFrames >= previous.missingFrames
        && next.discontinuities >= previous.discontinuities
        && next.requestRejections >= previous.requestRejections
        && next.responseFullEvents >= previous.responseFullEvents
        && next.staleBlocks >= previous.staleBlocks
        && next.decodedBlocks >= previous.decodedBlocks
        && next.decodeFailures >= previous.decodeFailures
        && next.endOfStreamFrames >= previous.endOfStreamFrames
        && next.seekGeneration >= previous.seekGeneration;
}

[[nodiscard]] bool validHealthHeader(
    ComponentActivationAudioStreamHealthSharedPlaneV1& plane,
    const ComponentActivationAudioBindingV1& binding) noexcept
{
    const auto publication = loadAtomic(
        plane.publication, std::memory_order_acquire);
    if ((publication & 1U) != 0U)
        return false;
    const auto magic = loadAtomic(plane.magic, std::memory_order_relaxed);
    const auto protocol = loadAtomic(
        plane.protocolVersion, std::memory_order_relaxed);
    const auto headerBytes = loadAtomic(
        plane.headerBytes, std::memory_order_relaxed);
    const auto planeBytes = loadAtomic(
        plane.planeBytes, std::memory_order_relaxed);
    const auto observedBinding = loadObject(plane.binding);
    const auto reserved0 = loadAtomic(
        plane.reserved0, std::memory_order_relaxed);
    const auto reserved1 = loadObject(plane.reserved1);
    std::atomic_thread_fence(std::memory_order_acquire);
    return loadAtomic(plane.publication, std::memory_order_relaxed)
            == publication
        && magic == componentActivationAudioStreamHealthPlaneMagicV1
        && protocol
            == componentActivationAudioStreamHealthProtocolVersionV1
        && headerBytes
            == offsetof(
                ComponentActivationAudioStreamHealthSharedPlaneV1, record)
        && planeBytes
            == sizeof(ComponentActivationAudioStreamHealthSharedPlaneV1)
        && observedBinding == binding
        && reserved0 == 0U
        && std::ranges::all_of(
            reserved1,
            [](std::uint64_t value) { return value == 0U; });
}
}

ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioStreamHealthPlaneV1(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactHealthStorage(storage))
        return ComponentActivationAudioDiagnosticV1::InvalidStorage;
    if (!validAuthorization(authorization))
        return ComponentActivationAudioDiagnosticV1::AuthorizationInvalid;
    if (!pristine(storage))
        return ComponentActivationAudioDiagnosticV1::StorageNotPristine;
    auto* plane = std::construct_at(healthPlaneFrom(storage));
    plane->magic = componentActivationAudioStreamHealthPlaneMagicV1;
    plane->protocolVersion =
        componentActivationAudioStreamHealthProtocolVersionV1;
    plane->headerBytes = offsetof(
        ComponentActivationAudioStreamHealthSharedPlaneV1, record);
    plane->planeBytes =
        sizeof(ComponentActivationAudioStreamHealthSharedPlaneV1);
    plane->binding = bindingFor(authorization);
    std::atomic_thread_fence(std::memory_order_release);
    return ComponentActivationAudioDiagnosticV1::Ready;
}

std::optional<ComponentActivationAudioStreamHealthProducerV1>
ComponentActivationAudioStreamHealthProducerV1::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactHealthStorage(storage) || !validAuthorization(authorization))
        return std::nullopt;
    auto* plane = healthPlaneFrom(storage);
    if (!validHealthHeader(*plane, bindingFor(authorization)))
        return std::nullopt;
    return ComponentActivationAudioStreamHealthProducerV1{plane};
}

bool ComponentActivationAudioStreamHealthProducerV1::publish(
    const ComponentActivationAudioStreamHealthV1& health) noexcept
{
    if (plane_ == nullptr || !validHealthState(health)
        || (published_ && !monotonicHealth(previous_, health)))
    {
        return false;
    }
    const auto publication = loadAtomic(
        plane_->publication, std::memory_order_acquire);
    if ((publication & 1U) != 0U
        || publication > std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        return false;
    }
    ComponentActivationAudioStreamHealthRecordV1 record;
    record.sequence = publication / 2U + 1U;
    record.health = health;
    atomic(plane_->publication).store(
        record.sequence * 2U - 1U, std::memory_order_release);
    storeObject(plane_->record, record);
    std::atomic_thread_fence(std::memory_order_release);
    atomic(plane_->publication).store(
        record.sequence * 2U, std::memory_order_release);
    previous_ = health;
    published_ = true;
    return true;
}

std::optional<ComponentActivationAudioStreamHealthConsumerV1>
ComponentActivationAudioStreamHealthConsumerV1::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization) noexcept
{
    if (!exactHealthStorage(storage) || !validAuthorization(authorization))
        return std::nullopt;
    auto* plane = healthPlaneFrom(storage);
    if (!validHealthHeader(*plane, bindingFor(authorization)))
        return std::nullopt;
    return ComponentActivationAudioStreamHealthConsumerV1{plane};
}

std::optional<ComponentActivationAudioStreamHealthV1>
ComponentActivationAudioStreamHealthConsumerV1::read() noexcept
{
    const auto result = readDetailed();
    return result.diagnostic
            == ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid
        ? std::optional{result.health}
        : std::nullopt;
}

ComponentActivationAudioStreamHealthReadResultV1
ComponentActivationAudioStreamHealthConsumerV1::readDetailed() noexcept
{
    ComponentActivationAudioStreamHealthReadResultV1 result;
    if (plane_ == nullptr)
        return result;
    const auto publication = loadAtomic(
        plane_->publication, std::memory_order_acquire);
    if (publication == 0U)
        return result;
    if ((publication & 1U) != 0U)
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy;
        return result;
    }
    const auto record = loadObject(plane_->record);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(plane_->publication, std::memory_order_relaxed)
        != publication)
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy;
        return result;
    }
    if (record.sequence != publication / 2U
        || !validHealthState(record.health)
        || record.sequence < previousSequence_
        || (record.sequence == previousSequence_
            && record.health != previous_)
        || (previousSequence_ != 0U
            && !monotonicHealth(previous_, record.health)))
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Invalid;
        return result;
    }
    previousSequence_ = record.sequence;
    previous_ = record.health;
    result.diagnostic =
        ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid;
    result.health = record.health;
    return result;
}

namespace
{
[[nodiscard]] bool exactHealthStorageV2(
    std::span<std::byte> storage) noexcept
{
    return storage.size()
            == componentActivationAudioStreamHealthPlaneBytesV2
        && storage.data() != nullptr
        && reinterpret_cast<std::uintptr_t>(storage.data())
            % alignof(ComponentActivationAudioStreamHealthSharedPlaneV2)
            == 0;
}

[[nodiscard]] ComponentActivationAudioStreamHealthSharedPlaneV2*
healthPlaneV2From(std::span<std::byte> storage) noexcept
{
    return reinterpret_cast<
        ComponentActivationAudioStreamHealthSharedPlaneV2*>(storage.data());
}

template <typename Bytes>
[[nodiscard]] bool zeroBytes(const Bytes& bytes) noexcept
{
    return std::ranges::all_of(bytes, [](auto value) { return value == 0; });
}

[[nodiscard]] bool validHealthSlotBindingV2(
    const ComponentActivationAudioStreamHealthSlotBindingV2& binding,
    const ComponentActivationAudioStreamHealthSetBindingV2& set,
    std::size_t slot) noexcept
{
    return slot < set.slotCount
        && binding.resourceSetDigest == set.resourceSetDigest
        && !zeroBytes(binding.memberGrantDigest)
        && !zeroBytes(binding.sourceContentDigest)
        && binding.resourceGeneration != 0U
        && binding.memberOrdinal < set.memberCount
        && binding.audioSlotOrdinal == slot
        && binding.memberCount == set.memberCount
        && binding.reserved0 == 0U
        && zeroBytes(binding.reserved1);
}

[[nodiscard]] bool validHealthSetBindingV2(
    const ComponentActivationAudioStreamHealthSetBindingV2& binding) noexcept
{
    if (zeroBytes(binding.resourceSetDigest)
        || binding.memberCount == 0U
        || binding.memberCount
            > maximumComponentActivationResourceSetMembersV1
        || binding.slotCount == 0U
        || binding.slotCount
            > maximumComponentActivationResourceSetAudioStreamsV1
        || binding.slotCount > binding.memberCount
        || binding.reserved0 != 0U || !zeroBytes(binding.reserved1))
    {
        return false;
    }
    for (std::size_t slot = 0; slot < binding.slots.size(); ++slot)
    {
        if (slot < binding.slotCount)
        {
            if (!validHealthSlotBindingV2(binding.slots[slot], binding, slot))
                return false;
            for (std::size_t prior = 0; prior < slot; ++prior)
            {
                if (binding.slots[prior].memberOrdinal
                    == binding.slots[slot].memberOrdinal)
                {
                    return false;
                }
            }
        }
        else if (binding.slots[slot]
            != ComponentActivationAudioStreamHealthSlotBindingV2{})
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validHealthHeaderV2(
    ComponentActivationAudioStreamHealthSharedPlaneV2& plane,
    const ComponentActivationAudioBindingV1& binding,
    const ComponentActivationAudioStreamHealthSetBindingV2& setBinding)
    noexcept
{
    const auto publication = loadAtomic(
        plane.publication, std::memory_order_acquire);
    if (publication != 2U)
        return false;
    const auto magic = loadAtomic(plane.magic, std::memory_order_relaxed);
    const auto protocol = loadAtomic(
        plane.protocolVersion, std::memory_order_relaxed);
    const auto headerBytes = loadAtomic(
        plane.headerBytes, std::memory_order_relaxed);
    const auto planeBytes = loadAtomic(
        plane.planeBytes, std::memory_order_relaxed);
    const auto reserved0 = loadAtomic(
        plane.reserved0, std::memory_order_relaxed);
    const auto observedBinding = loadObject(plane.binding);
    const auto observedSetBinding = loadObject(plane.setBinding);
    const auto reserved1 = loadObject(plane.reserved1);
    std::atomic_thread_fence(std::memory_order_acquire);
    return loadAtomic(plane.publication, std::memory_order_relaxed)
            == publication
        && magic == componentActivationAudioStreamHealthPlaneMagicV1
        && protocol
            == componentActivationAudioStreamHealthProtocolVersionV2
        && headerBytes
            == offsetof(
                ComponentActivationAudioStreamHealthSharedPlaneV2, slots)
        && planeBytes
            == sizeof(ComponentActivationAudioStreamHealthSharedPlaneV2)
        && reserved0 == 0U && observedBinding == binding
        && observedSetBinding == setBinding
        && validHealthSetBindingV2(observedSetBinding)
        && zeroBytes(reserved1);
}
}

std::optional<ComponentActivationAudioStreamHealthSetBindingV2>
componentActivationAudioStreamHealthSetBindingV2(
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationSealedSessionV1& session,
    const ComponentActivationResourceSetGrantV1& grant,
    std::span<const ComponentActivationResourceSetMemberV1> members) noexcept
{
    if (validateComponentActivationAuthorizationV1(authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || validateComponentActivationResourceSetGrantV1(
               grant, authorization, session)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || validateComponentActivationResourceSetMembersV1(grant, members)
            != ComponentActivationProtocolDiagnosticV1::Valid
        || grant.audioStreamCount == 0U)
    {
        return std::nullopt;
    }
    ComponentActivationAudioStreamHealthSetBindingV2 result;
    result.resourceSetDigest = grant.resourceSetDigest;
    result.memberCount = grant.memberCount;
    result.slotCount = grant.audioStreamCount;
    std::size_t audioSlot = 0;
    for (const auto& member : members)
    {
        if (static_cast<ComponentActivationResourceSetMemberKindV1>(
                member.memberKind)
            != ComponentActivationResourceSetMemberKindV1::Audio)
        {
            continue;
        }
        if (audioSlot >= result.slots.size())
            return std::nullopt;
        auto& slot = result.slots[audioSlot];
        slot.resourceSetDigest = grant.resourceSetDigest;
        slot.memberGrantDigest = member.memberGrantDigest;
        slot.sourceContentDigest = member.sourceContentDigest;
        slot.resourceGeneration = member.resourceGeneration;
        slot.memberOrdinal = member.memberOrdinal;
        slot.audioSlotOrdinal = static_cast<std::uint16_t>(audioSlot);
        slot.memberCount = grant.memberCount;
        ++audioSlot;
    }
    return audioSlot == grant.audioStreamCount
            && validHealthSetBindingV2(result)
        ? std::optional{result}
        : std::nullopt;
}

ComponentActivationAudioDiagnosticV1
initializeComponentActivationAudioStreamHealthPlaneV2(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationAudioStreamHealthSetBindingV2& binding) noexcept
{
    if (!exactHealthStorageV2(storage))
        return ComponentActivationAudioDiagnosticV1::InvalidStorage;
    if (!validAuthorization(authorization))
        return ComponentActivationAudioDiagnosticV1::AuthorizationInvalid;
    if (!validHealthSetBindingV2(binding))
        return ComponentActivationAudioDiagnosticV1::BindingMismatch;
    if (!pristine(storage))
        return ComponentActivationAudioDiagnosticV1::StorageNotPristine;
    auto* plane = std::construct_at(healthPlaneV2From(storage));
    atomic(plane->publication).store(1U, std::memory_order_release);
    plane->magic = componentActivationAudioStreamHealthPlaneMagicV1;
    plane->protocolVersion =
        componentActivationAudioStreamHealthProtocolVersionV2;
    plane->headerBytes = offsetof(
        ComponentActivationAudioStreamHealthSharedPlaneV2, slots);
    plane->planeBytes =
        sizeof(ComponentActivationAudioStreamHealthSharedPlaneV2);
    plane->binding = bindingFor(authorization);
    plane->setBinding = binding;
    std::atomic_thread_fence(std::memory_order_release);
    atomic(plane->publication).store(2U, std::memory_order_release);
    return ComponentActivationAudioDiagnosticV1::Ready;
}

std::optional<ComponentActivationAudioStreamHealthProducerV2>
ComponentActivationAudioStreamHealthProducerV2::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationAudioStreamHealthSetBindingV2& binding) noexcept
{
    if (!exactHealthStorageV2(storage) || !validAuthorization(authorization)
        || !validHealthSetBindingV2(binding))
    {
        return std::nullopt;
    }
    auto* plane = healthPlaneV2From(storage);
    if (!validHealthHeaderV2(*plane, bindingFor(authorization), binding))
        return std::nullopt;
    return ComponentActivationAudioStreamHealthProducerV2{plane, binding};
}

bool ComponentActivationAudioStreamHealthProducerV2::publish(
    std::size_t slot,
    const ComponentActivationAudioStreamHealthV1& health) noexcept
{
    if (plane_ == nullptr || slot >= binding_.slotCount
        || !validHealthState(health)
        || (published_[slot] && !monotonicHealth(previous_[slot], health)))
    {
        return false;
    }
    auto& destination = plane_->slots[slot];
    const auto publication = loadAtomic(
        destination.publication, std::memory_order_acquire);
    if ((publication & 1U) != 0U
        || publication > std::numeric_limits<std::uint64_t>::max() - 2U)
    {
        return false;
    }
    ComponentActivationAudioStreamHealthRecordV2 record;
    record.sequence = publication / 2U + 1U;
    record.binding = binding_.slots[slot];
    record.health = health;
    atomic(destination.publication).store(
        record.sequence * 2U - 1U, std::memory_order_release);
    storeObject(destination.record, record);
    std::atomic_thread_fence(std::memory_order_release);
    atomic(destination.publication).store(
        record.sequence * 2U, std::memory_order_release);
    previous_[slot] = health;
    published_[slot] = true;
    return true;
}

std::optional<ComponentActivationAudioStreamHealthConsumerV2>
ComponentActivationAudioStreamHealthConsumerV2::attach(
    std::span<std::byte> storage,
    const ComponentActivationAuthorizationV1& authorization,
    const ComponentActivationAudioStreamHealthSetBindingV2& binding) noexcept
{
    if (!exactHealthStorageV2(storage) || !validAuthorization(authorization)
        || !validHealthSetBindingV2(binding))
    {
        return std::nullopt;
    }
    auto* plane = healthPlaneV2From(storage);
    if (!validHealthHeaderV2(*plane, bindingFor(authorization), binding))
        return std::nullopt;
    return ComponentActivationAudioStreamHealthConsumerV2{plane, binding};
}

std::optional<ComponentActivationAudioStreamHealthV1>
ComponentActivationAudioStreamHealthConsumerV2::read(
    std::size_t slot) noexcept
{
    const auto result = readDetailed(slot);
    return result.diagnostic
            == ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid
        ? std::optional{result.health}
        : std::nullopt;
}

ComponentActivationAudioStreamHealthReadResultV1
ComponentActivationAudioStreamHealthConsumerV2::readDetailed(
    std::size_t slot) noexcept
{
    ComponentActivationAudioStreamHealthReadResultV1 result;
    if (plane_ == nullptr || slot >= binding_.slotCount)
        return result;
    auto& source = plane_->slots[slot];
    const auto publication = loadAtomic(
        source.publication, std::memory_order_acquire);
    if (publication == 0U)
        return result;
    if ((publication & 1U) != 0U)
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy;
        return result;
    }
    const auto record = loadObject(source.record);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (loadAtomic(source.publication, std::memory_order_relaxed)
        != publication)
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy;
        return result;
    }
    if (record.sequence != publication / 2U
        || record.binding != binding_.slots[slot]
        || !validHealthState(record.health)
        || record.sequence < previousSequence_[slot]
        || (record.sequence == previousSequence_[slot]
            && record.health != previous_[slot])
        || (previousSequence_[slot] != 0U
            && !monotonicHealth(previous_[slot], record.health)))
    {
        result.diagnostic =
            ComponentActivationAudioStreamHealthReadDiagnosticV1::Invalid;
        return result;
    }
    previousSequence_[slot] = record.sequence;
    previous_[slot] = record.health;
    result.diagnostic =
        ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid;
    result.health = record.health;
    return result;
}

std::uint64_t
ComponentActivationAudioStreamHealthConsumerV2::lastAcceptedSequence(
    std::size_t slot) const noexcept
{
    return plane_ != nullptr && slot < binding_.slotCount
        ? previousSequence_[slot] : 0U;
}

std::uint16_t
ComponentActivationAudioStreamHealthConsumerV2::slotCount() const noexcept
{
    return plane_ != nullptr ? binding_.slotCount : 0U;
}
}
