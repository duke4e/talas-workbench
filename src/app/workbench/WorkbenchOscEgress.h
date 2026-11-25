#pragma once

#include "talas/project/VirtualControlOsc.h"
#include "talas/runtime/OscProtocol.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace juce
{
class DatagramSocket;
}

namespace talas::app::workbench
{
inline constexpr std::size_t maximumOscEgressQueueRecords = 256;

enum class WorkbenchOscEgressState : std::uint8_t
{
    Stopped,
    Ready,
    StartFailed,
};

enum class WorkbenchOscEgressOutcome : std::uint8_t
{
    Sent,
    EncodeFailed,
    RouteStale,
    QueueFull,
    SendFailed,
    ResyncIncomplete,
};

struct PreparedWorkbenchOscEgressRoute
{
    talas::project::OscEgressMapping mapping;
    std::string eventName;
    talas::project::VirtualControlKind kind =
        talas::project::VirtualControlKind::Fader;
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 1.0;
    std::uint64_t stageGeneration = 0;
};

struct WorkbenchOscEgressMonitorRecord
{
    std::uint64_t sequence = 0;
    std::uint64_t routeGeneration = 0;
    std::uint16_t endpointPort = 0;
    std::array<char, talas::project::maximumOscAddressBytes + 1> address{};
    std::uint16_t addressSize = 0;
    double normalizedValue = 0.0;
    WorkbenchOscEgressOutcome outcome =
        WorkbenchOscEgressOutcome::EncodeFailed;
};

struct WorkbenchOscEgressTelemetry
{
    WorkbenchOscEgressState state = WorkbenchOscEgressState::Stopped;
    std::uint16_t activePort = 0;
    std::uint16_t failedPort = 0;
    std::uint64_t sentMessages = 0;
    std::uint64_t encodeFailures = 0;
    std::uint64_t staleRoutes = 0;
    std::uint64_t queueFull = 0;
    std::uint64_t sendFailures = 0;
    std::uint64_t startFailures = 0;
    std::uint64_t resyncIncomplete = 0;
    bool resyncRequired = false;

    friend bool operator==(
        const WorkbenchOscEgressTelemetry&,
        const WorkbenchOscEgressTelemetry&) = default;
};

struct WorkbenchOscEgressConfigureResult
{
    bool accepted = false;
    WorkbenchOscEgressState state = WorkbenchOscEgressState::Stopped;
    std::string message;
};

class WorkbenchOscEgress final
{
public:
    class Candidate final
    {
    public:
        Candidate();
        ~Candidate();
        Candidate(Candidate&&) noexcept;
        Candidate& operator=(Candidate&&) noexcept;

        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;

    private:
        friend class WorkbenchOscEgress;

        talas::project::ProjectOscEgressSettings settings_;
        std::vector<PreparedWorkbenchOscEgressRoute> routes_;
        std::unique_ptr<juce::DatagramSocket> socket_;
        std::shared_ptr<std::atomic<std::uint8_t>> workerStart_;
        std::jthread worker_;
    };

    struct Preparation
    {
        WorkbenchOscEgressConfigureResult result;
        std::optional<Candidate> candidate;
    };

    WorkbenchOscEgress() noexcept = default;
    ~WorkbenchOscEgress();

    WorkbenchOscEgress(const WorkbenchOscEgress&) = delete;
    WorkbenchOscEgress& operator=(const WorkbenchOscEgress&) = delete;

    // Configuration, publication, and resync are message-thread operations.
    // The worker owns socket I/O; telemetry readers may run concurrently.

    [[nodiscard]] WorkbenchOscEgressConfigureResult configure(
        talas::project::ProjectOscEgressSettings settings,
        std::span<const PreparedWorkbenchOscEgressRoute> routes);
    [[nodiscard]] Preparation prepareCandidate(
        talas::project::ProjectOscEgressSettings settings,
        std::span<const PreparedWorkbenchOscEgressRoute> routes);
    void suspend() noexcept;
    void commitCandidate(Candidate&& candidate) noexcept;
    void resume() noexcept;
    void invalidateRoutes() noexcept;
    void stop() noexcept;

    [[nodiscard]] bool publish(
        std::string_view eventName,
        double persistedValue,
        std::uint64_t stageGeneration) noexcept;
    [[nodiscard]] bool resync() noexcept;

