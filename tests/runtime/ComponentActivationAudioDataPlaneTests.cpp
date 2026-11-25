#include "talas/runtime/ComponentActivationAudioDataPlane.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <new>
#include <utility>

namespace { thread_local std::size_t ordinaryAllocations = 0; }
void* operator new(std::size_t size)
{
    ++ordinaryAllocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
namespace activation = talas::runtime::component_activation;
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

activation::ComponentActivationSealedSessionV1 sealedSession()
{
    activation::ComponentActivationSealedSessionV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::SealedSession,
        sizeof(value));
    value.profileVersion = activation::componentActivationProfileVersionV1;
    value.supportedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.safeguards = allComponentIsolationSafeguardsV1;
    value.runtimeSessionGeneration = 17;
    value.sharedStorageIdentity = 23;
    value.sharedStorageBytes = 64U * 1024U;
    value.runtimeSessionNonce.fill(0x33);
    return value;
}

activation::ComponentActivationAuthorizationV1 authorization(
    std::uint32_t sampleRate,
    std::uint32_t blockFrames,
    std::uint16_t channels)
{
    const auto sealed = sealedSession();
    activation::ComponentActivationAuthorizationV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::Authorization,
        sizeof(value));
    constexpr std::string_view bundleId = "org.talas.activation";
    constexpr std::string_view version = "1.2.3";
    constexpr std::string_view descriptor = "org.talas.activation.gain";
    value.bundleIdBytes = bundleId.size();
    value.semanticVersionBytes = version.size();
    value.descriptorIdBytes = descriptor.size();
    std::copy(bundleId.begin(), bundleId.end(), value.bundleId.begin());
    std::copy(version.begin(), version.end(), value.semanticVersion.begin());
    std::copy(
        descriptor.begin(), descriptor.end(), value.descriptorId.begin());
    value.contentDigest.fill(0x11);
    value.signerIdentityDigest.fill(0x22);
    value.entryDigest.fill(0x44);
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles = componentRole(
        ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = currentComponentTrustPolicyVersion;
    value.sampleRate = sampleRate;
    value.blockFrames = blockFrames;
    value.inputChannelCount = channels;
    value.outputChannelCount = channels;
    value.entryByteCount = 4096;
    value.artifactGeneration = 29;
    value.activationGeneration = 31;
    value.runtimeSessionGeneration = sealed.runtimeSessionGeneration;
    value.sharedStorageIdentity = sealed.sharedStorageIdentity;
    value.validationNonce.fill(0x5c);
    value.runtimeSessionNonce = sealed.runtimeSessionNonce;
    value.authorizationNonce.fill(0xa5);
    return value;
}

struct SharedStorage
{
    alignas(activation::ComponentActivationAudioSharedPlaneV1)
        std::array<std::byte, activation::componentActivationAudioPlaneBytesV1>
            bytes{};
};

struct HealthStorage
{
    alignas(activation::ComponentActivationAudioStreamHealthSharedPlaneV1)
        std::array<
            std::byte,
            activation::componentActivationAudioStreamHealthPlaneBytesV1>
            bytes{};
};

struct HealthStorageV2
{
    alignas(activation::ComponentActivationAudioStreamHealthSharedPlaneV2)
        std::array<
            std::byte,
            activation::componentActivationAudioStreamHealthPlaneBytesV2>
            bytes{};
};

struct ResourceSetHealthFixture
{
    activation::ComponentActivationSealedSessionV1 session;
    activation::ComponentActivationAuthorizationV1 authority;
    activation::ComponentActivationResourceSetGrantV1 grant;
    std::array<activation::ComponentActivationResourceSetMemberV1, 2>
        members{};
    activation::ComponentActivationAudioStreamHealthSetBindingV2 binding;

