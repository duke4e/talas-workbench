#pragma once

#include "talas/project/AlternateTunings.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// The Tools menu's analysis tools.

// What a tool plays of the file chosen: Spectral Analysis loops the sample; RE and GA
// Analysis play the sample, or a spectrum's resynthesis, once, to check it is
// the one wanted.
enum class AnalysisToolKind
{
    Spectral,
    Re,
    Ga,
};

[[nodiscard]] bool analysisToolPreviewLoops(AnalysisToolKind kind) noexcept;

// Spectral Analysis's resynthesis: one sine wave oscillator a track, the count shown below Audition.
[[nodiscard]] std::size_t resynthesisOscillatorCount(const talas::project::PartialTrackAsset& analysis);
// Settings that need more oscillators at once than the
// signal processor can play, more tracks sounding in a frame than the
// realtime player's voices, are warned of.
[[nodiscard]] std::optional<std::string> resynthesisCapacityWarning(const talas::project::PartialTrackAsset& analysis);

// The Spectral Analysis Tool's five lowest-frequency settings, "Above 1F"
// to "Above 5F": the F of octaves 1 to 5 (43.65 Hz doubling). The highest
// setting below a sample's fundamental is the one to take, Above 3F for
// middle C.
inline constexpr int spectralAnalysisLowestFrequencySettings = 5;
[[nodiscard]] double spectralAnalysisLowestFrequencyHz(int setting) noexcept;
[[nodiscard]] std::string spectralAnalysisLowestFrequencyLabel(int setting);
// The analysis's tracks, its band-pass filters equally spaced from 0 hz to
// half the sample rate: 512 for 1F, half as many for each octave
// up, as a LiveSpectralAnalysis's LowestAnalyzedFreq gives.
[[nodiscard]] std::size_t spectralAnalysisTracks(int setting) noexcept;
// The spectrum file's name after the sample's: an s and the number of
// tracks, as "voice s512.talaspartial".
[[nodiscard]] std::string spectralAnalysisFileSuffix(int setting);
// BestFreq, BetterFreq, BetterTime and BestTime, one to four
// analysis bins wide.
[[nodiscard]] std::vector<std::string> spectralAnalysisResponseLabels();
[[nodiscard]] std::size_t spectralAnalysisResponseBins(int response) noexcept;

// Where a tool's file goes by default: beside the sample, its name with a
// suffix, e.g. "voice.wav" and " RE.txt" give "voice RE.txt".
[[nodiscard]] std::filesystem::path analysisOutputPath(
    const std::filesystem::path& sample,
    std::string_view suffix);
// An RE file goes by default into the RE folder of
// the Wavetables folder, here the Talas folder's, named for the sample.
[[nodiscard]] std::filesystem::path reAnalysisOutputPath(
    const std::filesystem::path& sample,
    const std::filesystem::path& talasFolder);
// The RE Analysis window's parameters,
// in its order, each with its range, the analysis's default and a unit:
// the filter order, the filter updates a second and the averaging time.
struct AnalysisWindowParameter
{
    std::string name;
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
    std::string unit;

    bool operator==(const AnalysisWindowParameter&) const = default;
};
[[nodiscard]] std::vector<AnalysisWindowParameter> reAnalysisWindowParameters();

struct AnalysisExampleResource
{
    std::string nodeId;
    std::string parameterId;
    std::filesystem::path file;
};

// The example Sound a tool leaves after an analysis,
// as a Sound definition whose resource fields take the files written, and
// the VCS controls it brings at their starting values where the project
// has none of the names yet.
struct AnalysisExampleSound
{
    talas::project::ProjectSoundDefinition definition;
    std::vector<AnalysisExampleResource> resources;
    std::vector<talas::project::VirtualControl> controls;
};

// The spectrum played by a partial-track player, a SumOfSines.
[[nodiscard]] AnalysisExampleSound spectralAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& spectrumFile);

// What a stereo sample's spectrum file analyzes,
// a mono mix of its two channels, one of them, or the left and right into
// separate files tagged L and R; the files, and the channels each takes
// (none for the mix); and the stereo resynthesis, a partial-track player
// for each file combined by a ChannelJoin.
enum class StereoAnalysisChoice : std::uint8_t
{
    MonoMix,
    Left,
    Right,
    LeftAndRight,
};

