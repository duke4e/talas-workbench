#include "talas/runtime/ComponentIsolationMacOSHost.h"

#include "runtime/component_isolation/ComponentIsolationLiveEvidence.h"
#include "runtime/macos/ComponentIsolationXpcEnvelope.h"

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

#include <atomic>
#include <memory>
#include <new>
#include <utility>

namespace talas::runtime
{
namespace
{
constexpr char productServiceIdentityV1[] =
    "com.talas.workbench.component-host";
constexpr char malformedServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.malformed";
constexpr char wrongVersionServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.wrong-version";
constexpr char wrongNonceServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.wrong-nonce";
constexpr char staleGenerationServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.stale-generation";
constexpr char controlledFailureServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.failure";
constexpr char crashServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.crash";
constexpr char hangServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.hang";
constexpr char oversizedDiagnosticServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.oversized";
constexpr char earlyExitServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.early-exit";
constexpr char teardownRaceServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.teardown-race";
constexpr char sandboxProbeServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.sandbox";
constexpr char heartbeatHangServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.heart-hang";
constexpr char teardownHangServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.td-hang";
constexpr char heartbeatLateServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.heart-late";
constexpr char prewarmHangServiceIdentityV1[] =
    "com.talas.workbench.component-host.fixture.prewarm-hang";
constexpr char controlQueueIdentityV1[] =
    "com.talas.workbench.component-host.control.v1";

static_assert(sizeof(productServiceIdentityV1) <= 64);
static_assert(sizeof(malformedServiceIdentityV1) <= 64);
static_assert(sizeof(wrongVersionServiceIdentityV1) <= 64);
static_assert(sizeof(wrongNonceServiceIdentityV1) <= 64);
static_assert(sizeof(staleGenerationServiceIdentityV1) <= 64);
static_assert(sizeof(controlledFailureServiceIdentityV1) <= 64);
static_assert(sizeof(crashServiceIdentityV1) <= 64);
static_assert(sizeof(hangServiceIdentityV1) <= 64);
static_assert(sizeof(oversizedDiagnosticServiceIdentityV1) <= 64);
static_assert(sizeof(earlyExitServiceIdentityV1) <= 64);
static_assert(sizeof(teardownRaceServiceIdentityV1) <= 64);
static_assert(sizeof(sandboxProbeServiceIdentityV1) <= 64);
static_assert(sizeof(heartbeatHangServiceIdentityV1) <= 64);
static_assert(sizeof(teardownHangServiceIdentityV1) <= 64);
static_assert(sizeof(heartbeatLateServiceIdentityV1) <= 64);
static_assert(sizeof(prewarmHangServiceIdentityV1) <= 64);

[[nodiscard]] constexpr const char* serviceIdentity(
    ComponentIsolationMacOSProductEndpointV1 endpoint) noexcept
{
    switch (endpoint)
    {
    case ComponentIsolationMacOSProductEndpointV1::ProductionGain:
        return productServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureMalformed:
        return malformedServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureWrongVersion:
        return wrongVersionServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureWrongNonce:
        return wrongNonceServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureStaleGeneration:
        return staleGenerationServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureControlledFailure:
        return controlledFailureServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureCrash:
        return crashServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureHang:
        return hangServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureOversizedDiagnostic:
        return oversizedDiagnosticServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureEarlyExit:
        return earlyExitServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureTeardownRace:
        return teardownRaceServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureSandboxProbe:
        return sandboxProbeServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatHang:
        return heartbeatHangServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureTeardownHang:
        return teardownHangServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixtureHeartbeatLate:
        return heartbeatLateServiceIdentityV1;
    case ComponentIsolationMacOSProductEndpointV1::FixturePrewarmHang:
        return prewarmHangServiceIdentityV1;
    }
    return nullptr;
}

[[nodiscard]] bool sharedAudioMatches(
    const ComponentIsolationControlSessionConfigurationV1& configuration,
    const ComponentIsolationAudioDataPlane& sharedAudio) noexcept
{
    const auto region = sharedAudio.sharedRegion();
    const auto layout = sharedAudio.sharedRegionLayout();
    if (region.data() == nullptr
        || region.size() != layout.regionBytes
        || region.size() % componentIsolationSharedProtectionPageBytesV1 != 0
        || layout.requestRegionBytes != layout.responseRegionOffset
        || layout.responseRegionOffset == 0
        || layout.responseRegionOffset >= layout.regionBytes
        || layout.responseRegionOffset
                % componentIsolationSharedProtectionPageBytesV1
            != 0
        || region.size()
            < sizeof(ComponentIsolationAudioSharedRegionHeaderV1))
    {
        return false;
    }
    const auto* header = reinterpret_cast<
        const ComponentIsolationAudioSharedRegionHeaderV1*>(region.data());
    const auto& contract = configuration.contract;
    return !sharedAudio.isQuarantined()
        && header->publication == 0
        && header->magic == componentIsolationAudioSharedRegionMagicV1
        && header->protocolVersion
            == currentComponentIsolationProtocolVersion
        && header->headerBytes
            == sizeof(ComponentIsolationAudioSharedRegionHeaderV1)
        && header->regionBytes == region.size()
        && header->responseRegionOffset == layout.responseRegionOffset
        && header->sessionGeneration == contract.sessionGeneration
        && header->sharedStorageIdentity == contract.sharedStorageIdentity
        && header->validationNonce == contract.validationNonce
        && header->slotCount == componentIsolationAudioSlotCountV1
        && header->inputChannelCount == contract.inputChannelCount
        && header->outputChannelCount == contract.outputChannelCount
        && header->maximumFrames == contract.maximumFrames
        && header->reserved0 == 0
        && header->reserved1 == 0;
}
}

struct ComponentIsolationMacOSHost::Impl
    : std::enable_shared_from_this<ComponentIsolationMacOSHost::Impl>
{
    Impl(
        ComponentIsolationControlSessionV1 preparedSession,
        const char* productServiceIdentity,
        ComponentIsolationMacOSProductEndpointV1 preparedEndpoint,
        std::shared_ptr<ComponentIsolationAudioDataPlane> preparedSharedAudio,
        xpc_object_t preparedSharedMemory)
        : session(std::move(preparedSession)),
          serviceIdentity(productServiceIdentity),
          endpoint(preparedEndpoint),
          sharedAudio(std::move(preparedSharedAudio)),
          sharedMemory(preparedSharedMemory),
          liveEvidenceState(std::make_shared<
              detail::ComponentIsolationLiveEvidenceStateV1>(
              session.contract(),
              sharedAudio))
    {
        stateSnapshot.store(
            static_cast<std::uint8_t>(session.state()),
            std::memory_order_relaxed);
        diagnosticSnapshot.store(
            static_cast<std::uint8_t>(session.diagnostic()),
            std::memory_order_relaxed);
        lifecycleTraceSnapshot.store(
            stateBit(session.state()),
            std::memory_order_relaxed);
    }

    ~Impl()
    {
        closing.store(true, std::memory_order_release);
        if (connection != nullptr)
        {
            xpc_connection_cancel(connection);
            xpc_release(connection);
            connection = nullptr;
        }
        if (sharedMemory != nullptr)
        {
            xpc_release(sharedMemory);
            sharedMemory = nullptr;
        }
#if !OS_OBJECT_USE_OBJC
        if (controlQueue != nullptr)
            dispatch_release(controlQueue);
#endif
        controlQueue = nullptr;
    }

    [[nodiscard]] static constexpr std::uint32_t stateBit(
        ComponentIsolationControlSessionState state) noexcept
    {
        return 1U << static_cast<std::uint8_t>(state);
    }

    void publishLifecycle(
        ComponentIsolationControlSessionState state) noexcept
    {
        const auto audioReady = state
                == ComponentIsolationControlSessionState::Ready
            && session.state() == ComponentIsolationControlSessionState::Ready
            && session.diagnostic()
                == ComponentIsolationControlSessionDiagnostic::Ready
            && connection != nullptr && sharedAudio != nullptr
            && !sharedAudio->isQuarantined();
        if (sharedAudio != nullptr)
            sharedAudio->setHostLifecycleReady(audioReady);
        const auto isolationReady = componentIsolationBackendAvailableV1
            && endpoint
                == ComponentIsolationMacOSProductEndpointV1::ProductionGain
            && audioReady;
        if (isolationReady)
        {
            liveEvidenceState->audioPlaneRevision.store(
                sharedAudio->lifecycleRevision(),
                std::memory_order_relaxed);
        }
        liveEvidenceState->ready.store(
            isolationReady,
            std::memory_order_release);
        diagnosticSnapshot.store(
            static_cast<std::uint8_t>(session.diagnostic()),
            std::memory_order_relaxed);
        lifecycleTraceSnapshot.fetch_or(
            stateBit(state),
            std::memory_order_relaxed);
        stateSnapshot.store(
            static_cast<std::uint8_t>(state),
            std::memory_order_release);
    }

    void publishSessionSnapshot() noexcept
    {
        publishLifecycle(session.state());
    }

    void revokeReadyState() noexcept
    {
        if (sharedAudio != nullptr)
            sharedAudio->setHostLifecycleReady(false);
        liveEvidenceState->ready.store(false, std::memory_order_release);
    }

    void cancelConnection() noexcept
    {
        if (connection == nullptr)
            return;
        xpc_connection_cancel(connection);
        xpc_release(connection);
        connection = nullptr;
    }

    void invalidateWatchdog() noexcept
    {
        ++watchdogToken;
        if (watchdogToken == 0)
            ++watchdogToken;
    }

    [[nodiscard]] std::int64_t watchdogDelayNanoseconds(
        ComponentIsolationControlSessionState phase) const noexcept
    {
        if (endpoint != ComponentIsolationMacOSProductEndpointV1::ProductionGain)
            return NSEC_PER_MSEC * 500;
        return phase == ComponentIsolationControlSessionState::Handshaking
            ? NSEC_PER_SEC * 5
            : NSEC_PER_SEC * 2;
    }

    [[nodiscard]] std::uint64_t armWatchdog(
        ComponentIsolationControlSessionState phase,
        ComponentIsolationControlSessionDiagnostic reason) noexcept
    {
        invalidateWatchdog();
        const auto token = watchdogToken;
        const auto weak = weak_from_this();
        dispatch_after(
            dispatch_time(
                DISPATCH_TIME_NOW,
                watchdogDelayNanoseconds(phase)),
            controlQueue,
            ^{
                if (const auto implementation = weak.lock())
                {
                    implementation->watchdogFired(token, phase, reason);
                }
            });
        return token;
    }

    [[nodiscard]] bool activeWatchdog(
        std::uint64_t token,
        ComponentIsolationControlSessionState phase) const noexcept
    {
        return token == watchdogToken
            && stateSnapshot.load(std::memory_order_relaxed)
                == static_cast<std::uint8_t>(phase);
    }

    void failClosed(
        ComponentIsolationControlSessionDiagnostic reason) noexcept
    {
        invalidateWatchdog();
        if (reason == ComponentIsolationControlSessionDiagnostic::ProtocolViolation)
            session.quarantineProtocol();
        else if (reason == ComponentIsolationControlSessionDiagnostic::TransportFailure)
            session.quarantineTransport();
        else
            session.quarantineDeadline(reason);
        commandScheduled.store(false, std::memory_order_release);
        if (sharedAudio != nullptr)
            sharedAudio->quarantine();
        publishSessionSnapshot();
        cancelConnection();
    }

    void watchdogFired(
        std::uint64_t token,
        ComponentIsolationControlSessionState phase,
        ComponentIsolationControlSessionDiagnostic reason) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || !activeWatchdog(token, phase))
        {
            return;
        }
        failClosed(reason);
    }

    void shutdown() noexcept
    {
        bool expected = false;
        if (!shutdownStarted.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            return;
        }
        revokeReadyState();
        closing.store(true, std::memory_order_release);
        if (controlQueue != nullptr)
        {
            dispatch_sync(controlQueue, ^{
                invalidateWatchdog();
                commandScheduled.store(false, std::memory_order_release);
                if (sharedAudio != nullptr)
                    sharedAudio->quarantine();
                cancelConnection();
            });
            dispatch_sync(controlQueue, ^{});
        }
    }

    void enqueueConnectionEvent(xpc_object_t event) noexcept
    {
        if (event != nullptr)
            xpc_retain(event);
        const auto weak = weak_from_this();
        dispatch_async(controlQueue, ^{
            if (const auto implementation = weak.lock())
                implementation->handleConnectionEvent(event);
            if (event != nullptr)
                xpc_release(event);
        });
    }

    void enqueueHandshakeReply(
        xpc_object_t reply,
        std::uint64_t token) noexcept
    {
        if (reply != nullptr)
            xpc_retain(reply);
        const auto weak = weak_from_this();
        dispatch_async(controlQueue, ^{
            if (const auto implementation = weak.lock())
                implementation->handleHandshakeReply(reply, token);
            if (reply != nullptr)
                xpc_release(reply);
        });
    }

    void enqueueControlReply(
        xpc_object_t reply,
        ComponentIsolationRecordKind kind,
        bool prewarm,
        std::uint64_t token) noexcept
    {
        if (reply != nullptr)
            xpc_retain(reply);
        const auto weak = weak_from_this();
        dispatch_async(controlQueue, ^{
            if (const auto implementation = weak.lock())
            {
                implementation->handleControlReply(
                    reply,
                    kind,
                    prewarm,
                    token);
            }
            if (reply != nullptr)
                xpc_release(reply);
        });
    }

    void handleConnectionEvent(xpc_object_t event) noexcept
    {
        if (closing.load(std::memory_order_acquire))
            return;
        const auto state = static_cast<ComponentIsolationControlSessionState>(
            stateSnapshot.load(std::memory_order_relaxed));
        if (state == ComponentIsolationControlSessionState::Stopped
            || state == ComponentIsolationControlSessionState::Quarantined)
        {
            return;
        }
        if (event == nullptr || xpc_get_type(event) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        failClosed(ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
    }

    void handleHandshakeReply(
        xpc_object_t reply,
        std::uint64_t token) noexcept
    {
        if (closing.load(std::memory_order_acquire)
            || !activeWatchdog(
                token,
                ComponentIsolationControlSessionState::Handshaking))
        {
            return;
        }
        invalidateWatchdog();
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        ComponentIsolationXpcEnvelopeV1 envelope;
        if (decodeComponentIsolationXpcEnvelopeV1(reply, envelope)
                != ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            || envelope.kind
                != ComponentIsolationXpcEnvelopeKindV1::Handshake)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
            return;
        }
        if (session.acceptHandshake(envelope.handshake)
            != ComponentIsolationControlSessionOperation::Accepted)
        {
            if (sharedAudio != nullptr)
                sharedAudio->quarantine();
            publishSessionSnapshot();
            cancelConnection();
            return;
        }
        publishLifecycle(ComponentIsolationControlSessionState::Prewarming);
        sendControlOnQueue(ComponentIsolationRecordKind::Heartbeat, true);
    }

    void handleControlReply(
        xpc_object_t reply,
        ComponentIsolationRecordKind expectedKind,
        bool prewarm,
        std::uint64_t token) noexcept
    {
        const auto expectedState = prewarm
            ? ComponentIsolationControlSessionState::Prewarming
            : expectedKind == ComponentIsolationRecordKind::Heartbeat
                ? ComponentIsolationControlSessionState::HeartbeatPending
                : ComponentIsolationControlSessionState::Draining;
        if (closing.load(std::memory_order_acquire)
            || !activeWatchdog(token, expectedState))
        {
            return;
        }
        invalidateWatchdog();
        if (reply == nullptr || xpc_get_type(reply) == XPC_TYPE_ERROR)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        ComponentIsolationXpcEnvelopeV1 envelope;
        if (decodeComponentIsolationXpcEnvelopeV1(reply, envelope)
                != ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            || envelope.kind != ComponentIsolationXpcEnvelopeKindV1::Control)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
            return;
        }
        if (expectedKind == ComponentIsolationRecordKind::Heartbeat)
        {
            if (envelope.diagnosticBytes != 0)
            {
                failClosed(
                    ComponentIsolationControlSessionDiagnostic::ProtocolViolation);
                return;
            }
            else
                static_cast<void>(session.acceptHeartbeat(envelope.control));
        }
        else
        {
            static_cast<void>(session.acceptTeardown(
                envelope.control,
                envelope.diagnostic()));
        }
        if (!prewarm)
            commandScheduled.store(false, std::memory_order_release);
        if (session.state() == ComponentIsolationControlSessionState::Quarantined)
        {
            if (sharedAudio != nullptr)
                sharedAudio->quarantine();
            publishSessionSnapshot();
            cancelConnection();
            return;
        }
        if (session.state() == ComponentIsolationControlSessionState::Stopped
            && sharedAudio != nullptr)
        {
            sharedAudio->quarantine();
        }
        if (prewarm)
            publishLifecycle(ComponentIsolationControlSessionState::Ready);
        else
            publishSessionSnapshot();
        if (session.state() == ComponentIsolationControlSessionState::Stopped)
            cancelConnection();
    }

    void startOnControlQueue() noexcept
    {
        if (closing.load(std::memory_order_acquire))
            return;
        publishLifecycle(ComponentIsolationControlSessionState::Launching);
        connection = xpc_connection_create(
            serviceIdentity,
            controlQueue);
        if (connection == nullptr)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        const auto weak = weak_from_this();
        xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
            if (const auto implementation = weak.lock())
                implementation->enqueueConnectionEvent(event);
        });
        xpc_connection_activate(connection);

        ComponentIsolationHandshakeV1 request;
        if (session.beginHandshake(request)
            != ComponentIsolationControlSessionOperation::Published)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        publishSessionSnapshot();
        auto message = createComponentIsolationXpcMessageV1(
            request,
            sharedMemory);
        if (message == nullptr)
        {
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        const auto token = armWatchdog(
            ComponentIsolationControlSessionState::Handshaking,
            ComponentIsolationControlSessionDiagnostic::HandshakeTimeout);
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                    implementation->enqueueHandshakeReply(reply, token);
            });
        xpc_release(message);
    }

    void sendControlOnQueue(
        ComponentIsolationRecordKind kind,
        bool prewarm = false) noexcept
    {
        if (closing.load(std::memory_order_acquire))
            return;
        ComponentIsolationControlRecordV1 request;
        const auto operation = kind == ComponentIsolationRecordKind::Heartbeat
            ? session.beginHeartbeat(request)
            : session.beginTeardown(request);
        if (operation != ComponentIsolationControlSessionOperation::Published)
        {
            commandScheduled.store(false, std::memory_order_release);
            if (session.state()
                == ComponentIsolationControlSessionState::Quarantined)
            {
                if (sharedAudio != nullptr)
                    sharedAudio->quarantine();
                publishSessionSnapshot();
                cancelConnection();
            }
            else
            {
                failClosed(
                    ComponentIsolationControlSessionDiagnostic::TransportFailure);
            }
            return;
        }
        const auto lifecycleState = prewarm
            ? ComponentIsolationControlSessionState::Prewarming
            : session.state();
        publishLifecycle(lifecycleState);
        auto message = createComponentIsolationXpcMessageV1(request);
        if (message == nullptr || connection == nullptr)
        {
            if (message != nullptr)
                xpc_release(message);
            failClosed(
                ComponentIsolationControlSessionDiagnostic::TransportFailure);
            return;
        }
        const auto reason = prewarm
            ? ComponentIsolationControlSessionDiagnostic::PrewarmTimeout
            : kind == ComponentIsolationRecordKind::Heartbeat
                ? ComponentIsolationControlSessionDiagnostic::HeartbeatTimeout
                : ComponentIsolationControlSessionDiagnostic::TeardownTimeout;
        const auto token = armWatchdog(lifecycleState, reason);
        const auto weak = weak_from_this();
        xpc_connection_send_message_with_reply(
            connection,
            message,
            controlQueue,
            ^(xpc_object_t reply) {
                if (const auto implementation = weak.lock())
                {
                    implementation->enqueueControlReply(
                        reply,
                        kind,
                        prewarm,
                        token);
                }
            });
        xpc_release(message);
    }

    ComponentIsolationControlSessionV1 session;
    const char* serviceIdentity = nullptr;
    ComponentIsolationMacOSProductEndpointV1 endpoint =
        ComponentIsolationMacOSProductEndpointV1::ProductionGain;
    std::shared_ptr<ComponentIsolationAudioDataPlane> sharedAudio;
    xpc_object_t sharedMemory = nullptr;
    std::shared_ptr<detail::ComponentIsolationLiveEvidenceStateV1>
        liveEvidenceState;
    dispatch_queue_t controlQueue = nullptr;
    xpc_connection_t connection = nullptr;
    std::atomic<std::uint8_t> stateSnapshot{
        static_cast<std::uint8_t>(
            ComponentIsolationControlSessionState::Stopped)};
    std::atomic<std::uint8_t> diagnosticSnapshot{
        static_cast<std::uint8_t>(
            ComponentIsolationControlSessionDiagnostic::InvalidContract)};
    std::atomic<std::uint32_t> lifecycleTraceSnapshot{0};
    std::atomic<bool> closing{false};
    std::atomic<bool> shutdownStarted{false};
    std::atomic<bool> startScheduled{false};
    std::atomic<bool> commandScheduled{false};
    std::uint64_t watchdogToken = 1;
};

