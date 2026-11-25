#include "talas/app/MemoryWarnings.h"
#include "talas/app/SampleEditorModel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

using talas::app::SampleEditorModel;
using talas::app::SampleTemplate;

SampleEditorModel ramp(std::size_t frames, std::size_t channels = 1)
{
    std::vector<std::vector<double>> data(channels, std::vector<double>(frames));
    for (std::size_t c = 0; c < channels; ++c)
    {
        for (std::size_t n = 0; n < frames; ++n)
            data[c][n] = static_cast<double>(n) / 100.0 + static_cast<double>(c);
    }
    return *SampleEditorModel::fromChannels(1000.0, data);
}

// Cut, Copy, Paste, Trim and Clear work on the selection.
void testEditMenuOnTheSelection()
{
    auto editor = ramp(10);
    editor.selectAll();
    require(editor.selection() == talas::app::SampleSelection{0, 10}, "Select all should select the whole file");
    editor.select(6, 2);
    require(editor.selection() == talas::app::SampleSelection{2, 6}, "a selection should be kept in order");
    require(editor.cut() && editor.frames() == 6 && near(editor.channels()[0][2], 0.06)
            && editor.selection() == talas::app::SampleSelection{2, 2},
        "Cut should remove the selection, leaving the insertion point");
    editor.select(0, 0);
    require(editor.paste() && editor.frames() == 10 && near(editor.channels()[0][0], 0.02)
            && near(editor.channels()[0][3], 0.05) && near(editor.channels()[0][4], 0.0)
            && editor.selection() == talas::app::SampleSelection{0, 4},
        "Paste should insert the cut samples at the insertion point and select them");
    require(editor.undo() && editor.frames() == 6 && editor.undo() && editor.frames() == 10
            && near(editor.channels()[0][9], 0.09),
        "Undo should take back the Paste and the Cut");
    require(editor.redo() && editor.frames() == 6, "Redo should do the Cut again");
    editor = ramp(10);
    editor.select(3, 5);
    require(editor.trim() && editor.frames() == 2 && near(editor.channels()[0][0], 0.03)
            && editor.selection() == talas::app::SampleSelection{0, 2},
        "Trim should keep only the selection");
    editor.select(0, 1);
    require(editor.clear() && editor.frames() == 1 && !editor.hasClipboard(), "Clear should remove without copying");
    editor.select(0, 0);
    require(!editor.cut() && !editor.trim() && !editor.paste(), "nothing selected or copied, nothing changes");
    auto stereo = ramp(4, 2);
    auto mono = ramp(4);
    mono.select(0, 2);
    require(mono.copy(), "copy from a mono file");
    stereo.select(4, 4);
    require(stereo.replaceSelection({mono.channels()[0]}) && stereo.frames() == 8 && near(stereo.channels()[1][4], 0.0),
        "a mono segment should go on both channels of a stereo file");
}

// A clip copied in the sample editor pastes into a
// Timeline: the samples copied, saved as a file of their own named for the
// sample they came from and " clip" with a number no file has yet.
void testCopiedSamplesPasteIntoATimeline()
{
    auto editor = ramp(10, 2);
    editor.select(2, 5);
    require(editor.copy() && editor.clipboard().size() == 2 && editor.clipboard()[0].size() == 3
            && near(editor.clipboard()[0][0], 0.02) && near(editor.clipboard()[1][2], 1.04),
        "Copy should keep the selected samples of each channel for the Timeline to paste");
    const auto folder = std::filesystem::temp_directory_path() / "talas_sample_clip_paths";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto first = talas::app::pastedSampleClipPath(folder, "/samples/Voice.aif");
    std::ofstream{first} << "x";
    require(first == folder / "Voice clip 1.wav" && talas::app::pastedSampleClipPath(folder, "/samples/Voice.aif") == folder / "Voice clip 2.wav"
            && talas::app::pastedSampleClipPath(folder, {}) == folder / "Sample clip 1.wav",
        "the pasted clip should be a file of its own, named for its sample and numbered");
    std::filesystem::remove_all(folder);
}

// A new sample file and the pencil.
void testNewFileAndPencil()
{
    SampleEditorModel editor(48000.0, 2, 100);
    require(editor.frames() == 100 && editor.channelCount() == 2 && editor.peak() == 0.0 && !editor.modified(),
        "a new sample file should be silence");
    editor.beginStroke();
    require(editor.drawLine(std::nullopt, 10, 0.0, 20, 2.0) && near(editor.channels()[0][15], 0.5)
            && near(editor.channels()[1][20], 1.0) && editor.modified(),
        "the pencil should draw a straight line within -1 to 1 on every channel");
    require(editor.drawLine(std::size_t{1}, 30, -0.5, 30, -0.5) && near(editor.channels()[1][30], -0.5)
            && editor.channels()[0][30] == 0.0,
        "the pencil should draw on one channel");
    require(editor.undo() && editor.peak() == 0.0, "one Undo should take back a whole stroke");
    require(!SampleEditorModel::fromChannels(48000.0, {{0.0}, {0.0, 1.0}}).has_value()
            && !SampleEditorModel::fromChannels(48000.0, {{0.0}, {0.0}, {0.0}}).has_value(),
        "a file's channels should be one or two of one length");
}

