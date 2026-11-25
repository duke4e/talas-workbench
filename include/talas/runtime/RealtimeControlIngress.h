#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ControllerRecording.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace talas::runtime
{
enum class RealtimeControlTiming : std::uint8_t
{
    NextCallbackOffset,
    ArrivalTimestamp,
    AbsoluteSteadyTimestamp,
};

enum class RealtimeControlCommandKind : std::uint8_t
{
    EventValue,
    VariableAssignment,
    SoundVariableAssignment,
    MidiNoteOn,
    MidiNoteOff,
    MidiProgramChange,
    OscLiveSet,
    MidiClockTick,
    MidiClockStart,
    MidiClockContinue,
    MidiClockStop,
    MidiSongPosition,
    ControllerRecordingObservation,
};

struct RealtimeControlCommand
{
    RealtimeControlCommandKind kind = RealtimeControlCommandKind::EventValue;
    std::uint32_t eventId = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    RealtimeControlTiming timing = RealtimeControlTiming::NextCallbackOffset;
    double arrivalTimeSeconds = 0.0;
    std::int64_t targetTimeNanoseconds = 0;
    std::size_t frameOffset = 0;
    std::uint64_t stageId = 0;
    // Optional exact prepared-graph generation. Zero retains the legacy
    // unbound control API; nonzero commands must match the render target.
    std::uint64_t stageGeneration = 0;
    std::uint64_t voiceGeneration = 0;
    std::uint16_t selectedOrdinal = 0;
    std::uint64_t generation = 1;
    std::uint64_t sequence = 0;
    std::uint8_t midiChannel = 0;
    std::uint8_t midiNote = 0;
    std::uint8_t midiVelocity = 0;
    bool builtInMidiControllerAccepted = false;
    std::uint8_t midiSourceSlot = 0;
    std::uint16_t midiSongPosition = 0;
    std::uint64_t midiSourceGeneration = 0;
    std::int64_t recordingSourceTimestampNanoseconds = 0;
    std::uint64_t recordingRouteGeneration = 0;
    std::uint64_t recordingSourceGeneration = 0;
    std::uint64_t recordingTakeGeneration = 0;
    talas::project::ControllerRecordingOrigin recordingOrigin =
        talas::project::ControllerRecordingOrigin::Ui;
};

class RealtimeControlIngress
{
public:
    static constexpr std::size_t maximumProducerAttempts = 8;

    explicit RealtimeControlIngress(std::size_t capacity);
    ~RealtimeControlIngress();

    RealtimeControlIngress(const RealtimeControlIngress&) = delete;
    RealtimeControlIngress& operator=(const RealtimeControlIngress&) = delete;
    RealtimeControlIngress(RealtimeControlIngress&&) = delete;
    RealtimeControlIngress& operator=(RealtimeControlIngress&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    // Bounded multi-producer publication. Returns immediately when full.
    [[nodiscard]] bool tryPush(const RealtimeControlCommand& command) noexcept;
    // Single-consumer operation.
    [[nodiscard]] bool tryPop(RealtimeControlCommand& command) noexcept;

private:
    struct Slot;

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> enqueuePosition_ = 0;
    std::uint64_t dequeuePosition_ = 0;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