    ResourceSetHealthFixture()
    {
        session = sealedSession();
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::ImmutableResourceRead);
        session.supportedPermissions |= componentPermission(
            ComponentExecutionPermission::BoundedAudioStreamRead);
        authority = authorization(48'000U, 256U, 1U);
        authority.requestedPermissions = session.supportedPermissions;
        authority.declaredRoles =
            componentRole(ComponentExecutionRole::RealtimeProcessor)
            | componentRole(
                ComponentExecutionRole::ImmutableResourceConsumer)
            | componentRole(
                ComponentExecutionRole::BoundedAudioStreamConsumer);
        authority.staticParameterCount = 2U;
        authority.staticParameterDigest.fill(0x71U);

        constexpr std::array<std::string_view, 2> parameterIds{
            "audio-a", "audio-b"};
        constexpr std::array<std::string_view, 2> resourceIds{
            "Audio/Voice-A", "Audio/Voice-B"};
        for (std::size_t index = 0; index < members.size(); ++index)
        {
            auto& member = members[index];
            member.header = activation::componentActivationHeaderV1(
                activation::ComponentActivationRecordKindV1::ResourceSetMember,
                sizeof(member));
            member.memberGrantDigest.fill(
                static_cast<std::uint8_t>(0x41U + index));
            member.sourceContentDigest.fill(
                static_cast<std::uint8_t>(0x51U + index));
            member.mediatedPayloadDigest = member.sourceContentDigest;
            member.resourceGeneration = 7U + index;
            member.sourceByteCount = 4'096U;
            member.transportByteCount = 4'096U;
            member.privateMappedByteCount = 4'096U;
            member.workingByteCount =
                activation::maximumComponentActivationAudioStreamWorkingBytesV1;
            member.parameterIdBytes = parameterIds[index].size();
            member.resourceIdBytes = resourceIds[index].size();
            member.memberKind = static_cast<std::uint32_t>(
                activation::ComponentActivationResourceSetMemberKindV1::Audio);
            member.memberOrdinal = static_cast<std::uint16_t>(index);
            member.memberCount = members.size();
            std::copy(
                parameterIds[index].begin(),
                parameterIds[index].end(),
                member.parameterId.begin());
            std::copy(
                resourceIds[index].begin(),
                resourceIds[index].end(),
                member.resourceId.begin());
        }
        grant.header = activation::componentActivationHeaderV1(
            activation::ComponentActivationRecordKindV1::ResourceSetGrant,
            sizeof(grant));
        grant.contentDigest = authority.contentDigest;
        grant.validationNonce = authority.validationNonce;
        grant.runtimeSessionNonce = authority.runtimeSessionNonce;
        grant.authorizationNonce = authority.authorizationNonce;
        grant.artifactGeneration = authority.artifactGeneration;
        grant.activationGeneration = authority.activationGeneration;
        grant.runtimeSessionGeneration = authority.runtimeSessionGeneration;
        grant.sharedStorageIdentity = authority.sharedStorageIdentity;
        grant.aggregateSourceBytes = 8'192U;
        grant.aggregateTransportBytes = 8'192U;
        grant.aggregatePrivateMappedBytes = 8'192U;
        grant.aggregateWorkingBytes = 2U
            * activation::maximumComponentActivationAudioStreamWorkingBytesV1;
        grant.profileVersion =
            activation::componentActivationResourceSetProfileVersionV1;
        grant.memberCount = members.size();
        grant.audioStreamCount = members.size();
        grant.resourceSetDigest =
            activation::calculateComponentActivationResourceSetDigestV1(
                grant, members);
        for (auto& member : members)
            member.resourceSetDigest = grant.resourceSetDigest;
        const auto authored =
            activation::componentActivationAudioStreamHealthSetBindingV2(
                authority, session, grant, members);
        if (!authored.has_value())
        {
            std::cerr << "health-v2 fixture diagnostics auth="
                      << static_cast<unsigned>(
                             activation::validateComponentActivationAuthorizationV1(
                                 authority, session))
                      << " grant="
                      << static_cast<unsigned>(
                             activation::validateComponentActivationResourceSetGrantV1(
                                 grant, authority, session))
                      << " members="
                      << static_cast<unsigned>(
                             activation::validateComponentActivationResourceSetMembersV1(
                                 grant, members))
                      << '\n';
        }
        require(
            authored.has_value(),
            "resource-set health binding fixture should author exactly");
        binding = *authored;
    }
};