// The generator templates.
void testGeneratorTemplates()
{
    // The template is chosen from a list, each with its fields.
    require(talas::app::sampleTemplates().size() == 11 && talas::app::sampleTemplates().front() == SampleTemplate::Buzz
            && talas::app::sampleTemplateName(SampleTemplate::Buzz) == "Buzz"
            && !talas::app::sampleTemplateFields(SampleTemplate::Buzz).empty(),
        "the list of templates for algorithmic sample creation");
    SampleEditorModel editor(48000.0, 1, 0);
    require(talas::app::applySampleTemplate(editor, SampleTemplate::Buzz,
                {{"Duration", "64 samp"}, {"Harmonics", "3"}, {"Lowest Harmonic", "2"}, {"AmpBase", "0.5"}, {"Gain", "1"}})
                .ok(),
        "Buzz should generate");
    auto expected = 0.0;
    for (const auto harmonic : {2.0, 3.0, 4.0})
        expected += std::pow(0.5, harmonic) * std::sin(2.0 * std::numbers::pi * harmonic * 5.0 / 64.0);
    require(editor.frames() == 64 && near(editor.channels()[0][5], expected),
        "a harmonic's amplitude should be AmpBase raised to its number, one cycle over the duration");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::LineSegments,
                {{"Duration", "10 samp"}, {"End Points", "0 1 -1"}, {"Proportions", "1 1"}})
                .ok()
            && editor.frames() == 10 && near(editor.channels()[0][0], 0.0) && near(editor.channels()[0][4], 0.8)
            && near(editor.channels()[0][5], 1.0) && near(editor.channels()[0][7], 0.2),
        "line segments should replace the selection, each its proportion of the duration");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::ExponentialSegments,
                {{"Duration", "4 samp"}, {"End Points", "0.01 1"}, {"Proportions", "1"}})
                .ok()
            && near(editor.channels()[0][2], 0.1),
        "an exponential segment should move by equal ratios");
    require(!talas::app::applySampleTemplate(editor, SampleTemplate::ExponentialSegments,
                {{"End Points", "0 1"}, {"Proportions", "1"}})
                 .ok(),
        "exponential end points should lie above 0");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::Fourier,
                {{"Duration", "8 samp"}, {"Harmonics", "1"}, {"Amplitudes", "1"}, {"Phases", "{Float pi / 2}"}})
                .ok()
            && near(editor.channels()[0][0], 1.0) && near(editor.channels()[0][4], -1.0),
        "Fourier should add the harmonics at their phases, {Float pi} in braces");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::Polynomial,
                {{"Duration", "3 samp"}, {"Coefficients", "5 4 -2.5"}, {"Interval", "-1 1"}})
                .ok()
            && near(editor.channels()[0][0], 5.0 - 4.0 - 2.5) && near(editor.channels()[0][1], 5.0)
            && near(editor.channels()[0][2], 5.0 + 4.0 - 2.5),
        "Polynomial should evaluate its coefficients over the interval");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::CubicSpline,
                {{"Duration", "5 samp"}, {"Points", "0 1 0"}, {"Proportions", "0.5 0.5"}})
                .ok()
            && near(editor.channels()[0][0], 0.0) && near(editor.channels()[0][2], 1.0) && near(editor.channels()[0][4], 0.0)
            && editor.channels()[0][1] > 0.5,
        "a cubic spline should pass through its points smoothly");
    editor.selectAll();
    require(talas::app::applySampleTemplate(editor, SampleTemplate::Random, {{"Duration", "100 samp"}, {"Seed", "7"}}).ok(),
        "Random should generate");
    const auto first = editor.channels()[0];
    editor.selectAll();
    (void)talas::app::applySampleTemplate(editor, SampleTemplate::Random, {{"Duration", "100 samp"}, {"Seed", "7"}});
    require(editor.channels()[0] == first && editor.peak() <= 1.0 && editor.peak() > 0.5,
        "a seed should repeat its random waveform within -1 to 1");
    SampleEditorModel ir(1000.0, 1, 0);
    require(talas::app::applySampleTemplate(ir, SampleTemplate::ImpulseResponse,
                {{"Duration", "100 samp"}, {"Frequencies", "100"}, {"Bandwidths", "10"}, {"Amplitudes", "1"}, {"Gain", "1"}})
                .ok()
            && near(ir.channels()[0][3], std::exp(-std::numbers::pi * 10.0 * 0.003) * std::sin(2.0 * std::numbers::pi * 0.3)),
        "an impulse response should ring at its formant's frequency, decaying by its bandwidth");
    require(!talas::app::applySampleTemplate(editor, SampleTemplate::Buzz, {{"Duration", "loud"}}).ok()
            && !talas::app::applySampleTemplate(editor, SampleTemplate::LineSegments, {{"End Points", "0 1 0"}, {"Proportions", "1"}})
                    .ok(),
        "a template with fields it cannot read should refuse");
}

