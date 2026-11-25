#include "app/workbench/WorkbenchOscIngress.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <system_error>

namespace talas::app::workbench
{
WorkbenchOscIngress::Candidate::Candidate() = default;
WorkbenchOscIngress::Candidate::~Candidate() = default;
WorkbenchOscIngress::Candidate::Candidate(Candidate&&) noexcept = default;
WorkbenchOscIngress::Candidate& WorkbenchOscIngress::Candidate::operator=(
    Candidate&&) noexcept = default;

WorkbenchOscIngress::WorkbenchOscIngress(
    talas::runtime::ProjectRuntimeBridge& bridge,
    talas::runtime::RealtimeControlIngress* performanceIngress) noexcept
    : bridge_(bridge)
    , performanceIngress_(performanceIngress)
{
}

WorkbenchOscIngress::~WorkbenchOscIngress()
{
    stop();
}

void WorkbenchOscIngress::incrementSaturating(
    std::atomic<std::uint64_t>& counter) noexcept
{
    const auto value = counter.load(std::memory_order_relaxed);
    if (value != std::numeric_limits<std::uint64_t>::max())
        counter.store(value + 1, std::memory_order_relaxed);
}

WorkbenchOscConfigureResult WorkbenchOscIngress::configure(
    talas::project::ProjectOscIngressSettings settings,
    std::span<const talas::project::PreparedOscControlRoute> routes,
    std::span<const std::string> liveGridAddresses)
{
    auto preparation = prepareCandidate(settings, routes, liveGridAddresses);
    if (!preparation.candidate.has_value())
        return preparation.result;

    suspend();
    if (!commitCandidate(std::move(*preparation.candidate)))
    {
        static_cast<void>(resume());
        preparation.result.accepted = false;
        preparation.result.state = state_.load(std::memory_order_acquire);
        preparation.result.message =
            "Could not discard the bounded prior OSC ingress backlog";
        return preparation.result;
    }
    if (!resume())
    {
        preparation.result.accepted = false;
        preparation.result.state = state_.load(std::memory_order_acquire);
        preparation.result.message =
            "OSC ingress authority generation was exhausted";
        return preparation.result;
    }
    preparation.result.accepted = true;
    preparation.result.state = state_.load(std::memory_order_acquire);
    preparation.result.message = settings.enabled
        ? "Listening on 127.0.0.1:" + std::to_string(settings.port)
        : "OSC loopback ingress stopped";
    return preparation.result;
}

WorkbenchOscIngress::Preparation WorkbenchOscIngress::prepareCandidate(
    talas::project::ProjectOscIngressSettings settings,
    std::span<const talas::project::PreparedOscControlRoute> routes,
    std::span<const std::string> liveGridAddresses)
{
    Preparation preparation;
    auto& result = preparation.result;
    result.state = state_.load(std::memory_order_acquire);
    if (!talas::project::isValidOscIngressSettings(settings))
    {
        result.message = "Invalid OSC loopback settings";
        return preparation;
    }

    Candidate candidate;
    candidate.settings_ = settings;
    result.routes = candidate.routes_.replace(routes);
    if (!result.routes.ok())
    {
        result.message = "Invalid or conflicting OSC route set";
        return preparation;
    }
    try
    {
        candidate.liveGridAddresses_.reserve(liveGridAddresses.size());
        for (const auto& address : liveGridAddresses)
        {
            if (!talas::project::isValidOscAddress(address)
                || std::ranges::any_of(
                    candidate.liveGridAddresses_,
                    [&](const auto& existing) { return existing == address; })
                || std::ranges::any_of(routes, [&](const auto& route) {
                       return route.mapping.address == address;
                   }))
            {
                result.message =
                    "Invalid or conflicting OSC Live Grid route set";
                return preparation;
            }
            candidate.liveGridAddresses_.push_back(address);
        }
    }
    catch (const std::bad_alloc&)
    {
        result.message = "OSC Live Grid route allocation failed";
        return preparation;
    }
    catch (const std::length_error&)
    {
        result.message = "OSC Live Grid route set exceeded supported limits";
        return preparation;
    }

    if (!settings.enabled)
    {
        result.accepted = true;
        result.state = WorkbenchOscIngressState::Stopped;
        result.message = "OSC loopback ingress prepared to stop";
        preparation.candidate.emplace(std::move(candidate));
        return preparation;
    }

    const auto requestedPort = static_cast<std::uint16_t>(settings.port);
    candidate.reuseEndpoint_ =
        state_.load(std::memory_order_acquire)
            == WorkbenchOscIngressState::Listening
        && activePort_.load(std::memory_order_acquire) == requestedPort;
    if (candidate.reuseEndpoint_)
    {
        result.accepted = true;
        result.message = "Prepared same-port OSC ingress route replacement";
        preparation.candidate.emplace(std::move(candidate));
        return preparation;
    }

    try
    {
        candidate.socket_ = std::make_unique<juce::DatagramSocket>(false);
    }
    catch (const std::bad_alloc&)
    {
        result.message = "Could not prepare the OSC loopback socket";
        return preparation;
    }
    if (!candidate.socket_->bindToPort(settings.port, "127.0.0.1"))
    {
        incrementSaturating(bindFailures_);
        failedPort_.store(requestedPort, std::memory_order_release);
        if (state_.load(std::memory_order_acquire)
            != WorkbenchOscIngressState::Listening)
        {
            state_.store(
                WorkbenchOscIngressState::BindFailed,
                std::memory_order_release);
        }
        result.state = state_.load(std::memory_order_acquire);
        result.message = "Could not bind requested 127.0.0.1:"
            + std::to_string(settings.port);
        const auto retainedPort = activePort_.load(std::memory_order_acquire);
        if (retainedPort != 0)
        {
            result.message += "; still listening on 127.0.0.1:"
                + std::to_string(retainedPort);
        }
        return preparation;
    }

    try
    {
        candidate.workerStart_ =
            std::make_shared<std::atomic<std::uint8_t>>(0);
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
        if (failNextWorkerStartForTesting_.exchange(
                false, std::memory_order_acq_rel))
        {
            throw std::system_error{
                std::make_error_code(std::errc::resource_unavailable_try_again)};
        }
#endif
        candidate.worker_ = std::jthread(
            [this, workerStart = candidate.workerStart_](
                std::stop_token stopToken) {
                while (workerStart->load(std::memory_order_acquire) == 0
                       && !stopToken.stop_requested())
                {
                    std::this_thread::yield();
                }
                if (!stopToken.stop_requested()
                    && workerStart->load(std::memory_order_acquire) == 1)
                {
                    run(stopToken);
                }
            });
    }
    catch (const std::exception&)
    {
        failedPort_.store(requestedPort, std::memory_order_release);
        result.state = state_.load(std::memory_order_acquire);
        result.message = "Could not start OSC loopback worker for 127.0.0.1:"
            + std::to_string(settings.port);
        const auto retainedPort = activePort_.load(std::memory_order_acquire);
        if (retainedPort != 0)
        {
            result.message += "; still listening on 127.0.0.1:"
                + std::to_string(retainedPort);
        }
        return preparation;
    }

    result.accepted = true;
    result.message = "Prepared OSC ingress on 127.0.0.1:"
        + std::to_string(settings.port);
    preparation.candidate.emplace(std::move(candidate));
    return preparation;
}

void WorkbenchOscIngress::suspend() noexcept
{
    if (suspended_)
        return;
    static_cast<void>(
        advanceWorkbenchAuthorityGeneration(routeGeneration_));
    admissionGate_.closeAndWait();
    bridge_.resetRealtimeControlTiming();
    suspended_ = true;
}

bool WorkbenchOscIngress::discardPendingPacketsLocked() noexcept
{
    if (socket_ == nullptr)
        return true;
    std::array<std::uint8_t, talas::runtime::maximumOscUdpPacketBytes> packet{};
    constexpr std::size_t maximumDiscardedPackets = 1'024;
    for (std::size_t discarded = 0;
         discarded < maximumDiscardedPackets;
         ++discarded)
    {
        const auto ready = socket_->waitUntilReady(true, 0);
        if (ready == 0)
            return true;
        if (ready < 0
            || socket_->read(
                   packet.data(), static_cast<int>(packet.size()), false) < 0)
        {
            return false;
        }
        incrementSaturating(staleRoutes_);
    }
    return socket_->waitUntilReady(true, 0) == 0;
}

bool WorkbenchOscIngress::commitCandidate(Candidate&& candidate) noexcept
{
    if (!suspended_)
        return false;
    if (currentWorkbenchAuthorityGeneration(routeGeneration_) == 0)
        return false;
    if (!candidate.settings_.enabled)
    {
        stopLocked(true);
        failedPort_.store(0, std::memory_order_release);
        state_.store(
            WorkbenchOscIngressState::Stopped,
            std::memory_order_release);
        return true;
    }

    if (candidate.reuseEndpoint_ && !discardPendingPacketsLocked())
        return false;

    if (!candidate.reuseEndpoint_)
    {
        stopLocked(false);
        socket_ = std::move(candidate.socket_);
        workerStart_ = std::move(candidate.workerStart_);
        worker_ = std::move(candidate.worker_);
    }
    routes_ = candidate.routes_;
    liveGridAddresses_ = std::move(candidate.liveGridAddresses_);
    activePort_.store(
        static_cast<std::uint16_t>(candidate.settings_.port),
        std::memory_order_release);
    failedPort_.store(0, std::memory_order_release);
    state_.store(
        WorkbenchOscIngressState::Listening,
        std::memory_order_release);
    return true;
}

void WorkbenchOscIngress::setLiveGridCallback(LiveGridCallback callback)
{
    if (worker_.joinable())
        return;
    liveGridCallback_ = std::move(callback);
}

bool WorkbenchOscIngress::resume() noexcept
{
    if (!suspended_)
        return currentWorkbenchAuthorityGeneration(routeGeneration_) != 0;
    if (currentWorkbenchAuthorityGeneration(routeGeneration_) == 0)
    {
        const auto retainedPort = activePort_.load(std::memory_order_acquire);
        stopLocked(true);
        failedPort_.store(retainedPort, std::memory_order_release);
        state_.store(
            WorkbenchOscIngressState::BindFailed,
            std::memory_order_release);
        suspended_ = false;
        return false;
    }
    if (workerStart_ != nullptr)
        workerStart_->store(1, std::memory_order_release);
    admissionGate_.open();
    suspended_ = false;
    return true;
}

void WorkbenchOscIngress::stopLocked(bool clearRoutes) noexcept
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
    workerStart_.reset();
    socket_.reset();
    activePort_.store(0, std::memory_order_release);
    if (clearRoutes)
    {
        routes_.clear();
        liveGridAddresses_.clear();
    }
}

