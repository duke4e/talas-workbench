#include "talas/app/DiskSplicerSegments.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The DiskSplicer's Segments field.
namespace
{
using talas::app::SplicedSegment;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::optional<std::size_t> heldFrames(std::string_view file)
{
    if (file == "Samples/A")
        return 1000;
    if (file == "Samples/B")
        return 400;
    if (file == "hi fu mi yo.wav")
        return 300;
    return std::nullopt;
}

// The field's text, a line a segment, read as the DiskSplicer plays it and
// written back with a whole file by its name alone.
void testTheFieldsText()
{
    std::string error;
    const auto read = talas::app::readSplicedSegments("Samples/A 0 500\n\nSamples/B\n", heldFrames, error);
    require(read && *read == std::vector<SplicedSegment>{{"Samples/A", 0, 500}, {"Samples/B", 0, 400}},
        "a segment of part of a file and one of a whole file: " + error);
    require(talas::app::splicedSegmentsText(*read, heldFrames) == "Samples/A 0 500\nSamples/B", "the field's text written back");
    const auto quoted = talas::app::readSplicedSegments("'hi fu mi yo.wav' 10 20", heldFrames, error);
    require(quoted && *quoted == std::vector<SplicedSegment>{{"hi fu mi yo.wav", 10, 20}}
            && talas::app::splicedSegmentsText(*quoted, heldFrames) == "'hi fu mi yo.wav' 10 20"
            && !talas::app::readSplicedSegments("'hi fu 10 20", heldFrames, error),
        "a file's name with spaces within quotes: " + error);
    require(!talas::app::readSplicedSegments("Samples/Missing", heldFrames, error)
            && error.find("no sample file") != std::string::npos,
        "a file the project does not hold is refused");
    require(!talas::app::readSplicedSegments("Samples/B 300 500", heldFrames, error)
            && !talas::app::readSplicedSegments("Samples/B 10", heldFrames, error)
            && !talas::app::readSplicedSegments("Samples/B 20 20", heldFrames, error),
        "a part past the file's end, without its end, or of no samples is refused");
}

// Cut, Copy, Paste, Clear and Trim on a selection of the spliced samples,
// leaving the files as they are.
void testEditingTheSelection()
{
    talas::app::DiskSplicerSegments splicer{{{"Samples/A", 0, 500}, {"Samples/B", 0, 400}}};
    require(splicer.frames() == 900, "the segments spliced one after another");
    splicer.select(400, 600);
    require(splicer.selected() == std::vector<SplicedSegment>{{"Samples/A", 400, 500}, {"Samples/B", 0, 100}},
        "a selection across the splice takes the end of one and the beginning of the next");
    require(splicer.cut() && splicer.frames() == 700
            && splicer.segments() == std::vector<SplicedSegment>{{"Samples/A", 0, 400}, {"Samples/B", 100, 400}}
            && splicer.selection().empty() && splicer.selection().start == 400,
        "Cut removes the selected samples, leaving the insertion point");
    require(splicer.paste() && splicer.frames() == 900
            && splicer.segments() == std::vector<SplicedSegment>{{"Samples/A", 0, 500}, {"Samples/B", 0, 400}}
            && splicer.selection() == talas::app::SampleSelection{400, 600},
        "Paste puts them back, the pieces of each file joined again, and selects them");
    splicer.select(0, 100);
    require(splicer.copy() && splicer.hasClipboard(), "Copy keeps the selection");
    splicer.select(900, 900);
    require(splicer.paste() && splicer.segments().back() == SplicedSegment{"Samples/A", 0, 100}, "Paste at the end");
    splicer.select(100, 300);
    require(splicer.clear() && splicer.frames() == 800 && splicer.segments().front() == SplicedSegment{"Samples/A", 0, 100},
        "Clear removes the selection without the clipboard");
    splicer.select(50, 150);
    require(splicer.trim() && splicer.segments() == std::vector<SplicedSegment>{{"Samples/A", 50, 100}, {"Samples/A", 300, 350}}
            && splicer.selection() == talas::app::SampleSelection{0, 100},
        "Trim keeps only the selection");
    require(!splicer.trim() && !talas::app::DiskSplicerSegments{}.cut(), "nothing to trim or cut");
    require(splicer.sourceAt(60) == talas::app::DiskSplicerSegments::Source{1, 310} && !splicer.sourceAt(100)
            && splicer.segmentStarts() == std::vector<std::size_t>{0, 50},
        "where a spliced sample comes from");
}

// Select All, then the disk button: the file the prototype played replaced
// by the whole of another.
void testPastingAFileOverTheSelection()
{
    talas::app::DiskSplicerSegments splicer{{{"Samples/A", 0, 1000}}};
    splicer.selectAll();
    require(splicer.pasteFile("Samples/B", 400) && splicer.segments() == std::vector<SplicedSegment>{{"Samples/B", 0, 400}}
            && splicer.selection() == talas::app::SampleSelection{0, 400}
            && talas::app::splicedSegmentsText(splicer.segments(), heldFrames) == "Samples/B",
        "the samples of the file chosen appear in the field");
    splicer.select(100, 100);
    require(splicer.pasteFile("Samples/A", 1000) && splicer.frames() == 1400 && splicer.segments().size() == 3,
        "a file pasted at the insertion point splices it in");
}

// The overview picks what the right half shows; the icon buttons scroll a
// page, make the selection fill the display, and contract or expand it in
// time and amplitude; the play button plays the selected samples.
void testTheDisplayAndThePlayButton()
{
    talas::app::DiskSplicerSegments splicer{{{"Samples/A", 0, 500}, {"Samples/B", 0, 400}}};
    const auto total = splicer.frames();
    auto view = talas::app::splicedViewBetween({0, 900, 1.0}, 600, 300, total);
    require(view == talas::app::SampleEditorView{300, 300, 1.0}, "a drag in the overview, either way");
    view = talas::app::sampleViewPaged(view, total, true);
    require(view.start == 600 && talas::app::sampleViewPaged(view, total, false).start == 300, "a page right and left");
    splicer.select(450, 550);
    view = talas::app::sampleViewFilled(view, splicer.selection());
    require(view.start == 450 && view.frames == 100, "the selection exactly fills the display");
    require(talas::app::sampleViewZoomed(view, total, false).frames == 200 && talas::app::sampleViewZoomed(view, total, true).frames == 50
            && talas::app::sampleViewLevelled(view, true).level == 2.0 && talas::app::sampleViewLevelled(view, false).level == 0.5,
        "the display contracted and expanded in time and amplitude");

    const std::vector<std::vector<double>> mono{std::vector<double>(1000, 0.25)};
    const std::vector<std::vector<double>> stereo{std::vector<double>(400, 0.5), std::vector<double>(400, -0.5)};
    const auto files = [&](std::string_view file) -> const std::vector<std::vector<double>>* {
        return file == "Samples/A" ? &mono : file == "Samples/B" ? &stereo : nullptr;
    };
    const auto played = talas::app::splicedSamples(splicer.selected(), files);
    require(played.size() == 2 && played[0].size() == 100 && played[0][49] == 0.25 && played[1][49] == 0.25
            && played[0][50] == 0.5 && played[1][50] == -0.5,
        "the selected samples across the splice, a mono file on both channels");
    const auto silent = talas::app::splicedSamples({{"Samples/Missing", 0, 10}}, files);
    require(silent.size() == 1 && silent[0] == std::vector<double>(10, 0.0), "a file the project does not hold is silent");
}

// The field below the overview: the selection's size and range, in samples
// or seconds.
void testTheSelectionReadouts()
{
    const talas::app::SampleSelection selection{4800, 9600};
    require(talas::app::splicedSelectionSize(selection, 48000.0, true) == "4800 samp"
            && talas::app::splicedSelectionSize(selection, 48000.0, false) == "0.1000 s",
        "the size of the selection in either unit");
    require(talas::app::splicedSelectionRange(selection, 48000.0, true) == "4800 - 9600 samp"
            && talas::app::splicedSelectionRange(selection, 48000.0, false) == "0.1000 - 0.2000 s",
        "the range of the selection in either unit");
    require(talas::app::sampleMagnificationReadout(900, 450) == "1:2", "the magnification of the display, samples a pixel");
}
}

int main()
{
    try
    {
        testTheFieldsText();
        testEditingTheSelection();
        testPastingAFileOverTheSelection();
        testTheDisplayAndThePlayButton();
        testTheSelectionReadouts();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