// Normalize, Window and Interpolate Selection change
// the selection.
void testSelectionTemplates()
{
    auto editor = *SampleEditorModel::fromChannels(1000.0, {{0.1, -0.25, 0.2, 0.5}});
    require(!talas::app::applySampleTemplate(editor, SampleTemplate::NormalizeSelection, {}).ok(),
        "Normalize Selection should need a selection");
    editor.select(0, 3);
    require(talas::app::applySampleTemplate(editor, SampleTemplate::NormalizeSelection, {{"Amplitude", "0.5"}}).ok()
            && near(editor.channels()[0][1], -0.5) && near(editor.channels()[0][0], 0.2) && near(editor.channels()[0][3], 0.5),
        "Normalize Selection should scale the selection's peak to the Amplitude");
    // An Amplitude of 0 turns the selection into zeroes.
    editor.select(1, 3);
    require(talas::app::applySampleTemplate(editor, SampleTemplate::NormalizeSelection, {{"Amplitude", "0"}}).ok()
            && near(editor.channels()[0][1], 0.0) && near(editor.channels()[0][2], 0.0) && near(editor.channels()[0][0], 0.2),
        "Normalize Selection to an Amplitude of 0 should give zeroes");
    SampleEditorModel ones(1000.0, 1, 0);
    (void)talas::app::applySampleTemplate(ones, SampleTemplate::LineSegments,
        {{"Duration", "100 samp"}, {"End Points", "1 1"}, {"Proportions", "1"}});
    ones.selectAll();
    require(talas::app::applySampleTemplate(ones, SampleTemplate::WindowSelection, {{"Transition Duration", "10 ms"}}).ok()
            && near(ones.channels()[0][0], 0.0) && near(ones.channels()[0][5], 0.5) && near(ones.channels()[0][50], 1.0)
            && near(ones.channels()[0][99], 0.0) && near(ones.channels()[0][94], 0.5),
        "Window Selection should fade the selection in and out over the transition");
    SampleEditorModel sine(1000.0, 1, 0);
    (void)talas::app::applySampleTemplate(sine, SampleTemplate::Fourier, {{"Duration", "64 samp"}});
    sine.selectAll();
    require(talas::app::applySampleTemplate(sine, SampleTemplate::Fourier,
                {{"Duration", "64 samp"}, {"Harmonics", "1"}, {"Amplitudes", "1"}, {"Phases", "0"}})
                .ok(),
        "a single cycle");
    sine.selectAll();
    require(talas::app::applySampleTemplate(sine, SampleTemplate::InterpolateSelection,
                {{"Length", "128 samp"}, {"Gain", "1"}, {"Interpolation Points", "16"}})
                .ok()
            && sine.frames() == 128 && near(sine.channels()[0][32], 1.0, 0.03) && near(sine.channels()[0][96], -1.0, 0.03),
        "Interpolate Selection should stretch the selection to the Length");
}

