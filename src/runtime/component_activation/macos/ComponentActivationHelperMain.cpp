#include "runtime/component_activation/macos/ComponentActivationXpcEnvelope.h"
#include "talas/runtime/ComponentActivationAudioDataPlane.h"

#ifndef TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE
#define TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE 0
#endif

#if TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 0 && \
    !defined(TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE)
#include "runtime/component_activation/macos/ComponentActivationLoader.h"
#include "runtime/component_activation/ComponentActivationWorker.h"
#define TALAS_COMPONENT_ACTIVATION_RUNTIME_LOADING 1
#else
#define TALAS_COMPONENT_ACTIVATION_RUNTIME_LOADING 0
#endif

#ifdef TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE
#include "runtime/component_activation/macos/ComponentActivationSandboxReport.h"
#endif

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace talas::runtime::component_activation
{
namespace
{
std::atomic<bool> peerClaimed{false};

struct PeerContext
{
    ComponentActivationSealedSessionV1 session;
    void* sharedStorage = nullptr;
    std::size_t sharedStorageBytes = 0;
    std::uint64_t sequence = 0;
    xpc_object_t withheldReply = nullptr;
    bool sealed = false;
#if TALAS_COMPONENT_ACTIVATION_RUNTIME_LOADING
    ComponentActivationAuthorizationV1 authorization;
    ComponentActivationPrivateAudioSourceV1 audioSource;
    ComponentActivationPrivateResourceSetV1 resourceSet;
    std::unique_ptr<ComponentActivationLoadedInstanceV1> loadedInstance;
    std::unique_ptr<ComponentActivationWorkerV1> worker;
    bool activationFailed = false;
    bool runtimePrepared = false;
    std::uint64_t checkpointId = 0;
#endif
#ifdef TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE
    ComponentActivationSandboxReportV1 sandboxReport;
    bool sandboxReportReady = false;
#endif
};

[[nodiscard]] bool exactSession(
    const ComponentActivationSealedSessionV1& left,
    const ComponentActivationSealedSessionV1& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

void rejectPeer(xpc_connection_t peer) noexcept;

#if TALAS_COMPONENT_ACTIVATION_RUNTIME_LOADING
[[nodiscard]] ComponentActivationRuntimeDiagnosticV1 loaderDiagnostic(
    ComponentActivationLoaderDiagnosticV1 diagnostic) noexcept
{
    switch (diagnostic)
    {
        case ComponentActivationLoaderDiagnosticV1::Ready:
            return ComponentActivationRuntimeDiagnosticV1::Ready;
        case ComponentActivationLoaderDiagnosticV1::EntryRejected:
            return ComponentActivationRuntimeDiagnosticV1::EntryRejected;
        case ComponentActivationLoaderDiagnosticV1::LoadRejected:
        case ComponentActivationLoaderDiagnosticV1::CreateRejected:
        case ComponentActivationLoaderDiagnosticV1::AllocationFailed:
            return ComponentActivationRuntimeDiagnosticV1::LoadRejected;
        case ComponentActivationLoaderDiagnosticV1::AbiRejected:
            return ComponentActivationRuntimeDiagnosticV1::AbiRejected;
        case ComponentActivationLoaderDiagnosticV1::PrepareRejected:
        case ComponentActivationLoaderDiagnosticV1::HostViolation:
            return ComponentActivationRuntimeDiagnosticV1::PrepareRejected;
        case ComponentActivationLoaderDiagnosticV1::ProtocolRejected:
        case ComponentActivationLoaderDiagnosticV1::ManifestRejected:
        case ComponentActivationLoaderDiagnosticV1::ResourceRejected:
        case ComponentActivationLoaderDiagnosticV1::AudioStreamRejected:
            return ComponentActivationRuntimeDiagnosticV1::ProtocolRejected;
    }
    return ComponentActivationRuntimeDiagnosticV1::ProtocolRejected;
}

[[nodiscard]] ComponentActivationTransitionV1 activationTransition(
    PeerContext& context,
    ComponentActivationRecordKindV1 kind,
    ComponentActivationRuntimeStateV1 state,
    ComponentActivationRuntimeDiagnosticV1 diagnostic) noexcept
{
    ComponentActivationTransitionV1 transition;
    transition.header = componentActivationHeaderV1(kind, sizeof(transition));
    transition.contentDigest = context.authorization.contentDigest;
    transition.artifactGeneration = context.authorization.artifactGeneration;
    transition.activationGeneration =
        context.authorization.activationGeneration;
    transition.runtimeSessionGeneration =
        context.authorization.runtimeSessionGeneration;
    transition.sharedStorageIdentity =
        context.authorization.sharedStorageIdentity;
    transition.validationNonce = context.authorization.validationNonce;
    transition.runtimeSessionNonce =
        context.authorization.runtimeSessionNonce;
    transition.authorizationNonce = context.authorization.authorizationNonce;
    transition.state = static_cast<std::uint32_t>(state);
    transition.diagnostic = static_cast<std::uint32_t>(diagnostic);
    transition.sequence = ++context.sequence;
    return transition;
}

void sendActivationTransition(
    PeerContext& context,
    xpc_connection_t peer,
    xpc_object_t request,
    ComponentActivationRecordKindV1 kind,
    ComponentActivationRuntimeStateV1 state,
    ComponentActivationRuntimeDiagnosticV1 diagnostic) noexcept
{
    auto reply = xpc_dictionary_create_reply(request);
    const auto transition = activationTransition(
        context, kind, state, diagnostic);
    if (reply == nullptr
        || !writeComponentActivationXpcTransitionV1(
            reply,
            transition,
            kind,
            context.authorization))
    {
        if (reply != nullptr)
            xpc_release(reply);
        rejectPeer(peer);
        return;
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

void handleCheckpointRequest(
    PeerContext& context, xpc_connection_t peer, xpc_object_t event,
    const ComponentActivationCheckpointCommandV1& request) noexcept
{
    using Operation = ComponentActivationCheckpointOperationV1;
    using Status = ComponentActivationCheckpointStatusV1;
    if (!context.runtimePrepared || context.activationFailed || context.worker == nullptr
        || request.command.sequence != context.sequence + 1U)
    {
        rejectPeer(peer);
        return;
    }
    ComponentActivationCheckpointReplyV1 response;
    response.request = request;
    if (!context.loadedInstance->active()) response.status = Status::Fatal;
    else if (request.operation == Operation::Query)
        response.status = context.loadedInstance->supportsOfflineCheckpoint()
            ? Status::Supported : Status::Unsupported;
    else if ((request.operation == Operation::Begin && context.checkpointId != 0)
        || (request.operation != Operation::Begin && request.checkpointId != context.checkpointId))
        response.status = Status::InvalidBoundary;
    else
    {
        auto status = TalasKernelAbiStatusV1{TALAS_KERNEL_ABI_STATUS_FATAL};
        switch (request.operation)
        {
            case Operation::Begin:
                status = context.worker->beginOfflineCheckpoint(request.completedAudioSequence);
                if (status == TALAS_KERNEL_ABI_STATUS_OK) context.checkpointId = request.checkpointId;
                break;
            case Operation::Restore:
                status = context.worker->restoreOfflineCheckpoint(request.completedAudioSequence);
                if (status == TALAS_KERNEL_ABI_STATUS_OK) context.checkpointId = 0;
                break;
            case Operation::Commit:
                status = context.worker->commitOfflineCheckpoint(request.completedAudioSequence);
                if (status == TALAS_KERNEL_ABI_STATUS_OK) context.checkpointId = 0;
                break;
            case Operation::Query: break;
        }
        if (status == TALAS_KERNEL_ABI_STATUS_OK)
            response.status = request.operation == Operation::Begin ? Status::Captured
                : (request.operation == Operation::Restore ? Status::Restored : Status::Committed);
        else if (status == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT) response.status = Status::InvalidBoundary;
        else if (request.operation == Operation::Begin && status == TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE)
            response.status = Status::Unsupported;
        else if (request.operation == Operation::Begin && status == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR)
            response.status = Status::CaptureFailed;
        else response.status = Status::Fatal;
        // Even a retryable capture failure leaves the private worker stopped.
        // Resume before acknowledging that the ordinary audio path is usable.
        if (response.status != Status::Fatal && !context.worker->running()
            && !context.worker->start()) response.status = Status::Fatal;
    }
    if (response.status == Status::Fatal)
    {
        context.activationFailed = true;
        context.worker->stop();
    }
    auto reply = xpc_dictionary_create_reply(event);
    if (reply == nullptr
        || !writeComponentActivationXpcCheckpointReplyV1(reply, response, context.authorization))
    {
        if (reply != nullptr) xpc_release(reply);
        rejectPeer(peer);
        return;
    }
    context.sequence = request.command.sequence;
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

void handleActivationRequest(
    PeerContext& context,
    xpc_connection_t peer,
    xpc_object_t event) noexcept
{
    if (context.loadedInstance != nullptr || context.activationFailed)
    {
        rejectPeer(peer);
        return;
    }
    ComponentActivationXpcLoadPayloadViewV1 payload;
    if (decodeComponentActivationXpcLoadPayloadV1(
            event, context.session, payload)
        != ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
    {
        rejectPeer(peer);
        return;
    }
    context.authorization = payload.authorization;
    const ComponentActivationAudioStreamGrantV1* audioStreamGrant = nullptr;
    std::span<const std::uint8_t> audioSourceBytes;
    if (payload.audioStreamGrant.has_value())
    {
        const auto sourceDiagnostic =
            materializeComponentActivationPrivateAudioSourceV1(
                payload.audioSourceSharedMemory,
                *payload.audioStreamGrant,
                context.audioSource);
        if (sourceDiagnostic
            != ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            context.activationFailed = true;
            sendActivationTransition(
                context,
                peer,
                event,
                ComponentActivationRecordKindV1::Terminal,
                ComponentActivationRuntimeStateV1::Quarantined,
                ComponentActivationRuntimeDiagnosticV1::LoadRejected);
            return;
        }
        audioStreamGrant = &*payload.audioStreamGrant;
        audioSourceBytes = context.audioSource.bytes();
    }
    if (payload.resourceSetGrant.has_value())
    {
        const auto resourceSetDiagnostic =
            materializeComponentActivationResourceSetV1(
                payload, context.resourceSet);
        if (resourceSetDiagnostic
            != ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            context.activationFailed = true;
            sendActivationTransition(
                context,
                peer,
                event,
                ComponentActivationRecordKindV1::Terminal,
                ComponentActivationRuntimeStateV1::Quarantined,
                ComponentActivationRuntimeDiagnosticV1::LoadRejected);
            return;
        }
    }
    std::array<
        ComponentActivationPreparedResourceSetMemberV1,
        maximumComponentActivationResourceSetMembersV1>
        preparedResourceSetMembers{};
    std::optional<ComponentActivationPreparedResourceSetV1>
        preparedResourceSet;
    if (payload.resourceSetGrant.has_value())
    {
        for (std::size_t index = 0;
             index < payload.resourceSetMembers.size();
             ++index)
        {
            const auto& source = payload.resourceSetMembers[index];
            preparedResourceSetMembers[index] = {
                .member = &source.member,
                .immutableGrant = source.immutableGrant.has_value()
                    ? &*source.immutableGrant
                    : nullptr,
                .immutableBytes = source.immutableBytes,
                .audioGrant = source.audioGrant.has_value()
                    ? &*source.audioGrant
                    : nullptr,
                .audioSourceBytes = context.resourceSet.audioBytes(index),
            };
        }
        preparedResourceSet.emplace(
            ComponentActivationPreparedResourceSetV1{
                .grant = &*payload.resourceSetGrant,
                .members = {
                    preparedResourceSetMembers.data(),
                    payload.resourceSetMembers.size()},
            });
    }
    const auto materializationFileDescriptor =
        xpc_fd_dup(payload.materializationFile);
    if (materializationFileDescriptor < 0)
    {
        rejectPeer(peer);
        return;
    }
    auto loaded = loadComponentActivationV1(
        payload.authorization,
        context.session,
        payload.canonicalManifest,
        payload.entryBytes,
        payload.staticParameters,
        materializationFileDescriptor,
        payload.resourceGrant.has_value() ? &*payload.resourceGrant : nullptr,
        payload.resourceBytes,
        audioStreamGrant,
        audioSourceBytes,
        preparedResourceSet.has_value() ? &*preparedResourceSet : nullptr);
    if (!loaded.ok())
    {
        context.activationFailed = true;
        sendActivationTransition(
            context,
            peer,
            event,
            ComponentActivationRecordKindV1::Terminal,
            ComponentActivationRuntimeStateV1::Quarantined,
            loaderDiagnostic(loaded.diagnostic));
        return;
    }
    context.loadedInstance = std::move(loaded.instance);
    ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
    const auto planeOffset = componentActivationAudioCanaryPlaneOffsetV1;
    const auto setHealth = context.loadedInstance->audioStreamHealthSet();
    const auto healthPlaneBytes = setHealth.has_value()
        ? componentActivationAudioStreamHealthPlaneBytesV2
        : componentActivationAudioStreamHealthPlaneBytesV1;
    if (context.sharedStorageBytes
            < planeOffset + componentActivationAudioPlaneBytesV1
        || context.sharedStorageBytes
            < componentActivationAudioStreamHealthPlaneOffsetV1
                + healthPlaneBytes)
    {
        context.activationFailed = true;
        context.loadedInstance.reset();
        sendActivationTransition(
            context,
            peer,
            event,
            ComponentActivationRecordKindV1::Terminal,
            ComponentActivationRuntimeStateV1::Quarantined,
            ComponentActivationRuntimeDiagnosticV1::ProtocolRejected);
        return;
    }
    context.worker = ComponentActivationWorkerV1::prepare(
        {static_cast<std::byte*>(context.sharedStorage) + planeOffset,
         componentActivationAudioPlaneBytesV1},
        context.authorization,
        *context.loadedInstance,
        workerDiagnostic,
        {static_cast<std::byte*>(context.sharedStorage)
                + componentActivationAudioStreamHealthPlaneOffsetV1,
         healthPlaneBytes});
    if (context.worker == nullptr || !context.worker->start())
    {
        context.activationFailed = true;
        context.worker.reset();
        context.loadedInstance.reset();
        sendActivationTransition(
            context,
            peer,
            event,
            ComponentActivationRecordKindV1::Terminal,
            ComponentActivationRuntimeStateV1::Quarantined,
            ComponentActivationRuntimeDiagnosticV1::PrepareRejected);
        return;
    }
    sendActivationTransition(
        context,
        peer,
        event,
        ComponentActivationRecordKindV1::Prepared,
        ComponentActivationRuntimeStateV1::Prepared,
        ComponentActivationRuntimeDiagnosticV1::Ready);
}
#endif

[[nodiscard]] bool mapPristineSharedStorage(
    PeerContext& context,
    xpc_object_t sharedMemory,
    const ComponentActivationSealedSessionV1& session) noexcept
{
    if (context.sharedStorage != nullptr || sharedMemory == nullptr
        || xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM
        || session.sharedStorageBytes > SIZE_MAX)
    {
        return false;
    }
    void* storage = nullptr;
    const auto storageBytes = xpc_shmem_map(sharedMemory, &storage);
    if (storage == nullptr
        || storageBytes != session.sharedStorageBytes
        || storageBytes < sizeof(session)
        || storageBytes
            < componentActivationAudioStreamHealthPlaneOffsetV1
                + componentActivationAudioStreamHealthPlaneBytesV1
        || storageBytes > maximumComponentActivationSharedStorageBytesV1)
    {
        if (storage != nullptr)
            static_cast<void>(munmap(storage, storageBytes));
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(storage);
    const auto pristine = std::memcmp(bytes, &session, sizeof(session)) == 0
        && std::all_of(
            bytes + sizeof(session),
            bytes + storageBytes,
            [](std::byte value) { return value == std::byte{}; });
    if (!pristine
        || mprotect(
            storage,
            componentActivationAudioCanaryPlaneOffsetV1,
            PROT_READ) != 0)
    {
        static_cast<void>(munmap(storage, storageBytes));
        return false;
    }
    context.session = session;
    context.sharedStorage = storage;
    context.sharedStorageBytes = storageBytes;
    context.sealed = true;
    return true;
}

[[nodiscard]] ComponentActivationTransitionV1 sealedTransition(
    PeerContext& context,
    ComponentActivationRecordKindV1 kind) noexcept
{
    ComponentActivationTransitionV1 transition;
    transition.header = componentActivationHeaderV1(kind, sizeof(transition));
    transition.runtimeSessionGeneration =
        context.session.runtimeSessionGeneration;
    transition.sharedStorageIdentity = context.session.sharedStorageIdentity;
    transition.runtimeSessionNonce = context.session.runtimeSessionNonce;
    transition.state = static_cast<std::uint32_t>(
        ComponentActivationRuntimeStateV1::Sealed);
    transition.diagnostic = static_cast<std::uint32_t>(
        ComponentActivationRuntimeDiagnosticV1::Ready);
    transition.sequence = ++context.sequence;
#if TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 1
    ++transition.header.protocolVersion;
#elif TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 3
    transition.runtimeSessionNonce.front() ^= 0xffU;
#endif
    return transition;
}

void rejectPeer(xpc_connection_t peer) noexcept
{
    xpc_connection_cancel(peer);
}

void sendTransition(
    PeerContext& context,
    xpc_connection_t peer,
    xpc_object_t request,
    ComponentActivationRecordKindV1 kind) noexcept
{
#if TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 2 || \
    TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 4
#if TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 2
    const auto withhold = true;
#else
    const auto withhold = kind == ComponentActivationRecordKindV1::Heartbeat;
#endif
    if (withhold)
    {
        if (context.withheldReply == nullptr)
            context.withheldReply = xpc_dictionary_create_reply(request);
        if (context.withheldReply == nullptr)
            rejectPeer(peer);
        return;
    }
#endif
    auto reply = xpc_dictionary_create_reply(request);
    const auto transition = sealedTransition(context, kind);
    if (reply == nullptr)
    {
        rejectPeer(peer);
        return;
    }
#if TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 1 || \
    TALAS_COMPONENT_ACTIVATION_HELPER_FIXTURE_MODE == 3
    xpc_dictionary_set_data(
        reply,
        "talas.component-activation.transition.v1",
        &transition,
        sizeof(transition));
#else
    if (!writeComponentActivationXpcSealedTransitionV1(
            reply, transition, kind, context.session))
    {
        xpc_release(reply);
        rejectPeer(peer);
        return;
    }
#ifdef TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE
    if (kind == ComponentActivationRecordKindV1::SealedReady
        && context.sandboxReportReady)
    {
        xpc_dictionary_set_data(
            reply,
            "talas.component-activation.sandbox-report.v1",
            &context.sandboxReport,
            sizeof(context.sandboxReport));
    }
#endif
#endif
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

void handlePeerEvent(xpc_connection_t peer, xpc_object_t event) noexcept
{
    auto* context = static_cast<PeerContext*>(
        xpc_connection_get_context(peer));
    if (context == nullptr || event == nullptr)
    {
        rejectPeer(peer);
        return;
    }
    if (xpc_get_type(event) == XPC_TYPE_ERROR)
    {
        // This helper serves exactly one peer and may still be running
        // untrusted component code. Once that peer is gone nothing may keep
        // running, even if the host could not terminate this process.
        _exit(EXIT_SUCCESS);
    }

    if (!context->sealed)
    {
        ComponentActivationSealedSessionV1 session;
        xpc_object_t sharedMemory = nullptr;
        if (decodeComponentActivationXpcSealedPrewarmV1(
                event, session, sharedMemory)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || !mapPristineSharedStorage(
                *context, sharedMemory, session))
        {
            rejectPeer(peer);
            return;
        }
#ifdef TALAS_COMPONENT_ACTIVATION_SANDBOX_PROBE_FIXTURE
        context->sandboxReport = runComponentActivationSandboxReportV1(
            xpc_connection_get_pid(peer));
        context->sandboxReportReady = true;
#endif
        sendTransition(
            *context,
            peer,
            event,
            ComponentActivationRecordKindV1::SealedReady);
        return;
    }

#if TALAS_COMPONENT_ACTIVATION_RUNTIME_LOADING
    const auto eventFieldCount = xpc_get_type(event) == XPC_TYPE_DICTIONARY
        ? xpc_dictionary_get_count(event)
        : 0;
    if (eventFieldCount == 5 || eventFieldCount == 6
        || eventFieldCount == 7)
    {
        handleActivationRequest(*context, peer, event);
        return;
    }
    if (context->activationFailed)
    {
        rejectPeer(peer);
        return;
    }
    if (context->loadedInstance != nullptr)
    {
        ComponentActivationCheckpointCommandV1 checkpoint;
        if (decodeComponentActivationXpcCheckpointCommandV1(event, context->authorization, checkpoint)
            == ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            handleCheckpointRequest(*context, peer, event, checkpoint);
            return;
        }
        ComponentActivationCommandV1 command;
        if (decodeComponentActivationXpcCommandV1(
                event,
                ComponentActivationRecordKindV1::Teardown,
                context->authorization,
                command)
                == ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            if (command.sequence != context->sequence + 1U)
            {
                rejectPeer(peer);
                return;
            }
            context->worker->stop();
            context->worker.reset();
            const auto clean = context->loadedInstance->shutdown();
            context->loadedInstance.reset();
            sendActivationTransition(
                *context,
                peer,
                event,
                clean ? ComponentActivationRecordKindV1::Teardown
                      : ComponentActivationRecordKindV1::Terminal,
                clean ? ComponentActivationRuntimeStateV1::Closed
                      : ComponentActivationRuntimeStateV1::Quarantined,
                clean ? ComponentActivationRuntimeDiagnosticV1::Closed
                      : ComponentActivationRuntimeDiagnosticV1::ProcessCrashed);
            return;
        }
        if (decodeComponentActivationXpcCommandV1(
                event,
                ComponentActivationRecordKindV1::Reset,
                context->authorization,
                command)
                == ComponentActivationXpcEnvelopeDiagnosticV1::Valid)
        {
            if (command.sequence != context->sequence + 1U)
            {
                rejectPeer(peer);
                return;
            }
            if (context->checkpointId != 0)
            {
                rejectPeer(peer);
                return;
            }
            context->worker->stop();
            context->worker.reset();
            ComponentActivationWorkerDiagnosticV1 workerDiagnostic;
            const auto setHealth =
                context->loadedInstance->audioStreamHealthSet();
            const auto healthPlaneBytes = setHealth.has_value()
                ? componentActivationAudioStreamHealthPlaneBytesV2
                : componentActivationAudioStreamHealthPlaneBytesV1;
            context->worker = ComponentActivationWorkerV1::prepare(
                {static_cast<std::byte*>(context->sharedStorage)
                        + componentActivationAudioRuntimePlaneOffsetV1,
                 componentActivationAudioPlaneBytesV1},
                context->authorization,
                *context->loadedInstance,
                workerDiagnostic,
                {static_cast<std::byte*>(context->sharedStorage)
                        + componentActivationAudioStreamHealthPlaneOffsetV1,
                 healthPlaneBytes});
            if (context->worker == nullptr || !context->worker->start())
            {
                context->activationFailed = true;
                context->worker.reset();
                sendActivationTransition(
                    *context,
                    peer,
                    event,
                    ComponentActivationRecordKindV1::Terminal,
                    ComponentActivationRuntimeStateV1::Quarantined,
                    ComponentActivationRuntimeDiagnosticV1::PrepareRejected);
                return;
            }
            context->runtimePrepared = true;
            sendActivationTransition(
                *context,
                peer,
                event,
                ComponentActivationRecordKindV1::Reset,
                ComponentActivationRuntimeStateV1::Prepared,
                ComponentActivationRuntimeDiagnosticV1::Ready);
            return;
        }
        if (decodeComponentActivationXpcCommandV1(
                event,
                ComponentActivationRecordKindV1::Heartbeat,
                context->authorization,
                command)
                != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
            || command.sequence != context->sequence + 1U)
        {
            rejectPeer(peer);
            return;
        }
        sendActivationTransition(
            *context,
            peer,
            event,
            ComponentActivationRecordKindV1::Heartbeat,
            ComponentActivationRuntimeStateV1::Prepared,
            ComponentActivationRuntimeDiagnosticV1::Ready);
        return;
    }
    if (context->activationFailed)
    {
        rejectPeer(peer);
        return;
    }
#endif

    ComponentActivationSealedSessionV1 heartbeat;
    if (decodeComponentActivationXpcSealedSessionV1(event, heartbeat)
            != ComponentActivationXpcEnvelopeDiagnosticV1::Valid
        || !exactSession(heartbeat, context->session))
    {
        rejectPeer(peer);
        return;
    }
    sendTransition(
        *context,
        peer,
        event,
        ComponentActivationRecordKindV1::Heartbeat);
}

void destroyPeerContext(void* rawContext)
{
    auto* context = static_cast<PeerContext*>(rawContext);
    if (context == nullptr)
        return;
    if (context->withheldReply != nullptr)
        xpc_release(context->withheldReply);
    // Capture the mapping, then destroy the complete context first. Member
    // destruction stops and joins the worker before it releases the loaded
    // instance, so neither can touch the shared planes after munmap.
    auto* sharedStorage = context->sharedStorage;
    const auto sharedStorageBytes = context->sharedStorageBytes;
    delete context;
    if (sharedStorage != nullptr)
    {
        static_cast<void>(munmap(
            sharedStorage,
            sharedStorageBytes));
    }
}

void acceptPeer(xpc_connection_t peer)
{
    bool expected = false;
    if (!peerClaimed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        rejectPeer(peer);
        return;
    }
    auto* context = new (std::nothrow) PeerContext;
    if (context == nullptr)
    {
        rejectPeer(peer);
        return;
    }
    xpc_connection_set_target_queue(peer, dispatch_get_main_queue());
    xpc_connection_set_context(peer, context);
    xpc_connection_set_finalizer_f(peer, destroyPeerContext);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        handlePeerEvent(peer, event);
    });
    xpc_connection_activate(peer);
}
}
}

int main()
{
    xpc_main(talas::runtime::component_activation::acceptPeer);
    return EXIT_FAILURE;
}
