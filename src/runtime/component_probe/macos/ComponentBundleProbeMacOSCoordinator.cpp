#include "runtime/component_probe/macos/ComponentBundleProbeMacOSCoordinator.h"

#include "runtime/component_bundle/ComponentBundleOwnerAuthorization.h"
#include "runtime/component_probe/ComponentBundleProbeEvidence.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMaterialization.h"
#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <Security/SecRandom.h>
#include <dispatch/dispatch.h>
#include <libproc.h>
#include <xpc/xpc.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <unistd.h>

namespace talas::runtime::component_probe
{
namespace
{
constexpr char productProbeServiceIdentityV1[] =
    "com.talas.workbench.component-probe";
constexpr std::int64_t productionDeadlineNanosecondsV1 = 5 * NSEC_PER_SEC;
constexpr std::int64_t testDeadlineNanosecondsV1 = NSEC_PER_SEC;

std::atomic<std::uint64_t> nextArtifactGenerationV1{1};

struct ProbeProcessIdentityV1
{
    std::int32_t processIdentifier = 0;
    std::uint64_t startSeconds = 0;
    std::uint64_t startMicroseconds = 0;
    uid_t effectiveUser = 0;
    bool valid = false;

    bool operator==(const ProbeProcessIdentityV1&) const = default;
};

[[nodiscard]] ProbeProcessIdentityV1 probeProcessIdentity(
    pid_t process) noexcept
{
    ProbeProcessIdentityV1 identity;
    if (process <= 0)
        return identity;
    proc_bsdinfo info{};
    if (proc_pidinfo(
            process,
            PROC_PIDTBSDINFO,
            0,
            &info,
            sizeof(info)) != static_cast<int>(sizeof(info))
        || info.pbi_pid != static_cast<std::uint32_t>(process)
        || info.pbi_uid != geteuid() || info.pbi_start_tvsec == 0)
    {
        return identity;
    }
    identity.processIdentifier = static_cast<std::int32_t>(process);
    identity.startSeconds = info.pbi_start_tvsec;
    identity.startMicroseconds = info.pbi_start_tvusec;
    identity.effectiveUser = info.pbi_uid;
    identity.valid = true;
    return identity;
}

[[nodiscard]] std::uint64_t claimArtifactGeneration() noexcept
{
    auto current = nextArtifactGenerationV1.load(std::memory_order_relaxed);
    while (current != 0
        && current != std::numeric_limits<std::uint64_t>::max())
    {
        if (nextArtifactGenerationV1.compare_exchange_weak(
                current,
                current + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return current;
        }
    }
    return 0;
}

[[nodiscard]] ComponentApprovalFingerprint candidateFor(
    const ComponentBundleProbeMacOSCoordinatorInputsV1& inputs) noexcept
{
    const auto signature = inputs.signature.inspect();
    ComponentApprovalFingerprint candidate;
    candidate.bundleId = inputs.snapshot.manifest.bundleId;
    candidate.semanticVersion = inputs.snapshot.manifest.semanticVersion;
    candidate.contentDigest = inputs.snapshot.contentDigest;
    candidate.signerIdentityDigest = signature.signerIdentityDigest;
    candidate.requestedPermissions =
        inputs.snapshot.manifest.requestedPermissions;
    candidate.policyVersion = currentComponentTrustPolicyVersion;
    candidate.hasContentDigest = true;
    candidate.hasSignerIdentity = signature.hasSignerIdentity;
    return candidate;
}

[[nodiscard]] ComponentTrustDecision evaluateProbeEligibility(
    const ComponentBundleProbeMacOSCoordinatorInputsV1& inputs,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    ComponentTrustEvidence evidence;
    evidence.candidate = candidateFor(inputs);
    evidence.ownerAuthorizationEvidence = inputs.ownerAuthorization;
    evidence.signature = inputs.signature;
    evidence.validationNonce = request.validationNonce;
    evidence.artifactGeneration = request.artifactGeneration;
    evidence.declaredRoles = inputs.snapshot.manifest.declaredRoles;
    evidence.hasValidationNonce = true;
    return evaluateComponentTrust(evidence);
}

[[nodiscard]] bool eligibleForProbe(ComponentTrustDecision decision) noexcept
{
    return decision
        == ComponentTrustDecision{
            ComponentTrustDisposition::EligibleForProbe,
            ComponentTrustDiagnosticCode::ProbeRequired};
}

[[nodiscard]] const component_bundle::ComponentBundleFrozenPayload*
entryPayload(const component_bundle::ComponentBundleSnapshot& snapshot)
    noexcept
{
    const component_bundle::ComponentBundleFrozenPayload* result = nullptr;
    for (const auto& payload : snapshot.payloads)
    {
        if (!payload.isEntry)
            continue;
        if (result != nullptr)
            return nullptr;
        result = &payload;
    }
    if (result == nullptr
        || result->path != snapshot.manifest.entry.path
        || result->sha256 != snapshot.manifest.entry.sha256
        || result->bytes.size() != snapshot.manifest.entry.byteCount)
    {
        return nullptr;
    }
    return result;
}

[[nodiscard]] bool makeProtocolRecords(
    const ComponentBundleProbeMacOSCoordinatorInputsV1& inputs,
    const component_bundle::ComponentBundleFrozenPayload& entry,
    std::uint64_t generation,
    const std::array<std::uint8_t, componentProbeNonceBytes>& nonce,
    ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeExpectedAbiV1& expected) noexcept
{
    const auto signature = inputs.signature.inspect();
    const auto& manifest = inputs.snapshot.manifest;
    request.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    request.bundleIdBytes = static_cast<std::uint32_t>(manifest.bundleId.size());
    request.semanticVersionBytes =
        static_cast<std::uint32_t>(manifest.semanticVersion.size());
    std::copy(
        manifest.bundleId.begin(),
        manifest.bundleId.end(),
        request.bundleId.begin());
    std::copy(
        manifest.semanticVersion.begin(),
        manifest.semanticVersion.end(),
        request.semanticVersion.begin());
    request.contentDigest = inputs.snapshot.contentDigest;
    request.signerIdentityDigest = signature.signerIdentityDigest;
    request.requestedPermissions = manifest.requestedPermissions;
    request.declaredRoles = manifest.declaredRoles;
    request.policyVersion = currentComponentTrustPolicyVersion;
    request.entryByteCount = entry.bytes.size();
    request.artifactGeneration = generation;
    request.validationNonce = nonce;

    expected.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(expected));
    expected.entryDigest = entry.sha256;
    expected.entryByteCount = entry.bytes.size();
    expected.minimumEngineAbiVersion = manifest.minimumEngineAbiVersion;
    expected.maximumEngineAbiVersion = manifest.maximumEngineAbiVersion;
    expected.minimumEngineApiVersion = manifest.minimumEngineApiVersion;
    expected.maximumEngineApiVersion = manifest.maximumEngineApiVersion;
    expected.nodeCount = static_cast<std::uint32_t>(manifest.nodes.size());
    for (std::size_t index = 0; index < manifest.nodes.size(); ++index)
    {
        const auto& typeId = manifest.nodes[index].typeId;
        expected.nodeIdBytes[index] =
            static_cast<std::uint32_t>(typeId.size());
        std::copy(
            typeId.begin(),
            typeId.end(),
            expected.nodeIds[index].begin());
    }
    return validateComponentBundleProbeExpectedAbiV1(expected, request)
        == ComponentBundleProbeProtocolDiagnosticV1::Valid;
}

[[nodiscard]] constexpr ComponentBundleProbeMacOSCoordinatorDiagnosticV1
coordinatorDiagnostic(ComponentProbeState state) noexcept
{
    switch (state)
    {
        case ComponentProbeState::Clean:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::Ready;
        case ComponentProbeState::Rejected:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProbeRejected;
        case ComponentProbeState::ControlledFatal:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ControlledFatal;
        case ComponentProbeState::AbnormalTermination:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProcessCrashed;
        case ComponentProbeState::TimedOut:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                DeadlineExpired;
        case ComponentProbeState::ProtocolError:
        case ComponentProbeState::NotRun:
            return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                ProtocolRejected;
    }
    return ComponentBundleProbeMacOSCoordinatorDiagnosticV1::InternalFailure;
}
}

struct ComponentBundleProbeMacOSCoordinator::Impl
    : std::enable_shared_from_this<ComponentBundleProbeMacOSCoordinator::Impl>
{
    Impl(
        ComponentBundleProbeMacOSCoordinatorInputsV1 ownedInputs,
        ComponentBundleProbeRequestV1 ownedRequest,
        ComponentBundleProbeExpectedAbiV1 ownedExpected,
        ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1 ownedDeadline,
        dispatch_queue_t ownedQueue) noexcept
        : inputs(std::move(ownedInputs)),
          request(ownedRequest),
          expected(ownedExpected),
          deadlineProfile(ownedDeadline),
          controlQueue(ownedQueue)
    {
    }

    ~Impl()
    {
        if (connection != nullptr)
        {
            xpc_connection_cancel(connection);
            xpc_release(connection);
        }
#if !OS_OBJECT_USE_OBJC
        if (controlQueue != nullptr)
            dispatch_release(controlQueue);
#endif
    }

    [[nodiscard]] std::int64_t deadlineNanoseconds() const noexcept
    {
        return deadlineProfile
                == ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1::
                    TestShort
            ? testDeadlineNanosecondsV1
            : productionDeadlineNanosecondsV1;
    }

    [[nodiscard]] bool running() const noexcept
    {
        return state.load(std::memory_order_acquire)
            == static_cast<std::uint8_t>(
                ComponentBundleProbeMacOSCoordinatorStateV1::Running);
    }

    [[nodiscard]] bool tryClaimTerminal() noexcept
    {
        auto expected = false;
        return terminalClaimed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void invalidateDeadline() noexcept
    {
        ++deadlineToken;
        if (deadlineToken == 0)
            ++deadlineToken;
    }

    void updateProcessIdentifier() noexcept
    {
        if (connection == nullptr)
            return;
        const auto process = xpc_connection_get_pid(connection);
        if (process > 0)
        {
            processIdentifier.store(
                static_cast<std::int32_t>(process),
                std::memory_order_release);
        }
    }

    [[nodiscard]] bool closeConnection(
        bool terminate,
        bool requireExactStartedProcess = false) noexcept
    {
        bool terminatedCleanly = true;
        bool exactTerminationObserved = !requireExactStartedProcess;
        if (connection != nullptr)
        {
            updateProcessIdentifier();
            const auto process = xpc_connection_get_pid(connection);
            const auto exactStartedProcess = processIdentity.valid
                && processIdentity.processIdentifier == process
                && probeProcessIdentity(process) == processIdentity;
            if (terminate && exactStartedProcess)
            {
                // macOS exposes no pidfd-like handle for an XPC service. The
                // Started record and both parent samples bind PID reuse as
                // tightly as the public PID-addressed termination API permits.
                errno = 0;
                if (kill(process, SIGKILL) != 0)
                {
                    if (errno != ESRCH || requireExactStartedProcess)
                        terminatedCleanly = false;
                }
                else
                {
                    for (std::size_t attempt = 0; attempt < 200; ++attempt)
                    {
                        if (probeProcessIdentity(process) != processIdentity)
                            break;
                        usleep(1'000);
                    }
                    if (probeProcessIdentity(process) == processIdentity)
                        terminatedCleanly = false;
                    else
                        exactTerminationObserved = true;
                }
            }
            else if (terminate && requireExactStartedProcess)
            {
                terminatedCleanly = false;
            }
            xpc_connection_cancel(connection);
            xpc_release(connection);
            connection = nullptr;
        }
        else if (requireExactStartedProcess)
        {
            terminatedCleanly = false;
        }
        const auto* entry = entryPayload(inputs.snapshot);
        return entry != nullptr
            && cleanupComponentBundleProbeMaterializationV1(
                request,
                expected,
                std::as_bytes(std::span{entry->bytes}))
            && terminatedCleanly && exactTerminationObserved;
    }

    void publishState(
        ComponentBundleProbeMacOSCoordinatorStateV1 nextState,
        ComponentBundleProbeMacOSCoordinatorDiagnosticV1 nextDiagnostic)
        noexcept
    {
        diagnostic.store(
            static_cast<std::uint8_t>(nextDiagnostic),
            std::memory_order_relaxed);
        state.store(
            static_cast<std::uint8_t>(nextState),
            std::memory_order_release);
        operationPending.store(false, std::memory_order_release);
    }

    void finishWithoutEvidence(
        ComponentBundleProbeMacOSCoordinatorStateV1 nextState,
        ComponentBundleProbeMacOSCoordinatorDiagnosticV1 nextDiagnostic,
        bool terminate) noexcept
    {
        if (!running() || !tryClaimTerminal())
            return;
        invalidateDeadline();
        const auto cleanupComplete = closeConnection(terminate);
        publishState(
            cleanupComplete
                ? nextState
                : ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
            cleanupComplete
                ? nextDiagnostic
                : ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    InternalFailure);
    }

    void storeEvidence(
        ComponentBundleProbeEvidenceAuthoringV1 value) noexcept
    {
        const std::scoped_lock lock(evidenceMutex);
        authoredEvidence.emplace(std::move(value));
    }

    void clearPendingValidatedEvidence() noexcept
    {
        if (pendingValidatedEvidence)
            pendingValidatedEvidence->session.revoke();
        pendingValidatedEvidence.reset();
    }

    void finishParentFailure(
        ComponentProbeState failureState,
        ComponentBundleProbeMacOSCoordinatorDiagnosticV1 nextDiagnostic,
        bool terminate) noexcept
    {
        if (!running())
            return;
        clearPendingValidatedEvidence();
        auto authored =
            ComponentBundleProbeEvidenceFactory::makeParentClassifiedFailure(
                failureState,
                request);
        if (!authored)
        {
            finishWithoutEvidence(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    InternalFailure,
                terminate);
            return;
        }
        if (!tryClaimTerminal())
            return;
        invalidateDeadline();
        if (!closeConnection(terminate))
        {
            authored->session.revoke();
            publishState(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    InternalFailure);
            return;
        }
        storeEvidence(std::move(*authored));
        publishState(
            ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
            nextDiagnostic);
    }

    void publishPostReplyTerminationFailure() noexcept
    {
        clearPendingValidatedEvidence();
        auto failure =
            ComponentBundleProbeEvidenceFactory::makeParentClassifiedFailure(
                ComponentProbeState::AbnormalTermination,
                request);
        if (failure)
        {
            storeEvidence(std::move(*failure));
            publishState(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProcessCrashed);
            return;
        }
        publishState(
            ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
            ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                InternalFailure);
    }

    void handleTerminationReady(xpc_object_t reply) noexcept
    {
        if (!running() || cancelRequested.load(std::memory_order_acquire))
            return;
        if (!pendingValidatedEvidence || reply == nullptr
            || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            finishParentFailure(
                ComponentProbeState::AbnormalTermination,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProcessCrashed,
                true);
            return;
        }
        ComponentBundleProbeTerminationReadyV1 ready;
        if (decodeComponentBundleProbeXpcTerminationReadyV1(
                reply,
                request,
                ready)
            != ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        if (!tryClaimTerminal())
            return;
        invalidateDeadline();
        if (!closeConnection(true, true))
        {
            publishPostReplyTerminationFailure();
            return;
        }

        auto authored = std::move(*pendingValidatedEvidence);
        pendingValidatedEvidence.reset();
        const auto probeState = pendingValidatedState;
        if (probeState == ComponentProbeState::Clean)
        {
            auto approvalLease = inputs.ownerAuthorization
                                     .acquireExactApprovalLease(
                                         candidateFor(inputs));
            if (!approvalLease.approved())
            {
                authored.session.revoke();
                publishState(
                    ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                    ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                        PolicyChanged);
                return;
            }
            resultDiagnostic.store(
                pendingValidatedResultDiagnostic,
                std::memory_order_relaxed);
            storeEvidence(std::move(authored));
            publishState(
                ComponentBundleProbeMacOSCoordinatorStateV1::Finished,
                coordinatorDiagnostic(probeState));
            return;
        }
        resultDiagnostic.store(
            pendingValidatedResultDiagnostic,
            std::memory_order_relaxed);
        storeEvidence(std::move(authored));
        publishState(
            ComponentBundleProbeMacOSCoordinatorStateV1::Finished,
            coordinatorDiagnostic(probeState));
    }

    void finishValidatedResult(const ComponentBundleProbeResultV1& result)
        noexcept
    {
        if (!running())
            return;
        const auto probeState = static_cast<ComponentProbeState>(result.state);
        if (probeState == ComponentProbeState::Clean
            && !eligibleForProbe(evaluateProbeEligibility(inputs, request)))
        {
            finishWithoutEvidence(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    PolicyChanged,
                true);
            return;
        }
        auto authored = ComponentBundleProbeEvidenceFactory::makeFromValidatedResult(
            result,
            request);
        if (!authored)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        const auto process = connection != nullptr
            ? xpc_connection_get_pid(connection)
            : 0;
        if (!helperStarted || !processIdentity.valid
            || processIdentity.processIdentifier != process
            || probeProcessIdentity(process) != processIdentity)
        {
            authored->session.revoke();
            finishParentFailure(
                ComponentProbeState::AbnormalTermination,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProcessCrashed,
                true);
            return;
        }
        ComponentBundleProbeFinalizationV1 finalization;
        finalization.header = componentBundleProbeHeaderV1(
            ComponentBundleProbeRecordKindV1::Finalization,
            sizeof(finalization));
        finalization.contentDigest = request.contentDigest;
        finalization.artifactGeneration = request.artifactGeneration;
        finalization.validationNonce = request.validationNonce;
        auto message = xpc_dictionary_create_empty();
        if (message == nullptr
            || !writeComponentBundleProbeXpcFinalizationV1(
                message,
                finalization,
                request))
        {
            if (message != nullptr)
                xpc_release(message);
            authored->session.revoke();
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        pendingValidatedEvidence.emplace(std::move(*authored));
        pendingValidatedState = probeState;
        pendingValidatedResultDiagnostic = result.diagnostic;
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t finalizationReply) {
                if (const auto implementation = weak.lock())
                    implementation->handleTerminationReady(finalizationReply);
            });
        xpc_release(message);
    }

    void handleReply(xpc_object_t reply) noexcept
    {
        if (!running() || cancelRequested.load(std::memory_order_acquire))
            return;
        updateProcessIdentifier();
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            const auto launched = processIdentifier.load(
                    std::memory_order_acquire) > 0
                || reply == XPC_ERROR_CONNECTION_INTERRUPTED
                || reply == XPC_ERROR_TERMINATION_IMMINENT;
            finishParentFailure(
                ComponentProbeState::AbnormalTermination,
                launched
                    ? ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                        ProcessCrashed
                    : ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                        LaunchDenied,
                true);
            return;
        }
        if (!helperStarted)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        ComponentBundleProbeResultV1 result;
        if (decodeComponentBundleProbeXpcReplyV1(reply, request, result)
            != ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        finishValidatedResult(result);
    }

    void handleStarted(xpc_object_t event) noexcept
    {
        ComponentBundleProbeStartedV1 started;
        const auto actualProcess = connection != nullptr
            ? xpc_connection_get_pid(connection)
            : 0;
        const auto actualIdentity = probeProcessIdentity(actualProcess);
        if (helperStarted || actualProcess <= 0
            || decodeComponentBundleProbeXpcStartedV1(
                   event,
                   request,
                   started)
                != ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            || started.processIdentifier
                != static_cast<std::uint32_t>(actualProcess)
            || !actualIdentity.valid
            || started.effectiveUser != actualIdentity.effectiveUser
            || started.startSeconds != actualIdentity.startSeconds
            || started.startMicroseconds != actualIdentity.startMicroseconds)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        processIdentifier.store(
            static_cast<std::int32_t>(actualProcess),
            std::memory_order_release);
        processIdentity = actualIdentity;
        helperStarted = true;
        if (cancelRequested.load(std::memory_order_acquire)
            || terminalClaimed.load(std::memory_order_acquire))
        {
            return;
        }
        ComponentBundleProbeAcknowledgementV1 acknowledgement;
        acknowledgement.header = componentBundleProbeHeaderV1(
            ComponentBundleProbeRecordKindV1::Acknowledgement,
            sizeof(acknowledgement));
        acknowledgement.contentDigest = request.contentDigest;
        acknowledgement.artifactGeneration = request.artifactGeneration;
        acknowledgement.validationNonce = request.validationNonce;
        auto message = xpc_dictionary_create_empty();
        if (message == nullptr
            || !writeComponentBundleProbeXpcAcknowledgementV1(
                message,
                acknowledgement,
                request))
        {
            if (message != nullptr)
                xpc_release(message);
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        xpc_connection_send_message(connection, message);
        xpc_release(message);
    }

    void handleConnectionEvent(xpc_object_t event) noexcept
    {
        if (!running() || cancelRequested.load(std::memory_order_acquire))
            return;
        if (event != nullptr && xpc_get_type(event) != XPC_TYPE_ERROR)
        {
            handleStarted(event);
            return;
        }
        updateProcessIdentifier();
        const auto launched = processIdentifier.load(
                std::memory_order_acquire) > 0
            || event == XPC_ERROR_CONNECTION_INTERRUPTED
            || event == XPC_ERROR_TERMINATION_IMMINENT;
        finishParentFailure(
            ComponentProbeState::AbnormalTermination,
            launched
                ? ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProcessCrashed
                : ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    LaunchDenied,
            true);
    }

    void deadlineExpired(std::uint64_t token) noexcept
    {
        if (!running() || token != deadlineToken
            || cancelRequested.load(std::memory_order_acquire))
        {
            return;
        }
        finishParentFailure(
            ComponentProbeState::TimedOut,
            ComponentBundleProbeMacOSCoordinatorDiagnosticV1::DeadlineExpired,
            true);
    }

    void begin() noexcept
    {
        if (!running())
            return;
        if (cancelRequested.load(std::memory_order_acquire))
        {
            finishWithoutEvidence(
                ComponentBundleProbeMacOSCoordinatorStateV1::Cancelled,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::Cancelled,
                true);
            return;
        }
        if (!eligibleForProbe(evaluateProbeEligibility(inputs, request)))
        {
            finishWithoutEvidence(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    PolicyChanged,
                false);
            return;
        }
        const auto* entry = entryPayload(inputs.snapshot);
        if (entry == nullptr)
        {
            finishWithoutEvidence(
                ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    InternalFailure,
                false);
            return;
        }
        connection = xpc_connection_create(
            productProbeServiceIdentityV1,
            controlQueue);
        if (connection == nullptr)
        {
            finishParentFailure(
                ComponentProbeState::AbnormalTermination,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::LaunchDenied,
                false);
            return;
        }
        const auto weak = weak_from_this();
        xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
            if (const auto implementation = weak.lock())
                implementation->handleConnectionEvent(event);
        });
        xpc_connection_resume(connection);
        const auto bytes = std::span(
            reinterpret_cast<const std::byte*>(entry->bytes.data()),
            entry->bytes.size());
        auto message = createComponentBundleProbeXpcRequestV1(
            request,
            expected,
            bytes);
        if (message == nullptr)
        {
            finishParentFailure(
                ComponentProbeState::ProtocolError,
                ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    ProtocolRejected,
                true);
            return;
        }
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->handleReply(reply);
            });
        xpc_release(message);
        invalidateDeadline();
        const auto token = deadlineToken;
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, deadlineNanoseconds()),
            controlQueue,
            ^{
                if (const auto implementation = weak.lock())
                    implementation->deadlineExpired(token);
            });
    }

    void cancelOnQueue() noexcept
    {
        if (!running())
            return;
        invalidateDeadline();
        clearPendingValidatedEvidence();
        const auto cleanupComplete = closeConnection(true);
        publishState(
            cleanupComplete
                ? ComponentBundleProbeMacOSCoordinatorStateV1::Cancelled
                : ComponentBundleProbeMacOSCoordinatorStateV1::Failed,
            cleanupComplete
                ? ComponentBundleProbeMacOSCoordinatorDiagnosticV1::Cancelled
                : ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                    InternalFailure);
    }

    void shutdown() noexcept
    {
        cancelRequested.store(true, std::memory_order_release);
        if (controlQueue != nullptr)
        {
            dispatch_sync(controlQueue, ^{
                invalidateDeadline();
                clearPendingValidatedEvidence();
                static_cast<void>(closeConnection(true));
                const auto current = state.load(std::memory_order_relaxed);
                if (current
                        == static_cast<std::uint8_t>(
                            ComponentBundleProbeMacOSCoordinatorStateV1::Running)
                    || current
                        == static_cast<std::uint8_t>(
                            ComponentBundleProbeMacOSCoordinatorStateV1::Prepared))
                {
                    publishState(
                        ComponentBundleProbeMacOSCoordinatorStateV1::Cancelled,
                        ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                            Cancelled);
                }
                const std::scoped_lock lock(evidenceMutex);
                if (authoredEvidence)
                    authoredEvidence->session.revoke();
                authoredEvidence.reset();
            });
            dispatch_sync(controlQueue, ^{});
        }
    }

    ComponentBundleProbeMacOSCoordinatorInputsV1 inputs;
    ComponentBundleProbeRequestV1 request;
    ComponentBundleProbeExpectedAbiV1 expected;
    ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1 deadlineProfile;
    dispatch_queue_t controlQueue = nullptr;
    xpc_connection_t connection = nullptr;
    std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(
        ComponentBundleProbeMacOSCoordinatorStateV1::Prepared)};
    std::atomic<std::uint8_t> diagnostic{static_cast<std::uint8_t>(
        ComponentBundleProbeMacOSCoordinatorDiagnosticV1::Prepared)};
    std::atomic<std::uint32_t> resultDiagnostic{static_cast<std::uint32_t>(
        ComponentBundleProbeResultDiagnosticV1::ProtocolRejected)};
    std::atomic<bool> operationPending{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> terminalClaimed{false};
    std::atomic<std::int32_t> processIdentifier{0};
    ProbeProcessIdentityV1 processIdentity;
    bool helperStarted = false;
    std::uint64_t deadlineToken = 0;
    mutable std::mutex evidenceMutex;
    std::optional<ComponentBundleProbeEvidenceAuthoringV1> authoredEvidence;
    std::optional<ComponentBundleProbeEvidenceAuthoringV1>
        pendingValidatedEvidence;
    ComponentProbeState pendingValidatedState = ComponentProbeState::NotRun;
    std::uint32_t pendingValidatedResultDiagnostic = static_cast<std::uint32_t>(
        ComponentBundleProbeResultDiagnosticV1::ProtocolRejected);
};

