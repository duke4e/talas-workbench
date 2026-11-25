#pragma once

#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/project/ProjectLiveSets.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <span>
#include <vector>

namespace talas::runtime
{
class PreparedLiveSet;
class PreparedLiveTools;
class PreparedLiveSetRuntime;
class RealtimeAudioEngine
{
public:
    class PreparedTransportRelocation
    {
        friend class RealtimeAudioEngine;
        std::unique_ptr<RealtimeRenderSession::PreparedTransportRelocation> root;
        std::vector<std::unique_ptr<PreparedTransportRelocation>> cells;
    public:
        PreparedTransportRelocation() = default;
    };
    [[nodiscard]] std::unique_ptr<PreparedTransportRelocation> prepareTransportRelocation(
        std::uint64_t frame, std::uint64_t& remainingPreparationFrames, std::string& error) const;
    // Audio-boundary installation; the host applies the matching transport frame
    // afterward. Consumed candidates must be retired on the control thread.
    [[nodiscard]] bool canInstallTransportRelocation(const PreparedTransportRelocation&) const noexcept;
    [[nodiscard]] bool installTransportRelocation(PreparedTransportRelocation&) noexcept;

    explicit RealtimeAudioEngine(const PreparedRuntimeGraph& graph);

    ~RealtimeAudioEngine();

