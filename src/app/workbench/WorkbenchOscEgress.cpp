#include "app/workbench/WorkbenchOscEgress.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <system_error>
#include <unordered_set>

#if JUCE_WINDOWS
#include <winsock2.h>
#else
#include <fcntl.h>
#endif

namespace talas::app::workbench
{
namespace
{
struct ActiveWorkerOperation final
{
    std::atomic<std::uint32_t>& count;
    ~ActiveWorkerOperation()
    {
        count.fetch_sub(1, std::memory_order_release);
    }
};

[[nodiscard]] bool validRoute(
    const PreparedWorkbenchOscEgressRoute& route) noexcept
{
    return talas::project::isValidOscEgressMapping(route.mapping)
        && !route.eventName.empty()
        && (route.kind == talas::project::VirtualControlKind::Fader
            || route.kind == talas::project::VirtualControlKind::Gate
            || route.kind == talas::project::VirtualControlKind::Toggle)
        && std::isfinite(route.value)
        && std::isfinite(route.minimum)
        && std::isfinite(route.maximum)
        && route.minimum < route.maximum
        && route.value >= route.minimum
        && route.value <= route.maximum
        && route.stageGeneration != 0;
}

[[nodiscard]] bool makeSocketNonblocking(
    juce::DatagramSocket& socket) noexcept
{
#if JUCE_WINDOWS
    u_long enabled = 1;
    return ::ioctlsocket(
               static_cast<SOCKET>(socket.getRawSocketHandle()),
               FIONBIO,
               &enabled)
        == 0;
#else
    const auto handle = socket.getRawSocketHandle();
    const auto flags = ::fcntl(handle, F_GETFL, 0);
    return flags >= 0 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}
}

WorkbenchOscEgress::Candidate::Candidate() = default;
WorkbenchOscEgress::Candidate::~Candidate() = default;
WorkbenchOscEgress::Candidate::Candidate(Candidate&&) noexcept = default;
WorkbenchOscEgress::Candidate& WorkbenchOscEgress::Candidate::operator=(
    Candidate&&) noexcept = default;

WorkbenchOscEgress::~WorkbenchOscEgress()
{
    stop();
}

void WorkbenchOscEgress::incrementSaturating(
    std::atomic<std::uint64_t>& counter) noexcept
{
    const auto value = counter.load(std::memory_order_relaxed);
    if (value != std::numeric_limits<std::uint64_t>::max())
        counter.store(value + 1, std::memory_order_relaxed);
}

WorkbenchOscEgressConfigureResult WorkbenchOscEgress::configure(
    talas::project::ProjectOscEgressSettings settings,
    std::span<const PreparedWorkbenchOscEgressRoute> routes)
{
    auto preparation = prepareCandidate(settings, routes);
    if (!preparation.candidate.has_value())
    {
        if (preparation.result.state == WorkbenchOscEgressState::Ready)
            resume();
        return preparation.result;
    }

    suspend();
    commitCandidate(std::move(*preparation.candidate));
    resume();
    const auto synced = !settings.enabled || resync();
    preparation.result.accepted = true;
    preparation.result.state = state_.load(std::memory_order_acquire);
    preparation.result.message = !settings.enabled
        ? "OSC loopback feedback stopped"
        : synced
            ? "OSC feedback ready on 127.0.0.1:"
                + std::to_string(settings.port)
            : "OSC feedback ready; initial resync is incomplete";
    return preparation.result;
}

WorkbenchOscEgress::Preparation WorkbenchOscEgress::prepareCandidate(
    talas::project::ProjectOscEgressSettings settings,
    std::span<const PreparedWorkbenchOscEgressRoute> routes)
{
    Preparation preparation;
    auto& result = preparation.result;
    result.state = state_.load(std::memory_order_acquire);
    if (!talas::project::isValidOscEgressSettings(settings))
    {
        result.message = "Invalid OSC feedback settings";
        return preparation;
    }
    Candidate candidate;
    candidate.settings_ = settings;
    if (!settings.enabled)
    {
        result.accepted = true;
        result.state = WorkbenchOscEgressState::Stopped;
        result.message = "OSC loopback feedback prepared to stop";
        preparation.candidate.emplace(std::move(candidate));
        return preparation;
    }
    if (routes.size() > talas::project::maximumOscControlMappings)
    {
        result.message = "OSC feedback exceeds the fixed 128-route limit";
        return preparation;
    }

    try
    {
        std::unordered_set<std::string> addresses;
        std::unordered_set<std::string> eventNames;
        candidate.routes_.reserve(routes.size());
        for (const auto& route : routes)
        {
            if (!validRoute(route)
                || !addresses.insert(route.mapping.address).second
                || !eventNames.insert(route.eventName).second)
            {
                result.message = "Invalid or conflicting OSC feedback route set";
                return preparation;
            }
            candidate.routes_.push_back(route);
        }
    }
    catch (const std::bad_alloc&)
    {
        result.message = "Could not prepare the bounded OSC feedback route set";
        return preparation;
    }

    const auto requestedPort = static_cast<std::uint16_t>(settings.port);
    try
    {
        candidate.socket_ = std::make_unique<juce::DatagramSocket>(false);
        if (!makeSocketNonblocking(*candidate.socket_))
            throw std::system_error{
                std::make_error_code(std::errc::io_error)};
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
        incrementSaturating(startFailures_);
        failedPort_.store(requestedPort, std::memory_order_release);
        if (state_.load(std::memory_order_acquire)
            != WorkbenchOscEgressState::Ready)
        {
            state_.store(
                WorkbenchOscEgressState::StartFailed,
                std::memory_order_release);
        }
        result.state = state_.load(std::memory_order_acquire);
        result.message = "Could not start OSC feedback worker for 127.0.0.1:"
            + std::to_string(settings.port);
        const auto retainedPort = activePort_.load(std::memory_order_acquire);
        if (retainedPort != 0)
        {
            result.message += "; still sending to 127.0.0.1:"
                + std::to_string(retainedPort);
        }
        return preparation;
    }

    result.accepted = true;
    result.message = "Prepared OSC feedback endpoint 127.0.0.1:"
        + std::to_string(settings.port);
    preparation.candidate.emplace(std::move(candidate));
    return preparation;
}

void WorkbenchOscEgress::suspend() noexcept
{
    invalidateRoutes();
}

void WorkbenchOscEgress::commitCandidate(Candidate&& candidate) noexcept
{
    if (!candidate.settings_.enabled)
    {
        stopWorker();
        clearQueue();
        routes_.clear();
        activePort_.store(0, std::memory_order_release);
        failedPort_.store(0, std::memory_order_release);
        resyncRequired_.store(false, std::memory_order_release);
        state_.store(
            WorkbenchOscEgressState::Stopped,
            std::memory_order_release);
        return;
    }

    stopWorker();
    clearQueue();
    routes_ = std::move(candidate.routes_);
    socket_ = std::move(candidate.socket_);
    workerStart_ = std::move(candidate.workerStart_);
    worker_ = std::move(candidate.worker_);
    activePort_.store(
        static_cast<std::uint16_t>(candidate.settings_.port),
        std::memory_order_release);
    failedPort_.store(0, std::memory_order_release);
    state_.store(WorkbenchOscEgressState::Ready, std::memory_order_release);
}

void WorkbenchOscEgress::stopWorker() noexcept
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
    workerStart_.reset();
    socket_.reset();
}