void testAudioStreamHealthPlaneV2AuthenticatesIndependentSlots()
{
    ResourceSetHealthFixture fixture;
    HealthStorageV2 storage;
    require(
        activation::initializeComponentActivationAudioStreamHealthPlaneV2(
            storage.bytes, fixture.authority, fixture.binding)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "resource-set health plane should initialize exactly once");
    auto producer =
        activation::ComponentActivationAudioStreamHealthProducerV2::attach(
            storage.bytes, fixture.authority, fixture.binding);
    auto consumer =
        activation::ComponentActivationAudioStreamHealthConsumerV2::attach(
            storage.bytes, fixture.authority, fixture.binding);
    require(
        producer.has_value() && consumer.has_value()
            && consumer->slotCount() == 2U,
        "resource-set health endpoints should bind both fixed slots");

    activation::ComponentActivationAudioStreamHealthV1 first;
    first.seekGeneration = 1U;
    first.decodedBlocks = 4U;
    first.bufferedBlocks = 4U;
    first.workerState =
        activation::ComponentActivationAudioStreamWorkerStateV1::Ready;
    auto second = first;
    second.seekGeneration = 3U;
    second.decodedBlocks = 7U;
    second.bufferedBlocks = 2U;
    require(
        producer->publish(0U, first) && producer->publish(1U, second),
        "each health slot should publish independently");
    const auto observedFirst = consumer->read(0U);
    const auto observedSecond = consumer->read(1U);
    require(
        observedFirst.has_value() && observedSecond.has_value()
            && observedFirst->decodedBlocks == 4U
            && observedSecond->decodedBlocks == 7U,
        "each health slot should retain only its own monotonic state");

    auto advancedFirst = first;
    advancedFirst.seekGeneration = 2U;
    advancedFirst.decodedBlocks = 5U;
    auto regressedSecond = second;
    regressedSecond.decodedBlocks = 6U;
    require(
        producer->publish(0U, advancedFirst)
            && !producer->publish(1U, regressedSecond),
        "one slot may advance while another slot regression fails closed");

    require(consumer->lastAcceptedSequence(0U) == 1U
            && consumer->lastAcceptedSequence(1U) == 1U
            && consumer->readDetailed(0U).diagnostic
                == activation::ComponentActivationAudioStreamHealthReadDiagnosticV1::Valid
            && consumer->lastAcceptedSequence(0U) == 2U
            && consumer->lastAcceptedSequence(1U) == 1U
            && consumer->lastAcceptedSequence(2U) == 0U,
        "local accepted sequences identify a partial resource-set publication without accepting another slot");

    auto changedBinding = fixture.binding;
    changedBinding.slots[1].sourceContentDigest[0] ^= 0x01U;
    require(
        !activation::ComponentActivationAudioStreamHealthConsumerV2::attach(
             storage.bytes, fixture.authority, changedBinding)
             .has_value(),
        "a changed Audio member identity must not attach to the set plane");

    auto* plane = reinterpret_cast<
        activation::ComponentActivationAudioStreamHealthSharedPlaneV2*>(
        storage.bytes.data());
    plane->slots[1].record = plane->slots[0].record;
    std::atomic_thread_fence(std::memory_order_release);
    const auto replayPublication = std::atomic_ref<std::uint64_t>{
        plane->slots[0].publication}
                                       .load(std::memory_order_acquire);
    std::atomic_ref<std::uint64_t>{plane->slots[1].publication}.store(
        replayPublication, std::memory_order_release);
    require(
        consumer->readDetailed(1U).diagnostic
            == activation::
                ComponentActivationAudioStreamHealthReadDiagnosticV1::Invalid,
        "a stable health record replayed across Audio slots must fail closed");
    require(consumer->lastAcceptedSequence(1U) == 1U,
        "invalid peer publication cannot advance the last accepted slot sequence");

    auto attackerBinding = fixture.binding;
    attackerBinding.slots[0].sourceContentDigest[0] ^= 0x80U;
    plane->setBinding = attackerBinding;
    auto forged = plane->slots[0].record;
    forged.sequence = 3U;
    forged.binding = attackerBinding.slots[0];
    forged.health = advancedFirst;
    std::atomic_ref<std::uint64_t>{plane->slots[0].publication}.store(
        5U, std::memory_order_release);
    plane->slots[0].record = forged;
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<std::uint64_t>{plane->slots[0].publication}.store(
        6U, std::memory_order_release);
    require(
        consumer->slotCount() == fixture.binding.slotCount
            && consumer->readDetailed(0U).diagnostic
                == activation::
                    ComponentActivationAudioStreamHealthReadDiagnosticV1::
                        Invalid,
        "post-attach shared-header and record rebinding must fail against the private endpoint identity");

    auto laterSecond = second;
    laterSecond.seekGeneration = 4U;
    laterSecond.decodedBlocks = 8U;
    require(
        producer->publish(1U, laterSecond),
        "post-attach producer bounds must remain tied to the private set identity");
}

