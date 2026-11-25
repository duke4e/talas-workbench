#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <xpc/xpc.h>

#include <libproc.h>

#include <atomic>
#include <cstdlib>
#include <unistd.h>

#ifndef TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE
#error TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE is required
#endif

namespace
{
namespace probe = talas::runtime::component_probe;

enum class FixturePhase : std::uint8_t
{
    AwaitingRequest,
    AwaitingAcknowledgement,
    Running,
};

std::atomic<FixturePhase> phase{FixturePhase::AwaitingRequest};
std::atomic<bool> peerClaimed{false};
probe::ComponentBundleProbeRequestV1 pendingRequest;
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

void finish(xpc_connection_t peer) noexcept
{
    xpc_connection_send_barrier(peer, ^{
        xpc_connection_cancel(peer);
        std::_Exit(EXIT_SUCCESS);
    });
}

[[maybe_unused]] bool sendStarted(
    xpc_connection_t peer,
    const probe::ComponentBundleProbeRequestV1& request,
    std::uint32_t processIdentifier) noexcept
{
    proc_bsdinfo processInfo{};
    const auto actualProcess = getpid();
    if (actualProcess <= 0
        || proc_pidinfo(
               actualProcess,
               PROC_PIDTBSDINFO,
               0,
               &processInfo,
               sizeof(processInfo)) != static_cast<int>(sizeof(processInfo))
        || processInfo.pbi_pid != static_cast<std::uint32_t>(actualProcess)
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
    started.processIdentifier = processIdentifier;
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

[[maybe_unused]] void replyCleanNow(
    xpc_connection_t peer,
    xpc_object_t event,
    const probe::ComponentBundleProbeRequestV1& request) noexcept
{
    probe::ComponentBundleProbeResultV1 result;
    result.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(
        talas::runtime::ComponentProbeState::Clean);
    result.diagnostic = static_cast<std::uint32_t>(
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    auto reply = xpc_dictionary_create_reply(event);
    if (reply == nullptr
        || !probe::writeComponentBundleProbeXpcReplyV1(
            reply,
            result,
            request))
    {
        if (reply != nullptr)
            xpc_release(reply);
        finishWithoutReply(peer);
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    finish(peer);
}

[[maybe_unused]] void replyCleanThenCrash(
    xpc_connection_t peer,
    xpc_object_t event,
    const probe::ComponentBundleProbeRequestV1& request) noexcept
{
    probe::ComponentBundleProbeResultV1 result;
    result.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(
        talas::runtime::ComponentProbeState::Clean);
    result.diagnostic = static_cast<std::uint32_t>(
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    auto reply = xpc_dictionary_create_reply(event);
    if (reply == nullptr
        || !probe::writeComponentBundleProbeXpcReplyV1(
            reply,
            result,
            request))
    {
        if (reply != nullptr)
            xpc_release(reply);
        finishWithoutReply(peer);
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    xpc_connection_send_barrier(peer, ^{
        __builtin_trap();
    });
}

#if TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 1
void replyMalformed(xpc_connection_t peer, xpc_object_t event) noexcept
{
    auto reply = xpc_dictionary_create_reply(event);
    if (reply == nullptr)
        std::_Exit(EXIT_FAILURE);
    xpc_dictionary_set_string(reply, "unexpected", "malformed");
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    xpc_release(event);
    pendingRequestEvent = nullptr;
    finish(peer);
}
#else

[[maybe_unused]] void replyCleanAfterDelay(
    xpc_connection_t peer,
    xpc_object_t event,
    probe::ComponentBundleProbeRequestV1 request,
    std::int64_t delayNanoseconds) noexcept
{
    xpc_retain(peer);
    pendingRequestEvent = nullptr;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, delayNanoseconds),
        dispatch_get_main_queue(),
        ^{
            probe::ComponentBundleProbeResultV1 result;
            result.header = probe::componentBundleProbeHeaderV1(
                probe::ComponentBundleProbeRecordKindV1::Result,
                sizeof(result));
            result.state = static_cast<std::uint32_t>(
                talas::runtime::ComponentProbeState::Clean);
            result.diagnostic = static_cast<std::uint32_t>(
                probe::ComponentBundleProbeResultDiagnosticV1::Ready);
            result.contentDigest = request.contentDigest;
            result.artifactGeneration = request.artifactGeneration;
            result.validationNonce = request.validationNonce;
            auto reply = xpc_dictionary_create_reply(event);
            if (reply != nullptr
                && probe::writeComponentBundleProbeXpcReplyV1(
                    reply,
                    result,
                    request))
            {
                xpc_connection_send_message(peer, reply);
                xpc_release(reply);
                finish(peer);
            }
            else
            {
                if (reply != nullptr)
                    xpc_release(reply);
                std::_Exit(EXIT_FAILURE);
            }
            xpc_release(event);
            xpc_release(peer);
        });
}
#endif

void handleEvent(xpc_connection_t peer, xpc_object_t event) noexcept
{
    if (event == nullptr || xpc_get_type(event) == XPC_TYPE_ERROR)
        finishWithoutReply(peer);
    auto current = phase.load(std::memory_order_acquire);
    if (current == FixturePhase::AwaitingRequest)
    {
        if (!phase.compare_exchange_strong(
                current,
                FixturePhase::AwaitingAcknowledgement,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            finishWithoutReply(peer);
        }
        probe::ComponentBundleProbeXpcRequestEnvelopeV1 envelope;
        if (probe::decodeComponentBundleProbeXpcRequestV1(event, envelope)
            != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
        {
            finishWithoutReply(peer);
        }
#if TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 4
        phase.store(FixturePhase::Running, std::memory_order_release);
        replyCleanNow(peer, event, envelope.request);
#else
        auto startedProcess = static_cast<std::uint32_t>(getpid());
#if TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 5
        ++startedProcess;
#endif
        if (!sendStarted(peer, envelope.request, startedProcess))
            finishWithoutReply(peer);
        pendingRequest = envelope.request;
        xpc_retain(event);
        pendingRequestEvent = event;
#endif
        return;
    }
    if (current != FixturePhase::AwaitingAcknowledgement
        || !phase.compare_exchange_strong(
            current,
            FixturePhase::Running,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        finishWithoutReply(peer);
    }
    probe::ComponentBundleProbeAcknowledgementV1 acknowledgement;
    if (pendingRequestEvent == nullptr
        || probe::decodeComponentBundleProbeXpcAcknowledgementV1(
               event,
               pendingRequest,
               acknowledgement)
            != probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
    {
        finishWithoutReply(peer);
    }
#if TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 1
    replyMalformed(peer, pendingRequestEvent);
#elif TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 2
    replyCleanAfterDelay(
        peer,
        pendingRequestEvent,
        pendingRequest,
        300 * NSEC_PER_MSEC);
#elif TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 3
    replyCleanAfterDelay(
        peer,
        pendingRequestEvent,
        pendingRequest,
        1500 * NSEC_PER_MSEC);
#elif TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 4
    finishWithoutReply(peer);
#elif TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 5
    finishWithoutReply(peer);
#elif TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE == 6
    replyCleanThenCrash(peer, pendingRequestEvent, pendingRequest);
#else
#error Unknown TALAS_COMPONENT_PROBE_COORDINATOR_FIXTURE_MODE
#endif
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
        handleEvent(peer, event);
    });
    xpc_connection_resume(peer);
}
}

int main()
{
    xpc_main(acceptPeer);
}