void WorkbenchOscEgress::clearQueue() noexcept
{
    queueHead_.store(0, std::memory_order_relaxed);
    queueTail_.store(0, std::memory_order_relaxed);
}

void WorkbenchOscEgress::invalidateRoutes() noexcept
{
    {
        const std::lock_guard admissionLock{workerAdmissionGate_};
        if (!workerSuspended_)
        {
            workerSuspended_ = true;
            routeGeneration_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    while (activeWorkerOperations_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    clearQueue();
    if (!routes_.empty()
        && state_.load(std::memory_order_acquire)
            == WorkbenchOscEgressState::Ready)
    {
        resyncRequired_.store(true, std::memory_order_release);
    }
}

void WorkbenchOscEgress::resume() noexcept
{
    {
        const std::lock_guard admissionLock{workerAdmissionGate_};
        workerSuspended_ = false;
    }
    if (workerStart_ != nullptr)
        workerStart_->store(1, std::memory_order_release);
}

void WorkbenchOscEgress::stop() noexcept
{
    invalidateRoutes();
    stopWorker();
    clearQueue();
    routes_.clear();
    activePort_.store(0, std::memory_order_release);
    failedPort_.store(0, std::memory_order_release);
    resyncRequired_.store(false, std::memory_order_release);
    state_.store(WorkbenchOscEgressState::Stopped, std::memory_order_release);
}

bool WorkbenchOscEgress::enqueue(
    const PreparedWorkbenchOscEgressRoute& route,
    double persistedValue,
    std::uint64_t routeGeneration) noexcept
{
    const auto normalized = talas::project::normalizeOscEgressValue(
        route.kind,
        persistedValue,
        route.minimum,
        route.maximum);
    PacketRecord record;
    record.routeGeneration = routeGeneration;
    record.endpointPort = activePort_.load(std::memory_order_acquire);
    if (!normalized.has_value()
        || record.endpointPort == 0
        || route.mapping.address.size()
            > talas::project::maximumOscAddressBytes)
    {
        incrementSaturating(encodeFailures_);
        resyncRequired_.store(true, std::memory_order_release);
        record.normalizedValue = normalized.value_or(0.0);
        publishMonitor(record, WorkbenchOscEgressOutcome::EncodeFailed);
        return false;
    }
    record.normalizedValue = *normalized;
    record.addressSize = static_cast<std::uint16_t>(
        route.mapping.address.size());
    std::memcpy(
        record.address.data(),
        route.mapping.address.data(),
        route.mapping.address.size());
    const auto encoded = talas::runtime::encodeOscControlMessage(
        route.mapping.address,
        *normalized,
        record.packet);
    if (!encoded.encoded())
    {
        incrementSaturating(encodeFailures_);
        resyncRequired_.store(true, std::memory_order_release);
        publishMonitor(record, WorkbenchOscEgressOutcome::EncodeFailed);
        return false;
    }
    record.packetSize = static_cast<std::uint16_t>(encoded.packetSize);

    const auto head = queueHead_.load(std::memory_order_relaxed);
    const auto tail = queueTail_.load(std::memory_order_acquire);
    if (head - tail >= queue_.size())
    {
        incrementSaturating(queueFull_);
        resyncRequired_.store(true, std::memory_order_release);
        publishMonitor(record, WorkbenchOscEgressOutcome::QueueFull);
        return false;
    }
    queue_[head % queue_.size()] = record;
    queueHead_.store(head + 1, std::memory_order_release);
    return true;
}

bool WorkbenchOscEgress::publish(
    std::string_view eventName,
    double persistedValue,
    std::uint64_t stageGeneration) noexcept
{
    if (state_.load(std::memory_order_acquire)
        != WorkbenchOscEgressState::Ready)
    {
        return false;
    }
    const auto generation = routeGeneration_.load(std::memory_order_acquire);
    const auto found = std::ranges::find(
        routes_, eventName, &PreparedWorkbenchOscEgressRoute::eventName);
    if (found == routes_.end() || found->stageGeneration != stageGeneration)
    {
        incrementSaturating(staleRoutes_);
        PacketRecord record;
        record.routeGeneration = generation;
        record.endpointPort = activePort_.load(std::memory_order_acquire);
        publishMonitor(record, WorkbenchOscEgressOutcome::RouteStale);
        return false;
    }
    const auto normalized = talas::project::normalizeOscEgressValue(
        found->kind,
        persistedValue,
        found->minimum,
        found->maximum);
    if (!normalized.has_value())
        return enqueue(*found, persistedValue, generation);
    found->value = persistedValue;
    return enqueue(*found, persistedValue, generation);
}

bool WorkbenchOscEgress::resync() noexcept
{
    if (state_.load(std::memory_order_acquire)
        != WorkbenchOscEgressState::Ready)
    {
        return false;
    }
    const auto head = queueHead_.load(std::memory_order_relaxed);
    const auto tail = queueTail_.load(std::memory_order_acquire);
    if (routes_.size() > queue_.size() - (head - tail))
    {
        incrementSaturating(resyncIncomplete_);
        resyncRequired_.store(true, std::memory_order_release);
        PacketRecord record;
        record.routeGeneration = routeGeneration_.load(
            std::memory_order_acquire);
        record.endpointPort = activePort_.load(std::memory_order_acquire);
        publishMonitor(record, WorkbenchOscEgressOutcome::ResyncIncomplete);
        return false;
    }
    resyncRequired_.store(false, std::memory_order_release);
    const auto generation = routeGeneration_.load(std::memory_order_acquire);
    for (const auto& route : routes_)
    {
        if (!enqueue(route, route.value, generation))
        {
            incrementSaturating(resyncIncomplete_);
            resyncRequired_.store(true, std::memory_order_release);
            return false;
        }
    }
    return true;
}

void WorkbenchOscEgress::run(std::stop_token stopToken) noexcept
{
    while (!stopToken.stop_requested())
    {
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
        if (pauseWorkerForTesting_.load(std::memory_order_acquire))
        {
            workerPausedForTesting_.store(true, std::memory_order_release);
            while (pauseWorkerForTesting_.load(std::memory_order_acquire)
                   && !stopToken.stop_requested())
            {
                std::this_thread::yield();
            }
            workerPausedForTesting_.store(false, std::memory_order_release);
            continue;
        }
#endif
        bool admitted = false;
        {
            const std::lock_guard admissionLock{workerAdmissionGate_};
            if (!workerSuspended_ && !stopToken.stop_requested())
            {
                activeWorkerOperations_.fetch_add(
                    1, std::memory_order_acq_rel);
                admitted = true;
            }
        }
        if (!admitted)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const ActiveWorkerOperation activeOperation{activeWorkerOperations_};
        const auto tail = queueTail_.load(std::memory_order_relaxed);
        const auto head = queueHead_.load(std::memory_order_acquire);
        if (tail == head)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto record = queue_[tail % queue_.size()];
        queueTail_.store(tail + 1, std::memory_order_release);
        if (record.routeGeneration
            != routeGeneration_.load(std::memory_order_acquire))
        {
            incrementSaturating(workerStaleRoutes_);
            publishMonitor(record, WorkbenchOscEgressOutcome::RouteStale);
            continue;
        }
        auto* socket = socket_.get();
        if (socket == nullptr)
            break;
        int written = -1;
        bool staleAfterWait = false;
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
        const auto failSend = failNextSendForTesting_.exchange(
            false, std::memory_order_acq_rel);
        if (!failSend)
#endif
        {
            const auto ready = socket->waitUntilReady(false, 10);
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
            if (ready > 0
                && pauseBeforeSendForTesting_.load(
                    std::memory_order_acquire))
            {
                workerBeforeSendForTesting_.store(
                    true, std::memory_order_release);
                while (pauseBeforeSendForTesting_.load(
                           std::memory_order_acquire)
                       && !stopToken.stop_requested()
                       && record.routeGeneration
                           == routeGeneration_.load(
                               std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                workerBeforeSendForTesting_.store(
                    false, std::memory_order_release);
            }
#endif
            staleAfterWait = record.routeGeneration
                != routeGeneration_.load(std::memory_order_acquire);
            if (ready > 0 && !stopToken.stop_requested() && !staleAfterWait)
            {
                socket = socket_.get();
                written = socket != nullptr
                    ? socket->write(
                          "127.0.0.1",
                          static_cast<int>(record.endpointPort),
                          record.packet.data(),
                          static_cast<int>(record.packetSize))
                    : -1;
            }
        }
        if (staleAfterWait)
        {
            incrementSaturating(workerStaleRoutes_);
            publishMonitor(record, WorkbenchOscEgressOutcome::RouteStale);
            continue;
        }
        if (stopToken.stop_requested())
            continue;
        if (written == static_cast<int>(record.packetSize))
        {
            incrementSaturating(sentMessages_);
            publishMonitor(record, WorkbenchOscEgressOutcome::Sent);
        }
        else
        {
            incrementSaturating(sendFailures_);
            resyncRequired_.store(true, std::memory_order_release);
            publishMonitor(record, WorkbenchOscEgressOutcome::SendFailed);
        }
    }
}

void WorkbenchOscEgress::publishMonitor(
    const PacketRecord& record,
    WorkbenchOscEgressOutcome outcome) noexcept
{
    auto sequence = nextMonitorSequence_.load(std::memory_order_relaxed);
    bool reservedSequence = sequence == std::numeric_limits<std::uint64_t>::max();
    for (std::size_t attempt = 0; !reservedSequence && attempt < 8; ++attempt)
    {
        reservedSequence = nextMonitorSequence_.compare_exchange_weak(
            sequence,
            sequence + 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed);
    }
    if (!reservedSequence)
        return;
    auto& slot = monitor_[sequence % monitor_.size()];
    auto publication = slot.publication.load(std::memory_order_acquire);
    if ((publication & 1U) != 0
        || !slot.publication.compare_exchange_strong(
            publication,
            publication + 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }
    slot.sequence.store(sequence, std::memory_order_relaxed);
    slot.routeGeneration.store(
        record.routeGeneration, std::memory_order_relaxed);
    slot.endpointPort.store(record.endpointPort, std::memory_order_relaxed);
    for (std::size_t index = 0; index < record.addressSize; ++index)
        slot.address[index].store(record.address[index], std::memory_order_relaxed);
    slot.address[record.addressSize].store('\0', std::memory_order_relaxed);
    slot.addressSize.store(record.addressSize, std::memory_order_relaxed);
    slot.normalizedValue.store(record.normalizedValue, std::memory_order_relaxed);
    slot.outcome.store(outcome, std::memory_order_relaxed);
    slot.publication.store(publication + 2, std::memory_order_release);
}

WorkbenchOscEgressTelemetry WorkbenchOscEgress::telemetry() const noexcept
{
    const auto producerStale = staleRoutes_.load(std::memory_order_acquire);
    const auto workerStale = workerStaleRoutes_.load(std::memory_order_acquire);
    const auto totalStale = producerStale
            > std::numeric_limits<std::uint64_t>::max() - workerStale
        ? std::numeric_limits<std::uint64_t>::max()
        : producerStale + workerStale;
    return {
        state_.load(std::memory_order_acquire),
        activePort_.load(std::memory_order_acquire),
        failedPort_.load(std::memory_order_acquire),
        sentMessages_.load(std::memory_order_acquire),
        encodeFailures_.load(std::memory_order_acquire),
        totalStale,
        queueFull_.load(std::memory_order_acquire),
        sendFailures_.load(std::memory_order_acquire),
        startFailures_.load(std::memory_order_acquire),
        resyncIncomplete_.load(std::memory_order_acquire),
        resyncRequired_.load(std::memory_order_acquire),
    };
}

std::array<WorkbenchOscEgressMonitorRecord, 8>
WorkbenchOscEgress::recentRecords() const noexcept
{
    std::array<WorkbenchOscEgressMonitorRecord, 8> result{};
    for (std::size_t index = 0; index < monitor_.size(); ++index)
    {
        const auto& slot = monitor_[index];
        for (std::size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = slot.publication.load(std::memory_order_acquire);
            if ((before & 1U) != 0)
                continue;
            WorkbenchOscEgressMonitorRecord candidate;
            candidate.sequence = slot.sequence.load(std::memory_order_relaxed);
            candidate.routeGeneration = slot.routeGeneration.load(
                std::memory_order_relaxed);
            candidate.endpointPort = slot.endpointPort.load(
                std::memory_order_relaxed);
            candidate.addressSize = slot.addressSize.load(
                std::memory_order_relaxed);
            if (candidate.addressSize > talas::project::maximumOscAddressBytes)
                continue;
            for (std::size_t byte = 0; byte <= candidate.addressSize; ++byte)
                candidate.address[byte] = slot.address[byte].load(
                    std::memory_order_relaxed);
            candidate.normalizedValue = slot.normalizedValue.load(
                std::memory_order_relaxed);
            candidate.outcome = slot.outcome.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = slot.publication.load(std::memory_order_relaxed);
            if (before == after && (after & 1U) == 0)
            {
                result[index] = candidate;
                break;
            }
        }
    }
    std::ranges::sort(result, {}, &WorkbenchOscEgressMonitorRecord::sequence);
    return result;
}
}