void testAudioStreamHealthPlaneIsBoundedMonotonicAndAuthenticated()
{
    HealthStorage storage;
    const auto authority = authorization(48'000U, 256U, 1U);
    require(
        activation::initializeComponentActivationAudioStreamHealthPlaneV1(
            storage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "health plane should initialize from pristine exact storage");
    auto producer =
        activation::ComponentActivationAudioStreamHealthProducerV1::attach(
            storage.bytes, authority);
    auto consumer =
        activation::ComponentActivationAudioStreamHealthConsumerV1::attach(
            storage.bytes, authority);
    require(
        producer.has_value() && consumer.has_value()
            && consumer->readDetailed().diagnostic
                == activation::
                    ComponentActivationAudioStreamHealthReadDiagnosticV1::
                        Unavailable,
        "exact endpoints should attach before the first publication");

    activation::ComponentActivationAudioStreamHealthV1 first;
    first.seekGeneration = 1U;
    first.bufferedBlocks = 8U;
    first.pendingRequests = 1U;
    first.decodedBlocks = 8U;
    first.workerState =
        activation::ComponentActivationAudioStreamWorkerStateV1::Ready;
    require(producer->publish(first), "first health snapshot should publish");
    const auto observedFirst = consumer->read();
    require(
        observedFirst.has_value()
            && observedFirst->seekGeneration == 1U
            && observedFirst->decodedBlocks == 8U
            && observedFirst->bufferedBlocks == 8U,
        "consumer should read one exact stable health snapshot");

    auto next = first;
    next.seekGeneration = 2U;
    next.discontinuities = 1U;
    next.bufferedBlocks = 2U;
    next.pendingRequests = 0U;
    require(
        producer->publish(next),
        "monotonic counters should permit non-monotonic bounded gauges");
    const auto observedNext = consumer->read();
    require(
        observedNext.has_value()
            && observedNext->seekGeneration == 2U
            && observedNext->discontinuities == 1U
            && observedNext->bufferedBlocks == 2U,
        "second stable health snapshot should advance monotonically");

    auto regressed = next;
    regressed.decodedBlocks = 7U;
    require(
        !producer->publish(regressed),
        "counter regression must fail closed before publication");
    auto excessiveBufferedGauge = next;
    excessiveBufferedGauge.bufferedBlocks =
        activation::componentActivationAudioResponseCapacityV1 + 1U;
    require(
        !producer->publish(excessiveBufferedGauge),
        "a health snapshot must reject a buffered-block gauge above its fixed capacity");
    auto excessiveRequestGauge = next;
    excessiveRequestGauge.pendingRequests =
        activation::componentActivationAudioRequestCapacityV1 + 1U;
    require(
        !producer->publish(excessiveRequestGauge),
        "a health snapshot must reject a pending-request gauge above its fixed capacity");
    auto impossibleFailure = next;
    impossibleFailure.decodeFailures = 1U;
    require(
        !producer->publish(impossibleFailure),
        "a health snapshot must reject an impossible decode failure state");
    impossibleFailure = next;
    impossibleFailure.workerState =
        activation::ComponentActivationAudioStreamWorkerStateV1::Failed;
    require(
        !producer->publish(impossibleFailure),
        "a health snapshot must reject an impossible failed worker state");

    auto changedAuthority = authority;
    ++changedAuthority.activationGeneration;
    require(
        !activation::ComponentActivationAudioStreamHealthConsumerV1::attach(
             storage.bytes, changedAuthority)
             .has_value(),
        "another activation binding must not attach to health evidence");

    auto* plane = reinterpret_cast<
        activation::ComponentActivationAudioStreamHealthSharedPlaneV1*>(
        storage.bytes.data());
    plane->record.health.decodedBlocks += 1U;
    std::atomic_thread_fence(std::memory_order_release);
    require(
        consumer->readDetailed().diagnostic
            == activation::
                ComponentActivationAudioStreamHealthReadDiagnosticV1::Invalid,
        "one stable health sequence must never project different payload bytes");
    std::atomic_ref<std::uint64_t>{plane->publication}.store(
        5U, std::memory_order_release);
    require(
        consumer->readDetailed().diagnostic
            == activation::
                ComponentActivationAudioStreamHealthReadDiagnosticV1::Busy,
        "an in-flight odd publication must never project a torn snapshot");
    plane->record.sequence = 3U;
    plane->record.health = excessiveBufferedGauge;
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<std::uint64_t>{plane->publication}.store(
        6U, std::memory_order_release);
    require(
        consumer->readDetailed().diagnostic
            == activation::
                ComponentActivationAudioStreamHealthReadDiagnosticV1::Invalid,
        "a stable malformed publication must be distinguished from an in-flight writer");
}

void testEveryExactProfileHasOneBlockLatency()
{
    for (const auto sampleRate : {44'100U, 48'000U})
    {
        for (const auto blockFrames : {256U, 512U})
        {
            for (const auto channels : {std::uint16_t{1}, std::uint16_t{2}})
            {
                SharedStorage storage;
                const auto authority = authorization(
                    sampleRate, blockFrames, channels);
                require(
                    activation::initializeComponentActivationAudioDataPlaneV1(
                        storage.bytes, authority)
                        == activation::ComponentActivationAudioDiagnosticV1::Ready,
                    "an exact admitted plane should initialize once");
                auto host = activation::ComponentActivationAudioHostV1::attach(
                    storage.bytes, authority);
                auto worker =
                    activation::ComponentActivationAudioWorkerPlaneV1::attach(
                        storage.bytes, authority);
                require(
                    host.has_value() && worker.has_value(),
                    "host and worker should attach to one exact binding");
                require(
                    !host->publishedResponseReady(),
                    "no response may be ready before request one is published");

                std::array<std::array<float, 512>, 2> firstInput{};
                std::array<std::array<float, 512>, 2> secondInput{};
                std::array<std::array<float, 512>, 2> output{};
                std::array<const float*, 2> inputPointers{};
                std::array<float*, 2> outputPointers{};
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    std::fill_n(
                        firstInput[channel].begin(), blockFrames,
                        static_cast<float>(channel + 1U));
                    std::fill_n(
                        secondInput[channel].begin(), blockFrames,
                        static_cast<float>(channel + 3U));
                    std::fill_n(output[channel].begin(), blockFrames, 9.0F);
                    inputPointers[channel] = firstInput[channel].data();
                    outputPointers[channel] = output[channel].data();
                }
                activation::ComponentActivationAudioTransportV1 transport;
                transport.generation = 1;
                transport.sourceFrame = 100;
                require(
                    host->process(
                        {inputPointers.data(), channels},
                        {outputPointers.data(), channels},
                        blockFrames,
                        transport,
                        true)
                        == activation::ComponentActivationAudioOperationV1::
                            FirstBlockSilence,
                    "the first callback should publish request one and stay silent");
                require(
                    !host->publishedResponseReady(),
                    "request publication alone must not report a ready response");
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    require(
                        std::all_of(
                            output[channel].begin(),
                            output[channel].begin() + blockFrames,
                            [](float value) { return value == 0.0F; }),
                        "first-block silence must be deterministic");
                }

                activation::ComponentActivationAudioWorkerBlockV1 block;
                require(
                    worker->acquire(block)
                        == activation::ComponentActivationAudioOperationV1::Acquired
                        && block.sequence == 1
                        && block.transport == transport,
                    "worker should acquire exact request one");
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    for (std::size_t frame = 0; frame < blockFrames; ++frame)
                    {
                        const auto index = channel * 512U + frame;
                        block.outputSamples[index] =
                            block.inputSamples[index] * 0.5F;
                    }
                }
                require(
                    worker->publish(block)
                        == activation::ComponentActivationAudioOperationV1::Published,
                    "worker should publish one finite exact response");
                require(
                    host->publishedResponseReady(),
                    "the exact stable response publication should be observable");
                for (std::size_t channel = 0; channel < channels; ++channel)
                    inputPointers[channel] = secondInput[channel].data();
                ++transport.sourceFrame;
                require(
                    host->process(
                        {inputPointers.data(), channels},
                        {outputPointers.data(), channels},
                        blockFrames,
                        transport,
                        true)
                        == activation::ComponentActivationAudioOperationV1::Rendered,
                    "callback two should consume response one then publish request two");
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    for (std::size_t frame = 0; frame < blockFrames; ++frame)
                    {
                        require(
                            output[channel][frame]
                                == static_cast<float>(channel + 1U) * 0.5F,
                            "one-block delayed output must preserve channel order");
                    }
                }
            }
        }
    }
}