// A template's Duration is its length (4096 samp a standard
// wavetable), its Proportions split it, a segment list takes one proportion
// fewer than its end points, and Gain scales the result towards -1 to 1.
void testTemplateLengthsAndGains()
{
    SampleEditorModel spline(48000.0, 1, 0);
    require(talas::app::applySampleTemplate(spline, SampleTemplate::CubicSpline, {}).ok() && spline.frames() == 4096,
        "a cubic spline's Duration of 4096 samp should make a standard wavetable");
    spline.selectAll();
    require(!talas::app::applySampleTemplate(spline, SampleTemplate::CubicSpline,
                 {{"Points", "0 1 0 -1 0"}, {"Proportions", "0.5 0.5"}})
                 .ok()
            && spline.frames() == 4096,
        "a spline of five points should refuse two segments");

    SampleEditorModel exponential(48000.0, 1, 0);
    require(talas::app::applySampleTemplate(exponential, SampleTemplate::ExponentialSegments,
                {{"End Points", "0.001 1 0.001"}, {"Proportions", "0.1 0.9"}})
                .ok()
            && exponential.frames() == 4096,
        "exponential segments of 4096 samp should make a standard wavetable");
    const auto& rise = exponential.channels()[0];
    const auto top = std::distance(rise.begin(), std::max_element(rise.begin(), rise.end()));
    require(top == 410 && near(rise[2253], std::sqrt(0.001), 1e-4),
        "the first segment should take a tenth of the duration and the second the rest");
    exponential.selectAll();
    require(!talas::app::applySampleTemplate(exponential, SampleTemplate::ExponentialSegments,
                 {{"End Points", "0.001 1 0.001"}, {"Proportions", "1"}})
                 .ok(),
        "three end points should refuse one segment");

    const auto generate = [](SampleTemplate kind, const std::map<std::string, std::string>& fields) {
        SampleEditorModel editor(1000.0, 1, 0);
        require(talas::app::applySampleTemplate(editor, kind, fields).ok(), "the template should generate");
        return editor;
    };
    const auto buzz = generate(SampleTemplate::Buzz, {{"Gain", "1"}});
    const auto quieter = generate(SampleTemplate::Buzz, {{"Gain", "0.5"}});
    const auto fitted = generate(SampleTemplate::Buzz, {{"Gain", std::to_string(1.0 / buzz.peak())}});
    require(buzz.peak() > 1.0 && near(quieter.channels()[0][100], 0.5 * buzz.channels()[0][100])
            && near(fitted.peak(), 1.0, 1e-6),
        "Buzz's Gain should scale the waveform, so a Gain of one over its peak fills -1 to 1");
    const auto ring = generate(SampleTemplate::ImpulseResponse, {{"Gain", "0.1"}});
    const auto louder = generate(SampleTemplate::ImpulseResponse, {{"Gain", "0.2"}});
    require(ring.peak() > 0.0 && near(louder.peak(), 2.0 * ring.peak())
            && near(louder.channels()[0][7], 2.0 * ring.channels()[0][7]),
        "the impulse response's Gain should scale its amplitude");
}

// Interpolate Selection stretches a cycle to exactly 4096
// samples, lowers a sample's frequency by stretching it, scales it by Gain,
// and interpolates better with more Interpolation Points.
void testInterpolateSelection()
{
    const auto tone = [](std::size_t frames, double cyclesPerFrame) {
        std::vector<double> samples(frames);
        for (std::size_t n = 0; n < frames; ++n)
            samples[n] = std::sin(2.0 * std::numbers::pi * cyclesPerFrame * static_cast<double>(n) + 0.3);
        return *SampleEditorModel::fromChannels(48000.0, {samples});
    };
    auto cycle = tone(100, 0.01);
    cycle.selectAll();
    require(talas::app::applySampleTemplate(cycle, SampleTemplate::InterpolateSelection, {}).ok()
            && cycle.frames() == 4096 && near(cycle.channels()[0][1024], std::cos(0.3), 0.02)
            && near(cycle.channels()[0][3072], -std::cos(0.3), 0.02),
        "a single cycle should stretch to exactly 4096 samples for an Oscillator's wavetable");

    // The interior, away from the selection's ends, against a tone.
    const auto distance = [](const std::vector<double>& samples, std::size_t from, std::size_t to,
                              double cyclesPerFrame) {
        auto worst = 0.0;
        for (auto n = from; n < to; ++n)
        {
            worst = std::max(worst,
                std::abs(samples[n]
                    - std::sin(2.0 * std::numbers::pi * cyclesPerFrame * static_cast<double>(n) + 0.3)));
        }
        return worst;
    };
    auto lower = tone(512, 1.0 / 16.0);
    lower.selectAll();
    require(talas::app::applySampleTemplate(lower, SampleTemplate::InterpolateSelection,
                {{"Length", "1024 samp"}, {"Interpolation Points", "16"}})
                .ok()
            && lower.frames() == 1024 && distance(lower.channels()[0], 256, 768, 1.0 / 32.0) < 0.01,
        "stretching a sample to twice its length should halve its frequency");
    auto high = tone(512, 0.4);
    high.selectAll();
    require(talas::app::applySampleTemplate(high, SampleTemplate::InterpolateSelection,
                {{"Length", "256 samp"}, {"Interpolation Points", "16"}})
                .ok()
            && std::ranges::all_of(high.channels()[0].begin() + 64, high.channels()[0].begin() + 192,
                [](double sample) { return std::abs(sample) < 0.05; }),
        "compressing a tone past the Nyquist frequency should drop it rather than alias it");

    auto gained = tone(512, 1.0 / 16.0);
    gained.selectAll();
    (void)talas::app::applySampleTemplate(gained, SampleTemplate::InterpolateSelection,
        {{"Length", "1024 samp"}, {"Gain", "0.5"}, {"Interpolation Points", "16"}});
    require(near(gained.channels()[0][500], 0.5 * lower.channels()[0][500]),
        "Gain should scale the interpolated samples");

    const auto error = [&](const char* points) {
        auto stretched = tone(512, 1.0 / 16.0);
        stretched.selectAll();
        (void)talas::app::applySampleTemplate(stretched, SampleTemplate::InterpolateSelection,
            {{"Length", "2048 samp"}, {"Interpolation Points", points}});
        return distance(stretched.channels()[0], 512, 1536, 1.0 / 64.0);
    };
    const auto coarse = error("2");
    const auto fine = error("32");
    require(fine < 0.01 && coarse > 4.0 * fine,
        "more Interpolation Points should interpolate more closely");
}

