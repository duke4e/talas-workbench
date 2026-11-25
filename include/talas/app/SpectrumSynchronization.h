#pragma once

#include "talas/app/AnalysisToolsModel.h"
#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Synchronizing Spectra tool: markers at corresponding points of a guide
// spectrum and a warped one, the suggested markers among them, saved with each
// spectrum file; a mix of the two resynthesized, the guide left and the warped
// right, its timing warped to line its markers up with the guide's; and a
// morph between the two in amplitude, frequency and time.
namespace talas::app
{
// A marker at a frame of a spectrum file, the index of the frame.
struct SpectrumMarker
{
    std::string name;
    std::size_t frame = 0;

    bool operator==(const SpectrumMarker&) const = default;
};

// The time of a frame, in seconds from the spectrum's start; the end of
// the spectrum after its last frame.
[[nodiscard]] double spectrumFrameSeconds(const talas::project::PartialTrackAsset& spectrum, std::size_t frame) noexcept;
// Dragging the yellow scrub bar plays the spectrum there: the samples of its
// resynthesis, `samples` long, from the frame's time on, for `seconds`; begin
// and end, within it.
[[nodiscard]] std::pair<std::size_t, std::size_t> spectrumScrubSlice(
    const talas::project::PartialTrackAsset& spectrum, std::size_t frame, std::size_t samples, double seconds = 0.1) noexcept;

// The markers the a key suggests: at the first and last frames and wherever
// the spectrum changes most from the frame before, at least
// `minimumSpacingSeconds` apart, named by their frame.
[[nodiscard]] std::vector<SpectrumMarker> autoSpectrumMarkers(
    const talas::project::PartialTrackAsset& spectrum, double minimumSpacingSeconds = 0.08);

// A marker added at a frame (the m key), named m and the frame unless
// named, renamed, or deleted; the markers kept in the order of their
// frames, one to a frame; nothing when the edit does not apply.
[[nodiscard]] std::optional<std::vector<SpectrumMarker>> spectrumMarkersWith(
    std::vector<SpectrumMarker> markers, std::size_t frame, std::string name = {});
[[nodiscard]] std::optional<std::vector<SpectrumMarker>> spectrumMarkersRenamed(
    std::vector<SpectrumMarker> markers, std::size_t index, std::string name);
[[nodiscard]] std::optional<std::vector<SpectrumMarker>> spectrumMarkersWithout(
    std::vector<SpectrumMarker> markers, std::size_t index);

// The markers' colours, in order from left to right: red, green, blue,
// red again; the selected marker white.
enum class SpectrumMarkerColour : std::uint8_t
{
    Red,
    Green,
    Blue,
    White,
};

[[nodiscard]] SpectrumMarkerColour spectrumMarkerColour(std::size_t index, bool selected = false) noexcept;

// The tool's keys act in the active one of its
// two views, the one with the white border: Tab makes the other view active,
// as a click in it does; Space auditions the active spectrum and the arrows
// move its scrub bar a frame; m adds a marker at the scrub bar and a
// suggests markers; s saves them; Delete removes the selected marker and
// Return names it, or with none selected shows the whole spectrum again.
// Those that need a spectrum or a selected marker do nothing without one.
enum class SpectrumSyncKey : std::uint8_t
{
    Tab,
    Space,
    Left,
    Right,
    M,
    A,
    S,
    Delete,
    Return,
    Other,
};

enum class SpectrumSyncAction : std::uint8_t
{
    SwitchView,
    Audition,
    ScrubBack,
    ScrubOn,
    AddMarker,
    SuggestMarkers,
    SaveMarkers,
    DeleteMarker,
    NameMarker,
    ShowWhole,
    None,
};

[[nodiscard]] SpectrumSyncAction spectrumSyncAction(
    SpectrumSyncKey key, bool spectrumLoaded, bool markerSelected) noexcept;

// Save Markers: the markers beside their spectrum file, "voice h.talaspartial"
// keeping its in "voice h.talaspartial.markers", a marker a line, its frame
// then its name; read back when the file is opened again (none without
// one).
[[nodiscard]] std::filesystem::path spectrumMarkersPath(const std::filesystem::path& spectrumFile);
[[nodiscard]] bool writeSpectrumMarkers(
    const std::filesystem::path& spectrumFile, const std::vector<SpectrumMarker>& markers, std::string& error);
[[nodiscard]] std::optional<std::vector<SpectrumMarker>> readSpectrumMarkers(
    const std::filesystem::path& spectrumFile, std::string& error);

// The Mix and Morph examples need the same number of markers in both
// files, two at least.
[[nodiscard]] bool spectrumMarkersCorrespond(
    const std::vector<SpectrumMarker>& guide, const std::vector<SpectrumMarker>& warped) noexcept;

// A spectrum file chosen for a field
// that reads a table, SpectrumInRAM's Analysis, SpectrumOnDisk's FileName
// or SumOfSines' Analysis0 and Analysis1, plays through a table written
// beside it ("voice h table.txt"): SpectrumInRAM's and SpectrumOnDisk's
// triples, SumOfSines' harmonic amplitudes. The fields that describe the
// table are set with it: its frames, and the 1 hz AnalysisFrequency the
// triples' frequencies are in hertz for.
enum class SpectrumTableUse : std::uint8_t
{
    None,
    Triples,
    HarmonicAmplitudes,
};

[[nodiscard]] SpectrumTableUse spectrumTableUse(std::string_view typeId, std::string_view parameterId) noexcept;
[[nodiscard]] std::filesystem::path spectrumTablePath(const std::filesystem::path& spectrumFile);
[[nodiscard]] std::vector<talas::project::ProjectParameterExpression> spectrumTableFields(
    std::string_view parameterId, SpectrumTableUse use, std::size_t frames);

// The markers' times as WarpedTimeIndex reads its time points: "{0 s}
// {0.52 s} {1.3 s}".
[[nodiscard]] std::string spectrumTimePoints(
    const talas::project::PartialTrackAsset& spectrum, const std::vector<SpectrumMarker>& markers);

// A spectrum as the examples play it: its SpectrumInRAM table, the
// table's frames and partials, and its markers' time points.
struct SynchronizedSpectrum
{
    std::filesystem::path table;
    std::size_t frames = 0;
    std::size_t partials = 0;
    std::string timePoints;
};

// The mix, each spectrum through a SpectrumInRAM into an
// OscillatorBank, the guide in the left channel and the warped in the
// right, the warped's TimeIndex a WarpedTimeIndex lining its markers up
// with the guide's.
[[nodiscard]] AnalysisExampleSound spectrumMixExampleSound(
    const talas::project::ProjectDocument& document,
    const std::string& name,
    const SynchronizedSpectrum& guide,
    const SynchronizedSpectrum& warped);

// The morph, in amplitude, frequency and time, the guide with
// its own timing at Morph 0 and the warped with its own at 1: each
// spectrum's TimeIndex a WarpedTimeIndex from its own markers toward the
// guide's at Morph 0 and the warped's at 1, the two SpectrumInRAMs'
// amplitudes and frequencies cross-faded by !Morph into one OscillatorBank
// of the partials they share.
[[nodiscard]] AnalysisExampleSound spectrumMorphExampleSound(
    const talas::project::ProjectDocument& document,
    const std::string& name,
    const SynchronizedSpectrum& guide,
    const SynchronizedSpectrum& warped);

// The Synchronizing Spectra tool opens on its
// instructions and a Continue button; Continue starts it, the Guide and
// Warped spectra and their buttons in their place.
enum class SynchronizingSpectraPart : std::uint8_t
{
    Instructions,
    ContinueButton,
    Spectra,
};
[[nodiscard]] bool synchronizingSpectraShows(bool continued, SynchronizingSpectraPart part) noexcept;
}
