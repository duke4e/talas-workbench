#include "runtime/component_probe/macos/ComponentBundleProbeLoader.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMaterialization.h"
#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <xpc/xpc.h>

#include <libproc.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

namespace
{
namespace probe = talas::runtime::component_probe;

enum class ProbeHelperPhase : std::uint8_t
{
    AwaitingRequest,
    AwaitingAcknowledgement,
    Running,
    AwaitingFinalization,
    TerminationReady,
};

std::atomic<ProbeHelperPhase> phase{ProbeHelperPhase::AwaitingRequest};
std::atomic<bool> peerClaimed{false};
probe::ComponentBundleProbeXpcRequestEnvelopeV1 pendingEnvelope;
xpc_object_t pendingRequestEvent = nullptr;

[[noreturn]] void finishWithoutReply(xpc_connection_t peer) noexcept
{
    if (pendingRequestEvent != nullptr)
    {
        xpc_release(pendingRequestEvent);
        pendingRequestEvent = nullptr;
    }
    xpc_connection_cancel(peer);
    std::_Exit(EXIT_FAILURE);
}

bool sendStarted(
    xpc_connection_t peer,
    const probe::ComponentBundleProbeRequestV1& request) noexcept
{
    const auto process = getpid();
    proc_bsdinfo processInfo{};
    if (process <= 0
        || proc_pidinfo(
               process,
               PROC_PIDTBSDINFO,
               0,
               &processInfo,
               sizeof(processInfo)) != static_cast<int>(sizeof(processInfo))
        || processInfo.pbi_pid != static_cast<std::uint32_t>(process)
        || processInfo.pbi_uid != geteuid()
        || processInfo.pbi_start_tvsec == 0
        || processInfo.pbi_start_tvusec >= 1'000'000)
    {
        return false;
    }
    probe::ComponentBundleProbeStartedV1 started;
    started.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Started,
        sizeof(started));
    started.contentDigest = request.contentDigest;
    started.artifactGeneration = request.artifactGeneration;
    started.validationNonce = request.validationNonce;
    started.processIdentifier = static_cast<std::uint32_t>(process);
    started.effectiveUser = processInfo.pbi_uid;
    started.startSeconds = processInfo.pbi_start_tvsec;
    started.startMicroseconds = processInfo.pbi_start_tvusec;
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr
        || !probe::writeComponentBundleProbeXpcStartedV1(
            message,
            started,
            request))
    {
        if (message != nullptr)
            xpc_release(message);
        return false;
    }
    xpc_connection_send_message(peer, message);
    xpc_release(message);
    return true;
}

void handleRequest(xpc_connection_t peer, xpc_object_t event) noexcept
{
    probe::ComponentBundleProbeXpcRequestEnvelopeV1 envelope;
    if (probe::decodeComponentBundleProbeXpcRequestV1(event, envelope)
            != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
        || !sendStarted(peer, envelope.request))
    {
        finishWithoutReply(peer);
    }
    pendingEnvelope = envelope;
    xpc_retain(event);
    pendingRequestEvent = event;
}

void handleAcknowledgement(
    xpc_connection_t peer,
    xpc_object_t event) noexcept
{
    probe::ComponentBundleProbeAcknowledgementV1 acknowledgement;
    if (pendingRequestEvent == nullptr
        || probe::decodeComponentBundleProbeXpcAcknowledgementV1(
               event,
               pendingEnvelope.request,
               acknowledgement)
            != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
    {
        finishWithoutReply(peer);
    }
    const auto result = probe::runComponentBundleProbeLoaderV1(
        pendingEnvelope.request,
        pendingEnvelope.expectedAbi,
        pendingEnvelope.entryBytes);
    auto reply = xpc_dictionary_create_reply(pendingRequestEvent);
    if (reply == nullptr
        || !probe::writeComponentBundleProbeXpcReplyV1(
            reply,
            result,
            pendingEnvelope.request))
    {
        if (reply != nullptr)
            xpc_release(reply);
        finishWithoutReply(peer);
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    xpc_release(pendingRequestEvent);
    pendingRequestEvent = nullptr;
    phase.store(
        ProbeHelperPhase::AwaitingFinalization,
        std::memory_order_release);
}

void handleFinalization(
    xpc_connection_t peer,
    xpc_object_t event) noexcept
{
    probe::ComponentBundleProbeFinalizationV1 finalization;
    if (probe::decodeComponentBundleProbeXpcFinalizationV1(
            event,
            pendingEnvelope.request,
            finalization)
        != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
    {
        finishWithoutReply(peer);
    }
    auto reply = xpc_dictionary_create_reply(event);
    probe::ComponentBundleProbeTerminationReadyV1 ready;
    ready.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::TerminationReady,
        sizeof(ready));
    ready.contentDigest = pendingEnvelope.request.contentDigest;
    ready.artifactGeneration = pendingEnvelope.request.artifactGeneration;
    ready.validationNonce = pendingEnvelope.request.validationNonce;
    if (reply == nullptr
        || !probe::writeComponentBundleProbeXpcTerminationReadyV1(
            reply,
            ready,
            pendingEnvelope.request))
    {
        if (reply != nullptr)
            xpc_release(reply);
        finishWithoutReply(peer);
    }
    phase.store(ProbeHelperPhase::TerminationReady, std::memory_order_release);
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

void handlePeerEvent(xpc_connection_t peer, xpc_object_t event) noexcept
{
    if (event == nullptr || xpc_get_type(event) == XPC_TYPE_ERROR)
        finishWithoutReply(peer);

    auto current = phase.load(std::memory_order_acquire);
    if (current == ProbeHelperPhase::AwaitingRequest)
    {
        if (!phase.compare_exchange_strong(
                current,
                ProbeHelperPhase::AwaitingAcknowledgement,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            finishWithoutReply(peer);
        }
        handleRequest(peer, event);
        return;
    }
    if (current == ProbeHelperPhase::AwaitingAcknowledgement)
    {
        if (!phase.compare_exchange_strong(
                current,
                ProbeHelperPhase::Running,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            finishWithoutReply(peer);
        }
        handleAcknowledgement(peer, event);
        return;
    }
    if (current == ProbeHelperPhase::AwaitingFinalization)
    {
        if (!phase.compare_exchange_strong(
                current,
                ProbeHelperPhase::Running,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            finishWithoutReply(peer);
        }
        handleFinalization(peer, event);
        return;
    }
    finishWithoutReply(peer);
}

void acceptPeer(xpc_connection_t peer)
{
    if (peerClaimed.exchange(true, std::memory_order_acq_rel))
    {
        xpc_connection_set_event_handler(peer, ^(xpc_object_t) {});
        xpc_connection_resume(peer);
        xpc_connection_cancel(peer);
        return;
    }
    xpc_connection_set_target_queue(peer, dispatch_get_main_queue());
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        handlePeerEvent(peer, event);
    });
    xpc_connection_resume(peer);
}
}

int main()
{
    constexpr std::uint64_t staleMaterializationSeconds = 10U * 60U;
    static_cast<void>(
        talas::runtime::component_probe::sweepStaleComponentBundleProbeMaterializationsV1(
            staleMaterializationSeconds));
    xpc_main(acceptPeer);
}