void testFieldReading()
{
    require(talas::app::sampleTemplateFrames("4096 samp", 48000.0) == std::optional<std::size_t>{4096}
            && talas::app::sampleTemplateFrames("0.5 s", 48000.0) == std::optional<std::size_t>{24000}
            && talas::app::sampleTemplateFrames("10 ms", 48000.0) == std::optional<std::size_t>{480}
            && !talas::app::sampleTemplateFrames("4 hz", 48000.0).has_value(),
        "a duration field should read samples, seconds and milliseconds");
    require(talas::app::sampleTemplateNumbers("1 -2.5 {2 * Float pi}")
            == std::optional<std::vector<double>>{{1.0, -2.5, 2.0 * std::numbers::pi}},
        "a list field should read numbers and braces");
    require(talas::app::sampleTemplateFields(SampleTemplate::Buzz).front().name == "Duration"
            && talas::app::sampleTemplates().size() == 11,
        "every sample template should be offered");
}
}

// The numbers beside the overview give the
// selection's start, end and duration in seconds or samples, the peak in dB
// or linear, and the detail view's magnification.
void testReadoutsAndHeader()
{
    auto editor = talas::app::SampleEditorModel(8000.0, 1, 8000);
    editor.select(800, 1600);
    require(talas::app::sampleSelectionReadout(editor, false) == "0.1000 s - 0.2000 s (0.1000 s)"
            && talas::app::sampleSelectionReadout(editor, true) == "800 - 1600 (800 samples)",
        "the selection's times should read in seconds or in samples");
    require(talas::app::samplePeakReadout(0.5, true) == "-6.0 dB" && talas::app::samplePeakReadout(0.5, false) == "0.5000"
            && talas::app::samplePeakReadout(0.0, true) == "-inf dB",
        "the peak should read in dB or linear");
    require(talas::app::sampleMagnificationReadout(8000, 800) == "1:10" && talas::app::sampleMagnificationReadout(100, 800) == "8:1",
        "the magnification should give samples a pixel or pixels a sample");
    // Edit info changes the header: the rate, the samples kept,
    // and the channels.
    require(editor.setHeader(16000.0, 2) && editor.sampleRate() == 16000.0 && editor.channelCount() == 2 && editor.frames() == 8000,
        "the header's rate and channels should change, the samples kept");
    require(editor.undo() && editor.channelCount() == 1, "the change of channels should be one undo step");
    require(!editor.setHeader(0.0, 1) && !editor.setHeader(8000.0, 3), "a header out of range should be refused");
}

// The play button plays the selection, or
// the whole file with nothing selected.
void testPlayTakesTheSelection()
{
    auto editor = talas::app::SampleEditorModel(8000.0, 2, 0);
    require(editor.insertSegment({0.1, 0.2, 0.3, 0.4}), "fill the file");
    editor.select(1, 3);
    const auto selected = talas::app::sampleEditorPlayed(editor);
    require(selected.size() == 2 && selected[0] == std::vector<double>{0.2, 0.3} && selected[1] == selected[0],
        "the play button should play the selected samples");
    editor.select(0, 0);
    require(talas::app::sampleEditorPlayed(editor)[0].size() == 4, "with nothing selected the whole file should play");
}