ComponentBundleProbeMacOSCoordinator::ComponentBundleProbeMacOSCoordinator(
    std::shared_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentBundleProbeMacOSCoordinator::~ComponentBundleProbeMacOSCoordinator()
{
    if (impl_)
        impl_->shutdown();
}

ComponentBundleProbeMacOSCoordinatorPreparationV1
ComponentBundleProbeMacOSCoordinator::prepare(
    ComponentBundleProbeMacOSCoordinatorInputsV1 inputs,
    ComponentBundleProbeMacOSCoordinatorDeadlineProfileV1 deadlineProfile)
    noexcept
{
    ComponentBundleProbeMacOSCoordinatorPreparationV1 preparation;
    if (!componentIsolationBackendAvailableV1)
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                BackendUnavailable;
        return preparation;
    }
    if (!component_bundle::validateComponentBundleSnapshotIdentity(
            inputs.snapshot))
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                SnapshotInvalid;
        return preparation;
    }
    const auto* entry = entryPayload(inputs.snapshot);
    if (entry == nullptr)
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                EntryMissing;
        return preparation;
    }
    if (entry->bytes.empty()
        || entry->bytes.size() > maximumComponentBundleProbeEntryBytesV1)
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                EntryInvalid;
        return preparation;
    }
    const auto generation = claimArtifactGeneration();
    if (generation == 0)
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                GenerationUnavailable;
        return preparation;
    }
    std::array<std::uint8_t, componentProbeNonceBytes> nonce{};
    if (SecRandomCopyBytes(
            kSecRandomDefault,
            nonce.size(),
            nonce.data()) != errSecSuccess
        || std::ranges::all_of(nonce, [](auto byte) { return byte == 0; }))
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                RandomUnavailable;
        return preparation;
    }
    ComponentBundleProbeRequestV1 request;
    ComponentBundleProbeExpectedAbiV1 expected;
    if (!makeProtocolRecords(
            inputs,
            *entry,
            generation,
            nonce,
            request,
            expected))
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                EntryInvalid;
        return preparation;
    }
    preparation.trustDecision = evaluateProbeEligibility(inputs, request);
    if (!eligibleForProbe(preparation.trustDecision))
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                PolicyIneligible;
        return preparation;
    }
    auto queue = dispatch_queue_create(
        "com.talas.component-probe.coordinator.v1",
        DISPATCH_QUEUE_SERIAL);
    if (queue == nullptr)
    {
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                QueueUnavailable;
        return preparation;
    }
    try
    {
        auto implementation = std::make_shared<Impl>(
            std::move(inputs),
            request,
            expected,
            deadlineProfile,
            queue);
        queue = nullptr;
        auto coordinator = std::unique_ptr<ComponentBundleProbeMacOSCoordinator>(
            new ComponentBundleProbeMacOSCoordinator(
                std::move(implementation)));
        preparation.coordinator = std::move(coordinator);
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::Ready;
        return preparation;
    }
    catch (const std::bad_alloc&)
    {
#if !OS_OBJECT_USE_OBJC
        if (queue != nullptr)
            dispatch_release(queue);
#endif
        preparation.diagnostic =
            ComponentBundleProbeMacOSCoordinatorPreparationDiagnosticV1::
                AllocationFailed;
        return preparation;
    }
}