void WorkbenchOscIngress::stop() noexcept
{
    suspend();
    stopLocked(true);
    state_.store(
        WorkbenchOscIngressState::Stopped,
        std::memory_order_release);
    failedPort_.store(0, std::memory_order_release);
}

void WorkbenchOscIngress::run(std::stop_token stopToken) noexcept
{
    std::array<std::uint8_t, talas::runtime::maximumOscUdpPacketBytes>
        packet{};
    std::array<talas::runtime::OscControlMessage,
               talas::runtime::maximumOscPacketElements>
        messages{};
    std::array<talas::runtime::OscNtpConversionResult,
               talas::runtime::maximumOscPacketElements>
        timing{};
    while (!stopToken.stop_requested())
    {
        if (!admissionGate_.tryBegin())
        {
            std::this_thread::yield();
            continue;
        }
        struct AdmissionScope
        {
            talas::project::RealtimeOscRouteAdmissionGate& gate;
            ~AdmissionScope() { gate.end(); }
        } admission{admissionGate_};
        auto* socket = socket_.get();
        if (socket == nullptr)
            break;
        const auto ready = socket->waitUntilReady(true, 10);
        if (ready <= 0)
            continue;
        const auto packetRouteGeneration =
            currentWorkbenchAuthorityGeneration(routeGeneration_);
        const auto packetPort = activePort_.load(std::memory_order_acquire);
        const auto bytesRead = socket->read(
            packet.data(), static_cast<int>(packet.size()), false);
        if (bytesRead < 0)
            continue;

        incrementSaturating(receivedPackets_);
        const auto steadyBeforeMilliseconds =
            juce::Time::getMillisecondCounterHiRes();
        const auto wallNanoseconds = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                         .count();
        const auto steadyAfterMilliseconds =
            juce::Time::getMillisecondCounterHiRes();
        const auto validSteadyBracket =
            std::isfinite(steadyBeforeMilliseconds)
            && std::isfinite(steadyAfterMilliseconds)
            && steadyBeforeMilliseconds > 0.0
            && steadyAfterMilliseconds >= steadyBeforeMilliseconds
            && steadyAfterMilliseconds < 9'000'000'000'000.0;
        const auto steadyBeforeNanoseconds = validSteadyBracket
            ? static_cast<std::int64_t>(std::llround(
                steadyBeforeMilliseconds * 1'000'000.0))
            : std::int64_t{0};
        const auto steadyAfterNanoseconds = validSteadyBracket
            ? static_cast<std::int64_t>(std::llround(
                steadyAfterMilliseconds * 1'000'000.0))
            : std::int64_t{0};
        const auto steadyNanoseconds = validSteadyBracket
            ? steadyBeforeNanoseconds
                + (steadyAfterNanoseconds - steadyBeforeNanoseconds) / 2
            : std::int64_t{0};
        const auto arrivalTimeSeconds =
            static_cast<double>(steadyNanoseconds) / 1'000'000'000.0;
        const auto clockAnchor = talas::runtime::makeOscNtpClockAnchor(
            wallNanoseconds, steadyNanoseconds);
        const auto monitor = [&](std::string_view address,
                                 double normalizedValue,
                                 WorkbenchOscMonitorOutcome outcome) {
            publishMonitor(
                address,
                normalizedValue,
                outcome,
                packetPort,
                packetRouteGeneration);
        };
        if (bytesRead == 0)
        {
            incrementSaturating(malformedPackets_);
            monitor({}, 0.0, WorkbenchOscMonitorOutcome::Malformed);
            continue;
        }
        const auto parsed = talas::runtime::parseOscControlPacket(
            std::span<const std::uint8_t>{
                packet.data(), static_cast<std::size_t>(bytesRead)},
            messages);
        if (!parsed.accepted())
        {
            if (parsed.status
                == talas::runtime::OscPacketStatus::UnsupportedTimetag)
            {
                incrementSaturating(unsupportedTimetags_);
                monitor({}, 0.0,
                        WorkbenchOscMonitorOutcome::UnsupportedTimetag);
            }
            else if (parsed.status
                     == talas::runtime::OscPacketStatus::RegressingTimetag)
            {
                incrementSaturating(regressingTimetags_);
                monitor({}, 0.0,
                        WorkbenchOscMonitorOutcome::RegressingTimetag);
            }
            else
            {
                incrementSaturating(malformedPackets_);
                monitor({}, 0.0,
                        WorkbenchOscMonitorOutcome::Malformed);
            }
            continue;
        }
        if (packetRouteGeneration == 0)
        {
            for (std::size_t index = 0; index < parsed.messageCount; ++index)
            {
                incrementSaturating(staleRoutes_);
                monitor(
                    messages[index].addressView(),
                    messages[index].normalizedValue,
                    WorkbenchOscMonitorOutcome::RouteStale);
            }
            continue;
        }

        auto timingRejected = false;
        for (std::size_t index = 0; index < parsed.messageCount; ++index)
        {
            timing[index] = talas::runtime::convertOscNtpTimetag(
                messages[index].timetag, clockAnchor);
            if (timing[index].accepted())
                continue;
            timingRejected = true;
            switch (timing[index].status)
            {
            case talas::runtime::OscNtpConversionStatus::TooLate:
                incrementSaturating(timetagsTooLate_);
                monitor({}, 0.0, WorkbenchOscMonitorOutcome::TimetagTooLate);
                break;
            case talas::runtime::OscNtpConversionStatus::TooFar:
                incrementSaturating(timetagsTooFar_);
                monitor({}, 0.0, WorkbenchOscMonitorOutcome::TimetagTooFar);
                break;
            default:
                incrementSaturating(timetagConversionFailures_);
                monitor(
                    {}, 0.0,
                    WorkbenchOscMonitorOutcome::TimetagConversionFailed);
                break;
            }
            break;
        }
        if (timingRejected)
            continue;

#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
        if (pauseAfterReadForTesting_.load(std::memory_order_acquire))
        {
            readPausedForTesting_.store(true, std::memory_order_release);
            while (pauseAfterReadForTesting_.load(
                       std::memory_order_acquire)
                   && !stopToken.stop_requested())
            {
                std::this_thread::yield();
            }
            readPausedForTesting_.store(false, std::memory_order_release);
            if (stopToken.stop_requested())
                break;
        }
#endif

        if (currentWorkbenchAuthorityGeneration(routeGeneration_)
            != packetRouteGeneration)
        {
            for (std::size_t index = 0; index < parsed.messageCount; ++index)
            {
                incrementSaturating(staleRoutes_);
                monitor(
                    messages[index].addressView(),
                    messages[index].normalizedValue,
                    WorkbenchOscMonitorOutcome::RouteStale);
            }
            continue;
        }
        for (std::size_t index = 0; index < parsed.messageCount; ++index)
        {
            const auto& message = messages[index];
            const auto liveGridRoute = std::ranges::find(
                liveGridAddresses_, message.addressView());
            if (liveGridRoute != liveGridAddresses_.end())
            {
                const auto scheduled = timing[index].status
                        == talas::runtime::OscNtpConversionStatus::Scheduled
                    || timing[index].status
                        == talas::runtime::OscNtpConversionStatus::Late;
                if (scheduled || !liveGridCallback_)
                {
                    incrementSaturating(unsupportedTimetags_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::UnsupportedTimetag);
                    continue;
                }
                const auto routeIndex = static_cast<std::uint16_t>(
                    std::distance(
                        liveGridAddresses_.begin(), liveGridRoute));
                if (liveGridCallback_(routeIndex, packetRouteGeneration))
                {
                    incrementSaturating(acceptedMessages_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::Accepted);
                }
                else
                {
                    incrementSaturating(queueFull_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::QueueFull);
                }
                continue;
            }
            const auto mapped = routes_.map(
                message.addressView(), message.normalizedValue);
            if (!mapped.has_value())
            {
                incrementSaturating(unmappedMessages_);
                monitor(
                    message.addressView(),
                    message.normalizedValue,
                    WorkbenchOscMonitorOutcome::Unmapped);
                continue;
            }
            if (bridge_.latestStageGeneration()
                != mapped->stageGeneration)
            {
                incrementSaturating(staleRoutes_);
                monitor(
                    message.addressView(),
                    message.normalizedValue,
                    WorkbenchOscMonitorOutcome::RouteStale);
                continue;
            }
            const auto scheduled = timing[index].status
                    == talas::runtime::OscNtpConversionStatus::Scheduled
                || timing[index].status
                    == talas::runtime::OscNtpConversionStatus::Late;
            if (mapped->performanceMacro)
            {
                if (scheduled || performanceIngress_ == nullptr)
                {
                    incrementSaturating(unsupportedTimetags_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::UnsupportedTimetag);
                    continue;
                }
                talas::runtime::RealtimeControlCommand command;
                command.eventId = mapped->eventId;
                command.value = mapped->value;
                command.domain = mapped->domain;
                command.stageGeneration = mapped->stageGeneration;
                command.generation = packetRouteGeneration;
                command.recordingOrigin =
                    talas::project::ControllerRecordingOrigin::Osc;
                command.recordingSourceTimestampNanoseconds =
                    steadyNanoseconds;
                command.recordingRouteGeneration = packetRouteGeneration;
                command.recordingSourceGeneration = packetRouteGeneration;
                if (performanceIngress_->tryPush(command))
                {
                    incrementSaturating(acceptedMessages_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::Accepted);
                }
                else
                {
                    incrementSaturating(queueFull_);
                    monitor(
                        message.addressView(),
                        message.normalizedValue,
                        WorkbenchOscMonitorOutcome::QueueFull);
                }
                continue;
            }
            const auto admitted = scheduled
                ? bridge_.enqueueEventValueAtSteadyTimeForStage(
                    mapped->eventId,
                    mapped->value,
                    mapped->domain,
                    timing[index].steadyDeadlineNanoseconds,
                    mapped->stageGeneration,
                    {
                        talas::project::ControllerRecordingOrigin::Osc,
                        timing[index].steadyDeadlineNanoseconds,
                        packetRouteGeneration,
                        packetRouteGeneration,
                    })
                : bridge_.enqueueEventValueAtTimeForStage(
                    mapped->eventId,
                    mapped->value,
                    mapped->domain,
                    arrivalTimeSeconds,
                    mapped->stageGeneration,
                    {
                        talas::project::ControllerRecordingOrigin::Osc,
                        steadyNanoseconds,
                        packetRouteGeneration,
                        packetRouteGeneration,
                    });
            if (admitted)
            {
                incrementSaturating(acceptedMessages_);
                if (scheduled)
                    incrementSaturating(scheduledMessages_);
                if (timing[index].status
                    == talas::runtime::OscNtpConversionStatus::Late)
                {
                    incrementSaturating(lateMessages_);
                }
                monitor(
                    message.addressView(),
                    message.normalizedValue,
                    timing[index].status
                            == talas::runtime::OscNtpConversionStatus::Late
                        ? WorkbenchOscMonitorOutcome::LateAccepted
                        : scheduled
                            ? WorkbenchOscMonitorOutcome::Scheduled
                            : WorkbenchOscMonitorOutcome::Accepted);
            }
            else if (bridge_.latestStageGeneration()
                     != mapped->stageGeneration)
            {
                incrementSaturating(staleRoutes_);
                monitor(
                    message.addressView(),
                    message.normalizedValue,
                    WorkbenchOscMonitorOutcome::RouteStale);
            }
            else
            {
                if (scheduled)
                    incrementSaturating(scheduleFull_);
                else
                    incrementSaturating(queueFull_);
                monitor(
                    message.addressView(),
                    message.normalizedValue,
                    scheduled
                        ? WorkbenchOscMonitorOutcome::ScheduleFull
                        : WorkbenchOscMonitorOutcome::QueueFull);
            }
        }
    }
}