// The disk button pastes another file over the
// selection, or inserts it at the insertion point; Display Lissajous plots
// the waveform against itself some samples later.
void testPasteAFileAndLissajous()
{
    auto editor = talas::app::SampleEditorModel(8000.0, 1, 10);
    editor.select(2, 5);
    const std::vector<std::vector<double>> other{{0.5, 0.5}};
    require(editor.replaceSelection(other) && editor.frames() == 9 && editor.channels()[0][2] == 0.5 && editor.channels()[0][3] == 0.5,
        "a file should replace the selection");
    editor.select(0, 0);
    require(editor.replaceSelection(other) && editor.frames() == 11 && editor.channels()[0][0] == 0.5,
        "with an insertion point the whole file should go in there");
    auto ramp = talas::app::SampleEditorModel(8000.0, 1, 0);
    std::vector<double> segment;
    for (int index = 0; index < 8; ++index)
        segment.push_back(index * 0.1);
    require(ramp.insertSegment(segment), "make a ramp");
    ramp.select(0, 0);
    const auto points = talas::app::sampleLissajous(ramp, 2);
    require(points.size() == 6 && std::abs(points[0].first - 0.0) < 1e-12 && std::abs(points[0].second - 0.2) < 1e-12
            && std::abs(points[5].second - 0.7) < 1e-12,
        "Lissajous should set each sample against the one the delay later");
    const auto extremes = talas::app::waveformExtremes({{0.25, -0.5}, {0.75, 0.0}});
    require(extremes.minimum == -0.5 && extremes.maximum == 0.75, "the full waveform's minimum and maximum");
    // The output against time on two tracks, a mono
    // Sound's the same on both.
    const std::vector<double> mono{0.0, 0.5, -0.25};
    const auto lanes = talas::app::fullWaveformLanes(mono, {});
    const auto stereo = talas::app::fullWaveformLanes(mono, {0.1, 0.2, 0.3});
    require(lanes.size() == 2 && lanes[0] == mono && lanes[1] == mono && stereo.size() == 2 && stereo[0] == mono
            && stereo[1] == std::vector<double>{0.1, 0.2, 0.3},
        "Full waveform should show the output on both tracks, a mono Sound's on each");
    // The graphic editor on top, the
    // generator templates below it.
    const auto layout = talas::app::sampleEditorLayout(800, 600);
    require(layout.overview.bottom() <= layout.detail.y && layout.detail.bottom() <= layout.toolbar.y
            && layout.toolbar.bottom() <= layout.templates.y && layout.templates.bottom() <= 600
            && layout.detail.height > layout.templates.height && layout.templates.height > 0,
        "the graphic editor should take the top of the window, the templates the bottom");
}

// A click in the overview centres the detail
// there; the buttons scroll a page left or right, make the selection fill
// the display, contract or expand it in time, and in amplitude.
void testTheViewButtons()
{
    const talas::app::SampleEditorView view{1000, 400, 1.0};
    require(talas::app::sampleViewOfWholeFile(10000) == talas::app::SampleEditorView{0, 10000, 1.0},
        "at first both views should show the same range of samples");
    // The insertion point is a red line blinking each half
    // second; two channels show, even for a file of one.
    const auto red = talas::app::sampleInsertionPointColour();
    require(talas::app::sampleInsertionPointLit(0) && talas::app::sampleInsertionPointLit(499) && !talas::app::sampleInsertionPointLit(500)
            && talas::app::sampleInsertionPointLit(1000) && (red >> 16 & 0xffu) > 0xc0 && (red >> 8 & 0xffu) < 0x60
            && (red & 0xffu) < 0x60,
        "the insertion point should be a red line that blinks");
    require(talas::app::sampleEditorShownChannels(1) == 2 && talas::app::sampleEditorShownChannels(2) == 2,
        "two channels should be displayed, even for a single channel file");
    require(talas::app::sampleViewCentredAt(view, 5000).start == 4800, "the overview chooses the part of the file shown");
    require(talas::app::sampleViewPaged(view, 10000, false).start == 600 && talas::app::sampleViewPaged(view, 10000, true).start == 1400
            && talas::app::sampleViewPaged({100, 400, 1.0}, 10000, false).start == 0,
        "the arrows scroll one page of samples left or right");
    require(talas::app::sampleViewPaged(view, 10000, true, 250).start == 1250
            && talas::app::sampleViewPaged(view, 10000, false, 250).start == 750
            && talas::app::sampleViewPaged(view, 10000, true, 250).frames == 400,
        "with a selection the arrows jump the display by the selection's size");
    const auto filled = talas::app::sampleViewFilled(view, {2000, 2600});
    require(filled.start == 2000 && filled.frames == 600, "the selection exactly fills the display area");
    require(talas::app::sampleViewFilled(view, {2000, 2600}, 0.25).level == 4.0
            && talas::app::sampleViewFilled(view, {2000, 2600}, 0.0).level == 1.0,
        "the magnifying glass should fill the display up and down as well, the selection's peak at the top");
    require(talas::app::sampleViewZoomed(view, 10000, false).frames == 800 && talas::app::sampleViewZoomed(view, 10000, true).frames == 200,
        "the display contracts and expands in the time direction");
    require(talas::app::sampleViewLevelled(view, true).level == 2.0 && talas::app::sampleViewLevelled(view, false).level == 0.5,
        "the display contracts and expands in the amplitude direction");
}

