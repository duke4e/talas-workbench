#pragma once

#include "talas/app/SampleEditorModel.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The DiskSplicer's Segments field: segments
// of one or more sample files spliced one after another, edited without
// changing the files. A selection of the spliced samples is what Cut, Copy,
// Paste, Clear and Trim work on, what the play button plays and what the
// disk button pastes a whole file over.
namespace talas::app
{
// One line of the field: a sample file, its first sample and the sample
// after its last.
struct SplicedSegment
{
    std::string file;
    std::size_t first = 0;
    std::size_t last = 0;

    [[nodiscard]] std::size_t length() const noexcept { return last > first ? last - first : 0; }
    bool operator==(const SplicedSegment&) const = default;
};

// How many samples a file the project holds has, or none for a file it
// does not hold.
using SampleFileFrames = std::function<std::optional<std::size_t>(std::string_view file)>;

// The field's text as segments, a line each: the file, then optionally its
// first sample and the sample after its last (the whole file without
// them), as the DiskSplicer plays it. Nothing, with the reason, for a line
// the DiskSplicer refuses.
[[nodiscard]] std::optional<std::vector<SplicedSegment>> readSplicedSegments(
    std::string_view text, const SampleFileFrames& frames, std::string& error);
// The segments as the field's text, a whole file by its name alone.
[[nodiscard]] std::string splicedSegmentsText(const std::vector<SplicedSegment>& segments, const SampleFileFrames& frames);

class DiskSplicerSegments
{
public:
    explicit DiskSplicerSegments(std::vector<SplicedSegment> segments = {});

    [[nodiscard]] const std::vector<SplicedSegment>& segments() const noexcept { return segments_; }
    // The spliced samples, every segment's one after another.
    [[nodiscard]] std::size_t frames() const noexcept;

    // The selection, within the spliced samples and in order; an empty one
    // is the insertion point.
    void select(std::size_t start, std::size_t end);
    void selectAll();
    [[nodiscard]] const SampleSelection& selection() const noexcept { return selection_; }

    // Edit menu: Cut and Copy put the selected segments on the clipboard,
    // Paste puts them in place of the selection, Clear removes the
    // selection and Trim keeps only it; false when nothing changes.
    bool cut();
    bool copy();
    bool paste();
    bool clear();
    bool trim();
    [[nodiscard]] bool hasClipboard() const noexcept { return !clipboard_.empty(); }
    // The disk button: the whole of a file in place of the selection, which
    // it becomes.
    bool pasteFile(std::string file, std::size_t fileFrames);

    // The selected samples as segments: what the play button plays.
    [[nodiscard]] std::vector<SplicedSegment> selected() const;
    // The segments of the spliced samples from one to before another.
    [[nodiscard]] std::vector<SplicedSegment> between(std::size_t start, std::size_t end) const;

    // Where a spliced sample comes from: its segment and the sample of
    // that segment's file.
    struct Source
    {
        std::size_t segment = 0;
        std::size_t sample = 0;
        bool operator==(const Source&) const = default;
    };
    [[nodiscard]] std::optional<Source> sourceAt(std::size_t frame) const;
    // Where each segment begins among the spliced samples.
    [[nodiscard]] std::vector<std::size_t> segmentStarts() const;

private:
    void replace(std::size_t start, std::size_t end, const std::vector<SplicedSegment>& with);

    std::vector<SplicedSegment> segments_;
    SampleSelection selection_;
    std::vector<SplicedSegment> clipboard_;
};

// A file's samples, a channel each, or none for a file the project does
// not hold.
using SampleFileChannels = std::function<const std::vector<std::vector<double>>*(std::string_view file)>;
// What the play button plays: the segments' samples one after another, a
// sample a frame whatever rate a file was recorded at, on as many channels
// as the widest file (up to two), a mono file on both; silence for a file
// the project does not hold.
[[nodiscard]] std::vector<std::vector<double>> splicedSamples(
    const std::vector<SplicedSegment>& segments, const SampleFileChannels& channels);

// A drag in the overview from one spliced sample to another: the right half
// shows the samples between them.
[[nodiscard]] SampleEditorView splicedViewBetween(
    SampleEditorView view, std::size_t from, std::size_t to, std::size_t total) noexcept;

// The field below the overview: the magnification of the
// selection on the left; the size and range of the selection on the right,
// in samples or seconds at the sample rate it plays at (a sample a frame,
// whatever rate its files were recorded at).
[[nodiscard]] std::string splicedSelectionSize(const SampleSelection& selection, double sampleRate, bool inSamples);
[[nodiscard]] std::string splicedSelectionRange(const SampleSelection& selection, double sampleRate, bool inSamples);
}