void testMissingResponseAndRevocationLatchSilence()
{
    SharedStorage storage;
    const auto authority = authorization(48'000, 256, 1);
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            storage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "missing-response plane should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        storage.bytes, authority);
    auto worker = activation::ComponentActivationAudioWorkerPlaneV1::attach(
        storage.bytes, authority);
    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    activation::ComponentActivationAudioTransportV1 transport{1, 0, 0, 0};
    require(
        host->process(inputs, outputs, 256, transport, true)
            == activation::ComponentActivationAudioOperationV1::FirstBlockSilence,
        "request one should start the pipeline");
    require(
        host->process(inputs, outputs, 256, transport, true)
            == activation::ComponentActivationAudioOperationV1::Missing
            && host->isQuarantined()
            && host->quarantineReason()
                == activation::ComponentActivationAudioQuarantineReasonV1::
                    ResponseMissing,
        "a missed one-block response must latch quarantine and silence");
    activation::ComponentActivationAudioWorkerBlockV1 late;
    require(
        worker->acquire(late)
            == activation::ComponentActivationAudioOperationV1::Quarantined,
        "late work must never resurrect a quarantined generation");

    SharedStorage revokedStorage;
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            revokedStorage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "revocation plane should initialize");
    auto revokedHost = activation::ComponentActivationAudioHostV1::attach(
        revokedStorage.bytes, authority);
    std::fill(output.begin(), output.end(), 1.0F);
    require(
        revokedHost->process(inputs, outputs, 256, transport, false)
            == activation::ComponentActivationAudioOperationV1::Quarantined
            && revokedHost->quarantineReason()
                == activation::ComponentActivationAudioQuarantineReasonV1::Revoked
            && std::all_of(output.begin(), output.end(), [](float value) {
                   return value == 0.0F;
               }),
        "revocation must silence before publishing or entering component code");
}

