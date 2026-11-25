#pragma once

#include "talas/project/ProjectLiveSets.h"

#include <array>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace talas::runtime
{
inline constexpr std::size_t maximumRealtimeLiveSetIngressCapacity = 1024;

enum class RealtimeLiveSetTargetKind : std::uint8_t
{
    Sound,
    Snapshot,
    ScoreRegion,
    TrackMute,
    Stop,
};

enum class RealtimeLiveSetSourceKind : std::uint8_t
{
    Workbench,
    MidiProgram,
    Osc,
    Tool,
    Offline,
};

struct RealtimeLiveSetRequest
{
    RealtimeLiveSetTargetKind targetKind =
        RealtimeLiveSetTargetKind::Stop;
    RealtimeLiveSetSourceKind sourceKind =
        RealtimeLiveSetSourceKind::Workbench;
    std::uint16_t preparedCellIndex = 0;
    std::uint64_t resolvedFrame = 0;
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::uint64_t transportEpoch = 0;
    std::uint64_t transportDiscontinuitySequence = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t routeGeneration = 0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t ingressSequence = 0;

    bool operator==(const RealtimeLiveSetRequest&) const = default;
};

class RealtimeLiveSetIngress
{
public:
    static constexpr std::size_t maximumProducerAttempts = 8;

    explicit RealtimeLiveSetIngress(std::size_t capacity);
    ~RealtimeLiveSetIngress();

    RealtimeLiveSetIngress(const RealtimeLiveSetIngress&) = delete;
    RealtimeLiveSetIngress& operator=(const RealtimeLiveSetIngress&) = delete;
    RealtimeLiveSetIngress(RealtimeLiveSetIngress&&) = delete;
    RealtimeLiveSetIngress& operator=(RealtimeLiveSetIngress&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool tryPush(
        const RealtimeLiveSetRequest& request) noexcept;
    [[nodiscard]] bool tryPop(
        RealtimeLiveSetRequest& request) noexcept;

private:
    struct Slot;

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> enqueuePosition_ = 0;
    std::uint64_t dequeuePosition_ = 0;
};

class RealtimeLiveSetSourceAuthorityPublication
{
public:
    void publish(std::uint64_t sourceGeneration,
                 std::uint64_t routeGeneration) noexcept;
    [[nodiscard]] bool read(std::uint64_t& sourceGeneration,
                            std::uint64_t& routeGeneration) const noexcept;
    void clear() noexcept;

private:
    std::atomic<std::uint64_t> sourceGeneration_ = 0;
    std::atomic<std::uint64_t> routeGeneration_ = 0;
    std::atomic<std::uint64_t> publicationGeneration_ = 0;
};

struct RealtimeLiveSetActiveState
{
    std::uint64_t requestGeneration = 0;
    std::uint32_t cellIndexPlusOne = 0;
    bool scoreRegionActive = false;
    std::array<std::uint32_t, talas::project::projectLiveSetColumns>
        soundCellIndexPlusOneByColumn{};
    std::uint32_t mutedSoundColumnsMask = 0;

    bool operator==(const RealtimeLiveSetActiveState&) const = default;
};

class RealtimeLiveSetActiveStatePublication
{
public:
    void publish(RealtimeLiveSetActiveState state) noexcept;
    [[nodiscard]] bool read(RealtimeLiveSetActiveState& state) const noexcept;
    void clear() noexcept;

private:
    std::atomic<std::uint64_t> requestGeneration_ = 0;
    std::atomic<std::uint64_t> cellIndexPlusOne_ = 0;
    std::atomic<std::uint64_t> scoreRegionState_ = 0;
    std::array<std::atomic<std::uint32_t>,
        talas::project::projectLiveSetColumns>
        soundCellIndexPlusOneByColumn_{};
    std::atomic<std::uint32_t> mutedSoundColumnsMask_ = 0;
    std::atomic<std::uint64_t> publicationGeneration_ = 0;
};

enum class RealtimeLiveSetTerminalOutcome : std::uint8_t
{
    None,
    Completed,
    StaleAuthority,
    Failed,
};

struct RealtimeLiveSetTerminalState
{
    std::uint64_t requestGeneration = 0;
    RealtimeLiveSetTerminalOutcome outcome =
        RealtimeLiveSetTerminalOutcome::None;

    bool operator==(const RealtimeLiveSetTerminalState&) const = default;
};

class RealtimeLiveSetTerminalStatePublication
{
public:
    void publish(RealtimeLiveSetTerminalState state) noexcept;
    [[nodiscard]] bool read(
        RealtimeLiveSetTerminalState& state) const noexcept;

private:
    std::atomic<std::uint64_t> requestGeneration_ = 0;
    std::atomic<std::uint8_t> outcome_ = 0;
    std::atomic<std::uint64_t> publicationGeneration_ = 0;
};

struct RealtimeLiveSetAuthority
{
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::uint64_t transportEpoch = 0;
    std::uint64_t transportDiscontinuitySequence = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t routeGeneration = 0;
    std::array<std::uint64_t, 5> sourceGenerations{};
    std::array<std::uint64_t, 5> routeGenerations{};
};

[[nodiscard]] bool realtimeLiveSetRequestMatchesAuthority(
    const RealtimeLiveSetRequest& request,
    const RealtimeLiveSetAuthority& authority) noexcept;

class RealtimeLiveSetScheduler
{
public:
    explicit RealtimeLiveSetScheduler(std::size_t capacity);
    ~RealtimeLiveSetScheduler();

    RealtimeLiveSetScheduler(const RealtimeLiveSetScheduler&) = delete;
    RealtimeLiveSetScheduler& operator=(const RealtimeLiveSetScheduler&) = delete;
    RealtimeLiveSetScheduler(RealtimeLiveSetScheduler&&) = delete;
    RealtimeLiveSetScheduler& operator=(RealtimeLiveSetScheduler&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool schedule(
        const RealtimeLiveSetRequest& request) noexcept;
    [[nodiscard]] std::optional<RealtimeLiveSetRequest> takeNextInRange(
        std::uint64_t callbackStart,
        std::uint64_t callbackEndExclusive) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> nextFrame() const noexcept;
    [[nodiscard]] std::size_t discardBefore(
        std::uint64_t frame,
        std::uint64_t* latestDiscardedRequestGeneration = nullptr) noexcept;
    [[nodiscard]] std::size_t discardStale(
        const RealtimeLiveSetAuthority& authority,
        std::uint64_t* latestDiscardedRequestGeneration = nullptr) noexcept;
    void clear(
        std::uint64_t* latestDiscardedRequestGeneration = nullptr) noexcept;

private:
    std::unique_ptr<RealtimeLiveSetRequest[]> pending_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
