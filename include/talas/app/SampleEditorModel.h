#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The sample editor: a sample file's channels,
// a selection that Cut, Copy, Paste, Clear and Trim work on, a pencil, and
// the generator templates that make new segments or change the selection.
namespace talas::app
{
struct SampleSelection
{
    std::size_t start = 0;
    std::size_t end = 0;

    [[nodiscard]] bool empty() const noexcept { return end <= start; }
    [[nodiscard]] std::size_t length() const noexcept { return empty() ? 0 : end - start; }
    bool operator==(const SampleSelection&) const = default;
};

inline constexpr std::size_t maximumSampleEditorFrames = std::size_t{1} << 26;
inline constexpr std::size_t maximumSampleEditorChannels = 2;

// Samples copied in the sample editor and pasted in
// a Timeline are saved as a file of their own in `folder`, named for the
// file they came from (or "Sample") and " clip" with the first number from
// 1 that names no file there yet, and play as a Sound of the project.
[[nodiscard]] std::filesystem::path pastedSampleClipPath(
    const std::filesystem::path& folder, const std::filesystem::path& source);

class SampleEditorModel
{
public:
    // A new sample file of silence (File > New... > Sample file).
    SampleEditorModel(double sampleRate = 48000.0, std::size_t channels = 1, std::size_t frames = 0);
    // An existing sample file's channels (1 or 2, all the same length).
    [[nodiscard]] static std::optional<SampleEditorModel> fromChannels(
        double sampleRate, std::vector<std::vector<double>> channels);

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::size_t channelCount() const noexcept { return channels_.size(); }
    [[nodiscard]] std::size_t frames() const noexcept { return channels_.empty() ? 0 : channels_.front().size(); }
    [[nodiscard]] const std::vector<std::vector<double>>& channels() const noexcept { return channels_; }
    [[nodiscard]] bool modified() const noexcept { return modified_; }
    void markSaved() noexcept { modified_ = false; }

    // The selection, kept within the file and in order; an empty selection
    // is the insertion point.
    void select(std::size_t start, std::size_t end);
    void selectAll();
    [[nodiscard]] const SampleSelection& selection() const noexcept { return selection_; }

    // Edit menu: Cut and Copy put the selection on the editor's
    // clipboard; Paste puts the clipboard in place of the selection (or at
    // the insertion point); Clear removes the selection; Trim keeps only
    // the selection. Each is one undo step; false when nothing changes.
    bool cut();
    bool copy();
    bool paste();
    bool clear();
    bool trim();
    [[nodiscard]] bool hasClipboard() const noexcept { return !clipboard_.empty() && !clipboard_.front().empty(); }
    // The samples Cut or Copy put on the clipboard, a vector a channel,
    // which the Timeline pastes as a clip.
    [[nodiscard]] const std::vector<std::vector<double>>& clipboard() const noexcept { return clipboard_; }

    // The pencil: a straight line of values from one frame to another on a
    // channel (or every channel), each value within -1 to 1. A drag's lines
    // after beginStroke are one undo step.
    void beginStroke();
    bool drawLine(std::optional<std::size_t> channel, std::size_t fromFrame, double fromValue, std::size_t toFrame,
        double toValue);

    // A generated segment (the same on each channel) in place of the
    // selection or at the insertion point (Insert).
    bool insertSegment(const std::vector<double>& segment);
    // The selection replaced by `segment` per channel.
    bool replaceSelection(const std::vector<std::vector<double>>& segments);

    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }

    // The largest absolute value in the selection (or the file).
    [[nodiscard]] double peak() const;

    // Edit info: the header's sample rate, the samples left as
    // they are, and its channels, a stereo file made mono keeping its left
    // channel and a mono one made stereo copying it. The channels are one
    // undo step.
    bool setHeader(double sampleRate, std::size_t channels);

private:
    struct State
    {
        std::vector<std::vector<double>> channels;
        SampleSelection selection;
    };
    void remember();
    bool removeRange(std::size_t start, std::size_t end);