// After Insert the new waveform shows in the
// overview, which always spans the whole file, but it may not be in the
// larger waveform view, which keeps its place; Select All and the
// magnifying glass bring it into view.
void testInsertShowsInTheOverviewFirst()
{
    SampleEditorModel editor(48000.0, 1, 10000);
    // The detail shows 400 samples from frame 4000; the insertion point is
    // at frame 1000, before them.
    const talas::app::SampleEditorView detail{4000, 400, 2.0};
    editor.select(1000, 1000);
    require(talas::app::applySampleTemplate(editor, SampleTemplate::Buzz,
                {{"Duration", "500 samp"}, {"Harmonics", "3"}, {"Lowest Harmonic", "1"}, {"AmpBase", "0.5"}, {"Gain", "1"}})
                .ok()
            && editor.frames() == 10500,
        "Insert should put the template's waveform into the file at the insertion point");
    const auto inserted = [&](std::size_t from, std::size_t frames) {
        for (std::size_t frame = from; frame < from + frames && frame < editor.frames(); ++frame)
        {
            if (frame >= 1000 && frame < 1500 && std::abs(editor.channels()[0][frame]) > 1.0e-6)
                return true;
        }
        return false;
    };
    const auto overview = talas::app::sampleViewOfWholeFile(editor.frames());
    require(overview.start == 0 && overview.frames == editor.frames() && inserted(overview.start, overview.frames),
        "the overview should show the whole file, the inserted waveform with it");
    const auto after = talas::app::sampleViewAfterInsert(detail, editor.frames());
    require(after.start == detail.start && after.level == detail.level && !inserted(after.start, after.frames),
        "the larger view should keep its place, the inserted waveform not visible there");
    editor.selectAll();
    const auto fitted = talas::app::sampleViewFilled(after, editor.selection(), editor.peak());
    require(inserted(fitted.start, fitted.frames) && fitted.start == 0 && fitted.frames == editor.frames(),
        "Select All and the magnifying glass should bring the waveform into view");
    // An empty new file shows its first segment whole.
    SampleEditorModel empty(48000.0, 1, 0);
    require(talas::app::applySampleTemplate(empty, SampleTemplate::Buzz, {{"Duration", "4096 samp"}}).ok()
            && talas::app::sampleViewAfterInsert(talas::app::sampleViewOfWholeFile(0), empty.frames()).frames == 4096,
        "a new file's first segment should fill the larger view");
}

talas::project::ProjectNode memoryNode(std::string id, std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    return {std::move(id), std::move(typeId), std::move(parameters), {}, {}};
}

// Reading memory before anything is written there,
// or writing memory nothing reads, gives a warning; a TimeOffset on the
// reader puts it right.
void testSampleMemoryWarnings()
{
    using talas::project::ProjectParameterExpression;
    talas::project::ProjectGraph graph;
    graph.nodes.push_back(memoryNode("writer", "talas.memoryWriter",
        {{.parameterId = "recordingName", .resource = std::string{"echo"}}}));
    graph.nodes.push_back(memoryNode("reader", "talas.sample",
        {{.parameterId = "sample", .resource = std::string{"echo"}},
            {.parameterId = "fromMemoryWriter", .boolean = true},
            {.parameterId = "gate", .expression = std::string{"1.0"}}}));
    graph.nodes.push_back(memoryNode("mix", "talas.mixer", {}));
    graph.connections = {{"writer", "audioOut", "mix", "in1"}, {"reader", "audioOut", "mix", "in2"}};
    auto warnings = talas::app::memoryWarnings(graph);
    require(warnings.size() == 1 && warnings.front().kind == talas::app::MemoryWarningKind::ReadBeforeWritten
            && warnings.front().nodeId == "reader" && warnings.front().recording == "echo"
            && warnings.front().message.find("TimeOffset") != std::string::npos,
        "reading memory before anything is written there gives a warning");
    require(talas::app::memoryWarningsStatus(graph) == " - warning: " + warnings.front().message,
        "the warning shows as the Sound plays");

    graph.nodes.push_back(memoryNode("late", "talas.timeOffset",
        {{.parameterId = "offset", .expression = std::string{"2 s"}}}));
    graph.connections = {{"writer", "audioOut", "mix", "in1"}, {"reader", "audioOut", "late", "audioIn"},
        {"late", "audioOut", "mix", "in2"}};
    require(talas::app::memoryWarnings(graph).empty(), "a TimeOffset on the Sound that reads the memory solves it");

    graph.nodes[1].parameters[2].expression = "!KeyDown";
    graph.connections = {{"writer", "audioOut", "mix", "in1"}, {"reader", "audioOut", "mix", "in2"}};
    require(talas::app::memoryWarnings(graph).empty(), "a reader that waits for its Gate is not warned of");

    graph.nodes[1].parameters[1].boolean = false;
    warnings = talas::app::memoryWarnings(graph);
    require(warnings.size() == 1 && warnings.front().kind == talas::app::MemoryWarningKind::WrittenNeverRead
            && warnings.front().nodeId == "writer",
        "writing memory that no Sound reads gives a warning");
}