void WorkbenchOscIngress::publishMonitor(
    std::string_view address,
    double normalizedValue,
    WorkbenchOscMonitorOutcome outcome,
    std::uint16_t endpointPort,
    std::uint64_t routeGeneration) noexcept
{
    auto sequence = nextMonitorSequence_.load(std::memory_order_relaxed);
    if (sequence != std::numeric_limits<std::uint64_t>::max())
        nextMonitorSequence_.store(sequence + 1, std::memory_order_relaxed);
    auto& slot = monitor_[sequence % monitor_.size()];
    slot.publication.fetch_add(1, std::memory_order_acq_rel);
    slot.sequence.store(sequence, std::memory_order_relaxed);
    slot.routeGeneration.store(
        routeGeneration, std::memory_order_relaxed);
    slot.endpointPort.store(endpointPort, std::memory_order_relaxed);
    const auto addressSize = std::min(
        address.size(), talas::project::maximumOscAddressBytes);
    for (std::size_t index = 0; index < addressSize; ++index)
        slot.address[index].store(address[index], std::memory_order_relaxed);
    slot.address[addressSize].store('\0', std::memory_order_relaxed);
    slot.addressSize.store(
        static_cast<std::uint16_t>(addressSize),
        std::memory_order_relaxed);
    slot.normalizedValue.store(normalizedValue, std::memory_order_relaxed);
    slot.outcome.store(outcome, std::memory_order_relaxed);
    slot.publication.fetch_add(1, std::memory_order_release);
}