    double sampleRate_ = 48000.0;
    std::vector<std::vector<double>> channels_;
    SampleSelection selection_;
    std::vector<std::vector<double>> clipboard_;
    std::vector<State> undo_;
    std::vector<State> redo_;
    bool modified_ = false;
};

// The generator templates. Each reads its fields as text (numbers, or lists of
// numbers separated by spaces; a duration as `4096 samp`, `0.1 s` or `100 ms`)
// and either makes a segment that replaces the selection or changes the
// selection.
enum class SampleTemplate
{
    Buzz,
    CubicSpline,
    ExponentialSegments,
    Fourier,
    ImpulseResponse,
    InterpolateSelection,
    LineSegments,
    NormalizeSelection,
    Polynomial,
    Random,
    WindowSelection,
};

struct SampleTemplateField
{
    std::string name;
    std::string defaultText;
};

[[nodiscard]] std::string_view sampleTemplateName(SampleTemplate kind);
[[nodiscard]] const std::vector<SampleTemplate>& sampleTemplates();
// The fields a template asks for, with the defaults it starts with.
[[nodiscard]] std::vector<SampleTemplateField> sampleTemplateFields(SampleTemplate kind);

struct SampleTemplateResult
{
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Evaluates the template with the fields given (missing ones take their
// defaults) on the editor: a segment in place of the selection, or the
// selection changed. One undo step.
SampleTemplateResult applySampleTemplate(
    SampleEditorModel& editor, SampleTemplate kind, const std::map<std::string, std::string>& fields);

// The numbers beside the overview: the selection's
// start, end and duration, in seconds or samples; the peak of the selection
// (or the file), in dB or linear; and the magnification of the detail view,
// samples a pixel.
[[nodiscard]] std::string sampleSelectionReadout(const SampleEditorModel& editor, bool inSamples);
[[nodiscard]] std::string samplePeakReadout(double peak, bool inDecibels);
[[nodiscard]] std::string sampleMagnificationReadout(std::size_t viewFrames, int pixels);

// What the play button plays: the selected samples, or the
// whole file when nothing is selected.
[[nodiscard]] std::vector<std::vector<double>> sampleEditorPlayed(const SampleEditorModel& editor);

// Display Lissajous: the first channel's samples across
// against the same samples a number of frames later up, over the selection
// (or the file), at most `limit` points.
[[nodiscard]] std::vector<std::pair<double, double>> sampleLissajous(
    const SampleEditorModel& editor, std::size_t delay, std::size_t limit = 4096);

// The editor's two parts in
// a window of the size given, the graphic editor on top (the overview
// above the detail view, then its buttons) and the generator templates
// below it.
struct SampleEditorArea
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int bottom() const noexcept { return y + height; }
};
struct SampleEditorLayout
{
    SampleEditorArea overview;
    SampleEditorArea detail;
    SampleEditorArea toolbar;
    SampleEditorArea templates;
};
[[nodiscard]] SampleEditorLayout sampleEditorLayout(int width, int height) noexcept;

// Info > Full waveform: a recording's lowest and highest
// sample.
struct WaveformExtremes
{
    double minimum = 0.0;
    double maximum = 0.0;
};
[[nodiscard]] WaveformExtremes waveformExtremes(const std::vector<std::vector<double>>& channels);
// Its tracks: the recording's output against time, left and right; a mono
// Sound's one channel on both.
[[nodiscard]] std::vector<std::vector<double>> fullWaveformLanes(std::vector<double> left, std::vector<double> right);

// A duration field as frames at a sample rate: `4096 samp`, `0.5 s`,
// `100 ms`, or a bare number of samples.
[[nodiscard]] std::optional<std::size_t> sampleTemplateFrames(std::string_view text, double sampleRate);
[[nodiscard]] std::optional<std::vector<double>> sampleTemplateNumbers(std::string_view text);
// The graphic editor's view: the part of the
// file the detail shows and its amplitude magnification; a click in the
// overview centres the detail there; the buttons scroll a page left or
// right, a page the size of the selection when there is one,
// make the selection exactly fill the display (in amplitude too, its
// peak reaching the top), contract or expand it in time, and contract or
// expand it in amplitude.
struct SampleEditorView
{
    std::size_t start = 0;
    std::size_t frames = 64;
    double level = 1.0;

