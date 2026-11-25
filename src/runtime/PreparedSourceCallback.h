#pragma once

#include "talas/runtime/RealtimeRenderSession.h"

#include <string_view>
#include <vector>

namespace talas::runtime
{
struct AudioStreamPreparedWindowBudget;
// Internal parent-owned audio source. At most one tentative callback may be
// outstanding. The parent publishes mixed audio only after every source succeeds.
class PreparedSourceCallback
{
public:
    class PreparedPoint
    {
    public:
        ~PreparedPoint();
    private:
        friend class PreparedSourceCallback;
        struct State;
        explicit PreparedPoint(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    explicit PreparedSourceCallback(const PreparedRuntimeGraph& graph);
    ~PreparedSourceCallback();
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    // Holding composition time freezes compiled automation and MIDI file readers.
    // DSP, input and live-event/sample deadlines keep advancing. The held boundary's
    // events are consumed once. Both cursors participate in rollback/reset.
    [[nodiscard]] bool renderTentative(RealtimeAudioInputBlock input,
        double* const* outputs, std::size_t channels, std::size_t frames,
        bool compositionAdvancing = true) noexcept;
    // Append to the open transaction; total frames cannot exceed the prepared
    // block size. Any append failure aborts the complete transaction.
    [[nodiscard]] bool renderNextSegment(RealtimeAudioInputBlock input,
        double* const* outputs, std::size_t channels, std::size_t frames,
        bool compositionAdvancing = true) noexcept;
    // Compiler-owned Timeline binding. Other local gates never report a parent hold.
    bool bindTimelineWaitSteps(std::span<const std::size_t> steps);
    bool renderTimelineFrame(RealtimeAudioInputBlock, double* const*, std::size_t channels) noexcept;
    bool timelineDecisionPending() const noexcept { return timelineDecisionPending_; }
    bool timelineHeld() const noexcept;
    bool resolveTimelineFrame(bool advanceComposition) noexcept;
    std::unique_ptr<RealtimeRenderSession::OfflineTransaction> beginOfflineTransaction();
    // Control-thread-only preparation at the post-offset source cursor.
    bool prepareStreamWindows(AudioStreamPreparedWindowBudget&);
    // Control-thread capture; audio-thread restore, with no replay or allocation.
    bool prepareResetPoint();
    bool resetToPreparedPoint() noexcept;
    bool canResetToPreparedPoint() const noexcept;
    // Exclusive control-thread capture. Points retain exact source identity and
    // may outlive it; they cannot be adopted by another instance of the graph.
    std::unique_ptr<PreparedPoint> capturePreparedPoint();
    // Audio-thread adoption; rejects foreign points and outstanding callbacks.
    bool restorePreparedPoint(PreparedPoint&) noexcept;
    bool canRestorePreparedPoint(const PreparedPoint&) const noexcept;
    // The channels, a bit each from channel 1, on
    // which the source's MIDIVoices and MIDIMappers play live notes, and
    // its default channel when it reads the keyboard's !KeyDown, !KeyPitch
    // or !KeyVelocity, which follow the last key down there.
    [[nodiscard]] std::uint16_t midiChannels() const noexcept;
    // What the MIDI output within the source's Sound
    // has sent since its tentative callback began, at frames from its start.
    [[nodiscard]] std::span<const RealtimeMidiOutputEvent> midiOutputEvents() const noexcept;
    // Audio-thread only, before the callback's first render: a live note at
    // a frame of that callback. A note the callback does not reach ends
    // with it, at its commit or rollback.
    bool scheduleMidiNote(RealtimeMidiNoteEvent, std::size_t frameOffset) noexcept;
    // The global Event Values the source reads, at
    // their starting values; and, audio-thread only before the callback's
    // first render, a live change of one of them at a frame of that
    // callback, as a note is scheduled.
    [[nodiscard]] std::vector<RealtimeEventValueUpdate> globalEventValues() const;
    bool scheduleEventValue(std::string_view name, double value, talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    void commit() noexcept;
    void rollback() noexcept;
    [[nodiscard]] bool pending() const noexcept;
private:
    std::shared_ptr<const std::uint8_t> pointOwner_ = std::make_shared<const std::uint8_t>(0);
    std::unique_ptr<RealtimeRenderSession> session_;
    bool pending_ = false;
    bool timelineDecisionPending_ = false;
    std::vector<std::size_t> timelineWaitSteps_;
    std::uint8_t defaultMidiChannel_ = 1;
    bool readsKeyDown_ = false, readsPitch_ = false, readsKeyVelocity_ = false, readsKeys_ = false;
    // The key the keyboard's values follow, committed and in this callback.
    int key_ = -1, pendingKey_ = -1;
};
}
