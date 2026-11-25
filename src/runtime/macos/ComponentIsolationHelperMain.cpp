#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "runtime/component_isolation/ComponentIsolationProductAudioWorker.h"
#include "runtime/component_isolation/ComponentIsolationProductHelperSession.h"
#include "runtime/macos/ComponentIsolationXpcEnvelope.h"

#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
#include "runtime/macos/ComponentIsolationSandboxProbe.h"
#endif

#include <xpc/xpc.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <sys/mman.h>

#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
#include <cstring>
#endif

namespace
{
using namespace talas::runtime;

struct AudioPumpState
{
    ComponentIsolationProductAudioWorkerV1 worker;
    std::atomic<bool> closing{false};
};

struct PeerContext
{
    PeerContext() noexcept
        : session(
            ComponentIsolationProductServiceV1::GainFixture,
            static_cast<ComponentIsolationProductHelperFixtureModeV1>(
                fixtureMode)),
          audioQueue(dispatch_queue_create(
              "com.talas.component-host.audio-pump",
              DISPATCH_QUEUE_SERIAL))
    {
    }

    ~PeerContext()
    {
        if (audioPump != nullptr)
            audioPump->closing.store(true, std::memory_order_release);
        if (audioTimer != nullptr)
            dispatch_source_cancel(audioTimer);
        if (audioQueue != nullptr)
            dispatch_sync(audioQueue, ^{});
#if !OS_OBJECT_USE_OBJC
        if (audioTimer != nullptr)
            dispatch_release(audioTimer);
        if (audioQueue != nullptr)
            dispatch_release(audioQueue);
#endif
        audioTimer = nullptr;
        audioQueue = nullptr;
        audioPump.reset();
        if (withheldReply != nullptr)
            xpc_release(withheldReply);
        if (sharedRegion != nullptr)
            static_cast<void>(munmap(sharedRegion, sharedRegionBytes));
    }

#ifdef TALAS_COMPONENT_ISOLATION_HELPER_FIXTURE_MODE
    static constexpr int fixtureMode =
        TALAS_COMPONENT_ISOLATION_HELPER_FIXTURE_MODE;
#else
    static constexpr int fixtureMode = 0;
#endif