    bool operator==(const SampleEditorView&) const = default;
};

// The insertion point, a click with no selection,
// shows as a single vertical red line that blinks as a text editor's
// insertion cursor does, lit for half a second, then dark for half a
// second (the colour as ARGB).
[[nodiscard]] bool sampleInsertionPointLit(std::uint64_t milliseconds) noexcept;
[[nodiscard]] std::uint32_t sampleInsertionPointColour() noexcept;
// The editor shows two channels, even for a file of one.
[[nodiscard]] std::size_t sampleEditorShownChannels(std::size_t fileChannels) noexcept;

// A file opened, made or shown: at first the detail
// shows the same range of samples as the overview, all of them, at full
// scale.
[[nodiscard]] SampleEditorView sampleViewOfWholeFile(std::size_t frames) noexcept;
// Insert: the overview always shows the whole
// file, the new segment with it, but the detail keeps its start and its
// level, only reaching on to the file's new end when the file grew past
// it, so a segment inserted before the detail's start is not seen there
// until Select All and the magnifying glass (sampleViewFilled) bring it in.
[[nodiscard]] SampleEditorView sampleViewAfterInsert(SampleEditorView view, std::size_t frames) noexcept;
[[nodiscard]] SampleEditorView sampleViewCentredAt(SampleEditorView view, std::size_t frame) noexcept;
[[nodiscard]] SampleEditorView sampleViewPaged(
    SampleEditorView view, std::size_t total, bool right, std::size_t selectionFrames = 0) noexcept;
[[nodiscard]] SampleEditorView sampleViewFilled(
    SampleEditorView view, const SampleSelection& selection, double peak = 0.0) noexcept;
[[nodiscard]] SampleEditorView sampleViewZoomed(SampleEditorView view, std::size_t total, bool in) noexcept;
[[nodiscard]] SampleEditorView sampleViewLevelled(SampleEditorView view, bool in) noexcept;

// Save: the file's type,
// its channels and its bits per sample, asked before its name. The types
// are the sample file formats written: WAV, AIFF, IRCAM, SD-I and, on a
// Macintosh, SD-II.
enum class SampleFileType : std::uint8_t
{
    Wav,
    Aiff,
    Ircam,
    SoundDesignerI,
    SoundDesignerII,
};

// The types Save and Edit Info offer, in their lists' order: SD-II only on
// a Macintosh, where files have resource forks.
[[nodiscard]] std::vector<SampleFileType> sampleFileTypes();
// Its name in those lists: WAV, AIFF, IRCAM, SD-I, SD-II.
[[nodiscard]] std::string sampleFileTypeName(SampleFileType type);
// The type a file's extension gives, any case: .aif, .aiff or .aifc AIFF,
// .sf IRCAM, .sd1 SD-I, .sd2 SD-II; WAV for others.
[[nodiscard]] SampleFileType sampleFileTypeOfExtension(const std::filesystem::path& path);

struct SampleFileFormat
{
    SampleFileType type = SampleFileType::Wav;
    std::size_t channels = 1;
    std::uint16_t bits = 16;

    bool operator==(const SampleFileFormat&) const = default;
};

[[nodiscard]] std::string sampleFileExtension(SampleFileType type);
// Edit Info: the dialog
// changes the file's format, its type and 8, 16 or 24 bits, as well as its
// header's rate and channels; the format chosen is the one the file is
// saved in next. Its text, as `AIFF, 24 bits`; nothing for other bits, or
// for an SD-I file of two channels (an SD-I file holds one).
[[nodiscard]] std::string sampleFileFormatText(const SampleFileFormat& format);
[[nodiscard]] std::optional<SampleFileFormat> sampleFileFormatFromInfo(
    SampleFileType type, std::uint16_t bits, std::size_t channels) noexcept;
// The channels saved: a stereo file saved mono keeps its left channel, a
// mono one saved stereo has it twice.
[[nodiscard]] std::vector<std::vector<double>> sampleChannelsToSave(
    const std::vector<std::vector<double>>& channels, std::size_t count);
}