WorkbenchOscIngressTelemetry WorkbenchOscIngress::telemetry() const noexcept
{
    const auto controls = bridge_.realtimeControlTelemetry();
    const auto scheduleFull =
        scheduleFull_.load(std::memory_order_acquire);
    const auto combinedScheduleFull = scheduleFull
            > std::numeric_limits<std::uint64_t>::max()
                - controls.scheduledPendingFull
        ? std::numeric_limits<std::uint64_t>::max()
        : scheduleFull + controls.scheduledPendingFull;
    return {
        state_.load(std::memory_order_acquire),
        activePort_.load(std::memory_order_acquire),
        failedPort_.load(std::memory_order_acquire),
        receivedPackets_.load(std::memory_order_acquire),
        acceptedMessages_.load(std::memory_order_acquire),
        unmappedMessages_.load(std::memory_order_acquire),
        malformedPackets_.load(std::memory_order_acquire),
        unsupportedTimetags_.load(std::memory_order_acquire),
        regressingTimetags_.load(std::memory_order_acquire),
        scheduledMessages_.load(std::memory_order_acquire),
        lateMessages_.load(std::memory_order_acquire),
        controls.scheduledLate,
        timetagsTooLate_.load(std::memory_order_acquire),
        timetagsTooFar_.load(std::memory_order_acquire),
        timetagConversionFailures_.load(std::memory_order_acquire),
        controls.scheduledApplied,
        controls.scheduledStale,
        controls.scheduledPending,
        controls.scheduledPendingHighWatermark,
        staleRoutes_.load(std::memory_order_acquire),
        queueFull_.load(std::memory_order_acquire),
        combinedScheduleFull,
        bindFailures_.load(std::memory_order_acquire),
    };
}