[[nodiscard]] std::vector<std::string> stereoAnalysisChoiceLabels();
[[nodiscard]] std::vector<std::filesystem::path> stereoAnalysisFiles(
    const std::filesystem::path& spectrumFile, StereoAnalysisChoice choice);
[[nodiscard]] std::vector<std::optional<std::size_t>> stereoAnalysisChannels(StereoAnalysisChoice choice);
// The samples each file analyzes: the mix of the two channels, or a
// channel; a mono sample's one channel.
[[nodiscard]] std::vector<std::vector<double>> stereoAnalysisSamples(
    const std::vector<std::vector<double>>& channels, StereoAnalysisChoice choice);
// The steps before the file is saved, the audition
// and the quasi-harmonic pages, analyze a stereo sample's left channel
// only; a mono sample its one channel.
[[nodiscard]] std::vector<double> spectralAnalysisAuditionedChannel(const std::vector<std::vector<double>>& channels);
[[nodiscard]] AnalysisExampleSound stereoSpectralAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& leftFile,
    const std::filesystem::path& rightFile);

// A harmonic analysis's files after the
// sample's name and an h for harmonic, "voice h.talaspartial", with its
// SumOfSines table beside it, "voice h.txt"; and the two example Sounds
// it leaves, SumOfSines on the table at the pitch given for the analysis,
// its OnDuration the sample's duration: one plain, one with Morph
// controls, the Gate button stretching its OnDuration by 100 s and Morph
// driving DBMorph and PchMorph toward Analysis1, the analysis itself until
// another spectrum is named there.
[[nodiscard]] std::string harmonicAnalysisFileSuffix();
[[nodiscard]] std::filesystem::path harmonicAnalysisTablePath(const std::filesystem::path& spectrumFile);
[[nodiscard]] std::vector<AnalysisExampleSound> harmonicAnalysisExampleSounds(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& tableFile,
    std::size_t frames,
    std::size_t partials,
    double pitchHz,
    double durationSeconds);

// A region copied in the spectrum editor joins the
// Sounds as a SumOfSines, which can time-stretch and frequency-scale it.
// Behind the scenes a SumOfSines is a SpectrumInRAM
// controlling an OscillatorBank, and so is this one, on the spectrum's
// SpectrumInRAM table of `frames` frames and `partials` tracks (the
// spectrum file bound to its Analysis is written as that table): its
// TimeIndex a FunctionGenerator's FullRamp over the spectrum's duration
// times !Stretch, its frequencies times !FreqScale, both 1 to start.
// A straight or a quasi-harmonic spectrum plays alike.
[[nodiscard]] AnalysisExampleSound spectrumSumOfSinesSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& spectrumFile,
    std::size_t frames,
    std::size_t partials,
    double durationSeconds);

// A GA analysis played by a GAOscillators at its
// base pitch, its TimeIndex ramped from -1 to 1 over the analysed sound's
// duration by a FunctionGenerator's FullRamp.
[[nodiscard]] AnalysisExampleSound gaAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& analysisFile,
    double basePitch,
    double durationSeconds);

// A sample a CDP process wrote (CDP Process to Sample, ADR-0018 amendment
// 2026-10-19), played by a Sample.
[[nodiscard]] AnalysisExampleSound cdpProcessExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile);

// A sample file the sample editor saved, played by a
// Sample, so the project uses it.
[[nodiscard]] AnalysisExampleSound sampleEditorExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile);
// A sample file dragged in: a GenericSource reading it from the disk, named for the file.
[[nodiscard]] AnalysisExampleSound sampleFileSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile);

// The REResonator on the RE table, its TimeIndex ramped from -1 to 1 over
// the sound's duration, fed the EX file or, without one, white noise
// attenuated by 0.001, as the filter overflows on other inputs.
// !Rate sets how fast the filter's
// parameters change and the excitation plays, !ExAmp the excitation's
// amplitude (the filter's input) and !Gain the filter's output, each at 1
// to start.
[[nodiscard]] AnalysisExampleSound reAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& tableFile,
    const std::filesystem::path& excitationFile,
    double durationSeconds);

// The frequency of !KeyNumber in a tuning as a Val expression in hz:
// equal steps a power of 2, otherwise the degrees of an octave from an
// array, at most 32 of them (an expression's longest array), a file's
// tuning only where its keys repeat each octave within 32 keys.
[[nodiscard]] std::optional<std::string> tuningFrequencyExpression(
    const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies,
    std::string& error);

