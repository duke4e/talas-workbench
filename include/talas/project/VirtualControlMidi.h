#pragma once

#include "talas/project/ProjectDocument.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace talas::project
{
inline constexpr std::size_t maximumMidiSourceIdentifierBytes = 512;

struct BuiltInMidiController
{
    std::uint8_t number = 0;
    VirtualControlKind kind = VirtualControlKind::Fader;
};

[[nodiscard]] std::optional<BuiltInMidiController>
parseBuiltInMidiControllerName(std::string_view name) noexcept;

[[nodiscard]] bool isValidMidiSourceIdentifier(
    std::string_view identifier) noexcept;

[[nodiscard]] bool isValidMidiControlMapping(
    const MidiControlMapping& mapping) noexcept;
[[nodiscard]] bool midiControlMappingsConflict(
    const MidiControlMapping& left,
    const MidiControlMapping& right) noexcept;

struct MidiControllerMessage
{
    int channel = 1;
    int controller = 0;
    int value = 0;
    std::uint8_t sourceSlot = 0;
    std::uint64_t sourceGeneration = 0;
};

struct VirtualControlMidiValue
{
    std::string eventName;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
};

struct RealtimeMidiControlValue
{
    std::uint32_t eventId = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
};

enum class MidiLearnDisposition
{
    Ready,
    AlreadyMapped,
    Conflict,
    UnknownTarget,
    InvalidSource
};

struct MidiLearnPlan
{
    MidiLearnDisposition disposition = MidiLearnDisposition::InvalidSource;
    MidiControlMapping mapping;
    std::string requestedEventName;
    std::string requestedTargetIdentity;
    std::string conflictingEventName;
    std::string conflictingTargetIdentity;

    [[nodiscard]] bool ready() const noexcept
    {
        return disposition == MidiLearnDisposition::Ready
            || disposition == MidiLearnDisposition::AlreadyMapped;
    }
};

[[nodiscard]] std::string virtualControlTargetIdentity(
    const VirtualControl& control);

[[nodiscard]] MidiLearnPlan planVirtualControlMidiLearn(
    const ProjectDocument& document,
    std::string_view eventName,
    MidiControlMapping mapping);

struct PreparedMidiControlRoute
{
    MidiControlMapping mapping;
    std::uint32_t eventId = 0;
    VirtualControlKind kind = VirtualControlKind::Fader;
    double minimum = 0.0;
    double maximum = 1.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::uint8_t sourceSlot = 0;
    std::uint64_t sourceGeneration = 0;
};

enum class MidiControlRouteSetDiagnosticCode
{
    None,
    TooManyRoutes,
    InvalidSource,
    DuplicateSource,
    InvalidEventId,
    InvalidKind,
    InvalidDomain,
    InvalidRange
};

struct MidiControlRouteSetResult
{
    MidiControlRouteSetDiagnosticCode code =
        MidiControlRouteSetDiagnosticCode::None;
    std::size_t routeIndex = 0;
    std::size_t conflictingRouteIndex =
        std::numeric_limits<std::size_t>::max();

    [[nodiscard]] bool ok() const noexcept
    {
        return code == MidiControlRouteSetDiagnosticCode::None;
    }
};

class RealtimeMidiRouteAdmissionGate
{
public:
    [[nodiscard]] bool tryBegin() noexcept;
    void end() noexcept;
    void closeAndWait() noexcept;
    void open() noexcept;

private:
    static constexpr std::uint64_t closedBit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t activeMask = closedBit - 1;
    std::atomic<std::uint64_t> state_ = 0;
};

class RealtimeMidiControlRoutes
{
public:
    // Single control-thread writer; any number of realtime readers.
    [[nodiscard]] bool publish(
        MidiControlMapping mapping,
        std::uint32_t eventId,
        VirtualControlKind kind,
        double minimum,
        double maximum,
        talas::expr::ValueDomain domain,
        std::uint8_t sourceSlot = 0,
        std::uint64_t sourceGeneration = 0) noexcept;
    [[nodiscard]] MidiControlRouteSetResult replace(
        std::span<const PreparedMidiControlRoute> routes) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::optional<RealtimeMidiControlValue> map(MidiControllerMessage message) const noexcept;

private:
    static constexpr std::size_t maximumSourceSlots = 16;
    static constexpr std::size_t routeCount = 16 * 128;
    static constexpr std::size_t routeStorageCount =
        (maximumSourceSlots + 1) * routeCount;

    struct Route
    {
        std::atomic<std::uint32_t> eventId = 0;
        std::atomic<double> minimum = 0.0;
        std::atomic<double> maximum = 1.0;
        std::atomic<VirtualControlKind> kind = VirtualControlKind::Fader;
        std::atomic<talas::expr::ValueDomain> domain = talas::expr::ValueDomain::Scalar;
        std::atomic<std::uint64_t> sourceGeneration = 0;
    };

    std::atomic<std::uint64_t> publicationSequence_ = 0;
    std::array<Route, routeStorageCount> routes_{};
    std::array<std::size_t, routeCount> activeRouteIndices_{};
    std::size_t activeRouteCount_ = 0;
    std::array<std::array<char, maximumMidiSourceIdentifierBytes>,
               maximumSourceSlots + 1>
        sourceIdentifierBytes_{};
    std::array<std::uint16_t, maximumSourceSlots + 1>
        sourceIdentifierLengths_{};
};

static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<VirtualControlKind>::is_always_lock_free);
static_assert(std::atomic<talas::expr::ValueDomain>::is_always_lock_free);

[[nodiscard]] std::optional<VirtualControlMidiValue> mapMidiController(
    const ProjectDocument& document,
    MidiControllerMessage message,
    std::string_view sourceIdentifier = {});
}