    [[nodiscard]] WorkbenchOscEgressTelemetry telemetry() const noexcept;
    [[nodiscard]] std::array<WorkbenchOscEgressMonitorRecord, 8>
    recentRecords() const noexcept;
    [[nodiscard]] std::uint64_t routeGeneration() const noexcept
    {
        return routeGeneration_.load(std::memory_order_acquire);
    }

#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
    void failNextWorkerStartForTesting() noexcept
    {
        failNextWorkerStartForTesting_.store(true, std::memory_order_release);
    }
    void failNextSendForTesting() noexcept
    {
        failNextSendForTesting_.store(true, std::memory_order_release);
    }
    void pauseWorkerForTesting(bool pause) noexcept
    {
        pauseWorkerForTesting_.store(pause, std::memory_order_release);
    }
    [[nodiscard]] bool workerPausedForTesting() const noexcept
    {
        return workerPausedForTesting_.load(std::memory_order_acquire);
    }
    void pauseBeforeSendForTesting(bool pause) noexcept
    {
        pauseBeforeSendForTesting_.store(pause, std::memory_order_release);
    }
    [[nodiscard]] bool workerBeforeSendForTesting() const noexcept
    {
        return workerBeforeSendForTesting_.load(std::memory_order_acquire);
    }
#endif

private:
    struct PacketRecord
    {
        std::uint64_t routeGeneration = 0;
        std::uint16_t endpointPort = 0;
        std::array<std::uint8_t,
                   talas::runtime::maximumOscControlMessageBytes>
            packet{};
        std::uint16_t packetSize = 0;
        std::array<char, talas::project::maximumOscAddressBytes + 1> address{};
        std::uint16_t addressSize = 0;
        double normalizedValue = 0.0;
    };

    struct MonitorSlot
    {
        std::atomic<std::uint64_t> publication = 0;
        std::atomic<std::uint64_t> sequence = 0;
        std::atomic<std::uint64_t> routeGeneration = 0;
        std::atomic<std::uint16_t> endpointPort = 0;
        std::array<std::atomic<char>,
                   talas::project::maximumOscAddressBytes + 1>
            address{};
        std::atomic<std::uint16_t> addressSize = 0;
        std::atomic<double> normalizedValue = 0.0;
        std::atomic<WorkbenchOscEgressOutcome> outcome =
            WorkbenchOscEgressOutcome::EncodeFailed;
    };

    [[nodiscard]] bool enqueue(
        const PreparedWorkbenchOscEgressRoute& route,
        double persistedValue,
        std::uint64_t routeGeneration) noexcept;
    void run(std::stop_token stopToken) noexcept;
    void stopWorker() noexcept;
    void clearQueue() noexcept;
    void publishMonitor(
        const PacketRecord& record,
        WorkbenchOscEgressOutcome outcome) noexcept;
    static void incrementSaturating(
        std::atomic<std::uint64_t>& counter) noexcept;

    std::vector<PreparedWorkbenchOscEgressRoute> routes_;
    std::unique_ptr<juce::DatagramSocket> socket_;
    std::shared_ptr<std::atomic<std::uint8_t>> workerStart_;
    std::jthread worker_;
    std::mutex workerAdmissionGate_;
    bool workerSuspended_ = false;
    std::atomic<std::uint32_t> activeWorkerOperations_ = 0;
    std::array<PacketRecord, maximumOscEgressQueueRecords> queue_{};
    std::atomic<std::uint64_t> queueHead_ = 0;
    std::atomic<std::uint64_t> queueTail_ = 0;
    std::atomic<WorkbenchOscEgressState> state_ =
        WorkbenchOscEgressState::Stopped;
    std::atomic<std::uint16_t> activePort_ = 0;
    std::atomic<std::uint16_t> failedPort_ = 0;
    std::atomic<std::uint64_t> routeGeneration_ = 1;
    std::atomic<std::uint64_t> sentMessages_ = 0;
    std::atomic<std::uint64_t> encodeFailures_ = 0;
    std::atomic<std::uint64_t> staleRoutes_ = 0;
    std::atomic<std::uint64_t> workerStaleRoutes_ = 0;
    std::atomic<std::uint64_t> queueFull_ = 0;
    std::atomic<std::uint64_t> sendFailures_ = 0;
    std::atomic<std::uint64_t> startFailures_ = 0;
    std::atomic<std::uint64_t> resyncIncomplete_ = 0;
    std::atomic<bool> resyncRequired_ = false;
    std::array<MonitorSlot, 8> monitor_{};
    std::atomic<std::uint64_t> nextMonitorSequence_ = 1;
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
    std::atomic<bool> failNextWorkerStartForTesting_ = false;
    std::atomic<bool> failNextSendForTesting_ = false;
    std::atomic<bool> pauseWorkerForTesting_ = false;
    std::atomic<bool> workerPausedForTesting_ = false;
    std::atomic<bool> pauseBeforeSendForTesting_ = false;
    std::atomic<bool> workerBeforeSendForTesting_ = false;
#endif
};
}