ComponentBundleProbeMacOSCoordinatorOperationV1
ComponentBundleProbeMacOSCoordinator::start() noexcept
{
    if (!impl_ || !componentIsolationBackendAvailableV1)
        return ComponentBundleProbeMacOSCoordinatorOperationV1::Unavailable;
    auto expected = static_cast<std::uint8_t>(
        ComponentBundleProbeMacOSCoordinatorStateV1::Prepared);
    if (!impl_->state.compare_exchange_strong(
            expected,
            static_cast<std::uint8_t>(
                ComponentBundleProbeMacOSCoordinatorStateV1::Running),
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentBundleProbeMacOSCoordinatorOperationV1::InvalidState;
    }
    impl_->diagnostic.store(
        static_cast<std::uint8_t>(
            ComponentBundleProbeMacOSCoordinatorDiagnosticV1::Running),
        std::memory_order_relaxed);
    impl_->operationPending.store(true, std::memory_order_release);
    const auto implementation = impl_;
    dispatch_async(impl_->controlQueue, ^{
        implementation->begin();
    });
    return ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled;
}

ComponentBundleProbeMacOSCoordinatorOperationV1
ComponentBundleProbeMacOSCoordinator::cancel() noexcept
{
    if (!impl_ || !componentIsolationBackendAvailableV1)
        return ComponentBundleProbeMacOSCoordinatorOperationV1::Unavailable;
    for (;;)
    {
        auto current = impl_->state.load(std::memory_order_acquire);
        if (current == static_cast<std::uint8_t>(
                ComponentBundleProbeMacOSCoordinatorStateV1::Prepared))
        {
            if (!impl_->state.compare_exchange_strong(
                    current,
                    static_cast<std::uint8_t>(
                        ComponentBundleProbeMacOSCoordinatorStateV1::Cancelled),
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                continue;
            }
            impl_->terminalClaimed.store(true, std::memory_order_release);
            impl_->cancelRequested.store(true, std::memory_order_release);
            impl_->diagnostic.store(
                static_cast<std::uint8_t>(
                    ComponentBundleProbeMacOSCoordinatorDiagnosticV1::
                        Cancelled),
                std::memory_order_release);
            return ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled;
        }
        if (current != static_cast<std::uint8_t>(
                ComponentBundleProbeMacOSCoordinatorStateV1::Running))
        {
            return ComponentBundleProbeMacOSCoordinatorOperationV1::
                InvalidState;
        }
        if (!impl_->tryClaimTerminal())
        {
            return ComponentBundleProbeMacOSCoordinatorOperationV1::
                InvalidState;
        }
        impl_->cancelRequested.store(true, std::memory_order_release);
        const auto implementation = impl_;
        dispatch_async(impl_->controlQueue, ^{
            implementation->cancelOnQueue();
        });
        return ComponentBundleProbeMacOSCoordinatorOperationV1::Scheduled;
    }
}

ComponentBundleProbeMacOSCoordinatorStateV1
ComponentBundleProbeMacOSCoordinator::state() const noexcept
{
    return impl_
        ? static_cast<ComponentBundleProbeMacOSCoordinatorStateV1>(
              impl_->state.load(std::memory_order_acquire))
        : ComponentBundleProbeMacOSCoordinatorStateV1::Failed;
}

ComponentBundleProbeMacOSCoordinatorDiagnosticV1
ComponentBundleProbeMacOSCoordinator::diagnostic() const noexcept
{
    return impl_
        ? static_cast<ComponentBundleProbeMacOSCoordinatorDiagnosticV1>(
              impl_->diagnostic.load(std::memory_order_acquire))
        : ComponentBundleProbeMacOSCoordinatorDiagnosticV1::InternalFailure;
}

ComponentBundleProbeResultDiagnosticV1
ComponentBundleProbeMacOSCoordinator::resultDiagnostic() const noexcept
{
    return impl_
        ? static_cast<ComponentBundleProbeResultDiagnosticV1>(
              impl_->resultDiagnostic.load(std::memory_order_acquire))
        : ComponentBundleProbeResultDiagnosticV1::ProtocolRejected;
}

bool ComponentBundleProbeMacOSCoordinator::operationPending() const noexcept
{
    return impl_
        && impl_->operationPending.load(std::memory_order_acquire);
}

ComponentProbeEvidence
ComponentBundleProbeMacOSCoordinator::evidence() const noexcept
{
    if (!impl_)
        return {};
    const std::scoped_lock lock(impl_->evidenceMutex);
    return impl_->authoredEvidence
        ? impl_->authoredEvidence->evidence
        : ComponentProbeEvidence{};
}

std::uint64_t
ComponentBundleProbeMacOSCoordinator::artifactGeneration() const noexcept
{
    return impl_ ? impl_->request.artifactGeneration : 0;
}

std::array<std::uint8_t, componentProbeNonceBytes>
ComponentBundleProbeMacOSCoordinator::validationNonce() const noexcept
{
    return impl_
        ? impl_->request.validationNonce
        : std::array<std::uint8_t, componentProbeNonceBytes>{};
}

std::int32_t
ComponentBundleProbeMacOSCoordinator::processIdentifier() const noexcept
{
    return impl_
        ? impl_->processIdentifier.load(std::memory_order_acquire)
        : 0;
}
}