static_assert(
    static_cast<std::uint8_t>(
        ComponentIsolationControlSessionState::Quarantined)
    < 32);

ComponentIsolationMacOSHost::ComponentIsolationMacOSHost(
    std::shared_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentIsolationMacOSHost::~ComponentIsolationMacOSHost()
{
    if (impl_ != nullptr)
        impl_->shutdown();
}

ComponentIsolationMacOSHostPreparation ComponentIsolationMacOSHost::prepare(
    const ComponentIsolationControlSessionConfigurationV1& configuration,
    std::shared_ptr<ComponentIsolationAudioDataPlane> sharedAudio,
    ComponentIsolationMacOSProductEndpointV1 endpoint)
    noexcept
{
    ComponentIsolationMacOSHostPreparation result;
    const auto* identity = serviceIdentity(endpoint);
    if (identity == nullptr || sharedAudio == nullptr)
    {
        result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
            InvalidConfiguration;
        return result;
    }
    const auto lifecycleAudio = sharedAudio;
    if (!lifecycleAudio->bindHostLifecycle())
    {
        result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
            InvalidConfiguration;
        return result;
    }
    auto lifecycleBound = true;
    if (!sharedAudioMatches(configuration, *sharedAudio))
    {
        lifecycleAudio->unbindHostLifecycle();
        result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
            InvalidConfiguration;
        return result;
    }
    const auto sharedRegion = sharedAudio->sharedRegion();
    auto sharedMemory = xpc_shmem_create(
        const_cast<std::byte*>(sharedRegion.data()),
        sharedRegion.size());
    if (sharedMemory == nullptr)
    {
        lifecycleAudio->unbindHostLifecycle();
        result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
            SharedStorageUnavailable;
        return result;
    }
    auto control = ComponentIsolationControlSessionV1::prepare(
        ComponentIsolationProductServiceV1::GainFixture,
        configuration);
    if (control.diagnostic
        != ComponentIsolationControlSessionDiagnostic::Ready)
    {
        xpc_release(sharedMemory);
        lifecycleAudio->unbindHostLifecycle();
        result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
            InvalidConfiguration;
        return result;
    }
    try
    {
        auto implementation = std::make_shared<Impl>(
            std::move(control.session),
            identity,
            endpoint,
            std::move(sharedAudio),
            sharedMemory);
        sharedMemory = nullptr;
        implementation->controlQueue = dispatch_queue_create(
            controlQueueIdentityV1,
            DISPATCH_QUEUE_SERIAL);
        if (implementation->controlQueue == nullptr)
        {
            lifecycleAudio->unbindHostLifecycle();
            lifecycleBound = false;
            result.diagnostic = ComponentIsolationMacOSHostPreparationDiagnostic::
                ControlQueueUnavailable;
            return result;
        }
        result.host = std::unique_ptr<ComponentIsolationMacOSHost>(
            new ComponentIsolationMacOSHost(std::move(implementation)));
        lifecycleBound = false;
        result.diagnostic =
            ComponentIsolationMacOSHostPreparationDiagnostic::Ready;
    }
    catch (const std::bad_alloc&)
    {
        if (lifecycleBound)
            lifecycleAudio->unbindHostLifecycle();
        if (sharedMemory != nullptr)
            xpc_release(sharedMemory);
        result.host.reset();
        result.diagnostic =
            ComponentIsolationMacOSHostPreparationDiagnostic::AllocationFailed;
    }
    return result;
}

ComponentIsolationMacOSHostOperation ComponentIsolationMacOSHost::start()
    noexcept
{
    if (state() != ComponentIsolationControlSessionState::Prepared)
        return ComponentIsolationMacOSHostOperation::InvalidState;
    bool expected = false;
    if (!impl_->startScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentIsolationMacOSHostOperation::InvalidState;
    }
    impl_->revokeReadyState();
    const auto weak = std::weak_ptr<Impl>(impl_);
    dispatch_async(impl_->controlQueue, ^{
        if (const auto implementation = weak.lock())
            implementation->startOnControlQueue();
    });
    return ComponentIsolationMacOSHostOperation::Scheduled;
}

ComponentIsolationMacOSHostOperation
ComponentIsolationMacOSHost::requestHeartbeat() noexcept
{
    if (state() != ComponentIsolationControlSessionState::Ready)
        return ComponentIsolationMacOSHostOperation::InvalidState;
    bool expected = false;
    if (!impl_->commandScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentIsolationMacOSHostOperation::InvalidState;
    }
    impl_->revokeReadyState();
    const auto weak = std::weak_ptr<Impl>(impl_);
    dispatch_async(impl_->controlQueue, ^{
        if (const auto implementation = weak.lock())
        {
            implementation->sendControlOnQueue(
                ComponentIsolationRecordKind::Heartbeat);
        }
    });
    return ComponentIsolationMacOSHostOperation::Scheduled;
}

ComponentIsolationMacOSHostOperation
ComponentIsolationMacOSHost::requestTeardown() noexcept
{
    if (state() != ComponentIsolationControlSessionState::Ready)
        return ComponentIsolationMacOSHostOperation::InvalidState;
    bool expected = false;
    if (!impl_->commandScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentIsolationMacOSHostOperation::InvalidState;
    }
    impl_->revokeReadyState();
    const auto weak = std::weak_ptr<Impl>(impl_);
    dispatch_async(impl_->controlQueue, ^{
        if (const auto implementation = weak.lock())
        {
            implementation->sendControlOnQueue(
                ComponentIsolationRecordKind::TeardownRequest);
        }
    });
    return ComponentIsolationMacOSHostOperation::Scheduled;
}

ComponentIsolationMacOSHostOperation
ComponentIsolationMacOSHost::reportResponseDeadlineMiss() noexcept
{
    if (state() != ComponentIsolationControlSessionState::Ready)
        return ComponentIsolationMacOSHostOperation::InvalidState;
    bool expected = false;
    if (!impl_->commandScheduled.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
    {
        return ComponentIsolationMacOSHostOperation::InvalidState;
    }
    impl_->revokeReadyState();
    const auto weak = std::weak_ptr<Impl>(impl_);
    dispatch_async(impl_->controlQueue, ^{
        if (const auto implementation = weak.lock())
        {
            if (implementation->stateSnapshot.load(std::memory_order_relaxed)
                == static_cast<std::uint8_t>(
                    ComponentIsolationControlSessionState::Ready))
            {
                implementation->failClosed(
                    ComponentIsolationControlSessionDiagnostic::
                        ResponseDeadlineMiss);
            }
            else
            {
                implementation->commandScheduled.store(
                    false,
                    std::memory_order_release);
            }
        }
    });
    return ComponentIsolationMacOSHostOperation::Scheduled;
}

ComponentIsolationControlSessionState ComponentIsolationMacOSHost::state()
    const noexcept
{
    return static_cast<ComponentIsolationControlSessionState>(
        impl_->stateSnapshot.load(std::memory_order_acquire));
}

ComponentIsolationControlSessionDiagnostic
ComponentIsolationMacOSHost::diagnostic() const noexcept
{
    return static_cast<ComponentIsolationControlSessionDiagnostic>(
        impl_->diagnosticSnapshot.load(std::memory_order_acquire));
}

std::size_t ComponentIsolationMacOSHost::diagnosticBytes() const noexcept
{
    __block std::size_t bytes = 0;
    dispatch_sync(impl_->controlQueue, ^{
        bytes = impl_->session.diagnosticBytes();
    });
    return bytes;
}

std::size_t ComponentIsolationMacOSHost::copyDiagnostic(
    std::span<std::byte> destination) const noexcept
{
    __block std::size_t copied = 0;
    dispatch_sync(impl_->controlQueue, ^{
        copied = impl_->session.copyDiagnostic(destination);
    });
    return copied;
}

std::uint32_t ComponentIsolationMacOSHost::lifecycleTrace() const noexcept
{
    return impl_->lifecycleTraceSnapshot.load(std::memory_order_acquire);
}

bool ComponentIsolationMacOSHost::operationPending() const noexcept
{
    return impl_->commandScheduled.load(std::memory_order_acquire);
}

ComponentIsolationEvidenceV1
ComponentIsolationMacOSHost::isolationEvidence() const noexcept
{
    if (impl_->endpoint
        != ComponentIsolationMacOSProductEndpointV1::ProductionGain)
    {
        return {};
    }
    return ComponentIsolationEvidenceV1{impl_->liveEvidenceState};
}
}
