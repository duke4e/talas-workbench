#pragma once

#include "talas/app/PartialTrackEditorModel.h"
#include "talas/project/ProjectPartialTrackEdits.h"

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The spectrum editor's selections, markers and playing: tracks and frames
// selected by boxes, with Shift extending them or cutting holes; selections by
// number and by amplitude or track-number criteria; the information on the
// point under the mouse; markers named by their frame, the loop markers and
// their pre- and post-roll; playing all, the selection, or what lies between
// or outside the loop markers; and cutting the frames between them. A track's
// number is its trackId; frames are source frames, a point's time in samples.
namespace talas::app
{
struct PartialTrackBox
{
    std::uint64_t firstTrack = 0;
    std::uint64_t lastTrack = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
};

class PartialTrackSelection
{
public:
    // A box selects the points within it, replacing the selection; with
    // Shift it extends the selection, or, begun inside the selection, cuts a
    // hole in it.
    void selectBox(const talas::project::MaterializedPartialTrackEdits& materialized, PartialTrackBox box, bool shift,
        std::uint64_t startTrack, std::uint64_t startFrame);
    // Whether a track and frame lie within the selection: between two
    // selected frames of that track.
    [[nodiscard]] bool inside(std::uint64_t trackId, std::uint64_t frame) const;
    // A track, by clicking it or typing its number (1 to 9, 0 for ten).
    void selectTrack(const talas::project::MaterializedPartialTrackEdits& materialized, std::uint64_t trackId, bool shift);
    void selectComplement(const talas::project::MaterializedPartialTrackEdits& materialized);
    // Select All (Ctrl+A): every track.
    void selectAll(const talas::project::MaterializedPartialTrackEdits& materialized);
    void add(std::uint64_t trackId, std::uint64_t frame);
    // The points a box drawn on the display holds, with the same Shift
    // rules: begun inside the selection, a hole.
    void selectPoints(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& points, bool shift, bool startsInside);
    void clear() noexcept;

    [[nodiscard]] bool contains(std::uint64_t trackId, std::uint64_t frame) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::vector<std::uint64_t> trackIds() const;
    // The frames the selection spans, first to last.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> frameSpan() const;
    [[nodiscard]] const std::set<std::pair<std::uint64_t, std::uint64_t>>& points() const noexcept;

private:
    std::set<std::pair<std::uint64_t, std::uint64_t>> points_;
};

// The track a number key selects: 1 to 9, and 0 for ten.
[[nodiscard]] std::optional<std::uint64_t> partialTrackForNumberKey(char key) noexcept;

// Copy in the spectrum editor: a new spectrum of
// the selected region alone, the selected points of each frame the region
// spans, its first frame at the start; none for an empty selection.
[[nodiscard]] std::optional<talas::project::PartialTrackAsset> partialTrackRegionAsset(
    const talas::project::PartialTrackAsset& asset, const PartialTrackSelection& selection);

enum class PartialTrackSelectionCriterion : std::uint8_t
{
    AmplitudesWithinRange,
    AboveAmplitudeThreshold,
    EvenTracks,
    OddTracks,
    OctaveHarmonics,
    FibonacciTracks,
    PrimeTracks,
};

// The tracks and frames a criterion selects; raw amplitudes run from 0 to
// 127, the value the readout shows in parentheses.
[[nodiscard]] PartialTrackSelection selectPartialTracksBy(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    PartialTrackSelectionCriterion criterion,
    int lowRaw = 0,
    int highRaw = 127);

[[nodiscard]] int partialTrackRawAmplitude(double amplitude) noexcept;

// What the editor shows for the point under the mouse: its track, its time in
// seconds, its amplitude in dB with the raw value in parentheses, and its
// frequency in hertz.
[[nodiscard]] std::string partialTrackReadout(
    const talas::project::PartialTrackPoint& point, std::uint64_t frame, double sampleRate);

struct PartialTrackMarker
{
    std::string name;
    std::uint64_t frame = 0;

