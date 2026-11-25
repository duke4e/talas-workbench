#pragma once

#include "app/workbench/WorkbenchAuthorityGeneration.h"
#include "talas/project/VirtualControlOsc.h"
#include "talas/runtime/OscProtocol.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <string>
#include <thread>

namespace juce
{
class DatagramSocket;
}

namespace talas::app::workbench
{
enum class WorkbenchOscIngressState : std::uint8_t
{
    Stopped,
    Listening,
    BindFailed,
};

enum class WorkbenchOscMonitorOutcome : std::uint8_t
{
    Accepted,
    Scheduled,
    LateAccepted,
    Unmapped,
    Malformed,
    UnsupportedTimetag,
    RegressingTimetag,
    TimetagTooLate,
    TimetagTooFar,
    TimetagConversionFailed,
    RouteStale,
    QueueFull,
    ScheduleFull,
};

struct WorkbenchOscMonitorRecord
{
    std::uint64_t sequence = 0;
    std::uint64_t routeGeneration = 0;
    std::uint16_t endpointPort = 0;
    std::array<char, talas::project::maximumOscAddressBytes + 1> address{};
    std::uint16_t addressSize = 0;
    double normalizedValue = 0.0;
    WorkbenchOscMonitorOutcome outcome =
        WorkbenchOscMonitorOutcome::Malformed;
};

struct WorkbenchOscIngressTelemetry
{
    WorkbenchOscIngressState state = WorkbenchOscIngressState::Stopped;
    std::uint16_t activePort = 0;
    std::uint16_t failedPort = 0;
    std::uint64_t receivedPackets = 0;
    std::uint64_t acceptedMessages = 0;
    std::uint64_t unmappedMessages = 0;
    std::uint64_t malformedPackets = 0;
    std::uint64_t unsupportedTimetags = 0;
    std::uint64_t regressingTimetags = 0;
    std::uint64_t scheduledMessages = 0;
    std::uint64_t lateMessages = 0;
    std::uint64_t scheduledLateApplied = 0;
    std::uint64_t timetagsTooLate = 0;
    std::uint64_t timetagsTooFar = 0;
    std::uint64_t timetagConversionFailures = 0;
    std::uint64_t scheduledApplied = 0;
    std::uint64_t scheduledStale = 0;
    std::uint64_t scheduledPending = 0;
    std::uint64_t scheduledPendingHighWatermark = 0;
    std::uint64_t staleRoutes = 0;
    std::uint64_t queueFull = 0;
    std::uint64_t scheduleFull = 0;
    std::uint64_t bindFailures = 0;

    friend bool operator==(
        const WorkbenchOscIngressTelemetry&,
        const WorkbenchOscIngressTelemetry&) = default;
};

struct WorkbenchOscConfigureResult
{
    bool accepted = false;
    WorkbenchOscIngressState state = WorkbenchOscIngressState::Stopped;
    talas::project::OscControlRouteSetResult routes;
    std::string message;
};

class WorkbenchOscIngress final
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
        friend class WorkbenchOscIngress;

        talas::project::ProjectOscIngressSettings settings_;
        talas::project::RealtimeOscControlRoutes routes_;
        std::vector<std::string> liveGridAddresses_;
        std::unique_ptr<juce::DatagramSocket> socket_;
        std::shared_ptr<std::atomic<std::uint8_t>> workerStart_;
        std::jthread worker_;
        bool reuseEndpoint_ = false;
    };

    struct Preparation
    {
        WorkbenchOscConfigureResult result;
        std::optional<Candidate> candidate;
    };

    explicit WorkbenchOscIngress(
        talas::runtime::ProjectRuntimeBridge& bridge,
        talas::runtime::RealtimeControlIngress* performanceIngress = nullptr)
        noexcept;
    ~WorkbenchOscIngress();

    WorkbenchOscIngress(const WorkbenchOscIngress&) = delete;
    WorkbenchOscIngress& operator=(const WorkbenchOscIngress&) = delete;

    [[nodiscard]] WorkbenchOscConfigureResult configure(
        talas::project::ProjectOscIngressSettings settings,
        std::span<const talas::project::PreparedOscControlRoute> routes,
        std::span<const std::string> liveGridAddresses = {});
    [[nodiscard]] Preparation prepareCandidate(
        talas::project::ProjectOscIngressSettings settings,
        std::span<const talas::project::PreparedOscControlRoute> routes,
        std::span<const std::string> liveGridAddresses = {});
    using LiveGridCallback = std::function<bool(
        std::uint16_t routeIndex,
        std::uint64_t routeGeneration)>;
    void setLiveGridCallback(LiveGridCallback callback);
    void suspend() noexcept;
    [[nodiscard]] bool commitCandidate(Candidate&& candidate) noexcept;
    [[nodiscard]] bool resume() noexcept;
    void stop() noexcept;

    [[nodiscard]] WorkbenchOscIngressTelemetry telemetry() const noexcept;
    [[nodiscard]] std::array<WorkbenchOscMonitorRecord, 8>
    recentRecords() const noexcept;
    [[nodiscard]] std::uint64_t routeGeneration() const noexcept
    {
        return currentWorkbenchAuthorityGeneration(routeGeneration_);
    }