    RealtimeAudioEngine(const RealtimeAudioEngine&) = delete;
    RealtimeAudioEngine& operator=(const RealtimeAudioEngine&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    [[nodiscard]] std::size_t maxBlockFrames() const noexcept;
    [[nodiscard]] std::size_t outputChannelCount() const noexcept;
    // What each DiskRecorder within the Sound
    // recorded during this engine's last render of its own Sound, one or
    // two float channels of diskRecorderTapFrames() frames; no frames after
    // a failed render or one of a Live set.
    [[nodiscard]] std::size_t diskRecorderTapCount() const noexcept;
    [[nodiscard]] std::size_t diskRecorderTapFrames() const noexcept;
    [[nodiscard]] std::span<float* const> diskRecorderTapChannels(std::size_t tap) const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedScoreSchedule>
    preparedScoreSchedule() const noexcept;
    [[nodiscard]] bool setPreparedLiveSet(std::shared_ptr<const PreparedLiveSet> prepared);
    [[nodiscard]] const PreparedLiveSet* preparedLiveSet() const noexcept;
    [[nodiscard]] bool setPreparedLiveTools(
        std::shared_ptr<const PreparedLiveTools> prepared) noexcept;
    [[nodiscard]] const PreparedLiveTools* preparedLiveTools() const noexcept;
    [[nodiscard]] bool activatePreparedLiveSetProgram(std::size_t cellIndex,
                                                      std::uint64_t absoluteFrame,
                                                      std::uint64_t transitionFrames = 0) noexcept;
    [[nodiscard]] std::uint32_t activePreparedLiveSetCellIndexPlusOne() const noexcept;
    [[nodiscard]] std::uint32_t activePreparedLiveSetSoundCellIndexPlusOne(
        std::uint32_t column) const noexcept;
    [[nodiscard]] std::size_t activePreparedLiveSetSoundCount() const noexcept;
    [[nodiscard]] bool setPreparedLiveSetSoundColumnMuted(
        std::uint32_t column, bool muted) noexcept;
    [[nodiscard]] bool preparedLiveSetSoundColumnMuted(
        std::uint32_t column) const noexcept;
    [[nodiscard]] bool preparedLiveSetActivationPending() const noexcept;
    void deactivatePreparedLiveSetProgram(std::uint64_t absoluteFrame) noexcept;
    [[nodiscard]] std::optional<TransportLoopRegion> transportLoop() const noexcept;
    // Source history belongs to one prepared absolute transport frame, so a
    // replacement may become audible only exactly there.
    void setPreparedStageTransportFrame(std::uint64_t absoluteFrame) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> preparedStageTransportFrame() const noexcept;
    [[nodiscard]] bool canAdoptAtTransportFrame(std::uint64_t absoluteFrame) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> midiSongPositionFrame(
        std::uint16_t songPosition) const noexcept;
    // The Timeline's composition cursor, held while a WaitUntil within it
    // waits; none without a prepared Score.
    [[nodiscard]] std::optional<std::uint64_t> preparedScorePlaybackFrame() const noexcept;
    [[nodiscard]] std::optional<talas::project::MusicalPosition>
    musicalPositionAtFrame(std::uint64_t frame) const noexcept;
    [[nodiscard]] std::optional<talas::project::MusicalPosition>
    musicalPositionAtTick(std::uint64_t tick) const noexcept;
    [[nodiscard]] RealtimeStreamHealthSnapshot streamHealth() const;
    [[nodiscard]] PreparedNodeTimingSnapshot preparedNodeTiming() const;
    [[nodiscard]] PreparedVariableValueSnapshot preparedVariableValues() const;
    [[nodiscard]] PreparedSoundVariableValueSnapshot preparedSoundVariableValues() const;
    [[nodiscard]] bool performanceStateCompatibleWith(
        const RealtimeAudioEngine& source) const noexcept;
    [[nodiscard]] bool transferPerformanceStateFrom(
        const RealtimeAudioEngine& source) noexcept;
    void setStageId(std::uint64_t stageId) noexcept;
    [[nodiscard]] std::uint64_t stageId() const noexcept;
    void setSnapshotGeneration(std::uint64_t generation) noexcept;
    [[nodiscard]] std::uint64_t snapshotGeneration() const noexcept;
    // Audio-thread only, before each render callback.
    void setTransportSnapshotBeforeRender(RealtimeTransportSnapshot snapshot) noexcept;
    // Audio-thread only, before the engine's first render.
    // An interior live Score position requires a prepared relocation candidate
    // installed at this boundary; this setter never reconstructs source history.
    void setTransportFrameBeforeFirstRender(std::uint64_t frame) noexcept;
    void synchronizeTransportFrame(std::uint64_t frame) noexcept;
    void resetTransport() noexcept;

    void setPlaying(bool playing) noexcept;
    [[nodiscard]] bool updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain);
    // Single-producer only across both scheduling APIs.
    [[nodiscard]] bool scheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] bool scheduleEventValues(
        std::span<const RealtimeEventValueUpdate> values,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleSnapshotDirectTransaction(
        const SnapshotDirectTransaction& transaction) noexcept;
    [[nodiscard]] RealtimeMidiDispatchResult scheduleMidiNote(
        RealtimeMidiNoteEvent event,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] std::optional<TypedEventTarget> eventValueTarget(
        std::string_view name,
        talas::expr::ValueDomain domain,
        std::uint64_t generation) const noexcept;
    [[nodiscard]] std::optional<TypedEventTarget> eventValueTarget(
        RuntimeScopeId scopeId,
        std::string_view name,
        talas::expr::ValueDomain domain,
        std::uint64_t generation) const noexcept;
    [[nodiscard]] std::optional<TypedEventTarget> variableTarget(
        const talas::project::PreparedProjectVariableIdentity& identity,
        talas::expr::ValueDomain domain,
        std::uint64_t generation) const noexcept;
    [[nodiscard]] std::optional<SoundVariableTypedEventTarget> soundVariableTarget(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::string_view selectedDefinitionId,
        std::uint64_t generation,
        std::uint64_t voiceGeneration = 0
    ) const noexcept;
    [[nodiscard]] std::optional<TypedEventTarget> gateTarget(
        std::string_view nodeId,
        std::uint64_t generation) const noexcept;
    [[nodiscard]] bool scheduleTypedEvents(
        std::span<const PreparedTypedEvent> events,
        std::span<RealtimeTypedEventDispatchResult> results) noexcept;
    [[nodiscard]] bool schedulePreparedLiveToolEvents(
        std::span<const PreparedTypedEvent> events,
        std::span<RealtimeTypedEventDispatchResult> results) noexcept;
    [[nodiscard]] bool resolvePreparedLiveToolEvent(
        std::string_view name,
        talas::expr::ExpressionValue& value) const noexcept;
    [[nodiscard]] bool hasEventValueBinding(std::string_view name, talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] bool eventValueRequiresReprepare(
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] bool canScheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] bool render(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    // Strict project-ordinal mapping: no mono-to-stereo fallback.
    [[nodiscard]] bool renderOrdered(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderTerminalEvents(
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output) noexcept;

private:
    [[nodiscard]] bool renderWithOutputMapping(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        bool duplicateMonoToStereo) noexcept;

    [[nodiscard]] bool renderOwnWithOutputMapping(RealtimeAudioInputBlock input,
                                                  float* const* outputChannels,
                                                  std::size_t channelCount,
                                                  std::size_t frameCount,
                                                  bool duplicateMonoToStereo) noexcept;
    [[nodiscard]] RealtimeAudioEngine* liveSetControlTarget() noexcept;
    [[nodiscard]] const RealtimeAudioEngine* liveSetControlTarget() const noexcept;
    [[nodiscard]] bool retainLiveSetEventValuesFrom(
        const RealtimeAudioEngine& source) noexcept;
    [[nodiscard]] bool scheduleRetainedLiveSetEventValuesOn(
        RealtimeAudioEngine& candidate) noexcept;
    void resetRootEventValueBaselineForRetained() noexcept;
    void markPendingRootEventValue(
        std::uint32_t targetIndex, double value) noexcept;
    void beginRootEventValueCallback() noexcept;
    void requeueRootEventValueCallback() noexcept;
    void captureRenderedRootEventValueChanges() noexcept;

    RealtimeRenderSession session_;
    std::size_t rootEventValueBindingCount_ = 0;
    std::unique_ptr<std::atomic<bool>[]> rootPendingEventValueDirty_;
    std::unique_ptr<std::atomic<double>[]> rootPendingEventValueDesired_;
    std::vector<std::uint8_t> rootCallbackEventValueDirty_;
    std::vector<double> rootCallbackEventValueDesired_;
    std::vector<std::vector<double>> scratchChannels_;
    std::vector<double*> scratchChannelPointers_;
    std::vector<std::vector<std::vector<float>>> diskRecorderTapScratch_;
    std::vector<std::vector<float*>> diskRecorderTapPointers_;
    std::size_t diskRecorderTapFrames_ = 0;
    std::atomic<bool> playing_ = false;
    std::shared_ptr<const PreparedLiveSet> preparedLiveSet_;
    std::vector<RealtimeEventValueUpdate> retainedLiveSetEventValues_;
    std::vector<std::uint32_t> retainedRootEventValueTargetIndices_;
    std::vector<double> retainedRootEventValueBaseline_;
    std::size_t retainedLiveSetEventValueCount_ = 0;
    std::shared_ptr<const PreparedLiveTools> preparedLiveTools_;
    std::unique_ptr<PreparedLiveSetRuntime> preparedLiveSetRuntime_;
    std::atomic<RealtimeAudioEngine*> activeLiveSetEngine_ = nullptr;
    std::atomic<RealtimeAudioEngine*> liveSetControlEngine_ = nullptr;
    RealtimeAudioEngine* liveSetTransitionFrom_ = nullptr;
    RealtimeAudioEngine* liveSetTransitionTo_ = nullptr;
    std::uint32_t liveSetTransitionCellIndexPlusOne_ = 0;
    std::atomic<std::uint32_t> activeLiveSetCellIndexPlusOne_ = 0;
    std::array<std::atomic<RealtimeAudioEngine*>,
        talas::project::projectLiveSetColumns> activeLiveSetSoundByColumn_{};
    std::array<std::atomic<std::uint32_t>,
        talas::project::projectLiveSetColumns> activeLiveSetSoundCellByColumn_{};
    std::array<std::atomic<bool>,
        talas::project::projectLiveSetColumns> liveSetSoundMutedByColumn_{};
    std::uint64_t liveSetTransitionFrames_ = 0;
    std::uint64_t liveSetTransitionCompletedFrames_ = 0;
    std::vector<std::vector<float>> liveSetOldScratch_;
    std::vector<std::vector<float>> liveSetNewScratch_;
    std::array<std::vector<std::vector<float>>,
        talas::project::projectLiveSetColumns> liveSetRoutedInputScratch_;
    std::vector<const float*> liveSetRoutedInputPointers_;
    std::vector<float*> liveSetOldPointers_;
    std::vector<float*> liveSetNewPointers_;
    std::uint64_t stageId_ = 0;
    std::uint64_t snapshotGeneration_ = 0;
    std::optional<std::uint64_t> preparedStageFrame_;
    bool hasRendered_ = false;
};
}