// Save and Edit Info offer every sample
// file format Talas writes, WAV, AIFF, IRCAM, SD-I and, on a Macintosh,
// SD-II, each naming its file by its extension and known again by it; an
// SD-I file holds one channel.
void testSavingInEveryTalasFormat()
{
    using talas::app::SampleFileType;
    std::vector<SampleFileType> expected{
        SampleFileType::Wav, SampleFileType::Aiff, SampleFileType::Ircam, SampleFileType::SoundDesignerI};
#if defined(__APPLE__)
    expected.push_back(SampleFileType::SoundDesignerII);
#endif
    require(talas::app::sampleFileTypes() == expected, "Save should offer each format Talas creates");
    for (const auto type : {SampleFileType::Wav, SampleFileType::Aiff, SampleFileType::Ircam,
             SampleFileType::SoundDesignerI, SampleFileType::SoundDesignerII})
    {
        require(talas::app::sampleFileTypeOfExtension("voice" + talas::app::sampleFileExtension(type)) == type,
            "each type's extension should give it back: " + talas::app::sampleFileTypeName(type));
    }
    require(talas::app::sampleFileExtension(SampleFileType::SoundDesignerI) == ".sd1"
            && talas::app::sampleFileExtension(SampleFileType::Ircam) == ".sf"
            && talas::app::sampleFileTypeOfExtension("voice.AIFC") == SampleFileType::Aiff
            && talas::app::sampleFileTypeOfExtension("voice.SD2") == SampleFileType::SoundDesignerII
            && talas::app::sampleFileTypeOfExtension("voice") == SampleFileType::Wav,
        "SD-I files end in .sd1, IRCAM files in .sf");
    require(talas::app::sampleFileFormatText({SampleFileType::SoundDesignerII, 2, 24}) == "SD-II, 24 bits"
            && talas::app::sampleFileFormatText({SampleFileType::Ircam, 1, 8}) == "IRCAM, 8 bits",
        "Edit Info should name the format");
    require(!talas::app::sampleFileFormatFromInfo(SampleFileType::SoundDesignerI, 16, 2).has_value()
            && talas::app::sampleFileFormatFromInfo(SampleFileType::SoundDesignerI, 16, 1).has_value(),
        "an SD-I file holds one channel");
}

// Save asks the file's type,
// channels and bits; a changed file is saved before the editor closes.
void testSavingTheFile()
{
    require(talas::app::sampleFileExtension(talas::app::SampleFileType::Aiff) == ".aif"
            && talas::app::sampleFileExtension(talas::app::SampleFileType::Wav) == ".wav",
        "the file type chosen should name the file");
    // Edit Info changes the file's format, WAV or
    // AIFF and 8, 16 or 24 bits, as well as its header.
    const auto aiff = talas::app::sampleFileFormatFromInfo(talas::app::SampleFileType::Aiff, 24, 2);
    require(aiff == std::optional<talas::app::SampleFileFormat>{{talas::app::SampleFileType::Aiff, 2, 24}}
            && talas::app::sampleFileFormatText(*aiff) == "AIFF, 24 bits"
            && talas::app::sampleFileFormatText({}) == "WAV, 16 bits"
            && !talas::app::sampleFileFormatFromInfo(talas::app::SampleFileType::Wav, 12, 1).has_value(),
        "Edit Info should change the file's format, its type and bits per sample");
    const std::vector<std::vector<double>> stereo{{0.5, 0.25}, {-0.5, -0.25}};
    require(talas::app::sampleChannelsToSave(stereo, 1) == std::vector<std::vector<double>>{{0.5, 0.25}}
            && talas::app::sampleChannelsToSave({{0.5, 0.25}}, 2) == std::vector<std::vector<double>>{{0.5, 0.25}, {0.5, 0.25}},
        "a stereo file saved mono should keep its left channel, a mono one saved stereo have it twice");
    auto model = talas::app::SampleEditorModel::fromChannels(8000.0, stereo).value();
    model.select(0, 1);
    require(!model.modified() && model.clear() && model.modified(), "an edit leaves the file to be saved on closing");
    model.markSaved();
    require(!model.modified(), "a saved file closes without asking");
}

int main()
{
    try
    {
        testEditMenuOnTheSelection();
        testSavingTheFile();
    testSavingInEveryTalasFormat();
        testNewFileAndPencil();
        testGeneratorTemplates();
        testSelectionTemplates();
        testTemplateLengthsAndGains();
        testInterpolateSelection();
        testFieldReading();
    testReadoutsAndHeader();
    testPlayTakesTheSelection();
    testPasteAFileAndLissajous();
    testTheViewButtons();
    testInsertShowsInTheOverviewFirst();
    testSampleMemoryWarnings();
    testCopiedSamplesPasteIntoATimeline();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "sample editor model tests passed\n";
    return 0;
}