#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
    void failNextWorkerStartForTesting() noexcept
    {
        failNextWorkerStartForTesting_.store(true, std::memory_order_release);
    }
    void pauseAfterReadForTesting(bool pause) noexcept
    {
        pauseAfterReadForTesting_.store(pause, std::memory_order_release);
    }
    [[nodiscard]] bool readPausedForTesting() const noexcept
    {
        return readPausedForTesting_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t routeGenerationForTesting() const noexcept
    {
        return routeGeneration_.load(std::memory_order_acquire);
    }
    void seedRouteGenerationForTesting(std::uint64_t generation) noexcept
    {
        routeGeneration_.store(generation, std::memory_order_release);
    }
#endif

private:
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
        std::atomic<WorkbenchOscMonitorOutcome> outcome =
            WorkbenchOscMonitorOutcome::Malformed;
    };

    static void incrementSaturating(
        std::atomic<std::uint64_t>& counter) noexcept;
    void run(std::stop_token stopToken) noexcept;
    void publishMonitor(
        std::string_view address,
        double normalizedValue,
        WorkbenchOscMonitorOutcome outcome,
        std::uint16_t endpointPort,
        std::uint64_t routeGeneration) noexcept;
    void stopLocked(bool clearRoutes) noexcept;
    [[nodiscard]] bool discardPendingPacketsLocked() noexcept;

    talas::runtime::ProjectRuntimeBridge& bridge_;
    talas::runtime::RealtimeControlIngress* performanceIngress_ = nullptr;
    talas::project::RealtimeOscRouteAdmissionGate admissionGate_;
    talas::project::RealtimeOscControlRoutes routes_;
    std::vector<std::string> liveGridAddresses_;
    LiveGridCallback liveGridCallback_;
    std::unique_ptr<juce::DatagramSocket> socket_;
    std::shared_ptr<std::atomic<std::uint8_t>> workerStart_;
    std::jthread worker_;
    bool suspended_ = false;
    std::atomic<WorkbenchOscIngressState> state_ =
        WorkbenchOscIngressState::Stopped;
    std::atomic<std::uint16_t> activePort_ = 0;
    std::atomic<std::uint16_t> failedPort_ = 0;
    std::atomic<std::uint64_t> routeGeneration_ = 1;
    std::atomic<std::uint64_t> receivedPackets_ = 0;
    std::atomic<std::uint64_t> acceptedMessages_ = 0;
    std::atomic<std::uint64_t> unmappedMessages_ = 0;
    std::atomic<std::uint64_t> malformedPackets_ = 0;
    std::atomic<std::uint64_t> unsupportedTimetags_ = 0;
    std::atomic<std::uint64_t> regressingTimetags_ = 0;
    std::atomic<std::uint64_t> scheduledMessages_ = 0;
    std::atomic<std::uint64_t> lateMessages_ = 0;
    std::atomic<std::uint64_t> timetagsTooLate_ = 0;
    std::atomic<std::uint64_t> timetagsTooFar_ = 0;
    std::atomic<std::uint64_t> timetagConversionFailures_ = 0;
    std::atomic<std::uint64_t> staleRoutes_ = 0;
    std::atomic<std::uint64_t> queueFull_ = 0;
    std::atomic<std::uint64_t> scheduleFull_ = 0;
    std::atomic<std::uint64_t> bindFailures_ = 0;
    std::array<MonitorSlot, 8> monitor_{};
    std::atomic<std::uint64_t> nextMonitorSequence_ = 1;
#if defined(TALAS_WORKBENCH_OSC_TEST_SUPPORT)
    std::atomic<bool> failNextWorkerStartForTesting_ = false;
    std::atomic<bool> pauseAfterReadForTesting_ = false;
    std::atomic<bool> readPausedForTesting_ = false;
#endif
};
}
