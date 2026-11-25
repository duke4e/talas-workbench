#pragma once

#include "talas/app/TimelineGrid.h"
#include "talas/app/TimelineInfo.h"
#include "talas/audio/TimelineRecording.h"
#include "talas/runtime/EnvelopeExtraction.h"
#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/TimelineMarks.h"
#include "talas/app/TimelineScoreProjection.h"

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
struct TimelineOutputMeterSnapshot
{
    std::uint64_t stageId = 0;
    std::array<float, 8> peaks{};
};

struct TimelineControllerRecordingMenuState
{
    bool startOrStopEnabled = false;
    bool previewEnabled = false;
    bool approveEnabled = false;
    bool discardEnabled = false;
};

enum class TimelineScoreClipArtifactStateKind
{
    Live,
    RenderingCache,
    Freezing,
    Cached,
    CacheStale,
    Frozen,
    Missing,
    Invalid,
    Cancelling,
};

enum class TimelineScoreClipArtifactAction
{
    Cache,
    RefreshCache,
    Freeze,
    Unfreeze,
    Cancel,
    // The Cache menu's Live: the cached or frozen
    // Sound played live again.
    Live,
};

struct TimelineScoreClipArtifactState
{
    TimelineScoreClipArtifactStateKind kind =
        TimelineScoreClipArtifactStateKind::Live;
    std::string label = "Live";
    std::string detail;
    std::uint16_t progressPermille = 0;
    bool sourceAvailable = true;
};

// The fields of the strip at the top of the Timeline: the cursor's time,
// the selected Sound's start, anchor, duration and end, and the selected
// marker's name and time.
enum class TimelineInfoField : std::uint8_t
{
    Cursor,
    SoundStart,
    SoundAnchor,
    SoundDuration,
    SoundEnd,
    MarkerName,
    MarkerTime,
    PointTime,
    PointValue,
};

// The context menu's Lane State and Automation menu items.
inline constexpr int timelineLaneStateMenuBase = 7300;
inline constexpr int timelineAutomationMenuBase = 7310;

class TimelinePanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using OutputMeterSnapshotCallback =
        std::function<TimelineOutputMeterSnapshot()>;
    using AcceptedStageCurrentCallback = std::function<bool()>;
    using ClipArtifactStateCallback =
        std::function<TimelineScoreClipArtifactState(std::string_view)>;
    using ClipArtifactActionCallback = std::function<void(
        std::string, TimelineScoreClipArtifactAction)>;

    ~TimelinePanel() override = default;

    virtual void refresh() = 0;
    virtual void setTransportSnapshot(
        const talas::runtime::RealtimeMusicalTransportSnapshot& snapshot) = 0;
    virtual void setExternalTransportAccessibilityDetail(
        const juce::String& detail) = 0;
    virtual void focusTimeline() = 0;
    // A MIDI program change on the Timeline's
    // channel jumps the cursor to its marker and is consumed; on another
    // channel it is left to the presets.
    virtual bool jumpToMarkerForProgramChange(std::uint8_t channel, std::uint8_t program) = 0;
    // The marked Sounds and the mode the
    // Timeline plays in.
    virtual bool markClip(const std::string& clipId, bool toggle) = 0;
    [[nodiscard]] virtual talas::app::TimelineMarks& marks() noexcept = 0;
    // What Ctrl+V pastes when no clips were copied, a Sound
    // copied elsewhere made one of the project's Sounds; its id.
    virtual void setSoundPasteHandler(std::function<std::optional<std::string>()> handler) = 0;
    // Something else copied since: Paste takes it, not the clips copied.
    virtual void forgetCopiedClips() {}
    // The Sounds of the clips copied, for a Sound file
    // window's Paste.
    virtual void setSoundCopyHandler(std::function<void(std::vector<std::string>)> handler) = 0;
    // A double-click on a Sound in the Timeline opens its
    // editor.
    virtual void setSoundEditHandler(std::function<void(const std::string&)> handler) = 0;
    // Sounds copied in one Timeline paste into another
    // with the Sounds they play, checked against the registry, their
    // samples found from the project's file, where it has one.
    virtual void setPasteSources(const talas::core::NodeRegistry& registry,
        std::function<std::optional<std::filesystem::path>()> projectFile)
    {
        (void) registry;
        (void) projectFile;
    }
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    // The strip at the top of the Timeline
    // for a selection, a field typed into with Enter, a marker's program
    // change, the program change channel, the Anchor button, Capture, and
    // the alignments to an anchor.
    [[nodiscard]] virtual std::string infoFieldTextForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field) = 0;
    [[nodiscard]] virtual bool enterInfoFieldForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field, const std::string& text) = 0;
    [[nodiscard]] virtual std::optional<TimelineInfoField> typingInfoFieldForSmoke() const = 0;
    // Keys typed into a field, its text selected first; what it then holds.
    [[nodiscard]] virtual std::string typeIntoInfoFieldForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field, const juce::String& keys) = 0;
    [[nodiscard]] virtual bool setMarkerProgramForSmoke(TimelineScoreSelection selection, std::uint8_t program) = 0;
    [[nodiscard]] virtual bool setMarkerProgramChannelForSmoke(std::uint8_t channel) = 0;
    [[nodiscard]] virtual bool anchorAtCursorForSmoke(TimelineScoreSelection selection) = 0;
    [[nodiscard]] virtual bool captureMarkerForSmoke() = 0;
    // The score menu's Make a Sound of the Timeline.
    [[nodiscard]] virtual bool makeSoundOfTimelineForSmoke() = 0;
    [[nodiscard]] virtual bool editClipSoundForSmoke(std::string_view clipId) = 0;
    // Copy the clips and Paste them on a
    // track at a frame; whether they were pasted.
    [[nodiscard]] virtual bool copyAndPasteClipsForSmoke(
        const std::vector<std::string>& clipIds, const std::string& trackId, std::uint64_t frame) = 0;
    // A lane's vertical zoom moved by steps
    // out (negative, in); whether the lane took them.
    [[nodiscard]] virtual bool zoomLaneValuesForSmoke(const std::string& laneId, int steps) = 0;
    [[nodiscard]] virtual bool alignClipForSmoke(
        TimelineScoreSelection selection, std::string_view anchoredClipId, bool end) = 0;
    // What the Automate menu offers for a track or clip, and the
    // automation edits the context menu makes.
    // A Sound dragged from the Sound browser and dropped on a track.
    [[nodiscard]] virtual bool dropSoundOnTrackForSmoke(
        std::string_view definitionId,
        std::string_view trackId,
        std::uint64_t frame) = 0;
    // A Sound dropped below the last track.
    [[nodiscard]] virtual bool dropSoundBelowTracksForSmoke(std::string_view definitionId, std::uint64_t frame) = 0;
    // A drag's description, as a Sound Browser gives it, dropped on a track.
    [[nodiscard]] virtual bool dropDescriptionOnTrackForSmoke(
        const juce::String& description, std::string_view trackId, std::uint64_t frame) = 0;
    // An effect dropped on a clip, minus, and a Sound dropped
    // on a clip with Option (Control); a pencil stroke of
    // (frame, value) points over a lane.
    [[nodiscard]] virtual bool applyEffectToClipForSmoke(
        std::string_view clipId, std::string_view typeId) = 0;
    [[nodiscard]] virtual bool replaceClipInputForSmoke(
        std::string_view clipId, std::string_view definitionId) = 0;
    [[nodiscard]] virtual bool removeClipEffectForSmoke(std::string_view clipId) = 0;
    // A track's input chosen from its Input menu, and that menu.
    [[nodiscard]] virtual bool setTrackInputForSmoke(std::string_view trackId, std::string_view inputTrackId) = 0;
    [[nodiscard]] virtual std::vector<juce::String> trackInputMenuForSmoke(std::string_view trackId) = 0;
    // The Submix menu connecting a track to a submix, or to a new
    // one.
    [[nodiscard]] virtual bool setTrackSendForSmoke(std::string_view trackId, std::string_view submixId, bool connected) = 0;
    [[nodiscard]] virtual bool addSubmixForSmoke(std::string_view trackId, std::string_view name) = 0;
    // An output channel toggled, or 0 for the pan controls.
    [[nodiscard]] virtual bool toggleTrackOutputForSmoke(std::string_view trackId, std::uint32_t channel) = 0;
    // The MIDI status control's menu, a channel or 0 for the
    // default one.
    [[nodiscard]] virtual bool setTrackMidiChannelForSmoke(std::string_view trackId, std::uint8_t channel) = 0;
    // One of the hardware's audio inputs as a track's input.
    [[nodiscard]] virtual bool setTrackHardwareInputForSmoke(std::string_view trackId, std::uint32_t channel) = 0;
    // No output from the audio output menu.
    [[nodiscard]] virtual bool setTrackNoOutputForSmoke(std::string_view trackId) = 0;
    // Points selected together, by a box over their lane
    // or as given, a key pressed in the Timeline (Delete, Enter, the
    // arrows, Ctrl+A), a value for the selected points, a Ctrl+Click point
    // at a height in its lane, and which points are selected.
    [[nodiscard]] virtual bool selectPointsForSmoke(std::string_view laneId, std::vector<std::size_t> indices) = 0;
    [[nodiscard]] virtual bool boxLaneForSmoke(std::string_view laneId) = 0;
    [[nodiscard]] virtual bool keyForSmoke(const juce::KeyPress& key) = 0;
    [[nodiscard]] virtual bool setSelectedPointsValueForSmoke(const std::string& text) = 0;
    [[nodiscard]] virtual bool addPointForSmoke(std::string_view laneId, std::uint64_t frame, double height) = 0;
    [[nodiscard]] virtual std::vector<std::size_t> selectedPointsForSmoke() = 0;
    // The Grid and Quantization menus' choices, a selection, and
    // the Timeline's sample rate for the frames a smoke drops Sounds at.
    [[nodiscard]] virtual bool setGridForSmoke(talas::app::TimelineGridMode mode, talas::app::TimelineQuantization quantization) = 0;
    virtual void selectForSmoke(TimelineScoreSelection selection) = 0;
    [[nodiscard]] virtual std::uint32_t sampleRateForSmoke() = 0;
    // The selected lane's Source of Control menu.
    [[nodiscard]] virtual bool newTakeForSmoke() = 0;
    [[nodiscard]] virtual bool chooseTakeForSmoke(const std::string& name) = 0;
    [[nodiscard]] virtual bool renameTakeForSmoke(const std::string& name) = 0;
    // A Sound's envelope extracted into a controller: the
    // extractor, which knows the project's samples, and a smoke's request.
    using EnvelopeExtractor = std::function<talas::runtime::EnvelopeExtractionResult(const talas::runtime::EnvelopeExtractionRequest&)>;
    virtual void setEnvelopeExtractor(EnvelopeExtractor extractor) = 0;
    [[nodiscard]] virtual bool extractEnvelopeForSmoke(
        std::string_view laneId, const talas::runtime::EnvelopeExtractionRequest& request) = 0;
    // Record to disk..., the recorder that makes a job recording
    // the Timeline as the project is now, which runs off the message
    // thread; a smoke runs it at once.
    using TimelineRecorder =
        std::function<std::function<talas::audio::TimelineRecordingResult()>(talas::audio::TimelineRecordingOptions)>;
    virtual void setTimelineRecorder(TimelineRecorder recorder) = 0;
    [[nodiscard]] virtual talas::audio::TimelineRecordingResult recordTimelineForSmoke(
        talas::audio::TimelineRecordingOptions options) = 0;
    // The time cursor's source menu, Free running or Time code,
    // which the chooser applies to the transport sync settings; and Set
    // time code offset.
    using TimeSourceChooser = std::function<bool(bool timeCode)>;
    virtual void setTimeSourceChooser(TimeSourceChooser chooser) = 0;
    // A Sound or a file dragged from a Sound Browser, or a
    // prototype, brought into the project's Sounds; the new Sound's
    // definition.
    using DragImporter = std::function<std::optional<std::string>(const std::string& description)>;
    virtual void setDragImporter(DragImporter importer) = 0;
    // Space in the marker name field plays and pauses, as it
    // does everywhere else.
    using SpaceKey = std::function<void()>;
    virtual void setSpaceKey(SpaceKey key) = 0;
    [[nodiscard]] virtual bool setTimeCodeOffsetForSmoke(std::string_view text) = 0;
    // Sounds Shift+clicked into a group, and the group selected.
    [[nodiscard]] virtual bool shiftClickClipForSmoke(const std::string& clipId) = 0;
    [[nodiscard]] virtual std::vector<std::string> selectedClipsForSmoke() = 0;
    // Slave to and New Master.
    [[nodiscard]] virtual bool slaveLaneForSmoke(const std::string& masterLaneId) = 0;
    [[nodiscard]] virtual bool newMasterControlForSmoke(const std::string& name) = 0;
    [[nodiscard]] virtual bool drawAutomationStrokeForSmoke(
        std::string_view laneId,
        std::vector<std::pair<std::uint64_t, double>> stroke) = 0;
    [[nodiscard]] virtual std::vector<juce::String> automateMenuForSmoke(
        TimelineScoreSelection selection) = 0;
    [[nodiscard]] virtual bool automateForSmoke(
        TimelineScoreSelection selection,
        std::string_view eventName) = 0;
    [[nodiscard]] virtual bool shapeAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationCurveShape shape) = 0;
    [[nodiscard]] virtual bool transformAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationTransform transform,
        talas::app::AutomationTransformParameters parameters) = 0;
    // The Lane State menu (base plus
    // Automated 0, Live 1, Record Enable 2) and the Automation menu (base
    // plus twice the state, plus one for Track Level & Angle Controls).
    [[nodiscard]] virtual bool invokeLaneStateMenuActionForSmoke(
        TimelineScoreSelection selection, int actionId) = 0;
    [[nodiscard]] virtual bool setControllerRecordingTakeForSmoke(
        talas::project::ControllerRecordingTake take) = 0;
    [[nodiscard]] virtual TimelineControllerRecordingMenuState
    controllerRecordingMenuStateForSmoke() = 0;
    [[nodiscard]] virtual bool invokeControllerRecordingMenuActionForSmoke(
        int actionId) = 0;
    [[nodiscard]] virtual TimelineScoreClipArtifactState
    clipArtifactStateForSmoke(std::string_view clipId) = 0;
    [[nodiscard]] virtual bool invokeClipArtifactActionForSmoke(
        std::string clipId,
        TimelineScoreClipArtifactAction action) = 0;
    // The Cache menu's look for a clip, and the doughnut's colour now.
    [[nodiscard]] virtual talas::app::TimelineCacheLook cacheLookForSmoke(std::string_view clipId) = 0;
    [[nodiscard]] virtual juce::Colour cacheMenuColourForSmoke() = 0;
#endif
};

[[nodiscard]] std::unique_ptr<TimelinePanel> makeTimelinePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    TimelinePanel::DocumentChangeCallback onDocumentChange,
    TimelinePanel::StatusCallback onStatus,
    TimelinePanel::OutputMeterSnapshotCallback outputMeters,
    TimelinePanel::AcceptedStageCurrentCallback acceptedStageCurrent,
    TimelinePanel::ClipArtifactStateCallback clipArtifactState,
    TimelinePanel::ClipArtifactActionCallback clipArtifactAction);
}