void testCanaryConsumesFinalResponseWithoutPublishingAnotherRequest()
{
    SharedStorage storage;
    const auto authority = authorization(48'000, 256, 1);
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            storage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "canary plane should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        storage.bytes, authority);
    auto worker = activation::ComponentActivationAudioWorkerPlaneV1::attach(
        storage.bytes, authority);
    require(
        host.has_value() && worker.has_value(),
        "canary endpoints should attach to one exact plane");

    std::array<float, 256> input{};
    std::array<float, 256> output{};
    std::fill(input.begin(), input.end(), 0.25F);
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    const activation::ComponentActivationAudioTransportV1 transport{
        1, 0, 1, 0};
    require(
        host->process(inputs, outputs, 256, transport, true)
            == activation::ComponentActivationAudioOperationV1::
                FirstBlockSilence,
        "canary should publish exactly one request");

    activation::ComponentActivationAudioWorkerBlockV1 block;
    require(
        worker->acquire(block)
            == activation::ComponentActivationAudioOperationV1::Acquired,
        "worker should acquire the one canary request");
    std::copy_n(
        block.inputSamples.begin(), block.frameCount, block.outputSamples.begin());
    require(
        worker->publish(block)
            == activation::ComponentActivationAudioOperationV1::Published,
        "worker should publish the canary response");
    std::uint64_t processingNanoseconds = 0;
    require(
        host->consumeFinal(outputs, 256, true, processingNanoseconds)
                == activation::ComponentActivationAudioOperationV1::Rendered
            && output == input && processingNanoseconds == 0,
        "final canary consumption should return the exact response");

    block = {};
    require(
        worker->acquire(block)
            == activation::ComponentActivationAudioOperationV1::Missing,
        "final canary consumption must not publish a second request");
}

void testMalformedProfilesAndNonFiniteAudioFailClosed()
{
    SharedStorage storage;
    auto authority = authorization(48'000, 256, 1);
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            storage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "non-finite plane should initialize");
    auto host = activation::ComponentActivationAudioHostV1::attach(
        storage.bytes, authority);
    std::array<float, 256> input{};
    std::array<float, 256> output{};
    input.front() = std::numeric_limits<float>::quiet_NaN();
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    require(
        host->process(
            inputs,
            outputs,
            256,
            activation::ComponentActivationAudioTransportV1{1, 0, 0, 0},
            true)
            == activation::ComponentActivationAudioOperationV1::NonFinite
            && host->isQuarantined(),
        "non-finite host input must fail closed before publication");

    SharedStorage mismatchedStorage;
    require(
        activation::initializeComponentActivationAudioDataPlaneV1(
            mismatchedStorage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready,
        "binding mismatch plane should initialize");
    ++authority.activationGeneration;
    require(
        !activation::ComponentActivationAudioHostV1::attach(
             mismatchedStorage.bytes, authority)
             .has_value(),
        "another activation generation must not attach to shared slots");
}
}

namespace
{
struct PendingCheckpointRig
{
    using Host = activation::ComponentActivationAudioHostV1;
    using Result = activation::ComponentActivationAudioCheckpointResultV1;
    using Operation = activation::ComponentActivationAudioOperationV1;
    SharedStorage storage;
    std::optional<Host> host;
    std::optional<activation::ComponentActivationAudioWorkerPlaneV1> worker;
    std::array<float, 256> input{}, output{};
    std::array<const float*, 1> inputs{input.data()};
    std::array<float*, 1> outputs{output.data()};
    PendingCheckpointRig()
    {
        const auto authority = authorization(48000, 256, 1);
        require(activation::initializeComponentActivationAudioDataPlaneV1(storage.bytes, authority)
            == activation::ComponentActivationAudioDiagnosticV1::Ready, "edge plane initializes");
        host = Host::attach(storage.bytes, authority);
        worker = activation::ComponentActivationAudioWorkerPlaneV1::attach(storage.bytes, authority);
        require(host && worker, "edge endpoints attach");
    }
    void submit(float value, bool complete = true)
    {
        input.fill(value);
        const auto result = host->process(inputs, outputs, 256,
            {3, static_cast<std::int64_t>(host->lastSubmittedSequence() * 256), 0, 0}, true);
        require(result == Operation::Rendered || result == Operation::FirstBlockSilence, "edge request succeeds");
        if (complete) pump();
    }
    void pump()
    {
        activation::ComponentActivationAudioWorkerBlockV1 block;
        require(worker->acquire(block) == Operation::Acquired, "edge worker acquires");
        std::copy_n(block.inputSamples.begin(), 256, block.outputSamples.begin());
        require(worker->publish(block) == Operation::Published, "edge worker publishes");
    }
    activation::ComponentActivationAudioSharedSlotV1& latestResponse()
    {
        auto& plane = *reinterpret_cast<activation::ComponentActivationAudioSharedPlaneV1*>(storage.bytes.data());
        return plane.responses[(host->lastSubmittedSequence() - 1) % activation::componentActivationAudioSlotCountV1];
    }
};

void testPendingAudioCheckpointAuthorityAndLifecycle()
{
    using Rig = PendingCheckpointRig;
    using Result = Rig::Result;
    using Operation = Rig::Operation;
    {
        Rig rig;
        rig.submit(0.25F, false);
        const auto before = rig.storage.bytes;
        require(rig.host->beginOfflineCheckpoint(true) == Result::MissingResponse
            && !rig.host->isQuarantined() && rig.storage.bytes == before,
            "capture with an unfinished worker request must be retryable and read-only");
        rig.pump();
        require(rig.host->beginOfflineCheckpoint(true) == Result::Captured, "capture retries after matching publication");
        rig.submit(0.5F, false);
        require(rig.host->commitOfflineCheckpoint(true) == Result::Committed,
            "commit can retain a still-pending ordinary response");
        rig.pump();
        rig.submit(0.75F);
        require(rig.output[0] == 0.5F, "commit preserves latest delayed output");
    }
    // Each corruption is applied at a quiesced manual-worker boundary.
    for (int corruption = 0; corruption < 5; ++corruption)
    for (bool duringRestore : {false, true})
    {
        Rig rig;
        rig.submit(0.25F);
        if (duringRestore)
        {
            require(rig.host->beginOfflineCheckpoint(true) == Result::Captured, "corruption baseline captures");
            rig.submit(0.5F);
        }
        auto& response = rig.latestResponse();
        if (corruption == 0) ++response.record.binding.activationGeneration;
        if (corruption == 1) ++response.record.transport.sourceFrame;
        if (corruption == 2) response.samples[255] = std::numeric_limits<float>::quiet_NaN();
        if (corruption == 3) response.publication += 4;
        if (corruption == 4) ++response.record.sequence;
        const auto result = duringRestore ? rig.host->restoreOfflineCheckpoint(true)
            : rig.host->beginOfflineCheckpoint(true);
        require(result == Result::Quarantined && rig.host->isQuarantined()
            && rig.host->restoreOfflineCheckpoint(true) == Result::Quarantined
            && rig.host->commitOfflineCheckpoint(true) == Result::Quarantined,
            "invalid identity, transport, samples or future publication permanently prevents rollback");
    }
    for (int terminal = 0; terminal < 3; ++terminal)
    for (bool cached : {false, true})
    {
        Rig rig;
        rig.submit(0.25F);
        require(rig.host->beginOfflineCheckpoint(true) == Result::Captured, "authority baseline captures");
        if (cached)
        {
            rig.submit(0.5F);
            require(rig.host->restoreOfflineCheckpoint(true) == Result::Restored
                && rig.host->beginOfflineCheckpoint(true) == Result::Captured, "cached baseline recaptures");
        }
        if (terminal == 0) rig.host->quarantine();
        const auto result = terminal == 2 ? rig.host->commitOfflineCheckpoint(false)
            : rig.host->restoreOfflineCheckpoint(terminal == 0);
        require(result == Result::Quarantined
            && rig.host->beginOfflineCheckpoint(true) == Result::Quarantined,
            "retained checkpoint and cached samples cannot revive lost authority");
        rig.output.fill(1);
        require(rig.host->process(rig.inputs, rig.outputs, 256, {3, 0, 0, 0}, true) == Operation::Quarantined
            && std::ranges::all_of(rig.output, [](float value) { return value == 0; }),
            "terminal rollback failure keeps callback silent");
    }
    {
        Rig rig, destination;
        rig.submit(0.25F);
        require(rig.host->beginOfflineCheckpoint(true) == Result::Captured, "move baseline captures");
        rig.submit(0.5F);
        const auto beforeMove = ordinaryAllocations;
        Rig::Host moved(std::move(*rig.host));
        require(ordinaryAllocations == beforeMove && rig.host->lastSubmittedSequence() == 0
            && rig.host->restoreOfflineCheckpoint(true) == Result::Quarantined,
            "move constructor transfers sole checkpoint ownership without allocation");
        require(moved.restoreOfflineCheckpoint(true) == Result::Restored
            && moved.beginOfflineCheckpoint(true) == Result::Captured,
            "moved baseline restores and cached output recaptures");
        *destination.host = std::move(moved);
        require(moved.isQuarantined() && moved.lastSubmittedSequence() == 0
            && destination.host->commitOfflineCheckpoint(true) == Result::Committed,
            "move assignment transfers cached output and baseline and disables source");
        require(destination.host->process(rig.inputs, rig.outputs, 256, {9, 1234, 1, 0}, true) == Operation::Rendered
            && std::ranges::all_of(rig.output, [](float value) { return value == 0.25F; })
            && destination.host->lastSubmittedSequence() == 3,
            "committing a recaptured cache preserves exact baseline audio and accepts new logical transport");
    }
    for (int state = 0; state < 3; ++state)
    {
        Rig rig;
        rig.submit(0.25F);
        std::uint64_t duration = 0;
        if (state == 0)
        {
            require(rig.host->consumeFinal(rig.outputs, 256, true, duration) == Operation::Rendered
                && rig.host->beginOfflineCheckpoint(true) == Result::InvalidState,
                "terminal canary cannot acquire a checkpoint");
        }
        else
        {
            require(rig.host->beginOfflineCheckpoint(true) == Result::Captured, "canary baseline captures");
            if (state == 2) require(rig.host->restoreOfflineCheckpoint(true) == Result::Restored, "canary baseline caches");
            require(rig.host->consumeFinal(rig.outputs, 256, true, duration) == Operation::InvalidArgument
                && rig.host->isQuarantined(), "canary cannot consume held or restored logical state");
        }
    }
}


template<class Host>
void testLogicalPendingAudioCheckpoint()
{
    if constexpr (!requires(Host& host) {
        host.beginOfflineCheckpoint(true); host.restoreOfflineCheckpoint(true);
        host.commitOfflineCheckpoint(true); host.lastSubmittedSequence();
    })
    {
        require(false, "audio host lacks logical pending-output checkpoint independent of physical sequence");
    }
    else
    {
        using Result = decltype(std::declval<Host&>().beginOfflineCheckpoint(true));
        for (const auto sampleRate : {44100U, 48000U})
        for (const auto frames : {256U, 512U})
        for (const auto channels : {std::uint16_t{1}, std::uint16_t{2}})
        for (const auto prefix : {0U, 2U})
        {
            SharedStorage storage;
            const auto authority = authorization(sampleRate, frames, channels);
            require(activation::initializeComponentActivationAudioDataPlaneV1(storage.bytes, authority)
                == activation::ComponentActivationAudioDiagnosticV1::Ready, "checkpoint plane should initialize");
            auto attached = Host::attach(storage.bytes, authority);
            auto worker = activation::ComponentActivationAudioWorkerPlaneV1::attach(storage.bytes, authority);
            require(attached && worker, "checkpoint endpoints should attach");
            Host& host = *attached;
            std::array<std::array<float, 512>, 2> input{}, output{}, expectedPending{};
            std::array<const float*, 2> inputs{};
            std::array<float*, 2> outputs{};
            for (std::size_t c = 0; c < channels; ++c) { inputs[c] = input[c].data(); outputs[c] = output[c].data(); }
            std::array<double, 2> workerHistory{}, oracleHistory{};
            std::uint64_t submitted = 0;
            bool hasPending = false;
            activation::ComponentActivationAudioTransportV1 lastTransport;
            auto pump = [&] {
                activation::ComponentActivationAudioWorkerBlockV1 block;
                require(worker->acquire(block) == activation::ComponentActivationAudioOperationV1::Acquired
                    && block.sequence == submitted && block.transport == lastTransport,
                    "worker must acquire the next unique physical sequence and exact transport");
                for (std::size_t c = 0; c < channels; ++c)
                for (std::size_t f = 0; f < frames; ++f)
                {
                    const auto index = c * 512 + f;
                    require(block.inputSamples[index] == input[c][f], "physical request must carry the current logical input");
                    workerHistory[c] = workerHistory[c] * 0.5 + block.inputSamples[index];
                    block.outputSamples[index] = static_cast<float>(workerHistory[c]);
                }
                require(worker->publish(block) == activation::ComponentActivationAudioOperationV1::Published,
                    "worker must publish using the same unique sequence");
            };
            auto submit = [&](std::uint64_t logicalBlock, bool complete = true) {
                for (std::size_t c = 0; c < channels; ++c)
                    input[c].fill(static_cast<float>((logicalBlock % 7 + c * 2 + 1) * 0.0625));
                lastTransport = {7, static_cast<std::int64_t>(logicalBlock * frames), 0, 0};
                const auto before = ordinaryAllocations;
                const auto status = host.process({inputs.data(), channels}, {outputs.data(), channels}, frames, lastTransport, true);
                const auto after = ordinaryAllocations;
                require(before == after && status == (hasPending
                    ? activation::ComponentActivationAudioOperationV1::Rendered
                    : activation::ComponentActivationAudioOperationV1::FirstBlockSilence),
                    "logical first-block state must control latency without allocating");
                ++submitted;
                require(host.lastSubmittedSequence() == submitted, "rollback must never reuse a physical request sequence");
                for (std::size_t c = 0; c < channels; ++c)
                for (std::size_t f = 0; f < frames; ++f)
                {
                    const auto expected = hasPending ? expectedPending[c][f] : 0.0F;
                    require(std::abs(output[c][f] - expected) < 1e-6F,
                        "delayed output must match independent closed-form recursive history");
                }
                for (std::size_t c = 0; c < channels; ++c)
                {
                    const auto target = static_cast<double>(input[c][0]) * 2;
                    for (std::size_t f = 0; f < frames; ++f)
                        expectedPending[c][f] = static_cast<float>(target
                            + (oracleHistory[c] - target) * std::ldexp(1.0, -static_cast<int>(f + 1)));
                    oracleHistory[c] = target + (oracleHistory[c] - target) * std::ldexp(1.0, -static_cast<int>(frames));
                }
                hasPending = true;
                if (complete) pump();
            };
            for (std::uint64_t i = 0; i < prefix; ++i) submit(i);
            std::uint64_t logicalBlock = prefix;
            for (std::size_t attempt = 0; attempt < 24; ++attempt)
            {
                const auto savedWorker = workerHistory;
                const auto savedOracle = oracleHistory;
                const auto savedPending = expectedPending;
                const auto savedHasPending = hasPending;
                const auto beforeCapture = ordinaryAllocations;
                const auto captured = host.beginOfflineCheckpoint(true);
                const auto afterCapture = ordinaryAllocations;
                require(captured == Result::Captured && beforeCapture == afterCapture,
                    "pending response capture must allocate no memory");
                require(host.beginOfflineCheckpoint(true) == Result::InvalidState, "nested capture must retain original pending response");
                for (std::size_t i = 0; i < activation::componentActivationAudioSlotCountV1 + 2; ++i)
                    submit(100 + attempt * 5 + i);
                submit(400 + attempt, false);
                require(host.restoreOfflineCheckpoint(true) == Result::MissingResponse && !host.isQuarantined(),
                    "control restore must wait for the exact unfinished request without weakening callback policy");
                pump();
                const auto sharedBefore = storage.bytes;
                const auto beforeRestore = ordinaryAllocations;
                const auto restored = host.restoreOfflineCheckpoint(true);
                const auto afterRestore = ordinaryAllocations;
                require(restored == Result::Restored && beforeRestore == afterRestore
                    && host.lastSubmittedSequence() == submitted && storage.bytes == sharedBefore,
                    "restore must change only private logical state without allocation or shared slot rewrites");
                workerHistory = savedWorker; oracleHistory = savedOracle;
                expectedPending = savedPending; hasPending = savedHasPending;
                submit(logicalBlock++);
            }
            require(host.beginOfflineCheckpoint(true) == Result::Captured, "commit should capture pending audio");
            submit(logicalBlock++);
            const auto sharedBefore = storage.bytes;
            const auto beforeCommit = ordinaryAllocations;
            const auto committed = host.commitOfflineCheckpoint(true);
            const auto afterCommit = ordinaryAllocations;
            require(committed == Result::Committed && beforeCommit == afterCommit && sharedBefore == storage.bytes,
                "commit must preserve physical and logical continuation without allocation");
            require(host.restoreOfflineCheckpoint(true) == Result::InvalidState, "committed baseline must be consumed");
            submit(logicalBlock);
        }
    }
}
}

int main()
{
    testAudioStreamHealthPlaneIsBoundedMonotonicAndAuthenticated();
    testAudioStreamHealthPlaneV2AuthenticatesIndependentSlots();
    testEveryExactProfileHasOneBlockLatency();
    testMissingResponseAndRevocationLatchSilence();
    testCanaryConsumesFinalResponseWithoutPublishingAnotherRequest();
    testMalformedProfilesAndNonFiniteAudioFailClosed();
    testLogicalPendingAudioCheckpoint<activation::ComponentActivationAudioHostV1>();
    testPendingAudioCheckpointAuthorityAndLifecycle();
    return 0;
}