    ComponentIsolationProductHelperSessionV1 session;
    xpc_object_t withheldReply = nullptr;
    void* sharedRegion = nullptr;
    std::size_t sharedRegionBytes = 0;
    std::shared_ptr<AudioPumpState> audioPump;
    dispatch_queue_t audioQueue = nullptr;
    dispatch_source_t audioTimer = nullptr;
#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
    ComponentIsolationSandboxProbeReportV1 sandboxReport;
    bool sandboxProbeComplete = false;
#endif
};

[[nodiscard]] bool prepareAudioPump(
    PeerContext& context,
    std::size_t requestRegionBytes) noexcept
{
    if (context.audioQueue == nullptr || context.audioTimer != nullptr
        || context.audioPump != nullptr || context.sharedRegion == nullptr
        || requestRegionBytes == 0
        || requestRegionBytes >= context.sharedRegionBytes)
    {
        return false;
    }
    try
    {
        context.audioPump = std::make_shared<AudioPumpState>();
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    const auto* header = reinterpret_cast<
        const ComponentIsolationAudioSharedRegionHeaderV1*>(
        context.sharedRegion);
    ComponentIsolationSessionContractV1 contract;
    contract.validationNonce = header->validationNonce;
    contract.sessionGeneration = header->sessionGeneration;
    contract.sharedStorageIdentity = header->sharedStorageIdentity;
    contract.inputChannelCount = header->inputChannelCount;
    contract.outputChannelCount = header->outputChannelCount;
    contract.maximumFrames = header->maximumFrames;
    const auto request = std::span<const std::byte>{
        static_cast<const std::byte*>(context.sharedRegion),
        requestRegionBytes};
    const auto response = std::span<std::byte>{
        static_cast<std::byte*>(context.sharedRegion) + requestRegionBytes,
        context.sharedRegionBytes - requestRegionBytes};
    if (context.audioPump->worker.prepare(
            ComponentIsolationProductServiceV1::GainFixture,
            contract,
            request,
            response)
        != ComponentIsolationProductAudioWorkerPreparationDiagnosticV1::Ready)
    {
        context.audioPump.reset();
        return false;
    }
    return true;
}

[[nodiscard]] bool startAudioPump(PeerContext& context) noexcept
{
    if (context.audioQueue == nullptr || context.audioTimer != nullptr
        || context.audioPump == nullptr)
    {
        return false;
    }
    context.audioTimer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER,
        0,
        0,
        context.audioQueue);
    if (context.audioTimer == nullptr)
        return false;
    const auto pump = context.audioPump;
    dispatch_source_set_event_handler(context.audioTimer, ^{
        if (pump->closing.load(std::memory_order_acquire))
            return;
        for (std::size_t index = 0;
             index < componentIsolationAudioSlotCountV1;
             ++index)
        {
            const auto result = pump->worker.pump();
            if (result
                != ComponentIsolationProductAudioWorkerOperationV1::Published)
            {
                return;
            }
        }
    });
    dispatch_source_set_timer(
        context.audioTimer,
        dispatch_time(DISPATCH_TIME_NOW, 0),
        NSEC_PER_USEC * 250,
        NSEC_PER_USEC * 50);
    dispatch_resume(context.audioTimer);
    return true;
}

template <typename Value>
[[nodiscard]] Value loadShared(
    const Value& source,
    std::memory_order order = std::memory_order_relaxed) noexcept
{
    return std::atomic_ref<Value>(const_cast<Value&>(source)).load(order);
}

[[nodiscard]] bool mapSharedAudio(
    PeerContext& context,
    xpc_object_t sharedMemory,
    const ComponentIsolationHandshakeV1& handshake) noexcept
{
    if (context.sharedRegion != nullptr
        || sharedMemory == nullptr
        || xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
    {
        return false;
    }
    void* mappedRegion = nullptr;
    const auto mappedBytes = xpc_shmem_map(sharedMemory, &mappedRegion);
    if (mappedRegion == nullptr
        || mappedBytes
            < sizeof(ComponentIsolationAudioSharedRegionHeaderV1)
        || mappedBytes > maximumComponentIsolationAudioStorageBytesV1
        || mappedBytes
                % componentIsolationSharedProtectionPageBytesV1
            != 0)
    {
        if (mappedRegion != nullptr)
            static_cast<void>(munmap(mappedRegion, mappedBytes));
        return false;
    }

    const auto* header = reinterpret_cast<
        const ComponentIsolationAudioSharedRegionHeaderV1*>(mappedRegion);
    const auto publication = loadShared(
        header->publication,
        std::memory_order_acquire);
    if ((publication & 1U) != 0U)
    {
        static_cast<void>(munmap(mappedRegion, mappedBytes));
        return false;
    }
    const auto generation = loadShared(header->sessionGeneration);
    const auto storageIdentity = loadShared(header->sharedStorageIdentity);
    std::array<
        std::uint8_t,
        componentIsolationValidationNonceBytesV1> nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index)
        nonce[index] = loadShared(header->validationNonce[index]);
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto confirmedPublication = loadShared(header->publication);

    const auto requestBytes = header->responseRegionOffset;
    const auto valid = confirmedPublication == publication
        && (confirmedPublication & 1U) == 0U
        && header->magic == componentIsolationAudioSharedRegionMagicV1
        && header->protocolVersion
            == currentComponentIsolationProtocolVersion
        && header->headerBytes
            == sizeof(ComponentIsolationAudioSharedRegionHeaderV1)
        && header->regionBytes == mappedBytes
        && requestBytes > 0
        && requestBytes < mappedBytes
        && requestBytes
                % componentIsolationSharedProtectionPageBytesV1
            == 0
        && generation == handshake.header.sessionGeneration
        && storageIdentity == handshake.sharedStorageIdentity
        && nonce == handshake.header.validationNonce
        && header->slotCount == componentIsolationAudioSlotCountV1
        && header->inputChannelCount == handshake.inputChannelCount
        && header->outputChannelCount == handshake.outputChannelCount
        && header->maximumFrames == handshake.maximumFrames
        && header->reserved0 == 0
        && header->reserved1 == 0;
    if (!valid
        || mprotect(mappedRegion, requestBytes, PROT_READ) != 0
        || mprotect(
               static_cast<std::byte*>(mappedRegion) + requestBytes,
               mappedBytes - requestBytes,
               PROT_READ | PROT_WRITE)
            != 0)
    {
        static_cast<void>(munmap(mappedRegion, mappedBytes));
        return false;
    }
    context.sharedRegion = mappedRegion;
    context.sharedRegionBytes = mappedBytes;
    if (!prepareAudioPump(context, requestBytes))
    {
        static_cast<void>(munmap(mappedRegion, mappedBytes));
        context.sharedRegion = nullptr;
        context.sharedRegionBytes = 0;
        return false;
    }
    return true;
}

#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
void attachSandboxProbeReport(
    PeerContext& context,
    const ComponentIsolationControlRecordV1& request,
    ComponentIsolationProductHelperResultV1& result) noexcept
{
    if (PeerContext::fixtureMode != static_cast<int>(
            ComponentIsolationProductHelperFixtureModeV1::SandboxProbe)
        || static_cast<ComponentIsolationRecordKind>(
               request.header.recordKind)
            != ComponentIsolationRecordKind::TeardownRequest
        || result.action != ComponentIsolationProductHelperActionV1::Respond
        || result.responseKind
            != ComponentIsolationProductHelperResponseKindV1::Control
        || !context.sandboxProbeComplete)
    {
        return;
    }
    static_assert(
        sizeof(ComponentIsolationSandboxProbeReportV1)
        <= maximumComponentIsolationControlPayloadBytesV1);
    result.control.outcome = static_cast<std::uint32_t>(
        ComponentIsolationControlOutcome::ControlledFailure);
    result.control.diagnosticBytes = sizeof(context.sandboxReport);
    result.diagnosticBytes = sizeof(context.sandboxReport);
    std::memcpy(
        result.diagnosticStorage.data(),
        &context.sandboxReport,
        sizeof(context.sandboxReport));
}
#endif

void destroyPeerContext(void* context)
{
    delete static_cast<PeerContext*>(context);
}

void rejectPeer(xpc_connection_t peer) noexcept
{
    xpc_connection_cancel(peer);
}

void sendResult(
    PeerContext& context,
    xpc_connection_t peer,
    xpc_object_t request,
    const ComponentIsolationProductHelperResultV1& result) noexcept
{
    switch (result.action)
    {
    case ComponentIsolationProductHelperActionV1::Reject:
        rejectPeer(peer);
        return;
    case ComponentIsolationProductHelperActionV1::Crash:
        __builtin_trap();
    case ComponentIsolationProductHelperActionV1::Hang:
        context.withheldReply = xpc_dictionary_create_reply(request);
        if (context.withheldReply == nullptr)
            rejectPeer(peer);
        return;
    case ComponentIsolationProductHelperActionV1::Exit:
        std::_Exit(EXIT_SUCCESS);
    case ComponentIsolationProductHelperActionV1::Respond:
        break;
    }

    auto reply = xpc_dictionary_create_reply(request);
    if (reply == nullptr
        || !writeComponentIsolationXpcResponseV1(reply, result))
    {
        if (reply != nullptr)
            xpc_release(reply);
        rejectPeer(peer);
        return;
    }
    const auto delayHeartbeatReply = PeerContext::fixtureMode
            == static_cast<int>(
                ComponentIsolationProductHelperFixtureModeV1::HeartbeatLate)
        && result.responseKind
            == ComponentIsolationProductHelperResponseKindV1::Control
        && static_cast<ComponentIsolationRecordKind>(
               result.control.header.recordKind)
            == ComponentIsolationRecordKind::Heartbeat
        && result.control.controlSequence > 1;
    if (delayHeartbeatReply)
    {
        xpc_retain(peer);
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_MSEC * 750),
            dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
            ^{
                xpc_connection_send_message(peer, reply);
                xpc_release(reply);
                xpc_release(peer);
            });
        return;
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    if (result.responseKind
            == ComponentIsolationProductHelperResponseKindV1::Control
        && static_cast<ComponentIsolationRecordKind>(
               result.control.header.recordKind)
            == ComponentIsolationRecordKind::TeardownResponse)
    {
        xpc_connection_cancel(peer);
    }
}