// The example the Design Alternate Tunings Tool's Create Example makes
//: a MIDIVoice of eight voices for the MIDI keyboard,
// each a 21-harmonic sawtooth or a sine shaped by an ADSR, or the chosen
// sample, at its key's frequency in the tuning, at the level of the Volume
// fader (MIDI controller 7, at its top at first). Its expressions are
// Val, so a project in an older expression language has none.
enum class TuningExampleInstrument : std::uint8_t
{
    Sine,
    Sample,
    Sawtooth,
};

[[nodiscard]] std::optional<AnalysisExampleSound> tuningExampleSound(
    const talas::project::ProjectDocument& document,
    const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies,
    TuningExampleInstrument instrument,
    const std::filesystem::path& sampleFile,
    std::string& error);

// Throughout the Design
// Alternate Tunings Tool the MIDI keyboard tries out the tuning in real
// time. What the Tool plays while it is open: the example Sound on its own,
// the Sound to play of a document of its own with its Volume fader at its
// top, the sample of the Sample instrument still to be bound to its node's
// field.
struct TuningTryOut
{
    talas::project::ProjectDocument document;
    std::vector<AnalysisExampleResource> resources;
};

[[nodiscard]] std::optional<TuningTryOut> tuningTryOut(
    const talas::project::ProjectDocument& document,
    const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies,
    TuningExampleInstrument instrument,
    const std::filesystem::path& sampleFile,
    std::string& error);

// The Design Alternate Tunings Tool's pages:
// the main page, no kind of tuning chosen, holds the global settings (the
// tonic and the example's instrument) and the buttons for the five kinds of
// tuning; a kind's page holds its fields and example presets, the octave
// from the tonic and, at its lower right, Create Example, Load, Save and
// Back, which takes it back to the main page.
enum class TuningToolPart : std::uint8_t
{
    Tonic,
    Instrument,
    KindButtons,
    KindFields,
    Octave,
    PageButtons,
};

[[nodiscard]] bool tuningToolPageShows(
    std::optional<talas::project::tunings::Method> page, TuningToolPart part) noexcept;

// A spectrum view's window on times and frequencies: a box drawn with Ctrl or
// Command, given as fractions of the view from its top left, is magnified to
// fill the view, its top the highest frequency; Return shows the whole
// spectrum again. A box too small to mean one is ignored.
struct SpectrumViewWindow
{
    double lowTime = 0.0;
    double highTime = 1.0;
    double lowHz = 0.0;
    double highHz = 2000.0;

    bool operator==(const SpectrumViewWindow&) const = default;
};

[[nodiscard]] std::optional<SpectrumViewWindow> spectrumViewZoomedTo(
    const SpectrumViewWindow& shown, double left, double top, double right, double bottom) noexcept;

// Holding Control or Command over a spectrum view turns the
// cursor into a magnifying glass, the centre of the transparent plus in
// its lens the point the zoom box is drawn from (x and y in the 24-point
// cursor image).
enum class SpectrumViewCursor : std::uint8_t
{
    Arrow,
    Magnifier,
};
[[nodiscard]] SpectrumViewCursor spectrumViewCursor(bool controlOrCommandDown) noexcept;
inline constexpr std::pair<int, int> spectrumViewMagnifierHotSpot{9, 9};

// A click in the black spectrum display selects it, and the
// white line traced over it then shows its points as squares; unselected,
// the line alone.
enum class SpectrumTraceHandles : std::uint8_t
{
    None,
    Squares,
};
[[nodiscard]] SpectrumTraceHandles spectrumTraceHandles(bool displaySelected) noexcept;

// The trace view's keys: Return shows the whole spectrum
// again, L (lowercase) draws the tracks as unconnected frames or as frames
// joined by lines in turn, and Delete removes the selected point.
enum class TraceViewKey : std::uint8_t
{
    Return,
    LowercaseL,
    Delete,
    Other,
};
enum class TraceViewAction : std::uint8_t
{
    ShowWhole,
    ToggleConnectedFrames,
    DeletePoint,
    None,
};
[[nodiscard]] TraceViewAction traceViewAction(TraceViewKey key, bool pointSelected) noexcept;
}