std::array<WorkbenchOscMonitorRecord, 8>
WorkbenchOscIngress::recentRecords() const noexcept
{
    std::array<WorkbenchOscMonitorRecord, 8> result{};
    for (std::size_t index = 0; index < monitor_.size(); ++index)
    {
        const auto& slot = monitor_[index];
        for (std::size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = slot.publication.load(
                std::memory_order_acquire);
            if ((before & 1U) != 0)
                continue;
            WorkbenchOscMonitorRecord candidate;
            candidate.sequence = slot.sequence.load(
                std::memory_order_relaxed);
            candidate.routeGeneration = slot.routeGeneration.load(
                std::memory_order_relaxed);
            candidate.endpointPort = slot.endpointPort.load(
                std::memory_order_relaxed);
            candidate.addressSize = slot.addressSize.load(
                std::memory_order_relaxed);
            if (candidate.addressSize
                > talas::project::maximumOscAddressBytes)
            {
                continue;
            }
            for (std::size_t byte = 0;
                 byte <= candidate.addressSize;
                 ++byte)
            {
                candidate.address[byte] = slot.address[byte].load(
                    std::memory_order_relaxed);
            }
            candidate.normalizedValue = slot.normalizedValue.load(
                std::memory_order_relaxed);
            candidate.outcome = slot.outcome.load(
                std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = slot.publication.load(
                std::memory_order_relaxed);
            if (before == after && (after & 1U) == 0)
            {
                result[index] = candidate;
                break;
            }
        }
    }
    std::ranges::sort(result, {}, &WorkbenchOscMonitorRecord::sequence);
    return result;
}
}
