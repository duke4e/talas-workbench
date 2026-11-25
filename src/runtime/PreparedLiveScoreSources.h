#pragma once
#include "PreparedSourceCallback.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/AudioStream.h"

#include <limits>
#include <optional>
#include <string_view>

namespace talas::runtime
{
inline constexpr std::string_view liveScoreSourceOperationType = "talas.internalLiveScoreSource";
inline bool requiresLiveScoreSourceAuthority(const RuntimePlan& plan) noexcept
{
    if (plan.requiresLiveScoreSources) return true;
    for (const auto& step : plan.steps)
        if (step.typeId == liveScoreSourceOperationType) return true;
    return false;
}
// Private compiler-owned authority: public RenderSettings cannot provide sources.
struct PreparedLiveScoreSources
{
    struct Source
    {
        std::size_t clipIndex = 0;
        PreparedScoreClipIteration iteration;
        PreparedRuntimeGraph graph;
        std::vector<std::size_t> timelineWaitSteps;
    };
    std::shared_ptr<const PreparedScoreSchedule> schedule;
    std::vector<Source> sources;
    std::shared_ptr<PreparedScoreSourceAnalysisSet> analysisSources;
    RuntimePlan parentPlan;
    RenderSettings parentSettings;
};
std::shared_ptr<PreparedLiveScoreSources> prepareLiveScoreSources(
    const talas::project::ProjectDocument&, const talas::core::NodeRegistry&,
    const RenderSettings&, std::string& error);

// A live note the parent takes at a frame of its
// callback.
struct LiveScoreMidiNote
{
    std::size_t callbackOffset = 0;
    RealtimeMidiNoteEvent event;
};

class PreparedLiveScorePlayback
{
public:
    class OfflineCheckpoint;
    explicit PreparedLiveScorePlayback(std::shared_ptr<const PreparedLiveScoreSources>);
    ~PreparedLiveScorePlayback();
    bool ready() const noexcept { return error_.empty(); }
    std::string_view error() const noexcept { return error_; }
    bool begin(std::uint64_t playhead, std::size_t frames) noexcept;
    // A loop can enter a second source owner after earlier callback frames
    // have rendered. Both owners remain tentative until the parent commits.
    bool beginSegment(std::uint64_t playhead, std::size_t callbackFrames,
        std::size_t callbackOffset) noexcept;
    bool sealSegment(std::size_t callbackEndOffset) noexcept;
    bool prepareTransportPoint(std::uint64_t frame, std::string& error);
    bool canRelocatePrepared(std::uint64_t frame) const noexcept;
    bool relocatePrepared(std::uint64_t frame) noexcept;
    bool canReplaceAtTransportBoundary() const noexcept { return !callbackActive_ && !offline_; }
    // The callback's live notes reach each Sound under the cursor from its
    // first frame in the callback on, on the channels it plays.
    bool renderSegment(RealtimeAudioInputBlock, std::uint64_t playhead, std::size_t frames,
        std::size_t inputOffset, std::size_t outputOffset, bool compositionAdvancing,
        std::span<const LiveScoreMidiNote> notes = {}) noexcept;
    bool hasTimelineGates() const noexcept { return hasTimelineGates_; }
    bool renderTimelineFrame(RealtimeAudioInputBlock, std::uint64_t playhead,
        std::size_t inputOffset, std::size_t outputOffset, std::span<const LiveScoreMidiNote> notes = {}) noexcept;
    std::uint16_t midiChannels() const noexcept { return midiChannels_; }
    // The parent's starting and current value of a
    // global Event Value. A Sound under the cursor follows each live change
    // of one it reads, a MIDI fader's, the surface's or a preset's, from its
    // first frame of the next callback, unless the Timeline automates it
    // there or it is the keyboard's or the tempo's; while the parent's value
    // is the one it started with, the Sound keeps its own.
    struct ParentEventValueState
    {
        double initial = 0.0;
        double current = 0.0;
    };
    using ParentEventValue = std::optional<ParentEventValueState> (*)(
        const void* parent, std::string_view name, talas::expr::ValueDomain domain) noexcept;
    void followParentEventValues(const void* parent, ParentEventValue value) noexcept
    {
        parent_ = parent;
        parentValue_ = value;
    }
    // What the MIDI outputs within the Sounds under
    // the cursor sent during the callback, at its frames.
    std::span<const RealtimeMidiOutputEvent> midiOutputEvents() const noexcept
    { return {midiOutput_.data(), midiOutputCount_}; }
    bool timelineHeld() const noexcept;
    bool resolveTimelineFrame(bool advanceComposition) noexcept;
    void reset() noexcept;
    bool canCommit() const noexcept;
    bool commit() noexcept;
    void rollback() noexcept;
    void collectOfflineAnalysis(RealtimeRenderSession::OfflineTransaction& destination);
    const double* channel(std::size_t clip, std::size_t channel) const noexcept;
    std::unique_ptr<OfflineCheckpoint> checkpoint();
private:
    struct SourceState
    {
        std::unique_ptr<PreparedSourceCallback> callback;
        std::uint64_t position = 0;
        std::size_t pendingFrames = 0;
        std::size_t pendingCompositionFrames = 0;
        std::size_t lastCommittedFrames = 0;
        std::uint16_t midiChannels = 0;
        // How many of its MIDI output's events this callback are taken.
        std::size_t midiTaken = 0;
        std::vector<std::vector<double>> scratch;
        // The parent's Event Values it follows, with the parent's value each
        // last followed, committed and in this callback (NaN: none yet, so
        // the parent's starting value).
        struct FollowedValue
        {
            std::string_view name;
            talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
            double committed = std::numeric_limits<double>::quiet_NaN();
            double pending = std::numeric_limits<double>::quiet_NaN();
        };
        std::vector<FollowedValue> followed;
    };
    void restartFollowedValues() noexcept;
    const void* parent_ = nullptr;
    ParentEventValue parentValue_ = nullptr;
    std::shared_ptr<const PreparedLiveScoreSources> authority_;
    std::vector<SourceState> states_;
    std::vector<std::vector<std::vector<double>>> audio_;
    std::string error_;
    AudioStreamPreparedWindowBudget streamWindowBudget_{8192, 16U * 1024U * 1024U};
    std::optional<std::uint64_t> preparedTransportFrame_;
    std::vector<std::unique_ptr<PreparedSourceCallback::PreparedPoint>> transportPoints_;
    std::vector<std::uint64_t> transportPositions_;
    bool hasBegunRender_ = false;
    bool positioned_ = true;
    bool callbackActive_ = false;
    bool hasTimelineGates_ = false, timelineDecisionPending_ = false;
    std::uint16_t midiChannels_ = 0;
    std::vector<RealtimeMidiOutputEvent> midiOutput_;
    std::size_t midiOutputCount_ = 0;
    bool preparingTimelineFrame_ = false;
    std::size_t callbackFrames_ = 0, renderedFrames_ = 0;
    std::uint64_t expectedPlayhead_ = 0;
    OfflineCheckpoint* offline_ = nullptr;
};
class PreparedLiveScorePlayback::OfflineCheckpoint
{
public:
    explicit OfflineCheckpoint(PreparedLiveScorePlayback&);
    ~OfflineCheckpoint();
    bool commit() noexcept;
    void collect(std::size_t source, std::size_t frames,
        RealtimeRenderSession::OfflineTransaction& destination);
private:
    PreparedLiveScorePlayback& owner_;
    std::vector<std::uint64_t> positions_;
    std::vector<double> followed_;
    std::vector<std::unique_ptr<RealtimeRenderSession::OfflineTransaction>> children_;
    bool positioned_ = true;
    bool committed_ = false;
};
}