void handlePeerEvent(xpc_connection_t peer, xpc_object_t event) noexcept
{
    if (event == nullptr)
        return;

    if (xpc_get_type(event) == XPC_TYPE_ERROR)
    {
        auto* context = static_cast<PeerContext*>(
            xpc_connection_get_context(peer));
        if (context != nullptr && context->withheldReply != nullptr)
        {
            xpc_release(context->withheldReply);
            context->withheldReply = nullptr;
        }
        xpc_connection_cancel(peer);
        return;
    }

    ComponentIsolationXpcEnvelopeV1 envelope;
    if (decodeComponentIsolationXpcEnvelopeV1(event, envelope)
        != ComponentIsolationXpcEnvelopeDiagnosticV1::Valid)
    {
        rejectPeer(peer);
        return;
    }
    auto* context = static_cast<PeerContext*>(
        xpc_connection_get_context(peer));
    if (context == nullptr)
    {
        rejectPeer(peer);
        return;
    }
    if (envelope.kind == ComponentIsolationXpcEnvelopeKindV1::Handshake)
    {
        auto result = context->session.handleHandshake(envelope.handshake);
        if ((result.action
                    == ComponentIsolationProductHelperActionV1::Respond
                || result.action
                    == ComponentIsolationProductHelperActionV1::Hang)
            && !mapSharedAudio(
                    *context,
                    envelope.sharedMemory,
                    envelope.handshake))
        {
            rejectPeer(peer);
            return;
        }
#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
        if (PeerContext::fixtureMode == static_cast<int>(
                ComponentIsolationProductHelperFixtureModeV1::SandboxProbe)
            && result.action
                == ComponentIsolationProductHelperActionV1::Respond
            && result.responseKind
                == ComponentIsolationProductHelperResponseKindV1::Handshake)
        {
            context->sandboxReport = runComponentIsolationSandboxProbeV1(
                xpc_connection_get_pid(peer));
            context->sandboxProbeComplete = true;
        }
#endif
        sendResult(*context, peer, event, result);
        return;
    }
    if (envelope.kind == ComponentIsolationXpcEnvelopeKindV1::Control
        && envelope.diagnosticBytes == 0)
    {
        auto result = context->session.handleControl(envelope.control);
        const auto acceptedPrewarm = result.action
                == ComponentIsolationProductHelperActionV1::Respond
            && result.responseKind
                == ComponentIsolationProductHelperResponseKindV1::Control
            && static_cast<ComponentIsolationRecordKind>(
                   envelope.control.header.recordKind)
                == ComponentIsolationRecordKind::Heartbeat
            && envelope.control.controlSequence == 1;
        if (acceptedPrewarm && !startAudioPump(*context))
        {
            rejectPeer(peer);
            return;
        }
#ifdef TALAS_COMPONENT_ISOLATION_SANDBOX_PROBE_FIXTURE
        attachSandboxProbeReport(*context, envelope.control, result);
#endif
        sendResult(*context, peer, event, result);
        return;
    }
    rejectPeer(peer);
}

void acceptPeer(xpc_connection_t peer)
{
    auto* context = new PeerContext;
    xpc_connection_set_context(peer, context);
    xpc_connection_set_finalizer_f(peer, destroyPeerContext);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        handlePeerEvent(peer, event);
    });
    xpc_connection_resume(peer);
}
}

int main()
{
    xpc_main(acceptPeer);
}
