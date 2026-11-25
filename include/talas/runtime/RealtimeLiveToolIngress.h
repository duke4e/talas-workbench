#pragma once

#include "talas/project/ProjectLiveSets.h"
#include "talas/project/VirtualControlMidi.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace talas::runtime
{
enum class RealtimeLiveToolSourceKind : std::uint8_t
{
    Workbench,
    EventValue,
    Variable,
    MidiProgram,
    Osc,
    Periodic,
    Offline,
};

inline constexpr std::size_t realtimeLiveToolSourceKindCount = 7;
inline constexpr std::size_t maximumRealtimeLiveToolIdentifierBytes =
    talas::project::maximumMidiSourceIdentifierBytes;

struct RealtimeLiveToolTriggerRequest
{
    talas::project::ProjectLiveToolTriggerKind triggerKind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    RealtimeLiveToolSourceKind sourceKind =
        RealtimeLiveToolSourceKind::Workbench;
    std::uint16_t preparedToolIndex = 0;
    std::array<char, maximumRealtimeLiveToolIdentifierBytes + 1>
        identifier{};
    std::uint16_t identifierSize = 0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double variableNumber = 0.0;
    bool variableBoolean = false;
    bool hasVariableValue = false;
    std::int32_t midiChannel = 1;
    std::int32_t midiProgram = 0;
    std::uint64_t musicalTick = 0;
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t routeGeneration = 0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t ingressSequence = 0;

    [[nodiscard]] std::string_view identifierView() const noexcept
    {
        return {identifier.data(), identifierSize};
    }

    bool operator==(const RealtimeLiveToolTriggerRequest&) const = default;
};

[[nodiscard]] bool setRealtimeLiveToolTriggerIdentifier(
    RealtimeLiveToolTriggerRequest& request,
    std::string_view identifier) noexcept;

struct RealtimeLiveToolAuthority
{
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::array<std::uint64_t, realtimeLiveToolSourceKindCount>
        sourceGenerations{};
    std::array<std::uint64_t, realtimeLiveToolSourceKindCount>
        routeGenerations{};
};

[[nodiscard]] bool realtimeLiveToolTriggerMatchesAuthority(
    const RealtimeLiveToolTriggerRequest& request,
    const RealtimeLiveToolAuthority& authority) noexcept;

class RealtimeLiveToolIngress
{
public:
    static constexpr std::size_t maximumProducerAttempts = 8;

    explicit RealtimeLiveToolIngress(std::size_t capacity);
    ~RealtimeLiveToolIngress();

    RealtimeLiveToolIngress(const RealtimeLiveToolIngress&) = delete;
    RealtimeLiveToolIngress& operator=(const RealtimeLiveToolIngress&) = delete;
    RealtimeLiveToolIngress(RealtimeLiveToolIngress&&) = delete;
    RealtimeLiveToolIngress& operator=(RealtimeLiveToolIngress&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] bool tryPush(
        const RealtimeLiveToolTriggerRequest& request) noexcept;
    [[nodiscard]] bool tryPop(
        RealtimeLiveToolTriggerRequest& request) noexcept;

private:
    struct Slot;

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> enqueuePosition_ = 0;
    std::uint64_t dequeuePosition_ = 0;
};

class RealtimeLiveToolSourceAuthorityPublication
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

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