    bool operator==(const PartialTrackMarker&) const = default;
};

struct PartialTrackMarkers
{
    std::vector<PartialTrackMarker> markers;
    std::optional<std::uint64_t> loopStart;
    std::optional<std::uint64_t> loopEnd;
    double preRollSeconds = 0.0;
    double postRollSeconds = 0.0;
};

// A marker at a frame is named m and the number of the analysis frame there.
[[nodiscard]] std::string defaultPartialTrackMarkerName(const talas::project::PartialTrackAsset& asset, std::uint64_t frame);

// The markers, labelled, and the loop markers are
// kept in the spectrum file's header the next time it is saved (those
// outside the source, and loop markers out of order, left out), and a file
// opened brings its own to the editor.
[[nodiscard]] talas::project::PartialTrackAsset partialTrackAssetWithMarkers(
    talas::project::PartialTrackAsset asset, const PartialTrackMarkers& markers);
[[nodiscard]] PartialTrackMarkers partialTrackMarkersOf(const talas::project::PartialTrackAsset& asset);

enum class PartialTrackPlay : std::uint8_t
{
    All,
    Selection,
    BetweenLoopMarkers,
    OutsideLoopMarkers,
    UpToStartMarker,
};

// What a play button plays: every point, the selected ones, those between
// the loop markers with their pre- and post-roll, those outside the loop
// markers, the later ones moved up to close the gap, or those before the
// start loop marker, then stopping (no end marker needed); the frames start
// at 0. Nothing when the loop markers a play needs are missing or out of
// order.
[[nodiscard]] std::optional<talas::project::MaterializedPartialTrackEdits> partialTrackPlayable(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    PartialTrackPlay play,
    const PartialTrackSelection& selection,
    const PartialTrackMarkers& markers);

// The scrub bar heard: moved, it plays the `frames` from the bar on at their natural
// rate, never faster, of all the tracks or of the selected tracks only, as
// the play button used last chose (Selection for the selected ones); drawing
// plays the selected track drawn on the same way. The frames start at 0;
// nothing when no point lies there.
[[nodiscard]] std::optional<talas::project::MaterializedPartialTrackEdits> partialTrackScrubbed(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    PartialTrackPlay lastPlay,
    const PartialTrackSelection& selection,
    std::uint64_t frame,
    std::uint64_t frames);

// Selecting a cleared, greyed track restores it: the state that lifts its mute when the track
// just selected, not selected before, is muted; nothing otherwise.
[[nodiscard]] std::optional<talas::project::ProjectPartialTrackStateMutation> partialTrackRestoredBySelection(
    std::uint64_t trackId, bool muted, bool solo, std::optional<std::uint64_t> selectedBefore) noexcept;

// Save as... in the spectrum editor: the spectrum as edited, made permanent: the cleared tracks
// (muted, or with a track soloed every other one) gone along with the
// frames cut, so that selecting a track restores nothing.
[[nodiscard]] talas::project::PartialTrackAsset partialTrackAssetSaved(const talas::project::MaterializedPartialTrackEdits& materialized);

// Cutting the frames from the loop start up to the loop end: the points
// there are deleted and the later ones moved up by the length cut.
[[nodiscard]] std::vector<talas::project::ProjectPartialTrackPointMutation> cutPartialTrackFrames(
    const talas::project::MaterializedPartialTrackEdits& materialized, std::uint64_t startFrame, std::uint64_t endFrame);
// The strip of buttons across the bottom of the spectrum editor: twelve in
// groups of four, each pressed too by its function key, F1 to F12: playing
// all, the selection, between and outside the loop markers; a marker, the loop
// start and end at the scrub bar, the pre- and post-roll; selecting by
// criteria, the track filters, cutting between the loop markers, drawing.
enum class SpectrumEditorButton : std::uint8_t
{
    PlayAll,
    PlaySelection,
    PlayBetweenLoopMarkers,
    PlayOutsideLoopMarkers,
    Marker,
    LoopStart,
    LoopEnd,
    PrePostRoll,
    SelectionCriteria,
    TrackFilters,
    Cut,
    Draw,
};

struct SpectrumEditorButtonInfo
{
    SpectrumEditorButton button = SpectrumEditorButton::PlayAll;
    int functionKey = 1;
    std::string_view label;
};

[[nodiscard]] std::span<const SpectrumEditorButtonInfo> spectrumEditorButtons() noexcept;
[[nodiscard]] std::optional<SpectrumEditorButton> spectrumEditorButtonForFunctionKey(int functionKey) noexcept;
// The fourth play button (F4) plays everything but the part between the loop
// markers or, with the spectrum menu's choice "Fourth button plays up to the
// start marker", up to the start marker and then stops; the first is the
// default. What it plays and what it is labelled.
[[nodiscard]] PartialTrackPlay spectrumEditorFourthButtonPlay(bool upToStartMarker) noexcept;
[[nodiscard]] std::string_view spectrumEditorFourthButtonLabel(bool upToStartMarker) noexcept;

// The scissors first ask to verify the cut, naming the first and the last
// frame it deletes.
[[nodiscard]] std::string cutPartialTrackFramesQuestion(std::uint64_t startFrame, std::uint64_t endFrame);

// The display: frequency on the vertical axis,
// labelled FREQ in green, or amplitude, AMP in red; each frame a dot, or the
// dots joined into lines.
enum class PartialTrackVerticalAxis : std::uint8_t
{
    Frequency,
    Amplitude,
};

[[nodiscard]] std::string_view partialTrackAxisLabel(PartialTrackVerticalAxis axis) noexcept;

// Join-the-dots takes effect only where the frames are far enough apart for
// a gap between their dots:
// two dots are joined when the mode is on and the gap between their centres,
// in pixels, is wider than a dot.
[[nodiscard]] bool partialTrackDotsJoined(bool joinMode, double gapPixels, double dotPixels) noexcept;

// Drawing mode: what the
// pen at a height (0 at the top, 1 at the bottom) sets the selected
// envelope to: an amplitude from 1 at the top to 0 at the bottom, or a
// frequency between the display's highest and lowest.
[[nodiscard]] double partialTrackDrawnValue(
    PartialTrackVerticalAxis axis, double heightRatio, double lowestHz, double highestHz) noexcept;
[[nodiscard]] std::uint32_t partialTrackAxisColour(PartialTrackVerticalAxis axis) noexcept;

// The spectrum display: a point's colour from its amplitude, dark blue for the quietest
// (60 dB below full scale or less) through green to yellow for the
// loudest, brighter when its track is selected; as ARGB.
[[nodiscard]] std::uint32_t partialTrackAmplitudeColour(double amplitude, bool selected) noexcept;
// A cleared track shows dark gray until selected
// or saved; once saved it is gone from the spectrum, and the display's
// black shows where it was. As ARGB.
[[nodiscard]] std::uint32_t partialTrackClearedColour() noexcept;
[[nodiscard]] std::uint32_t spectrumEditorBackgroundColour() noexcept;
// A start or end loop marker shows red at the top
// of the display, where its name is. As ARGB.
[[nodiscard]] std::uint32_t spectrumEditorLoopMarkerColour() noexcept;

// Zooming: Zoom in looks closer at the middle, Zoom out goes back in
// steps, a box drawn with Control or Command fills the display, and the full
// display button shows the whole file.
[[nodiscard]] PartialTrackEditorViewport partialTrackZoomedIn(PartialTrackEditorViewport viewport);
[[nodiscard]] PartialTrackEditorViewport partialTrackZoomedOut(
    PartialTrackEditorViewport viewport, std::uint64_t sourceFrames, double minimumHz, double maximumHz);
[[nodiscard]] PartialTrackEditorViewport partialTrackZoomedToBox(PartialTrackEditorViewport viewport,
    std::uint64_t startFrame, std::uint64_t endFrame, double lowHz, double highHz);
[[nodiscard]] PartialTrackEditorViewport partialTrackFullDisplay(
    PartialTrackEditorViewport viewport, std::uint64_t sourceFrames, double minimumHz, double maximumHz);

// The scrub bar: a frame left or right with the arrow keys; dragged,
// it moves with the mouse, or with Shift jumps to it; MIDI pitch bend
// (0 to 16383) places it across the file.
[[nodiscard]] std::uint64_t partialTrackScrubStepped(std::uint64_t frame, std::uint64_t sourceFrames, bool right) noexcept;
[[nodiscard]] std::uint64_t partialTrackScrubDragged(std::uint64_t barAtStart, std::int64_t mouseMoved,
    std::uint64_t mouseFrame, bool shift, std::uint64_t sourceFrames) noexcept;
[[nodiscard]] std::uint64_t partialTrackScrubForPitchBend(int bend, std::uint64_t sourceFrames) noexcept;
// The scrub bar is a yellow time cursor, its time
// shown in white at the top of the display, in seconds to the millisecond;
// the colours as ARGB.
[[nodiscard]] std::uint32_t partialTrackScrubBarColour() noexcept;
[[nodiscard]] std::uint32_t partialTrackScrubTimeColour() noexcept;
[[nodiscard]] std::string partialTrackScrubTimeText(std::uint64_t frame, double sampleRate);

// The up and down arrows select the track above or below the selected one
//.
[[nodiscard]] std::optional<std::uint64_t> partialTrackAdjacent(
    const std::vector<std::uint64_t>& trackIds, std::optional<std::uint64_t> current, bool up);
}
